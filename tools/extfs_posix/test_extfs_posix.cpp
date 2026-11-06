/*
 * test_extfs_posix.cpp - tests src/port/ii/extfs_posix.cpp on the PC.
 *
 * The Mac II's SD Card disk (Basilisk II's ExtFS) reads and writes files
 * through extfs_posix.cpp, which keeps POSIX's file positions on FatFs.
 * Here lseek() and ftruncate() are stand-ins (the program is linked with
 * --wrap) that do what circle-newlib's do on FatFs:
 *
 * - lseek() past the end of a file open for writing grows the file to
 *   there, the new part left with what the card held (0xAA here);
 *   past the end of one open only for reading, it stops at the end.
 * - ftruncate() goes to the new length, shortens the file and goes back to
 *   where it was (growing the file again if that was past the new end).
 *
 * Random reads, writes, seeks and truncations go to a file through
 * extfs_posix.cpp on those stand-ins, and the same to another file through
 * the C library's own calls (POSIX): the two must read back the same and
 * report the same positions. The stand-ins on their own must not, or they
 * don't show what FatFs does.
 *
 *   tools/extfs_posix/run_tests.sh
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define EXTFS_POSIX_IMPLEMENTATION
#include "../../src/port/ii/extfs_posix.h"

extern "C" off_t __real_lseek(int fd, off_t offset, int whence);
extern "C" int __real_ftruncate(int fd, off_t length);

// ---- FatFs's lseek() and ftruncate(), as circle-newlib has them ----

static off_t size_of(int fd)
{
	struct stat st;
	return fstat(fd, &st) == 0 ? st.st_size : -1;
}

static bool writable(int fd)
{
	return (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY;
}

extern "C" off_t __wrap_lseek(int fd, off_t offset, int whence)
{
	off_t cur = __real_lseek(fd, 0, SEEK_CUR);
	off_t size = size_of(fd);
	off_t to = whence == SEEK_SET ? offset : whence == SEEK_CUR ? cur + offset : size + offset;
	if (to < 0) {
		errno = EINVAL;
		return -1;
	}
	if (to > size) {
		if (!writable(fd))
			to = size;  // f_lseek() clips in read mode
		else {
			// f_lseek() grows the file: its new part is undefined
			std::vector<char> junk(to - size, (char)0xAA);
			__real_lseek(fd, size, SEEK_SET);
			if (write(fd, junk.data(), junk.size()) != (ssize_t)junk.size())
				return -1;
		}
	}
	return __real_lseek(fd, to, SEEK_SET);
}

extern "C" int __wrap_ftruncate(int fd, off_t length)
{
	off_t cur = __real_lseek(fd, 0, SEEK_CUR);
	if (__wrap_lseek(fd, length, SEEK_SET) < 0)
		return -1;
	int r = __real_ftruncate(fd, length);
	__wrap_lseek(fd, cur, SEEK_SET);  // circle-newlib goes back
	return r;
}

// ---- the tests ----

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static std::vector<char> contents(const char *path)
{
	std::vector<char> v;
	FILE *f = fopen(path, "rb");
	if (!f)
		return v;
	int c;
	while ((c = fgetc(f)) != EOF)
		v.push_back((char)c);
	fclose(f);
	return v;
}

// What one file is used through: the layer, the stand-ins on their own,
// or the C library (POSIX, the reference).
struct Ops {
	int (*open_)(const char *, int, ...);
	int (*close_)(int);
	off_t (*lseek_)(int, off_t, int);
	int (*ftruncate_)(int, off_t);
	ssize_t (*read_)(int, void *, size_t);
	ssize_t (*write_)(int, const void *, size_t);
};

static ssize_t c_read(int fd, void *b, size_t n) { return read(fd, b, n); }
static ssize_t c_write(int fd, const void *b, size_t n) { return write(fd, b, n); }

static const Ops layer = {extfs_posix_open, extfs_posix_close, extfs_posix_lseek,
                          extfs_posix_ftruncate, extfs_posix_read, extfs_posix_write};
static const Ops fatfs = {open, close, __wrap_lseek, __wrap_ftruncate, c_read, c_write};
static const Ops posix = {open, close, __real_lseek, __real_ftruncate, c_read, c_write};

// Runs the same random operations on two files; returns whether they ended
// the same (and reported the same along the way).
static bool same_run(const Ops &a, const char *pa, const Ops &b, const char *pb,
                     unsigned seed, int steps, bool report)
{
	srand(seed);
	unlink(pa);
	unlink(pb);
	int fa = a.open_(pa, O_RDWR | O_CREAT, 0644);
	int fb = b.open_(pb, O_RDWR | O_CREAT, 0644);
	if (fa < 0 || fb < 0) {
		perror("open");
		failures++;
		return false;
	}
	bool same = true;
	char buf_a[512], buf_b[512], data[512];
	for (int i = 0; i < steps && same; i++) {
		int op = rand() % 6;
		off_t n = rand() % 3000;
		off_t ra = 0, rb = 0;
		const char *what = "";
		switch (op) {
		case 0:
			what = "lseek SET";
			ra = a.lseek_(fa, n, SEEK_SET);
			rb = b.lseek_(fb, n, SEEK_SET);
			break;
		case 1:
			what = "lseek CUR";
			n -= 1500;
			ra = a.lseek_(fa, n, SEEK_CUR);
			rb = b.lseek_(fb, n, SEEK_CUR);
			break;
		case 2:
			what = "lseek END";
			n -= 1000;
			ra = a.lseek_(fa, n, SEEK_END);
			rb = b.lseek_(fb, n, SEEK_END);
			break;
		case 3: {
			what = "write";
			size_t len = rand() % sizeof(data);
			for (size_t j = 0; j < len; j++)
				data[j] = (char)('a' + rand() % 26);
			ra = a.write_(fa, data, len);
			rb = b.write_(fb, data, len);
			break;
		}
		case 4: {
			what = "read";
			size_t len = rand() % sizeof(buf_a);
			ra = a.read_(fa, buf_a, len);
			rb = b.read_(fb, buf_b, len);
			if (ra == rb && ra > 0 && memcmp(buf_a, buf_b, ra) != 0)
				rb = -2;
			break;
		}
		case 5:
			what = "ftruncate";
			ra = a.ftruncate_(fa, n);
			rb = b.ftruncate_(fb, n);
			break;
		}
		off_t pa_ = a.lseek_(fa, 0, SEEK_CUR), pb_ = b.lseek_(fb, 0, SEEK_CUR);
		if (getenv("TRACE") && (unsigned)atoi(getenv("TRACE")) == seed && report)
			fprintf(stderr, "%d %s %lld: %lld %lld at %lld %lld size %lld %lld\n", i, what, (long long)n, (long long)ra, (long long)rb, (long long)pa_, (long long)pb_, (long long)size_of(fa), (long long)size_of(fb));
		if (ra != rb || pa_ != pb_) {
			if (report)
				CHECK(0, "seed %u step %d: %s %lld: %lld vs %lld, at %lld vs %lld",
				      seed, i, what, (long long)n, (long long)ra, (long long)rb,
				      (long long)pa_, (long long)pb_);
			same = false;
		}
	}
	a.close_(fa);
	b.close_(fb);
	if (same && contents(pa) != contents(pb)) {
		if (report)
			CHECK(0, "seed %u: the files differ (%zu vs %zu bytes)", seed,
			      contents(pa).size(), contents(pb).size());
		same = false;
	}
	return same;
}

// What the Mac does when it saves over a file with less: write, shorten.
static void test_save_shorter(const char *dir)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/save", dir);
	unlink(path);
	int fd = extfs_posix_open(path, O_RDWR | O_CREAT, 0644);
	char big[1000], small[300];
	memset(big, 'B', sizeof(big));
	memset(small, 's', sizeof(small));
	extfs_posix_write(fd, big, sizeof(big));
	extfs_posix_close(fd);

	fd = extfs_posix_open(path, O_RDWR);
	extfs_posix_lseek(fd, 0, SEEK_SET);
	extfs_posix_write(fd, small, sizeof(small));
	CHECK(extfs_posix_ftruncate(fd, sizeof(small)) == 0, "ftruncate failed");
	CHECK(extfs_posix_lseek(fd, 0, SEEK_CUR) == (off_t)sizeof(small), "position moved");
	extfs_posix_close(fd);
	std::vector<char> got = contents(path);
	CHECK(got.size() == sizeof(small), "saved file is %zu bytes, not %zu",
	      got.size(), sizeof(small));

	// Shorten while the position is past the new end: FatFs grew it back.
	fd = extfs_posix_open(path, O_RDWR);
	extfs_posix_lseek(fd, 250, SEEK_SET);
	CHECK(extfs_posix_ftruncate(fd, 100) == 0, "ftruncate failed");
	CHECK(extfs_posix_lseek(fd, 0, SEEK_CUR) == 250, "position isn't kept");
	extfs_posix_close(fd);
	CHECK(contents(path).size() == 100, "file is %zu bytes, not 100", contents(path).size());

	// Seeking past the end alone doesn't grow it; writing there fills zeros.
	fd = extfs_posix_open(path, O_RDWR);
	CHECK(extfs_posix_lseek(fd, 5000, SEEK_SET) == 5000, "lseek");
	extfs_posix_close(fd);
	CHECK(contents(path).size() == 100, "a seek grew the file to %zu", contents(path).size());
	fd = extfs_posix_open(path, O_RDWR);
	extfs_posix_lseek(fd, 200, SEEK_SET);
	CHECK(extfs_posix_write(fd, "x", 1) == 1, "write");
	extfs_posix_close(fd);
	got = contents(path);
	CHECK(got.size() == 201, "file is %zu bytes, not 201", got.size());
	bool zeros = got.size() == 201;
	for (int i = 100; zeros && i < 200; i++)
		zeros = got[i] == 0;
	CHECK(zeros, "the gap isn't zeros");
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	char pa[1024], pb[1024];
	snprintf(pa, sizeof(pa), "%s/a", dir);
	snprintf(pb, sizeof(pb), "%s/b", dir);

	test_save_shorter(dir);

	// The stand-ins must break files as FatFs does, or this tests nothing.
	int broke = 0;
	for (unsigned seed = 1; seed <= 50; seed++)
		if (!same_run(fatfs, pa, posix, pb, seed, 200, false))
			broke++;
	CHECK(broke > 0, "the FatFs stand-ins behave as POSIX");

	const int runs = 2000;
	for (unsigned seed = 1; seed <= runs; seed++)
		same_run(layer, pa, posix, pb, seed, 200, true);

	printf("extfs_posix: FatFs's calls broke %d of 50 runs; through extfs_posix: %d run%s of %d failed\n",
	       broke, failures, failures == 1 ? "" : "s", runs);
	return failures ? 1 : 0;
}
