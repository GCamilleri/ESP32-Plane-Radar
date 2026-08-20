#!/bin/sh
# Fix up /data ownership, then drop privileges.
#
# A bind mount replaces the image's own /data, ownership included, so whatever the
# host says wins and nothing the Dockerfile did to that directory survives. Unraid's
# appdata is nobody:users (99:100), which the image's own unprivileged user cannot
# write to, and SQLite reports that as a bare "unable to open database file".
#
# So the ownership fix has to happen here, at container start, which is the first
# point the real ownership is visible. PUID/PGID default to Unraid's convention and
# can be overridden on any other host.
set -eu

PUID="${PUID:-99}"
PGID="${PGID:-100}"
DB_DIR="$(dirname "${DB_PATH:-/data/tags.db}")"

mkdir -p "$DB_DIR" 2>/dev/null || true

# SQLite reports any of this as a bare "unable to open database file", so every path
# below checks writability first and says which user could not write where.
complain() {
  echo "error: $DB_DIR is not writable by $1." >&2
  echo "       Mount a directory that user can write, set PUID/PGID to its owner," >&2
  echo "       or chown it on the host. On Unraid that is usually 99:100." >&2
  exit 1
}

# Already unprivileged (docker run --user, or a host that refuses root): nothing to
# drop and no way to chown, so the mount has to be right already.
if [ "$(id -u)" != "0" ]; then
  [ -w "$DB_DIR" ] || complain "uid $(id -u)"
  exec node src/index.ts
fi

chown -R "$PUID:$PGID" "$DB_DIR" 2>/dev/null || true
su-exec "$PUID:$PGID" test -w "$DB_DIR" || complain "${PUID}:${PGID}"

exec su-exec "$PUID:$PGID" node src/index.ts
