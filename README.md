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
| `src/care.c`      | plant care dates: when a care rule is next due           |
| `src/auth.c`      | password hashing (Argon2id), sessions                    |
| `src/db.c`        | SQLite connection, applies migrations                    |
| `src/migrations.c`| the schema, one appended entry per change                |
| `public/`         | `index.html`, `app.js`, `style.css`                      |
| `tests/`          | unit tests (`test_*.c`), end-to-end (`smoke.sh`)         |
| `deploy/`         | `install.sh`, systemd unit, root action scripts          |

On the server: binary `/usr/local/bin/nylm`, config `/etc/nylm.conf`, data
`/var/lib/nylm/`, frontend `/usr/local/share/nylm/public/`, actions
`/usr/local/lib/nylm/actions/`, sudo rule `/etc/sudoers.d/nylm`.

## Plants

Plant care tracker (`src/api_plants.c`, API under `/api/plants`). Plants and
care types (watering, fertilising, ...) are archived, never deleted. A care
rule per plant and type says how often it is due: every N days, with optional
seasonal periods that change the interval or pause it, or once a year on a
fixed date. Logging a care entry makes the next due date count from that day;
nothing comes due during a pause. Due dates are computed on every read, from
the server's local date (the system time zone).

## Commands

```sh
make run                          # debug build on http://127.0.0.1:8080
make test                         # unit + end-to-end tests
sudo deploy/install.sh            # on the server: install or update
journalctl -u nylm -f             # server logs
sudo -u nylm env NYLM_DB=/var/lib/nylm/nylm.db nylm set-password
```

Configuration is environment variables; `nylm --help` lists them.
Build needs `gcc`, `make` and the system libraries `sqlite` (3.38+), `cjson`
and `openssl` (3.2+), linked dynamically: `pacman -Syu` brings their fixes.
