# nylm — Scope

A personal, old-school web app for day-to-day tools, written in C from scratch
and shipped as a Docker image. The tools themselves are defined later; this
document fixes the base they will be built on.

## Pragma

- **Tried and tested.** Prefer boring, well-understood techniques (POSIX sockets,
  Makefiles, SQL) over clever ones.
- **Simplify everything.** Solve the problem in front of us, not the general case.
- **Not a framework.** No abstraction layers "for later". Features are built as
  needed; refactor when duplication actually hurts.
- **Few dependencies.** Three external libraries: two vendored as source,
  plus OpenSSL from the distro.
- **Simple code, fewer bugs.** Less code means fewer places for bugs to hide.
- **One-directional data flow.** The server serves files and data; it never
  builds UI. Fewer paths for data to travel means fewer security holes.

## Stack

| Layer      | Choice                                                        |
|------------|---------------------------------------------------------------|
| Language   | C11, compiled with `gcc` (`-Wall -Wextra -Werror`)            |
| Build      | Plain `Makefile`                                              |
| HTTP       | Hand-written HTTP/1.1 subset over POSIX sockets               |
| Database   | SQLite (amalgamation `sqlite3.c`, vendored, statically linked)|
| JSON       | cJSON (`cJSON.c`/`cJSON.h`, vendored)                         |
| TLS/crypto | OpenSSL 3.2+ (`libssl-dev` from Debian, dynamically linked)   |
| Frontend   | Static HTML, CSS, vanilla JS — served by the C binary         |
| Deployment | Docker, multi-stage build, single container                   |

No other libraries. libc only beyond the three above.

## Architecture

```
browser ──HTTPS─> C binary ──> router ──> /api/*   handlers ──> SQLite file
                                     └──> /*       static files from ./public
```

- **One process, one binary.** Serves both the static frontend and the JSON API.
- **Concurrency:** single-threaded loop — `poll()` on the listening sockets,
  then `accept`, TLS handshake, read request, handle, write, close, one
  connection at a time. Each read/write may block at most 5 s and a whole
  request at most 15 s, so a slow client can delay others but not hang the
  server. This matches SQLite's single-writer model and is enough for one
  user. Revisit (a small thread pool) only if it becomes a measurable problem.
- **Routing:** a static table of `{method, path, handler, public}` in
  `router.c`. Exact matches, or a pattern ending in `/:` that captures one
  more segment (`/api/notes/:` matches `/api/notes/42`).
- **API:** JSON in, JSON out. Frontend talks to it with `fetch()`.
- **Split of responsibilities:**
  - *Backend* serves static files unchanged, and stores and returns data.
    Every piece of incoming data is validated (types, lengths, ranges)
    before it touches the database.
  - *Frontend* owns the whole UI: it builds the DOM, holds the view state,
    and keeps itself in sync with the API. The backend never renders HTML.
- **Memory:** a per-request arena allocator; everything allocated while handling
  a request is freed in one shot when the request ends.
- **Config:** environment variables, no config files (`nylm --help` lists them):

  | Variable                 | Default          | Meaning                                  |
  |--------------------------|------------------|------------------------------------------|
  | `NYLM_DB`                | `nylm.db`        | SQLite file                              |
  | `NYLM_PUBLIC`            | `public`         | static files directory                   |
  | `NYLM_TLS`               | `on`             | `off` = plain HTTP for development       |
  | `NYLM_HTTP_PORT`         | `8080`           | app (TLS off) or redirect + ACME (TLS on)|
  | `NYLM_HTTPS_PORT`        | `8443`           | app when TLS is on                       |
  | `NYLM_CERT` / `NYLM_KEY` | `certs/*.pem`    | PEM certificate chain and key            |
  | `NYLM_ACME_DIR`          | `acme`           | certbot `--webroot` directory            |
  | `NYLM_PUBLIC_HTTPS_PORT` | `443`            | port written into redirects              |
- **Logging:** one line per request to stdout (Docker collects it).

## HTTP — what we support

In: `GET`, `POST`, `PUT`, `DELETE`; request line, headers, `Content-Length`
bodies; query strings; URL decoding; cookies (if auth needs them).

Out: status line, headers, body; `Connection: close`; correct `Content-Type`
for a small fixed set of extensions (html, css, js, json, svg, png, ico).

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

- Reverse proxies; ACME / certificate renewal inside the binary
- HTTP/2, keep-alive, chunked encoding, WebSockets
- Multipart / file uploads (until a feature needs them)
- Server-side templating — pages are static, data comes from the API
- Frontend frameworks, bundlers, npm, CSS preprocessors
- Multi-user, sign-up, password reset by email, horizontal scaling, Windows

## Data

- One SQLite file on a Docker volume.
- Schema migrations: numbered `.sql` files compiled into the binary, applied in
  order at startup, tracked with `PRAGMA user_version`.
- **Always** prepared statements with bound parameters. Never build SQL with
  string formatting.
- `PRAGMA journal_mode=WAL`, `foreign_keys=ON`, `STRICT` tables.

## Authentication

Single user (me), simple login.

- **Library: OpenSSL** (already used for TLS). Argon2id via `EVP_KDF`
  for passwords, SHA-256 for session token hashes, `CRYPTO_memcmp` for
  constant-time comparison.
- **Randomness:** `RAND_bytes` for salts and session tokens.
- **The user** is created/updated from the command line
  (`nylm set-password`), never over HTTP. No sign-up endpoint.
- **Password storage:** Argon2id (19 MiB, 2 passes) hash + random salt in
  SQLite; the cost parameters are stored with the hash so they can change.
  Setting a new password logs out every session.
- **Login:** `POST /api/login` checks the password; on success creates a
  session. A failed attempt sleeps 1 s (the whole server waits — fine for
  one user, and it caps guessing at about one per second).
- **Sessions:** 32 random bytes, sent as a cookie
  (`HttpOnly; Secure; SameSite=Strict; Path=/`).
  Only a SHA-256 hash of the token is stored in the DB, with a 7-day expiry.
  `POST /api/logout` deletes it.
- **Protection:** every `/api/*` route not marked public in the route table
  requires a valid session. Static files are public (they contain no data).
- **CSRF:** `SameSite=Strict` plus API only accepting
  `Content-Type: application/json`.

### Token theft

A session cookie works like a key: whoever holds it is logged in. We keep the
defences simple:

- `HttpOnly` + `Secure` cookie, served over TLS (see below).
- Frontend inserts data with `textContent`, never `innerHTML`.
- Random 256-bit tokens, a fresh one per login, single fixed expiry.
- Logout deletes the session server-side.

If a feature is ever sensitive enough to need more, we add a second layer:
short-lived, single-use privileged tokens for that action only.

## TLS

The binary terminates TLS itself, using OpenSSL. Responsibilities are split
across three places:

**C binary (`tls.c`, `server.c`)**
- Listens on 8443 (HTTPS) and 8080 (HTTP); Docker maps them to 443 and 80.
- Port 80 does exactly two things: serves `/.well-known/acme-challenge/*`
  from a mounted directory (for certificate renewal) and redirects
  everything else to HTTPS with `301`.
- Loads certificate and key from paths given by env vars, at startup.
- Minimum TLS 1.2; OpenSSL's default cipher list (no hand-tuning).
- Handshake bounded by the same socket timeouts as reads/writes.
- Sends `Strict-Transport-Security` on every HTTPS response.
- `NYLM_TLS=off` for local development over plain HTTP (also drops the
  `Secure` cookie flag).

**Docker**
- Runtime image installs `libssl3t64`; build image installs `libssl-dev`.
- Publishes ports 80 and 443.
- Mounts `/srv/nylm/certs` and the ACME webroot `/srv/nylm/acme` read-only.

**Host server**
- Domain name with DNS pointing to the server.
- `certbot` (distro package, comes with a renewal timer) in `--webroot`
  mode, writing challenges into the directory the app serves on port 80.
- Renewal deploy hook (`deploy/certbot-deploy-hook.sh`): copies the cert and
  key to `/srv/nylm/certs` readable by the container's uid 10001 (certbot's
  keys are root-only), then `docker restart nylm`.
- First certificate: `certbot --standalone` (nylm cannot start without one),
  then `certbot reconfigure` to webroot. Step by step in `deploy/README.md`.
- Firewall: only 80 and 443 open.

**Development:** `make cert` (self-signed for localhost), or `NYLM_TLS=off`
(`make run`).

Security updates for OpenSSL come from rebuilding the image on a fresh
Debian base.

## Security baseline

Even for a personal app:

- Static file serving rejects `..` and resolves paths inside the public dir only.
- All input lengths checked; all buffers bounded.
- Debug builds run with `-fsanitize=address,undefined`.
- Container runs as a non-root user with a read-only root filesystem and
  all capabilities dropped.
- Every response sets `Content-Security-Policy`, `X-Content-Type-Options:
  nosniff` and `Referrer-Policy: no-referrer`.

## Project layout

```
nylm/
├── scope.md
├── Makefile           # make, make debug, make run, make test, make cert
├── Dockerfile
├── compose.yaml
├── src/
│   ├── main.c         # env config, CLI (serve / set-password)
│   ├── server.c/.h    # listeners, accept loop, app vs redirect handling
│   ├── conn.c/.h      # socket I/O (plain or TLS), timeouts
│   ├── tls.c/.h       # OpenSSL context and handshake
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
├── vendor/
│   ├── sqlite/        # sqlite3.c, sqlite3.h (3.53.4)
│   └── cjson/         # cJSON.c, cJSON.h (1.7.19)
├── public/            # index.html, style.css, app.js, favicon.svg
├── deploy/            # host setup guide, certbot deploy hook
└── tests/
    ├── test.h         # CHECK macros
    ├── test_*.c       # unit tests, one binary each
    └── smoke.sh       # curl against a running server, HTTP and TLS
```

## Docker

- **Build stage:** `debian:trixie-slim` + `gcc`/`make`/`libssl-dev`; vendored
  objects are built in their own layer, then the release binary.
- **Runtime stage:** `debian:trixie-slim` + `libssl3t64`, user `nylm`
  (uid 10001), the binary and `public/`. DB on a named volume (`/data`).
- `compose.yaml`: one service, ports 80/443, read-only root, `/tmp` tmpfs.
- `SIGTERM` finishes the current request and exits cleanly.

## Testing

- Unit tests for the parts that are easy to get wrong: HTTP parsing, URL
  decoding, path sanitising, routing, cookie parsing, redirect targets.
- `smoke.sh`: start the binary in plain and TLS mode, hit every route with
  `curl`, check status codes and headers, fail on any sanitizer report.
- `make test` runs both. No test framework.

## Milestones

All done (2026-10-02).

1. **Skeleton** — Makefile, `main.c`, server answers every request with
   `200 hello`. Builds clean with warnings-as-errors.
2. **HTTP** — proper request parsing, limits, error responses, request log.
3. **Static files** — serve `public/` safely with correct content types.
4. **Router + JSON** — route table, cJSON wired in, `GET /api/health`.
5. **SQLite** — open DB, migrations, one example table with CRUD endpoints.
6. **Frontend shell** — `index.html` + `app.js` that calls the API.
7. **Auth** — Argon2id via OpenSSL, `set-password` command, login/logout, session check.
8. **TLS** — OpenSSL, port 80 redirect + ACME webroot, HSTS, self-signed
   cert for development.
9. **Docker** — multi-stage image, volumes for DB and certs, runs as non-root.
10. **Tests** — unit tests + smoke script, `make test`.

After milestone 10 the base is done and we start adding actual tools.

## Open questions

- **Tools:** what the app actually does — to be defined in a later session.
