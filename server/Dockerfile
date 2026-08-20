# Node 24 runs TypeScript directly (type stripping, no build step) and has SQLite
# compiled into the binary, so this image needs no dependency install, no build
# stage and no native modules. That also means it builds for arm64 as readily as
# amd64, which matters if the Unraid box is ever not x86.
FROM node:24-alpine

# su-exec only, to drop privileges after fixing up the volume. See entrypoint.sh.
RUN apk add --no-cache su-exec

WORKDIR /app

# No runtime dependencies, so the source is the whole image payload.
COPY package.json tsconfig.json ./
COPY src ./src
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# SQLite lives here. Mount it, or device registrations are forgotten on every
# restart, which changes every handle and so the colour other radars draw for it.
RUN mkdir -p /data
VOLUME /data

ENV NODE_ENV=production \
    HOST=0.0.0.0 \
    PORT=8787 \
    DB_PATH=/data/tags.db \
    PUID=99 \
    PGID=100

EXPOSE 8787

# Deliberately no USER: the entrypoint starts as root only long enough to chown the
# mounted volume, then execs as PUID:PGID. A bind mount arrives owned by the host,
# so this cannot be settled at build time.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD wget -qO- "http://127.0.0.1:${PORT}/healthz" >/dev/null 2>&1 || exit 1

ENTRYPOINT ["/entrypoint.sh"]
