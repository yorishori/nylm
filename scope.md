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
- **Few dependencies.** Only three external libraries, all vendored as source.
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
| Crypto     | Monocypher (`monocypher.c`/`.h`, vendored) — password hashing |
| Frontend   | Static HTML, CSS, vanilla JS — served by the C binary         |
| Deployment | Docker, multi-stage build, single container                   |

No other libraries. libc only beyond the three above.

## Architecture

```
browser ──HTTP──> C binary ──> router ──> /api/*   handlers ──> SQLite file
                                     └──> /*       static files from ./public
```

- **One process, one binary.** Serves both the static frontend and the JSON API.
- **Concurrency:** single-threaded loop — `accept`, read request, handle, write,
  close. Socket timeouts so a slow client can't hang the server. This matches
  SQLite's single-writer model and is enough for one user. Revisit (`poll()` or
  a small thread pool) only if it becomes a measurable problem.
- **Routing:** a static table of `{method, path, handler}` in one file. Exact
  matches, plus simple prefix match for routes with an ID (`/api/notes/42`).
- **API:** JSON in, JSON out. Frontend talks to it with `fetch()`.
- **Split of responsibilities:**
  - *Backend* serves static files unchanged, and stores and returns data.
    Every piece of incoming data is validated (types, lengths, ranges)
    before it touches the database.
  - *Frontend* owns the whole UI: it builds the DOM, holds the view state,
    and keeps itself in sync with the API. The backend never renders HTML.
- **Memory:** a per-request arena allocator; everything allocated while handling
  a request is freed in one shot when the request ends.
- **Config:** environment variables (port, DB path, static dir). No config files.
- **Logging:** one line per request to stdout (Docker collects it).

## HTTP — what we support

In: `GET`, `POST`, `PUT`, `DELETE`; request line, headers, `Content-Length`
bodies; query strings; URL decoding; cookies (if auth needs them).

Out: status line, headers, body; `Connection: close`; correct `Content-Type`
for a small fixed set of extensions (html, css, js, json, svg, png, ico).

Hard limits: max header size, max body size, max path length. Anything over a
limit gets `413`/`431`, never a buffer overrun.

## Explicitly out of scope

- TLS (terminate it in a reverse proxy if the app is ever exposed)
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
- `PRAGMA journal_mode=WAL`, `foreign_keys=ON`.

## Authentication

Single user (me), simple login.

- **Library: Monocypher.** One `.c`/`.h` pair, audited, no dependencies.
  Provides Argon2 (password hashing), BLAKE2b, and constant-time comparison.
- **Randomness:** `getrandom(2)` from libc for salts and session tokens.
- **The user** is created/updated from the command line
  (`nylm set-password`), never over HTTP. No sign-up endpoint.
- **Password storage:** Argon2 hash + random salt in SQLite.
- **Login:** `POST /api/login` checks the password; on success creates a
  session. A fixed delay on failure slows down guessing.
- **Sessions:** 32 random bytes, sent as a cookie
  (`HttpOnly; SameSite=Strict; Path=/`, plus `Secure` when behind TLS).
  Only a BLAKE2b hash of the token is stored in the DB, with an expiry.
  `POST /api/logout` deletes it.
- **Protection:** every `/api/*` route except `/api/login` requires a valid
  session. Static files are public (they contain no data).
- **CSRF:** `SameSite=Strict` plus API only accepting
  `Content-Type: application/json`.

### Token theft

A session cookie works like a key: whoever holds it is logged in. There are
three main ways it can be stolen, and each one is blocked separately:

| Way it is stolen               | Defence                                           |
|--------------------------------|---------------------------------------------------|
| Read off the network           | TLS whenever the app is reached by anything other than localhost; cookie marked `Secure` |
| Read by injected JS (XSS)      | `HttpOnly` cookie; frontend never puts data into `innerHTML`, only `textContent`; `Content-Security-Policy: default-src 'self'` (no inline or third-party scripts) |
| Guessed or planted             | 256-bit random tokens; a fresh token on every login; unknown tokens are rejected, never adopted |

Limiting the damage if a token is stolen anyway:

- **Two expiries:** an absolute lifetime (e.g. 7 days) and an idle timeout
  (e.g. 24 h without use), both checked by the server.
- **Revocation:** `POST /api/logout` kills the current session;
  `nylm logout-all` (CLI) deletes every session.
- **No binding to IP or User-Agent:** IPs change (mobile, VPN) and
  User-Agents are easy to fake, so this adds complexity without real security.

Out of reach: someone with access to the unlocked machine or browser profile.

## Security baseline

Even for a personal app:

- Static file serving rejects `..` and resolves paths inside the public dir only.
- All input lengths checked; all buffers bounded.
- Debug builds run with `-fsanitize=address,undefined`.
- Container runs as a non-root user.
- Every response sets `Content-Security-Policy`, `X-Content-Type-Options:
  nosniff` and `Referrer-Policy: no-referrer`.

## Project layout

```
nylm/
├── scope.md
├── Makefile
├── Dockerfile
├── src/
│   ├── main.c        # startup, config, accept loop
│   ├── http.c/.h     # request parsing, response writing
│   ├── router.c/.h   # route table
│   ├── db.c/.h       # SQLite open, migrations, helpers
│   ├── arena.c/.h    # per-request allocator
│   ├── auth.c/.h     # password check, sessions
│   └── api_*.c       # one file per feature
├── migrations/       # 001_init.sql, 002_...sql
├── vendor/
│   ├── sqlite/       # sqlite3.c, sqlite3.h
│   ├── cjson/        # cJSON.c, cJSON.h
│   └── monocypher/   # monocypher.c, monocypher.h
├── public/           # index.html, style.css, app.js
└── tests/
    ├── test_*.c      # unit tests (plain asserts, no framework)
    └── smoke.sh      # curl against a running server
```

## Docker

- **Build stage:** `debian:stable-slim` + `gcc`/`make`; builds a release binary.
- **Runtime stage:** `debian:stable-slim`, non-root user, copies the binary and
  `public/`. DB lives on a mounted volume (`/data`).
- One `docker run` (or a minimal `compose.yaml`) is all it takes to start it.

## Testing

- Unit tests for the parts that are easy to get wrong: HTTP parsing, URL
  decoding, path sanitising, routing.
- `smoke.sh`: start the binary, hit every route with `curl`, check status codes.
- `make test` runs both. No test framework.

## Milestones

1. **Skeleton** — Makefile, `main.c`, server answers every request with
   `200 hello`. Builds clean with warnings-as-errors.
2. **HTTP** — proper request parsing, limits, error responses, request log.
3. **Static files** — serve `public/` safely with correct content types.
4. **Router + JSON** — route table, cJSON wired in, `GET /api/health`.
5. **SQLite** — open DB, migrations, one example table with CRUD endpoints.
6. **Frontend shell** — `index.html` + `app.js` that calls the API.
7. **Auth** — Monocypher, `set-password` command, login/logout, session check.
8. **Docker** — multi-stage image, volume for the DB, runs as non-root.
9. **Tests** — unit tests + smoke script, `make test`.

After milestone 9 the base is done and we start adding actual tools.

## Open questions

- **Tools:** what the app actually does — to be defined in a later session.
