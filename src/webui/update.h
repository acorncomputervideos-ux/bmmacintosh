//
// update.h - installs a BMMacintosh release from its zip, on the card.
//
// The web page uploads the release's zip (the SD card files, as
// build_sdcard.sh stages them and the releases hold them); UpdateInstall()
// puts its files on the card for the next boot:
//
// - The kernels, the Pi's boot files and device trees, overlays, Wi-Fi
//   firmware, licences and READMEs are replaced. The kernels replaced are
//   kept as <name>.old, to go back to by hand.
// - What is the user's is left alone: the disks, shared and roms folders
//   (their files only made where missing), and the settings, wpa_supplicant
//   .conf, bmmac.cfg and macii.prefs. config.txt and cmdline.txt are made
//   where missing; when the release's differ, theirs are put next to them
//   as config.txt.new and cmdline.txt.new.
// - A zip without a BMMacintosh kernel (mac*.img) isn't one: nothing is
//   done. Nor when a file in it doesn't unpack whole and right (CRC-32):
//   everything is unpacked to <name>.upd first, and only then put in
//   place.
//
// A folder at the top of the zip that holds it all (as some zip tools
// make) is passed by; macOS's __MACOSX folder and ._ files are left out.
//
// Only sd_fs.h and zlib are used, so tools/update tests it on the PC.
//
// This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
// the Raspberry Pi. See LICENSE for licensing details.
//

#ifndef _update_h
#define _update_h

struct UpdateResult {
  bool ok;
  unsigned replaced;     // files replaced or made
  unsigned kept;         // the user's files left as they were
  char version[64];      // the release's VERSION.txt, or ""
  char message[256];     // for people: what was done, or why not
};

// ZIP_PATH: the zip, on the card. YIELD (or 0) is called now and then, so
// that the network and the emulator run while it unpacks.
void UpdateInstall(const char *zip_path, UpdateResult *result, void (*yield)(void));

#endif
