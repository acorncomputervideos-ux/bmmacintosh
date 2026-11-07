/*
 * timer_ii.cpp - Basilisk II's clocks for BMMacintosh.
 *
 * The Mac's date and time are the Pi's (set from the network, see the User
 * Guide) in the time zone the menu sets; its Time Manager and Microseconds
 * count the emulated time (main_ii.cpp's ii_mac_time_usec()), which moves
 * with the CPU's work and keeps the frames' pace.
 *
 * From Basilisk II's Unix timer_unix.cpp.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "sysdeps.h"

#include "macos_util.h"
#include "timer.h"

#include "../../host.h"
#include "ii.h"

/* Seconds from 1904 (the Mac's epoch) to 1970 (the C library's). */
#define MAC_EPOCH_DIFF 2082844800u

uint64 GetTicks_usec(void)
{
	return ii_mac_time_usec();
}

void Delay_usec(uint64 usec)
{
	host_sleep_us((unsigned) usec);
}

/* Mac OS is idle (Basilisk II's SynchIdleTime() patch, no events
   waiting): no sleeping here, but the moment for what must run as a
   program would (menu_ii.cpp's ejects). */
void ii_idle(void);

void idle_wait(void)
{
	ii_idle();
}

void idle_resume(void)
{
}

/* The local date and time, in seconds since 1904. */
uint32 TimerDateTime(void)
{
	time_t now = time(NULL);
	return (uint32) ((int64) now + host_option_int("timezone", 0) * 60 + MAC_EPOCH_DIFF);
}

void Microseconds(uint32 &hi, uint32 &lo)
{
	uint64 t = GetTicks_usec();
	hi = (uint32) (t >> 32);
	lo = (uint32) t;
}

void timer_current_time(tm_time_t &t)
{
	uint64 us = GetTicks_usec();
	t.tv_sec = (time_t) (us / 1000000);
	t.tv_usec = (suseconds_t) (us % 1000000);
}

void timer_add_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res.tv_sec = a.tv_sec + b.tv_sec;
	res.tv_usec = a.tv_usec + b.tv_usec;
	if (res.tv_usec >= 1000000) {
		res.tv_sec++;
		res.tv_usec -= 1000000;
	}
}

void timer_sub_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res.tv_sec = a.tv_sec - b.tv_sec;
	res.tv_usec = a.tv_usec - b.tv_usec;
	if (res.tv_usec < 0) {
		res.tv_sec--;
		res.tv_usec += 1000000;
	}
}

int timer_cmp_time(tm_time_t a, tm_time_t b)
{
	if (a.tv_sec != b.tv_sec)
		return a.tv_sec < b.tv_sec ? -1 : 1;
	if (a.tv_usec != b.tv_usec)
		return a.tv_usec < b.tv_usec ? -1 : 1;
	return 0;
}

/* A Mac time: more than 0 is milliseconds, less than 0 microseconds. */
void timer_mac2host_time(tm_time_t &res, int32 mactime)
{
	if (mactime > 0) {
		res.tv_sec = mactime / 1000;
		res.tv_usec = (mactime % 1000) * 1000;
	} else {
		res.tv_sec = -mactime / 1000000;
		res.tv_usec = -mactime % 1000000;
	}
}

int32 timer_host2mac_time(tm_time_t hosttime)
{
	if (hosttime.tv_sec < 0)
		return 0;
	uint64 t = (uint64) hosttime.tv_sec * 1000000 + hosttime.tv_usec;
	if (t > 0x7fffffff)
		return (int32) (t / 1000);
	return -(int32) t;
}
