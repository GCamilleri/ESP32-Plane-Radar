import { envNumber, type Env } from './env';
import { aircraftLine, type FeedAircraft } from './protocol';

// Everything here exists to keep the upstream request rate away from adsb.fi's
// public limit of 1 request/second per IP. A Worker aggregates every device behind
// Cloudflare's shared egress addresses, so N devices polling directly through the
// proxy would breach that limit immediately.
//
// The defence is that the A block depends only on a *quantised* location, so all
// devices in the same neighbourhood share one cache entry and therefore one
// upstream fetch. Upstream request rate is bounded by
// (distinct populated cells) / FEED_CACHE_SECONDS rather than by device count.

/** ~5.5 km cells. Coarse enough to pool neighbours, fine enough that the 64-slot cap still lands sensibly. */
const CELL_DEGREES = 0.05;

/** Matches the firmware's services::adsb::kMaxAircraft. */
export const MAX_AIRCRAFT = 64;

export interface FeedRequest {
  lat: number;
  lon: number;
  distNm: number;
  includeGround: boolean;
}

export function parseFeedRequest(url: URL): FeedRequest | null {
  const lat = Number(url.searchParams.get('lat'));
  const lon = Number(url.searchParams.get('lon'));
  const distNm = Number(url.searchParams.get('dist'));
  if (!Number.isFinite(lat) || lat < -90 || lat > 90) return null;
  if (!Number.isFinite(lon) || lon < -180 || lon > 180) return null;
  if (!Number.isFinite(distNm) || distNm <= 0 || distNm > 250) return null;
  return {
    lat,
    lon,
    distNm,
    includeGround: url.searchParams.get('gnd') === '1',
  };
}

function quantise(value: number): number {
  return Math.round(value / CELL_DEGREES) * CELL_DEGREES;
}

/** Short cell identifier for logs, so shared upstream fetches are visible. */
export function cellLabel(req: FeedRequest): string {
  return `${quantise(req.lat).toFixed(2)}/${quantise(req.lon).toFixed(2)}/${Math.ceil(req.distNm)}`;
}

export interface AircraftBlock {
  lines: string[];
  /** 'hit' means this request cost no adsb.fi fetch. */
  cache: 'hit' | 'miss';
}

/**
 * Cache key for the A block. Deliberately not the caller's URL: quantising the
 * centre is what lets neighbouring devices share an upstream fetch.
 */
function cellCacheKey(req: FeedRequest): Request {
  const lat = quantise(req.lat).toFixed(2);
  const lon = quantise(req.lon).toFixed(2);
  const dist = Math.ceil(req.distNm);
  const gnd = req.includeGround ? 1 : 0;
  return new Request(
    `https://feed.cache.invalid/cell?lat=${lat}&lon=${lon}&dist=${dist}&gnd=${gnd}`,
  );
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

/** Mirrors formatAltitudeTag in src/services/adsb_parse.cpp so both paths render alike. */
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

function isOnGround(ac: UpstreamAircraft): boolean {
  return ac.alt_baro === 'ground';
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
 * A block for the requested cell, from the edge cache when possible.
 *
 * Two jobs move off the device here. The response carries only the ten fields the
 * radar actually draws, roughly a fifth of adsb.fi's payload, and the 64 aircraft
 * kept are the 64 *nearest* rather than whatever order the upstream happened to
 * return. The firmware's direct-fetch fallback cannot do either cheaply.
 */
export async function aircraftBlock(req: FeedRequest, env: Env): Promise<AircraftBlock> {
  const cache = caches.default;
  const key = cellCacheKey(req);

  const hit = await cache.match(key);
  if (hit) {
    const text = await hit.text();
    return { lines: text.length > 0 ? text.split('\n') : [], cache: 'hit' };
  }

  const lat = quantise(req.lat).toFixed(4);
  const lon = quantise(req.lon).toFixed(4);
  const dist = Math.ceil(req.distNm);
  const upstream = `${env.ADSB_BASE}/lat/${lat}/lon/${lon}/dist/${dist}`;

  const res = await fetch(upstream, {
    headers: { 'user-agent': 'plane-radar-feed (https://github.com/GCamilleri/ESP32-Plane-Radar)' },
  });
  if (!res.ok) {
    throw new FeedUpstreamError(`adsb.fi returned ${res.status}`, res.status);
  }
  const payload = (await res.json()) as { ac?: UpstreamAircraft[] };

  const centreLat = Number(lat);
  const centreLon = Number(lon);
  const lines = (payload.ac ?? [])
    .filter((ac) => req.includeGround || !isOnGround(ac))
    .map((ac) => ({ ac, d: approxDistSq(ac.lat ?? 0, ac.lon ?? 0, centreLat, centreLon) }))
    .sort((a, b) => a.d - b.d)
    .map((entry) => toFeedAircraft(entry.ac))
    .filter((ac): ac is FeedAircraft => ac !== null)
    .slice(0, MAX_AIRCRAFT)
    .map(aircraftLine);

  const ttl = envNumber(env.FEED_CACHE_SECONDS, 4);
  await cache.put(
    key,
    new Response(lines.join('\n'), {
      headers: {
        'content-type': 'text/plain; charset=utf-8',
        'cache-control': `public, max-age=${ttl}`,
      },
    }),
  );
  return { lines, cache: 'miss' };
}

export class FeedUpstreamError extends Error {
  constructor(message: string, readonly upstreamStatus: number) {
    super(message);
    this.name = 'FeedUpstreamError';
  }
}
