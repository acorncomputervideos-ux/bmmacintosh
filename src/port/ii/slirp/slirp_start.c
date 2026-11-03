/*
 * slirp_start.c - starting Basilisk II's slirp before the Pi's network is
 * up (src/port/ii/ether_ii.cpp).
 *
 * The Mac asks for its address (DHCP) as Mac OS starts, seconds after the
 * Pi does, and gives up after a while; the Pi's Wi-Fi can take longer than
 * that to connect. slirp answers DHCP itself, so it is started at once.
 * But slirp_init() wants a DNS server, which the Pi only knows once its
 * network is up, and without one it stops before setting slirp's own
 * addresses (10.0.2.x): bmslirp_start() sets them all the same, and
 * bmslirp_find_dns() takes the DNS server from /etc/resolv.conf (on the
 * Pi, src/slirp_sockets.cpp's) later.
 *
 * Compiled with slirp's sources and flags.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include <slirp.h>

/* Starts slirp. TRUE if it found a DNS server too. */
int bmslirp_start(void)
{
	if (slirp_init() == 0)
		return 1;
	inet_aton(CTL_SPECIAL, &special_addr);
	alias_addr.s_addr = special_addr.s_addr | htonl(CTL_ALIAS);
	getouraddr();
	return 0;
}

/* The first nameserver in /etc/resolv.conf becomes slirp's DNS server (the
   Mac's 10.0.2.3). TRUE if there is one. */
int bmslirp_find_dns(void)
{
	char line[256], name[64];
	struct in_addr address;
	int found = 0;
	FILE *f = fopen("/etc/resolv.conf", "r");
	if (f == NULL)
		return 0;
	while (!found && fgets(line, sizeof line, f) != NULL) {
		if (sscanf(line, "nameserver%*[ \t]%63s", name) == 1 && inet_aton(name, &address)
		    && address.s_addr != 0) {
			getouraddr();
			if (address.s_addr == loopback_addr.s_addr)
				address = our_addr;
			dns_addr = address;
			found = 1;
		}
	}
	fclose(f);
	return found;
}
