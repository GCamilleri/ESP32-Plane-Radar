import { DurableObject } from 'cloudflare:workers';
import { envNumber, type Env, type Reply } from './env';
import { normaliseHandle, tagLine } from './protocol';

// A single Durable Object owns the whole tag table. A DO handles one request at a
// time, which is what makes "first claim wins" correct without any locking of our
// own. Workers KV cannot express this: it is eventually consistent and capped at
// 1000 writes/day on the free plan.
//
// The free plan allows 5M SQLite rows read/day. Running a SELECT on every device
// poll would burn through that, so the serialised tag block is held in memory and
// only rebuilt after a write or when it ages out. Steady-state reads touch no rows.

const SWEEP_INTERVAL_MS = 60_000;

/** Blunts the most obvious impersonation and slur handles. Not exhaustive. */
const HANDLE_BLOCKLIST = new Set([
  'ADMIN', 'ROOT', 'NULL', 'TEST', 'ATC', 'MAYD', 'SOS', 'FUCK', 'CUNT',
  'SHIT', 'RAPE', 'NAZI', 'KKK', 'HEIL', 'NIGG', 'FAG',
]);

// The index signature is what SqlStorageCursor's row type constraint wants; the
// named fields are still checked on access.
interface DeviceRow {
  [key: string]: SqlStorageValue;
  id: string;
  secret: string;
  handle: string;
  created_at: number;
  claims: number;
  window_start: number;
}

interface TagRow {
  [key: string]: SqlStorageValue;
  icao: string;
  handle: string;
  device: string;
  claimed_at: number;
  expires_at: number;
}

export interface TagBlock {
  lines: string[];
  lockSeconds: number;
}

export class TagRegistry extends DurableObject<Env> {
  private snapshot: TagBlock | null = null;
  private snapshotAt = 0;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);

    this.ctx.blockConcurrencyWhile(async () => {
      const sql = this.ctx.storage.sql;
      sql.exec(`CREATE TABLE IF NOT EXISTS tags (
        icao       TEXT PRIMARY KEY,
        handle     TEXT NOT NULL,
        device     TEXT NOT NULL,
        claimed_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL
      )`);
      sql.exec(`CREATE TABLE IF NOT EXISTS devices (
        id           TEXT PRIMARY KEY,
        secret       TEXT NOT NULL,
        handle       TEXT NOT NULL,
        created_at   INTEGER NOT NULL,
        claims       INTEGER NOT NULL DEFAULT 0,
        window_start INTEGER NOT NULL DEFAULT 0
      )`);
      sql.exec('CREATE UNIQUE INDEX IF NOT EXISTS devices_handle ON devices (handle)');
      sql.exec('CREATE INDEX IF NOT EXISTS tags_expiry ON tags (expires_at)');
      if ((await this.ctx.storage.getAlarm()) === null) {
        await this.ctx.storage.setAlarm(Date.now() + SWEEP_INTERVAL_MS);
      }
    });
  }

  /** Periodic sweep so reads never have to filter expired rows out of a large table. */
  override async alarm(): Promise<void> {
    const removed = this.ctx.storage.sql
      .exec('DELETE FROM tags WHERE expires_at <= ?', nowSec())
      .rowsWritten;
    if (removed > 0) this.snapshot = null;
    await this.ctx.storage.setAlarm(Date.now() + SWEEP_INTERVAL_MS);
  }

  private get lockSeconds(): number {
    return envNumber(this.env.LOCK_SECONDS, 1800);
  }

  private get maxActiveTags(): number {
    return envNumber(this.env.MAX_ACTIVE_TAGS, 64);
  }

  private get claimsPerHour(): number {
    return envNumber(this.env.CLAIMS_PER_HOUR, 10);
  }

  /**
   * Serialised T block plus the lock duration, for the feed to concatenate.
   * Identical for every device, so the caller can cache it at the edge.
   */
  async tagBlock(): Promise<TagBlock> {
    const now = nowSec();
    if (this.snapshot !== null && now - this.snapshotAt < 1) {
      return this.snapshot;
    }
    const rows = this.ctx.storage.sql
      .exec<Pick<TagRow, 'icao' | 'handle' | 'expires_at'>>(
        `SELECT icao, handle, expires_at FROM tags
         WHERE expires_at > ? ORDER BY claimed_at DESC LIMIT ?`,
        now,
        this.maxActiveTags,
      )
      .toArray();
    this.snapshot = {
      lines: rows.map((r) => tagLine({ icao: r.icao, handle: r.handle, ttl: r.expires_at - now })),
      lockSeconds: this.lockSeconds,
    };
    this.snapshotAt = now;
    return this.snapshot;
  }

  /**
   * Trust on first use: the device announces a secret it generated itself and we
   * remember it. Anyone can register, so this is not authentication. What it buys is
   * a stable identity to rate limit and revoke, and it stops a claim being forged
   * under someone else's handle.
   */
  async register(deviceId: string, secret: string, requestedHandle: string): Promise<Reply> {
    const sql = this.ctx.storage.sql;
    const existing = sql
      .exec<DeviceRow>('SELECT * FROM devices WHERE id = ?', deviceId)
      .toArray()[0];

    if (existing && existing.secret !== secret) {
      return { status: 403, body: 'device id already registered with a different secret' };
    }

    let handle = normaliseHandle(requestedHandle);
    if (handle !== null && HANDLE_BLOCKLIST.has(handle)) handle = null;
    if (handle === null) handle = existing ? existing.handle : handleFromDeviceId(deviceId);

    // First come, first served. A taken handle falls back to a derived one rather
    // than failing outright, so a fresh device is never left unable to register.
    const clash = sql
      .exec<{ id: string }>(
        'SELECT id FROM devices WHERE handle = ? AND id != ?',
        handle,
        deviceId,
      )
      .toArray()[0];
    if (clash) {
      if (existing) return { status: 409, body: `handle taken\nhandle=${existing.handle}` };
      handle = handleFromDeviceId(deviceId);
      const stillClashing = sql
        .exec<{ id: string }>('SELECT id FROM devices WHERE handle = ?', handle)
        .toArray()[0];
      if (stillClashing) return { status: 409, body: 'handle taken' };
    }

    if (existing) {
      sql.exec('UPDATE devices SET handle = ? WHERE id = ?', handle, deviceId);
    } else {
      sql.exec(
        'INSERT INTO devices (id, secret, handle, created_at) VALUES (?, ?, ?, ?)',
        deviceId,
        secret,
        handle,
        nowSec(),
      );
    }
    return { status: 200, body: `handle=${handle}\nlock=${this.lockSeconds}` };
  }

  async deviceSecret(deviceId: string): Promise<string | null> {
    const row = this.ctx.storage.sql
      .exec<{ secret: string }>('SELECT secret FROM devices WHERE id = ?', deviceId)
      .toArray()[0];
    return row ? row.secret : null;
  }

  /**
   * Claim an aircraft. Held against other devices until it expires; the owner may
   * re-claim to refresh. Returns 409 with the current owner if someone else holds it.
   */
  async claim(deviceId: string, icao: string): Promise<Reply> {
    const sql = this.ctx.storage.sql;
    const now = nowSec();

    const device = sql
      .exec<DeviceRow>('SELECT * FROM devices WHERE id = ?', deviceId)
      .toArray()[0];
    if (!device) return { status: 401, body: 'unregistered device' };

    const held = sql
      .exec<TagRow>('SELECT * FROM tags WHERE icao = ? AND expires_at > ?', icao, now)
      .toArray()[0];

    if (held && held.device !== deviceId) {
      return { status: 409, body: `held\nhandle=${held.handle}\nttl=${held.expires_at - now}` };
    }

    // Refreshes are rate limited too, otherwise one device can hold a tag forever.
    const windowStart = now - (now % 3600);
    const claims = device.window_start === windowStart ? device.claims : 0;
    if (claims >= this.claimsPerHour) {
      return { status: 429, body: `rate limited\nretry=${windowStart + 3600 - now}` };
    }

    if (!held) {
      const active = sql
        .exec<{ n: number }>('SELECT COUNT(*) AS n FROM tags WHERE expires_at > ?', now)
        .toArray()[0].n;
      if (active >= this.maxActiveTags) return { status: 507, body: 'tag table full' };
    }

    sql.exec(
      `INSERT INTO tags (icao, handle, device, claimed_at, expires_at)
       VALUES (?, ?, ?, ?, ?)
       ON CONFLICT(icao) DO UPDATE SET
         handle = excluded.handle,
         device = excluded.device,
         claimed_at = excluded.claimed_at,
         expires_at = excluded.expires_at`,
      icao,
      device.handle,
      deviceId,
      now,
      now + this.lockSeconds,
    );
    sql.exec(
      'UPDATE devices SET claims = ?, window_start = ? WHERE id = ?',
      claims + 1,
      windowStart,
      deviceId,
    );
    this.snapshot = null;
    return { status: 200, body: `claimed\nhandle=${device.handle}\nttl=${this.lockSeconds}` };
  }

  /** Release a tag early. Owner only. */
  async release(deviceId: string, icao: string): Promise<Reply> {
    const sql = this.ctx.storage.sql;
    const held = sql
      .exec<{ device: string }>('SELECT device FROM tags WHERE icao = ?', icao)
      .toArray()[0];
    if (!held) return { status: 404, body: 'not tagged' };
    if (held.device !== deviceId) return { status: 403, body: 'not your tag' };
    sql.exec('DELETE FROM tags WHERE icao = ?', icao);
    this.snapshot = null;
    return { status: 200, body: 'released' };
  }
}

function nowSec(): number {
  return Math.floor(Date.now() / 1000);
}

/** Deterministic 4-char fallback handle so registration always succeeds. */
function handleFromDeviceId(deviceId: string): string {
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
