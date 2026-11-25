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
 * A registration that fails before anything is sent (REGISTER_FAIL straight
 * after REGISTERING, in the same turn of the main loop) leaves libre no
 * request to retry, and baresip does not retry it either. That is tried
 * again here every 30 to 60 s, for every account, until one goes out. A
 * REGISTER that was sent and failed is retried by libre itself. While an
 * account is not registered no OPTIONS are sent.
 *
 * An account opts out with ;keepalive=no (t48-sip writes that for
 * keepalive=off).
 */
#include <string.h>
#include <re.h>
#include <baresip.h>

enum {
	UDP_INTERVAL = 29,       /* seconds, RFC 5626 section 4.4.1 */
	STREAM_INTERVAL = 120,   /* TCP and TLS */
	RETRY_MS = 30000,        /* a registration that could not be sent */
};

/* every account: is a registration being started right now */
struct start {
	struct le le;
	struct tmr settle;        /* clears starting on the next loop turn */
	struct tmr retry;
	struct ua *ua;            /* not a reference, as for struct ka */
	bool starting;
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
static struct list startl;
static unsigned next_id;


static void start_destructor(void *arg)
{
	struct start *st = arg;

	tmr_cancel(&st->settle);
	tmr_cancel(&st->retry);
	list_unlink(&st->le);
}


static struct start *start_find(const struct ua *ua)
{
	struct le *le;

	for (le = startl.head; le; le = le->next) {
		struct start *st = le->data;

		if (st->ua == ua)
			return st;
	}

	return NULL;
}


static void settled(void *arg)
{
	struct start *st = arg;

	st->starting = false;
}


static void retry(void *arg)
{
	struct start *st = arg;

	(void)ua_register(st->ua);
}


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


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct ua *ua = bevent_get_ua(event);
	struct start *st;
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
	case BEVENT_REGISTERING:
		st = start_find(ua);
		if (!st && ua) {
			st = mem_zalloc(sizeof(*st), start_destructor);
			if (!st)
				break;
			st->ua = ua;
			list_append(&startl, &st->le, st);
		}
		if (!st)
			break;
		st->starting = true;
		tmr_cancel(&st->retry);
		tmr_start(&st->settle, 0, settled, st);
		break;
	case BEVENT_REGISTER_FAIL:
		/* kept: keepalives wait for the next REGISTER_OK */
		ka = ka_find(ua);
		if (ka) {
			ka->registered = false;
			tmr_cancel(&ka->tmr);
		}
		st = start_find(ua);
		if (st && st->starting) {
			info("t48keepalive: %s: registration not sent (%s),"
			     " trying again\n", account_aor(ua_account(ua)),
			     bevent_get_text(event));
			tmr_start(&st->retry, RETRY_MS + rand_u16() % RETRY_MS,
				  retry, st);
		}
		break;
	case BEVENT_UNREGISTERING:
		mem_deref(ka_find(ua));
		mem_deref(start_find(ua));
		break;
	case BEVENT_SHUTDOWN:
		list_flush(&kal);
		list_flush(&startl);
		break;
	default:
		break;
	}
}


static int module_init(void)
{
	list_init(&kal);
	list_init(&startl);
	return bevent_register(event_handler, NULL);
}


static int module_close(void)
{
	bevent_unregister(event_handler);
	list_flush(&kal);
	list_flush(&startl);
	return 0;
}


const struct mod_export DECL_EXPORTS(t48keepalive) = {
	"t48keepalive",
	"application",
	module_init,
	module_close
};
