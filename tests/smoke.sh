#!/bin/sh
# End-to-end checks against a running server: the API and static files, then
# listen addresses and the client allowlist.
# Usage: tests/smoke.sh [binary]   (default: ./nylm-debug, which has sanitizers)
set -u

BIN=${1:-./nylm-debug}
PORT=18080
TMP=$(mktemp -d)
PID=
FAILED=0
PASSED=0

cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

export NYLM_DB="$TMP/nylm.db" NYLM_PUBLIC=public NYLM_PORT=$PORT

# expect STATUS DESCRIPTION curl-args...
expect() {
    want=$1 desc=$2
    shift 2
    got=$(curl -s -o "$TMP/body" -w '%{http_code}' --max-time 10 "$@")
    if [ "$got" = "$want" ]; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
        echo "FAIL: $desc: want $want, got $got ($(head -c 200 "$TMP/body"))"
    fi
}

# expect_header PATTERN DESCRIPTION curl-args...
expect_header() {
    pattern=$1 desc=$2
    shift 2
    if curl -s -D - -o /dev/null --max-time 10 "$@" | grep -qi "$pattern"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
        echo "FAIL: $desc: no header matching '$pattern'"
    fi
}

# start URL [curl-args...]: runs the server, waits until URL answers.
start() {
    url=$1
    shift
    "$BIN" >"$TMP/log" 2>&1 &
    PID=$!
    i=0
    while ! curl -s -o /dev/null "$@" "$url" 2>/dev/null; do
        i=$((i + 1))
        if [ $i -gt 100 ]; then
            echo "server did not start:"; cat "$TMP/log"; exit 1
        fi
        sleep 0.05
    done
}

stop() {
    kill "$PID" && wait "$PID"
    PID=
    if grep -q "AddressSanitizer\|runtime error\|LeakSanitizer" "$TMP/log"; then
        FAILED=$((FAILED + 1))
        echo "FAIL: sanitizer report:"; cat "$TMP/log"
    fi
}

# ---- setup ---------------------------------------------------------------

echo 'smoke test password' | "$BIN" set-password 2>/dev/null ||
    { echo "set-password failed"; exit 1; }

# ---- API and static files (defaults: 127.0.0.1, allow 127.0.0.0/8) ---------

start "http://127.0.0.1:$PORT/"
B="http://127.0.0.1:$PORT"
J="Content-Type: application/json"
JAR="$TMP/cookies"

expect 200 "index"              "$B/"
expect 200 "app.js"             "$B/app.js"
expect 404 "missing file"       "$B/nope.html"
expect 404 "traversal"          --path-as-is "$B/../scope.md"
expect 404 "encoded traversal"  --path-as-is "$B/%2e%2e/scope.md"
expect 404 "hidden file"        "$B/.git/config"
expect 405 "POST to static"     -X POST "$B/"
expect 501 "unknown method"     -X PATCH "$B/"
expect 400 "bad header"         -H "Bad Name: x" "$B/"
expect 413 "body too large"     -H "$J" -H "Content-Length: 2000000" -X POST "$B/api/login"
expect_header "content-security-policy: default-src 'self'" "CSP header" "$B/"
expect_header "x-content-type-options: nosniff"             "nosniff header" "$B/"

expect 200 "health"             "$B/api/health"
expect 404 "unknown api"        "$B/api/nope"
expect 401 "session needs login" "$B/api/session"
expect 401 "forged cookie"      -b "nylm_session=$(printf '%064d' 0)" "$B/api/session"
expect 401 "malformed cookie"   -b "nylm_session=xyz" "$B/api/session"
expect 401 "wrong password"     -H "$J" -d '{"password":"nope"}' "$B/api/login"
expect 415 "login needs json"   -d '{"password":"smoke test password"}' "$B/api/login"
expect 400 "login invalid json" -H "$J" -d '{"password":' "$B/api/login"
expect 400 "login not object"   -H "$J" -d '["x"]' "$B/api/login"
expect 400 "login no password"  -H "$J" -d '{}' "$B/api/login"
expect 400 "password not string" -H "$J" -d '{"password":1}' "$B/api/login"
expect 405 "wrong method"       -X GET "$B/api/login"
expect 204 "login"              -c "$JAR" -H "$J" -d '{"password":"smoke test password"}' "$B/api/login"
expect_header "set-cookie: nylm_session=.*HttpOnly; SameSite=Strict" "cookie flags" \
    -H "$J" -d '{"password":"smoke test password"}' "$B/api/login"
expect 204 "session"            -b "$JAR" "$B/api/session"

expect 204 "logout"             -b "$JAR" -c "$JAR" -X POST "$B/api/logout"
expect 401 "after logout"       -b "$JAR" "$B/api/session"

stop

# ---- listen addresses and client allowlist ----------------------------------

# Two loopback addresses stand in for the WireGuard and LAN interfaces.
export NYLM_LISTEN="127.0.0.1 127.0.0.2" NYLM_ALLOW=127.0.0.2/32
start "http://127.0.0.2:$PORT/" --interface 127.0.0.2

expect 200 "second listen address"     --interface 127.0.0.2 "http://127.0.0.2:$PORT/api/health"
expect 000 "client outside NYLM_ALLOW" --interface 127.0.0.1 "http://127.0.0.1:$PORT/api/health"
expect 000 "address not listened on"   "http://127.0.0.3:$PORT/api/health"
if grep -q "127.0.0.1 rejected" "$TMP/log"; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: rejection not logged"; fi

stop

echo "smoke: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
