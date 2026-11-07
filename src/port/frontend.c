/*
 * frontend.c - the frame loop's half that both of BMMacintosh's emulators
 * share.
 *
 * On top of the Circle kernel's services (src/host.h): the display (the
 * Mac's screen copied to it each frame, its size and aspect), pacing (each
 * frame on a display refresh when the display runs at the Mac's 60.15 Hz,
 * or close to it, which keeps scrolling smooth; a timer otherwise), the
 * sound output, the keyboard, mouse and gamepads, the disk activity light,
 * the menu, and the web UI's requests. The emulators (src/port/ii/ and
 * src/port/plus/) run the Mac a frame at a time and call bmmac_frame()
 * after each.
 *
 * From BMAtariST's platform.c, audio.c and gui_event.c.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../host.h"
#include "bmmac.h"
#include "osd.h"
#include "transfer.h"

/* ---- Settings ------------------------------------------------------------- */

const char *bmmac_sd_root(void)
{
	return host_option("root", "/");
}

void bmmac_card_path(char *path, unsigned size, const char *dir, const char *name)
{
	const char *root = bmmac_sd_root();
	size_t len = strlen(root);
	snprintf(path, size, "%s%s%s%s%s", root, len > 0 && root[len - 1] == '/' ? "" : "/",
	         dir, dir[0] ? "/" : "", name);
}

int bmmac_setting_on(const char *name, int default_value)
{
	const char *value = host_option(name, NULL);
	if (value == NULL || *value == '\0')
		return default_value;
	return strcmp(value, "1") == 0 || strcmp(value, "on") == 0
	    || strcmp(value, "yes") == 0 || strcmp(value, "true") == 0;
}

int bmmac_save_setting(const char *name, const char *value)
{
	if (host_option_set(name, value) && host_options_save())
		return TRUE;
	printf("BMMacintosh: cannot save %s= in bmmac.cfg\n", name);
	return FALSE;
}

const char *bmmac_machine_text(void)
{
	return emu_machine_text();
}

/* ---- Video ------------------------------------------------------------------- */

static uint16_t *fb_pixels;
static int fb_pitch_px;
static int osd_view;

static double refresh_rate = 60.0;
static int vsync_lock;

uint16_t *bmmac_framebuffer(int *pitch_px)
{
	*pitch_px = fb_pitch_px;
	return fb_pixels;
}

static void clear_framebuffer(void)
{
	int y;
	for (y = 0; y < BMMAC_FB_H; y++)
		memset(fb_pixels + y * fb_pitch_px, 0, BMMAC_FB_W * 2);
}

static void apply_view(void)
{
	int w, h;
	double aspect;
	if (osd_view) {
		host_video_set_view(0, 0, 640, 460, 640.0 / 460.0);
		return;
	}
	emu_screen_view(&w, &h, &aspect);
	host_video_set_view(0, 0, w, h, aspect * host_option_double("aspect", 1.0));
}

void bmmac_osd_view(int on)
{
	osd_view = on;
	if (!on)
		clear_framebuffer();
	apply_view();
	if (!on)
		emu_screen_copy(fb_pixels, fb_pitch_px, TRUE);
}

void bmmac_screen_changed(void)
{
	if (fb_pixels == NULL || osd_view)
		return;
	clear_framebuffer();
	apply_view();
	emu_screen_copy(fb_pixels, fb_pitch_px, TRUE);
}

static void measure_refresh_rate(void)
{
	enum { SKIP = 3, COUNT = 20 };
	double start = 0.0, rate;
	int i;
	for (i = 0; i < SKIP + COUNT; i++) {
		host_video_present(TRUE);
		if (i == SKIP - 1)
			start = host_time();
	}
	rate = COUNT / (host_time() - start);
	if (rate > 20.0 && rate < 250.0)
		refresh_rate = rate;
	printf("BMMacintosh: display refresh rate %.3f Hz\n", refresh_rate);
}

static void update_sync_mode(void)
{
	const char *mode = host_option("vsync", "auto");
	double rate = emu_frame_rate();
	if (strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0)
		vsync_lock = FALSE;
	else if (strcmp(mode, "on") == 0 || strcmp(mode, "1") == 0)
		vsync_lock = TRUE;
	else {
		double ratio = refresh_rate / rate;
		vsync_lock = ratio > 0.98 && ratio < 1.02;
	}
	printf("BMMacintosh: display %.2f Hz, Mac %.2f Hz, %s\n", refresh_rate, rate,
	       vsync_lock ? "synchronised to the display" : "timer paced");
}

double bmmac_refresh_rate(void)
{
	return refresh_rate;
}

int bmmac_vsync_locked(void)
{
	return vsync_lock;
}

void bmmac_video_settings_changed(void)
{
	if (fb_pixels == NULL)
		return;
	update_sync_mode();
	clear_framebuffer();
	apply_view();
}

void bmmac_display_mode_changed(void)
{
	/* Where the host can set the video mode (the Pi 4 and 5), the display
	   runs at the Mac's rate. */
	if (host_display_match_rate(emu_frame_rate()))
		measure_refresh_rate();
	update_sync_mode();
	host_sound_close();
	bmmac_sound_rate();
	apply_view();
}

/* ---- Sound ------------------------------------------------------------------- */

#define AO_RATE 48000

static unsigned out_rate;            /* 0: no sound */
static unsigned out_latency;         /* frames */
static int sound_opened;
static int16_t silence[2048 * 2];
static int volume = -1;              /* 0-100, -1 until read */

int bmmac_volume(void)
{
	if (volume < 0) {
		volume = host_option_int("volume", 100);
		if (volume < 0)
			volume = 0;
		if (volume > 100)
			volume = 100;
	}
	return volume;
}

void bmmac_set_volume(int v)
{
	char text[8];
	volume = v < 0 ? 0 : v > 100 ? 100 : v;
	snprintf(text, sizeof text, "%d", volume);
	bmmac_save_setting("volume", text);
}

/* Opens the output the first time: 48 kHz where it can be (the Pi 4 and
   5's HDMI), 44.1 kHz on the Pi Zero to 3, whose VideoCore plays at that
   rate. The latency is asked for in 44.1 kHz frames (host.h). */
unsigned bmmac_sound_rate(void)
{
	unsigned ms;
	if (sound_opened)
		return out_rate;
	sound_opened = TRUE;
	if (!bmmac_setting_on("sound", TRUE))
		return out_rate = 0;
	ms = (unsigned) host_option_int("audio_buffer_ms", 60);
	if (ms < 20)
		ms = 20;
	if (ms > 500)
		ms = 500;
	out_rate = host_sound_open(AO_RATE, 2, 44100 * ms / 1000);
	out_latency = (out_rate ? out_rate : AO_RATE) * ms / 1000;
	if (out_rate == 0)
		printf("BMMacintosh: no sound\n");
	return out_rate;
}

/* N stereo frames to the output, at the volume. */
static void write_frames(const int16_t *samples, unsigned n)
{
	static int16_t scaled[1024 * 2];
	int v = bmmac_volume();
	if (v >= 100) {
		host_sound_write(samples, n);
		return;
	}
	while (n > 0) {
		unsigned chunk = n > 1024 ? 1024 : n, i;
		for (i = 0; i < chunk * 2; i++)
			scaled[i] = (int16_t) (samples[i] * v / 100);
		host_sound_write(scaled, chunk);
		samples += chunk * 2;
		n -= chunk;
	}
}

unsigned bmmac_sound_wanted(void)
{
	unsigned free_frames, used;
	if (out_rate == 0)
		return 0;
	free_frames = host_sound_free_frames();
	used = free_frames < out_latency ? out_latency - free_frames : 0;
	return used < out_latency / 2 ? out_latency / 2 - used : 0;
}

void bmmac_feed_silence(void)
{
	unsigned frames;
	if (out_rate == 0)
		return;
	frames = host_sound_free_frames();
	if (frames > out_latency / 2) {
		frames -= out_latency / 2;
		while (frames > 0) {
			unsigned n = frames > 2048 ? 2048 : frames;
			host_sound_write(silence, n);
			frames -= n;
		}
	}
}

#define PLAY_CHUNK 512

/* N stereo frames resampled to WANT (linear interpolation, in 16.16 fixed
   point) into OUT. */
static void resample(const int16_t *in, unsigned n, int16_t *out, unsigned want)
{
	unsigned i, ch;
	for (i = 0; i < want; i++) {
		uint32_t pos = want > 1 ? (uint32_t) (((uint64_t) i * (n - 1) << 16) / (want - 1)) : 0;
		unsigned k = pos >> 16;
		int frac = (int) (pos & 0xffff);
		for (ch = 0; ch < 2; ch++) {
			int a = in[k * 2 + ch];
			int b = k + 1 < n ? in[(k + 1) * 2 + ch] : a;
			out[i * 2 + ch] = (int16_t) (a + (((b - a) * frac) >> 16));
		}
	}
}

/* Up to PLAY_CHUNK stereo frames to the output, squeezed or stretched to
   keep the buffer about half full: the Mac's 60.15 Hz locked to a 60 Hz
   display runs 0.25% fast. */
static void play_chunk(const int16_t *samples, unsigned n)
{
	static int16_t buf[(PLAY_CHUNK * 2 + 1) * 2];
	static double carry;   /* the fraction of a frame left over */
	unsigned free_frames, used, want;
	free_frames = host_sound_free_frames();
	used = free_frames < out_latency ? out_latency - free_frames : 0;
	if (n < 2) {
		want = n;
	} else if (used < out_latency / 4) {
		/* Running dry (the emulation slower than real time): stretched
		   to up to twice its length, lower in pitch, rather than gaps. */
		unsigned quarter = out_latency / 4;
		want = n + 1 + n * (quarter - used) / quarter;
		if (want > PLAY_CHUNK * 2 + 1)
			want = PLAY_CHUNK * 2 + 1;
		carry = 0.0;
	} else {
		double half = out_latency / 2.0;
		double error = (half - used) / half;   /* 1 empty, -1 full */
		double exact;
		if (error > 1.0)
			error = 1.0;
		if (error < -1.0)
			error = -1.0;
		exact = n * (1.0 + 0.02 * error) + carry;
		want = (unsigned) exact;
		carry = exact - want;
	}
	if (want != n) {
		resample(samples, n, buf, want);
		samples = buf;
		n = want;
	}
	if (n > free_frames)
		n = free_frames;          /* no room at all (far ahead): drop it */
	if (n > 0)
		write_frames(samples, n);
}

void bmmac_sound_play(const int16_t *samples, unsigned n)
{
	if (out_rate == 0)
		return;
	while (n > 0) {
		unsigned m = n > PLAY_CHUNK ? PLAY_CHUNK : n;
		play_chunk(samples, m);
		samples += m * 2;
		n -= m;
	}
}

/* ---- Disk activity --------------------------------------------------------- */

static int da_enabled = -1;
static double disk_used_at = -10.0;

int bmmac_disk_activity(void)
{
	if (da_enabled < 0)
		da_enabled = host_option_int("disk_activity", 1) != 0;
	return da_enabled;
}

void bmmac_set_disk_activity(int on)
{
	da_enabled = on != 0;
}

void bmmac_disk_accessed(void)
{
	disk_used_at = host_time();
}

/* The light: a small red square in the bottom right corner of the Mac's
   screen while a disk is used, and for a moment after. Returns TRUE if it
   drew (or has just stopped drawing). */
static int draw_disk_light(int w, int h)
{
	static int was_on;
	int on = bmmac_disk_activity() && host_time() - disk_used_at < 0.25;
	int x, y;
	if (!on && !was_on)
		return FALSE;
	was_on = on;
	if (!on)
		return TRUE;
	for (y = h - 10; y < h - 4; y++)
		for (x = w - 14; x < w - 4; x++)
			if (x >= 0 && y >= 0)
				fb_pixels[y * fb_pitch_px + x] = 0xf800;
	return TRUE;
}

/* ---- Input ------------------------------------------------------------------- */

static int menu_requested;
static int pad_last[HOST_PADS];

static void request_power_key(void);

static void keyboard(void)
{
	int usage, pressed;
	while (host_key_event(&usage, &pressed)) {
		int code;
		/* The power key: Shut Down (the Pi 400 sends it for Fn+F10). */
		if (usage == USAGE_POWER) {
			if (pressed)
				request_power_key();
			continue;
		}
		if (usage == KEY_MENU) {
			if (pressed)
				menu_requested = TRUE;
			continue;
		}
		code = bmmac_mac_keycode(usage);
		if (code >= 0)
			emu_key(code, pressed);
	}
}

/* The mouse, and the gamepads as one: the d-pad or stick moves the pointer,
   A is the button. X is Space and Y Return, for games. Home, or Select and
   Start together, open the menu. */
/* ---- The mouse ---------------------------------------------------------------

   The USB mouse is read smoothed (host_mouse_sample(), mouse_track.c), more
   than once a frame: bmmac_mouse_tick() is called four times a frame by the
   Mac II (between parts of its frame) and every 5 ms step by the Mac Plus.
   Each call gives the Mac the movement since the last, times the menu's
   Mouse Speed, with what is left over (fractions, and what didn't fit in
   one step) carried to the next:

   - The Mac II's mouse packets (Basilisk II's ADB) carry -64 to 63 counts:
     more wrapped round, and the pointer jumped the wrong way, as it did on
     any quick movement of a modern mouse (800 or more counts an inch,
     where Apple's had 100). emu_mouse_step_max() is 63 there.
   - umac moves the Mac Plus's mouse a count each 5 ms step, and dropped
     what was more than 30 counts ahead. It is given 1 a step.

   The gamepads' movement (pad_dx, pad_dy) and button go the same way. */

static double pad_dx, pad_dy;    /* to be given to the Mac */
static int pad_buttons;
static double mouse_speed = 1.0;

void bmmac_mouse_tick(void)
{
	static double last_x, last_y, carry_x, carry_y;
	static int have_last, last_buttons = -1;
	/* Not more than ten steps behind (630 counts on the Mac II, 50 ms on
	   the Mac Plus): a flick faster than the Mac can take isn't played
	   back long after the hand stopped. */
	double x, y, step = emu_mouse_step_max(), limit = step * 10.0;
	int buttons = pad_buttons, dx, dy;
	int mb;
	if (host_mouse_sample(&x, &y, &mb)) {
		if (have_last) {
			carry_x += (x - last_x) * mouse_speed;
			carry_y += (y - last_y) * mouse_speed;
		}
		last_x = x;
		last_y = y;
		have_last = TRUE;
		buttons |= mb & 1;
	}
	carry_x += pad_dx;
	carry_y += pad_dy;
	pad_dx = pad_dy = 0.0;
	if (carry_x > limit) carry_x = limit;
	if (carry_x < -limit) carry_x = -limit;
	if (carry_y > limit) carry_y = limit;
	if (carry_y < -limit) carry_y = -limit;
	dx = (int) (carry_x > step ? step : carry_x < -step ? -step : carry_x);
	dy = (int) (carry_y > step ? step : carry_y < -step ? -step : carry_y);
	carry_x -= dx;
	carry_y -= dy;
	if (dx != 0 || dy != 0 || buttons != last_buttons) {
		emu_mouse(dx, dy, buttons);
		last_buttons = buttons;
	}
}

static void mouse_and_pads(void)
{
	static double pad_fx, pad_fy;
	int buttons = 0, pad;
	mouse_speed = host_option_double("mouse_speed", 1.0);
	if (mouse_speed < 0.1 || mouse_speed > 4.0)
		mouse_speed = 1.0;
	for (pad = 0; pad < HOST_PADS; pad++) {
		int state = host_pad_state(pad), menu, ax, ay;
		double fx = 0.0, fy = 0.0;
		if (state < 0)
			state = 0;
		menu = (state & HOST_PAD_HOME)
		       || ((state & HOST_PAD_SELECT) && (state & HOST_PAD_START));
		if (menu && !(pad_last[pad] & 0x10000))
			menu_requested = TRUE;
		if (state & HOST_PAD_LEFT) fx -= 1.0;
		if (state & HOST_PAD_RIGHT) fx += 1.0;
		if (state & HOST_PAD_UP) fy -= 1.0;
		if (state & HOST_PAD_DOWN) fy += 1.0;
		ax = host_pad_axis(pad, 0);
		ay = host_pad_axis(pad, 1);
		if (ax >= 0 && ay >= 0) {
			double sx = (ax - 32768) / 32768.0, sy = (ay - 32768) / 32768.0;
			if (sx > 0.2 || sx < -0.2) fx += sx;
			if (sy > 0.2 || sy < -0.2) fy += sy;
		}
		/* Up to six pixels a frame (360 a second). */
		pad_fx += fx * 6.0;
		pad_fy += fy * 6.0;
		if (state & HOST_PAD_A)
			buttons |= 1;
		if ((state ^ pad_last[pad]) & HOST_PAD_X)
			emu_key(0x31, (state & HOST_PAD_X) != 0);    /* Space */
		if ((state ^ pad_last[pad]) & HOST_PAD_Y)
			emu_key(0x24, (state & HOST_PAD_Y) != 0);    /* Return */
		pad_last[pad] = (state & 0xffff) | (menu ? 0x10000 : 0);
	}
	pad_dx += (int) pad_fx;
	pad_dy += (int) pad_fy;
	pad_fx -= (int) pad_fx;
	pad_fy -= (int) pad_fy;
	pad_buttons = buttons;
	/* Once a frame at least (the Mac II and Plus call it more often). */
	bmmac_mouse_tick();
}

/* ---- Requests from the web UI ------------------------------------------------ */

static volatile int request_power = -1;   /* 0 reboot, 1 power off */
static volatile int request_coldstart = FALSE;
static char disk_path[600];
static int disk_op, disk_index;
static volatile int request_disk = FALSE;
static char disk_result[200];
static volatile unsigned disks_done;

void bmmac_request_power(int power_off)
{
	request_power = power_off ? 1 : 0;
}

static void request_power_key(void)
{
	request_power = 1;
}

void bmmac_request_coldstart(void)
{
	request_coldstart = TRUE;
}

/* Called by the web UI's task: the emulator takes it up between frames
   (handle_requests()). One at a time. */
int bmmac_disk_interrupt(int op, const char *card_path, int index)
{
	if (request_disk)
		return FALSE;
	disk_op = op;
	disk_index = index;
	if (card_path != NULL && card_path[0] != '\0')
		bmmac_card_path(disk_path, sizeof disk_path, "",
		                card_path[0] == '/' ? card_path + 1 : card_path);
	else
		disk_path[0] = '\0';
	__sync_synchronize();
	request_disk = TRUE;
	return TRUE;
}

const char *bmmac_disk_result(int *pending, unsigned *done)
{
	if (pending != NULL)
		*pending = request_disk;
	if (done != NULL)
		*done = disks_done;
	return disk_result;
}

void bmmac_card_relative(const char *full, char *out, unsigned size)
{
	const char *root = bmmac_sd_root();
	size_t len = strlen(root);
	while (len > 0 && root[len - 1] == '/')
		len--;
	const char *colon = strstr(full, ":/");
	if (full[0] == '\0') {
		snprintf(out, size, "%s", "");
	} else if (len > 0 && strncmp(full, root, len) == 0 && full[len] == '/') {
		snprintf(out, size, "%s", full + len);
	} else if (len == 0 && full[0] == '/') {
		snprintf(out, size, "%s", full);
	} else if (colon != NULL && strchr(full, '/') == colon + 1) {
		/* "SD:/disks/x" */
		snprintf(out, size, "%s", colon + 1);
	} else {
		snprintf(out, size, "%s", "");
	}
}

void bmmac_disk_note(const char *text)
{
	snprintf(disk_result, sizeof disk_result, "%s", text);
	printf("Web UI: %s\n", disk_result);
	disks_done++;
}

int bmmac_pending_requests(void)
{
	return request_power >= 0 || request_coldstart || request_disk;
}

static void sync_files(void)
{
	int fd;
	fflush(NULL);
	for (fd = 3; fd < 30; fd++)
		fsync(fd);
}

static void power(int off)
{
	printf("%s\n", off ? "Shutting down" : "Rebooting");
	emu_sync_disks();
	sync_files();
	if (off)
		host_power_off();
	else
		host_reboot();
}

static void handle_requests(void)
{
	if (request_disk) {
		__sync_synchronize();
		emu_disk_request(disk_op, disk_path, disk_index, disk_result,
		                 sizeof disk_result);
		printf("Web UI: %s\n", disk_result);
		disks_done++;
		__sync_synchronize();
		request_disk = FALSE;
	}
	if (request_coldstart) {
		request_coldstart = FALSE;
		printf("Web UI: restart\n");
		emu_cold_reset();
	}
	if (request_power >= 0) {
		int off = request_power;
		request_power = -1;
		power(off);
	}
}

static double last_sync_time;

void bmmac_idle(void)
{
	double now = host_time();
	bmmac_feed_silence();
	if (now - last_sync_time >= 1.0) {
		last_sync_time = now;
		emu_sync_disks();
		sync_files();
	}
	if (request_power >= 0)
		handle_requests();
}

/* ---- File transfers: the Mac paused, a window on the screen -------------------

   While the network drive or the web page moves a big file, the Mac stops
   and a window shows the transfer: the network then has the Pi's core to
   itself, where it shared it with the Mac (and Wi-Fi above all crawled).
   Small ones (the Finder's many little requests) leave the Mac running. The
   Mac starts again 2 seconds after the last transfer, so a folder of files
   is one pause. Esc lets it run meanwhile (slower), until the transfers
   stop; the menu's Network can turn the pausing off. Each pause's total and
   speed go in the log. */

#define TRANSFER_PAUSE_BYTES (256 * 1024)
#define TRANSFER_LINGER 2.0

static int transfer_skip;            /* Esc: not until the transfers stop */
static double transfer_seen;         /* when a transfer was last running */

static void format_size(char *out, size_t size, double bytes)
{
	if (bytes < 1024.0 * 1024.0)
		snprintf(out, size, "%.0f KB", bytes / 1024.0);
	else
		snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024.0));
}

static void draw_transfer(const struct transfer_info *t, double rate, double elapsed)
{
	char text[900], done[24], total[24], speed[24], session[24], bar[40], left[48];
	const char *name = t->name[0] == '/' ? t->name + 1 : t->name;
	int n = 0;
	format_size(done, sizeof done, (double) t->done);
	format_size(speed, sizeof speed, rate);
	format_size(session, sizeof session, (double) (t->bytes + (t->active ? t->done : 0)));
	if (t->active) {
		if (t->total > 0) {
			int fill = (int) (30.0 * (double) t->done / (double) t->total), i;
			if (fill > 30)
				fill = 30;
			bar[0] = '[';
			for (i = 0; i < 30; i++)
				bar[1 + i] = i < fill ? '#' : '.';
			bar[31] = ']';
			bar[32] = '\0';
			format_size(total, sizeof total, (double) t->total);
			if (rate > 512.0) {
				double secs = (double) (t->total - t->done) / rate;
				if (secs < 90)
					snprintf(left, sizeof left, ", about %.0f seconds left", secs < 1 ? 1 : secs);
				else
					snprintf(left, sizeof left, ", about %.0f minutes left", secs / 60);
			} else {
				left[0] = '\0';
			}
			n = snprintf(text, sizeof text,
			             "%s\n  %.60s\n\n  %s  %s of %s\n  %s a second%s\n",
			             t->upload ? "Receiving from your computer:" : "Sending to your computer:",
			             name, bar, done, total, speed, left);
		} else {
			n = snprintf(text, sizeof text,
			             "%s\n  %.60s\n\n  %s so far, %s a second\n",
			             t->upload ? "Receiving from your computer:" : "Sending to your computer:",
			             name, done, speed);
		}
	} else {
		n = snprintf(text, sizeof text, "Waiting for more files...\n\n\n\n");
	}
	if (n < 0 || (size_t) n >= sizeof text)
		n = (int) strlen(text);
	snprintf(text + n, sizeof text - n,
	         "\nSo far: %u file%s finished, %s moved in %.0f seconds%s.\n\n"
	         "The Mac is paused, so the transfer has the Pi to itself.\n"
	         "It carries on 2 seconds after the last file.",
	         t->files, t->files == 1 ? "" : "s", session, elapsed,
	         t->failed ? " (some broke off)" : "");
	osd_show_text("File Transfer", text, "Esc: use the Mac meanwhile (slower)");
}

/* Pauses the Mac while a big transfer runs (called each frame, and while
   the Mac is stopped). TRUE if it paused, and the screen needs drawing
   again. */
static int transfer_pause(void)
{
	struct transfer_info t;
	double now = host_time();
	transfer_get(&t);
	if (t.active)
		transfer_seen = now;
	else if (transfer_skip && now - transfer_seen > TRANSFER_LINGER)
		transfer_skip = FALSE;
	if (!t.active || transfer_skip || !bmmac_setting_on("transfer_pause", TRUE)
	    || (t.total < 512 * 1024 && t.done < TRANSFER_PAUSE_BYTES))
		return FALSE;

	int was_open = osd_is_open();
	double start = now, last_draw = 0.0, rate = 0.0, rate_time = now, idle_since = 0.0;
	long long base = t.done, rate_bytes = 0, moved = 0;
	emu_release_keys();
	transfer_session_reset();
	if (!was_open)
		osd_open();
	printf("BMMacintosh: the Mac paused for a file transfer\n");
	for (;;) {
		transfer_get(&t);
		now = host_time();
		/* Bytes moved since the pause began. */
		moved = t.bytes + (t.active ? t.done : 0) - base;
		if (now - rate_time >= 0.5) {
			double r = (double) (moved - rate_bytes) / (now - rate_time);
			rate = rate == 0.0 ? r : rate * 0.6 + r * 0.4;
			rate_time = now;
			rate_bytes = moved;
		}
		if (t.active) {
			transfer_seen = now;
			idle_since = 0.0;
		} else if (idle_since == 0.0) {
			idle_since = now;
		}
		if (!t.active && now - idle_since >= TRANSFER_LINGER)
			break;
		if (now - last_draw >= 0.25) {
			draw_transfer(&t, rate, now - start);
			last_draw = now;
		}
		/* Waits for the display's next refresh: Circle's tasks (the
		   network, Wi-Fi, the web UI) run all the while. */
		if (osd_poll_key(FALSE) == OSD_KEY_BACK) {
			transfer_skip = TRUE;
			break;
		}
		bmmac_idle();
	}
	{
		double secs = host_time() - start - (idle_since > 0.0 ? host_time() - idle_since : 0.0);
		char size[24];
		format_size(size, sizeof size, (double) moved);
		printf("BMMacintosh: file transfer: %u file%s, %s in %.1f s (%.0f KB/s)%s\n",
		       t.files, t.files == 1 ? "" : "s", size, secs,
		       secs > 0.0 ? (double) moved / 1024.0 / secs : 0.0,
		       transfer_skip ? ", the Mac running again (Esc)" : "");
	}
	if (!was_open)
		osd_close();
	return TRUE;
}

/* ---- The frame --------------------------------------------------------------- */

static int speed_percent = 100;
static double frame_due;
static double last_second;
static long frames, frames_at_second;

int bmmac_speed_percent(void)
{
	return speed_percent;
}

static void show_network_address(void)
{
	static int shown;
	char address[32];
	if (shown || !bmmac_setting_on("webui", TRUE))
		return;
	if (host_network_address(address, sizeof(address))) {
		char text[64];
		snprintf(text, sizeof(text), "Web UI: http://%s/", address);
		osd_notify(text);
		shown = TRUE;
	}
}

void bmmac_frame(void)
{
	double now;

	if (transfer_pause())
		frame_due = 0.0;     /* the Mac's pace starts afresh */

	if (!osd_view) {
		/* What's drawn over the Mac's screen (a notification, the disk
		   light) is drawn again over a fresh copy of it each frame. */
		static int overlay_drawn;
		int w, h;
		double aspect;
		emu_screen_copy(fb_pixels, fb_pitch_px, overlay_drawn);
		emu_screen_view(&w, &h, &aspect);
		overlay_drawn = osd_draw_notification(fb_pixels, fb_pitch_px, w, h);
		overlay_drawn |= draw_disk_light(w, h);
	}
	host_video_present(vsync_lock);
	if (!vsync_lock) {
		/* Paced by the clock instead. */
		double period = 1.0 / emu_frame_rate();
		now = host_time();
		if (frame_due == 0.0 || now - frame_due > 0.25)
			frame_due = now;
		frame_due += period;
		if (frame_due > now)
			host_sleep_us((unsigned) ((frame_due - now) * 1000000.0));
	}
	frames++;

	keyboard();
	mouse_and_pads();
	if (menu_requested) {
		menu_requested = FALSE;
		emu_release_keys();
		bmmac_menu();
		apply_view();
		emu_screen_copy(fb_pixels, fb_pitch_px, TRUE);
		frame_due = 0.0;
		last_second = host_time();
		frames_at_second = frames;
	}

	now = host_time();
	if (now - last_second >= 1.0) {
		speed_percent = (int) ((frames - frames_at_second) / (now - last_second)
		                       / emu_frame_rate() * 100.0 + 0.5);
		last_second = now;
		frames_at_second = frames;
		emu_sync_disks();
		sync_files();
		show_network_address();
	}
	handle_requests();
}

void bmmac_stopped(const char *why)
{
	char text[400];
	snprintf(text, sizeof text, "%s\n\nF12: the menu.", why);
	for (;;) {
		int usage, pressed, key = -1;
		osd_open();
		osd_message(text, FALSE);
		while (key < 0) {
			bmmac_idle();
			if (transfer_pause())
				osd_message(text, FALSE);
			host_video_present(TRUE);
			if (request_coldstart || request_disk) {
				handle_requests();
				osd_close();
				return;
			}
			while (host_key_event(&usage, &pressed))
				if (pressed)
					key = usage;
		}
		if (key == USAGE_POWER) {
			bmmac_request_power(1);
			bmmac_idle();
		}
		if (key != KEY_MENU) {
			osd_close();
			return;
		}
		/* The menu, and then try again. */
		bmmac_menu();
		osd_close();
		return;
	}
}

/* ---- Starting ---------------------------------------------------------------- */

void bmmac_main(void)
{
	int pitch;
	if (host_video_init(BMMAC_FB_W, BMMAC_FB_H, &fb_pixels, &pitch) != 0) {
		printf("BMMacintosh: no display\n");
		for (;;)
			host_sleep_us(1000000);
	}
	fb_pitch_px = pitch / 2;
	clear_framebuffer();
	apply_view();
	host_display_match_rate(emu_frame_rate());
	measure_refresh_rate();
	update_sync_mode();
	bmmac_sound_rate();
	bmmac_rom_scan();
	last_second = host_time();
	emu_main();
}
