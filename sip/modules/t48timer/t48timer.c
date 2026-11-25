/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48timer: SIP session timers (RFC 4028), issue #77, docs/sip.md. baresip
 * has none. A session timer has one side of a call (the refresher) repeat
 * the call's request now and then, so that proxies and B2BUAs keeping state
 * for it know it is still up; a call that is not refreshed in time is ended.
 *
 * An account with ;timer=N (seconds, 90 or more; t48-sip writes it):
 *  - says Supported: timer, so a caller that requires timers is answered
 *    rather than refused with 420;
 *  - asks for Session-Expires: N on the calls it makes;
 *  - on a call it is offered with Session-Expires, answers with the same
 *    interval and a refresher: the one the caller named, else the caller if
 *    it supports timers, else us;
 *  - when it is the refresher, sends UPDATE with Session-Expires every half
 *    interval, or a re-INVITE if the other side does not allow UPDATE or
 *    refuses one. A refresh that times out or gets 408 or 481 ends the call,
 *    as RFC 4028 says: the other side has lost it.
 * When the other side is the refresher nothing is needed here: baresip
 * answers its re-INVITEs and UPDATEs. Without ;timer the account behaves
 * as plain baresip: it does not offer timers, the far end refreshes if it
 * wants to (RFC 4028 makes it), and a caller that requires them gets 420.
 *
 * Needs the patch sip/patches/baresip-0001-session-timer-hooks.patch, which
 * gives a call's setup message and SIP session, and puts a call's custom
 * headers on the 2xx that answers it.
 */
#include <string.h>
#include <re.h>
#include <baresip.h>

enum { MIN_SE = 90 };

struct st {
	struct le le;
	struct tmr tmr;
	struct sip_request *req;
	struct call *call;       /* not a reference: dropped at CALL_CLOSED */
	uint32_t se;             /* interval, seconds */
	bool refresher;          /* we refresh */
	bool update;             /* the other side allows UPDATE */
};

static struct list stl;


static void st_destructor(void *arg)
{
	struct st *st = arg;

	tmr_cancel(&st->tmr);
	mem_deref(st->req);
	list_unlink(&st->le);
}


static struct st *st_find(const struct call *call)
{
	struct le *le;

	for (le = list_head(&stl); le; le = le->next) {
		struct st *st = le->data;
		if (st->call == call)
			return st;
	}
	return NULL;
}


/* the account's interval, 0 when it has no session timer */
static uint32_t acc_timer(const struct account *acc)
{
	struct sip_addr *laddr = account_laddr(acc);
	struct pl v;
	uint32_t n;

	if (!laddr || msg_param_decode(&laddr->params, "timer", &v))
		return 0;
	n = pl_u32(&v);
	return n && n < MIN_SE ? MIN_SE : n;
}


/* Session-Expires of a message: the interval, and the refresher in r
 * ('c' for uac, 's' for uas, 0 if not named). 0 if there is none. */
static uint32_t msg_se(const struct sip_msg *msg, char *r)
{
	const struct sip_hdr *h = sip_msg_hdr(msg, SIP_HDR_SESSION_EXPIRES);
	struct pl n, v;

	*r = 0;
	if (!h || re_regex(h->val.p, h->val.l, "[0-9]+", &n))
		return 0;
	if (!msg_param_decode(&h->val, "refresher", &v))
		*r = !pl_strcasecmp(&v, "uac") ? 'c' :
			!pl_strcasecmp(&v, "uas") ? 's' : 0;
	return pl_u32(&n);
}


static bool allows_update(const struct sip_msg *msg)
{
	return sip_msg_hdr_has_value(msg, SIP_HDR_ALLOW, "UPDATE");
}


static void refresh(void *arg);


static void schedule(struct st *st)
{
	tmr_start(&st->tmr, st->se * 1000 / 2, refresh, st);
}


static void reinvite(struct st *st)
{
	int err = call_modify(st->call);

	if (err)
		warning("t48timer: re-INVITE failed (%m)\n", err);
	schedule(st);
}


static int send_handler(enum sip_transp tp, struct sa *src,
			const struct sa *dst, struct mbuf *mb,
			struct mbuf **contp, void *arg)
{
	struct st *st = arg;
	struct sip_contact contact;
	(void)dst;
	(void)contp;

	sip_contact_set(&contact, ua_cuser(call_get_ua(st->call)), src, tp);
	return mbuf_printf(mb, "%H", sip_contact_print, &contact);
}


static void resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct st *st = arg;
	uint32_t se;
	char r;

	st->req = mem_deref(st->req);
	if (err || msg->scode == 408 || msg->scode == 481) {
		info("t48timer: refresh failed (%m %u), call ended\n", err,
		     msg ? msg->scode : 0);
		ua_hangup(call_get_ua(st->call), st->call, 0, NULL);
		return;
	}
	if (msg->scode < 200)
		return;
	if (msg->scode >= 300) {
		/* no UPDATE here after all, or a clash: a re-INVITE instead */
		info("t48timer: UPDATE refused (%u), re-INVITE instead\n",
		     msg->scode);
		st->update = false;
		reinvite(st);
		return;
	}
	se = msg_se(msg, &r);
	if (se)
		st->se = se;
	if (r == 's') {
		info("t48timer: the other side refreshes from now\n");
		return;
	}
	/* no Session-Expires in the answer ends the timer by RFC 4028, but
	 * refreshing anyway costs one request and keeps a confused side happy */
	schedule(st);
}


static void refresh(void *arg)
{
	struct st *st = arg;
	struct sipsess *sess = call_sipsess(st->call);
	int err;

	if (!st->update || !sess) {
		reinvite(st);
		return;
	}
	err = sip_drequestf(&st->req, uag_sip(), true, "UPDATE",
			    sipsess_dialog(sess), 0, NULL,
			    send_handler, resp_handler, st,
			    "Session-Expires: %u;refresher=uac\r\n"
			    "Supported: timer\r\n"
			    "Content-Length: 0\r\n\r\n", st->se);
	if (err) {
		warning("t48timer: UPDATE failed (%m)\n", err);
		reinvite(st);
	}
}


static struct st *st_new(struct call *call, uint32_t se, bool refresher,
			 bool update)
{
	struct st *st = st_find(call);

	if (!st) {
		st = mem_zalloc(sizeof(*st), st_destructor);
		if (!st)
			return NULL;
		st->call = call;
		list_append(&stl, &st->le, st);
	}
	st->se = se;
	st->refresher = refresher;
	st->update = update;
	return st;
}


/* an incoming call offered with Session-Expires: answer with it */
static void incoming(struct call *call)
{
	const struct sip_msg *msg = call_setup_msg(call);
	struct list hdrs;
	bool supports;
	uint32_t se;
	char r;

	if (!msg || !acc_timer(call_account(call)))
		return;
	list_init(&hdrs);
	se = msg_se(msg, &r);
	if (!se) {
		/* baresip gave the call the account's own Session-Expires,
		 * which is for the calls it makes */
		call_set_custom_hdrs(call, &hdrs);
		return;
	}
	supports = sip_msg_hdr_has_value(msg, SIP_HDR_SUPPORTED, "timer") ||
		sip_msg_hdr_has_value(msg, SIP_HDR_REQUIRE, "timer");
	if (!r)
		r = supports ? 'c' : 's';

	if (custom_hdrs_add(&hdrs, "Session-Expires", "%u;refresher=%s", se,
			    r == 'c' ? "uac" : "uas") ||
	    (supports && custom_hdrs_add(&hdrs, "Require", "timer"))) {
		list_flush(&hdrs);
		return;
	}
	call_set_custom_hdrs(call, &hdrs);
	list_flush(&hdrs);
	st_new(call, se, r == 's', allows_update(msg));
}


static void established(struct call *call)
{
	const struct sip_msg *msg = call_setup_msg(call);
	struct st *st;
	uint32_t se;
	char r;

	if (!msg || !acc_timer(call_account(call)))
		return;
	if (call_is_outgoing(call)) {
		/* the 2xx says who refreshes; without a refresher, we do */
		se = msg_se(msg, &r);
		if (!se)
			return;
		st = st_new(call, se, r != 's', allows_update(msg));
	}
	else {
		st = st_find(call);
	}
	if (!st || !st->refresher)
		return;
	info("t48timer: %s: refreshing every %u s\n", call_id(call),
	     st->se / 2);
	schedule(st);
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct call *call = bevent_get_call(event);
	(void)arg;

	if (!call)
		return;
	switch (ev) {
	case BEVENT_CALL_INCOMING:
		incoming(call);
		break;
	case BEVENT_CALL_ESTABLISHED:
		established(call);
		break;
	case BEVENT_CALL_CLOSED:
		mem_deref(st_find(call));
		break;
	default:
		break;
	}
}


/* every account with a timer offers it, and asks for it on its calls */
static bool ua_setup(struct le *le, void *arg)
{
	struct ua *ua = le->data;
	uint32_t se = acc_timer(ua_account(ua));
	char v[16];
	struct pl name = PL("Session-Expires"), val;
	(void)arg;

	if (!se)
		return false;
	ua_add_extension(ua, "timer");
	re_snprintf(v, sizeof(v), "%u", se);
	pl_set_str(&val, v);
	if (ua_add_custom_hdr(ua, &name, &val))
		warning("t48timer: %s: no Session-Expires\n",
			account_aor(ua_account(ua)));
	else
		info("t48timer: %s: session timer %u s\n",
		     account_aor(ua_account(ua)), se);
	return false;
}


static int module_init(void)
{
	list_init(&stl);
	(void)list_apply(uag_list(), true, ua_setup, NULL);
	return bevent_register(event_handler, NULL);
}


static int module_close(void)
{
	bevent_unregister(event_handler);
	list_flush(&stl);
	return 0;
}


const struct mod_export DECL_EXPORTS(t48timer) = {
	"t48timer",
	"application",
	module_init,
	module_close
};
