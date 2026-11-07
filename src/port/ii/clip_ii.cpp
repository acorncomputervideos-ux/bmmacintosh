/*
 *  clip_ii.cpp - the Mac II's shared clipboard, for BMMacintosh
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

/*
 *  BMMacintosh: Basilisk II's clipboard driver (in place of its
 *  dummy/clip_dummy.cpp), from its Unix one (clip_unix.cpp), with the web
 *  page in X11's place:
 *
 *  - When a Mac program copies text, Mac OS's PutScrap() comes here
 *    (Basilisk II patches it): the text is kept, as UTF-8, for the web
 *    page's Clipboard (emu_clipboard_text()).
 *  - Text set on the web page (emu_clipboard_set()) is given to the Mac the
 *    next time a program reads the clipboard (GetScrap()): it is put there
 *    with Mac OS's own ZeroScrap() and PutScrap(), as clip_unix.cpp does.
 *
 *  Text only (styles, pictures and the rest stay on the Mac), up to
 *  CLIP_MAX bytes. The web page's task and the Mac run on the same core,
 *  one at a time, so the two never meet halfway.
 */

#include "sysdeps.h"

#include "clip.h"
#include "macos_util.h"
#include "cpu_emulation.h"
#include "emul_op.h"
#include "main.h"

#include "../bmmac.h"
#include "../clip_text.h"

#define CLIP_MAX (64 * 1024)

/* The clipboard's text, as UTF-8, and who put it there last. */
static char text[CLIP_MAX * 3 + 1];
static int from_mac;
/* Set on the web page, not given to the Mac yet. */
static bool for_mac;
/* The Mac's PutScrap() is about to give back what GetScrap() put in. */
static bool we_put_this_data;

void ClipInit(void)
{
}

void ClipExit(void)
{
}

void ZeroScrap()
{
}

/* A Mac program copied. */
void PutScrap(uint32 type, void *scrap, int32 length)
{
	if (we_put_this_data) {
		we_put_this_data = false;
		return;
	}
	if (type != FOURCC('T','E','X','T') || length < 0)
		return;
	if (length > CLIP_MAX)
		length = CLIP_MAX;
	clip_mac_to_utf8((const unsigned char *) scrap, (unsigned) length, text, sizeof text);
	from_mac = 1;
	for_mac = false;
}

/* A Mac program pastes: the web page's text, if it has new text. */
void GetScrap(void **handle, uint32 type, int32 offset)
{
	(void) handle;
	(void) offset;
	if (type != FOURCC('T','E','X','T') || !for_mac)
		return;
	for_mac = false;

	static unsigned char mac[CLIP_MAX];
	unsigned length = clip_utf8_to_mac(text, (unsigned) strlen(text), mac, sizeof mac);

	M68kRegisters r;
	r.d[0] = length > 0 ? length : 1;
	Execute68kTrap(0xa71e, &r);			// NewPtrSysClear()
	uint32 scrap_area = r.a[0];
	if (scrap_area == 0)
		return;
	Host2Mac_memcpy(scrap_area, mac, length);

	static const uint8 proc[] = {
		0x59, 0x8f,					// subq.l	#4,sp
		0xa9, 0xfc,					// ZeroScrap()
		0x2f, 0x3c, 0, 0, 0, 0,		// move.l	#length,-(sp)
		0x2f, 0x3c, 0, 0, 0, 0,		// move.l	#type,-(sp)
		0x2f, 0x3c, 0, 0, 0, 0,		// move.l	#outbuf,-(sp)
		0xa9, 0xfe,					// PutScrap()
		0x58, 0x8f,					// addq.l	#4,sp
		M68K_RTS >> 8, M68K_RTS & 0xff
	};
	r.d[0] = sizeof(proc);
	Execute68kTrap(0xa71e, &r);			// NewPtrSysClear()
	uint32 proc_area = r.a[0];
	if (proc_area != 0) {
		// In the Mac's memory, as clip_unix.cpp has it.
		Host2Mac_memcpy(proc_area, proc, sizeof(proc));
		WriteMacInt32(proc_area +  6, length);
		WriteMacInt32(proc_area + 12, type);
		WriteMacInt32(proc_area + 18, scrap_area);
		we_put_this_data = true;
		Execute68k(proc_area, &r);
		r.a[0] = proc_area;
		Execute68kTrap(0xa01f, &r);		// DisposePtr
	}
	r.a[0] = scrap_area;
	Execute68kTrap(0xa01f, &r);			// DisposePtr
}

/* ---- The web page's Clipboard ------------------------------------------------- */

const char *emu_clipboard_text(int *mac)
{
	if (mac != NULL)
		*mac = from_mac;
	return text;
}

int emu_clipboard_set(const char *utf8, unsigned length)
{
	if (length > CLIP_MAX)
		return FALSE;
	memcpy(text, utf8, length);
	text[length] = '\0';
	from_mac = 0;
	for_mac = true;
	return TRUE;
}
