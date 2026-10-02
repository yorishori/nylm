#ifndef STATIC_H
#define STATIC_H

#include <stddef.h>

#include "http.h"

/*
 * Turns a decoded URL path into a path relative to the public root.
 * Rejects "..", hidden segments (".x"), backslashes and empty paths;
 * "/" and "/dir/" map to index.html. Returns 0 if safe, -1 otherwise.
 */
int static_safe_path(const char *url_path, char *out, size_t out_len);

const char *static_content_type(const char *path);

/* Serves url_path from the directory root into res (404 if missing). */
void static_serve(const char *root, const char *url_path, struct response *res);

#endif
