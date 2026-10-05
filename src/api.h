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
void plants_photo(struct request *req, struct response *res);
void plants_photo_add(struct request *req, struct response *res);
void plants_photo_delete(struct request *req, struct response *res);

/* api_music.c */
void music_overview(struct request *req, struct response *res);
void music_albums(struct request *req, struct response *res);
void music_album(struct request *req, struct response *res);
void music_values(struct request *req, struct response *res);
void music_charts(struct request *req, struct response *res);
void music_changes(struct request *req, struct response *res);
void music_art(struct request *req, struct response *res);
void music_queue(struct request *req, struct response *res);
void music_duplicates(struct request *req, struct response *res);
void music_merge(struct request *req, struct response *res);
void music_names(struct request *req, struct response *res);
void music_rename(struct request *req, struct response *res);
void music_remove(struct request *req, struct response *res);
void music_fix_split(struct request *req, struct response *res);
void music_fix_composers(struct request *req, struct response *res);
void music_musicbrainz(struct request *req, struct response *res);
void music_musicbrainz_start(struct request *req, struct response *res);
void music_cover(struct request *req, struct response *res);
void music_discard(struct request *req, struct response *res);
void music_scan_start(struct request *req, struct response *res);
void music_write_start(struct request *req, struct response *res);
void music_moves(struct request *req, struct response *res);
void music_move_start(struct request *req, struct response *res);
void music_qobuz(struct request *req, struct response *res);
void music_qobuz_start(struct request *req, struct response *res);

/* api_server.c */
void server_overview(struct request *req, struct response *res);
void server_audit(struct request *req, struct response *res);
void server_log(struct request *req, struct response *res);
void server_disk_usage(struct request *req, struct response *res);
void server_disk_usage_start(struct request *req, struct response *res);
void server_smart(struct request *req, struct response *res);
void server_units(struct request *req, struct response *res);
void server_updates(struct request *req, struct response *res);
void server_updates_check(struct request *req, struct response *res);
void server_update(struct request *req, struct response *res);
void server_reboot(struct request *req, struct response *res);
void server_backups(struct request *req, struct response *res);
void server_backup_start(struct request *req, struct response *res);
void server_ports(struct request *req, struct response *res);
void server_wireguard(struct request *req, struct response *res);
void server_wireguard_name(struct request *req, struct response *res);
void server_containers(struct request *req, struct response *res);
void server_container_log(struct request *req, struct response *res);
void server_container_restart(struct request *req, struct response *res);
/* Checks the server app's settings (NYLM_UNITS, NYLM_BACKUP...) once at
 * start; -1 (logged) if any is invalid. */
int server_configure(void);

#endif
