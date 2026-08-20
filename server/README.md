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
PR1 1787198230 1800 3 1
A,C87F23,-36.77660,174.65286,235,232,103,TEXSIL,TEX2,-75 ft
A,C81C27,-36.79358,174.44161,67,67,83,ZKMBZ,PA38,1100 ft
A,C82513,-36.97728,174.71528,301,310,104,GBA109,C208,1650 ft
T,C87F23,ZQN,1731
```

`A` lines are aircraft, `T` lines are active tags, and the device joins them on the
ICAO hex. The format is defined in `src/protocol.ts` and parsed by
`src/services/adsb_feed.cpp` in the firmware, with host tests pinning the contract.

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

Check it is working from the box itself before putting a proxy in front:

```bash
curl "http://192.168.1.50:8787/v1/feed?lat=-36.827&lon=174.6155&dist=20&gnd=0"
docker logs -f plane-radar-feed
```

Radars point at the public hostname, not this address; see the next section.

## Putting it behind a reverse proxy

Radars reach the server over the internet, so it sits behind a hostname on your
domain. The container listens on plain HTTP; the proxy terminates TLS.

**One thing will silently break tagging if you get it wrong: do not rewrite the
path.** Writes are signed HMAC-SHA256 over `method\npath\ntimestamp\nbody`, so if
the proxy serves the feed under a subpath and strips it, or normalises the URI, every
signature stops matching and claims fail with 401 while the feed keeps working
perfectly. Give the server its own hostname at the root, and note the missing
trailing slash on `proxy_pass` below, which is what preserves the URI in nginx.

```nginx
server {
    listen 443 ssl http2;
    server_name radar.example.com;

    # your usual certificate directives here

    # Bodies are a couple of hundred bytes; anything larger is not from a radar.
    client_max_body_size 8k;

    location / {
        # No trailing slash: passes the URI through untouched, which the request
        # signatures depend on.
        proxy_pass http://192.168.1.50:8787;

        # Keep-alive to the upstream. The radar holds one connection open and
        # sends its tag POST down the same socket as the feed GET, so closing it
        # between requests throws away the reason the protocol is shaped this way.
        proxy_http_version 1.1;
        proxy_set_header Connection "";

        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;

        # A poll is worthless once the next one is due.
        proxy_read_timeout 15s;
        proxy_connect_timeout 5s;
    }
}
```

Then build the firmware against the public hostname:

```bash
RADAR_FEED_URL=https://radar.example.com pio run -e local -t upload
```

`https` is all the firmware needs to switch to TLS; there is no other change.

### Worth knowing before you expose it

`/v1/feed` is unsigned, and `/v1/register` is open to anyone. On a LAN that was
fine. On the public internet:

- Anyone who learns the hostname can pull the feed, which spends **your** adsb.fi
  request budget from **your** IP.
- Anyone can register a device and then claim aircraft. They cannot touch your
  tags (release is owner-only) but they can fill `MAX_ACTIVE_TAGS` and crowd
  yours out.

Neither is catastrophic and neither is hard to bound. The cheapest effective fix is
a shared header checked at the proxy, so unauthenticated traffic never reaches the
container:

```nginx
    # In the server block, before location /
    if ($http_x_radar_key != "some-long-random-string") { return 404; }
```

That needs the firmware to send the header, which it does not do yet. Cloudflare
Access with a Service Auth policy is the tidier version of the same idea (two
static headers, no proxy config), and it is worth doing if this ever hosts more
than your own radars.

Also note the firmware calls `setInsecure()`, so it does not verify the server's
certificate. Over a LAN that hardly mattered; over the internet it means a
machine-in-the-middle could impersonate the server, collect a device secret and
feed false tags. Fixing it means embedding a CA bundle in the firmware, which is
real work and has its own maintenance cost when roots rotate.

## Configuration

All optional; the defaults suit a home server.

| Variable | Default | Notes |
|---|---|---|
| `PORT` | `8787` | |
| `HOST` | `0.0.0.0` | Must stay `0.0.0.0` inside a container |
| `DB_PATH` | `/data/tags.db` | Keep on a mounted volume |
| `LOCK_SECONDS` | `1800` | How long a claim holds an aircraft |
| `CLAIMS_PER_HOUR` | `10` | Per device; refreshes count |
| `MAX_ACTIVE_TAGS` | `64` | Also bounds the response size |
| `FEED_CACHE_SECONDS` | `4` | Floor on upstream fetches per map cell |
| `ADSB_BASE` | `https://opendata.adsb.fi/api/v3` | |
| `MAX_CLOCK_SKEW_SECONDS` | `600` | The ESP32 has no RTC and drifts |
| `UPSTREAM_TIMEOUT_MS` | `8000` | |

Everything here was policy on the Cloudflare side too. The firmware renders what it
is told, so all of it changes with a container restart rather than a reflash.

## Staying inside adsb.fi's limits

Their public endpoints allow 1 request/second per IP. The aircraft block is keyed on
a **quantised** centre (0.05 degree cells, about 5.5 km), so every radar in the same
neighbourhood shares one cache entry and therefore one upstream fetch. Upstream rate
is bounded by `(populated cells) / FEED_CACHE_SECONDS`, not by how many radars you
own. Raise `FEED_CACHE_SECONDS` before adding radars in scattered locations.

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
