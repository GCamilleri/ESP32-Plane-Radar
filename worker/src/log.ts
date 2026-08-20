// One line per event, key=value, so `wrangler dev` output and `wrangler tail`
// output are both greppable without a parser. Fields are ordered widest-to-narrowest
// scope: what happened, who did it, what to.

function fields(pairs: Record<string, unknown>): string {
  const parts: string[] = [];
  for (const [key, value] of Object.entries(pairs)) {
    if (value === undefined || value === null || value === '') continue;
    parts.push(`${key}=${value}`);
  }
  return parts.join(' ');
}

/** A served feed request. `cache` says whether it cost an adsb.fi fetch. */
export function logFeed(opts: {
  lat: number;
  lon: number;
  distNm: number;
  cell: string;
  cache: 'hit' | 'miss';
  aircraft: number;
  tags: number;
  ms: number;
}): void {
  console.log(
    `feed ${fields({
      cell: opts.cell,
      lat: opts.lat.toFixed(4),
      lon: opts.lon.toFixed(4),
      dist: opts.distNm,
      upstream: opts.cache === 'miss' ? 'fetch' : 'cached',
      ac: opts.aircraft,
      tags: opts.tags,
      ms: opts.ms,
    })}`,
  );
}

/** A tag claimed, refreshed, released, or refused. */
export function logTag(opts: {
  action: 'claim' | 'release' | 'release-all';
  device: string;
  handle?: string;
  icao: string;
  status: number;
  detail?: string;
}): void {
  const verdict = opts.status === 200 ? 'ok' : 'refused';
  console.log(
    `tag ${fields({
      action: opts.action,
      result: verdict,
      status: opts.status,
      device: opts.device,
      handle: opts.handle,
      icao: opts.icao,
      detail: opts.detail,
    })}`,
  );
}

export function logRegister(opts: {
  device: string;
  handle: string;
  status: number;
  fresh: boolean;
}): void {
  console.log(
    `register ${fields({
      result: opts.status === 200 ? 'ok' : 'refused',
      status: opts.status,
      device: opts.device,
      handle: opts.handle,
      state: opts.fresh ? 'new' : 'returning',
    })}`,
  );
}

export function logRejected(opts: {
  route: string;
  status: number;
  reason: string;
  device?: string;
}): void {
  console.log(`rejected ${fields(opts)}`);
}
