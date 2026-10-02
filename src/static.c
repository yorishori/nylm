#define _POSIX_C_SOURCE 200809L

#include "static.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int static_safe_path(const char *url_path, char *out, size_t out_len)
{
    if (url_path[0] != '/' || strchr(url_path, '\\') != NULL)
        return -1;

    /* Every segment must be non-empty and must not start with a dot. */
    const char *seg = url_path + 1;
    while (*seg != '\0') {
        const char *slash = strchr(seg, '/');
        size_t seg_len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (seg_len == 0 || seg[0] == '.')
            return -1;
        if (slash == NULL)
            break;
        seg = slash + 1;
    }

    size_t len = strlen(url_path);
    const char *index = url_path[len - 1] == '/' ? "index.html" : "";
    int n = snprintf(out, out_len, "%s%s", url_path + 1, index);
    if (n < 0 || (size_t)n >= out_len)
        return -1;
    return 0;
}

const char *static_content_type(const char *path)
{
    static const struct { const char *ext, *type; } types[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".css",  "text/css; charset=utf-8" },
        { ".js",   "text/javascript; charset=utf-8" },
        { ".json", "application/json" },
        { ".svg",  "image/svg+xml" },
        { ".png",  "image/png" },
        { ".ico",  "image/x-icon" },
        { ".txt",  "text/plain; charset=utf-8" },
    };
    const char *dot = strrchr(path, '.');
    if (dot != NULL)
        for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
            if (strcmp(dot, types[i].ext) == 0)
                return types[i].type;
    return "application/octet-stream";
}

void static_serve(const char *root, const char *url_path, struct response *res)
{
    char rel[HTTP_MAX_PATH + 16];
    if (static_safe_path(url_path, rel, sizeof rel) != 0) {
        http_text(res, 404, "404 Not Found\n");
        return;
    }

    int dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        http_text(res, 500, "500 Internal Server Error\n");
        return;
    }
    int fd = openat(dir, rel, O_RDONLY | O_CLOEXEC);
    close(dir);

    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        http_text(res, 404, "404 Not Found\n");
        return;
    }

    res->status = 200;
    res->content_type = static_content_type(rel);
    res->cache_control = "no-cache";
    res->file_fd = fd;
    res->file_size = (size_t)st.st_size;
}
