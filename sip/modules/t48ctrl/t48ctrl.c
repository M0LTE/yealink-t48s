/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * t48ctrl: call control and events for the shell (t48-shell, issue #26) on a
 * Unix socket only root can open. It speaks the protocol of baresip's own
 * ctrl_tcp module (JSON in netstrings, docs/sip.md), which listens on TCP
 * where any app could connect and place calls. This module replaces it.
 *
 *   in:   <len>:{"command":"dial","params":"1002","token":"7"},
 *   out:  <len>:{"response":true,"ok":true,"data":"...","token":"7"},
 *   out:  <len>:{"event":true,"class":"call","type":"CALL_INCOMING",...},
 *
 * Additions to ctrl_tcp:
 *  - Several clients at once (the shell, and a tool over SSH). Each gets
 *    every event.
 *  - A client that has just connected is told how things stand, as events
 *    marked "sync":true: one REGISTER_OK, REGISTER_FAIL or REGISTERING for
 *    each account, and one CALL_INCOMING, CALL_OUTGOING, CALL_RINGING or
 *    CALL_ESTABLISHED for each call. So a restarted shell picks a call back
 *    up. The command "sync" asks for the same again.
 *  - A command may name an account, "account":"sip:user@domain". It is then
 *    run for that account: "dial" calls out from it. Without one baresip
 *    picks, which with a single account is that account.
 *  - The command "joincalls <call-id> <call-id>" connects the two people we
 *    are talking to with each other and takes us out: an attended transfer.
 *    The first call is the one sent away (it must be on hold), the second
 *    the one it is sent to.
 *
 * Who may connect: the socket is made mode 0600 inside t48-sip's run
 * directory (0700), and each connection's peer is checked: only root, or the
 * user baresip itself runs as, is let in.
 *
 * Config:  t48ctrl_socket /run/t48-sip/ctrl.sock     (optional; that is the default)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <re.h>
#include <baresip.h>

#define DEFAULT_SOCKET "/run/t48-sip/ctrl.sock"

enum {
	MAX_MSG = 4096,       /* longest command we accept */
	MAX_CLIENTS = 4,
};

struct client {
	struct le le;
	int fd;
	struct re_fhs *fhs;
	char rx[MAX_MSG + 16];
	size_t rxn;
};

static struct {
	int fd;
	struct re_fhs *fhs;
	struct list clients;
	char path[108];
} ctl = { .fd = -1 };


static void client_destructor(void *arg)
{
	struct client *c = arg;

	list_unlink(&c->le);
	c->fhs = fd_close(c->fhs);
	if (c->fd >= 0)
		close(c->fd);
}


static int print_handler(const char *p, size_t size, void *arg)
{
	return mbuf_write_mem(arg, (const uint8_t *)p, size);
}


/* Send one JSON object as a netstring. A client that does not read is
 * dropped rather than waited for: returns false if it has gone. */
static bool client_send(struct client *c, const struct odict *od)
{
	struct mbuf *mb = mbuf_alloc(1024);
	struct re_printf pf = {print_handler, mb};
	char head[16];
	struct iovec iov[3];
	struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 3};
	ssize_t n = -1;

	if (!mb)
		return true;
	if (json_encode_odict(&pf, od)) {
		warning("t48ctrl: cannot encode JSON\n");
		mem_deref(mb);
		return true;
	}

	iov[0].iov_base = head;
	iov[0].iov_len = (size_t)re_snprintf(head, sizeof(head), "%zu:",
					     mb->end);
	iov[1].iov_base = mb->buf;
	iov[1].iov_len = mb->end;
	iov[2].iov_base = (void *)",";
	iov[2].iov_len = 1;
	n = sendmsg(c->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
	if (n != (ssize_t)(iov[0].iov_len + mb->end + 1)) {
		info("t48ctrl: client dropped (%s)\n",
		     n < 0 ? strerror(errno) : "not reading");
		mem_deref(mb);
		mem_deref(c);
		return false;
	}
	mem_deref(mb);
	return true;
}


/* ---------- how things stand, for a client that has just arrived ---------- */

static bool sync_one(struct client *c, struct ua *ua, struct call *call,
		     const char *type)
{
	struct odict *od = NULL;
	bool alive;
	int err;

	if (odict_alloc(&od, 8))
		return true;
	err  = odict_entry_add(od, "event", ODICT_BOOL, true);
	err |= odict_entry_add(od, "sync", ODICT_BOOL, true);
	err |= odict_entry_add(od, "class", ODICT_STRING,
			       call ? "call" : "register");
	err |= odict_entry_add(od, "type", ODICT_STRING, type);
	err |= odict_entry_add(od, "accountaor", ODICT_STRING,
			       account_aor(ua_account(ua)));
	if (call) {
		const struct sdp_media *m =
			stream_sdpmedia(audio_strm(call_audio(call)));

		err |= odict_entry_add(od, "direction", ODICT_STRING,
				       call_is_outgoing(call) ?
				       "outgoing" : "incoming");
		if (call_peeruri(call))
			err |= odict_entry_add(od, "peeruri", ODICT_STRING,
					       call_peeruri(call));
		if (call_peername(call))
			err |= odict_entry_add(od, "peerdisplayname",
					       ODICT_STRING,
					       call_peername(call));
		if (call_id(call))
			err |= odict_entry_add(od, "id", ODICT_STRING,
					       call_id(call));
		err |= odict_entry_add(od, "remoteaudiodir", ODICT_STRING,
				       sdp_dir_name(sdp_media_rdir(m)));
		err |= odict_entry_add(od, "onhold", ODICT_BOOL,
				       call_is_onhold(call));
		err |= odict_entry_add(od, "duration", ODICT_INT,
				       (int64_t)call_duration(call));
	}
	alive = err ? true : client_send(c, od);
	mem_deref(od);
	return alive;
}


static void sync_client(struct client *c)
{
	struct le *le, *lec;

	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = le->data;
		const char *type = ua_isregistered(ua) ? "REGISTER_OK" :
			ua_regfailed(ua) ? "REGISTER_FAIL" : "REGISTERING";

		if (!sync_one(c, ua, NULL, type))
			return;
		for (lec = list_head(ua_calls(ua)); lec; lec = lec->next) {
			struct call *call = lec->data;

			switch (call_state(call)) {
			case CALL_STATE_INCOMING: type = "CALL_INCOMING"; break;
			case CALL_STATE_OUTGOING: type = "CALL_OUTGOING"; break;
			case CALL_STATE_RINGING:
			case CALL_STATE_EARLY: type = "CALL_RINGING"; break;
			case CALL_STATE_ESTABLISHED:
			case CALL_STATE_TRANSFER:
				type = "CALL_ESTABLISHED"; break;
			default: continue;
			}
			if (!sync_one(c, ua, call, type))
				return;
		}
	}
}


/* ---------- commands ---------- */

/* returns false if the client has gone */
static bool handle_command(struct client *c, const char *json, size_t len)
{
	struct mbuf *resp = mbuf_alloc(1024);
	struct re_printf pf = {print_handler, resp};
	struct odict *od = NULL, *out = NULL;
	const char *cmd, *prm, *tok, *acc;
	struct ua *ua = NULL;
	char line[1024], m[128];
	char *data = NULL;
	bool alive = true;
	int err, cerr;

	if (!resp)
		return true;
	err = json_decode_odict(&od, 32, json, len, 16);
	if (err) {
		warning("t48ctrl: not JSON (%m)\n", err);
		goto out;
	}
	cmd = odict_string(od, "command");
	prm = odict_string(od, "params");
	tok = odict_string(od, "token");
	acc = odict_string(od, "account");
	if (!cmd) {
		warning("t48ctrl: no command in message\n");
		goto out;
	}

	if (!strcmp(cmd, "sync")) {
		cerr = 0;
	}
	else if (acc && !(ua = uag_find_aor(acc))) {
		mbuf_printf(resp, "no account %s", acc);
		cerr = ENOENT;
	}
	else {
		re_snprintf(line, sizeof(line), "%s%s%s", cmd,
			    prm ? " " : "", prm ? prm : "");
		cerr = cmd_process_long(baresip_commands(), line,
					str_len(line), &pf, ua);
	}

	resp->pos = 0;
	if (resp->end)
		err = mbuf_strdup(resp, &data, resp->end);
	if (err || odict_alloc(&out, 8))
		goto out;
	err  = odict_entry_add(out, "response", ODICT_BOOL, true);
	err |= odict_entry_add(out, "ok", ODICT_BOOL, (bool)!cerr);
	err |= odict_entry_add(out, "data", ODICT_STRING,
			       data ? data :
			       cerr ? str_error(cerr, m, sizeof(m)) : "");
	if (tok)
		err |= odict_entry_add(out, "token", ODICT_STRING, tok);
	if (!err)
		alive = client_send(c, out);
	if (alive && !strcmp(cmd, "sync"))
		sync_client(c);

 out:
	mem_deref(data);
	mem_deref(resp);
	mem_deref(od);
	mem_deref(out);
	return alive;
}


/* joincalls <call sent away> <call it is sent to>: attended transfer */
static int cmd_joincalls(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct pl a, b;
	char ida[200], idb[200];
	struct call *away, *to;
	int err;

	if (re_regex(carg->prm, str_len(carg->prm), "[^ ]+ [^ ]+", &a, &b)) {
		re_hprintf(pf, "usage: joincalls <call-id> <call-id>");
		return EINVAL;
	}
	pl_strcpy(&a, ida, sizeof(ida));
	pl_strcpy(&b, idb, sizeof(idb));
	away = uag_call_find(ida);
	to = uag_call_find(idb);
	if (!away || !to || away == to) {
		re_hprintf(pf, "no such calls");
		return ENOENT;
	}
	err = call_hold(to, true);
	if (!err)
		err = call_replace_transfer(away, to);
	return err;
}


static const struct cmd cmdv[] = {
	{"joincalls", 0, CMD_PRM, "Attended transfer <call-id> <call-id>",
	 cmd_joincalls},
};


static void client_read(int flags, void *arg)
{
	struct client *c = arg;
	ssize_t n;
	(void)flags;

	n = recv(c->fd, c->rx + c->rxn, sizeof(c->rx) - 1 - c->rxn,
		 MSG_DONTWAIT);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		mem_deref(c);
		return;
	}
	c->rxn += (size_t)n;

	/* netstrings: <decimal length>:<bytes>, */
	for (;;) {
		size_t i = 0, len = 0;

		while (i < c->rxn && c->rx[i] >= '0' && c->rx[i] <= '9' &&
		       i < 6)
			len = len * 10 + (size_t)(c->rx[i++] - '0');
		if (i == c->rxn)
			break;                   /* length not complete */
		if (i == 0 || c->rx[i] != ':' || len > MAX_MSG) {
			warning("t48ctrl: bad framing, client dropped\n");
			mem_deref(c);
			return;
		}
		if (c->rxn < i + 1 + len + 1)
			break;                   /* message not complete */
		if (c->rx[i + 1 + len] != ',') {
			warning("t48ctrl: bad framing, client dropped\n");
			mem_deref(c);
			return;
		}
		c->rx[i + 1 + len] = '\0';
		if (!handle_command(c, c->rx + i + 1, len))
			return;
		memmove(c->rx, c->rx + i + len + 2, c->rxn - (i + len + 2));
		c->rxn -= i + len + 2;
	}
}


static void accept_handler(int flags, void *arg)
{
	struct client *c;
	struct ucred cred;
	socklen_t cl = sizeof(cred);
	int fd;
	(void)flags;
	(void)arg;

	fd = accept4(ctl.fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (fd < 0)
		return;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) < 0 ||
	    (cred.uid != 0 && cred.uid != geteuid())) {
		warning("t48ctrl: refused a connection from user %d\n",
			(int)cred.uid);
		close(fd);
		return;
	}
	if (list_count(&ctl.clients) >= MAX_CLIENTS) {
		warning("t48ctrl: too many clients\n");
		close(fd);
		return;
	}
	c = mem_zalloc(sizeof(*c), client_destructor);
	if (!c) {
		close(fd);
		return;
	}
	c->fd = fd;
	list_append(&ctl.clients, &c->le, c);
	if (fd_listen(&c->fhs, fd, FD_READ, client_read, c)) {
		mem_deref(c);
		return;
	}
	sync_client(c);
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct odict *od = NULL;
	struct le *le;
	(void)arg;

	/* level meters and RTCP reports are no use to the shell */
	if (ev == BEVENT_VU_TX || ev == BEVENT_VU_RX || ev == BEVENT_CALL_RTCP)
		return;
	if (!list_head(&ctl.clients) || odict_alloc(&od, 8))
		return;
	if (odict_entry_add(od, "event", ODICT_BOOL, true) ||
	    bevent_odict_encode(od, event)) {
		warning("t48ctrl: cannot encode an event\n");
		mem_deref(od);
		return;
	}
	le = list_head(&ctl.clients);
	while (le) {
		struct client *c = le->data;

		le = le->next;
		client_send(c, od);
	}
	mem_deref(od);
}


static int module_init(void)
{
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	mode_t old;
	int err;

	if (conf_get_str(conf_cur(), "t48ctrl_socket", ctl.path,
			 sizeof(ctl.path)))
		str_ncpy(ctl.path, DEFAULT_SOCKET, sizeof(ctl.path));
	str_ncpy(sa.sun_path, ctl.path, sizeof(sa.sun_path));

	ctl.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
			0);
	if (ctl.fd < 0)
		return errno;
	unlink(ctl.path);
	old = umask(0177);       /* never open to others, even for a moment */
	err = bind(ctl.fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ? errno : 0;
	umask(old);
	if (!err && listen(ctl.fd, 4) < 0)
		err = errno;
	if (!err)
		err = fd_listen(&ctl.fhs, ctl.fd, FD_READ, accept_handler,
				NULL);
	if (!err)
		err = bevent_register(event_handler, NULL);
	if (!err)
		err = cmd_register(baresip_commands(), cmdv,
				   RE_ARRAY_SIZE(cmdv));
	if (err) {
		warning("t48ctrl: %s: %m\n", ctl.path, err);
		ctl.fhs = fd_close(ctl.fhs);
		close(ctl.fd);
		ctl.fd = -1;
		return err;
	}
	info("t48ctrl: listening on %s\n", ctl.path);
	return 0;
}


static int module_close(void)
{
	bevent_unregister(event_handler);
	cmd_unregister(baresip_commands(), cmdv);
	list_flush(&ctl.clients);
	ctl.fhs = fd_close(ctl.fhs);
	if (ctl.fd >= 0) {
		close(ctl.fd);
		unlink(ctl.path);
	}
	ctl.fd = -1;
	return 0;
}


const struct mod_export DECL_EXPORTS(t48ctrl) = {
	"t48ctrl",
	"application",
	module_init,
	module_close
};
