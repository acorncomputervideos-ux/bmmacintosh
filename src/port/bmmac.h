/*
 * bmmac.h - shared declarations for BMMacintosh's two emulators.
 *
 * BMMacintosh is two kernels: the Mac II and Quadra (Basilisk II, the
 * front end in src/port/ii/, the Pi 4 and 5 only) and the Mac Plus (umac,
 * src/port/plus/, every Pi). They share everything else in src/port/:
 * the frame loop's display, pacing and sound (frontend.c), the keyboard
 * (keymap.c), the menu (menu.c and osd.c), switching between the two
 * (machine.c) and finding the user's ROMs (roms.c).
 *
 * The emulator's side is the emu_*() functions below, which each front end
 * has: emu_main() runs the Mac and calls bmmac_frame() once a frame.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#ifndef BMMAC_PORT_H_
#define BMMAC_PORT_H_

#include <stddef.h>
#include <stdint.h>

#include "osd.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef BMMAC_VERSION
#define BMMAC_VERSION "dev"
#endif

/* The board the kernel is built for (Circle's RASPPI: 1 for the Pi Zero);
   the PC simulator is a Pi 5. */
#ifndef BMMAC_RASPPI
#define BMMAC_RASPPI 5
#endif

/* USB HID usage codes (keyboard page) that BMMacintosh itself uses. */
enum {
	USAGE_A = 0x04,
	USAGE_RETURN = 0x28, USAGE_ESCAPE = 0x29, USAGE_BACKSPACE = 0x2a,
	USAGE_TAB = 0x2b, USAGE_SPACE = 0x2c,
	USAGE_F1 = 0x3a, USAGE_F4 = 0x3d, USAGE_F10 = 0x43, USAGE_F11 = 0x44,
	USAGE_F12 = 0x45,
	USAGE_PAUSE = 0x48, USAGE_HOME = 0x4a, USAGE_PAGEUP = 0x4b,
	USAGE_DELETE = 0x4c, USAGE_END = 0x4d, USAGE_PAGEDOWN = 0x4e,
	USAGE_RIGHT = 0x4f, USAGE_LEFT = 0x50, USAGE_DOWN = 0x51, USAGE_UP = 0x52,
	USAGE_KP_ENTER = 0x58, USAGE_POWER = 0x66,
	USAGE_LCTRL = 0xe0, USAGE_LSHIFT = 0xe1, USAGE_LALT = 0xe2,
	USAGE_LGUI = 0xe3, USAGE_RCTRL = 0xe4, USAGE_RSHIFT = 0xe5,
	USAGE_RALT = 0xe6, USAGE_RGUI = 0xe7
};

/* F12 opens the menu (as in BMCoco, BMApple2 and BMAtariST). */
#define KEY_MENU   USAGE_F12

/* The two machines, each a kernel of its own. */
enum {
	BMMAC_PLUS = 0,    /* Mac Plus: umac */
	BMMAC_II = 1,      /* Mac II and Quadra: Basilisk II */
	BMMAC_MACHINES
};

/* ---- frontend.c ----------------------------------------------------------- */

/* The frame buffer's size (the biggest Mac screen): the Mac II's
   1600x1200, the Mac Plus's 1024x768 (its kernels run on the small Pis
   too, so it keeps the size it always had). */
#define BMMAC_FB_W (emu_machine() == BMMAC_II ? 1600 : 1024)
#define BMMAC_FB_H (emu_machine() == BMMAC_II ? 1200 : 768)

/* Starts the display and sound, then the emulator (emu_main()). */
void bmmac_main(void);
const char *bmmac_sd_root(void);
/* PATH in the card's DIR folder ("" for the root). */
void bmmac_card_path(char *path, unsigned size, const char *dir, const char *name);
/* The frame buffer shown on the display (RGB565). */
uint16_t *bmmac_framebuffer(int *pitch_px);
/* Shows the on-screen display's 640x460 area (TRUE) or the Mac's screen
   (FALSE). */
void bmmac_osd_view(int on);
/* The Mac's screen changed size: the display shows the new one. */
void bmmac_screen_changed(void);
/* Called while a menu waits: keeps sound fed and runs network requests. */
void bmmac_idle(void);
double bmmac_refresh_rate(void);
int bmmac_vsync_locked(void);
void bmmac_video_settings_changed(void);
/* The display's resolution setting changed (the Pi 5). */
void bmmac_display_mode_changed(void);
/* Reboot or power off, closing files first. */
void bmmac_request_power(int power_off);
/* The web UI's Restart: the Mac starts again. */
void bmmac_request_coldstart(void);
/* The web page's change to the disks (BMMAC_DISK_*, PATH on the card,
   "/disks/x.dsk"), done between frames. FALSE while another waits. */
int bmmac_disk_interrupt(int op, const char *card_path, int index);
/* Later news of the last one (an eject done when the Mac was next idle):
   the new result, and "done" goes up again. */
void bmmac_disk_note(const char *text);
/* What the last one did ("" before any), whether one waits, and how many
   have been done. */
const char *bmmac_disk_result(int *pending, unsigned *done);
/* A path as on the card ("/disks/x.dsk") for a full one; "" when it isn't
   on the card. */
void bmmac_card_relative(const char *full, char *out, unsigned size);
/* The last file transfer of 64 KB or more ("received x.dsk, 5.0 MB in
   12.3 s (416 KB/s), the Mac paused"), or "". */
const char *bmmac_last_transfer(void);
/* Gives the Mac the mouse's movement since the last call, smoothed (see
   frontend.c): called several times a frame by the emulators. */
void bmmac_mouse_tick(void);
/* How long the last frame waited for the display's refresh (or, without
   it, the clock), in seconds: time the emulator could have used. */
double bmmac_frame_wait(void);
/* Web UI requests waiting to be done; menus close for them. */
int bmmac_pending_requests(void);
/* The machine emulated, for the web UI and the menu. */
const char *bmmac_machine_text(void);
/* How fast the emulation runs, in percent of the real Mac's speed. */
int bmmac_speed_percent(void);
/* The disk activity light (disk_activity setting), and the emulator
   saying a disk was just read or written. */
int bmmac_disk_activity(void);
void bmmac_set_disk_activity(int on);
void bmmac_disk_accessed(void);
/* Settings in bmmac.cfg: on/off, and saving one (FALSE if it can't). */
int bmmac_setting_on(const char *name, int default_value);
int bmmac_save_setting(const char *name, const char *value);

/* The Mac can't run (no ROM, or it shut down): WHY is shown until a key
   is pressed (F12 opens the menu, where the ROM or the machine can be
   changed); then the emulator tries again. */
void bmmac_stopped(const char *why);

/* One emulated frame is done: the Mac's screen is shown (emu_screen_copy())
   and the frame paced to the Mac's 60.15 Hz (or the display's refresh),
   the keyboard, mouse and gamepads read (emu_key(), emu_mouse()), the menu
   opened if asked for, and the web UI's requests done. */
void bmmac_frame(void);

/* Sound: the emulator's output, 16-bit stereo at bmmac_sound_rate()
   (0: no sound). bmmac_sound_play() squeezes or stretches it a little to
   keep the output about half full. bmmac_sound_wanted() is how many frames
   the output is short of that. */
unsigned bmmac_sound_rate(void);
void bmmac_sound_play(const int16_t *samples, unsigned frames);
unsigned bmmac_sound_wanted(void);
void bmmac_feed_silence(void);
/* The sound volume, 0-100 (kept in bmmac.cfg). */
int bmmac_volume(void);
void bmmac_set_volume(int volume);

/* ---- keymap.c ------------------------------------------------------------- */

/* The Mac's (ADB) key code for a USB HID usage, or -1. */
int bmmac_mac_keycode(int usage);

/* ---- machine.c ------------------------------------------------------------ */

/* Can this Pi run MACHINE? (The Mac II and Quadra need a Pi 4 or 5.) */
int bmmac_machine_available(int machine);
/* The kernel file of MACHINE for this board. */
const char *bmmac_kernel_name(int machine);
/* Switches to MACHINE: rewrites the kernel= lines of config.txt's [pi4]
   and [pi5] sections for it. The Pi then has to reboot. Returns FALSE if
   config.txt can't be written. */
int bmmac_switch_machine(int machine);

/* ---- roms.c --------------------------------------------------------------- */

/* A Mac ROM found on the card: a file, or a file in a zip (MAME's sets,
   whose Mac Plus ROM is in two chips that roms.c puts together). */
struct bmmac_rom {
	char path[200];       /* "roms/macqd800.zip:f1acad13.rom" */
	char name[64];        /* "Quadra 650/800 (F1ACAD13)" */
	uint32_t checksum;    /* the ROM's own, its first four bytes */
	uint32_t size;
	int machine;          /* BMMAC_PLUS, BMMAC_II, or -1: neither can run it */
};

/* Looks for ROMs in the card's roms folder (and its root). */
int bmmac_rom_scan(void);
int bmmac_rom_count(void);
const struct bmmac_rom *bmmac_rom_get(int n);
/* The ROM PATH names (as in struct bmmac_rom), or NULL. */
const struct bmmac_rom *bmmac_rom_find(const char *path);
/* The first ROM found that MACHINE can run, or NULL. */
const struct bmmac_rom *bmmac_rom_default(int machine);
/* Reads ROM into BUF (SIZE bytes at least its size). Returns its size, or
   0 if it can't be read. */
uint32_t bmmac_rom_load(const struct bmmac_rom *rom, uint8_t *buf, uint32_t size);
/* The ROM's description for a checksum, or NULL if it isn't known. */
const char *bmmac_rom_describe(uint32_t checksum, int *machine);

/* ---- menu.c --------------------------------------------------------------- */

void bmmac_menu(void);
/* For the emulators' menus: a check box item's flags, a file's name, and
   the file browser (starting in the card's DIR, then where it was). */
unsigned bmmac_check(int on);
const char *bmmac_base_name(const char *path);
int bmmac_browse(const char *title, const char *dir, const char *const exts[],
                 char *path, size_t size);
/* Something changed that needs a reboot: the menu offers one. */
void bmmac_reboot_needed(void);

/* ---- The emulator (src/port/ii/ or src/port/plus/) ------------------------ */

/* BMMAC_PLUS or BMMAC_II. */
int emu_machine(void);
/* Runs the Mac, calling bmmac_frame() once a frame. Doesn't return. */
void emu_main(void);
/* "Macintosh Plus, 4 MB". */
const char *emu_machine_text(void);
/* What runs it, for About: "umac 0.1 (Matt Evans), Musashi 4.60 ...". */
const char *emu_credits(void);
/* The Mac's frame rate (60.15 Hz). */
double emu_frame_rate(void);
/* The Mac's screen: its size, and the display's aspect ratio for it. */
void emu_screen_view(int *w, int *h, double *aspect);
/* Copies the Mac's screen to the frame buffer, if it changed (or FORCE).
   Returns TRUE if it copied it. */
int emu_screen_copy(uint16_t *fb, int pitch_px, int force);
/* A key (the Mac's key code) pressed or released, and the mouse moved
   (DX right, DY down, in pixels; BUTTONS bit 0 the button). */
void emu_key(int mac_code, int pressed);
void emu_mouse(int dx, int dy, int buttons);
/* The most movement emu_mouse() takes in one call (counts, each way). */
int emu_mouse_step_max(void);
/* Lets go of every key (before a menu opens). */
void emu_release_keys(void);
/* The emulator's own items at the top of the menu (ids 100 and up), and
   choosing one: returns TRUE if the menu should close. */
int emu_menu_items(struct osd_item *items, int max);
int emu_menu_choose(int id);
/* Switching the Mac off and on, from the menu and the web UI. */
void emu_cold_reset(void);
/* "Start from this disk": a disk image is put in and the Mac started from
   it (the web page's Disks). */
void emu_autostart(const char *path);

/* The Mac's disks, for the web page's Disks (src/webui/webui_disks.cpp):
   paths as on the card ("/disks/System.dsk"), "" for none. */
#define BMMAC_DISK_PATH 256
#define BMMAC_OPEN_DISKS 16
#define BMMAC_HARD_DISKS 8
struct bmmac_disks {
	/* The Mac II's hard disks, from its next start: it starts from the
	   first. */
	int hard_count;
	char hard[BMMAC_HARD_DISKS][BMMAC_DISK_PATH];
	/* The Mac II's floppy drives and CD-ROM drive, now. */
	char floppy[2][BMMAC_DISK_PATH];
	char cdrom[BMMAC_DISK_PATH];
	/* The Mac Plus's disk. */
	char disk[BMMAC_DISK_PATH];
	/* Every image the Mac has open now (a hard disk taken off the list
	   stays open until the Mac restarts): not to be deleted. */
	int open_count;
	char open[BMMAC_OPEN_DISKS][BMMAC_DISK_PATH];
};
void emu_disks(struct bmmac_disks *disks);

/* The Mac II's network, for the menu ("ready: set Mac OS's TCP/IP to
   DHCP"); NULL on the Mac Plus, which has none. */
const char *emu_network_text(void);

/* The shared clipboard (the Mac II's; NULL on the Mac Plus): the text a
   Mac program copied last, or the web page set, as UTF-8; *FROM_MAC says
   which. */
const char *emu_clipboard_text(int *from_mac);
/* The web page's text for the Mac, which it gets when a program pastes.
   FALSE on the Mac Plus, or if it is too long (64 KB). */
int emu_clipboard_set(const char *utf8, unsigned length);

/* Changes to the disks the web page asks for (bmmac_disk_interrupt()). */
enum {
	BMMAC_DISK_START = 1,  /* start the Mac from PATH */
	BMMAC_DISK_INSERT,     /* the Mac II: PATH into a floppy or the CD-ROM drive */
	BMMAC_DISK_ADD,        /* the Mac II: PATH a hard disk from its next start */
	BMMAC_DISK_REMOVE,     /* the Mac II: hard disk INDEX no more */
	BMMAC_DISK_EJECT,      /* the Mac II: eject floppy drive INDEX (0, 1) or the CD (2) */
};
/* Does one, between frames: PATH is the image's full path. Writes what
   happened, for the web page, to MESSAGE. */
void emu_disk_request(int op, const char *path, int index, char *message, unsigned size);
/* The PC simulator's network test ("netprobe"): STEP 0 asks the Mac's
   network for an address (DHCP), 1 looks a name up (DNS), as the Mac
   would; the answers are logged. The Mac Plus has no network. */
void emu_net_probe(int step);
/* The disk image types the Mac takes (lower case, no dot). */
const char *const *emu_run_types(void);
/* Writes everything the Mac has changed to the card (before power off,
   and once a second). */
void emu_sync_disks(void);

#ifdef __cplusplus
}
#endif

#endif /* BMMAC_PORT_H_ */
