BMMacintosh - bare metal 68k Macintosh emulator for the Raspberry Pi
====================================================================

Copy everything here to the root of a FAT32 SD card and boot a Raspberry
Pi from it, with a USB keyboard and mouse connected. There are two Macs,
each a kernel of its own:

  Mac II and Quadra   Basilisk II, a 68040: System 7 to Mac OS 8.1.
                      A Pi 5, 500, 4 or 400; they start as this one.
  Mac Plus            umac, a 68000: the System software of its time.
                      Every Pi (the only one on a Pi 3, Zero 2 W, 2 or
                      Zero).

F12, Machine switches between them (the Pi restarts).

ROMs: Apple's aren't free, so none are included. Put your own in the roms
folder: ROM files, or MAME's zipped sets as they are (roms/ROMS.txt says
which). Without one the Mac says so.

Disks: put your disk images in the disks folder, then F12, Hard Disks
(Mac II) or Disk (Mac Plus), and Restart the Mac. The Mac writes to them
in place. On the Mac II, Insert a Floppy Disk and Insert a CD-ROM work
while it runs, and the shared folder is a disk on its desktop, SD Card.
Hold Shift while Mac OS starts to start it without extensions.

  F12          BMMacintosh's menu      Alt              Command
  Windows key  Option                  Ctrl             Control (Mac II)

Gamepads move the mouse: stick or D-pad to move, A to click, X Space,
Y Return, Home (or Select+Start) for the menu.

Networking: choose Ethernet or Wi-Fi under Network in the menu (or add
network=ethernet or network=wifi to cmdline.txt), then open the address
shown on screen in a browser to manage the files on this card and start
disk images.

No need to shut down: just switch it off once the disk light is off.

Licences: BMMacintosh is under the GPL version 3 (LICENSE.txt); the licenses
folder has the notices for other projects' code it includes (Basilisk II,
umac, Musashi, zlib and the rest), and the firmware folder the Raspberry Pi
and Wi-Fi firmware's licences.

Options are in cmdline.txt; see docs/USER_GUIDE.md in BMMacintosh's source
for all of them: https://github.com/jhonnaker1/bmmacintosh
