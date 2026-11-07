/*
 * sys_ii.cpp - Basilisk II's disk access for BMMacintosh.
 *
 * The Mac's disks are image files on the SD card, read and written through
 * the C library (newlib on the Pi, on Circle's FatFs): hard disks ("disk"
 * in macii.prefs), floppy disks ("floppy") and a CD-ROM ("cdrom", an .iso
 * or .toast image). Raw images, and DiskCopy 4.2 ones (whose header is
 * skipped, FileDiskLayout()).
 *
 * There are always two floppy drives and a CD-ROM drive, which can be
 * empty ("" in macii.prefs): the menu (menu_ii.cpp) puts a disk into one
 * while the Mac runs, and the Mac ejects it. Hard disks are there from
 * the Mac's start.
 *
 * On the Pi the images are read and written with FatFs itself (sd_fs.h),
 * as newlib's lseek() takes an int, which would stop at 2 GB: FAT32 takes
 * files, and so images, up to 4 GB. The simulator uses the C library.
 *
 * From Basilisk II's Unix sys_unix.cpp.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "sysdeps.h"

#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "sys.h"
#include "sony.h"
#include "cdrom.h"

#include "../bmmac.h"
#include "ii.h"

#ifdef II_SD_FS
#include "../../sdcard/sd_fs.h"
#endif

struct bm_file {
	char name[256];
#ifdef II_SD_FS
	sd_file *sd;          /* NULL: no disk in the drive */
#else
	int fd;               /* -1: no disk in the drive */
#endif
	bool read_only;
	bool removable;       /* a floppy or CD-ROM drive */
	bool is_cdrom;
	loff_t start_byte;    /* a header's size (DiskCopy 4.2) */
	loff_t size;
	bool open;            /* not yet closed by Basilisk II */
};

#define MAX_FILES 16
static bm_file files[MAX_FILES];

/* ---- Drives --------------------------------------------------------------------- */

/* Is NAME one of the "floppy" (or "cdrom") items in macii.prefs? */
static bool in_prefs(const char *item, const char *name)
{
	const char *s;
	for (int i = 0; (s = PrefsFindString(item, i)) != NULL; i++) {
		if (*s == '*')
			s++;
		if (strcmp(s, name) == 0)
			return true;
	}
	return false;
}

/* Two floppy drives and a CD-ROM drive, empty or not. */
void ii_prepare_drives(void)
{
	int n = 0;
	while (PrefsFindString("floppy", n) != NULL)
		n++;
	for (; n < 2; n++)
		PrefsAddString("floppy", "");
	if (PrefsFindString("cdrom", 0) == NULL)
		PrefsAddString("cdrom", "");
}

void SysInit(void)
{
}

void SysExit(void)
{
}

void SysAddFloppyPrefs(void)
{
}

void SysAddDiskPrefs(void)
{
}

void SysAddCDROMPrefs(void)
{
}

void SysAddSerialPrefs(void)
{
}

/* ---- The image files ------------------------------------------------------------ */

#ifdef II_SD_FS

static bool has_disk(const bm_file *f)
{
	return f->sd != NULL;
}

static void no_disk(bm_file *f)
{
	f->sd = NULL;
}

static bool image_open(bm_file *f, const char *name, bool read_only)
{
	f->sd = sd_open(name, read_only ? SD_READ : SD_UPDATE);
	return f->sd != NULL;
}

static loff_t image_size(bm_file *f)
{
	return sd_size(f->sd);
}

static bool image_seek(bm_file *f, loff_t pos)
{
	return pos >= 0 && pos <= 0xffffffffLL && sd_seek(f->sd, (uint32_t) pos) == SD_OK;
}

static ssize_t image_read(bm_file *f, void *buffer, size_t length)
{
	unsigned got;
	return sd_read(f->sd, buffer, length, &got) == SD_OK ? (ssize_t) got : -1;
}

static ssize_t image_write(bm_file *f, const void *buffer, size_t length)
{
	return sd_write(f->sd, buffer, length) == SD_OK ? (ssize_t) length : -1;
}

static void image_sync(bm_file *f)
{
	sd_sync(f->sd);
}

static void image_close(bm_file *f)
{
	if (f->sd != NULL)
		sd_close(f->sd);
	f->sd = NULL;
}

#else

static bool has_disk(const bm_file *f)
{
	return f->fd >= 0;
}

static void no_disk(bm_file *f)
{
	f->fd = -1;
}

static bool image_open(bm_file *f, const char *name, bool read_only)
{
	f->fd = open(name, read_only ? O_RDONLY : O_RDWR);
	return f->fd >= 0;
}

static loff_t image_size(bm_file *f)
{
	return lseek(f->fd, 0, SEEK_END);
}

static bool image_seek(bm_file *f, loff_t pos)
{
	return lseek(f->fd, pos, SEEK_SET) >= 0;
}

static ssize_t image_read(bm_file *f, void *buffer, size_t length)
{
	return read(f->fd, buffer, length);
}

static ssize_t image_write(bm_file *f, const void *buffer, size_t length)
{
	return write(f->fd, buffer, length);
}

static void image_sync(bm_file *f)
{
	fsync(f->fd);
}

static void image_close(bm_file *f)
{
	if (f->fd >= 0)
		close(f->fd);
	f->fd = -1;
}

#endif

/* ---- Opening ---------------------------------------------------------------------- */

static bool open_image(bm_file *f, const char *name, bool read_only)
{
	uint8 data[256];
	snprintf(f->name, sizeof f->name, "%s", name);
	no_disk(f);
	f->size = 0;
	f->start_byte = 0;
	if (name[0] == '\0')
		return true;
	f->read_only = read_only || f->is_cdrom;
	if (!image_open(f, name, f->read_only)) {
		if (f->read_only || !image_open(f, name, true)) {
			printf("BMMacintosh: cannot open the disk image %s\n", name);
			return false;
		}
		f->read_only = true;
	}
	loff_t size = image_size(f);
	memset(data, 0, sizeof data);
	if (!image_seek(f, 0) || image_read(f, data, sizeof data) < 0)
		printf("BMMacintosh: cannot read %s\n", name);
	FileDiskLayout(size, data, f->start_byte, f->size);
	printf("BMMacintosh: disk %s, %ld KB%s\n", name, (long) (f->size / 1024),
	       f->read_only ? ", read only" : "");
	return true;
}

void *Sys_open(const char *name, bool read_only, bool is_cdrom)
{
	bm_file *f = NULL;
	for (int i = 0; i < MAX_FILES; i++) {
		if (!files[i].open) {
			f = &files[i];
			break;
		}
	}
	if (f == NULL)
		return NULL;
	memset(f, 0, sizeof *f);
	/* An empty name is an empty drive, and both empty floppy drives' and
	   an empty CD-ROM drive's are "": only the caller can tell them apart
	   (cdrom.cpp says so), or the floppy drives become CD-ROM drives. */
	f->is_cdrom = is_cdrom || (name[0] != '\0' && in_prefs("cdrom", name));
	f->removable = f->is_cdrom || in_prefs("floppy", name);
	/* An empty name is an empty floppy or CD-ROM drive. */
	if (name[0] == '\0' && !f->removable)
		return NULL;
	if (!open_image(f, name, read_only) && !f->removable)
		return NULL;
	f->open = true;
	return f;
}

void Sys_close(void *arg)
{
	bm_file *f = (bm_file *) arg;
	if (f == NULL)
		return;
	if (has_disk(f)) {
		image_sync(f);
		image_close(f);
	}
	f->open = false;
}

size_t Sys_read(void *arg, void *buffer, loff_t offset, size_t length)
{
	bm_file *f = (bm_file *) arg;
	if (f == NULL || !has_disk(f))
		return 0;
	if (!image_seek(f, offset + f->start_byte))
		return 0;
	bmmac_disk_accessed();
	ssize_t got = image_read(f, buffer, length);
	return got < 0 ? 0 : (size_t) got;
}

size_t Sys_write(void *arg, void *buffer, loff_t offset, size_t length)
{
	bm_file *f = (bm_file *) arg;
	if (f == NULL || !has_disk(f) || f->read_only)
		return 0;
	if (!image_seek(f, offset + f->start_byte))
		return 0;
	bmmac_disk_accessed();
	ssize_t put = image_write(f, buffer, length);
	return put < 0 ? 0 : (size_t) put;
}

loff_t SysGetFileSize(void *arg)
{
	bm_file *f = (bm_file *) arg;
	return f != NULL && has_disk(f) ? f->size : 0;
}

/* Replaces NAME with VALUE in the "item" list of macii.prefs. */
static void replace_pref(const char *item, const char *name, const char *value)
{
	const char *s;
	for (int i = 0; (s = PrefsFindString(item, i)) != NULL; i++) {
		if (*s == '*')
			s++;
		if (strcmp(s, name) == 0) {
			PrefsReplaceString(item, value, i);
			SavePrefs();
			return;
		}
	}
}

/* The Mac ejected the disk: the drive is empty, now and at the next
   start. */
void SysEject(void *arg)
{
	bm_file *f = (bm_file *) arg;
	if (f == NULL || !has_disk(f) || !f->removable)
		return;
	printf("BMMacintosh: ejected %s\n", f->name);
	image_sync(f);
	image_close(f);
	replace_pref(f->is_cdrom ? "cdrom" : "floppy", f->name, "");
	f->name[0] = '\0';
}

/* Formatting a disk (Mac OS's Initialize, before it writes the volume):
   an image needs nothing done, as Basilisk II's own sys_unix.cpp has it.
   The floppy driver (sony.cpp) asks this; refusing made every floppy's
   initializing fail ("Initialization failed!"). */
bool SysFormat(void *arg)
{
	bm_file *f = (bm_file *) arg;
	return f != NULL && has_disk(f) && !f->read_only;
}

bool SysIsReadOnly(void *arg)
{
	bm_file *f = (bm_file *) arg;
	return f == NULL || f->read_only;
}

bool SysIsFixedDisk(void *arg)
{
	bm_file *f = (bm_file *) arg;
	return f == NULL || !f->removable;
}

bool SysIsDiskInserted(void *arg)
{
	bm_file *f = (bm_file *) arg;
	return f != NULL && has_disk(f);
}

void SysPreventRemoval(void *arg)
{
	(void) arg;
}

void SysAllowRemoval(void *arg)
{
	(void) arg;
}

/* ---- Changing disks while the Mac runs (menu_ii.cpp) ---------------------------- */

/* Puts the image PATH into an empty floppy drive (or the CD-ROM drive).
   Returns FALSE if there is none, or the image can't be opened. */
bool ii_insert_disk(const char *path, bool cdrom)
{
	for (int i = 0; i < MAX_FILES; i++) {
		bm_file *f = &files[i];
		if (!f->open || !f->removable || f->is_cdrom != cdrom || has_disk(f))
			continue;
		char old_name[256];
		snprintf(old_name, sizeof old_name, "%s", f->name);
		if (!open_image(f, path, false) || !has_disk(f)) {
			f->name[0] = '\0';
			return false;
		}
		replace_pref(cdrom ? "cdrom" : "floppy", old_name, path);
		if (cdrom)
			CDROMMountVolume(f);
		else
			SonyMountVolume(f);
		return true;
	}
	return false;
}

/* The Mac's floppy drives (or CD-ROM drives): none while it isn't running. */
int ii_drive_count(bool cdrom)
{
	int n = 0;
	for (int i = 0; i < MAX_FILES; i++)
		if (files[i].open && files[i].removable && files[i].is_cdrom == cdrom)
			n++;
	return n;
}

/* Takes the image out of floppy drive N (0 or 1), as the floppy driver's
   eject does (SysEject(): closed, and gone from macii.prefs). For a disk
   Mac OS has no volume on: menu_ii.cpp. */
bool ii_eject_floppy_image(int n)
{
	int k = 0;
	for (int i = 0; i < MAX_FILES; i++) {
		bm_file *f = &files[i];
		if (!f->open || !f->removable || f->is_cdrom)
			continue;
		if (k++ == n) {
			if (!has_disk(f))
				return false;
			SysEject(f);
			return true;
		}
	}
	return false;
}

/* The disk in floppy drive N (0 or 1), or "". */
/* The N-th disk image open now (hard disk, floppy or CD-ROM), "" past the
   last; a drive with no disk in it is skipped. */
const char *ii_open_name(int n)
{
	int k = 0;
	for (int i = 0; i < MAX_FILES; i++) {
		bm_file *f = &files[i];
		if (!f->open || !has_disk(f))
			continue;
		if (k++ == n)
			return f->name;
	}
	return "";
}

const char *ii_floppy_name(int n)
{
	int k = 0;
	for (int i = 0; i < MAX_FILES; i++) {
		bm_file *f = &files[i];
		if (!f->open || !f->removable || f->is_cdrom)
			continue;
		if (k++ == n)
			return f->name;
	}
	return "";
}

void emu_sync_disks(void)
{
	for (int i = 0; i < MAX_FILES; i++)
		if (files[i].open && has_disk(&files[i]) && !files[i].read_only)
			image_sync(&files[i]);
}

/* ---- CD-ROM images -------------------------------------------------------------- */

/* A CD-ROM image is one data track. */
bool SysCDReadTOC(void *arg, uint8 *toc)
{
	bm_file *f = (bm_file *) arg;
	if (f == NULL || !has_disk(f))
		return false;
	uint32 blocks = (uint32) (f->size / 2048) + 150;
	uint8 *p = toc + 2;
	*p++ = 1;               /* first track */
	*p++ = 1;               /* last track */
	*p++ = 0;               /* track 1: data */
	*p++ = 0x14;
	*p++ = 1;
	*p++ = 0;
	*p++ = 0; *p++ = 0; *p++ = 2; *p++ = 0;    /* 00:02:00 */
	*p++ = 0;               /* lead out */
	*p++ = 0x14;
	*p++ = 0xaa;
	*p++ = 0;
	*p++ = 0;
	*p++ = (uint8) (blocks / (60 * 75));
	*p++ = (uint8) (blocks / 75 % 60);
	*p++ = (uint8) (blocks % 75);
	int size = (int) (p - toc);
	toc[0] = (uint8) ((size - 2) >> 8);
	toc[1] = (uint8) (size - 2);
	return true;
}

bool SysCDGetPosition(void *arg, uint8 *pos)
{
	(void) arg;
	(void) pos;
	return false;
}

bool SysCDPlay(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m,
               uint8 end_s, uint8 end_f)
{
	(void) arg; (void) start_m; (void) start_s; (void) start_f;
	(void) end_m; (void) end_s; (void) end_f;
	return false;
}

bool SysCDPause(void *arg)
{
	(void) arg;
	return false;
}

bool SysCDResume(void *arg)
{
	(void) arg;
	return false;
}

bool SysCDStop(void *arg, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f)
{
	(void) arg; (void) lead_out_m; (void) lead_out_s; (void) lead_out_f;
	return false;
}

bool SysCDScan(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse)
{
	(void) arg; (void) start_m; (void) start_s; (void) start_f; (void) reverse;
	return false;
}

void SysCDSetVolume(void *arg, uint8 left, uint8 right)
{
	(void) arg; (void) left; (void) right;
}

void SysCDGetVolume(void *arg, uint8 &left, uint8 &right)
{
	(void) arg;
	left = right = 0;
}
