# BMMacintosh User Guide

BMMacintosh turns a Raspberry Pi into a 68k Macintosh: a Mac II or Quadra
(a 68040 with an FPU, System 7 to Mac OS 8.1) on a Pi 4 or 5, and a Mac
Plus on every Pi. It runs the emulators on the bare hardware, so it starts
in a couple of seconds and can be switched off at any time.

## The two Macs

| | Mac II and Quadra | Mac Plus |
|---|---|---|
| Emulator | Basilisk II (kanjitalk755's macemu) | umac (Matt Evans), with Musashi |
| Kernels | `macii8-rpi4.img`, `macii_2712.img` | `macplus.img`, `macplus7.img`, `macplus8-32.img`, `macplus8-rpi4.img`, `macplus_2712.img` |
| Pi | 4, 400, 5, 500, 500+ | every Pi |
| CPU | 68040 with an FPU (or a 68020 or 68030) | 68000, 7.83 MHz |
| Memory | 8 to 256 MB | 128K to 4 MB |
| Screen | 640x480, 800x600, 832x624, 1024x768, 1152x870, 1280x1024, and 1600x1200 on a display of 1080 lines or more; black and white to millions of colours (the Monitors control panel) | 512x342, black and white |
| ROM | a IIci's or a Quadra's (best a Quadra 650's or 800's) | the Mac Plus's (its last) |
| Disks | hard disks, two floppy drives, a CD-ROM drive, and a folder on the card (*SD Card*) | one disk, a floppy's or a hard disk's |
| Settings | `macii.prefs` (Basilisk II's own) | `bmmac.cfg` |

Each is a kernel of its own (two 68k CPU cores in one program would
clash). The Pi's firmware starts the one `config.txt` names. **Machine** in
the menu switches: it changes the `kernel=` lines of `config.txt`'s `[pi4]`
and `[pi5]` sections and restarts the Pi. A Pi 4 or 5 starts as a Mac II
the first time; a Pi Zero, 2 or 3 is always a Mac Plus. Both Macs use the
same card: the ROMs, the disks, the web UI and BMMacintosh's settings.

### Left out, for now

These were decided against for this version; they may come later:

- **Basilisk II's JIT** (the Mac II's faster CPU): it writes x86 code only.
  On the Pi's ARM it would need a new JIT of its own, a project of its
  own. The Mac II runs on Basilisk II's interpreting 68040.
- **Saved states** (freezing a Mac and starting it again where it was):
  Basilisk II has no way to save its state, and umac keeps parts of its
  own (its VIA and SCC) where a front end can't reach them.
- **A second drive on the Mac Plus**: umac has one disk drive, and a
  second means changing umac itself, which BMMacintosh uses unmodified.

Not supported:

- **The startup chime** (the Mac II): a real Mac plays it from its ROM on
  its own sound chip. Basilisk II doesn't emulate that chip (the Mac's
  sound reaches it through the Sound Manager, once Mac OS is running) and
  skips the ROM's chime, so the Mac II starts silently, as Basilisk II
  does everywhere. Apple's recording of the chime can't be shipped
  instead.

## The SD card

| File or folder   | What it is                                                  |
|------------------|-------------------------------------------------------------|
| `macii*.img`, `macplus*.img` | The kernels (above)                            |
| `bootcode.bin`, `start.elf`, `fixup.dat`, `start4.elf`, `fixup4.dat`, `armstub8-rpi4.bin`, `*.dtb`, `overlays/` | Raspberry Pi firmware |
| `config.txt`     | Raspberry Pi firmware settings: the kernel each board starts, the display mode |
| `cmdline.txt`    | BMMacintosh's options (see Options)                         |
| `bmmac.cfg`      | Settings changed in BMMacintosh's menu, and the Mac Plus's (written by BMMacintosh) |
| `macii.prefs`    | The Mac II's settings, in Basilisk II's own format: the ROM, the disks, the memory, the screen (written by BMMacintosh) |
| `macii.xpram`    | The Mac II's parameter RAM (the Control Panel's settings) |
| `macplus.pram`   | The Mac Plus's: its clock chip's parameter RAM, and how far its clock was set from the Pi's |
| `roms/`          | Your Mac ROMs (see ROMs)                                    |
| `disks/`         | For your disk images (any folder works)                     |
| `shared/`        | A folder the Mac II sees as a disk, *SD Card*               |
| `firmware/`      | Wi-Fi firmware                                              |

The Macs read and write their disk images in place, and BMMacintosh pushes
everything out to the card every second, so the Pi can be switched off at
any time (give it a second or two after saving, once the disk light is
off).

## ROMs

Apple's ROMs aren't free, so none come with BMMacintosh. Put your own in
the card's `roms` folder: ROM files, or MAME's zipped ROM sets as they are
(MAME keeps some ROMs in their chips, two or four, and BMMacintosh puts
them back together). BMMacintosh tells ROMs by their contents, not their
names. [ROMS.txt](ROMS.txt) lists which ones each Mac takes. *ROM* in the
menu chooses between the ones found; without a ROM, the Mac says what's
missing (and F12 still opens the menu, to switch Macs).

## Disks

The Mac starts from a disk with a System folder; without one it shows a
flashing "?" disk. System software isn't free either: use your own disk
images. MAME's hard disks (CHD files) need making raw first: see
[ROMS.txt](ROMS.txt).

**Mac II.** *Hard Disks* in the menu adds hard disk images (raw, or with an
Apple partition map, up to 4 GB): the Mac starts from the first. They
change when the Mac restarts (*Restart the Mac*). *Insert a Floppy Disk*
puts a floppy image (400K, 800K, 1.4 MB, raw or DiskCopy 4.2) in one of
the two floppy drives while the Mac runs, *Insert a CD-ROM* an `.iso` or
`.toast` image in the CD-ROM drive; the Mac ejects them (Command-E, or
dragging them to the Trash), or *Eject a Floppy or CD-ROM* asks Mac OS to
(as the web page's Eject does: when Mac OS is next idle, and not while a
file on the disk is open; a floppy Mac OS has no volume on, such as a
blank one it couldn't initialize, comes out at once). A hard disk is taken off under *Hard Disks*
(*Remove*), from the Mac's next start. *Shared Folder* makes the card's `shared`
folder a disk on the Mac's desktop, *SD Card*: files copied into it (from
a PC, or over the network drive) are the Mac's, and the other way round. The Mac's
files keep their type and creator, and their resource forks, in `.finf`
and `.rsrc` folders next to them; characters FAT can't have in a name are
written `%XX` on the card. Files copied to the card on a Mac running macOS
bring their type, creator and resource fork in the `._` files macOS writes
next to them, and the SD Card disk uses those. A file without a type (from
Windows or Linux, or a `._` file without one) gets one from its extension,
in any case: `.sit` StuffIt, `.sitx`, `.cpt` Compact Pro, `.hqx`, `.bin`,
`.dsk`, `.img`, `.image`, pictures, sounds and text. Copied from Windows or
Linux, a file has only its data: one with a resource fork (an
application, most System files) only travels in an archive or disk image
(`.sit`, `.hqx`, `.bin`, `.dsk`, `.img`), which the Mac then opens.

**Mac Plus.** *Disk* in the menu chooses its disk (a floppy image, raw or
DiskCopy 4.2, or a hard disk's, raw or partitioned: it starts from the
first HFS partition), and the Mac starts again from
it. A DiskCopy image's checksum is kept right as the Mac writes to it. *No Disk*
takes it out. When the Mac ejects its disk the drive stays empty until
the Mac restarts.

**Extensions.** Hold Shift while Mac OS starts to start it without its
extensions, as on a real Mac: the way past an extension that stops it.
Mac OS 8.1 from MAME's `macos81.chd`, for one, stops at its A/ROSE
extension otherwise.

## The menu

**F12** (or a gamepad's Home, or Select and Start together) opens the menu,
and the Mac stops while it's open. The arrow keys (or the D-pad) move,
**Enter** (or A) chooses, **Esc** (or B) goes back, and F12 again closes it.

| Item          | What it has |
|---------------|-------------|
| Insert a Floppy Disk, Insert a CD-ROM | Mac II: a disk into an empty drive |
| Hard Disks    | Mac II: adding and removing hard disk images |
| Shared Folder | Mac II: the card's `shared` folder as a disk |
| Disk, No Disk | Mac Plus: its disk |
| ROM           | The ROMs found that this Mac takes |
| Memory        | The Mac's memory (it restarts) |
| Restart the Mac | Switching the Mac off and on |
| Machine       | The Mac Plus or the Mac II (the Pi restarts) |
| Video         | Screen size, frame lock, CRT effect, smooth scaling, disk activity |
| Sound         | Volume, HDMI or the headphone jack |
| Mouse Speed   | The USB mouse's speed on the Mac, from a quarter to twice |
| Network       | Ethernet or Wi-Fi, the network drive and web page |
| About         | Versions, the machine, the display |

The menu's last line shows how often frames are shown, as a percentage of
the Mac's 60.15 a second. The Mac II runs as many instructions in a frame
as the Pi manages; the Mac Plus runs at about a real Plus's speed, and
under 100% the Pi can't keep up.

## Keyboard and mouse

Keys go to the Mac by what they are. The keys around the space bar are
where a Mac's are:

| PC key                   | Mac key         |
|--------------------------|-----------------|
| Alt (either)             | Command         |
| Windows key              | Option          |
| Ctrl                     | Control (Mac II)|
| Backspace                | Delete          |
| Delete                   | Forward delete (Mac II) |
| F11, Insert              | Help (Mac II)   |
| Num Lock                 | Clear           |
| Print Screen, Scroll Lock, Pause | F13, F14, F15 (Mac II) |

Caps Lock locks, as the Mac's does. F12 is BMMacintosh's menu. The power
key (a Pi 400's Fn+F10) shuts down.

The Mac Plus's keyboard is the Plus's: the main keys, the arrow keys and
the keypad (its + * / = come with Shift, as on the Plus's own keyboard),
no Control or Escape. Keys go to it one at a time, every other frame, from
a queue.

A USB mouse is the Mac's mouse (the left button; the Mac has one). A
gamepad moves the mouse too: the left stick or D-pad moves the pointer,
A clicks, X types Space and Y Return.

The pointer moves smoothly: the mouse's reports (125 a second for most
mice, which doesn't divide into the Mac's 60.15 frames) are evened out, at
the cost of about one report's time (8 ms), and the Mac II gets the
movement four times a frame. A modern mouse counts eight or more times as
many steps an inch as the Macs' did: if the pointer is too quick, choose a
slower **Mouse Speed** in the menu (or in Mac OS's Mouse control panel).
The Mac Plus moves its pointer at most 200 steps a second, as umac has
it: a quick sweep is cut short there, and a slower Mouse Speed suits it.

## Display and timing

The Mac shows 60.15 frames a second. When the display's refresh rate is
within 2% of that, each frame is shown on exactly one display refresh;
otherwise the emulation keeps exact time. `config.txt` sets the display
to 720p at 60 Hz, and on a Pi 4 or 5 BMMacintosh sets it to the Mac's
exact rate itself (see [PI4.md](PI4.md)). `vsync=on` or `vsync=off` in
`cmdline.txt` (or *Frame Lock* in the menu) forces one or the other.

A small red light at the bottom right of the Mac's screen shows the disk
in use (*Video*, *Show Disk Activity*).

## Networking, the network drive and the web UI

BMMacintosh can join your network over Ethernet or Wi-Fi. The SD card then
opens as a **network drive** in your computer's own file manager (WebDAV),
so disk images, ROMs and files can be copied to and from the Pi without
taking the card out, and a small web page has the Pi's settings and
controls.

Turn it on in the menu's *Network*. For Ethernet, set *Network* to Ethernet
and reboot. For Wi-Fi, choose *Scan for Wi-Fi Networks*: the first time,
it offers to switch Wi-Fi on and reboot; after that it lists the networks
it can hear, strongest first. Pick yours: it fills in *Wi-Fi Network*, and
if the network needs a password, *Wi-Fi Password* is next. Then reboot.
Set *Wi-Fi Country* to your two letter country code too; it decides which
channels are allowed. A hidden network can be typed in under *Wi-Fi
Network* instead. Or, in `cmdline.txt` (on the same single line as the
other options):

- Ethernet: `network=ethernet`
- Wi-Fi: `network=wifi`, and copy `wpa_supplicant.conf.example` to
  `wpa_supplicant.conf` in the root of the card with your network's name,
  password and country.

Wi-Fi needs the files in the `firmware` folder, which `build_sdcard.sh` puts
there.

Once the Pi has an address, it is shown at the bottom of the screen for a
few seconds (`WEB UI: HTTP://192.168.1.50/`) and written to the log, and
the menu's *Network* shows it with the network drive's address, and how
the Mac II's own network is (waiting for the Pi's network, ready for Mac
OS's TCP/IP, or in use).
Open that address in a browser: the page shows the network drive's address
(the same, with `/sd/` after it) and how to connect.

### The network drive

The card is at `http://<the Pi's address>/sd/`:

- **Windows**: in File Explorer, right-click **This PC**, **Map network
  drive…**, and enter `http://192.168.1.50/sd/` as the folder. Windows
  only sends a password over HTTPS, so with a `webui_pin` set it won't
  connect unless Windows is told to allow it (the `BasicAuthLevel` setting
  of the WebClient service, `2`) — or leave the PIN off on a home network.
- **macOS**: in the Finder, **Go ▸ Connect to Server…** (⌘K), enter
  `http://192.168.1.50/sd/` and connect as a guest (or with any name and
  the PIN). Files copied from the Mac keep their type, creator and
  resource fork on the Mac II's *SD Card* disk: macOS writes them in `._`
  files next to them, which BMMacintosh reads.
- **Linux**: in Files, **Other Locations**, **Connect to Server**,
  `dav://192.168.1.50/sd/`; or rclone, davfs2, cadaver or any other
  WebDAV client with `http://192.168.1.50/sd/`.

Files can be copied, renamed, moved and deleted, and folders made, as in
any folder. When a file on the *SD Card* disk is renamed, moved or
deleted, its Mac type, creator and resource fork (in `.finf` and `.rsrc`)
go with it. A copy that breaks off leaves the file as it was.

The boot configuration (`cmdline.txt`, `config.txt`,
`wpa_supplicant.conf`, edited on the web page instead) and the `firmware`
folder can be read but not changed. Everything else, including the
kernels, can be replaced, so a new build of BMMacintosh can be copied over
and started with **Reboot the Pi**.

Things to know:

- Don't replace or change a disk image the Mac is using: eject it, or
  choose another in the menu, first.
- Names on the card are in FAT's code page (CP850): a name with a
  character it hasn't (the euro sign, Greek, Chinese, emoji) is refused.
- While a file of 64 KB or more is copied to or from the card, the Mac
  pauses and the Pi's screen shows the transfer: the file, how far it has
  got, the speed and the time left. The network then has the Pi to itself;
  sharing it with the Mac, Wi-Fi above all was very slow. The Mac carries
  on 2 seconds after the last file, so a folder of files is one pause.
  **Esc** lets the Mac run meanwhile (the transfer is slower then), and
  *Pause the Mac During Transfers* in the menu's Network turns the pausing
  off. Each transfer's size, time and speed are on the web page (System,
  *Last transfer*), in the menu's Network, and in the log (`bmmac.log`).
- The Pi answers one request at a time: a big copy slows the others.
- Locks are granted but not enforced, and other properties (Windows's
  file times) aren't kept: the card is meant for one computer at a time.

### The web page

- **The SD card on your computer**: the network drive's address, how to
  connect from Windows, macOS and Linux, and the card's free space.
- **The Mac**: **Restart the Mac** (as switching it off and on),
  **Reboot the Pi**, **Shut down** (the Pi halts; switch it off, then on
  again to restart; a Pi 400 switches itself off, and its power key starts
  it again).
- **Copy files to the Pi**: drop files or folders anywhere on the page
  (or choose them) and they are copied to the card, each with its
  progress: disk images to the `disks` folder, everything else to
  `shared` (the Mac II's *SD Card* disk), or where *Put them in* says.
  Folders keep what is in them; macOS's `.DS_Store` and `._` files are
  left out, and a file already there is replaced only when you say so. A
  browser hands over a file's contents only, so Mac files with a resource
  fork (applications, many games) should come as an archive (`.sit`,
  `.hqx`, `.bin`) or a disk image; it needs no network drive set up.
- **Update BMMacintosh** (under System): drop a new release's zip on the
  box (or choose it) and the Pi installs it on its card, then offers to
  reboot into it. The kernels, the Pi's boot files, overlays, Wi-Fi
  firmware and licences are replaced (the old kernels kept as `.old`, to
  go back to by renaming them); your `disks`, `shared` and `roms` folders,
  `wpa_supplicant.conf`, `bmmac.cfg` and `macii.prefs` are left as they
  are. `config.txt` and `cmdline.txt` are kept too: when the release's
  differ, they are put next to yours as `config.txt.new` and
  `cmdline.txt.new`. A zip that isn't a BMMacintosh release, or doesn't
  unpack whole (a broken download), changes nothing.
- **Disks**: the Mac's disks now, and the disk images in the card's
  `disks` folder (and the folders in it). On the Mac II, **Insert** puts
  a floppy or CD-ROM image in a drive while the Mac runs, **Eject** asks
  Mac OS to put one away (as dragging it to the Trash does: when Mac OS is
  next idle, and not while a file on it is open; a blank floppy Mac OS
  couldn't initialize comes out at once), **Add** makes a hard
  disk image one of its hard disks
  and **Remove** takes one off (from the Mac's next start), and **Start
  from** restarts the Mac from any image. **Delete** deletes an image the
  Mac isn't using from the card (one in a drive, on the hard disk list or
  still open since the Mac started can't be). On the Mac Plus, **Start from**
  changes its one disk and restarts it. **New disk image** (the Mac II)
  makes an empty floppy (800K or 1.4 MB) or hard disk (20 MB to 2 GB) in
  `disks`, and puts it in the Mac (a floppy now, a hard disk from its next
  start); Mac OS then asks to initialize it. Making one takes a moment,
  up to a few seconds for 2 GB, and the Mac pauses meanwhile.
- **Clipboard** (the Mac II): text a Mac program copies shows here, to
  copy to your computer; text sent from here is what the Mac pastes next
  (Command-V). Text only, up to 64 KB, in the Mac's character set
  (MacRoman): characters it hasn't become "?".
- **Settings**: `config.txt`, `cmdline.txt`, `bmmac.cfg`, `macii.prefs` and
  `wpa_supplicant.conf`, edited in the browser; changes take effect after
  a reboot, and the previous version is kept as `.bak`. A mistake in
  `cmdline.txt` or `config.txt` can stop the Pi starting, and then needs
  the card in a computer (the `.bak` copy makes that easy).
- **System**: the Pi's model, address, uptime, temperature, power and
  throttling warnings, and a way to turn the web page and network drive
  off until the next reboot.

The web page and the network drive are open to anyone on your network. To
protect them with a PIN, set `webui_pin=1234` in `cmdline.txt`; the
browser and the file manager then ask for a user name (anything) and the
PIN. To network without either, set `webui=0`.

### The Mac II on the network

With the Pi on the network, the Mac II is too: its Ethernet card is
connected to a small network of its own inside BMMacintosh (slirp, as in
Basilisk II and QEMU), which reaches the internet through the Pi's
connection, Ethernet or Wi-Fi. In Mac OS, open the *TCP/IP* control panel,
connect via *Ethernet*, configure *Using DHCP Server*, and close it to
save: the Mac gets the address 10.0.2.15, with 10.0.2.2 its router and
10.0.2.3 its DNS server (which asks the Pi's). Open Transport loads TCP/IP
only when a program asks for it; to have it at once, *Edit*, *User Mode*,
*Advanced*, then *Options* and untick *Load only when needed*.

The Mac's programs can then connect out (web browsers, FTP, mail, news):
their connections leave from the Pi's address. Nothing on your network
can connect to the Mac, there's no AppleTalk (so no file sharing or
printing to other Macs), and ping doesn't get past the Pi. Old browsers
can't open most of today's web sites, which need newer encryption than
they have.

The Mac starts with the network up (the log says "the Mac's network is
up" once the Pi has its address). To leave it off, add `nonet true` to
`macii.prefs`. The Mac Plus has no network.

The network also sets the Pi's clock, so files saved on the card get the
right date and time. Set `timezone` to your offset from UTC in minutes, for
example `timezone=600` for UTC+10. Both Macs' clocks are the Pi's, in that
time zone; setting the Mac's clock (the Control Panel, the Alarm Clock)
moves it from there, and the Mac Plus keeps that in `macplus.pram`.

## Options

`cmdline.txt` is a single line of `name=value` options separated by spaces.
BMMacintosh's menu saves the options it changes to `bmmac.cfg`, one
`name=value` per line, and those win over `cmdline.txt`. Either file can be
edited from the web UI. The Mac II's own settings are in `macii.prefs`,
which the menu changes.

| Option              | Values                       | Default  | What it does |
|---------------------|------------------------------|----------|--------------|
| `aspect`            | number                       | `1.0`    | Multiplies the width of the picture |
| `disk_activity`     | `0`, `1`                     | `1`      | The disk light (Video, Show Disk Activity) |
| `scale`             | 0.1 to 1.0                   | `1.0`    | Height of the picture as a fraction of the screen (Video, Screen Size; less for a TV that cuts off the edges) |
| `smooth`            | `0`, `1`                     | `0`      | Bilinear filtering instead of sharp pixels |
| `crt_shader`        | `0`, `1`                     | `0`      | CRT look (scanlines) from BMC64 |
| `crt_curvature`     | `0`, `1`                     | `0`      | With `crt_shader`, a curved screen |
| `crt_gpu`           | `0`, `1`                     | `1`      | Pi 4 and 5: the 3D GPU draws the smooth and CRT pictures; `0` has the ARM draw them (see [PI4.md](PI4.md)) |
| `vsync`             | `auto`, `on`, `off`          | `auto`   | Lock frames to the display (see Display and timing) |
| `resolution`        | `auto`, `1920x1080`, `1280x720` | `auto` | Pi 5 only, with `video_driver=kms`: the display's resolution; `auto` keeps the one the firmware chose (the Pi 5's firmware ignores `hdmi_mode`) |
| `video_driver`      | `kms`, `hvs`, `firmware`     | `kms`    | Pi 4 and 5: `kms` runs the display at the Mac's exact frame rate; `hvs` leaves the mode set in `config.txt`; `firmware` uses the firmware's framebuffer, if the others give trouble (see [PI4.md](PI4.md) and [PI5.md](PI5.md)) |
| `sound`             | `0`, `1`                     | `1`      | Sound on or off |
| `volume`            | 0 to 100                     | `100`    | Sound volume |
| `audio_out`         | `auto`, `hdmi`, `headphones` | `auto`   | Where the sound goes (`auto`: HDMI, or the headphone jack if the display takes no sound) |
| `audio_buffer_ms`   | 20 to 500                    | `60`     | Sound buffer; raise it if the sound breaks up |
| `plus_rom`, `plus_disk`, `plus_ram` | a path; KB (128 to 4096) | | The Mac Plus's ROM, disk and memory (the menu sets them) |
| `ii_max_instructions` | number                     | `2000000` | The most instructions the Mac II runs in a frame |
| `enable_logging`    | `off`, `uart`, `file`        | `off`    | Where the log goes (see Logging) |
| `netprobe`          | `0`, `1`                     | `0`      | Mac II: tests its network once the Pi's is up (an address, then a name looked up) and logs the answers |
| `network`           | `off`, `ethernet`, `wifi`    | `off`    | Networking (see Networking, the network drive and the web UI) |
| `webui`             | `0`, `1`                     | `1`      | Web page and network drive when networking is on |
| `webui_pin`         | digits                       | none     | PIN for the web page and network drive |
| `timezone`          | minutes east of UTC          | `0`      | Time zone for the network clock |
| `usbpowerdelay`     | milliseconds                 | Circle's | Circle: wait for USB devices to power up (by default 300 ms, 510 ms behind a hub) |

## Logging

BMMacintosh logs what it is doing, which helps when something doesn't work:
the version, the display and sound set up, the ROMs found and the one
used, the disk images opened, devices coming and going, network status,
and the emulators' own messages. Choose
where it goes with `enable_logging` in `cmdline.txt`:

- **Off** (the default, also when `enable_logging` isn't set): no log.
- **UART**: the serial port, GPIO14 TX and GPIO15 RX at 115200 baud
  (all boards, the Pi 5 too). Needs a USB serial adapter.
- **File**: the serial port, and `bmmac.log` in the root of the SD
  card. The file starts with what was logged since boot, is written to the
  card every few seconds (and is complete after a crash, a hang or
  switching off), and starts again at boot once it is over 1 MB. Copy it
  from the network drive to send with a bug report. Writing it
  costs a little time on each frame, so switch it off once you have what
  you need.

## Troubleshooting

**"No Mac II or Quadra ROM" or "No Mac Plus ROM"**: put a ROM that Mac
takes in the card's `roms` folder ([ROMS.txt](ROMS.txt)), or switch Macs
(F12, Machine).

**The flashing "?" disk**: the Mac has no disk it can start from. Choose
one in the menu (Hard Disks or Disk) and *Restart the Mac*. A Mac II hard
disk from MAME needs making raw first ([ROMS.txt](ROMS.txt)); the Mac Plus
starts from its first HFS partition.

**"Sorry, a system error occurred" while Mac OS starts**: often an
extension: hold Shift while it starts.

**No picture**: uncomment `hdmi_safe=1` in `config.txt`. If that works, your
display doesn't accept 720p at 60 Hz; try another `hdmi_mode`.

**No sound over HDMI**: try `hdmi_drive=2` in `config.txt`, or
`audio_out=hdmi` in `cmdline.txt`.

**Sound breaks up**: raise `audio_buffer_ms` in `cmdline.txt`, e.g. to 100.

**The TV cuts off the top and bottom of the picture or the menu**: many TVs
enlarge the picture and crop its edges (overscan). Make *Video*, *Screen
Size* smaller (90% usually does), or turn overscan off on the TV ("Just
Scan", "Screen Fit" or "1:1").

**Keyboard, mouse or gamepad not working**: unplug it and plug it in again;
devices can come and go while the emulator runs. Some keyboards need a
longer `usbpowerdelay` (add `usbpowerdelay=1000` to `cmdline.txt`).

**8BitDo M30 2.4G buttons are wrong**: its receiver can be left in Switch
mode, where it acts as a Switch Pro controller. With the pad connected,
hold Home (the heart) and D-pad Down for 5 seconds until the LEDs flash; it
goes back to its own mode and keeps it.

