/*
 * test_appledouble.cpp - tests that the Mac II's SD Card disk (Basilisk
 * II's ExtFS, src/port/ii/extfs_ii.cpp) takes a file's type, creator and
 * resource fork from the AppleDouble file ("._name") macOS writes next to
 * a file it copies to a FAT card, and keeps them with the file when the
 * Mac renames or deletes it.
 *
 * extfs_ii.cpp is compiled in here, with stand-ins for the little of
 * Basilisk II it uses besides.
 *
 *   tools/extfs_posix/run_tests.sh
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "../../src/port/ii/extfs_ii.cpp"

#include <string>
#include <vector>

static int failures;

// Basilisk II's, which extfs_ii.cpp's other functions use (the Mac's
// memory isn't touched here)
time_t MacTimeToTime(uint32 t) { return t; }
uintptr MEMBaseDiff;
uintptr mac_memory_size;
uae_u32 mac_stray_access(const void *, int) { return 0; }

#define CHECK(cond, ...) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static void put32(std::vector<uint8> &v, uint32 x)
{
	v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

static void write_file(const char *path, const std::vector<uint8> &v)
{
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(v.data(), 1, v.size(), f) != v.size()) {
		fprintf(stderr, "FAIL: can't write %s\n", path);
		failures++;
	}
	if (f)
		fclose(f);
}

static std::vector<uint8> read_file(const char *path)
{
	std::vector<uint8> v;
	FILE *f = fopen(path, "rb");
	if (!f)
		return v;
	int c;
	while ((c = fgetc(f)) != EOF)
		v.push_back((uint8)c);
	fclose(f);
	return v;
}

static bool exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

// An AppleDouble file as macOS writes it: Finder info first (with its
// extended attributes after the 32 bytes), then the resource fork.
static std::vector<uint8> appledouble(const char type[4], const char creator[4],
                                      const std::vector<uint8> &rsrc)
{
	const uint32 finf_len = 32 + 50;  // macOS's: FInfo, FXInfo, xattrs
	std::vector<uint8> v;
	put32(v, 0x00051607);
	put32(v, 0x00020000);
	const char filler[17] = "Mac OS X        ";
	v.insert(v.end(), filler, filler + 16);
	v.push_back(0); v.push_back(2);
	uint32 finf_off = 26 + 2 * 12;
	put32(v, 9); put32(v, finf_off); put32(v, finf_len);
	put32(v, 2); put32(v, finf_off + finf_len); put32(v, (uint32)rsrc.size());
	std::vector<uint8> finf(finf_len, 0);
	memcpy(&finf[0], type, 4);
	memcpy(&finf[4], creator, 4);
	finf[8] = 0x01;  // fdFlags
	finf[16 + 8] = 0x77;  // something in FXInfo
	finf[40] = 0xEE;  // macOS's extended attributes: not Finder info
	v.insert(v.end(), finf.begin(), finf.end());
	v.insert(v.end(), rsrc.begin(), rsrc.end());
	return v;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	char file[1024], ad[1024], finf[1024], rsrc[1024], path[1024];
	snprintf(file, sizeof(file), "%s/SimpleText", dir);
	snprintf(ad, sizeof(ad), "%s/._SimpleText", dir);
	snprintf(finf, sizeof(finf), "%s/.finf/SimpleText", dir);
	snprintf(rsrc, sizeof(rsrc), "%s/.rsrc/SimpleText", dir);

	std::vector<uint8> fork;
	for (int i = 0; i < 20000; i++)  // bigger than the copying buffer
		fork.push_back((uint8)(i * 7));
	write_file(file, std::vector<uint8>(100, 'd'));
	write_file(ad, appledouble("APPL", "ttxt", fork));

	// The resource fork: the size, then the fork itself
	CHECK(get_rfork_size(file) == fork.size(), "resource fork is %u bytes, not %zu",
	      get_rfork_size(file), fork.size());
	std::vector<uint8> got = read_file(rsrc);
	CHECK(got == fork, "the .rsrc file isn't the resource fork (%zu bytes)", got.size());

	// The Finder info: the first 32 bytes only
	got = read_file(finf);
	CHECK(got.size() == 32, ".finf is %zu bytes, not 32", got.size());
	CHECK(got.size() >= 8 && memcmp(&got[0], "APPLttxt", 8) == 0, "type and creator aren't APPL ttxt");
	CHECK(got.size() == 32 && got[24] == 0x77, "FXInfo isn't kept");

	// Once made, the Mac's own are used, not the AppleDouble file's again
	write_file(finf, std::vector<uint8>(32, 0x11));
	int fd = open_rfork(file, O_RDONLY);
	CHECK(fd >= 0, "open_rfork");
	close_rfork(file, fd);
	CHECK(read_file(finf) == std::vector<uint8>(32, 0x11), "the Mac's Finder info was replaced");

	// A file without one: no helpers made, no resource fork
	snprintf(path, sizeof(path), "%s/plain.txt", dir);
	write_file(path, std::vector<uint8>(10, 't'));
	CHECK(get_rfork_size(path) == 0, "plain file has a resource fork");
	snprintf(path, sizeof(path), "%s/.rsrc/plain.txt", dir);
	CHECK(!exists(path), "an empty .rsrc was made");

	// Not an AppleDouble file: left alone
	snprintf(path, sizeof(path), "%s/odd", dir);
	write_file(path, std::vector<uint8>(10, 'o'));
	snprintf(path, sizeof(path), "%s/._odd", dir);
	write_file(path, std::vector<uint8>(64, 'x'));
	snprintf(path, sizeof(path), "%s/odd", dir);
	CHECK(get_rfork_size(path) == 0, "a bad ._ file made a resource fork");

	// One whose entries go past its end: those are skipped
	snprintf(path, sizeof(path), "%s/._short", dir);
	std::vector<uint8> bad = appledouble("TEXT", "ttxt", fork);
	bad.resize(bad.size() - 100);
	write_file(path, bad);
	snprintf(path, sizeof(path), "%s/short", dir);
	write_file(path, std::vector<uint8>(10, 's'));
	CHECK(get_rfork_size(path) == 0, "a cut-off resource fork was used");
	snprintf(path, sizeof(path), "%s/.finf/short", dir);
	got = read_file(path);
	CHECK(got.size() == 32 && memcmp(&got[0], "TEXTttxt", 8) == 0, "the Finder info wasn't used");

	// Renamed by the Mac: the AppleDouble file goes with it
	char to[1024], ad_to[1024];
	snprintf(to, sizeof(to), "%s/Renamed", dir);
	snprintf(ad_to, sizeof(ad_to), "%s/._Renamed", dir);
	CHECK(extfs_rename(file, to), "rename");
	CHECK(!exists(ad) && exists(ad_to), "._ file wasn't renamed");
	snprintf(path, sizeof(path), "%s/.rsrc/Renamed", dir);
	CHECK(read_file(path) == fork, ".rsrc wasn't renamed");

	// Deleted by the Mac: a new file of that name doesn't get its forks
	CHECK(extfs_remove(to), "remove");
	CHECK(!exists(ad_to), "._ file wasn't removed");
	write_file(to, std::vector<uint8>(5, 'n'));
	CHECK(get_rfork_size(to) == 0, "a new file got the deleted one's resource fork");

	// ---- The type and creator the Mac sees (get_finfo()) ----
	static uint8 mac[4096];          // a little of the Mac's memory
	MEMBaseDiff = (uintptr) mac;
	mac_memory_size = sizeof mac;
	auto type_of = [&](const char *p) {
		memset(mac, 0, sizeof mac);
		get_finfo(p, 0x100, 0x200, false);
		return std::string((const char *) mac + 0x100 + fdType, 8);
	};
	char a[1024], b[1024];

	// macOS's ._ file with no type or creator (a download's quarantine):
	// the extension's, and no .finf made from it.
	snprintf(a, sizeof a, "%s/Game.sit", dir);
	snprintf(b, sizeof b, "%s/._Game.sit", dir);
	write_file(a, std::vector<uint8>(100, 's'));
	write_file(b, appledouble("\0\0\0\0", "\0\0\0\0", std::vector<uint8>()));
	CHECK(type_of(a) == "SIT!SITx", "a ._ file with no type: the .sit extension's SIT! SITx (got %s)",
	      type_of(a).c_str());
	snprintf(b, sizeof b, "%s/.finf/Game.sit", dir);
	CHECK(!exists(b), "no empty .finf made from it");

	// An empty .finf already there (an earlier BMMacintosh's): the extension's.
	snprintf(a, sizeof a, "%s/Old.sit", dir);
	snprintf(b, sizeof b, "%s/.finf/Old.sit", dir);
	write_file(a, std::vector<uint8>(100, 's'));
	write_file(b, std::vector<uint8>(32, 0));
	CHECK(type_of(a) == "SIT!SITx", "an empty .finf: the extension's (got %s)", type_of(a).c_str());

	// Capitals.
	snprintf(a, sizeof a, "%s/GAME2.SIT", dir);
	write_file(a, std::vector<uint8>(100, 's'));
	CHECK(type_of(a) == "SIT!SITx", "GAME2.SIT: SIT! SITx (got %s)", type_of(a).c_str());

	// A real type in the ._ file still wins.
	snprintf(a, sizeof a, "%s/Tool.sit", dir);
	snprintf(b, sizeof b, "%s/._Tool.sit", dir);
	write_file(a, std::vector<uint8>(100, 's'));
	write_file(b, appledouble("APPL", "TOOL", std::vector<uint8>(64, 1)));
	CHECK(type_of(a) == "APPLTOOL", "a ._ file's own type wins (got %s)", type_of(a).c_str());

	// New extensions.
	snprintf(a, sizeof a, "%s/Archive.cpt", dir);
	write_file(a, std::vector<uint8>(100, 'c'));
	CHECK(type_of(a) == "PACTCPCT", ".cpt: Compact Pro's (got %s)", type_of(a).c_str());

	// The Mac saw the file before macOS's ._ file came (an empty .finf
	// made then): the ._ file's type is taken, and kept in the .finf.
	snprintf(a, sizeof a, "%s/Late.sit", dir);
	snprintf(b, sizeof b, "%s/.finf/Late.sit", dir);
	write_file(a, std::vector<uint8>(100, 's'));
	write_file(b, std::vector<uint8>(32, 0));
	snprintf(b, sizeof b, "%s/._Late.sit", dir);
	write_file(b, appledouble("SITD", "SIT!", std::vector<uint8>()));
	CHECK(type_of(a) == "SITDSIT!", "a late ._ file's type (got %s)", type_of(a).c_str());
	snprintf(b, sizeof b, "%s/.finf/Late.sit", dir);
	got = read_file(b);
	CHECK(got.size() == 32 && memcmp(&got[0], "SITDSIT!", 8) == 0, "... kept in the .finf");

	// What the Mac writes in a new folder (StuffIt Expander unpacking on
	// the SD Card disk): kept, though the folder has no .finf or .rsrc
	// yet. (On the Pi, open() says ENOTDIR for those, not ENOENT.)
	snprintf(path, sizeof path, "%s/Unpacked", dir);
	mkdir(path, 0777);
	snprintf(a, sizeof a, "%s/Unpacked/Marathon", dir);
	write_file(a, std::vector<uint8>(10, 'm'));
	memset(mac, 0, sizeof mac);
	memcpy(mac + 0x100 + fdType, "APPL26.\xb0", 8);
	set_finfo(a, 0x100, 0, false);
	CHECK(type_of(a) == std::string("APPL26.\xb0", 8), "a new folder: the type the Mac set is kept (got %s)",
	      type_of(a).c_str());
	int rf = open_rfork(a, O_RDWR);
	CHECK(rf >= 0, "a new folder: the resource fork can be made");
	if (rf >= 0) {
		CHECK(write(rf, "resources", 9) == 9, "resource fork write");
		close_rfork(a, rf);
	}
	CHECK(get_rfork_size(a) == 9, "a new folder: the resource fork is kept (%u bytes)", get_rfork_size(a));

	// And a ._ file there (macOS's copy) is read.
	snprintf(path, sizeof path, "%s/Copied", dir);
	mkdir(path, 0777);
	snprintf(a, sizeof a, "%s/Copied/Game", dir);
	snprintf(b, sizeof b, "%s/Copied/._Game", dir);
	write_file(a, std::vector<uint8>(10, 'g'));
	write_file(b, appledouble("APPL", "GAME", std::vector<uint8>(77, 3)));
	CHECK(get_rfork_size(a) == 77, "a ._ file in a folder without .rsrc: its resource fork (%u)",
	      get_rfork_size(a));
	CHECK(type_of(a) == "APPLGAME", "... and its type (got %s)", type_of(a).c_str());

	printf("appledouble: %d failed\n", failures);
	return failures ? 1 : 0;
}
