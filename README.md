# nylm

A personal web app in plain C: a hand-written HTTP/1.1 server with TLS
(OpenSSL), SQLite, cJSON, and a static vanilla-JS frontend. See `scope.md`
for the design and `deploy/README.md` for running it on a server.

```sh
make run                               # debug build, http://localhost:8080
echo 'some password' | ./nylm-debug set-password
make test                              # unit tests + smoke test
make cert && ./nylm-debug              # local HTTPS on https://localhost:8443
docker compose up -d --build           # production (needs certs, see deploy/)
```

Build needs `gcc`, `make` and OpenSSL 3.2+ headers (`libssl-dev`).
