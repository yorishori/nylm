#include "arena.h"

#include <stdlib.h>
#include <string.h>

static char *base;
static size_t cap;
static size_t used;

int arena_init(size_t capacity)
{
    base = malloc(capacity);
    if (base == NULL)
        return -1;
    cap = capacity;
    used = 0;
    return 0;
}

void *arena_alloc(size_t size)
{
    /* Keep every allocation aligned for any type. */
    size_t align = _Alignof(max_align_t);
    size_t start = (used + align - 1) & ~(align - 1);
    if (start > cap || size > cap - start)
        return NULL;
    used = start + size;
    return base + start;
}

char *arena_strndup(const char *s, size_t len)
{
    char *p = arena_alloc(len + 1);
    if (p == NULL)
        return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

void arena_reset(void)
{
    used = 0;
}
