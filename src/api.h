#ifndef API_H
#define API_H

#include "http.h"

/* Handlers, one api_*.c file per feature. Routes live in router.c. */

/* api_session.c */
void session_login(struct request *req, struct response *res);
void session_logout(struct request *req, struct response *res);
void session_check(struct request *req, struct response *res);

/* api_notes.c */
void notes_list(struct request *req, struct response *res);
void notes_create(struct request *req, struct response *res);
void notes_get(struct request *req, struct response *res);
void notes_update(struct request *req, struct response *res);
void notes_delete(struct request *req, struct response *res);

#endif
