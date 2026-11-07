/*
 * test_disks.cpp - the web page's Disks (src/webui/webui_disks.cpp) on the
 * PC, over a folder as the card (tools/webdav/sd_fs_posix.cpp), with
 * stand-ins for the emulator.
 *
 *   test_disks <folder> ii|plus json            prints GET /api/disks
 *   test_disks <folder> ii|plus post <query>    prints the status and answer,
 *                                               and the request queued
 *
 * tools/disks/run_tests.sh runs it.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "../../src/webui/webui_disks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../../src/port/bmmac.h"
void sd_posix_root(const char *path);
}

static int machine = BMMAC_II;
static bool busy;
static int queued_op = -1, queued_index = -1;
static char queued_path[600];

extern "C" int emu_machine(void) { return machine; }

extern "C" const char *const *emu_run_types(void) {
	static const char *const ii[] = {"dsk", "img", "image", "hfv", "hda", "dc42",
	                                 "diskcopy", "iso", "toast", "cdr", NULL};
	static const char *const plus[] = {"dsk", "img", "image", "hfv", "hda", "dc42",
	                                   "diskcopy", NULL};
	return machine == BMMAC_II ? ii : plus;
}

extern "C" void emu_disks(struct bmmac_disks *d) {
	memset(d, 0, sizeof *d);
	if (machine == BMMAC_II) {
		d->hard_count = 1;
		strcpy(d->hard[0], "/disks/Mac OS 8.1.hda");
		strcpy(d->floppy[1], "/disks/caf\x82.dsk");  /* CP850 é */
	} else {
		strcpy(d->disk, "/disks/sys.dsk");
	}
}

extern "C" int bmmac_disk_interrupt(int op, const char *path, int index) {
	if (busy) return 0;
	queued_op = op;
	queued_index = index;
	snprintf(queued_path, sizeof queued_path, "%s", path);
	return 1;
}

extern "C" const char *bmmac_disk_result(int *pending, unsigned *done) {
	*pending = 0;
	*done = 7;
	return "f.dsk is in the floppy drive.";
}

int main(int argc, char **argv) {
	if (argc < 4) return 2;
	sd_posix_root(argv[1]);
	machine = strcmp(argv[2], "plus") == 0 ? BMMAC_PLUS : BMMAC_II;
	static char out[48 * 1024];
	if (strcmp(argv[3], "json") == 0) {
		int n = WebUiDisksJson(out, sizeof out);
		if (n < 0) return 1;
		fwrite(out, 1, (size_t) n, stdout);
		return 0;
	}
	busy = getenv("BUSY") != NULL;
	int status = WebUiDisksRequest(argc > 4 ? argv[4] : "", out, sizeof out);
	printf("%d %s\n", status, out);
	if (queued_op >= 0) printf("queued %d %d %s\n", queued_op, queued_index, queued_path);
	return 0;
}
