#!/bin/bash
#
# Tests the web page's update (src/webui/update.cpp) on the PC: release
# zips, made here or the one build_sdcard.sh staged, installed on a folder
# standing in for the SD card.
#
#   tools/update/run_tests.sh
#
# This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
# the Raspberry Pi. See LICENSE for licensing details.
#
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$HERE/out
rm -rf "$OUT"
mkdir -p "$OUT"
Z=$ROOT/third_party/zlib
${CXX:-c++} -std=gnu++14 -O1 -g -Wall -Wextra -Werror -I "$Z" -o "$OUT/test_update" \
    "$HERE/test_update.cpp" "$ROOT/src/webui/update.cpp" "$ROOT/tools/webdav/sd_fs_posix.cpp" \
    -x c -w "$Z/adler32.c" "$Z/crc32.c" "$Z/inffast.c" "$Z/inflate.c" "$Z/inftrees.c" "$Z/zutil.c" \
    || exit 1

FAILED=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

# A card as it is in use: the user's disks, settings and files, an older
# release's kernels.
card() {
	rm -rf "$OUT/card"
	mkdir -p "$OUT/card/disks" "$OUT/card/shared" "$OUT/card/roms" "$OUT/card/firmware"
	printf 'old kernel' > "$OUT/card/macii8-rpi4.img"
	printf 'old plus' > "$OUT/card/macplus.img"
	printf 'old fw' > "$OUT/card/firmware/brcmfmac43455-sdio.bin"
	printf 'kernel=mine\n' > "$OUT/card/config.txt"
	printf 'audio_out=auto\n' > "$OUT/card/cmdline.txt"
	printf 'network={ssid="home"}\n' > "$OUT/card/wpa_supplicant.conf"
	printf 'my prefs' > "$OUT/card/macii.prefs"
	printf 'HFS' > "$OUT/card/disks/OS81.hda"
	printf 'my readme' > "$OUT/card/disks/README.txt"
	printf 'game' > "$OUT/card/shared/Game.sit"
	printf 'rom' > "$OUT/card/roms/quadra.rom"
}

# make_zip NAME [top folder] [stored]: a small release.
make_zip() {
	python3 - "$OUT/$1" "${2:-}" "${3:-}" <<'PY'
import sys, zipfile
path, top, stored = sys.argv[1], sys.argv[2], sys.argv[3]
c = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
with zipfile.ZipFile(path, "w", c) as z:
    def add(n, data): z.writestr(top + n, data)
    add("VERSION.txt", "0.1.6 (abc1234)\n")
    add("macii8-rpi4.img", b"NEW KERNEL" * 50000)
    add("macplus.img", b"new plus" * 1000)
    add("firmware/brcmfmac43455-sdio.bin", b"new fw" * 100)
    add("overlays/new.dtbo", b"overlay")
    add("config.txt", "kernel=release\n")
    add("cmdline.txt", "audio_out=auto\n")
    add("wpa_supplicant.conf.example", "example")
    add("disks/README.txt", "release readme")
    add("shared/README.txt", "shared readme")
    add("__MACOSX/._macii8-rpi4.img", "junk")
    add("._config.txt", "junk")
PY
}

install() { cp "$OUT/$1" "$OUT/card/bmmac-update.zip"; "$OUT/test_update" "$OUT/card" /bmmac-update.zip; }

# ---- A release ----
card
make_zip rel.zip
R=$(install rel.zip)
echo "      $R"
C=$OUT/card
check "installed, its version read" '[ "${R%%|*}" = 1 ] && echo "$R" | grep -q "|0.1.6 (abc1234)|"'
check "the kernels replaced; the old ones kept as .old" 'cmp -s <(python3 -c "import sys; sys.stdout.buffer.write(b\"NEW KERNEL\"*50000)") "$C/macii8-rpi4.img" && [ "$(cat "$C/macii8-rpi4.img.old")" = "old kernel" ] && [ "$(cat "$C/macplus.img.old")" = "old plus" ]'
check "Wi-Fi firmware and overlays replaced or made" '[ "$(head -c 6 "$C/firmware/brcmfmac43455-sdio.bin")" = "new fw" ] && [ -f "$C/overlays/new.dtbo" ] && [ ! -e "$C/firmware/brcmfmac43455-sdio.bin.old" ]'
check "the disks, shared and roms folders left as they were" '[ "$(cat "$C/disks/OS81.hda")" = HFS ] && [ "$(cat "$C/disks/README.txt")" = "my readme" ] && [ "$(cat "$C/shared/Game.sit")" = game ] && [ "$(cat "$C/roms/quadra.rom")" = rom ]'
check "... but what is missing there made (shared/README.txt)" '[ "$(cat "$C/shared/README.txt")" = "shared readme" ]'
check "settings kept: wpa_supplicant.conf, macii.prefs" '[ "$(cat "$C/wpa_supplicant.conf")" = "network={ssid=\"home\"}" ] && [ "$(cat "$C/macii.prefs")" = "my prefs" ]'
check "config.txt kept, the release one beside it as config.txt.new" '[ "$(cat "$C/config.txt")" = "kernel=mine" ] && [ "$(cat "$C/config.txt.new")" = "kernel=release" ]'
check "cmdline.txt the same: no .new" '[ ! -e "$C/cmdline.txt.new" ]'
check "the __MACOSX folder and ._ files left out" '[ ! -e "$C/__MACOSX" ] && [ ! -e "$C/._config.txt" ]'
check "no .upd files left" '[ -z "$(find "$C" -name "*.upd")" ]'

# ---- A top folder, stored ----
card
make_zip top.zip "BMMacintosh-0.1.6/" stored
R=$(install top.zip)
check "a zip with a top folder, stored: installed from inside it" '[ "${R%%|*}" = 1 ] && [ "$(head -c 10 "$C/macii8-rpi4.img")" = "NEW KERNEL" ] && [ ! -e "$C/BMMacintosh-0.1.6" ]'

# ---- Not a release ----
card
python3 -c "
import zipfile; z = zipfile.ZipFile('$OUT/other.zip', 'w'); z.writestr('photo.jpg', 'x'); z.close()"
R=$(install other.zip)
check "a zip without a kernel: refused, nothing changed" '[ "${R%%|*}" = 0 ] && echo "$R" | grep -q "isn.t a BMMacintosh release" && [ "$(cat "$C/macii8-rpi4.img")" = "old kernel" ] && [ ! -e "$C/photo.jpg" ]'
printf 'not a zip at all' > "$OUT/junk.zip"
R=$(install junk.zip)
check "not a zip: refused" '[ "${R%%|*}" = 0 ] && echo "$R" | grep -q "isn.t a zip"'

# ---- Damaged ----
card
make_zip bad.zip
python3 - "$OUT/bad.zip" <<'PY'
import sys
p = sys.argv[1]; d = bytearray(open(p, "rb").read())
i = d.find(b"macii8-rpi4.img") + len(b"macii8-rpi4.img") + 40   # inside the kernel's compressed data
d[i] ^= 0xFF
open(p, "wb").write(d)
PY
R=$(install bad.zip)
check "a damaged zip: refused, nothing changed, nothing left" '[ "${R%%|*}" = 0 ] && [ "$(cat "$C/macii8-rpi4.img")" = "old kernel" ] && [ -z "$(find "$C" -name "*.upd")" ] && [ ! -e "$C/overlays/new.dtbo" ]'

# ---- Names going outside the card ----
card
python3 -c "
import zipfile; z = zipfile.ZipFile('$OUT/evil.zip', 'w'); z.writestr('macii8-rpi4.img', 'k'); z.writestr('../evil.txt', 'x'); z.close()"
R=$(install evil.zip)
check "a name with ..: refused, nothing changed" '[ "${R%%|*}" = 0 ] && [ ! -e "$OUT/evil.txt" ] && [ "$(cat "$C/macii8-rpi4.img")" = "old kernel" ]'

# ---- The release build_sdcard.sh staged, if there is one ----
if [ -d "$ROOT/build/sdcard" ] && ls "$ROOT/build/sdcard"/mac*.img > /dev/null 2>&1; then
	card
	rm -f "$OUT/real.zip"
	(cd "$ROOT/build/sdcard" && zip -qr "$OUT/real.zip" .)
	R=$(install real.zip)
	echo "      $R"
	check "the staged release: installed, every kernel the same as the build made" '[ "${R%%|*}" = 1 ] && for k in "$ROOT"/build/sdcard/mac*.img; do cmp -s "$k" "$C/$(basename "$k")" || exit 1; done'
	check "... its firmware and boot files too" 'cmp -s "$ROOT/build/sdcard/bootcode.bin" "$C/bootcode.bin" && cmp -s "$ROOT/build/sdcard/firmware/brcmfmac43455-sdio.bin" "$C/firmware/brcmfmac43455-sdio.bin"'
	check "... and the disk image still there" '[ "$(cat "$C/disks/OS81.hda")" = HFS ]'
fi

if [ "$FAILED" -eq 0 ]; then echo "All tests passed."; else echo "$FAILED failed."; exit 1; fi
