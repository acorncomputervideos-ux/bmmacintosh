/*
 * dav_server.cpp - the network drive (src/webui/webdav.cpp) on the PC.
 *
 *   dav_server <folder> <port>
 *
 * Serves the folder as the SD card, one connection at a time, reading each
 * request as the web UI's server does (src/webui/webui.cpp: the headers up
 * to 4 KB, then WebDavHandle(), then the connection is closed), so that
 * tools/webdav/run_tests.sh can try WebDAV clients against it.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "../../src/webui/webdav.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" void sd_posix_root(const char *path);

// The clock src/port/transfer.c times transfers with (the kernel's on the Pi).
extern "C" double host_time(void) {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

class SocketConnection : public CWebDavConnection {
public:
	explicit SocketConnection(int fd) : m_fd(fd) {}
	int Receive(void *buffer, unsigned length) override {
		return (int) recv(m_fd, buffer, length, 0);
	}
	bool Send(const void *data, unsigned length) override {
		const char *p = (const char *) data;
		while (length > 0) {
			ssize_t n = send(m_fd, p, length, MSG_NOSIGNAL);
			if (n <= 0) return false;
			p += n;
			length -= (unsigned) n;
		}
		return true;
	}
	void Yield(void) override {}
private:
	int m_fd;
};

static void handle(int fd) {
	struct timeval tv = {8, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	char request[4096];
	unsigned length = 0;
	for (;;) {
		if (length >= sizeof(request) - 1) return;
		ssize_t n = recv(fd, request + length, sizeof(request) - 1 - length, 0);
		if (n <= 0) return;
		length += (unsigned) n;
		request[length] = '\0';
		if (strstr(request, "\r\n\r\n")) break;
	}
	const unsigned char *body = (const unsigned char *) (strstr(request, "\r\n\r\n") + 4);
	unsigned prefetched = (unsigned) ((const unsigned char *) (request + length) - body);
	char method[16], target[4096];
	if (sscanf(request, "%15s %4095s", method, target) != 2) return;
	SocketConnection c(fd);
	if (getenv("DAV_LOG")) fprintf(stderr, "%s %s\n", method, target);
	if (WebDavWants(method, target)) {
		WebDavHandle(&c, method, target, request, body, prefetched);
	} else {
		const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		c.Send(r, (unsigned) strlen(r));
	}
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: dav_server <folder> <port>\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	sd_posix_root(argv[1]);
	int s = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short) atoi(argv[2]));
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(s, 8) != 0) {
		perror("listen");
		return 1;
	}
	for (;;) {
		int fd = accept(s, 0, 0);
		if (fd < 0) continue;
		handle(fd);
		shutdown(fd, SHUT_WR);
		// Let the client see the end before closing (as Circle's close does).
		char sink[512];
		struct timeval tv = {1, 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		while (recv(fd, sink, sizeof(sink), 0) > 0) {
		}
		close(fd);
	}
}
