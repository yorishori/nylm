#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "db.h"
#include "json.h"

#define TITLE_MAX 200
#define BODY_MAX  10000

#define NOTE_COLUMNS "id, title, body, created_at, updated_at"

/* Parses the :id route segment; 0 if it is not a positive integer. */
static sqlite3_int64 parse_id(const char *s)
{
    if (s == NULL || *s == '\0' || strlen(s) > 18)
        return 0;
    for (const char *p = s; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return strtoll(s, NULL, 10);
}

/* Builds a note object from a row selected with NOTE_COLUMNS. */
static cJSON *note_from_row(sqlite3_stmt *st)
{
    cJSON *note = cJSON_CreateObject();
    cJSON_AddNumberToObject(note, "id", (double)sqlite3_column_int64(st, 0));
    cJSON_AddStringToObject(note, "title", (const char *)sqlite3_column_text(st, 1));
    cJSON_AddStringToObject(note, "body", (const char *)sqlite3_column_text(st, 2));
    cJSON_AddNumberToObject(note, "created_at", (double)sqlite3_column_int64(st, 3));
    cJSON_AddNumberToObject(note, "updated_at", (double)sqlite3_column_int64(st, 4));
    return note;
}

/*
 * Steps a statement expected to return at most one note and replies with
 * it (ok_status), 404 if there was no row, or 500 on error.
 */
static void reply_one(sqlite3_stmt *st, int ok_status, struct response *res)
{
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        json_reply(res, ok_status, note_from_row(st));
    else if (rc == SQLITE_DONE)
        json_error(res, 404, "note not found");
    else {
        db_log_error("notes");
        json_error(res, 500, "database error");
    }
    sqlite3_finalize(st);
}

/* Validates {"title": ..., "body": ...}; on error replies and returns -1. */
static int read_note(const struct request *req, struct response *res, const char **title,
                     const char **body)
{
    cJSON *obj = json_body(req, res);
    if (obj == NULL)
        return -1;
    const char *err = json_get_string(obj, "title", 1, TITLE_MAX, title);
    if (err == NULL)
        err = json_get_string(obj, "body", 0, BODY_MAX, body);
    if (err != NULL) {
        json_error(res, 400, err);
        return -1;
    }
    return 0;
}

void notes_list(struct request *req, struct response *res)
{
    (void)req;
    sqlite3_stmt *st = db_prepare("SELECT " NOTE_COLUMNS " FROM notes "
                                  "ORDER BY updated_at DESC, id DESC");
    if (st == NULL) {
        json_error(res, 500, "database error");
        return;
    }
    cJSON *list = cJSON_CreateArray();
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        cJSON_AddItemToArray(list, note_from_row(st));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        db_log_error("notes_list");
        json_error(res, 500, "database error");
        return;
    }
    json_reply(res, 200, list);
}

void notes_create(struct request *req, struct response *res)
{
    const char *title, *body;
    if (read_note(req, res, &title, &body) != 0)
        return;
    sqlite3_stmt *st = db_prepare("INSERT INTO notes (title, body) VALUES (?, ?) "
                                  "RETURNING " NOTE_COLUMNS);
    if (st == NULL) {
        json_error(res, 500, "database error");
        return;
    }
    sqlite3_bind_text(st, 1, title, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, body, -1, SQLITE_STATIC);
    reply_one(st, 201, res);
}

void notes_get(struct request *req, struct response *res)
{
    sqlite3_int64 id = parse_id(req->param);
    if (id == 0) {
        json_error(res, 404, "note not found");
        return;
    }
    sqlite3_stmt *st = db_prepare("SELECT " NOTE_COLUMNS " FROM notes WHERE id = ?");
    if (st == NULL) {
        json_error(res, 500, "database error");
        return;
    }
    sqlite3_bind_int64(st, 1, id);
    reply_one(st, 200, res);
}

void notes_update(struct request *req, struct response *res)
{
    sqlite3_int64 id = parse_id(req->param);
    if (id == 0) {
        json_error(res, 404, "note not found");
        return;
    }
    const char *title, *body;
    if (read_note(req, res, &title, &body) != 0)
        return;
    sqlite3_stmt *st = db_prepare("UPDATE notes SET title = ?, body = ?, "
                                  "updated_at = unixepoch() WHERE id = ? "
                                  "RETURNING " NOTE_COLUMNS);
    if (st == NULL) {
        json_error(res, 500, "database error");
        return;
    }
    sqlite3_bind_text(st, 1, title, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, body, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, id);
    reply_one(st, 200, res);
}

void notes_delete(struct request *req, struct response *res)
{
    sqlite3_int64 id = parse_id(req->param);
    if (id == 0) {
        json_error(res, 404, "note not found");
        return;
    }
    sqlite3_stmt *st = db_prepare("DELETE FROM notes WHERE id = ?");
    if (st == NULL) {
        json_error(res, 500, "database error");
        return;
    }
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        db_log_error("notes_delete");
        json_error(res, 500, "database error");
    } else if (sqlite3_changes(db) == 0) {
        json_error(res, 404, "note not found");
    } else {
        res->status = 204;
    }
}
