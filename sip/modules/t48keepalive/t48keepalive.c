/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48keepalive: NAT keepalives, issue #77. While an account is registered it
 * sends the server an OPTIONS request on the account's own transport and
 * route (the outbound proxy, if any), every 23 to 29 s over UDP and every 96
 * to 120 s over TCP and TLS: RFC 5626's intervals, spread the same way libre
 * spreads its own. That keeps a NAT router's binding for the phone's SIP
 * port open between registrations, so calls still reach it.
 *
 * Why OPTIONS and not libre's own keepalives: libre only starts those when
 * the registrar does SIP outbound (RFC 5626), which most providers do not;
 * its UDP keepalive is a STUN request that most SIP servers ignore; and its
 * TCP keepalive closes the connection when the server does not answer the
 * CRLF ping, which servers without outbound do not. Every SIP server
 * answers OPTIONS, with 200, 401, 404 or 405; any answer will do.
 *
 * Two things start a new registration, which tells the server where the
 * phone is now. An answer that shows the phone's public address or port
 * has changed (the server's received and rport on our Via): the router lost
 * the binding and made a new one, and calls would go to the old one until
 * the next registration. And, over TCP and TLS only, an OPTIONS that gets no
 * answer at all (the connection has gone), once per run of failures. Over
 * UDP no answer only means packets are lost for now: libre's own refresh
 * deals with an outage, and the address check catches a new binding once
 * answers come back.
 *
 * A registration that fails is retried by libre alone, with its back-off
 * (sip/patches/re-0002 covers one that could not be sent). Nothing here
 * registers while an account is not registered, as that would start the
 * back-off again from the top; and no OPTIONS are sent then. The one
 * exception: when a network link comes back (carrier up, on a netlink
 * socket), every account registers at once, so a cable put back in is not
 * left waiting out a back-off of up to 30 min. The address may be the same
 * one, so baresip is not restarted for it.
 *
 * An account opts out with ;keepalive=no (t48-sip writes that for
 * keepalive=off).
 */
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <re.h>
#include <baresip.h>

#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000     /* carrier: linux/if.h, not in net/if.h */
#endif

enum {
	UDP_INTERVAL = 29,       /* seconds, RFC 5626 section 4.4.1 */
	STREAM_INTERVAL = 120,   /* TCP and TLS */
};

struct ka {
	struct le le;
	struct tmr tmr;
	struct ua *ua;            /* not a reference: dropped before the ua goes */
	char *uri;                /* the server: the account's URI without the user */
	uint32_t interval;
	unsigned id;              /* the response handler finds us by this */
	bool stream;              /* TCP or TLS */
	bool registered;
	bool pending;
	bool failed;              /* the last OPTIONS had no answer */
	struct sa server;         /* the server that last answered */
	char mapped[64];          /* our public address:port, as it saw it */
};

static struct list kal;
static unsigned next_id;

/* links seen on the netlink socket: was each one's carrier up */
enum { KA_LINKS = 8 };
static struct { int index; bool up; } links[KA_LINKS];
static unsigned nlinks;
static re_sock_t nlfd = RE_BAD_SOCK;
static struct re_fhs *nlfhs;


static void ka_destructor(void *arg)
{
	struct ka *ka = arg;

	tmr_cancel(&ka->tmr);
	list_unlink(&ka->le);
	mem_deref(ka->uri);
}


static struct ka *ka_find(const struct ua *ua)
{
	struct le *le;

	for (le = kal.head; le; le = le->next) {
		struct ka *ka = le->data;

		if (ka->ua == ua)
			return ka;
	}

	return NULL;
}


static struct ka *ka_find_id(unsigned id)
{
	struct le *le;

	for (le = kal.head; le; le = le->next) {
		struct ka *ka = le->data;

		if (ka->id == id)
			return ka;
	}

	return NULL;
}


/* 80 to 100 per cent of the interval, in ms */
static uint64_t wait_ms(uint32_t interval)
{
	return (uint64_t)interval * (800 + rand_u16() % 201);
}


static void timeout(void *arg);


/* where the server saw the request come from, from its Via; empty if it
 * did not say */
static void public_addr(char *out, size_t n, const struct sip_msg *msg)
{
	struct pl rec, rport;

	out[0] = '\0';
	if (msg_param_decode(&msg->via.params, "rport", &rport) ||
	    !pl_isset(&rport))
		return;
	if (msg_param_decode(&msg->via.params, "received", &rec))
		(void)re_snprintf(out, n, "%j:%r", &msg->via.addr, &rport);
	else
		(void)re_snprintf(out, n, "%r:%r", &rec, &rport);
}


/* register again. The caller's last use of ka for this event */
static void reregister(struct ka *ka)
{
	(void)ua_register(ka->ua);
}


static void resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct ka *ka = ka_find_id((unsigned)(uintptr_t)arg);
	const char *aor;
	char now[64];
	bool moved;

	/* the account unregistered while this was in flight */
	if (!ka)
		return;

	ka->pending = false;
	if (!ka->registered)
		return;
	aor = account_aor(ua_account(ka->ua));

	if (err || !msg) {
		if (ka->failed || !ka->stream)
			return;
		ka->failed = true;
		info("t48keepalive: %s: no answer from the server (%m),"
		     " registering again\n", aor, err);
		reregister(ka);
		return;
	}

	ka->failed = false;
	public_addr(now, sizeof now, msg);
	if (!now[0])
		return;
	/* only the same server's view counts: behind a NAT that maps each
	 * destination apart, another server sees another port */
	moved = ka->mapped[0] && sa_cmp(&ka->server, &msg->src, SA_ALL) &&
		strcmp(ka->mapped, now);
	if (moved)
		info("t48keepalive: %s: public address now %s, was %s,"
		     " registering again\n", aor, now, ka->mapped);
	ka->server = msg->src;
	str_ncpy(ka->mapped, now, sizeof ka->mapped);
	if (moved)
		reregister(ka);
}


static void timeout(void *arg)
{
	struct ka *ka = arg;
	int err;

	tmr_start(&ka->tmr, wait_ms(ka->interval), timeout, ka);

	/* a request still waiting for its answer keeps the binding open too */
	if (ka->pending)
		return;

	err = ua_options_send(ka->ua, ka->uri, resp_handler,
			      (void *)(uintptr_t)ka->id);
	if (!err)
		ka->pending = true;
}


/* registered (again): keepalives from now; what the server saw before
 * this registration no longer counts */
static void registered(struct ka *ka)
{
	if (!ka->registered || !tmr_isrunning(&ka->tmr))
		tmr_start(&ka->tmr, wait_ms(ka->interval), timeout, ka);
	ka->registered = true;
	ka->failed = false;
	ka->mapped[0] = '\0';
}


static void start(struct ua *ua)
{
	struct account *acc = ua_account(ua);
	struct sip_addr *laddr = account_laddr(acc);
	struct uri uri;
	struct pl v, tp;
	struct ka *ka;

	if (laddr && !msg_param_decode(&laddr->params, "keepalive", &v) &&
	    !pl_strcasecmp(&v, "no"))
		return;

	ka = mem_zalloc(sizeof(*ka), ka_destructor);
	if (!ka)
		return;

	/* the request goes where REGISTER goes: the account's URI, no user */
	uri = *account_luri(acc);
	uri.user = pl_null;
	if (re_sdprintf(&ka->uri, "%H", uri_encode, &uri)) {
		mem_deref(ka);
		return;
	}

	ka->ua = ua;
	ka->id = ++next_id;
	ka->stream = !msg_param_decode(&uri.params, "transport", &tp) &&
		(!pl_strcasecmp(&tp, "tcp") || !pl_strcasecmp(&tp, "tls"));
	ka->interval = ka->stream ? STREAM_INTERVAL : UDP_INTERVAL;

	list_append(&kal, &ka->le, ka);
	registered(ka);
	info("t48keepalive: %s: OPTIONS to %s every %u to %u s\n",
	     account_aor(acc), ka->uri, ka->interval * 4 / 5, ka->interval);
}


/* a link's carrier came back: every account registers now */
static void link_up(const char *name)
{
	struct le *le;

	info("t48keepalive: %s up, registering again\n", name);
	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = le->data;

		if (account_regint(ua_account(ua)))
			(void)ua_register(ua);
	}
}


/* true if this link's carrier was seen down and is now up */
static bool carrier_back(int index, bool up)
{
	unsigned i;
	bool back;

	for (i = 0; i < nlinks && links[i].index != index; i++)
		;
	if (i == nlinks) {
		/* first sight: nothing to compare with */
		if (nlinks == KA_LINKS)
			return false;
		links[nlinks].index = index;
		links[nlinks++].up = up;
		return false;
	}
	back = up && !links[i].up;
	links[i].up = up;
	return back;
}


static void netlink_handler(int flags, void *arg)
{
	static char buf[16384];
	ssize_t n;
	(void)flags;
	(void)arg;

	while ((n = recv(nlfd, buf, sizeof buf, 0)) > 0) {
		struct nlmsghdr *h = (struct nlmsghdr *)(void *)buf;
		size_t len = (size_t)n;

		for (; NLMSG_OK(h, len); h = NLMSG_NEXT(h, len)) {
			struct ifinfomsg *ifi = NLMSG_DATA(h);
			char name[IF_NAMESIZE] = "link";

			if (h->nlmsg_type != RTM_NEWLINK ||
			    (ifi->ifi_flags & IFF_LOOPBACK))
				continue;
			if (!carrier_back(ifi->ifi_index,
					  (ifi->ifi_flags & IFF_UP) &&
					  (ifi->ifi_flags & IFF_LOWER_UP)))
				continue;
			(void)if_indextoname((unsigned)ifi->ifi_index, name);
			link_up(name);
		}
	}
}


static void netlink_open(void)
{
	struct sockaddr_nl sa;
	struct {
		struct nlmsghdr h;
		struct rtgenmsg g;
	} dump;

	memset(&sa, 0, sizeof sa);
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_LINK;
	nlfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (nlfd == RE_BAD_SOCK) {
		warning("t48keepalive: cannot watch network links\n");
		return;
	}
	if (net_sockopt_blocking_set(nlfd, false) ||
	    bind(nlfd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
	    fd_listen(&nlfhs, nlfd, FD_READ, netlink_handler, NULL)) {
		warning("t48keepalive: cannot watch network links\n");
		(void)close(nlfd);
		nlfd = RE_BAD_SOCK;
		return;
	}

	/* every link's state now, so the first change is seen as one */
	memset(&dump, 0, sizeof dump);
	dump.h.nlmsg_len = sizeof dump;
	dump.h.nlmsg_type = RTM_GETLINK;
	dump.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	dump.g.rtgen_family = AF_UNSPEC;
	(void)send(nlfd, &dump, sizeof dump, 0);
}


static void netlink_close(void)
{
	nlfhs = fd_close(nlfhs);
	if (nlfd != RE_BAD_SOCK)
		(void)close(nlfd);
	nlfd = RE_BAD_SOCK;
	nlinks = 0;
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct ua *ua = bevent_get_ua(event);
	struct ka *ka;
	(void)arg;

	switch (ev) {
	case BEVENT_REGISTER_OK:
		ka = ka_find(ua);
		if (ka)
			registered(ka);
		else if (ua)
			start(ua);
		break;
	case BEVENT_REGISTER_FAIL:
		/* kept: keepalives wait for the next REGISTER_OK */
		ka = ka_find(ua);
		if (ka) {
			ka->registered = false;
			tmr_cancel(&ka->tmr);
		}
		break;
	case BEVENT_UNREGISTERING:
		mem_deref(ka_find(ua));
		break;
	case BEVENT_SHUTDOWN:
		list_flush(&kal);
		break;
	default:
		break;
	}
}


static int module_init(void)
{
	list_init(&kal);
	netlink_open();
	return bevent_register(event_handler, NULL);
}


static int module_close(void)
{
	bevent_unregister(event_handler);
	netlink_close();
	list_flush(&kal);
	return 0;
}


const struct mod_export DECL_EXPORTS(t48keepalive) = {
	"t48keepalive",
	"application",
	module_init,
	module_close
};
