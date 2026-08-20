import { envNumber, type Env, type Reply } from './env';
import { FeedUpstreamError, aircraftBlock, cellLabel, parseFeedRequest } from './feed';
import { logFeed, logRegister, logRejected, logTag } from './log';
import { headerLine, normaliseIcao } from './protocol';
import { TagRegistry, type TagBlock } from './tag_registry';

export { TagRegistry };

/** Rejects replayed signatures. Generous because the ESP32 has no RTC and drifts. */
const MAX_CLOCK_SKEW_SEC = 600;

const ATTRIBUTION =
  'Aircraft data from adsb.fi (https://adsb.fi/), used under their open data terms.';

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    try {
      switch (`${request.method} ${url.pathname}`) {
        case 'GET /':
          return text(200, [
            'plane-radar-feed',
            '',
            'GET  /v1/feed?lat=&lon=&dist=&gnd=  aircraft + active tags, PR1 format',
            'GET  /v1/tags                       active tags only',
            'POST /v1/register                   dev, secret, handle',
            'POST /v1/tag                        icao (signed)',
            'POST /v1/untag                      icao (signed)',
            '',
            ATTRIBUTION,
          ].join('\n'));

        case 'GET /v1/feed':
          return await handleFeed(url, env);

        case 'GET /v1/tags':
          return await handleTags(env);

        case 'POST /v1/register':
          return await handleRegister(request, env);

        case 'POST /v1/tag':
          return await handleClaim(request, env, 'claim');

        case 'POST /v1/untag':
          return await handleClaim(request, env, 'release');

        default:
          return text(404, 'not found');
      }
    } catch (err) {
      if (err instanceof FeedUpstreamError) {
        // 502 rather than 500: the firmware treats it as "proxy unhealthy" and
        // falls back to fetching adsb.fi directly.
        return text(502, `upstream: ${err.message}`);
      }
      return text(500, err instanceof Error ? err.message : 'error');
    }
  },
} satisfies ExportedHandler<Env>;

function registry(env: Env) {
  // One object holds every tag. Serialised access is the point; at hobby scale the
  // throughput of a single DO is not remotely a constraint.
  return env.TAGS.get(env.TAGS.idFromName('global'));
}

function text(status: number, body: string): Response {
  return new Response(`${body}\n`, {
    status,
    headers: {
      'content-type': 'text/plain; charset=utf-8',
      'cache-control': 'no-store',
    },
  });
}

/**
 * The T block is identical for every device, so it is cached at the edge too. That
 * matters for more than latency: Durable Object requests have their own 100k/day
 * free allowance, and hitting the DO once per feed poll would drain two separate
 * budgets in step.
 */
async function cachedTagBlock(env: Env): Promise<TagBlock> {
  const cache = caches.default;
  const key = new Request('https://feed.cache.invalid/tags');

  const hit = await cache.match(key);
  if (hit) {
    const lockSeconds = Number(hit.headers.get('x-lock-seconds'));
    const body = await hit.text();
    return {
      lines: body.length > 0 ? body.split('\n') : [],
      lockSeconds: Number.isFinite(lockSeconds) ? lockSeconds : envNumber(env.LOCK_SECONDS, 1800),
    };
  }

  const block = await registry(env).tagBlock();
  const ttl = envNumber(env.TAG_CACHE_SECONDS, 5);
  await cache.put(
    key,
    new Response(block.lines.join('\n'), {
      headers: {
        'content-type': 'text/plain; charset=utf-8',
        'cache-control': `public, max-age=${ttl}`,
        'x-lock-seconds': String(block.lockSeconds),
      },
    }),
  );
  return block;
}

async function handleFeed(url: URL, env: Env): Promise<Response> {
  const req = parseFeedRequest(url);
  if (req === null) {
    logRejected({ route: '/v1/feed', status: 400, reason: 'bad-params' });
    return text(400, 'bad lat/lon/dist');
  }

  const started = Date.now();
  const [aircraft, tags] = await Promise.all([aircraftBlock(req, env), cachedTagBlock(env)]);

  logFeed({
    lat: req.lat,
    lon: req.lon,
    distNm: req.distNm,
    cell: cellLabel(req),
    cache: aircraft.cache,
    aircraft: aircraft.lines.length,
    tags: tags.lines.length,
    ms: Date.now() - started,
  });

  const epoch = Math.floor(Date.now() / 1000);
  const body = [
    headerLine(epoch, tags.lockSeconds, aircraft.lines.length, tags.lines.length),
    ...aircraft.lines,
    ...tags.lines,
  ].join('\n');

  return new Response(`${body}\n`, {
    headers: {
      'content-type': 'text/plain; charset=utf-8',
      'cache-control': 'no-store',
      'x-attribution': ATTRIBUTION,
    },
  });
}

async function handleTags(env: Env): Promise<Response> {
  const tags = await cachedTagBlock(env);
  return text(200, [
    headerLine(Math.floor(Date.now() / 1000), tags.lockSeconds, 0, tags.lines.length),
    ...tags.lines,
  ].join('\n'));
}

async function handleRegister(request: Request, env: Env): Promise<Response> {
  const form = new URLSearchParams(await request.text());
  const deviceId = (form.get('dev') ?? '').trim();
  const secret = (form.get('secret') ?? '').trim();
  const handle = (form.get('handle') ?? '').trim();

  if (!/^[0-9a-f]{12,32}$/.test(deviceId)) {
    logRejected({ route: '/v1/register', status: 400, reason: 'bad-dev' });
    return text(400, 'bad dev');
  }
  if (!/^[0-9a-f]{32,64}$/.test(secret)) {
    logRejected({ route: '/v1/register', status: 400, reason: 'bad-secret', device: deviceId });
    return text(400, 'bad secret');
  }

  const reply = await registry(env).register(deviceId, secret, handle);
  logRegister({
    device: deviceId,
    handle: reply.handle ?? handle,
    status: reply.status,
    fresh: reply.fresh === true,
  });
  return text(reply.status, reply.body);
}

async function handleClaim(
  request: Request,
  env: Env,
  action: 'claim' | 'release',
): Promise<Response> {
  const body = await request.text();
  const auth = await verifySignature(request, body, env);
  if ('status' in auth) {
    logRejected({
      route: action === 'claim' ? '/v1/tag' : '/v1/untag',
      status: auth.status,
      reason: auth.body,
      device: request.headers.get('x-radar-device') ?? undefined,
    });
    return text(auth.status, auth.body);
  }

  const icao = normaliseIcao(new URLSearchParams(body).get('icao'));
  if (icao === null) {
    logRejected({
      route: action === 'claim' ? '/v1/tag' : '/v1/untag',
      status: 400,
      reason: 'bad-icao',
      device: auth.deviceId,
    });
    return text(400, 'bad icao');
  }

  const stub = registry(env);
  const reply =
    action === 'claim' ? await stub.claim(auth.deviceId, icao) : await stub.release(auth.deviceId, icao);
  logTag({
    action,
    device: auth.deviceId,
    handle: reply.handle,
    icao,
    status: reply.status,
    detail: reply.detail,
  });
  return text(reply.status, reply.body);
}

/**
 * Writes are signed HMAC-SHA256 over method, path, timestamp and body, with the
 * secret the device registered.
 *
 * This is not authentication of a person: registration is open, so anyone can mint
 * an identity. It stops one device forging a claim under another's handle, and it
 * gives claims a stable subject to rate limit. Reads are deliberately unsigned so
 * the firmware's hot path stays simple; guard the request budget with a Cloudflare
 * rate limiting rule instead (see README).
 */
async function verifySignature(
  request: Request,
  body: string,
  env: Env,
): Promise<{ deviceId: string } | Reply> {
  const deviceId = (request.headers.get('x-radar-device') ?? '').trim();
  const timestamp = (request.headers.get('x-radar-ts') ?? '').trim();
  const signature = (request.headers.get('x-radar-sig') ?? '').trim().toLowerCase();

  if (!/^[0-9a-f]{12,32}$/.test(deviceId)) return { status: 400, body: 'bad device header' };
  if (!/^[0-9a-f]{64}$/.test(signature)) return { status: 400, body: 'bad signature header' };

  const ts = Number(timestamp);
  if (!Number.isFinite(ts)) return { status: 400, body: 'bad timestamp header' };
  const skew = Math.abs(Math.floor(Date.now() / 1000) - ts);
  if (skew > MAX_CLOCK_SKEW_SEC) return { status: 401, body: `clock skew ${skew}s` };

  const secret = await registry(env).deviceSecret(deviceId);
  if (secret === null) return { status: 401, body: 'unregistered device' };

  const url = new URL(request.url);
  const expected = await hmacSha256Hex(secret, `${request.method}\n${url.pathname}\n${ts}\n${body}`);
  if (!timingSafeEqualHex(expected, signature)) return { status: 401, body: 'bad signature' };

  return { deviceId };
}

function hexToBytes(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; ++i) {
    out[i] = Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

async function hmacSha256Hex(secretHex: string, message: string): Promise<string> {
  const key = await crypto.subtle.importKey(
    'raw',
    hexToBytes(secretHex),
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    ['sign'],
  );
  const mac = await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(message));
  return [...new Uint8Array(mac)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

function timingSafeEqualHex(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  return crypto.subtle.timingSafeEqual(hexToBytes(a), hexToBytes(b));
}
