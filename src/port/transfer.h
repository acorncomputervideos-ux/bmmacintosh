/*
 * transfer.h - the web UI's file transfers (the network drive's and the web
 * page's), for the screen.
 *
 * The web UI's task says what it is sending or receiving and how far it
 * has got; the emulator reads it. While a big one runs, the Mac is paused
 * and a window on the screen shows it (frontend.c): the network then has
 * the Pi's core to itself (it shares the Mac's otherwise, and Wi-Fi above
 * all crawled).
 *
 * Both run on the same core, one at a time: no locking.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#ifndef BMMAC_TRANSFER_H
#define BMMAC_TRANSFER_H

#ifdef __cplusplus
extern "C" {
#endif

/* The web UI's side. NAME is the file's path on the card; TOTAL its size,
   or -1 when it isn't known (a chunked upload). transfer_begin() returns
   the transfer's number for the other two. Several can run at once (the
   network drive answers a few requests at a time): the first is the one
   shown, the others are only counted. */
int transfer_begin(const char *name, int upload, long long total);
void transfer_progress(int transfer, long long done);
void transfer_end(int transfer, int ok);

/* The emulator's side. */
struct transfer_info {
	int active;               /* one is running */
	char name[256];
	int upload;               /* from the computer to the card */
	long long total, done;    /* total -1: not known */
	unsigned serial;          /* goes up with each transfer begun */
	unsigned synced;          /* the serial the emulator last saved its disks for */
	/* Since transfer_session_reset(): files finished, and their bytes. */
	unsigned files;
	long long bytes;
	int failed;               /* how many broke off */
	/* The last one finished (or broken off): finished goes up with each. */
	unsigned finished;
	char last_name[256];
	int last_upload, last_ok;
	long long last_bytes;
	double last_seconds;
};
void transfer_get(struct transfer_info *info);
/* The emulator has written its disk images' pending changes to the card
   for transfer SERIAL (at a frame's end, as it begins): a download of one
   is then the disk as the Mac left it. */
void transfer_synced(unsigned serial);
void transfer_session_reset(void);

#ifdef __cplusplus
}
#endif

#endif
