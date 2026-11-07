//
// webui_disks.h - the web page's Disks: the Mac's disks, the disk images on
// the card, and changing them.
//
//   GET /api/disks   -> WebUiDisksJson()
//   POST /api/disks?op=start|insert|add|remove&path=/disks/x.dsk&index=N
//                    -> WebUiDisksRequest()
//
// The change is made by the emulator between frames (bmmac_disk_interrupt()
// in src/port/frontend.c); /api/disks says when it is done ("done" counts
// them) and what happened ("result").
//
// Only sd_fs.h and src/port/bmmac.h are used, so tools/disks tests it on
// the PC.
//
// This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
// the Raspberry Pi. See LICENSE for licensing details.
//

#ifndef _webui_disks_h
#define _webui_disks_h

// The JSON for GET /api/disks in out: its length, or -1 if it didn't fit.
int WebUiDisksJson(char *out, unsigned size);

// Does a POST /api/disks: the HTTP status, with a JSON answer in out.
int WebUiDisksRequest(const char *query, char *out, unsigned size);

#endif
