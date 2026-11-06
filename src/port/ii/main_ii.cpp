/*
 * main_ii.cpp - Basilisk II's main program for BMMacintosh: the Mac II
 * and Quadra.
 *
 * Does what Basilisk II's Unix main_unix.cpp does, on BMMacintosh's
 * front end (src/port/frontend.c): reads the settings (macii.prefs on the
 * card, in Basilisk II's own format), puts the Mac's RAM, its ROM and its
 * frame buffer in one block of memory (direct addressing, see sysdeps.h),
 * starts the drivers (InitAll()) and the 68040.
 *
 * There are no threads. The CPU counts down emulated_ticks with each
 * instruction, and calls cpu_do_check_ticks() when it runs out: four times
 * a frame, it asks the Mac for more sound when the output wants it; once a
 * frame it raises the 60 Hz interrupt (and the 1 Hz one each second), and
 * bmmac_frame() shows the screen, paces the frame and reads the keyboard
 * and mouse. How many instructions a frame gets is adjusted to what the Pi
 * manages in a frame's time.
 *
 * Restarting the Mac (the menu, the web UI, Shut Down in the Finder)
 * throws mac_restart out of the CPU's loop to emu_main(), which starts
 * everything again.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "sysdeps.h"

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "xpram.h"
#include "timer.h"
#include "video.h"
#include "adb.h"
#include "rom_patches.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "user_strings.h"
#include "macos_util.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"

#include <sys/stat.h>

#include "../../host.h"
#include "../bmmac.h"
#include "ii.h"

/* ---- Basilisk II's globals ---------------------------------------------------- */

int CPUType;
bool CPUIs68060;
int FPUType;
bool TwentyFourBitAddressing;
uint32 InterruptFlags = 0;
uint8 *ScratchMem = NULL;
bool tick_inhibit;
char *vde_sock;

/* The instruction count down (newcpu.h, USE_CPU_EMUL_SERVICES). */
int32 emulated_ticks = 1000;

const uint32 ROM_MAX_SIZE = 0x100000;
const uint32 SCRATCH_MEM_SIZE = 0x10000;

/* The block the Mac's memory is in: RAM, ROM, scratch memory, then the
   frame buffer (video_ii.cpp). */
static uint8 *mac_memory;
uint8 *ii_frame_buffer;

/* For sysdeps.h: the block's size. */
uintptr mac_memory_size;

/* The Mac read or wrote where it has no memory (sysdeps.h): what it
   reads is 0. The first few are logged. */
uae_u32 mac_stray_access(const void *a, int write)
{
	static int logged;
	if (logged < 8) {
		logged++;
		printf("BMMacintosh: the Mac %s %08x, where it has no memory (pc %08x)\n",
		       write ? "wrote to" : "read", (unsigned) ((uintptr) a - MEMBaseDiff),
		       (unsigned) m68k_getpc());
	}
	return 0;
}

/* ---- Platform functions (main.h) ---------------------------------------------- */

void SetInterruptFlag(uint32 flag)
{
	InterruptFlags |= flag;
}

void ClearInterruptFlag(uint32 flag)
{
	InterruptFlags &= ~flag;
}

struct B2_mutex {
	int dummy;
};

B2_mutex *B2_create_mutex(void)
{
	return new B2_mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
	(void) mutex;
}

void B2_unlock_mutex(B2_mutex *mutex)
{
	(void) mutex;
}

void B2_delete_mutex(B2_mutex *mutex)
{
	delete mutex;
}

void FlushCodeCache(void *start, uint32 size)
{
	(void) start;
	(void) size;
}

void ErrorAlert(const char *text)
{
	printf("Basilisk II: error: %s\n", text);
	osd_notify(text);
}

void WarningAlert(const char *text)
{
	printf("Basilisk II: warning: %s\n", text);
	osd_notify(text);
}

bool ChoiceAlert(const char *text, const char *pos, const char *neg)
{
	printf("Basilisk II asks: %s (%s)\n", text, neg);
	(void) pos;
	return false;
}

/* Thrown out of the CPU's loop to start the Mac again. */
struct mac_restart {
	int shut_down;    /* the Mac shut itself down */
};

/* The Mac shut down (the Finder's Shut Down), or Basilisk II can't go on. */
void QuitEmulator(void)
{
	printf("Basilisk II: the Mac has shut down\n");
	throw mac_restart{1};
}

/* ---- Restarting ------------------------------------------------------------------ */

static volatile bool restart_requested;

void emu_cold_reset(void)
{
	restart_requested = true;
}

/* ---- The frame loop (cpu_do_check_ticks()) ------------------------------------ */

/* Instructions a frame: a quarter of them between calls. */
#define SUBTICKS 4
static int32 frame_budget = 400000;
static int32 max_budget = 2000000;
static int subtick;
static double cpu_started;

/* The Mac's clock (Microseconds, the Time Manager: ii_mac_time_usec()):
   a frame's time for each frame run, and within one the share of its
   instructions done. It moves smoothly while the CPU runs, which the
   kernel's clock in the PC simulator doesn't (it moves a frame at a time,
   and the Mac then saw no time pass, and divided by zero); on the Pi it
   keeps the frames' pace, 60.15 a second. */
static uint64 mac_frames_usec;
static int32 subtick_budget = 100000;

uint64 ii_mac_time_usec(void)
{
	int32 done = subtick * subtick_budget + (subtick_budget - emulated_ticks);
	int32 frame = SUBTICKS * subtick_budget;
	if (done < 0)
		done = 0;
	if (done > frame)
		done = frame;
	return mac_frames_usec + (uint64) (1000000.0 / 60.15 * done / frame);
}

static void one_tick(void)
{
	static int tick_counter;
	if (++tick_counter >= 60) {
		tick_counter = 0;
		/* The 1 Hz interrupt, and the Mac's clock (main_unix.cpp's
		   one_second()). */
		WriteMacInt32(0x20c, TimerDateTime());
		SetInterruptFlag(INTFLAG_1HZ);
		TriggerInterrupt();
	}
	if (ROMVersion != ROM_VERSION_CLASSIC || HasMacStarted()) {
		SetInterruptFlag(INTFLAG_60HZ);
		TriggerInterrupt();
	}
	ii_ether_tick();
}

/* More instructions a frame while they fit in 80% of its time, fewer when
   they don't. */
static void adjust_budget(double cpu_time)
{
	double period = 1.0 / emu_frame_rate();
	double target = period * 0.8;
	if (cpu_time <= 0.0) {
		frame_budget = max_budget;
		return;
	}
	double scaled = frame_budget * (target / cpu_time);
	if (scaled > frame_budget * 1.25)
		scaled = frame_budget * 1.25;
	if (scaled < frame_budget * 0.5)
		scaled = frame_budget * 0.5;
	frame_budget = (int32) scaled;
	if (frame_budget > max_budget)
		frame_budget = max_budget;
	if (frame_budget < 20000)
		frame_budget = 20000;
}

static void frame(void)
{
	double cpu_time = host_time() - cpu_started;
	if (!tick_inhibit)
		one_tick();
	ii_audio_frame();
	bmmac_frame();
	ii_adb_frame();
	adjust_budget(cpu_time);
	if (restart_requested) {
		restart_requested = false;
		throw mac_restart{0};
	}
	cpu_started = host_time();
}

void cpu_do_check_ticks(void)
{
	ii_audio_service();
	host_poll_tasks();
	if (++subtick >= SUBTICKS) {
		mac_frames_usec += (uint64) (1000000.0 / 60.15);
		subtick = 0;
		frame();
		subtick_budget = frame_budget / SUBTICKS;
	}
	emulated_ticks = subtick_budget;
}

/* ---- Memory and ROM -------------------------------------------------------------- */

static bool alloc_memory(void)
{
	RAMSize = PrefsFindInt32("ramsize");
	if (RAMSize <= 1000)
		RAMSize *= 1024 * 1024;
	RAMSize &= 0xfff00000;    /* whole megabytes */
	if (RAMSize < 8 * 1024 * 1024)
		RAMSize = 8 * 1024 * 1024;
	if (RAMSize > 512 * 1024 * 1024)
		RAMSize = 512 * 1024 * 1024;
	free(mac_memory);
	mac_memory_size = 0;
	size_t size = RAMSize + ROM_MAX_SIZE + SCRATCH_MEM_SIZE + II_FRAME_BUFFER_SIZE;
	mac_memory = (uint8 *) calloc(1, size + 4096);
	if (mac_memory == NULL) {
		printf("BMMacintosh: no memory for a %u MB Mac\n", (unsigned) (RAMSize >> 20));
		return false;
	}
	RAMBaseHost = (uint8 *) (((uintptr) mac_memory + 4095) & ~(uintptr) 4095);
	ROMBaseHost = RAMBaseHost + RAMSize;
	ScratchMem = ROMBaseHost + ROM_MAX_SIZE + SCRATCH_MEM_SIZE / 2;
	ii_frame_buffer = ROMBaseHost + ROM_MAX_SIZE + SCRATCH_MEM_SIZE;
	MEMBaseDiff = (uintptr) RAMBaseHost;
	mac_memory_size = size;
	RAMBaseMac = 0;
	ROMBaseMac = Host2MacAddr(ROMBaseHost);
	return true;
}

static const struct bmmac_rom *rom_in_use;

/* The ROM from macii.prefs, or the best one found. */
static bool load_rom(void)
{
	const struct bmmac_rom *rom = bmmac_rom_find(PrefsFindString("rom"));
	if (rom == NULL || rom->machine != BMMAC_II)
		rom = bmmac_rom_default(BMMAC_II);
	if (rom == NULL)
		return false;
	ROMSize = bmmac_rom_load(rom, ROMBaseHost, ROM_MAX_SIZE);
	if (ROMSize == 0)
		return false;
	rom_in_use = rom;
	if (PrefsFindString("rom") == NULL || strcmp(PrefsFindString("rom"), rom->path) != 0) {
		PrefsReplaceString("rom", rom->path);
		SavePrefs();
	}
	printf("BMMacintosh: %s, %u MB, ROM %s\n", emu_machine_text(),
	       (unsigned) (RAMSize >> 20), rom->path);
	return true;
}

/* ---- The emulator's side of the front end ----------------------------------------- */

int emu_machine(void)
{
	return BMMAC_II;
}

const char *emu_machine_text(void)
{
	static char text[96];
	const char *model = rom_in_use != NULL ? rom_in_use->name : "no ROM";
	char cpu[16];
	snprintf(cpu, sizeof cpu, "680%d0", CPUType > 0 ? CPUType : 4);
	snprintf(text, sizeof text, "%s, %s, %u MB", model, cpu, (unsigned) (RAMSize >> 20));
	return text;
}

const char *emu_credits(void)
{
	return "Emulation: Basilisk II (GPL 2 or later), by Christian Bauer\n"
	       "and others; kanjitalk755's macemu.";
}

double emu_frame_rate(void)
{
	return 60.15;
}

const char *const *emu_run_types(void)
{
	static const char *const types[] = {
		"dsk", "img", "image", "hfv", "hda", "dc42", "diskcopy", "iso", "toast", "cdr", NULL
	};
	return types;
}

/* ---- Running ---------------------------------------------------------------------- */

static bool start_mac(char *why, size_t size)
{
	if (!alloc_memory()) {
		snprintf(why, size, "Not enough memory for the Mac's RAM");
		return false;
	}
	if (!load_rom()) {
		snprintf(why, size, "No Mac II or Quadra ROM in the roms folder.\n"
		         "See roms/ROMS.txt, or switch to the Mac Plus\n"
		         "(menu, F12, Machine).");
		return false;
	}
	ii_prepare_drives();
	/* The shared folder (ExtFS) is made if it isn't there. */
	const char *extfs = PrefsFindString("extfs");
	if (extfs != NULL && extfs[0])
		mkdir(extfs, 0777);
	if (!InitAll(NULL)) {
		snprintf(why, size, "Basilisk II cannot start with this ROM.\n"
		         "Choose another one (menu, F12, ROM).");
		ExitAll();
		return false;
	}
	ADBSetRelMouseMode(true);
	return true;
}

void emu_main(void)
{
	static int argc = 1;
	static char *args[] = {(char *) "BasiliskII", NULL};
	char **argv = args;
	char why[256];

	PrefsInit(NULL, argc, argv);
	max_budget = host_option_int("ii_max_instructions", 2000000);
	if (max_budget < 100000)
		max_budget = 100000;
	frame_budget = max_budget / 4;

	for (;;) {
		if (!start_mac(why, sizeof why)) {
			printf("BMMacintosh: %s\n", why);
			bmmac_stopped(why);
			continue;
		}
		bmmac_screen_changed();
		subtick = 0;
		subtick_budget = frame_budget / SUBTICKS;
		emulated_ticks = subtick_budget;
		cpu_started = host_time();
		int shut_down = 0;
		try {
			Start680x0();
		} catch (const mac_restart &r) {
			shut_down = r.shut_down;
		}
		ii_audio_stop();
		ExitAll();
		emu_sync_disks();
		if (shut_down)
			bmmac_stopped("The Mac has shut down.\n"
			              "Press a key to start it again.");
	}
}
