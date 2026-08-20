// PR1 wire format, shared by the feed builder and mirrored by the firmware parser
// in src/services/adsb_feed.cpp. Line oriented and comma separated so the ESP32
// can parse it with strtoul/strtof into a fixed array and allocate nothing.
//
//   PR1 <server_epoch> <lock_seconds> <aircraft_count> <tag_count>
//   A,<hex>,<lat>,<lon>,<nose>,<track>,<gs>,<callsign>,<type>,<alt>
//   T,<hex>,<handle>,<ttl_seconds>
//
// Field notes:
//   hex       6 uppercase hex digits, the ICAO 24-bit address
//   lat/lon   5 decimal places, about 1 m
//   nose      degrees, integer, nose/heading for the symbol
//   track     degrees, integer, direction of the speed vector
//   gs        knots, integer
//   callsign  up to 8 chars, falls back to hex when the feed has no flight id
//   type      up to 4 chars, ICAO type designator, may be empty
//   alt       pre-formatted, "12000 ft" or "GND", may be empty
//   handle    3 or 4 chars identifying the device that claimed the tag
//
// The A block depends only on the quantised location and the T block on nothing at
// all, so both are byte-identical for every device and cache cleanly at the edge.
// Nothing device-specific appears in the response: the firmware decides which tags
// are its own by comparing the handle against its own.

export const PROTOCOL_VERSION = 'PR1';

export interface FeedAircraft {
  hex: string;
  lat: number;
  lon: number;
  nose: number;
  track: number;
  gs: number;
  callsign: string;
  type: string;
  alt: string;
}

export interface FeedTag {
  icao: string;
  handle: string;
  ttl: number;
}

/** Strip anything that would break the line format or the firmware's field widths. */
export function sanitiseField(value: unknown, maxLen: number): string {
  if (typeof value !== 'string') return '';
  let out = '';
  for (const ch of value) {
    const code = ch.charCodeAt(0);
    if (code < 0x20 || code > 0x7e) continue; // control chars and non-ASCII
    if (ch === ',' || ch === '\n' || ch === '\r') continue;
    out += ch;
  }
  return out.trim().slice(0, maxLen);
}

function num(value: unknown, decimals: number): string {
  const n = Number(value);
  if (!Number.isFinite(n)) return decimals > 0 ? (0).toFixed(decimals) : '0';
  return decimals > 0 ? n.toFixed(decimals) : String(Math.round(n));
}

export function aircraftLine(ac: FeedAircraft): string {
  return [
    'A',
    ac.hex,
    num(ac.lat, 5),
    num(ac.lon, 5),
    num(ac.nose, 0),
    num(ac.track, 0),
    num(ac.gs, 0),
    sanitiseField(ac.callsign, 8),
    sanitiseField(ac.type, 4),
    sanitiseField(ac.alt, 11),
  ].join(',');
}

export function tagLine(tag: FeedTag): string {
  return ['T', tag.icao, sanitiseField(tag.handle, 4), num(tag.ttl, 0)].join(',');
}

export function headerLine(
  epoch: number,
  lockSeconds: number,
  aircraftCount: number,
  tagCount: number,
): string {
  return `${PROTOCOL_VERSION} ${epoch} ${lockSeconds} ${aircraftCount} ${tagCount}`;
}

/** A handle is 3-4 chars of [A-Z0-9]. Returns null when the input cannot be coerced. */
export function normaliseHandle(raw: unknown): string | null {
  if (typeof raw !== 'string') return null;
  const cleaned = raw.toUpperCase().replace(/[^A-Z0-9]/g, '').slice(0, 4);
  return cleaned.length >= 3 ? cleaned : null;
}

/** Six uppercase hex digits, or null. */
export function normaliseIcao(raw: unknown): string | null {
  if (typeof raw !== 'string') return null;
  const cleaned = raw.trim().toUpperCase();
  return /^[0-9A-F]{6}$/.test(cleaned) ? cleaned : null;
}
