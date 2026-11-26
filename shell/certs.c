// SPDX-License-Identifier: AGPL-3.0-or-later
/* Root certificates the owner adds for SIP over TLS (issue #78, docs/sip.md#certificates). The
 * file given may come from anywhere, so every length is checked against what is left before it
 * is used, nothing is recursive, and only the fields shown are read: the names, the dates and
 * whether it says CA. What is kept is the DER written back out as PEM, not the file given. */
#include "certs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- DER: one element at a time, inside bounds ---------- */

struct der {
    const unsigned char *p, *end;
};

/* the next element in d: its tag and its contents in *in (start: where it began). 0, or -1 */
static int der_next(struct der *d, int *tag, struct der *in, const unsigned char **start)
{
    const unsigned char *s = d->p;
    size_t l;
    if (d->end - d->p < 2) return -1;
    *tag = *d->p++;
    if ((*tag & 0x1f) == 0x1f) return -1;          /* long tag numbers: not in a certificate */
    l = *d->p++;
    if (l & 0x80) {
        int k = l & 0x7f;
        if (k == 0 || k > 3) return -1;            /* indefinite, or more than 16 MB */
        if (d->end - d->p < k) return -1;
        for (l = 0; k--; ) l = l << 8 | *d->p++;
    }
    if (l > (size_t)(d->end - d->p)) return -1;
    in->p = d->p;
    in->end = d->p + l;
    d->p += l;
    if (start) *start = s;
    return 0;
}

/* the next element, which must have tag t */
static int der_want(struct der *d, int t, struct der *in)
{
    int tag;
    return der_next(d, &tag, in, NULL) == 0 && tag == t ? 0 : -1;
}

/* ---------- names and dates ---------- */

static const unsigned char OID_CN[] = {0x55, 0x04, 0x03}, OID_O[] = {0x55, 0x04, 0x0a}, OID_OU[] = {0x55, 0x04, 0x0b};
static const unsigned char OID_BC[] = {0x55, 0x1d, 0x13};

static int oid_is(const struct der *o, const unsigned char *want, size_t n)
{
    return (size_t)(o->end - o->p) == n && !memcmp(o->p, want, n);
}

/* a directory string as printable ASCII: anything else becomes one ? per character */
static void name_text(int tag, const struct der *v, char *out, size_t n)
{
    size_t k = 0;
    const unsigned char *p = v->p;
    while (p < v->end && k + 1 < n) {
        unsigned c;
        if (tag == 0x1e) {                          /* BMPString: two bytes a character */
            if (v->end - p < 2) break;
            c = p[0] ? '?' : p[1];
            p += 2;
        } else {
            c = *p++;
            if (c >= 0x80) {                        /* UTF-8: skip the rest of the character */
                while (p < v->end && (*p & 0xc0) == 0x80) p++;
                c = '?';
            }
        }
        out[k++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    out[k] = 0;
}

/* a Name: its common name, else its organisation, else its unit. 0, or -1 if it is not a Name */
static int name_read(struct der name, char *out, size_t n)
{
    char cn[72] = "", o[72] = "", ou[72] = "";
    struct der set, atv, oid, val;
    int tag;
    while (name.p < name.end) {
        if (der_want(&name, 0x31, &set) < 0) return -1;
        while (set.p < set.end) {
            if (der_want(&set, 0x30, &atv) < 0 || der_want(&atv, 0x06, &oid) < 0 || der_next(&atv, &tag, &val, NULL) < 0)
                return -1;
            if (oid_is(&oid, OID_CN, 3) && !cn[0]) name_text(tag, &val, cn, sizeof cn);
            else if (oid_is(&oid, OID_O, 3) && !o[0]) name_text(tag, &val, o, sizeof o);
            else if (oid_is(&oid, OID_OU, 3) && !ou[0]) name_text(tag, &val, ou, sizeof ou);
        }
    }
    snprintf(out, n, "%s", cn[0] ? cn : o[0] ? o : ou[0] ? ou : "No name");
    return 0;
}

/* days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil) */
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static int digits(const unsigned char *p, int n)
{
    int v = 0;
    while (n--) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p++ - '0');
    }
    return v;
}

/* UTCTime (YYMMDDHHMMSSZ) or GeneralizedTime (YYYYMMDDHHMMSSZ). 0, or -1 */
static int time_read(int tag, const struct der *t, long long *out)
{
    size_t n = (size_t)(t->end - t->p);
    const unsigned char *p = t->p;
    int y;
    if (tag == 0x17 && n == 13) { y = digits(p, 2); if (y < 0) return -1; y += y < 50 ? 2000 : 1900; p += 2; }
    else if (tag == 0x18 && n == 15) { y = digits(p, 4); if (y < 0) return -1; p += 4; }
    else return -1;
    int mo = digits(p, 2), d = digits(p + 2, 2), h = digits(p + 4, 2), mi = digits(p + 6, 2), s = digits(p + 8, 2);
    if (p[10] != 'Z' || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) return -1;
    *out = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
    return 0;
}

void cert_date(long long t, char *out, size_t n)
{
    /* civil_from_days, the inverse of the above */
    long long z = (t >= 0 ? t : t - 86399) / 86400 + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, n, "%04lld-%02u-%02u", (long long)yoe + era * 400 + (m <= 2), m, d);
}

/* ---------- one certificate ---------- */

/* basicConstraints' cA, from the extensions ([3]). Unknown extensions are passed over. */
static void extensions_read(struct der ext, struct cert *c)
{
    struct der seq, e, oid, val, bc, b;
    int tag;
    if (der_want(&ext, 0x30, &seq) < 0) return;
    while (seq.p < seq.end) {
        if (der_want(&seq, 0x30, &e) < 0 || der_want(&e, 0x06, &oid) < 0) return;
        if (der_next(&e, &tag, &val, NULL) < 0) return;
        if (tag == 0x01 && der_next(&e, &tag, &val, NULL) < 0) return;    /* critical */
        if (tag != 0x04 || !oid_is(&oid, OID_BC, 3)) continue;
        if (der_want(&val, 0x30, &bc) < 0) return;
        if (bc.p < bc.end && der_next(&bc, &tag, &b, NULL) == 0 && tag == 0x01 && b.end - b.p == 1 && b.p[0]) c->ca = 1;
    }
}

int cert_info(const unsigned char *der, size_t len, struct cert *c, const char **why)
{
    struct der all = {der, der + len}, cert, tbs, x, issuer, subject, validity;
    const unsigned char *is, *ss;
    int tag;
    uint64_t h = 0xcbf29ce484222325ull;

    memset(c, 0, sizeof *c);
    *why = "Not a certificate";
    if (len > CERT_DER_MAX) { *why = "Certificate too large"; return -1; }
    if (der_want(&all, 0x30, &cert) < 0 || all.p != all.end) return -1;
    if (der_want(&cert, 0x30, &tbs) < 0 || der_want(&cert, 0x30, &x) < 0 || der_want(&cert, 0x03, &x) < 0 || cert.p != cert.end)
        return -1;
    if (der_next(&tbs, &tag, &x, NULL) < 0) return -1;
    if (tag == 0xa0 && der_next(&tbs, &tag, &x, NULL) < 0) return -1;     /* version */
    if (tag != 0x02) return -1;                                              /* serial */
    if (der_want(&tbs, 0x30, &x) < 0) return -1;                            /* signature algorithm */
    if (der_next(&tbs, &tag, &issuer, &is) < 0 || tag != 0x30 || name_read(issuer, c->issuer, sizeof c->issuer) < 0) return -1;
    if (der_want(&tbs, 0x30, &validity) < 0) return -1;
    if (der_next(&validity, &tag, &x, NULL) < 0 || time_read(tag, &x, &c->not_before) < 0) { *why = "Bad date in certificate"; return -1; }
    if (der_next(&validity, &tag, &x, NULL) < 0 || time_read(tag, &x, &c->not_after) < 0) { *why = "Bad date in certificate"; return -1; }
    if (der_next(&tbs, &tag, &subject, &ss) < 0 || tag != 0x30 || name_read(subject, c->subject, sizeof c->subject) < 0) return -1;
    if (der_want(&tbs, 0x30, &x) < 0) return -1;                            /* the public key */
    while (tbs.p < tbs.end) {
        if (der_next(&tbs, &tag, &x, NULL) < 0) return -1;
        if (tag == 0xa3) extensions_read(x, c);
    }
    c->self_signed = issuer.end - is == subject.end - ss && !memcmp(is, ss, (size_t)(issuer.end - is));
    for (size_t i = 0; i < len; i++) h = (h ^ der[i]) * 0x100000001b3ull;   /* FNV-1a: a file name, not a check */
    snprintf(c->id, sizeof c->id, "%016llx", (unsigned long long)h);
    return 0;
}

/* ---------- PEM ---------- */

static int b64_value(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

/* base64 between p and end, white space allowed, into a malloc'd buffer. Its length, or -1 */
static long b64_decode(const unsigned char *p, const unsigned char *end, unsigned char **out)
{
    size_t cap = (size_t)(end - p) / 4 * 3 + 3, n = 0;
    unsigned acc = 0;
    int bits = 0, pad = 0;
    unsigned char *b = malloc(cap);
    if (!b) return -1;
    for (; p < end; p++) {
        int v;
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        if (*p == '=') { pad++; continue; }
        if (pad || (v = b64_value(*p)) < 0) { free(b); return -1; }   /* nothing after padding */
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; b[n++] = (unsigned char)(acc >> bits); }
    }
    if (pad > 2) { free(b); return -1; }
    *out = b;
    return (long)n;
}

static const unsigned char *find(const unsigned char *p, const unsigned char *end, const char *s)
{
    size_t k = strlen(s);
    for (; (size_t)(end - p) >= k; p++)
        if (*p == (unsigned char)s[0] && !memcmp(p, s, k)) return p;
    return NULL;
}

int cert_read(const unsigned char *in, size_t n, unsigned char **der, size_t *len, int max, const char **why)
{
    const unsigned char *p = in, *end = in + n;
    int count = 0;
    struct cert c;

    *why = "No certificate in file";
    if (n > CERT_FILE_MAX) { *why = "File too large"; return -1; }
    /* a key anywhere, in any block, and nothing of the file is taken */
    if (find(in, end, "PRIVATE KEY-----")) { *why = "File holds a private key; not added"; return -1; }
    if (n && in[0] == 0x30) {
        /* DER: one certificate, the whole file */
        if (max < 1 || cert_info(in, n, &c, why) < 0 || !(der[0] = malloc(n))) return -1;
        memcpy(der[0], in, n);
        len[0] = n;
        return 1;
    }
    while ((p = find(p, end, "-----BEGIN "))) {
        const unsigned char *label = p + 11, *le = find(label, end, "-----"), *body, *stop;
        unsigned char *b;
        long bl;
        if (!le || le - label > 40) { *why = "Malformed PEM"; goto bad; }
        if (find(label, le, "PRIVATE KEY")) { *why = "File holds a private key; not added"; goto bad; }
        body = le + 5;
        if (le - label != 11 || memcmp(label, "CERTIFICATE", 11)) {
            /* some other block: passed over */
            stop = find(body, end, "-----END ");
            if (!stop) { *why = "Malformed PEM"; goto bad; }
            p = stop + 9;
            continue;
        }
        stop = find(body, end, "-----END CERTIFICATE-----");
        if (!stop) { *why = "Malformed PEM"; goto bad; }
        if (count == max) { *why = "Too many certificates in file"; goto bad; }
        bl = b64_decode(body, stop, &b);
        if (bl <= 0) { *why = "Malformed PEM"; if (bl == 0) free(b); goto bad; }
        if (cert_info(b, (size_t)bl, &c, why) < 0) { free(b); goto bad; }
        der[count] = b;
        len[count++] = (size_t)bl;
        p = stop + 25;
    }
    if (count) return count;
bad:
    while (count) free(der[--count]);
    return -1;
}

/* ---------- the folder ---------- */

static int id_ok(const char *id)
{
    if (strlen(id) != 16) return 0;
    for (int i = 0; i < 16; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    return 1;
}

int cert_save(const char *dir, const unsigned char *der, size_t len, const char *id)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char path[512], tmp[520];
    FILE *f;
    int col = 0, ok;
    if (!id_ok(id)) return -1;
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s.pem", dir, id);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return -1;
    fputs("-----BEGIN CERTIFICATE-----\n", f);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)der[i] << 16 | (i + 1 < len ? (unsigned)der[i + 1] << 8 : 0) | (i + 2 < len ? der[i + 2] : 0);
        char q[4] = {tab[v >> 18 & 63], tab[v >> 12 & 63], i + 1 < len ? tab[v >> 6 & 63] : '=', i + 2 < len ? tab[v & 63] : '='};
        fwrite(q, 1, 4, f);
        if ((col += 4) == 64) { fputc('\n', f); col = 0; }
    }
    if (col) fputc('\n', f);
    fputs("-----END CERTIFICATE-----\n", f);
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) < 0) { unlink(tmp); return -1; }
    return 0;
}

static int by_subject(const void *a, const void *b)
{
    const struct cert *x = a, *y = b;
    int r = strcasecmp(x->subject, y->subject);
    return r ? r : strcmp(x->id, y->id);
}

int cert_list(const char *dir, struct cert *out, int max)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d) return 0;
    while (n < max && (e = readdir(d))) {
        char id[17], path[512];
        unsigned char buf[CERT_FILE_MAX], *der[1];
        size_t len[1], k = strlen(e->d_name);
        const char *why;
        int fd;
        ssize_t got;
        if (k != 20 || strcmp(e->d_name + 16, ".pem")) continue;
        memcpy(id, e->d_name, 16);
        id[16] = 0;
        if (!id_ok(id)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if ((fd = open(path, O_RDONLY)) < 0) continue;
        got = read(fd, buf, sizeof buf);
        close(fd);
        if (got <= 0 || cert_read(buf, (size_t)got, der, len, 1, &why) != 1) continue;
        if (cert_info(der[0], len[0], &out[n], &why) == 0) {
            snprintf(out[n].id, sizeof out[n].id, "%s", id);   /* the file's own name, so Remove finds it */
            n++;
        }
        free(der[0]);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_subject);
    return n;
}

int cert_remove(const char *dir, const char *id)
{
    char path[512];
    if (!id_ok(id)) return -1;
    snprintf(path, sizeof path, "%s/%s.pem", dir, id);
    return unlink(path) < 0 ? -1 : 0;
}
