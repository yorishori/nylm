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

Tag editor for the music folder `NYLM_MUSIC` (`src/api_music.c`, API under
`/api/music`). The files are the truth: `music.db` caches their tags. A
track is any file below the folder with a music extension (mp3, mp2,
flac, ogg, oga, opus, m4a, m4b, m4p, mp4, aac, wav, aif, aiff, aifc, afc,
ape, wv, tta, mpc, spx, wma, asf, shn, mka, dsf, dff, dsdiff; any case).
An album is the tracks that share ALBUM and ALBUMARTIST. Dot files are
skipped and symlinks never followed. Only TagLib opens the files, and only
these tags are read or written: TITLE ALBUM ARTIST ALBUMARTIST TRACKNUMBER
DISCNUMBER DATE GENRE COMPOSER COMPILATION ISRC ASIN BPM COPYRIGHT
ENCODEDBY MOOD MEDIA LABEL CATALOGNUMBER BARCODE TITLESORT ALBUMSORT
ARTISTSORT ALBUMARTISTSORT COMPOSERSORT MUSICBRAINZ_TRACKID
MUSICBRAINZ_ALBUMID NAVIDROME_ID. Genre and composer keep several values,
in order; another tag with several values shows them joined by `"; "` and
is written back as one.

The server never opens a music file. It reads the cache, queues changes
and scans, and starts the two services that do work on the files, each its
own process, started by hand or from the web app (password again,
recorded in the `audit` table) through a root action that only starts its
unit:

- `nylm music-scan [PATH]` (`nylm-music-scan.service`): with a path (the
  folder, or a folder or file in it) scans that; without, runs the scans
  the web app queued (the whole library, or an album's files to read them
  again), or the whole library if none is queued. A folder's new and
  changed files are read (size, or ctime after the last scan), a single
  file always; tracks no longer there leave the cache. A scan of the whole
  library also counts the other files by extension (`[blank]`: none).
- `nylm music-write` (`nylm-music-write.service`): writes the pending
  changes, track by track, then reads each track back into the cache.

They never run together: each holds `$NYLM_DATA/music/library.lock`
exclusively. The server holds it shared for its own writes to `music.db`,
and refuses to queue or discard anything while a service runs (409).

The web app has three tabs. Albums: the last scan and a button to scan
again, filters (search, a field each, and switches for albums with
invalid tags, missing tags, several artists without being a compilation,
tracks without art), and a table of albums edited in place: album, album
artist, date, composers, genres, compilation, for every track at once.
The tracks count opens the album: every track with every tag, edited in
place; a value that differs from the rest of the album is ringed rose.
Disc and track numbers are set in a popup, X first and Y worked out:
saving numbers the whole album again (Y per disc for track numbers; a
missing X gets the lowest free number). Every edit is queued at once as
one batch (`changes` table), and colours what will change. Changes: the
pending changes by batch, Discard per batch, and Write. Info: library
counts, the rules, and the written changes with their results.

The rules for a value: required are title, album, artist, album artist,
track and disc number, date, genre, composer and compilation; text is
UTF-8 without control characters, at most 500 bytes; track and disc
number `X/Y`, positive whole numbers, X at most Y; date (the year) and BPM
positive whole numbers; compilation `0` or `1` (absent reads as `0`);
genres lowercase `a-z`, `0-9` and `-`.

The write service, per track, only through TagLib (`src/tags.c`): applies
the track's pending changes to the cached tags; mirrors TITLESORT,
ALBUMSORT, ARTISTSORT, ALBUMARTISTSORT and COMPOSERSORT (the composers
joined by `"; "`) from the tags that change, cutting them to 500 bytes;
sets a missing or invalid disc number to `1/1`; checks every tag against
the rules; checks that the file still has the cached tags; writes what
differs and reads the file back. Each change ends `done`, `warning` (done,
and what nylm also changed, e.g. the disc number) or `failed` (with the
cause: an invalid tag, a file changed since the scan, a file that does not
read back as written).

TagLib writes in place. It saves MP3 tags as ID3v2.4 (upgrading ID3v2.3)
and adds an ID3v1 tag; both stay as TagLib writes them.

Album art, later: the scan records whether a track has a picture
(`has_art`). Uploading art will be a pending change like the others (the
picture saved under `$NYLM_DATA/music/`, the write service setting it
through TagLib's PICTURE property), and showing it a read of the cache. A
picture can be bigger than the 1 MiB request body limit allows today.

## Commands

```sh
make run                          # debug build on http://127.0.0.1:8080
make test                         # unit + end-to-end tests
sudo deploy/install.sh            # on the server: install or update
journalctl -u nylm -f             # server logs
sudo -u nylm env NYLM_DATA=/mnt/data/nylm nylm set-password
sudo systemctl start nylm-music-scan   # scan the music folder (or what is queued)
sudo systemctl start nylm-music-write  # write the pending tag changes
sudo -u nylm env NYLM_DATA=/mnt/data/nylm NYLM_MUSIC=/mnt/data/music \
    nylm music-scan /mnt/data/music/Some/Album   # scan one folder or file
journalctl -u nylm-music-scan -u nylm-music-write   # their output
NYLM_DATA=dev-data ./nylm-debug set-password   # password for make run
```

Configuration is environment variables; `nylm --help` lists them.
Build needs `gcc`, `make` and the system libraries `sqlite` (3.44+), `cjson`,
`openssl` (3.2+) and `taglib` (2.0+), linked dynamically: `pacman -Syu`
brings their fixes.
