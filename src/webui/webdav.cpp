//
// webdav.cpp - the SD card as a network drive (WebDAV, RFC 4918).
//
// See webdav.h. What clients need of it:
//
// - OPTIONS, PROPFIND (Depth 0 and 1; infinity is taken as 1), GET, HEAD,
//   PUT, DELETE, MKCOL, COPY, MOVE, PROPPATCH, LOCK and UNLOCK.
// - Locks are granted but not enforced (class 2 in name only): macOS's
//   Finder only writes to a server that locks, and Windows asks for them
//   too, but the card has one user at a time. PROPPATCH is answered as
//   done and keeps nothing (Windows sets its file times that way).
// - Every response closes the connection. The server answers one
//   connection at a time, and macOS's Finder opens several at once: one
//   kept open would hold the others back.
// - PUT bodies come with a length or chunked (macOS's Finder sends
//   chunked), after "100 Continue" when the client asks for it (Windows,
//   curl). They go to a temporary file first, which replaces the file at
//   the end, so a transfer that breaks off leaves the file as it was.
// - Names on the card are FatFs's (CP850, see cp850.h); in URLs and
//   listings they are UTF-8.
// - The Mac II's SD Card disk keeps a file's type, creator and resource
//   fork in .finf and .rsrc folders next to it (src/port/ii/extfs_ii.cpp):
//   they are moved, copied and deleted with it.
// - What the web UI protects stays protected: the boot configuration files
//   (config.txt, cmdline.txt, wpa_supplicant.conf; changed in the web UI's
//   editor) and the Wi-Fi firmware folder can be read, not changed.
//
// Cross-site pages can't use this (with a browser's saved PIN): browsers
// send these methods to another site only after asking with OPTIONS, and
// the answer never allows it (no Access-Control-Allow-* headers). GET and
// HEAD only read.
//
// This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
// the Raspberry Pi. See LICENSE for licensing details.
//

#include "webdav.h"
#include "cp850.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sdcard/sd_fs.h"
#include "../port/transfer.h"

namespace {

// Paths on the card (sd_fs.h): a little more than a long name and a few
// folders, as the web UI has them.
const unsigned kPathMax = 560;
const unsigned kIoChunk = 32 * 1024;
const unsigned kXmlBodyMax = 8 * 1024;
// Folders deep a COPY goes (about 2 KB of the web UI task's 64 KB stack
// each); a deeper one is refused (MOVE and DELETE don't recurse here).
const unsigned kCopyDepthMax = 10;

// One request at a time (the web UI's task), so one buffer for file data.
unsigned char s_io[kIoChunk];
// And one for an XML request body.
char s_xml[kXmlBodyMax + 1];
unsigned s_lock_serial;

// ---- Small string helpers ----

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c; }

bool CiEqual(const char *a, const char *b) {
  for (; *a != '\0' && *b != '\0'; a++, b++) {
    if (Lower(*a) != Lower(*b)) return false;
  }
  return *a == *b;
}

bool CiPrefix(const char *s, const char *prefix) {
  for (; *prefix != '\0'; s++, prefix++) {
    if (Lower(*s) != Lower(*prefix)) return false;
  }
  return true;
}

// Finds a header's value (trimmed) in the request. False when it isn't
// there or doesn't fit.
bool GetHeader(const char *request, const char *name, char *out,
               unsigned out_size) {
  const char *p = strstr(request, "\r\n");
  size_t name_len = strlen(name);
  while (p != 0 && p[0] == '\r' && p[1] == '\n' && p[2] != '\r') {
    p += 2;
    if (CiPrefix(p, name) && p[name_len] == ':') {
      const char *v = p + name_len + 1;
      while (*v == ' ' || *v == '\t') v++;
      const char *end = strstr(v, "\r\n");
      if (end == 0) end = v + strlen(v);
      while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
      unsigned n = (unsigned) (end - v);
      if (n + 1 > out_size) return false;
      memcpy(out, v, n);
      out[n] = '\0';
      return true;
    }
    p = strstr(p, "\r\n");
  }
  return false;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---- Paths: URLs (UTF-8, percent-encoded) and the card's (CP850) ----

// The card path for a URL path (already without "http://host"), or false
// when it isn't the card's or isn't a name the card can have.
bool CardPath(const char *url, char *out, unsigned out_size) {
  size_t plen = strlen(WEBDAV_PREFIX);
  if (strncmp(url, WEBDAV_PREFIX, plen) != 0 ||
      (url[plen] != '\0' && url[plen] != '/' && url[plen] != '?')) {
    return false;
  }
  url += plen;

  // Percent-decode up to the query, and UTF-8 to CP850.
  char decoded[kPathMax];
  unsigned n = 0;
  // A fragment is the client's, never sent: one here is a bad request (and
  // must not stand for the folder before it).
  if (strchr(url, '#') != 0) return false;
  for (const char *p = url; *p != '\0' && *p != '?'; p++) {
    int c = (unsigned char) *p;
    if (c == '%') {
      int hi = HexDigit(p[1]), lo = hi < 0 ? -1 : HexDigit(p[2]);
      if (lo < 0) return false;
      c = hi << 4 | lo;
      p += 2;
    }
    if (n + 1 >= sizeof(decoded)) return false;
    decoded[n++] = (char) c;
  }
  decoded[n] = '\0';

  char cp850[kPathMax];
  unsigned m = 0;
  for (const unsigned char *p = (const unsigned char *) decoded; *p != 0;) {
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
      return false;  // nothing beyond the BMP is in CP850
    }
    p++;
    for (int i = 0; i < extra; i++, p++) {
      if ((*p & 0xC0) != 0x80) return false;
      cp = cp << 6 | (*p & 0x3F);
    }
    unsigned char b = Cp850FromUnicode(cp);
    if (b == 0) return false;
    if (m + 1 >= sizeof(cp850)) return false;
    cp850[m++] = (char) b;
  }
  cp850[m] = '\0';

  // One "/" between names, no "." or "..", nothing FAT can't have.
  if (out_size < 2) return false;
  unsigned o = 0;
  out[o++] = '/';
  const char *p = cp850;
  while (*p != '\0') {
    while (*p == '/') p++;
    const char *seg = p;
    while (*p != '\0' && *p != '/') p++;
    unsigned len = (unsigned) (p - seg);
    if (len == 0 || (len == 1 && seg[0] == '.')) continue;
    if (len == 2 && seg[0] == '.' && seg[1] == '.') return false;
    if (len > SD_MAX_NAME_LEN) return false;
    for (unsigned i = 0; i < len; i++) {
      unsigned char c = (unsigned char) seg[i];
      if (c < 0x20 || strchr("\\:*?\"<>|", c) != 0) return false;
    }
    // FatFs would drop a trailing dot or space and make another name.
    if (seg[len - 1] == '.' || seg[len - 1] == ' ') return false;
    if (o > 1) {
      if (o + 1 >= out_size) return false;
      out[o++] = '/';
    }
    if (o + len >= out_size) return false;
    memcpy(out + o, seg, len);
    o += len;
  }
  out[o] = '\0';
  return true;
}

// The URL path in a Destination header ("http://host/sd/..." or
// "/sd/...").
const char *DestinationPath(const char *dest) {
  const char *scheme = strstr(dest, "://");
  if (scheme == 0) return dest;
  const char *path = strchr(scheme + 3, '/');
  return path != 0 ? path : "/";
}

bool IsRoot(const char *path) { return strcmp(path, "/") == 0; }

const char *BaseName(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash != 0 ? slash + 1 : path;
}

// The parent folder of path ("/" for one in the root).
void ParentOf(const char *path, char *out, unsigned out_size) {
  const char *slash = strrchr(path, '/');
  unsigned n = slash != 0 ? (unsigned) (slash - path) : 0;
  if (n == 0) {
    snprintf(out, out_size, "/");
    return;
  }
  if (n >= out_size) n = out_size - 1;
  memcpy(out, path, n);
  out[n] = '\0';
}

// path's helper next to it: ".finf" -> /dir/.finf/name.
bool HelperPath(const char *path, const char *helper, char *out,
                unsigned out_size) {
  char parent[kPathMax];
  ParentOf(path, parent, sizeof(parent));
  int n = snprintf(out, out_size, "%s/%s/%s", IsRoot(parent) ? "" : parent,
                   helper, BaseName(path));
  return n > 0 && (unsigned) n < out_size;
}

// The boot configuration and the Wi-Fi firmware: the web UI's editor
// changes the one, nothing changes the other (webui_fs.cpp's rules).
bool IsProtected(const char *path) {
  const char *seg = path;
  while (*seg == '/') seg++;
  if (CiPrefix(seg, "firmware") && (seg[8] == '\0' || seg[8] == '/')) {
    return true;
  }
  const char *base = BaseName(path);
  return CiEqual(base, "config.txt") || CiEqual(base, "cmdline.txt") ||
         CiEqual(base, "wpa_supplicant.conf");
}

// Whether path is, or is inside, folder.
bool Within(const char *path, const char *folder) {
  if (IsRoot(folder)) return true;
  size_t n = strlen(folder);
  return strncmp(path, folder, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

bool Stat(const char *path, sd_info *info) {
  if (IsRoot(path)) {
    memset(info, 0, sizeof(*info));
    info->is_dir = 1;
    return true;
  }
  return sd_stat_info(path, info) == SD_OK;
}

// ---- Responses ----

const char *Reason(int status) {
  switch (status) {
  case 100: return "Continue";
  case 200: return "OK";
  case 201: return "Created";
  case 204: return "No Content";
  case 207: return "Multi-Status";
  case 400: return "Bad Request";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 412: return "Precondition Failed";
  case 413: return "Payload Too Large";
  case 415: return "Unsupported Media Type";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 507: return "Insufficient Storage";
  default: return "Internal Server Error";
  }
}

// A response written in pieces: a buffer at a time, chunked when its
// length isn't known first.
class Response {
public:
  explicit Response(CWebDavConnection *c) : m_c(c), m_len(0), m_chunked(false),
                                            m_failed(false) {}

  // extra: more header lines, each ending in "\r\n" (or 0). length < 0:
  // chunked.
  void Begin(int status, const char *content_type, long length,
             const char *extra) {
    char header[768];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Server: BMMacintosh\r\n"
                     "Connection: close\r\n"
                     "%s%s%s"
                     "%s",
                     status, Reason(status),
                     content_type != 0 ? "Content-Type: " : "",
                     content_type != 0 ? content_type : "",
                     content_type != 0 ? "\r\n" : "", extra != 0 ? extra : "");
    if (n <= 0 || (unsigned) n >= sizeof(header)) {
      m_failed = true;
      return;
    }
    char tail[64];
    int t;
    if (length < 0) {
      m_chunked = true;
      t = snprintf(tail, sizeof(tail), "Transfer-Encoding: chunked\r\n\r\n");
    } else {
      t = snprintf(tail, sizeof(tail), "Content-Length: %ld\r\n\r\n", length);
    }
    if (!m_c->Send(header, (unsigned) n) || !m_c->Send(tail, (unsigned) t)) {
      m_failed = true;
    }
  }

  void Write(const void *data, unsigned len) {
    const char *p = (const char *) data;
    while (len > 0 && !m_failed) {
      unsigned space = sizeof(m_buf) - m_len;
      unsigned n = len < space ? len : space;
      memcpy(m_buf + m_len, p, n);
      m_len += n;
      p += n;
      len -= n;
      if (m_len == sizeof(m_buf)) Flush();
    }
  }

  void Text(const char *s) { Write(s, (unsigned) strlen(s)); }

  void Printf(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n < 0) return;
    if ((unsigned) n >= sizeof(line)) n = sizeof(line) - 1;
    Write(line, (unsigned) n);
  }

  // XML text: UTF-8 from CP850, with & < > " escaped.
  void XmlText(const char *cp850) {
    for (const unsigned char *p = (const unsigned char *) cp850; *p; p++) {
      if (*p == '&') Text("&amp;");
      else if (*p == '<') Text("&lt;");
      else if (*p == '>') Text("&gt;");
      else if (*p == '"') Text("&quot;");
      else Utf8(*p);
    }
  }

  // A path as a URL: each name's UTF-8, percent-encoded.
  void Href(const char *path, bool folder) {
    Text(WEBDAV_PREFIX);
    for (const unsigned char *p = (const unsigned char *) path; *p; p++) {
      if (*p == '/') {
        Text("/");
        continue;
      }
      char utf8[4];
      unsigned n = Utf8Bytes(*p, utf8);
      for (unsigned i = 0; i < n; i++) {
        unsigned char c = (unsigned char) utf8[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || strchr("-._~!$'()*+,;=@", c) != 0) {
          Write(&utf8[i], 1);
        } else {
          Printf("%%%02X", c);
        }
      }
    }
    // The root's "/" is written above already.
    if (folder && !IsRoot(path)) Text("/");
  }

  void Finish(void) {
    Flush();
    if (m_chunked && !m_failed) {
      m_failed = !m_c->Send("0\r\n\r\n", 5);
    }
  }

  bool Failed(void) const { return m_failed; }

private:
  static unsigned Utf8Bytes(unsigned char cp850, char *out) {
    unsigned cp = cp850 < 0x80 ? cp850 : kCp850High[cp850 - 0x80];
    if (cp < 0x80) {
      out[0] = (char) cp;
      return 1;
    }
    if (cp < 0x800) {
      out[0] = (char) (0xC0 | (cp >> 6));
      out[1] = (char) (0x80 | (cp & 0x3F));
      return 2;
    }
    out[0] = (char) (0xE0 | (cp >> 12));
    out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char) (0x80 | (cp & 0x3F));
    return 3;
  }

  void Utf8(unsigned char cp850) {
    char out[4];
    Write(out, Utf8Bytes(cp850, out));
  }

  void Flush(void) {
    if (m_len == 0 || m_failed) {
      m_len = 0;
      return;
    }
    if (m_chunked) {
      char size[16];
      int n = snprintf(size, sizeof(size), "%x\r\n", m_len);
      m_failed = !m_c->Send(size, (unsigned) n) || !m_c->Send(m_buf, m_len) ||
                 !m_c->Send("\r\n", 2);
    } else {
      m_failed = !m_c->Send(m_buf, m_len);
    }
    m_len = 0;
  }

  CWebDavConnection *m_c;
  char m_buf[4096];
  unsigned m_len;
  bool m_chunked;
  bool m_failed;
};

void SendStatus(CWebDavConnection *c, int status, const char *extra = 0) {
  char body[96];
  int n = snprintf(body, sizeof(body), "%d %s\n", status, Reason(status));
  Response r(c);
  r.Begin(status, status == 204 || status == 201 ? 0 : "text/plain; charset=utf-8",
          status == 204 ? 0 : n, extra);
  if (status != 204) r.Write(body, (unsigned) n);
  r.Finish();
}

int StatusFor(int sd_result) {
  switch (sd_result) {
  case SD_NOT_FOUND: return 404;
  case SD_EXISTS: return 412;
  case SD_DENIED: return 403;
  case SD_INVALID: return 400;
  default: return 500;
  }
}

// ---- Request bodies: a length, chunked, or none ----

class Body {
public:
  Body(CWebDavConnection *c, const char *request, const unsigned char *pre,
       unsigned pre_len)
      : m_c(c), m_pre(pre), m_pre_len(pre_len), m_chunked(false),
        m_left(0), m_chunk_left(0), m_done(false), m_error(false) {
    char value[64];
    if (GetHeader(request, "Transfer-Encoding", value, sizeof(value)) &&
        CiEqual(value, "chunked")) {
      m_chunked = true;
    } else if (GetHeader(request, "Content-Length", value, sizeof(value))) {
      m_left = strtoul(value, 0, 10);
    }
    m_left_start = m_left;
    m_done = !m_chunked && m_left == 0;
    m_continue = GetHeader(request, "Expect", value, sizeof(value)) &&
                 CiEqual(value, "100-continue");
  }

  bool Empty(void) const { return m_done; }

  // The body's length, or -1 when it isn't known (chunked).
  long long Length(void) const { return m_chunked ? -1 : (long long) m_left_start; }

  // Tells a client waiting for "100 Continue" to send the body. Once.
  bool Go(void) {
    if (m_continue && !m_done) {
      m_continue = false;
      static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
      return m_c->Send(kContinue, sizeof(kContinue) - 1);
    }
    return true;
  }

  // Bytes of the body read into buf: 0 at its end, -1 when the client
  // broke off or the chunks don't parse.
  int Read(void *buf, unsigned len) {
    if (m_error) return -1;
    if (m_done || len == 0) return 0;
    if (!Go()) return Fail();
    if (!m_chunked) {
      unsigned want = m_left < len ? (unsigned) m_left : len;
      int n = Raw(buf, want);
      if (n <= 0) return Fail();
      m_left -= (unsigned) n;
      if (m_left == 0) m_done = true;
      return n;
    }
    if (m_chunk_left == 0) {
      char line[40];
      if (!Line(line, sizeof(line))) return Fail();
      unsigned long size = strtoul(line, 0, 16);
      if (size == 0) {
        // Trailers, to the blank line.
        for (;;) {
          if (!Line(line, sizeof(line))) return Fail();
          if (line[0] == '\0') break;
        }
        m_done = true;
        return 0;
      }
      m_chunk_left = size;
    }
    unsigned want = m_chunk_left < len ? (unsigned) m_chunk_left : len;
    int n = Raw(buf, want);
    if (n <= 0) return Fail();
    m_chunk_left -= (unsigned) n;
    if (m_chunk_left == 0) {
      char crlf[2];
      if (RawAll(crlf, 2) != 2 || crlf[0] != '\r' || crlf[1] != '\n') return Fail();
    }
    return n;
  }

  // The whole body into buf (NUL-terminated); false if it doesn't fit.
  bool ReadAll(char *buf, unsigned size, unsigned *got) {
    unsigned n = 0;
    for (;;) {
      if (n + 1 >= size) {
        char spill;
        if (Read(&spill, 1) != 0) return false;
        break;
      }
      int r = Read(buf + n, size - 1 - n);
      if (r < 0) return false;
      if (r == 0) break;
      n += (unsigned) r;
    }
    buf[n] = '\0';
    *got = n;
    return true;
  }

  // Reads to the end (to keep the connection's state clean).
  void Discard(void) {
    char sink[512];
    while (Read(sink, sizeof(sink)) > 0) {
    }
  }

private:
  int Fail(void) {
    m_error = true;
    return -1;
  }

  // Some bytes: what came with the headers first, then the socket.
  int Raw(void *buf, unsigned len) {
    if (m_pre_len > 0) {
      unsigned n = m_pre_len < len ? m_pre_len : len;
      memcpy(buf, m_pre, n);
      m_pre += n;
      m_pre_len -= n;
      return (int) n;
    }
    return m_c->Receive(buf, len);
  }

  int RawAll(void *buf, unsigned len) {
    unsigned got = 0;
    while (got < len) {
      int n = Raw((char *) buf + got, len - got);
      if (n <= 0) return -1;
      got += (unsigned) n;
    }
    return (int) got;
  }

  // One line of the chunked framing, without its CRLF.
  bool Line(char *out, unsigned size) {
    unsigned n = 0;
    for (;;) {
      char c;
      if (RawAll(&c, 1) != 1) return false;
      if (c == '\n') break;
      if (c == '\r') continue;
      if (n + 1 >= size) return false;
      out[n++] = c;
    }
    out[n] = '\0';
    return true;
  }

  CWebDavConnection *m_c;
  const unsigned char *m_pre;
  unsigned m_pre_len;
  bool m_chunked;
  unsigned long m_left;
  unsigned long m_left_start;
  unsigned long m_chunk_left;
  bool m_done;
  bool m_error;
  bool m_continue;
};

// ---- Properties ----

const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Day of the week (0 Sunday) for a date (Sakamoto's).
int Weekday(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// The card's times are the Pi's local time, with no zone: given as GMT.
void HttpDate(const sd_time *t, char *out, unsigned size) {
  static const char *const kDays[] = {"Sun", "Mon", "Tue", "Wed",
                                      "Thu", "Fri", "Sat"};
  if (t->year == 0 || t->month < 1 || t->month > 12) {
    snprintf(out, size, "Tue, 01 Jan 1980 00:00:00 GMT");
    return;
  }
  snprintf(out, size, "%s, %02u %s %04u %02u:%02u:%02u GMT",
           kDays[Weekday(t->year, t->month, t->day)], t->day,
           kMonths[t->month - 1], t->year, t->hour, t->minute, t->second);
}

void IsoDate(const sd_time *t, char *out, unsigned size) {
  if (t->year == 0) {
    snprintf(out, size, "1980-01-01T00:00:00Z");
    return;
  }
  snprintf(out, size, "%04u-%02u-%02uT%02u:%02u:%02uZ", t->year, t->month,
           t->day, t->hour, t->minute, t->second);
}

void ETag(const sd_info *info, char *out, unsigned size) {
  const sd_time *t = &info->mtime;
  snprintf(out, size, "\"%lx-%04u%02u%02u%02u%02u%02u\"",
           (unsigned long) info->size, t->year, t->month, t->day, t->hour,
           t->minute, t->second);
}

const char *ContentType(const char *name) {
  const char *dot = strrchr(name, '.');
  if (dot == 0) return "application/octet-stream";
  static const char *const kTypes[][2] = {
      {".txt", "text/plain"},     {".htm", "text/html"},
      {".html", "text/html"},     {".css", "text/css"},
      {".js", "text/javascript"}, {".json", "application/json"},
      {".png", "image/png"},      {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"},    {".gif", "image/gif"},
      {".pdf", "application/pdf"}, {".zip", "application/zip"},
  };
  for (unsigned i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) {
    if (CiEqual(dot, kTypes[i][0])) return kTypes[i][1];
  }
  return "application/octet-stream";
}

bool Contains(const char *xml, const char *local);

// The properties this server has, in DAV:.
enum Prop {
  P_RESOURCETYPE,
  P_CONTENTLENGTH,
  P_LASTMODIFIED,
  P_CREATIONDATE,
  P_DISPLAYNAME,
  P_CONTENTTYPE,
  P_ETAG,
  P_SUPPORTEDLOCK,
  P_LOCKDISCOVERY,
  P_QUOTA_AVAILABLE,
  P_QUOTA_USED,
  P_COUNT
};

const char *const kPropNames[P_COUNT] = {
    "resourcetype",   "getcontentlength", "getlastmodified",
    "creationdate",   "displayname",      "getcontenttype",
    "getetag",        "supportedlock",    "lockdiscovery",
    "quota-available-bytes", "quota-used-bytes",
};

// What a PROPFIND asks for.
struct PropRequest {
  bool all;        // allprop (or no body)
  bool names;      // propname
  bool want[P_COUNT];
  // Asked for and not here (another namespace, or a DAV: one this server
  // hasn't): answered 404.
  unsigned unknown_count;
  char unknown_ns[16][96];
  char unknown_name[16][64];
};

// The namespace a prefix stands for in an XML text: the nearest
// declaration before "at" (good enough for clients' request bodies).
void ResolveNamespace(const char *xml, const char *at, const char *prefix,
                      unsigned prefix_len, char *out, unsigned size) {
  char attr[48];
  if (prefix_len == 0) {
    snprintf(attr, sizeof(attr), "xmlns=\"");
  } else {
    snprintf(attr, sizeof(attr), "xmlns:%.*s=\"", (int) prefix_len, prefix);
  }
  const char *found = 0;
  for (const char *p = strstr(xml, attr); p != 0; p = strstr(p + 1, attr)) {
    // A declaration in the element itself counts too (it comes after "<").
    const char *tag_end = strchr(at, '>');
    if (p > at && (tag_end == 0 || p > tag_end)) break;
    found = p + strlen(attr);
  }
  out[0] = '\0';
  if (found != 0) {
    const char *end = strchr(found, '"');
    unsigned n = end != 0 ? (unsigned) (end - found) : 0;
    if (n >= size) n = size - 1;
    memcpy(out, found, n);
    out[n] = '\0';
  }
}

// The local name of the element at "<", and where its prefix is.
bool ElementName(const char *lt, const char **prefix, unsigned *prefix_len,
                 char *name, unsigned size) {
  const char *p = lt + 1;
  if (*p == '/' || *p == '?' || *p == '!') return false;
  const char *start = p;
  while (*p != '\0' && *p != '>' && *p != '/' && *p != ' ' && *p != '\t' &&
         *p != '\r' && *p != '\n') {
    p++;
  }
  const char *colon = 0;
  for (const char *q = start; q < p; q++) {
    if (*q == ':') colon = q;
  }
  const char *local = colon != 0 ? colon + 1 : start;
  *prefix = start;
  *prefix_len = colon != 0 ? (unsigned) (colon - start) : 0;
  unsigned n = (unsigned) (p - local);
  if (n == 0 || n >= size) return false;
  memcpy(name, local, n);
  name[n] = '\0';
  return true;
}

// Whether an XML request body holds together: one element at the top,
// each end tag closing the last element opened. (Enough to refuse a broken
// body rather than half-read it; the rest isn't checked.)
bool WellFormed(const char *xml) {
  char stack[32][80];
  int depth = 0;
  bool any = false;
  for (const char *p = strchr(xml, '<'); p != 0; p = strchr(p + 1, '<')) {
    if (p[1] == '?') {
      p = strstr(p, "?>");
      if (p == 0) return false;
      continue;
    }
    if (strncmp(p, "<!--", 4) == 0) {
      p = strstr(p, "-->");
      if (p == 0) return false;
      continue;
    }
    const char *gt = strchr(p, '>');
    if (gt == 0) return false;
    bool end = p[1] == '/';
    const char *n = p + (end ? 2 : 1);
    const char *q = n;
    while (q < gt && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n' &&
           *q != '/') {
      q++;
    }
    unsigned len = (unsigned) (q - n);
    if (len == 0 || len >= sizeof(stack[0])) return false;
    if (end) {
      if (depth == 0 || strncmp(stack[depth - 1], n, len) != 0 ||
          stack[depth - 1][len] != '\0') {
        return false;
      }
      depth--;
      continue;
    }
    if (depth == 0 && any) return false;  // a second top element
    any = true;
    if (gt[-1] == '/') continue;
    if (depth == 32) return false;
    memcpy(stack[depth], n, len);
    stack[depth][len] = '\0';
    depth++;
  }
  return depth == 0;
}

// Calls fn(lt, prefix, prefix_len, name) for each element directly inside
// a <prop> element (any namespace's prefix), in order.
template <typename F>
void ForEachPropChild(const char *xml, F fn) {
  int depth = 0;
  int prop_depth = -1;
  for (const char *p = strchr(xml, '<'); p != 0; p = strchr(p + 1, '<')) {
    if (p[1] == '?' || p[1] == '!') continue;
    const char *gt = strchr(p, '>');
    if (gt == 0) return;
    if (p[1] == '/') {
      depth--;
      if (depth == prop_depth) prop_depth = -1;
      continue;
    }
    char name[64];
    const char *prefix;
    unsigned prefix_len;
    if (!ElementName(p, &prefix, &prefix_len, name, sizeof(name))) continue;
    bool empty = gt[-1] == '/';
    if (prop_depth >= 0 && depth == prop_depth + 1) {
      fn(p, prefix, prefix_len, name);
    } else if (prop_depth < 0 && strcmp(name, "prop") == 0 && !empty) {
      prop_depth = depth;
    }
    if (!empty) depth++;
  }
}

void ParsePropfind(const char *xml, PropRequest *req) {
  memset(req, 0, sizeof(*req));
  if (Contains(xml, "propname")) {
    req->names = true;
    return;
  }
  bool any = false;
  ForEachPropChild(xml, [&](const char *lt, const char *prefix,
                            unsigned prefix_len, const char *name) {
    any = true;
    char ns[96];
    ResolveNamespace(xml, lt, prefix, prefix_len, ns, sizeof(ns));
    bool known = false;
    if (strcmp(ns, "DAV:") == 0) {
      for (int i = 0; i < P_COUNT; i++) {
        if (strcmp(name, kPropNames[i]) == 0) {
          req->want[i] = true;
          known = true;
        }
      }
    }
    if (!known && req->unknown_count < 16) {
      unsigned i = req->unknown_count++;
      snprintf(req->unknown_ns[i], sizeof(req->unknown_ns[i]), "%s", ns);
      snprintf(req->unknown_name[i], sizeof(req->unknown_name[i]), "%s", name);
    }
  });
  // allprop, an empty body, or one this doesn't follow: everything.
  if (!any) req->all = true;
}

void XmlAttrText(Response *r, const char *s) {
  for (; *s != '\0'; s++) {
    if (*s == '&') r->Text("&amp;");
    else if (*s == '<') r->Text("&lt;");
    else if (*s == '"') r->Text("&quot;");
    else r->Write(s, 1);
  }
}

// An empty property of another namespace (or none: xmlns="", as a prefix
// can't be bound to no namespace).
void WriteForeignProp(Response *r, unsigned i, const char *ns, const char *name) {
  if (ns[0] == '\0') {
    r->Printf("<%s xmlns=\"\"/>", name);
    return;
  }
  r->Printf("<x%u:%s xmlns:x%u=\"", i, name, i);
  XmlAttrText(r, ns);
  r->Text("\"/>");
}

void WriteProp(Response *r, int prop, const char *path, const sd_info *info,
               bool value) {
  if (!value) {
    r->Printf("<D:%s/>", kPropNames[prop]);
    return;
  }
  char text[64];
  switch (prop) {
  case P_RESOURCETYPE:
    r->Text(info->is_dir ? "<D:resourcetype><D:collection/></D:resourcetype>"
                         : "<D:resourcetype/>");
    break;
  case P_CONTENTLENGTH:
    r->Printf("<D:getcontentlength>%lu</D:getcontentlength>",
              (unsigned long) info->size);
    break;
  case P_LASTMODIFIED:
    HttpDate(&info->mtime, text, sizeof(text));
    r->Printf("<D:getlastmodified>%s</D:getlastmodified>", text);
    break;
  case P_CREATIONDATE:
    IsoDate(&info->mtime, text, sizeof(text));
    r->Printf("<D:creationdate>%s</D:creationdate>", text);
    break;
  case P_DISPLAYNAME:
    r->Text("<D:displayname>");
    r->XmlText(IsRoot(path) ? "SD card" : BaseName(path));
    r->Text("</D:displayname>");
    break;
  case P_CONTENTTYPE:
    r->Printf("<D:getcontenttype>%s</D:getcontenttype>",
              info->is_dir ? "httpd/unix-directory" : ContentType(path));
    break;
  case P_ETAG:
    ETag(info, text, sizeof(text));
    r->Text("<D:getetag>");
    XmlAttrText(r, text);
    r->Text("</D:getetag>");
    break;
  case P_SUPPORTEDLOCK:
    r->Text("<D:supportedlock>"
            "<D:lockentry><D:lockscope><D:exclusive/></D:lockscope>"
            "<D:locktype><D:write/></D:locktype></D:lockentry>"
            "<D:lockentry><D:lockscope><D:shared/></D:lockscope>"
            "<D:locktype><D:write/></D:locktype></D:lockentry>"
            "</D:supportedlock>");
    break;
  case P_LOCKDISCOVERY:
    r->Text("<D:lockdiscovery/>");
    break;
  case P_QUOTA_AVAILABLE:
  case P_QUOTA_USED: {
    uint32_t total_kb = 0, free_kb = 0;
    sd_space_kb(&total_kb, &free_kb);
    unsigned long long bytes =
        (unsigned long long) (prop == P_QUOTA_AVAILABLE ? free_kb
                                                        : total_kb - free_kb) * 1024;
    r->Printf("<D:%s>%llu</D:%s>", kPropNames[prop], bytes, kPropNames[prop]);
    break;
  }
  }
}

// A property this server has for this resource (a folder has no length,
// type or ETag; quotas are asked for by name only, as reading the card's
// free space can take a while).
bool HasProp(int prop, const sd_info *info, bool all) {
  if (info->is_dir && (prop == P_CONTENTLENGTH || prop == P_ETAG)) return false;
  if (all && (prop == P_QUOTA_AVAILABLE || prop == P_QUOTA_USED)) return false;
  if (!info->is_dir && (prop == P_QUOTA_AVAILABLE || prop == P_QUOTA_USED)) {
    return false;
  }
  return true;
}

void WriteResponse(Response *r, const char *path, const sd_info *info,
                   const PropRequest *req) {
  r->Text("<D:response><D:href>");
  r->Href(path, info->is_dir != 0);
  r->Text("</D:href><D:propstat><D:prop>");
  bool missing = false;
  for (int i = 0; i < P_COUNT; i++) {
    bool asked = req->all || req->names || req->want[i];
    if (!asked) continue;
    if (!HasProp(i, info, req->all || req->names)) {
      if (req->want[i]) missing = true;
      continue;
    }
    WriteProp(r, i, path, info, !req->names);
  }
  r->Text("</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>");
  if (missing || req->unknown_count > 0) {
    r->Text("<D:propstat><D:prop>");
    for (int i = 0; i < P_COUNT; i++) {
      if (req->want[i] && !HasProp(i, info, false)) {
        r->Printf("<D:%s/>", kPropNames[i]);
      }
    }
    for (unsigned i = 0; i < req->unknown_count; i++) {
      WriteForeignProp(r, i, req->unknown_ns[i], req->unknown_name[i]);
    }
    r->Text("</D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>");
  }
  r->Text("</D:response>\n");
}

// ---- The methods ----

struct ListContext {
  Response *r;
  const char *folder;
  const PropRequest *req;
  CWebDavConnection *c;
  unsigned count;
};

int ListOne(void *ctx, const sd_info *info) {
  ListContext *l = (ListContext *) ctx;
  char path[kPathMax];
  int n = snprintf(path, sizeof(path), "%s/%s", IsRoot(l->folder) ? "" : l->folder,
                   info->name);
  if (n <= 0 || (unsigned) n >= sizeof(path)) return 0;
  WriteResponse(l->r, path, info, l->req);
  if (++l->count % 32 == 0) l->c->Yield();
  return l->r->Failed() ? 1 : 0;
}

void Propfind(CWebDavConnection *c, const char *path, const char *request,
              Body *body) {
  char depth[16] = "1";
  GetHeader(request, "Depth", depth, sizeof(depth));
  unsigned got = 0;
  if (!body->ReadAll(s_xml, sizeof(s_xml), &got)) {
    SendStatus(c, 413);
    return;
  }
  if (got > 0 && !WellFormed(s_xml)) {
    SendStatus(c, 400);
    return;
  }
  sd_info info;
  if (!Stat(path, &info)) {
    SendStatus(c, 404);
    return;
  }
  static PropRequest req;
  ParsePropfind(s_xml, &req);

  Response r(c);
  r.Begin(207, "application/xml; charset=utf-8", -1, 0);
  r.Text("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
         "<D:multistatus xmlns:D=\"DAV:\">\n");
  WriteResponse(&r, path, &info, &req);
  if (info.is_dir && strcmp(depth, "0") != 0) {
    ListContext l = {&r, path, &req, c, 0};
    sd_list_info(path, ListOne, &l);
  }
  r.Text("</D:multistatus>\n");
  r.Finish();
}

void Proppatch(CWebDavConnection *c, const char *path, Body *body) {
  unsigned got = 0;
  if (!body->ReadAll(s_xml, sizeof(s_xml), &got)) {
    SendStatus(c, 413);
    return;
  }
  if (got > 0 && !WellFormed(s_xml)) {
    SendStatus(c, 400);
    return;
  }
  sd_info info;
  if (!Stat(path, &info)) {
    SendStatus(c, 404);
    return;
  }
  // Every property set or removed is "done" (none is kept).
  Response r(c);
  r.Begin(207, "application/xml; charset=utf-8", -1, 0);
  r.Text("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
         "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
  r.Href(path, info.is_dir != 0);
  r.Text("</D:href><D:propstat><D:prop>");
  unsigned i = 0;
  ForEachPropChild(s_xml, [&](const char *lt, const char *prefix,
                              unsigned prefix_len, const char *name) {
    if (i >= 64) return;
    char ns[96];
    ResolveNamespace(s_xml, lt, prefix, prefix_len, ns, sizeof(ns));
    WriteForeignProp(&r, i, ns, name);
    i++;
  });
  r.Text("</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
         "</D:response></D:multistatus>\n");
  r.Finish();
}

// An HTML page of a folder, for a browser.
int HtmlOne(void *ctx, const sd_info *info) {
  ListContext *l = (ListContext *) ctx;
  // The Mac's helpers (.finf, .rsrc) and macOS's ._ files: not for a
  // browser (file managers still have them).
  if (info->name[0] == '.') return 0;
  char path[kPathMax];
  int n = snprintf(path, sizeof(path), "%s/%s", IsRoot(l->folder) ? "" : l->folder,
                   info->name);
  if (n <= 0 || (unsigned) n >= sizeof(path)) return 0;
  l->r->Text(info->is_dir ? "<li class=d><a href=\"" : "<li><a href=\"");
  l->r->Href(path, info->is_dir != 0);
  l->r->Text("\">");
  l->r->XmlText(info->name);
  if (info->is_dir) {
    l->r->Text("/</a></li>\n");
  } else {
    unsigned long kb = (info->size + 1023) / 1024;
    if (kb < 1024) {
      l->r->Printf("</a><span>%lu KB</span></li>\n", kb);
    } else {
      l->r->Printf("</a><span>%lu.%lu MB</span></li>\n", kb / 1024, kb % 1024 * 10 / 1024);
    }
  }
  if (++l->count % 32 == 0) l->c->Yield();
  return l->r->Failed() ? 1 : 0;
}

void Get(CWebDavConnection *c, const char *path, bool head) {
  sd_info info;
  if (!Stat(path, &info)) {
    SendStatus(c, 404);
    return;
  }
  if (info.is_dir) {
    Response r(c);
    r.Begin(200, "text/html; charset=utf-8", -1, 0);
    if (!head) {
      // A page for a browser (file managers use PROPFIND).
      r.Text("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
             "<title>SD card");
      if (!IsRoot(path)) {
        r.Text(": ");
        r.XmlText(path);
      }
      r.Text("</title><style>"
             ":root{color-scheme:light dark}"
             "body{font:15px/1.5 -apple-system,Segoe UI,Roboto,Ubuntu,sans-serif;"
             "max-width:760px;margin:0 auto;padding:28px 20px}"
             "h1{font-size:22px;margin:0 0 4px}p{margin:0 0 18px;color:#888}"
             "ul{list-style:none;margin:0;padding:0}"
             "li{display:flex;justify-content:space-between;gap:16px;padding:8px 2px;"
             "border-top:1px solid rgba(128,128,128,.25)}"
             "li span{color:#888;white-space:nowrap}"
             "li.d a{font-weight:600}a{text-decoration:none}a:hover{text-decoration:underline}"
             "</style></head><body><h1>SD card");
      if (!IsRoot(path)) {
        r.Text(": ");
        r.XmlText(path);
      }
      r.Text("</h1><p>BMMacintosh's network drive: open this address in your "
             "file manager to copy files. <a href=\"/\">The web page</a></p><ul>\n");
      if (!IsRoot(path)) {
        char parent[kPathMax];
        ParentOf(path, parent, sizeof(parent));
        r.Text("<li class=d><a href=\"");
        r.Href(parent, true);
        r.Text("\">&#8592; Up</a></li>\n");
      }
      ListContext l = {&r, path, 0, c, 0};
      sd_list_info(path, HtmlOne, &l);
      r.Text("</ul></body></html>\n");
    }
    r.Finish();
    return;
  }

  sd_file *f = sd_open(path, SD_READ);
  if (f == 0) {
    SendStatus(c, 404);
    return;
  }
  char extra[160];
  char date[48], etag[48];
  HttpDate(&info.mtime, date, sizeof(date));
  ETag(&info, etag, sizeof(etag));
  snprintf(extra, sizeof(extra), "Last-Modified: %s\r\nETag: %s\r\n", date, etag);
  Response r(c);
  r.Begin(200, ContentType(path), (long) info.size, extra);
  if (!head && !r.Failed()) {
    // Shown on the Pi's screen while it lasts (src/port/transfer.h).
    transfer_begin(path, 0, (long long) info.size);
    unsigned long left = info.size;
    while (left > 0) {
      unsigned got = 0;
      unsigned want = left < kIoChunk ? (unsigned) left : kIoChunk;
      if (sd_read(f, s_io, want, &got) != SD_OK || got == 0) break;
      if (!c->Send(s_io, got)) break;
      left -= got;
      transfer_progress((long long) (info.size - left));
      c->Yield();
    }
    transfer_end(left == 0);
  }
  sd_close(f);
}

// Removes a file and its .finf and .rsrc, or a folder and all in it.
int RemoveAll(CWebDavConnection *c, const char *path, bool is_dir);

void YieldWhileRemoving(void *ctx) { ((CWebDavConnection *) ctx)->Yield(); }

int RemoveAll(CWebDavConnection *c, const char *path, bool is_dir) {
  if (is_dir) {
    return sd_remove_tree(path, 0, YieldWhileRemoving, c);
  }
  int rc = sd_unlink(path);
  if (rc == SD_OK) {
    char helper[kPathMax];
    if (HelperPath(path, ".finf", helper, sizeof(helper))) sd_unlink(helper);
    if (HelperPath(path, ".rsrc", helper, sizeof(helper))) sd_unlink(helper);
  }
  return rc;
}

void Put(CWebDavConnection *c, const char *path, Body *body) {
  if (IsRoot(path) || IsProtected(path)) {
    body->Discard();
    SendStatus(c, 403);
    return;
  }
  sd_info info;
  bool existed = Stat(path, &info);
  if (existed && info.is_dir) {
    SendStatus(c, 405);
    return;
  }
  char parent[kPathMax];
  ParentOf(path, parent, sizeof(parent));
  sd_info pinfo;
  if (!Stat(parent, &pinfo) || !pinfo.is_dir) {
    SendStatus(c, 409);
    return;
  }

  // A temporary file next to it, unless that name would be too long.
  char temp[kPathMax];
  const char *base = BaseName(path);
  bool use_temp = strlen(base) + 6 <= SD_MAX_NAME_LEN;
  if (use_temp) {
    int n = snprintf(temp, sizeof(temp), "%s/.~dav-%s", IsRoot(parent) ? "" : parent,
                     base);
    use_temp = n > 0 && (unsigned) n < sizeof(temp);
  }
  const char *target = use_temp ? temp : path;

  if (!body->Go()) return;
  sd_file *f = sd_open(target, SD_WRITE);
  if (f == 0) {
    body->Discard();
    SendStatus(c, 409);
    return;
  }
  // Gathered a buffer at a time (see webui_fs.cpp's uploads). Shown on
  // the Pi's screen while it lasts (src/port/transfer.h).
  transfer_begin(path, 1, body->Length());
  long long received = 0;
  unsigned buffered = 0;
  int status = 0;
  for (;;) {
    int n = body->Read(s_io + buffered, kIoChunk - buffered);
    if (n < 0) {
      status = 400;
      break;
    }
    buffered += (unsigned) n;
    received += n;
    transfer_progress(received);
    if (buffered == kIoChunk || (n == 0 && buffered > 0)) {
      if (sd_write(f, s_io, buffered) != SD_OK) {
        status = 507;
        break;
      }
      buffered = 0;
    }
    if (n == 0) break;
    c->Yield();
  }
  if (sd_close(f) != SD_OK && status == 0) status = 507;
  transfer_end(status == 0);
  if (status != 0) {
    sd_unlink(target);
    if (status == 400) return;  // the client has gone
    body->Discard();
    SendStatus(c, status);
    return;
  }
  if (use_temp) {
    if (existed && sd_unlink(path) != SD_OK) {
      sd_unlink(temp);
      SendStatus(c, 403);
      return;
    }
    if (sd_rename(temp, path) != SD_OK) {
      sd_unlink(temp);
      SendStatus(c, 500);
      return;
    }
  }
  SendStatus(c, existed ? 204 : 201);
}

void Delete(CWebDavConnection *c, const char *path) {
  if (IsRoot(path) || IsProtected(path)) {
    SendStatus(c, 403);
    return;
  }
  sd_info info;
  if (!Stat(path, &info)) {
    SendStatus(c, 404);
    return;
  }
  int rc = RemoveAll(c, path, info.is_dir != 0);
  SendStatus(c, rc == SD_OK ? 204 : StatusFor(rc));
}

void Mkcol(CWebDavConnection *c, const char *path, Body *body) {
  if (!body->Empty()) {
    body->Discard();
    SendStatus(c, 415);
    return;
  }
  if (IsRoot(path) || IsProtected(path)) {
    SendStatus(c, 403);
    return;
  }
  sd_info info;
  if (Stat(path, &info)) {
    SendStatus(c, 405);
    return;
  }
  char parent[kPathMax];
  ParentOf(path, parent, sizeof(parent));
  if (!Stat(parent, &info) || !info.is_dir) {
    SendStatus(c, 409);
    return;
  }
  SendStatus(c, sd_mkdir(path) == SD_OK ? 201 : 403);
}

int CopyFile(CWebDavConnection *c, const char *from, const char *to) {
  sd_file *in = sd_open(from, SD_READ);
  if (in == 0) return SD_NOT_FOUND;
  sd_file *out = sd_open(to, SD_WRITE);
  if (out == 0) {
    sd_close(in);
    return SD_ERROR;
  }
  int rc = SD_OK;
  for (;;) {
    unsigned got = 0;
    if (sd_read(in, s_io, kIoChunk, &got) != SD_OK) {
      rc = SD_ERROR;
      break;
    }
    if (got == 0) break;
    if (sd_write(out, s_io, got) != SD_OK) {
      rc = SD_ERROR;
      break;
    }
    c->Yield();
  }
  sd_close(in);
  if (sd_close(out) != SD_OK) rc = SD_ERROR;
  if (rc != SD_OK) sd_unlink(to);
  return rc;
}

// A file's .finf and .rsrc (the Mac's), copied or moved with it.
void CarryHelpers(CWebDavConnection *c, const char *from, const char *to,
                  bool move) {
  static const char *const kHelpers[] = {".finf", ".rsrc"};
  for (unsigned i = 0; i < 2; i++) {
    char hf[kPathMax], ht[kPathMax];
    if (!HelperPath(from, kHelpers[i], hf, sizeof(hf)) ||
        !HelperPath(to, kHelpers[i], ht, sizeof(ht))) {
      continue;
    }
    sd_info info;
    if (!Stat(hf, &info) || info.is_dir) continue;
    char dir[kPathMax];
    ParentOf(ht, dir, sizeof(dir));
    sd_mkdir(dir);
    sd_unlink(ht);
    if (move) {
      sd_rename(hf, ht);
    } else {
      CopyFile(c, hf, ht);
    }
  }
}

struct CopyContext {
  CWebDavConnection *c;
  const char *from;
  const char *to;
  unsigned depth;
  int rc;
};

int CopyTree(CWebDavConnection *c, const char *from, const char *to,
             unsigned depth);

int CopyOne(void *ctx, const sd_info *info) {
  CopyContext *cc = (CopyContext *) ctx;
  char from[kPathMax], to[kPathMax];
  int a = snprintf(from, sizeof(from), "%s/%s", IsRoot(cc->from) ? "" : cc->from,
                   info->name);
  int b = snprintf(to, sizeof(to), "%s/%s", IsRoot(cc->to) ? "" : cc->to, info->name);
  if (a <= 0 || b <= 0 || (unsigned) a >= sizeof(from) || (unsigned) b >= sizeof(to)) {
    cc->rc = SD_INVALID;
    return 1;
  }
  int rc = info->is_dir ? CopyTree(cc->c, from, to, cc->depth + 1)
                        : CopyFile(cc->c, from, to);
  if (rc != SD_OK) {
    cc->rc = rc;
    return 1;
  }
  return 0;
}

int CopyTree(CWebDavConnection *c, const char *from, const char *to,
             unsigned depth) {
  if (depth > kCopyDepthMax) return SD_INVALID;
  int rc = sd_mkdir(to);
  if (rc != SD_OK) return rc;
  CopyContext cc = {c, from, to, depth, SD_OK};
  // The helpers are folders here like any other: copied as they are.
  sd_list_info(from, CopyOne, &cc);
  return cc.rc;
}

void CopyMove(CWebDavConnection *c, const char *path, const char *request,
              bool move) {
  char dest_header[1024];
  char dest[kPathMax];
  if (!GetHeader(request, "Destination", dest_header, sizeof(dest_header))) {
    SendStatus(c, 400);
    return;
  }
  if (!CardPath(DestinationPath(dest_header), dest, sizeof(dest))) {
    SendStatus(c, 502);  // somewhere this server isn't
    return;
  }
  char overwrite[8] = "T";
  GetHeader(request, "Overwrite", overwrite, sizeof(overwrite));
  bool may_overwrite = !(overwrite[0] == 'F' || overwrite[0] == 'f');
  char depth[16] = "infinity";
  GetHeader(request, "Depth", depth, sizeof(depth));

  sd_info info;
  if (!Stat(path, &info)) {
    SendStatus(c, 404);
    return;
  }
  if (IsRoot(path) || IsRoot(dest) || IsProtected(dest) ||
      (move && IsProtected(path))) {
    SendStatus(c, 403);
    return;
  }
  if (strcmp(path, dest) == 0 || (info.is_dir && Within(dest, path))) {
    SendStatus(c, 403);
    return;
  }
  char parent[kPathMax];
  ParentOf(dest, parent, sizeof(parent));
  sd_info pinfo;
  if (!Stat(parent, &pinfo) || !pinfo.is_dir) {
    SendStatus(c, 409);
    return;
  }
  sd_info dinfo;
  bool existed = Stat(dest, &dinfo);
  // A change of case only is the same name on FAT: a rename, kept.
  bool same_name = existed && CiEqual(path, dest);
  if (existed && !same_name) {
    if (!may_overwrite) {
      SendStatus(c, 412);
      return;
    }
    int rc = RemoveAll(c, dest, dinfo.is_dir != 0);
    if (rc != SD_OK) {
      SendStatus(c, StatusFor(rc));
      return;
    }
  }

  int rc;
  if (move) {
    rc = sd_rename(path, dest);
    if (rc == SD_OK && !info.is_dir) CarryHelpers(c, path, dest, true);
  } else if (info.is_dir) {
    rc = strcmp(depth, "0") == 0 ? sd_mkdir(dest) : CopyTree(c, path, dest, 0);
  } else {
    rc = CopyFile(c, path, dest);
    if (rc == SD_OK) CarryHelpers(c, path, dest, false);
  }
  if (rc != SD_OK) {
    SendStatus(c, rc == SD_ERROR ? 507 : StatusFor(rc));
    return;
  }
  SendStatus(c, existed && !same_name ? 204 : 201);
}

// The text inside an element (owner's href or name), roughly.
void ElementText(const char *xml, const char *local, char *out, unsigned size) {
  out[0] = '\0';
  char want[40];
  snprintf(want, sizeof(want), "%s", local);
  for (const char *p = strchr(xml, '<'); p != 0; p = strchr(p + 1, '<')) {
    char name[64];
    const char *prefix;
    unsigned prefix_len;
    if (!ElementName(p, &prefix, &prefix_len, name, sizeof(name)) ||
        strcmp(name, want) != 0) {
      continue;
    }
    const char *q = strchr(p, '>');
    if (q == 0 || q[-1] == '/') return;
    q++;
    unsigned n = 0;
    // Text only, without the tags inside.
    bool in_tag = false;
    for (; *q != '\0'; q++) {
      if (*q == '<') {
        if (q[1] == '/' && !in_tag) {
          // The end of this element, or of one inside it.
          const char *gt = strchr(q, '>');
          char close[64];
          if (gt != 0) {
            unsigned len = (unsigned) (gt - q - 2);
            if (len < sizeof(close)) {
              memcpy(close, q + 2, len);
              close[len] = '\0';
              const char *colon = strchr(close, ':');
              if (strcmp(colon != 0 ? colon + 1 : close, want) == 0) break;
            }
          }
        }
        in_tag = true;
        continue;
      }
      if (*q == '>') {
        in_tag = false;
        continue;
      }
      if (!in_tag && n + 1 < size) out[n++] = *q;
    }
    // Trim
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n' ||
                     out[n - 1] == '\r' || out[n - 1] == '\t')) {
      n--;
    }
    out[n] = '\0';
    unsigned s = 0;
    while (out[s] == ' ' || out[s] == '\n' || out[s] == '\r' || out[s] == '\t') s++;
    if (s > 0) memmove(out, out + s, n - s + 1);
    return;
  }
}

bool Contains(const char *xml, const char *local) {
  for (const char *p = strchr(xml, '<'); p != 0; p = strchr(p + 1, '<')) {
    char name[64];
    const char *prefix;
    unsigned prefix_len;
    if (ElementName(p, &prefix, &prefix_len, name, sizeof(name)) &&
        strcmp(name, local) == 0) {
      return true;
    }
  }
  return false;
}

void Lock(CWebDavConnection *c, const char *path, const char *request,
          Body *body) {
  unsigned got = 0;
  if (!body->ReadAll(s_xml, sizeof(s_xml), &got)) {
    SendStatus(c, 413);
    return;
  }
  if (got > 0 && !WellFormed(s_xml)) {
    SendStatus(c, 400);
    return;
  }
  sd_info info;
  bool existed = Stat(path, &info);
  char token[96];
  char if_header[256];
  if (got == 0) {
    // A refresh: the token is in the If header, "(<token>)".
    if (!existed) {
      SendStatus(c, 404);
      return;
    }
    token[0] = '\0';
    if (GetHeader(request, "If", if_header, sizeof(if_header))) {
      const char *lt = strchr(if_header, '<');
      const char *gt = lt != 0 ? strchr(lt, '>') : 0;
      if (lt != 0 && gt != 0 && (unsigned) (gt - lt - 1) < sizeof(token)) {
        memcpy(token, lt + 1, (unsigned) (gt - lt - 1));
        token[gt - lt - 1] = '\0';
      }
    }
    if (token[0] == '\0') {
      SendStatus(c, 412);
      return;
    }
  } else {
    snprintf(token, sizeof(token), "opaquelocktoken:bmmac-%08x-%08x",
             ++s_lock_serial, (unsigned) rand());
  }

  int status = 200;
  if (!existed) {
    // Locking a name that isn't there makes it, empty (RFC 4918 7.3).
    if (IsRoot(path) || IsProtected(path)) {
      SendStatus(c, 403);
      return;
    }
    char parent[kPathMax];
    sd_info pinfo;
    ParentOf(path, parent, sizeof(parent));
    if (!Stat(parent, &pinfo) || !pinfo.is_dir) {
      SendStatus(c, 409);
      return;
    }
    sd_file *f = sd_open(path, SD_WRITE);
    if (f == 0 || sd_close(f) != SD_OK) {
      SendStatus(c, 409);
      return;
    }
    status = 201;
  }

  char owner[256];
  ElementText(s_xml, "owner", owner, sizeof(owner));
  bool shared = Contains(s_xml, "shared");
  char depth[16] = "infinity";
  GetHeader(request, "Depth", depth, sizeof(depth));
  char timeout[64] = "Second-3600";

  char extra[160];
  snprintf(extra, sizeof(extra), "Lock-Token: <%s>\r\n", token);
  Response r(c);
  r.Begin(status, "application/xml; charset=utf-8", -1, extra);
  r.Text("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
         "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
         "<D:locktype><D:write/></D:locktype><D:lockscope>");
  r.Text(shared ? "<D:shared/>" : "<D:exclusive/>");
  r.Printf("</D:lockscope><D:depth>%s</D:depth>",
           strcmp(depth, "0") == 0 ? "0" : "infinity");
  if (owner[0] != '\0') {
    r.Text("<D:owner><D:href>");
    XmlAttrText(&r, owner);
    r.Text("</D:href></D:owner>");
  }
  r.Printf("<D:timeout>%s</D:timeout><D:locktoken><D:href>", timeout);
  XmlAttrText(&r, token);
  r.Text("</D:href></D:locktoken><D:lockroot><D:href>");
  r.Href(path, existed && info.is_dir);
  r.Text("</D:href></D:lockroot></D:activelock></D:lockdiscovery></D:prop>\n");
  r.Finish();
}

}  // namespace

bool WebDavWants(const char *method, const char *target) {
  static const char *const kDavMethods[] = {
      "OPTIONS", "PROPFIND", "PROPPATCH", "MKCOL", "PUT",
      "DELETE",  "COPY",     "MOVE",      "LOCK",  "UNLOCK",
  };
  for (unsigned i = 0; i < sizeof(kDavMethods) / sizeof(kDavMethods[0]); i++) {
    if (strcmp(method, kDavMethods[i]) == 0) return true;
  }
  if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
    size_t n = strlen(WEBDAV_PREFIX);
    return strncmp(target, WEBDAV_PREFIX, n) == 0 &&
           (target[n] == '\0' || target[n] == '/' || target[n] == '?');
  }
  return false;
}

void WebDavHandle(CWebDavConnection *c, const char *method, const char *target,
                  const char *request, const unsigned char *body_data,
                  unsigned body_len) {
  Body body(c, request, body_data, body_len);

  if (strcmp(method, "OPTIONS") == 0) {
    body.Discard();
    Response r(c);
    r.Begin(200, 0, 0,
            "DAV: 1, 2\r\n"
            "MS-Author-Via: DAV\r\n"
            "Allow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, COPY, MOVE, "
            "PROPFIND, PROPPATCH, LOCK, UNLOCK\r\n");
    r.Finish();
    return;
  }

  char path[kPathMax];
  if (!CardPath(target, path, sizeof(path))) {
    // Outside /sd: clients look at the server's root first (Windows). It
    // is a folder with the card in it.
    if (strcmp(method, "PROPFIND") == 0 &&
        (strcmp(target, "/") == 0 || strncmp(target, "/?", 2) == 0)) {
      char depth[16] = "1";
      GetHeader(request, "Depth", depth, sizeof(depth));
      body.Discard();
      Response r(c);
      r.Begin(207, "application/xml; charset=utf-8", -1, 0);
      r.Text("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
             "<D:multistatus xmlns:D=\"DAV:\">"
             "<D:response><D:href>/</D:href><D:propstat><D:prop>"
             "<D:resourcetype><D:collection/></D:resourcetype>"
             "<D:displayname>BMMacintosh</D:displayname>"
             "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
             "</D:response>\n");
      if (strcmp(depth, "0") != 0) {
        r.Text("<D:response><D:href>" WEBDAV_PREFIX "/</D:href><D:propstat><D:prop>"
               "<D:resourcetype><D:collection/></D:resourcetype>"
               "<D:displayname>SD card</D:displayname>"
               "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
               "</D:response>\n");
      }
      r.Text("</D:multistatus>\n");
      r.Finish();
      return;
    }
    body.Discard();
    SendStatus(c, strncmp(target, WEBDAV_PREFIX, strlen(WEBDAV_PREFIX)) == 0 ? 400
                                                                        : 404);
    return;
  }

  if (strcmp(method, "PROPFIND") == 0) {
    Propfind(c, path, request, &body);
  } else if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
    body.Discard();
    Get(c, path, strcmp(method, "HEAD") == 0);
  } else if (strcmp(method, "PUT") == 0) {
    Put(c, path, &body);
  } else if (strcmp(method, "DELETE") == 0) {
    body.Discard();
    Delete(c, path);
  } else if (strcmp(method, "MKCOL") == 0) {
    Mkcol(c, path, &body);
  } else if (strcmp(method, "COPY") == 0 || strcmp(method, "MOVE") == 0) {
    body.Discard();
    CopyMove(c, path, request, strcmp(method, "MOVE") == 0);
  } else if (strcmp(method, "PROPPATCH") == 0) {
    Proppatch(c, path, &body);
  } else if (strcmp(method, "LOCK") == 0) {
    Lock(c, path, request, &body);
  } else if (strcmp(method, "UNLOCK") == 0) {
    body.Discard();
    SendStatus(c, 204);
  } else {
    body.Discard();
    SendStatus(c, 501);
  }
}
