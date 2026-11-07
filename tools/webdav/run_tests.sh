#!/bin/bash
#
# Tests the network drive (src/webui/webdav.cpp) on the PC: tools/webdav's
# dav_server serves a folder as the SD card, as the Pi's web UI does, and
# WebDAV clients use it.
#
#   tools/webdav/run_tests.sh
#
# curl and python3 are needed. litmus (the WebDAV test suite) and rclone
# are used when they are installed (apt install litmus rclone), and skipped
# otherwise. litmus's "props" and "locks" are left out: properties set with
# PROPPATCH aren't kept, and locks are granted but not enforced, as
# webdav.cpp says. So is basic's put_get_utf8_segment: its name has a
# character (the euro sign) the card's code page (CP850) doesn't have.
#

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$HERE/out
mkdir -p "$OUT"

${CXX:-c++} -std=gnu++14 -O1 -g -Wall -Wextra -Werror -o "$OUT/dav_server" \
    "$HERE/dav_server.cpp" "$HERE/sd_fs_posix.cpp" "$ROOT/src/webui/webdav.cpp" \
    "$ROOT/src/port/transfer.c" || exit 1

CARD=$OUT/card
rm -rf "$CARD" "$OUT/work"
mkdir -p "$CARD/shared" "$OUT/work"
PORT=$((20000 + RANDOM % 20000))
"$OUT/dav_server" "$CARD" "$PORT" 2> "$OUT/server.log" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
URL=http://127.0.0.1:$PORT/sd
for _ in 1 2 3 4 5 6 7 8 9 10; do
    curl -s -o /dev/null -X OPTIONS "$URL/" && break
    sleep 0.2
done

FAILED=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }
status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

W=$OUT/work

# ---- Files up and down ----
head -c 5000000 /dev/urandom > "$W/big.bin"
check "PUT 5 MB with a length: 201" '[ "$(status -T "$W/big.bin" "$URL/shared/big.bin")" = 201 ]'
check "... the card has it" 'cmp -s "$W/big.bin" "$CARD/shared/big.bin"'
check "GET gives it back" 'curl -s "$URL/shared/big.bin" -o "$W/got.bin" && cmp -s "$W/big.bin" "$W/got.bin"'
check "PUT over it: 204" '[ "$(status -T "$W/big.bin" "$URL/shared/big.bin")" = 204 ]'
check "PUT chunked (as macOS sends)" '[ "$(status -T - -H "Transfer-Encoding: chunked" "$URL/shared/chunked.bin" < "$W/big.bin")" = 201 ] && cmp -s "$W/big.bin" "$CARD/shared/chunked.bin"'
check "PUT after 100 Continue (as Windows and curl send)" \
    '[ "$(status -T "$W/big.bin" -H "Expect: 100-continue" "$URL/shared/cont.bin")" = 201 ] && cmp -s "$W/big.bin" "$CARD/shared/cont.bin"'
check "PUT of nothing makes an empty file" '[ "$(status -T /dev/null "$URL/shared/empty")" = 201 ] && [ -f "$CARD/shared/empty" ] && [ ! -s "$CARD/shared/empty" ]'
check "no temporary file left" '! ls -a "$CARD/shared" | grep -q "^\.~dav-"'
check "HEAD has the length" 'curl -s -I "$URL/shared/big.bin" | grep -qi "^Content-Length: 5000000"'

# A transfer that breaks off leaves the file as it was.
printf 'old contents' > "$CARD/shared/keep.txt"
python3 - "$PORT" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"PUT /sd/shared/keep.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n" + b"x" * 5000)
s.close()
PY
sleep 0.5
check "a broken-off PUT leaves the file as it was" '[ "$(cat "$CARD/shared/keep.txt")" = "old contents" ] && ! ls -a "$CARD/shared" | grep -q "^\.~dav-"'

# ---- Names ----
check "UTF-8 names are CP850 on the card" '[ "$(status -T /dev/null "$URL/shared/caf%C3%A9.txt")" = 201 ] && [ -f "$CARD/shared/caf"$'"'"'\x82'"'"'".txt" ]'
check "... and UTF-8 in listings" 'curl -s -X PROPFIND -H "Depth: 1" "$URL/shared/" | grep -q "caf%C3%A9.txt"'
check "a name CP850 hasn't: 400" '[ "$(status -T /dev/null "$URL/shared/%E2%82%AC")" = 400 ]'
check "\"..\": refused" '[ "$(status --path-as-is "$URL/shared/../../etc")" = 400 ]'
check "\"..\" encoded: refused" '[ "$(status "$URL/shared/%2e%2e/%2e%2e/x")" = 400 ]'
check "a trailing dot (FAT would drop it): refused" '[ "$(status -T /dev/null "$URL/shared/name.")" = 400 ]'
# curl drops a fragment before sending: sent as it is, to a folder of its own.
mkdir -p "$CARD/frag"
python3 - "$PORT" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"DELETE /sd/frag/#x HTTP/1.1\r\nHost: x\r\n\r\n")
s.recv(100)
s.close()
PY
check "a fragment: refused, the folder before it kept" '[ -d "$CARD/frag" ]'

# ---- Listings ----
python3 - "$URL" <<'PY' && pass "PROPFIND Depth 1: well formed, folder and files with their properties" || fail "PROPFIND Depth 1"
import sys, urllib.request, xml.etree.ElementTree as ET
req = urllib.request.Request(sys.argv[1] + "/shared/", method="PROPFIND", headers={"Depth": "1"})
body = urllib.request.urlopen(req).read()
root = ET.fromstring(body)
D = "{DAV:}"
r = {e.find(D + "href").text: e for e in root.findall(D + "response")}
assert "/sd/shared/" in r, r.keys()
assert r["/sd/shared/"].find(".//" + D + "collection") is not None
big = r["/sd/shared/big.bin"]
assert big.find(".//" + D + "getcontentlength").text == "5000000"
assert big.find(".//" + D + "getlastmodified").text.endswith("GMT")
assert big.find(".//" + D + "getetag") is not None
PY
python3 - "$URL" <<'PY' && pass "PROPFIND for named properties: 200 and 404 (an unknown one, in no namespace)" || fail "PROPFIND for named properties"
import sys, urllib.request, xml.etree.ElementTree as ET
body = b'<?xml version="1.0"?><D:propfind xmlns:D="DAV:"><D:prop><D:getcontentlength/><D:quota-available-bytes/><odd xmlns=""/></D:prop></D:propfind>'
req = urllib.request.Request(sys.argv[1] + "/", data=body, method="PROPFIND", headers={"Depth": "0"})
root = ET.fromstring(urllib.request.urlopen(req).read())
D = "{DAV:}"
stats = {ps.find(D + "status").text: ps for ps in root.iter(D + "propstat")}
ok = stats["HTTP/1.1 200 OK"]
assert int(ok.find(".//" + D + "quota-available-bytes").text) > 0
missing = stats["HTTP/1.1 404 Not Found"]
assert missing.find(".//odd") is not None and missing.find(".//" + D + "getcontentlength") is not None
PY
check "the card's top folder is /sd/ (not /sd//)" 'curl -s -X PROPFIND -H "Depth: 0" "$URL/" | grep -q "<D:href>/sd/</D:href>" && ! curl -s -X PROPFIND -H "Depth: 1" "$URL/" | grep -q "/sd//" && ! curl -s "$URL/shared/" | grep -q "/sd//"'
check "PROPFIND of the server's root lists the card" 'curl -s -X PROPFIND -H "Depth: 1" "http://127.0.0.1:$PORT/" | grep -q "<D:href>/sd/</D:href>"'
check "broken XML: 400" '[ "$(status -X PROPFIND -d "<D:propfind xmlns:D=\"DAV:\"><D:prop>" "$URL/")" = 400 ]'
check "GET of a folder: a page of it" 'curl -s "$URL/shared/" | grep -q "big.bin"'

# ---- The Mac's helpers go with a file ----
mkdir -p "$CARD/shared/.finf" "$CARD/shared/.rsrc" "$CARD/shared/sub"
printf 'APPLttxt' > "$CARD/shared/.finf/empty"
printf 'resources' > "$CARD/shared/.rsrc/empty"
check "MOVE: 201" '[ "$(status -X MOVE -H "Destination: $URL/shared/sub/moved" "$URL/shared/empty")" = 201 ]'
check "... the Finder info and resource fork moved with it" '[ -f "$CARD/shared/sub/.finf/moved" ] && [ -f "$CARD/shared/sub/.rsrc/moved" ] && [ ! -e "$CARD/shared/.finf/empty" ]'
check "COPY: the helpers copied" '[ "$(status -X COPY -H "Destination: $URL/shared/copied" "$URL/shared/sub/moved")" = 201 ] && [ "$(cat "$CARD/shared/.rsrc/copied")" = resources ] && [ -f "$CARD/shared/sub/.rsrc/moved" ]'
check "DELETE: the helpers deleted" '[ "$(status -X DELETE "$URL/shared/copied")" = 204 ] && [ ! -e "$CARD/shared/.finf/copied" ] && [ ! -e "$CARD/shared/.rsrc/copied" ]'
printf '\0\0\0\0\0\0\0\0' > "$CARD/shared/.finf/fresh.sit"
printf 'old' > "$CARD/shared/.rsrc/fresh.sit"
check "PUT of a file: its stale Finder info and resource fork go" '[ "$(status -T "$W/big.bin" "$URL/shared/fresh.sit")" = 201 ] && [ ! -e "$CARD/shared/.finf/fresh.sit" ] && [ ! -e "$CARD/shared/.rsrc/fresh.sit" ]'
printf '\0\0\0\0\0\0\0\0' > "$CARD/shared/.finf/fresh.sit"
check "PUT of its ._ file: the same" '[ "$(status -T "$W/big.bin" "$URL/shared/._fresh.sit")" = 201 ] && [ ! -e "$CARD/shared/.finf/fresh.sit" ] && [ -f "$CARD/shared/fresh.sit" ]'
check "MOVE onto a file, Overwrite F: 412" '[ "$(status -X MOVE -H "Destination: $URL/shared/big.bin" -H "Overwrite: F" "$URL/shared/cont.bin")" = 412 ]'
check "MOVE of a folder into itself: 403" '[ "$(status -X MOVE -H "Destination: $URL/shared/sub/x" "$URL/shared")" = 403 ]'
check "MOVE elsewhere (another server): 502" '[ "$(status -X MOVE -H "Destination: http://example.com/x" "$URL/shared/cont.bin")" = 502 ]'
check "rename, a change of case only" '[ "$(status -X MOVE -H "Destination: $URL/shared/BIG.BIN" "$URL/shared/big.bin")" = 201 ] && [ -f "$CARD/shared/BIG.BIN" ]'

# ---- What stays protected ----
printf 'kernel=x\n' > "$CARD/config.txt"
mkdir -p "$CARD/firmware"
check "config.txt can be read" '[ "$(curl -s "$URL/config.txt")" = "kernel=x" ]'
check "... not written, deleted or moved" '[ "$(status -T "$W/big.bin" "$URL/config.txt")" = 403 ] && [ "$(status -X DELETE "$URL/config.txt")" = 403 ] && [ "$(status -X MOVE -H "Destination: $URL/c.txt" "$URL/config.txt")" = 403 ] && [ "$(cat "$CARD/config.txt")" = "kernel=x" ]'
check "nothing can be put in firmware/" '[ "$(status -T /dev/null "$URL/firmware/x")" = 403 ] && [ "$(status -X MKCOL "$URL/firmware/d")" = 403 ]'
check "the card itself can't be deleted" '[ "$(status -X DELETE "$URL/")" = 403 ] && [ -d "$CARD/shared" ]'

# ---- Locks (macOS's Finder only writes where it can lock) ----
LOCK='<?xml version="1.0"?><D:lockinfo xmlns:D="DAV:"><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype><D:owner><D:href>me</D:href></D:owner></D:lockinfo>'
check "LOCK of a new name makes it: 201 and a token" 'curl -s -i -X LOCK -d "$LOCK" "$URL/shared/locked.txt" | tee "$W/lock.txt" | grep -q "^HTTP/1.1 201" && grep -qi "^Lock-Token: <opaquelocktoken:" "$W/lock.txt" && [ -f "$CARD/shared/locked.txt" ]'
check "UNLOCK: 204" '[ "$(status -X UNLOCK -H "Lock-Token: <x>" "$URL/shared/locked.txt")" = 204 ]'

# ---- WebDAV clients ----
if command -v litmus >/dev/null 2>&1; then
    (cd "$OUT" && litmus -k "$URL/" > "$OUT/litmus.log" 2>&1)
    summary() { grep "summary for \`$1'" "$OUT/litmus.log" | sed 's/.*of \([0-9]*\) tests run: \([0-9]*\) passed.*/\2\/\1/'; }
    check "litmus basic: all but the euro sign's ($(summary basic))" \
        '[ "$(summary basic)" = 15/16 ] && grep -q "put_get_utf8_segment.. FAIL" "$OUT/litmus.log"'
    check "litmus copymove ($(summary copymove))" '[ "$(summary copymove)" = 13/13 ]'
    check "litmus http ($(summary http))" '[ "$(summary http)" = 4/4 ]'
    check "litmus props: the server's answers parse" '! grep -q "XML parse error" "$OUT/litmus.log"'
else
    echo "SKIP  litmus (not installed)"
fi

if command -v rclone >/dev/null 2>&1; then
    RC="rclone --config /dev/null --webdav-url=$URL --webdav-vendor=other -q"
    mkdir -p "$W/tree/a/b" "$W/tree/c"
    for i in 1 2 3 4 5; do head -c $((i * 70000)) /dev/urandom > "$W/tree/a/b/f$i"; done
    printf 'hello' > "$W/tree/c/hello.txt"
    printf 'bonjour' > "$W/tree/c/très.txt"
    check "rclone: a folder tree up" '$RC copy "$W/tree" :webdav:shared/tree'
    check "rclone: it checks out, byte for byte" '$RC check --download "$W/tree" :webdav:shared/tree'
    check "rclone: and down again" '$RC copy :webdav:shared/tree "$W/back" && diff -r "$W/tree" "$W/back" >/dev/null'
    check "rclone: delete it" '$RC purge :webdav:shared/tree && [ ! -e "$CARD/shared/tree" ]'
else
    echo "SKIP  rclone (not installed)"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "All tests passed."
else
    echo "$FAILED failed (server log: $OUT/server.log)."
    exit 1
fi
