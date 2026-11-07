/*
 * clip_text.c - see clip_text.h. tools/clipboard tests it against
 * Python's mac_roman codec, which this table was made from.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "clip_text.h"

/* MacRoman 0x80..0xFF -> Unicode (Mac OS 8.5's: 0xDB is the euro sign). */
static const unsigned short mac_roman[128] = {
	0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
	0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
	0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
	0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
	0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
	0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
	0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
	0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
	0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
	0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
	0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
	0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
	0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
	0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
	0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
	0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

unsigned clip_mac_to_utf8(const unsigned char *in, unsigned len, char *out, unsigned size)
{
	unsigned n = 0, i;
	if (size == 0)
		return 0;
	for (i = 0; i < len; i++) {
		unsigned cp = in[i] < 0x80 ? in[i] : mac_roman[in[i] - 0x80];
		char c[3];
		unsigned k;
		if (cp == 13)
			cp = 10;
		if (cp < 0x80) {
			c[0] = (char) cp;
			k = 1;
		} else if (cp < 0x800) {
			c[0] = (char) (0xC0 | (cp >> 6));
			c[1] = (char) (0x80 | (cp & 0x3F));
			k = 2;
		} else {
			c[0] = (char) (0xE0 | (cp >> 12));
			c[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
			c[2] = (char) (0x80 | (cp & 0x3F));
			k = 3;
		}
		if (n + k + 1 > size)
			break;
		for (unsigned j = 0; j < k; j++)
			out[n++] = c[j];
	}
	out[n] = '\0';
	return n;
}

static unsigned char to_mac(unsigned cp)
{
	unsigned i;
	if (cp < 0x80)
		return (unsigned char) cp;
	for (i = 0; i < 128; i++)
		if (mac_roman[i] == cp)
			return (unsigned char) (0x80 + i);
	return '?';
}

unsigned clip_utf8_to_mac(const char *in, unsigned len, unsigned char *out, unsigned size)
{
	const unsigned char *p = (const unsigned char *) in, *end = p + len;
	unsigned n = 0;
	while (p < end && n < size) {
		unsigned cp;
		int extra;
		if (*p < 0x80) {
			cp = *p;
			extra = 0;
		} else if ((*p & 0xE0) == 0xC0) {
			cp = *p & 0x1F;
			extra = 1;
		} else if ((*p & 0xF0) == 0xE0) {
			cp = *p & 0x0F;
			extra = 2;
		} else if ((*p & 0xF8) == 0xF0) {
			cp = *p & 0x07;
			extra = 3;
		} else {
			p++;
			out[n++] = '?';
			continue;
		}
		p++;
		int ok = 1;
		for (int i = 0; i < extra; i++) {
			if (p >= end || (*p & 0xC0) != 0x80) {
				ok = 0;
				break;
			}
			cp = cp << 6 | (*p++ & 0x3F);
		}
		if (!ok) {
			out[n++] = '?';
			continue;
		}
		if (cp == '\r' && p < end && *p == '\n')
			continue; /* CR LF: the LF makes the CR */
		out[n++] = cp == '\n' ? '\r' : to_mac(cp);
	}
	return n;
}
