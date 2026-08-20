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

  /**
   * How many aircraft one device may hold tags on at once.
   *
   * A concurrency cap, not a rate limit: an expired or released tag frees its slot
   * immediately, and refreshing a tag you already hold does not consume another.
   * That makes it behave the way a user expects, where the earlier hourly counter
   * both counted refreshes and never gave slots back.
   */
  maxTagsPerDevice: num('MAX_TAGS_PER_DEVICE', 10),

  /**
   * Hard bound on how many tags a feed response carries, purely to bound its size.
   * Not a claim limit: a claim is never refused because other people hold tags.
   *
   * Must not exceed the firmware's services::adsb::kMaxFeedTags, or the device
   * silently drops the excess. Beyond this many active tags the response carries the
   * most recently claimed ones.
   */
  maxFeedTags: num('MAX_FEED_TAGS', 64),

  /** Registrations allowed per client address per hour. */
  registrationsPerHourPerIp: num('REGISTRATIONS_PER_HOUR_PER_IP', 5),

  /**
   * Minimum gap between requests to adsb.fi, across all clients.
   *
   * This is the one limit that protects something we do not control: adsb.fi allows
   * 1 request/second per IP, and every fetch here comes from a single address. Per
   * client rate limiting cannot enforce it, because one client asking about many
   * different map cells multiplies upstream fetches without making many requests.
   * So the ceiling is enforced where the fetches actually happen.
   */
  upstreamMinIntervalMs: num('UPSTREAM_MIN_INTERVAL_MS', 1000),

  /**
   * How stale a cached cell may be when the upstream limiter refuses a fetch.
   * Slightly old aircraft beat both a failed poll and getting the address
   * restricted.
   */
  maxStaleSeconds: num('MAX_STALE_SECONDS', 60),

  /** Rejects replayed signatures. Generous: the ESP32 has no RTC and drifts. */
  maxClockSkewSeconds: num('MAX_CLOCK_SKEW_SECONDS', 600),

  /** How often expired tags are swept out of the table. */
  sweepIntervalMs: num('SWEEP_INTERVAL_MS', 60_000),

  /** Upstream request timeout. Keeps a slow adsb.fi from stacking up requests. */
  upstreamTimeoutMs: num('UPSTREAM_TIMEOUT_MS', 8000),
} as const;

export const ATTRIBUTION =
  'Aircraft data from adsb.fi (https://adsb.fi/), used under their open data terms.';
