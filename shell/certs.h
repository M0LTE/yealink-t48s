// SPDX-License-Identifier: AGPL-3.0-or-later
/* Root certificates the owner adds for SIP over TLS (issue #78, docs/sip.md#certificates):
 * reading PEM or DER, the few fields the screens show, and the folder they are kept in. The
 * shell lists them; the web setup page adds and removes them. Nothing here checks a signature:
 * t48-sip hands the files to baresip, whose OpenSSL does the checking. */
#ifndef CERTS_H
#define CERTS_H
#include <stddef.h>

#define CERT_MAX 16              /* roots kept at once */
#define CERT_DER_MAX 16384       /* one certificate */
#define CERT_FILE_MAX 65536      /* a file given to the web setup page */

struct cert {
    char id[17];                 /* 16 hex digits from the DER; the file is <id>.pem */
    char subject[72];            /* its common name, else organisation or unit; printable ASCII */
    char issuer[72];
    long long not_before, not_after;   /* seconds since 1970, UTC */
    int ca;                      /* basic constraints say CA */
    int self_signed;             /* issuer and subject are the same name: a root */
};

/* The certificates in a file: PEM (each CERTIFICATE block; other blocks are skipped) or one DER
 * certificate. Up to max of them, each malloc'd in der[i], len[i]; the count, or -1 with the
 * reason in *why. A private key anywhere in the file is refused, so it is never kept. */
int cert_read(const unsigned char *in, size_t n, unsigned char **der, size_t *len, int max, const char **why);
/* What one DER certificate says. 0, or -1 with the reason in *why if it is not one. */
int cert_info(const unsigned char *der, size_t len, struct cert *c, const char **why);
/* Keep a certificate as dir/<id>.pem (dir made if need be). 0, or -1. */
int cert_save(const char *dir, const unsigned char *der, size_t len, const char *id);
/* The certificates kept in dir, sorted by subject; the count */
int cert_list(const char *dir, struct cert *out, int max);
/* Take one out by its id. 0, or -1 if the id is not one or there is no such file. */
int cert_remove(const char *dir, const char *id);
/* A date as the screens show it, 2027-01-14 */
void cert_date(long long t, char *out, size_t n);

#endif
