/*
 * sd_fs_posix.cpp - src/sdcard/sd_fs.h on the PC, in a folder, for
 * tools/webdav's tests of the network drive (src/webui/webdav.cpp).
 *
 * As FatFs (sd_fs_fatfs.c) has it: sd_rename() won't replace what is
 * there, sd_unlink() removes files and empty folders, sd_mkdir() is fine
 * with a folder that is there already.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "../../src/sdcard/sd_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

static char root[1024] = ".";

extern "C" void sd_posix_root(const char *path) {
	snprintf(root, sizeof(root), "%s", path);
}

struct sd_file {
	FILE *f;
};

static int full(const char *path, char *out, size_t size) {
	int n = snprintf(out, size, "%s%s", root, path);
	return n > 0 && (size_t) n < size ? 0 : -1;
}

static int result(int err) {
	switch (err) {
	case ENOENT: case ENOTDIR: return SD_NOT_FOUND;
	case EEXIST: return SD_EXISTS;
	case EACCES: case EPERM: case ENOTEMPTY: case EBUSY: return SD_DENIED;
	case ENAMETOOLONG: case EINVAL: return SD_INVALID;
	default: return SD_ERROR;
	}
}

sd_file *sd_open(const char *path, int mode) {
	char p[1200];
	if (full(path, p, sizeof(p)) < 0) return 0;
	const char *m = mode == SD_READ ? "rb" : mode == SD_WRITE ? "wb" : mode == SD_APPEND ? "ab" : "r+b";
	FILE *f = fopen(p, m);
	if (!f) return 0;
	sd_file *s = (sd_file *) malloc(sizeof(*s));
	s->f = f;
	return s;
}

int sd_read(sd_file *f, void *buf, unsigned len, unsigned *got) {
	size_t n = fread(buf, 1, len, f->f);
	*got = (unsigned) n;
	return ferror(f->f) ? SD_ERROR : SD_OK;
}

int sd_write(sd_file *f, const void *buf, unsigned len) {
	return fwrite(buf, 1, len, f->f) == len ? SD_OK : SD_ERROR;
}

int sd_seek(sd_file *f, uint32_t pos) { return fseek(f->f, pos, SEEK_SET) == 0 ? SD_OK : SD_ERROR; }

uint32_t sd_size(sd_file *f) {
	struct stat st;
	return fstat(fileno(f->f), &st) == 0 ? (uint32_t) st.st_size : 0;
}

int sd_sync(sd_file *f) { return fflush(f->f) == 0 ? SD_OK : SD_ERROR; }

int sd_close(sd_file *f) {
	int r = fclose(f->f);
	free(f);
	return r == 0 ? SD_OK : SD_ERROR;
}

static void fill(const char *name, const struct stat *st, sd_info *info) {
	memset(info, 0, sizeof(*info));
	snprintf(info->name, sizeof(info->name), "%s", name);
	info->size = S_ISDIR(st->st_mode) ? 0 : (uint32_t) st->st_size;
	info->is_dir = S_ISDIR(st->st_mode);
	struct tm tm;
	localtime_r(&st->st_mtime, &tm);
	info->mtime.year = (uint16_t) (tm.tm_year + 1900);
	info->mtime.month = (uint8_t) (tm.tm_mon + 1);
	info->mtime.day = (uint8_t) tm.tm_mday;
	info->mtime.hour = (uint8_t) tm.tm_hour;
	info->mtime.minute = (uint8_t) tm.tm_min;
	info->mtime.second = (uint8_t) (tm.tm_sec & ~1);
}

int sd_stat_info(const char *path, sd_info *info) {
	char p[1200];
	struct stat st;
	if (full(path, p, sizeof(p)) < 0) return SD_INVALID;
	if (stat(p, &st) != 0) return result(errno);
	if (info) {
		const char *base = strrchr(path, '/');
		fill(base ? base + 1 : path, &st, info);
	}
	return SD_OK;
}

int sd_rename(const char *from, const char *to) {
	char a[1200], b[1200];
	struct stat st;
	if (full(from, a, sizeof(a)) < 0 || full(to, b, sizeof(b)) < 0) return SD_INVALID;
	if (strcasecmp(from, to) != 0 && stat(b, &st) == 0) return SD_EXISTS;
	return rename(a, b) == 0 ? SD_OK : result(errno);
}

int sd_unlink(const char *path) {
	char p[1200];
	struct stat st;
	if (full(path, p, sizeof(p)) < 0) return SD_INVALID;
	if (stat(p, &st) != 0) return result(errno);
	return (S_ISDIR(st.st_mode) ? rmdir(p) : unlink(p)) == 0 ? SD_OK : result(errno);
}

int sd_mkdir(const char *path) {
	char p[1200];
	struct stat st;
	if (full(path, p, sizeof(p)) < 0) return SD_INVALID;
	if (mkdir(p, 0777) == 0) return SD_OK;
	if (errno == EEXIST && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return SD_OK;
	return result(errno);
}

int sd_set_mtime(const char *path, const sd_time *t) {
	(void) path;
	(void) t;
	return SD_OK;
}

int sd_list_info(const char *path, sd_info_cb cb, void *ctx) {
	char p[1200];
	if (full(path, p, sizeof(p)) < 0) return SD_INVALID;
	DIR *d = opendir(p);
	if (!d) return result(errno);
	struct dirent *e;
	while ((e = readdir(d)) != 0) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char q[1500];
		struct stat st;
		snprintf(q, sizeof(q), "%s/%s", p, e->d_name);
		if (stat(q, &st) != 0) continue;
		sd_info info;
		fill(e->d_name, &st, &info);
		if (cb(ctx, &info)) break;
	}
	closedir(d);
	return SD_OK;
}

static int remove_tree(const char *p, unsigned *removed, void (*progress)(void *), void *ctx) {
	struct stat st;
	if (lstat(p, &st) != 0) return result(errno);
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(p);
		if (!d) return result(errno);
		struct dirent *e;
		while ((e = readdir(d)) != 0) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
			char q[1500];
			snprintf(q, sizeof(q), "%s/%s", p, e->d_name);
			int r = remove_tree(q, removed, progress, ctx);
			if (r != SD_OK) {
				closedir(d);
				return r;
			}
		}
		closedir(d);
		if (rmdir(p) != 0) return result(errno);
	} else if (unlink(p) != 0) {
		return result(errno);
	}
	if (removed) (*removed)++;
	if (progress) progress(ctx);
	return SD_OK;
}

int sd_remove_tree(const char *path, unsigned *removed, void (*progress)(void *ctx), void *ctx) {
	char p[1200];
	if (removed) *removed = 0;
	if (full(path, p, sizeof(p)) < 0) return SD_INVALID;
	return remove_tree(p, removed, progress, ctx);
}

int sd_space_kb(uint32_t *total_kb, uint32_t *free_kb) {
	struct statvfs v;
	if (statvfs(root, &v) != 0) return SD_ERROR;
	*total_kb = (uint32_t) ((unsigned long long) v.f_blocks * v.f_frsize / 1024);
	*free_kb = (uint32_t) ((unsigned long long) v.f_bavail * v.f_frsize / 1024);
	return SD_OK;
}
