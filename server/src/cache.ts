// Replaces the Workers Cache API. On a normal server the whole thing is a Map with
// expiry times, which is one of the reasons self-hosting made this code smaller
// rather than larger.
//
// Entries are bounded by the number of distinct map cells radars are looking at, so
// there is no eviction policy beyond expiry; a stale-entry sweep keeps it tidy if
// someone moves their radar around.

interface Entry<T> {
  value: T;
  expiresAtMs: number;
}

export class TtlCache<T> {
  private readonly entries = new Map<string, Entry<T>>();

  get(key: string): T | undefined {
    const entry = this.entries.get(key);
    if (entry === undefined) return undefined;
    if (Date.now() >= entry.expiresAtMs) {
      this.entries.delete(key);
      return undefined;
    }
    return entry.value;
  }

  set(key: string, value: T, ttlSeconds: number): void {
    this.entries.set(key, { value, expiresAtMs: Date.now() + ttlSeconds * 1000 });
  }

  /** Drop anything already expired. Cheap; the map holds tens of entries at most. */
  sweep(): void {
    const now = Date.now();
    for (const [key, entry] of this.entries) {
      if (now >= entry.expiresAtMs) this.entries.delete(key);
    }
  }

  get size(): number {
    return this.entries.size;
  }
}
