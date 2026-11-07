/*
 * ii.h - what the parts of BMMacintosh's Basilisk II front end share.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#ifndef BMMAC_II_H_
#define BMMAC_II_H_

/* main_ii.cpp: the frame buffer, after the ROM in the Mac's memory: the
   biggest screen, 1600x1200 in millions of colours. */
#define II_FRAME_BUFFER_SIZE (1600 * 1200 * 4)
extern uint8 *ii_frame_buffer;

/* main_ii.cpp: the Mac's clock, in microseconds (timer_ii.cpp). */
uint64 ii_mac_time_usec(void);

/* audio_ii.cpp: asks the Mac for sound when the output wants more (four
   times a frame), plays silence when the Mac plays none (once a frame),
   and forgets what was asked when the Mac stops. */
void ii_audio_service(void);
void ii_audio_frame(void);
void ii_audio_stop(void);

/* menu_ii.cpp: keys pressed before the Mac started, told it once it has
   (once a frame). */
void ii_adb_frame(void);
/* The web page's ejects (menu_ii.cpp): done when Mac OS is idle (ii_idle(),
   from timer_ii.cpp's idle_wait()), or given up after a few seconds
   (ii_eject_check(), each frame). */
void ii_idle(void);
void ii_eject_check(void);

/* sys_ii.cpp: the floppy and CD-ROM drives macii.prefs doesn't have (two
   floppy drives and a CD-ROM drive, empty), before InitAll(). */
void ii_prepare_drives(void);

/* ether_ii.cpp: the Ethernet interrupt when slirp has frames for the Mac
   (each tick). */
void ii_ether_tick(void);

#endif /* BMMAC_II_H_ */
