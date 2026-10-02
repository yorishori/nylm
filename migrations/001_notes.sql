-- Example feature table, used to exercise the stack end to end.
CREATE TABLE notes (
    id         INTEGER PRIMARY KEY,
    title      TEXT    NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
    body       TEXT    NOT NULL DEFAULT '' CHECK (length(body) <= 10000),
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    updated_at INTEGER NOT NULL DEFAULT (unixepoch())
) STRICT;
