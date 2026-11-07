/*
 * test_mouse.c - tests src/port/mouse_track.c, the USB mouse's movement
 * smoothed for the Mac, on the PC: a hand moving evenly, as a USB mouse
 * reports it (125 or 1000 reports a second, their times a little uneven),
 * read at the Mac's 60.15 frames a second. The steps from frame to frame
 * must be even (they weren't, summing the reports in each frame), and the
 * pointer must end up where the hand did.
 *
 *   tools/mouse/run_tests.sh
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "../../src/port/mouse_track.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failed;

static void check(int ok, const char *what)
{
	printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failed++;
}

/* The furthest a frame's step is from the hand's movement in a frame
   (IDEAL counts). The mouse only reports whole counts, so even read
   perfectly a step is up to about one count out. */
static double worst(const double *steps, int n, double ideal)
{
	double w = 0;
	for (int i = 0; i < n; i++)
		if (fabs(steps[i] - ideal) > w)
			w = fabs(steps[i] - ideal);
	return w;
}

/* A hand moving SPEED counts a second for SECONDS, reported RATE times a
   second; the frames' steps, the old way (whole reports summed per frame)
   and through mouse_track. */
static void run(double rate, double speed, double seconds, double *old_cv, double *new_cv,
                double *final_x, double *total)
{
	enum { MAXF = 2000 };
	static double old_steps[MAXF], new_steps[MAXF];
	double frame = 1e6 / 60.15, period = 1e6 / rate;
	double hand = 0, reported = 0, t_report = 1000;
	int frames = 0, old_pending = 0;
	double last = 0;
	mouse_track_reset();
	srand(1);
	for (double t = frame; t < seconds * 1e6 && frames < MAXF; t += frame) {
		/* Reports up to this frame, a little early or late. */
		while (t_report <= t) {
			hand = speed * t_report / 1e6;
			int d = (int) (hand - reported);
			reported += d;
			old_pending += d;
			double jitter = ((rand() % 1001) - 500);       /* +-0.5 ms */
			mouse_track_report((uint64_t) (t_report + jitter), d, 0, 0);
			t_report += period;
		}
		double x, y;
		int b;
		mouse_track_sample((uint64_t) t, &x, &y, &b);
		if (frames >= 0) {
			old_steps[frames] = old_pending;
			new_steps[frames] = x - last;
		}
		old_pending = 0;
		last = x;
		frames++;
	}
	/* Skip the first frames (starting) */
	*old_cv = worst(old_steps + 10, frames - 10, speed / 60.15);
	*new_cv = worst(new_steps + 10, frames - 10, speed / 60.15);
	/* The hand stops: a little later the pointer is where it is. */
	double x, y;
	int b;
	mouse_track_sample((uint64_t) (seconds * 1e6 + 100000), &x, &y, &b);
	*final_x = x;
	*total = reported;
}

int main(void)
{
	double o, n, fx, total;
	char text[200];

	/* A hand moving 600 counts a second: 9.98 counts a frame. */
	run(125, 600, 3, &o, &n, &fx, &total);
	snprintf(text, sizeof text, "125 reports a second: frame steps up to %.1f counts out "
	         "before (a whole report), %.1f now", o, n);
	check(o > 4 && n < 1.5, text);
	check(fabs(fx - total) < 1e-9, "125 a second: the pointer ends where the hand stopped");

	run(1000, 600, 3, &o, &n, &fx, &total);
	snprintf(text, sizeof text, "1000 reports a second: up to %.1f counts out before, %.1f now", o, n);
	check(n < 1.5, text);

	/* A fast flick: 3000 counts a second with a 125 Hz mouse. */
	run(125, 3000, 2, &o, &n, &fx, &total);
	snprintf(text, sizeof text, "a fast flick (3000 counts a second): up to %.1f counts out "
	         "before, %.1f now", o, n);
	/* (Each report's time is up to 0.5 ms out: at this speed 1.5 counts,
	   on top of the one count.) */
	check(o > 20 && n <= 2.6, text);
	check(fabs(fx - total) < 1e-9, "1000 a second: the pointer ends where the hand stopped");

	/* At rest, then one report: no slow creep from the last report a
	   second ago; the move takes about one interval. */
	double x, y;
	int b;
	mouse_track_reset();
	for (int i = 0; i < 20; i++)
		mouse_track_report(1000000 + i * 8000, 1, 0, 0);    /* 20 counts, then rest */
	mouse_track_report(3000000, 10, 0, 0);                  /* 2 s later: a nudge */
	mouse_track_sample(2995000, &x, &y, &b);
	int before = fabs(x - 20) < 1e-9;
	mouse_track_sample(3000000 + 20000, &x, &y, &b);
	check(before && fabs(x - 30) < 1e-9, "after a rest: no creeping, then the whole nudge");

	/* Buttons: as they were at the time read. */
	mouse_track_reset();
	mouse_track_report(1000000, 0, 0, 0);
	mouse_track_report(1008000, 0, 0, 1);
	mouse_track_sample(1008000 + 2000, &x, &y, &b);
	int early = b;
	mouse_track_sample(1008000 + 20000, &x, &y, &b);
	check(early == 0 && b == 1, "the button goes down with its report's place in time");

	/* Round the ring many times: still right. */
	mouse_track_reset();
	for (int i = 0; i < 5000; i++)
		mouse_track_report(1000 + i * 1000, 2, -1, 0);
	mouse_track_sample(1000 + 5000 * 1000 + 50000, &x, &y, &b);
	check(fabs(x - 10000) < 1e-9 && fabs(y + 5000) < 1e-9, "5000 reports: the position is right");

	mouse_track_reset();
	check(!mouse_track_sample(1000000, &x, &y, &b), "no mouse: nothing");

	printf("\n%s\n", failed ? "FAILED" : "All tests passed.");
	return failed ? 1 : 0;
}
