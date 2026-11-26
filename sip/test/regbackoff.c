// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Registration back-off (issue #77, docs/sip.md), on libre itself with a
// fake clock: nothing waits, nothing goes on the network. Linked with
//   -Wl,--wrap=clock_gettime  libre's timers read the fake clock
//   -Wl,--wrap=sendto         a REGISTER over UDP is counted, not sent
//   -Wl,--wrap=rand_u16       the jitter is fixed: lowest or highest
// so every time below is exact.
//
//   1. a server that never answers (UDP): each REGISTER is retransmitted at
//      0.5, 1, 2, 4, 4 ... s (RFC 3261) and given up at 32 s; the next is
//      30 to 60 s later, doubling each time to 15 to 30 min, and stays there
//   2. a REGISTER that cannot be sent (no transport): the same back-off,
//      where before (no sip/patches/re-0002) it was never tried again
//   3. a new registration (a network change, the account saved) goes at once;
//      after failures baresip carries one failure over (sipreg_incfailc), so
//      if it fails too the wait is the second one, 60 to 120 s
//   4. a refresh (sipreg_refresh, a cable put back): nothing more while a
//      REGISTER is on its way; at once while backing off, which goes on
//   5. unregistering (expires 0) that cannot be sent still returns the error
//      and is not retried
//
// Build and run: sip/test/regbackoff-test.sh
#define _GNU_SOURCE
#include <re.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static uint64_t now_ms = 1000000;    /* the fake clock */
static uint16_t rand_value;          /* 0: the lowest jitter, 500: the highest */
static unsigned sends, fails;
static uint64_t send_at[4096], fail_at[64];
static int failures;

int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
	(void)id;
	ts->tv_sec = now_ms / 1000;
	ts->tv_nsec = (now_ms % 1000) * 1000000;
	return 0;
}

ssize_t __real_sendto(int, const void *, size_t, int,
		      const struct sockaddr *, socklen_t);

ssize_t __wrap_sendto(int fd, const void *buf, size_t len, int flags,
		      const struct sockaddr *to, socklen_t tolen)
{
	(void)fd; (void)buf; (void)flags; (void)to; (void)tolen;
	if (len > 8 && !memcmp(buf, "REGISTER", 8) &&
	    sends < sizeof send_at / sizeof send_at[0])
		send_at[sends++] = now_ms;
	return (ssize_t)len;
}

uint16_t __real_rand_u16(void);

uint16_t __wrap_rand_u16(void)
{
	return rand_value;
}

static void resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	(void)msg; (void)arg;
	if (err && fails < sizeof fail_at / sizeof fail_at[0])
		fail_at[fails++] = now_ms;
}

/* move the fake clock to the next timer and run it */
static void step(void)
{
	struct tmrl *tl = re_tmrl_get();
	uint64_t dt = tmr_next_timeout(tl);

	if (!dt && !tmrl_count(tl)) {
		printf("FAIL no timer left\n");
		exit(1);
	}
	now_ms += dt;
	tmr_poll(tl);
}

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

/* libre's wait after the nth failure in a row, for the fixed jitter */
static uint64_t wait_ms(unsigned n)
{
	uint64_t base = 30u << (n < 6 ? n : 6);
	return (base < 1800 ? base : 1800) * (500 + rand_value % 501);
}

static struct sipreg *reg_new(struct sip *sip, uint32_t expires)
{
	struct sipreg *reg = NULL;
	int err = sipreg_alloc(&reg, sip, "sip:127.0.0.1:5199;transport=udp",
			       "sip:t@127.0.0.1", NULL, "sip:t@127.0.0.1",
			       expires, "t", NULL, 0, 0, NULL, NULL, false,
			       resp_handler, NULL, NULL, NULL);
	if (err) {
		printf("FAIL sipreg_alloc: %d\n", err);
		exit(1);
	}
	return reg;
}

static void reset(void)
{
	sends = fails = 0;
}

static void silent_server(uint16_t r)
{
	static const uint64_t retx[] = {0, 500, 1500, 3500, 7500, 11500,
					15500, 19500, 23500, 27500, 31500};
	struct sip *sip = NULL;
	struct sipreg *reg;
	struct sa laddr;
	char what[160];
	uint64_t t0, start;
	unsigned n, i, first;
	int ok;

	rand_value = r;
	reset();
	if (sip_alloc(&sip, NULL, 32, 32, 32, "test", NULL, NULL) ||
	    sa_set_str(&laddr, "127.0.0.1", 0) ||
	    sip_transp_add(sip, SIP_TRANSP_UDP, &laddr)) {
		printf("FAIL sip stack\n");
		exit(1);
	}

	reg = reg_new(sip, 600);
	t0 = now_ms;
	check(sipreg_send(reg) == 0, "REGISTER goes out");
	while (fails < 1)
		step();

	ok = sends == 11;
	for (i = 0; ok && i < 11; i++)
		ok = send_at[i] - t0 == retx[i];
	check(ok && fail_at[0] - t0 == 32000,
	      "one REGISTER: 11 sends at 0, 0.5, 1.5, 3.5 then every 4 s, given up at 32 s");

	/* nine more failures: past the cap */
	for (n = 1; n < 10; n++) {
		first = sends;
		while (fails < n + 1)
			step();
		start = send_at[first];
		snprintf(what, sizeof what,
			 "failure %u: next REGISTER %llu s later (jitter %s)",
			 n, (unsigned long long)wait_ms(n) / 1000,
			 r ? "highest" : "lowest");
		check(start - fail_at[n - 1] == wait_ms(n) &&
		      sends - first == 11 && fail_at[n] - start == 32000, what);
	}
	check(wait_ms(9) == (r ? 1800000 : 900000),
	      r ? "longest wait 30 min" : "shortest wait at the cap 15 min");

	/* 3. what a network change or a saved account does: baresip's
	 *    reg_register makes a new registration, carries one failure over
	 *    from the old, and sends it straight away */
	mem_deref(reg);
	first = sends;
	n = fails;
	reg = reg_new(sip, 600);
	sipreg_incfailc(reg);
	check(sipreg_send(reg) == 0 && sends == first + 1 &&
	      send_at[first] == now_ms, "new registration sent at once");
	while (fails < n + 1)
		step();
	first = sends;
	while (sends == first)
		step();
	check(send_at[first] - fail_at[n] == wait_ms(2),
	      "after it fails too: the second wait, one failure carried over");

	/* 4. the cable put back (t48keepalive, sipreg_refresh): with a REGISTER
	 *    on its way, nothing more; while backing off, one at once on the
	 *    same registration, and the back-off goes on growing */
	first = sends;
	check(sipreg_refresh(reg) == 0 && sends == first,
	      "refresh with a REGISTER on its way: nothing more sent");
	n = fails;
	while (fails < n + 1)
		step();
	first = sends;
	check(sipreg_refresh(reg) == 0 && sends == first + 1 &&
	      send_at[first] == now_ms, "refresh while backing off: sent at once");
	while (fails < n + 2)
		step();
	first = sends;
	while (sends == first)
		step();
	check(send_at[first] - fail_at[n + 1] == wait_ms(4),
	      "and if it fails, the back-off goes on (the fourth wait)");

	mem_deref(reg);
	sip_close(sip, true);
	mem_deref(sip);
}

static void cannot_send(void)
{
	struct sip *sip = NULL;
	struct sipreg *reg;
	char what[120];
	unsigned n;
	int err;

	rand_value = 0;
	reset();
	if (sip_alloc(&sip, NULL, 32, 32, 32, "test", NULL, NULL)) {
		printf("FAIL sip stack\n");
		exit(1);
	}
	/* no transport at all: nothing can be sent */
	reg = reg_new(sip, 600);
	err = sipreg_send(reg);
	check(err == 0 && fails == 1 && sends == 0,
	      "REGISTER that cannot be sent: reported as a failure");
	check(tmrl_count(re_tmrl_get()) > 0, "and tried again later");
	if (tmrl_count(re_tmrl_get()) == 0)
		goto out;

	for (n = 1; n < 8; n++) {
		while (fails < n + 1)
			step();
		snprintf(what, sizeof what, "unsent %u: tried again after %llu s",
			 n, (unsigned long long)wait_ms(n) / 1000);
		check(fail_at[n] - fail_at[n - 1] == wait_ms(n), what);
	}
 out:
	mem_deref(reg);

	/* 5. unregistering cannot be sent either: the error comes back, once,
	 *    and nothing is left to try again */
	{
		uint32_t timers = tmrl_count(re_tmrl_get());
		struct sipreg *un = reg_new(sip, 0);

		n = fails;
		check(sipreg_send(un) != 0 && fails == n,
		      "expires 0 that cannot be sent: sipreg_send returns the error");
		sipreg_unregister(un);
		check(fails == n + 1 && tmrl_count(re_tmrl_get()) == timers,
		      "unregister that cannot be sent: reported once, not retried");
		mem_deref(un);
	}
	sip_close(sip, true);
	mem_deref(sip);
}

int main(void)
{
	if (libre_init()) {
		printf("FAIL libre_init\n");
		return 1;
	}

	silent_server(0);
	silent_server(500);
	cannot_send();

	libre_close();
	if (failures)
		printf("%d failed\n", failures);
	return failures ? 1 : 0;
}
