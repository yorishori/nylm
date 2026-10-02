#!/bin/sh
# End-to-end checks against a running server: plain HTTP mode, then TLS mode.
# Usage: tests/smoke.sh [binary]   (default: ./nylm-debug, which has sanitizers)
set -u

BIN=${1:-./nylm-debug}
HTTP_PORT=18080
HTTPS_PORT=18443
TMP=$(mktemp -d)
PID=
FAILED=0
PASSED=0

cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

export NYLM_DB="$TMP/nylm.db" NYLM_PUBLIC=public NYLM_ACME_DIR="$TMP/acme"
export NYLM_HTTP_PORT=$HTTP_PORT NYLM_HTTPS_PORT=$HTTPS_PORT NYLM_PUBLIC_HTTPS_PORT=$HTTPS_PORT

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

start() {
    "$BIN" >"$TMP/log" 2>&1 &
    PID=$!
    i=0
    while ! curl -sk -o /dev/null "$1" 2>/dev/null; do
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

# ---- plain HTTP mode -------------------------------------------------------

export NYLM_TLS=off
start "http://localhost:$HTTP_PORT/"
B="http://localhost:$HTTP_PORT"
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
expect 413 "body too large"     -H "$J" -H "Content-Length: 2000000" -X POST "$B/api/notes"
expect_header "content-security-policy: default-src 'self'" "CSP header" "$B/"
expect_header "x-content-type-options: nosniff"             "nosniff header" "$B/"

expect 200 "health"             "$B/api/health"
expect 404 "unknown api"        "$B/api/nope"
expect 401 "notes need login"   "$B/api/notes"
expect 401 "forged cookie"      -b "nylm_session=$(printf '%064d' 0)" "$B/api/notes"
expect 401 "wrong password"     -H "$J" -d '{"password":"nope"}' "$B/api/login"
expect 415 "login needs json"   -d '{"password":"smoke test password"}' "$B/api/login"
expect 204 "login"              -c "$JAR" -H "$J" -d '{"password":"smoke test password"}' "$B/api/login"
expect_header "set-cookie: nylm_session=.*HttpOnly; SameSite=Strict" "cookie flags" \
    -H "$J" -d '{"password":"smoke test password"}' "$B/api/login"
expect 204 "session"            -b "$JAR" "$B/api/session"

expect 200 "list (empty)"       -b "$JAR" "$B/api/notes"
expect 201 "create"             -b "$JAR" -H "$J" -d '{"title":"one","body":"first"}' "$B/api/notes"
expect 200 "get"                -b "$JAR" "$B/api/notes/1"
expect 200 "update"             -b "$JAR" -H "$J" -X PUT -d '{"title":"uno","body":""}' "$B/api/notes/1"
expect 400 "missing title"      -b "$JAR" -H "$J" -d '{"body":"x"}' "$B/api/notes"
expect 400 "title not string"   -b "$JAR" -H "$J" -d '{"title":1,"body":""}' "$B/api/notes"
expect 400 "invalid json"       -b "$JAR" -H "$J" -d '{"title":' "$B/api/notes"
expect 404 "bad id"             -b "$JAR" "$B/api/notes/abc"
expect 405 "wrong method"       -b "$JAR" -X PUT -H "$J" -d '{}' "$B/api/notes"
expect 204 "delete"             -b "$JAR" -X DELETE "$B/api/notes/1"
expect 404 "deleted"            -b "$JAR" "$B/api/notes/1"
expect 204 "logout"             -b "$JAR" -c "$JAR" -X POST "$B/api/logout"
expect 401 "after logout"       -b "$JAR" "$B/api/notes"

stop

# ---- TLS mode ---------------------------------------------------------------

export NYLM_TLS=on NYLM_CERT="$TMP/cert.pem" NYLM_KEY="$TMP/key.pem"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 \
    -subj /CN=localhost -addext subjectAltName=DNS:localhost \
    -keyout "$NYLM_KEY" -out "$NYLM_CERT" 2>/dev/null
mkdir -p "$TMP/acme/.well-known/acme-challenge"
echo token-content > "$TMP/acme/.well-known/acme-challenge/abc_DEF-123"

start "https://localhost:$HTTPS_PORT/"
S="https://localhost:$HTTPS_PORT"
H="http://localhost:$HTTP_PORT"
CA="--cacert $NYLM_CERT"

expect 200 "https index"        $CA "$S/"
expect 200 "https health"       $CA "$S/api/health"
expect_header "strict-transport-security" "HSTS" $CA "$S/"
expect_header "set-cookie: .*; Secure" "Secure cookie" $CA -H "$J" \
    -d '{"password":"smoke test password"}' "$S/api/login"
expect 000 "TLS 1.1 refused"    $CA --tlsv1.1 --tls-max 1.1 "$S/"
expect 301 "http redirects"     "$H/some/page"
expect_header "location: https://localhost:$HTTPS_PORT/some/page" "redirect target" "$H/some/page"
expect 200 "acme challenge"     "$H/.well-known/acme-challenge/abc_DEF-123"
expect 404 "acme traversal"     --path-as-is "$H/.well-known/acme-challenge/../../x"
expect 400 "bad Host"           -H "Host: a/b" "$H/"

stop

echo "smoke: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
