/*
 * Plant care: plants, care types (watering, fertilising, ...), care rules
 * (how often each type is due for a plant) and the care log.
 *
 * Reads are GET with ids in the query string; writes are POST with JSON.
 * Plants and care types are archived, never deleted. Due dates are computed
 * on every read from the rule and the latest log entry (src/care.c).
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "arena.h"
#include "care.h"
#include "db.h"
#include "json.h"

#define ID_MAX           9007199254740991L /* 2^53 - 1: exact in a JSON number */
#define PLANT_NAME_MAX   100
#define PLANT_FIELD_MAX  100  /* species, location */
#define NOTES_MAX        4000 /* plant notes, log notes */
#define TYPE_NAME_MAX    50
#define LOG_PAGE         50   /* log entries per GET */

/* ---- database helpers --------------------------------------------------- */

/*
 * Prepares sql and binds one argument per character of types: 'i' long,
 * 'z' long bound as NULL when 0, 't' text (a NULL pointer binds NULL).
 * Text must outlive the statement. Returns NULL on error (logged).
 */
static sqlite3_stmt *vprepare(const char *sql, const char *types, va_list ap)
{
    sqlite3_stmt *st = db_prepare(plants_db, sql);
    if (st == NULL)
        return NULL;
    int rc = SQLITE_OK;
    for (int i = 0; types[i] != '\0' && rc == SQLITE_OK; i++) {
        if (types[i] == 't') {
            const char *s = va_arg(ap, const char *);
            rc = s != NULL ? sqlite3_bind_text(st, i + 1, s, -1, SQLITE_STATIC)
                           : sqlite3_bind_null(st, i + 1);
        } else {
            long v = va_arg(ap, long);
            rc = types[i] == 'z' && v == 0 ? sqlite3_bind_null(st, i + 1)
                                           : sqlite3_bind_int64(st, i + 1, v);
        }
    }
    if (rc != SQLITE_OK) {
        db_log_error(plants_db, sql);
        sqlite3_finalize(st);
        return NULL;
    }
    return st;
}

static sqlite3_stmt *prepare(const char *sql, const char *types, ...)
{
    va_list ap;
    va_start(ap, types);
    sqlite3_stmt *st = vprepare(sql, types, ap);
    va_end(ap);
    return st;
}

/*
 * Runs a statement that returns no rows. 0 on success, 1 if it broke a
 * constraint (e.g. a duplicate name), -1 on any other error (logged).
 */
static int run(const char *sql, const char *types, ...)
{
    va_list ap;
    va_start(ap, types);
    sqlite3_stmt *st = vprepare(sql, types, ap);
    va_end(ap);
    if (st == NULL)
        return -1;
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT)
        db_log_error(plants_db, sql);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : rc == SQLITE_CONSTRAINT ? 1 : -1;
}

/*
 * Looks up the archived flag of a plant or care type. sql selects
 * `archived` by id. Returns 0 missing, 1 active, 2 archived, -1 on error.
 */
static int archived_state(const char *sql, long id)
{
    sqlite3_stmt *st = prepare(sql, "i", id);
    if (st == NULL)
        return -1;
    int rc = sqlite3_step(st);
    int state = rc == SQLITE_ROW ? 1 + sqlite3_column_int(st, 0) : rc == SQLITE_DONE ? 0 : -1;
    if (state < 0)
        db_log_error(plants_db, sql);
    sqlite3_finalize(st);
    return state;
}

#define PLANT_STATE "SELECT archived FROM plants WHERE id = ?"
#define TYPE_STATE  "SELECT archived FROM care_types WHERE id = ?"

/*
 * A JSON object of the first ncols columns of the current row, keyed by
 * column name: integers, text or null. NULL when out of memory.
 */
static cJSON *row_object(sqlite3_stmt *st, int ncols)
{
    cJSON *obj = cJSON_CreateObject();
    for (int i = 0; obj != NULL && i < ncols; i++) {
        const char *key = sqlite3_column_name(st, i);
        cJSON *item;
        switch (sqlite3_column_type(st, i)) {
        case SQLITE_INTEGER:
            /* ids and small numbers: exact in a double */
            item = cJSON_AddNumberToObject(obj, key, (double)sqlite3_column_int64(st, i));
            break;
        case SQLITE_TEXT:
            item = cJSON_AddStringToObject(obj, key, (const char *)sqlite3_column_text(st, i));
            break;
        default:
            item = cJSON_AddNullToObject(obj, key);
        }
        if (item == NULL)
            obj = NULL; /* the arena frees what was built */
    }
    return obj;
}

/* Adds day as "YYYY-MM-DD", or null for CARE_NEVER. NULL when out of memory. */
static cJSON *add_date(cJSON *obj, const char *key, long day)
{
    if (day == CARE_NEVER)
        return cJSON_AddNullToObject(obj, key);
    char s[16];
    care_format_date(day, s);
    return cJSON_AddStringToObject(obj, key, s);
}

/* Replies 201 {"id": the last inserted row id}. */
static void reply_created(struct response *res)
{
    cJSON *out = cJSON_CreateObject();
    /* row ids stay far below 2^53: exact in a double */
    if (out == NULL ||
        cJSON_AddNumberToObject(out, "id", (double)sqlite3_last_insert_rowid(plants_db)) == NULL) {
        fprintf(stderr, "plants: out of memory replying with a new id\n");
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 201, out);
}

/* Copies a text column into the arena; NULL if NULL or out of memory. */
static const char *column_dup(sqlite3_stmt *st, int col)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    return s != NULL ? arena_strndup(s, (size_t)sqlite3_column_bytes(st, col)) : NULL;
}

/* ---- input helpers ------------------------------------------------------ */

/* Replies 400 with err and returns 1 if err is set. */
static int bad_request(struct response *res, const char *err)
{
    if (err == NULL)
        return 0;
    json_error(res, 400, err);
    return 1;
}

/*
 * Reads a positive id from the query string; if optional, an absent one is
 * 0. Replies 400 and returns -1 if invalid.
 */
static int query_id(const struct request *req, struct response *res, const char *name,
                    int optional, long *out)
{
    const char *s = NULL;
    int found = http_query(req, name, &s);
    if (found == 1 && optional) {
        *out = 0;
        return 0;
    }
    if (found == 0 && s[0] >= '0' && s[0] <= '9') {
        char *end;
        errno = 0;
        long v = strtol(s, &end, 10);
        if (errno == 0 && *end == '\0' && v >= 1 && v <= ID_MAX) {
            *out = v;
            return 0;
        }
    }
    char msg[96];
    snprintf(msg, sizeof msg, "query parameter '%s' must be a positive whole number", name);
    json_error(res, 400, msg);
    return -1;
}

/* Reads obj[key] as an id; 0 if the value is null and nullable is set. */
static const char *get_id(const cJSON *obj, const char *key, int nullable, long *out)
{
    if (nullable && cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(obj, key))) {
        *out = 0;
        return NULL;
    }
    return json_get_int(obj, key, 1, ID_MAX, out);
}

/* Reads an interval in days; 0 (paused) if the value is null. */
static const char *get_interval(const cJSON *obj, const char *key, int *out)
{
    long v = 0;
    const char *err = NULL;
    if (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(obj, key)))
        err = json_get_int(obj, key, 1, CARE_MAX_INTERVAL, &v);
    *out = (int)v;
    return err;
}

/*
 * Reads a date "YYYY-MM-DD" that is not in the future. If nullable, null is
 * accepted and gives *out = NULL.
 */
static const char *get_past_date(const cJSON *obj, const char *key, int nullable,
                                 const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (nullable && cJSON_IsNull(item)) {
        *out = NULL;
        return NULL;
    }
    long day;
    if (cJSON_IsString(item) && care_parse_date(item->valuestring, &day) == 0 &&
        day <= care_today()) {
        *out = item->valuestring;
        return NULL;
    }
    char *msg = arena_alloc(128);
    if (msg == NULL)
        return "invalid date";
    snprintf(msg, 128, "'%s' must be %sa date YYYY-MM-DD, not in the future", key,
             nullable ? "null or " : "");
    return msg;
}

/* Reads month/day pair keys; both must form a valid month/day (Feb 29 allowed). */
static const char *get_month_day(const cJSON *obj, const char *mkey, const char *dkey,
                                 int *month, int *day)
{
    long m, d;
    const char *err = json_get_int(obj, mkey, 1, 12, &m);
    if (err == NULL)
        err = json_get_int(obj, dkey, 1, 31, &d);
    if (err != NULL)
        return err;
    if (!care_month_day_valid(m, d)) {
        char *msg = arena_alloc(128);
        if (msg == NULL)
            return "invalid month/day";
        snprintf(msg, 128, "'%s'/'%s' is not a day of the year", mkey, dkey);
        return msg;
    }
    *month = (int)m;
    *day = (int)d;
    return NULL;
}

/* ---- plants ------------------------------------------------------------- */

struct plant_fields {
    const char *name, *species, *location, *acquired, *notes;
};

static const char *get_plant_fields(const cJSON *obj, struct plant_fields *p)
{
    const char *err;
    if ((err = json_get_text(obj, "name", 1, PLANT_NAME_MAX, 0, &p->name)) != NULL ||
        (err = json_get_text(obj, "species", 0, PLANT_FIELD_MAX, 0, &p->species)) != NULL ||
        (err = json_get_text(obj, "location", 0, PLANT_FIELD_MAX, 0, &p->location)) != NULL ||
        (err = get_past_date(obj, "acquired", 1, &p->acquired)) != NULL ||
        (err = json_get_text(obj, "notes", 0, NOTES_MAX, 1, &p->notes)) != NULL)
        return err;
    return NULL;
}

/*
 * Appends one JSON object per row of st to array. Row columns are copied
 * as-is, except a last column named "archived" becomes true/false.
 * 0, or -1 on error (logged).
 */
static int add_rows(cJSON *array, sqlite3_stmt *st)
{
    int ncols = sqlite3_column_count(st);
    int last_is_archived =
        ncols > 0 && strcmp(sqlite3_column_name(st, ncols - 1), "archived") == 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *obj = row_object(st, last_is_archived ? ncols - 1 : ncols);
        if (obj == NULL || !cJSON_AddItemToArray(array, obj) ||
            (last_is_archived &&
             cJSON_AddBoolToObject(obj, "archived", sqlite3_column_int(st, ncols - 1)) == NULL)) {
            fprintf(stderr, "plants: out of memory building a list\n");
            return -1;
        }
    }
    if (rc != SQLITE_DONE) {
        db_log_error(plants_db, sqlite3_sql(st));
        return -1;
    }
    return 0;
}

/* Adds key: [rows of sql] to obj. 0 or -1 (logged). */
static int add_list(cJSON *obj, const char *key, const char *sql)
{
    cJSON *array = cJSON_AddArrayToObject(obj, key);
    sqlite3_stmt *st = array != NULL ? prepare(sql, "") : NULL;
    if (st == NULL)
        return -1;
    int rc = add_rows(array, st);
    sqlite3_finalize(st);
    return rc;
}

/* GET /api/plants: every plant and care type, archived ones included. */
void plants_list(struct request *req, struct response *res)
{
    (void)req;
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL ||
        add_list(obj, "plants",
                 "SELECT id, name, species, location, acquired, notes, archived FROM plants"
                 " ORDER BY name COLLATE NOCASE, id") != 0 ||
        add_list(obj, "care_types",
                 "SELECT id, name, archived FROM care_types ORDER BY name COLLATE NOCASE") != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* POST /api/plants/add {name, species, location, acquired, notes} -> 201 {id} */
void plants_add(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    struct plant_fields p;
    if (obj == NULL || bad_request(res, get_plant_fields(obj, &p)))
        return;
    if (run("INSERT INTO plants (name, species, location, acquired, notes) VALUES (?, ?, ?, ?, ?)",
            "ttttt", p.name, p.species, p.location, p.acquired, p.notes) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    reply_created(res);
}

/* POST /api/plants/update {id, name, species, location, acquired, notes} */
void plants_update(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    struct plant_fields p;
    long id;
    if (obj == NULL || bad_request(res, get_id(obj, "id", 0, &id)) ||
        bad_request(res, get_plant_fields(obj, &p)))
        return;
    if (run("UPDATE plants SET name = ?, species = ?, location = ?, acquired = ?, notes = ?"
            " WHERE id = ?",
            "ttttti", p.name, p.species, p.location, p.acquired, p.notes, id) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (sqlite3_changes(plants_db) == 0) {
        json_error(res, 404, "plant not found");
        return;
    }
    res->status = 204;
}

/* Shared by the two archive routes: sql updates `archived` by id. */
static void set_archived(struct request *req, struct response *res, const char *sql,
                         const char *not_found)
{
    cJSON *obj = json_body(req, res);
    long id;
    int archived;
    if (obj == NULL || bad_request(res, get_id(obj, "id", 0, &id)) ||
        bad_request(res, json_get_bool(obj, "archived", &archived)))
        return;
    if (run(sql, "ii", (long)archived, id) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (sqlite3_changes(plants_db) == 0) {
        json_error(res, 404, not_found);
        return;
    }
    res->status = 204;
}

/* POST /api/plants/archive {id, archived} */
void plants_archive(struct request *req, struct response *res)
{
    set_archived(req, res, "UPDATE plants SET archived = ? WHERE id = ?", "plant not found");
}

/* ---- care types --------------------------------------------------------- */

/* POST /api/plants/types/add {name} -> 201 {id} */
void plants_type_add(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    const char *name;
    if (obj == NULL || bad_request(res, json_get_text(obj, "name", 1, TYPE_NAME_MAX, 0, &name)))
        return;
    int rc = run("INSERT INTO care_types (name) VALUES (?)", "t", name);
    if (rc != 0) {
        json_error(res, rc == 1 ? 409 : 500,
                   rc == 1 ? "a care type with this name exists" : "internal error");
        return;
    }
    reply_created(res);
}

/* POST /api/plants/types/update {id, name} */
void plants_type_update(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    const char *name;
    long id;
    if (obj == NULL || bad_request(res, get_id(obj, "id", 0, &id)) ||
        bad_request(res, json_get_text(obj, "name", 1, TYPE_NAME_MAX, 0, &name)))
        return;
    int rc = run("UPDATE care_types SET name = ? WHERE id = ?", "ti", name, id);
    if (rc != 0) {
        json_error(res, rc == 1 ? 409 : 500,
                   rc == 1 ? "a care type with this name exists" : "internal error");
        return;
    }
    if (sqlite3_changes(plants_db) == 0) {
        json_error(res, 404, "care type not found");
        return;
    }
    res->status = 204;
}

/* POST /api/plants/types/archive {id, archived} */
void plants_type_archive(struct request *req, struct response *res)
{
    set_archived(req, res, "UPDATE care_types SET archived = ? WHERE id = ?",
                 "care type not found");
}

/* ---- rules and due dates ------------------------------------------------ */

/* A rule read from the database, with its computed due date. */
struct loaded_rule {
    struct care_rule rule;
    struct care_period periods[CARE_MAX_PERIODS];
    long created, last, due; /* last, due: CARE_NEVER if none */
};

/*
 * The columns load_rule() reads, in this order: interval_days, yearly_month,
 * yearly_day, created, and the date it was last done (NULL if never).
 */
#define RULE_COLUMNS                                                         \
    "r.interval_days, r.yearly_month, r.yearly_day, r.created,"              \
    " (SELECT max(date) FROM care_log l"                                     \
    "  WHERE l.plant_id = r.plant_id AND l.care_type_id = r.care_type_id)"

/* Parses a stored date column; CARE_NEVER if NULL. -1 if corrupt (logged). */
static int column_date(sqlite3_stmt *st, int col, long *out)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    if (s == NULL) {
        *out = CARE_NEVER;
        return 0;
    }
    if (care_parse_date(s, out) != 0) {
        fprintf(stderr, "plants: stored date '%.16s' is invalid\n", s);
        return -1;
    }
    return 0;
}

/*
 * Reads RULE_COLUMNS from st at column col, loads the rule's periods and
 * computes its due date. 0 or -1 on error (logged).
 */
static int load_rule(sqlite3_stmt *st, int col, long plant_id, long type_id,
                     struct loaded_rule *r)
{
    memset(r, 0, sizeof *r);
    r->rule.interval = sqlite3_column_int(st, col);
    r->rule.yearly_month = sqlite3_column_int(st, col + 1);
    r->rule.yearly_day = sqlite3_column_int(st, col + 2);
    r->rule.periods = r->periods;
    if (column_date(st, col + 3, &r->created) != 0 || column_date(st, col + 4, &r->last) != 0)
        return -1;

    sqlite3_stmt *ps = prepare("SELECT start_month, start_day, end_month, end_day, interval_days"
                               " FROM care_periods WHERE plant_id = ? AND care_type_id = ?",
                               "ii", plant_id, type_id);
    if (ps == NULL)
        return -1;
    int rc;
    while ((rc = sqlite3_step(ps)) == SQLITE_ROW) {
        if (r->rule.nperiods == CARE_MAX_PERIODS) {
            fprintf(stderr, "plants: rule %ld/%ld has more than %d periods\n", plant_id,
                    type_id, CARE_MAX_PERIODS);
            sqlite3_finalize(ps);
            return -1;
        }
        struct care_period *p = &r->periods[r->rule.nperiods++];
        p->start_month = sqlite3_column_int(ps, 0);
        p->start_day = sqlite3_column_int(ps, 1);
        p->end_month = sqlite3_column_int(ps, 2);
        p->end_day = sqlite3_column_int(ps, 3);
        p->interval = sqlite3_column_int(ps, 4);
    }
    if (rc != SQLITE_DONE)
        db_log_error(plants_db, "care_periods");
    sqlite3_finalize(ps);
    if (rc != SQLITE_DONE)
        return -1;

    r->due = care_next_due(&r->rule, r->created, r->last);
    return 0;
}

/* Adds due (date or null) and days_left (due - today, or null). */
static int add_due(cJSON *obj, long due, long today)
{
    if (add_date(obj, "due", due) == NULL)
        return -1;
    cJSON *left = due == CARE_NEVER ? cJSON_AddNullToObject(obj, "days_left")
                                    : cJSON_AddNumberToObject(obj, "days_left",
                                                              (double)(due - today));
    return left == NULL ? -1 : 0;
}

/* Adds v as a number, or null when v is 0 (paused / not yearly). */
static cJSON *add_int_or_null(cJSON *obj, const char *key, int v)
{
    return v == 0 ? cJSON_AddNullToObject(obj, key) : cJSON_AddNumberToObject(obj, key, v);
}

/* The JSON for one rule of a plant (see plants_get). */
static cJSON *rule_object(const struct loaded_rule *r, long type_id, const char *type_name,
                          long today)
{
    const struct care_rule *rule = &r->rule;
    cJSON *obj = cJSON_CreateObject();
    int ok = obj != NULL;
    ok = ok && cJSON_AddNumberToObject(obj, "care_type_id", (double)type_id) != NULL;
    ok = ok && cJSON_AddStringToObject(obj, "care_type", type_name) != NULL;
    ok = ok && add_int_or_null(obj, "interval_days", rule->interval) != NULL;
    ok = ok && add_int_or_null(obj, "yearly_month", rule->yearly_month) != NULL;
    ok = ok && add_int_or_null(obj, "yearly_day", rule->yearly_day) != NULL;
    cJSON *periods = ok ? cJSON_AddArrayToObject(obj, "periods") : NULL;
    ok = periods != NULL;
    for (size_t i = 0; ok && i < rule->nperiods; i++) {
        const struct care_period *p = &rule->periods[i];
        cJSON *po = cJSON_CreateObject();
        ok = po != NULL && cJSON_AddItemToArray(periods, po);
        ok = ok && cJSON_AddNumberToObject(po, "start_month", p->start_month) != NULL;
        ok = ok && cJSON_AddNumberToObject(po, "start_day", p->start_day) != NULL;
        ok = ok && cJSON_AddNumberToObject(po, "end_month", p->end_month) != NULL;
        ok = ok && cJSON_AddNumberToObject(po, "end_day", p->end_day) != NULL;
        ok = ok && add_int_or_null(po, "interval_days", p->interval) != NULL;
    }
    ok = ok && add_date(obj, "created", r->created) != NULL;
    ok = ok && add_date(obj, "last_done", r->last) != NULL;
    ok = ok && add_due(obj, r->due, today) == 0;
    return ok ? obj : NULL;
}

/* GET /api/plants/plant?id=N: the plant and its rules with due dates. */
void plants_get(struct request *req, struct response *res)
{
    long id;
    if (query_id(req, res, "id", 0, &id) != 0)
        return;

    sqlite3_stmt *st = prepare("SELECT id, name, species, location, acquired, notes, archived"
                               " FROM plants WHERE id = ?", "i", id);
    if (st == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    cJSON *plant = cJSON_CreateArray();
    int rc = plant != NULL ? add_rows(plant, st) : -1;
    sqlite3_finalize(st);
    if (rc != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    cJSON *obj = cJSON_GetArrayItem(plant, 0);
    if (obj == NULL) {
        json_error(res, 404, "plant not found");
        return;
    }
    cJSON_DetachItemViaPointer(plant, obj);

    cJSON *rules = cJSON_AddArrayToObject(obj, "rules");
    st = rules != NULL ? prepare("SELECT r.care_type_id, t.name, " RULE_COLUMNS
                                 " FROM care_rules r JOIN care_types t ON t.id = r.care_type_id"
                                 " WHERE r.plant_id = ? ORDER BY t.name COLLATE NOCASE", "i", id)
                       : NULL;
    if (st == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    long today = care_today();
    /* A break leaves rc == SQLITE_ROW: failed, cause already logged. */
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct loaded_rule r;
        long type_id = sqlite3_column_int64(st, 0);
        if (load_rule(st, 2, id, type_id, &r) != 0)
            break;
        cJSON *rule = rule_object(&r, type_id, (const char *)sqlite3_column_text(st, 1), today);
        if (rule == NULL || !cJSON_AddItemToArray(rules, rule)) {
            fprintf(stderr, "plants: out of memory building rules\n");
            break;
        }
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        db_log_error(plants_db, "plants_get rules");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* One line of the due list. */
struct due_item {
    long plant_id, type_id, due;
    const char *plant, *type;
};

static int due_cmp(const void *a, const void *b)
{
    const struct due_item *x = a, *y = b;
    if (x->due != y->due)
        return x->due < y->due ? -1 : 1;
    int c = strcmp(x->plant, y->plant);
    return c != 0 ? c : strcmp(x->type, y->type);
}

#define DUE_FROM                                                             \
    " FROM care_rules r JOIN plants p ON p.id = r.plant_id"                  \
    " JOIN care_types t ON t.id = r.care_type_id"                            \
    " WHERE p.archived = 0 AND t.archived = 0"

/*
 * GET /api/plants/due: every rule of an active plant and care type that has
 * a due date, soonest first. days_left < 0 means overdue.
 */
void plants_due(struct request *req, struct response *res)
{
    (void)req;
    sqlite3_stmt *st = prepare("SELECT count(*)" DUE_FROM, "");
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    size_t max = rc == SQLITE_ROW ? (size_t)sqlite3_column_int64(st, 0) : 0;
    if (st != NULL && rc != SQLITE_ROW)
        db_log_error(plants_db, "plants_due count");
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
        json_error(res, 500, "internal error");
        return;
    }

    struct due_item *items = arena_alloc((max > 0 ? max : 1) * sizeof *items);
    st = items != NULL ? prepare("SELECT r.plant_id, p.name, r.care_type_id, t.name, "
                                 RULE_COLUMNS DUE_FROM, "")
                       : NULL;
    if (st == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    /* A break leaves rc == SQLITE_ROW: failed, cause already logged. */
    size_t n = 0, rows = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (rows++ == max) { /* cannot happen: one connection, one request */
            fprintf(stderr, "plants: due list grew while reading\n");
            break;
        }
        struct due_item *it = &items[n];
        struct loaded_rule r;
        it->plant_id = sqlite3_column_int64(st, 0);
        it->type_id = sqlite3_column_int64(st, 2);
        it->plant = column_dup(st, 1);
        it->type = column_dup(st, 3);
        if (it->plant == NULL || it->type == NULL) {
            fprintf(stderr, "plants: out of memory reading the due list\n");
            break;
        }
        if (load_rule(st, 4, it->plant_id, it->type_id, &r) != 0)
            break;
        it->due = r.due;
        if (r.due != CARE_NEVER)
            n++;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        db_log_error(plants_db, "plants_due");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    qsort(items, n, sizeof *items, due_cmp);

    long today = care_today();
    cJSON *list = cJSON_CreateArray();
    for (size_t i = 0; list != NULL && i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        int ok = o != NULL && cJSON_AddItemToArray(list, o);
        ok = ok && cJSON_AddNumberToObject(o, "plant_id", (double)items[i].plant_id) != NULL;
        ok = ok && cJSON_AddStringToObject(o, "plant", items[i].plant) != NULL;
        ok = ok && cJSON_AddNumberToObject(o, "care_type_id", (double)items[i].type_id) != NULL;
        ok = ok && cJSON_AddStringToObject(o, "care_type", items[i].type) != NULL;
        ok = ok && add_due(o, items[i].due, today) == 0;
        if (!ok)
            list = NULL;
    }
    if (list == NULL)
        fprintf(stderr, "plants: out of memory building the due list\n");
    json_reply(res, 200, list);
}

/* Validates obj's "periods" array into periods[0..*n). */
static const char *get_periods(const cJSON *obj, struct care_period *periods, size_t *n)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(obj, "periods");
    if (!cJSON_IsArray(array))
        return "'periods' must be an array";
    int count = cJSON_GetArraySize(array);
    if (count > CARE_MAX_PERIODS)
        return "'periods' may have at most 12 entries";
    *n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        struct care_period *p = &periods[*n];
        const char *err;
        if (!cJSON_IsObject(item))
            return "each period must be an object";
        if ((err = get_month_day(item, "start_month", "start_day", &p->start_month,
                                 &p->start_day)) != NULL ||
            (err = get_month_day(item, "end_month", "end_day", &p->end_month,
                                 &p->end_day)) != NULL ||
            (err = get_interval(item, "interval_days", &p->interval)) != NULL)
            return err;
        for (size_t j = 0; j < *n; j++)
            if (care_periods_overlap(&periods[j], p))
                return "periods must not overlap";
        (*n)++;
    }
    return NULL;
}

/*
 * Validates a rule: either a yearly date, or an interval (null: paused)
 * with seasonal periods; it must come due on some day of the year.
 */
static const char *get_rule(const cJSON *obj, struct care_rule *rule,
                            struct care_period *periods)
{
    const char *err;
    memset(rule, 0, sizeof *rule);
    rule->periods = periods;
    if ((err = get_interval(obj, "interval_days", &rule->interval)) != NULL ||
        (err = get_periods(obj, periods, &rule->nperiods)) != NULL)
        return err;

    int no_month = cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(obj, "yearly_month"));
    int no_day = cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(obj, "yearly_day"));
    if (!no_month || !no_day) {
        if ((err = get_month_day(obj, "yearly_month", "yearly_day", &rule->yearly_month,
                                 &rule->yearly_day)) != NULL)
            return err;
        if (rule->interval != 0 || rule->nperiods > 0)
            return "a yearly rule has no 'interval_days' and no 'periods'";
        return NULL;
    }

    int active = rule->interval != 0;
    for (size_t i = 0; i < rule->nperiods; i++)
        active |= periods[i].interval != 0;
    if (!active)
        return "the rule is paused all year: set 'interval_days' or a period's interval";
    return NULL;
}

/* Checks a care type can be used in a rule or log entry; replies if not. 0 if ok. */
static int check_type(struct response *res, long type_id)
{
    int state = archived_state(TYPE_STATE, type_id);
    if (state == 1)
        return 0;
    if (state < 0)
        json_error(res, 500, "internal error");
    else if (state == 0)
        json_error(res, 404, "care type not found");
    else
        json_error(res, 400, "care type is archived");
    return -1;
}

/* Checks the plant exists; replies if not. 0 if ok. */
static int check_plant(struct response *res, long plant_id)
{
    int state = archived_state(PLANT_STATE, plant_id);
    if (state > 0)
        return 0;
    json_error(res, state < 0 ? 500 : 404, state < 0 ? "internal error" : "plant not found");
    return -1;
}

/* Inserts or replaces a rule and its periods. 0 or -1 (logged). */
static int store_rule(long plant_id, long type_id, const struct care_rule *rule)
{
    char today[16];
    care_format_date(care_today(), today);
    if (run("INSERT INTO care_rules"
            " (plant_id, care_type_id, interval_days, yearly_month, yearly_day, created)"
            " VALUES (?, ?, ?, ?, ?, ?)"
            " ON CONFLICT (plant_id, care_type_id) DO UPDATE SET"
            " interval_days = excluded.interval_days, yearly_month = excluded.yearly_month,"
            " yearly_day = excluded.yearly_day",
            "iizzzt", plant_id, type_id, (long)rule->interval, (long)rule->yearly_month,
            (long)rule->yearly_day, today) != 0 ||
        run("DELETE FROM care_periods WHERE plant_id = ? AND care_type_id = ?", "ii", plant_id,
            type_id) != 0)
        return -1;
    for (size_t i = 0; i < rule->nperiods; i++) {
        const struct care_period *p = &rule->periods[i];
        if (run("INSERT INTO care_periods (plant_id, care_type_id, start_month, start_day,"
                " end_month, end_day, interval_days) VALUES (?, ?, ?, ?, ?, ?, ?)",
                "iiiiiiz", plant_id, type_id, (long)p->start_month, (long)p->start_day,
                (long)p->end_month, (long)p->end_day, (long)p->interval) != 0)
            return -1;
    }
    return 0;
}

/*
 * POST /api/plants/rules/save {plant_id, care_type_id, interval_days,
 * yearly_month, yearly_day, periods: [{start_month, start_day, end_month,
 * end_day, interval_days}]}: creates or replaces the rule. A new rule is
 * first due today (or on its next yearly date / first unpaused day).
 */
void plants_rule_save(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    long plant_id, type_id;
    struct care_rule rule;
    struct care_period periods[CARE_MAX_PERIODS];
    if (obj == NULL || bad_request(res, get_id(obj, "plant_id", 0, &plant_id)) ||
        bad_request(res, get_id(obj, "care_type_id", 0, &type_id)) ||
        bad_request(res, get_rule(obj, &rule, periods)) || check_plant(res, plant_id) != 0 ||
        check_type(res, type_id) != 0)
        return;

    if (db_exec(plants_db, "BEGIN IMMEDIATE") != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (store_rule(plant_id, type_id, &rule) != 0 || db_exec(plants_db, "COMMIT") != 0) {
        db_exec(plants_db, "ROLLBACK");
        json_error(res, 500, "internal error");
        return;
    }
    res->status = 204;
}

/* POST /api/plants/rules/delete {plant_id, care_type_id}; the log is kept. */
void plants_rule_delete(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    long plant_id, type_id;
    if (obj == NULL || bad_request(res, get_id(obj, "plant_id", 0, &plant_id)) ||
        bad_request(res, get_id(obj, "care_type_id", 0, &type_id)))
        return;
    /* care_periods go with it (ON DELETE CASCADE) */
    if (run("DELETE FROM care_rules WHERE plant_id = ? AND care_type_id = ?", "ii", plant_id,
            type_id) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (sqlite3_changes(plants_db) == 0) {
        json_error(res, 404, "rule not found");
        return;
    }
    res->status = 204;
}

/* ---- care log ----------------------------------------------------------- */

/*
 * GET /api/plants/log?plant_id=N[&before=ID]: the newest LOG_PAGE entries,
 * or those older than entry ID. {entries: [{id, care_type_id, date, note}],
 * more: whether older entries exist}
 */
void plants_log(struct request *req, struct response *res)
{
    long plant_id, before = 0;
    if (query_id(req, res, "plant_id", 0, &plant_id) != 0 ||
        query_id(req, res, "before", 1, &before) != 0 ||
        check_plant(res, plant_id) != 0)
        return;
    if (before != 0) {
        sqlite3_stmt *st = prepare("SELECT 1 FROM care_log WHERE id = ? AND plant_id = ?", "ii",
                                   before, plant_id);
        int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
        sqlite3_finalize(st);
        if (rc != SQLITE_ROW) {
            if (rc != SQLITE_DONE)
                db_log_error(plants_db, "plants_log before");
            json_error(res, rc == SQLITE_DONE ? 404 : 500,
                       rc == SQLITE_DONE ? "log entry not found" : "internal error");
            return;
        }
    }

    sqlite3_stmt *st = prepare(
        "SELECT id, care_type_id, date, note FROM care_log WHERE plant_id = ?1"
        " AND (?2 IS NULL OR (date, id) < (SELECT date, id FROM care_log WHERE id = ?2))"
        " ORDER BY date DESC, id DESC LIMIT ?3",
        "izi", plant_id, before, (long)LOG_PAGE + 1);
    cJSON *obj = cJSON_CreateObject();
    cJSON *entries = obj != NULL ? cJSON_AddArrayToObject(obj, "entries") : NULL;
    int rc = st != NULL && entries != NULL ? add_rows(entries, st) : -1;
    sqlite3_finalize(st);
    if (rc != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    int more = cJSON_GetArraySize(entries) > LOG_PAGE;
    if (more)
        cJSON_DeleteItemFromArray(entries, LOG_PAGE);
    json_reply(res, 200, cJSON_AddBoolToObject(obj, "more", more) != NULL ? obj : NULL);
}

struct log_fields {
    long type_id; /* 0: a plain note */
    const char *date, *note;
};

static const char *get_log_fields(const cJSON *obj, struct log_fields *f)
{
    const char *err;
    if ((err = get_id(obj, "care_type_id", 1, &f->type_id)) != NULL ||
        (err = get_past_date(obj, "date", 0, &f->date)) != NULL ||
        (err = json_get_text(obj, "note", 0, NOTES_MAX, 1, &f->note)) != NULL)
        return err;
    return NULL;
}

/* POST /api/plants/log/add {plant_id, care_type_id or null, date, note} -> 201 {id} */
void plants_log_add(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    long plant_id;
    struct log_fields f;
    if (obj == NULL || bad_request(res, get_id(obj, "plant_id", 0, &plant_id)) ||
        bad_request(res, get_log_fields(obj, &f)) || check_plant(res, plant_id) != 0 ||
        (f.type_id != 0 && check_type(res, f.type_id) != 0))
        return;
    if (run("INSERT INTO care_log (plant_id, care_type_id, date, note) VALUES (?, ?, ?, ?)",
            "iztt", plant_id, f.type_id, f.date, f.note) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    reply_created(res);
}

/*
 * POST /api/plants/log/update {id, care_type_id or null, date, note}.
 * An entry may keep an archived care type, but not be moved to one.
 */
void plants_log_update(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    long id;
    struct log_fields f;
    if (obj == NULL || bad_request(res, get_id(obj, "id", 0, &id)) ||
        bad_request(res, get_log_fields(obj, &f)))
        return;

    sqlite3_stmt *st = prepare("SELECT ifnull(care_type_id, 0) FROM care_log WHERE id = ?", "i", id);
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    long old_type = rc == SQLITE_ROW ? (long)sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE)
            db_log_error(plants_db, "plants_log_update");
        json_error(res, rc == SQLITE_DONE ? 404 : 500,
                   rc == SQLITE_DONE ? "log entry not found" : "internal error");
        return;
    }
    if (f.type_id != 0 && f.type_id != old_type && check_type(res, f.type_id) != 0)
        return;

    if (run("UPDATE care_log SET care_type_id = ?, date = ?, note = ? WHERE id = ?", "ztti",
            f.type_id, f.date, f.note, id) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    res->status = 204;
}

/* POST /api/plants/log/delete {id} */
void plants_log_delete(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    long id;
    if (obj == NULL || bad_request(res, get_id(obj, "id", 0, &id)))
        return;
    if (run("DELETE FROM care_log WHERE id = ?", "i", id) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (sqlite3_changes(plants_db) == 0) {
        json_error(res, 404, "log entry not found");
        return;
    }
    res->status = 204;
}
