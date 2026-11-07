/*
 * mouse_track.c - see mouse_track.h. tools/mouse tests it.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "mouse_track.h"

#define RING 128                 /* reports kept: a second at 125 a second */
#define MIN_LAG_US 1000          /* read at least this far back */
#define MAX_LAG_US 16000         /* and no further (a slow mouse) */
#define IDLE_US 50000            /* a longer gap is the mouse at rest */

struct report {
	uint64_t t;
	int64_t x, y;                /* where the mouse is, after this report */
	int buttons;
};

static struct report ring[RING];
/* Reports written so far (the newest is ring[(head - 1) % RING]). */
static volatile uint32_t head;
static int64_t pos_x, pos_y;     /* the writer's own running position */

void mouse_track_report(uint64_t t_us, int dx, int dy, int buttons)
{
	uint32_t h = __atomic_load_n(&head, __ATOMIC_RELAXED);
	struct report *r = &ring[h % RING];
	pos_x += dx;
	pos_y += dy;
	r->t = t_us;
	r->x = pos_x;
	r->y = pos_y;
	r->buttons = buttons;
	__atomic_store_n(&head, h + 1, __ATOMIC_RELEASE);
}

void mouse_track_reset(void)
{
	__atomic_store_n(&head, 0, __ATOMIC_RELEASE);
	pos_x = pos_y = 0;
}

/* The usual time between reports: the middle of the last few gaps that
   weren't the mouse at rest. */
static uint64_t interval(const struct report *r, uint32_t n)
{
	uint64_t gaps[8];
	int count = 0, i, j;
	for (i = (int) n - 1; i > 0 && count < 8; i--) {
		uint64_t g = r[i].t - r[i - 1].t;
		if (r[i].t > r[i - 1].t && g < IDLE_US)
			gaps[count++] = g;
	}
	if (count == 0)
		return 8000;
	for (i = 1; i < count; i++)          /* sort the few there are */
		for (j = i; j > 0 && gaps[j] < gaps[j - 1]; j--) {
			uint64_t t = gaps[j];
			gaps[j] = gaps[j - 1];
			gaps[j - 1] = t;
		}
	return gaps[count / 2];
}

int mouse_track_sample(uint64_t now_us, double *x, double *y, int *buttons)
{
	/* A copy of the recent reports, oldest first, taken again if the
	   writer went round the ring meanwhile. */
	static struct report r[32];
	uint32_t n, h, h2;
	int tries;
	for (tries = 0; tries < 4; tries++) {
		uint32_t i;
		h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
		if (h == 0)
			return 0;
		n = h < 32 ? h : 32;
		for (i = 0; i < n; i++)
			r[i] = ring[(h - n + i) % RING];
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		h2 = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
		if (h2 - h < RING - 32)
			break;
	}

	uint64_t gap = interval(r, n);
	uint64_t lag = gap + 500;
	if (lag < MIN_LAG_US)
		lag = MIN_LAG_US;
	if (lag > MAX_LAG_US)
		lag = MAX_LAG_US;
	uint64_t target = now_us > lag ? now_us - lag : 0;

	/* The newest report at or before the target... */
	int i = (int) n - 1;
	while (i > 0 && r[i].t > target)
		i--;
	if (r[i].t > target || i == (int) n - 1) {
		/* All newer (then the oldest), or none newer: as it is. */
		*x = (double) r[i].x;
		*y = (double) r[i].y;
		*buttons = r[i].buttons;
		return 1;
	}
	/* ...and the next: between the two. After a rest, the movement is
	   taken to have started a usual interval before the report. */
	const struct report *a = &r[i], *b = &r[i + 1];
	uint64_t start = a->t;
	if (b->t > gap && b->t - gap > start)
		start = b->t - gap;
	double f = 0.0;
	if (target > start && b->t > start)
		f = (double) (target - start) / (double) (b->t - start);
	if (f > 1.0)
		f = 1.0;
	*x = (double) a->x + (double) (b->x - a->x) * f;
	*y = (double) a->y + (double) (b->y - a->y) * f;
	*buttons = a->buttons;
	return 1;
}
