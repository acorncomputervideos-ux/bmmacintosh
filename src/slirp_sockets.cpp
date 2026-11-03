//
// slirp_sockets.cpp
//
// The BSD socket calls Basilisk II's slirp makes (the Mac II's network,
// src/port/ii/ether_ii.cpp), on Circle's own sockets: slirp's sources call
// these under their usual names (src/port/ii/slirp/circle_sockets.h says
// why). Only what slirp needs: TCP connections out, UDP datagrams, and
// select() to see which are ready, never waiting.
//
// connect() starts a Circle task that connects, and returns EINPROGRESS;
// the socket is writable once it has connected (or failed, which send()
// then reports). A TCP socket's received segment is kept until it has all
// been read, as Circle drops what doesn't fit the buffer it is given; so
// is a UDP datagram, read whole by recvfrom().
//
// All on core 1: slirp runs in a Circle task, which takes turns with the
// emulator and the connecting tasks.
//
// This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
// the Raspberry Pi. See LICENSE for licensing details.
//

#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/netdevice.h>
#include <circle/sched/task.h>
#include <circle/string.h>

// Circle's "don't wait" flag, taken before newlib's <sys/socket.h>
// defines MSG_DONTWAIT again, as 1 << 20, which Circle's sockets refuse
// (SendTo() returned NET_ERROR_INVALID_VALUE: the Mac's DNS queries never
// left the Pi, issue #5).
static const int kDontWait = MSG_DONTWAIT;

#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>

namespace {

const int kMaxSockets = 32;  // fd_set holds 32 on Circle
const unsigned kBufferSize = FRAME_BUFFER_SIZE;

enum State { kFree, kNew, kConnecting, kConnected, kFailed };

struct Socket {
  State state;
  bool tcp;
  bool closed;  // closed by slirp while still connecting
  CSocket *socket;
  CIPAddress foreign_ip;
  u16 foreign_port;
  u8 buffer[kBufferSize];
  unsigned pos, length;
  // A UDP datagram's sender.
  CIPAddress from_ip;
  u16 from_port;
};

Socket s_sockets[kMaxSockets];

// The first few of each happening are logged, for a network that doesn't
// work on a Pi: where the Mac's datagrams and connections go, and what
// Circle said.
const int kLogFirst = 4;

const char *Text(const CIPAddress &ip) {
  static char text[4][16];
  static unsigned next;
  char *t = text[next++ % 4];
  const u8 *b = ip.Get();
  snprintf(t, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return t;
}

Socket *Get(int fd) {
  if (fd < 0 || fd >= kMaxSockets || s_sockets[fd].state == kFree) {
    errno = EBADF;
    return 0;
  }
  return &s_sockets[fd];
}

void FreeSlot(Socket *s) {
  delete s->socket;
  s->socket = 0;
  s->state = kFree;
}

// Connects a TCP socket, waiting as long as it takes.
class CConnectTask : public CTask {
public:
  explicit CConnectTask(Socket *s) : mSocket(s) {}

  void Run(void) override {
    Socket *s = mSocket;
    CSocket *socket = new CSocket(CNetSubSystem::Get(), IPPROTO_TCP);
    int result = socket->Connect(s->foreign_ip, s->foreign_port);
    bool ok = result >= 0;
    static int logged;
    if (logged < kLogFirst) {
      logged++;
      printf("BMMacintosh: slirp: TCP to %s:%u %s (%d)\n", Text(s->foreign_ip),
             s->foreign_port, ok ? "connected" : "failed", result);
    }
    if (s->closed) {
      delete socket;
      s->closed = false;
      s->state = kFree;
      return;
    }
    if (ok) {
      s->socket = socket;
      s->state = kConnected;
    } else {
      delete socket;
      s->state = kFailed;
    }
  }

private:
  Socket *mSocket;
};

// Fills the buffer if it is empty and something has arrived. Returns
// false at the end of a TCP connection (or an error).
bool Fill(Socket *s) {
  if (s->pos < s->length) {
    return true;
  }
  s->pos = s->length = 0;
  if (s->socket == 0) {
    return s->state != kFailed;
  }
  int got;
  if (s->tcp) {
    got = s->socket->Receive(s->buffer, kBufferSize, kDontWait);
  } else {
    got = s->socket->ReceiveFrom(s->buffer, kBufferSize, kDontWait,
                                 &s->from_ip, &s->from_port);
    // A UDP socket has no end: an error is only nothing to read. Reported
    // as readable, it would make slirp send the Mac an ICMP "unreachable"
    // for its lookup.
    static int logged;
    if (got != 0 && logged < kLogFirst) {
      logged++;
      if (got < 0) {
        printf("BMMacintosh: slirp: UDP receive failed (%d)\n", got);
      } else {
        printf("BMMacintosh: slirp: UDP %d bytes from %s:%u\n", got, Text(s->from_ip),
               s->from_port);
      }
    }
    if (got < 0) {
      got = 0;
    }
  }
  if (got < 0) {
    return false;
  }
  s->length = (unsigned)got;
  return true;
}

void ToSockaddr(const CIPAddress &ip, u16 port, struct sockaddr *address,
                socklen_t *length) {
  if (address == 0 || length == 0) {
    return;
  }
  struct sockaddr_in in;
  memset(&in, 0, sizeof in);
  in.sin_family = AF_INET;
  in.sin_port = htons(port);
  ip.CopyTo((u8 *)&in.sin_addr.s_addr);
  socklen_t n = *length < sizeof in ? *length : sizeof in;
  memcpy(address, &in, n);
  *length = sizeof in;
}

}  // namespace

extern "C" {

int bmslirp_socket(int domain, int type, int protocol) {
  (void)protocol;
  CNetSubSystem *net = CNetSubSystem::Get();
  if (net == 0 || !net->IsRunning()) {
    errno = ENETDOWN;
    return -1;
  }
  if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM)) {
    errno = EPROTONOSUPPORT;
    return -1;
  }
  for (int fd = 0; fd < kMaxSockets; fd++) {
    Socket *s = &s_sockets[fd];
    if (s->state != kFree) {
      continue;
    }
    s->state = kNew;
    s->tcp = type == SOCK_STREAM;
    s->closed = false;
    s->pos = s->length = 0;
    s->socket = 0;
    if (!s->tcp) {
      // Bound now (to a port of Circle's choosing), so that it can be read
      // before it has sent anything, as BMNeXT's host_udp_open() does.
      s->socket = new CSocket(net, IPPROTO_UDP);
      int result = s->socket->Bind(0);
      if (result < 0) {
        printf("BMMacintosh: slirp: no UDP socket (%d)\n", result);
        FreeSlot(s);
        errno = ENOBUFS;
        return -1;
      }
    }
    return fd;
  }
  errno = EMFILE;
  return -1;
}

int bmslirp_connect(int fd, const struct sockaddr *address, socklen_t length) {
  Socket *s = Get(fd);
  if (s == 0) {
    return -1;
  }
  if (!s->tcp || s->state != kNew || address == 0 ||
      length < sizeof(struct sockaddr_in)) {
    errno = EINVAL;
    return -1;
  }
  const struct sockaddr_in *in = (const struct sockaddr_in *)address;
  s->foreign_ip.Set(in->sin_addr.s_addr);
  s->foreign_port = ntohs(in->sin_port);
  s->state = kConnecting;
  new CConnectTask(s);
  errno = EINPROGRESS;
  return -1;
}

// slirp listens only for redirections from the Pi's own ports, which
// BMMacintosh doesn't set up.
int bmslirp_bind(int fd, const struct sockaddr *address, socklen_t length) {
  (void)address;
  (void)length;
  return Get(fd) != 0 ? 0 : -1;
}

int bmslirp_listen(int fd, int backlog) {
  (void)fd;
  (void)backlog;
  errno = EOPNOTSUPP;
  return -1;
}

int bmslirp_accept(int fd, struct sockaddr *address, socklen_t *length) {
  (void)fd;
  (void)address;
  (void)length;
  errno = EOPNOTSUPP;
  return -1;
}

ssize_t bmslirp_send(int fd, const void *buffer, size_t length, int flags) {
  (void)flags;
  Socket *s = Get(fd);
  if (s == 0) {
    return -1;
  }
  if (s->state == kConnecting || s->state == kNew) {
    errno = s->state == kNew ? ENOTCONN : EINPROGRESS;
    return -1;
  }
  if (s->state == kFailed || s->socket == 0) {
    errno = ECONNREFUSED;
    return -1;
  }
  if (length == 0) {
    return 0;
  }
  int sent = s->socket->Send(buffer, length, kDontWait);
  if (sent < 0) {
    errno = EPIPE;
    return -1;
  }
  if (sent == 0) {
    errno = EWOULDBLOCK;
    return -1;
  }
  return sent;
}

ssize_t bmslirp_recv(int fd, void *buffer, size_t length, int flags) {
  (void)flags;
  Socket *s = Get(fd);
  if (s == 0) {
    return -1;
  }
  if (s->state != kConnected) {
    errno = s->state == kFailed ? ECONNREFUSED : ENOTCONN;
    return -1;
  }
  if (!Fill(s)) {
    return 0;  // the end of the connection
  }
  unsigned n = s->length - s->pos;
  if (n == 0) {
    errno = EWOULDBLOCK;
    return -1;
  }
  if (n > length) {
    n = length;
  }
  memcpy(buffer, s->buffer + s->pos, n);
  s->pos += n;
  return n;
}

ssize_t bmslirp_sendto(int fd, const void *buffer, size_t length, int flags,
                       const struct sockaddr *address, socklen_t address_length) {
  (void)flags;
  Socket *s = Get(fd);
  if (s == 0) {
    return -1;
  }
  if (s->tcp || address == 0 || address_length < sizeof(struct sockaddr_in)) {
    return bmslirp_send(fd, buffer, length, flags);
  }
  const struct sockaddr_in *in = (const struct sockaddr_in *)address;
  CIPAddress ip(in->sin_addr.s_addr);
  int sent = s->socket->SendTo(buffer, length, kDontWait, ip, ntohs(in->sin_port));
  static int logged;
  if (logged < kLogFirst) {
    logged++;
    printf("BMMacintosh: slirp: UDP %u bytes to %s:%u (%d)\n", (unsigned)length, Text(ip),
           ntohs(in->sin_port), sent);
  }
  if (sent < 0) {
    errno = EHOSTUNREACH;
    return -1;
  }
  return sent;
}

ssize_t bmslirp_recvfrom(int fd, void *buffer, size_t length, int flags,
                         struct sockaddr *address, socklen_t *address_length) {
  Socket *s = Get(fd);
  if (s == 0) {
    return -1;
  }
  if (s->tcp) {
    return bmslirp_recv(fd, buffer, length, flags);
  }
  if (!Fill(s) || s->length == 0) {
    errno = EWOULDBLOCK;
    return -1;
  }
  // A datagram is read whole; what doesn't fit is lost, as with BSD's.
  unsigned n = s->length - s->pos;
  if (n > length) {
    n = length;
  }
  memcpy(buffer, s->buffer + s->pos, n);
  s->pos = s->length = 0;
  ToSockaddr(s->from_ip, s->from_port, address, address_length);
  return n;
}

// Never waits: slirp's task does, between calls.
int bmslirp_select(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *errorfds, struct timeval *timeout) {
  (void)timeout;
  int ready = 0;
  if (nfds > kMaxSockets) {
    nfds = kMaxSockets;
  }
  for (int fd = 0; fd < nfds; fd++) {
    Socket *s = &s_sockets[fd];
    bool want_read = readfds != 0 && FD_ISSET(fd, readfds);
    bool want_write = writefds != 0 && FD_ISSET(fd, writefds);
    if (errorfds != 0) {
      FD_CLR(fd, errorfds);
    }
    bool can_read = false, can_write = false;
    if (s->state == kConnected || (!s->tcp && s->state == kNew)) {
      if (want_read) {
        // Ready with data, or at the end of a TCP connection (Fill()
        // never fails for UDP).
        can_read = !Fill(s) || s->pos < s->length;
      }
      if (want_write) {
        can_write = !s->tcp || s->socket->GetStatus().bTxReady;
      }
    } else if (s->state == kFailed) {
      can_read = want_read;
      can_write = want_write;
    }
    if (want_read && !can_read) {
      FD_CLR(fd, readfds);
    }
    if (want_write && !can_write) {
      FD_CLR(fd, writefds);
    }
    ready += can_read + can_write;
  }
  return ready;
}

int bmslirp_close(int fd) {
  Socket *s = Get(fd);
  if (s == 0) {
    return -1;
  }
  if (s->state == kConnecting) {
    s->closed = true;  // its task frees it
    return 0;
  }
  FreeSlot(s);
  return 0;
}

// Circle can't close one direction of a connection alone: the Mac's end
// closes when slirp closes the socket.
int bmslirp_shutdown(int fd, int how) {
  (void)how;
  return Get(fd) != 0 ? 0 : -1;
}

int bmslirp_getsockname(int fd, struct sockaddr *address, socklen_t *length) {
  Socket *s = Get(fd);
  if (s == 0) {
    return -1;
  }
  CIPAddress any((u32)0);
  ToSockaddr(any, s->socket != 0 ? s->socket->GetOwnPort() : 0, address, length);
  return 0;
}

int bmslirp_setsockopt(int fd, int level, int option, const void *value,
                       socklen_t length) {
  (void)level;
  (void)option;
  (void)value;
  (void)length;
  return Get(fd) != 0 ? 0 : -1;
}

// The sockets never wait anyway (fd_nonblock()).
int bmslirp_fcntl(int fd, int command, ...) {
  (void)command;
  return Get(fd) != 0 ? 0 : -1;
}

int bmslirp_gethostname(char *name, size_t length) {
  snprintf(name, length, "bmmacintosh");
  return 0;
}

// slirp looks its own name up for the Pi's address, used only for
// redirections; without, it takes 127.0.0.1.
struct hostent *bmslirp_gethostbyname(const char *name) {
  (void)name;
  return 0;
}

in_addr_t bmslirp_inet_addr(const char *text) {
  struct in_addr address;
  if (inet_pton(AF_INET, text, &address) != 1) {
    return (in_addr_t)0xffffffff;  // INADDR_NONE
  }
  return address.s_addr;
}

// For slirp's "exec" redirections, which BMMacintosh doesn't set up.
uid_t bmslirp_getuid(void) { return 0; }

int bmslirp_execvp(const char *file, char *const argv[]) {
  (void)file;
  (void)argv;
  errno = ENOSYS;
  return -1;
}

int bmslirp_chown(const char *path, uid_t owner, gid_t group) {
  (void)path;
  (void)owner;
  (void)group;
  return 0;
}

int bmslirp_chmod(const char *path, mode_t mode) {
  (void)path;
  (void)mode;
  return 0;
}

char *bmslirp_inet_ntoa(struct in_addr address) {
  static char text[16];
  const u8 *b = (const u8 *)&address.s_addr;
  snprintf(text, sizeof text, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return text;
}

FILE *bmslirp_fopen(const char *path, const char *mode) {
  static char text[64];
  CNetSubSystem *net = CNetSubSystem::Get();
  if (strcmp(path, "/etc/resolv.conf") != 0 || net == 0) {
    return fopen(path, mode);
  }
  // Empty until the Pi's network is up and knows its DNS server
  // (src/port/ii/slirp/slirp_start.c asks again later).
  const CIPAddress *dns = net->IsRunning() ? net->GetConfig()->GetDNSServer() : 0;
  text[0] = '\0';
  if (dns != 0 && !dns->IsNull()) {
    CString address;
    dns->Format(&address);
    snprintf(text, sizeof text, "nameserver %s\n", (const char *)address);
  }
  return fmemopen(text, strlen(text) > 0 ? strlen(text) : 1, "r");
}

}  // extern "C"
