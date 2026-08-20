# Social aircraft tags: design

Users tag an interesting aircraft on their own radar. Other radars show that tag
when the same aircraft passes through their range, along with a short handle
identifying who tagged it. A tagged aircraft is locked to its original tagger for a
moderate period.

## Shape

The server in `server/` is the device's feed: a Docker container on a home box. It
fetches adsb.fi itself, strips the payload to the ten fields the radar draws, merges
in the active tags, and serves the lot as one line-oriented response. The device
makes one request per poll to one host.

This began as a Cloudflare Worker, which is worth recording because the reasoning
still shapes the code. Three things moved it onto self-hosted hardware:

- **The request cap.** Workers' free plan allows 100,000 requests/day, which at the
  firmware's 3s poll is three radars. Self-hosted has no cap.
- **adsb.fi's rate limit.** They allow 1 request/second per IP, and a Worker put
  every radar behind Cloudflare's shared egress. Self-hosting makes that budget
  yours: fairer to them, and predictable for you.
- **The code got smaller.** Durable Objects and the Cache API exist to work around
  Workers' execution model. In a normal process they collapse to a SQLite file and a
  `Map`, "first claim wins" comes free from `node:sqlite` being synchronous on a
  single-threaded runtime, and the tag snapshot can be invalidated on write instead
  of expiring on a timer, which removed a two-to-three-cycle delay before your own
  tag appeared on your own radar.

Self-hosting also fixed a stability problem that was purely local: `wrangler dev`
exits when a client abandons a request mid-flight, which an ESP32 does, and it
restarted seven times during one test session. The container took the same abuse
with zero restarts.

Serving both in one response is also what makes the feature affordable in RAM. The
alternative, keeping adsb.fi direct and adding a second connection for tags, means two concurrent
TLS sessions and another ~32 KB of mbedTLS buffers on a board where
`src/services/adsb_client.cpp` already carries scar tissue from handshakes failing
on a fragmented heap. Routing everything through one host means the tag POST rides
the keep-alive socket the feed just used.

Direct adsb.fi access survives as the fallback only. If the server fails
`kFeedProxyFailuresBeforeBackoff` times in a row, the device fetches adsb.fi itself
and retries the server after an exponential backoff. That is the standalone
guarantee in code rather than in a comment: a dead or misconfigured server costs one
poll cycle, never the radar.

The backoff doubles from `kFeedProxyBackoffBaseMs` (30s) to
`kFeedProxyBackoffMaxMs` (15 min), and any successful proxy fetch resets it. A flat
delay could not serve both cases: a server restarted for twenty seconds wants a
short retry, a server gone for the weekend wants a long one. It was a flat five
minutes at first, and the cost showed up immediately during testing as a radar
sitting on the fallback long after the server was back.

Asking to tag also cancels the backoff outright. Claims only travel on the proxy
connection, so otherwise a deliberate tag could sit unsent for the whole window
because of an unrelated earlier failure, and a user reaching for the button is a
better signal that the server is back than any timer.

## The constraint to watch

**adsb.fi allows 1 request/second per IP.** Self-hosting makes that limit yours
rather than a shared pool's, which is an improvement and still a limit. The aircraft
block is keyed on a quantised centre (0.05 degree cells, ~5.5 km), so radars in the
same neighbourhood share a cache entry and therefore one upstream fetch. Upstream
rate is bounded by `(populated cells) / FEED_CACHE_SECONDS`, independent of how many
radars you own. Measured with two radars polling every 3s: 68 requests to the server
produced 21 fetches to adsb.fi.

Their terms also restrict the data to personal, non-commercial use and require
attribution, which `GET /` carries.

## Data model

The server never stores where any device is. It holds a flat list of tags keyed by
ICAO hex; the device joins them onto the aircraft it can already see.

```
tag: icao -> { handle, owner_device_id, claimed_at, expires_at }
```

Storage is a SQLite file. "First claim wins" is correct without any locking of our
own because `node:sqlite` is a synchronous API on a single-threaded runtime, so a
claim cannot interleave with another as long as nothing awaits partway through.
Nothing in `server/src/store.ts` is async, deliberately.

Nothing device-specific appears in the response, which is deliberate. There is no
"this one is yours" flag; the device compares handles against its own. That keeps the
whole response byte-identical for every device, which is what lets the aircraft block
be shared between neighbouring radars at all.

The tag block is held in memory and invalidated on write, so steady-state polls read
no rows, and a new tag is in the very next response rather than waiting out a cache
TTL. Expired tags are swept on a timer so reads never filter a growing table.

## Identity and abuse

The device generates 16 bytes from `esp_random()` on first boot, keeps them in NVS,
and derives a 12 hex char device id from their SHA-256. Writes are signed
HMAC-SHA256 over `method\npath\ntimestamp\nbody`.

This is not authentication of a person. Registration is open, so anyone can mint an
identity; saying otherwise would be worse than saying nothing. What it buys is that
one device cannot forge a claim under another's handle, and that claims have a
stable subject to rate limit and revoke.

The ESP32 has no RTC, so the timestamp comes from the server's clock as carried in
the PR1 header. Until the first feed response arrives, signed requests are held
rather than sent with a timestamp that would be rejected for skew.

Server-side policy, all of it off the device: lock duration, tag TTL,
`MAX_TAGS_PER_DEVICE`, handle charset and length, first-come handle uniqueness, and a
blocklist to blunt impersonation and slurs.

## Open on purpose, bounded instead

Anyone can build a radar and point it at the server. There is no shared key and
nothing to onboard, which is a deliberate choice about what is worth defending.

A shared secret was built and then removed. It was a barrier to the thing the project
wants to encourage, and it did not work anyway: the key had to be compiled into the
firmware, so anyone holding a binary had it, and one shared key cannot be revoked for
one person. It protected an endpoint that nobody minds being used.

What is left to protect is availability, not secrecy, so the controls are limits:

- **`UPSTREAM_MIN_INTERVAL_MS`** caps adsb.fi fetches across all clients. This is the
  only limit guarding something outside our control. Per-client rate limiting cannot
  do it, because one client asking about many scattered cells multiplies upstream
  fetches without making many requests. When the limiter refuses, a cell up to
  `MAX_STALE_SECONDS` old is served instead: slightly stale aircraft beat both a
  failed poll and a restricted address.
- **`MAX_TAGS_PER_DEVICE`** caps concurrent tags per device. Counted from unexpired
  rows rather than a stored tally, so an expired or released tag frees its slot at
  once and refreshing a tag you hold costs nothing. The earlier design was an hourly
  counter, which both counted refreshes and never gave slots back: it measured the
  wrong thing.
- **`REGISTRATIONS_PER_HOUR_PER_IP`** stops identities being minted in bulk to get
  around the per-device cap.

There is deliberately no global tag cap. A claim is never refused because other
people hold tags. `MAX_FEED_TAGS` only bounds response size, and must not exceed the
firmware's `kMaxFeedTags` or the device silently drops the excess.

If someone abuses it regardless, the escalation is deleting their rows from
`devices`. Reactive, and proportionate at this scale.

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

## What else could move to the server

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
- `adsb_client` picks its transport from the URL scheme, so a plain-HTTP LAN server
  needs no other change, and that is the normal case now: no TLS session means ~32 KB
  of heap that mbedTLS would otherwise hold. Behind a Cloudflare Tunnel it is HTTPS
  again. The device secret crosses the wire once at registration, which is worth
  knowing if the LAN is not trusted.
- A queued claim gives up after `kSocialRequestTimeoutMs` and the picker stays open
  while one is outstanding, because the picker is the only place the result is
  shown. Without both, a claim made while the proxy was unreachable left the UI
  showing "tagging..." with no resolution.
- Asking to tag cancels an active proxy backoff. Claims only travel on the proxy
  connection, so otherwise a deliberate tag could sit unsent for five minutes
  because of an unrelated earlier failure.
