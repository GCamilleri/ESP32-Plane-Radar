import { createHmac, timingSafeEqual } from 'node:crypto';
import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';

import { ATTRIBUTION, config } from './config.ts';
import {
  FeedUpstreamError,
  aircraftBlock,
  cellLabel,
  parseFeedRequest,
  sweepFeedCache,
} from './feed.ts';
import { FixedWindowCounter } from './cache.ts';
import { logFeed, logInfo, logRegister, logRejected, logTag } from './log.ts';
import { headerLine, normaliseIcao } from './protocol.ts';
import { TagStore, type Reply } from './store.ts';

const store = new TagStore();

/**
 * Registration is open by design: anyone should be able to build a radar and use
 * this. Open does not have to mean unbounded, though, and minting identities in
 * bulk is the one thing that would let someone crowd out everyone else's tags.
 */
const registrationLimiter = new FixedWindowCounter(
  3_600_000,
  config.registrationsPerHourPerIp,
);

/** Cap on request bodies. Every legitimate one is well under 200 bytes. */
const MAX_BODY_BYTES = 4096;

function send(res: ServerResponse, status: number, body: string): void {
  const payload = body.endsWith('\n') ? body : `${body}\n`;
  res.writeHead(status, {
    'content-type': 'text/plain; charset=utf-8',
    'cache-control': 'no-store',
    'content-length': Buffer.byteLength(payload),
  });
  res.end(payload);
}

function readBody(req: IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    let total = 0;
    req.on('data', (chunk: Buffer) => {
      total += chunk.length;
      if (total > MAX_BODY_BYTES) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

async function handleFeed(params: URLSearchParams, res: ServerResponse): Promise<void> {
  const req = parseFeedRequest(params);
  if (req === null) {
    logRejected({ route: '/v1/feed', status: 400, reason: 'bad-params' });
    send(res, 400, 'bad lat/lon/dist');
    return;
  }

  const started = Date.now();
  const aircraft = await aircraftBlock(req);
  const tags = store.tagBlock();

  logFeed({
    lat: req.lat,
    lon: req.lon,
    distNm: req.distNm,
    cell: cellLabel(req),
    cache: aircraft.cache,
    ageSeconds: aircraft.ageSeconds,
    aircraft: aircraft.lines.length,
    tags: tags.lines.length,
    ms: Date.now() - started,
  });

  const epoch = Math.floor(Date.now() / 1000);
  send(
    res,
    200,
    [
      headerLine(
        epoch,
        tags.lockSeconds,
        aircraft.lines.length,
        tags.lines.length,
        aircraft.ageMs,
      ),
      ...aircraft.lines,
      ...tags.lines,
    ].join('\n'),
  );
}

function handleRegister(req: IncomingMessage, body: string, res: ServerResponse): void {
  const form = new URLSearchParams(body);
  const deviceId = (form.get('dev') ?? '').trim();
  const secret = (form.get('secret') ?? '').trim();
  const handle = (form.get('handle') ?? '').trim();

  if (!/^[0-9a-f]{12,32}$/.test(deviceId)) {
    logRejected({ route: '/v1/register', status: 400, reason: 'bad-dev' });
    send(res, 400, 'bad dev');
    return;
  }
  if (!/^[0-9a-f]{32,64}$/.test(secret)) {
    logRejected({ route: '/v1/register', status: 400, reason: 'bad-secret', device: deviceId });
    send(res, 400, 'bad secret');
    return;
  }

  // Rate limit new identities only. A device re-registers on every boot, so counting
  // returning devices meant a handful of reboots from one address locked a radar out
  // of tagging with a 429 it could never clear. The limiter exists to stop identities
  // being minted in bulk, and a device already on record with a matching secret is
  // not minting anything.
  if (!store.isKnownDevice(deviceId, secret)) {
    const address = clientAddress(req);
    if (!registrationLimiter.tryAcquire(address)) {
      logRejected({
        route: '/v1/register',
        status: 429,
        reason: 'register-rate-limit',
        device: deviceId,
      });
      send(res, 429, 'too many registrations');
      return;
    }
  }

  const reply = store.register(deviceId, secret, handle);
  logRegister({
    device: deviceId,
    handle: reply.handle ?? handle,
    status: reply.status,
    fresh: reply.fresh === true,
  });
  send(res, reply.status, reply.body);
}

type ClaimAction = 'claim' | 'release' | 'release-all';

function handleClaim(
  req: IncomingMessage,
  body: string,
  action: ClaimAction,
  route: string,
  res: ServerResponse,
): void {
  const auth = verifySignature(req, body, route);
  if ('status' in auth) {
    logRejected({
      route,
      status: auth.status,
      reason: auth.body,
      device: header(req, 'x-radar-device'),
    });
    send(res, auth.status, auth.body);
    return;
  }

  let reply: Reply;
  let icao = '-';
  if (action === 'release-all') {
    reply = store.releaseAll(auth.deviceId);
  } else {
    const parsed = normaliseIcao(new URLSearchParams(body).get('icao'));
    if (parsed === null) {
      logRejected({ route, status: 400, reason: 'bad-icao', device: auth.deviceId });
      send(res, 400, 'bad icao');
      return;
    }
    icao = parsed;
    reply = action === 'claim' ? store.claim(auth.deviceId, icao) : store.release(auth.deviceId, icao);
  }

  logTag({
    action,
    device: auth.deviceId,
    handle: reply.handle,
    icao,
    status: reply.status,
    detail: reply.detail,
  });
  send(res, reply.status, reply.body);
}

/**
 * Client address as the reverse proxy saw it.
 *
 * The *last* X-Forwarded-For entry, not the first: nginx appends the peer address
 * it observed, so a client sending its own X-Forwarded-For only pollutes the
 * earlier entries. Taking the first would let anyone choose their own rate limit
 * bucket.
 */
function clientAddress(req: IncomingMessage): string {
  const forwarded = header(req, 'x-forwarded-for');
  if (forwarded !== '') {
    const parts = forwarded.split(',');
    const last = parts[parts.length - 1];
    if (last !== undefined && last.trim() !== '') return last.trim();
  }
  return req.socket.remoteAddress ?? 'unknown';
}

function header(req: IncomingMessage, name: string): string {
  const value = req.headers[name];
  if (Array.isArray(value)) return (value[0] ?? '').trim();
  return (value ?? '').trim();
}

/**
 * Writes are signed HMAC-SHA256 over method, path, timestamp and body, with the
 * secret the device registered.
 *
 * Not authentication of a person: registration is open, so anyone can mint an
 * identity. It stops one device claiming under another's handle and gives claims a
 * stable subject to rate limit. Reads are unsigned so the firmware's hot path stays
 * simple, which is safe here in a way it was not on Cloudflare: there is no daily
 * request budget for a stranger to burn.
 */
function verifySignature(
  req: IncomingMessage,
  body: string,
  route: string,
): { deviceId: string } | Reply {
  const deviceId = header(req, 'x-radar-device');
  const timestamp = header(req, 'x-radar-ts');
  const signature = header(req, 'x-radar-sig').toLowerCase();

  if (!/^[0-9a-f]{12,32}$/.test(deviceId)) return { status: 400, body: 'bad device header' };
  if (!/^[0-9a-f]{64}$/.test(signature)) return { status: 400, body: 'bad signature header' };

  const ts = Number(timestamp);
  if (!Number.isFinite(ts)) return { status: 400, body: 'bad timestamp header' };
  const skew = Math.abs(Math.floor(Date.now() / 1000) - ts);
  if (skew > config.maxClockSkewSeconds) return { status: 401, body: `clock skew ${skew}s` };

  const secret = store.deviceSecret(deviceId);
  if (secret === null) return { status: 401, body: 'unregistered device' };

  const expected = createHmac('sha256', Buffer.from(secret, 'hex'))
    .update(`POST\n${route}\n${ts}\n${body}`)
    .digest();
  const given = Buffer.from(signature, 'hex');
  if (expected.length !== given.length || !timingSafeEqual(expected, given)) {
    return { status: 401, body: 'bad signature' };
  }
  return { deviceId };
}

const server = createServer((req, res) => {
  // An ESP32 that gives up mid-request must not be able to take the process down.
  // wrangler dev did exactly that, and it is the main reason this exists.
  req.on('error', () => res.destroy());
  res.on('error', () => undefined);

  void (async () => {
    try {
      const url = new URL(req.url ?? '/', 'http://internal');
      const route = `${req.method ?? 'GET'} ${url.pathname}`;

      if (route === 'GET /healthz') {
        send(res, 200, 'ok');
        return;
      }
      if (route === 'GET /') {
        send(res, 200, usage());
        return;
      }
      if (route === 'GET /v1/feed') {
        await handleFeed(url.searchParams, res);
        return;
      }
      if (route === 'GET /v1/tags') {
        const tags = store.tagBlock();
        send(
          res,
          200,
          [
            headerLine(Math.floor(Date.now() / 1000), tags.lockSeconds, 0, tags.lines.length),
            ...tags.lines,
          ].join('\n'),
        );
        return;
      }

      if (req.method !== 'POST') {
        send(res, 404, 'not found');
        return;
      }

      const body = await readBody(req);
      switch (url.pathname) {
        case '/v1/register':
          handleRegister(req, body, res);
          return;
        case '/v1/tag':
          handleClaim(req, body, 'claim', '/v1/tag', res);
          return;
        case '/v1/untag':
          handleClaim(req, body, 'release', '/v1/untag', res);
          return;
        case '/v1/untagall':
          handleClaim(req, body, 'release-all', '/v1/untagall', res);
          return;
        default:
          send(res, 404, 'not found');
          return;
      }
    } catch (err) {
      if (err instanceof FeedUpstreamError) {
        // 502 rather than 500: the firmware treats it as "proxy unhealthy" and
        // falls back to fetching adsb.fi directly.
        send(res, 502, `upstream: ${err.message}`);
        return;
      }
      send(res, 500, err instanceof Error ? err.message : 'error');
    }
  })();
});

function usage(): string {
  return [
    'plane-radar-feed (self-hosted)',
    '',
    'GET  /v1/feed?lat=&lon=&dist=&gnd=  aircraft + active tags, PR1 format',
    'GET  /v1/tags                       active tags only',
    'GET  /healthz                       liveness',
    'POST /v1/register                   dev, secret, handle',
    'POST /v1/tag                        icao (signed)',
    'POST /v1/untag                      icao (signed)',
    'POST /v1/untagall                   (signed)',
    '',
    ATTRIBUTION,
  ].join('\n');
}

const sweeper = setInterval(() => {
  const removed = store.sweep();
  sweepFeedCache();
  registrationLimiter.sweep();
  if (removed > 0) logInfo('sweep', { expired: removed });
}, config.sweepIntervalMs);
sweeper.unref();

server.listen(config.port, config.host, () => {
  logInfo('listening', {
    host: config.host,
    port: config.port,
    db: config.dbPath,
    lock: config.lockSeconds,
  });
});

for (const signal of ['SIGTERM', 'SIGINT'] as const) {
  process.on(signal, () => {
    logInfo('shutdown', { signal });
    server.close(() => {
      store.close();
      process.exit(0);
    });
    // Don't let a hung keep-alive connection block the container from stopping.
    setTimeout(() => process.exit(0), 3000).unref();
  });
}
