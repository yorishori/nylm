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

# expect_body TEXT DESCRIPTION: the last response body contains TEXT.
expect_body() {
    if grep -qF -- "$1" "$TMP/body"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
        echo "FAIL: $2: body lacks '$1' ($(head -c 300 "$TMP/body"))"
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
expect 400 "escaped NUL"        -H "$J" -d '{"password":"smoke\u0000x"}' "$B/api/login"
printf '{"password":"smoke\000x"}' > "$TMP/nul.json"
expect 400 "raw NUL byte"       -H "$J" --data-binary "@$TMP/nul.json" "$B/api/login"
expect 204 "login"              -c "$JAR" -H "$J" -d '{"password":"smoke test password"}' "$B/api/login"
expect_header "set-cookie: nylm_session=.*HttpOnly; SameSite=Strict" "cookie flags" \
    -H "$J" -d '{"password":"smoke test password"}' "$B/api/login"
expect 204 "session"            -b "$JAR" "$B/api/session"

# ---- plants ------------------------------------------------------------------

# post DESCRIPTION STATUS PATH JSON: a logged-in JSON POST.
post() {
    expect "$2" "$1" -b "$JAR" -H "$J" -d "$4" "$B$3"
}
TODAY=$(date +%F)
TOMORROW=$(date -d tomorrow +%F)
IN4=$(date -d '+4 days' +%F)
LONG101=$(printf '%0101d' 0 | tr 0 a)
LONG4001=$(printf '%04001d' 0 | tr 0 a)

for route in "GET /api/plants" "GET /api/plants/due" "GET /api/plants/plant?id=1" \
             "GET /api/plants/log?plant_id=1" \
             "POST /api/plants/add" "POST /api/plants/update" "POST /api/plants/archive" \
             "POST /api/plants/types/add" "POST /api/plants/types/update" \
             "POST /api/plants/types/archive" "POST /api/plants/rules/save" \
             "POST /api/plants/rules/delete" "POST /api/plants/log/add" \
             "POST /api/plants/log/update" "POST /api/plants/log/delete"; do
    expect 401 "${route#* } needs login" -X "${route%% *}" -H "$J" "$B${route#* }"
done

# plants
P='"species":"Monstera deliciosa","location":"living room","acquired":null,"notes":"line 1\nline 2"'
post "plant add"               201 /api/plants/add "{\"name\":\"Monty\",$P}"
expect_body '"id":1' "plant add returns id"
post "plant add (2)"           201 /api/plants/add "{\"name\":\"Fern\",$P}"
post "plant add utf-8"         201 /api/plants/add "{\"name\":\"Piléa 🌱\",$P}"
expect 415 "plant add needs json" -b "$JAR" -d "{\"name\":\"x\",$P}" "$B/api/plants/add"
post "plant add invalid json"  400 /api/plants/add '{"name":'
post "plant add no name"       400 /api/plants/add "{$P}"
expect_body "'name' must be a string" "plant add no name message"
post "plant add empty name"    400 /api/plants/add "{\"name\":\"\",$P}"
post "plant add long name"     400 /api/plants/add "{\"name\":\"$LONG101\",$P}"
post "plant add name newline"  400 /api/plants/add "{\"name\":\"a\nb\",$P}"
post "plant add bad utf-8"     400 /api/plants/add "{\"name\":\"\ud800\",$P}"
post "plant add species type"  400 /api/plants/add '{"name":"x","species":1,"location":"","acquired":null,"notes":""}'
post "plant add no location"   400 /api/plants/add '{"name":"x","species":"","acquired":null,"notes":""}'
post "plant add notes long"    400 /api/plants/add "{\"name\":\"x\",\"species\":\"\",\"location\":\"\",\"acquired\":null,\"notes\":\"$LONG4001\"}"
post "plant add notes tab"     400 /api/plants/add '{"name":"x","species":"","location":"","acquired":null,"notes":"a\tb"}'
post "plant add acquired"      201 /api/plants/add "{\"name\":\"Cactus\",\"species\":\"\",\"location\":\"\",\"acquired\":\"$TODAY\",\"notes\":\"\"}"
post "plant add future date"   400 /api/plants/add "{\"name\":\"x\",\"species\":\"\",\"location\":\"\",\"acquired\":\"$TOMORROW\",\"notes\":\"\"}"
post "plant add bad date"      400 /api/plants/add '{"name":"x","species":"","location":"","acquired":"2024-02-30","notes":""}'
post "plant add date number"   400 /api/plants/add '{"name":"x","species":"","location":"","acquired":20240101,"notes":""}'

post "plant update"            204 /api/plants/update "{\"id\":1,\"name\":\"Monty II\",$P}"
post "plant update missing"    404 /api/plants/update "{\"id\":999,\"name\":\"x\",$P}"
post "plant update id 0"       400 /api/plants/update "{\"id\":0,\"name\":\"x\",$P}"
post "plant update id string"  400 /api/plants/update "{\"id\":\"1\",\"name\":\"x\",$P}"
post "plant update id float"   400 /api/plants/update "{\"id\":1.5,\"name\":\"x\",$P}"
post "plant update bad field"  400 /api/plants/update '{"id":1,"name":""}'

post "plant archive"           204 /api/plants/archive '{"id":2,"archived":true}'
post "plant archive missing"   404 /api/plants/archive '{"id":999,"archived":true}'
post "plant archive not bool"  400 /api/plants/archive '{"id":2,"archived":1}'
post "plant archive no id"     400 /api/plants/archive '{"archived":true}'

expect 200 "plant list"        -b "$JAR" "$B/api/plants"
expect_body '"name":"Monty II"' "plant list has the update"
expect_body '"notes":"line 1\nline 2"' "plant list keeps notes"
expect_body '"name":"Fern","species":"Monstera deliciosa","location":"living room","acquired":null,"notes":"line 1\nline 2","archived":true' "plant list shows archived"
expect_body "\"acquired\":\"$TODAY\"" "plant list has acquired"

# care types
post "type add"                201 /api/plants/types/add '{"name":"Watering"}'
post "type add (2)"            201 /api/plants/types/add '{"name":"Fertilising"}'
post "type add (3)"            201 /api/plants/types/add '{"name":"Repotting"}'
post "type add (4)"            201 /api/plants/types/add '{"name":"Misting"}'
post "type add duplicate"      409 /api/plants/types/add '{"name":"watering"}'
post "type add empty"          400 /api/plants/types/add '{"name":""}'
post "type add long"           400 /api/plants/types/add "{\"name\":\"$(printf '%051d' 0)\"}"
post "type add not string"     400 /api/plants/types/add '{"name":5}'
post "type update"             204 /api/plants/types/update '{"id":2,"name":"Fertilizing"}'
post "type update duplicate"   409 /api/plants/types/update '{"id":2,"name":"Watering"}'
post "type update missing"     404 /api/plants/types/update '{"id":999,"name":"x"}'
post "type update bad name"    400 /api/plants/types/update '{"id":2,"name":"a\nb"}'
post "type archive"            204 /api/plants/types/archive '{"id":4,"archived":true}'
post "type archive missing"    404 /api/plants/types/archive '{"id":999,"archived":false}'
post "type archive not bool"   400 /api/plants/types/archive '{"id":4,"archived":"yes"}'
expect 200 "type list"         -b "$JAR" "$B/api/plants"
expect_body '"care_types":[{"id":2,"name":"Fertilizing","archived":false},{"id":4,"name":"Misting","archived":true}' "type list sorted, archived"

# rules
R='"plant_id":1,"care_type_id":1'
post "rule save"               204 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[]}"
expect 200 "plant get"         -b "$JAR" "$B/api/plants/plant?id=1"
expect_body "\"care_type\":\"Watering\",\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[],\"created\":\"$TODAY\",\"last_done\":null,\"due\":\"$TODAY\",\"days_left\":0" "new rule is due today"
post "rule save periods"       204 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[{\"start_month\":11,\"start_day\":1,\"end_month\":2,\"end_day\":29,\"interval_days\":10},{\"start_month\":7,\"start_day\":1,\"end_month\":7,\"end_day\":31,\"interval_days\":null}]}"
expect 200 "plant get periods" -b "$JAR" "$B/api/plants/plant?id=1"
expect_body '"periods":[{"start_month":11,"start_day":1,"end_month":2,"end_day":29,"interval_days":10},{"start_month":7,"start_day":1,"end_month":7,"end_day":31,"interval_days":null}]' "periods stored"
post "rule save yearly"        204 /api/plants/rules/save '{"plant_id":1,"care_type_id":3,"interval_days":null,"yearly_month":2,"yearly_day":29,"periods":[]}'
post "rule save paused default" 204 /api/plants/rules/save '{"plant_id":1,"care_type_id":2,"interval_days":null,"yearly_month":null,"yearly_day":null,"periods":[{"start_month":1,"start_day":1,"end_month":12,"end_day":31,"interval_days":14}]}'
expect 200 "plant get yearly"  -b "$JAR" "$B/api/plants/plant?id=1"
expect_body '"care_type":"Repotting","interval_days":null,"yearly_month":2,"yearly_day":29' "yearly rule stored"

post "rule overlap"            400 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[{\"start_month\":11,\"start_day\":1,\"end_month\":2,\"end_day\":28,\"interval_days\":10},{\"start_month\":2,\"start_day\":1,\"end_month\":3,\"end_day\":1,\"interval_days\":null}]}"
expect_body "periods must not overlap" "rule overlap message"
PER='{"start_month":1,"start_day":1,"end_month":1,"end_day":1,"interval_days":1}'
TWELVE=""
for m in 1 2 3 4 5 6 7 8 9 10 11 12; do
    TWELVE="$TWELVE${TWELVE:+,}{\"start_month\":$m,\"start_day\":1,\"end_month\":$m,\"end_day\":2,\"interval_days\":3}"
done
post "rule 12 periods"         204 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[$TWELVE]}"
post "rule 13 periods"         400 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[$TWELVE,{\"start_month\":12,\"start_day\":20,\"end_month\":12,\"end_day\":21,\"interval_days\":3}]}"
post "rule periods not array"  400 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":{}}"
post "rule period not object"  400 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[1]}"
post "rule period bad day"     400 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[{\"start_month\":2,\"start_day\":30,\"end_month\":3,\"end_day\":1,\"interval_days\":1}]}"
post "rule period month 13"    400 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[{\"start_month\":13,\"start_day\":1,\"end_month\":3,\"end_day\":1,\"interval_days\":1}]}"
post "rule period interval 0"  400 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[{\"start_month\":1,\"start_day\":1,\"end_month\":3,\"end_day\":1,\"interval_days\":0}]}"
post "rule interval 0"         400 /api/plants/rules/save "{$R,\"interval_days\":0,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[]}"
post "rule interval max"       204 /api/plants/rules/save "{$R,\"interval_days\":3650,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[]}"
post "rule interval max + 1"   400 /api/plants/rules/save "{$R,\"interval_days\":3651,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[]}"
post "rule no interval key"    400 /api/plants/rules/save "{$R,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[]}"
post "rule paused all year"    400 /api/plants/rules/save "{$R,\"interval_days\":null,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[]}"
expect_body "paused all year" "rule paused message"
post "rule yearly + interval"  400 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":3,\"yearly_day\":1,\"periods\":[]}"
post "rule yearly + periods"   400 /api/plants/rules/save "{$R,\"interval_days\":null,\"yearly_month\":3,\"yearly_day\":1,\"periods\":[$PER]}"
post "rule yearly no day"      400 /api/plants/rules/save "{$R,\"interval_days\":null,\"yearly_month\":3,\"yearly_day\":null,\"periods\":[]}"
post "rule yearly Feb 30"      400 /api/plants/rules/save "{$R,\"interval_days\":null,\"yearly_month\":2,\"yearly_day\":30,\"periods\":[]}"
post "rule plant missing"      404 /api/plants/rules/save '{"plant_id":999,"care_type_id":1,"interval_days":4,"yearly_month":null,"yearly_day":null,"periods":[]}'
post "rule type missing"       404 /api/plants/rules/save '{"plant_id":1,"care_type_id":999,"interval_days":4,"yearly_month":null,"yearly_day":null,"periods":[]}'
post "rule type archived"      400 /api/plants/rules/save '{"plant_id":1,"care_type_id":4,"interval_days":4,"yearly_month":null,"yearly_day":null,"periods":[]}'
post "rule no plant id"        400 /api/plants/rules/save '{"care_type_id":1,"interval_days":4,"yearly_month":null,"yearly_day":null,"periods":[]}'
post "rule save back"          204 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[]}"

post "rule delete"             204 /api/plants/rules/delete '{"plant_id":1,"care_type_id":2}'
post "rule delete again"       404 /api/plants/rules/delete '{"plant_id":1,"care_type_id":2}'
post "rule delete bad id"      400 /api/plants/rules/delete '{"plant_id":1,"care_type_id":null}'

# log
L="\"plant_id\":1,\"care_type_id\":1,\"date\":\"$TODAY\""
post "log add"                 201 /api/plants/log/add "{$L,\"note\":\"\"}"
expect_body '"id":1' "log add returns id"
expect 200 "due after log"     -b "$JAR" "$B/api/plants/plant?id=1"
expect_body "\"care_type\":\"Watering\",\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[],\"created\":\"$TODAY\",\"last_done\":\"$TODAY\",\"due\":\"$IN4\",\"days_left\":4" "logging moves the due date"
post "log add note"            201 /api/plants/log/add "{\"plant_id\":1,\"care_type_id\":null,\"date\":\"$TODAY\",\"note\":\"new leaf\nunfurling\"}"
post "log add archived plant"  201 /api/plants/log/add "{\"plant_id\":2,\"care_type_id\":null,\"date\":\"$TODAY\",\"note\":\"\"}"
post "log add future"          400 /api/plants/log/add "{\"plant_id\":1,\"care_type_id\":1,\"date\":\"$TOMORROW\",\"note\":\"\"}"
post "log add bad date"        400 /api/plants/log/add '{"plant_id":1,"care_type_id":1,"date":"2026-13-01","note":""}'
post "log add 1899"            400 /api/plants/log/add '{"plant_id":1,"care_type_id":1,"date":"1899-12-31","note":""}'
post "log add null date"       400 /api/plants/log/add '{"plant_id":1,"care_type_id":1,"date":null,"note":""}'
post "log add note long"       400 /api/plants/log/add "{$L,\"note\":\"$LONG4001\"}"
post "log add no type key"     400 /api/plants/log/add "{\"plant_id\":1,\"date\":\"$TODAY\",\"note\":\"\"}"
post "log add plant missing"   404 /api/plants/log/add "{\"plant_id\":999,\"care_type_id\":1,\"date\":\"$TODAY\",\"note\":\"\"}"
post "log add type missing"    404 /api/plants/log/add "{\"plant_id\":1,\"care_type_id\":999,\"date\":\"$TODAY\",\"note\":\"\"}"
post "log add type archived"   400 /api/plants/log/add "{\"plant_id\":1,\"care_type_id\":4,\"date\":\"$TODAY\",\"note\":\"\"}"

post "log update"              204 /api/plants/log/update "{\"id\":2,\"care_type_id\":null,\"date\":\"2026-01-01\",\"note\":\"edited\"}"
post "log update missing"      404 /api/plants/log/update "{\"id\":999,\"care_type_id\":null,\"date\":\"$TODAY\",\"note\":\"\"}"
post "log update to archived"  400 /api/plants/log/update "{\"id\":2,\"care_type_id\":4,\"date\":\"$TODAY\",\"note\":\"\"}"
post "log update future"       400 /api/plants/log/update "{\"id\":2,\"care_type_id\":null,\"date\":\"$TOMORROW\",\"note\":\"\"}"

expect 200 "log list"          -b "$JAR" "$B/api/plants/log?plant_id=1"
expect_body "{\"entries\":[{\"id\":1,\"care_type_id\":1,\"date\":\"$TODAY\",\"note\":\"\"},{\"id\":2,\"care_type_id\":null,\"date\":\"2026-01-01\",\"note\":\"edited\"}],\"more\":false}" "log list newest first"
expect 200 "log list before"   -b "$JAR" "$B/api/plants/log?plant_id=1&before=1"
expect_body '{"entries":[{"id":2,' "log list pages"
expect 404 "log before other plant" -b "$JAR" "$B/api/plants/log?plant_id=1&before=3"
expect 404 "log before missing" -b "$JAR" "$B/api/plants/log?plant_id=1&before=999"
expect 400 "log before bad"    -b "$JAR" "$B/api/plants/log?plant_id=1&before=%zz"
expect 400 "log before empty"  -b "$JAR" "$B/api/plants/log?plant_id=1&before="
expect 400 "log no plant"      -b "$JAR" "$B/api/plants/log"
expect 400 "log plant text"    -b "$JAR" "$B/api/plants/log?plant_id=abc"
expect 400 "log plant signed"  -b "$JAR" "$B/api/plants/log?plant_id=+1"
expect 404 "log plant missing" -b "$JAR" "$B/api/plants/log?plant_id=999"
expect 405 "log list POST"     -b "$JAR" -X POST -H "$J" -d '{}' "$B/api/plants/log"

i=0
while [ $i -lt 51 ]; do
    curl -s -o /dev/null -b "$JAR" -H "$J" \
        -d "{\"plant_id\":3,\"care_type_id\":null,\"date\":\"$TODAY\",\"note\":\"$i\"}" \
        "$B/api/plants/log/add"
    i=$((i + 1))
done
expect 200 "log list 51"       -b "$JAR" "$B/api/plants/log?plant_id=3"
expect_body '"more":true' "log list has more"

post "log delete"              204 /api/plants/log/delete '{"id":1}'
post "log delete again"        404 /api/plants/log/delete '{"id":1}'
post "log delete bad id"       400 /api/plants/log/delete '{"id":-1}'
expect 200 "due after delete"  -b "$JAR" "$B/api/plants/plant?id=1"
expect_body "\"last_done\":null,\"due\":\"$TODAY\",\"days_left\":0" "deleting the log resets the due date"

# plant and due list
expect 200 "plant get ok"      -b "$JAR" "$B/api/plants/plant?id=1"
expect_body '"id":1,"name":"Monty II"' "plant get fields"
expect 400 "plant get no id"   -b "$JAR" "$B/api/plants/plant"
expect 400 "plant get id 0"    -b "$JAR" "$B/api/plants/plant?id=0"
expect 400 "plant get id -1"   -b "$JAR" "$B/api/plants/plant?id=-1"
expect 400 "plant get id huge" -b "$JAR" "$B/api/plants/plant?id=99999999999999999999"
expect 404 "plant get missing" -b "$JAR" "$B/api/plants/plant?id=999"

post "rule on archived plant"  204 /api/plants/rules/save '{"plant_id":2,"care_type_id":1,"interval_days":4,"yearly_month":null,"yearly_day":null,"periods":[]}'
expect 200 "due list"          -b "$JAR" "$B/api/plants/due"
expect_body "{\"plant_id\":1,\"plant\":\"Monty II\",\"care_type_id\":1,\"care_type\":\"Watering\",\"due\":\"$TODAY\",\"days_left\":0}" "due list has the rule"
if grep -q '"plant_id":2' "$TMP/body"; then
    FAILED=$((FAILED + 1)); echo "FAIL: due list shows an archived plant"
else PASSED=$((PASSED + 1)); fi

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
