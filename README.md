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
- The frontend (`public/`) builds the whole UI in JavaScript, one page per app
  (`/plants/`) plus the home page; the server only serves files and JSON.
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
| `src/music.c`     | music folder, lock, `nylm music-scan` and `music-write`  |
| `src/tags.c`      | music file tags through TagLib: read, write, verify      |
| `src/action.c`    | runs root actions through `sudo -n`                      |
| `src/auth.c`      | password hashing (Argon2id), sessions                    |
| `src/db.c`        | SQLite connection, applies migrations                    |
| `src/migrations.c`| the schema, one appended entry per change                |
| `public/`         | home page, `common.js` + `style.css` shared by all pages |
| `public/<app>/`   | one page per app: `index.html`, `<app>.js`, `<app>.css`  |
| `tests/`          | unit tests (`test_*.c`), end-to-end (`smoke.sh`)         |
| `deploy/`         | `install.sh`, systemd unit, root action scripts          |

On the server: binary `/usr/local/bin/nylm`, config `/etc/nylm.conf`, data
in `NYLM_DATA` (on the data drive), frontend `/usr/local/share/nylm/public/`, actions
`/usr/local/lib/nylm/actions/`, sudo rule `/etc/sudoers.d/nylm`.

## Data

Everything nylm saves lives in one folder, `NYLM_DATA` in `/etc/nylm.conf`.
Each app has its own subfolder and SQLite database in it:

```
$NYLM_DATA/core/core.db       login password and sessions
$NYLM_DATA/plants/plants.db   plant care
$NYLM_DATA/music/music.db     music tags cache, changes, scans, audit log
```

nylm refuses to start if the folder does not exist. In an empty folder it
creates every app's folder and database: a fresh install (set a password).
Point `NYLM_DATA` at a folder *inside* the drive (`/mnt/data/nylm`), not at
the mount point: when the drive is not mounted the folder is missing, so nylm
stops (and systemd retries) instead of starting empty on the system disk.
Create the folder and make it writable by user `nylm` before installing.

## Plants

Plant care tracker (`src/api_plants.c`, API under `/api/plants`). Plants and
care types (watering, fertilising, ...) are archived, never deleted. A care
rule per plant and type says how often it is due: every N days, with optional
seasonal periods that change the interval or pause it, or once a year on a
fixed date. Logging a care entry makes the next due date count from that day;
nothing comes due during a pause. Due dates are computed on every read, from
the server's local date (the system time zone).

Each plant and care type has a colour from a fixed palette (butter, lime,
mint, teal, sky, periwinkle, lavender, orchid). In the app a plant's colour
is the stripe on its card and the dot before its name, a care type's colour
is its chip, and only late (rose) and today (peach) colour the due label.

## Music

Album-first tag editor for the music folder `NYLM_MUSIC` (`src/api_music.c`,
API under `/api/music`). The files are the truth: `music.db` caches their
tags. An album is a folder; its tracks are the `.mp3` and `.flac` files in
it. Dot files are skipped and symlinks never followed.

The server never opens a music file. It reads the cache, queues changes,
and starts the two services that do work on the files, each its own
process, started by hand or from the web app (password again, recorded in
the `audit` table) through a root action that only starts its unit:

- `nylm music-scan` (`nylm-music-scan.service`): reads new and changed
  files (size or mtime) into the cache, drops files that are gone.
- `nylm music-write` (`nylm-music-write.service`): writes the pending rows
  of the `changes` table into the files.

They never run together: each holds `$NYLM_DATA/music/library.lock`
exclusively. The server holds it shared for its own writes to `music.db`,
and refuses to queue or cancel anything while a service runs (409).

Editing an album queues album-wide changes (album, album artist, genre,
date, compilation) and per-track ones (title, artist, track and disc
number): one `changes` row per tag of a file, with the value it had when
queued. Pending changes can be cancelled. Every track needs a track and a
disc number: an album's changes are refused while one would have none
(the editor fills in a missing disc number as `1/1`).

The rules for a value: one line of UTF-8, at most 500 bytes, no leading
or trailing space; title, artist, album, album artist, track and disc
number can not be empty; dates `YYYY[-MM[-DD]]`; numbers `N` or `N/M`;
genres lowercase `a-z` and `-`, several separated by `"; "`
(`rock; pop-punk`), written as one string.

The write service, per file, only through TagLib (`src/tags.c`): checks
each value again against those rules and that the file still has the
queued-against value, sets the tags and saves, then reads the file back.
Each change ends `done`, `failed` (with the cause), or `warning`:
written, but TagLib also changed other tags, pictures or audio
properties, listed in its note. It only changes tags, never a file's
path. A tag with several values in the file is not changed, except the
genre: its values are replaced by the one new string.

TagLib writes in place. It saves MP3 tags as ID3v2.4 (upgrading ID3v2.3)
and adds an ID3v1 tag; both stay as TagLib writes them.

## Commands

```sh
make run                          # debug build on http://127.0.0.1:8080
make test                         # unit + end-to-end tests
sudo deploy/install.sh            # on the server: install or update
journalctl -u nylm -f             # server logs
sudo -u nylm env NYLM_DATA=/mnt/data/nylm nylm set-password
sudo systemctl start nylm-music-scan   # scan the music folder
sudo systemctl start nylm-music-write  # write the pending tag changes
journalctl -u nylm-music-scan -u nylm-music-write   # their output
NYLM_DATA=dev-data ./nylm-debug set-password   # password for make run
```

Configuration is environment variables; `nylm --help` lists them.
Build needs `gcc`, `make` and the system libraries `sqlite` (3.38+), `cjson`,
`openssl` (3.2+) and `taglib` (2.0+), linked dynamically: `pacman -Syu`
brings their fixes.
