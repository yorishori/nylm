#ifndef JSON_H
#define JSON_H

#include <cJSON.h>

#include "http.h"

/* Routes all cJSON allocations through the per-request arena. */
void json_init(void);

/* Serialises obj as the response body. */
void json_reply(struct response *res, int status, const cJSON *obj);

/* Replies {"error": msg}. */
void json_error(struct response *res, int status, const char *msg);

/*
 * Parses the request body as a JSON object. On failure sets an error
 * response (415 wrong content type, 400 invalid JSON) and returns NULL.
 */
cJSON *json_body(const struct request *req, struct response *res);

/*
 * Reads obj[key] as a string of min..max bytes into *out. Returns NULL on
 * success or an error message suitable for json_error().
 */
const char *json_get_string(const cJSON *obj, const char *key, size_t min, size_t max,
                            const char **out);

#endif
