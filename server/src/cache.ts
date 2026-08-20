// Replaces the Workers Cache API. On a normal server the whole thing is a Map with
// expiry times, which is one of the reasons self-hosting made this code smaller
// rather than larger.
//
// Entries are bounded by the number of distinct map cells radars are looking at, so
// there is no eviction policy beyond expiry.
//
// Expired entries are kept for a while rather than dropped, because a stale answer
// is genuinely useful here: when the upstream limiter refuses a fetch, serving
// aircraft positions a few seconds old beats failing the poll.

interface Entry<T> {
  value: T;
  expiresAtMs: number;
}

export interface StaleResult<T> {
  value: T;
  ageSeconds: number;
}

export class TtlCache<T> {
  private readonly entries = new Map<string, Entry<T>>();

  /** Fresh value only. */
  get(key: string): T | undefined {
    const entry = this.entries.get(key);
    if (entry === undefined || Date.now() >= entry.expiresAtMs) return undefined;
    return entry.value;
  }

  /** Value even if expired, as long as it is within `maxStaleSeconds` of expiry. */
  getStale(key: string, maxStaleSeconds: number): StaleResult<T> | undefined {
    const entry = this.entries.get(key);
    if (entry === undefined) return undefined;
    const staleMs = Date.now() - entry.expiresAtMs;
    if (staleMs > maxStaleSeconds * 1000) {
      this.entries.delete(key);
      return undefined;
    }
    return { value: entry.value, ageSeconds: Math.max(0, Math.round(staleMs / 1000)) };
  }

  set(key: string, value: T, ttlSeconds: number): void {
    this.entries.set(key, { value, expiresAtMs: Date.now() + ttlSeconds * 1000 });
  }

  /** Drop anything expired past the point it could still be served as stale. */
  sweep(maxStaleSeconds: number): void {
    const now = Date.now();
    for (const [key, entry] of this.entries) {
      if (now - entry.expiresAtMs > maxStaleSeconds * 1000) this.entries.delete(key);
    }
  }

  get size(): number {
    return this.entries.size;
  }
}

/**
 * Minimum-interval gate shared by all callers.
 *
 * Deliberately not a token bucket: a bucket allows a burst, and a burst is exactly
 * what would trip adsb.fi's 1 request/second limit. A flat minimum gap cannot burst.
 */
export class MinIntervalLimiter {
  private lastMs = 0;
  // Longhand, not a constructor parameter property: Node strips types rather than
  // compiling, and that mode cannot express them. It fails at startup, not typecheck.
  private readonly intervalMs: number;

  constructor(intervalMs: number) {
    this.intervalMs = intervalMs;
  }

  /** True if a call may proceed now, in which case the interval is consumed. */
  tryAcquire(): boolean {
    const now = Date.now();
    if (now - this.lastMs < this.intervalMs) return false;
    this.lastMs = now;
    return true;
  }
}

/**
 * Fixed-window counter per key. Coarse by design: at the boundary a client can get
 * two windows' worth, which does not matter for what it guards.
 */
export class FixedWindowCounter {
  private readonly hits = new Map<string, { windowStart: number; count: number }>();
  private readonly windowMs: number;
  private readonly limit: number;

  constructor(windowMs: number, limit: number) {
    this.windowMs = windowMs;
    this.limit = limit;
  }

  /** True if this key is under its limit, in which case the hit is recorded. */
  tryAcquire(key: string): boolean {
    const now = Date.now();
    const windowStart = now - (now % this.windowMs);
    const entry = this.hits.get(key);

    if (entry === undefined || entry.windowStart !== windowStart) {
      this.hits.set(key, { windowStart, count: 1 });
      return true;
    }
    if (entry.count >= this.limit) return false;
    entry.count += 1;
    return true;
  }

  /** Drop keys from previous windows so this cannot grow without bound. */
  sweep(): void {
    const now = Date.now();
    const windowStart = now - (now % this.windowMs);
    for (const [key, entry] of this.hits) {
      if (entry.windowStart !== windowStart) this.hits.delete(key);
    }
  }
}
