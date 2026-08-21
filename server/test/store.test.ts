// Covers what a claim means now: the tag is permanent, the exclusivity is not.
//
// The lock has to be aged for any of this to be testable, and the store reads the
// clock itself, so these tests reach into the database file with a second connection
// and move locked_until back. That is the one thing they do that the server never
// does; everything else goes through the public methods.

import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { test } from 'node:test';

import { config } from '../src/config.ts';
import { TagStore } from '../src/store.ts';

const ICAO = 'C81A2B';

function tempDb(): { path: string; cleanup: () => void } {
  const dir = mkdtempSync(join(tmpdir(), 'plane-radar-store-'));
  return { path: join(dir, 'tags.db'), cleanup: () => rmSync(dir, { recursive: true, force: true }) };
}

function device(store: TagStore, n: number): string {
  const id = `abcdef01234${n}`;
  const secret = String(n).repeat(32);
  store.register(id, secret, `RD0${n}`);
  return id;
}

/** Pretend the lock on every tag was taken `seconds` ago. */
function ageLocks(path: string, seconds: number): void {
  const db = new DatabaseSync(path);
  db.exec(`UPDATE tags SET claimed_at = claimed_at - ${seconds},
           locked_until = locked_until - ${seconds}`);
  db.close();
}

function withStore(fn: (store: TagStore, path: string) => void): void {
  const { path, cleanup } = tempDb();
  const store = new TagStore(path);
  try {
    fn(store, path);
  } finally {
    store.close();
    cleanup();
  }
}

test('a tag survives its lock instead of expiring', () => {
  withStore((store, path) => {
    const owner = device(store, 1);
    assert.equal(store.claim(owner, ICAO).status, 200);

    ageLocks(path, config.lockSeconds + 60);
    store.sweep();

    const lines = store.tagBlock().lines;
    assert.equal(lines.length, 1);
    // Still tagged, still the same handle, and no exclusivity left.
    assert.match(lines[0]!, new RegExp(`^T,${ICAO},RD01,0$`));
  });
});

test('another device is refused while the lock holds', () => {
  withStore((store) => {
    const owner = device(store, 1);
    const other = device(store, 2);
    store.claim(owner, ICAO);

    const reply = store.claim(other, ICAO);
    assert.equal(reply.status, 409);
    assert.equal(reply.handle, 'RD01');
    assert.match(reply.body, /lock=\d+/);
  });
});

test('another device takes the tag over once the lock has run out', () => {
  withStore((store, path) => {
    const owner = device(store, 1);
    const other = device(store, 2);
    store.claim(owner, ICAO);
    ageLocks(path, config.lockSeconds + 1);

    assert.equal(store.claim(other, ICAO).status, 200);
    assert.deepEqual(store.tagBlock().lines, [`T,${ICAO},RD02,${config.lockSeconds}`]);
    // And the previous owner is refused straight back, having just lost it.
    assert.equal(store.claim(owner, ICAO).status, 409);
  });
});

test('the owner re-claiming pushes the lock out without spending a slot', () => {
  withStore((store, path) => {
    const owner = device(store, 1);
    store.claim(owner, ICAO);
    ageLocks(path, config.lockSeconds + 1);

    assert.equal(store.claim(owner, ICAO).status, 200);
    assert.deepEqual(store.tagBlock().lines, [`T,${ICAO},RD01,${config.lockSeconds}`]);
  });
});

test("an unlocked tag still occupies one of the device's slots", () => {
  withStore((store, path) => {
    const owner = device(store, 1);
    for (let i = 0; i < config.maxTagsPerDevice; ++i) {
      assert.equal(store.claim(owner, `AA00${i.toString(16).padStart(2, '0')}`).status, 200);
    }
    ageLocks(path, config.lockSeconds + 1);
    store.sweep();

    // Nothing expired, so nothing was handed back.
    const reply = store.claim(owner, ICAO);
    assert.equal(reply.status, 429);
    assert.equal(store.release(owner, 'AA0000').status, 200);
    assert.equal(store.claim(owner, ICAO).status, 200);
  });
});

test('a takeover hands the losing device its slot back', () => {
  withStore((store, path) => {
    const owner = device(store, 1);
    const other = device(store, 2);
    for (let i = 0; i < config.maxTagsPerDevice; ++i) {
      store.claim(owner, `BB00${i.toString(16).padStart(2, '0')}`);
    }
    ageLocks(path, config.lockSeconds + 1);

    assert.equal(store.claim(other, 'BB0000').status, 200);
    assert.equal(store.claim(owner, ICAO).status, 200);
  });
});

test('a database written before tags were permanent keeps its rows', () => {
  const { path, cleanup } = tempDb();
  try {
    const old = new DatabaseSync(path);
    old.exec(`CREATE TABLE tags (
      icao       TEXT PRIMARY KEY,
      handle     TEXT NOT NULL,
      device     TEXT NOT NULL,
      claimed_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL
    )`);
    // Expired under the old rules, which used to mean gone.
    old.exec(`INSERT INTO tags VALUES ('${ICAO}', 'OLD1', 'abcdef012341', 1, 2)`);
    old.close();

    const store = new TagStore(path);
    try {
      store.sweep();
      assert.deepEqual(store.tagBlock().lines, [`T,${ICAO},OLD1,0`]);
    } finally {
      store.close();
    }
  } finally {
    cleanup();
  }
});
