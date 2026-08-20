# plane-radar-feed

Cloudflare Worker that serves the radar its aircraft and carries social tags between
devices. Runs on the free plan.

Aircraft data from [adsb.fi](https://adsb.fi/), used under their open data terms.

## What it does

One endpoint gives the device everything it needs:

```
GET /v1/feed?lat=-41.3272&lon=174.8053&dist=13&gnd=0
```

```
PR1 1787193390 1800 3 1
A,C81102,-41.29407,174.82631,273,273,93,ZKTAW,PA38,725 ft
A,C81AC5,-41.40179,174.74217,224,224,108,SDA123,C208,2600 ft
A,C82870,-41.43013,174.97026,210,212,234,ANZ783M,AT76,20025 ft
T,4CA1FB,ZQN,1757
```

`A` lines are aircraft, `T` lines are active tags, and the device joins the two on
the ICAO hex. Format is defined in `src/protocol.ts` and parsed by
`src/services/adsb_feed.cpp` in the firmware.

Three things move off the device by doing it here:

- **Payload size.** Only the ten fields the radar draws, roughly a fifth of what
  adsb.fi returns, in a format the ESP32 parses with `strtoul` into a fixed array.
  No JSON, no heap, on a board where the heap is tightest exactly while the
  response is arriving.
- **Picking the right 64.** The device has 64 aircraft slots. The Worker sorts by
  distance so those slots hold the 64 *nearest*, not whatever order the upstream
  happened to return.
- **All the tag policy.** Lock duration, TTL, rate limits, handle validation and
  the blocklist live in `wrangler.toml` and the Durable Object. The firmware
  renders what it is told, so tuning any of it needs no reflash.

## Endpoints

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/v1/feed` | none | aircraft + active tags |
| GET | `/v1/tags` | none | active tags only, for debugging |
| POST | `/v1/register` | none | announce a device id and secret, receive a handle |
| POST | `/v1/tag` | signed | claim an aircraft |
| POST | `/v1/untag` | signed | release your own claim |

Writes are signed HMAC-SHA256 over `method\npath\ntimestamp\nbody` with the secret
the device registered. This is not authentication of a person: registration is
open, so anyone can mint an identity. What it buys is that one device cannot forge
a claim under another's handle, and that claims have a stable subject to rate
limit.

## Deploy

```bash
cd worker
npm install
npx wrangler deploy
```

Then put the resulting URL in `include/config.h`:

```cpp
constexpr char kFeedProxyBaseUrl[] = "https://plane-radar-feed.<subdomain>.workers.dev";
```

Leaving that empty builds a device with no social features that fetches adsb.fi
directly, which is also what every device falls back to if the Worker stops
answering.

Local development, including the Durable Object and its SQLite storage:

```bash
npx wrangler dev
curl "http://127.0.0.1:8787/v1/feed?lat=-41.3272&lon=174.8053&dist=13&gnd=0"
```

## Testing against a radar on your LAN

```bash
cd worker && script -q /dev/null npx wrangler dev --ip 0.0.0.0    # see notes below
RADAR_FEED_URL=http://192.168.1.17:8787 pio run -e local -t upload
```

`--ip 0.0.0.0` is needed or wrangler only listens on loopback. The firmware picks
its transport from the URL scheme, so a plain `http://` LAN address needs no other
change.

Two things that will otherwise waste your time:

- **Wrap it in `script -q /dev/null`.** With stdout redirected to a file, wrangler
  suppresses the worker's own `console.log`, so all the feed and tag logging
  silently disappears. Giving it a PTY brings it back.
- **`wrangler dev` exits on abrupt client disconnects.** It logs
  `Failed to drain the unused request body` / `Network connection lost` and
  sometimes dies. An ESP32 that gives up on a request mid-flight triggers it, and
  because the firmware then sees TCP resets it hits its failure threshold and backs
  off the proxy for five minutes. Supervise the process in a restart loop for a test
  session. It is a dev-server robustness problem, not something the firmware should
  be reshaped around; real Cloudflare handles the same traffic fine.

Asking to tag something cancels any active proxy backoff
(`services::adsb::retryProxyNow()`), so you do not have to wait out the timer after
restarting the Worker.

Watch traffic and tag events:

```
feed cell=-36.85/174.60/20 lat=-36.8270 lon=174.6155 dist=20 upstream=fetch ac=5 tags=1 ms=330
register result=ok status=200 device=54c504b6a0ec handle=3ZW9 state=returning
tag action=claim result=ok status=200 device=54c504b6a0ec handle=3ZW9 icao=C87F26
```

`upstream=fetch` means that request cost an adsb.fi call; `upstream=cached` means it
was served from the shared cell cache, which is the mechanism keeping the upstream
inside 1 req/s.

## Capacity, honestly

The free plan allows **100,000 Worker requests/day**, and that is the number this
design lives or dies by.

| Device poll rate | Requests/day/device | Devices before the cap |
|---|---|---|
| 3s (firmware default) | 28,800 | 3 |
| 5s | 17,280 | 5 |
| 10s | 8,640 | 11 |

So a handful of radars on 10s polling fits comfortably, and past that the $5/month
paid plan removes the daily cap entirely with no change to any of this code. The
firmware's Poll Rate menu setting is the lever.

Two smaller allowances sit behind the same traffic. Durable Objects have their own
100k requests/day, and the SQLite backend allows 5M rows read/day. Hitting the DO
once per feed poll would drain both in step with the Worker budget, so the tag
block is cached in the DO's memory and again at the edge for `TAG_CACHE_SECONDS`.
Steady-state feed polls read zero rows and usually never reach the DO.

## Staying inside adsb.fi's limits

adsb.fi rate limits public endpoints to **1 request/second per IP**, and a Worker
puts every device behind Cloudflare's shared egress addresses. Proxying naively
would breach that with three devices and get the proxy IP-restricted.

The defence is that the `A` block is keyed on a *quantised* centre: 0.05 degree
cells, about 5.5 km. Every device in the same neighbourhood shares one cache entry
and therefore one upstream fetch, so the upstream rate is bounded by
`(distinct populated cells) / FEED_CACHE_SECONDS` rather than by device count. With
the default 4 second TTL, ten separate neighbourhoods is 2.5 requests/second, which
is already over the limit, so raise `FEED_CACHE_SECONDS` before adding users in
scattered locations.

This is the part of the design with the least headroom, and it is worth watching
rather than trusting. Their terms also restrict the data to personal,
non-commercial use and require attribution, which is why the root endpoint and this
file both carry the credit.

## Abuse

`/v1/feed` is unsigned so the firmware's hot path stays simple, which means anyone
who finds the URL can burn the daily request budget. Signing would not fix it, since
registration is open. The right tool is a Cloudflare **rate limiting rule** on the
zone (one is available on the free plan): limit by IP on `/v1/*`, something like 60
requests/minute, which is far above any real device and well below what it takes to
drain 100k/day.

Tag-specific limits are already enforced in the Durable Object: `CLAIMS_PER_HOUR`
per device and `MAX_ACTIVE_TAGS` globally, which also bounds the response size.
