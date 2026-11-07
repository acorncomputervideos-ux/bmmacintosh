# BMMacintosh Patches

## Circle

The following patches are applied by `make_all.sh`, in this order, while
preparing the Circle dependency. They carry changes that are not part of the
checked-out upstream revision. `make_all.sh --skip-patches` (and
`build_sdcard.sh --skip-patches`) leave Circle as it is.

Patches are applied to the submodule revisions pinned by circle-stdlib v21
(Circle 51.1.1, tag `Step51.1.1`), the same revisions BMC64 uses.

| Patch | Description |
| --- | --- |
| `circle_bolt_keyboard_patch.diff` | Adds Logitech Bolt keyboard receiver support (from BMC64). |
| `circle_linux_bug_patch.diff` | Turns off `BUG_ON`. Circle's Linux emulation, which VCHIQ uses for the display and sound, asserts with `BUG_ON` that it only runs on core 0; BMMacintosh calls it from core 1, where the emulator runs (as BMC64 does with VICE). Without it the Pi halts with `mutex.cpp(9): assertion failed`. From BMC64's `circle_patch.diff`. |
| `circle_sysconfig_patch.diff` | Enables multi-core support and the USB FIQ in `sysconfig.h`. From BMC64's `circle_patch.diff`. Also saves the floating point registers on interrupts on the Pi Zero, as Circle already does on the Pi 2 and later with GCC 12 or newer; on its one core, interrupts would otherwise clobber the emulator's (BMC64's Pi Zero build does the same). |

| `circle_tcp_window_patch.diff` | Raises the TCP receive window from 10 segments (14.6 KB) to 32 (46.7 KB). The Pi only takes in what the network brought when its tasks run (see `src/kernel.cpp`), and a sender may only have one window on the way at a time: uploads to the web UI, and the Mac II's downloads through slirp, were held to one window per turn. 32 segments stay inside the Pi 5's Ethernet receive ring (64 frames) and the Pi 4's (256). |

KEGS is not patched; see `docs/UPDATING.md`.
