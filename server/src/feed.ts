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
//
// What is cached per cell is the parsed aircraft, not the rendered lines, and the
// cell is fetched at one canonical radius rather than at whatever radius asked for
// it. Three things follow, all of which were wrong when the radius was part of the
// key and the lines were cached whole:
//
//   - Two radars in the same place on different range settings share one fetch. The
//     smaller circle is a subset of the larger, so fetching both was pure waste.
//   - A device gets its own circle, not the cell's. A radar can sit up to half a
//     cell diagonal from the centre it shares, so the far edge of its range used to
//     fall outside what was ever fetched and those aircraft simply never appeared.
//   - The 64 slots go to the 64 nearest *the device*. Nearest the cell centre is a
//     different set, and in dense airspace that costs a device its closest traffic.

/** ~5.5 km cells: coarse enough to pool neighbours, fine enough for the 64 slots. */
const CELL_DEGREES = 0.05;

/**
 * Furthest a device can sit from its cell's centre, in nm: half the cell diagonal,
 * taken at the equator where a degree of longitude is widest.
 */
const CELL_PAD_NM = (Math.SQRT2 * (CELL_DEGREES / 2) * 111.32) / 1.852;

/**
 * Radius every cell is fetched at. Comfortably clears the largest range preset's
 * ~19.8 nm plus CELL_PAD_NM, so in practice every radar shares one entry per cell
 * whatever its range. A request for more than this gets its own entry rather than
 * being quietly served short.
 */
const CANONICAL_DIST_NM = 25;

/**
 * Bound on aircraft held per cell. Only reached over somewhere like Heathrow, and
 * well above the 64 any one response can carry.
 */
const MAX_CACHED_AIRCRAFT = 250;

/** Matches the firmware's services::adsb::kMaxAircraft. */
export const MAX_AIRCRAFT = 64;

/** A parsed upstream aircraft, kept ready to render for any radius in the cell. */
interface CellAircraft {
  aircraft: FeedAircraft;
  onGround: boolean;
}

const cellCache = new TtlCache<CellAircraft[]>();
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

/** Radius this request's cell is fetched at. The canonical one unless asked for more. */
export function fetchRadiusNm(req: FeedRequest): number {
  return Math.max(CANONICAL_DIST_NM, Math.ceil(req.distNm + CELL_PAD_NM));
}

/**
 * Short cell identifier, also the cache key, so shared fetches show up in the log.
 *
 * Neither the requested radius nor the ground flag appears here any more: both are
 * applied when the response is built, so radars differing only in those share the
 * one fetch.
 */
export function cellLabel(req: FeedRequest): string {
  return `${quantise(req.lat).toFixed(2)}/${quantise(req.lon).toFixed(2)}/${fetchRadiusNm(req)}`;
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

const DEG_TO_NM = 111.32 / 1.852;

function approxDistNm(lat: number, lon: number, fromLat: number, fromLon: number): number {
  return Math.sqrt(approxDistSq(lat, lon, fromLat, fromLon)) * DEG_TO_NM;
}

/**
 * The lines for one request, taken from its cell's aircraft.
 *
 * Measured from where the radar actually is rather than from the cell centre it
 * shares, so it gets its own circle and its own nearest 64. Exported for the tests:
 * this is the whole per-request half of the feed and it needs no network.
 */
export function selectLines(req: FeedRequest, cell: CellAircraft[]): string[] {
  return cell
    .filter((entry) => req.includeGround || !entry.onGround)
    .map((entry) => ({
      entry,
      d: approxDistNm(entry.aircraft.lat, entry.aircraft.lon, req.lat, req.lon),
    }))
    .filter((scored) => scored.d <= req.distNm)
    .sort((a, b) => a.d - b.d)
    .slice(0, MAX_AIRCRAFT)
    .map((scored) => aircraftLine(scored.entry.aircraft));
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
 * A block for the requesting radar, from its cell's cache when possible.
 *
 * Two jobs stay off the device here: the response carries only the ten fields the
 * radar draws, roughly a fifth of adsb.fi's payload, and the 64 aircraft kept are
 * the 64 *nearest that radar* rather than whatever order the upstream returned. The
 * firmware's direct-fetch fallback cannot do either cheaply.
 */
export async function aircraftBlock(req: FeedRequest): Promise<AircraftBlock> {
  const key = cellLabel(req);
  const cached = cellCache.get(key);
  if (cached !== undefined) {
    return { lines: selectLines(req, cached.value), cache: 'hit', ageMs: cached.ageMs };
  }

  // Cache miss, so this would cost an upstream fetch. If that would breach the
  // interval, fall back to whatever we last had for this cell.
  if (!upstreamLimiter.tryAcquire()) {
    const stale = cellCache.getStale(key, config.maxStaleSeconds);
    if (stale !== undefined) {
      return {
        lines: selectLines(req, stale.value),
        cache: 'stale',
        ageMs: stale.ageMs,
        ageSeconds: Math.round(stale.staleMs / 1000),
      };
    }
    throw new FeedUpstreamError('upstream rate limited and no cached cell', 0);
  }

  const lat = quantise(req.lat).toFixed(4);
  const lon = quantise(req.lon).toFixed(4);
  const url = `${config.adsbBase}/lat/${lat}/lon/${lon}/dist/${fetchRadiusNm(req)}`;

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

  // Ground aircraft are kept here and filtered per request: gnd is no longer part of
  // the key, so this one entry has to be able to answer either way.
  const centreLat = Number(lat);
  const centreLon = Number(lon);
  const cell: CellAircraft[] = (payload.ac ?? [])
    .map((ac) => ({ ac, d: approxDistSq(ac.lat ?? 0, ac.lon ?? 0, centreLat, centreLon) }))
    .sort((a, b) => a.d - b.d)
    .slice(0, MAX_CACHED_AIRCRAFT)
    .map((entry) => {
      const aircraft = toFeedAircraft(entry.ac);
      return aircraft === null
        ? null
        : { aircraft, onGround: entry.ac.alt_baro === 'ground' };
    })
    .filter((entry): entry is CellAircraft => entry !== null);

  cellCache.set(key, cell, config.feedCacheSeconds);
  // Age 0: whatever adsb.fi's own latency is, we cannot see it from here, and the
  // one part we can account for is how long we then held the block ourselves.
  return { lines: selectLines(req, cell), cache: 'miss', ageMs: 0 };
}

export function sweepFeedCache(): void {
  cellCache.sweep(config.maxStaleSeconds);
}
