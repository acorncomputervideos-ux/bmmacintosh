/*
 * menu_ii.cpp - the Mac II and Quadra's part of the menu, keyboard and
 * mouse, for BMMacintosh.
 *
 * The menu's top items (menu.c shows them): the floppy disks and the
 * CD-ROM (put in while the Mac runs; the Mac ejects them), the hard disks
 * and the shared folder (from the Mac's next start), the ROM and the
 * memory, and restarting. All are kept in macii.prefs.
 *
 * Keys and the mouse go to the Mac's Apple Desktop Bus (adb.cpp).
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "sysdeps.h"

#include "main.h"
#include "prefs.h"
#include "adb.h"
#include "macos_util.h"
#include "cpu_emulation.h"
#include "cdrom.h"
#include "sony.h"

#include <sys/stat.h>

#include "../bmmac.h"
#include "../../host.h"
#include "ii.h"

bool ii_insert_disk(const char *path, bool cdrom);
const char *ii_floppy_name(int n);
int ii_drive_count(bool cdrom);

/* ---- Keyboard and mouse -------------------------------------------------------- */

#define MAC_CAPS_LOCK 0x39

static bool key_down[128];
static bool caps_locked;
static int mouse_buttons;

/* Keys pressed before the Mac has started (Shift held to start without
   extensions): Basilisk II queues them, but tells the Mac only with the
   next key's interrupt. ii_adb_frame() tells it once it has started. */
static bool adb_waiting;

void ii_adb_frame(void)
{
	if (adb_waiting && HasMacStarted()) {
		adb_waiting = false;
		SetInterruptFlag(INTFLAG_ADB);
		TriggerInterrupt();
	}
}

void emu_key(int code, int pressed)
{
	if (code < 0 || code > 127)
		return;
	/* The Mac's Caps Lock stays down while it is locked. */
	if (code == MAC_CAPS_LOCK) {
		if (!pressed)
			return;
		caps_locked = !caps_locked;
		pressed = caps_locked;
	}
	if (pressed == key_down[code])
		return;
	key_down[code] = pressed != 0;
	if (pressed)
		ADBKeyDown(code);
	else
		ADBKeyUp(code);
	if (!HasMacStarted())
		adb_waiting = true;
}

void emu_release_keys(void)
{
	for (int code = 0; code < 128; code++)
		if (key_down[code] && code != MAC_CAPS_LOCK) {
			key_down[code] = false;
			ADBKeyUp(code);
		}
	if (mouse_buttons & 1)
		ADBMouseUp(0);
	mouse_buttons = 0;
}

/* An ADB mouse packet's -64 to 63 counts (adb.cpp's ADBInterrupt() sends
   what has gathered since the last in one): bmmac_mouse_tick() gives no
   more, four times a frame. */
int emu_mouse_step_max(void)
{
	return 63;
}

void emu_mouse(int dx, int dy, int buttons)
{
	/* Movement gathers until Mac OS takes it: before it has started, it
	   would arrive in one oversized packet. */
	if ((dx != 0 || dy != 0) && HasMacStarted())
		ADBMouseMoved(dx, dy);
	if ((buttons ^ mouse_buttons) & 1) {
		if (buttons & 1)
			ADBMouseDown(0);
		else
			ADBMouseUp(0);
	}
	mouse_buttons = buttons;
}

/* ---- Disks --------------------------------------------------------------------- */

static const char *const disk_exts[] = {
	"dsk", "img", "image", "hfv", "hda", "dc42", "diskcopy", "toast", "iso", "cdr", "bin", NULL
};
static const char *const cd_exts[] = {"iso", "toast", "cdr", "img", "bin", NULL};

static bool is_cd_image(const char *path)
{
	const char *dot = strrchr(path, '.');
	return dot != NULL && (strcasecmp(dot, ".iso") == 0 || strcasecmp(dot, ".toast") == 0
	                       || strcasecmp(dot, ".cdr") == 0);
}

/* A floppy's size: 400K, 800K or 1.4 MB, with or without a DiskCopy 4.2
   header. */
static bool is_floppy_image(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return false;
	long size = (long) st.st_size, data = size;
	if (size % 512 == 84)
		data = size - 84;
	return data == 409600 || data == 819200 || data == 1474560;
}

static void insert_disk(bool cdrom)
{
	char path[512];
	if (!bmmac_browse(cdrom ? "Insert a CD-ROM Image" : "Insert a Floppy Disk", "disks",
	                  cdrom ? cd_exts : disk_exts, path, sizeof path))
		return;
	if (!ii_insert_disk(path, cdrom)) {
		const char *why = cdrom ? "Eject the CD-ROM on the Mac first (Command-E)."
		                        : "Both floppy drives are full: eject one on the Mac first.";
		printf("BMMacintosh: %s\n", why);
		osd_message(why, TRUE);
	}
}

static void restart_message(void)
{
	osd_message("This takes effect when the Mac restarts (Restart the Mac).", TRUE);
}

static void hard_disks_menu(void)
{
	enum { H_ADD = 1 };
	int sel = 0;
	for (;;) {
		struct osd_item items[12];
		int n = 0, chosen;
		const char *name;
		for (int i = 0; i < 8 && (name = PrefsFindString("disk", i)) != NULL; i++)
			items[n++] = {bmmac_base_name(name), "Remove", 0, 100 + i};
		if (n == 0)
			items[n++] = {"(no hard disks)", NULL, OSD_ITEM_LABEL, -1};
		items[n++] = {"", NULL, OSD_ITEM_LABEL, -1};
		items[n++] = {"Add a Hard Disk Image...", NULL, OSD_ITEM_SUBMENU, H_ADD};
		items[n++] = {"", NULL, OSD_ITEM_LABEL, -1};
		items[n++] = {"Hard disks change when the Mac restarts.", NULL, OSD_ITEM_LABEL, -1};
		chosen = osd_menu("Hard Disks", items, n, &sel);
		if (chosen < 0)
			return;
		if (chosen == H_ADD) {
			char path[512];
			if (!bmmac_browse("Add a Hard Disk Image", "disks", disk_exts, path, sizeof path))
				continue;
			PrefsAddString("disk", path);
		} else if (chosen >= 100) {
			PrefsRemoveItem("disk", chosen - 100);
		}
		SavePrefs();
		restart_message();
	}
}

/* ---- ROM and memory --------------------------------------------------------------- */

static void rom_menu(void)
{
	const struct bmmac_rom *roms[32];
	const char *names[33];
	int n = 0, current = 0, i;
	const char *now = PrefsFindString("rom");
	bmmac_rom_scan();
	for (i = 0; i < bmmac_rom_count() && n < 32; i++) {
		const struct bmmac_rom *r = bmmac_rom_get(i);
		if (r->machine != BMMAC_II)
			continue;
		if (now != NULL && strcmp(now, r->path) == 0)
			current = n;
		roms[n] = r;
		names[n++] = r->name;
	}
	names[n] = NULL;
	if (n == 0) {
		osd_message("No Mac II or Quadra ROM in the roms folder (see roms/ROMS.txt).", TRUE);
		return;
	}
	i = osd_choose("ROM", names, current);
	if (i < 0 || i == current)
		return;
	PrefsReplaceString("rom", roms[i]->path);
	SavePrefs();
	emu_cold_reset();
}

static void memory_menu(void)
{
	static const char *const names[] = {
		"8 MB", "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", NULL
	};
	static const int sizes[] = {8, 16, 32, 64, 128, 256};
	int mb = PrefsFindInt32("ramsize") >> 20, current = 2, i;
	for (i = 0; i < 6; i++)
		if (sizes[i] == mb)
			current = i;
	i = osd_choose("Memory", names, current);
	if (i < 0 || i == current)
		return;
	PrefsReplaceInt32("ramsize", sizes[i] << 20);
	SavePrefs();
	emu_cold_reset();
}

/* ---- The menu's items ------------------------------------------------------------- */

enum {
	I_FLOPPY = 100, I_CDROM, I_EJECT, I_HD, I_SHARED, I_ROM, I_MEMORY, I_RESTART
};

/* Ejects a floppy or the CD as the web page's Eject does (see "Ejecting"
   above): when Mac OS is next idle, once the menu has closed. */
static bool eject_menu(void)
{
	const char *names[4];
	int slots[3], n = 0;
	for (int i = 0; i < 3; i++) {
		const char *in = i == 2 ? PrefsFindString("cdrom") : ii_floppy_name(i);
		if (in != NULL && in[0]) {
			slots[n] = i;
			names[n++] = bmmac_base_name(in);
		}
	}
	names[n] = NULL;
	if (n == 0) {
		osd_message("There is no floppy disk or CD-ROM in the Mac.", TRUE);
		return false;
	}
	int i = osd_choose("Eject", names, 0);
	if (i < 0)
		return false;
	char message[200];
	emu_disk_request(BMMAC_DISK_EJECT, "", slots[i], message, sizeof message);
	osd_notify(message);
	return true;
}

int emu_menu_items(struct osd_item *items, int max)
{
	static char floppies[64], ram[16];
	const char *a = ii_floppy_name(0), *b = ii_floppy_name(1);
	const char *cd = PrefsFindString("cdrom");
	const char *extfs = PrefsFindString("extfs");
	int n = 0;
	if (a[0] && b[0])
		snprintf(floppies, sizeof floppies, "%s, %s", bmmac_base_name(a), bmmac_base_name(b));
	else
		snprintf(floppies, sizeof floppies, "%s", a[0] ? bmmac_base_name(a)
		                                          : b[0] ? bmmac_base_name(b) : "(none)");
	snprintf(ram, sizeof ram, "%d MB", (int) (PrefsFindInt32("ramsize") >> 20));
	const struct osd_item mine[] = {
		{"Insert a Floppy Disk", floppies, OSD_ITEM_SUBMENU, I_FLOPPY},
		{"Insert a CD-ROM", cd && cd[0] ? bmmac_base_name(cd) : "(none)", OSD_ITEM_SUBMENU,
		 I_CDROM},
		{"Eject a Floppy or CD-ROM", NULL, OSD_ITEM_SUBMENU, I_EJECT},
		{"Hard Disks", NULL, OSD_ITEM_SUBMENU, I_HD},
		{"Shared Folder (SD Card disk)", NULL, bmmac_check(extfs != NULL && extfs[0]), I_SHARED},
		{"ROM", PrefsFindString("rom") ? bmmac_base_name(PrefsFindString("rom")) : "(none)",
		 OSD_ITEM_SUBMENU, I_ROM},
		{"Memory", ram, OSD_ITEM_SUBMENU, I_MEMORY},
		{"Restart the Mac", NULL, 0, I_RESTART},
	};
	for (unsigned i = 0; i < sizeof mine / sizeof mine[0] && n < max; i++)
		items[n++] = mine[i];
	return n;
}

int emu_menu_choose(int id)
{
	switch (id) {
	case I_FLOPPY:
		insert_disk(false);
		return TRUE;
	case I_CDROM:
		insert_disk(true);
		return TRUE;
	case I_EJECT:
		return eject_menu();
	case I_HD:
		hard_disks_menu();
		return FALSE;
	case I_SHARED: {
		const char *extfs = PrefsFindString("extfs");
		if (extfs != NULL && extfs[0]) {
			PrefsReplaceString("extfs", "");
		} else {
			char shared[256];
			bmmac_card_path(shared, sizeof shared, "shared", "");
			shared[strlen(shared) - 1] = '\0';
			mkdir(shared, 0777);
			PrefsReplaceString("extfs", shared);
		}
		SavePrefs();
		restart_message();
		return FALSE;
	}
	case I_ROM:
		rom_menu();
		return FALSE;
	case I_MEMORY:
		memory_menu();
		return FALSE;
	case I_RESTART:
		emu_cold_reset();
		return TRUE;
	}
	return TRUE;
}

/* ---- The web page's Disks ------------------------------------------------------------ */

/* An eject the web page asked for, waiting for Mac OS to be idle (see
   "Ejecting" below). */
static int eject_slot = -1;          /* 0, 1: a floppy drive; 2: the CD */
static double eject_asked;
static char eject_name[64];


void emu_disks(struct bmmac_disks *d)
{
	memset(d, 0, sizeof *d);
	const char *name;
	for (int i = 0; i < BMMAC_HARD_DISKS && (name = PrefsFindString("disk", i)) != NULL; i++)
		bmmac_card_relative(name, d->hard[d->hard_count++], BMMAC_DISK_PATH);
	for (int i = 0; i < 2; i++)
		bmmac_card_relative(ii_floppy_name(i), d->floppy[i], BMMAC_DISK_PATH);
	const char *cd = PrefsFindString("cdrom");
	bmmac_card_relative(cd != NULL ? cd : "", d->cdrom, BMMAC_DISK_PATH);
}

void emu_disk_request(int op, const char *path, int index, char *message, unsigned size)
{
	const char *name = bmmac_base_name(path);
	switch (op) {
	case BMMAC_DISK_START:
		emu_autostart(path);
		snprintf(message, size, "The Mac starts again, from %s.", name);
		return;
	case BMMAC_DISK_INSERT: {
		bool cdrom = is_cd_image(path);
		if (!cdrom && !is_floppy_image(path)) {
			snprintf(message, size, "%s isn't a floppy (400K, 800K or 1.4 MB) or a "
			         "CD-ROM image: add it as a hard disk.", name);
			return;
		}
		if (ii_drive_count(cdrom) == 0) {
			snprintf(message, size, "The Mac isn't running: start it first.");
			return;
		}
		if (!ii_insert_disk(path, cdrom)) {
			snprintf(message, size, "%s", cdrom ? "Eject the CD-ROM on the Mac first (Command-E)."
			         : "Both floppy drives are full: eject one on the Mac first.");
			return;
		}
		snprintf(message, size, "%s is in the %s.", name, cdrom ? "CD-ROM drive" : "floppy drive");
		return;
	}
	case BMMAC_DISK_ADD: {
		int n = 0;
		const char *s;
		while ((s = PrefsFindString("disk", n)) != NULL) {
			if (strcmp(s, path) == 0) {
				snprintf(message, size, "%s is a hard disk already.", name);
				return;
			}
			n++;
		}
		if (n >= BMMAC_HARD_DISKS) {
			snprintf(message, size, "The Mac has %d hard disks: remove one first.", n);
			return;
		}
		PrefsAddString("disk", path);
		SavePrefs();
		snprintf(message, size, "%s is a hard disk when the Mac restarts.", name);
		return;
	}
	case BMMAC_DISK_EJECT: {
		const char *in = index == 2 ? PrefsFindString("cdrom") : ii_floppy_name(index);
		if (index < 0 || index > 2 || in == NULL || in[0] == '\0') {
			snprintf(message, size, "There is no disk in that drive.");
			return;
		}
		if (!HasMacStarted()) {
			snprintf(message, size, "The Mac isn't running: start it first.");
			return;
		}
		eject_slot = index;
		eject_asked = host_time();
		snprintf(eject_name, sizeof eject_name, "%s", bmmac_base_name(in));
		snprintf(message, size, "Ejecting %s when the Mac is next idle...", eject_name);
		return;
	}
	case BMMAC_DISK_REMOVE: {
		const char *s = PrefsFindString("disk", index);
		if (index < 0 || s == NULL) {
			snprintf(message, size, "There is no hard disk %d.", index + 1);
			return;
		}
		char removed[BMMAC_DISK_PATH];
		snprintf(removed, sizeof removed, "%s", bmmac_base_name(s));
		PrefsRemoveItem("disk", index);
		SavePrefs();
		snprintf(message, size, "%s is gone when the Mac restarts.", removed);
		return;
	}
	}
	snprintf(message, size, "Unknown request.");
}

/* ---- Ejecting, from the web page ----------------------------------------------------
   Pulling an image out under Mac OS would leave it a volume that isn't
   there (and a floppy's last writes lost). Mac OS is asked instead, as
   dragging the disk to the Trash does: UnmountVol, then Eject. Its File
   and Memory Managers may be busy when the web page asks, so it is done
   when Mac OS is next idle (SynchIdleTime(), timer_ii.cpp's idle_wait()):
   in a program's place, as a program would. If Mac OS isn't idle within a
   few seconds (a program busy, or a System the patch didn't fit), the web
   page is told to eject it on the Mac. */


/* The Mac's drive number for a slot: the drive queue's floppy drives
   (Sony's) are in the order sys_ii.cpp opened them, as ii_floppy_name()
   counts them; the CD-ROM drive is CDROMRefNum's. 0 if none. */
static int drive_number(int slot)
{
	int16 refnum = slot == 2 ? CDROMRefNum : SonyRefNum;
	int k = 0;
	for (uint32 q = ReadMacInt32(0x308 + 2); q != 0; q = ReadMacInt32(q)) {
		if ((int16) ReadMacInt16(q + 8) != refnum)
			continue;
		if (slot == 2 || k++ == slot)
			return (int16) ReadMacInt16(q + 6);
	}
	return 0;
}

void ii_idle(void)
{
	if (eject_slot < 0)
		return;
	if (ReadMacInt16(0x360) != 0)        /* FSBusy: the File Manager is at work */
		return;
	int slot = eject_slot, drive = drive_number(slot);
	eject_slot = -1;
	char text[160];
	if (drive == 0) {
		snprintf(text, sizeof text, "The Mac has no such drive.");
		bmmac_disk_note(text);
		return;
	}
	M68kRegisters r;
	r.d[0] = 80;
	Execute68kTrap(0xa71e, &r);           /* NewPtrSysClear: a parameter block */
	uint32 pb = r.a[0];
	if (pb == 0) {
		bmmac_disk_note("The Mac is out of memory: eject it on the Mac (Command-E).");
		return;
	}
	WriteMacInt16(pb + 22, drive);        /* ioVRefNum: the drive */
	r.a[0] = pb;
	Execute68kTrap(0xa00e, &r);           /* UnmountVol */
	int16 err = (int16) r.d[0];
	/* -35 (nsvErr): no volume on it (an empty or unreadable disk): eject
	   it all the same. */
	if (err != 0 && err != -35) {
		if (err == -47)                   /* fBsyErr */
			snprintf(text, sizeof text, "%s is in use on the Mac: close its files "
			         "and windows, then eject it again.", eject_name);
		else
			snprintf(text, sizeof text, "The Mac couldn't put %s away (error %d): "
			         "eject it on the Mac (Command-E).", eject_name, err);
	} else {
		WriteMacInt16(pb + 22, drive);
		r.a[0] = pb;
		Execute68kTrap(0xa017, &r);       /* Eject */
		err = (int16) r.d[0];
		if (err == 0)
			snprintf(text, sizeof text, "%s is ejected.", eject_name);
		else
			snprintf(text, sizeof text, "The Mac couldn't eject %s (error %d).",
			         eject_name, err);
	}
	r.a[0] = pb;
	Execute68kTrap(0xa01f, &r);           /* DisposePtr */
	bmmac_disk_note(text);
}

/* Called each frame: an eject Mac OS hasn't had the time for. */
void ii_eject_check(void)
{
	if (eject_slot >= 0 && host_time() - eject_asked > 5.0) {
		eject_slot = -1;
		char text[160];
		snprintf(text, sizeof text, "The Mac was too busy to eject %s: eject it on the "
		         "Mac (drag it to the Trash, or Command-E).", eject_name);
		bmmac_disk_note(text);
	}
}

/* ---- Starting from a disk ------------------------------------------------------------- */

/* A floppy or CD-ROM image goes into its drive (the first floppy drive),
   a hard disk image becomes the first hard disk, and the Mac restarts
   from it. */
void emu_autostart(const char *path)
{
	/* The startup device (Basilisk II puts these in the Mac's parameter RAM
	   at each start): the CD-ROM driver for a CD, which the Mac doesn't
	   start from otherwise; any for the rest (a startable floppy first,
	   then the first hard disk with a System). */
	PrefsReplaceInt32("bootdrive", 0);
	PrefsReplaceInt32("bootdriver", is_cd_image(path) ? CDROMRefNum : 0);
	if (is_cd_image(path)) {
		PrefsReplaceString("cdrom", path);
	} else if (is_floppy_image(path)) {
		PrefsReplaceString("floppy", path, 0);
	} else {
		const char *first = PrefsFindString("disk", 0);
		if (first == NULL || strcmp(first, path) != 0) {
			/* In front of the others: the Mac starts from the first. */
			int n = 0;
			char *others[8];
			const char *s;
			while (n < 8 && (s = PrefsFindString("disk", 0)) != NULL) {
				others[n++] = strdup(s);
				PrefsRemoveItem("disk", 0);
			}
			PrefsAddString("disk", path);
			for (int i = 0; i < n; i++) {
				if (strcmp(others[i], path) != 0)
					PrefsAddString("disk", others[i]);
				free(others[i]);
			}
		}
	}
	SavePrefs();
	emu_cold_reset();
}
