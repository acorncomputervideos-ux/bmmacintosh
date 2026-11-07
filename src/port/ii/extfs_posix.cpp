/*
 * extfs_posix.cpp - POSIX file positions for Basilisk II's ExtFS on FatFs.
 *
 * See extfs_posix.h. Each file ExtFS opens for writing has its size and,
 * when the Mac has moved past its end, the position there kept here:
 * FatFs is only moved inside the file, and the file only grows when the
 * Mac writes, with zeros up to where it writes. Files opened only for
 * reading are left to FatFs, which keeps their positions inside them.
 *
 * The size is kept from when the file is opened: ExtFS lets the Mac write
 * to a fork through one path at a time (fsWrPerm), so nothing else
 * changes it meanwhile. Going to the end (SEEK_END) reads it again anyway.
 *
 * tools/extfs_posix/ tests these on the PC against stand-ins with FatFs's
 * lseek() and ftruncate().
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#define EXTFS_POSIX_IMPLEMENTATION
#include "extfs_posix.h"

namespace {

// circle-newlib has 20 file descriptors (FileTable::MAX_OPEN_FILES); one
// past that is left to the C library as it is.
const int kMaxFds = 64;

struct FdState {
	bool tracked;   // opened for writing, through extfs_posix_open()
	off_t size;     // the file's size
	off_t past_end; // the position, when past the end; -1 otherwise
};

FdState fds[kMaxFds];

FdState *tracked(int fd)
{
	if (fd < 0 || fd >= kMaxFds || !fds[fd].tracked)
		return 0;
	return &fds[fd];
}

// Writes zeros from the end of the file to "to", which is past it.
int fill_to(int fd, FdState *s, off_t to)
{
	static const char zeros[4096] = {0};
	if (lseek(fd, s->size, SEEK_SET) != s->size)
		return -1;
	while (s->size < to) {
		off_t left = to - s->size;
		size_t n = left < (off_t)sizeof(zeros) ? (size_t)left : sizeof(zeros);
		ssize_t done = write(fd, zeros, n);
		if (done <= 0) {
			if (done == 0)
				errno = ENOSPC;
			return -1;
		}
		s->size += done;
	}
	return 0;
}

// Moves to "to" (from the start): FatFs only inside the file.
off_t move_to(int fd, FdState *s, off_t to)
{
	if (to <= s->size) {
		s->past_end = -1;
		return lseek(fd, to, SEEK_SET);
	}
	s->past_end = to;
	return to;
}

off_t position(int fd, FdState *s)
{
	return s->past_end >= 0 ? s->past_end : lseek(fd, 0, SEEK_CUR);
}

} // namespace

extern "C" int extfs_posix_open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	int fd = open(path, flags, mode);
	if (fd < 0 || fd >= kMaxFds)
		return fd;

	FdState &s = fds[fd];
	s.tracked = false;
	s.past_end = -1;
	if ((flags & O_ACCMODE) != O_RDONLY && !(flags & O_APPEND)) {
		off_t size = lseek(fd, 0, SEEK_END);
		if (size >= 0 && lseek(fd, 0, SEEK_SET) == 0) {
			s.size = size;
			s.tracked = true;
		}
	}
	return fd;
}

extern "C" int extfs_posix_close(int fd)
{
	if (fd >= 0 && fd < kMaxFds)
		fds[fd].tracked = false;
	return close(fd);
}

extern "C" off_t extfs_posix_lseek(int fd, off_t offset, int whence)
{
	FdState *s = tracked(fd);
	if (!s)
		return lseek(fd, offset, whence);

	off_t pos = position(fd, s);
	if (pos < 0)
		return -1;
	off_t base;
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = pos;
		break;
	case SEEK_END:
		base = lseek(fd, 0, SEEK_END);
		if (base < 0)
			return -1;
		s->size = base;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if (base + offset < 0) {
		// POSIX leaves the position where it was
		move_to(fd, s, pos);
		errno = EINVAL;
		return -1;
	}
	return move_to(fd, s, base + offset);
}

extern "C" int extfs_posix_ftruncate(int fd, off_t length)
{
	FdState *s = tracked(fd);
	if (!s)
		return ftruncate(fd, length);
	if (length < 0) {
		errno = EINVAL;
		return -1;
	}

	off_t pos = position(fd, s);
	if (pos < 0)
		return -1;
	if (length > s->size) {
		if (fill_to(fd, s, length) < 0)
			return -1;
	} else {
		// circle-newlib's ftruncate() goes back to where the file was at,
		// and FatFs grows the file to there if it is past the new end: be
		// at the new end.
		if (lseek(fd, length, SEEK_SET) != length || ftruncate(fd, length) < 0)
			return -1;
		s->size = length;
	}
	return move_to(fd, s, pos) < 0 ? -1 : 0;
}

extern "C" ssize_t extfs_posix_read(int fd, void *buffer, size_t length)
{
	FdState *s = tracked(fd);
	if (s && s->past_end >= 0)
		return 0;
	return read(fd, buffer, length);
}

extern "C" ssize_t extfs_posix_write(int fd, const void *buffer, size_t length)
{
	FdState *s = tracked(fd);
	if (!s)
		return write(fd, buffer, length);

	if (length == 0)
		return 0;  // POSIX: writing nothing doesn't grow the file
	if (s->past_end >= 0) {
		if (fill_to(fd, s, s->past_end) < 0)
			return -1;
		s->past_end = -1;
	}
	ssize_t done = write(fd, buffer, length);
	if (done > 0) {
		off_t pos = lseek(fd, 0, SEEK_CUR);
		if (pos > s->size)
			s->size = pos;
	}
	return done;
}
