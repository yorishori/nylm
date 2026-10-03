#ifndef API_H
#define API_H

#include "http.h"

/* Handlers, one api_*.c file per feature. Routes live in router.c. */

/* api_session.c */
void session_login(struct request *req, struct response *res);
void session_logout(struct request *req, struct response *res);
void session_check(struct request *req, struct response *res);

/* api_plants.c */
void plants_list(struct request *req, struct response *res);
void plants_due(struct request *req, struct response *res);
void plants_get(struct request *req, struct response *res);
void plants_add(struct request *req, struct response *res);
void plants_update(struct request *req, struct response *res);
void plants_archive(struct request *req, struct response *res);
void plants_type_add(struct request *req, struct response *res);
void plants_type_update(struct request *req, struct response *res);
void plants_type_archive(struct request *req, struct response *res);
void plants_rule_save(struct request *req, struct response *res);
void plants_rule_delete(struct request *req, struct response *res);
void plants_log(struct request *req, struct response *res);
void plants_log_add(struct request *req, struct response *res);
void plants_log_update(struct request *req, struct response *res);
void plants_log_delete(struct request *req, struct response *res);

/* api_music.c */
void music_overview(struct request *req, struct response *res);
void music_albums(struct request *req, struct response *res);
void music_album(struct request *req, struct response *res);
void music_album_save(struct request *req, struct response *res);
void music_scan_start(struct request *req, struct response *res);

#endif
