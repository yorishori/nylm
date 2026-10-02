#ifndef SERVER_H
#define SERVER_H

struct server_config {
    const char *public_dir; /* static files */
    int tls;                /* 0: app on http_port; 1: app on https_port */
    int http_port;
    int https_port;
    /* TLS only: port 80 side */
    const char *acme_dir;   /* certbot --webroot directory */
    int public_https_port;  /* port used in redirects (443 behind Docker) */
};

/* Serves until SIGTERM/SIGINT (returns 0); -1 on startup failure. */
int server_run(const struct server_config *cfg);

/* Builds the https:// redirect target, or NULL if Host is unusable. */
char *server_redirect_location(const char *host, const char *path, const char *query,
                               int https_port);

#endif
