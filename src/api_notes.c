/*
 * Notes: quick reminders, notes (a little Markdown, no title) and the tags
 * that sort them.
 *
 * Every device keeps its own copy and works on it, online or not, then
 * syncs with one call, POST /api/notes/sync: it sends what changed on the
 * device and gets back every row that changed since the last sync. Rows
 * carry a rev, the value of one counter (table sync) when they last
 * changed. What happens when both sides changed the same row:
 *   - a reminder is only added or removed: both are kept, removed wins;
 *   - a tag: the device's change is the latest, it wins; if its name is
 *     another tag's, it gets a number ("work 2");
 *   - a note: its change says which rev it started from (base). If the
 *     note moved on since, nothing is lost: the server's note stays, and
 *     the device's text comes back as a new note next to it (a device's
 *     removal of a note that changed since is dropped).
 * A change sent again (a sync whose reply was lost) changes nothing.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "arena.h"
#include "care.h"
#include "db.h"
#include "json.h"
#include "notes.h"

#define REV_MAX        9007199254740991L /* 2^53 - 1: exact in a JSON number */
#define REMINDER_MAX   200   /* bytes in a reminder */
#define NOTE_MAX       20000 /* bytes in a note */
#define NOTE_TAGS_MAX  20    /* tags on one note */
#define CHANGES_MAX    500   /* changes in one sync */
#define PULL_PAGE      100   /* rows sent back by one sync, at most */

enum kind { REMINDER, NOTE, TAG };

/* One change from a device, validated. */
struct change {
    enum kind kind;
    const char *id;
    int deleted;
    long base;                /* note: the rev it started from, 0 if new */
    const char *text;         /* reminder, note */
    const char *tags;         /* note: JSON array of tag ids, as stored */
    long created, updated;    /* reminder (created), note: unix seconds */
    const char *name, *color; /* tag */
};

/* ---- database helpers --------------------------------------------------- */

/*
 * Prepares sql on notes_db and binds one argument per character of types:
 * 'i' long, 't' text (must outlive the statement). NULL on error (logged).
 */
static sqlite3_stmt *prepare(const char *sql, const char *types, ...)
{
    sqlite3_stmt *st = db_prepare(notes_db, sql);
    if (st == NULL)
        return NULL;
    va_list ap;
    va_start(ap, types);
    int rc = SQLITE_OK;
    for (int i = 0; types[i] != '\0' && rc == SQLITE_OK; i++)
        rc = types[i] == 't'
                 ? sqlite3_bind_text(st, i + 1, va_arg(ap, const char *), -1, SQLITE_STATIC)
                 : sqlite3_bind_int64(st, i + 1, va_arg(ap, long));
    va_end(ap);
    if (rc != SQLITE_OK) {
        db_log_error(notes_db, sql);
        sqlite3_finalize(st);
        return NULL;
    }
    return st;
}

/* Steps st once and finalizes it. The step's result (SQLITE_ROW / DONE),
 * or SQLITE_ERROR (logged); a NULL st is an error already logged. */
static int step_once(sqlite3_stmt *st)
{
    if (st == NULL)
        return SQLITE_ERROR;
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(notes_db, sqlite3_sql(st));
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? rc : SQLITE_ERROR;
}

/* The next value of the sync counter, or -1 (logged). */
static long next_rev(void)
{
    sqlite3_stmt *st = prepare("UPDATE sync SET rev = rev + 1 RETURNING rev", "");
    if (st == NULL)
        return -1;
    long rev = sqlite3_step(st) == SQLITE_ROW ? (long)sqlite3_column_int64(st, 0) : -1;
    if (rev < 0)
        db_log_error(notes_db, "next_rev");
    sqlite3_finalize(st);
    return rev;
}

/* For a write that may change nothing: it sets the row's rev to REV_NEXT,
 * then took_rev() moves the counter on if a row did change. */
#define REV_NEXT "(SELECT rev + 1 FROM sync)"

/* 0 or -1 (logged). */
static int took_rev(void)
{
    if (sqlite3_changes(notes_db) == 0)
        return 0;
    return step_once(prepare("UPDATE sync SET rev = rev + 1", "")) == SQLITE_DONE ? 0 : -1;
}

/* ---- validating the changes --------------------------------------------- */

/* "changes[i]: err" in the arena (err itself if out of memory). */
static const char *at(int i, const char *err)
{
    char *msg = arena_alloc(160);
    if (msg == NULL)
        return err;
    snprintf(msg, 160, "changes[%d]: %s", i, err);
    return msg;
}

/* A time in unix seconds; one in the future (a device's clock ahead) is now. */
static const char *get_time(const cJSON *obj, const char *key, long *out)
{
    const char *err = json_get_int(obj, key, 1, REV_MAX, out);
    long now = (long)time(NULL);
    if (err == NULL && *out > now)
        *out = now;
    return err;
}

/* A note's "tags": distinct tag ids, at most NOTE_TAGS_MAX, as stored. */
static const char *get_tags(const cJSON *obj, const char **out)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(obj, "tags");
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > NOTE_TAGS_MAX)
        return "'tags' must be an array of at most 20 tag ids";
    const cJSON *item, *other;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || !notes_id_valid(item->valuestring))
            return "each tag id must be 32 lowercase hex characters";
        for (other = array->child; other != item; other = other->next)
            if (strcmp(other->valuestring, item->valuestring) == 0)
                return "'tags' has the same tag twice";
    }
    *out = cJSON_PrintUnformatted(array);
    return *out != NULL ? NULL : "out of memory";
}

/* Reads one change; NULL or the error. */
static const char *get_change(const cJSON *obj, struct change *c)
{
    const char *kind, *err;
    memset(c, 0, sizeof *c);
    if (!cJSON_IsObject(obj))
        return "each change must be an object";
    if (json_get_string(obj, "kind", 1, 16, &kind) != NULL ||
        (strcmp(kind, "reminder") != 0 && strcmp(kind, "note") != 0 &&
         strcmp(kind, "tag") != 0))
        return "'kind' must be reminder, note or tag";
    c->kind = kind[0] == 'r' ? REMINDER : kind[0] == 'n' ? NOTE : TAG;
    if (json_get_string(obj, "id", NOTES_ID_LEN, NOTES_ID_LEN, &c->id) != NULL ||
        !notes_id_valid(c->id))
        return "'id' must be 32 lowercase hex characters";
    if ((err = json_get_bool(obj, "deleted", &c->deleted)) != NULL)
        return err;
    if (c->kind == NOTE && (err = json_get_int(obj, "base", 0, REV_MAX, &c->base)) != NULL)
        return err;
    if (c->deleted)
        return NULL; /* the id is all a removal needs */

    if (c->kind == REMINDER)
        return (err = json_get_text(obj, "text", 1, REMINDER_MAX, 0, &c->text)) != NULL
                   ? err
                   : get_time(obj, "created", &c->created);
    if (c->kind == NOTE) {
        if ((err = json_get_text(obj, "text", 1, NOTE_MAX, 1, &c->text)) != NULL ||
            (err = get_tags(obj, &c->tags)) != NULL ||
            (err = get_time(obj, "created", &c->created)) != NULL ||
            (err = get_time(obj, "updated", &c->updated)) != NULL)
            return err;
        return NULL;
    }
    if ((err = json_get_text(obj, "name", 1, NOTES_TAG_MAX, 0, &c->name)) != NULL)
        return err;
    if (json_get_string(obj, "color", 1, 16, &c->color) != NULL || !care_color_valid(c->color))
        return "'color' must be one of: " CARE_COLOR_NAMES;
    return NULL;
}

/* Reads body["changes"] into *out (arena). NULL or the error. */
static const char *get_changes(const cJSON *body, struct change **out, int *n)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(body, "changes");
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > CHANGES_MAX)
        return "'changes' must be an array of at most 500 changes";
    *n = cJSON_GetArraySize(array);
    *out = arena_alloc((size_t)(*n > 0 ? *n : 1) * sizeof **out);
    if (*out == NULL)
        return "out of memory";
    int i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        struct change *c = &(*out)[i];
        const char *err = get_change(item, c);
        if (err != NULL)
            return at(i, err);
        for (int j = 0; j < i; j++)
            if ((*out)[j].kind == c->kind && strcmp((*out)[j].id, c->id) == 0)
                return at(i, "the same kind and id as an earlier change");
        i++;
    }
    return NULL;
}

/* ---- applying them ------------------------------------------------------ */

/* A reminder is added once, removed once. 0 or -1 (logged). */
static int apply_reminder(const struct change *c)
{
    int rc = c->deleted
                 ? step_once(prepare("UPDATE reminders SET text = '', deleted = 1,"
                                     " rev = " REV_NEXT " WHERE id = ? AND deleted = 0", "t",
                                     c->id))
                 : step_once(prepare("INSERT INTO reminders (id, text, created, rev)"
                                     " VALUES (?, ?, ?, " REV_NEXT ")"
                                     " ON CONFLICT (id) DO NOTHING", "tti", c->id, c->text,
                                     c->created));
    return rc == SQLITE_DONE ? took_rev() : -1;
}

/* 1 if a live tag other than id has name (any case), 0 if not, -1 (logged). */
static int name_taken(const char *name, const char *id)
{
    int rc = step_once(prepare("SELECT 1 FROM tags WHERE name = ? COLLATE NOCASE"
                               " AND deleted = 0 AND id <> ?", "tt", name, id));
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/*
 * A tag: the device's change wins. A name another tag has gets a number.
 * 0, 1 if every numbered name is taken too, -1 (logged).
 */
static int apply_tag(const struct change *c)
{
    if (c->deleted)
        return step_once(prepare("UPDATE tags SET name = '', deleted = 1, rev = " REV_NEXT
                                 " WHERE id = ? AND deleted = 0", "t", c->id)) == SQLITE_DONE
                   ? took_rev()
                   : -1;
    static char numbered[NOTES_TAG_MAX + 1];
    const char *name = c->name;
    int taken = name_taken(name, c->id);
    for (int n = 2; taken == 1 && n <= 99; n++) {
        if (notes_suffix_name(c->name, n, numbered) != 0)
            return -1;
        name = numbered;
        taken = name_taken(name, c->id);
    }
    if (taken != 0)
        return taken < 0 ? -1 : 1;

    /* Sent again unchanged: nothing to do. */
    int rc = step_once(prepare("SELECT 1 FROM tags WHERE id = ? AND deleted = 0"
                               " AND name = ? AND color = ?", "ttt", c->id, name, c->color));
    if (rc != SQLITE_DONE)
        return rc == SQLITE_ROW ? 0 : -1;
    long rev = next_rev();
    return rev >= 0 && step_once(prepare("INSERT INTO tags (id, name, color, rev)"
                                         " VALUES (?, ?, ?, ?) ON CONFLICT (id) DO UPDATE SET"
                                         " name = excluded.name, color = excluded.color,"
                                         " rev = excluded.rev, deleted = 0", "ttti",
                                         c->id, name, c->color, rev)) == SQLITE_DONE ? 0 : -1;
}

/* Inserts a note with the device's content under id. 0 or -1 (logged). */
static int insert_note(const char *id, const struct change *c, long rev)
{
    return step_once(prepare("INSERT INTO notes (id, text, tags, created, updated, rev)"
                             " VALUES (?, ?, ?, ?, ?, ?)", "tttiii", id, c->text, c->tags,
                             c->created, c->updated, rev)) == SQLITE_DONE ? 0 : -1;
}

/*
 * A note (see the top of the file). Adds id: rev to applied when the note
 * now holds the device's change. 0 or -1 (logged).
 */
static int apply_note(const struct change *c, cJSON *applied)
{
    sqlite3_stmt *st = prepare("SELECT text, tags, rev, deleted FROM notes WHERE id = ?", "t",
                               c->id);
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        if (st != NULL)
            db_log_error(notes_db, "apply_note");
        sqlite3_finalize(st);
        return -1;
    }
    int exists = rc == SQLITE_ROW;
    long rev = exists ? (long)sqlite3_column_int64(st, 2) : 0;
    int deleted = exists && sqlite3_column_int(st, 3);
    const char *text = exists ? (const char *)sqlite3_column_text(st, 0) : NULL;
    const char *tags = exists ? (const char *)sqlite3_column_text(st, 1) : NULL;
    if (exists && (text == NULL || tags == NULL)) {
        fprintf(stderr, "notes: out of memory reading note %s\n", c->id);
        sqlite3_finalize(st);
        return -1;
    }
    int same = exists && deleted == c->deleted &&
               (deleted || (strcmp(text, c->text) == 0 && strcmp(tags, c->tags) == 0));
    sqlite3_finalize(st);

    if (!exists && c->deleted)
        return 0; /* never reached the server: nothing to remove */
    if (!same && exists && rev != c->base) {
        /* Both changed it. A removal is dropped; an edit becomes a new note. */
        if (c->deleted)
            return 0;
        char id[NOTES_ID_LEN + 1];
        long new_rev = next_rev();
        return notes_new_id(id) == 0 && new_rev >= 0 && insert_note(id, c, new_rev) == 0 ? 0
                                                                                        : -1;
    }
    if (!same) {
        rev = next_rev();
        if (rev < 0)
            return -1;
        if (!exists)
            rc = insert_note(c->id, c, rev) == 0 ? SQLITE_DONE : SQLITE_ERROR;
        else if (c->deleted)
            rc = step_once(prepare("UPDATE notes SET text = '', tags = '[]', deleted = 1,"
                                   " rev = ? WHERE id = ?", "it", rev, c->id));
        else
            rc = step_once(prepare("UPDATE notes SET text = ?, tags = ?, updated = ?,"
                                   " deleted = 0, rev = ? WHERE id = ?", "ttiit", c->text,
                                   c->tags, c->updated, rev, c->id));
        if (rc != SQLITE_DONE)
            return -1;
    }
    /* rev of a note stays far below 2^53: exact in a double */
    if (cJSON_AddNumberToObject(applied, c->id, (double)rev) == NULL) {
        fprintf(stderr, "notes: out of memory listing applied changes\n");
        return -1;
    }
    return 0;
}

/* ---- sending rows back -------------------------------------------------- */

/* The rows past since, never deleted ones for a device that has nothing
 * yet (since 0). ?1 since, ?2 up to this rev. */
#define PAST(cols, table) \
    "SELECT " cols " FROM " table " WHERE rev > ?1 AND rev <= ?2" \
    " AND (deleted = 0 OR ?1 > 0) ORDER BY rev"

/*
 * Adds key: [rows of sql] to obj. Columns are copied as they are, but a
 * column "tags" (JSON text) becomes an array and the last, "deleted",
 * true/false. 0 or -1 (logged).
 */
static int add_rows(cJSON *obj, const char *key, const char *sql, long since, long upto)
{
    cJSON *array = cJSON_AddArrayToObject(obj, key);
    sqlite3_stmt *st = array != NULL ? prepare(sql, "ii", since, upto) : NULL;
    if (st == NULL)
        return -1;
    int ncols = sqlite3_column_count(st);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *row = json_row(st, ncols - 1);
        int ok = row != NULL && cJSON_AddItemToArray(array, row) &&
                 cJSON_AddBoolToObject(row, "deleted", sqlite3_column_int(st, ncols - 1)) != NULL;
        const cJSON *text = ok ? cJSON_GetObjectItemCaseSensitive(row, "tags") : NULL;
        if (ok && text != NULL) {
            cJSON *list = cJSON_IsString(text) ? cJSON_Parse(text->valuestring) : NULL;
            ok = list != NULL && cJSON_ReplaceItemInObjectCaseSensitive(row, "tags", list);
        }
        if (!ok) {
            fprintf(stderr, "notes: out of memory building the rows of %s\n", key);
            sqlite3_finalize(st);
            return -1;
        }
    }
    if (rc != SQLITE_DONE)
        db_log_error(notes_db, sql);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/*
 * The rows past since, at most PULL_PAGE of them, oldest change first,
 * into obj; rev: the last rev they cover, more: whether there are others
 * after it. 0 or -1 (logged).
 */
static int add_pull(cJSON *obj, long since, long current)
{
    /* The PULL_PAGE-th row's rev, and whether a row comes after it. */
    sqlite3_stmt *st = prepare(
        "SELECT rev FROM (SELECT rev, deleted FROM reminders WHERE rev > ?1"
        " UNION ALL SELECT rev, deleted FROM notes WHERE rev > ?1"
        " UNION ALL SELECT rev, deleted FROM tags WHERE rev > ?1)"
        " WHERE deleted = 0 OR ?1 > 0 ORDER BY rev LIMIT 2 OFFSET ?2",
        "ii", since, (long)PULL_PAGE - 1);
    if (st == NULL)
        return -1;
    long upto = current;
    int rows = 0, rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        if (rows++ == 0)
            upto = (long)sqlite3_column_int64(st, 0);
    if (rc != SQLITE_DONE)
        db_log_error(notes_db, "add_pull");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return -1;
    int more = rows == 2;
    if (!more)
        upto = current;

    if (add_rows(obj, "reminders", PAST("id, text, created, rev, deleted", "reminders"), since,
                 upto) != 0 ||
        add_rows(obj, "notes",
                 PAST("id, text, tags, created, updated, rev, deleted", "notes"), since,
                 upto) != 0 ||
        add_rows(obj, "tags", PAST("id, name, color, rev, deleted", "tags"), since, upto) != 0)
        return -1;
    if (cJSON_AddNumberToObject(obj, "rev", (double)upto) == NULL ||
        cJSON_AddBoolToObject(obj, "more", more) == NULL) {
        fprintf(stderr, "notes: out of memory replying to a sync\n");
        return -1;
    }
    return 0;
}

/*
 * POST /api/notes/sync {since, changes: [...]}: applies the device's
 * changes (all of them or none), then sends back what changed past since.
 *
 * A change: {kind: "reminder", id, deleted, text, created}, {kind: "note",
 * id, base, deleted, text, tags: [tag id, ...], created, updated} or
 * {kind: "tag", id, deleted, name, color}; a removal (deleted true) needs
 * only kind, id (and a note's base).
 *
 * -> {rev, more, reset, applied: {note id: rev}, reminders: [{id, text,
 * created, rev, deleted}], notes: [{id, text, tags, created, updated, rev,
 * deleted}], tags: [{id, name, color, rev, deleted}]}. The device syncs
 * again with since = rev while more is true. reset: since was past the
 * server's counter (its data came back from a backup): the rows are
 * everything, and the device drops what it has that is not among them
 * and not its own change. applied: the rev each note now has the
 * device's change at (a note the server kept its own version of is not
 * there).
 */
void notes_sync(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    long since;
    struct change *changes;
    int n;
    if (body == NULL)
        return;
    const char *err = json_get_int(body, "since", 0, REV_MAX, &since);
    if (err == NULL)
        err = get_changes(body, &changes, &n);
    if (err != NULL) {
        int oom = strcmp(err, "out of memory") == 0;
        json_error(res, oom ? 500 : 400, oom ? "internal error" : err);
        return;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON *applied = out != NULL ? cJSON_AddObjectToObject(out, "applied") : NULL;
    if (applied == NULL || db_exec(notes_db, "BEGIN IMMEDIATE") != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    int rc = 0;
    for (int i = 0; i < n && rc == 0; i++)
        rc = changes[i].kind == REMINDER ? apply_reminder(&changes[i])
             : changes[i].kind == NOTE   ? apply_note(&changes[i], applied)
                                         : apply_tag(&changes[i]);
    if (rc != 0 || db_exec(notes_db, "COMMIT") != 0) {
        db_exec(notes_db, "ROLLBACK");
        json_error(res, rc > 0 ? 409 : 500,
                   rc > 0 ? "too many tags have that name: rename one first" : "internal error");
        return;
    }

    sqlite3_stmt *st = prepare("SELECT rev FROM sync", "");
    long current = st != NULL && sqlite3_step(st) == SQLITE_ROW
                       ? (long)sqlite3_column_int64(st, 0) : -1;
    if (st != NULL && current < 0)
        db_log_error(notes_db, "notes_sync rev");
    sqlite3_finalize(st);
    int reset = since > current;
    if (current < 0 || cJSON_AddBoolToObject(out, "reset", reset) == NULL ||
        add_pull(out, reset ? 0 : since, current) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}
