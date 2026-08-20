// Everything tunable, from the environment, with defaults that suit a home server.
//
// The values that were policy on the Cloudflare side are still policy here: the
// firmware renders what it is told, so lock duration and the rest can change with a
// container restart rather than a reflash.

function num(name: string, fallback: number): number {
  const raw = process.env[name];
  if (raw === undefined || raw === '') return fallback;
  const parsed = Number(raw);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function str(name: string, fallback: string): string {
  const raw = process.env[name];
  return raw === undefined || raw === '' ? fallback : raw;
}

export const config = {
  port: num('PORT', 8787),
  /** Bind address. 0.0.0.0 so radars on the LAN can reach it from a container. */
  host: str('HOST', '0.0.0.0'),

  adsbBase: str('ADSB_BASE', 'https://opendata.adsb.fi/api/v3'),

  /** Where the SQLite file lives. Mount this path as a volume to keep handles. */
  dbPath: str('DB_PATH', '/data/tags.db'),

  /** How long a claim holds an aircraft against other devices. */
  lockSeconds: num('LOCK_SECONDS', 1800),

  /**
   * Floor on how often a single map cell can hit adsb.fi, whose public limit is
   * 1 request/second. Self-hosting means that limit applies to your own address
   * rather than a shared pool, but it still applies: with several radars polling
   * every 3s, this cache is what keeps you inside it.
   */
  feedCacheSeconds: num('FEED_CACHE_SECONDS', 4),
  // No tag-block TTL to configure: the store holds one snapshot in memory and
  // invalidates it on write, so a new tag is in the very next feed response. The
  // Cloudflare version needed a TTL there and it cost two or three poll cycles
  // before your own tag showed up on your own radar.

  claimsPerHour: num('CLAIMS_PER_HOUR', 10),
  maxActiveTags: num('MAX_ACTIVE_TAGS', 64),

  /**
   * When set, every request must carry `X-Radar-Key` with this value or it is
   * refused. Empty disables the check.
   *
   * Belt to the proxy's braces. Checking it here as well means the gate still holds
   * if the reverse proxy is reconfigured or bypassed, which is the failure mode that
   * matters: the proxy is the thing most likely to be edited by hand later.
   */
  feedKey: str('FEED_KEY', ''),

  /** Rejects replayed signatures. Generous: the ESP32 has no RTC and drifts. */
  maxClockSkewSeconds: num('MAX_CLOCK_SKEW_SECONDS', 600),

  /** How often expired tags are swept out of the table. */
  sweepIntervalMs: num('SWEEP_INTERVAL_MS', 60_000),

  /** Upstream request timeout. Keeps a slow adsb.fi from stacking up requests. */
  upstreamTimeoutMs: num('UPSTREAM_TIMEOUT_MS', 8000),
} as const;

export const ATTRIBUTION =
  'Aircraft data from adsb.fi (https://adsb.fi/), used under their open data terms.';
