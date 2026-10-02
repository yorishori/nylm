#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>

#include "http.h"

#define AUTH_MIN_PASSWORD 8
#define AUTH_MAX_PASSWORD 1024
#define AUTH_COOKIE "nylm_session"

/* Stores a new password and logs out every existing session. */
int auth_set_password(const char *password);

/* 1 if password matches, 0 if not (or no password set), -1 on error. */
int auth_check_password(const char *password);

/* Creates a session and adds its Set-Cookie header to res. 0 or -1. */
int auth_start_session(struct response *res);

/*
 * Deletes the request's session (if it has one) and adds a header clearing
 * the cookie. Returns -1 if the session could not be deleted.
 */
int auth_end_session(const struct request *req, struct response *res);

/* 1 if the request has a valid, unexpired session; 0 if not; -1 on error. */
int auth_session_valid(const struct request *req);

/* Finds name=value in a Cookie header. Returns value length, -1 if absent. */
int auth_cookie_value(const char *header, const char *name, const char **value);

#endif
