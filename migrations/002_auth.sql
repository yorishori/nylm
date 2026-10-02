-- The single user's password (Argon2id) and active login sessions.
CREATE TABLE user (
    id         INTEGER PRIMARY KEY CHECK (id = 1),
    salt       BLOB    NOT NULL,
    hash       BLOB    NOT NULL,
    memcost    INTEGER NOT NULL, -- KiB
    iterations INTEGER NOT NULL,
    updated_at INTEGER NOT NULL DEFAULT (unixepoch())
) STRICT;

-- Only the SHA-256 of each token is stored, never the token itself.
CREATE TABLE sessions (
    token_hash BLOB    PRIMARY KEY,
    expires_at INTEGER NOT NULL
) STRICT, WITHOUT ROWID;
