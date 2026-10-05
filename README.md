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
| `src/move.c`      | the naming rule, `nylm music-move`                       |
| `src/dupes.c`     | when two names are the same, for the duplicates          |
| `src/qobuz.c`     | Qobuz without the network: links, login, bundle, tags    |
| `src/qobuz_*.c`   | `nylm-qobuz`, the Qobuz service (its own binary)         |
| `src/musicbrainz*.c` | `nylm-musicbrainz`, album ids and genres from MusicBrainz |
| `src/https.c`     | HTTPS client (libssl), only in the two services above    |
| `src/tags.c`      | music file tags through TagLib: read, write, verify      |
| `src/art.c`       | album art files, named by SHA-256; base64                |
| `src/image.c`     | thumbnails (libjpeg-turbo, libpng), only in the services |
| `src/action.c`    | runs root actions through `sudo -n`, and their output    |
| `src/audit.c`     | dangerous actions: the password again, the audit log     |
| `src/sysinfo.c`   | the server app's parsers of /proc, /sys, actions' output |
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
$NYLM_DATA/plants/photos/     journal photos: each once, and its thumbnail
$NYLM_DATA/music/music.db     music tags cache, changes, scans, audit log
$NYLM_DATA/music/art/         album art: each picture once, and its thumbnail
$NYLM_DATA/music/*.lock       the library's lock, the Qobuz and MusicBrainz services
$NYLM_DATA/server/server.db   server app: audit log, WireGuard peer names
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
the server's local date (the system time zone). Due lists what is late,
today and this week; Later is grouped by plant, each opening on its care.
A plant's page has its care rules, details and, at the bottom, its journal.

A journal entry has up to 4 photos. The browser makes each one a JPEG of at
most 1600 pixels and 640 KiB, and a thumbnail of at most 320 pixels and 64
KiB, and sends both as base64 (`POST /api/plants/photos/add`); the server
checks only that they are base64 JPEGs and never decodes a picture. Each is
stored once, named by the SHA-256 of the photo, in `$NYLM_DATA/plants/photos/`
(`care_photos` table), and sent as it is by `GET /api/plants/photo?hash=H&
size=full|thumb`. Removing a photo or deleting its entry removes the files
no other entry has.

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
and scans, and starts the three services that do work on the files, each its
own process, started by hand or from the web app (password again,
recorded in the `audit` table) through a root action that only starts its
unit:

- `nylm music-scan [PATH]` (`nylm-music-scan.service`): with a path (the
  folder, or a folder or file in it) scans that; without, runs the scans
  the web app queued (the whole library, or an album's files to read them
  again), or the whole library if none is queued. A folder's new and
  changed files are read (size, or ctime after the last scan), a single
  file always; tracks no longer there leave the cache. A scan of the whole
  library also counts the other files by extension (`[blank]`: none), and
  removes the stored pictures no track has.
- `nylm music-write` (`nylm-music-write.service`): writes the pending
  changes, track by track, then reads each track back into the cache.
- `nylm music-move` (`nylm-music-move.service`): moves each track to where
  the naming rule puts it (below), never over an existing file, and puts
  its new path in the cache; each move is recorded (`moves` table). The
  web app starts it only when no change is pending.

They never run together: each holds `$NYLM_DATA/music/library.lock`
exclusively. The server holds it shared for its own writes to `music.db`,
and refuses to queue or discard anything while a service runs (409).

The web app has seven tabs. Albums: the last scan and a button to scan
again, filters (search; a field each, with a switch for albums where a track
has no value for it; and switches for albums with invalid tags, missing
tags, invalid genres, genres that differ between tracks, several artists
without being a compilation, tracks without art, art that differs between
tracks), and a table of
albums edited in place: the cover, album, album artist, date, composers,
genres, compilation, for every track at once. The tracks count opens the
album: buttons that search RateYourMusic, Wikipedia and MusicBrainz for it
(in a new tab), its pictures (each once: type, size, on how many tracks, full
size) with Set cover, then every track with every tag, edited in place, under an album line
that sets a tag for every track at once (not the tags each track has its
own: disc, track, title, BPM, ISRC, MusicBrainz track);
a value that differs from the rest of the album is ringed rose. Set
cover reads a picture file in the browser, scales it to at most 1200
pixels on a canvas and makes it a JPEG of at most 700 KiB, shows it, and
queues it.
Disc and track numbers are set in a popup, X first and Y worked out:
saving numbers the whole album again (Y per disc for track numbers; a
missing X gets the lowest free number). Every edit is queued at once as
one batch (`changes` table), and colours what will change. Changes: the
pending changes by album, then track (those of tracks a scan removed in
one group), a search over every field, Discard for an album, a track or
all, and Write; a track whose changes the write would refuse (another tag
breaks a rule, after the write's own fixes such as the disc number) is
marked ⚠, a button to its album. Duplicates (`GET /api/music/duplicates`, by the planned
tags; at most 500 rows a list): tracks with the same MusicBrainz track
id, ISRC, or artist and title; albums (the tracks of one album and album
artist in one folder) with the same MusicBrainz album id, barcode, album
and album artist, or album with other album artists; and the spellings of
an artist, album artist, composer or genre. Names are the same when their
keys are (`src/dupes.c`): lower case, without accents on Latin letters,
punctuation, symbols and spaces, `&` read as "and", a leading "The "
dropped. A track or album has Copy path (or folder) and Open album; nylm
never removes a file. A spelling has Use this one (`POST
/api/music/merge`), which queues, as one batch, the change of the other
spellings to it on every track. Fixes: changes for at most 10 albums at
a time (`POST /api/music/fix/...`), in the order of the albums list, by the
planned tags, queued as one batch like any edit; an album a fix would
leave breaking the rules is left alone and named (the first 50) with
Open album. Split genres (`fix/split-genres`, delimiter `,`, `;` or `:`):
a genre holding the delimiter becomes several, each part trimmed, spaces
made one, lowercase; other genres stay as they are. Composer from the
album artist (`fix/composers`): each track without a composer gets its
album artist; compilations are not changed, an album without an album
artist is left alone. Album ids and genres from MusicBrainz (below). Files: the naming
rule, the tracks
that move (from, to) and those that can not (why), Move files, and what
the moves did (`GET /api/music/moves`). Qobuz: connect, download albums,
and what came of each download. Info: library
counts, albums by genre (a donut of the 12 largest, the rest as "other",
and every genre in a table) and by year (`GET /api/music/charts`, from the
files' tags), the rules, and the written changes with their results, by
day, then album, then track, each opening on its own.

The rules for a value: required are title, album, artist, album artist,
track and disc number, date, composer and compilation (genre may be left
out); text is UTF-8 without control characters, at most 500 bytes; track
and disc number `X/Y`, positive whole numbers, X at most Y; date (the year)
and BPM positive whole numbers; compilation `0` or `1` (absent reads as
`0`); genres are words of lowercase `a-z`, `0-9`, `-`, `&` and `/` with one
space between them (`pop rock`, `r&b`).

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

The naming rule, from the cached tags: `ALBUMARTIST/ALBUM/NN - TITLE.ext`
below the music folder, or `D-NN - TITLE.ext` when the disc total is more
than 1 (NN: the track number, zero-padded to 2 digits or to the digits of
the track total; the extension in lower case). In each name
`/ \ : * ? " < > |` and control characters become `_`, spaces around and
dots at the end go, a leading dot becomes `_`, and it is cut to 255 bytes.
A track without album artist, album, title or an `X/Y` track number (or
with a disc number that is not `X/Y`) stays, and so do two tracks that
would get the same name. When all the tracks of a folder went to one
folder, the folder's other files go there too; emptied folders are
removed. Navidrome sees a moved file as a new one.

TagLib writes in place. It saves MP3 tags as ID3v2.4 (upgrading ID3v2.3)
and adds an ID3v1 tag; both stay as TagLib writes them.

Album art: the scan reads every picture of a track (at most 32) with its
type ("Front Cover", ...) and description, and stores each picture once,
named by the SHA-256 of its bytes, in `$NYLM_DATA/music/art/`
(`track_pictures` and `art` tables). JPEG and PNG pictures also get a
thumbnail of at most 256 pixels, made by the scan with libjpeg-turbo and
libpng; the server never decodes a picture. `GET /api/music/art?hash=H&size=full|thumb`
sends one (the only API answer that is not JSON): the type from the
`art` table, cached by the browser for a year.

A new album cover is a JPEG of at most 700 KiB, sent as base64 in JSON
(`POST /api/music/cover`; the browser scales it down first). The server
checks only that it is base64 and starts like a JPEG, stores it in the
art folder, and queues a `picture` change (its hash) for every track of
the album. A picture already stored can be used the same way by its hash
(`{album, hash}`, Use on all tracks under it) when it is a JPEG of at
most 700 KiB, as it is; any other the browser makes a JPEG and sends like
an upload. The write service checks that the stored bytes still have
that hash and decode completely (which makes the thumbnail), then
replaces all of the track's pictures with it as the front cover, and
checks it reads back.

Qobuz (`nylm-qobuz`, `nylm-qobuz.service`, root action `qobuz`): one of
the two parts of nylm that talk to the internet (with MusicBrainz, below),
each a binary of its own so that only they link libssl (`src/https.c`:
TLS 1.2+, certificates checked, 30 s timeouts; the server never links it). The web app queues what it should do (`POST
/api/music/qobuz/start`, password again, audited) and shows how it went
(`GET /api/music/qobuz`); each run:

1. reads the Qobuz web player's `bundle.js` for its app id, OAuth key and
   the secrets that sign download requests;
2. finishes a login: the user opens the login link (with that app id),
   logs in, and pastes the `http://localhost/?code=...` address Qobuz
   then opens; the service exchanges the code for a token, kept in
   `music.db` (`qobuz_account`) and never sent to the browser;
3. downloads each queued album (`qobuz_downloads`; links like
   `https://www.qobuz.com/us-en/album/name/ID`, at most 50 at a time): the
   best FLAC Qobuz has, track by track, into `.nylm-qobuz/` in the music
   folder (scans skip dot folders); tags each through TagLib from Qobuz's
   data (title and album with their version, artist, album artist, track
   `N/tracks on its disc`, disc `D/discs`, year, composer, copyright,
   label, ISRC, barcode, compilation 0; no genre: the user's own) with the
   album cover; then, holding the library lock, moves the tracks where the
   naming rule puts them (never over a file) and scans those folders.

It holds `$NYLM_DATA/music/qobuz.lock`, not the library's, while it
downloads, so the library can be edited meanwhile. The signing scheme is
Qobuz's own and may change; then the service says that no secret signs
downloads.

MusicBrainz (`nylm-musicbrainz ids|genres`, `nylm-musicbrainz@.service`,
root action `musicbrainz ids|genres`; Fixes tab): two jobs, each started
with the password again (`POST /api/music/musicbrainz/start {password,
what}`, audited), what they did in `GET /api/music/musicbrainz` (the
latest 100 searches and lookups). Both ask musicbrainz.org (one request a
second) about 10 albums a run and queue what they find as one batch, like
any edit, holding the library lock as the server does; they never touch
the files.

Album ids (`ids`): the next 10 albums, by their planned album and album
artist (both present), none of whose tracks has a valid planned
MusicBrainz album id (a release id, lowercase `8-4-4-4-12` hex; anything
else counts as none), in the order of the albums list. Each is searched
for (`release:"album" AND artist:"album artist"`, the first 25 releases;
again without a leading "The " in the artist when that finds nothing
sure). Only a release whose title and artist credit are the same names
once normalised (ASCII case and punctuation, typographic quotes and
dashes, spaces, a leading "the" aside) is taken, an Official one first;
its id is queued for the tracks still without a valid one. Recorded in
`musicbrainz_searches`: queued, unsure (releases found, none with these
names), not_found, failed, skipped (the album got an id meanwhile). An
album searched by the same names with queued, unsure or not_found is not
searched again; failed and skipped are.

Genres (`genres`): the next 10 albums without a planned genre, in the
order of the albums list, whose tracks without a genre all have one
planned MusicBrainz album id (one queued by `ids` counts). It asks for
the release, then its release group: the genres MusicBrainz's users voted
for the group, else for the release; the 5 with the most votes that fit
the genre rule (and are at most 100 bytes), then the release's language
as a genre (`spanish`, `instrumental` for no lyrics; left out when it has
several or one not in nylm's list, and when `instrumental` is among the
voted genres: MusicBrainz's language is that of the release's text). They are queued for the tracks that
still have no genre. Recorded in `musicbrainz_lookups`: queued, none,
not_found, failed, skipped (the album got a genre meanwhile). An album
whose id was queued, none or not_found is not asked again; failed and
skipped are.

The service holds `$NYLM_DATA/music/musicbrainz.lock` while it runs (one
job at a time), so the library can be edited meanwhile; it does not need
`NYLM_MUSIC`.

## Server

Maintenance of the machine nylm runs on (`src/api_server.c`, API under
`/api/server`). Reads never ask for anything; everything that changes the
machine asks for the password again and is recorded in the `audit` table
of `server.db` (shown as the latest actions). Status that needs no root
comes from `/proc`, `/sys` and `statvfs()` (parsed by `src/sysinfo.c`);
what needs root goes through a root action.

System: host name, kernel, uptime, load, memory and swap, temperatures
(`/sys/class/hwmon`), and "reboot needed" when the running kernel's
modules folder (`/usr/lib/modules/$(uname -r)`) is gone, i.e. a newer
kernel was installed. Disks: each filesystem on a disk (ext4, btrfs, xfs,
vfat, ...; each device once), its size and free space; network and pseudo
filesystems are left out (a network mount that is gone would hang the
server). Rose from 90 % used, peach from 80 %. Disk health: each disk's
SMART data (action `smart`: `smartctl -j -a -n standby` for every disk
`lsblk` lists, so a disk that is asleep is not woken): pass or fail,
temperature, hours on, and the signs of wear: reallocated, pending and
uncorrectable sectors (SATA), % used, spare and media errors (NVMe).

Long work is a job: a root script in `/usr/local/lib/nylm/jobs/` (from
`deploy/jobs/`) run by its own systemd unit, which a root action starts.
Every job holds an exclusive flock on that folder while it runs, so only
one runs at a time (the web app answers 409 meanwhile). The page shows the
unit's state (`systemctl show`, no root) and its last run's log (action
`unit-log`, which shows only nylm's own units, those in `NYLM_UNITS` and
the backup jobs). The actions and jobs read `/etc/nylm.conf` through
`deploy/lib.sh` (installed as `/usr/local/lib/nylm/lib.sh`), which checks
its values by the same rules as nylm; nylm refuses to start when a value
is invalid.

Services: the units in `NYLM_UNITS`, then nylm's own: state, since when
or how the last run ended (rose: failed, or a service that is stopped),
and Log (the unit's last run). A name without a unit type is a service,
as systemd reads it (`docker` is `docker.service`).

Disk usage (`nylm-disk-usage.service`): Measure (password) sizes nylm's
data, the music, the backups, `/var/lib/docker` and each backup entry
with `du -sxb`, one line each in the journal, which the page reads back.

Containers: every Docker container (action `docker-list`, which prints
only the fields shown, never a container's environment): state, health,
up since or exit code, restarts, published ports, CPU and memory; Log
(action `docker-logs NAME`: its last 500 lines) and Restart (password,
action `docker-restart NAME`: `docker restart`, not while a job runs).
A name is checked by Docker's rule in nylm and in the action, which also
checks that the container exists. nylm is never in the `docker` group.

Network: WireGuard (action `wg-show`: `wg show all dump` without the
private and preshared keys, which never leave the action): each
interface and its port, and each peer: where it last connected from (its
public address), its last handshake (connected while it is less than 3
minutes old), its VPN address and traffic, and a name you give it
(`wg_peers` in `server.db`, by public key; no password: a label). Open
ports: the listening TCP and unconnected UDP sockets of
`/proc/net/{tcp,udp}{,6}` (no root), each with what it is: nylm,
WireGuard, the container that publishes it, or a well-known service.

Updates: Check (password; `nylm-updates-check.service` runs
`checkupdates` as user nylm, which syncs a copy of the package databases,
never the system's own, so nothing is ever half-upgraded) lists each
package with an update, read back from the journal. Update now
(password; `nylm-update.service`, a root job) runs `pacman -Sy
archlinux-keyring` and then `pacman -Su`, unattended (`--noconfirm`), on
its own: closing the page or restarting nylm does not stop it, and it is
never stopped halfway. Read the Arch news first: an update that needs
steps by hand fails or needs them after. The page shows the last full
upgrade (from `/var/log/pacman.log`), the update's log, and whether a
reboot is needed. Reboot (password, action `reboot`) is refused while a
job runs; the page comes back when the server is up again.

Backups: by hand, one entry at a time: `nylm` (nylm's data) and each
entry of `NYLM_BACKUP` (an app: its folders). Back up now (password;
`nylm-backup@NAME.service`, a root job) stops the running containers that
use any of the entry's folders, archives them (`tar --numeric-owner`,
each path without its leading `/`, through `zstd -19 --long=27`), starts
the containers again (also when something failed), checks the archive
(`zstd -t`), writes its SHA-256 and only then gives it its name:
`$NYLM_BACKUP_DIR/NAME/NAME-YYYYMMDDTHHMMSSZ.tar.zst` (UTC) and `.sha256`.
Then the entry's backups beyond the newest `NYLM_BACKUP_KEEP` go. nylm's
databases are copied with `sqlite3 .backup` (as user nylm, safe while it
runs) and archived under their own names; lock files are left out.
Archives are owned by root, readable by `NYLM_BACKUP_GROUP` (mode 640),
else by root only. The backup folder is on the data drive: it protects
against mistakes, not against that drive failing; keep a copy on your PC.

Settings in `/etc/nylm.conf` (restart nylm after changing them):

```sh
NYLM_UNITS="wg-quick@wg0 docker sshd"   # more units to show
NYLM_BACKUP="davis=/var/lib/docker/volumes/davis_data/_data immich=/srv/immich,/srv/immich-db compose=/home/you/docker"
NYLM_BACKUP_DIR=/mnt/data/backups       # must exist; not inside NYLM_DATA or an entry
NYLM_BACKUP_KEEP=2                      # backups kept of each entry, 1 to 100
NYLM_BACKUP_GROUP=you                   # may read the backups (for the copy to your PC)
```

Units are named as systemd names them (A-Z a-z 0-9 @ . _ : -). A backup
entry is `name=/path[,/path...]`: a name of a-z 0-9 - (not `nylm`, which is
nylm's own data) and absolute paths of A-Z a-z 0-9 / . _ - only. Put an
app and its database in one entry: they are stopped and archived together.
Back up the folder with your compose file and its `.env` as an entry too.

### Copying the backups to your PC

The PC pulls them over SSH with a key that may only read the backup
folder (`rrsync -ro`): the server never needs access to the PC, and the
key can not change anything on the server. Once, on the PC:

```sh
ssh-keygen -t ed25519 -f ~/.ssh/nylm-backups -N '' -C nylm-backups
cat ~/.ssh/nylm-backups.pub            # copy this line
```

On the server, as the user in `NYLM_BACKUP_GROUP`, add a line to
`~/.ssh/authorized_keys` (one line; paste the key):

```
command="/usr/bin/rrsync -ro /mnt/data/backups",restrict ssh-ed25519 AAAA... nylm-backups
```

Then, on the PC, whenever you want the new backups (without `--delete`
the PC keeps every backup it ever got), and check them:

```sh
rsync -av -e 'ssh -i ~/.ssh/nylm-backups' you@server: ~/nylm-backups/
cd ~/nylm-backups/davis && sha256sum -c davis-*.sha256
```

(With `rrsync` the remote path is relative to the backup folder: `you@server:`
is all of it, `you@server:davis/` one entry.)

### Restoring a backup

By hand, one entry at a time. The archive holds each path without its
leading `/`, with numeric owners, so it unpacks into place from `/`:

```sh
cd /mnt/data/backups/davis
sha256sum -c davis-20261004T123015Z.tar.zst.sha256    # it is whole
tar --zstd -tvf davis-20261004T123015Z.tar.zst | less  # what it holds
docker stop davis                                     # what uses it (nylm: sudo systemctl stop nylm)
sudo mv /var/lib/docker/volumes/davis_data/_data /var/lib/docker/volumes/davis_data/_data.old
sudo tar --zstd --numeric-owner -xpf davis-20261004T123015Z.tar.zst -C /
docker start davis                                    # check it works, then remove _data.old
```

From the PC: copy the archive back first (`rsync -av davis-....tar.zst
you@server:/tmp/` with a normal key), or unpack it on the PC with `-C`
into another folder to take single files out.

## Commands

On the server (the deployed app, run as user `nylm` by systemd):

```sh
sudo deploy/install.sh                 # install or update (after git pull)
sudo -u nylm env NYLM_DATA=/mnt/data/nylm nylm set-password
journalctl -u nylm -f                  # server logs
sudo systemctl start nylm-music-scan   # scan the music folder (or what is queued)
sudo systemctl start nylm-music-write  # write the pending tag changes
sudo systemctl start nylm-music-move   # move the files where their tags put them
sudo systemctl start nylm-qobuz        # Qobuz: what the web app queued
sudo systemctl start nylm-musicbrainz@ids     # MusicBrainz album ids, 10 albums
sudo systemctl start nylm-musicbrainz@genres  # genres from MusicBrainz, 10 albums
sudo -u nylm env NYLM_DATA=/mnt/data/nylm NYLM_MUSIC=/mnt/data/music \
    nylm music-scan /mnt/data/music/Some/Album   # scan one folder or file
journalctl -u nylm-music-scan -u nylm-music-write -u nylm-music-move -u nylm-qobuz \
    -u 'nylm-musicbrainz@*'
sudo systemctl start nylm-disk-usage    # measure the folders (see the System tab)
journalctl -u nylm-disk-usage
sudo systemctl start nylm-updates-check # list the package updates
sudo systemctl start nylm-update       # update every package (what Update now does)
journalctl -u nylm-updates-check -u nylm-update
sudo systemctl start nylm-backup@davis  # back up an entry (nylm: nylm's data)
journalctl -u nylm-backup@davis
```

In a checkout (a debug build with sanitizers, data in `./dev-data`):

```sh
make test                              # unit + end-to-end tests
NYLM_DATA=dev-data ./nylm-debug set-password   # once, before make run
make run                               # server on http://127.0.0.1:8080
# The services by hand (the web app's buttons need the deployed root
# actions); NYLM_MUSIC is the music folder to use:
NYLM_DATA=dev-data NYLM_MUSIC=/path/to/music ./nylm-debug music-scan
NYLM_DATA=dev-data NYLM_MUSIC=/path/to/music ./nylm-debug music-write
NYLM_DATA=dev-data NYLM_MUSIC=/path/to/music ./nylm-debug music-move
NYLM_DATA=dev-data NYLM_MUSIC=/path/to/music ./nylm-qobuz-debug
NYLM_DATA=dev-data ./nylm-musicbrainz-debug ids      # or genres
```

Configuration is environment variables; `nylm --help` lists them.
Build needs `gcc`, `make` and the system libraries `sqlite` (3.44+), `cjson`,
`openssl` (3.2+; libssl only for `nylm-qobuz` and `nylm-musicbrainz`),
`taglib` (2.0+),
`libjpeg-turbo` and `libpng`, linked dynamically: `pacman -Syu` brings their
fixes. `make` builds `nylm`, `nylm-qobuz` and `nylm-musicbrainz`. The server app's root
actions also use `smartmontools`, `wireguard-tools`, `pacman-contrib`,
`fakeroot`, `zstd`, and `rsync` for the copy to your PC (`install.sh`
installs them).
