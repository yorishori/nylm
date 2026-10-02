#include "tls.h"

#include <stdio.h>

#include <openssl/err.h>

static SSL_CTX *ctx;

static void log_openssl_errors(const char *context)
{
    unsigned long err;
    while ((err = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof buf);
        fprintf(stderr, "tls: %s: %s\n", context, buf);
    }
}

int tls_init(const char *cert_path, const char *key_path)
{
    ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        log_openssl_errors("SSL_CTX_new");
        return -1;
    }

    /* TLS 1.2+ with OpenSSL's default ciphers; no renegotiation. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
        log_openssl_errors(cert_path);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        log_openssl_errors(key_path);
        return -1;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        log_openssl_errors("certificate and key do not match");
        return -1;
    }
    return 0;
}

int tls_accept(struct conn *c)
{
    c->ssl = SSL_new(ctx);
    if (c->ssl == NULL || SSL_set_fd(c->ssl, c->fd) != 1) {
        log_openssl_errors("SSL_new");
        return -1;
    }
    /* Bounded by the socket timeouts set in conn_init(). */
    if (SSL_accept(c->ssl) != 1) {
        ERR_clear_error(); /* scanners and bad clients: not worth a log line each */
        SSL_free(c->ssl);
        c->ssl = NULL;
        return -1;
    }
    return 0;
}
