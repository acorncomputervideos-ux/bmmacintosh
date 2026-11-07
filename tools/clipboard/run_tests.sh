#!/bin/bash
#
# Tests the shared clipboard's text conversion (src/port/clip_text.c):
# MacRoman with CR line ends to UTF-8 with LF, and back, against Python's
# mac_roman codec.
#
#   tools/clipboard/run_tests.sh
#

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$HERE/out"
${CC:-cc} -std=gnu99 -O1 -Wall -Wextra -Werror -o "$HERE/out/clip_tool" \
    "$HERE/clip_tool.c" "$ROOT/src/port/clip_text.c" || exit 1

python3 - "$HERE/out/clip_tool" <<'PY'
import subprocess, sys
tool = sys.argv[1]
failed = 0
def run(mode, data, size=None):
    args = [tool, mode] + ([str(size)] if size else [])
    return subprocess.run(args, input=data, capture_output=True, check=True).stdout
def check(name, ok):
    global failed
    print(("PASS  " if ok else "FAIL  ") + name)
    failed += not ok

every = bytes(b for b in range(256) if b not in (10, 13))
check("all of MacRoman to UTF-8, as Python's mac_roman has it",
      run("frommac", every) == every.decode("mac_roman").encode("utf-8"))
check("and back", run("tomac", every.decode("mac_roman").encode("utf-8")) == every)
check("the Mac's CR line ends become LF", run("frommac", b"a\rb\r") == b"a\nb\n")
check("LF and CR LF become CR", run("tomac", b"a\nb\r\nc") == b"a\rb\rc")
check("a character MacRoman hasn't becomes ?", run("tomac", "x中y😀z".encode()) == b"x?y?z")
check("broken UTF-8 becomes ?", run("tomac", b"a\xffb\xc3") == b"a?b?")
check("the euro sign (Mac OS 8.5's 0xDB)", run("tomac", "€".encode()) == b"\xdb")
cut = run("frommac", "é".encode("mac_roman") * 10, size=8)
check("a full buffer stops between characters", cut == "é".encode() * 3)
print()
print("All tests passed." if not failed else f"{failed} failed.")
sys.exit(1 if failed else 0)
PY
