#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/*
 * Per-request bump allocator. One arena for the whole process (the server
 * is single-threaded); arena_reset() at the end of every request frees
 * everything allocated during it.
 */

int arena_init(size_t capacity);
void *arena_alloc(size_t size); /* NULL when the arena is full */
char *arena_strndup(const char *s, size_t len);
void arena_reset(void);

#endif
