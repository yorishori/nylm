# nylm

Personal server tools as a web app. One C binary serves a static frontend and
a JSON API. Runs on the server as a systemd service, reachable only from
WireGuard and the home LAN.

## How it works

```
browser ── HTTP ──> nylm ──> /api/*  router ──> handler ──> SQLite
                         │                          └──> sudo actions/ (root)
                         └─> /*      files from public/
```

- One process, one request at a time. Every request gets its own memory
  arena, freed when it ends.
- The frontend (`public/`) builds the whole UI in JavaScript; the server only
  serves files and JSON.
- Access: bind to WireGuard + LAN addresses only → drop clients outside the
  allowed subnets → password login with a session cookie.
- nylm runs as user `nylm`. Root work happens only through scripts in
  `/usr/local/lib/nylm/actions/`, via a sudo rule that allows nothing else.

## Where things are

| Path              | What                                                     |
|-------------------|----------------------------------------------------------|
| `src/main.c`      | config from env vars, CLI (`nylm`, `nylm set-password`)  |
| `src/server.c`    | sockets, subnet allowlist, request loop                  |
| `src/http.c`      | parse requests, write responses                          |
| `src/router.c`    | route table: method, path, handler, login required       |
| `src/api_*.c`     | handlers, one file per feature                           |
| `src/auth.c`      | password hashing (Argon2id), sessions                    |
| `src/db.c`        | SQLite connection, migrations                            |
| `migrations/`     | numbered `.sql` files, compiled into the binary          |
| `public/`         | `index.html`, `app.js`, `style.css`                      |
| `tests/`          | unit tests (`test_*.c`), end-to-end (`smoke.sh`)         |
| `deploy/`         | `install.sh`, systemd unit, root action scripts          |
| `vendor/`         | SQLite 3.53.4, cJSON 1.7.19 — upstream, never edited     |

On the server: binary `/usr/local/bin/nylm`, config `/etc/nylm.conf`, data
`/var/lib/nylm/`, frontend `/usr/local/share/nylm/public/`, actions
`/usr/local/lib/nylm/actions/`, sudo rule `/etc/sudoers.d/nylm`.

## Commands

```sh
make run                          # debug build on http://127.0.0.1:8080
make test                         # unit + end-to-end tests
sudo deploy/install.sh            # on the server: install or update
journalctl -u nylm -f             # server logs
sudo -u nylm env NYLM_DB=/var/lib/nylm/nylm.db nylm set-password
```

Configuration is environment variables; `nylm --help` lists them.
Build needs `gcc`, `make`, OpenSSL 3.2+.
