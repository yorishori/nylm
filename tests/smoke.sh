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

# A music library from the test files: one album (Some Album by Some
# Artist) in two folders, one track with two genres and two titles, files
# that are not music, a file with a music extension that is not audio, a
# hidden folder and a symlink.
M="$TMP/music"
mkdir -p "$M/Artist/Album" "$M/Artist/Multi" "$M/.hidden"
cp tests/data/tagged.mp3 "$M/Artist/Album/01.mp3"
cp tests/data/tagged.flac "$M/Artist/Album/02.FLAC"
cp tests/data/multi.flac "$M/Artist/Multi/01.flac"
cp tests/data/tagged.mp3 "$M/.hidden/hidden.mp3"
echo "not audio" > "$M/Artist/Album/03.mp3"
echo "cover" > "$M/Artist/Album/cover.jpg"
echo "cover" > "$M/Artist/Multi/Cover.JPG"
echo "notes" > "$M/Artist/README"
ln -s "$M/Artist/Album/01.mp3" "$M/Artist/link.mp3"
export NYLM_MUSIC="$M"
# A file changed in the second a scan saw it is read again by the next
# scan: let that second pass first.
sleep 1
MDB="$TMP/data/music/music.db"

# service WANT DESCRIPTION COMMAND [env-args]: `nylm COMMAND` (music-scan,
# music-write) exits with WANT (0 or 1); its output is in $TMP/service.log.
service() {
    want=$1 desc=$2 cmd=$3
    shift 3
    if env "$@" "$BIN" "$cmd" >"$TMP/service.log" 2>&1; then got=0; else got=1; fi
    if [ "$got" = "$want" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $desc: exit $got: $(cat "$TMP/service.log")"; fi
}
# scan_path WANT DESCRIPTION PATH: `nylm music-scan PATH` exits with WANT.
scan_path() {
    if "$BIN" music-scan "$3" >"$TMP/service.log" 2>&1; then got=0; else got=1; fi
    if [ "$got" = "$1" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $2: exit $got: $(cat "$TMP/service.log")"; fi
}
# logged TEXT DESCRIPTION: the last service's output contains TEXT.
logged() {
    if grep -qF -- "$1" "$TMP/service.log"; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $2: $(cat "$TMP/service.log")"; fi
}
# query WANT DESCRIPTION SQL: the music database answers SQL with WANT.
query() {
    got=$(sqlite3 "$MDB" "$3")
    if [ "$got" = "$1" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $2: got '$got', want '$1'"; fi
}
service 1 "music-scan without NYLM_MUSIC" music-scan NYLM_MUSIC=
service 1 "music-scan, NYLM_MUSIC /"      music-scan NYLM_MUSIC=/
service 1 "music-scan, relative"          music-scan NYLM_MUSIC=music
service 1 "music-scan, missing folder"    music-scan NYLM_MUSIC="$TMP/nope"
service 1 "music-write without NYLM_MUSIC" music-write NYLM_MUSIC=
service 0 "music-scan"                    music-scan
logged "4 files, 3 read, 0 removed, 1 failed" "scan counts (hidden and symlink skipped)"
query "[blank]|1
jpg|2" "other extensions, lower case, counted" \
    "SELECT ext, count FROM scan_extensions WHERE scan_id = 1 ORDER BY ext"
query "mp3|flac|flac" "music extensions, lower case" \
    "SELECT group_concat(ext, '|') FROM (SELECT ext FROM tracks ORDER BY path)"
query "Rock|Pop" "genres in order" \
    "SELECT group_concat(value, '|') FROM (SELECT value FROM track_values
     WHERE track_id = (SELECT id FROM tracks WHERE path LIKE '%Multi/01.flac')
     AND field = 'genre' ORDER BY position)"
query "0|0|0" "compilation defaults to 0" "SELECT group_concat(compilation, '|') FROM tracks"
service 0 "music-scan again"              music-scan
logged "4 files, 0 read" "rescan reads only changed files"
scan_path 0 "scan a folder"               "$M/Artist/Album"
logged "3 files, 0 read, 0 removed, 1 failed" "folder scan counts"
scan_path 0 "scan a file"                 "$M/Artist/Album/01.mp3"
logged "1 files, 1 read, 0 removed, 0 failed" "a file is always read again"
scan_path 0 "scan the library folder"     "$M"
logged "4 files, 0 read" "the library folder is a whole scan"
scan_path 1 "scan outside the library"    "$TMP/data"
logged "is not the music folder (NYLM_MUSIC) or a path in it" "outside message"
scan_path 1 "scan with .."                "$M/../data"
scan_path 1 "scan a relative path"        "music"
scan_path 0 "scan a missing path"         "$M/nope.mp3"
logged "0 files, 0 read, 0 removed" "nothing to scan"
service 0 "music-write, nothing pending"  music-write
logged "0 tracks written" "write with nothing pending"

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

for route in "GET /api/music" "GET /api/music/albums" "GET /api/music/album?track=1" \
             "GET /api/music/values?field=artist" "GET /api/music/changes" \
             "POST /api/music/queue" "POST /api/music/discard" "POST /api/music/scan" \
             "POST /api/music/write" "GET /api/music/art?hash=$(printf '%064d' 0)&size=full" \
             "POST /api/music/cover"; do
    expect 401 "${route#* } needs login" -X "${route%% *}" -H "$J" "$B${route#* }"
done

# same_file DESCRIPTION A B: the two files are byte for byte the same.
same_file() {
    if cmp -s "$2" "$3"; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $1: files differ"; fi
}
LOCK="$TMP/data/music/library.lock"
PW='"password":"smoke test password"'

expect 200 "music overview"    -b "$JAR" "$B/api/music"
expect_body '{"stats":{"tracks":3,"albums":1,"size":' "music counts"
expect_body '"extensions":[{"ext":"flac","tracks":2,' "music extensions"
expect_body "\"configured\":true,\"available\":true,\"root\":\"$M\",\"busy\":null,\"pending\":0,\"queued_scans\":0" "music state"
expect_body '"state":"done","files":4,"parsed":0,"removed":0,"failed":1,"others":[{"ext":"jpg","count":2},{"ext":"[blank]","count":1}]},"running":null' "last whole scan"

expect 200 "albums"            -b "$JAR" "$B/api/music/albums"
expect_body '"album":"Some Album","albumartist":"Some Artist","date":"2001","compilation":"0","genre":null,"composer":[],"tracks":3,"mixed":["genre"],"changed":[],"missing":true,"invalid":true,"several_artists":true,"no_art":true}]' "album row"
A=$(grep -o '"track":[0-9]*' "$TMP/body" | head -1 | grep -o '[0-9]*')
expect 200 "album"             -b "$JAR" "$B/api/music/album?track=$A"
# id_of FILE: the id of the track whose path ends in FILE, from the last body.
id_of() {
    grep -o "\"id\":[0-9]*,\"path\":\"[^\"]*$1\"" "$TMP/body" | grep -o '^"id":[0-9]*' |
        grep -o '[0-9]*'
}
T1=$(id_of Album/01.mp3)
T2=$(id_of Album/02.FLAC)
T3=$(id_of Multi/01.flac)
expect_body "\"path\":\"$M/Artist/Album/01.mp3\",\"size\":1523,\"ext\":\"mp3\",\"scanned\":" "track file"
expect_body '"tags":{"title":"Song One","album":"Some Album","artist":"Some Artist","albumartist":"Some Artist","tracknumber":"1/2","discnumber":null,"date":"2001","compilation":"0"' "track tags"
expect_body '"genre":["Rock"],"composer":[]},"pending":{},"pictures":[],"missing":["discnumber","composer"],"invalid":["genre"]}' "track problems"
expect_body '"title":"Song Two; Other Title"' "several titles shown joined"
expect_body '"genre":["Rock","Pop"]' "several genres in order"
expect_body '"invalid":["tracknumber","genre"]' "track number without a total is invalid"
expect 400 "album no track"    -b "$JAR" "$B/api/music/album"
expect 400 "album track 0"     -b "$JAR" "$B/api/music/album?track=0"
expect 400 "album track text"  -b "$JAR" "$B/api/music/album?track=x"
expect 404 "album missing"     -b "$JAR" "$B/api/music/album?track=999"
expect 200 "one album row"     -b "$JAR" "$B/api/music/albums?track=$T3"
expect_body "[{\"track\":$A," "albums?track= gives that album"
expect 404 "one album, missing" -b "$JAR" "$B/api/music/albums?track=999"
expect 400 "one album, bad id" -b "$JAR" "$B/api/music/albums?track=x"
expect 200 "artist values"     -b "$JAR" "$B/api/music/values?field=artist"
expect_body '["Some Artist","Some Artist; Other Artist"]' "artist values"
expect 200 "genre values"      -b "$JAR" "$B/api/music/values?field=genre"
expect_body '["Pop","Rock"]' "genre values"
expect 200 "composer values"   -b "$JAR" "$B/api/music/values?field=composer"
expect_body '[]' "no composers yet"
expect 400 "values, bad field" -b "$JAR" "$B/api/music/values?field=title"
expect 400 "values, no field"  -b "$JAR" "$B/api/music/values"

# Queueing changes writes no file.
Q=/api/music/queue
cp "$M/Artist/Album/01.mp3" "$TMP/before.mp3"
post "queue album fields"      200 $Q "{\"album\":$T1,\"set\":{\"date\":\"1999\",\"genre\":[\"rock\",\"pop\"],\"composer\":[\"Bach\",\"Händel\"]}}"
expect_body '"batch":1,"queued":9,"dropped":0' "one change per tag and track, one batch"
same_file "queueing leaves the file alone" "$TMP/before.mp3" "$M/Artist/Album/01.mp3"
expect 200 "album row planned" -b "$JAR" "$B/api/music/albums?track=$T1"
expect_body '"date":"1999","compilation":"0","genre":["rock","pop"],"composer":["Bach","Händel"],"tracks":3,"mixed":[],"changed":["date","genre","composer"]' "row shows the planned values"
post "queue the same again"    200 $Q "{\"album\":$T1,\"set\":{\"date\":\"1999\"}}"
expect_body '"queued":0,"dropped":0' "the same pending value is not queued again"
post "queue track fields"      200 $Q "{\"edits\":[{\"track\":$T1,\"field\":\"title\",\"value\":\"New Title\"},{\"track\":$T1,\"field\":\"tracknumber\",\"value\":\"1/3\"},{\"track\":$T2,\"field\":\"tracknumber\",\"value\":\"2/3\"},{\"track\":$T3,\"field\":\"tracknumber\",\"value\":\"3/3\"},{\"track\":$T3,\"field\":\"title\",\"value\":\"Song Three\"},{\"track\":$T1,\"field\":\"isrc\",\"value\":\"\"}]}"
expect_body '"batch":2,"queued":5,"dropped":0' "queue several tracks (removing an absent tag is nothing)"
post "back to the file value"  200 $Q "{\"edits\":[{\"track\":$T1,\"field\":\"title\",\"value\":\"Song One\"}]}"
expect_body '"queued":0,"dropped":1' "the file's own value drops the pending change"
post "queue title again"       200 $Q "{\"edits\":[{\"track\":$T1,\"field\":\"title\",\"value\":\"New Title\"}]}"
expect 200 "album with pending" -b "$JAR" "$B/api/music/album?track=$T1"
expect_body '"pending":{"title":"New Title","tracknumber":"1/3","date":"1999","genre":["rock","pop"],"composer":["Bach","Händel"]},"pictures":[],"missing":["discnumber"],"invalid":[]' "album shows pending changes and planned problems"
expect 200 "composer values, pending" -b "$JAR" "$B/api/music/values?field=composer"
expect_body '["Bach","Händel"]' "values include pending ones"
expect 200 "overview pending"  -b "$JAR" "$B/api/music"
expect_body '"pending":14,' "overview counts pending changes"

expect 415 "queue needs json"  -b "$JAR" -d "{\"album\":$T1}" "$B$Q"
post "queue neither"           400 $Q '{}'
expect_body "give either 'album' and 'set', or 'edits'" "shape message"
post "queue both"              400 $Q "{\"album\":$T1,\"set\":{\"date\":\"1\"},\"edits\":[]}"
post "queue album bad id"      400 $Q '{"album":"x","set":{"date":"1"}}'
post "queue album id 0"        400 $Q '{"album":0,"set":{"date":"1"}}'
post "queue album missing"     404 $Q '{"album":999,"set":{"date":"1"}}'
post "queue no set"            400 $Q "{\"album\":$T1}"
post "queue set not object"    400 $Q "{\"album\":$T1,\"set\":[]}"
post "queue set empty"         400 $Q "{\"album\":$T1,\"set\":{}}"
post "queue unknown tag"       400 $Q "{\"album\":$T1,\"set\":{\"comment\":\"x\"}}"
expect_body "'comment' is not a tag that can be changed" "unknown tag message"
post "queue sort tag"          400 $Q "{\"album\":$T1,\"set\":{\"titlesort\":\"x\"}}"
post "queue navidrome id"      400 $Q "{\"album\":$T1,\"set\":{\"navidrome_id\":\"x\"}}"
post "queue tag twice"         400 $Q "{\"album\":$T1,\"set\":{\"date\":\"1\",\"date\":\"2\"}}"
expect_body "'date' is set twice" "twice message"
post "queue date not a year"   400 $Q "{\"album\":$T1,\"set\":{\"date\":\"1999-01-02\"}}"
expect_body "'date' must be a year" "date message"
post "queue date 0"            400 $Q "{\"album\":$T1,\"set\":{\"date\":\"0\"}}"
post "queue date empty"        400 $Q "{\"album\":$T1,\"set\":{\"date\":\"\"}}"
expect_body "'date' is required" "required message"
post "queue date number"       400 $Q "{\"album\":$T1,\"set\":{\"date\":1999}}"
post "queue genre capitals"    400 $Q "{\"album\":$T1,\"set\":{\"genre\":[\"Jazz\"]}}"
expect_body "'genre' may only use lowercase a-z, 0-9 and -" "genre message"
post "queue genre space"       400 $Q "{\"album\":$T1,\"set\":{\"genre\":[\"hip hop\"]}}"
post "queue genre string"      400 $Q "{\"album\":$T1,\"set\":{\"genre\":\"rock\"}}"
expect_body "'genre' must be a list of at most 64 strings" "list message"
post "queue genre none"        400 $Q "{\"album\":$T1,\"set\":{\"genre\":[]}}"
post "queue genre not strings" 400 $Q "{\"album\":$T1,\"set\":{\"genre\":[1]}}"
post "queue genre too many"    400 $Q "{\"album\":$T1,\"set\":{\"genre\":[$(seq -s, 1 65 | sed 's/[0-9]*/"g&"/g')]}}"
post "queue composer empty"    400 $Q "{\"album\":$T1,\"set\":{\"composer\":[\"\"]}}"
post "queue compilation yes"   400 $Q "{\"album\":$T1,\"set\":{\"compilation\":\"yes\"}}"
post "queue compilation bool"  400 $Q "{\"album\":$T1,\"set\":{\"compilation\":true}}"
post "queue bpm 0"             400 $Q "{\"album\":$T1,\"set\":{\"bpm\":\"0\"}}"
post "queue title empty"       400 $Q "{\"album\":$T1,\"set\":{\"title\":\"\"}}"
post "queue title newline"     400 $Q "{\"album\":$T1,\"set\":{\"title\":\"a\\nb\"}}"
post "queue title too long"    400 $Q "{\"album\":$T1,\"set\":{\"title\":\"$(printf '%0501d' 0)\"}}"
post "queue title max"         200 $Q "{\"album\":$T1,\"set\":{\"isrc\":\"$(printf '%0500d' 0)\"}}"
post "queue edits not list"    400 $Q '{"edits":{}}'
post "queue edits empty"       400 $Q '{"edits":[]}'
post "queue edits too many"    400 $Q "{\"edits\":[$(seq -s, 1 2001 | sed 's/[0-9]*/{}/g')]}"
post "queue edit not object"   400 $Q '{"edits":[1]}'
post "queue edit no track"     400 $Q '{"edits":[{"field":"title","value":"x"}]}'
post "queue edit no field"     400 $Q "{\"edits\":[{\"track\":$T1,\"value\":\"x\"}]}"
post "queue edit no value"     400 $Q "{\"edits\":[{\"track\":$T1,\"field\":\"title\"}]}"
post "queue edit bad number"   400 $Q "{\"edits\":[{\"track\":$T1,\"field\":\"tracknumber\",\"value\":\"4/3\"}]}"
expect_body "'tracknumber' must be two positive whole numbers like 3/12" "number message"
post "queue edit no total"     400 $Q "{\"edits\":[{\"track\":$T1,\"field\":\"discnumber\",\"value\":\"1\"}]}"
post "queue edit twice"        400 $Q "{\"edits\":[{\"track\":$T1,\"field\":\"bpm\",\"value\":\"1\"},{\"track\":$T1,\"field\":\"bpm\",\"value\":\"2\"}]}"
expect_body "'bpm' of one track is changed twice" "edit twice message"
post "queue edit missing track" 404 $Q '{"edits":[{"track":999,"field":"bpm","value":"1"}]}'
post "queue edit sort tag"     400 $Q "{\"edits\":[{\"track\":$T1,\"field\":\"albumsort\",\"value\":\"x\"}]}"

# The pending changes, and discarding a batch.
expect 200 "changes"           -b "$JAR" "$B/api/music/changes"
expect_body "\"track\":$T1,\"path\":\"$M/Artist/Album/01.mp3\",\"title\":\"Song One\",\"field\":\"date\",\"value\":\"1999\",\"now\":\"2001\"" "pending change with the file's value"
expect_body '"field":"genre","value":"[\"rock\",\"pop\"]","now":"[\"Rock\"]"' "list values as JSON"
expect_body '"history":[],"count":17' "count and empty history"
D=/api/music/discard
ISRC_BATCH=$(sqlite3 "$MDB" "SELECT DISTINCT batch FROM changes WHERE field = 'isrc'")
post "discard a batch"         200 $D "{\"batch\":$ISRC_BATCH}"
expect_body '{"discarded":3}' "discard counts"
post "discard again"           200 $D "{\"batch\":$ISRC_BATCH}"
expect_body '{"discarded":0}' "nothing left to discard"
post "discard no batch"        400 $D '{}'
post "discard bad batch"       400 $D '{"batch":"x"}'
post "discard batch 0"         400 $D '{"batch":0}'
expect 415 "discard needs json" -b "$JAR" -d '{"batch":1}' "$B$D"

# Starting the services: the password, then nothing may be running. (The
# right password with something to do runs the root action through sudo,
# which is not tried here: on a server it would start the real service.)
W=/api/music/write
S=/api/music/scan
post "write no password"       400 $W '{}'
post "write wrong password"    403 $W '{"password":"nope"}'
expect 415 "write needs json"  -b "$JAR" -d '{"password":"x"}' "$B$W"
post "scan no password"        400 $S '{}'
post "scan wrong password"     403 $S '{"password":"nope"}'
expect 415 "scan needs json"   -b "$JAR" -d '{"password":"x"}' "$B$S"
post "scan bad track"          400 $S "{$PW,\"track\":\"x\"}"
post "scan missing track"      404 $S "{$PW,\"track\":999}"
query "0" "refused scans queue nothing" "SELECT count(*) FROM scans WHERE state = 'queued'"

# While a service holds the library lock, the server writes nothing.
flock "$LOCK" sleep 8 &
LOCKER=$!
sleep 0.3
expect 200 "overview while busy" -b "$JAR" "$B/api/music"
expect_body '"busy":"' "overview shows the library busy"
post "queue while busy"        409 $Q "{\"album\":$T1,\"set\":{\"date\":\"2000\"}}"
expect_body "the library is busy" "busy message"
post "discard while busy"      409 $D '{"batch":1}'
post "scan while busy"         409 $S "{$PW}"
post "write while busy"        409 $W "{$PW}"
service 1 "music-write while busy" music-write
logged "the library is busy" "service refuses while another runs"
service 1 "music-scan while busy" music-scan
wait "$LOCKER"
# A short write by the server (shared lock): the service waits for it.
flock -s "$LOCK" sleep 1 &
LOCKER=$!
sleep 0.2

# The write service writes the pending changes, track by track: the disc
# numbers are missing, so each track is set to 1/1 (a warning).
service 0 "music-write"        music-write
logged "3 tracks written; 0 changes done, 14 with warnings, 0 failed" "write counts"
wait "$LOCKER"
if tail -c 128 "$M/Artist/Album/01.mp3" | head -c 3 | grep -q TAG; then
    PASSED=$((PASSED + 1)); else FAILED=$((FAILED + 1)); echo "FAIL: TagLib's ID3v1 tag removed"; fi
expect 200 "album after write" -b "$JAR" "$B/api/music/album?track=$T1"
expect_body '"tags":{"title":"New Title","album":"Some Album","artist":"Some Artist","albumartist":"Some Artist","tracknumber":"1/3","discnumber":"1/1","date":"1999","compilation":"0"' "written tags read back"
expect_body '"titlesort":"New Title","albumsort":null' "the sort tag of a changed tag is mirrored, others left"
expect_body '"composersort":"Bach; Händel"' "composer sort joins the composers"
expect_body '"genre":["rock","pop"],"composer":["Bach","Händel"]},"pending":{},"pictures":[],"missing":[],"invalid":[]' "lists written in order; nothing missing"
expect_body '"title":"Song Three"' "several titles replaced by one"
expect 200 "changes after write" -b "$JAR" "$B/api/music/changes"
expect_body '"pending":[],' "nothing pending after the write"
expect_body '"field":"title","value":"New Title",' "history keeps the change"
expect_body '"state":"warning","note":"disc number set to 1/1"' "history notes the warning"
query "14|14|0" "written changes are finished, with times" \
    "SELECT count(*), sum(done), sum(started IS NULL OR finished IS NULL) FROM changes"
post "write, nothing pending"  409 $W "{$PW}"
expect_body "no pending changes" "nothing to write message"

# A file changed after the last scan: its track is not written, and its
# cache is read again; the other tracks are written.
post "queue genre again"       200 $Q "{\"album\":$T1,\"set\":{\"genre\":[\"blues\"]}}"
sleep 1 # a cp in the second of the write's own reads would look unchanged anyway
cp tests/data/tagged.flac "$M/Artist/Album/02.FLAC"
cp "$M/Artist/Album/02.FLAC" "$TMP/before.flac"
service 0 "music-write, one file changed" music-write
logged "2 tracks written; 2 changes done, 0 with warnings, 1 failed" "write counts with a failure"
logged "the file changed since it was scanned (TRACKNUMBER); scan it again" "failure logged"
same_file "a refused write leaves the file alone" "$TMP/before.flac" "$M/Artist/Album/02.FLAC"
query "2" "the refused file is read again" "SELECT tracknumber FROM tracks WHERE id = $T2"

# The service checks the whole track itself: a bad value put in the table
# directly is not written.
sqlite3 "$MDB" "INSERT INTO changes (batch, track_id, field, value) VALUES (99, $T1, 'date', 'abc')"
service 0 "music-write, invalid value" music-write
logged "not written: DATE must be a year" "service validates values"
query "1999" "invalid value not written" "SELECT date FROM tracks WHERE id = $T1"
# ... and a track with an invalid track number is not written at all.
post "queue on a bad track"    200 $Q "{\"edits\":[{\"track\":$T2,\"field\":\"bpm\",\"value\":\"120\"}]}"
service 0 "music-write, bad track number" music-write
logged "not written: TRACKNUMBER must be two positive whole numbers" "track number blocks the track"

# A track removed by a scan: its pending change fails.
post "queue on the other folder" 200 $Q "{\"edits\":[{\"track\":$T3,\"field\":\"mood\",\"value\":\"calm\"}]}"
mv "$M/Artist/Multi" "$TMP/multi.away"
service 0 "scan after removing a folder" music-scan
logged " read, 1 removed, 1 failed" "removed file counted"
service 0 "music-write, file gone" music-write
logged "0 tracks written; 0 changes done, 0 with warnings, 1 failed" "changes for a removed track fail"
expect 200 "changes after removal" -b "$JAR" "$B/api/music/changes"
expect_body '"track":null,"path":null,"field":"mood","value":"calm",' "removed track in the history"
expect_body '"note":"the track is no longer in the library"' "removal note"
mv "$TMP/multi.away" "$M/Artist/Multi"

# Scans the web app queued run first, each its own path.
sqlite3 "$MDB" "INSERT INTO scans (path) VALUES ('$M/Artist/Multi'), ('$M/Artist/Album/01.mp3')"
service 0 "queued scans"       music-scan
logged "scanning $M/Artist/Multi" "first queued scan"
logged "scanning $M/Artist/Album/01.mp3" "second queued scan"
query "done,done" "queued scans ran" \
    "SELECT group_concat(state) FROM (SELECT state FROM scans ORDER BY id DESC LIMIT 2)"
query "$M/Artist/Album/01.mp3" "no whole scan added after them" \
    "SELECT path FROM scans ORDER BY id DESC LIMIT 1"
sqlite3 "$MDB" "INSERT INTO scans (path) VALUES ('/etc')"
service 1 "queued scan outside the library" music-scan
logged "is not in the music folder" "outside path refused"

# The folder is gone (drive not mounted): services and their start refuse.
post "queue for folder test"   200 $Q "{\"album\":$T1,\"set\":{\"bpm\":\"90\"}}"
mv "$M" "$TMP/music.away"
post "write, folder missing"   503 $W "{$PW}"
post "scan, folder missing"    503 $S "{$PW}"
service 1 "music-write, folder missing" music-write
expect 200 "overview, folder missing" -b "$JAR" "$B/api/music"
expect_body '"available":false,' "folder shown missing"
expect_body '"pending":3,' "pending changes kept while the folder is missing"
mv "$TMP/music.away" "$M"

# Album art: a track's pictures are stored once each by their SHA-256, with
# a thumbnail; art.flac has a JPEG front cover (600x600, "front") and a PNG
# back cover (50x30).
FRONT=14119064e0ef22b0b4207937be10d472a2cc547d58567e7c4bb203c0fe55ef15
BACK=468c143e2ba5751d19914f40386f1304e406d5f6d703afc2bf51ae3fdb481c47
ART="$TMP/data/music/art"
mkdir "$M/Artist/Art"
cp tests/data/art.flac "$M/Artist/Art/01.flac"
cp tests/data/art.flac "$M/Artist/Art/02.flac"
scan_path 0 "scan tracks with pictures" "$M/Artist/Art"
logged "2 files, 2 read, 0 removed, 0 failed" "pictures scan counts"
query "$FRONT|image/jpeg|2388|600|600|1
$BACK|image/png|142|50|30|1" "each picture stored once, with its size" \
    "SELECT hash, mime, size, width, height, thumb FROM art ORDER BY mime"
query "0|$FRONT|Front Cover|front
1|$BACK|Back Cover|" "a track's pictures in order" \
    "SELECT position, hash, type, description FROM track_pictures
     WHERE track_id = (SELECT id FROM tracks WHERE path LIKE '%Art/01.flac') ORDER BY position"
for f in "$FRONT" "$FRONT.thumb" "$BACK" "$BACK.thumb"; do
    if [ -f "$ART/$f" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $f not stored"; fi
done
if [ "$(od -An -tx1 -N3 "$ART/$BACK.thumb" | tr -d ' ')" = ffd8ff ]; then
    PASSED=$((PASSED + 1)); else FAILED=$((FAILED + 1)); echo "FAIL: thumbnail is not a JPEG"; fi
TA=$(sqlite3 "$MDB" "SELECT id FROM tracks WHERE path LIKE '%Art/01.flac'")
expect 200 "album with pictures" -b "$JAR" "$B/api/music/album?track=$TA"
expect_body "\"pictures\":[{\"hash\":\"$FRONT\",\"type\":\"Front Cover\",\"description\":\"front\",\"mime\":\"image/jpeg\",\"size\":2388,\"width\":600,\"height\":600,\"thumb\":1},{\"hash\":\"$BACK\",\"type\":\"Back Cover\"" "album lists the pictures"
# The pictures as images: the stored bytes, the type from the art table.
AR="$B/api/music/art"
expect 200 "front cover"        -b "$JAR" "$AR?hash=$FRONT&size=full"
if [ "$(sha256sum < "$TMP/body" | cut -c1-64)" = "$FRONT" ]; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: the picture sent is not the one stored"; fi
expect_header "content-type: image/jpeg"  "picture type"      -b "$JAR" "$AR?hash=$FRONT&size=full"
expect_header "cache-control: private, max-age=31536000, immutable" "picture cached" \
    -b "$JAR" "$AR?hash=$FRONT&size=full"
expect_header "x-content-type-options: nosniff" "picture nosniff" -b "$JAR" "$AR?hash=$FRONT&size=full"
expect_header "content-security-policy: default-src 'self'" "picture CSP" \
    -b "$JAR" "$AR?hash=$FRONT&size=full"
expect_header "content-type: image/png"   "PNG type"          -b "$JAR" "$AR?hash=$BACK&size=full"
expect 200 "thumbnail"          -b "$JAR" "$AR?hash=$BACK&size=thumb"
same_file "the thumbnail is the stored one" "$TMP/body" "$ART/$BACK.thumb"
expect_header "content-type: image/jpeg"  "thumbnail type"    -b "$JAR" "$AR?hash=$BACK&size=thumb"
sqlite3 "$MDB" "UPDATE art SET thumb = 0 WHERE hash = '$BACK'"
expect 200 "no thumbnail: the picture" -b "$JAR" "$AR?hash=$BACK&size=thumb"
same_file "the picture instead of a thumbnail" "$TMP/body" "$ART/$BACK"
sqlite3 "$MDB" "UPDATE art SET mime = NULL WHERE hash = '$BACK'"
expect 404 "a type nylm does not show" -b "$JAR" "$AR?hash=$BACK&size=full"
mv "$ART/$FRONT.thumb" "$TMP/thumb.away"
expect 404 "thumbnail file missing" -b "$JAR" "$AR?hash=$FRONT&size=thumb"
mv "$TMP/thumb.away" "$ART/$FRONT.thumb"
expect 404 "picture not stored" -b "$JAR" "$AR?hash=$(printf '%064d' 0)&size=full"
expect 400 "art no hash"        -b "$JAR" "$AR?size=full"
expect 400 "art hash short"     -b "$JAR" "$AR?hash=${FRONT%?}&size=full"
expect 400 "art hash long"      -b "$JAR" "$AR?hash=${FRONT}0&size=full"
expect 400 "art hash capitals"  -b "$JAR" "$AR?hash=$(echo "$FRONT" | tr a-f A-F)&size=full"
expect 400 "art hash path"      -b "$JAR" "$AR?hash=..%2F..%2F..%2Fetc%2Fpasswd&size=full"
expect_body "'hash' must be 64 lowercase hex characters" "hash message"
expect 400 "art no size"        -b "$JAR" "$AR?hash=$FRONT"
expect 400 "art bad size"       -b "$JAR" "$AR?hash=$FRONT&size=big"
expect_body "'size' must be full or thumb" "size message"
expect 405 "art POST"           -b "$JAR" -H "$J" -d '{}' "$AR?hash=$FRONT&size=full"

# Setting the album cover: the upload is stored and queued for every track
# of the album; the write service makes it each track's only picture.
C=/api/music/cover
COVER=d5f6219958dc70a334eed2ab216ad29f487608ffabd2af5d9a0578fd0e1a4fcd
# cover_json FILE: the body that sets the cover of track $TA's album to FILE.
cover_json() {
    printf '{"album":%s,"image":"%s"}' "$TA" "$(base64 -w0 "$1")" > "$TMP/cover.json"
}
BPM_BATCH=$(sqlite3 "$MDB" "SELECT DISTINCT batch FROM changes WHERE state = 'pending'")
post "discard before covers"   200 $D "{\"batch\":$BPM_BATCH}"
N=$(sqlite3 "$MDB" "SELECT count(*) FROM tracks WHERE album = 'Some Album'")
{ printf '\377\330\377'; head -c 716797 /dev/zero; } > "$TMP/max.jpg"
cover_json "$TMP/max.jpg"
expect 200 "cover of 700 KiB"  -b "$JAR" -H "$J" --data-binary "@$TMP/cover.json" "$B$C"
post "discard it"              200 $D "{\"batch\":$(sqlite3 "$MDB" "SELECT max(batch) FROM changes")}"
printf '\0' >> "$TMP/max.jpg"
cover_json "$TMP/max.jpg"
expect 413 "cover too big"     -b "$JAR" -H "$J" --data-binary "@$TMP/cover.json" "$B$C"
expect_body "bigger than 700 KiB" "too big message"
cover_json tests/data/cover.jpg
expect 415 "cover needs json"  -b "$JAR" --data-binary "@$TMP/cover.json" "$B$C"
post "cover no album"          400 $C '{"image":"/9j/"}'
post "cover album 0"           400 $C '{"album":0,"image":"/9j/"}'
post "cover album text"        400 $C '{"album":"x","image":"/9j/"}'
post "cover no image"          400 $C "{\"album\":$TA}"
post "cover image number"      400 $C "{\"album\":$TA,\"image\":1}"
post "cover image empty"       400 $C "{\"album\":$TA,\"image\":\"\"}"
post "cover not base64"        400 $C "{\"album\":$TA,\"image\":\"/9j!\"}"
expect_body "'image' must be base64" "base64 message"
post "cover base64 length"     400 $C "{\"album\":$TA,\"image\":\"/9j/4\"}"
post "cover base64 lines"      400 $C "{\"album\":$TA,\"image\":\"/9j/\\n4AAA\"}"
post "cover not a JPEG"        400 $C "{\"album\":$TA,\"image\":\"$(printf '\211PNG\r\n\032\n' | base64)\"}"
expect_body "'image' must be a JPEG picture" "JPEG message"
post "cover missing album"     404 $C '{"album":999,"image":"/9j/"}'
flock "$LOCK" sleep 1 &
LOCKER=$!
sleep 0.3
expect 409 "cover while busy"  -b "$JAR" -H "$J" --data-binary "@$TMP/cover.json" "$B$C"
wait "$LOCKER"
cp "$M/Artist/Art/01.flac" "$TMP/before.flac"
expect 200 "set the cover"     -b "$JAR" -H "$J" --data-binary "@$TMP/cover.json" "$B$C"
expect_body "\"queued\":$N,\"dropped\":0,\"hash\":\"$COVER\"}" "one change per track"
same_file "uploading leaves the file alone" "$TMP/before.flac" "$M/Artist/Art/01.flac"
same_file "the cover is stored as sent" tests/data/cover.jpg "$ART/$COVER"
query "image/jpeg|763||0" "the cover listed, not decoded" \
    "SELECT mime, size, width, thumb FROM art WHERE hash = '$COVER'"
expect 200 "new cover's thumbnail: the picture" -b "$JAR" "$AR?hash=$COVER&size=thumb"
same_file "the cover is sent" tests/data/cover.jpg "$TMP/body"
expect 200 "album with a new cover" -b "$JAR" "$B/api/music/album?track=$TA"
expect_body "\"pending\":{\"picture\":\"$COVER\"},\"pictures\":[{\"hash\":\"$FRONT\"" "the new cover is pending"
expect 200 "albums with a new cover" -b "$JAR" "$B/api/music/albums?track=$TA"
expect_body '"changed":["picture"]' "the album's cover changes"
expect 200 "changes with a cover" -b "$JAR" "$B/api/music/changes"
expect_body "\"field\":\"picture\",\"value\":\"$COVER\",\"now\":\"[\\\"$FRONT\\\",\\\"$BACK\\\"]\"" "the change shows the pictures now"
expect 200 "the same cover again" -b "$JAR" -H "$J" --data-binary "@$TMP/cover.json" "$B$C"
expect_body '"queued":0,"dropped":0' "nothing new to queue"
post "picture through queue"   400 $Q "{\"edits\":[{\"track\":$TA,\"field\":\"picture\",\"value\":\"$COVER\"}]}"

# The write: each valid track gets the cover as its only picture (the Art
# copies and 02.FLAC break the rules and are not written); the cover is
# decoded and gets a thumbnail.
service 0 "music-write, cover"  music-write
logged "2 tracks written; 2 changes done, 0 with warnings, 3 failed" "cover write counts"
query "0|$COVER|Front Cover|" "the cover is the only picture (FLAC)" \
    "SELECT position, hash, type, description FROM track_pictures
     WHERE track_id = (SELECT id FROM tracks WHERE path LIKE '%Multi/01.flac')"
query "0|$COVER|Front Cover|" "the cover is the only picture (MP3)" \
    "SELECT position, hash, type, description FROM track_pictures
     WHERE track_id = (SELECT id FROM tracks WHERE path LIKE '%Album/01.mp3')"
query "300|300|1" "the cover decoded" "SELECT width, height, thumb FROM art WHERE hash = '$COVER'"
if [ -f "$ART/$COVER.thumb" ]; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: no thumbnail of the cover"; fi
expect 200 "same cover, now in the files" -b "$JAR" -H "$J" --data-binary "@$TMP/cover.json" "$B$C"
expect_body '"queued":3,"dropped":0' "only the tracks without it are queued"
post "discard that"            200 $D "{\"batch\":$(sqlite3 "$MDB" "SELECT max(batch) FROM changes")}"

# A cover that does not decode is not written; the files stay as they were.
head -c 400 tests/data/cover.jpg > "$TMP/damaged.jpg"
cover_json "$TMP/damaged.jpg"
post "a damaged cover is queued" 200 $C "$(cat "$TMP/cover.json")"
cp "$M/Artist/Album/01.mp3" "$TMP/before.mp3"
service 0 "music-write, damaged cover" music-write
logged "not written: the picture can not be read" "damaged cover refused"
same_file "a refused cover leaves the file alone" "$TMP/before.mp3" "$M/Artist/Album/01.mp3"
query "$COVER" "a refused cover leaves the cache alone" \
    "SELECT hash FROM track_pictures
     WHERE track_id = (SELECT id FROM tracks WHERE path LIKE '%Album/01.mp3')"

# A scan of the whole library removes the pictures no track has.
rm -r "$M/Artist/Art"
service 0 "scan after removing the pictures" music-scan
logged "6 unused picture files removed" "unused picture files removed"
query "$COVER" "unused art rows removed" "SELECT hash FROM art"
expect 404 "removed picture"    -b "$JAR" "$AR?hash=$FRONT&size=full"

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
