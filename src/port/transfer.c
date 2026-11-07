/*
 * transfer.c - see transfer.h.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "transfer.h"

#include <stdio.h>
#include <string.h>

static struct transfer_info state;
static double started;

/* The kernel's (and the PC simulator's) clock, in seconds. */
#ifdef __cplusplus
extern "C"
#endif
double host_time(void);

int transfer_begin(const char *name, int upload, long long total)
{
	if (state.active)
		return 0;   /* another is shown: this one is counted at its end */
	snprintf(state.name, sizeof state.name, "%s", name);
	state.upload = upload;
	state.total = total;
	state.done = 0;
	state.serial++;
	state.active = 1;
	started = host_time();
	return (int) state.serial;
}

void transfer_progress(int transfer, long long done)
{
	if (state.active && transfer == (int) state.serial)
		state.done = done;
}

void transfer_end(int transfer, int ok)
{
	if (!state.active || transfer != (int) state.serial) {
		if (ok)
			state.files++;
		else
			state.failed++;
		return;
	}
	state.active = 0;
	snprintf(state.last_name, sizeof state.last_name, "%s", state.name);
	state.last_upload = state.upload;
	state.last_ok = ok;
	state.last_bytes = state.done;
	state.last_seconds = host_time() - started;
	state.finished++;
	if (ok) {
		state.files++;
		state.bytes += state.done;
	} else {
		state.failed++;
	}
}

void transfer_get(struct transfer_info *info)
{
	*info = state;
}

void transfer_session_reset(void)
{
	state.files = 0;
	state.bytes = 0;
	state.failed = 0;
}
