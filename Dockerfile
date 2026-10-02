# ---- build ---------------------------------------------------------------
FROM debian:trixie-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev make libssl-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./
COPY tools tools
COPY vendor vendor
# Vendored objects first: SQLite is slow to compile and rarely changes.
RUN make build/vendor/sqlite3.o build/vendor/cJSON.o

COPY migrations migrations
COPY src src
RUN make release

# ---- runtime -------------------------------------------------------------
FROM debian:trixie-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3t64 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid 10001 nylm \
 && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin nylm \
 && mkdir /data && chown nylm:nylm /data

COPY --from=build /src/nylm /usr/local/bin/nylm
COPY public /app/public

ENV NYLM_DB=/data/nylm.db \
    NYLM_PUBLIC=/app/public \
    NYLM_CERT=/certs/fullchain.pem \
    NYLM_KEY=/certs/privkey.pem \
    NYLM_ACME_DIR=/acme \
    NYLM_HTTP_PORT=8080 \
    NYLM_HTTPS_PORT=8443

USER nylm
WORKDIR /app
VOLUME /data
EXPOSE 8080 8443
ENTRYPOINT ["nylm"]
