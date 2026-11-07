#!/bin/bash
#
# Tests the Mac II's SD Card disk (Basilisk II's ExtFS) on the PC: its
# file positions on FatFs (src/port/ii/extfs_posix.cpp, see
# test_extfs_posix.cpp) and macOS's AppleDouble files
# (src/port/ii/extfs_ii.cpp, see test_appledouble.cpp).
#
#   tools/extfs_posix/run_tests.sh
#

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$HERE/out
mkdir -p "$OUT"
rm -rf "$OUT/files"
mkdir -p "$OUT/files"

${CXX:-c++} -std=gnu++14 -O1 -g -Wall -Wextra -o "$OUT/test_extfs_posix" \
    "$HERE/test_extfs_posix.cpp" "$ROOT/src/port/ii/extfs_posix.cpp" \
    -Wl,--wrap=lseek -Wl,--wrap=ftruncate
"$OUT/test_extfs_posix" "$OUT/files"

# extfs_ii.cpp with Basilisk II's headers, as tools/hostsim builds it; its
# generated CPU table header from there too.
make -s -C "$ROOT/tools/hostsim" build/gen/cputbl.h >/dev/null
B2=$ROOT/third_party/macemu/BasiliskII/src
rm -rf "$OUT/ad"
mkdir -p "$OUT/ad"
# open() as circle-newlib's on the Pi: ENOTDIR for a missing folder.
${CXX:-c++} -std=gnu++14 -O1 -g -w -DUPDATE_UAE -DFPU_UAE -DEXTFS_POSIX_TEST_CIRCLE_ERRNO \
    -I "$ROOT/src/port/ii" -I "$B2/uae_cpu_2021" -I "$B2/include" \
    -I "$ROOT/tools/hostsim/build/gen" -o "$OUT/test_appledouble" \
    "$HERE/test_appledouble.cpp" "$ROOT/src/port/ii/extfs_posix.cpp"
"$OUT/test_appledouble" "$OUT/ad"
