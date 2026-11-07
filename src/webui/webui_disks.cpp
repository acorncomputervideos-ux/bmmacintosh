//
// webui_disks.cpp - the web page's Disks (see webui_disks.h).
//
// The answer to GET /api/disks:
//
//   {"machine":"ii",                      (or "plus")
//    "hard":["/disks/Mac OS 8.1.hda"],    (the Mac II's, from its next start)
//    "floppy":["/disks/Tools.dsk",""],    (the Mac II's drives, now)
//    "cdrom":"",
//    "disk":"",                           (the Mac Plus's)
//    "pending":false,"done":3,"result":"Tools.dsk is in the floppy drive.",
//    "images":[{"path":"/disks/Tools.dsk","size":819200,"kind":"floppy",
//               "startable":true},...],
//    "more":false}                        (true when there were too many)
//
// The images are the disk images in the card's disks folder and the
// folders in it. Each has the kind the Mac II takes it as: "cd" (.iso,
// .toast, .cdr), "floppy" (400K, 800K or 1.4 MB, raw or DiskCopy 4.2) or
// "hard"; the Mac Plus's are all "disk". Names are UTF-8 (the card's are
// CP850, cp850.h). "startable" says whether a Mac can start from it (it
// has boot blocks; see Startable()).
//
// This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
// the Raspberry Pi. See LICENSE for licensing details.
//

#include "webui_disks.h"
#include "cp850.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sdcard/sd_fs.h"

extern "C" {
#include "../port/bmmac.h"
}

namespace {

const char *const kDisksFolder = "/disks";
const unsigned kMaxImages = 300;
const unsigned kMaxDepth = 4;

// JSON written into a buffer, which says when it ran out.
class Json {
public:
  Json(char *out, unsigned size) : m_out(out), m_size(size), m_len(0), m_full(false) {
    if (size > 0) out[0] = '\0';
  }

  void Raw(const char *s) {
    unsigned n = (unsigned) strlen(s);
    if (m_full || m_len + n + 1 > m_size) {
      m_full = true;
      return;
    }
    memcpy(m_out + m_len, s, n + 1);
    m_len += n;
  }

  void Printf(const char *format, ...) {
    char text[128];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Raw(text);
  }

  // A string, from CP850 to UTF-8, escaped.
  void String(const char *s) {
    Raw("\"");
    for (const unsigned char *p = (const unsigned char *) s; *p != 0; p++) {
      char out[8];
      if (*p == '"' || *p == '\\') {
        out[0] = '\\';
        out[1] = (char) *p;
        out[2] = '\0';
      } else if (*p < 0x20) {
        snprintf(out, sizeof(out), "\\u%04x", *p);
      } else if (*p < 0x80) {
        out[0] = (char) *p;
        out[1] = '\0';
      } else {
        unsigned cp = kCp850High[*p - 0x80];
        if (cp < 0x800) {
          out[0] = (char) (0xC0 | (cp >> 6));
          out[1] = (char) (0x80 | (cp & 0x3F));
          out[2] = '\0';
        } else {
          out[0] = (char) (0xE0 | (cp >> 12));
          out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
          out[2] = (char) (0x80 | (cp & 0x3F));
          out[3] = '\0';
        }
      }
      Raw(out);
    }
    Raw("\"");
  }

  bool Full(void) const { return m_full; }
  unsigned Length(void) const { return m_len; }

private:
  char *m_out;
  unsigned m_size;
  unsigned m_len;
  bool m_full;
};

bool CiEqual(const char *a, const char *b) {
  for (; *a != '\0' && *b != '\0'; a++, b++) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char) (ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = (char) (cb + 32);
    if (ca != cb) return false;
  }
  return *a == *b;
}

const char *Extension(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *dot = strrchr(slash != 0 ? slash : path, '.');
  return dot != 0 ? dot + 1 : "";
}

// A disk image the Mac takes (emu_run_types()).
bool IsImage(const char *path) {
  const char *ext = Extension(path);
  for (const char *const *t = emu_run_types(); *t != 0; t++) {
    if (CiEqual(ext, *t)) return true;
  }
  return false;
}

bool IsCd(const char *path) {
  const char *ext = Extension(path);
  return CiEqual(ext, "iso") || CiEqual(ext, "toast") || CiEqual(ext, "cdr");
}

// 400K, 800K or 1.4 MB, raw or with DiskCopy 4.2's 84 byte header (as the
// Mac II's menu has it, menu_ii.cpp).
bool IsFloppySize(uint32_t size) {
  uint32_t data = size % 512 == 84 ? size - 84 : size;
  return data == 409600 || data == 819200 || data == 1474560;
}

const char *Kind(const char *path, uint32_t size) {
  if (emu_machine() != BMMAC_II) return "disk";
  if (IsCd(path)) return "cd";
  if (IsFloppySize(size)) return "floppy";
  return "hard";
}

// ---- Whether a Mac can start from an image ----
//
// A disk a Mac starts from has boot blocks: its volume's first two bytes
// are "LK". They are at the start of a plain image (after DiskCopy 4.2's
// 84 bytes), or of an Apple_HFS partition of one with a partition map ("ER"
// at its start, then a "PM" entry a block for each partition: hard disks
// and most Mac CD-ROMs). Without them (an empty or data-only disk) the Mac
// passes the disk by and starts from another, as a real one does.

uint16_t Be16(const unsigned char *p) { return (uint16_t) (p[0] << 8 | p[1]); }
uint32_t Be32(const unsigned char *p) {
  return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

// Reads LEN bytes at POS of an open image; false if it can't.
bool ReadAt(sd_file *f, uint32_t pos, unsigned char *buf, unsigned len) {
  unsigned got = 0;
  return sd_seek(f, pos) == SD_OK && sd_read(f, buf, len, &got) == SD_OK && got == len;
}

bool HasBootBlocks(sd_file *f, uint32_t pos) {
  unsigned char lk[2];
  return ReadAt(f, pos, lk, 2) && lk[0] == 'L' && lk[1] == 'K';
}

bool Startable(const char *path, uint32_t size) {
  sd_file *f = sd_open(path, SD_READ);
  if (f == 0) return false;
  bool yes = false;
  uint32_t base = size % 512 == 84 ? 84 : 0;  // DiskCopy 4.2's header
  unsigned char block[512];
  if (HasBootBlocks(f, base)) {
    yes = true;
  } else if (ReadAt(f, 0, block, sizeof(block)) && block[0] == 'E' && block[1] == 'R') {
    // A partition map: its entries are a block apart (the map's own block
    // size: 512, or 2048 on some CD-ROMs).
    uint32_t bs = Be16(block + 2);
    if (bs != 512 && bs != 1024 && bs != 2048) bs = 512;
    uint32_t entries = 1;
    for (uint32_t i = 1; i <= entries && i <= 64 && !yes; i++) {
      unsigned char pm[96];
      if (!ReadAt(f, i * bs, pm, sizeof(pm)) || pm[0] != 'P' || pm[1] != 'M') break;
      if (i == 1) entries = Be32(pm + 4);
      char type[33];
      memcpy(type, pm + 48, 32);
      type[32] = '\0';
      if (strcmp(type, "Apple_HFS") == 0) {
        uint32_t start = Be32(pm + 8);
        if (start < size / bs && HasBootBlocks(f, start * bs)) yes = true;
      }
    }
  }
  sd_close(f);
  return yes;
}

struct Scan {
  Json *json;
  const char *folder;
  unsigned depth;
  unsigned count;
  bool more;
};

int ScanOne(void *ctx, const sd_info *info);

void ScanFolder(Json *json, const char *folder, unsigned depth, unsigned *count,
                bool *more) {
  Scan scan = {json, folder, depth, *count, *more};
  sd_list_info(folder, ScanOne, &scan);
  *count = scan.count;
  *more = scan.more;
}

int ScanOne(void *ctx, const sd_info *info) {
  Scan *s = (Scan *) ctx;
  if (info->name[0] == '.') return 0;  // the Mac's helpers, macOS's ._ files
  char path[600];
  int n = snprintf(path, sizeof(path), "%s/%s", s->folder, info->name);
  if (n <= 0 || (unsigned) n >= sizeof(path)) return 0;
  if (info->is_dir) {
    if (s->depth + 1 < kMaxDepth) {
      ScanFolder(s->json, path, s->depth + 1, &s->count, &s->more);
    }
    return s->more ? 1 : 0;
  }
  if (!IsImage(path)) return 0;
  if (s->count >= kMaxImages) {
    s->more = true;
    return 1;
  }
  s->json->Raw(s->count == 0 ? "{\"path\":" : ",{\"path\":");
  s->json->String(path);
  s->json->Printf(",\"size\":%lu,\"kind\":\"%s\",\"startable\":%s}",
                  (unsigned long) info->size, Kind(path, info->size),
                  Startable(path, info->size) ? "true" : "false");
  s->count++;
  return s->json->Full() ? 1 : 0;
}

// Percent-decodes a query parameter's value.
bool QueryValue(const char *query, const char *key, char *out, unsigned size) {
  size_t klen = strlen(key);
  for (const char *p = query; p != 0 && *p != '\0';) {
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char *v = p + klen + 1;
      unsigned n = 0;
      for (; *v != '\0' && *v != '&'; v++) {
        int c = (unsigned char) *v;
        if (c == '+') {
          c = ' ';
        } else if (c == '%' && v[1] != '\0' && v[2] != '\0') {
          char hex[3] = {v[1], v[2], '\0'};
          char *end;
          c = (int) strtol(hex, &end, 16);
          if (*end != '\0') return false;
          v += 2;
        }
        if (n + 1 >= size) return false;
        out[n++] = (char) c;
      }
      out[n] = '\0';
      return true;
    }
    p = strchr(p, '&');
    if (p != 0) p++;
  }
  return false;
}

// UTF-8 (as the page sends the names it was given) to the card's CP850.
bool Utf8ToCp850(const char *in, char *out, unsigned size) {
  unsigned n = 0;
  for (const unsigned char *p = (const unsigned char *) in; *p != 0;) {
    unsigned cp;
    int extra;
    if (*p < 0x80) {
      cp = *p;
      extra = 0;
    } else if ((*p & 0xE0) == 0xC0) {
      cp = *p & 0x1F;
      extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
      cp = *p & 0x0F;
      extra = 2;
    } else {
      return false;
    }
    p++;
    for (int i = 0; i < extra; i++, p++) {
      if ((*p & 0xC0) != 0x80) return false;
      cp = cp << 6 | (*p & 0x3F);
    }
    unsigned char b = Cp850FromUnicode(cp);
    if (b == 0 || n + 1 >= size) return false;
    out[n++] = (char) b;
  }
  out[n] = '\0';
  return true;
}

// A disk image in the disks folder: "/disks/...", no "..".
bool ValidImagePath(const char *path) {
  size_t n = strlen(kDisksFolder);
  if (strncmp(path, kDisksFolder, n) != 0 || path[n] != '/') return false;
  if (strstr(path, "/../") != 0 || strstr(path, "/./") != 0 ||
      strstr(path, "//") != 0) {
    return false;
  }
  size_t len = strlen(path);
  if (len >= 3 && strcmp(path + len - 3, "/..") == 0) return false;
  for (const unsigned char *p = (const unsigned char *) path; *p != 0; p++) {
    if (*p < 0x20 || strchr("\\:*?\"<>|", *p) != 0) return false;
  }
  return IsImage(path);
}

// A new disk image's name: one name, plain ASCII, nothing FAT can't have
// or would change (the extension is added).
bool ValidNewName(const char *name) {
  size_t n = strlen(name);
  if (n == 0 || n > 64 || name[0] == ' ' || name[0] == '.' ||
      name[n - 1] == ' ' || name[n - 1] == '.') {
    return false;
  }
  for (const unsigned char *p = (const unsigned char *) name; *p != 0; p++) {
    if (*p < 0x20 || *p >= 0x7F || strchr("/\\:*?\"<>|", *p) != 0) return false;
  }
  return true;
}

// Makes an empty disk image of SIZE bytes. Only its first 64 KB and its
// last block are written (zeros): the rest is grown without writing (FatFs
// grows a file when it is moved past its end), as writing 2 GB would take
// minutes. The zeros are where Mac OS looks for a volume, so it sees an
// empty disk and offers to initialize it, whatever the card held there
// before. SD_OK, or what went wrong (the file is removed).
int MakeBlankImage(const char *path, uint32_t size) {
  static const char zeros[4096] = {0};
  sd_file *f = sd_open(path, SD_WRITE);
  if (f == 0) return SD_ERROR;
  int rc = SD_OK;
  uint32_t head = size < 65536 ? size : 65536;
  for (uint32_t done = 0; rc == SD_OK && done < head; done += sizeof(zeros)) {
    uint32_t n = head - done < sizeof(zeros) ? head - done : sizeof(zeros);
    rc = sd_write(f, zeros, n);
  }
  if (rc == SD_OK && size > head) {
    uint32_t tail = size - 512 > head ? size - 512 : head;
    // On a full card FatFs stops short: the size is checked below.
    rc = sd_seek(f, tail);
    if (rc == SD_OK) rc = sd_write(f, zeros, size - tail);
  }
  if (sd_close(f) != SD_OK && rc == SD_OK) rc = SD_ERROR;
  if (rc == SD_OK) {
    sd_info info;
    if (sd_stat_info(path, &info) != SD_OK || info.size != size) rc = SD_ERROR;
  }
  if (rc != SD_OK) sd_unlink(path);
  return rc;
}

int Answer(char *out, unsigned size, int status, const char *message) {
  Json json(out, size);
  json.Raw(status < 300 ? "{\"ok\":true,\"message\":" : "{\"ok\":false,\"message\":");
  json.String(message);
  json.Raw("}");
  return status;
}

}  // namespace

int WebUiDisksJson(char *out, unsigned size) {
  static bmmac_disks disks;
  emu_disks(&disks);
  Json json(out, size);
  bool ii = emu_machine() == BMMAC_II;
  json.Printf("{\"machine\":\"%s\",\"hard\":[", ii ? "ii" : "plus");
  for (int i = 0; i < disks.hard_count; i++) {
    if (i > 0) json.Raw(",");
    json.String(disks.hard[i]);
  }
  json.Raw("],\"floppy\":[");
  json.String(disks.floppy[0]);
  json.Raw(",");
  json.String(disks.floppy[1]);
  json.Raw("],\"cdrom\":");
  json.String(disks.cdrom);
  json.Raw(",\"disk\":");
  json.String(disks.disk);

  int pending = 0;
  unsigned done = 0;
  const char *result = bmmac_disk_result(&pending, &done);
  json.Printf(",\"pending\":%s,\"done\":%u,\"result\":", pending ? "true" : "false",
              done);
  json.String(result);

  json.Raw(",\"images\":[");
  unsigned count = 0;
  bool more = false;
  ScanFolder(&json, kDisksFolder, 0, &count, &more);
  json.Printf("],\"more\":%s}", more ? "true" : "false");
  return json.Full() ? -1 : (int) json.Length();
}

// POST /api/disks?op=new&name=Work&kind=floppy|hard&size=BYTES: an empty
// disk image in the disks folder (Work.dsk or Work.hda), made here and now.
int NewImage(const char *query, char *out, unsigned size) {
  if (emu_machine() != BMMAC_II) {
    return Answer(out, size, 400,
                  "The Mac Plus starts from its one disk: an empty one is no use to it.");
  }
  char name[80], kind[16], size_text[16];
  if (!QueryValue(query, "name", name, sizeof(name)) || !ValidNewName(name)) {
    return Answer(out, size, 400,
                  "Give it a name of plain letters and numbers (up to 64), without "
                  "/ \\ : * ? \" < > |.");
  }
  if (!QueryValue(query, "kind", kind, sizeof(kind)) ||
      (strcmp(kind, "floppy") != 0 && strcmp(kind, "hard") != 0)) {
    return Answer(out, size, 400, "A floppy or a hard disk? (kind)");
  }
  char *end;
  if (!QueryValue(query, "size", size_text, sizeof(size_text))) {
    return Answer(out, size, 400, "How big? (size)");
  }
  unsigned long bytes = strtoul(size_text, &end, 10);
  bool floppy = strcmp(kind, "floppy") == 0;
  if (*end != '\0' ||
      (floppy && bytes != 819200 && bytes != 1474560) ||
      (!floppy && (bytes < 1048576 || bytes > 2048UL * 1048576 || bytes % 512 != 0))) {
    return Answer(out, size, 400, floppy ? "A floppy is 800K or 1.4 MB."
                                         : "A hard disk is 1 MB to 2 GB.");
  }
  uint32_t total_kb = 0, free_kb = 0;
  if (sd_space_kb(&total_kb, &free_kb) == SD_OK &&
      (unsigned long long) free_kb * 1024 < (unsigned long long) bytes + 1048576) {
    return Answer(out, size, 507, "There isn't room on the card for it.");
  }

  char path[160];
  snprintf(path, sizeof(path), "%s/%s.%s", kDisksFolder, name, floppy ? "dsk" : "hda");
  if (sd_stat_info(path, 0) == SD_OK) {
    return Answer(out, size, 409, "There is a disk image of that name already.");
  }
  sd_mkdir(kDisksFolder);
  if (MakeBlankImage(path, (uint32_t) bytes) != SD_OK) {
    return Answer(out, size, 507, "It couldn't be made: is the card full?");
  }
  Json json(out, size);
  json.Raw("{\"ok\":true,\"path\":");
  json.String(path);
  json.Raw(",\"message\":");
  char message[160];
  snprintf(message, sizeof(message), "%s.%s is made.", name, floppy ? "dsk" : "hda");
  json.String(message);
  json.Raw("}");
  return 201;
}

int WebUiDisksRequest(const char *query, char *out, unsigned size) {
  char op_text[16];
  if (!QueryValue(query, "op", op_text, sizeof(op_text))) {
    return Answer(out, size, 400, "Which change? (op)");
  }
  if (strcmp(op_text, "new") == 0) return NewImage(query, out, size);
  int op;
  if (strcmp(op_text, "start") == 0) op = BMMAC_DISK_START;
  else if (strcmp(op_text, "insert") == 0) op = BMMAC_DISK_INSERT;
  else if (strcmp(op_text, "add") == 0) op = BMMAC_DISK_ADD;
  else if (strcmp(op_text, "remove") == 0) op = BMMAC_DISK_REMOVE;
  else return Answer(out, size, 400, "Unknown change.");

  bool ii = emu_machine() == BMMAC_II;
  if (!ii && op != BMMAC_DISK_START) {
    return Answer(out, size, 400, "The Mac Plus has one disk: start it from one.");
  }

  char path[600] = "";
  int index = 0;
  if (op == BMMAC_DISK_REMOVE) {
    char text[16];
    char *end;
    if (!QueryValue(query, "index", text, sizeof(text))) {
      return Answer(out, size, 400, "Which hard disk? (index)");
    }
    index = (int) strtol(text, &end, 10);
    if (*end != '\0' || index < 0 || index >= BMMAC_HARD_DISKS) {
      return Answer(out, size, 400, "No such hard disk.");
    }
  } else {
    char utf8[600];
    if (!QueryValue(query, "path", utf8, sizeof(utf8)) ||
        !Utf8ToCp850(utf8, path, sizeof(path)) || !ValidImagePath(path)) {
      return Answer(out, size, 400, "Not a disk image in the disks folder.");
    }
    sd_info info;
    if (sd_stat_info(path, &info) != SD_OK || info.is_dir) {
      return Answer(out, size, 404, "That disk image isn't on the card.");
    }
    if (op == BMMAC_DISK_START && !Startable(path, info.size)) {
      return Answer(out, size, 409,
                    "The Mac can't start from that disk: it has no System on it "
                    "(no boot blocks). Insert or add it instead.");
    }
    if (op == BMMAC_DISK_INSERT && !IsCd(path) && !IsFloppySize(info.size)) {
      return Answer(out, size, 400,
                    "Only floppy (400K, 800K, 1.4 MB) and CD-ROM images go in a "
                    "drive: add this one as a hard disk.");
    }
  }
  if (!bmmac_disk_interrupt(op, path, index)) {
    return Answer(out, size, 409, "Another change is being made: try again.");
  }
  return Answer(out, size, 202, "On its way.");
}
