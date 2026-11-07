# Changelog

## Unreleased

- *Eject a Floppy or CD-ROM* in the Mac II's menu (F12), as the web page's
  Eject does, and an eject's outcome shown on the Pi's screen as well
- StuffIt archives (and other files) copied to the SD Card disk open in
  StuffIt Expander again. Modern macOS's `._` files mostly have no type or
  creator in them (only a download's quarantine and the like), and taking
  those made such files typeless where their extension (`.sit`: SIT!)
  would have said what they are: Expander didn't know them. Empty ones are
  passed by now, and a file whose Finder info has neither is given its
  extension's, which mends files already copied. Extensions are matched in
  any case (`GAME.SIT`), and `.sitx`, `.cpt` (Compact Pro) and `.image`
  (Disk Copy 6) are known. The uploads themselves were checked byte for
  byte (80 of them, chunked and not, split at awkward places)
- Uploads to the network drive (and the web page) gave the processor away
  after every packet read (1.4 KB): with data waiting, that let the Mac run
  between each, and held uploads to tens of KB a second. They hand over
  after each 32 KB written to the card now; reading waits anyway when
  nothing has come
- The transfer window comes for any transfer of 64 KB or more, and at
  once for an upload of unknown size (macOS's), where it waited for 256 KB
  (half a minute at the old speed)
- Each transfer of 64 KB or more is timed: its size, time and speed, and
  whether the Mac was paused, are on the web page (System, Last transfer),
  in the menu's Network, and in the log
- Eject on the web page's Disks (the Mac II's floppy drives and CD-ROM
  drive). Mac OS is asked, as dragging the disk to the Trash does
  (UnmountVol, then Eject), when it is next idle: Basilisk II's
  SynchIdleTime() patch (idlewait, on now; BMMacintosh's idle_wait()
  doesn't sleep) runs it in a program's place, where the File and Memory
  Managers are free. A disk with a file open stays in ("in use"), and if
  Mac OS isn't idle within 5 seconds the page says to eject it on the Mac
- The Mac pauses during big file transfers (the network drive's and the web
  page's, 512 KB or more): a window on the Pi's screen shows the file, the
  progress, the speed and the time left, and the network has the Pi's core
  to itself, where it shared it with the Mac (a 5 MB file over Wi-Fi took
  minutes). The Mac carries on 2 seconds after the last file; Esc lets it
  run meanwhile; *Pause the Mac During Transfers* in the Network menu turns
  it off. Each pause's size, time and speed go in the log
  (`src/port/transfer.c`; the PC simulator's `xfer` command and three
  tests)
- Faster transfers over Wi-Fi (the network drive and the web page). Circle's
  Wi-Fi driver is tasks of its own, which hand over at every lock, wait
  and packet, and while the web UI answered a request they got one turn at
  a time between the parts of a frame, plus the frame's idle time: a
  packet took several turns. They get a time slice of up to 1.5 ms there
  now (four a frame on the Mac II, every 5 ms step on the Mac Plus): about
  half of each frame goes to the network while a transfer runs, and the
  Mac runs slower meanwhile. Not measured on a Pi
- The web page's Disks offers Start from only for disks a Mac can start
  from (with boot blocks: a System on them), and the server refuses the
  rest with a message. An empty floppy was offered, and the Mac, as a real
  one does, passed it by and started from its hard disk. Start from a
  CD-ROM image now sets the Mac II's startup device to the CD-ROM driver
  (Basilisk II's bootdriver), without which it never started from one;
  starting from anything else sets it back to any
- Floppy disks can be initialized on the Mac II: Mac OS asks the floppy
  driver to format the disk first, and BMMacintosh's `SysFormat()` always
  said it failed ("Initialization failed!"), so a new blank floppy from
  the web page couldn't be used. It succeeds now for an image in the drive
  that can be written, as in Basilisk II's own Unix version (hard disks
  never asked it)
- The web page looks like a desktop of the early 90s: a grey patterned
  desktop, a menu bar (its menus go to the windows; the Pi's status at the
  right), black-outlined windows with striped title bars and close boxes,
  round-cornered buttons, radio buttons, pop-up menus, and the settings
  editor as a dialog with a double border. No Apple logo, and none of
  Apple's fonts shipped: Chicago, Charcoal and Geneva where the computer
  has them, a plain sans-serif otherwise

## 0.1.5 – 2026-11-07

Built for every board and tested on the PC (the simulator and the tests
below), but not tried on a Raspberry Pi yet.

- A smooth mouse pointer. A USB mouse reports 125 times a second (most of
  them) and the Mac's frames are 60.15: a frame had two reports' movement,
  the next three, and the pointer stuttered. Each report is kept with its
  time now, and the mouse read between them as it was a report's interval
  earlier (`src/port/mouse_track.c`, tested by `tools/mouse`: a frame's
  step was up to a whole report, 5 counts, from the hand's; one now). On
  the Mac II the pointer also jumped back on quick movements: an ADB mouse
  packet holds -64 to 63 counts, and more wrapped round. It gets the
  movement four times a frame, 63 counts at most each time, with the rest
  carried over. umac's Mac Plus is given its one count a 5 ms step, rather
  than up to a frame's worth that it partly dropped
- Mouse Speed in the menu: a quarter to twice the USB mouse's movement
- The network drive called the card's top folder `/sd//` rather than
  `/sd/`, which a file manager could take for a folder of its own inside
  it. Opened in a browser, a folder is a readable page now, with sizes,
  without the Mac's helper files, and a link to the web page
- New blank disk images from the web page's Disks (the Mac II): an empty
  800K or 1.4 MB floppy or a 20 MB to 2 GB hard disk in `disks`, put in
  the Mac if wanted, for Mac OS to initialize. Only its start and end are
  written; the rest is grown without writing, so a big one takes seconds,
  not minutes
- Disks on the web page: the Mac's disks, and the disk images in the
  card's `disks` folder, with what each can do. On the Mac II, insert a
  floppy or CD-ROM image while the Mac runs, add or remove hard disks, or
  start from any image; on the Mac Plus, start from one. The Mac makes the
  change between its frames, and the page shows what happened
  (`src/webui/webui_disks.cpp`, `bmmac_disk_interrupt()`; tested by
  `tools/disks` and `tools/hostsim`)
- A shared clipboard for the Mac II: text a Mac program copies shows on
  the web page, and text sent from the page is what the Mac pastes next.
  Basilisk II's clipboard driver is BMMacintosh's own now
  (`src/port/ii/clip_ii.cpp`, from its Unix one), with MacRoman and UTF-8
  converted both ways (`src/port/clip_text.c`, tested against Python's
  mac_roman codec by `tools/clipboard`). Not tried with Mac OS yet
- The menu's *Network* shows the web page's and the network drive's
  addresses, and how the Mac II's network is (waiting for the Pi's
  network, ready for Mac OS's TCP/IP, or in use)
- The SD card is a network drive: Windows (Map network drive), macOS
  (Connect to Server) and Linux file managers open
  `http://<the Pi>/sd/` as a folder of their own (WebDAV,
  `src/webui/webdav.cpp`), to copy, rename, move and delete files and make
  folders. Uploads with a length or chunked (macOS), after
  "100 Continue" (Windows), through a temporary file; a file's Mac type,
  creator and resource fork go with it; the boot configuration and
  `firmware` stay protected. `tools/webdav` tests it on the PC with curl,
  rclone and litmus (the WebDAV test suite: all of its basic, copymove and
  http tests but one name in a character FAT's code page hasn't)
- The web page is one simple page instead of the file manager: the network
  drive's address and how to connect, restarting the Mac and rebooting or
  shutting down the Pi, the settings files' editor, and the Pi's status.
  Light or dark as the computer is. Disk images are started from the
  menu (F12) now
- The Mac II's SD Card disk (the shared folder) no longer spoils files the
  Mac writes on a Pi. When a program saved over a file with less than it
  held, or the Mac moved past a file's end, FatFs (through circle-newlib)
  made the file longer, ending in whatever the card held, and it wouldn't
  open again. The file positions now follow POSIX's rules, as on a PC,
  where the PC simulator's tests ran (`src/port/ii/extfs_posix.cpp`,
  tested against stand-ins for FatFs's calls by `tools/extfs_posix`)
- Files copied to the card from a Mac running macOS keep their type,
  creator and resource fork on the SD Card disk: they come from the
  AppleDouble files (`._name`) macOS writes next to them, which were left
  out, so applications and many documents came without them and wouldn't
  open
- Faster uploads and downloads in the web UI. The network and the web UI
  run between the Mac's frames, and had one turn a frame: while the web
  UI answers a request they now also run between the parts of a frame
  (four times as often on the Mac II). Uploads are written to the card
  32 KB at a time instead of a network packet (1460 bytes) at a time, and
  the Pi's TCP receive window is 32 packets instead of 10
  (`src/patches/circle_tcp_window_patch.diff`), which also helps the Mac
  II's own downloads. Not measured on a Pi yet
- The Mac II's CPU core is compiled without a frame pointer: each 68k
  instruction's function no longer saves and restores one (3.4% less of
  the core's code on the Pi 5, cortex-a76). Not measured on a Pi yet

## 0.1.4 – 2026-11-03

- The Mac II reaches the Internet from a Pi (issue #5): it had an address
  but no name could be looked up and no connection made. On the Pi slirp's
  sockets (Circle's) were asked not to wait with newlib's MSG_DONTWAIT
  (1 << 20), not Circle's (0x40), and Circle refused every datagram sent.
  Tested on a Pi 5: DNS, and Internet Explorer 4 loading web pages. UDP
  sockets are bound when made, an error reading one no longer turns into
  an ICMP "unreachable" for the Mac, and the log names slirp's DNS server
  and the first datagrams and connections, with Circle's result
- `netprobe=1` in `cmdline.txt`: once the Pi's network is up, the Mac II's
  network is asked for an address and a name as the Mac would, and the
  answers logged, a test of the network without a program on the Mac.
  The PC simulator's `netprobe dhcp|dns` does the same
- The Mac II's network (slirp) starts with the Mac, not once the Pi's own
  network is up: the Mac asks for its address as Mac OS starts, and the
  Pi's Wi-Fi can take longer than it waits (19 s on a Pi 5). Its DNS
  server comes when the Pi's network does. slirp also sets its own
  10.0.2.x addresses when there is no DNS server yet, which it skipped

## 0.1.3 – 2026-10-30

- The Mac II's menu puts floppy images into an empty floppy drive again,
  instead of saying both drives are full (issue #4): an empty floppy
  drive's name is "", as the empty CD-ROM drive's is, and the floppy
  drives were taken for CD-ROM drives

## 0.1.2 – 2026-10-30

- A picture wider than the display (a 4:3 or 3:2 Mac on a 5:4 monitor)
  keeps its shape: as wide as the display, with bars above and below,
  where its width was cut and it came out squashed (issue #3). In sharp
  mode it is drawn at fewer whole multiples of its lines to fit. On the
  Pi 3, 2 and Zero a smaller Screen Size now narrows the picture as well
  as shortening it. All three display paths use one layout
  (`src/av/layout.h`), tested on the PC for every Mac screen on 16:9,
  16:10 and 5:4 displays (`tools/pi4scaler`)
- `tools/qemu/run_qemu.sh` starts a Pi 4 kernel in QEMU and prints its
  log, for crashes before the log reaches the SD card (from BMAmiga)
- The Mac II's screen can also be 1600x1200 (Apple's 20-inch ColorSync
  Display's largest size), offered when the display has 800 lines or more
  (1080p, 1920x1200): a screen taller than the display is shrunk to it,
  and the Pi's display hardware shrinks to two thirds at most

## 0.1.1 – 2026-10-28

- The web UI saves `config.txt` and the other settings files, and shuts
  down and restarts the Pi, again: the Pi checked for a different header
  name from the one its pages send (issue #2). Uploading over a file the
  Pi protects (`config.txt`, `cmdline.txt`) now says to edit it instead
- The Mac II's screen can also be 832x624, 1152x870 and 1280x1024
  (Apple's monitors' sizes); on the Pi 4, a screen taller than the display
  is shrunk to it in sharp mode rather than drawn past its end
- The Mac II's speed in the menu says it keeps up (the share of the Mac's
  frames shown) and that its CPU runs at full speed, which is often
  several times a real Quadra's; it isn't timed to one
- The User Guide says why there is no startup chime, and that screens
  bigger than 1280x1024 are left out for now

## 0.1.0 – 2026-10-28

The first BMMacintosh: BMAtariST 0.1.0 (itself from BMApple2, BMCoco,
BMA800XL and BMC64), with Hatari replaced by two 68k Macs, each a kernel
of its own.

- The Mac II and Quadra (`macii*.img`, the Pi 4 and 5): Basilisk II from
  kanjitalk755's macemu, with its ARM CPU core (uae_cpu_2021: a 68040 with
  an FPU), direct addressing, 8 to 256 MB, screens of 640x480 to 1024x768
  in black and white to millions of colours, sound, and its tick, sound and
  disks driven from the frame loop (no threads, VOSF or JIT).
  Where the Mac reads or writes memory it doesn't have, it reads 0 and its
  writes are dropped, as Basilisk II on Unix skips them
- The Mac II's network: Basilisk II's slirp, in the background, through
  the Pi's Ethernet or Wi-Fi (DHCP gives the Mac 10.0.2.15; its TCP and
  UDP go out from the Pi's address), on Circle's own sockets on the Pi,
  which connect without holding up the Mac
- Hard disk images (raw or with an Apple partition map, up to 4 GB: read
  and written through FatFs itself, past newlib's 2 GB seek), two floppy drives
  and a CD-ROM drive that take images while the Mac runs, and the card's
  `shared` folder as a disk on the Mac's desktop (ExtFS, on FAT)
- The Mac Plus (`macplus*.img`, every Pi): umac by Matt Evans with Musashi,
  the Mac Plus's ROM with 128K to 4 MB, its disk read and written in place
  through a disk driver of BMMacintosh's own (Basilisk II's, GPL version 2
  or later, in place of umac's, marked "GPLv2"), its sound buffer played;
  its keyboard with the arrow keys and keypad (the Instant command umac
  doesn't answer, answered in front of it), and a locking Caps Lock; its
  clock chip, with the Pi's time and its parameter RAM kept on the card
  (`macplus.pram`); DiskCopy 4.2 images (their checksum kept right), and
  hard disks with an Apple partition map (their first HFS partition); its
  sound at the volume and from the buffer the Mac chooses, and off when
  the Mac switches it off
- Switching Macs from the menu (Machine), which rewrites `config.txt`'s
  `kernel=` lines and restarts the Pi, as BMC64 switched machines
- The user's own ROMs, as files or as MAME's zipped sets (whose chips are
  put back together); `tools/mac_media.py` makes ROM files and raw disk
  images from MAME's
- Keys pressed while the Mac starts reach it (Shift for no extensions)
- A Mac-style menu (F12) with the Linux console's font; the keyboard as a
  Mac's (Alt Command, the Windows key Option); gamepads as the mouse
- The PC simulator's tests boot both Macs' ROMs, and with the user's disks
  Mac OS 8.1 and System 4.3 to the Finder
- Left out for now (see the User Guide's *Left out, for now*): Basilisk
  II's JIT (x86 only), saved states, and a second drive on the Mac Plus

Tested in the PC simulator; all seven kernels build. On a Raspberry Pi 5
the Mac II starts Mac OS 8.1; the Mac Plus, the Mac II's network and the
other Pis haven't been tried on a Pi yet.

BMAtariST's own history is in its repository:
https://github.com/jhonnaker1/bmatarist/blob/main/CHANGELOG.md
