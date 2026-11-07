/*
 * prefs_ii.cpp - Basilisk II's settings, PRAM and messages for
 * BMMacintosh.
 *
 * The settings are Basilisk II's own, in its own format, in macii.prefs in
 * the card's root (which the web UI can edit), and the Mac's parameter RAM
 * in macii.xpram. A card without them starts as a Quadra 900 (Basilisk
 * II's usual choice for a Quadra ROM) with a 68040 and 32 MB, a 640x480
 * screen in 256 colours, and the card's "shared" folder as a disk on the
 * Mac's desktop (ExtFS).
 *
 * From Basilisk II's Unix prefs_unix.cpp, xpram_unix.cpp and
 * user_strings_unix.cpp.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "sysdeps.h"

#include "prefs.h"
#include "xpram.h"
#include "user_strings.h"

#include "../bmmac.h"

/* ---- Settings --------------------------------------------------------------------- */

prefs_desc platform_prefs_items[] = {
	{"idlewait", TYPE_BOOLEAN, false, "sleep when idle"},
	{"nonet", TYPE_BOOLEAN, false, "no network for the Mac (ether_ii.cpp)"},
	{NULL, TYPE_END, false, NULL}
};

static char prefs_path[256];

void AddPlatformPrefsDefaults(void)
{
	char shared[256];
	bmmac_card_path(shared, sizeof shared, "shared", "");
	/* The trailing slash off. */
	if (strlen(shared) > 1 && shared[strlen(shared) - 1] == '/')
		shared[strlen(shared) - 1] = '\0';
	PrefsReplaceString("extfs", shared);
	PrefsReplaceInt32("ramsize", 32 * 1024 * 1024);
	PrefsReplaceInt32("modelid", 14);
	PrefsReplaceInt32("cpu", 4);
	PrefsReplaceBool("fpu", true);
	PrefsReplaceString("screen", "win/640/480");
	PrefsReplaceInt32("displaycolordepth", 8);
	PrefsReplaceInt32("frameskip", 0);
	/* Basilisk II's SynchIdleTime() patch: the web page's ejects are done
	   when Mac OS is idle (menu_ii.cpp). idle_wait() doesn't sleep. */
	PrefsReplaceBool("idlewait", true);
	PrefsReplaceBool("nonet", false);
}

void LoadPrefs(const char *vmdir)
{
	FILE *f;
	(void) vmdir;
	bmmac_card_path(prefs_path, sizeof prefs_path, "", "macii.prefs");
	f = fopen(prefs_path, "r");
	if (f != NULL) {
		LoadPrefsFromStream(f);
		fclose(f);
	} else {
		SavePrefs();
	}
}

void SavePrefs(void)
{
	FILE *f = fopen(prefs_path, "w");
	if (f == NULL) {
		printf("BMMacintosh: cannot write %s\n", prefs_path);
		return;
	}
	SavePrefsToStream(f);
	fclose(f);
}

/* ---- Parameter RAM ------------------------------------------------------------------ */

static void xpram_path(char *path, size_t size)
{
	bmmac_card_path(path, (unsigned) size, "", "macii.xpram");
}

void LoadXPRAM(const char *vmdir)
{
	char path[256];
	FILE *f;
	(void) vmdir;
	xpram_path(path, sizeof path);
	f = fopen(path, "rb");
	if (f == NULL)
		return;
	if (fread(XPRAM, 1, XPRAM_SIZE, f) != XPRAM_SIZE)
		printf("BMMacintosh: %s is short\n", path);
	fclose(f);
}

void SaveXPRAM(void)
{
	char path[256];
	FILE *f;
	xpram_path(path, sizeof path);
	f = fopen(path, "wb");
	if (f == NULL)
		return;
	fwrite(XPRAM, 1, XPRAM_SIZE, f);
	fclose(f);
}

void ZapPRAM(void)
{
	char path[256];
	xpram_path(path, sizeof path);
	remove(path);
}

/* ---- Messages ------------------------------------------------------------------------ */

user_string_def platform_strings[] = {
	/* The ExtFS disk's name on the Mac's desktop. */
	{STR_EXTFS_VOLUME_NAME, "SD Card"},
	{-1, NULL}
};

const char *GetString(int num)
{
	int i;
	for (i = 0; platform_strings[i].num >= 0; i++)
		if (platform_strings[i].num == num)
			return platform_strings[i].str;
	for (i = 0; common_strings[i].num >= 0; i++)
		if (common_strings[i].num == num)
			return common_strings[i].str;
	return NULL;
}
