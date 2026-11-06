# BMMacintosh

BMMacintosh is a bare metal 68k Macintosh emulator for the Raspberry Pi: a
Mac II or Quadra running System 7 to Mac OS 8.1 on a Pi 4 or 5, and a Mac
Plus on every Pi. It runs the emulators directly on the hardware, with no
Linux underneath, the same way [BMC64](https://github.com/randyrossi/bmc64)
does for the Commodore 64, [BMA800XL](https://github.com/aminch/bma800xl)
for the Atari 800XL, [BMCoco](https://github.com/jhonnaker1/bmcoco) for the
Tandy Color Computer, [BMApple2](https://github.com/jhonnaker1/bmapple2)
for the Apple II and [BMAtariST](https://github.com/jhonnaker1/bmatarist)
for the Atari ST.

Each Mac is a kernel of its own, with its own emulator:

- **Mac II and Quadra** (`macii*.img`, Pi 4 and 5):
  [Basilisk II](https://github.com/kanjitalk755/macemu), the 68040 Mac with
  an FPU, 8 to 256 MB of memory and a screen of up to 1600x1200 in millions
  of colours. It runs the ROM of a IIci or a Quadra (best a Quadra 650's or
  800's) and System 7 to Mac OS 8.1.
- **Mac Plus** (`macplus*.img`, every Pi):
  [umac](https://github.com/evansm7/umac) by Matt Evans, the core of
  pico-mac, with Karl Stenerud's Musashi 68000: the Mac 128K, 512K and
  Plus, 128K to 4 MB, the 512x342 black and white screen. It runs the Mac
  Plus's ROM and the System software of its time.

The menu's **Machine** item switches between them: it changes the
`kernel=` lines of `config.txt` and restarts the Pi, as BMC64 switched
between its machines. Both use the same SD card: the ROMs, the disks, the
settings and the web UI.

BMMacintosh is based on BMAtariST, which is based on BMApple2, BMCoco,
[BMA800XL](https://github.com/aminch/bma800xl) by Adam Minchinton and
[BMC64](https://github.com/randyrossi/bmc64) by Randy Rossi: the kernel,
display, sound, network and web UI come from those projects.

BMMacintosh wouldn't be possible without the work behind the open source
projects it builds on. Thanks to their authors and contributors for making
it available; see the [third-party](docs/THIRD_PARTY.md) acknowledgements.

- Disk images on the SD card: hard disks (raw, or with an Apple partition
  map), floppies (raw 400K, 800K and 1.4 MB, and DiskCopy 4.2) and CD-ROMs
  (`.iso`, `.toast`) on the Mac II; a floppy image (raw or DiskCopy 4.2) or
  a hard disk (raw or partitioned) on the Mac Plus. Written in place, so the Pi can be switched off at any time
- On the Mac II, a folder on the card (`shared`) is a disk on the Mac's
  desktop, *SD Card*: copy files to it from a PC, or over the network
- On the Mac II, the network: with the Pi on Ethernet or Wi-Fi, Mac OS's
  TCP/IP (set to DHCP) reaches the internet through the Pi, as QEMU's user
  networking does (slirp)
- Frames locked to the display's refresh, the Mac's 60.15 Hz (the Pi 4 and
  5 switch the display to it), with the sound kept in step
- USB keyboards (Alt is Command, the Windows key Option), USB mice, and
  USB gamepads as the mouse
- A menu (F12) for the disks, the ROM, the memory, the machine, video,
  sound and network
- Optional Ethernet or Wi-Fi: the SD card is a network drive in Windows,
  macOS and Linux file managers (WebDAV), and a web page has the Pi's
  settings and controls

## ROMs and System software

Apple's ROMs and system software aren't free, so BMMacintosh comes
without: put your own on the card. A Mac ROM goes in the card's `roms`
folder as a file, or as MAME's zipped set as it is (`macqd800.zip`,
`maciici.zip`, `macplus.zip`: BMMacintosh puts the ROMs that MAME keeps in
two or four chips back together itself). See
[docs/ROMS.txt](docs/ROMS.txt). Disk images go in `disks`.
`tools/mac_media.py` makes plain ROM files and raw disk images from MAME's
sets and CHD hard disks.

## Supported hardware

The **Raspberry Pi 4 Model B and Pi 400** (64-bit) and the **Raspberry
Pi 5, Pi 500 and Pi 500+** (64-bit) run both Macs: `macii8-rpi4.img` and
`macplus8-rpi4.img`, `macii_2712.img` and `macplus_2712.img`. See
[docs/PI4.md](docs/PI4.md) and [docs/PI5.md](docs/PI5.md).

The Raspberry Pi 3 Model B, B+ and A+ and the Pi Zero 2 W
(`macplus8-32.img`), the Pi 2 Model B (`macplus7.img`) and the Pi Zero and
Zero W (`macplus.img`) run the Mac Plus.

BMMacintosh has been tested in the PC simulator (below) and, for the
Mac II, on a Raspberry Pi 5, where it starts Mac OS 8.1. The Mac Plus, the
Mac II's network and the other Pis haven't been tried on a Pi yet.

## Getting started

1. Format an SD card as FAT32.
2. Copy the contents of a release zip (or of `build/sdcard/` after building)
   to the root of the card.
3. Copy your ROMs to `roms` and your disk images to `disks`.
4. Boot the Pi with a USB keyboard and mouse connected. A Pi 4 or 5 starts
   as a Mac II, the others as a Mac Plus: the Mac starts, to the flashing
   "?" disk until it has a disk to start from. Press **F12**, choose the
   disk (*Hard Disks* on the Mac II, *Disk* on the Mac Plus), then
   *Restart the Mac*.

To start Mac OS with its extensions off, hold Shift while it starts. That
is how to get past an extension that stops it: Mac OS 8.1 from MAME's
`macos81.chd`, for one, needs Shift for its A/ROSE extension.

## Keys

| Key                      | Does                                              |
|--------------------------|---------------------------------------------------|
| F12                      | BMMacintosh's menu                                |
| Alt                      | Command                                           |
| Windows key              | Option                                            |
| Ctrl                     | Control (Mac II)                                  |
| Backspace, Delete        | Delete, forward delete (Mac II)                   |
| F11, Insert              | Help (Mac II)                                     |
| The power key            | Shut Down (the Pi 400's Fn+F10)                   |

The Mac Plus's keyboard is the Plus's, with its arrow keys and keypad (it
has no Control or Escape keys).

Gamepads move the mouse: the left stick or D-pad moves the pointer, **A**
clicks, **X** types Space and **Y** Return. **Home**, or **Select+Start**
together, opens the menu, where the D-pad moves, **A** chooses and **B**
goes back.

See the [User Guide](docs/USER_GUIDE.md) for all the details, including the
options in `cmdline.txt`. All the documents are listed in
[docs/README.md](docs/README.md).

## Building

```
./build_sdcard.sh
```

builds everything (the kernels for all the boards; `./build_sdcard.sh pi4`
builds one board's) and stages the SD card files in `build/sdcard/`. It
works on Linux and macOS. See [docs/BUILDING.md](docs/BUILDING.md).

BMMacintosh also runs on a PC, in a simulator, for testing and debugging
(with gdb or lldb), and live in a web page: see
[docs/SIMULATOR.md](docs/SIMULATOR.md).

## How it fits together

```
src/main.cpp, kernel.cpp   Circle kernel: USB, GPIO, SD card; runs the Mac on core 1
src/fbl.cpp                Display (dispmanx layer, optional CRT shader), from BMC64
src/vchiqsound*.cpp        Sound over HDMI or the headphone jack, from BMC64
src/network.cpp, webui/    Ethernet / Wi-Fi and the web UI, from BMC64
src/webui/webdav.cpp       The SD card as a network drive (WebDAV)
src/host.h                 What the kernel offers the emulator
src/port/bmmac.h           What the two Macs and their shared front end offer each other
src/port/frontend.c        The frame loop: display, pacing, sound, input, requests
src/port/keymap.c          USB keys to the Mac's key codes
src/port/menu.c, osd.c     The menu
src/port/machine.c         Switching Macs: config.txt's kernel= lines
src/port/roms.c            The ROMs on the card, MAME's zips too
src/port/ii/               Basilisk II's front end: memory, video, sound, disks, ExtFS, Ethernet
src/slirp_sockets.cpp      The Mac II network's sockets, on Circle's
src/port/plus/             umac's front end
third_party/macemu         Basilisk II (submodule, kanjitalk755's macemu)
third_party/umac           umac and Musashi (submodule)
third_party/zlib           zlib, for MAME's zips (submodule)
third_party/circle-stdlib  Circle and newlib (submodule)
src/av/, src/v3d/, src/kms Shared Pi 4/5 video, GPU and display drivers
tools/hostsim              Runs both Macs on a PC for testing
tools/mac_media.py         ROM files and disk images from MAME's
```

Basilisk II and umac are used unmodified: everything specific to
BMMacintosh is in `src/`. Their CPU cores are generated when they are
built, as in their own builds.

## Licence

BMMacintosh is free software under the GNU General Public License version 3;
see [LICENSE](LICENSE). It includes software from other projects under
their own licences (Basilisk II under the GPL version 2 or later, umac and
Musashi under the MIT licence, slirp under BSD and MIT licences, zlib
under its own); see
[docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).
