# nylm — Scope

A personal, old-school web app for day-to-day server tools (Docker health,
backups, server health, updates, reboot), written in C from scratch. It runs
directly on the server as a systemd service and is reachable only from the
home LAN and WireGuard. The tools themselves are defined later; this document
fixes the base they are built on.

## Pragma

- **Tried and tested.** Prefer boring, well-understood techniques (POSIX sockets,
  Makefiles, SQL, systemd, sudo) over clever ones.
- **Simplify everything.** Solve the problem in front of us, not the general case.
- **Not a framework.** No abstraction layers "for later". Features are built as
  needed; refactor when duplication actually hurts. Unused code is deleted.
- **Few dependencies.** SQLite and cJSON vendored as source; OpenSSL's
  libcrypto from the distro for password hashing.
- **Simple code, fewer bugs.** Less code means fewer places for bugs to hide.
- **One-directional data flow.** The server serves files and data; it never
  builds UI. Fewer paths for data to travel means fewer security holes.

## Stack

| Layer      | Choice                                                         |
|------------|----------------------------------------------------------------|
| Language   | C11, compiled with `gcc` (`-Wall -Wextra -Werror` and more)    |
| Build      | Plain `Makefile`                                               |
| HTTP       | Hand-written HTTP/1.1 subset over POSIX sockets, plain HTTP    |
| Database   | SQLite 3.53.4 (amalgamation, vendored, statically linked)      |
| JSON       | cJSON 1.7.19 (vendored)                                        |
| Crypto     | OpenSSL 3.2+ libcrypto: Argon2id, SHA-256, random bytes        |
| Frontend   | Static HTML, CSS, vanilla JS — served by the C binary          |
| Deployment | systemd service on the host (Arch Linux), `deploy/install.sh`  |

No other libraries. libc only beyond the three above.

## Architecture

```
phone ──WireGuard──┐
                   ├─HTTP─> nylm (user nylm) ──> /api/*  handlers ──> SQLite
PC ────home LAN────┘                       │              └──sudo──> actions/ (root)
                                           └──> /*      static files
```

- **One process, one binary**, running as the unprivileged `nylm` user.
  Serves both the static frontend and the JSON API.
- **Concurrency:** single-threaded loop — `poll()` on the listening sockets,
  then `accept`, read request, handle, write, close, one connection at a time.
  Each read/write may block at most 5 s and a whole request at most 15 s, so a
  slow client can delay others but not hang the server. This matches SQLite's
  single-writer model and is enough for one user. Revisit (a small thread
  pool) only if it becomes a measurable problem.
- **Routing:** a static table of `{method, path, handler, public}` in
  `router.c`. Exact matches, or a pattern ending in `/:` that captures one
  more segment (`/api/notes/:` matches `/api/notes/42`).
- **API:** JSON in, JSON out. Frontend talks to it with `fetch()`.
- **Split of responsibilities:**
  - *Backend* serves static files unchanged, and stores and returns data.
    Every piece of incoming data is validated (types, lengths, ranges)
    before it touches the database or an action.
  - *Frontend* owns the whole UI: it builds the DOM, holds the view state,
    and keeps itself in sync with the API. The backend never renders HTML.
- **Memory:** a per-request arena allocator; everything allocated while handling
  a request is freed in one shot when the request ends.
- **Config:** environment variables (`/etc/nylm.conf` on the server, via the
  systemd unit). `nylm --help` lists them:

  | Variable      | Default       | Meaning                                         |
  |---------------|---------------|-------------------------------------------------|
  | `NYLM_DB`     | `nylm.db`     | SQLite file                                     |
  | `NYLM_PUBLIC` | `public`      | static files directory                          |
  | `NYLM_PORT`   | `8080`        | port                                            |
  | `NYLM_LISTEN` | `127.0.0.1`   | addresses to bind (space/comma separated)       |
  | `NYLM_ALLOW`  | `127.0.0.0/8` | client subnets accepted (space/comma separated) |

- **Logging:** one line per request to stdout (journald collects it).
- **Shutdown:** `SIGTERM` finishes the current request and exits cleanly.

## Network access

nylm is only for me, from two places: my phone over WireGuard and my PC on
the home LAN. Three independent layers:

1. **Bind** only to the WireGuard address (`10.0.0.1`) and the server's LAN
   address — never `0.0.0.0`. The router does not forward nylm's port, so the
   internet cannot reach it at all.
2. **Allowlist:** a connection whose source address is not in `NYLM_ALLOW`
   (`10.0.0.0/24` and the LAN subnet) is closed before anything is read, and
   logged.
3. **Login:** password + session cookie (below).

No TLS. Over WireGuard the tunnel encrypts and authenticates; on the home LAN
traffic is plain HTTP, which is accepted as a known trade-off (anyone on the
home network could read it). If that changes, the PC gets WireGuard too and
the LAN address is dropped from both settings.

## Privileged actions

Tools like backups, updates and reboot need root. nylm itself never runs as
root and is not in the `docker` group (which is root-equivalent).

- Each privileged operation is a small script in
  `/usr/local/lib/nylm/actions/` (source: `deploy/actions/`), root-owned and
  not writable by `nylm`.
- `/etc/sudoers.d/nylm` lets user `nylm` run the programs directly in that
  folder as root, and nothing else (`sudo -n`, no password, no shell).
- nylm runs actions with `execve` (no shell), from a fixed list of names.
  Arguments, if any, come from a fixed set or are strictly validated —
  never free text from the request. Each script validates its own arguments
  again.
- Read-only status (disk, memory, uptime) is read from `/proc`, `df` etc.
  without privileges.
- Dangerous actions (reboot, update, restore) will ask for the password again
  (a short-lived, single-use token) and are written to an audit log.
- systemd filesystem sandboxing is deliberately not used: it would also
  confine the root actions. The privilege boundary is the user + sudo rule.

The C side (running an action and capturing its output) is built with the
first tool that needs it.

## HTTP — what we support

In: `GET`, `POST`, `PUT`, `DELETE`; request line, headers, `Content-Length`
bodies; query strings; URL decoding; cookies.

Out: status line, headers, body; `Connection: close`; correct `Content-Type`
for a small fixed set of extensions (html, css, js, json, svg, png, ico, txt).

Hard limits: 8 KiB of headers (64 max), 1 MiB body, 2 KiB path. Anything over
a limit gets `413`/`414`/`431`, never a buffer overrun. Unsupported methods and
`Transfer-Encoding` get `501`.

## API

| Method | Path             | Login | Notes                                     |
|--------|------------------|-------|-------------------------------------------|
| GET    | `/api/health`    | no    | `{"status":"ok"}`                         |
| POST   | `/api/login`     | no    | `{"password"}` → `204` + session cookie   |
| POST   | `/api/logout`    | no    | deletes the session, clears the cookie    |
| GET    | `/api/session`   | yes   | `204` if logged in, else `401`            |
| GET    | `/api/notes`     | yes   | example feature: list                     |
| POST   | `/api/notes`     | yes   | `{"title","body"}` → `201` note           |
| GET    | `/api/notes/:id` | yes   |                                           |
| PUT    | `/api/notes/:id` | yes   | `{"title","body"}`                        |
| DELETE | `/api/notes/:id` | yes   | `204`                                     |

Errors are `{"error": "message"}` with a matching status code. Requests
with a body must send `Content-Type: application/json` (else `415`).

## Explicitly out of scope

- TLS, certificates, Docker, reverse proxies
- Exposure to the internet; IPv6
- HTTP/2, keep-alive, chunked encoding, WebSockets
- Multipart / file uploads (until a feature needs them)
- Server-side templating — pages are static, data comes from the API
- Frontend frameworks, bundlers, npm, CSS preprocessors
- Multi-user, sign-up, password reset by email, horizontal scaling

## Data

- One SQLite file in `/var/lib/nylm` (directory owned by `nylm`, mode 700).
- Schema migrations: numbered `.sql` files compiled into the binary, applied in
  order at startup, tracked with `PRAGMA user_version`.
- **Always** prepared statements with bound parameters. Never build SQL with
  string formatting.
- `PRAGMA journal_mode=WAL`, `foreign_keys=ON`, `STRICT` tables.

## Authentication

Single user (me), simple login.

- **Library: OpenSSL libcrypto.** Argon2id via `EVP_KDF` for passwords,
  SHA-256 for session token hashes, `CRYPTO_memcmp` for constant-time
  comparison, `RAND_bytes` for salts and tokens.
- **The user** is created/updated from the command line
  (`nylm set-password`) on the server, never over HTTP. No sign-up endpoint.
- **Password storage:** Argon2id (19 MiB, 2 passes) hash + random salt in
  SQLite; the cost parameters are stored with the hash so they can change.
  Setting a new password logs out every session.
- **Login:** `POST /api/login` checks the password; on success creates a
  session. A failed attempt sleeps 1 s (the whole server waits — fine for
  one user, and it caps guessing at about one per second).
- **Sessions:** 32 random bytes, sent as a cookie
  (`HttpOnly; SameSite=Strict; Path=/`; no `Secure`, since there is no TLS).
  Only a SHA-256 hash of the token is stored in the DB, with a 7-day expiry.
  `POST /api/logout` deletes it.
- **Protection:** every `/api/*` route not marked public in the route table
  requires a valid session. Static files are public (they contain no data).
- **CSRF:** `SameSite=Strict` plus API only accepting
  `Content-Type: application/json`.

### Token theft

A session cookie works like a key: whoever holds it is logged in. Defences:

- `HttpOnly` cookie; WireGuard encryption off the LAN.
- Frontend inserts data with `textContent`, never `innerHTML`; strict CSP.
- Random 256-bit tokens, a fresh one per login, single fixed expiry.
- Logout deletes the session server-side.

If a feature is sensitive enough to need more, it gets a second layer:
short-lived, single-use privileged tokens for that action only (see
Privileged actions).

## Security baseline

- Static file serving rejects `..`, hidden segments and backslashes, and
  resolves paths inside the public dir only.
- All input lengths checked; all buffers bounded.
- Debug builds and tests run with `-fsanitize=address,undefined`.
- Every response sets `Content-Security-Policy`, `X-Content-Type-Options:
  nosniff` and `Referrer-Policy: no-referrer`.

## Project layout

```
nylm/
├── scope.md
├── README.md
├── Makefile           # make, make debug, make run, make test
├── src/
│   ├── main.c         # env config, CLI (serve / set-password)
│   ├── server.c/.h    # listeners, allowlist, accept loop, request handling
│   ├── conn.c/.h      # socket I/O, timeouts
│   ├── http.c/.h      # request parsing, response writing
│   ├── static.c/.h    # safe static file serving
│   ├── router.c/.h    # route table, auth check
│   ├── json.c/.h      # cJSON on the arena, reply/validation helpers
│   ├── db.c/.h        # SQLite open, pragmas, migrations
│   ├── arena.c/.h     # per-request allocator
│   ├── auth.c/.h      # Argon2id passwords, sessions, cookies
│   ├── api.h          # handler declarations
│   └── api_*.c        # one file per feature (session, notes, ...)
├── migrations/        # 001_notes.sql, 002_auth.sql, ... (compiled in)
├── tools/
│   └── embed-migrations.sh
├── vendor/            # sqlite/, cjson/ — upstream files, see vendor/README.md
├── public/            # index.html, style.css, app.js, favicon.svg
├── deploy/
│   ├── install.sh     # install/update on the server
│   ├── nylm.service   # systemd unit
│   ├── README.md      # server operations
│   └── actions/       # root action scripts (added with the first tool)
└── tests/
    ├── test.h         # CHECK macros
    ├── test_*.c       # unit tests, one binary each
    └── smoke.sh       # curl against a running server
```

## Deployment

`sudo deploy/install.sh` on the server (Arch Linux), from the repo:

1. Installs build dependencies with pacman; builds and runs `make test`.
2. Creates the system user `nylm` and `/var/lib/nylm`.
3. Installs the binary, `public/`, actions, the systemd unit and the sudo rule
   (checked with `visudo`).
4. First run only: writes `/etc/nylm.conf` with the WireGuard (`wg0`) and LAN
   addresses detected, and asks for the login password.
5. Enables and restarts `nylm.service`.

Updating is `git pull` + the same command. Details in `deploy/README.md`.

## Testing

- Unit tests for the parts that are easy to get wrong: HTTP parsing, URL
  decoding, path sanitising, routing, cookie parsing, subnet parsing and
  matching.
- `smoke.sh`: start the binary, hit every route with `curl`, check status
  codes and headers, check listen addresses and the allowlist, fail on any
  sanitizer report.
- `make test` runs both, and `install.sh` runs `make test` before installing.

## Milestones

1–10 built the base (2026-10-02): skeleton, HTTP, static files, router + JSON,
SQLite, frontend, auth, TLS, Docker, tests. Milestone 11 replaced TLS and
Docker:

11. **Host deployment** — plain HTTP bound to WireGuard + LAN with a client
    allowlist; systemd service under user `nylm`; sudo rule for root actions;
    `deploy/install.sh`. TLS, ACME and Docker removed.

Next: the actual tools.

## Open questions

- **Tools:** what the app actually does — to be defined in a later session.
