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
# Disks a Mac can start from (boot blocks, "LK"), and some it can't.
python3 - "$CARD/disks" <<'PY'
import struct, sys
d = sys.argv[1]
def write(name, data, size):
    data = bytearray(data) + bytes(max(0, size - len(data)))
    open(d + "/" + name, "wb").write(bytes(data[:size]))
write("System.dsk", b"LK" + bytes(10), 819200)                   # a plain floppy
write("System.image", bytes(84) + b"LK", 819284)                 # DiskCopy 4.2
def partitioned(bs, hfs_start, boot):
    img = bytearray(bs * (hfs_start + 8))
    img[0:4] = b"ER" + struct.pack(">H", bs)
    for i, (ptype, start) in enumerate([(b"Apple_partition_map", 1), (b"Apple_HFS", hfs_start)]):
        e = bytearray(bs)
        e[0:2] = b"PM"
        e[4:8] = struct.pack(">I", 2)
        e[8:12] = struct.pack(">I", start)
        e[48:48 + len(ptype)] = ptype
        img[(i + 1) * bs:(i + 2) * bs] = e
    if boot:
        img[hfs_start * bs:hfs_start * bs + 2] = b"LK"
    return img
write("MacOS.hda", partitioned(512, 64, True), 1048576)          # a hard disk with a System
write("Data.hda", partitioned(512, 64, False), 1048576)          # partitioned, no System
write("Install.iso", partitioned(2048, 16, True), 2048 * 32)     # a Mac CD-ROM
PY

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
        "/disks/café.dsk": "floppy", "/disks/System.dsk": "floppy", "/disks/System.image": "floppy",
        "/disks/MacOS.hda": "hard", "/disks/Data.hda": "hard", "/disks/Install.iso": "cd"}
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
python3 - "$OUT/ii.json" <<'PY' && pass "startable: only disks with boot blocks (plain, DiskCopy, partitioned, a CD-ROM's 2048-byte map)" || fail "startable"
import json, sys
d = json.load(open(sys.argv[1]))
s = {i["path"]: i["startable"] for i in d["images"]}
want_yes = ["/disks/System.dsk", "/disks/System.image", "/disks/MacOS.hda", "/disks/Install.iso"]
want_no = ["/disks/f.dsk", "/disks/Data.hda", "/disks/Mac OS 8.1.hda", "/disks/cd.iso"]
assert all(s[p] for p in want_yes), s
assert not any(s[p] for p in want_no), s
PY
check "start from a disk without a System: refused" 'post ii post "op=start&path=/disks/f.dsk" | grep -q "^409 .*no System" && ! post ii post "op=start&path=/disks/f.dsk" | grep -q queued'
check "start from a startable one: queued" 'post ii post "op=start&path=/disks/MacOS.hda" | grep -q "^queued 1 0 /disks/MacOS.hda$"'
check "insert a floppy: queued" '[ "$(post ii post "op=insert&path=%2Fdisks%2Ff.dsk" | head -1 | cut -c1-3)" = 202 ] && post ii post "op=insert&path=%2Fdisks%2Ff.dsk" | grep -q "^queued 2 0 /disks/f.dsk$"'
check "a UTF-8 name: queued as the card's CP850" 'post ii post "op=insert&path=/disks/caf%C3%A9.dsk" | grep -q "^queued 2 0 /disks/caf"$'"'"'\x82'"'"'".dsk$"'
check "insert a hard disk image: refused" 'post ii post "op=insert&path=/disks/Mac%20OS%208.1.hda" | grep -q "^400 .*hard disk"'
check "add a hard disk: queued" 'post ii post "op=add&path=/disks/Mac+OS+8.1.hda" | grep -q "^queued 3 0 /disks/Mac OS 8.1.hda$"'
check "remove hard disk 2: queued" 'post ii post "op=remove&index=1" | grep -q "^queued 4 1 $"'
check "eject the second floppy drive: queued" 'post ii post "op=eject&index=1" | grep -q "^queued 5 1 $"'
check "eject a drive that isn't there: refused" '[ "$(post ii post "op=eject&index=3" | head -1 | cut -c1-3)" = 400 ] && [ "$(post ii post "op=eject" | head -1 | cut -c1-3)" = 400 ]'
cp "$CARD/disks/f.dsk" "$CARD/disks/gone.dsk"
check "delete an image: gone, at once" 'post ii post "op=delete&path=/disks/gone.dsk" | grep -q "^200 .*gone.dsk is deleted" && [ ! -e "$CARD/disks/gone.dsk" ]'
check "delete one in a drive: refused" 'post ii post "op=delete&path=/disks/caf%C3%A9.dsk" | grep -q "^409" && ls "$CARD/disks" | grep -q "^caf.\\.dsk$"'
check "delete a hard disk on the list: refused" 'post ii post "op=delete&path=/disks/Mac%20OS%208.1.hda" | grep -q "^409"'
check "delete one still open: refused" 'post ii post "op=delete&path=/disks/Data.hda" | grep -q "^409" && [ -e "$CARD/disks/Data.hda" ]'
check "delete outside the disks folder: refused" 'post ii post "op=delete&path=/config.txt" | grep -q "^400" && post ii post "op=delete&path=/disks/../config.txt" | grep -q "^400"'
check "delete the Mac Plus disk: refused" 'post plus post "op=delete&path=/disks/sys.dsk" | grep -q "^40[49]"'
check "eject on the Mac Plus: refused" 'post plus post "op=eject&index=0" | grep -q "^400"'
check "remove with a bad index: refused" '[ "$(post ii post "op=remove&index=x" | head -1 | cut -c1-3)" = 400 ] && [ "$(post ii post "op=remove&index=99" | head -1 | cut -c1-3)" = 400 ]'
check "outside the disks folder: refused" 'post ii post "op=start&path=/roms/notadisk.dsk" | grep -q "^400" && ! post ii post "op=start&path=/roms/notadisk.dsk" | grep -q queued'
check "\"..\": refused" 'post ii post "op=start&path=/disks/../roms/notadisk.dsk" | grep -q "^400"'
check "not a disk image: refused" 'post ii post "op=start&path=/disks/readme.txt" | grep -q "^400"'
check "not on the card: 404" 'post ii post "op=start&path=/disks/gone.dsk" | grep -q "^404"'
check "an unknown change: refused" 'post ii post "op=format&path=/disks/f.dsk" | grep -q "^400"'
check "the Mac Plus: start only" 'post plus post "op=start&path=/disks/System.dsk" | grep -q "^queued 1 0" && post plus post "op=add&path=/disks/f.dsk" | grep -q "^400"'
check "another change waiting: 409" 'BUSY=1 post ii post "op=insert&path=/disks/f.dsk" | grep -q "^409 .*Another"'
# ---- New blank disk images ----
check "a new 1.4 MB floppy: made, its size, zeros" 'post ii post "op=new&name=Blank&kind=floppy&size=1474560" | grep -q "^201 .*\"path\":\"/disks/Blank.dsk\"" && [ "$(stat -c %s "$CARD/disks/Blank.dsk")" = 1474560 ] && cmp -s "$CARD/disks/Blank.dsk" <(head -c 1474560 /dev/zero)'
check "a new 100 MB hard disk: made, its size" 'post ii post "op=new&name=Work+Disk&kind=hard&size=104857600" | grep -q "^201 .*/disks/Work Disk.hda" && [ "$(stat -c %s "$CARD/disks/Work Disk.hda")" = 104857600 ]'
check "... its start and end are zeros" '[ -z "$(head -c 65536 "$CARD/disks/Work Disk.hda" | tr -d "\\0")" ] && [ -z "$(tail -c 512 "$CARD/disks/Work Disk.hda" | tr -d "\\0")" ]'
check "... and listed as a hard disk" '"$OUT/test_disks" "$CARD" ii json | python3 -c "import json,sys; d=json.load(sys.stdin); assert {\"path\":\"/disks/Work Disk.hda\",\"size\":104857600,\"kind\":\"hard\",\"startable\":False} in d[\"images\"]"'
check "a name already there: 409, the file kept" 'post ii post "op=new&name=Blank&kind=floppy&size=819200" | grep -q "^409" && [ "$(stat -c %s "$CARD/disks/Blank.dsk")" = 1474560 ]'
check "a floppy that isn't 800K or 1.4 MB: refused" 'post ii post "op=new&name=Odd&kind=floppy&size=1000000" | grep -q "^400" && [ ! -e "$CARD/disks/Odd.dsk" ]'
check "a hard disk over 2 GB, or not whole blocks: refused" 'post ii post "op=new&name=Huge&kind=hard&size=3000000000" | grep -q "^400" && post ii post "op=new&name=Odd&kind=hard&size=1048577" | grep -q "^400"'
check "names with / .. : or not ASCII: refused" 'for n in "a%2Fb" ".." "a:b" "caf%C3%A9" "" "x."; do post ii post "op=new&name=$n&kind=floppy&size=819200" | grep -q "^400" || exit 1; done; [ -z "$(ls "$CARD/disks" | grep -v -e "^Games$" -e "\.dsk$" -e "\.hda$" -e "\.iso$" -e "\.image$" -e "\.txt$")" ]'
check "the Mac Plus: refused" 'post plus post "op=new&name=P&kind=floppy&size=819200" | grep -q "^400" && [ ! -e "$CARD/disks/P.dsk" ]'
check "the answers are JSON" 'post ii post "op=start&path=/disks/f.dsk" | head -1 | cut -c5- | python3 -c "import json,sys; json.load(sys.stdin)"'

echo
if [ "$FAILED" -eq 0 ]; then echo "All tests passed."; else echo "$FAILED failed."; exit 1; fi
