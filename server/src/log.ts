// One line per event, key=value, so `docker logs` output is greppable without a
// parser. Fields run widest-to-narrowest scope: what happened, who did it, to what.

function fields(pairs: Record<string, unknown>): string {
  const parts: string[] = [];
  for (const [key, value] of Object.entries(pairs)) {
    if (value === undefined || value === null || value === '') continue;
    parts.push(`${key}=${value}`);
  }
  return parts.join(' ');
}

function stamp(): string {
  // Container logs are usually read without a timestamp collector in front, and a
  // tag with no time is close to useless when diagnosing "it stopped working".
  return new Date().toISOString().replace('T', ' ').slice(0, 19);
}

function emit(kind: string, pairs: Record<string, unknown>): void {
  console.log(`${stamp()} ${kind} ${fields(pairs)}`);
}

/** A served feed request. `upstream` says whether it cost an adsb.fi fetch. */
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
  emit('feed', {
    cell: opts.cell,
    lat: opts.lat.toFixed(4),
    lon: opts.lon.toFixed(4),
    dist: opts.distNm,
    upstream: opts.cache === 'miss' ? 'fetch' : 'cached',
    ac: opts.aircraft,
    tags: opts.tags,
    ms: opts.ms,
  });
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
  emit('tag', {
    action: opts.action,
    result: opts.status === 200 ? 'ok' : 'refused',
    status: opts.status,
    device: opts.device,
    handle: opts.handle,
    icao: opts.icao,
    detail: opts.detail,
  });
}

export function logRegister(opts: {
  device: string;
  handle: string;
  status: number;
  fresh: boolean;
}): void {
  emit('register', {
    result: opts.status === 200 ? 'ok' : 'refused',
    status: opts.status,
    device: opts.device,
    handle: opts.handle,
    state: opts.fresh ? 'new' : 'returning',
  });
}

export function logRejected(opts: {
  route: string;
  status: number;
  reason: string;
  device?: string;
}): void {
  emit('rejected', opts);
}

export function logInfo(message: string, pairs: Record<string, unknown> = {}): void {
  emit(message, pairs);
}
