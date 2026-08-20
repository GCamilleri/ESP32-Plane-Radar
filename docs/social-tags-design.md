# Social aircraft tags: design

Users tag an interesting aircraft on their own radar. Other radars show that tag
when the same aircraft passes through their range, along with a short handle
identifying who tagged it. A tagged aircraft is locked to its original tagger for a
moderate period.

## Shape

The Cloudflare Worker in `worker/` is the device's feed. It fetches adsb.fi itself,
strips the payload to the ten fields the radar draws, merges in the active tags, and
serves the lot as one line-oriented response. The device makes one request per poll
to one host.

That last point is what makes the whole thing affordable in RAM. The alternative,
keeping adsb.fi direct and adding a second connection for tags, means two concurrent
TLS sessions and another ~32 KB of mbedTLS buffers on a board where
`src/services/adsb_client.cpp` already carries scar tissue from handshakes failing
on a fragmented heap. Routing everything through one host means the tag POST rides
the keep-alive socket the feed just used.

Direct adsb.fi access survives as the fallback only. If the Worker fails
`kFeedProxyFailuresBeforeBackoff` times in a row the device fetches adsb.fi itself
for `kFeedProxyBackoffMs` and then retries the proxy. That is the standalone
guarantee in code rather than in a comment: a dead, misconfigured or over-quota
Worker costs one poll cycle, never the radar.

## The constraint to watch

Two limits bound this, and neither is Cloudflare compute.

**Worker requests: 100,000/day on the free plan.** At the firmware's 3s default
that is 28,800/day/device, so three devices. At 10s it is eleven. For a handful of
radars this is fine, and the $5/month plan removes the cap without changing any
code. The Poll Rate menu setting is the lever. `worker/README.md` has the table.

**adsb.fi: 1 request/second per IP.** A Worker puts every device behind
Cloudflare's shared egress addresses, so this would break at three devices if each
poll caused an upstream fetch. It does not, because the aircraft block is keyed on a
quantised centre (0.05 degree cells, ~5.5 km): neighbours share a cache entry and
therefore an upstream fetch. Upstream rate is bounded by
`(distinct populated cells) / FEED_CACHE_SECONDS`, independent of device count. This
is the part with the least headroom; raise the TTL before adding users in scattered
places.

adsb.fi's terms also restrict the data to personal, non-commercial use and require
attribution, which the Worker's root endpoint and README carry. Re-serving their
feed to other people's devices is a grey area worth being aware of, and their stated
right to terminate access is a real single point of failure that the direct fallback
partly covers.

## Data model

The Worker never learns where any device is. It holds a flat list of tags keyed by
ICAO hex; the device joins them onto the aircraft it can already see.

```
tag: icao -> { handle, owner_device_id, claimed_at, expires_at }
```

Storage is a single **Durable Object** with the SQLite backend, the only backend
available on the free plan. A DO handles one request at a time, which is what makes
"first claim wins" correct with no locking of our own. KV cannot express it: 1,000
writes/day and eventually consistent.

Nothing device-specific appears in the response, which is deliberate. There is no
"this one is yours" flag; the device compares handles against its own. That keeps
the whole response byte-identical for every device, so both blocks cache at the edge
and most polls never reach the DO at all. That matters because Durable Objects have
their own 100k requests/day and 5M SQLite rows read/day, and hitting the DO once per
feed poll would drain three budgets in step. The DO also keeps its serialised tag
block in memory and sweeps expired rows on an alarm, so steady-state reads touch
zero rows.

## Identity and abuse

The device generates 16 bytes from `esp_random()` on first boot, keeps them in NVS,
and derives a 12 hex char device id from their SHA-256. Writes are signed
HMAC-SHA256 over `method\npath\ntimestamp\nbody`.

This is not authentication of a person. Registration is open, so anyone can mint an
identity; saying otherwise would be worse than saying nothing. What it buys is that
one device cannot forge a claim under another's handle, and that claims have a
stable subject to rate limit and revoke.

The ESP32 has no RTC, so the timestamp comes from the Worker's clock as carried in
the PR1 header. Until the first feed response arrives, signed requests are held
rather than sent with a timestamp that would be rejected for skew.

Server-side policy, all of it off the device: lock duration, tag TTL,
`CLAIMS_PER_HOUR`, `MAX_ACTIVE_TAGS`, handle charset and length, first-come handle
uniqueness, and a blocklist to blunt impersonation and slurs.

`/v1/feed` is unsigned so the firmware's hot path stays simple, which leaves the
request budget open to anyone who finds the URL. Signing would not help, since
registration is open. A Cloudflare rate limiting rule on the zone is the right tool.

## Interaction

One button, and both its gestures were already taken: tap cycles range, a 1s hold
opens the menu. Tagging therefore lives behind a **double tap**, which was free.

Inside the picker the same two gestures are reused with local meaning: tap moves the
cursor to the next aircraft outward, hold claims or releases. It exits on a 6s
timeout because there is no third gesture to spend on an explicit exit.

Two consequences worth knowing:

- The radar screen now routes taps through `bootButtonConsumeGesture()` rather than
  `bootButtonConsumeTap()`, because telling one tap from two requires waiting out
  `kBootGestureDebounceMs`. Range changes gained ~400 ms of latency. That is the
  cost of a second gesture on a one-button device.
- The picker disarms hold-to-reset, exactly as the menu does
  (`src/main.cpp`). A hold means "claim" there, and holding on past 3s must not
  erase the user's credentials.

Selection is tracked by ICAO, never by index: the aircraft array is rebuilt from
scratch every poll, so an index would silently start pointing at a different
aeroplane mid-gesture.

## Rendering

A tagged aircraft keeps its normal symbol and gains a corner-bracket reticle plus
the tagger's handle. It is not recoloured, because the symbol colour already means
something (military highlight) and overloading it would lose that.

The reticle and handle colour is derived from the handle itself, via FNV-1a into a
fixed eight-entry palette (`ui::radar::tagPaletteIndex`). A fixed palette rather
than a hash straight to RGB, so every colour is legible on the dark background and
none collide with the red aircraft symbols or the cyan military highlight. Because
the hash is deterministic, the same tagger renders in the same colour on every
radar, which is what makes the colour carry meaning at all.

The picker's cursor is the same bracket shape in white, one step further out, so a
selected tagged aircraft shows both.

The handle gets its own label line and survives label mode "None": a tag nobody can
read is not a tag. That made label block height per-aircraft rather than uniform,
which is why `resolveLabels()` computes it inside the loop now.

Beyond the outer ring there is no room for a reticle, so a tagged aircraft's rim dot
takes the tagger's colour instead.

## What else could move to the Worker

The **airport and runway dataset** is the big one, and deliberately not in this
change. `include/data/airports.h` embeds 29,199 airports and 27,941 runways, close
to 900 KB of the 3 MB app slot. A device only needs what is in range. Serving the
local subset and caching it in the unused 896 KB SPIFFS partition would free most of
that flash and let the data be corrected without reflashing. It touches
`runway_overlay`, SPIFFS and the build scripts, so it deserves its own diff.

Deliberately **not** moved: the military hex range table (`data/military_ranges.h`).
It is small, and the direct fallback needs it anyway, so having the proxy send the
flag would create two sources of truth. The device computes it from the ICAO on both
paths.

## Loose ends

- Positions are not interpolated between polls. At a 10s poll a 400 kt aircraft
  jumps ~2 km per update. Dead reckoning from the track and speed the device
  already stores would decouple perceived smoothness from poll rate, and so from
  the request budget. That is the natural follow-up if the free tier gets tight.
- `drawAircraftTag()` in `src/ui/radar_display.cpp` is unreachable (only the
  `Placed` variant is called) and was left as found, so it does not draw the handle
  line. Worth deleting rather than maintaining in parallel.
- Whether a `workers.dev` subdomain answers on plain HTTP is untested. It would save
  the TLS session entirely, and the data is public, but the secret does cross the
  wire once at registration. Verify with `curl` against a deployment before
  considering it.
