// Covers the half of the feed that needs no network: which cell a request lands in,
// how wide that cell is fetched, and which aircraft come back for a given radar.
//
// Run with `node --test test/` from server/. These are the rules that decide whether
// a radar sees the aircraft it should, and they are pure functions, so there is no
// excuse for leaving them to on-device observation.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { MAX_AIRCRAFT, cellLabel, fetchRadiusNm, selectLines, type FeedRequest } from '../src/feed.ts';
import { aircraftLine, type FeedAircraft } from '../src/protocol.ts';

const KM_PER_DEG = 111.32;
const KM_PER_NM = 1.852;

function req(overrides: Partial<FeedRequest> = {}): FeedRequest {
  return { lat: -36.827, lon: 174.6155, distNm: 19.8, includeGround: false, ...overrides };
}

function plane(hex: string, lat: number, lon: number, onGround = false) {
  const aircraft: FeedAircraft = {
    hex,
    lat,
    lon,
    nose: 0,
    track: 0,
    gs: 300,
    callsign: hex,
    type: 'B738',
    alt: onGround ? 'GND' : '10000 ft',
  };
  return { aircraft, onGround };
}

/** A point `km` due north of (lat, lon). */
function north(lat: number, km: number): number {
  return lat + km / KM_PER_DEG;
}

test('the requested radius does not split a cell', () => {
  // Two radars side by side on different range presets: 5 km and 25 km.
  const small = req({ lat: -36.8271, lon: 174.6157, distNm: 4 });
  const large = req({ lat: -36.827, lon: 174.6155, distNm: 19.8 });
  assert.equal(cellLabel(small), cellLabel(large));
});

test('the ground flag does not split a cell', () => {
  assert.equal(cellLabel(req({ includeGround: true })), cellLabel(req({ includeGround: false })));
});

test('a cell is fetched wide enough to cover a radar at its corner', () => {
  // Worst case: the radar sits half a cell diagonal from the centre it shares, so
  // its own circle reaches that much further out than the centre's.
  const halfDiagonalNm = (Math.SQRT2 * 0.025 * KM_PER_DEG) / KM_PER_NM;
  const r = req({ distNm: 19.8 });
  assert.ok(
    fetchRadiusNm(r) >= r.distNm + halfDiagonalNm,
    `${fetchRadiusNm(r)} must cover ${r.distNm} + ${halfDiagonalNm}`,
  );
});

test('an unusually large request gets its own entry rather than a short answer', () => {
  const huge = req({ distNm: 200 });
  assert.ok(fetchRadiusNm(huge) >= 200);
  assert.notEqual(cellLabel(huge), cellLabel(req()));
});

test('aircraft beyond the requested radius are left out', () => {
  const r = req({ distNm: 10 });
  const cell = [
    plane('AAAAAA', north(r.lat, 9), r.lon), // 9 km, inside 10 nm
    plane('BBBBBB', north(r.lat, 30), r.lon), // 30 km, outside
  ];
  const lines = selectLines(r, cell);
  assert.equal(lines.length, 1);
  assert.match(lines[0]!, /AAAAAA/);
});

test('distance is measured from the radar, not from the cell centre', () => {
  // This radar sits near the north edge of its cell. An aircraft 4 km further north
  // is close to the radar and comparatively far from the centre; one 4 km south of
  // the centre is the other way round. Ordering must follow the radar.
  const r = req({ lat: -36.8251, lon: 174.6249, distNm: 10 });
  const near = plane('NEAR01', north(r.lat, 4), r.lon);
  const far = plane('FAR001', north(-36.85, -4), 174.6);
  const lines = selectLines(r, [far, near]);
  assert.match(lines[0]!, /NEAR01/);
});

test('the ground filter is applied per request', () => {
  const r = req({ distNm: 10 });
  const cell = [plane('GND001', north(r.lat, 2), r.lon, true)];
  assert.equal(selectLines(r, cell).length, 0);
  assert.equal(selectLines({ ...r, includeGround: true }, cell).length, 1);
});

test('the response is capped at what the firmware can hold, keeping the nearest', () => {
  const r = req({ distNm: 100 });
  const cell = [];
  // Furthest first, so a cap that ignored distance would keep the wrong ones.
  for (let i = MAX_AIRCRAFT + 20; i > 0; i--) {
    cell.push(plane(`A${i.toString().padStart(5, '0')}`, north(r.lat, i), r.lon));
  }
  const lines = selectLines(r, cell);
  assert.equal(lines.length, MAX_AIRCRAFT);
  assert.equal(lines[0], aircraftLine(plane('A00001', north(r.lat, 1), r.lon).aircraft));
  assert.ok(!lines.some((line) => /A000(8|9)/.test(line)));
});

test('an empty cell yields an empty block rather than throwing', () => {
  assert.deepEqual(selectLines(req(), []), []);
});
