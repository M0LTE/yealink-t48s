// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Short words for long things (names.h). A call that fails is shown as a
 * reason a person can act on, not the far end's status line: providers send
 * whole Reason headers that the screen would cut off. An account with no
 * Account name is shown as its provider's domain, not as user@host, as some
 * providers hand out random users and long edge hosts. See docs/phone.md.
 * Tested by shell/tests/test_names.c.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "names.h"

enum { BUSY = 1, FAULT = 0 };

/* Q.850 causes worth naming; the rest fall back to the SIP status */
static const struct { int cause; const char *text; int busy; } causes[] = {
    {1, "Number not found", FAULT},    /* unallocated number */
    {3, "Number not found", FAULT},    /* no route to destination */
    {17, "Busy", BUSY},
    {18, "No answer", BUSY},           /* no user responding */
    {19, "No answer", BUSY},           /* no answer from user */
    {20, "Not available", BUSY},       /* subscriber absent */
    {21, "Declined", BUSY},            /* call rejected */
    {22, "Number not found", FAULT},   /* number changed */
    {27, "Not available", BUSY},       /* destination out of order */
    {28, "Wrong number", FAULT},       /* invalid number format */
    {34, "Network busy", BUSY},        /* no circuit available */
    {38, "Network busy", BUSY},        /* network out of order */
    {41, "Network busy", BUSY},        /* temporary failure */
    {42, "Network busy", BUSY},        /* switching equipment congestion */
    {88, "Call settings not accepted", FAULT},   /* incompatible destination */
};

static const struct { int sip; const char *text; int busy; } statuses[] = {
    {401, "Not allowed", FAULT},
    {403, "Not allowed", FAULT},
    {404, "Number not found", FAULT},
    {407, "Not allowed", FAULT},
    {408, "No answer from network", BUSY},
    {410, "Number not found", FAULT},
    {415, "Call settings not accepted", FAULT},
    {480, "Not available", BUSY},
    {484, "Wrong number", FAULT},
    {486, "Busy", BUSY},
    {487, "Cancelled", BUSY},
    {488, "Call settings not accepted", FAULT},
    {503, "No answer from network", BUSY},
    {504, "No answer from network", BUSY},
    {600, "Busy", BUSY},
    {603, "Declined", BUSY},
    {604, "Number not found", FAULT},
    {606, "Call settings not accepted", FAULT},
};

int call_fail_reason(const char *why, char *out, size_t n, int *sip, int *cause)
{
    const char *q;
    *sip = why ? atoi(why) : 0;
    *cause = 0;
    if (why && (q = strstr(why, "Q.850")) && (q = strstr(q, "cause=")))
        *cause = atoi(q + 6);
    for (size_t i = 0; i < sizeof causes / sizeof causes[0]; i++)
        if (causes[i].cause == *cause) { snprintf(out, n, "%s", causes[i].text); return causes[i].busy; }
    for (size_t i = 0; i < sizeof statuses / sizeof statuses[0]; i++)
        if (statuses[i].sip == *sip) { snprintf(out, n, "%s", statuses[i].text); return statuses[i].busy; }
    snprintf(out, n, "Call failed");
    return FAULT;
}

/* country domains where names are registered one level down: example.co.uk */
static const char *const two_part[] = {
    "co.uk", "org.uk", "me.uk", "ltd.uk", "plc.uk", "net.uk", "ac.uk", "gov.uk", "sch.uk",
    "com.au", "net.au", "org.au", "co.nz", "net.nz", "org.nz", "co.za", "org.za", "co.jp", "ne.jp", "or.jp",
    "com.br", "net.br", "co.in", "net.in", "org.in", "com.cn", "net.cn", "co.il", "co.kr", "com.mx",
    "com.ar", "com.tr", "com.sg", "com.hk", "com.tw", "co.id", "com.my", "com.ph", "co.th", "com.ua",
};

static int is_ip(const char *h)
{
    if (strchr(h, ':') || h[0] == '[') return 1;   /* IPv6 */
    for (const char *p = h; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '.') return 0;
    return h[0] != 0;
}

void host_short(const char *host, char *out, size_t n)
{
    char h[256];
    snprintf(h, sizeof h, "%s", host ? host : "");
    if (h[0] == '[') { snprintf(out, n, "%s", h); return; }
    if (strchr(h, ':') == strrchr(h, ':')) h[strcspn(h, ":")] = 0;   /* one colon: a port, not IPv6 */
    size_t k = strlen(h);
    while (k && h[k - 1] == '.') h[--k] = 0;
    if (is_ip(h)) { snprintf(out, n, "%s", h); return; }
    /* the start of the last two and last three labels */
    const char *dots[3] = {NULL, NULL, NULL};
    int nd = 0;
    for (const char *p = h + k; p > h && nd < 3; p--)
        if (p[-1] == '.') dots[nd++] = p;
    if (nd < 2) { snprintf(out, n, "%s", h); return; }     /* two labels or fewer */
    const char *two = dots[1];
    for (size_t i = 0; i < sizeof two_part / sizeof two_part[0]; i++)
        if (!strcasecmp(two, two_part[i])) { snprintf(out, n, "%s", nd == 3 ? dots[2] : h); return; }
    snprintf(out, n, "%s", two);
}

void acct_names(int n, const char *const user[], const char *const host[], const char *const label[],
                char out[][ACCT_NAME_MAX])
{
    for (int i = 0; i < n; i++) {
        out[i][0] = 0;
        if (!user[i] || !user[i][0]) continue;
        if (label[i] && label[i][0]) snprintf(out[i], ACCT_NAME_MAX, "%s", label[i]);
        else host_short(host[i], out[i], ACCT_NAME_MAX);
    }
    /* the same twice: an account named after its host is its user if that is short (an extension,
     * "103"), otherwise the host's name and the start of the user */
    char base[n > 0 ? n : 1][ACCT_NAME_MAX];
    for (int i = 0; i < n; i++) memcpy(base[i], out[i], ACCT_NAME_MAX);
    for (int i = 0; i < n; i++) {
        if (!base[i][0] || (label[i] && label[i][0])) continue;
        for (int j = 0; j < n; j++) {
            if (j == i || strcmp(base[i], base[j])) continue;
            if (strlen(user[i]) <= 8) snprintf(out[i], ACCT_NAME_MAX, "%s", user[i]);
            else snprintf(out[i], ACCT_NAME_MAX, "%.30s (%.4s)", base[i], user[i]);
            break;
        }
    }
}
