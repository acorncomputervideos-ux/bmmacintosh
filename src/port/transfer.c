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

void transfer_begin(const char *name, int upload, long long total)
{
	snprintf(state.name, sizeof state.name, "%s", name);
	state.upload = upload;
	state.total = total;
	state.done = 0;
	state.serial++;
	state.active = 1;
}

void transfer_progress(long long done)
{
	state.done = done;
}

void transfer_end(int ok)
{
	if (!state.active)
		return;
	state.active = 0;
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
