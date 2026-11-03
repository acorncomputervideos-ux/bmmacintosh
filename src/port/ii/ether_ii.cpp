/*
 * ether_ii.cpp - the Mac II's Ethernet card, for Basilisk II: its network
 * through slirp.
 *
 * Basilisk II's Ethernet card (ether.cpp, slot_rom.cpp) hands the frames
 * the Mac sends here, and takes the ones for it from here. Basilisk II's
 * slirp (third_party/macemu/BasiliskII/src/slirp) answers them as a small
 * network of its own, 10.0.2.0/24, as QEMU's user networking does: a DHCP
 * server gives the Mac 10.0.2.15, 10.0.2.2 is the router and 10.0.2.3 the
 * DNS server, and the Mac's TCP connections and UDP datagrams go out
 * through the Pi's own (Ethernet or Wi-Fi), with the Pi's address. No
 * AppleTalk, and nothing can connect to the Mac from outside.
 *
 * slirp runs in the background (host_run_in_background()), passing frames
 * between the Mac and its sockets: on the Pi Circle's
 * (src/slirp_sockets.cpp), on a PC the system's. It starts at once, so
 * that it answers the Mac's DHCP as Mac OS starts, before the Pi's network
 * is up (Wi-Fi can take longer than the Mac waits); its DNS server comes
 * when the Pi's network does (src/port/ii/slirp/slirp_start.c).
 * The frames go between it and the emulator through two queues, each with
 * one side writing and the other reading; once a frame tick (main_ii.cpp,
 * ii_ether_tick()) sees frames for the Mac, the Ethernet interrupt passes
 * them to its protocols (EtherInterrupt()).
 *
 * "nonet true" in macii.prefs turns it off. From Basilisk II's Unix
 * ether_unix.cpp.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include "sysdeps.h"

#include <map>

#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "ether.h"
#include "ether_defs.h"

#include "../../../third_party/macemu/BasiliskII/src/slirp/libslirp.h"

#include "../../host.h"
#include "../bmmac.h"
#include "ii.h"

#ifdef __circle__
/* slirp's sockets (src/slirp_sockets.cpp), not newlib's. */
extern "C" int bmslirp_select(int nfds, fd_set *readfds, fd_set *writefds,
                              fd_set *errorfds, struct timeval *timeout);
#define select bmslirp_select
#else
#include <unistd.h>
#endif

/* ---- The queues ------------------------------------------------------------------- */

#define MAX_FRAME 1516
#define QUEUE_FRAMES 64

struct frame {
	uint16 length;
	uint8 data[MAX_FRAME];
};

struct frame_queue {
	frame frames[QUEUE_FRAMES];
	unsigned head;   /* written by the writer only */
	unsigned tail;   /* written by the reader only */
};

static frame_queue to_slirp;   /* from the Mac */
static frame_queue to_mac;

static bool queue_put(frame_queue *q, const uint8 *data, int length)
{
	unsigned head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
	unsigned tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
	if (length <= 0 || length > MAX_FRAME || head - tail >= QUEUE_FRAMES)
		return false;
	frame *f = &q->frames[head % QUEUE_FRAMES];
	memcpy(f->data, data, length);
	f->length = (uint16) length;
	__atomic_store_n(&q->head, head + 1, __ATOMIC_RELEASE);
	return true;
}

/* The oldest frame, or NULL; queue_drop() when done with it. */
static frame *queue_peek(frame_queue *q)
{
	unsigned tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
	unsigned head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
	return head == tail ? NULL : &q->frames[tail % QUEUE_FRAMES];
}

static void queue_drop(frame_queue *q)
{
	__atomic_store_n(&q->tail, q->tail + 1, __ATOMIC_RELEASE);
}

/* ---- slirp, in the background ----------------------------------------------------- */

static volatile bool net_running;     /* slirp is set up */
static bool net_started;

/* src/port/ii/slirp/slirp_start.c */
extern "C" int bmslirp_start(void);
extern "C" int bmslirp_find_dns(void);

/* slirp's frames for the Mac. */
extern "C" int slirp_can_output(void)
{
	return 1;
}

extern "C" void slirp_output(const uint8 *packet, int length)
{
	/* When the Mac is slow to take them, they are lost, as on a real
	   network. */
	static bool told;
	if (!told) {
		told = true;
		printf("BMMacintosh: the Mac's network answered it\n");
	}
	queue_put(&to_mac, packet, length);
}

static void slirp_loop(void *arg)
{
	(void) arg;
	char address[64];
	bool have_dns = bmslirp_start() != 0, announced = false;
	double last_look = 0.0;
	net_running = true;
	for (;;) {
		/* Once a second until the Pi's network is up: then the DNS server,
		   if slirp didn't find one at the start. */
		if (!announced && host_time() - last_look >= 1.0
		    && (last_look = host_time(), host_network_address(address, sizeof address))) {
			if (!have_dns)
				have_dns = bmslirp_find_dns() != 0;
			announced = true;
			printf("BMMacintosh: the Mac's network is up (10.0.2.15, through %s)%s\n", address,
			       have_dns ? "" : ", but it has no DNS server");
		}
		frame *f;
		while ((f = queue_peek(&to_slirp)) != NULL) {
			slirp_input(f->data, f->length);
			queue_drop(&to_slirp);
		}
		fd_set rfds, wfds, xfds;
		int nfds = -1;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&xfds);
		slirp_select_fill(&nfds, &rfds, &wfds, &xfds);
		struct timeval tv = { 0, 0 };
		if (select(nfds + 1, &rfds, &wfds, &xfds, &tv) >= 0)
			slirp_select_poll(&rfds, &wfds, &xfds);
#ifdef __circle__
		/* The emulator's turn, and Circle's tasks'. */
		host_yield();
#else
		usleep(2000);
#endif
	}
}

/* ---- Basilisk II's Ethernet driver ------------------------------------------------ */

/* The protocols the Mac attached, by type, and their handlers. */
static std::map<uint16, uint32> net_protocols;

bool ether_init(void)
{
	/* slirp's address for the Mac's card (Basilisk II's). */
	static const uint8 address[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
	memcpy(ether_addr, address, 6);
	if (PrefsFindBool("nonet"))
		return false;
	if (!net_started) {
		/* Once: slirp keeps its connections across the Mac's restarts. */
		net_started = true;
		if (!host_run_in_background(slirp_loop, NULL)) {
			printf("BMMacintosh: cannot start the Mac's network\n");
			return false;
		}
	}
	return true;
}

void ether_exit(void)
{
}

void ether_reset(void)
{
	net_protocols.clear();
}

int16 ether_add_multicast(uint32 pb)
{
	(void) pb;
	return noErr;
}

int16 ether_del_multicast(uint32 pb)
{
	(void) pb;
	return noErr;
}

int16 ether_attach_ph(uint16 type, uint32 handler)
{
	if (net_protocols.find(type) != net_protocols.end())
		return lapProtErr;
	net_protocols[type] = handler;
	return noErr;
}

int16 ether_detach_ph(uint16 type)
{
	if (net_protocols.erase(type) == 0)
		return lapProtErr;
	return noErr;
}

int16 ether_write(uint32 wds)
{
	uint8 packet[MAX_FRAME];
	int length = ether_wds_to_buffer(wds, packet);
	static bool told;
	if (!told) {
		told = true;
		printf("BMMacintosh: the Mac sends on its network%s\n",
		       net_running ? "" : " (not up yet)");
	}
	if (!net_running)
		return noErr;     /* no network yet: as if the cable were out */
	return queue_put(&to_slirp, packet, length) ? noErr : excessCollsns;
}

bool ether_start_udp_thread(int socket_fd)
{
	(void) socket_fd;
	return false;
}

void ether_stop_udp_thread(void)
{
}

/* Passes a frame to the protocol the Mac attached for its type. */
static void ether_dispatch_packet(uint32 p, uint32 length)
{
	uint16 type = ReadMacInt16(p + 12);
	uint16 search_type = type <= 1500 ? 0 : type;
	std::map<uint16, uint32>::iterator it = net_protocols.find(search_type);
	if (it == net_protocols.end() || it->second == 0)
		return;
	/* The header goes to the read header area. */
	Mac2Mac_memcpy(ether_data + ed_RHA, p, 14);
	M68kRegisters r;
	r.d[0] = type;
	r.d[1] = length - 14;                    /* the rest, for ReadPacket */
	r.a[0] = p + 14;
	r.a[3] = ether_data + ed_RHA + 14;
	r.a[4] = ether_data + ed_ReadPacket;
	Execute68k(it->second, &r);
}

void EtherInterrupt(void)
{
	EthernetPacket ether_packet;
	uint32 packet = ether_packet.addr();
	frame *f;
	while ((f = queue_peek(&to_mac)) != NULL) {
		int length = f->length;
		if (length >= 14) {
			Host2Mac_memcpy(packet, f->data, length);
			queue_drop(&to_mac);
			ether_dispatch_packet(packet, length);
		} else {
			queue_drop(&to_mac);
		}
	}
}

/* Each frame tick: the Ethernet interrupt, when there are frames for the
   Mac. */
void ii_ether_tick(void)
{
	if (queue_peek(&to_mac) != NULL) {
		SetInterruptFlag(INTFLAG_ETHER);
		TriggerInterrupt();
	}
}
