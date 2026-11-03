/*
 * main_plus.c - umac's front end for BMMacintosh: the Mac Plus.
 *
 * umac (Matt Evans, the core of pico-mac) is a Mac 128K, 512K and Plus:
 * Musashi's 68000, the VIA, the SCC (for the mouse) and a disk driver of
 * its own in place of the floppy controller, which reads and writes a disk
 * image of any size. It runs the Mac Plus's ROM (the last one, 4D1F8172),
 * patched by rom.c, with 128K to 4 MB of RAM.
 *
 * Each frame (the Mac's 60.15 Hz) runs the 68000 for a frame's time at
 * 7.8336 MHz, umac_loop()'s 5 ms at a time, then raises the vertical blank
 * (and the one second interrupt each second); bmmac_frame() shows it.
 *
 * The screen is 512x342 in black and white, at the top of RAM, and the
 * sound buffer above it: 370 bytes a frame (22.25 kHz), played at the
 * volume the VIA sets (silence is whatever doesn't change: a constant is
 * taken out).
 *
 * The keyboard is the Plus's, with its keypad and arrow keys (see
 * Keyboard and mouse below): umac takes one key at a time, which is fed it every
 * other frame. The clock chip is rtc_plus.c's. The disk is read and
 * written in place on the SD card.
 * Settings (plus_rom, plus_ram, plus_disk) are in bmmac.cfg.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "umac.h"
#include "rom.h"

#include "../../host.h"
#include "../bmmac.h"
#include "plus.h"

/* umac's RAM size (machw.h's RAM_SIZE): the menu's choice. */
unsigned umac_memsize_kb = 4096;

#define PLUS_ROM_SIZE (128 * 1024)
#define SCREEN_W 512
#define SCREEN_H 342
#define CPU_HZ 7833600.0
#define FRAME_HZ 60.15

static uint8_t *rom;
static uint8_t *ram;
static FILE *disk;
static int disk_read_only;
static const struct bmmac_rom *rom_in_use;
static volatile int restart_requested;

/* ---- Settings ------------------------------------------------------------------ */

static const char *setting(const char *name)
{
	return host_option(name, "");
}

static unsigned ram_kb(void)
{
	static const unsigned sizes[] = {128, 512, 1024, 2048, 4096};
	unsigned kb = (unsigned) host_option_int("plus_ram", 4096), i;
	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		if (sizes[i] == kb)
			return kb;
	return 4096;
}

/* ---- The disk ------------------------------------------------------------------ */

/* A raw image, or a DiskCopy 4.2 one: an 84-byte header (the disk's name,
   the data's and the tags' sizes and checksums, the format) before the
   data, and the tags after it. The Mac reads and writes the data; after it
   has written, the data's checksum in the header is made right again
   (when the disk is synced), so DiskCopy and other emulators still take
   the image. */
#define DC42_HEADER 84

static long disk_start;          /* the data's place in the file */
static unsigned disk_size;       /* the data's size */
static int disk_dc42;
static int disk_written;

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static int disk_read(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
	(void) ctx;
	bmmac_disk_accessed();
	if (disk == NULL || fseek(disk, disk_start + (long) offset, SEEK_SET) != 0
	    || fread(data, 1, len, disk) != len)
		return -1;
	return 0;
}

static int disk_write(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
	(void) ctx;
	bmmac_disk_accessed();
	if (disk == NULL || disk_read_only || fseek(disk, disk_start + (long) offset, SEEK_SET) != 0
	    || fwrite(data, 1, len, disk) != len)
		return -1;
	disk_written = TRUE;
	return 0;
}

/* DiskCopy's checksum of the data: each big endian word added, and the
   sum rotated right a bit. */
static void dc42_update_checksum(void)
{
	static uint8_t buf[4096];
	uint32_t sum = 0, left = disk_size;
	uint8_t out[4];
	if (fseek(disk, disk_start, SEEK_SET) != 0)
		return;
	while (left > 0) {
		uint32_t n = left > sizeof buf ? sizeof buf : left, i;
		if (fread(buf, 1, n, disk) != n)
			return;
		for (i = 0; i + 1 < n; i += 2) {
			sum += (uint32_t) buf[i] << 8 | buf[i + 1];
			sum = sum >> 1 | sum << 31;
		}
		left -= n;
	}
	out[0] = (uint8_t) (sum >> 24);
	out[1] = (uint8_t) (sum >> 16);
	out[2] = (uint8_t) (sum >> 8);
	out[3] = (uint8_t) sum;
	if (fseek(disk, 72, SEEK_SET) == 0)
		fwrite(out, 1, 4, disk);
}

static void close_disk(void)
{
	if (disk != NULL) {
		if (disk_dc42 && disk_written)
			dc42_update_checksum();
		fflush(disk);
		fclose(disk);
	}
	disk = NULL;
	disk_written = FALSE;
}

/* Is the file (SIZE bytes, starting with HEAD) a DiskCopy 4.2 image? Its
   header's sizes add up to the file's, and its last word is 0x0100. */
static int is_dc42(const uint8_t *head, long size)
{
	uint32_t data = be32(head + 64), tags = be32(head + 68);
	return head[0] < 64 && head[82] == 1 && head[83] == 0 && data > 0
	    && (data & 0x1ff) == 0 && (long) (DC42_HEADER + data + tags) == size;
}

/* A hard disk with an Apple partition map (as MAME's and the Mac II's are):
   its first Apple_HFS partition's place and size in bytes. umac's disk
   driver gives the Mac a volume, not a disk, so the Mac Plus starts from
   that partition alone. */
static int hfs_partition(long size, long *start, unsigned *length)
{
	uint8_t block[512];
	uint32_t count, i;
	if (fseek(disk, 0, SEEK_SET) != 0 || fread(block, 1, 512, disk) != 512
	    || block[0] != 'E' || block[1] != 'R')
		return FALSE;
	for (i = 1, count = 1; i <= count && i < 64; i++) {
		if (fread(block, 1, 512, disk) != 512 || block[0] != 'P' || block[1] != 'M')
			return FALSE;
		if (i == 1)
			count = be32(block + 4);
		if (strncmp((const char *) block + 48, "Apple_HFS", 32) == 0) {
			long first = (long) be32(block + 8) * 512;
			long blocks = (long) be32(block + 12) * 512;
			if (first <= 0 || first >= size)
				return FALSE;
			if (blocks > size - first)
				blocks = size - first;
			*start = first;
			*length = (unsigned) blocks;
			return TRUE;
		}
	}
	return FALSE;
}

/* The disk image in bmmac.cfg, read and written in place: a floppy's or a
   hard disk's, raw or DiskCopy 4.2, or a partitioned hard disk's HFS
   partition. When the Mac ejects it, umac leaves
   the drive empty until the Mac restarts. */
static int open_disk(disc_descr_t *d)
{
	const char *path = setting("plus_disk");
	uint8_t head[DC42_HEADER];
	struct stat st;
	close_disk();
	if (path[0] == '\0')
		return FALSE;
	disk_read_only = FALSE;
	disk = fopen(path, "r+b");
	if (disk == NULL) {
		disk_read_only = TRUE;
		disk = fopen(path, "rb");
	}
	if (disk == NULL || stat(path, &st) != 0) {
		printf("BMMacintosh: cannot open the disk image %s\n", path);
		close_disk();
		return FALSE;
	}
	disk_start = 0;
	disk_size = (unsigned) st.st_size;
	disk_dc42 = FALSE;
	if (st.st_size > DC42_HEADER && fread(head, 1, sizeof head, disk) == sizeof head
	    && is_dc42(head, (long) st.st_size)) {
		disk_dc42 = TRUE;
		disk_start = DC42_HEADER;
		disk_size = be32(head + 64);
	} else if (hfs_partition((long) st.st_size, &disk_start, &disk_size)) {
		printf("BMMacintosh: the disk's HFS partition, %ld KB in\n", disk_start / 1024);
	}
	d->base = NULL;
	d->size = disk_size;
	d->read_only = disk_read_only;
	d->op_ctx = NULL;
	d->op_read = disk_read;
	d->op_write = disk_write;
	printf("BMMacintosh: disk %s, %u KB%s%s\n", path, disk_size / 1024,
	       disk_dc42 ? ", DiskCopy 4.2" : "", disk_read_only ? ", read only" : "");
	return TRUE;
}

/* The Mac ejected its disk (umac's disk driver calls this). umac's own
   resets the Mac (see umac.mk); here the drive stays empty until the Mac
   is restarted from the menu. */
void umac_disc_ejected(void)
{
	printf("BMMacintosh: the Mac ejected its disk\n");
}

void emu_sync_disks(void)
{
	if (disk != NULL) {
		if (disk_dc42 && disk_written) {
			disk_written = FALSE;
			dc42_update_checksum();
		}
		fflush(disk);
	}
	plus_rtc_save();
}

/* ---- Keyboard and mouse --------------------------------------------------------- */

/* The Mac Plus's keyboard (the M0110A): the Mac asks it for a key with an
   Inquiry command, and it answers with the key's transition, the Mac's key
   code shifted left with bit 0 set (bit 7 set for a release). A keypad or
   arrow key answers 0x79 instead, and the Mac then asks with an Instant
   command for the keypad's own code; the keypad's + * / and = keys also
   send a Shift press before (and a Shift release after), as on the real
   keyboard. umac answers Inquiry (one key at a time, umac_kbd_event()) but
   not Instant: plus_via_init() puts BMMacintosh's answer in front of it.

   The keypad codes are the M0110A's (as documented for TMK's converter):
   https://github.com/tmk/tmk_keyboard/wiki/Apple-M0110-Keyboard-Protocol */

#define KEY_QUEUE 64
#define KBD_INSTANT_A 0x14   /* Instant: Inside Macintosh's number... */
#define KBD_INSTANT_B 0x12   /* ...and TMK's */
#define KBD_KEYPAD 0x79
#define KBD_NULL 0x7b
#define KBD_SHIFT 0x71

struct key_event {
	uint8_t inquiry;     /* the answer to Inquiry (bit 7: a release) */
	int16_t instant;     /* the answer to the Instant that follows, or -1 */
};

static struct key_event key_queue[KEY_QUEUE];
static int key_head, key_tail;
static uint8_t key_down[128];
static int caps_locked;
static int mouse_button;
/* A keypad code waiting for the Mac's Instant, and an Instant to answer. */
static int instant_byte = -1;
static int instant_asked;

/* The keypad's codes for the Mac's (ADB) key codes; 0x100 marks the keys
   that come with Shift. */
static int keypad_code(int code)
{
	switch (code) {
	case 0x3b: return 0x0d;            /* Left */
	case 0x3c: return 0x05;            /* Right */
	case 0x3e: return 0x1b;            /* Up */
	case 0x3d: return 0x11;            /* Down */
	case 0x47: return 0x0f;            /* Clear */
	case 0x59: return 0x33;            /* 7 */
	case 0x5b: return 0x37;            /* 8 */
	case 0x5c: return 0x39;            /* 9 */
	case 0x4e: return 0x1d;            /* - */
	case 0x56: return 0x2d;            /* 4 */
	case 0x57: return 0x2f;            /* 5 */
	case 0x58: return 0x31;            /* 6 */
	case 0x53: return 0x27;            /* 1 */
	case 0x54: return 0x29;            /* 2 */
	case 0x55: return 0x2b;            /* 3 */
	case 0x4c: return 0x19;            /* Enter */
	case 0x52: return 0x25;            /* 0 */
	case 0x41: return 0x03;            /* . */
	case 0x45: return 0x100 | 0x0d;    /* + */
	case 0x43: return 0x100 | 0x05;    /* * */
	case 0x4b: return 0x100 | 0x1b;    /* / */
	case 0x51: return 0x100 | 0x11;    /* = */
	}
	return -1;
}

/* The Plus's keyboard's main keys: no Control (0x36) or Escape (0x35). */
static int main_key(int code)
{
	return code <= 0x3a && code != 0x34 && code != 0x35 && code != 0x36;
}

static void queue_key(uint8_t inquiry, int instant)
{
	int next = (key_tail + 1) % KEY_QUEUE;
	if (next == key_head)
		return;   /* full: dropped */
	key_queue[key_tail].inquiry = inquiry;
	key_queue[key_tail].instant = (int16_t) instant;
	key_tail = next;
}

#define MAC_CAPS_LOCK 0x39

void emu_key(int code, int pressed)
{
	int keypad;
	if (code < 0 || code > 127)
		return;
	/* Caps Lock locks: down while locked, as the Mac's. */
	if (code == MAC_CAPS_LOCK) {
		if (!pressed)
			return;
		caps_locked = !caps_locked;
		pressed = caps_locked;
	}
	keypad = keypad_code(code);
	if (keypad < 0 && !main_key(code))
		return;
	if (key_down[code] == (pressed != 0))
		return;
	key_down[code] = pressed != 0;
	if (keypad < 0) {
		queue_key((uint8_t) (((code << 1) | 1) | (pressed ? 0 : 0x80)), -1);
		return;
	}
	if ((keypad & 0x100) && pressed)
		queue_key(KBD_SHIFT, -1);
	queue_key(KBD_KEYPAD, (keypad & 0xff) | (pressed ? 0 : 0x80));
	if ((keypad & 0x100) && !pressed)
		queue_key(KBD_SHIFT | 0x80, -1);
}

void emu_release_keys(void)
{
	int code;
	for (code = 0; code < 128; code++)
		if (key_down[code] && code != MAC_CAPS_LOCK)
			emu_key(code, FALSE);
	mouse_button = 0;
	umac_mouse(0, 0, 0);
}

/* A key for the Mac's next Inquiry, every other frame (umac keeps one
   at a time), and none while a keypad code waits for its Instant. */
static void feed_key(void)
{
	static int wait;
	struct key_event k;
	if (key_head == key_tail || instant_byte >= 0 || ++wait < 2)
		return;
	wait = 0;
	k = key_queue[key_head];
	key_head = (key_head + 1) % KEY_QUEUE;
	umac_kbd_event(k.inquiry & 0x7f, !(k.inquiry & 0x80));
	instant_byte = k.instant;
}

/* The Mac's keyboard commands, which the VIA's shift register sends:
   Instant is answered here (after umac_loop()'s next step, as umac answers
   the others a little later, not to rush the Mac), the rest by umac. */
static void (*umac_sr_tx)(uint8_t data);

static void plus_sr_tx(uint8_t data)
{
	if (data == KBD_INSTANT_A || data == KBD_INSTANT_B) {
		instant_asked = TRUE;
		return;
	}
	umac_sr_tx(data);
}

static void answer_instant(void)
{
	if (!instant_asked)
		return;
	instant_asked = FALSE;
	via_sr_rx(instant_byte >= 0 ? (uint8_t) instant_byte : KBD_NULL);
	instant_byte = -1;
}

/* Port B: umac's mouse lines, the sound's switch (bit 7, set: off) and
   the clock chip's (rtc_plus.c), which umac leaves alone. Port A: the
   sound's volume (bits 0-2) and buffer (bit 3, set: the main one), which
   umac leaves alone too. */
static void (*umac_rb_change)(uint8_t val);
static uint8_t (*umac_rb_in)(void);
static void (*umac_ra_change)(uint8_t val);
static uint8_t sound_portb, sound_porta = 0x0f;

static void plus_rb_change(uint8_t val)
{
	sound_portb = val;
	plus_rtc_portb(val);
	if (umac_rb_change != NULL)
		umac_rb_change(val);
}

static void plus_ra_change(uint8_t val)
{
	sound_porta = val;
	if (umac_ra_change != NULL)
		umac_ra_change(val);
}

static uint8_t plus_rb_in(void)
{
	uint8_t v = umac_rb_in != NULL ? umac_rb_in() : 0;
	return (uint8_t) ((v & ~1) | (plus_rtc_data() & 1));
}

/* umac's main.c calls this for via_init() (umac.mk renames it): the
   VIA's callbacks, with the keyboard's commands and the clock chip's
   lines seen here first. */
void plus_via_init(struct via_cb *cb)
{
	struct via_cb mine = *cb;
	umac_sr_tx = cb->sr_tx;
	umac_rb_change = cb->rb_change;
	umac_rb_in = cb->rb_in;
	umac_ra_change = cb->ra_change;
	mine.ra_change = plus_ra_change;
	sound_porta = 0x0f;
	sound_portb = 0;
	mine.sr_tx = plus_sr_tx;
	mine.rb_change = plus_rb_change;
	mine.rb_in = plus_rb_in;
	instant_byte = -1;
	instant_asked = FALSE;
	via_init(&mine);
}

void emu_mouse(int dx, int dy, int buttons)
{
	mouse_button = buttons & 1;
	/* umac's Y goes up. */
	umac_mouse(dx, -dy, mouse_button);
}

/* ---- Screen and sound ------------------------------------------------------------ */

static uint8_t shadow[SCREEN_W / 8 * SCREEN_H];
static int repaint = TRUE;

void emu_screen_view(int *w, int *h, double *aspect)
{
	*w = SCREEN_W;
	*h = SCREEN_H;
	*aspect = (double) SCREEN_W / SCREEN_H;
}

int emu_screen_copy(uint16_t *fb, int pitch_px, int force)
{
	const uint8_t *screen;
	int y, x, copied = FALSE;
	if (ram == NULL)
		return FALSE;
	screen = ram + umac_get_fb_offset();
	if (repaint)
		force = TRUE;
	repaint = FALSE;
	for (y = 0; y < SCREEN_H; y++) {
		const uint8_t *src = screen + y * (SCREEN_W / 8);
		uint8_t *old = shadow + y * (SCREEN_W / 8);
		uint16_t *dst = fb + y * pitch_px;
		if (!force && memcmp(src, old, SCREEN_W / 8) == 0)
			continue;
		memcpy(old, src, SCREEN_W / 8);
		for (x = 0; x < SCREEN_W; x++)
			dst[x] = (src[x >> 3] & (0x80 >> (x & 7))) ? 0x0000 : 0xffff;
		copied = TRUE;
	}
	return copied;
}

/* The Plus's sound buffers: 370 words below the top of RAM (the main
   one) or 0x5f00 below it (the alternate one, which port A's bit 3
   chooses), the sample in each word's high byte. */
#define SOUND_SAMPLES 370
#define SOUND_OFFSET 0x300
#define SOUND_OFFSET_ALT 0x5f00

static void play_sound(void)
{
	static int16_t out[1200 * 2];
	static double dc;
	unsigned rate = bmmac_sound_rate(), n, i;
	const uint8_t *buf;
	int volume;
	if (rate == 0 || ram == NULL)
		return;
	buf = ram + umac_memsize_kb * 1024
	      - ((sound_porta & 0x08) ? SOUND_OFFSET : SOUND_OFFSET_ALT);
	/* The volume, 0-7; the switch off is silence. */
	volume = (sound_portb & 0x80) ? 0 : sound_porta & 7;
	/* A frame's 370 samples (22.25 kHz) at the output's rate. */
	n = (unsigned) (rate / FRAME_HZ + 0.5);
	if (n > 1200)
		n = 1200;
	for (i = 0; i < n; i++) {
		unsigned k = i * SOUND_SAMPLES / n;
		double v = (double) buf[k * 2] - 128.0;
		int s;
		/* What doesn't change is silence. */
		dc += (v - dc) * 0.002;
		s = (int) ((v - dc) * 160.0 * volume / 7);
		if (s > 32767)
			s = 32767;
		if (s < -32768)
			s = -32768;
		out[i * 2] = out[i * 2 + 1] = (int16_t) s;
	}
	bmmac_sound_play(out, n);
}

/* ---- The emulator's side of the front end ----------------------------------------- */

int emu_machine(void)
{
	return BMMAC_PLUS;
}

const char *emu_machine_text(void)
{
	static char text[64];
	unsigned kb = umac_memsize_kb;
	if (kb >= 1024)
		snprintf(text, sizeof text, "Macintosh Plus, %u MB", kb / 1024);
	else
		snprintf(text, sizeof text, "Macintosh Plus, %uK", kb);
	return text;
}

const char *emu_credits(void)
{
	return "Emulation: umac (MIT), by Matt Evans, with Karl Stenerud's\n"
	       "Musashi 68000 (MIT).";
}

double emu_frame_rate(void)
{
	return FRAME_HZ;
}

const char *const *emu_run_types(void)
{
	static const char *const types[] = {
		"dsk", "img", "image", "hfv", "hda", "dc42", "diskcopy", NULL
	};
	return types;
}

void emu_cold_reset(void)
{
	restart_requested = TRUE;
}

void emu_net_probe(int step)
{
	(void) step;
}

/* The web UI's "start this file": the disk, and the Mac starts again. */
void emu_autostart(const char *path)
{
	bmmac_save_setting("plus_disk", path);
	emu_cold_reset();
}

/* ---- Running ----------------------------------------------------------------------- */

static int start_mac(char *why, size_t size)
{
	disc_descr_t discs[DISC_NUM_DRIVES];
	const struct bmmac_rom *r = bmmac_rom_find(setting("plus_rom"));

	if (r == NULL || r->machine != BMMAC_PLUS)
		r = bmmac_rom_default(BMMAC_PLUS);
	if (r == NULL || bmmac_rom_load(r, rom, PLUS_ROM_SIZE) != PLUS_ROM_SIZE) {
		snprintf(why, size, "No Mac Plus ROM in the roms folder.\n"
		         "See roms/ROMS.txt%s.", BMMAC_RASPPI >= 4
		         ? ", or switch to the Mac II\n(menu, F12, Machine)" : "");
		return FALSE;
	}
	if (rom_patch(rom) != 0) {
		snprintf(why, size, "umac cannot patch this ROM.");
		return FALSE;
	}
	rom_in_use = r;
	if (strcmp(setting("plus_rom"), r->path) != 0)
		bmmac_save_setting("plus_rom", r->path);

	umac_memsize_kb = ram_kb();
	free(ram);
	ram = calloc(1, umac_memsize_kb * 1024);
	if (ram == NULL) {
		snprintf(why, size, "Not enough memory for the Mac's RAM.");
		return FALSE;
	}
	memset(discs, 0, sizeof discs);
	open_disk(&discs[0]);
	plus_rtc_load();
	umac_init(ram, rom, discs);
	repaint = TRUE;
	printf("BMMacintosh: %s, ROM %s\n", emu_machine_text(), r->path);
	return TRUE;
}

void emu_main(void)
{
	char why[256];
	rom = malloc(PLUS_ROM_SIZE);
	for (;;) {
		double emulated = 0.0, wanted = 0.0;
		int frames = 0;
		restart_requested = FALSE;
		if (rom == NULL || !start_mac(why, sizeof why)) {
			printf("BMMacintosh: %s\n", why);
			bmmac_stopped(why);
			continue;
		}
		bmmac_screen_changed();
		while (!restart_requested) {
			/* A frame's time at 7.8336 MHz, in umac's 5 ms steps. */
			wanted += 1000000.0 / FRAME_HZ;
			while (emulated < wanted) {
				int stopped = umac_loop();
				answer_instant();
				if (stopped) {
					printf("BMMacintosh: umac stopped\n");
					bmmac_stopped("The Mac has stopped (umac could not go on).\n"
					              "Press a key to start it again.");
					restart_requested = TRUE;
					break;
				}
				emulated += 5000.0;
			}
			umac_vsync_event();
			if (++frames % 60 == 0)
				umac_1hz_event();
			feed_key();
					play_sound();
			bmmac_frame();
		}
		emu_release_keys();
		emu_sync_disks();
	}
}

/* ---- The menu ------------------------------------------------------------------------- */

enum { I_DISK = 100, I_EJECT, I_ROM, I_MEMORY, I_RESTART };

static const char *const disk_exts[] = {
	"dsk", "img", "image", "hfv", "hda", "dc42", "diskcopy", NULL
};

static void rom_menu(void)
{
	const struct bmmac_rom *roms[32];
	const char *names[33];
	int n = 0, current = 0, i;
	bmmac_rom_scan();
	for (i = 0; i < bmmac_rom_count() && n < 32; i++) {
		const struct bmmac_rom *r = bmmac_rom_get(i);
		if (r->machine != BMMAC_PLUS)
			continue;
		if (r == rom_in_use || strcmp(setting("plus_rom"), r->path) == 0)
			current = n;
		roms[n] = r;
		names[n++] = r->name;
	}
	names[n] = NULL;
	if (n == 0) {
		osd_message("No Mac Plus ROM in the roms folder (see roms/ROMS.txt).", TRUE);
		return;
	}
	i = osd_choose("ROM", names, current);
	if (i < 0)
		return;
	bmmac_save_setting("plus_rom", roms[i]->path);
	emu_cold_reset();
}

static void memory_menu(void)
{
	static const char *const names[] = {
		"128K (Macintosh 128K)", "512K (Macintosh 512K)", "1 MB", "2 MB", "4 MB", NULL
	};
	static const unsigned sizes[] = {128, 512, 1024, 2048, 4096};
	unsigned kb = ram_kb();
	int current = 4, i;
	char text[8];
	for (i = 0; i < 5; i++)
		if (sizes[i] == kb)
			current = i;
	i = osd_choose("Memory", names, current);
	if (i < 0 || i == current)
		return;
	snprintf(text, sizeof text, "%u", sizes[i]);
	bmmac_save_setting("plus_ram", text);
	emu_cold_reset();
}

int emu_menu_items(struct osd_item *items, int max)
{
	static char ram_text[16];
	const char *path = setting("plus_disk");
	unsigned kb = ram_kb();
	int n = 0;
	unsigned i;
	if (kb >= 1024)
		snprintf(ram_text, sizeof ram_text, "%u MB", kb / 1024);
	else
		snprintf(ram_text, sizeof ram_text, "%uK", kb);
	{
		const struct osd_item mine[] = {
			{"Disk", path[0] ? bmmac_base_name(path) : "(none)", OSD_ITEM_SUBMENU, I_DISK},
			{"No Disk", NULL, 0, I_EJECT},
			{"ROM", rom_in_use ? bmmac_base_name(rom_in_use->path) : "(none)",
			 OSD_ITEM_SUBMENU, I_ROM},
			{"Memory", ram_text, OSD_ITEM_SUBMENU, I_MEMORY},
			{"Restart the Mac", NULL, 0, I_RESTART},
		};
		for (i = 0; i < sizeof mine / sizeof mine[0] && n < max; i++)
			items[n++] = mine[i];
	}
	return n;
}

int emu_menu_choose(int id)
{
	char path[512];
	switch (id) {
	case I_DISK:
		if (!bmmac_browse("Choose the Mac's Disk", "disks", disk_exts, path, sizeof path))
			return FALSE;
		bmmac_save_setting("plus_disk", path);
		emu_cold_reset();
		return TRUE;
	case I_EJECT:
		bmmac_save_setting("plus_disk", "");
		emu_cold_reset();
		return TRUE;
	case I_ROM:
		rom_menu();
		return restart_requested;
	case I_MEMORY:
		memory_menu();
		return restart_requested;
	case I_RESTART:
		emu_cold_reset();
		return TRUE;
	}
	return TRUE;
}
