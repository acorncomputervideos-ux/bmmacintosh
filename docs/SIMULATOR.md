# Testing and debugging BMMacintosh on a PC

BMMacintosh isn't tested in QEMU. On the Pi it runs bare metal, on the Circle
kernel with no Linux underneath, and QEMU's Raspberry Pi machines don't
emulate enough of the Pi (its VideoCore display, HDMI sound, USB and Wi-Fi
as BMMacintosh uses them) to run it. There would be nothing to strace either:
strace follows a Linux program's system calls, and BMMacintosh makes none.

What BMMacintosh has instead is a **PC simulator** (`tools/hostsim`, from
BMCoco): the same emulator code that runs on the Pi (Basilisk II and umac,
and BMMacintosh's front ends for them, `src/port/`), built as ordinary
programs for Linux or macOS, `build/macii_sim` and `build/macplus_sim`, with
a simulated display, sound, keyboard, mouse and SD card in place of the
Pi's. Because they're ordinary programs, **gdb and lldb work on them** as
on any other program.

## What it can and can't show

The simulator runs the Macs' emulation, Mac OS, the menu, the disk
handling, ExtFS, the machine switching and BMMacintosh's pacing and sound
logic. A problem in any of those usually shows up in it too.

It does **not** run the Pi side: the Circle kernel, the Pi's display and
sound drivers, USB, Wi-Fi and Circle's network stack, the SD card driver
and newlib's file system calls (the simulator's are the PC's). And a PC is
much faster than a Pi: the simulator says nothing about whether a Pi keeps
up.

So the simulator is the first place to look, and if a problem doesn't show
up there, the Pi's log (at the end of this page) is the next.

## Setting it up

You need a C and C++ compiler, `make`, `git` and Python 3. No Raspberry Pi
toolchain is needed for the simulator.

**Linux** (Debian, Ubuntu, Raspberry Pi OS):

```
sudo apt install build-essential git python3 gdb
```

**macOS**: install Xcode's command line tools (`xcode-select --install`),
which include `clang`, `make`, `git` and the `lldb` debugger.

Get the source, with the emulators and zlib (git submodules):

```
git clone https://github.com/jhonnaker1/bmmacintosh
cd bmmacintosh
git submodule update --init --recursive third_party/macemu third_party/umac third_party/zlib
```

(`git clone --recursive` works too, but also fetches circle-stdlib, which
only the Pi build needs.)

Build and run the tests, with your Mac ROMs (Apple's aren't free, so the
tests use yours: MAME's `macqd800.zip` and `macplus.zip`, or the ROM files,
in a folder):

```
BMMAC_ROMS=~/mame/roms tools/hostsim/run_tests.sh
```

Each Mac boots its ROM to the "?" disk, and the tests check the frame
lock, the sound, the mouse, the menu, saved settings, the memory, switching
Macs, the web UI's requests and the power key. With System disks as well,
each Mac starts from its disk to the Finder:

```
BMMAC_ROMS=~/mame/roms BMMAC_II_DISK=macos81.hda BMMAC_PLUS_DISK=system.dsk \
    tools/hostsim/run_tests.sh
```

`BMMAC_II_DISK` is a raw Mac OS 8.1 hard disk (MAME's `macos81.chd`, made
raw with `tools/mac_media.py disk`; the test holds Shift while it starts,
for its A/ROSE extension), `BMMAC_PLUS_DISK` an 800K System floppy (System
4.3's *System Tools 1*, for one). They are copied before the tests use
them. Without `BMMAC_ROMS` only the tests that need no ROM run.

Each test runs in its own folder under `tools/hostsim/out/`, which stands
in for the SD card, with its log (`log.txt`) and screenshots (`.ppm`).

## Using it live

```
tools/hostsim/live.sh ii ~/mame/roms
```

(or `plus` for the Mac Plus), then open <http://127.0.0.1:8064/> in a
browser. The Mac's screen, sound, keyboard, mouse and gamepads are in the
page, and F12 opens BMMacintosh's menu as on the Pi. The simulator's SD
card is `tools/hostsim/out/live`, set up as the Pi's: the ROMs from the
folder given are linked into `roms`, and `disks` and `shared` are there
for your files.

Options from `cmdline.txt` go in `BMMAC_CMDLINE`:

```
BMMAC_CMDLINE="volume=50" tools/hostsim/live.sh ii ~/mame/roms
```

Its messages (the same as the Pi's log) go to the terminal. Ctrl+C stops
it.

## Debugging it

### Where is it stuck? (gdb or lldb)

Start it live as above, get it into the state where it's stuck, and attach
a debugger to it from another terminal.

Linux:

```
sudo gdb -p $(pgrep -f build/macii_sim)
(gdb) thread apply all bt
```

macOS:

```
lldb -p $(pgrep -f build/macii_sim)
(lldb) thread backtrace all
```

The backtrace shows where each thread is, in BMMacintosh's and the
emulator's source (the simulator is built with debugging information).
When the Mac II is running normally, the main thread is somewhere under
`m68k_execute()` (the Mac's CPU) or `cpu_do_check_ticks()` (the frame
loop, which it calls). Type `continue` to let it go on, or `detach` and
`quit` to leave it running.

To run it under the debugger from the start instead, run it from its SD
card folder:

```
cd tools/hostsim/out/live
BMMAC_LIVE=8064 BMMAC_CMDLINE="root=." lldb ../../build/macii_sim
(lldb) process launch -i /dev/null
```

The Mac's own exceptions (a bus error, an illegal instruction) go through
the CPU core's `Exception()`; a breakpoint there, with a condition on its
first argument (the 68k's exception number), finds where the Mac went
wrong.

### Scripts

The simulator can also run a script of key presses, mouse moves and
screenshots on its standard input, with time measured in frames, so that a
problem can be repeated exactly. For example:

```
cd tools/hostsim/out/live
printf '600 shot desktop.ppm\n610 key 0x45 1\n612 key 0x45 0\n640 shot menu.ppm\n641 quit\n' |
    BMMAC_CMDLINE="root=." ../../build/macii_sim
```

The commands are listed at the top of `tools/hostsim/host_sim.c`. Among
them, `netprobe dhcp` and `netprobe dns` ask the Mac II's network for an
address and look example.com up as the Mac would, and log the answers
(after the Mac has started, around frame 900, with `BMMAC_IP` set).

saves the Mac's screen at frame 600, presses F12 (USB HID usage 0x45) and
saves the menu at frame 640. The commands are listed at the top of
`tools/hostsim/host_sim.c`, and `tools/hostsim/run_tests.sh` has many
examples.

Useful settings, as environment variables:

| Variable                    | Does                                                       |
|-----------------------------|------------------------------------------------------------|
| `BMMAC_CMDLINE`             | Options as in `cmdline.txt` (`root=.` is the SD card)      |
| `BMMAC_REFRESH`             | The display's refresh rate (default 60)                    |
| `BMMAC_SIM_RATE`            | The sound output's rate (48000 for a Pi 4 or 5's HDMI)     |
| `BMMAC_IP`                  | Pretend the network is up with this address (from frame 100) |
| `BMMAC_DISPLAY`             | The display's size the Mac is told (`1920x1200`; 1280x720 otherwise) |
| `BMMAC_WIFI`                | Pretend Wi-Fi is running, with a few networks to choose from |
| `BMMAC_REFRESH_LATER`       | Change the display's refresh rate to this at frame 30      |

## On the Pi itself

On the Pi there is no debugger or strace, but there is a log. Add
`enable_logging=file` to the line in `cmdline.txt`, and BMMacintosh writes
`bmmac.log` in the root of the SD card (and to the serial port: GPIO14
TX and GPIO15 RX at 115200 baud, for a USB serial adapter, which keeps
working even when the SD card doesn't).

What the log shows:

- The Pi, the display and sound, the ROMs found and the one used, the Mac.
- The disk images opened and ejected, and the emulators' own messages
  (Basilisk II's warnings; where the Mac reads or writes memory it doesn't
  have).
- If the emulation stops moving for 3 seconds, `Emulator has not moved for
  3 s; last seen:` and what it was doing. That line is the most useful one
  for a hang.

When something goes wrong, the most helpful report has:

1. What you did, step by step, and what happened.
2. The BMMacintosh version and which Pi.
3. `bmmac.log` from the card, with `enable_logging=file` (taking the
   card out right after the problem; the log is written as it goes).
4. Your `cmdline.txt`, `bmmac.cfg` and `macii.prefs`.
5. Which ROM (its checksum, from the log or the menu) and which System
   software and program.
