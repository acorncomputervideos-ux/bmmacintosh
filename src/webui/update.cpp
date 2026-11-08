//
// update.cpp - see update.h.
//
// This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
// the Raspberry Pi. See LICENSE for licensing details.
//

#include "update.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sdcard/sd_fs.h"
#include "zlib.h"

namespace {

const unsigned kMaxEntries = 1024;
const unsigned kMaxCentral = 1024 * 1024;
const unsigned kChunk = 32 * 1024;

enum Kind {
  K_REPLACE,      // the release's: replaced
  K_KEEP,         // the user's: made only where missing
  K_CONFIG,       // config.txt, cmdline.txt: made where missing, else .new
  K_SKIP,         // not for the card (folders, macOS's)
};

struct Entry {
  char target[256];   // on the card, without the leading "/"
  uint16_t method, flags;
  uint32_t crc, csize, usize, local;
  Kind kind;
  bool kernel;
  bool unpacked;      // <target>.upd made
  bool as_new;        // to <target>.new (a config file that differs)
};

uint16_t le16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) {
  return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

bool CiEqual(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char ca = *a >= 'A' && *a <= 'Z' ? (char) (*a + 32) : *a;
    char cb = *b >= 'A' && *b <= 'Z' ? (char) (*b + 32) : *b;
    if (ca != cb) return false;
  }
  return *a == *b;
}

bool CiPrefix(const char *s, const char *prefix) {
  for (; *prefix; s++, prefix++) {
    char ca = *s >= 'A' && *s <= 'Z' ? (char) (*s + 32) : *s;
    if (ca != *prefix) return false;
  }
  return true;
}

bool CiSuffix(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  return n >= m && CiEqual(s + n - m, suffix);
}

const char *BaseName(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// A BMMacintosh kernel at the top of the release: macplus.img,
// macii8-rpi4.img, ...
bool IsKernel(const char *target) {
  return strchr(target, '/') == 0 && CiPrefix(target, "mac") && CiSuffix(target, ".img");
}

Kind KindOf(const char *target) {
  const char *slash = strchr(target, '/');
  if (slash != 0) {
    size_t n = (size_t) (slash - target);
    char top[16];
    if (n < sizeof(top)) {
      memcpy(top, target, n);
      top[n] = '\0';
      if (CiEqual(top, "disks") || CiEqual(top, "shared") || CiEqual(top, "roms")) return K_KEEP;
    }
    return K_REPLACE;
  }
  if (CiEqual(target, "wpa_supplicant.conf") || CiEqual(target, "bmmac.cfg") ||
      CiEqual(target, "macii.prefs")) {
    return K_KEEP;
  }
  if (CiEqual(target, "config.txt") || CiEqual(target, "cmdline.txt")) return K_CONFIG;
  return K_REPLACE;
}

bool SafeName(const char *name) {
  if (name[0] == '\0' || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
  for (const char *p = name; *p;) {
    const char *end = strchr(p, '/');
    size_t n = end ? (size_t) (end - p) : strlen(p);
    if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
    if (n > SD_MAX_NAME_LEN) return false;
    if (!end) break;
    p = end + 1;
  }
  return true;
}

bool Exists(const char *target) {
  char path[300];
  snprintf(path, sizeof(path), "/%s", target);
  return sd_stat_info(path, 0) == SD_OK;
}

// The folders on the way to TARGET.
void MakeFolders(const char *target) {
  char path[300];
  snprintf(path, sizeof(path), "/%s", target);
  for (char *p = path + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      sd_mkdir(path);
      *p = '/';
    }
  }
}

class Zip {
public:
  explicit Zip(const char *path) : m_f(sd_open(path, SD_READ)) {}
  ~Zip() {
    if (m_f) sd_close(m_f);
  }
  bool Open() const { return m_f != 0; }

  bool ReadAt(uint32_t pos, void *buf, unsigned len) {
    unsigned got = 0;
    return sd_seek(m_f, pos) == SD_OK && sd_read(m_f, buf, len, &got) == SD_OK && got == len;
  }

  uint32_t Size() { return sd_size(m_f); }

  // Unpacks E to OUT (or, when OUT is 0, to MEM, up to MEM_SIZE bytes).
  // False if it doesn't unpack whole and right.
  bool Unpack(const Entry &e, sd_file *out, char *mem, unsigned mem_size, void (*yield)(void),
              uint8_t *in_buf, uint8_t *out_buf) {
    uint8_t local[30];
    if (!ReadAt(e.local, local, sizeof(local)) || le32(local) != 0x04034b50) return false;
    uint32_t pos = e.local + 30 + le16(local + 26) + le16(local + 28);
    if (sd_seek(m_f, pos) != SD_OK) return false;
    uint32_t left = e.csize, written = 0, crc = (uint32_t) crc32(0L, Z_NULL, 0);

    z_stream z;
    memset(&z, 0, sizeof(z));
    if (e.method == 8 && inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    bool ok = true, done = false;
    while (ok && !done) {
      unsigned n = left < kChunk ? left : kChunk, got = 0;
      if (n > 0 && (sd_read(m_f, in_buf, n, &got) != SD_OK || got != n)) {
        ok = false;
        break;
      }
      left -= n;
      if (e.method == 0) {
        // Stored.
        if (got == 0) break;
        if (!Put(out, mem, mem_size, written, in_buf, got)) ok = false;
        crc = (uint32_t) crc32(crc, in_buf, got);
        written += got;
        if (left == 0) done = true;
      } else {
        z.next_in = in_buf;
        z.avail_in = got;
        do {
          z.next_out = out_buf;
          z.avail_out = kChunk;
          int rc = inflate(&z, left == 0 ? Z_FINISH : Z_NO_FLUSH);
          if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            ok = false;
            break;
          }
          unsigned produced = kChunk - z.avail_out;
          if (produced > 0) {
            if (!Put(out, mem, mem_size, written, out_buf, produced)) {
              ok = false;
              break;
            }
            crc = (uint32_t) crc32(crc, out_buf, produced);
            written += produced;
          }
          if (rc == Z_STREAM_END) {
            done = true;
            break;
          }
          if (rc == Z_BUF_ERROR && produced == 0 && z.avail_in == 0 && left == 0) {
            ok = false;  // ran out before its end
            break;
          }
        } while (z.avail_out == 0 || z.avail_in > 0);
      }
      if (yield) yield();
    }
    if (e.method == 8) inflateEnd(&z);
    return ok && written == e.usize && crc == e.crc;
  }

private:
  static bool Put(sd_file *out, char *mem, unsigned mem_size, uint32_t at, const uint8_t *buf,
                  unsigned n) {
    if (out) return sd_write(out, buf, n) == SD_OK;
    if (at + n >= mem_size) return false;
    memcpy(mem + at, buf, n);
    mem[at + n] = '\0';
    return true;
  }

  sd_file *m_f;
};

void Say(UpdateResult *r, const char *text) { snprintf(r->message, sizeof(r->message), "%s", text); }

// Whether the file at PATH holds just what UPD (the unpacked one) does.
bool SameFile(const char *path, const char *upd, uint8_t *a, uint8_t *b) {
  sd_file *fa = sd_open(path, SD_READ);
  sd_file *fb = fa ? sd_open(upd, SD_READ) : 0;
  bool same = fa && fb && sd_size(fa) == sd_size(fb);
  while (same) {
    unsigned ga = 0, gb = 0;
    if (sd_read(fa, a, kChunk, &ga) != SD_OK || sd_read(fb, b, kChunk, &gb) != SD_OK || ga != gb) {
      same = false;
      break;
    }
    if (ga == 0) break;
    if (memcmp(a, b, ga) != 0) same = false;
  }
  if (fa) sd_close(fa);
  if (fb) sd_close(fb);
  return same;
}

void RemoveUnpacked(Entry *entries, unsigned n) {
  char path[300];
  for (unsigned i = 0; i < n; i++) {
    if (!entries[i].unpacked) continue;
    snprintf(path, sizeof(path), "/%s.upd", entries[i].target);
    sd_unlink(path);
    entries[i].unpacked = false;
  }
}

}  // namespace

void UpdateInstall(const char *zip_path, UpdateResult *r, void (*yield)(void)) {
  memset(r, 0, sizeof(*r));
  Zip zip(zip_path);
  if (!zip.Open()) {
    Say(r, "The uploaded file couldn't be opened.");
    return;
  }

  // The end of the central directory: within the last 64 KB (a comment).
  uint32_t size = zip.Size();
  unsigned tail_len = size < 65557 ? size : 65557;
  uint8_t *tail = (uint8_t *) malloc(tail_len);
  uint8_t *in_buf = (uint8_t *) malloc(kChunk);
  uint8_t *out_buf = (uint8_t *) malloc(kChunk);
  Entry *entries = new Entry[kMaxEntries];
  uint8_t *central = 0;
  unsigned n = 0;
  int top_kernels = 0;
  bool stripped = false;
  char top[64] = "";
  uint64_t need = 0;
  uint32_t total_kb = 0, free_kb = 0;
  long eocd = -1;

  if (!tail || !in_buf || !out_buf || !entries) {
    Say(r, "Not enough memory to unpack it.");
    goto out;
  }
  if (tail_len < 22 || !zip.ReadAt(size - tail_len, tail, tail_len)) {
    Say(r, "That isn't a zip file.");
    goto out;
  }
  for (long i = (long) tail_len - 22; i >= 0; i--) {
    if (le32(tail + i) == 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    Say(r, "That isn't a zip file.");
    goto out;
  }
  {
    unsigned count = le16(tail + eocd + 10);
    uint32_t cd_size = le32(tail + eocd + 12), cd_offset = le32(tail + eocd + 16);
    if (count == 0xFFFF || cd_offset == 0xFFFFFFFFu || cd_size > kMaxCentral ||
        (uint64_t) cd_offset + cd_size > size) {
      Say(r, "That zip is too big or damaged (zip64 isn't read here).");
      goto out;
    }
    central = (uint8_t *) malloc(cd_size + 1);
    if (!central || !zip.ReadAt(cd_offset, central, cd_size)) {
      Say(r, "The zip's list of files couldn't be read.");
      goto out;
    }

    // The entries, as they are named in the zip.
    uint32_t p = 0;
    for (unsigned i = 0; i < count; i++) {
      if (p + 46 > cd_size || le32(central + p) != 0x02014b50) {
        Say(r, "The zip's list of files is damaged.");
        goto out;
      }
      const uint8_t *h = central + p;
      unsigned name_len = le16(h + 28), extra = le16(h + 30), comment = le16(h + 32);
      if (p + 46 + name_len > cd_size) {
        Say(r, "The zip's list of files is damaged.");
        goto out;
      }
      if (n >= kMaxEntries) {
        Say(r, "That zip holds too many files to be a BMMacintosh release.");
        goto out;
      }
      Entry &e = entries[n];
      memset(&e, 0, sizeof(e));
      if (name_len >= sizeof(e.target)) {
        Say(r, "A name in the zip is too long.");
        goto out;
      }
      memcpy(e.target, h + 46, name_len);
      e.target[name_len] = '\0';
      e.flags = le16(h + 8);
      e.method = le16(h + 10);
      e.crc = le32(h + 16);
      e.csize = le32(h + 20);
      e.usize = le32(h + 24);
      e.local = le32(h + 42);
      p += 46 + name_len + extra + comment;

      const char *base = BaseName(e.target);
      size_t len = strlen(e.target);
      if (len == 0 || e.target[len - 1] == '/' || CiPrefix(e.target, "__macosx/") ||
          (base[0] == '.' && base[1] == '_') || CiEqual(base, ".DS_Store")) {
        e.kind = K_SKIP;  // a folder, or macOS's own
      }
      n++;
    }

    // A folder at the top that holds it all: passed by.
    for (unsigned i = 0; i < n; i++) {
      if (entries[i].kind == K_SKIP) continue;
      if (IsKernel(entries[i].target)) top_kernels++;
    }
    if (top_kernels == 0) {
      const char *first = 0;
      for (unsigned i = 0; i < n && !first; i++) {
        if (entries[i].kind != K_SKIP) first = entries[i].target;
      }
      const char *slash = first ? strchr(first, '/') : 0;
      if (slash && (size_t) (slash - first) + 1 < sizeof(top)) {
        memcpy(top, first, (size_t) (slash - first) + 1);
        top[slash - first + 1] = '\0';
        bool all = true;
        for (unsigned i = 0; i < n && all; i++) {
          if (entries[i].kind != K_SKIP && strncmp(entries[i].target, top, strlen(top)) != 0) {
            all = false;
          }
        }
        if (all) {
          size_t t = strlen(top);
          for (unsigned i = 0; i < n; i++) {
            if (strncmp(entries[i].target, top, t) == 0) {
              memmove(entries[i].target, entries[i].target + t, strlen(entries[i].target + t) + 1);
            }
          }
          stripped = true;
        }
      }
    }
    (void) stripped;

    // Checked, and sorted out.
    top_kernels = 0;
    for (unsigned i = 0; i < n; i++) {
      Entry &e = entries[i];
      if (e.kind == K_SKIP) continue;
      if (e.target[0] == '\0') {
        e.kind = K_SKIP;
        continue;
      }
      if (!SafeName(e.target)) {
        Say(r, "A name in the zip goes outside the card: not installed.");
        goto out;
      }
      if (e.flags & 1) {
        Say(r, "The zip is encrypted: not installed.");
        goto out;
      }
      if (e.method != 0 && e.method != 8) {
        Say(r, "The zip uses a kind of compression that isn't read here: not installed.");
        goto out;
      }
      e.kind = KindOf(e.target);
      e.kernel = IsKernel(e.target);
      if (e.kernel) top_kernels++;
      if (CiEqual(e.target, "VERSION.txt")) {
        char text[sizeof(r->version) + 1];
        if (e.usize < sizeof(text) &&
            zip.Unpack(e, 0, text, sizeof(text), yield, in_buf, out_buf)) {
          char *nl = strpbrk(text, "\r\n");
          if (nl) *nl = '\0';
          snprintf(r->version, sizeof(r->version), "%s", text);
        }
      }
      if (e.kind == K_KEEP && Exists(e.target)) {
        e.kind = K_SKIP;
        r->kept++;
        continue;
      }
      need += e.usize;
    }
    if (top_kernels == 0) {
      Say(r, "That zip isn't a BMMacintosh release (no BMMacintosh kernel, mac*.img, in it).");
      goto out;
    }
    if (sd_space_kb(&total_kb, &free_kb) == SD_OK && (uint64_t) free_kb * 1024 < need + 1048576) {
      Say(r, "There isn't room on the card to unpack it.");
      goto out;
    }

    // Unpacked, each to <name>.upd.
    for (unsigned i = 0; i < n; i++) {
      Entry &e = entries[i];
      if (e.kind == K_SKIP) continue;
      MakeFolders(e.target);
      char upd[300];
      snprintf(upd, sizeof(upd), "/%s.upd", e.target);
      sd_file *out = sd_open(upd, SD_WRITE);
      if (!out) {
        RemoveUnpacked(entries, n);
        snprintf(r->message, sizeof(r->message), "%s couldn't be written: nothing was changed.",
                 e.target);
        goto out;
      }
      e.unpacked = true;
      bool ok = zip.Unpack(e, out, 0, 0, yield, in_buf, out_buf);
      if (sd_close(out) != SD_OK) ok = false;
      if (!ok) {
        RemoveUnpacked(entries, n);
        snprintf(r->message, sizeof(r->message),
                 "%s didn't unpack right (a damaged download?): nothing was changed.", e.target);
        goto out;
      }
      if (e.kind == K_CONFIG && Exists(e.target)) {
        char path[300];
        snprintf(path, sizeof(path), "/%s", e.target);
        if (SameFile(path, upd, in_buf, out_buf)) {
          sd_unlink(upd);
          e.unpacked = false;
          e.kind = K_SKIP;
          r->kept++;
        } else {
          e.as_new = true;
        }
      }
    }

    // Put in place.
    unsigned news = 0;
    for (unsigned i = 0; i < n; i++) {
      Entry &e = entries[i];
      if (!e.unpacked) continue;
      char path[300], upd[300], old[310];
      snprintf(path, sizeof(path), "/%s", e.target);
      snprintf(upd, sizeof(upd), "/%s.upd", e.target);
      if (e.as_new) {
        snprintf(old, sizeof(old), "/%s.new", e.target);
        sd_unlink(old);
        sd_rename(upd, old);
        news++;
        r->kept++;
        continue;
      }
      if (Exists(e.target)) {
        if (e.kernel) {
          snprintf(old, sizeof(old), "/%s.old", e.target);
          sd_unlink(old);
          sd_rename(path, old);
        } else {
          sd_unlink(path);
        }
      }
      if (sd_rename(upd, path) != SD_OK) {
        snprintf(r->message, sizeof(r->message),
                 "%s couldn't be put in place: the card may need the release copied by hand.",
                 e.target);
        RemoveUnpacked(entries, n);
        goto out;
      }
      e.unpacked = false;
      r->replaced++;
    }
    r->ok = true;
    snprintf(r->message, sizeof(r->message),
             "%s installed: %u files replaced, %u of yours kept%s. It runs from the next boot.",
             r->version[0] ? r->version : "The release", r->replaced, r->kept,
             news ? " (its config.txt or cmdline.txt differs from yours: see the .new file)" : "");
  }

out:
  free(tail);
  free(in_buf);
  free(out_buf);
  free(central);
  delete[] entries;
}
