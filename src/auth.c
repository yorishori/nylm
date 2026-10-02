#define _POSIX_C_SOURCE 200809L

#include "auth.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include "arena.h"
#include "db.h"

#define SALT_LEN  16
#define HASH_LEN  32
#define TOKEN_LEN 32 /* random bytes; the cookie carries them as hex */

/* Argon2id cost for new passwords (OWASP minimum: 19 MiB, 2 passes). */
#define ARGON2_MEMCOST_KIB 19456
#define ARGON2_ITERATIONS  2

#define SESSION_SECONDS (7 * 24 * 60 * 60)

static int argon2id(const char *password, const unsigned char *salt, uint32_t memcost,
                    uint32_t iterations, unsigned char out[HASH_LEN])
{
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "ARGON2ID", NULL);
    EVP_KDF_CTX *ctx = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
    EVP_KDF_free(kdf);
    if (ctx == NULL) {
        fprintf(stderr, "auth: Argon2id not available (needs OpenSSL 3.2+)\n");
        return -1;
    }

    uint32_t one = 1;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, (void *)password,
                                          strlen(password)),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, SALT_LEN),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memcost),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &one),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &one),
        OSSL_PARAM_construct_end(),
    };
    int ok = EVP_KDF_derive(ctx, out, HASH_LEN, params) == 1;
    EVP_KDF_CTX_free(ctx);
    return ok ? 0 : -1;
}

static int sha256(const void *data, size_t len, unsigned char out[32])
{
    return EVP_Digest(data, len, out, NULL, EVP_sha256(), NULL) == 1 ? 0 : -1;
}

/* Stores the password row. Runs inside auth_set_password's transaction. */
static int store_password(const unsigned char *salt, const unsigned char *hash)
{
    sqlite3_stmt *st = db_prepare(
        "INSERT INTO user (id, salt, hash, memcost, iterations) VALUES (1, ?, ?, ?, ?) "
        "ON CONFLICT (id) DO UPDATE SET salt = excluded.salt, hash = excluded.hash, "
        "memcost = excluded.memcost, iterations = excluded.iterations, "
        "updated_at = unixepoch()");
    if (st == NULL)
        return -1;
    int rc = sqlite3_bind_blob(st, 1, salt, SALT_LEN, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_blob(st, 2, hash, HASH_LEN, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(st, 3, ARGON2_MEMCOST_KIB);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(st, 4, ARGON2_ITERATIONS);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        db_log_error("store_password");
        return -1;
    }
    return 0;
}

int auth_set_password(const char *password)
{
    unsigned char salt[SALT_LEN], hash[HASH_LEN];
    if (RAND_bytes(salt, sizeof salt) != 1 ||
        argon2id(password, salt, ARGON2_MEMCOST_KIB, ARGON2_ITERATIONS, hash) != 0)
        return -1;

    /* New password and logging out every session happen together or not at all. */
    if (db_exec("BEGIN IMMEDIATE") != 0)
        return -1;
    if (store_password(salt, hash) != 0 || db_exec("DELETE FROM sessions") != 0 ||
        db_exec("COMMIT") != 0) {
        db_exec("ROLLBACK");
        return -1;
    }
    return 0;
}

int auth_check_password(const char *password)
{
    sqlite3_stmt *st = db_prepare("SELECT salt, hash, memcost, iterations FROM user "
                                  "WHERE id = 1");
    if (st == NULL)
        return -1;
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        if (rc == SQLITE_DONE) {
            fprintf(stderr, "auth: no password set; run `nylm set-password`\n");
            return 0;
        }
        db_log_error("auth_check_password");
        return -1;
    }

    unsigned char salt[SALT_LEN], stored[HASH_LEN], computed[HASH_LEN];
    if (sqlite3_column_bytes(st, 0) != SALT_LEN || sqlite3_column_bytes(st, 1) != HASH_LEN) {
        sqlite3_finalize(st);
        fprintf(stderr, "auth: stored password hash is malformed\n");
        return -1;
    }
    memcpy(salt, sqlite3_column_blob(st, 0), SALT_LEN);
    memcpy(stored, sqlite3_column_blob(st, 1), HASH_LEN);
    uint32_t memcost = (uint32_t)sqlite3_column_int(st, 2);
    uint32_t iterations = (uint32_t)sqlite3_column_int(st, 3);
    sqlite3_finalize(st);

    if (argon2id(password, salt, memcost, iterations, computed) != 0)
        return -1;
    return CRYPTO_memcmp(stored, computed, HASH_LEN) == 0;
}

static void to_hex(const unsigned char *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

static int from_hex(const char *in, size_t in_len, unsigned char *out, size_t out_len)
{
    if (in_len != 2 * out_len)
        return -1;
    for (size_t i = 0; i < out_len; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = in[2 * i + (size_t)k];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0)
                return -1;
            v = v * 16 + d;
        }
        out[i] = (unsigned char)v;
    }
    return 0;
}

int auth_cookie_value(const char *header, const char *name, const char **value)
{
    size_t name_len = strlen(name);
    const char *p = header;
    while (*p != '\0') {
        while (*p == ' ' || *p == ';')
            p++;
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            *value = p + name_len + 1;
            return (int)(len - name_len - 1);
        }
        p += len;
    }
    return -1;
}

/* SHA-256 of the request's session token; -1 if there is no well-formed one. */
static int request_token_hash(const struct request *req, unsigned char hash[32])
{
    const char *cookie = http_header(req, "Cookie");
    const char *value;
    int len = cookie ? auth_cookie_value(cookie, AUTH_COOKIE, &value) : -1;
    unsigned char token[TOKEN_LEN];
    if (len < 0 || from_hex(value, (size_t)len, token, sizeof token) != 0)
        return -1;
    return sha256(token, sizeof token, hash);
}

int auth_start_session(struct response *res)
{
    unsigned char token[TOKEN_LEN], hash[32];
    if (RAND_bytes(token, sizeof token) != 1 || sha256(token, sizeof token, hash) != 0)
        return -1;

    /* Housekeeping: drop expired sessions whenever someone logs in. A failure
     * here is logged (by db_exec) but does not block the login. */
    db_exec("DELETE FROM sessions WHERE expires_at <= unixepoch()");

    sqlite3_stmt *st = db_prepare("INSERT INTO sessions (token_hash, expires_at) "
                                  "VALUES (?, unixepoch() + ?)");
    if (st == NULL)
        return -1;
    int rc = sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(st, 2, SESSION_SECONDS);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        db_log_error("auth_start_session");
        return -1;
    }

    char hex[2 * TOKEN_LEN + 1];
    to_hex(token, sizeof token, hex);
    char *cookie = arena_alloc(256);
    if (cookie == NULL)
        return -1;
    /* No Secure attribute: nylm is plain HTTP, reachable only via LAN/WireGuard. */
    snprintf(cookie, 256, "%s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict",
             AUTH_COOKIE, hex, SESSION_SECONDS);
    return http_add_header(res, "Set-Cookie", cookie);
}

int auth_end_session(const struct request *req, struct response *res)
{
    unsigned char hash[32];
    if (request_token_hash(req, hash) == 0) {
        sqlite3_stmt *st = db_prepare("DELETE FROM sessions WHERE token_hash = ?");
        if (st == NULL)
            return -1;
        int rc = sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
        if (rc == SQLITE_OK)
            rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            db_log_error("auth_end_session");
            return -1;
        }
    }
    return http_add_header(res, "Set-Cookie",
                           AUTH_COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
}

int auth_session_valid(const struct request *req)
{
    unsigned char hash[32];
    if (request_token_hash(req, hash) != 0)
        return 0;
    sqlite3_stmt *st = db_prepare("SELECT 1 FROM sessions "
                                  "WHERE token_hash = ? AND expires_at > unixepoch()");
    if (st == NULL)
        return -1;
    int rc = sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW)
        return 1;
    if (rc == SQLITE_DONE)
        return 0;
    db_log_error("auth_session_valid");
    return -1;
}
