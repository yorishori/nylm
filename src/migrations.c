/*
 * The database schemas: one list of migrations per app, each app with its
 * own database (see src/db.h). db_open() applies every migration past the
 * database's PRAGMA user_version, in order, each in its own transaction.
 *
 * To change an app's schema, append a new entry to its list. Never edit or
 * remove an entry that has been installed anywhere: databases already ran it.
 */

#include "db.h"

const char *const core_migrations[] = {
    /* 1: the single user's password (Argon2id) and active login sessions.
     * Only the SHA-256 of each session token is stored, never the token. */
    "CREATE TABLE user ("
    "    id         INTEGER PRIMARY KEY CHECK (id = 1),"
    "    salt       BLOB    NOT NULL,"
    "    hash       BLOB    NOT NULL,"
    "    memcost    INTEGER NOT NULL," /* KiB */
    "    iterations INTEGER NOT NULL,"
    "    updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ") STRICT;"
    "CREATE TABLE sessions ("
    "    token_hash BLOB    PRIMARY KEY,"
    "    expires_at INTEGER NOT NULL"
    ") STRICT, WITHOUT ROWID;",
};

const int core_migration_count = sizeof core_migrations / sizeof core_migrations[0];

const char *const plants_migrations[] = {
    /* 1: plant care. Plants and care types are archived, never deleted.
     * A rule says how often a care type is due for a plant; the log records
     * what was done (care_type_id NULL: a plain note). Due dates are not
     * stored: they are computed from the rule and the latest log entry.
     * Dates are "YYYY-MM-DD". Intervals: NULL means paused. */
    "CREATE TABLE plants ("
    "    id       INTEGER PRIMARY KEY,"
    "    name     TEXT    NOT NULL,"
    "    species  TEXT    NOT NULL,"
    "    location TEXT    NOT NULL,"
    "    acquired TEXT,"
    "    notes    TEXT    NOT NULL,"
    "    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0, 1))"
    ") STRICT;"
    "CREATE TABLE care_types ("
    "    id       INTEGER PRIMARY KEY,"
    "    name     TEXT    NOT NULL UNIQUE COLLATE NOCASE,"
    "    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0, 1))"
    ") STRICT;"
    "CREATE TABLE care_rules ("
    "    plant_id      INTEGER NOT NULL REFERENCES plants(id),"
    "    care_type_id  INTEGER NOT NULL REFERENCES care_types(id),"
    "    interval_days INTEGER CHECK (interval_days BETWEEN 1 AND 3650),"
    "    yearly_month  INTEGER CHECK (yearly_month BETWEEN 1 AND 12),"
    "    yearly_day    INTEGER CHECK (yearly_day BETWEEN 1 AND 31),"
    "    created       TEXT    NOT NULL," /* first due date counts from here */
    "    PRIMARY KEY (plant_id, care_type_id),"
    "    CHECK ((yearly_month IS NULL) = (yearly_day IS NULL)),"
    "    CHECK (yearly_month IS NULL OR interval_days IS NULL)"
    ") STRICT, WITHOUT ROWID;"
    /* Seasonal exceptions to a rule's interval; they never overlap. */
    "CREATE TABLE care_periods ("
    "    plant_id      INTEGER NOT NULL,"
    "    care_type_id  INTEGER NOT NULL,"
    "    start_month   INTEGER NOT NULL CHECK (start_month BETWEEN 1 AND 12),"
    "    start_day     INTEGER NOT NULL CHECK (start_day BETWEEN 1 AND 31),"
    "    end_month     INTEGER NOT NULL CHECK (end_month BETWEEN 1 AND 12),"
    "    end_day       INTEGER NOT NULL CHECK (end_day BETWEEN 1 AND 31),"
    "    interval_days INTEGER CHECK (interval_days BETWEEN 1 AND 3650),"
    "    FOREIGN KEY (plant_id, care_type_id) REFERENCES care_rules ON DELETE CASCADE"
    ") STRICT;"
    "CREATE INDEX care_periods_by_rule ON care_periods (plant_id, care_type_id);"
    "CREATE TABLE care_log ("
    "    id           INTEGER PRIMARY KEY,"
    "    plant_id     INTEGER NOT NULL REFERENCES plants(id),"
    "    care_type_id INTEGER REFERENCES care_types(id),"
    "    date         TEXT    NOT NULL,"
    "    note         TEXT    NOT NULL"
    ") STRICT;"
    "CREATE INDEX care_log_by_plant ON care_log (plant_id, date, id);"
    "CREATE INDEX care_log_by_rule ON care_log (plant_id, care_type_id, date);",

    /* 2: a colour per plant and care type, from a fixed palette, so the
     * frontend can show at a glance which plant and which care. */
    "ALTER TABLE plants ADD COLUMN color TEXT NOT NULL DEFAULT 'mint'"
    "    CHECK (color IN ('butter', 'lime', 'mint', 'teal', 'sky', 'periwinkle',"
    "                     'lavender', 'orchid'));"
    "ALTER TABLE care_types ADD COLUMN color TEXT NOT NULL DEFAULT 'sky'"
    "    CHECK (color IN ('butter', 'lime', 'mint', 'teal', 'sky', 'periwinkle',"
    "                     'lavender', 'orchid'));",

    /* 3: up to 4 photos per log entry, in order. A photo is a JPEG (and
     * its thumbnail) in <NYLM_DATA>/plants/photos/, named by its SHA-256
     * (src/art.h, ART_PLANTS); a file no row names is removed. */
    "CREATE TABLE care_photos ("
    "    log_id   INTEGER NOT NULL REFERENCES care_log(id) ON DELETE CASCADE,"
    "    hash     TEXT    NOT NULL CHECK (length(hash) = 64 AND hash NOT GLOB '*[^0-9a-f]*'),"
    "    position INTEGER NOT NULL,"
    "    PRIMARY KEY (log_id, hash)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE INDEX care_photos_by_hash ON care_photos (hash);",
};

const int plants_migration_count = sizeof plants_migrations / sizeof plants_migrations[0];

const char *const music_migrations[] = {
    /* 1: the music library (written before the app was first installed:
     * the earlier drafts were replaced, not migrated).
     *
     * tracks and track_values are a cache of the files, filled by `nylm
     * music-scan` and refreshed after every write; deleting them loses
     * nothing. path is the file's full path. A tag column holds the file's
     * value (NULL: absent; several values joined by "; "); the column
     * order is that of src/tags.h. Genre and composer can hold several
     * values: they are rows of track_values, in order. scanned is when a
     * scan last saw the file (unix time); a file whose ctime is older is
     * not read again.
     *
     * scans are requests to scan (path NULL: the whole library, else a
     * folder or a file) and what came of them; scan_extensions counts the
     * files a scan found that are not music, by extension.
     *
     * changes are the tag changes queued by the web app and written by
     * `nylm music-write`: one row per tag of a track, the changes saved
     * together sharing a batch. value is the new value ("" removes the
     * tag; for genre and composer a JSON array of the values). A track has
     * at most one pending change per tag. Written rows stay as history.
     *
     * audit records every start of a service from the web app. */
    "CREATE TABLE tracks ("
    "    id                  INTEGER PRIMARY KEY,"
    "    path                TEXT    NOT NULL UNIQUE,"
    "    size                INTEGER NOT NULL,"
    "    ext                 TEXT    NOT NULL,"
    "    scanned             INTEGER NOT NULL,"
    "    has_art             INTEGER NOT NULL CHECK (has_art IN (0, 1)),"
    "    title               TEXT,"
    "    album               TEXT,"
    "    artist              TEXT,"
    "    albumartist         TEXT,"
    "    tracknumber         TEXT,"
    "    discnumber          TEXT,"
    "    date                TEXT,"
    "    compilation         TEXT    NOT NULL DEFAULT '0',"
    "    isrc                TEXT,"
    "    asin                TEXT,"
    "    bpm                 TEXT,"
    "    copyright           TEXT,"
    "    encodedby           TEXT,"
    "    mood                TEXT,"
    "    media               TEXT,"
    "    label               TEXT,"
    "    catalognumber       TEXT,"
    "    barcode             TEXT,"
    "    titlesort           TEXT,"
    "    albumsort           TEXT,"
    "    artistsort          TEXT,"
    "    albumartistsort     TEXT,"
    "    composersort        TEXT,"
    "    musicbrainz_trackid TEXT,"
    "    musicbrainz_albumid TEXT,"
    "    navidrome_id        TEXT"
    ") STRICT;"
    "CREATE INDEX tracks_by_album ON tracks (album, albumartist);"
    "CREATE TABLE track_values ("
    "    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
    "    field    TEXT    NOT NULL CHECK (field IN ('genre', 'composer')),"
    "    position INTEGER NOT NULL,"
    "    value    TEXT    NOT NULL,"
    "    PRIMARY KEY (track_id, field, position)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE TABLE scans ("
    "    id        INTEGER PRIMARY KEY,"
    "    path      TEXT,"
    "    requested INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    started   INTEGER,"
    "    finished  INTEGER,"
    "    state     TEXT    NOT NULL DEFAULT 'queued' CHECK (state IN"
    "                      ('queued', 'running', 'done', 'incomplete', 'failed')),"
    "    files     INTEGER NOT NULL DEFAULT 0," /* music files found */
    "    parsed    INTEGER NOT NULL DEFAULT 0," /* new or changed: tags read */
    "    removed   INTEGER NOT NULL DEFAULT 0," /* tracks gone from the cache */
    "    failed    INTEGER NOT NULL DEFAULT 0"  /* unreadable files and folders */
    ") STRICT;"
    "CREATE TABLE scan_extensions ("
    "    scan_id INTEGER NOT NULL REFERENCES scans(id) ON DELETE CASCADE,"
    "    ext     TEXT    NOT NULL," /* lower case; "[blank]": none */
    "    count   INTEGER NOT NULL,"
    "    PRIMARY KEY (scan_id, ext)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE TABLE changes ("
    "    id       INTEGER PRIMARY KEY,"
    "    batch    INTEGER NOT NULL,"
    "    track_id INTEGER REFERENCES tracks(id) ON DELETE SET NULL,"
    "    field    TEXT    NOT NULL CHECK (field IN ('title', 'album', 'artist', 'albumartist',"
    "                     'tracknumber', 'discnumber', 'date', 'compilation', 'isrc', 'asin',"
    "                     'bpm', 'copyright', 'encodedby', 'mood', 'media', 'label',"
    "                     'catalognumber', 'barcode', 'musicbrainz_trackid',"
    "                     'musicbrainz_albumid', 'genre', 'composer')),"
    "    value    TEXT    NOT NULL,"
    "    started  INTEGER,"
    "    finished INTEGER,"
    "    state    TEXT    NOT NULL DEFAULT 'pending' CHECK (state IN"
    "                     ('pending', 'running', 'done', 'warning', 'failed')),"
    "    done     INTEGER NOT NULL DEFAULT 0 CHECK (done IN (0, 1))," /* finished */
    "    note     TEXT    NOT NULL DEFAULT ''"
    ") STRICT;"
    "CREATE UNIQUE INDEX changes_one_pending ON changes (track_id, field)"
    "    WHERE state = 'pending';"
    "CREATE INDEX changes_by_state ON changes (state, batch);"
    "CREATE TABLE audit ("
    "    id     INTEGER PRIMARY KEY,"
    "    at     INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    client TEXT    NOT NULL," /* IP address */
    "    action TEXT    NOT NULL,"
    "    detail TEXT    NOT NULL," /* JSON */
    "    result TEXT    NOT NULL"
    ") STRICT;",

    /* 2: album art. Every picture in a track is a row of track_pictures,
     * in the file's order; type and description as the file has them ("" if
     * none). Its bytes are stored once, in the art folder under their
     * SHA-256 (src/art.h), and listed in art: mime from the first bytes
     * (NULL: not a picture type nylm knows), size in bytes, width and
     * height (NULL until decoded), thumb 1 if a thumbnail was made. The
     * scan of the whole library removes art no track uses. tracks loses
     * has_art, and every track is read again by the next scan (scanned 0)
     * to find its pictures. */
    "ALTER TABLE tracks DROP COLUMN has_art;"
    "UPDATE tracks SET scanned = 0;"
    "CREATE TABLE art ("
    "    hash   TEXT    PRIMARY KEY CHECK (length(hash) = 64 AND hash NOT GLOB '*[^0-9a-f]*'),"
    "    mime   TEXT    CHECK (mime IN ('image/jpeg', 'image/png', 'image/gif', 'image/webp')),"
    "    size   INTEGER NOT NULL,"
    "    width  INTEGER,"
    "    height INTEGER,"
    "    thumb  INTEGER NOT NULL DEFAULT 0 CHECK (thumb IN (0, 1))"
    ") STRICT, WITHOUT ROWID;"
    "CREATE TABLE track_pictures ("
    "    track_id    INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
    "    position    INTEGER NOT NULL,"
    "    hash        TEXT    NOT NULL REFERENCES art(hash),"
    "    type        TEXT    NOT NULL,"
    "    description TEXT    NOT NULL,"
    "    PRIMARY KEY (track_id, position)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE INDEX track_pictures_by_hash ON track_pictures (hash);",

    /* 3: a change may set the album cover: field 'picture', value the
     * hash of an uploaded picture in art; written, it replaces all of the
     * track's pictures. SQLite can not change a CHECK: the table is made
     * again with the same rows. */
    "CREATE TABLE changes_new ("
    "    id       INTEGER PRIMARY KEY,"
    "    batch    INTEGER NOT NULL,"
    "    track_id INTEGER REFERENCES tracks(id) ON DELETE SET NULL,"
    "    field    TEXT    NOT NULL CHECK (field IN ('title', 'album', 'artist', 'albumartist',"
    "                     'tracknumber', 'discnumber', 'date', 'compilation', 'isrc', 'asin',"
    "                     'bpm', 'copyright', 'encodedby', 'mood', 'media', 'label',"
    "                     'catalognumber', 'barcode', 'musicbrainz_trackid',"
    "                     'musicbrainz_albumid', 'genre', 'composer', 'picture')),"
    "    value    TEXT    NOT NULL,"
    "    started  INTEGER,"
    "    finished INTEGER,"
    "    state    TEXT    NOT NULL DEFAULT 'pending' CHECK (state IN"
    "                     ('pending', 'running', 'done', 'warning', 'failed')),"
    "    done     INTEGER NOT NULL DEFAULT 0 CHECK (done IN (0, 1)),"
    "    note     TEXT    NOT NULL DEFAULT ''"
    ") STRICT;"
    "INSERT INTO changes_new (id, batch, track_id, field, value, started, finished, state,"
    "    done, note)"
    "  SELECT id, batch, track_id, field, value, started, finished, state, done, note"
    "  FROM changes;"
    "DROP TABLE changes;"
    "ALTER TABLE changes_new RENAME TO changes;"
    "CREATE UNIQUE INDEX changes_one_pending ON changes (track_id, field)"
    "    WHERE state = 'pending';"
    "CREATE INDEX changes_by_state ON changes (state, batch);",

    /* 4: what the move service did with each file: a track (track_id) or
     * another file of its folder moved (done), not moved (failed), or left
     * where it was (kept; to_path NULL). */
    "CREATE TABLE moves ("
    "    id        INTEGER PRIMARY KEY,"
    "    track_id  INTEGER REFERENCES tracks(id) ON DELETE SET NULL,"
    "    from_path TEXT    NOT NULL,"
    "    to_path   TEXT,"
    "    state     TEXT    NOT NULL CHECK (state IN ('done', 'failed', 'kept')),"
    "    note      TEXT    NOT NULL DEFAULT '',"
    "    finished  INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    CHECK ((state = 'done') = (to_path IS NOT NULL))"
    ") STRICT;",

    /* 5: Qobuz (nylm-qobuz). One account row: the web player's app id,
     * the login (token: a secret, never sent to the browser), a pasted
     * redirect waiting for the service, and the last error. Then the
     * albums to download, each with what came of it. */
    "CREATE TABLE qobuz_account ("
    "    id      INTEGER PRIMARY KEY CHECK (id = 1),"
    "    app_id  TEXT,"
    "    user_id TEXT,"
    "    token   TEXT,"
    "    label   TEXT,"
    "    login   TEXT,"
    "    error   TEXT    NOT NULL DEFAULT '',"
    "    updated INTEGER NOT NULL DEFAULT (unixepoch())"
    ") STRICT;"
    "INSERT INTO qobuz_account (id) VALUES (1);"
    "CREATE TABLE qobuz_downloads ("
    "    id        INTEGER PRIMARY KEY,"
    "    album_id  TEXT    NOT NULL,"
    "    title     TEXT,"
    "    artist    TEXT,"
    "    requested INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    started   INTEGER,"
    "    finished  INTEGER,"
    "    state     TEXT    NOT NULL DEFAULT 'queued' CHECK (state IN"
    "                      ('queued', 'running', 'done', 'warning', 'failed')),"
    "    tracks    INTEGER NOT NULL DEFAULT 0," /* of the album */
    "    saved     INTEGER NOT NULL DEFAULT 0," /* now in the library */
    "    note      TEXT    NOT NULL DEFAULT ''"
    ") STRICT;",

    /* 6: genres looked up on MusicBrainz for albums without one (by the
     * album's MusicBrainz album id), a row per album looked up: what was
     * found and queued, so that an album is looked up once (a failed
     * lookup is tried again). track: one of the album's tracks then. */
    "CREATE TABLE musicbrainz_lookups ("
    "    id          INTEGER PRIMARY KEY,"
    "    mbid        TEXT    NOT NULL CHECK (length(mbid) = 36),"
    "    track       INTEGER NOT NULL,"
    "    album       TEXT,"
    "    albumartist TEXT,"
    "    looked      INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    state       TEXT    NOT NULL CHECK (state IN"
    "                        ('queued', 'none', 'not_found', 'failed', 'skipped')),"
    "    genres      TEXT    NOT NULL DEFAULT '[]'," /* a JSON array */
    "    note        TEXT    NOT NULL DEFAULT ''"
    ") STRICT;"
    "CREATE INDEX musicbrainz_lookups_by_mbid ON musicbrainz_lookups (mbid);",

    /* 7: MusicBrainz album ids searched for by album and album artist, a
     * row per album searched: the id found and queued, so that an album
     * is searched once by these names (a failed search is tried again).
     * track: one of the album's tracks then. */
    "CREATE TABLE musicbrainz_searches ("
    "    id          INTEGER PRIMARY KEY,"
    "    track       INTEGER NOT NULL,"
    "    album       TEXT    NOT NULL,"
    "    albumartist TEXT    NOT NULL,"
    "    looked      INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    state       TEXT    NOT NULL CHECK (state IN"
    "                        ('queued', 'unsure', 'not_found', 'failed', 'skipped')),"
    "    mbid        TEXT    CHECK (mbid IS NULL OR length(mbid) = 36),"
    "    note        TEXT    NOT NULL DEFAULT ''"
    ") STRICT;"
    "CREATE INDEX musicbrainz_searches_by_names ON musicbrainz_searches (album, albumartist);",
};

const int music_migration_count = sizeof music_migrations / sizeof music_migrations[0];

const char *const server_migrations[] = {
    /* 1: every root job, restart and reboot started from the web app (the
     * password was given again): who, what, when, and how it went. */
    "CREATE TABLE audit ("
    "    id     INTEGER PRIMARY KEY,"
    "    at     INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    client TEXT    NOT NULL," /* IP address */
    "    action TEXT    NOT NULL,"
    "    detail TEXT    NOT NULL," /* what it acted on, "" if nothing */
    "    result TEXT    NOT NULL"
    ") STRICT;",

    /* 2: the names given to WireGuard peers, by their public key (WireGuard
     * itself only knows keys). */
    "CREATE TABLE wg_peers ("
    "    public_key TEXT PRIMARY KEY CHECK (length(public_key) = 44),"
    "    name       TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 100)"
    ") STRICT, WITHOUT ROWID;",
};

const int server_migration_count = sizeof server_migrations / sizeof server_migrations[0];
