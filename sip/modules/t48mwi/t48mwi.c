/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48mwi: message waiting (voicemail) for the T48S message lamp, issue #27.
 * For each account that registers it subscribes to the server's
 * message-summary event (RFC 3842) and passes every NOTIFY on as baresip's
 * own MWI_NOTIFY event, body and all, which t48ctrl hands to the shell.
 *
 * It does the job of baresip's mwi module, which we do not build: that one
 * can spin for ever at start (its start-up timer walks its list of
 * subscriptions while adding to it, which bites when an account registers
 * before the timer runs, as it does against a server on the same machine),
 * and it holds baresip open for half a second at shutdown, long enough for
 * a restart to find the SIP port still taken.
 *
 * An account opts out with ;mwi=no (t48-sip writes that for mwi=off). A
 * server that does not do message waiting just refuses; nothing is retried
 * until the account registers again.
 */
#include <string.h>
#include <re.h>
#include <baresip.h>

struct sub {
	struct le le;
	struct sipsub *sub;
	struct ua *ua;            /* not a reference: dropped at UNREGISTERING and SHUTDOWN */
};

static struct list subl;


static void sub_destructor(void *arg)
{
	struct sub *s = arg;

	list_unlink(&s->le);
	mem_deref(s->sub);
}


static struct sub *sub_find(const struct ua *ua)
{
	struct le *le;

	for (le = subl.head; le; le = le->next) {
		struct sub *s = le->data;

		if (s->ua == ua)
			return s;
	}

	return NULL;
}


static int auth_handler(char **username, char **password,
			const char *realm, void *arg)
{
	return account_auth(arg, username, password, realm);
}


static void notify_handler(struct sip *sip, const struct sip_msg *msg,
			   void *arg)
{
	struct sub *s = arg;

	if (mbuf_get_left(msg->mb))
		bevent_ua_emit(BEVENT_MWI_NOTIFY, s->ua, "%b",
			       mbuf_buf(msg->mb), mbuf_get_left(msg->mb));

	(void)sip_treply(NULL, sip, msg, 200, "OK");
}


static void close_handler(int err, const struct sip_msg *msg,
			  const struct sipevent_substate *substate, void *arg)
{
	struct sub *s = arg;
	(void)substate;

	info("t48mwi: %s: no message waiting from the server (%m%u %r)\n",
	     account_aor(ua_account(s->ua)), err, err ? 0 : msg->scode,
	     err ? NULL : &msg->reason);
	mem_deref(s);
}


static void subscribe(struct ua *ua)
{
	const char *aor = account_aor(ua_account(ua));
	const char *routev[1];
	struct sub *s;
	int err;

	s = mem_zalloc(sizeof(*s), sub_destructor);
	if (!s)
		return;
	s->ua = ua;
	list_append(&subl, &s->le, s);
	routev[0] = ua_outbound(ua);

	err = sipevent_subscribe(&s->sub, uag_sipevent_sock(), aor, NULL, aor,
				 "message-summary", NULL, 600, ua_cuser(ua),
				 routev, routev[0] ? 1 : 0,
				 auth_handler, ua_account(ua), true, NULL,
				 notify_handler, close_handler, s,
				 "Accept: application/simple-message-summary"
				 "\r\n");
	if (err) {
		warning("t48mwi: cannot subscribe for %s: %m\n", aor, err);
		mem_deref(s);
	}
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct ua *ua = bevent_get_ua(event);
	struct sub *s;
	(void)arg;

	switch (ev) {
	case BEVENT_REGISTER_OK:
		if (ua && !sub_find(ua) && account_mwi(ua_account(ua)))
			subscribe(ua);
		break;
	case BEVENT_UNREGISTERING:
		s = sub_find(ua);
		if (s)
			mem_deref(s);
		break;
	case BEVENT_SHUTDOWN:
		list_flush(&subl);
		break;
	default:
		break;
	}
}


static int module_init(void)
{
	list_init(&subl);
	return bevent_register(event_handler, NULL);
}


static int module_close(void)
{
	bevent_unregister(event_handler);
	list_flush(&subl);
	return 0;
}


const struct mod_export DECL_EXPORTS(t48mwi) = {
	"t48mwi",
	"application",
	module_init,
	module_close
};
