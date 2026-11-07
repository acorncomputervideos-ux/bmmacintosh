/*
 *  extfs_ii.cpp - Basilisk II's ExtFS on the SD card, for BMMacintosh
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 *  BMMacintosh: Basilisk II's Unix extfs_unix.cpp, changed for the SD
 *  card's FAT file system (Circle's FatFs, through newlib):
 *
 *  - FAT names can't have the characters * ? " < > | \ : nor end in a dot
 *    or a space, and the Mac's other characters (MacRoman, above 0x7f)
 *    aren't ASCII: those are written %XX in the card's names (and % itself
 *    %25), and read back.
 *  - FatFs removes a folder only when it is empty, without telling why it
 *    can't: a folder's helper folders (.finf and .rsrc) are removed first.
 *  - newlib has no utime(): src/port/posix_extra.c's (which does nothing).
 *  - FatFs grows a file when the Mac moves past its end, and ftruncate()
 *    grows it back: extfs_posix.h's calls keep POSIX's rules, here and in
 *    extfs.cpp.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <utime.h>
#include <strings.h>

// newlib has struct utimbuf but not utime() (BMMacintosh's posix_extra.c)
extern "C" int utime(const char *path, const struct utimbuf *times);

#include "sysdeps.h"
#include "macos_util.h"
#include "extfs.h"
#include "extfs_defs.h"

#define DEBUG 0
#include "debug.h"

// After the headers: open(), close(), lseek() and ftruncate() below are
// extfs_posix.cpp's.
#include "extfs_posix.h"


// Default Finder flags
const uint16 DEFAULT_FINDER_FLAGS = kHasBeenInited;


/*
 *  Initialization
 */

void extfs_init(void)
{
}


/*
 *  Deinitialization
 */

void extfs_exit(void)
{
}


/*
 *  Add component to path name
 */

void add_path_component(char *path, const char *component)
{
	int l = strlen(path);
	if (l < MAX_PATH_LENGTH-1 && path[l-1] != '/') {
		path[l] = '/';
		path[l+1] = 0;
	}
	strncat(path, component, MAX_PATH_LENGTH-1);
}


/*
 *  Finder info and resource forks are kept in helper files
 *
 *  Finder info:
 *    /path/.finf/file
 *  Resource fork:
 *    /path/.rsrc/file
 *
 *  The .finf files store a FInfo/DInfo, followed by a FXInfo/DXInfo
 *  (16+16 bytes)
 */

static void make_helper_path(const char *src, char *dest, const char *add, bool only_dir = false)
{
	dest[0] = 0;

	// Get pointer to last component of path
	const char *last_part = strrchr(src, '/');
	if (last_part)
		last_part++;
	else
		last_part = src;

	// Copy everything before
	strncpy(dest, src, last_part-src);
	dest[last_part-src] = 0;

	// Add additional component
	strncat(dest, add, MAX_PATH_LENGTH-1);

	// Add last component
	if (!only_dir)
		strncat(dest, last_part, MAX_PATH_LENGTH-1);
}

static int create_helper_dir(const char *path, const char *add)
{
	char helper_dir[MAX_PATH_LENGTH];
	make_helper_path(path, helper_dir, add, true);
	if (helper_dir[strlen(helper_dir) - 1] == '/')	// Remove trailing "/"
		helper_dir[strlen(helper_dir) - 1] = 0;
	return mkdir(helper_dir, 0777);
}

static int open_helper(const char *path, const char *add, int flag)
{
	char helper_path[MAX_PATH_LENGTH];
	make_helper_path(path, helper_path, add);

	if ((flag & O_ACCMODE) == O_RDWR || (flag & O_ACCMODE) == O_WRONLY)
		flag |= O_CREAT;
	int fd = open(helper_path, flag, 0666);
	if (fd < 0) {
		if (errno == ENOENT && (flag & O_CREAT)) {
			// One path component was missing, probably the helper
			// directory. Try to create it and re-open the file.
			int ret = create_helper_dir(path, add);
			if (ret < 0)
				return ret;
			fd = open(helper_path, flag, 0666);
		}
	}
	return fd;
}

/*
 *  AppleDouble files ("._name"), which macOS writes next to a file it
 *  copies to a FAT card: the file's Finder info (type and creator) and
 *  resource fork. ExtFS doesn't show files starting with a dot, and
 *  without these an application or a document copied from a Mac came
 *  without its type and resources and wouldn't open. The first time the
 *  Mac looks at a file that has one and no .finf or .rsrc of its own,
 *  they are made from it; after that the Mac's own are used (the "._"
 *  file is left as it is).
 */

static uint32 be32(const uint8 *p)
{
	return (uint32)p[0] << 24 | (uint32)p[1] << 16 | (uint32)p[2] << 8 | p[3];
}

static bool helper_exists(const char *path, const char *add)
{
	char helper_path[MAX_PATH_LENGTH];
	make_helper_path(path, helper_path, add);
	struct stat st;
	return stat(helper_path, &st) == 0;
}

// Copies "length" bytes at "offset" of fd to a new helper file of path's.
static bool copy_to_helper(int fd, uint32 offset, uint32 length, const char *path, const char *add)
{
	if (lseek(fd, offset, SEEK_SET) != (off_t)offset)
		return false;
	int out = open_helper(path, add, O_WRONLY | O_CREAT | O_TRUNC);
	if (out < 0)
		return false;
	static uint8 buf[8192];
	bool ok = true;
	while (ok && length > 0) {
		size_t n = length < sizeof(buf) ? length : sizeof(buf);
		ssize_t got = read(fd, buf, n);
		ok = got == (ssize_t)n && write(out, buf, n) == (ssize_t)n;
		length -= n;
	}
	close(out);
	if (!ok) {
		char helper_path[MAX_PATH_LENGTH];
		make_helper_path(path, helper_path, add);
		remove(helper_path);
	}
	return ok;
}

static void import_appledouble(const char *path)
{
	bool need_finf = !helper_exists(path, ".finf/");
	bool need_rsrc = !helper_exists(path, ".rsrc/");
	if (!need_finf && !need_rsrc)
		return;

	char ad_path[MAX_PATH_LENGTH];
	make_helper_path(path, ad_path, "._");
	int fd = open(ad_path, O_RDONLY);
	if (fd < 0)
		return;

	// The header: magic, version, 16 bytes, and the number of entries,
	// each an ID, an offset and a length
	uint8 header[26];
	if (read(fd, header, sizeof(header)) == (ssize_t)sizeof(header)
	    && be32(header) == 0x00051607
	    && (be32(header + 4) == 0x00020000 || be32(header + 4) == 0x00010000)) {
		struct stat st;
		off_t size = stat(ad_path, &st) == 0 ? st.st_size : 0;
		int entries = header[24] << 8 | header[25];
		for (int i = 0; i < entries && i < 32; i++) {
			uint8 e[12];
			if (read(fd, e, sizeof(e)) != (ssize_t)sizeof(e))
				break;
			uint32 id = be32(e), offset = be32(e + 4), length = be32(e + 8);
			if ((off_t)offset + length > size)
				continue;
			off_t next = lseek(fd, 0, SEEK_CUR);
			uint8 tc[8];
			if (id == 9 && need_finf && length >= SIZEOF_FInfo
			    && lseek(fd, offset, SEEK_SET) == (off_t)offset
			    && read(fd, tc, 8) == 8
			    && (be32(tc) != 0 || be32(tc + 4) != 0)) {
				// FInfo and FXInfo (macOS adds its extended attributes
				// after them). Not when the type and creator are empty,
				// as modern macOS mostly leaves them (its ._ files hold a
				// download's quarantine and the like): the name's
				// extension says more then (get_finfo()).
				uint32 n = length >= SIZEOF_FInfo + SIZEOF_FXInfo ? SIZEOF_FInfo + SIZEOF_FXInfo : SIZEOF_FInfo;
				copy_to_helper(fd, offset, n, path, ".finf/");
			} else if (id == 2 && need_rsrc && length > 0)
				copy_to_helper(fd, offset, length, path, ".rsrc/");
			if (lseek(fd, next, SEEK_SET) != next)
				break;
		}
	}
	close(fd);
}

/* The type and creator (and the rest of the FInfo and FXInfo) in a
   file's ._ file, if it has a type or creator: false if not, or no ._
   file. */
static bool appledouble_finfo(const char *path, uint8 *out, unsigned size)
{
	char ad_path[MAX_PATH_LENGTH];
	make_helper_path(path, ad_path, "._");
	int fd = open(ad_path, O_RDONLY);
	if (fd < 0)
		return false;
	bool found = false;
	uint8 header[26];
	if (read(fd, header, sizeof(header)) == (ssize_t)sizeof(header)
	    && be32(header) == 0x00051607) {
		int entries = header[24] << 8 | header[25];
		for (int i = 0; i < entries && i < 32 && !found; i++) {
			uint8 e[12];
			if (read(fd, e, sizeof(e)) != (ssize_t)sizeof(e))
				break;
			if (be32(e) != 9 || be32(e + 8) < 8)
				continue;
			unsigned n = be32(e + 8) < size ? be32(e + 8) : size;
			memset(out, 0, size);
			found = lseek(fd, be32(e + 4), SEEK_SET) == (off_t)be32(e + 4)
			     && read(fd, out, n) == (ssize_t)n
			     && (be32(out) != 0 || be32(out + 4) != 0);
		}
	}
	close(fd);
	return found;
}

static int open_finf(const char *path, int flag)
{
	return open_helper(path, ".finf/", flag);
}

static int open_rsrc(const char *path, int flag)
{
	// Most files have no resource fork: look for an AppleDouble file only
	// when there is no .rsrc (and before one is made for writing)
	if ((flag & O_ACCMODE) == O_RDONLY) {
		int fd = open_helper(path, ".rsrc/", flag);
		if (fd >= 0 || errno != ENOENT)
			return fd;
	}
	import_appledouble(path);
	return open_helper(path, ".rsrc/", flag);
}


/*
 *  Get/set finder info for file/directory specified by full path
 */

struct ext2type {
	const char *ext;
	uint32 type;
	uint32 creator;
};

static const ext2type e2t_translation[] = {
	{".Z", FOURCC('Z','I','V','M'), FOURCC('L','Z','I','V')},
	{".gz", FOURCC('G','z','i','p'), FOURCC('G','z','i','p')},
	{".hqx", FOURCC('T','E','X','T'), FOURCC('S','I','T','x')},
	{".bin", FOURCC('T','E','X','T'), FOURCC('S','I','T','x')},
	{".pdf", FOURCC('P','D','F',' '), FOURCC('C','A','R','O')},
	{".ps", FOURCC('T','E','X','T'), FOURCC('t','t','x','t')},
	{".sit", FOURCC('S','I','T','!'), FOURCC('S','I','T','x')},
	{".sitx", FOURCC('S','I','T','D'), FOURCC('S','I','T','!')},
	{".cpt", FOURCC('P','A','C','T'), FOURCC('C','P','C','T')},
	{".image", FOURCC('d','I','m','g'), FOURCC('d','C','p','y')},
	{".tar", FOURCC('T','A','R','F'), FOURCC('T','A','R',' ')},
	{".uu", FOURCC('T','E','X','T'), FOURCC('S','I','T','x')},
	{".uue", FOURCC('T','E','X','T'), FOURCC('S','I','T','x')},
	{".zip", FOURCC('Z','I','P',' '), FOURCC('Z','I','P',' ')},
	{".8svx", FOURCC('8','S','V','X'), FOURCC('S','N','D','M')},
	{".aifc", FOURCC('A','I','F','C'), FOURCC('T','V','O','D')},
	{".aiff", FOURCC('A','I','F','F'), FOURCC('T','V','O','D')},
	{".au", FOURCC('U','L','A','W'), FOURCC('T','V','O','D')},
	{".mid", FOURCC('M','I','D','I'), FOURCC('T','V','O','D')},
	{".midi", FOURCC('M','I','D','I'), FOURCC('T','V','O','D')},
	{".mp2", FOURCC('M','P','G',' '), FOURCC('T','V','O','D')},
	{".mp3", FOURCC('M','P','G',' '), FOURCC('T','V','O','D')},
	{".wav", FOURCC('W','A','V','E'), FOURCC('T','V','O','D')},
	{".bmp", FOURCC('B','M','P','f'), FOURCC('o','g','l','e')},
	{".gif", FOURCC('G','I','F','f'), FOURCC('o','g','l','e')},
	{".lbm", FOURCC('I','L','B','M'), FOURCC('G','K','O','N')},
	{".ilbm", FOURCC('I','L','B','M'), FOURCC('G','K','O','N')},
	{".jpg", FOURCC('J','P','E','G'), FOURCC('o','g','l','e')},
	{".jpeg", FOURCC('J','P','E','G'), FOURCC('o','g','l','e')},
	{".pict", FOURCC('P','I','C','T'), FOURCC('o','g','l','e')},
	{".png", FOURCC('P','N','G','f'), FOURCC('o','g','l','e')},
	{".sgi", FOURCC('.','S','G','I'), FOURCC('o','g','l','e')},
	{".tga", FOURCC('T','P','I','C'), FOURCC('o','g','l','e')},
	{".tif", FOURCC('T','I','F','F'), FOURCC('o','g','l','e')},
	{".tiff", FOURCC('T','I','F','F'), FOURCC('o','g','l','e')},
	{".htm", FOURCC('T','E','X','T'), FOURCC('M','O','S','S')},
	{".html", FOURCC('T','E','X','T'), FOURCC('M','O','S','S')},
	{".txt", FOURCC('T','E','X','T'), FOURCC('t','t','x','t')},
	{".rtf", FOURCC('T','E','X','T'), FOURCC('M','S','W','D')},
	{".c", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".C", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".cc", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".cpp", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".cxx", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".h", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".hh", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".hpp", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".hxx", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".s", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".S", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".i", FOURCC('T','E','X','T'), FOURCC('R','*','c','h')},
	{".mpg", FOURCC('M','P','E','G'), FOURCC('T','V','O','D')},
	{".mpeg", FOURCC('M','P','E','G'), FOURCC('T','V','O','D')},
	{".mov", FOURCC('M','o','o','V'), FOURCC('T','V','O','D')},
	{".fli", FOURCC('F','L','I',' '), FOURCC('T','V','O','D')},
	{".avi", FOURCC('V','f','W',' '), FOURCC('T','V','O','D')},
	{".qxd", FOURCC('X','D','O','C'), FOURCC('X','P','R','3')},
	{".hfv", FOURCC('D','D','i','m'), FOURCC('d','d','s','k')},
	{".dsk", FOURCC('D','D','i','m'), FOURCC('d','d','s','k')},
	{".img", FOURCC('r','o','h','d'), FOURCC('d','d','s','k')},
	{NULL, 0, 0}	// End marker
};

void get_finfo(const char *path, uint32 finfo, uint32 fxinfo, bool is_dir)
{
	// Set default finder info
	Mac_memset(finfo, 0, SIZEOF_FInfo);
	if (fxinfo)
		Mac_memset(fxinfo, 0, SIZEOF_FXInfo);
	WriteMacInt16(finfo + fdFlags, DEFAULT_FINDER_FLAGS);
	WriteMacInt32(finfo + fdLocation, (uint32)-1);

	// Read Finder info file (made from an AppleDouble file the first time)
	int fd = open_finf(path, O_RDONLY);
	if (fd < 0) {
		import_appledouble(path);
		fd = open_finf(path, O_RDONLY);
	}
	if (fd >= 0) {
		ssize_t actual = read(fd, Mac2HostAddr(finfo), SIZEOF_FInfo);
		if (fxinfo)
			actual += read(fd, Mac2HostAddr(fxinfo), SIZEOF_FXInfo);
		close(fd);
		// A file's Finder info without a type or creator: from a ._ file
		// of modern macOS's (before import_appledouble() passed those by),
		// or the Finder's, written when the file appeared, before its ._
		// file had come over the network. The ._ file's if it has them
		// (kept from now on), its name's extension's otherwise.
		if (actual >= SIZEOF_FInfo && (is_dir || ReadMacInt32(finfo + fdType) != 0
		                               || ReadMacInt32(finfo + fdCreator) != 0))
			return;
		uint8 ad[SIZEOF_FInfo + SIZEOF_FXInfo];
		if (!is_dir && appledouble_finfo(path, ad, sizeof(ad))) {
			// Its type and creator, the Finder's flags and place kept.
			Host2Mac_memcpy(finfo + fdType, ad + fdType, 8);
			int wfd = open_finf(path, O_RDWR);
			if (wfd >= 0) {
				write(wfd, Mac2HostAddr(finfo), SIZEOF_FInfo);
				if (fxinfo)
					write(wfd, Mac2HostAddr(fxinfo), SIZEOF_FXInfo);
				close(wfd);
			}
			return;
		}
	}

	// No Finder info file (or no type or creator in it), translate file name
	// extension to MacOS type/creator, in any case (GAME.SIT, game.sit)
	if (!is_dir) {
		int path_len = strlen(path);
		for (int i=0; e2t_translation[i].ext; i++) {
			int ext_len = strlen(e2t_translation[i].ext);
			if (path_len < ext_len)
				continue;
			if (!strcasecmp(path + path_len - ext_len, e2t_translation[i].ext)) {
				WriteMacInt32(finfo + fdType, e2t_translation[i].type);
				WriteMacInt32(finfo + fdCreator, e2t_translation[i].creator);
				break;
			}
		}
	}
}

void set_finfo(const char *path, uint32 finfo, uint32 fxinfo, bool is_dir)
{
	struct utimbuf times;
	times.actime = MacTimeToTime(ReadMacInt32(finfo - ioFlFndrInfo + ioFlCrDat));
	times.modtime = MacTimeToTime(ReadMacInt32(finfo - ioFlFndrInfo + ioFlMdDat));

	if (utime(path, &times) < 0) {
		D(bug("utime failed on %s\n", path));
	}

	// Open Finder info file
	int fd = open_finf(path, O_RDWR);
	if (fd < 0)
		return;

	// Write file
	write(fd, Mac2HostAddr(finfo), SIZEOF_FInfo);
	if (fxinfo)
		write(fd, Mac2HostAddr(fxinfo), SIZEOF_FXInfo);
	close(fd);
}


/*
 *  Resource fork emulation functions
 */

uint32 get_rfork_size(const char *path)
{
	// Open resource file
	int fd = open_rsrc(path, O_RDONLY);
	if (fd < 0)
		return 0;

	// Get size
	off_t size = lseek(fd, 0, SEEK_END);
	
	// Close file and return size
	close(fd);
	return size < 0 ? 0 : size;
}

int open_rfork(const char *path, int flag)
{
	return open_rsrc(path, flag);
}

void close_rfork(const char *path, int fd)
{
	close(fd);
}


/*
 *  Read "length" bytes from file to "buffer",
 *  returns number of bytes read (or -1 on error)
 */

ssize_t extfs_read(int fd, void *buffer, size_t length)
{
	return extfs_posix_read(fd, buffer, length);
}


/*
 *  Write "length" bytes from "buffer" to file,
 *  returns number of bytes written (or -1 on error)
 */

ssize_t extfs_write(int fd, void *buffer, size_t length)
{
	return extfs_posix_write(fd, buffer, length);
}


/*
 *  Remove file/directory (and associated helper files),
 *  returns false on error (and sets errno)
 */

bool extfs_remove(const char *path)
{
	// Remove helpers first, don't complain if this fails
	char helper_path[MAX_PATH_LENGTH];
	make_helper_path(path, helper_path, ".finf/", false);
	remove(helper_path);
	make_helper_path(path, helper_path, ".rsrc/", false);
	remove(helper_path);
	make_helper_path(path, helper_path, "._", false);
	remove(helper_path);

	// A folder: its helper folders first (FatFs removes empty folders only)
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		helper_path[0] = 0;
		strncpy(helper_path, path, MAX_PATH_LENGTH-1);
		add_path_component(helper_path, ".finf");
		remove(helper_path);
		helper_path[0] = 0;
		strncpy(helper_path, path, MAX_PATH_LENGTH-1);
		add_path_component(helper_path, ".rsrc");
		remove(helper_path);
	}
	return remove(path) == 0;
}


/*
 *  Rename/move file/directory (and associated helper files),
 *  returns false on error (and sets errno)
 */

bool extfs_rename(const char *old_path, const char *new_path)
{
	// Rename helpers first, don't complain if this fails
	char old_helper_path[MAX_PATH_LENGTH], new_helper_path[MAX_PATH_LENGTH];
	make_helper_path(old_path, old_helper_path, ".finf/", false);
	make_helper_path(new_path, new_helper_path, ".finf/", false);
	create_helper_dir(new_path, ".finf/");
	rename(old_helper_path, new_helper_path);
	make_helper_path(old_path, old_helper_path, ".rsrc/", false);
	make_helper_path(new_path, new_helper_path, ".rsrc/", false);
	create_helper_dir(new_path, ".rsrc/");
	rename(old_helper_path, new_helper_path);
	make_helper_path(old_path, old_helper_path, "._", false);
	make_helper_path(new_path, new_helper_path, "._", false);
	rename(old_helper_path, new_helper_path);

	// Now rename file
	return rename(old_path, new_path) == 0;
}


// FAT can't have these in names (and ':' is the Mac's '/', see extfs.cpp).
static bool needs_escape(unsigned char c, bool last)
{
	return c < 0x20 || c >= 0x7f || strchr("%*?\"<>|\\:", c) != NULL
	    || (last && (c == '.' || c == ' '));
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

// Convert from the host OS filename encoding to MacRoman: %XX back to the
// character
const char *host_encoding_to_macroman(const char *filename)
{
	static char name[MAX_PATH_LENGTH];
	size_t n = 0;
	while (*filename && n < sizeof(name) - 1) {
		int hi, lo;
		if (filename[0] == '%' && (hi = hex_digit(filename[1])) >= 0
		    && (lo = hex_digit(filename[2])) >= 0) {
			name[n++] = (char)(hi << 4 | lo);
			filename += 3;
		} else
			name[n++] = *filename++;
	}
	name[n] = 0;
	return name;
}

// Convert from MacRoman to host OS filename encoding: what FAT can't have
// as %XX
const char *macroman_to_host_encoding(const char *filename)
{
	static char name[MAX_PATH_LENGTH];
	size_t n = 0;
	for (; *filename && n < sizeof(name) - 4; filename++) {
		unsigned char c = (unsigned char)*filename;
		if (needs_escape(c, filename[1] == 0)) {
			snprintf(name + n, 4, "%%%02X", c);
			n += 3;
		} else
			name[n++] = (char)c;
	}
	name[n] = 0;
	return name;
}
