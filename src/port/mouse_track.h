/*
 * mouse_track.h - the USB mouse's movement, smoothed for the Mac.
 *
 * A USB mouse reports a few hundred times a second (125 a second for most),
 * which doesn't divide into the Mac's 60.15 frames: a frame got two reports'
 * movement, the next three, and the pointer stuttered even when the hand
 * moved evenly (Mac OS's mouse acceleration made it worse). Here each
 * report is kept with the time it came, and the mouse is read as it was a
 * report's interval ago, between the reports either side: it moves as
 * evenly as the hand did, about 8 ms late (with 125 reports a second).
 *
 * One writer (the USB interrupt: mouse_track_report()) and one reader (the
 * emulator: mouse_track_sample()), on different cores: a ring of reports,
 * published by its head index.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#ifndef BMMAC_MOUSE_TRACK_H
#define BMMAC_MOUSE_TRACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A report: its time (microseconds, any steady clock), the movement and
   the buttons (bit 0 the left one). */
void mouse_track_report(uint64_t t_us, int dx, int dy, int buttons);

/* The mouse at NOW_US: its position (counts moved since the start, with
   fractions) and buttons, as they were a report's interval earlier.
   FALSE when there has been no report yet. */
int mouse_track_sample(uint64_t now_us, double *x, double *y, int *buttons);

/* Forgets every report (a mouse unplugged, or the tests). */
void mouse_track_reset(void);

#ifdef __cplusplus
}
#endif

#endif
