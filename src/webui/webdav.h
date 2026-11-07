//
// webdav.h - the SD card as a network drive (WebDAV).
//
// The web UI's server (webui.cpp) hands WebDAV requests here: the card is
// at http://<the Pi>/sd/, which Windows (Map network drive), macOS (Finder,
// Go, Connect to Server) and Linux file managers open as a folder of their
// own. See docs/USER_GUIDE.md.
//
// Only this file's interface and sd_fs.h are used, so it builds on the PC
// too, where tools/webdav tests it with real WebDAV clients.
//
// This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
// the Raspberry Pi. See LICENSE for licensing details.
//

#ifndef _webdav_h
#define _webdav_h

// Where the card is in the server's URLs.
#define WEBDAV_PREFIX "/sd"

// What the WebDAV code reads and writes the request through.
class CWebDavConnection {
public:
  virtual ~CWebDavConnection(void) {}
  // Like CSocket::Receive(): bytes read, 0 or less when the client has gone
  // or timed out.
  virtual int Receive(void *buffer, unsigned length) = 0;
  // Sends all of it; false on an error.
  virtual bool Send(const void *data, unsigned length) = 0;
  // Lets the network and the emulator run during long work.
  virtual void Yield(void) = 0;
};

// Whether a request is the network drive's: a WebDAV method anywhere
// (OPTIONS, PROPFIND, PUT, ...), or GET or HEAD under WEBDAV_PREFIX.
bool WebDavWants(const char *method, const char *target);

// Answers one request and leaves the connection to be closed. request is
// the request line and headers (NUL-terminated, up to the blank line at
// least); body/body_len is what of the body arrived with them.
void WebDavHandle(CWebDavConnection *connection, const char *method,
                  const char *target, const char *request,
                  const unsigned char *body, unsigned body_len);

#endif
