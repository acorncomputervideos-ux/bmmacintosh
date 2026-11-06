/*
 * extfs_posix.h - POSIX file positions for Basilisk II's ExtFS on FatFs.
 *
 * Included first into Basilisk II's extfs.cpp (with -include: see
 * basilisk.mk), which is used unmodified, and into extfs_ii.cpp. Its
 * open(), close(), lseek() and ftruncate() calls come to extfs_ii.cpp's
 * versions, which keep POSIX's rules where FatFs (through circle-newlib)
 * breaks them:
 *
 * - lseek() past the end of a file opened for writing makes FatFs grow the
 *   file to there at once, with whatever the card held. POSIX only moves
 *   the position: the file grows when it is written there, the gap filled
 *   with zeros.
 * - ftruncate() moves back to the old position after shortening a file,
 *   and when that was past the new end, FatFs grows the file back to it.
 *
 * The Mac does both all the time (the Resource Manager and most
 * applications shorten a file they write over), so files written on the
 * SD Card disk came out the wrong length, ending in leftover data, and
 * wouldn't open again. The PC simulator's C library keeps POSIX's rules,
 * and these change nothing there.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#ifndef BMMAC_EXTFS_POSIX_H
#define BMMAC_EXTFS_POSIX_H

#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

int extfs_posix_open(const char *path, int flags, ...);
int extfs_posix_close(int fd);
off_t extfs_posix_lseek(int fd, off_t offset, int whence);
int extfs_posix_ftruncate(int fd, off_t length);
ssize_t extfs_posix_read(int fd, void *buffer, size_t length);
ssize_t extfs_posix_write(int fd, const void *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#ifndef EXTFS_POSIX_IMPLEMENTATION
#define open(...) extfs_posix_open(__VA_ARGS__)
#define close(fd) extfs_posix_close(fd)
#define lseek(fd, offset, whence) extfs_posix_lseek(fd, offset, whence)
#define ftruncate(fd, length) extfs_posix_ftruncate(fd, length)
#endif

#endif
