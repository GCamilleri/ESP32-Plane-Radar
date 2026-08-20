import { DatabaseSync } from 'node:sqlite';
import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';

import { config } from './config.ts';
import { normaliseHandle, tagLine } from './protocol.ts';

// Tag registry, replacing the Durable Object.
//
// The DO existed to serialise claims so "first claim wins" is correct. Here that
// comes free: node:sqlite's API is synchronous and Node runs JS on one thread, so a
// claim cannot be interleaved with another as long as nothing awaits partway
// through. Nothing in this file is async, deliberately.
//
// node:sqlite is a release candidate rather than fully stable, chosen because SQLite
// is compiled into the Node binary: no node-gyp and no prebuilt-binary trouble when
// the image is built for a different architecture than it runs on.

/** Blunts the most obvious impersonation and slur handles. Not exhaustive. */
const HANDLE_BLOCKLIST = new Set([
  'ADMIN', 'ROOT', 'NULL', 'TEST', 'ATC', 'MAYD', 'SOS', 'FUCK', 'CUNT',
  'SHIT', 'RAPE', 'NAZI', 'KKK', 'HEIL', 'NIGG', 'FAG',
]);

export interface Reply {
  status: number;
  body: string;
  handle?: string;
  fresh?: boolean;
  /** Short reason, for the log rather than the device. */
  detail?: string;
}

export interface TagBlock {
  lines: string[];
  lockSeconds: number;
}

interface DeviceRow {
  id: string;
  secret: string;
  handle: string;
}

interface TagRow {
  icao: string;
  handle: string;
  device: string;
  expires_at: number;
}

function nowSec(): number {
  return Math.floor(Date.now() / 1000);
}

/** Deterministic 4-char fallback handle so registration always succeeds. */
export function handleFromDeviceId(deviceId: string): string {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // no I/O/0/1
  let hash = 0x811c9dc5;
  for (let i = 0; i < deviceId.length; ++i) {
    hash ^= deviceId.charCodeAt(i);
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  let out = '';
  for (let i = 0; i < 4; ++i) {
    out += alphabet[hash % alphabet.length];
    hash = Math.floor(hash / alphabet.length);
  }
  return out;
}

export class TagStore {
  private readonly db: DatabaseSync;
  private snapshot: TagBlock | null = null;

  constructor(dbPath: string = config.dbPath) {
    if (dbPath !== ':memory:') {
      mkdirSync(dirname(dbPath), { recursive: true });
    }
    this.db = new DatabaseSync(dbPath);
    // WAL so a reader never blocks the writer; synchronous=NORMAL because losing
    // the last few seconds of tags after a power cut costs nothing.
    this.db.exec('PRAGMA journal_mode = WAL');
    this.db.exec('PRAGMA synchronous = NORMAL');
    this.db.exec(`CREATE TABLE IF NOT EXISTS tags (
      icao       TEXT PRIMARY KEY,
      handle     TEXT NOT NULL,
      device     TEXT NOT NULL,
      claimed_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL
    )`);
    // No claim counters: the per-device limit is a count of unexpired rows in `tags`,
    // so it needs no bookkeeping here and cannot drift out of step with reality.
    // Databases created before this may still carry unused claims/window_start
    // columns, which is harmless.
    this.db.exec(`CREATE TABLE IF NOT EXISTS devices (
      id         TEXT PRIMARY KEY,
      secret     TEXT NOT NULL,
      handle     TEXT NOT NULL,
      created_at INTEGER NOT NULL
    )`);
    this.db.exec('CREATE UNIQUE INDEX IF NOT EXISTS devices_handle ON devices (handle)');
    this.db.exec('CREATE INDEX IF NOT EXISTS tags_expiry ON tags (expires_at)');
    // The per-device cap counts unexpired rows for one device on every claim.
    this.db.exec('CREATE INDEX IF NOT EXISTS tags_device ON tags (device, expires_at)');
  }

  close(): void {
    this.db.close();
  }

  /** Delete expired tags so reads never have to filter a growing table. */
  sweep(): number {
    const removed = this.db
      .prepare('DELETE FROM tags WHERE expires_at <= ?')
      .run(nowSec()).changes;
    if (removed > 0) this.snapshot = null;
    return Number(removed);
  }

  /**
   * Serialised T block. Held in memory and invalidated on write: identical for every
   * device, so there is no reason to rebuild it per request.
   */
  tagBlock(): TagBlock {
    if (this.snapshot !== null) return this.snapshot;

    const now = nowSec();
    const rows = this.db
      .prepare(
        `SELECT icao, handle, expires_at FROM tags
         WHERE expires_at > ? ORDER BY claimed_at DESC LIMIT ?`,
      )
      .all(now, config.maxFeedTags) as unknown as TagRow[];

    this.snapshot = {
      lines: rows.map((r) => tagLine({ icao: r.icao, handle: r.handle, ttl: r.expires_at - now })),
      lockSeconds: config.lockSeconds,
    };
    return this.snapshot;
  }

  /**
   * Trust on first use: the device announces a secret it generated itself and we
   * remember it. Anyone can register, so this is not authentication of a person. It
   * stops one device claiming under another's handle and gives claims a stable
   * subject to rate limit.
   */
  register(deviceId: string, secret: string, requestedHandle: string): Reply {
    const existing = this.device(deviceId);

    if (existing !== undefined && existing.secret !== secret) {
      return {
        status: 403,
        body: 'device id already registered with a different secret',
        detail: 'secret-mismatch',
      };
    }

    let handle = normaliseHandle(requestedHandle);
    if (handle !== null && HANDLE_BLOCKLIST.has(handle)) handle = null;
    if (handle === null) {
      handle = existing !== undefined ? existing.handle : handleFromDeviceId(deviceId);
    }

    const clash = this.db
      .prepare('SELECT id FROM devices WHERE handle = ? AND id != ?')
      .get(handle, deviceId) as { id: string } | undefined;
    if (clash !== undefined) {
      if (existing !== undefined) {
        return {
          status: 409,
          body: `handle taken\nhandle=${existing.handle}`,
          handle: existing.handle,
          detail: 'handle-taken',
        };
      }
      handle = handleFromDeviceId(deviceId);
      const stillClashing = this.db
        .prepare('SELECT id FROM devices WHERE handle = ?')
        .get(handle) as { id: string } | undefined;
      if (stillClashing !== undefined) {
        return { status: 409, body: 'handle taken', detail: 'handle-taken' };
      }
    }

    if (existing !== undefined) {
      this.db.prepare('UPDATE devices SET handle = ? WHERE id = ?').run(handle, deviceId);
    } else {
      this.db
        .prepare('INSERT INTO devices (id, secret, handle, created_at) VALUES (?, ?, ?, ?)')
        .run(deviceId, secret, handle, nowSec());
    }
    // A handle change alters what every other radar draws, so drop the snapshot.
    this.snapshot = null;
    return {
      status: 200,
      body: `handle=${handle}\nlock=${config.lockSeconds}`,
      handle,
      fresh: existing === undefined,
    };
  }

  deviceSecret(deviceId: string): string | null {
    return this.device(deviceId)?.secret ?? null;
  }

  /**
   * Claim an aircraft. Held against other devices until it expires; the owner may
   * re-claim to refresh. Returns 409 with the current owner if someone else holds it.
   */
  claim(deviceId: string, icao: string): Reply {
    const now = nowSec();
    const device = this.device(deviceId);
    if (device === undefined) {
      return { status: 401, body: 'unregistered device', detail: 'unregistered' };
    }

    const held = this.db
      .prepare('SELECT * FROM tags WHERE icao = ? AND expires_at > ?')
      .get(icao, now) as TagRow | undefined;

    if (held !== undefined && held.device !== deviceId) {
      return {
        status: 409,
        body: `held\nhandle=${held.handle}\nttl=${held.expires_at - now}`,
        handle: held.handle,
        detail: 'held-by-other',
      };
    }

    // Concurrency cap, counted from the table rather than a stored tally, so an
    // expired tag stops occupying a slot the moment it expires. Refreshing a tag
    // this device already holds is exempt: it consumes no new slot.
    //
    // No global cap: a claim is never refused because other people hold tags, which
    // is what the old table-full check did.
    if (held === undefined) {
      const mine = this.db
        .prepare('SELECT COUNT(*) AS n FROM tags WHERE device = ? AND expires_at > ?')
        .get(deviceId, now) as { n: number };
      if (mine.n >= config.maxTagsPerDevice) {
        return {
          status: 429,
          body: `tag limit reached\nheld=${mine.n}\nmax=${config.maxTagsPerDevice}`,
          handle: device.handle,
          detail: `at-device-limit:${mine.n}`,
        };
      }
    }

    this.db
      .prepare(
        `INSERT INTO tags (icao, handle, device, claimed_at, expires_at)
         VALUES (?, ?, ?, ?, ?)
         ON CONFLICT(icao) DO UPDATE SET
           handle = excluded.handle,
           device = excluded.device,
           claimed_at = excluded.claimed_at,
           expires_at = excluded.expires_at`,
      )
      .run(icao, device.handle, deviceId, now, now + config.lockSeconds);
    this.snapshot = null;
    return {
      status: 200,
      body: `claimed\nhandle=${device.handle}\nttl=${config.lockSeconds}`,
      handle: device.handle,
    };
  }

  /**
   * Release every tag this device owns, for the radar's "Clear Tags" menu action.
   * Deliberately not rate limited: letting go of your own tags should never be
   * something the server refuses.
   */
  releaseAll(deviceId: string): Reply {
    const removed = Number(
      this.db.prepare('DELETE FROM tags WHERE device = ?').run(deviceId).changes,
    );
    if (removed > 0) this.snapshot = null;
    return { status: 200, body: `released=${removed}`, detail: `count=${removed}` };
  }

  /** Release a tag early. Owner only. */
  release(deviceId: string, icao: string): Reply {
    const held = this.db
      .prepare('SELECT device, handle FROM tags WHERE icao = ?')
      .get(icao) as Pick<TagRow, 'device' | 'handle'> | undefined;
    if (held === undefined) return { status: 404, body: 'not tagged', detail: 'not-tagged' };
    if (held.device !== deviceId) {
      return { status: 403, body: 'not your tag', detail: 'not-owner' };
    }
    this.db.prepare('DELETE FROM tags WHERE icao = ?').run(icao);
    this.snapshot = null;
    return { status: 200, body: 'released', handle: held.handle };
  }

  private device(deviceId: string): DeviceRow | undefined {
    return this.db.prepare('SELECT * FROM devices WHERE id = ?').get(deviceId) as
      | DeviceRow
      | undefined;
  }
}
