# Node 24 runs TypeScript directly (type stripping, no build step) and has SQLite
# compiled into the binary, so this image needs no dependency install, no build
# stage and no native modules. That also means it builds for arm64 as readily as
# amd64, which matters if the Unraid box is ever not x86.
FROM node:24-alpine

WORKDIR /app

# No runtime dependencies, so the source is the whole image payload.
COPY package.json tsconfig.json ./
COPY src ./src

# SQLite lives here; mount it as a volume or device handles are forgotten on every
# restart, which would change everyone's handle and therefore their tag colour.
RUN mkdir -p /data && chown -R node:node /data /app
VOLUME /data

ENV NODE_ENV=production \
    HOST=0.0.0.0 \
    PORT=8787 \
    DB_PATH=/data/tags.db

EXPOSE 8787
USER node

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD wget -qO- "http://127.0.0.1:${PORT}/healthz" >/dev/null 2>&1 || exit 1

CMD ["node", "src/index.ts"]
