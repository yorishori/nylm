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

export NYLM_DATA="$TMP/data" NYLM_PUBLIC=public NYLM_PORT=$PORT

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

# ---- data folder -----------------------------------------------------------

# refuses DESCRIPTION ENV-ARG [nylm-args...]: nylm, run with one change to its
# environment, exits non-zero with an error about the data folder.
refuses() {
    desc=$1 envarg=$2
    shift 2
    if env "$envarg" "$BIN" "$@" >"$TMP/log" 2>&1 </dev/null; then
        FAILED=$((FAILED + 1)); echo "FAIL: $desc: started anyway"
    elif grep -q "NYLM_DATA\|data folder" "$TMP/log"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1)); echo "FAIL: $desc: unclear error: $(cat "$TMP/log")"
    fi
}
refuses "NYLM_DATA unset"       -uNYLM_DATA
refuses "NYLM_DATA empty"       NYLM_DATA=
refuses "data folder missing"   NYLM_DATA="$TMP/data"
refuses "set-password, missing" NYLM_DATA="$TMP/data" set-password
touch "$TMP/file"
refuses "data folder is a file" NYLM_DATA="$TMP/file"
if [ -e "$TMP/data" ]; then
    FAILED=$((FAILED + 1)); echo "FAIL: a missing data folder was created"
else PASSED=$((PASSED + 1)); fi

# ---- setup ---------------------------------------------------------------

# An empty data folder is a fresh install: each app gets its folder and database.
mkdir "$TMP/data"
echo 'smoke test password' | "$BIN" set-password 2>/dev/null ||
    { echo "set-password failed"; exit 1; }
for f in core/core.db plants/plants.db music/music.db; do
    if [ -f "$TMP/data/$f" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $f not created"; fi
done
if [ "$(stat -c %a "$TMP/data/plants")" = 700 ]; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: app folder is not mode 700"; fi

# A music library from the test files: two albums (folders), one track with
# two genres, a file that is not audio, a hidden folder and a symlink.
M="$TMP/music"
mkdir -p "$M/Artist/Album" "$M/Artist/Multi" "$M/.hidden"
cp tests/data/tagged.mp3 "$M/Artist/Album/01.mp3"
cp tests/data/tagged.flac "$M/Artist/Album/02.flac"
cp tests/data/multi.flac "$M/Artist/Multi/01.flac"
cp tests/data/tagged.mp3 "$M/.hidden/hidden.mp3"
echo "not audio" > "$M/Artist/Album/03.mp3"
ln -s "$M/Artist/Album/01.mp3" "$M/Artist/link.mp3"
export NYLM_MUSIC="$M"

# service WANT DESCRIPTION COMMAND [env-args]: `nylm COMMAND` (music-scan,
# music-write) exits with WANT (0 or 1); its output is in $TMP/service.log.
service() {
    want=$1 desc=$2 cmd=$3
    shift 3
    if env "$@" "$BIN" "$cmd" >"$TMP/service.log" 2>&1; then got=0; else got=1; fi
    if [ "$got" = "$want" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $desc: exit $got: $(cat "$TMP/service.log")"; fi
}
# logged TEXT DESCRIPTION: the last service's output contains TEXT.
logged() {
    if grep -qF -- "$1" "$TMP/service.log"; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $2: $(cat "$TMP/service.log")"; fi
}
service 1 "music-scan without NYLM_MUSIC" music-scan NYLM_MUSIC=
service 1 "music-scan, NYLM_MUSIC /"      music-scan NYLM_MUSIC=/
service 1 "music-scan, relative"          music-scan NYLM_MUSIC=music
service 1 "music-scan, missing folder"    music-scan NYLM_MUSIC="$TMP/nope"
service 1 "music-write without NYLM_MUSIC" music-write NYLM_MUSIC=
service 0 "music-scan"                    music-scan
logged "4 files, 3 read, 0 removed, 1 failed" "scan counts"
service 0 "music-scan again"              music-scan
logged "4 files, 0 read" "rescan reads only changed files"
service 0 "music-write, nothing pending"  music-write
logged "0 files written" "write with nothing pending"

# ---- API and static files (defaults: 127.0.0.1, allow 127.0.0.0/8) ---------

start "http://127.0.0.1:$PORT/"
B="http://127.0.0.1:$PORT"
J="Content-Type: application/json"
JAR="$TMP/cookies"

expect 200 "index"              "$B/"
expect 200 "common.js"          "$B/common.js"
expect 200 "home.js"            "$B/home.js"
expect 200 "plants page"        "$B/plants/"
expect 200 "plants.js"          "$B/plants/plants.js"
expect 200 "plants.css"         "$B/plants/plants.css"
expect 200 "music page"         "$B/music/"
expect 200 "music.js"           "$B/music/music.js"
expect 200 "music.css"          "$B/music/music.css"
expect 404 "plants without /"   "$B/plants"
expect 404 "old app.js gone"    "$B/app.js"
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
P='"species":"Monstera deliciosa","location":"living room","acquired":null,"notes":"line 1\nline 2","color":"mint"'
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
post "plant add species type"  400 /api/plants/add '{"name":"x","species":1,"location":"","acquired":null,"notes":"","color":"mint"}'
post "plant add no location"   400 /api/plants/add '{"name":"x","species":"","acquired":null,"notes":"","color":"mint"}'
post "plant add notes long"    400 /api/plants/add "{\"name\":\"x\",\"species\":\"\",\"location\":\"\",\"acquired\":null,\"notes\":\"$LONG4001\",\"color\":\"mint\"}"
post "plant add notes tab"     400 /api/plants/add '{"name":"x","species":"","location":"","acquired":null,"notes":"a\tb","color":"mint"}'
post "plant add acquired"      201 /api/plants/add "{\"name\":\"Cactus\",\"species\":\"\",\"location\":\"\",\"acquired\":\"$TODAY\",\"notes\":\"\",\"color\":\"mint\"}"
post "plant add future date"   400 /api/plants/add "{\"name\":\"x\",\"species\":\"\",\"location\":\"\",\"acquired\":\"$TOMORROW\",\"notes\":\"\",\"color\":\"mint\"}"
post "plant add bad date"      400 /api/plants/add '{"name":"x","species":"","location":"","acquired":"2024-02-30","notes":"","color":"mint"}'
post "plant add date number"   400 /api/plants/add '{"name":"x","species":"","location":"","acquired":20240101,"notes":"","color":"mint"}'
N='"species":"","location":"","acquired":null,"notes":""'
post "plant add no color"      400 /api/plants/add "{\"name\":\"x\",$N}"
expect_body "'color' must be one of: butter, lime, mint, teal, sky, periwinkle, lavender, orchid" "plant color message"
post "plant add urgent color"  400 /api/plants/add "{\"name\":\"x\",$N,\"color\":\"rose\"}"
post "plant add peach"         400 /api/plants/add "{\"name\":\"x\",$N,\"color\":\"peach\"}"
post "plant add color case"    400 /api/plants/add "{\"name\":\"x\",$N,\"color\":\"Mint\"}"
post "plant add color number"  400 /api/plants/add "{\"name\":\"x\",$N,\"color\":3}"
post "plant add color empty"   400 /api/plants/add "{\"name\":\"x\",$N,\"color\":\"\"}"

post "plant update"            204 /api/plants/update "{\"id\":1,\"name\":\"Monty II\",$N,\"color\":\"teal\",\"notes\":\"line 1\nline 2\"}"
post "plant update bad color"  400 /api/plants/update "{\"id\":1,\"name\":\"Monty II\",$N,\"color\":\"red\"}"
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
expect_body '"name":"Fern","species":"Monstera deliciosa","location":"living room","acquired":null,"notes":"line 1\nline 2","color":"mint","archived":true' "plant list shows archived"
expect_body "\"acquired\":\"$TODAY\"" "plant list has acquired"

# care types
post "type add"                201 /api/plants/types/add '{"name":"Watering","color":"sky"}'
post "type add (2)"            201 /api/plants/types/add '{"name":"Fertilising","color":"lime"}'
post "type add (3)"            201 /api/plants/types/add '{"name":"Repotting","color":"butter"}'
post "type add (4)"            201 /api/plants/types/add '{"name":"Misting","color":"lavender"}'
post "type add duplicate"      409 /api/plants/types/add '{"name":"watering","color":"sky"}'
post "type add empty"          400 /api/plants/types/add '{"name":"","color":"sky"}'
post "type add long"           400 /api/plants/types/add "{\"name\":\"$(printf '%051d' 0)\",\"color\":\"sky\"}"
post "type add not string"     400 /api/plants/types/add '{"name":5,"color":"sky"}'
post "type add no color"       400 /api/plants/types/add '{"name":"Pruning"}'
post "type add bad color"      400 /api/plants/types/add '{"name":"Pruning","color":"rose"}'
expect_body "'color' must be one of" "type color message"
post "type update"             204 /api/plants/types/update '{"id":2,"name":"Fertilizing","color":"lime"}'
post "type update duplicate"   409 /api/plants/types/update '{"id":2,"name":"Watering","color":"lime"}'
post "type update missing"     404 /api/plants/types/update '{"id":999,"name":"x","color":"lime"}'
post "type update bad name"    400 /api/plants/types/update '{"id":2,"name":"a\nb","color":"lime"}'
post "type update bad color"   400 /api/plants/types/update '{"id":2,"name":"Fertilizing","color":"peach"}'
post "type update no color"    400 /api/plants/types/update '{"id":2,"name":"Fertilizing"}'
post "type archive"            204 /api/plants/types/archive '{"id":4,"archived":true}'
post "type archive missing"    404 /api/plants/types/archive '{"id":999,"archived":false}'
post "type archive not bool"   400 /api/plants/types/archive '{"id":4,"archived":"yes"}'
expect 200 "type list"         -b "$JAR" "$B/api/plants"
expect_body '"care_types":[{"id":2,"name":"Fertilizing","color":"lime","archived":false},{"id":4,"name":"Misting","color":"lavender","archived":true}' "type list sorted, archived"

# rules
R='"plant_id":1,"care_type_id":1'
post "rule save"               204 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[]}"
expect 200 "plant get"         -b "$JAR" "$B/api/plants/plant?id=1"
expect_body "\"care_type\":\"Watering\",\"care_type_color\":\"sky\",\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[],\"created\":\"$TODAY\",\"last_done\":null,\"due\":\"$TODAY\",\"days_left\":0" "new rule is due today"
post "rule save periods"       204 /api/plants/rules/save "{$R,\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[{\"start_month\":11,\"start_day\":1,\"end_month\":2,\"end_day\":29,\"interval_days\":10},{\"start_month\":7,\"start_day\":1,\"end_month\":7,\"end_day\":31,\"interval_days\":null}]}"
expect 200 "plant get periods" -b "$JAR" "$B/api/plants/plant?id=1"
expect_body '"periods":[{"start_month":11,"start_day":1,"end_month":2,"end_day":29,"interval_days":10},{"start_month":7,"start_day":1,"end_month":7,"end_day":31,"interval_days":null}]' "periods stored"
post "rule save yearly"        204 /api/plants/rules/save '{"plant_id":1,"care_type_id":3,"interval_days":null,"yearly_month":2,"yearly_day":29,"periods":[]}'
post "rule save paused default" 204 /api/plants/rules/save '{"plant_id":1,"care_type_id":2,"interval_days":null,"yearly_month":null,"yearly_day":null,"periods":[{"start_month":1,"start_day":1,"end_month":12,"end_day":31,"interval_days":14}]}'
expect 200 "plant get yearly"  -b "$JAR" "$B/api/plants/plant?id=1"
expect_body '"care_type":"Repotting","care_type_color":"butter","interval_days":null,"yearly_month":2,"yearly_day":29' "yearly rule stored"

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
expect_body "\"care_type\":\"Watering\",\"care_type_color\":\"sky\",\"interval_days\":4,\"yearly_month\":null,\"yearly_day\":null,\"periods\":[],\"created\":\"$TODAY\",\"last_done\":\"$TODAY\",\"due\":\"$IN4\",\"days_left\":4" "logging moves the due date"
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
expect_body "{\"plant_id\":1,\"plant\":\"Monty II\",\"plant_color\":\"teal\",\"care_type_id\":1,\"care_type\":\"Watering\",\"care_type_color\":\"sky\",\"due\":\"$TODAY\",\"days_left\":0}" "due list has the rule"
if grep -q '"plant_id":2' "$TMP/body"; then
    FAILED=$((FAILED + 1)); echo "FAIL: due list shows an archived plant"
else PASSED=$((PASSED + 1)); fi

# ---- music -------------------------------------------------------------------

for route in "GET /api/music" "GET /api/music/albums" "GET /api/music/album?id=1" \
             "GET /api/music/changes" "POST /api/music/album/save" \
             "POST /api/music/changes/cancel" "POST /api/music/scan" "POST /api/music/write"; do
    expect 401 "${route#* } needs login" -X "${route%% *}" -H "$J" "$B${route#* }"
done

# same_file DESCRIPTION A B: the two files are byte for byte the same.
same_file() {
    if cmp -s "$2" "$3"; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $1: files differ"; fi
}
LOCK="$TMP/data/music/library.lock"

expect 200 "music overview"    -b "$JAR" "$B/api/music"
expect_body '"configured":true,"available":true,"albums":2,"tracks":3,"pending":0,"busy":null' "music counts"
expect_body '"files":4,"parsed":0,"failed":1,"ok":1},"scan_state":"done"' "music last scan"

expect 200 "music albums"      -b "$JAR" "$B/api/music/albums"
expect_body '"dir":"Artist/Album","tracks":2,"with_art":0,"album":"Some Album","albumartist":"Some Artist","date":"2001","genre":"Rock","pending":0,"mixed":[]' "albums list values"
expect_body '"dir":"Artist/Multi"' "albums list second album"
A=$(grep -o '"id":[0-9]*,"dir":"Artist/Album"' "$TMP/body" | grep -o '[0-9]*' | head -1)
MA=$(grep -o '"id":[0-9]*,"dir":"Artist/Multi"' "$TMP/body" | grep -o '[0-9]*' | head -1)

expect 200 "music album"       -b "$JAR" "$B/api/music/album?id=$MA"
expect_body '"genre":"Rock; Pop"' "album shows several values joined"
expect_body '"file":"01.flac","locked":["genre"],"pending":{}' "album marks the locked field"
MT=$(grep -o '"tracks":\[{"id":[0-9]*' "$TMP/body" | grep -o '[0-9]*$')
expect 200 "music album (2)"   -b "$JAR" "$B/api/music/album?id=$A"
expect_body '"title":"Song One"' "album track tags"
T1=$(grep -o '"tracks":\[{"id":[0-9]*' "$TMP/body" | grep -o '[0-9]*$')
expect 400 "album no id"       -b "$JAR" "$B/api/music/album"
expect 400 "album id 0"        -b "$JAR" "$B/api/music/album?id=0"
expect 400 "album id text"     -b "$JAR" "$B/api/music/album?id=x"
expect 404 "album missing"     -b "$JAR" "$B/api/music/album?id=999"

# Queueing changes writes no file.
S=/api/music/album/save
cp "$M/Artist/Album/01.mp3" "$TMP/before.mp3"
post "queue genre for album"   200 $S "{\"id\":$A,\"album\":{\"genre\":\"Jazz\"},\"tracks\":[]}"
expect_body '{"queued":2,"dropped":0}' "queue counts"
same_file "queueing leaves the file alone" "$TMP/before.mp3" "$M/Artist/Album/01.mp3"
expect 200 "album with pending" -b "$JAR" "$B/api/music/album?id=$A"
expect_body '"genre":"Rock","date":"2001","tracknumber":"1/2","discnumber":null,"compilation":null,"file":"01.mp3","locked":[],"pending":{"genre":"Jazz"}' "album shows the file value and the pending one"
post "queue the same again"    200 $S "{\"id\":$A,\"album\":{\"genre\":\"Jazz\"},\"tracks\":[]}"
expect_body '{"queued":0,"dropped":0}' "same pending value is not queued again"
post "queue back to the file value" 200 $S "{\"id\":$A,\"album\":{\"genre\":\"Rock\"},\"tracks\":[]}"
expect_body '{"queued":0,"dropped":2}' "the file's own value drops the pending change"
post "queue track title"       200 $S "{\"id\":$A,\"album\":{},\"tracks\":[{\"id\":$T1,\"title\":\"New Title\",\"artist\":null}]}"
expect_body '{"queued":1,"dropped":0}' "queue one track field"
post "queue several"           200 $S "{\"id\":$A,\"album\":{\"genre\":\"Jazz\",\"date\":\"\",\"compilation\":true},\"tracks\":[]}"
expect_body '{"queued":6,"dropped":0}' "queue album fields"
expect 200 "overview pending"  -b "$JAR" "$B/api/music"
expect_body '"pending":7,"busy":null' "overview counts pending changes"

expect 415 "save needs json"   -b "$JAR" -d "{\"id\":$A}" "$B$S"
post "save no id"              400 $S '{"album":{},"tracks":[]}'
post "save no album"           400 $S "{\"id\":$A,\"tracks\":[]}"
post "save album not object"   400 $S "{\"id\":$A,\"album\":[],\"tracks\":[]}"
post "save no tracks"          400 $S "{\"id\":$A,\"album\":{}}"
post "save unknown field"      400 $S "{\"id\":$A,\"album\":{\"title\":\"x\"},\"tracks\":[]}"
expect_body "unknown field 'title'" "unknown field message"
post "save empty album"        400 $S "{\"id\":$A,\"album\":{\"album\":\"\"},\"tracks\":[]}"
expect_body "'album' can not be empty" "required field message"
post "save spaces"             400 $S "{\"id\":$A,\"album\":{\"album\":\" x\"},\"tracks\":[]}"
post "save bad date"           400 $S "{\"id\":$A,\"album\":{\"date\":\"2001-02-30\"},\"tracks\":[]}"
post "save compilation text"   400 $S "{\"id\":$A,\"album\":{\"compilation\":\"1\"},\"tracks\":[]}"
post "save genre number"       400 $S "{\"id\":$A,\"album\":{\"genre\":5},\"tracks\":[]}"
post "save genre newline"      400 $S "{\"id\":$A,\"album\":{\"genre\":\"a\\nb\"},\"tracks\":[]}"
post "save genre too long"     400 $S "{\"id\":$A,\"album\":{\"genre\":\"$(printf '%0501d' 0)\"},\"tracks\":[]}"
post "save bad track number"   400 $S "{\"id\":$A,\"album\":{},\"tracks\":[{\"id\":$T1,\"tracknumber\":\"3/2\"}]}"
post "save track not object"   400 $S "{\"id\":$A,\"album\":{},\"tracks\":[1]}"
post "save track no id"        400 $S "{\"id\":$A,\"album\":{},\"tracks\":[{\"title\":\"x\"}]}"
post "save track elsewhere"    400 $S "{\"id\":$A,\"album\":{},\"tracks\":[{\"id\":$MT,\"title\":\"x\"}]}"
post "save track twice"        400 $S "{\"id\":$A,\"album\":{},\"tracks\":[{\"id\":$T1},{\"id\":$T1}]}"
post "save track album field"  400 $S "{\"id\":$A,\"album\":{},\"tracks\":[{\"id\":$T1,\"genre\":\"x\"}]}"
post "save album missing"      404 $S '{"id":999,"album":{},"tracks":[]}'
post "save locked field"       409 $S "{\"id\":$MA,\"album\":{\"genre\":\"Rock\"},\"tracks\":[]}"
expect_body "genre has several values" "locked field message"

# The list of changes, and cancelling pending ones.
C=/api/music/changes/cancel
expect 200 "changes"           -b "$JAR" "$B/api/music/changes"
expect_body '"path":"Artist/Album/01.mp3","field":"title","old":"Song One","new":"New Title","state":"pending","note":""' "pending change listed"
COMP=$(grep -o '"id":[0-9]*,"album_id":[0-9]*,"path":"[^"]*","field":"compilation"' "$TMP/body" |
       grep -o '^"id":[0-9]*' | grep -o '[0-9]*' | tr '\n' ',' | sed 's/,$//')
post "cancel compilation"      200 $C "{\"ids\":[$COMP]}"
expect_body '{"cancelled":2}' "cancel counts"
post "cancel again"            200 $C "{\"ids\":[$COMP]}"
expect_body '{"cancelled":0}' "cancelled changes are gone"
expect 415 "cancel needs json" -b "$JAR" -d '{"ids":[1]}' "$B$C"
post "cancel no ids"           400 $C '{}'
post "cancel empty ids"        400 $C '{"ids":[]}'
post "cancel ids not numbers"  400 $C '{"ids":["1"]}'
post "cancel id 0"             400 $C '{"ids":[0]}'
post "cancel id fraction"      400 $C '{"ids":[1.5]}'
post "cancel too many"         400 $C "{\"ids\":[$(seq -s, 1 1001)]}"

# Starting the write service: password, then nothing may be running. (The
# right password with changes pending runs the root action through sudo,
# which is not tried here: on a server it would start the real service.)
W=/api/music/write
post "write no password"       400 $W '{}'
post "write wrong password"    403 $W '{"password":"nope"}'
expect 415 "write needs json"  -b "$JAR" -d '{"password":"x"}' "$B$W"

# While a service holds the library lock, the server writes nothing.
flock "$LOCK" sleep 8 &
LOCKER=$!
sleep 0.3
expect 200 "overview while busy" -b "$JAR" "$B/api/music"
expect_body '"busy":"' "overview shows the library busy"
post "queue while busy"        409 $S "{\"id\":$A,\"album\":{\"genre\":\"Pop\"},\"tracks\":[]}"
expect_body "the library is busy" "busy message"
post "cancel while busy"       409 $C '{"ids":[1]}'
post "scan while busy"         409 /api/music/scan '{"password":"smoke test password"}'
post "write while busy"        409 $W '{"password":"smoke test password"}'
service 1 "music-write while busy" music-write
logged "the library is busy" "service refuses while another runs"
service 1 "music-scan while busy" music-scan
wait "$LOCKER"
# A short write by the server (shared lock): the service waits for it.
flock -s "$LOCK" sleep 1 &
LOCKER=$!
sleep 0.2

# The write service writes the pending changes.
service 0 "music-write"        music-write
logged "2 files written; 5 changes done, 0 with warnings, 0 failed" "write counts"
wait "$LOCKER"
if tail -c 128 "$M/Artist/Album/01.mp3" | head -c 3 | grep -q TAG; then
    PASSED=$((PASSED + 1)); else FAILED=$((FAILED + 1)); echo "FAIL: TagLib's ID3v1 tag removed"; fi
expect 200 "album after write" -b "$JAR" "$B/api/music/album?id=$A"
expect_body '"title":"New Title","artist":"Some Artist","album":"Some Album","albumartist":"Some Artist","genre":"Jazz","date":null,"tracknumber":"1/2","discnumber":null,"compilation":null,"file":"01.mp3","locked":[],"pending":{}' "written tags read back"
expect 200 "changes after write" -b "$JAR" "$B/api/music/changes"
expect_body '"pending":[],' "nothing pending after the write"
expect_body '"field":"title","old":"Song One","new":"New Title","state":"done","note":""' "history keeps the change"
post "write, nothing pending"  409 $W '{"password":"smoke test password"}'
expect_body "no pending changes" "nothing to write message"

# A file changed after its change was queued: that change fails, the other
# file is written.
post "queue genre again"       200 $S "{\"id\":$A,\"album\":{\"genre\":\"Blues\"},\"tracks\":[]}"
cp tests/data/tagged.flac "$M/Artist/Album/02.flac"
cp "$M/Artist/Album/02.flac" "$TMP/before.flac"
service 0 "music-write, one file changed" music-write
logged "1 files written; 1 changes done, 0 with warnings, 1 failed" "write counts with a failure"
logged "the file changed since this was queued: it now has Rock" "failure logged"
same_file "a failed change leaves the file alone" "$TMP/before.flac" "$M/Artist/Album/02.flac"
expect 200 "changes after failure" -b "$JAR" "$B/api/music/changes"
expect_body '"path":"Artist/Album/02.flac","field":"genre","old":"Jazz","new":"Blues","state":"failed","note":"the file changed since this was queued: it now has Rock"' "failure recorded with its cause"

# The service checks the table itself: a bad value put there directly fails.
service 0 "rescan" music-scan
sqlite3 "$TMP/data/music/music.db" \
    "INSERT INTO changes (track_id, path, field, old, new, client) VALUES ($T1, 'Artist/Album/01.mp3', 'title', 'New Title', '', 'test')"
service 0 "music-write, invalid value" music-write
logged "invalid value: title can not be empty" "service validates values"
expect 200 "album after invalid" -b "$JAR" "$B/api/music/album?id=$A"
expect_body '"title":"New Title"' "invalid value not written"

# A file removed by a scan: its pending change fails.
post "queue on the other album" 200 $S "{\"id\":$MA,\"album\":{\"album\":\"Multi\"},\"tracks\":[]}"
mv "$M/Artist/Multi" "$TMP/multi.away"
service 0 "scan after removing a folder" music-scan
service 0 "music-write, file gone" music-write
logged "1 failed" "change for a removed file fails"
expect 200 "changes after removal" -b "$JAR" "$B/api/music/changes"
expect_body '"album_id":null,"path":"Artist/Multi/01.flac","field":"album","old":"Some Album","new":"Multi","state":"failed","note":"the file is no longer in the library"' "removed file recorded"
mv "$TMP/multi.away" "$M/Artist/Multi"
service 0 "rescan after restoring the folder" music-scan

# The folder is gone (drive not mounted): services and their start refuse.
post "queue for folder test"   200 $S "{\"id\":$A,\"album\":{\"genre\":\"Pop\"},\"tracks\":[]}"
mv "$M" "$TMP/music.away"
post "write, folder missing"   503 $W '{"password":"smoke test password"}'
post "scan, folder missing"    503 /api/music/scan '{"password":"smoke test password"}'
service 1 "music-write, folder missing" music-write
expect 200 "overview, folder missing" -b "$JAR" "$B/api/music"
expect_body '"configured":true,"available":false,"albums":2,"tracks":3,"pending":2' "pending changes kept while the folder is missing"
mv "$TMP/music.away" "$M"

post "scan no password"        400 /api/music/scan '{}'
post "scan wrong password"     403 /api/music/scan '{"password":"nope"}'
expect 415 "scan needs json"   -b "$JAR" -d '{"password":"x"}' "$B/api/music/scan"

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
