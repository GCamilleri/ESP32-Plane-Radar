# plane-radar-feed (self-hosted)

Serves the radar its aircraft and carries social tags between devices. Runs as a
Docker container on your own hardware.

Aircraft data from [adsb.fi](https://adsb.fi/), used under their open data terms.

## Why self-hosted rather than Cloudflare

This started as a Cloudflare Worker. Three things pushed it onto a home server:

- **The request cap.** Workers' free plan allows 100,000 requests/day, which at the
  firmware's 3s poll is three radars. Here there is no cap.
- **adsb.fi's rate limit.** They allow 1 request/second per IP. A Worker put every
  radar behind Cloudflare's shared egress addresses; self-hosting makes that budget
  yours, which is both fairer and more predictable.
- **The code got smaller.** Durable Objects and the Cache API exist to work around
  Workers' execution model. On a normal process they collapse into a SQLite file and
  a `Map`, and the tag snapshot can be invalidated on write instead of expiring on a
  timer, which removed a two-to-three-cycle delay before your own tag appeared on
  your own radar.

The trade-off is that your server becomes the tag dependency. That is survivable
because the firmware falls back to fetching adsb.fi directly after three consecutive
failures: a radar keeps working when this is down, it just stops showing tags.

## What it serves

```
GET /v1/feed?lat=-36.8270&lon=174.6155&dist=20&gnd=0
```

```
PR1 1787198230 1800 3 1 2143
A,C87F23,-36.77660,174.65286,235,232,103,TEXSIL,TEX2,-75 ft
A,C81C27,-36.79358,174.44161,67,67,83,ZKMBZ,PA38,1100 ft
A,C82513,-36.97728,174.71528,301,310,104,GBA109,C208,1650 ft
T,C87F23,ZQN,1731
```

`A` lines are aircraft, `T` lines are active tags, and the device joins them on the
ICAO hex. The format is defined in `src/protocol.ts` and parsed by
`src/services/adsb_feed.cpp` in the firmware, with host tests pinning the contract.

The header's last field is how old those positions are in milliseconds, 2143 above.
Radars looking at the same area share one cached fetch, so a response is usually not
brand new, and the device dead reckons aircraft forward between polls: told the wrong
age, it would tug every aeroplane backwards each time a fresher block landed.

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/v1/feed` | none | aircraft + active tags |
| GET | `/v1/tags` | none | active tags only |
| GET | `/healthz` | none | liveness, used by the container healthcheck |
| POST | `/v1/register` | none | announce a device id and secret, receive a handle |
| POST | `/v1/tag` | signed | claim an aircraft |
| POST | `/v1/untag` | signed | release your own claim |
| POST | `/v1/untagall` | signed | release all your claims ("Clear Tags" in the menu) |

Writes are signed HMAC-SHA256 over `method\npath\ntimestamp\nbody`. This is not
authentication of a person: registration is open, so anyone can mint an identity.
What it buys is that one device cannot claim under another's handle, and that claims
have a stable subject to rate limit.

## Running it on Unraid

The image has no runtime dependencies and no native modules, so it is published for
both amd64 and arm64.

### With Docker Compose (Compose Manager plugin)

```bash
mkdir -p /mnt/user/appdata/plane-radar-feed
cd /mnt/user/appdata/plane-radar-feed
# copy docker-compose.yml from this directory
docker compose up -d
```

### As an Unraid container template

- **Repository:** `ghcr.io/gcamilleri/plane-radar-feed:latest`
- **Network:** Bridge
- **Port:** `8787` → `8787` (TCP)
- **Path:** `/mnt/user/appdata/plane-radar-feed` → `/data`

The `/data` mapping matters more than it looks. Device registrations live there, and
a handle determines its own colour, so losing that volume changes what colour every
radar draws for every tagger.

**Ownership of that directory is the one thing that will stop the container
starting.** A bind mount replaces the image's `/data` including its ownership, so
whatever the host says wins, and SQLite reports the result as a bare "unable to open
database file". The entrypoint fixes it at startup by chowning to `PUID:PGID`, which
default to Unraid's `99:100`. On another host, set them to the owner of the directory
you mounted. If it still cannot write, the container now says so in plain language
instead of leaving the SQLite error as the only clue.

Check it is working from the box itself before putting a proxy in front:

```bash
curl "http://192.168.1.50:8787/v1/feed?lat=-36.827&lon=174.6155&dist=20&gnd=0"
docker logs -f plane-radar-feed
```

Radars point at the public hostname, not this address; see the next section.

## Putting it behind a reverse proxy

Radars reach the server over the internet, so it sits behind a hostname on your
domain. The container speaks plain HTTP; the proxy terminates TLS.

### Nginx Proxy Manager

Add a **Proxy Host**:

| Field | Value |
|---|---|
| Domain Names | `radar.example.com` |
| Scheme | `http` |
| Forward Hostname / IP | the Unraid host IP, or `plane-radar-feed` if NPM shares a docker network with it |
| Forward Port | `8787` |
| Block Common Exploits | on |
| Websockets Support | off, nothing here uses them |

On the **SSL** tab request a Let's Encrypt certificate and turn on *Force SSL*.

That is the whole setup. Nothing else is required, and in particular **do not add a
path or a custom location**: writes are signed HMAC-SHA256 over
`method\npath\ntimestamp\nbody`, so anything that rewrites the URI makes every
signature fail while the feed keeps working perfectly. Tagging returns 401 and
nothing else looks wrong. A plain Proxy Host passes the URI through untouched, which
is exactly what is needed; giving the server its own hostname at the root keeps it
that way.

### Optional: a rate limit in NPM

The server already caps what actually matters (see below), so this is a blunt outer
bound rather than the real defence. It needs two pieces in two different places,
because `limit_req_zone` is an `http`-context directive and NPM's Advanced tab
injects into a `server` block.

1. On the Unraid host, in NPM's appdata, create
   `/mnt/user/appdata/NginxProxyManager/nginx/custom/http_top.conf`:

   ```nginx
   limit_req_zone $binary_remote_addr zone=radar:10m rate=4r/s;
   ```

   Create the `custom` directory if it is not there. NPM includes that file at the
   top of the `http` block.

2. In the Proxy Host's **Advanced** tab:

   ```nginx
   limit_req zone=radar burst=40 nodelay;
   ```

3. Restart the NPM container so nginx reloads. If a snippet is in the wrong context
   nginx refuses to start and says so in the NPM log, so check it came back up.

`4r/s` with a burst of 40 is deliberately generous. Several radars in one house share
one public address, and each polls every 3 seconds, so a tight per-IP limit punishes
exactly the setup this is for.

Then build the firmware against the public hostname:

```bash
RADAR_FEED_URL=https://radar.example.com pio run -e local -t upload
```

`https` is all the firmware needs to switch to TLS; there is no other change.

## What is actually exposed

Anyone can build a radar and point it here, which is the intent. There is no shared
key and nothing to onboard: flash, set your location, done.

That is a deliberate choice about what is worth defending. The server proxies public
data, holds a tag row per tagged aircraft, and stores device
registrations. What is in the database is random secrets, four-character handles and
ICAO hexes; it never stores where any device is. Writes are signed per device, so
nobody can claim under your handle or release your tags. There is no privileged
path, no shell-out, no filesystem write from input, parameterised SQL only, and
bodies are capped at 4 KB. The container runs unprivileged with only `/data`
writable.

So the risks worth bounding are availability, not compromise, and both are bounded
by limits rather than by secrets:

**Getting your address rate limited by adsb.fi.** They allow 1 request/second per
IP. Cell sharing helps but does not enforce it, because one client asking about many
different cells multiplies upstream fetches without making many requests. So
`UPSTREAM_MIN_INTERVAL_MS` caps the fetch rate where the fetches happen, across all
clients. When it refuses, a cell up to `MAX_STALE_SECONDS` old is served instead:
slightly old aircraft beat both a failed poll and a restricted address. If no cached
cell exists, the request 502s, the firmware counts a proxy failure and falls back to
adsb.fi directly, which is the correct outcome.

**Tag crowding.** `MAX_TAGS_PER_DEVICE` (10) caps how many aircraft one device holds
at once. It is a concurrency cap, not a rate limit: a released tag frees its slot
immediately, as does one another device takes over after the lock runs out, and
refreshing a tag you already hold consumes nothing. Tags do not expire on their own,
so a slot stays occupied until one of those happens.
`REGISTRATIONS_PER_HOUR_PER_IP` stops anyone minting identities in bulk to get
around it. If someone does abuse it anyway, the escalation is deleting their rows
from `devices`, which is reactive and about right at this scale.

Watch it with `docker logs`. A steady stream of `upstream=stale` means the interval
limiter is being hit; repeated `register-rate-limit` or `at-device-limit` means
someone is pushing.

A proxy-level rate limit is worth adding on top as a blunt outer bound against
traffic that never reaches useful work; see the NPM steps above.

Note the firmware calls `setInsecure()`, so it does not verify the server's
certificate. Over a LAN that hardly mattered; over the internet it means a
machine-in-the-middle could impersonate the server, collect a device secret and feed
false tags. Fixing it means embedding a CA bundle, with its own maintenance cost when
roots rotate, so it is a known gap rather than an oversight.

## Configuration

All optional; the defaults suit a home server.

| Variable | Default | Notes |
|---|---|---|
| `PORT` | `8787` | |
| `HOST` | `0.0.0.0` | Must stay `0.0.0.0` inside a container |
| `DB_PATH` | `/data/tags.db` | Keep on a mounted volume |
| `LOCK_SECONDS` | `3600` | How long a claim holds an aircraft exclusively. Tags do not expire; this is only when another device may take one over |
| `TAG_RETENTION_SECONDS` | `0` | Forget tags this long after they were claimed. `0` keeps them until released or taken over |
| `MAX_TAGS_PER_DEVICE` | `10` | Concurrent tags per device; a release or a takeover frees a slot |
| `MAX_FEED_TAGS` | `64` | Response size bound only, never refuses a claim. Must not exceed the firmware's `kMaxFeedTags` |
| `FEED_CACHE_SECONDS` | `4` | Cache TTL per map cell |
| `UPSTREAM_MIN_INTERVAL_MS` | `1000` | Hard floor between adsb.fi fetches, all clients |
| `MAX_STALE_SECONDS` | `60` | How old a cell may be when the limiter refuses a fetch |
| `REGISTRATIONS_PER_HOUR_PER_IP` | `5` | |
| `PUID` / `PGID` | `99` / `100` | Owner applied to `/data` at startup |
| `ADSB_BASE` | `https://opendata.adsb.fi/api/v3` | |
| `MAX_CLOCK_SKEW_SECONDS` | `600` | The ESP32 has no RTC and drifts |
| `UPSTREAM_TIMEOUT_MS` | `8000` | |

Everything here was policy on the Cloudflare side too. The firmware renders what it
is told, so all of it changes with a container restart rather than a reflash.

## Staying inside adsb.fi's limits

Their public endpoints allow 1 request/second per IP, and every fetch from here
leaves one address, so this is the limit worth being careful about.

Two things keep us inside it. The aircraft block is keyed on a **quantised** centre
(0.05 degree cells, about 5.5 km), so every radar in the same neighbourhood shares
one cache entry and therefore one upstream fetch: cost scales with populated cells
rather than with how many radars you own. Measured with two radars polling every 3s,
68 requests produced 21 fetches.

Neither the requested radius nor the ground flag is part of that key. What is cached
is the cell's parsed aircraft, fetched at one canonical 25 nm radius, and each
response is built from it for the radar that asked: filtered to that radar's own
radius, measured from where it actually is, and capped at the nearest 64. Two radars
side by side on different range settings therefore cost one fetch, not two, and a
radar sitting near the edge of its cell still gets the whole of its own range rather
than the cell centre's.

Cell sharing alone is not a guarantee, though, because one client asking about many
scattered cells multiplies fetches without making many requests. So
`UPSTREAM_MIN_INTERVAL_MS` is a hard floor between fetches across all clients. When
it refuses, a cell up to `MAX_STALE_SECONDS` old is served and the log line reads
`upstream=stale`; if there is no cached cell at all the request 502s and the radar
falls back to adsb.fi on its own.

A steady stream of `upstream=stale` means the floor is being hit. Raising
`FEED_CACHE_SECONDS` is the first thing to try, since it makes each fetch serve more
requests.

Their terms also restrict the data to personal, non-commercial use and require
attribution, which is why `GET /` carries the credit.

## Development

```bash
npm install          # types only; there are no runtime dependencies
npx tsc --noEmit
node src/index.ts    # needs Node 24+
```

Node runs these `.ts` files by stripping types rather than compiling them. Some
valid TypeScript is unsupported in that mode, parameter properties and enums among
them, and it fails at *startup*, not typecheck. The `smoke` job in
`.github/workflows/server-image.yml` boots the server for exactly this reason.

Logs are one greppable line per event:

```
2026-08-20 04:02:56 listening host=0.0.0.0 port=8787 db=/data/tags.db lock=1800
2026-08-20 04:03:12 feed cell=-36.85/174.60/20/0 lat=-36.8270 lon=174.6155 dist=20 upstream=fetch ac=9 tags=1 ms=330
2026-08-20 04:03:15 register result=ok status=200 device=54c504b6a0ec handle=3PFP state=new
2026-08-20 04:03:22 tag action=claim result=ok status=200 device=54c504b6a0ec handle=3PFP icao=C87F23
```

`upstream=fetch` means that request cost an adsb.fi call; `upstream=cached` means it
was served from the shared cell cache.
