// SPDX-License-Identifier: AGPL-3.0-or-later
/* Short words for long things (shell/names.c): the reasons a failed call is shown with, and the
 * names of accounts with no Account name. Run by make check. */
#include <stdio.h>
#include <string.h>
#include "../names.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reason(const char *why, const char *want, int want_busy, int want_sip, int want_cause)
{
    char b[64];
    int sip = -1, cause = -1, busy = call_fail_reason(why, b, sizeof b, &sip, &cause);
    CHECK(!strcmp(b, want), "\"%s\": \"%s\", wanted \"%s\"", why, b, want);
    CHECK(busy == want_busy, "\"%s\": busy %d, wanted %d", why, busy, want_busy);
    CHECK(sip == want_sip && cause == want_cause, "\"%s\": %d cause %d, wanted %d cause %d", why, sip, cause, want_sip, want_cause);
    CHECK(strlen(b) <= 26, "\"%s\" is too long for one line", b);
}

static void shorter(const char *host, const char *want)
{
    char b[64];
    host_short(host, b, sizeof b);
    CHECK(!strcmp(b, want), "%s: %s, wanted %s", host, b, want);
}

#define N 4
static void names(const char *u0, const char *h0, const char *l0, const char *u1, const char *h1, const char *l1,
                  const char *u2, const char *h2, const char *l2, const char *w0, const char *w1, const char *w2)
{
    const char *u[N] = {u0, u1, u2, ""}, *h[N] = {h0, h1, h2, ""}, *l[N] = {l0, l1, l2, ""}, *w[3] = {w0, w1, w2};
    char out[N][ACCT_NAME_MAX];
    acct_names(N, u, h, l, out);
    for (int i = 0; i < 3; i++) CHECK(!strcmp(out[i], w[i]), "account %d: \"%s\", wanted \"%s\"", i + 1, out[i], w[i]);
    CHECK(!out[3][0], "an account not set up has no name: \"%s\"", out[3]);
}

int main(void)
{
    /* a provider's whole Reason header: the Q.850 cause is the more telling */
    reason("488 Not Acceptable Here,Q.850;cause=88;text=\"INCOMPATIBLE_DESTINATION\"", "Call settings not accepted", 0, 488, 88);
    reason("488 Not Acceptable Here", "Call settings not accepted", 0, 488, 0);
    reason("606 Not Acceptable", "Call settings not accepted", 0, 606, 0);
    reason("486 Busy Here", "Busy", 1, 486, 0);
    reason("600 Busy Everywhere", "Busy", 1, 600, 0);
    reason("404 Not Found", "Number not found", 0, 404, 0);
    reason("484 Address Incomplete", "Wrong number", 0, 484, 0);
    reason("480 Temporarily Unavailable", "Not available", 1, 480, 0);
    reason("403 Forbidden", "Not allowed", 0, 403, 0);
    reason("408 Request Timeout", "No answer from network", 1, 408, 0);
    reason("503 Service Unavailable", "No answer from network", 1, 503, 0);
    reason("504 Server Time-out", "No answer from network", 1, 504, 0);
    reason("487 Request Terminated", "Cancelled", 1, 487, 0);
    reason("603 Decline", "Declined", 1, 603, 0);
    reason("404 Not Found,Q.850;cause=1;text=\"UNALLOCATED_NUMBER\"", "Number not found", 0, 404, 1);
    reason("480 Temporarily Unavailable,Q.850;cause=17", "Busy", 1, 480, 17);
    reason("480 Temporarily Unavailable,Q.850;cause=18", "No answer", 1, 480, 18);
    reason("480 Temporarily Unavailable,Q.850;cause=19;text=\"NO_ANSWER\"", "No answer", 1, 480, 19);
    reason("403 Forbidden,Q.850;cause=21", "Declined", 1, 403, 21);
    reason("503 Service Unavailable,Q.850;cause=34", "Network busy", 1, 503, 34);
    reason("503 Service Unavailable,Q.850;cause=38", "Network busy", 1, 503, 38);
    reason("503 Service Unavailable,Q.850;cause=41", "Network busy", 1, 503, 41);
    reason("503 Service Unavailable,Q.850;cause=42", "Network busy", 1, 503, 42);
    /* a cause with no name of its own: the status decides */
    reason("486 Busy Here,Q.850;cause=16", "Busy", 1, 486, 16);
    reason("500 Server Internal Error", "Call failed", 0, 500, 0);
    reason("Connection reset by peer", "Call failed", 0, 0, 0);
    reason("", "Call failed", 0, 0, 0);

    shorter("sip.example.net", "example.net");
    shorter("edge4-tls.prod.sip.example.net", "example.net");
    shorter("example.net", "example.net");
    shorter("pbx", "pbx");
    shorter("voip.example.co.uk", "example.co.uk");
    shorter("example.co.uk", "example.co.uk");
    shorter("a.b.example.org.uk", "example.org.uk");
    shorter("sip.example.com.au", "example.com.au");
    shorter("sip.example.net:5061", "example.net");
    shorter("sip.example.net.", "example.net");
    shorter("192.168.1.20", "192.168.1.20");
    shorter("192.168.1.20:5060", "192.168.1.20");
    shorter("fe80::1", "fe80::1");
    shorter("[2001:db8::1]:5061", "[2001:db8::1]:5061");
    shorter("", "");

    /* the Account name wins; no name: the domain; two the same: the start of each user */
    names("Xq7rT2mLp9", "edge2-tls.prod.example.net", "", "1001", "pbx.example.org", "", "", "", "",
          "example.net", "example.org", "");
    names("Xq7rT2mLp9", "sip.example.net", "", "Bv3kW8nQz1", "sip2.example.net", "", "", "", "",
          "example.net (Xq7r)", "example.net (Bv3k)", "");
    names("Xq7rT2mLp9", "sip.example.net", "Home", "Bv3kW8nQz1", "sip2.example.net", "", "", "", "",
          "Home", "example.net", "");
    names("Xq7rT2mLp9", "sip.example.net", "example.net", "Bv3kW8nQz1", "sip2.example.net", "", "1001", "pbx.example.org", "Work",
          "example.net", "example.net (Bv3k)", "Work");
    names("100", "10.0.0.5", "", "101", "10.0.0.5", "", "", "", "",
          "10.0.0.5 (100)", "10.0.0.5 (101)", "");

    printf("%s names test: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
