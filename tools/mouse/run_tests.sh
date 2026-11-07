#!/bin/bash
#
# Tests the USB mouse's smoothing (src/port/mouse_track.c) on the PC: see
# test_mouse.c.
#
#   tools/mouse/run_tests.sh
#
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$HERE/out"
${CC:-cc} -std=gnu99 -O1 -g -Wall -Wextra -Werror -o "$HERE/out/test_mouse" \
    "$HERE/test_mouse.c" "$ROOT/src/port/mouse_track.c" -lm || exit 1
"$HERE/out/test_mouse"
