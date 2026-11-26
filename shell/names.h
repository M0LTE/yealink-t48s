// SPDX-License-Identifier: AGPL-3.0-or-later
/* Short words for long things: what a failed call was refused with, and what an account with no
 * Account name is called. Shared by the shell and the web setup page. See docs/phone.md. */
#ifndef NAMES_H
#define NAMES_H

#include <stddef.h>

/* A failed call, from baresip's reason ("488 Not Acceptable Here,Q.850;cause=88;text=..."):
 * a short line for the screen into out, the SIP status in *sip and the Q.850 cause in *cause
 * (0 where there is none). Returns 1 where the far end is busy or not answering (a busy tone),
 * 0 for a fault (the error tone). */
int call_fail_reason(const char *why, char *out, size_t n, int *sip, int *cause);

/* The registrable domain of a server host: "sip.example.net" -> "example.net",
 * "voip.example.co.uk" -> "example.co.uk". An IP address, or a name of two labels or fewer,
 * is kept as it is; a port is dropped. */
void host_short(const char *host, char *out, size_t n);

#define ACCT_NAME_MAX 48

/* What each of n accounts is called: its Account name, or the short form of its server host,
 * with the first 4 characters of its user added where two would otherwise read the same.
 * user[i] empty: the account is not set up, and out[i] is "". */
void acct_names(int n, const char *const user[], const char *const host[], const char *const label[],
                char out[][ACCT_NAME_MAX]);

#endif
