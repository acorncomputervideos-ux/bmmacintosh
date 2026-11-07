#!/bin/bash
#
# Runs both emulator ports on the PC (see host_sim.c) through a set of
# scripted tests and checks the results: the Mac II and Quadra
# (build/macii_sim, Basilisk II) and the Mac Plus (build/macplus_sim,
# umac).
#
#   BMMAC_ROMS=/path/to/roms tools/hostsim/run_tests.sh
#
# Apple's ROMs aren't free, so the tests use the user's: BMMAC_ROMS is a
# folder with MAME's macqd800.zip (the Quadra 650's and 800's ROM) and
# macplus.zip, or those ROMs as files. The files there whose names start
# with "mac", or end in .rom, are linked into each test's stand-in SD card.
# Without BMMAC_ROMS only the tests that need no ROM run. With them, each
# Mac boots its ROM to the "?" disk.
#
# With a System disk too, each Mac starts from it to the Finder:
#
#   BMMAC_II_DISK=macos81.hda     a raw Mac OS 8.1 hard disk (MAME's
#                                 macos81.chd made raw: tools/mac_media.py)
#   BMMAC_PLUS_DISK=system.dsk    a System 4.3 (or other) floppy or HFS volume
#
# They are copied first (the Macs write to them).
#
# Output, including screenshots (.ppm), goes to tools/hostsim/out/.
#

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$HERE/out
II=$HERE/build/macii_sim
PLUS=$HERE/build/macplus_sim

make -s -C "$HERE" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)" || exit 1

rm -rf "$OUT"
mkdir -p "$OUT/roms" "$OUT/empty"
HAVE_ROMS=0
if [ -n "${BMMAC_ROMS:-}" ]; then
    for f in "$BMMAC_ROMS"/mac* "$BMMAC_ROMS"/*.rom "$BMMAC_ROMS"/*.ROM; do
        [ -f "$f" ] && ln -sf "$f" "$OUT/roms/" && HAVE_ROMS=1
    done
fi

FAILED=0
SKIPPED=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1: $2"; FAILED=$((FAILED + 1)); }
skip() { echo "SKIP  $1"; SKIPPED=$((SKIPPED + 1)); }

# A copy of a disk image (a clone where the file system can).
copy_disk() {
    cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"
}

# run SIM NAME REFRESH CMDLINE SCRIPT: runs a simulator in out/NAME, a
# stand-in SD card with the ROMs in its roms folder and the Pi's
# config.txt.
run() {
    local sim=$1 name=$2 refresh=$3 cmdline=$4 script=$5
    mkdir -p "$OUT/$name"
    [ -e "$OUT/$name/roms" ] || ln -s "$OUT/roms" "$OUT/$name/roms"
    [ -e "$OUT/$name/config.txt" ] || cp "$ROOT/sdcard/config.txt" "$OUT/$name/config.txt"
    (cd "$OUT/$name" && printf "$script" | BMMAC_REFRESH=$refresh \
        BMMAC_CMDLINE="root=. $cmdline" "$sim" > log.txt 2>&1)
}

# check_log NAME REGEX MESSAGE: the log must contain REGEX
check_log() {
    if grep -Eq "$2" "$OUT/$1/log.txt"; then
        pass "$1: $3"
    else
        fail "$1" "$3 (see out/$1/log.txt)"
    fi
}

# check_cfg NAME FILE REGEX MESSAGE: out/NAME/FILE must have a line REGEX
check_cfg() {
    if grep -Eq "$3" "$OUT/$1/$2" 2>/dev/null; then
        pass "$1: $4"
    else
        fail "$1" "$4 (not in out/$1/$2)"
    fi
}

no_underruns() {
    if grep -q "audio underrun" "$OUT/$1/log.txt"; then
        fail "$1" "audio underrun (see out/$1/log.txt)"
    else
        pass "$1: no audio underruns"
    fi
}

# every_frame NAME: every display refresh showed a frame
every_frame() {
    local line frame presented
    line=$(grep "frames presented" "$OUT/$1/log.txt" | tail -1)
    frame=$(echo "$line" | sed 's/.*frame \([0-9]*\):.*/\1/')
    presented=$(echo "$line" | sed 's/.*: \([0-9]*\) frames presented/\1/')
    if [ -n "$frame" ] && [ "${presented:-0}" -ge "$frame" ]; then
        pass "$1: no dropped frames ($presented/$frame)"
    else
        fail "$1" "frames presented: $line"
    fi
}

# sound_made NAME: something other than silence reached the output
sound_made() {
    local changes
    changes=$(grep "changing samples" "$OUT/$1/log.txt" | tail -1 | sed 's/.*changing samples \([0-9]*\).*/\1/')
    if [ "${changes:-0}" -gt 0 ]; then
        pass "$1: sound output"
    else
        fail "$1" "no sound reached the output"
    fi
}

# screen NAME FILE: what's on the screenshot: "question" (the gray desktop
# with the "?" disk in its middle), "finder" (a menu bar along the top),
# "menu" (BMMacintosh's: mostly white), or "other"
screen() {
    python3 - "$OUT/$1/$2.ppm" <<'EOF'
import sys
try:
    data = open(sys.argv[1], 'rb').read()
except OSError:
    print('none')
    sys.exit(0)
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
def black(x, y):
    i = (y * w + x) * 3
    return px[i] < 64 and px[i + 1] < 64 and px[i + 2] < 64
def white(x, y):
    i = (y * w + x) * 3
    return px[i] > 192 and px[i + 1] > 192 and px[i + 2] > 192
total = w * h
blacks = sum(1 for y in range(h) for x in range(w) if black(x, y))
whites = sum(1 for y in range(h) for x in range(w) if white(x, y))
cx, cy = w // 2, h // 2
mid = [(x, y) for y in range(cy - 12, cy + 12) for x in range(cx - 10, cx + 10)]
mid_white = sum(1 for x, y in mid if white(x, y)) / len(mid)
# The menu bar: a white band at the top with a black line under it.
bar = max((y for y in range(10, 30) if sum(black(x, y) for x in range(w)) > 0.9 * w),
          default=None)
bar_white = bar is not None and sum(white(x, 3) for x in range(w)) > 0.6 * w
if bar_white:
    print('finder')
elif 0.4 < blacks / total < 0.6 and mid_white > 0.5:
    print('question')
elif whites / total > 0.7:
    print('menu')
else:
    print('other')
EOF
}

# shows NAME FILE WHAT MESSAGE: the screenshot is WHAT
shows() {
    local s
    s=$(screen "$1" "$2")
    if [ "$s" = "$3" ]; then
        pass "$1: $4 ($2)"
    else
        fail "$1" "$4: $2.ppm is \"$s\""
    fi
}
question() { shows "$1" "$2" question "the \"?\" disk"; }
menu_shown() { shows "$1" "$2" menu "the menu"; }
finder() { shows "$1" "$2" finder "the Finder's desktop"; }

same() {
    if [ -f "$OUT/$1/$2.ppm" ] && [ -f "$OUT/$1/$3.ppm" ] && cmp -s "$OUT/$1/$2.ppm" "$OUT/$1/$3.ppm"; then
        pass "$1: $2 and $3 are the same"
    else
        fail "$1" "$2 and $3 differ"
    fi
}
differ() {
    if [ -f "$OUT/$1/$2.ppm" ] && [ -f "$OUT/$1/$3.ppm" ] && ! cmp -s "$OUT/$1/$2.ppm" "$OUT/$1/$3.ppm"; then
        pass "$1: $2 and $3 differ"
    else
        fail "$1" "$2 and $3 should differ"
    fi
}

# USB HID usages used below.
F12=0x45 DOWN=0x51 UP=0x52 ENTER=0x28 ESC=0x29 POWER=0x66 LSHIFT=0xe1

# KEY USAGE FRAME: press at FRAME, release two frames later
key() { printf '%d key %s 1\\n%d key %s 0\\n' "$2" "$1" $(($2 + 2)) "$1"; }
# DOWNS N FRAME: N presses of Down from FRAME, four frames apart
downs() {
    local i
    for ((i = 0; i < $1; i++)); do key $DOWN $(($2 + i * 4)); done
}

echo "The web UI's pages and the Pi's web server"
# The server (Pi only) refuses saves and shutdowns without the header the
# pages send, so the two names must stay the same.
sent=$(grep -o '"X-[A-Za-z]*-Web"' "$ROOT/src/webui/assets/js/api.js" | tr -d '"' | tr 'A-Z' 'a-z')
if [ -n "$sent" ] && grep -q "HasHeader(request, \"$sent:\")" "$ROOT/src/webui/webui.cpp" \
   && ! grep "HasHeader(request, \"x-" "$ROOT/src/webui/webui.cpp" | grep -vq "\"$sent:\""; then
    pass "webui: the server checks the header the pages send ($sent)"
else
    fail webui "the pages send ${sent:-no header}, which src/webui/webui.cpp doesn't check for"
fi

echo "No ROM: both say what's missing"
mkdir -p "$OUT/norom_ii" "$OUT/norom_plus"
ln -s "$OUT/empty" "$OUT/norom_ii/roms"
ln -s "$OUT/empty" "$OUT/norom_plus/roms"
run "$II" norom_ii 60 "" '120 shot a.ppm\n121 quit\n'
check_log norom_ii "No Mac II or Quadra ROM" "the Mac II asks for a ROM"
menu_shown norom_ii a
run "$PLUS" norom_plus 60 "" '120 shot a.ppm\n121 quit\n'
check_log norom_plus "No Mac Plus ROM" "the Mac Plus asks for a ROM"
menu_shown norom_plus a

# The web page's Disks, through the queue the emulator takes them from
# between frames (no ROM is needed for the Mac II's hard disk list).
mkdir -p "$OUT/disks_ii/disks" "$OUT/disks_plus/disks"
ln -s "$OUT/empty" "$OUT/disks_ii/roms"
ln -s "$OUT/empty" "$OUT/disks_plus/roms"
head -c 1048576 /dev/zero > "$OUT/disks_ii/disks/a.hda"
head -c 1048576 /dev/zero > "$OUT/disks_ii/disks/b.hda"
head -c 819200 /dev/zero > "$OUT/disks_ii/disks/f.dsk"
head -c 819200 /dev/zero > "$OUT/disks_plus/disks/sys.dsk"
run "$II" disks_ii 60 "" '100 disk add 0 /disks/a.hda
110 disk add 0 /disks/b.hda
120 disk add 0 /disks/a.hda
130 disk remove 0
140 disk insert 0 /disks/f.dsk
150 quit
'
check_log disks_ii "Web UI: a.hda is a hard disk when the Mac restarts" "Disks: a hard disk added"
check_log disks_ii "Web UI: a.hda is a hard disk already" "Disks: not twice"
check_log disks_ii "Web UI: a.hda is gone when the Mac restarts" "Disks: a hard disk removed"
check_log disks_ii "Web UI: The Mac isn't running" "Disks: no floppy drive while the Mac isn't running"
check_cfg disks_ii macii.prefs "^disk .*/disks/b\.hda$" "Disks: macii.prefs has the hard disk left"
if grep -q "a\.hda" "$OUT/disks_ii/macii.prefs" 2>/dev/null; then
    fail disks_ii "Disks: the removed hard disk is still in macii.prefs"
else
    pass "disks_ii: Disks: the removed one is gone from macii.prefs"
fi
run "$PLUS" disks_plus 60 "" '100 disk start 0 /disks/sys.dsk
150 quit
'
check_log disks_plus "Web UI: The Mac starts again, from sys.dsk" "Disks: the Mac Plus starts from a disk"
check_cfg disks_plus bmmac.cfg "plus_disk.*/disks/sys\.dsk" "Disks: bmmac.cfg has the Mac Plus's disk"

if [ "$HAVE_ROMS" -eq 0 ]; then
    for t in "Mac II boot" "Mac Plus boot" "menus" "machine switching" "web UI requests" \
             "System disks"; do
        skip "$t (no BMMAC_ROMS)"
    done
    echo
    echo "$FAILED failed, $SKIPPED skipped (set BMMAC_ROMS to run them)."
    exit $((FAILED > 0))
fi

echo
echo "Mac II and Quadra (Basilisk II)"

echo "Boot the Quadra ROM to the \"?\" disk, locked to a 60 Hz display"
# BMMAC_IP: the "Pi's" network comes up at frame 100, and the Mac's with it.
BMMAC_IP=192.168.1.50 run "$II" iiboot 60 "" '600 shot a.ppm\n601 shot b.ppm\n900 audio\n901 quit\n'
check_log iiboot "Quadra 610, 650 and 800 \(F1ACAD13\), 68040, 32 MB" "the Quadra ROM, a 68040, 32 MB"
check_log iiboot "Mac 60\.15 Hz, synchronised to the display" "locked to the display"
check_log iiboot "Mac screen 640x480, VDEPTH_8BIT" "a 640x480 screen in 256 colours"
check_log iiboot "the Mac's network is up \(10\.0\.2\.15, through 192\.168\.1\.50, DNS server [0-9.]+\)" "the Mac's network (slirp) comes up with the Pi's, with its DNS server"

echo "The Mac's network, asked as the Mac would (netprobe)"
BMMAC_IP=192.168.1.50 run "$II" iinetprobe 60 "" '900 netprobe dhcp\n1000 netprobe dns\n1300 quit\n'
check_log iinetprobe "netprobe: DHCP offer of 10\.0\.2\.15" "slirp's DHCP gives 10.0.2.15"
if host example.com > /dev/null 2>&1 || nslookup example.com > /dev/null 2>&1; then
    check_log iinetprobe "netprobe: DNS answer, [1-9][0-9]* answer\(s\), code 0" "a name looked up through slirp"
else
    skip "netprobe: DNS (this computer is offline)"
fi
BMMAC_IP=192.168.1.50 run "$II" iinetprobe2 60 "netprobe=1" '1500 quit\n'
check_log iinetprobe2 "netprobe: DHCP offer of 10\.0\.2\.15" "netprobe=1 runs the test by itself once the network is up"
check_cfg iiboot macii.prefs "^rom roms/.*f1acad13" "macii.prefs names the ROM"
check_cfg iiboot macii.prefs "^extfs .*shared" "macii.prefs shares the card's shared folder"
question iiboot a
same iiboot a b
every_frame iiboot
no_underruns iiboot

echo "Starting again from the saved settings"
mkdir -p "$OUT/iiboot2"
cp "$OUT/iiboot/macii.prefs" "$OUT/iiboot2/"
run "$II" iiboot2 60 "" '600 shot a.ppm\n601 quit\n'
question iiboot2 a

echo "The bigger screens (Apple's monitors' sizes)"
for size in 832x624 1152x870 1280x1024; do
    mkdir -p "$OUT/ii$size"
    printf 'screen win/%s/%s\n' "${size%x*}" "${size#*x}" > "$OUT/ii$size/macii.prefs"
    run "$II" "ii$size" 60 "" '400 shot a.ppm\n401 quit\n'
    check_log "ii$size" "Mac screen $size, VDEPTH_8BIT" "a $size screen"
    question "ii$size" a
done
# 1600x1200 needs a display of 800 lines or more (the Pi's display shrinks
# a picture to 2/3 at most): on a 1920x1200 one it is there, on 720p the
# Mac starts at 640x480 instead.
mkdir -p "$OUT/ii1600x1200" "$OUT/ii1600at720"
printf 'screen win/1600/1200\n' > "$OUT/ii1600x1200/macii.prefs"
printf 'screen win/1600/1200\n' > "$OUT/ii1600at720/macii.prefs"
BMMAC_DISPLAY=1920x1200 run "$II" ii1600x1200 60 "" '400 shot a.ppm\n401 quit\n'
check_log ii1600x1200 "Mac screen 1600x1200, VDEPTH_8BIT" "a 1600x1200 screen on a 1920x1200 display"
question ii1600x1200 a
run "$II" ii1600at720 60 "" '400 shot a.ppm\n401 quit\n'
check_log ii1600at720 "Mac screen 640x480, VDEPTH_8BIT" "no 1600x1200 on a 720p display: 640x480"

echo "On a 50 Hz display"
run "$II" iitimer 50 "" '600 shot a.ppm\n900 audio\n901 quit\n'
check_log iitimer "timer paced" "the Mac's 60.15 Hz on a 50 Hz display is timer paced"
question iitimer a
no_underruns iitimer

echo "Mouse"
run "$II" iimouse 60 "" '600 shot before.ppm\n610 mouse 40 30 0\n611 mouse 40 30 0\n612 mouse 40 30 0\n640 shot after.ppm\n641 quit\n'
differ iimouse before after

echo "Menu (F12), and a gamepad's Home"
run "$II" iimenu 60 "" "600 shot before.ppm\n$(key $F12 610)630 shot menu.ppm\n$(key $ESC 640)660 shot after.ppm\n661 quit\n"
menu_shown iimenu menu
same iimenu before after
run "$II" iipadmenu 60 "" '600 pad 0 0\n610 pad 0 0x0100\n614 pad 0 0\n630 shot menu.ppm\n631 quit\n'
menu_shown iipadmenu menu

echo "Settings are saved"
# Menu, Video (eight down), Show Disk Activity (five down): off.
run "$II" iisettings 60 "" "$(key $F12 600)$(downs 8 610)$(key $ENTER 650)$(downs 5 660)$(key $ENTER 690)$(key $F12 700)720 quit\n"
check_cfg iisettings bmmac.cfg "^disk_activity=0" "Video > Show Disk Activity saved to bmmac.cfg"

echo "Memory: 64 MB, and the Mac restarts"
# Menu, Memory (five down), one down from 32 MB, then out.
run "$II" iimemory 60 "" "$(key $F12 600)$(downs 5 610)$(key $ENTER 640)$(downs 1 650)$(key $ENTER 660)$(key $ESC 670)1300 shot a.ppm\n1301 quit\n"
check_cfg iimemory macii.prefs "^ramsize 67108864" "64 MB saved in macii.prefs"
check_log iimemory "68040, 64 MB" "the Mac restarted with 64 MB"
question iimemory a

echo "A disk from the web UI"
python3 -c "open('$OUT/blank.dsk','wb').write(bytes(819200))"
run "$II" iistart 60 "" "600 run $OUT/blank.dsk\n900 shot a.ppm\n901 quit\n"
check_log iistart "Web UI: starting .*blank\.dsk" "starts a disk image"
check_log iistart "disk .*blank\.dsk, 800 KB" "the Mac restarted with it in the floppy drive"
check_log iistart "ejected .*blank\.dsk" "and ejects it: it isn't a Mac disk"

echo "A floppy from the menu, into an empty drive (issue #4)"
# Menu, Insert a Floppy Disk (the first item), the first file in disks.
mkdir -p "$OUT/iifloppy/disks"
cp "$OUT/blank.dsk" "$OUT/iifloppy/disks/blank.dsk"
run "$II" iifloppy 60 "" "$(key $F12 600)$(key $ENTER 620)$(key $ENTER 650)700 quit\n"
check_log iifloppy "disk .*disks/blank\.dsk, 800 KB" "goes into the empty floppy drive"
if grep -q "floppy drives are full" "$OUT/iifloppy/log.txt"; then
    fail iifloppy "the menu said both floppy drives were full"
else
    pass "iifloppy: no \"drives are full\" message"
fi

echo "Web UI requests"
run "$II" iireboot 60 "" '600 webui reboot\n660 quit\n'
check_log iireboot "reboot requested" "reboots"
run "$II" iicoldstart 60 "" '600 webui coldstart\n900 shot a.ppm\n901 quit\n'
check_log iicoldstart "Web UI: restart" "restarts the Mac"
question iicoldstart a

echo "Switching to the Mac Plus"
# Menu, Machine (seven down), Mac Plus, Yes (up from No).
run "$II" iiswitch 60 "" "$(key $F12 600)$(downs 7 610)$(key $ENTER 640)$(key $ENTER 650)$(key $UP 660)$(key $ENTER 670)700 quit\n"
check_cfg iiswitch config.txt "^kernel=macplus_2712\.img" "config.txt starts the Mac Plus on the Pi 5"
check_cfg iiswitch config.txt "^kernel=macplus8-rpi4\.img" "and on the Pi 4"
check_cfg iiswitch config.txt "^kernel=macplus8-32\.img" "the Pi 3 is left alone"
check_log iiswitch "reboot requested" "and the Pi reboots"

echo "The power key (Pi 400: Fn+F10) shuts down"
run "$II" iipowerkey 60 "" "$(key $POWER 600)660 quit\n"
check_log iipowerkey "power off requested" "the power key powers off"

if [ -n "${BMMAC_II_DISK:-}" ] && [ -f "$BMMAC_II_DISK" ]; then
    echo "Mac OS from a hard disk, Shift held (extensions off), to the Finder"
    mkdir -p "$OUT/iisystem/shared"
    copy_disk "$BMMAC_II_DISK" "$OUT/iisystem/system.hda"
    echo "Hello from the SD card" > "$OUT/iisystem/shared/Hello.txt"
    printf 'disk %s\n' "$OUT/iisystem/system.hda" > "$OUT/iisystem/macii.prefs"
    run "$II" iisystem 60 "" "1 key $LSHIFT 1\n2400 key $LSHIFT 0\n3600 shot a.ppm\n3601 quit\n"
    finder iisystem a
    if [ -d "$OUT/iisystem/shared/.finf" ]; then
        pass "iisystem: the Mac uses the SD card's shared folder (ExtFS)"
    else
        fail iisystem "no .finf in the shared folder: ExtFS wasn't mounted"
    fi
else
    skip "Mac OS from a hard disk (no BMMAC_II_DISK)"
fi

echo
echo "Mac Plus (umac)"

echo "Boot the Mac Plus ROM to the \"?\" disk, 4 MB (after its memory test)"
run "$PLUS" plusboot 60 "" '1800 shot a.ppm\n1801 shot b.ppm\n1900 audio\n1901 quit\n'
check_log plusboot "Macintosh Plus, 4 MB, ROM roms/.*342-0341-c" "the Plus ROM put together from MAME's two chips"
check_log plusboot "Mac 60\.15 Hz, synchronised to the display" "locked to the display"
check_cfg plusboot bmmac.cfg "^plus_rom=roms/" "bmmac.cfg names the ROM"
question plusboot a
same plusboot a b
every_frame plusboot
no_underruns plusboot
sound_made plusboot

echo "A Mac 128K's memory"
run "$PLUS" plus128 60 "plus_ram=128" '300 shot a.ppm\n301 quit\n'
check_log plus128 "Macintosh Plus, 128K" "128K"
question plus128 a

echo "Mouse"
run "$PLUS" plusmouse 60 "plus_ram=512" '900 shot before.ppm\n910 mouse 40 30 0\n911 mouse 40 30 0\n912 mouse 40 30 0\n940 shot after.ppm\n941 quit\n'
differ plusmouse before after

echo "Menu (F12)"
run "$PLUS" plusmenu 60 "plus_ram=512" "900 shot before.ppm\n$(key $F12 910)930 shot menu.ppm\n$(key $ESC 940)960 shot after.ppm\n961 quit\n"
menu_shown plusmenu menu
question plusmenu after

echo "A blank disk: read, and ejected (it isn't a Mac disk)"
cp "$OUT/blank.dsk" "$OUT/plusdisk.dsk"
run "$PLUS" plusdisk 60 "plus_ram=512 plus_disk=$OUT/plusdisk.dsk" '900 shot a.ppm\n901 quit\n'
check_log plusdisk "disk .*plusdisk\.dsk, 800 KB" "the disk is opened"
check_log plusdisk "the Mac ejected its disk" "the Mac reads it and ejects it"
question plusdisk a

echo "Memory: 1 MB from the menu"
# Menu, Memory (three down), two up from 4 MB.
run "$PLUS" plusmemory 60 "" "$(key $F12 600)$(downs 3 610)$(key $ENTER 630)$(key $UP 640)$(key $UP 644)$(key $ENTER 650)900 quit\n"
check_cfg plusmemory bmmac.cfg "^plus_ram=1024" "1 MB saved in bmmac.cfg"
check_log plusmemory "Macintosh Plus, 1 MB" "the Mac restarted with 1 MB"

echo "Switching to the Mac II"
# Menu, Machine (five down), Mac II and Quadra (one down), Yes.
run "$PLUS" plusswitch 60 "" "$(key $F12 600)$(downs 5 610)$(key $ENTER 640)$(downs 1 650)$(key $ENTER 660)$(key $UP 670)$(key $ENTER 680)700 quit\n"
check_cfg plusswitch config.txt "^kernel=macii_2712\.img" "config.txt starts the Mac II on the Pi 5"
check_cfg plusswitch config.txt "^kernel=macii8-rpi4\.img" "and on the Pi 4"
check_log plusswitch "reboot requested" "and the Pi reboots"

echo "Web UI: a disk image"
run "$PLUS" plusstart 60 "" "900 run $OUT/plusdisk.dsk\n1000 quit\n"
check_cfg plusstart bmmac.cfg "^plus_disk=.*plusdisk\.dsk" "the disk is the Mac's"
check_log plusstart "disk .*plusdisk\.dsk" "the Mac restarted with it"

if [ -n "${BMMAC_PLUS_DISK:-}" ] && [ -f "$BMMAC_PLUS_DISK" ]; then
    echo "System from a floppy, to the Finder"
    mkdir -p "$OUT/plussystem"
    copy_disk "$BMMAC_PLUS_DISK" "$OUT/plussystem/system.dsk"
    run "$PLUS" plussystem 60 "plus_disk=$OUT/plussystem/system.dsk" '2400 shot a.ppm\n2401 quit\n'
    finder plussystem a
    if python3 -c "import sys; d=open(sys.argv[1],'rb').read(); sys.exit(not (len(d) == 24 and d[0] == 0xa8))" \
           "$OUT/plussystem/macplus.pram" 2>/dev/null; then
        pass "plussystem: the clock chip's parameter RAM is kept in macplus.pram"
    else
        fail plussystem "no valid macplus.pram (the clock chip)"
    fi

    echo "The same floppy as a DiskCopy 4.2 image"
    mkdir -p "$OUT/plusdc42"
    python3 - "$BMMAC_PLUS_DISK" "$OUT/plusdc42/system.image" <<'EOF2'
import struct, sys
data = open(sys.argv[1], 'rb').read()
def checksum(b):
    s = 0
    for i in range(0, len(b), 2):
        s = (s + (b[i] << 8 | b[i + 1])) & 0xffffffff
        s = (s >> 1 | s << 31) & 0xffffffff
    return s
name = b'System'
head = bytes([len(name)]) + name.ljust(63, b'\0')
head += struct.pack('>IIII', len(data), 0, checksum(data), 0) + bytes([1, 0x22, 1, 0])
open(sys.argv[2], 'wb').write(head + data)
EOF2
    run "$PLUS" plusdc42 60 "plus_disk=$OUT/plusdc42/system.image" '2400 shot a.ppm\n2401 quit\n'
    check_log plusdc42 "disk .*system\.image, [0-9]+ KB, DiskCopy 4\.2" "the DiskCopy header is skipped"
    finder plusdc42 a
    if python3 - "$OUT/plusdc42/system.image" <<'EOF2'
import struct, sys
d = open(sys.argv[1], 'rb').read()
size, _, stored = struct.unpack('>III', d[64:76])
s = 0
for i in range(84, 84 + size, 2):
    s = (s + (d[i] << 8 | d[i + 1])) & 0xffffffff
    s = (s >> 1 | s << 31) & 0xffffffff
sys.exit(s != stored)
EOF2
    then
        pass "plusdc42: its checksum is right after the Mac wrote to it"
    else
        fail plusdc42 "the DiskCopy checksum doesn't match the data"
    fi

    echo "The same floppy as a partitioned hard disk's HFS partition"
    mkdir -p "$OUT/pluspart"
    python3 - "$BMMAC_PLUS_DISK" "$OUT/pluspart/system.hda" <<'EOF2'
import struct, sys
data = open(sys.argv[1], 'rb').read()
start = 64                       # blocks: the map's, then a driver's gap
blocks = len(data) // 512
def entry(first, count, name, kind):
    e = b'PM' + b'\0\0' + struct.pack('>III', 2, first, count)
    e += name.ljust(32, b'\0') + kind.ljust(32, b'\0')
    return e.ljust(512, b'\0')
ddm = (b'ER' + struct.pack('>HI', 512, start + blocks)).ljust(512, b'\0')
pm = entry(1, 63, b'Apple', b'Apple_partition_map') + entry(start, blocks, b'MacOS', b'Apple_HFS')
open(sys.argv[2], 'wb').write((ddm + pm).ljust(start * 512, b'\0') + data)
EOF2
    run "$PLUS" pluspart 60 "plus_disk=$OUT/pluspart/system.hda" '2400 shot a.ppm\n2401 quit\n'
    check_log pluspart "the disk's HFS partition, 32 KB in" "the partition map is skipped"
    check_log pluspart "disk .*system\.hda, [0-9]+ KB$" "the Mac gets the HFS partition alone"
    finder pluspart a
else
    skip "System from a floppy (no BMMAC_PLUS_DISK)"
fi

echo
if [ $FAILED -eq 0 ]; then
    echo "All tests passed ($SKIPPED skipped)."
else
    echo "$FAILED test(s) failed."
fi
exit $((FAILED > 0))
