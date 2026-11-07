#!/bin/bash
#
# Tests the web page's Disks (src/webui/webui_disks.cpp) on the PC: what
# GET /api/disks lists and POST /api/disks lets through. The emulator's side
# (the queue and the Macs' disks) is tested by tools/hostsim.
#
#   tools/disks/run_tests.sh
#

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$HERE/out
mkdir -p "$OUT"
${CXX:-c++} -std=gnu++14 -O1 -g -Wall -Wextra -Werror -o "$OUT/test_disks" \
    "$HERE/test_disks.cpp" "$ROOT/src/webui/webui_disks.cpp" \
    "$ROOT/tools/webdav/sd_fs_posix.cpp" || exit 1

CARD=$OUT/card
rm -rf "$CARD"
mkdir -p "$CARD/disks/Games/Deep" "$CARD/disks/.rsrc" "$CARD/roms"
head -c 819200 /dev/zero > "$CARD/disks/f.dsk"
head -c $((1474560 + 84)) /dev/zero > "$CARD/disks/big.image"
head -c 1048576 /dev/zero > "$CARD/disks/Mac OS 8.1.hda"
head -c 2048 /dev/zero > "$CARD/disks/cd.iso"
head -c 819200 /dev/zero > "$CARD/disks/Games/game.dsk"
head -c 819200 /dev/zero > "$CARD/disks/Games/Deep/deep.dsk"
head -c 819200 /dev/zero > "$CARD/disks/caf"$'\x82'".dsk"
head -c 100 /dev/zero > "$CARD/disks/readme.txt"
head -c 100 /dev/zero > "$CARD/disks/._f.dsk"
head -c 100 /dev/zero > "$CARD/disks/.rsrc/f.dsk"
head -c 819200 /dev/zero > "$CARD/roms/notadisk.dsk"

FAILED=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

"$OUT/test_disks" "$CARD" ii json > "$OUT/ii.json"
python3 - "$OUT/ii.json" <<'PY' && pass "Mac II: the JSON, the Mac's disks and the images with their kinds" || fail "Mac II JSON"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["machine"] == "ii"
assert d["hard"] == ["/disks/Mac OS 8.1.hda"], d["hard"]
assert d["floppy"] == ["", "/disks/café.dsk"], d["floppy"]
assert d["done"] == 7 and d["pending"] is False and "floppy" in d["result"]
kinds = {i["path"]: i["kind"] for i in d["images"]}
want = {"/disks/f.dsk": "floppy", "/disks/big.image": "floppy", "/disks/Mac OS 8.1.hda": "hard",
        "/disks/cd.iso": "cd", "/disks/Games/game.dsk": "floppy", "/disks/Games/Deep/deep.dsk": "floppy",
        "/disks/café.dsk": "floppy"}
assert kinds == want, kinds
assert d["more"] is False
PY
"$OUT/test_disks" "$CARD" plus json > "$OUT/plus.json"
python3 - "$OUT/plus.json" <<'PY' && pass "Mac Plus: its disk; no CD-ROM images; all \"disk\"" || fail "Mac Plus JSON"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["machine"] == "plus" and d["disk"] == "/disks/sys.dsk"
assert "/disks/cd.iso" not in [i["path"] for i in d["images"]]
assert {i["kind"] for i in d["images"]} == {"disk"}
PY

post() { "$OUT/test_disks" "$CARD" "$@"; }
check "insert a floppy: queued" '[ "$(post ii post "op=insert&path=%2Fdisks%2Ff.dsk" | head -1 | cut -c1-3)" = 202 ] && post ii post "op=insert&path=%2Fdisks%2Ff.dsk" | grep -q "^queued 2 0 /disks/f.dsk$"'
check "a UTF-8 name: queued as the card's CP850" 'post ii post "op=insert&path=/disks/caf%C3%A9.dsk" | grep -q "^queued 2 0 /disks/caf"$'"'"'\x82'"'"'".dsk$"'
check "insert a hard disk image: refused" 'post ii post "op=insert&path=/disks/Mac%20OS%208.1.hda" | grep -q "^400 .*hard disk"'
check "add a hard disk: queued" 'post ii post "op=add&path=/disks/Mac+OS+8.1.hda" | grep -q "^queued 3 0 /disks/Mac OS 8.1.hda$"'
check "remove hard disk 2: queued" 'post ii post "op=remove&index=1" | grep -q "^queued 4 1 $"'
check "remove with a bad index: refused" '[ "$(post ii post "op=remove&index=x" | head -1 | cut -c1-3)" = 400 ] && [ "$(post ii post "op=remove&index=99" | head -1 | cut -c1-3)" = 400 ]'
check "outside the disks folder: refused" 'post ii post "op=start&path=/roms/notadisk.dsk" | grep -q "^400" && ! post ii post "op=start&path=/roms/notadisk.dsk" | grep -q queued'
check "\"..\": refused" 'post ii post "op=start&path=/disks/../roms/notadisk.dsk" | grep -q "^400"'
check "not a disk image: refused" 'post ii post "op=start&path=/disks/readme.txt" | grep -q "^400"'
check "not on the card: 404" 'post ii post "op=start&path=/disks/gone.dsk" | grep -q "^404"'
check "an unknown change: refused" 'post ii post "op=format&path=/disks/f.dsk" | grep -q "^400"'
check "the Mac Plus: start only" 'post plus post "op=start&path=/disks/f.dsk" | grep -q "^queued 1 0" && post plus post "op=add&path=/disks/f.dsk" | grep -q "^400"'
check "another change waiting: 409" 'BUSY=1 post ii post "op=start&path=/disks/f.dsk" | grep -q "^409"'
check "the answers are JSON" 'post ii post "op=start&path=/disks/f.dsk" | head -1 | cut -c5- | python3 -c "import json,sys; json.load(sys.stdin)"'

echo
if [ "$FAILED" -eq 0 ]; then echo "All tests passed."; else echo "$FAILED failed."; exit 1; fi
