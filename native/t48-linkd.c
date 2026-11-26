// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-linkd: the Ethernet cable put back (issue #77, docs/sip.md).
 *
 *   t48-linkd [-i IFACE] [-c PROGRAM]     eth0 and /usr/bin/t48-apply
 *
 * Watches the link's carrier on a netlink socket (no polling). When it comes
 * back up, runs "PROGRAM link-up": a new DHCP lease at once, keeping the
 * address while it is asked for, so a phone moved to another network takes
 * its address there. At most once in 30 s: one sooner than that is put off
 * to 30 s after the last, and dropped if the cable goes again first
 * (native/linkwatch.h, tested on a fake clock in sip/test/linkwatch.c).
 * Registering again is baresip's part (t48keepalive). Always running, as the
 * runit service "link", account or none.
 *
 * Lines it prints (in /var/log/t48/link/current on the phone):
 *   t48-linkd: eth0 down
 *   t48-linkd: eth0 up, new DHCP lease
 *   t48-linkd: eth0 up again soon after, waiting 30 s
 * The program runs with no descriptors but 0, 1 and 2, so nothing it starts
 * (udhcpc, ntpd) holds on to ours. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "linkwatch.h"

#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif

static const char *ifname = "eth0";
static const char *program = "/usr/bin/t48-apply";

static uint64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void link_up(void)
{
	pid_t pid;

	printf("t48-linkd: %s up, new DHCP lease\n", ifname);
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		int fd, max = (int)sysconf(_SC_OPEN_MAX);

		for (fd = 3; fd < (max > 0 ? max : 1024); fd++)
			close(fd);
		execl(program, program, "link-up", (char *)NULL);
		_exit(127);
	}
	/* t48-apply starts udhcpc in the background and returns */
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
}

int main(int argc, char **argv)
{
	struct sockaddr_nl sa;
	struct { struct nlmsghdr h; struct rtgenmsg g; } dump;
	static char buf[16384];
	struct linkwatch lw;
	uint64_t due = 0;      /* an up put off till then; 0: none */
	int shown = -1;        /* the carrier last seen: -1 not yet */
	int fd, c;

	while ((c = getopt(argc, argv, "i:c:")) != -1) {
		if (c == 'i')
			ifname = optarg;
		else if (c == 'c')
			program = optarg;
		else {
			fprintf(stderr, "usage: t48-linkd [-i IFACE] [-c PROGRAM]\n");
			return 2;
		}
	}

	memset(&lw, 0, sizeof lw);
	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	memset(&sa, 0, sizeof sa);
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_LINK;
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		fprintf(stderr, "t48-linkd: no netlink: %s\n", strerror(errno));
		return 1;
	}
	/* every link's state now, so the first change is seen as one */
	memset(&dump, 0, sizeof dump);
	dump.h.nlmsg_len = sizeof dump;
	dump.h.nlmsg_type = RTM_GETLINK;
	dump.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	dump.g.rtgen_family = AF_UNSPEC;
	(void)send(fd, &dump, sizeof dump, 0);
	printf("t48-linkd: watching %s\n", ifname);
	fflush(stdout);

	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		int timeout = -1;
		ssize_t n;

		if (due) {
			uint64_t t = now_ms();

			if (t >= due) {
				due = 0;
				linkwatch_fired(&lw, t);
				link_up();
				continue;
			}
			timeout = (int)(due - t);
		}
		if (poll(&p, 1, timeout) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (!(p.revents & POLLIN))
			continue;
		n = recv(fd, buf, sizeof buf, 0);
		if (n < 0) {
			/* ENOBUFS: events lost; the next one puts it right */
			continue;
		}
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n);
		     h = NLMSG_NEXT(h, n)) {
			struct ifinfomsg *ifi = NLMSG_DATA(h);
			char name[IF_NAMESIZE];
			uint64_t when = 0, t;
			bool up;

			if (h->nlmsg_type != RTM_NEWLINK ||
			    !if_indextoname((unsigned)ifi->ifi_index, name) ||
			    strcmp(name, ifname))
				continue;
			up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_LOWER_UP);
			t = now_ms();
			switch (linkwatch_event(&lw, ifi->ifi_index, up, t, &when)) {
			case LW_NOW:
				due = 0;
				link_up();
				break;
			case LW_LATER:
				printf("t48-linkd: %s up again soon after, waiting %llu s\n",
				       ifname, (unsigned long long)(when - t + 999) / 1000);
				fflush(stdout);
				due = when;
				break;
			case LW_CANCEL:
				due = 0;
				break;
			default:
				break;
			}
			/* a down is said once per change (the first word is not) */
			if (!up && shown == 1) {
				printf("t48-linkd: %s down\n", ifname);
				fflush(stdout);
			}
			shown = up;
		}
	}
}
