// test_update.cpp - runs src/webui/update.cpp's UpdateInstall() on a
// folder standing in for the SD card (tools/webdav's sd_fs_posix.cpp):
//
//   test_update <card folder> <zip on the card, e.g. /bmmac-update.zip>
//
// Prints the result as "ok|replaced|kept|version|message".
//
// This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
// the Raspberry Pi. See LICENSE for licensing details.

#include <stdio.h>

#include "../../src/webui/update.h"

extern "C" void sd_posix_root(const char *path);

static unsigned yields;
static void Yield(void) { yields++; }

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: test_update <card> <zip>\n");
		return 2;
	}
	sd_posix_root(argv[1]);
	UpdateResult r;
	UpdateInstall(argv[2], &r, Yield);
	printf("%d|%u|%u|%s|%s\n", r.ok ? 1 : 0, r.replaced, r.kept, r.version, r.message);
	return 0;
}
