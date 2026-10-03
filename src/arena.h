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

/* For scratch work inside a request: arena_rewind(arena_mark()) frees what
 * was allocated since the mark (nothing allocated since may still be used). */
size_t arena_mark(void);
void arena_rewind(size_t mark);

#endif
