#ifndef TLS_H
#define TLS_H

#include "conn.h"

/* Loads certificate chain and key (PEM). Call once at startup. */
int tls_init(const char *cert_path, const char *key_path);

/* Runs the server-side handshake on c. Returns 0 on success. */
int tls_accept(struct conn *c);

#endif
