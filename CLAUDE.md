# CLAUDE.md — rules for working on nylm

nylm is a personal web app that manages the server it runs on. It is written in
C with no frameworks. Read `README.md` for how it works and where things are.

These rules are requirements, not suggestions. "MUST" and "NEVER" are absolute.
If a rule blocks the task, stop and ask the user; do not work around it.

## 1. What "simple" means here

1. Build only what the current feature needs. NEVER add options, parameters,
   abstraction layers, plugin points or configuration for a need that does not
   exist yet.
2. Build that feature completely and correctly the first time. Refactor later,
   when real duplication or a real new need appears — not before.
3. Simple NEVER means skipping any of the following. Each one is mandatory for
   every change:
   - checking the return value of every call that can fail, and handling it;
   - validating every input (type, length, range, allowed characters);
   - bounding every buffer and every loop that depends on input;
   - replying with a correct HTTP status and a clear error message;
   - logging server-side failures to stderr with enough context to debug;
   - tests (section 7).
4. Prefer the tried-and-tested solution (POSIX, libc, SQL, systemd, sudo) over
   a clever one. If two solutions are equally correct, pick the one with less
   code and fewer moving parts.
5. Delete code that is no longer used, in the same change that stops using it.
   NEVER leave dead code, commented-out code or "for later" stubs.

## 2. Dependencies

1. The only libraries are libc, SQLite, cJSON, OpenSSL libcrypto, TagLib,
   libjpeg-turbo and libpng, all linked dynamically from the distro packages
   (`sqlite`, `cjson`, `openssl`, `taglib`, `libjpeg-turbo`, `libpng`).
   NEVER add another library, tool, framework, package manager or build step,
   and NEVER copy library source into the repo, without the user's explicit
   approval.
2. Frontend: plain HTML, CSS and JavaScript only. NEVER add a framework,
   bundler, transpiler, npm package or CDN script.

## 3. Architecture

1. Data flows one way. The backend serves files from `public/` unchanged and
   answers JSON. The backend NEVER generates HTML.
2. The frontend builds all UI in JavaScript. Each app is a page:
   `public/<app>/index.html` with its own `<app>.js` (and `<app>.css`);
   the home page is `public/index.html` + `home.js`. Anything more than one
   page needs (helpers, session, router, dropdown, calendar, colour picker)
   lives in `public/common.js`, and look shared by all pages in
   `public/style.css`. Use the common dropdown and calendar, never native
   `<select>` or date inputs. Data goes in with `textContent` /
   `createTextNode` (the `el()` helper) and NEVER with `innerHTML`,
   `outerHTML`, `insertAdjacentHTML`, `document.write` or `eval`.
3. NEVER add inline `<script>`, inline `style=""` attributes or `on*=""`
   HTML attributes: the Content-Security-Policy blocks them.
4. Routes live only in the table in `src/router.c`. Handlers live in
   `src/api_<feature>.c`, one file per feature, declared in `src/api.h`.
5. New routes MUST have `public = 0` (login required). Making a route public
   requires the user's explicit approval.
6. Everything nylm saves (databases, files) lives in the `NYLM_DATA` folder,
   in the app's own subfolder: `<NYLM_DATA>/<app>/`. Each app has its own
   SQLite database there, its own connection (`src/db.h`) and its own
   migrations list in `src/migrations.c`; code uses only its own app's
   connection. Schema changes are a new entry appended to that app's list.
   NEVER edit, reorder or delete an existing entry.
   New tables are `STRICT`.
7. The server is single-threaded on purpose. NEVER add threads, forks for
   request handling, or async I/O without the user's approval.

## 4. C rules

1. C11, `gcc`. The build uses `-Wall -Wextra -Werror -Wpedantic -Wshadow
   -Wconversion`. NEVER weaken these flags or silence a warning with a cast or
   pragma just to make it compile; fix the cause.
2. NEVER use `strcpy`, `strcat`, `sprintf`, `gets`, `strtok` (non-`_r`),
   `atoi`/`atol`, or `system`/`popen`. Use `snprintf`, `strtol` with full
   error checks, and `execve`.
3. Memory needed during a request comes from the request arena
   (`arena_alloc`). Check every allocation for `NULL`.
4. SQL MUST use `sqlite3_prepare_v2` with `?` placeholders and
   `sqlite3_bind_*`. NEVER build SQL text from data. Check every
   `sqlite3_step` result. Writes that must succeed together go in one
   transaction.
5. Match the existing style: 4-space indent, braces as in the surrounding
   code, a short comment on every non-obvious function, `snake_case`.

## 5. Security rules

1. NEVER change these without the user's explicit approval:
   - binding only to `NYLM_LISTEN` (never `0.0.0.0`) and the `NYLM_ALLOW`
     subnet check;
   - the HTTP limits (header/body/path sizes, timeouts);
   - the response security headers (CSP, nosniff, Referrer-Policy);
   - the session cookie attributes (`HttpOnly; SameSite=Strict`);
   - the `application/json` requirement on request bodies;
   - Argon2id parameters (they may only go up), the 1 s delay on failed login;
   - static file path checks in `src/static.c`.
2. NEVER log or return passwords, session tokens, cookies, or password hashes.
3. NEVER commit secrets, databases (`*.db`), or machine-specific config.
4. Every value from a request (path, query, header, cookie, body field) is
   untrusted. Validate it in the handler before it reaches the database, the
   filesystem, or an action.

## 6. Privileged actions (anything that needs root)

nylm runs as the unprivileged user `nylm`. Root work happens only like this:

1. Each action is one script in `deploy/actions/<name>`, installed root-owned,
   mode 755, into `/usr/local/lib/nylm/actions/`. The sudo rule allows `nylm`
   to run only files in that directory.
2. Scripts start with `#!/bin/sh` and `set -eu`, use absolute paths for every
   command, and validate every argument against a fixed allowlist or a strict
   pattern; anything else exits non-zero with a message on stderr.
3. C code runs an action only with `execve("/usr/bin/sudo", ...)` and
   `sudo -n /usr/local/lib/nylm/actions/<name>`, where `<name>` is a string
   constant in the code. NEVER go through a shell. NEVER pass request text
   as an argument unless it was validated against the same allowlist or pattern
   as in the script.
4. NEVER add `nylm` to the `docker` group or any other privileged group, and
   NEVER widen `/etc/sudoers.d/nylm`.
5. Dangerous actions (reboot, update, restore, delete) MUST require the
   password again in the same request and MUST be written to an audit log
   (who, what, when, result).
6. Read-only status that needs no root (`/proc`, `df`, ...) MUST NOT use an
   action.

## 7. Tests

1. Every new parser or validator gets unit tests in `tests/test_*.c` covering
   valid input, each rejection path, and boundaries (empty, max, max + 1).
2. Every new route gets checks in `tests/smoke.sh`: success, `401` without
   login, and each validation error.
3. Run `make test` before saying a change is done. It MUST pass with zero
   failures and no sanitizer reports. If it fails, say so and show the output.
4. NEVER weaken, skip or delete a test to make it pass.

## 8. Workflow

1. Make one change (one feature or one fix) at a time, then commit it with a
   message that says what changed and why.
2. Update `README.md` in the same commit when paths, commands or
   configuration change. Keep it short.
3. When a requirement is unclear, or two rules conflict, ask the user before
   writing code. Do not guess on anything security-related.
