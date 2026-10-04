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
# The server app's settings, as /etc/nylm.conf would have them.
export NYLM_UNITS="docker.service wg-quick@wg0" \
       NYLM_BACKUP="davis=/srv/davis immich=/srv/immich,/srv/immich-db"

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
for f in core/core.db plants/plants.db music/music.db server/server.db; do
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
             "POST /api/plants/log/update" "POST /api/plants/log/delete" \
             "GET /api/plants/photo?hash=$(printf '%064d' 0)&size=full" \
             "POST /api/plants/photos/add" "POST /api/plants/photos/delete"; do
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
expect_body "{\"entries\":[{\"id\":1,\"care_type_id\":1,\"date\":\"$TODAY\",\"note\":\"\",\"photos\":[]},{\"id\":2,\"care_type_id\":null,\"date\":\"2026-01-01\",\"note\":\"edited\",\"photos\":[]}],\"more\":false}" "log list newest first"
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

# Photos of log entries: a JPEG and its thumbnail, both from the browser.
PH=/api/plants/photos/add
PHOTOS="$TMP/data/plants/photos"
# photo LOG IMAGE THUMB: the body that adds IMAGE (thumbnail THUMB) to LOG.
photo() {
    printf '{"log_id":%s,"image":"%s","thumb":"%s"}' "$1" "$(base64 -w0 "$2")" \
        "$(base64 -w0 "$3")" > "$TMP/photo.json"
}
# has_file DESCRIPTION WANT PATH: PATH exists (WANT 1) or not (0).
has_file() {
    if [ -e "$3" ]; then got=1; else got=0; fi
    if [ "$got" = "$2" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $1: $3 exists: $got"; fi
}
for n in 1 2 3 4 5; do
    { cat tests/data/cover.jpg; printf '%s' "$n"; } > "$TMP/p$n.jpg"
done
H1=$(sha256sum "$TMP/p1.jpg" | cut -c1-64)
photo 1 "$TMP/p1.jpg" tests/data/cover.jpg
expect 201 "photo add"         -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
expect_body "{\"hash\":\"$H1\"}" "photo add answers its hash"
has_file "the photo is stored" 1 "$PHOTOS/$H1"
has_file "its thumbnail is stored" 1 "$PHOTOS/$H1.thumb"
expect 409 "photo twice"       -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
expect_body "this photo is already in the entry" "twice message"
for n in 2 3 4; do
    photo 1 "$TMP/p$n.jpg" tests/data/cover.jpg
    expect 201 "photo add $n"  -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
done
photo 1 "$TMP/p5.jpg" tests/data/cover.jpg
expect 409 "photo 5"           -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
expect_body "an entry has at most 4 photos" "photo limit message"
expect 200 "log with photos"   -b "$JAR" "$B/api/plants/log?plant_id=1"
expect_body "\"note\":\"\",\"photos\":[\"$H1\",\"$(sha256sum "$TMP/p2.jpg" | cut -c1-64)\"," "photos in order"
expect 200 "photo full"        -b "$JAR" "$B/api/plants/photo?hash=$H1&size=full"
if cmp -s "$TMP/body" "$TMP/p1.jpg"; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: the photo is not sent as stored"; fi
expect_header "content-type: image/jpeg" "photo type" -b "$JAR" "$B/api/plants/photo?hash=$H1&size=thumb"
expect_header "cache-control: private, max-age=31536000, immutable" "photo cached" -b "$JAR" "$B/api/plants/photo?hash=$H1&size=thumb"
expect 200 "photo thumb"       -b "$JAR" "$B/api/plants/photo?hash=$H1&size=thumb"
if cmp -s "$TMP/body" tests/data/cover.jpg; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: the thumbnail is not sent as stored"; fi
expect 404 "photo not listed"  -b "$JAR" "$B/api/plants/photo?hash=$(printf '%064d' 0)&size=full"
expect 400 "photo bad hash"    -b "$JAR" "$B/api/plants/photo?hash=../x&size=full"
expect 400 "photo upper hash"  -b "$JAR" "$B/api/plants/photo?hash=$(echo "$H1" | tr a-f A-F)&size=full"
expect 400 "photo bad size"    -b "$JAR" "$B/api/plants/photo?hash=$H1&size=big"
expect 400 "photo no size"     -b "$JAR" "$B/api/plants/photo?hash=$H1"
# The checks of an upload.
post "photo no log"            400 $PH '{"image":"","thumb":""}'
photo 999 "$TMP/p5.jpg" tests/data/cover.jpg
expect 404 "photo log missing" -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
post "photo no image"          400 $PH '{"log_id":2,"thumb":"/9j/"}'
expect_body "'image' must be a JPEG picture in base64" "no image message"
post "photo no thumb"          400 $PH '{"log_id":2,"image":"/9j/"}'
post "photo not base64"        400 $PH '{"log_id":2,"image":"!!!!","thumb":"/9j/"}'
expect_body "'image' must be base64" "base64 message"
post "photo not a JPEG"        400 $PH "{\"log_id\":2,\"image\":\"$(printf 'GIF89a' | base64)\",\"thumb\":\"/9j/\"}"
expect_body "'image' must be a JPEG picture" "not JPEG message"
post "photo thumb not a JPEG"  400 $PH "{\"log_id\":2,\"image\":\"/9j/\",\"thumb\":\"$(printf 'GIF89a' | base64)\"}"
{ printf '\377\330\377'; head -c 655357 /dev/zero; } > "$TMP/max.jpg"
{ printf '\377\330\377'; head -c 65533 /dev/zero; } > "$TMP/maxthumb.jpg"
photo 2 "$TMP/max.jpg" "$TMP/maxthumb.jpg"
expect 201 "photo of 640 KiB, thumbnail of 64 KiB" -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
printf '\0' >> "$TMP/max.jpg"
photo 2 "$TMP/max.jpg" tests/data/cover.jpg
expect 413 "photo too big"     -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
expect_body "'image' is bigger than 640 KiB" "too big message"
printf '\0' >> "$TMP/maxthumb.jpg"
photo 2 "$TMP/p5.jpg" "$TMP/maxthumb.jpg"
expect 413 "thumbnail too big" -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
# A photo in two entries: its files stay until neither has it.
photo 2 "$TMP/p1.jpg" tests/data/cover.jpg
expect 201 "same photo, other entry" -b "$JAR" -H "$J" --data-binary "@$TMP/photo.json" "$B$PH"
PD=/api/plants/photos/delete
post "photo delete"            204 $PD "{\"log_id\":1,\"hash\":\"$H1\"}"
post "photo delete again"      404 $PD "{\"log_id\":1,\"hash\":\"$H1\"}"
post "photo delete bad hash"   400 $PD '{"log_id":1,"hash":"x"}'
post "photo delete no log"     400 $PD "{\"hash\":\"$H1\"}"
has_file "a photo another entry has stays" 1 "$PHOTOS/$H1"
post "photo delete, the other" 204 $PD "{\"log_id\":2,\"hash\":\"$H1\"}"
has_file "an unused photo goes" 0 "$PHOTOS/$H1"
has_file "and its thumbnail" 0 "$PHOTOS/$H1.thumb"
H2=$(sha256sum "$TMP/p2.jpg" | cut -c1-64)
has_file "the entry's other photos stay" 1 "$PHOTOS/$H2"

post "log delete"              204 /api/plants/log/delete '{"id":1}\'
has_file "deleting an entry removes its photos" 0 "$PHOTOS/$H2"
query_plants() { sqlite3 "$TMP/data/plants/plants.db" "$1"; }
if [ "$(query_plants "SELECT count(*) FROM care_photos WHERE log_id = 1")" = 0 ]; then
    PASSED=$((PASSED + 1)); else FAILED=$((FAILED + 1)); echo "FAIL: photo rows left"; fi
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
             "GET /api/music/values?field=artist" "GET /api/music/changes" "GET /api/music/charts" \
             "POST /api/music/queue" "POST /api/music/discard" "POST /api/music/scan" \
             "POST /api/music/write" "GET /api/music/art?hash=$(printf '%064d' 0)&size=full" \
             "POST /api/music/cover" "GET /api/music/moves" "POST /api/music/move" \
             "GET /api/music/qobuz" "POST /api/music/qobuz/start" "GET /api/music/duplicates" \
             "POST /api/music/merge"; do
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
expect_body '"album":"Some Album","albumartist":"Some Artist","date":"2001","compilation":"0","genre":null,"composer":[],"tracks":3,"mixed":["genre"],"changed":[],"missing":["discnumber","composer"],"invalid":["tracknumber","genre"],"several_artists":true,"no_art":true,"cover":null,"mixed_art":false}]' "album row"
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
expect 200 "charts"            -b "$JAR" "$B/api/music/charts"
expect_body '{"genres":[{"genre":"Pop","albums":1},{"genre":"Rock","albums":1}],"dates":[{"date":"2001","albums":1}]}' "charts: albums by genre and date"

# Possible duplicates, by the planned tags; merging names queues changes.
expect 200 "duplicates"        -b "$JAR" "$B/api/music/duplicates"
expect_body "\"name\":{\"rows\":[{\"key\":\"somealbum someartist\",\"id\":$T2,\"album\":\"Some Album\",\"albumartist\":\"Some Artist\",\"folder\":\"$M/Artist/Album\",\"tracks\":2},{\"key\":\"somealbum someartist\",\"id\":$T3,\"album\":\"Some Album\",\"albumartist\":\"Some Artist\",\"folder\":\"$M/Artist/Multi\",\"tracks\":1}],\"more\":false}" "one album in two folders"
expect_body '"names":{"artist":{"rows":[],"more":false},"albumartist":{"rows":[],"more":false},"composer":{"rows":[],"more":false},"genre":{"rows":[],"more":false}}' "no name duplicates yet"
DQ="{\"edits\":[{\"track\":$T1,\"field\":\"isrc\",\"value\":\"USAAA0000001\"},{\"track\":$T2,\"field\":\"isrc\",\"value\":\"us-aaa-00-00001\"}"
DQ="$DQ,{\"track\":$T1,\"field\":\"musicbrainz_trackid\",\"value\":\"ABC-1\"},{\"track\":$T3,\"field\":\"musicbrainz_trackid\",\"value\":\"abc1\"}"
DQ="$DQ,{\"track\":$T1,\"field\":\"title\",\"value\":\"Song\"},{\"track\":$T2,\"field\":\"title\",\"value\":\"song!\"},{\"track\":$T2,\"field\":\"artist\",\"value\":\"some artist\"}"
DQ="$DQ,{\"track\":$T1,\"field\":\"genre\",\"value\":[\"hiphop\",\"rock\"]},{\"track\":$T2,\"field\":\"genre\",\"value\":[\"hip-hop\"]}"
DQ="$DQ,{\"track\":$T1,\"field\":\"composer\",\"value\":[\"Händel\"]},{\"track\":$T2,\"field\":\"composer\",\"value\":[\"Handel\"]}"
DQ="$DQ,{\"track\":$T3,\"field\":\"albumartist\",\"value\":\"Other Artist\"},{\"track\":$T1,\"field\":\"barcode\",\"value\":\"0123\"},{\"track\":$T3,\"field\":\"barcode\",\"value\":\"0123\"}]}"
post "queue duplicates"        200 /api/music/queue "$DQ"
expect 200 "duplicates planned" -b "$JAR" "$B/api/music/duplicates"
expect_body "\"trackid\":{\"rows\":[{\"key\":\"abc1\",\"id\":$T1,\"path\":\"$M/Artist/Album/01.mp3\",\"artist\":\"Some Artist\",\"title\":\"Song\",\"album\":\"Some Album\",\"albumartist\":\"Some Artist\"},{\"key\":\"abc1\",\"id\":$T3," "same MusicBrainz track id"
expect_body "\"isrc\":{\"rows\":[{\"key\":\"usaaa0000001\",\"id\":$T1," "same ISRC"
expect_body "\"key\":\"usaaa0000001\",\"id\":$T2,\"path\":\"$M/Artist/Album/02.FLAC\",\"artist\":\"some artist\",\"title\":\"song!\"" "ISRC with planned tags"
expect_body "\"title\":{\"rows\":[{\"key\":\"someartist song\",\"id\":$T1," "same artist and title"
expect_body "\"barcode\":{\"rows\":[{\"key\":\"0123\",\"id\":$T3,\"album\":\"Some Album\",\"albumartist\":\"Other Artist\",\"folder\":\"$M/Artist/Multi\",\"tracks\":1},{\"key\":\"0123\",\"id\":$T1,\"album\":\"Some Album\",\"albumartist\":\"Some Artist\"" "same barcode"
expect_body '"name":{"rows":[],"more":false}' "album artists differ: not the same album"
expect_body "\"title\":{\"rows\":[{\"key\":\"somealbum\",\"id\":$T3,\"album\":\"Some Album\",\"albumartist\":\"Other Artist\",\"folder\":\"$M/Artist/Multi\",\"tracks\":1},{\"key\":\"somealbum\",\"id\":$T2,\"album\":\"Some Album\",\"albumartist\":\"Some Artist\",\"folder\":\"$M/Artist/Album\",\"tracks\":2}],\"more\":false}" "same album, other album artist"
expect_body '"artist":{"rows":[{"key":"someartist","value":"Some Artist","tracks":1},{"key":"someartist","value":"some artist","tracks":1}],"more":false}' "artist spellings"
expect_body '"composer":{"rows":[{"key":"handel","value":"Handel","tracks":1},{"key":"handel","value":"Händel","tracks":1}],"more":false}' "composer spellings"
expect_body '"genre":{"rows":[{"key":"hiphop","value":"hip-hop","tracks":1},{"key":"hiphop","value":"hiphop","tracks":1},{"key":"rock","value":"Rock","tracks":1},{"key":"rock","value":"rock","tracks":1}],"more":false}' "genre spellings, a group each"

MG=/api/music/merge
cp "$M/Artist/Album/01.mp3" "$TMP/before.mp3"
post "merge artist"            200 $MG '{"field":"artist","from":["some artist"],"to":"Some Artist"}'
expect_body '"queued":0,"dropped":1,"skipped":0' "back to the file's artist drops the change"
post "merge genre"             200 $MG '{"field":"genre","from":["hiphop"],"to":"hip-hop"}'
expect_body '"queued":1,"dropped":0,"skipped":0' "genre merged in a list"
post "merge composer"          200 $MG '{"field":"composer","from":["Handel"],"to":"Händel"}'
expect_body '"queued":1,"dropped":0,"skipped":0' "composer merged"
post "merge genre, invalid list" 200 $MG '{"field":"genre","from":["Pop"],"to":"pop"}'
expect_body '"queued":0,"dropped":0,"skipped":1' "a list with another invalid genre is skipped"
post "merge nothing to do"     200 $MG '{"field":"albumartist","from":["SOME ARTIST"],"to":"Some Artist"}'
expect_body '"batch":-1,"queued":0,"dropped":0,"skipped":0' "no track has it"
same_file "merging leaves the file alone" "$TMP/before.mp3" "$M/Artist/Album/01.mp3"
query "[\"hip-hop\",\"rock\"]|[\"Händel\"]" "merged lists queued" \
    "SELECT group_concat(value, '|') FROM (SELECT value FROM changes WHERE state = 'pending'
     AND track_id = $T1 AND field IN ('genre', 'composer') ORDER BY field DESC)"
expect 200 "duplicates merged" -b "$JAR" "$B/api/music/duplicates"
expect_body '"names":{"artist":{"rows":[],"more":false},"albumartist":{"rows":[],"more":false},"composer":{"rows":[],"more":false},"genre":{"rows":[{"key":"rock","value":"Rock","tracks":1},{"key":"rock","value":"rock","tracks":1}],"more":false}}' "merged names are gone, the skipped one stays"

expect 415 "merge needs json"  -b "$JAR" -d '{"field":"artist"}' "$B$MG"
post "merge no field"          400 $MG '{"from":["a"],"to":"A"}'
expect_body "'field' must be artist, albumartist, composer or genre" "merge field message"
post "merge title"             400 $MG '{"field":"title","from":["a"],"to":"A"}'
post "merge field number"      400 $MG '{"field":1,"from":["a"],"to":"A"}'
post "merge no to"             400 $MG '{"field":"artist","from":["a"]}'
expect_body "'to' must be a string" "merge to message"
post "merge to list"           400 $MG '{"field":"genre","from":["a"],"to":["a"]}'
post "merge to empty"          400 $MG '{"field":"artist","from":["a"],"to":""}'
expect_body "'artist' is required" "merge to empty message"
post "merge to breaks a rule"  400 $MG '{"field":"genre","from":["hip-hop"],"to":"Hip Hop"}'
expect_body "'genre' may only use lowercase a-z, 0-9 and -" "merge to rule message"
post "merge to too long"       400 $MG "{\"field\":\"artist\",\"from\":[\"a\"],\"to\":\"$(printf '%0501d' 0)\"}"
post "merge no from"           400 $MG '{"field":"artist","to":"A"}'
expect_body "'from' must be a list of 1 to 64 names" "merge from message"
post "merge from string"       400 $MG '{"field":"artist","from":"a","to":"A"}'
post "merge from empty"        400 $MG '{"field":"artist","from":[],"to":"A"}'
post "merge from 65"           400 $MG "{\"field\":\"artist\",\"from\":[$(seq -s, 1 65 | sed 's/[0-9]*/"a"/g')],\"to\":\"A\"}"
post "merge from 64"           200 $MG "{\"field\":\"artist\",\"from\":[$(seq 64 | while read -r i; do printf '"a%*s",' "$i" ''; done | sed 's/,$//')],\"to\":\"A\"}"
post "merge from number"       400 $MG '{"field":"artist","from":[1],"to":"A"}'
expect_body "each name in 'from' must be text of 1 to 4096 bytes" "merge name message"
post "merge from empty name"   400 $MG '{"field":"artist","from":[""],"to":"A"}'
post "merge from control"      400 $MG '{"field":"artist","from":["a\u0001"],"to":"A"}'
post "merge from too long"     400 $MG "{\"field\":\"artist\",\"from\":[\"0$(printf '%4096s' '')\"],\"to\":\"0\"}"
post "merge from longest"      200 $MG "{\"field\":\"artist\",\"from\":[\"0$(printf '%4095s' '')\"],\"to\":\"0\"}"
post "merge from is to"        400 $MG '{"field":"artist","from":["A"],"to":"A"}'
expect_body "each name in 'from' must be given once, and not be 'to'" "merge once message"
post "merge from twice"        400 $MG '{"field":"artist","from":["a","a"],"to":"A"}'
post "merge another name"      400 $MG '{"field":"artist","from":["Other Artist"],"to":"Some Artist"}'
expect_body "'Other Artist' is not another spelling of 'Some Artist'" "merge spelling message"
post "merge no letters"        400 $MG '{"field":"artist","from":["?"],"to":"!"}'
post "discard the duplicates"  200 /api/music/discard '{"all":true}'
query "0" "nothing pending after the duplicates" "SELECT count(*) FROM changes"

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

# The pending changes by album and track, and discarding them.
expect 200 "changes"           -b "$JAR" "$B/api/music/changes"
expect_body "\"track\":$T1,\"path\":\"$M/Artist/Album/01.mp3\",\"album\":\"Some Album\",\"albumartist\":\"Some Artist\",\"title\":\"Song One\",\"field\":\"date\",\"value\":\"1999\",\"now\":\"2001\"" "pending change with its album and the file's value"
expect_body '"field":"genre","value":"[\"rock\",\"pop\"]","now":"[\"Rock\"]"' "list values as JSON"
expect_body '"history":[],"count":17' "count and empty history"
D=/api/music/discard
post "isrc back as in the files" 200 $Q "{\"album\":$T1,\"set\":{\"isrc\":\"\"}}"
expect_body '"queued":0,"dropped":3' "back to the files' value drops the changes"
post "discard no key"          400 $D '{}'
expect_body "give exactly one of 'track', 'album', 'removed' or 'all'" "discard message"
post "discard two keys"        400 $D "{\"track\":$T1,\"album\":$T1}"
post "discard bad track"       400 $D '{"track":"x"}'
post "discard track 0"         400 $D '{"track":0}'
post "discard missing track"   404 $D '{"track":999}'
post "discard bad album"       400 $D '{"album":-1}'
post "discard missing album"   404 $D '{"album":999}'
post "discard removed false"   400 $D '{"removed":false}'
post "discard removed number"  400 $D '{"removed":1}'
post "discard removed"         200 $D '{"removed":true}'
expect_body '{"discarded":0}' "no removed tracks"
post "discard all false"       400 $D '{"all":false}'
expect_body "'all' must be true" "all message"
post "discard all and a track" 400 $D "{\"all\":true,\"track\":$T1}"
expect 415 "discard needs json" -b "$JAR" -d "{\"track\":$T1}" "$B$D"

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
flock "$LOCK" sleep 12 &
LOCKER=$!
sleep 0.3
expect 200 "overview while busy" -b "$JAR" "$B/api/music"
expect_body '"busy":"' "overview shows the library busy"
post "queue while busy"        409 $Q "{\"album\":$T1,\"set\":{\"date\":\"2000\"}}"
expect_body "the library is busy" "busy message"
post "discard while busy"      409 $D "{\"track\":$T1}"
post "merge while busy"        409 /api/music/merge '{"field":"artist","from":["some artist"],"to":"Some Artist"}'
post "scan while busy"         409 $S "{$PW}"
post "write while busy"        409 $W "{$PW}"
post "move while busy"         409 /api/music/move "{$PW}"
service 1 "music-move while busy" music-move
logged "the library is busy" "move refuses while another service runs"
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
post "queue to discard"         200 $Q "{\"album\":$TA,\"set\":{\"mood\":\"gone\"}}"
post "discard a track"         200 $D "{\"track\":$TA}"
expect_body '{"discarded":1}' "discard counts"
query "0|1" "the track has no pending changes, the others keep theirs" \
    "SELECT sum(track_id = $TA), count(*) > 1 FROM changes WHERE state = 'pending'"
post "discard the track again" 200 $D "{\"track\":$TA}"
expect_body '{"discarded":0}' "nothing left to discard"
post "discard before covers"   200 $D "{\"album\":$TA}"
query "0" "the album has no pending changes" "SELECT count(*) FROM changes WHERE state = 'pending'"
N=$(sqlite3 "$MDB" "SELECT count(*) FROM tracks WHERE album = 'Some Album'")
{ printf '\377\330\377'; head -c 716797 /dev/zero; } > "$TMP/max.jpg"
cover_json "$TMP/max.jpg"
expect 200 "cover of 700 KiB"  -b "$JAR" -H "$J" --data-binary "@$TMP/cover.json" "$B$C"
post "discard all"             200 $D '{"all":true}'
query "0" "nothing pending after discarding all" \
    "SELECT count(*) FROM changes WHERE state = 'pending'"
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
expect_body "\"changed\":[\"picture\"],\"missing\":[\"discnumber\",\"composer\"],\"invalid\":[\"tracknumber\",\"genre\"],\"several_artists\":true,\"no_art\":false,\"cover\":\"$COVER\",\"mixed_art\":false}" "the album's planned cover"
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
expect 200 "albums after the cover" -b "$JAR" "$B/api/music/albums?track=$TA"
expect_body "\"no_art\":true,\"cover\":\"$COVER\",\"mixed_art\":true}" "the art now differs between tracks"
if [ -f "$ART/$COVER.thumb" ]; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: no thumbnail of the cover"; fi
expect 200 "same cover, now in the files" -b "$JAR" -H "$J" --data-binary "@$TMP/cover.json" "$B$C"
expect_body '"queued":3,"dropped":0' "only the tracks without it are queued"
post "discard that"            200 $D "{\"album\":$TA}"

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

# Moving the files where their tags put them. The move reads only the
# cache: the tags are set there. Album/ goes to Some Artist/Some Album/,
# Multi/ to AC_DC_ Live/Some Album/ (unsafe characters), each with its
# other files; the emptied folders go. A track without an album artist
# can not move.
MV=/api/music/move
sqlite3 "$MDB" "UPDATE tracks SET title = 'Song Two', tracknumber = '2/3', discnumber = NULL
                WHERE path LIKE '%Album/02.FLAC';
                UPDATE tracks SET title = 'Song One', tracknumber = '1/3', discnumber = '1/1'
                WHERE path LIKE '%Album/01.mp3';
                UPDATE tracks SET title = 'Song Three', tracknumber = '3/3', discnumber = '1/1',
                albumartist = 'AC/DC: Live' WHERE path LIKE '%Multi/01.flac';
                INSERT INTO tracks (path, size, ext, scanned, title, album, tracknumber)
                VALUES ('$M/Loose/x.mp3', 1, 'mp3', 0, 'X', 'Y', '1/1'),
                ('$TMP/elsewhere/y.mp3', 1, 'mp3', 0, 'X', 'Y', '1/1')"
sqlite3 "$MDB" "UPDATE tracks SET albumartist = 'Z' WHERE path LIKE '%elsewhere/y.mp3'"
expect 200 "moves planned"     -b "$JAR" "$B/api/music/moves"
expect_body "\"moves\":[{\"track\":$T1,\"from\":\"$M/Artist/Album/01.mp3\",\"to\":\"$M/Some Artist/Some Album/01 - Song One.mp3\"}," "a planned move"
expect_body "\"to\":\"$M/Some Artist/Some Album/02 - Song Two.flac\"}" "the extension in lower case"
expect_body "\"to\":\"$M/AC_DC_ Live/Some Album/03 - Song Three.flac\"}" "unsafe characters replaced"
expect_body "\"problems\":[{\"track\":" "a problem listed"
expect_body "\"path\":\"$M/Loose/x.mp3\",\"problem\":\"it has no album artist\"}],\"history\":[],\"pending\":0,\"count\":3,\"problem_count\":2}" "the plan's counts"
expect_body "\"path\":\"$TMP/elsewhere/y.mp3\",\"problem\":\"it is not in the music folder: scan the library\"}" "a track outside the music folder stays"
post "move no password"        400 $MV '{}'
post "move wrong password"     403 $MV '{"password":"nope"}'
expect 415 "move needs json"   -b "$JAR" -d '{"password":"x"}' "$B$MV"
post "queue before a move"     200 $Q "{\"album\":$T1,\"set\":{\"mood\":\"wait\"}}"
post "move with changes pending" 409 $MV "{$PW}"
expect_body "write or discard the pending changes first" "pending message"
expect 200 "moves with changes pending" -b "$JAR" "$B/api/music/moves"
expect_body '"pending":2,' "the plan says changes are pending"
post "discard before the move" 200 $D "{\"album\":$T1}"
touch "$M/Artist/Album/.hidden"
service 0 "music-move"         music-move
logged "3 tracks moved, 0 failed, 2 can not move; 4 other files moved, 0 kept; 2 empty folders removed" "move counts"
for f in "Some Artist/Some Album/01 - Song One.mp3" "Some Artist/Some Album/02 - Song Two.flac" \
         "Some Artist/Some Album/03.mp3" "Some Artist/Some Album/cover.jpg" \
         "Some Artist/Some Album/.hidden" "AC_DC_ Live/Some Album/03 - Song Three.flac" \
         "AC_DC_ Live/Some Album/Cover.JPG" "Artist/README"; do
    if [ -f "$M/$f" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: not moved there: $f"; fi
done
if [ ! -e "$M/Artist/Album" ] && [ ! -e "$M/Artist/Multi" ]; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: emptied folders not removed"; fi
query "$M/Some Artist/Some Album/01 - Song One.mp3" "the cache has the new path" \
    "SELECT path FROM tracks WHERE id = $T1"
query "done|3|4|0" "moves recorded" \
    "SELECT state, sum(track_id IS NOT NULL), sum(track_id IS NULL), sum(note <> '')
     FROM moves GROUP BY state"
expect 200 "moves after the move" -b "$JAR" "$B/api/music/moves"
expect_body '"pending":0,"count":0,"problem_count":2}' "nothing left to move"
expect_body "\"from_path\":\"$M/Artist/Album/01.mp3\",\"to_path\":\"$M/Some Artist/Some Album/01 - Song One.mp3\",\"state\":\"done\"" "history"
post "move with nothing to move" 409 $MV "{$PW}"
expect_body "there is nothing to move" "nothing to move message"
# Never over an existing file: one is in the way, the track stays.
sqlite3 "$MDB" "DELETE FROM tracks WHERE path LIKE '%Loose/x.mp3' OR path LIKE '%elsewhere/y.mp3';
                UPDATE tracks SET title = 'Other' WHERE path LIKE '%03 - Song Three.flac'"
echo "in the way" > "$M/AC_DC_ Live/Some Album/03 - Other.flac"
service 0 "music-move, target taken" music-move
logged "0 tracks moved, 1 failed, 0 can not move" "a taken target fails"
query "failed|not moved: a file is already there" "the failure recorded" \
    "SELECT state, note FROM moves ORDER BY id DESC LIMIT 1"
if [ "$(cat "$M/AC_DC_ Live/Some Album/03 - Other.flac")" = "in the way" ]; then
    PASSED=$((PASSED + 1)); else FAILED=$((FAILED + 1)); echo "FAIL: a file was replaced"; fi
rm "$M/AC_DC_ Live/Some Album/03 - Other.flac"
service 0 "scan after the moves" music-scan
logged "0 removed" "the scan finds every moved track"

# Qobuz: the account and downloads; starting it is checked up to the root
# action (the service talks to Qobuz, which these tests do not).
QZ=/api/music/qobuz/start
QDB() { sqlite3 "$MDB" "SELECT (SELECT coalesce(login, '-') FROM qobuz_account),
                               (SELECT count(*) FROM qobuz_downloads)"; }
expect 200 "qobuz"             -b "$JAR" "$B/api/music/qobuz"
expect_body '{"running":false,"login_url":null,"connected":false,"label":null,"login_pending":false,"error":"","downloads":[]}' "qobuz, never run"
post "qobuz no password"       400 $QZ '{}'
post "qobuz wrong password"    403 $QZ '{"password":"nope"}'
expect 415 "qobuz needs json"  -b "$JAR" -d '{"password":"x"}' "$B$QZ"
post "qobuz login not text"    400 $QZ "{$PW,\"login\":1}"
expect_body "'login' must be the address Qobuz sent you to" "login message"
post "qobuz login no code"     400 $QZ "{$PW,\"login\":\"http://localhost/?x=1\"}"
post "qobuz login space"       400 $QZ "{$PW,\"login\":\"a b\"}"
post "qobuz login too long"    400 $QZ "{$PW,\"login\":\"$(printf '%02049d' 0)\"}"
post "qobuz urls not a list"   400 $QZ "{$PW,\"urls\":\"x\"}"
expect_body "'urls' must be a list of 1 to 50 Qobuz album links" "urls message"
post "qobuz urls empty"        400 $QZ "{$PW,\"urls\":[]}"
post "qobuz urls 51"           400 $QZ "{$PW,\"urls\":[$(seq -s, 1 51 | sed 's|[0-9]*|"https://open.qobuz.com/album/a&"|g')]}"
post "qobuz url not a link"    400 $QZ "{$PW,\"urls\":[\"https://open.qobuz.com/album/a\",\"https://evil.com/album/b\"]}"
expect_body "link 2 is not a Qobuz album link" "link message"
post "qobuz url a track"       400 $QZ "{$PW,\"urls\":[\"https://www.qobuz.com/us-en/track/x/1\"]}"
post "qobuz urls not text"     400 $QZ "{$PW,\"urls\":[1]}"
post "qobuz urls, not connected" 409 $QZ "{$PW,\"urls\":[\"https://open.qobuz.com/album/a\"]}"
expect_body "connect to Qobuz first" "connect message"
flock "$TMP/data/music/qobuz.lock" sleep 2 &
LOCKER=$!
sleep 0.3
expect 200 "qobuz while running" -b "$JAR" "$B/api/music/qobuz"
expect_body '{"running":true,' "qobuz shows it runs"
post "qobuz start while running" 409 $QZ "{$PW,\"login\":\"http://localhost/?code=abc\"}"
expect_body "Qobuz is busy" "busy message"
wait "$LOCKER"
if [ "$(QDB)" = "-|0" ]; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: refused Qobuz requests changed the database: $(QDB)"; fi
# What the service stored shows; the token never does.
sqlite3 "$MDB" "UPDATE qobuz_account SET app_id = '798273057', token = 'SECRET-TOKEN',
                user_id = '42', label = 'Studio', error = 'old';
                INSERT INTO qobuz_downloads (album_id, title, state, tracks, saved, note)
                VALUES ('abc', 'Some Album', 'warning', 3, 2, 'track 3: only a sample')"
expect 200 "qobuz, connected"  -b "$JAR" "$B/api/music/qobuz"
expect_body '"login_url":"https://www.qobuz.com/signin/oauth?ext_app_id=798273057&redirect_url=http://localhost","connected":true,"label":"Studio","login_pending":false,"error":"old"' "qobuz account"
expect_body '"album_id":"abc","title":"Some Album","artist":null,' "qobuz download"
expect_body '"state":"warning","tracks":3,"saved":2,"note":"track 3: only a sample"}]}' "qobuz download result"
if grep -q "SECRET-TOKEN" "$TMP/body"; then FAILED=$((FAILED + 1)); echo "FAIL: the token was sent"
else PASSED=$((PASSED + 1)); fi
# nylm-qobuz: no arguments, and it needs NYLM_MUSIC (it stops before the network).
QBIN=$(dirname "$BIN")/nylm-qobuz${BIN##*nylm}
if "$QBIN" x >/dev/null 2>&1; then FAILED=$((FAILED + 1)); echo "FAIL: nylm-qobuz takes an argument"
else PASSED=$((PASSED + 1)); fi
if env NYLM_MUSIC= "$QBIN" >"$TMP/service.log" 2>&1; then
    FAILED=$((FAILED + 1)); echo "FAIL: nylm-qobuz ran without NYLM_MUSIC"
else PASSED=$((PASSED + 1)); fi
logged "NYLM_MUSIC is not set" "nylm-qobuz says why"

# ---- server ------------------------------------------------------------------

# Every route of the server app needs a login.
for route in "GET /api/server" "GET /api/server/audit" "GET /api/server/log?unit=nylm.service" \
             "GET /api/server/disk-usage" "POST /api/server/disk-usage" "GET /api/server/smart" \
             "GET /api/server/containers" "GET /api/server/containers/log?name=web" \
             "POST /api/server/containers/restart"; do
    expect 401 "${route#* } needs login" -X "${route%% *}" -H "$J" "$B${route#* }"
done

# The host, read from /proc, /sys and statvfs (this machine's).
expect 200 "server host"         -b "$JAR" "$B/api/server"
for key in '"hostname":"' '"kernel":"' '"uptime":' '"load":[' '"cpus":' '"memory":{"total":' \
           '"temperatures":[' '"reboot_needed":' '"mounts":[{"path":"' '"used":'; do
    expect_body "$key" "server host has $key"
done
expect 200 "server audit, empty" -b "$JAR" "$B/api/server/audit"
expect_body '{"actions":[]}' "no actions yet"
sqlite3 "$TMP/data/server/server.db" \
    "INSERT INTO audit (client, action, detail, result) VALUES ('127.0.0.1', 'reboot', '', 'ok: started')"
expect 200 "server audit"        -b "$JAR" "$B/api/server/audit"
expect_body '"client":"127.0.0.1","action":"reboot","detail":"","result":"ok: started"}]}' "an action"
expect 405 "server is GET only"  -b "$JAR" -X POST -H "$J" "$B/api/server"

# expect_either A B DESCRIPTION curl-args...: the status is A or B (what
# depends on the machine: whether the root actions are installed).
expect_either() {
    a=$1 b=$2 desc=$3
    shift 3
    got=$(curl -s -o "$TMP/body" -w '%{http_code}' --max-time 40 "$@")
    if [ "$got" = "$a" ] || [ "$got" = "$b" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: $desc: want $a or $b, got $got ($(head -c 200 "$TMP/body"))"
    fi
}

# Logs: only of units nylm shows (its own, NYLM_UNITS, the backup jobs).
L="$B/api/server/log"
expect 400 "log without unit"    -b "$JAR" "$L"
expect 400 "log, bad unit"       -b "$JAR" "$L?unit=a%20b"
expect 400 "log, option"         -b "$JAR" "$L?unit=-x"
expect 400 "log, bad escape"     -b "$JAR" "$L?unit=%zz"
expect 404 "log, other unit"     -b "$JAR" "$L?unit=sshd.service"
expect_body "NYLM_UNITS" "log says how to show a unit"
expect 404 "log, unknown entry"  -b "$JAR" "$L?unit=nylm-backup@other.service"
expect 404 "log, by prefix"      -b "$JAR" "$L?unit=docker"
for unit in nylm.service docker.service wg-quick@wg0 nylm-backup@immich.service; do
    expect_either 200 502 "log of $unit" -b "$JAR" "$L?unit=$unit"
done

# The disk usage job: its state from systemd; starting it needs the password.
# (The right password is not tried: on a server it would start the job.)
DU=/api/server/disk-usage
expect 200 "disk usage"          -b "$JAR" "$B$DU"
expect_body '"job":{"unit":"nylm-disk-usage.service"' "disk usage job state"
expect_body '"busy":' "disk usage says whether a job runs"
post "disk usage, no password"   400 $DU '{}'
post "disk usage, wrong password" 403 $DU '{"password":"nope"}'
expect 415 "disk usage needs json" -b "$JAR" -d '{"password":"x"}' "$B$DU"
# SMART, through the root action (not installed here: 502).
expect_either 200 502 "smart"    -b "$JAR" "$B/api/server/smart"
expect 405 "smart is GET only"   -b "$JAR" -X POST -H "$J" "$B/api/server/smart"

# Containers, through the root actions (not installed here: 502).
expect_either 200 502 "containers" -b "$JAR" "$B/api/server/containers"
CL="$B/api/server/containers/log"
expect 400 "container log, no name"   -b "$JAR" "$CL"
expect 400 "container log, option"    -b "$JAR" "$CL?name=-f"
expect 400 "container log, slash"     -b "$JAR" "$CL?name=a%2Fb"
expect 400 "container log, space"     -b "$JAR" "$CL?name=a%20b"
expect 400 "container log, too long"  -b "$JAR" "$CL?name=$(printf '%0129d' 0)"
expect 502 "container log, none such" -b "$JAR" "$CL?name=nylm-smoke-none"
CR=/api/server/containers/restart
post "restart, no name"        400 $CR "{$PW}"
post "restart, bad name"       400 $CR "{$PW,\"name\":\"a b\"}"
post "restart, name not text"  400 $CR "{$PW,\"name\":1}"
post "restart, no password"    400 $CR '{"name":"web"}'
post "restart, wrong password" 403 $CR '{"name":"web","password":"nope"}'
expect 415 "restart needs json" -b "$JAR" -d '{"name":"web"}' "$B$CR"
# The right password, a container that does not exist: the action refuses.
post "restart, none such"      502 $CR "{$PW,\"name\":\"nylm-smoke-none\"}"
expect 200 "restart recorded"  -b "$JAR" "$B/api/server/audit"
expect_body '"action":"docker-restart","detail":"nylm-smoke-none","result":"failed: see the server log"' \
    "a failed restart is recorded"

expect 200 "audit after refusals" -b "$JAR" "$B/api/server/audit"
if grep -q '"action":"disk-usage"' "$TMP/body"; then
    FAILED=$((FAILED + 1)); echo "FAIL: a refused start was recorded"
else PASSED=$((PASSED + 1)); fi

expect 200 "server page"         "$B/server/"
expect 200 "server script"       "$B/server/server.js"

expect 204 "logout"            -b "$JAR" -c "$JAR" -X POST "$B/api/logout"
expect 401 "after logout"       -b "$JAR" "$B/api/session"

stop

# Invalid server app settings: nylm says which and does not start.
# refuses_setting DESCRIPTION VAR=VALUE TEXT
refuses_setting() {
    if env "$2" "$BIN" >"$TMP/log" 2>&1 </dev/null; then
        FAILED=$((FAILED + 1)); echo "FAIL: $1: started anyway"
    elif grep -q "$3" "$TMP/log"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1)); echo "FAIL: $1: unclear error: $(cat "$TMP/log")"
    fi
}
refuses_setting "bad unit"          'NYLM_UNITS=docker a;b'           "NYLM_UNITS"
refuses_setting "unit twice"        'NYLM_UNITS=docker docker'        "NYLM_UNITS"
refuses_setting "entry no path"     'NYLM_BACKUP=davis'               "NYLM_BACKUP"
refuses_setting "entry bad path"    'NYLM_BACKUP=davis=/srv/../etc'   "NYLM_BACKUP"
refuses_setting "entry named nylm"  'NYLM_BACKUP=nylm=/srv/x'         "NYLM_BACKUP"
refuses_setting "backup dir"        'NYLM_BACKUP_DIR=backups'         "NYLM_BACKUP_DIR"

# ---- root scripts ------------------------------------------------------------

# Every action and job: a shell script that stops at the first error.
for f in deploy/actions/* deploy/jobs/*; do
    if [ "$(head -n 1 "$f")" = "#!/bin/sh" ] && grep -q '^set -eu$' "$f" && [ -x "$f" ]; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1)); echo "FAIL: $f: not an executable #!/bin/sh script with set -eu"
    fi
done

# deploy/lib.sh's rules, on a copy that reads a test configuration and lock.
cat >"$TMP/nylm.conf" <<'EOF'
NYLM_UNITS="docker.service wg-quick@wg0"
NYLM_BACKUP="davis=/srv/davis immich=/srv/immich,/srv/immich-db"
EOF
mkdir "$TMP/jobs"
sed -e "s#/etc/nylm.conf#$TMP/nylm.conf#" -e "s#^JOBS_LOCK=.*#JOBS_LOCK=$TMP/jobs#" \
    deploy/lib.sh >"$TMP/lib.sh"
# lib WANT DESCRIPTION CODE: shell CODE with lib.sh loaded exits 0 (WANT ok)
# or not (WANT no); its output is in $TMP/lib.out.
lib() {
    if sh -c ". '$TMP/lib.sh'; $3" >"$TMP/lib.out" 2>&1; then got=ok; else got=no; fi
    if [ "$got" = "$1" ]; then PASSED=$((PASSED + 1)); else
        FAILED=$((FAILED + 1)); echo "FAIL: lib.sh: $2: want $1, got $got ($(cat "$TMP/lib.out"))"
    fi
}
lib ok "unit"               'valid_unit docker.service'
lib ok "unit with @"        'valid_unit wg-quick@wg0'
lib no "unit, option"       'valid_unit -x'
lib no "unit, space"        'valid_unit "a b"'
lib no "unit, slash"        'valid_unit a/b'
lib no "unit, empty"        'valid_unit ""'
lib ok "unit, 128"          "valid_unit $(printf '%0128d' 0)"
lib no "unit, 129"          "valid_unit $(printf '%0129d' 0)"
lib ok "container"          'valid_container immich_server-1.x'
lib no "container, option"  'valid_container -f'
lib no "container, _ first" 'valid_container _x'
lib no "container, slash"   'valid_container a/b'
lib no "container, colon"   'valid_container a:b'
lib ok "container, 128"     "valid_container $(printf '%0128d' 0)"
lib no "container, 129"     "valid_container $(printf '%0129d' 0)"
lib ok "entry"              'valid_entry immich-db2'
lib no "entry, upper case"  'valid_entry Davis'
lib no "entry, dash first"  'valid_entry -a'
lib ok "entry, 32"          "valid_entry $(printf '%032d' 0)"
lib no "entry, 33"          "valid_entry $(printf '%033d' 0)"
lib ok "path"               'valid_path /var/lib/docker/volumes/davis_data/_data'
lib ok "path, hidden part"  'valid_path /a/.hidden/b-c'
lib no "path, root"         'valid_path /'
lib no "path, relative"     'valid_path srv/a'
lib no "path, trailing /"   'valid_path /a/'
lib no "path, //"           'valid_path /a//b'
lib no "path, ."            'valid_path /a/./b'
lib no "path, .."           'valid_path /a/../b'
lib no "path, ends in .."   'valid_path /a/..'
lib no "path, space"        'valid_path "/a b"'
lib no "path, comma"        'valid_path /a,b'
lib no "path, glob"         'valid_path "/a*"'
lib ok "path, 1024"         "valid_path /$(printf '%01023d' 0)"
lib no "path, 1025"         "valid_path /$(printf '%01024d' 0)"
lib ok "shown, own"         'unit_shown nylm-disk-usage.service'
lib ok "shown, NYLM_UNITS"  'unit_shown wg-quick@wg0'
lib ok "shown, backup job"  'unit_shown nylm-backup@immich.service'
lib no "shown, other"       'unit_shown sshd.service'
lib no "shown, by prefix"   'unit_shown docker'
lib no "shown, other entry" 'unit_shown nylm-backup@other.service'
lib ok "entry paths"        '[ "$(entry_paths immich)" = "$(printf "/srv/immich\n/srv/immich-db")" ]'
lib no "entry paths, none"  'entry_paths other'
lib no "entry paths, nylm"  'NYLM_BACKUP="nylm=/srv/x"; entry_paths nylm'
for bad in 'davis' 'davis=' 'davis=/srv/a,' 'davis=,/srv/a' 'davis=/srv/a,,/srv/b' \
           'davis=/srv/../etc' 'davis=srv' 'Davis=/srv/a'; do
    name=${bad%%=*}
    lib no "entry paths, $bad" "NYLM_BACKUP='$bad'; entry_paths $name"
done
lib ok "lock"               'take_lock'
flock "$TMP/jobs" sleep 5 &
LOCKER=$!
sleep 0.3
lib no "lock, taken"        'take_lock'
if grep -q "another nylm job is running" "$TMP/lib.out"; then PASSED=$((PASSED + 1)); else
    FAILED=$((FAILED + 1)); echo "FAIL: lib.sh: busy lock not explained"; fi
kill "$LOCKER" 2>/dev/null
wait "$LOCKER" 2>/dev/null

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
