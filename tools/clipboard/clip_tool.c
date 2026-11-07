/*
 * clip_tool.c - src/port/clip_text.c on the PC, for tools/clipboard's
 * tests: "tomac" reads UTF-8 and writes MacRoman, "frommac" the other way.
 * SIZE (optional) is the output buffer's size.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "../../src/port/clip_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	static unsigned char in[1 << 20];
	static char out[3 << 20];
	size_t len = fread(in, 1, sizeof in, stdin);
	unsigned size = argc > 2 ? (unsigned) atoi(argv[2]) : sizeof out;
	unsigned n;
	if (argc > 1 && strcmp(argv[1], "tomac") == 0)
		n = clip_utf8_to_mac((const char *) in, (unsigned) len, (unsigned char *) out, size);
	else
		n = clip_mac_to_utf8(in, (unsigned) len, out, size);
	fwrite(out, 1, n, stdout);
	return 0;
}
