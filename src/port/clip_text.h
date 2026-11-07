/*
 * clip_text.h - text between the Mac's clipboard (MacRoman, CR line ends)
 * and the web page's (UTF-8, LF line ends), for the Mac II's shared
 * clipboard (src/port/ii/clip_ii.cpp).
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#ifndef BMMAC_CLIP_TEXT_H
#define BMMAC_CLIP_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* MacRoman (CR line ends) to UTF-8 (LF) in OUT, NUL-terminated; what
   doesn't fit is left off (never half a character). Returns its length. */
unsigned clip_mac_to_utf8(const unsigned char *in, unsigned len, char *out, unsigned size);

/* UTF-8 (LF or CR LF) to MacRoman (CR) in OUT; a character MacRoman
   hasn't becomes "?", and a broken UTF-8 byte too. Returns its length. */
unsigned clip_utf8_to_mac(const char *in, unsigned len, unsigned char *out, unsigned size);

#ifdef __cplusplus
}
#endif

#endif
