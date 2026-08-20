import { config } from './config.ts';
import { MinIntervalLimiter, TtlCache } from './cache.ts';
import { aircraftLine, type FeedAircraft } from './protocol.ts';

// Self-hosting removes the request cap that shaped the Cloudflare version, but it
// does not remove adsb.fi's limit of 1 request/second per IP. It just makes that
// limit yours rather than a shared pool's, which is an improvement and still a
// limit.
//
// Two mechanisms keep us inside it. The aircraft block is keyed on a quantised
// centre, so every radar in the same neighbourhood shares one cache entry and one
// upstream fetch. And a shared minimum-interval limiter caps the fetch rate outright,
// because cell sharing alone does not: one client asking about many different cells
// multiplies fetches without making many requests. When the limiter refuses, a
// slightly stale cell is served instead, which beats failing the poll and beats
// getting the address restricted.

/** ~5.5 km cells: coarse enough to pool neighbours, fine enough for the 64 slots. */
const CELL_DEGREES = 0.05;

/** Matches the firmware's services::adsb::kMaxAircraft. */
export const MAX_AIRCRAFT = 64;

const cellCache = new TtlCache<string[]>();
const upstreamLimiter = new MinIntervalLimiter(config.upstreamMinIntervalMs);

export interface FeedRequest {
  lat: number;
  lon: number;
  distNm: number;
  includeGround: boolean;
}

export interface AircraftBlock {
  lines: string[];
  /**
   * How this was served. 'hit' cost no adsb.fi fetch; 'stale' means the upstream
   * limiter refused and a slightly old cell was used instead.
   */
  cache: 'hit' | 'miss' | 'stale';
  /**
   * How old these positions are, in ms. Goes out in the PR1 header: pooling radars
   * onto one fetch per cell is what keeps us inside adsb.fi's rate limit, and this
   * is what stops that pooling showing up as a stutter on the radars.
   */
  ageMs: number;
  /** Seconds out of date, when `cache` is 'stale'. */
  ageSeconds?: number;
}

export class FeedUpstreamError extends Error {
  // Written out longhand rather than as a constructor parameter property: Node runs
  // these .ts files by stripping types, and parameter properties are one of the few
  // things that mode cannot express. It fails at startup, not at build time.
  readonly upstreamStatus: number;

  constructor(message: string, upstreamStatus: number) {
    super(message);
    this.name = 'FeedUpstreamError';
    this.upstreamStatus = upstreamStatus;
  }
}

export function parseFeedRequest(params: URLSearchParams): FeedRequest | null {
  const lat = Number(params.get('lat'));
  const lon = Number(params.get('lon'));
  const distNm = Number(params.get('dist'));
  if (!Number.isFinite(lat) || lat < -90 || lat > 90) return null;
  if (!Number.isFinite(lon) || lon < -180 || lon > 180) return null;
  if (!Number.isFinite(distNm) || distNm <= 0 || distNm > 250) return null;
  return { lat, lon, distNm, includeGround: params.get('gnd') === '1' };
}

function quantise(value: number): number {
  return Math.round(value / CELL_DEGREES) * CELL_DEGREES;
}

/** Short cell identifier, also the cache key, so shared fetches show up in logs. */
export function cellLabel(req: FeedRequest): string {
  const gnd = req.includeGround ? 1 : 0;
  return `${quantise(req.lat).toFixed(2)}/${quantise(req.lon).toFixed(2)}/${Math.ceil(req.distNm)}/${gnd}`;
}

interface UpstreamAircraft {
  hex?: string;
  flight?: string;
  t?: string;
  lat?: number;
  lon?: number;
  gs?: number;
  tas?: number;
  ias?: number;
  track?: number;
  true_heading?: number;
  mag_heading?: number;
  dir?: number;
  alt_baro?: number | string;
  alt_geom?: number;
}

function firstNumber(...values: unknown[]): number {
  for (const v of values) {
    if (typeof v === 'number' && Number.isFinite(v)) return v;
  }
  return 0;
}

/** Mirrors formatAltitudeTag in src/services/adsb_parse.cpp so both paths agree. */
function formatAltitude(ac: UpstreamAircraft): string {
  if (ac.alt_baro === 'ground') return 'GND';
  if (typeof ac.alt_baro === 'number' && Number.isFinite(ac.alt_baro)) {
    return `${Math.round(ac.alt_baro)} ft`;
  }
  if (typeof ac.alt_geom === 'number' && Number.isFinite(ac.alt_geom)) {
    return `${Math.round(ac.alt_geom)} ft`;
  }
  return '';
}

/** Equirectangular is plenty for ordering aircraft inside a 250 nm circle. */
function approxDistSq(lat: number, lon: number, centreLat: number, centreLon: number): number {
  const dLat = lat - centreLat;
  const dLon = (lon - centreLon) * Math.cos((centreLat * Math.PI) / 180);
  return dLat * dLat + dLon * dLon;
}

function toFeedAircraft(ac: UpstreamAircraft): FeedAircraft | null {
  if (typeof ac.lat !== 'number' || typeof ac.lon !== 'number') return null;
  const hex = (ac.hex ?? '').trim().toUpperCase();
  if (!/^[0-9A-F]{6}$/.test(hex)) return null;
  const callsign = (ac.flight ?? '').trim();
  return {
    hex,
    lat: ac.lat,
    lon: ac.lon,
    nose: firstNumber(ac.true_heading, ac.mag_heading, ac.track, ac.dir),
    track: firstNumber(ac.track, ac.true_heading, ac.mag_heading, ac.dir),
    gs: firstNumber(ac.gs, ac.tas, ac.ias),
    callsign: callsign.length > 0 ? callsign : hex,
    type: (ac.t ?? '').trim(),
    alt: formatAltitude(ac),
  };
}

/**
 * A block for the requested cell, from cache when possible.
 *
 * Two jobs stay off the device here: the response carries only the ten fields the
 * radar draws, roughly a fifth of adsb.fi's payload, and the 64 aircraft kept are
 * the 64 *nearest* rather than whatever order the upstream returned. The firmware's
 * direct-fetch fallback cannot do either cheaply.
 */
export async function aircraftBlock(req: FeedRequest): Promise<AircraftBlock> {
  const key = cellLabel(req);
  const cached = cellCache.get(key);
  if (cached !== undefined) return { lines: cached.value, cache: 'hit', ageMs: cached.ageMs };

  // Cache miss, so this would cost an upstream fetch. If that would breach the
  // interval, fall back to whatever we last had for this cell.
  if (!upstreamLimiter.tryAcquire()) {
    const stale = cellCache.getStale(key, config.maxStaleSeconds);
    if (stale !== undefined) {
      return {
        lines: stale.value,
        cache: 'stale',
        ageMs: stale.ageMs,
        ageSeconds: Math.round(stale.staleMs / 1000),
      };
    }
    throw new FeedUpstreamError('upstream rate limited and no cached cell', 0);
  }

  const lat = quantise(req.lat).toFixed(4);
  const lon = quantise(req.lon).toFixed(4);
  const dist = Math.ceil(req.distNm);
  const url = `${config.adsbBase}/lat/${lat}/lon/${lon}/dist/${dist}`;

  let payload: { ac?: UpstreamAircraft[] };
  try {
    const res = await fetch(url, {
      headers: {
        'user-agent': 'plane-radar-feed (https://github.com/GCamilleri/ESP32-Plane-Radar)',
      },
      signal: AbortSignal.timeout(config.upstreamTimeoutMs),
    });
    if (!res.ok) throw new FeedUpstreamError(`adsb.fi returned ${res.status}`, res.status);
    payload = (await res.json()) as { ac?: UpstreamAircraft[] };
  } catch (err) {
    if (err instanceof FeedUpstreamError) throw err;
    const reason = err instanceof Error ? err.message : 'unknown';
    throw new FeedUpstreamError(`adsb.fi unreachable: ${reason}`, 0);
  }

  const centreLat = Number(lat);
  const centreLon = Number(lon);
  const lines = (payload.ac ?? [])
    .filter((ac) => req.includeGround || ac.alt_baro !== 'ground')
    .map((ac) => ({ ac, d: approxDistSq(ac.lat ?? 0, ac.lon ?? 0, centreLat, centreLon) }))
    .sort((a, b) => a.d - b.d)
    .map((entry) => toFeedAircraft(entry.ac))
    .filter((ac): ac is FeedAircraft => ac !== null)
    .slice(0, MAX_AIRCRAFT)
    .map(aircraftLine);

  cellCache.set(key, lines, config.feedCacheSeconds);
  // Age 0: whatever adsb.fi's own latency is, we cannot see it from here, and the
  // one part we can account for is how long we then held the block ourselves.
  return { lines, cache: 'miss', ageMs: 0 };
}

export function sweepFeedCache(): void {
  cellCache.sweep(config.maxStaleSeconds);
}
