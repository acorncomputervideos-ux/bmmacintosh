//
// webui.cpp
//
// Minimal HTTP/1.1 server for the LAN web UI, from BMC64. Runs as a single
// Circle scheduler task. Scheduler tasks run when the emulator core yields
// (once a frame, and while it waits), so long I/O loops yield to keep the
// network stack and the emulation going.
//
// The SD card is a network drive (WebDAV) at /sd/: see webdav.cpp.
//
// Endpoints: GET static assets, GET /api/status, GET and POST /api/disks
// (webui_disks.cpp), GET and POST /api/clipboard (the Mac II's), POST /api/reboot,
// POST /api/shutdown, POST /api/reset, GET /api/volumes, GET /api/fs/list, GET /api/fs/download,
// POST /api/fs/upload, POST /api/fs/save, POST /api/fs/delete,
// POST /api/fs/mkdir, POST /api/fs/rename,
// POST /api/webui/disable.
//
// When a PIN is configured, every request must carry HTTP Basic Auth
// (Authorization: Basic base64(<user>:<pin>)); the username is ignored.
//
// Modified for BMA800XL: always built, with the BMA800XL host name,
// version and power and reset requests; BMC64's profiles are removed.
// Changed for BMCoco, BMApple2 and BMAtariST, and for BMMacintosh: the Mac
// and its files.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "webui.h"

#if BMC64_WEBUI

#include "webui_assets.h"
#include "webui_fs.h"
#include "webui_http.h"
#include "webui_machine.h"
#include "webdav.h"
#include "webui_disks.h"

#include <circle/bcmpropertytags.h>
#include <circle/logger.h>
#include <circle/machineinfo.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/synchronizationevent.h>
#include <circle/sched/task.h>
#include <circle/timer.h>
#include <circle/types.h>

#include <stdio.h>
#include <string.h>

// circle_get_network_status() / circle_get_network_ip_address() are the
// C bridge already implemented in src/vice_network.cpp.
extern "C" int circle_get_network_status(void);
extern "C" int circle_get_network_ip_address(char *address,
                                             unsigned int address_size);
// Requests for the emulator's main loop (src/port/platform.c), which acts
// on them between frames: a reboot or power off (after closing the disk
// images so their last writes reach the card), and a cold start.
extern "C" void bmmac_request_power(int power_off);
extern "C" void bmmac_request_coldstart(void);
extern "C" const char *bmmac_last_transfer(void);
// The Mac II's shared clipboard (src/port/ii/clip_ii.cpp).
extern "C" const char *emu_clipboard_text(int *from_mac);
extern "C" int emu_clipboard_set(const char *utf8, unsigned length);

#define WEBUI_LOG              "webui"
#define WEBUI_PORT             80
#define WEBUI_ALT_PORT         8080
#define WEBUI_TASK_STACK       0x10000
#define WEBUI_WORKERS          WEBDAV_AT_ONCE
#define WEBUI_RECV_TIMEOUT_US  8000000
#define WEBUI_REQUEST_MAX      4096
#define WEBUI_LISTEN_BACKLOG   16
#define WEBUI_PIN_MAX          16
#define WEBUI_AUTH_FAIL_MS     500  // slow brute-forcing a wrong PIN

// CNetSubSystem is created with this name in src/kernel.cpp.
#define WEBUI_HOSTNAME         "bmmac"

namespace {

// Keep in sync with the CIRCLE_NETWORK_* enum in third_party/common/circle.h.
const char *NetStatusText(int status) {
  switch (status) {
  case 0:  return "disabled";
  case 1:  return "Ethernet initializing";
  case 2:  return "waiting for DHCP";
  case 3:  return "connected (Ethernet)";
  case 4:  return "Ethernet unavailable";
  case 5:  return "Ethernet init failed";
  case 6:  return "Wi-Fi config missing";
  case 7:  return "Wi-Fi device initializing";
  case 8:  return "Wi-Fi device init failed";
  case 9:  return "Wi-Fi network init failed";
  case 10: return "Wi-Fi WPA initializing";
  case 11: return "Wi-Fi WPA init failed";
  case 12: return "Wi-Fi connecting";
  case 13: return "connected (Wi-Fi)";
  case 14: return "Wi-Fi connection timeout";
  case 15: return "Wi-Fi unavailable";
  case 16: return "Wi-Fi device not initialized";
  case 17: return "Wi-Fi unsupported";
  case 18: return "Wi-Fi firmware missing";
  default: return "unknown";
  }
}

using webhttp::SendResponse;
using webhttp::SendText;

// Case-insensitive test whether s begins with the (already lowercase) prefix.
boolean CiPrefix(const char *s, const char *lower_prefix) {
  for (; *lower_prefix != '\0'; s++, lower_prefix++) {
    char c = *s;
    if (c >= 'A' && c <= 'Z') {
      c = (char) (c + 32);
    }
    if (c != *lower_prefix) {
      return FALSE;
    }
  }
  return TRUE;
}

// Value of the Content-Length header in a raw request, or -1 if absent.
long ParseContentLength(const char *request) {
  const char *p = request;
  while (*p != '\0') {
    if (CiPrefix(p, "content-length:")) {
      p += 15;
      while (*p == ' ' || *p == '\t') {
        p++;
      }
      long value = 0;
      boolean any = FALSE;
      while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
        any = TRUE;
      }
      return any ? value : -1;
    }
    const char *nl = strstr(p, "\r\n");
    if (nl == 0) {
      break;
    }
    p = nl + 2;
  }
  return -1;
}

// TRUE if the raw request has a header whose name-and-colon matches
// name_colon (lower case, e.g. "x-bmmacintosh-web:"). Stops at the blank line so
// body text can't pose as a header.
boolean HasHeader(const char *request, const char *name_colon) {
  const char *p = request;
  while (*p != '\0' && !(p[0] == '\r' && p[1] == '\n')) {
    if (CiPrefix(p, name_colon)) {
      return TRUE;
    }
    const char *nl = strstr(p, "\r\n");
    if (nl == 0) {
      break;
    }
    p = nl + 2;
  }
  return FALSE;
}

const struct webui_asset *FindAsset(const char *path) {
  for (unsigned i = 0; i < g_webui_asset_count; i++) {
    if (strcmp(g_webui_assets[i].path, path) == 0) {
      return &g_webui_assets[i];
    }
  }
  return 0;
}

// SoC temperature as a JSON number ("47.2") into out, or "null" if the
// VideoCore mailbox query fails (e.g. QEMU).
void SocTempField(char *out, unsigned out_size) {
  CBcmPropertyTags tags;
  TPropertyTagTemperature tag;
  tag.nTemperatureId = TEMPERATURE_ID;
  if (tags.GetTag(PROPTAG_GET_TEMPERATURE, &tag, sizeof(tag), sizeof(u32)) &&
      tag.nValue > 0) {
    snprintf(out, out_size, "%u.%01u", tag.nValue / 1000,
             (tag.nValue % 1000) / 100);
  } else {
    strncpy(out, "null", out_size);
    out[out_size - 1] = '\0';
  }
}

// Raspberry Pi firmware "get_throttled" bitmask, forwarded raw and
// decoded client side. These are independent conditions: bit 0/16
// under-voltage (a 5V supply problem), bit 2/18 the ARM clock actually
// being throttled (from under-voltage or the 85C hard thermal limit),
// bit 1/17 the turbo frequency being capped, bit 3/19 the Pi 3B+
// "soft" temp limit (config.txt temp_soft_limit, default 60C): a
// partial throttle easing the ARM clock from 1.4GHz to 1.2GHz. Low
// bits are "now", bits 16-19 "since boot".
// "null" if the mailbox query is unavailable (e.g. QEMU).
void ThrottledField(char *out, unsigned out_size) {
  CBcmPropertyTags tags;
  TPropertyTagSimple tag;
  if (tags.GetTag(PROPTAG_GET_THROTTLED, &tag, sizeof(tag))) {
    snprintf(out, out_size, "%lu", (unsigned long) tag.nValue);
  } else {
    strncpy(out, "null", out_size);
    out[out_size - 1] = '\0';
  }
}

// Copies s into out as the inside of a JSON string (the machine's name,
// which can come from a settings file); characters other than plain text
// are replaced.
void JsonText(const char *s, char *out, unsigned out_size) {
  unsigned n = 0;
  for (; *s != '\0' && n + 2 < out_size; s++) {
    unsigned char c = (unsigned char) *s;
    if (c == '"' || c == '\\') {
      out[n++] = '\\';
      out[n++] = (char) c;
    } else {
      out[n++] = (c < 0x20 || c >= 0x80) ? '?' : (char) c;
    }
  }
  out[n] = '\0';
}

void HandleStatus(CSocket *socket) {
  char ip[32];
  boolean have_ip = circle_get_network_ip_address(ip, sizeof(ip)) != 0;
  int net_status = circle_get_network_status();
  unsigned uptime = CTimer::Get()->GetUptime();
  const char *model = CMachineInfo::Get()->GetMachineName();

  char soc_temp[16];
  char throttled[16];
  SocTempField(soc_temp, sizeof(soc_temp));
  ThrottledField(throttled, sizeof(throttled));

  // What this machine's web UI can do (webui_machine.h).
  char caps[256];
  if (WebUiMachineCapsJson(caps, sizeof(caps)) < 0) {
    SendText(socket, 500, "Internal Server Error", "status encode error\n");
    return;
  }

  char machine[128];
  JsonText(WebUiMachineName(), machine, sizeof(machine));
  // The last file transfer's size, time and speed (src/port/frontend.c).
  char last[400];
  JsonText(bmmac_last_transfer(), last, sizeof(last));

  char body[1400];
  int length = snprintf(
      body, sizeof(body),
      "{\"hostname\":\"%s\",\"version\":\"%s\",\"git\":\"%s\",\"machine\":\"%s\","
      "\"model\":\"%s\",\"ip\":\"%s\",\"net_status\":%d,\"net_text\":\"%s\","
      "\"uptime_secs\":%u,\"soc_temp_c\":%s,\"throttled\":%s,"
      "\"last_transfer\":\"%s\",\"caps\":%s}",
      WEBUI_HOSTNAME, BMMAC_VERSION, BMMAC_GIT, machine,
      model != 0 ? model : "",
      have_ip ? ip : "", net_status, NetStatusText(net_status), uptime,
      soc_temp, throttled, last, caps);
  if (length < 0 || (unsigned) length >= sizeof(body)) {
    SendText(socket, 500, "Internal Server Error", "status encode error\n");
    return;
  }
  SendResponse(socket, 200, "OK", "application/json", body, (unsigned) length);
}

// Server lifecycle flags. s_started guards against a second task;
// g_webui_stop is set by POST /api/webui/disable to end the accept loop.
boolean s_started = FALSE;
// Answering a request (WebUiBusy()): set and read on core 1, where the
// web UI's task and the emulator both run.
volatile int s_busy = 0;   // requests being answered
// The web page's own requests (not the network drive's) take turns: they
// share static buffers. See UiTurn.
boolean s_ui_turn_taken = FALSE;
boolean g_webui_stop = FALSE;

// HTTP Basic Auth PIN; empty means the server is open.
char s_pin[WEBUI_PIN_MAX] = {0};

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Decode base64 src into dst (NUL-terminated). Returns length, or -1 on
// invalid input or overflow.
int Base64Decode(const char *src, char *dst, unsigned dst_size) {
  unsigned out = 0;
  int quad[4];
  int have = 0;
  for (const char *p = src; *p != '\0'; p++) {
    if (*p == '=' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') {
      break;
    }
    int v = Base64Value(*p);
    if (v < 0) return -1;
    quad[have++] = v;
    if (have == 4) {
      if (out + 3 >= dst_size) return -1;
      dst[out++] = (char) ((quad[0] << 2) | (quad[1] >> 4));
      dst[out++] = (char) ((quad[1] << 4) | (quad[2] >> 2));
      dst[out++] = (char) ((quad[2] << 6) | quad[3]);
      have = 0;
    }
  }
  if (have == 2) {
    if (out + 1 >= dst_size) return -1;
    dst[out++] = (char) ((quad[0] << 2) | (quad[1] >> 4));
  } else if (have == 3) {
    if (out + 2 >= dst_size) return -1;
    dst[out++] = (char) ((quad[0] << 2) | (quad[1] >> 4));
    dst[out++] = (char) ((quad[1] << 4) | (quad[2] >> 2));
  } else if (have != 0) {
    return -1;
  }
  dst[out] = '\0';
  return (int) out;
}

enum { AUTH_OK, AUTH_MISSING, AUTH_BAD };

// Check HTTP Basic Auth against s_pin. Returns AUTH_OK when no PIN is
// set, or when the request's Authorization password matches.
int CheckAuth(const char *request) {
  if (s_pin[0] == '\0') {
    return AUTH_OK;
  }

  const char *p = request;
  const char *value = 0;
  while (*p != '\0') {
    if (CiPrefix(p, "authorization:")) {
      value = p + 14;
      break;
    }
    const char *nl = strstr(p, "\r\n");
    if (nl == 0) break;
    p = nl + 2;
  }
  if (value == 0) {
    return AUTH_MISSING;
  }

  while (*value == ' ' || *value == '\t') value++;
  if (!CiPrefix(value, "basic ")) {
    return AUTH_BAD;
  }
  value += 6;
  while (*value == ' ') value++;

  char decoded[128];
  if (Base64Decode(value, decoded, sizeof(decoded)) < 0) {
    return AUTH_BAD;
  }
  const char *colon = strchr(decoded, ':');
  const char *pass = colon != 0 ? colon + 1 : decoded;

  if (strlen(pass) != strlen(s_pin)) {
    return AUTH_BAD;
  }
  unsigned diff = 0;
  for (unsigned i = 0; s_pin[i] != '\0'; i++) {
    diff |= (unsigned char) (pass[i] ^ s_pin[i]);
  }
  return diff == 0 ? AUTH_OK : AUTH_BAD;
}

void Send401(CSocket *socket) {
  const char *body = "authentication required\n";
  char header[256];
  int n = snprintf(
      header, sizeof(header),
      "HTTP/1.1 401 Unauthorized\r\n"
      "WWW-Authenticate: Basic realm=\"BMMacintosh Web UI - any username, PIN as "
      "password\", charset=\"UTF-8\"\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Length: %u\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-store\r\n"
      "\r\n",
      (unsigned) strlen(body));
  if (n > 0 && (unsigned) n < sizeof(header) &&
      webhttp::SendAll(socket, header, (unsigned) n)) {
    webhttp::SendAll(socket, body, (unsigned) strlen(body));
  }
}

// The network drive's requests (webdav.cpp) through the connection's socket.
class CSocketDavConnection : public CWebDavConnection {
public:
  explicit CSocketDavConnection(CSocket *socket) : m_socket(socket) {}
  int Receive(void *buffer, unsigned length) override {
    return m_socket->Receive(buffer, length, 0);
  }
  bool Send(const void *data, unsigned length) override {
    return webhttp::SendAll(m_socket, data, length);
  }
  void Yield(void) override { CScheduler::Get()->Yield(); }

private:
  CSocket *m_socket;
};

// GET /api/clipboard: {"available":true,"from":"mac","text":"..."} (the
// text as UTF-8); "available" is false on the Mac Plus.
void HandleClipboardGet(CSocket *socket) {
  int from_mac = 0;
  const char *text = emu_clipboard_text(&from_mac);
  webhttp::CChunkedResponse r(socket, 200, "OK", "application/json");
  r.Begin();
  r.Printf("{\"available\":%s,\"from\":\"%s\",\"text\":\"",
           text != 0 ? "true" : "false", from_mac ? "mac" : "page");
  for (const char *p = text != 0 ? text : ""; *p != '\0' && !r.Failed(); p++) {
    unsigned char c = (unsigned char) *p;
    if (c == '"' || c == '\\') {
      char esc[2] = {'\\', (char) c};
      r.WriteN(esc, 2);
    } else if (c == '\n') {
      r.Write("\\n");
    } else if (c == '\t') {
      r.Write("\\t");
    } else if (c < 0x20) {
      r.Printf("\\u%04x", c);
    } else {
      r.WriteN(p, 1);  // UTF-8 as it is
    }
  }
  r.Write("\"}");
  r.Finish();
}

// POST /api/clipboard with the text (UTF-8) as the body: the Mac gets it
// when a program pastes.
void HandleClipboardPost(CSocket *socket, const unsigned char *body,
                         unsigned prefetched, long length) {
  static char text[64 * 1024];
  if (emu_clipboard_text(0) == 0) {
    SendText(socket, 400, "Bad Request", "the Mac Plus has no shared clipboard\n");
    return;
  }
  if (length < 0 || (unsigned long) length > sizeof(text)) {
    SendText(socket, 413, "Payload Too Large", "up to 64 KB of text\n");
    return;
  }
  unsigned got = prefetched < (unsigned) length ? prefetched : (unsigned) length;
  memcpy(text, body, got);
  while (got < (unsigned) length) {
    int n = socket->Receive(text + got, (unsigned) length - got, 0);
    if (n <= 0) {
      return;  // the client has gone
    }
    got += (unsigned) n;
  }
  emu_clipboard_set(text, got);
  const char *ok = "{\"ok\":true}";
  SendResponse(socket, 200, "OK", "application/json", ok, (unsigned) strlen(ok));
}

// The web page's requests' turn: waits while another has it.
class UiTurn {
public:
  UiTurn(void) {
    while (s_ui_turn_taken) {
      CScheduler::Get()->Yield();
    }
    s_ui_turn_taken = TRUE;
  }
  ~UiTurn(void) { s_ui_turn_taken = FALSE; }
};

boolean HandleConnection(CSocket *socket) {
  socket->SetOptionReceiveTimeout(WEBUI_RECV_TIMEOUT_US);

  char request[WEBUI_REQUEST_MAX];
  unsigned length = 0;
  for (;;) {
    if (length >= sizeof(request) - 1) {
      SendText(socket, 431, "Request Header Fields Too Large",
               "request too large\n");
      return FALSE;
    }
    int n = socket->Receive(request + length, sizeof(request) - 1 - length, 0);
    if (n <= 0) {
      return FALSE;  // client closed, timed out, or error
    }
    length += (unsigned) n;
    request[length] = '\0';
    if (strstr(request, "\r\n\r\n") != 0) {
      break;
    }
  }

  // Body bytes that arrived in the same read(s) as the headers, plus the
  // declared body length (for streamed uploads).
  const unsigned char *body =
      (const unsigned char *) (strstr(request, "\r\n\r\n") + 4);
  unsigned body_prefetched = (unsigned) ((const unsigned char *) (request + length) - body);
  long content_length = ParseContentLength(request);

  char method[16];  // PROPPATCH is 9 letters
  char target[WEBUI_REQUEST_MAX];
  if (sscanf(request, "%15s %4095s", method, target) != 2) {
    SendText(socket, 400, "Bad Request", "bad request line\n");
    return FALSE;
  }

  const char *query = "";
  char *question = strchr(target, '?');
  if (question != 0) {
    *question = '\0';
    query = question + 1;
  }

  boolean is_get = strcmp(method, "GET") == 0;
  boolean is_post = strcmp(method, "POST") == 0;

  int auth = CheckAuth(request);
  if (auth != AUTH_OK) {
    if (auth == AUTH_BAD) {
      CScheduler::Get()->MsSleep(WEBUI_AUTH_FAIL_MS);
    }
    Send401(socket);
    return FALSE;
  }

  // The SD card as a network drive (webdav.cpp), with the query left on
  // the target for it.
  if (WebDavWants(method, target)) {
    if (question != 0) {
      *question = '?';
    }
    CSocketDavConnection connection(socket);
    WebDavHandle(&connection, method, target, request, body, body_prefetched);
    return FALSE;
  }

  // The rest one at a time (until this returns).
  UiTurn turn;

  if (is_get && strcmp(target, "/api/status") == 0) {
    HandleStatus(socket);
    return FALSE;
  }

  if (is_get && strcmp(target, "/api/clipboard") == 0) {
    HandleClipboardGet(socket);
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/clipboard") == 0) {
    // Same cross-site protection as /api/fs/save below.
    if (!HasHeader(request, "x-bmmacintosh-web:")) {
      SendText(socket, 403, "Forbidden", "missing X-BMMacintosh-Web header\n");
      return FALSE;
    }
    HandleClipboardPost(socket, body, body_prefetched, content_length);
    return FALSE;
  }

  if (strcmp(target, "/api/disks") == 0 && (is_get || is_post)) {
    // One request at a time (this task), so one buffer, off the stack.
    static char json[48 * 1024];
    if (is_get) {
      int length = WebUiDisksJson(json, sizeof(json));
      if (length < 0) {
        SendText(socket, 500, "Internal Server Error", "too many disk images\n");
      } else {
        SendResponse(socket, 200, "OK", "application/json", json, (unsigned) length);
      }
      return FALSE;
    }
    // Changes the Mac's disks: the same cross-site protection as
    // /api/fs/save below.
    if (!HasHeader(request, "x-bmmacintosh-web:")) {
      SendText(socket, 403, "Forbidden", "missing X-BMMacintosh-Web header\n");
      return FALSE;
    }
    int status = WebUiDisksRequest(query, json, sizeof(json));
    SendResponse(socket, status,
                 status == 202 ? "Accepted"
                 : status == 201 ? "Created"
                 : status == 507 ? "Insufficient Storage"
                 : status == 404 ? "Not Found"
                 : status == 409 ? "Conflict" : "Bad Request",
                 "application/json", json, (unsigned) strlen(json));
    return FALSE;
  }

  if (is_get && strcmp(target, "/api/volumes") == 0) {
    WebUiFsVolumes(socket);
    return FALSE;
  }

  if (is_get && strcmp(target, "/api/fs/list") == 0) {
    WebUiFsList(socket, query);
    return FALSE;
  }

  if (is_get && strcmp(target, "/api/fs/download") == 0) {
    WebUiFsDownload(socket, query);
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/fs/upload") == 0) {
    WebUiFsUpload(socket, query, body, body_prefetched, content_length);
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/fs/save") == 0) {
    // Rewrites boot/config files, so demand a header that a cross-site form
    // or no-cors fetch cannot send (it would need a CORS preflight, which
    // this server never grants). Stops another site's page from using the
    // browser's cached PIN to change them.
    if (!HasHeader(request, "x-bmmacintosh-web:")) {
      SendText(socket, 403, "Forbidden", "missing X-BMMacintosh-Web header\n");
      return FALSE;
    }
    WebUiFsSave(socket, query, body, body_prefetched, content_length);
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/fs/delete") == 0) {
    // Deleting a whole folder is far more destructive than one file, so it
    // gets the same cross-site protection as /api/fs/save above.
    char recursive[4];
    if (webhttp::QueryParam(query, "recursive", recursive, sizeof(recursive)) &&
        !HasHeader(request, "x-bmmacintosh-web:")) {
      SendText(socket, 403, "Forbidden", "missing X-BMMacintosh-Web header\n");
      return FALSE;
    }
    WebUiFsDelete(socket, query);
    return FALSE;
  }

  if (is_post && (strcmp(target, "/api/fs/mkdir") == 0 ||
                  strcmp(target, "/api/fs/rename") == 0)) {
    // Same cross-site protection as /api/fs/save above.
    if (!HasHeader(request, "x-bmmacintosh-web:")) {
      SendText(socket, 403, "Forbidden", "missing X-BMMacintosh-Web header\n");
      return FALSE;
    }
    if (strcmp(target, "/api/fs/mkdir") == 0) {
      WebUiFsMkdir(socket, query);
    } else {
      WebUiFsRename(socket, query);
    }
    return FALSE;
  }


  if (is_post && strcmp(target, "/api/webui/disable") == 0) {
    const char *ok = "{\"ok\":true}";
    SendResponse(socket, 200, "OK", "application/json", ok,
                 (unsigned) strlen(ok));
    CLogger::Get()->Write(WEBUI_LOG, LogNotice,
                          "Web UI stop requested via web UI");
    g_webui_stop = TRUE;
    return FALSE;
  }

  if (is_post && (strcmp(target, "/api/reboot") == 0 ||
                  strcmp(target, "/api/shutdown") == 0)) {
    // Same cross-site protection as /api/fs/save: another site must not be
    // able to restart or power off the machine.
    if (!HasHeader(request, "x-bmmacintosh-web:")) {
      SendText(socket, 403, "Forbidden", "missing X-BMMacintosh-Web header\n");
      return FALSE;
    }
    int power_off = strcmp(target, "/api/shutdown") == 0;
    const char *ok = "{\"ok\":true}";
    SendResponse(socket, 202, "Accepted", "application/json", ok,
                 (unsigned) strlen(ok));
    CLogger::Get()->Write(WEBUI_LOG, LogNotice, "%s requested via web UI",
                          power_off ? "Power off" : "Reboot");
    CScheduler::Get()->MsSleep(250);  // give the socket time to flush
    // Queued for the emulator main loop, which writes out the disk images
    // and then restarts or halts.
    bmmac_request_power(power_off);
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/reset") == 0) {
    // Queued for the emulator main loop; BMC64 itself keeps running.
    bmmac_request_coldstart();
    const char *ok = "{\"ok\":true}";
    SendResponse(socket, 202, "Accepted", "application/json", ok,
                 (unsigned) strlen(ok));
    CLogger::Get()->Write(WEBUI_LOG, LogNotice,
                          "Hard reset requested via web UI");
    return FALSE;
  }

  if (is_get) {
    const struct webui_asset *asset = FindAsset(target);
    if (asset != 0) {
      SendResponse(socket, 200, "OK", asset->content_type, asset->data,
                   asset->length);
      return FALSE;
    }
  }

  if (!is_get && !is_post) {
    SendText(socket, 405, "Method Not Allowed", "method not allowed\n");
    return FALSE;
  }

  SendText(socket, 404, "Not Found", "not found\n");
  return FALSE;
}

// Answers connections the server task hands it, one after another. A few
// of them answer at once: the macOS Finder asks about folders, over
// connections of their own, while it uploads a file, and waiting for the
// upload made it hang, give up, and leave the file empty.
class CWebUiWorker : public CTask {
public:
  CWebUiWorker(void) : CTask(WEBUI_TASK_STACK), m_socket(0) { SetName("webui-worker"); }

  // Takes CONNECTION (and deletes it when done); false if busy.
  bool Take(CSocket *connection) {
    if (m_socket != 0) return false;
    m_socket = connection;
    s_busy++;
    m_event.Set();
    return true;
  }

  void Run(void) override {
    for (;;) {
      m_event.Wait();
      m_event.Clear();
      if (m_socket == 0) continue;
      HandleConnection(m_socket);
      delete m_socket;
      m_socket = 0;
      s_busy--;
    }
  }

private:
  CSocket *volatile m_socket;
  CSynchronizationEvent m_event;
};

class CWebUiTask : public CTask {
public:
  explicit CWebUiTask(CNetSubSystem *network)
      : CTask(WEBUI_TASK_STACK), m_network(network) {
    SetName("webui");
  }

  void Run(void) override {
    while (!m_network->IsRunning()) {
      CScheduler::Get()->Sleep(1);
    }

    CSocket listener(m_network, IPPROTO_TCP);
    u16 port = WEBUI_PORT;
    if (listener.Bind(port) < 0) {
      port = WEBUI_ALT_PORT;
      if (listener.Bind(port) < 0) {
        CLogger::Get()->Write(WEBUI_LOG, LogError,
                              "Cannot bind port %u or %u", (unsigned) WEBUI_PORT,
                              (unsigned) WEBUI_ALT_PORT);
        return;
      }
    }

    if (listener.Listen(WEBUI_LISTEN_BACKLOG) < 0) {
      CLogger::Get()->Write(WEBUI_LOG, LogError, "Cannot listen on port %u",
                            (unsigned) port);
      return;
    }

    CLogger::Get()->Write(WEBUI_LOG, LogNotice,
                          "Web UI listening on port %u", (unsigned) port);
    for (unsigned i = 0; i < WEBUI_WORKERS; i++) {
      m_workers[i] = new CWebUiWorker;
    }

    for (;;) {
      CIPAddress client_ip;
      u16 client_port;
      CSocket *connection = listener.Accept(&client_ip, &client_port);
      if (connection == 0) {
        CScheduler::Get()->MsSleep(50);
        continue;
      }
      // To a free worker, waiting for one if all are busy.
      for (;;) {
        bool taken = false;
        for (unsigned i = 0; i < WEBUI_WORKERS && !taken; i++) {
          taken = m_workers[i]->Take(connection);
        }
        if (taken) break;
        CScheduler::Get()->MsSleep(5);
      }
      if (g_webui_stop) {
        break;
      }
      CScheduler::Get()->Yield();
    }

    // listener goes out of scope here, freeing the port.
    CLogger::Get()->Write(WEBUI_LOG, LogNotice, "Web UI stopped");
    s_started = FALSE;
  }

private:
  CNetSubSystem *m_network;
  CWebUiWorker *m_workers[WEBUI_WORKERS];
};

}  // namespace

void WebUiStart(CNetSubSystem *network, const char *pin) {
  if (s_started || network == 0) {
    return;
  }
  s_started = TRUE;
  s_pin[0] = '\0';
  if (pin != 0) {
    strncpy(s_pin, pin, sizeof(s_pin) - 1);
    s_pin[sizeof(s_pin) - 1] = '\0';
  }
  new CWebUiTask(network);  // registers itself with the scheduler
}

bool WebUiBusy(void) { return s_busy > 0; }

#else  // no Circle networking on this machine build

void WebUiStart(CNetSubSystem *, const char *) {}

bool WebUiBusy(void) { return false; }

#endif
