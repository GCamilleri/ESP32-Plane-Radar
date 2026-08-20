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

Then point the radars at it and flash:

```bash
RADAR_FEED_URL=http://192.168.1.50:8787 pio run -e local -t upload
```

Check it is working:

```bash
curl "http://192.168.1.50:8787/v1/feed?lat=-36.827&lon=174.6155&dist=20&gnd=0"
docker logs -f plane-radar-feed
```

## Reaching it from outside the house

Radars elsewhere need a public hostname. [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-tunnel/)
is free, needs no port forwarding, and works behind CGNAT. It does require a domain
on Cloudflare DNS.

1. Cloudflare Zero Trust dashboard → Networks → Tunnels → Create a tunnel.
2. Add a public hostname, e.g. `radar.example.com`, service
   `http://plane-radar-feed:8787`.
3. Copy the tunnel token into `.env` next to the compose file:
   `TUNNEL_TOKEN=eyJ...`
4. `docker compose up -d` brings up the `cloudflared` service alongside the server.
5. Rebuild the firmware with `RADAR_FEED_URL=https://radar.example.com`.

Keep the LAN port published even when tunnelling, so radars at home keep working
when your internet does not.

Two caveats worth knowing. Free-plan tunnels are limited to the root domain and
single-level subdomains, and section 2.8 of Cloudflare's self-serve terms restricts
non-HTML content, irrelevant for a few KB of text, but it is why people are told
not to stream media through a tunnel. Also, TLS terminates at Cloudflare's edge, so
the tunnel is not end-to-end encryption to your box.

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
