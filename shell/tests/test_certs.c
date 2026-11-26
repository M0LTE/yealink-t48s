// SPDX-License-Identifier: AGPL-3.0-or-later
/* The certificate reader (certs.c, issue #78) on its own: good files read right, and hostile
 * ones (cut short, bytes changed, lengths that lie, keys, junk) are refused without reading out
 * of bounds. Built with -fsanitize=address,undefined it shows any slip: make -C shell
 * build/test_certs CFLAGS="-O1 -g -fsanitize=address,undefined". The two certificates are a
 * test root and a server certificate it issued, made for this file; their keys were thrown away. */
#include "../certs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char ROOT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBxjCCAWugAwIBAgIUNayFOWal6gLmkKwFz+jVXI1PkBgwCgYIKoZIzj0EAwIw\n"
    "MDEWMBQGA1UECgwNdDQ4IHRlc3Qgb25seTEWMBQGA1UEAwwNVDQ4IFRlc3QgUm9v\n"
    "dDAeFw0yNjExMjYwNTA4MDZaFw0zNjExMjMwNTA4MDZaMDAxFjAUBgNVBAoMDXQ0\n"
    "OCB0ZXN0IG9ubHkxFjAUBgNVBAMMDVQ0OCBUZXN0IFJvb3QwWTATBgcqhkjOPQIB\n"
    "BggqhkjOPQMBBwNCAAQubg/ui2GmUYbHRmfg8AThCO9RjLZyDMiTQFmj9+ggMTs2\n"
    "QUmhrqLs/eN5tWUMYauYfNklfjsqFISmTP2yfCU7o2MwYTAdBgNVHQ4EFgQUDYVy\n"
    "85Pn33e/XVN1gBzPolM/wI8wHwYDVR0jBBgwFoAUDYVy85Pn33e/XVN1gBzPolM/\n"
    "wI8wDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAgQwCgYIKoZIzj0EAwID\n"
    "SQAwRgIhAIq2D/FaKHSZ+pjJ/aTZxVd++6pGzJyw9sW1/DyfrfsrAiEAno2eWO4I\n"
    "PnSW5kFmLjcOStevJOD/j+qRNCW06YKZ+b0=\n"
    "-----END CERTIFICATE-----\n";
static const char LEAF[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBhzCCAS2gAwIBAgIUH6wNyk6gd20llcYpWAUZaTiRZtUwCgYIKoZIzj0EAwIw\n"
    "MDEWMBQGA1UECgwNdDQ4IHRlc3Qgb25seTEWMBQGA1UEAwwNVDQ4IFRlc3QgUm9v\n"
    "dDAeFw0yNjExMjYwNTA4MDZaFw0zNjExMjMwNTA4MDZaMBMxETAPBgNVBAMMCHNp\n"
    "cC50ZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEOqkl4nkunOK5jU+uayQG\n"
    "3jlBvzxpmDDaw/MvZEtvV+RCjCkMvp55umPkil45efpGLrRX1OaROY3EDlmEGYP5\n"
    "vKNCMEAwHQYDVR0OBBYEFDwfpPtRlmWk4LF22busAHzVe8MvMB8GA1UdIwQYMBaA\n"
    "FA2FcvOT5993v11TdYAcz6JTP8CPMAoGCCqGSM49BAMCA0gAMEUCIQDsAjY8Wv28\n"
    "fXsDJB5tVoMFPxI3BS60fKWTbIKNNwMZUgIgSmuOXgcrih8p5dYIPtpRTFIR1I9H\n"
    "hbK8+VdU4M8fOXY=\n"
    "-----END CERTIFICATE-----\n";

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int read_count(const char *s, size_t n, int max, const char **why)
{
    unsigned char *der[CERT_MAX];
    size_t len[CERT_MAX];
    int k = cert_read((const unsigned char *)s, n, der, len, max, why);
    for (int i = 0; i < k; i++) free(der[i]);
    return k;
}

int main(void)
{
    unsigned char *der[CERT_MAX];
    size_t len[CERT_MAX];
    const char *why;
    struct cert c;
    char date[16], buf[8192];
    int n;

    /* the root: its names, dates, CA, self-signed */
    n = cert_read((const unsigned char *)ROOT, strlen(ROOT), der, len, 4, &why);
    CHECK(n == 1, "the root is read");
    CHECK(n == 1 && cert_info(der[0], len[0], &c, &why) == 0, "the root's fields");
    CHECK(!strcmp(c.subject, "T48 Test Root"), "subject is the common name");
    CHECK(!strcmp(c.issuer, "T48 Test Root"), "issuer");
    CHECK(c.ca && c.self_signed, "a CA, self-signed");
    cert_date(c.not_after, date, sizeof date);
    CHECK(!strcmp(date, "2036-11-23"), "expiry date");
    cert_date(c.not_before, date, sizeof date);
    CHECK(!strcmp(date, "2026-11-26"), "start date");
    CHECK(strlen(c.id) == 16, "an id of 16 hex digits");
    /* the same as DER */
    {
        struct cert d;
        unsigned char *der2[1];
        size_t len2[1];
        CHECK(cert_read(der[0], len[0], der2, len2, 1, &why) == 1, "DER is read");
        CHECK(cert_info(der2[0], len2[0], &d, &why) == 0 && !strcmp(d.id, c.id), "DER gives the same id");
        free(der2[0]);
    }

    /* every cut short: refused, never read past the end */
    for (size_t k = 0; k < len[0]; k++)
        CHECK(cert_info(der[0], k, &c, &why) < 0, "a certificate cut short is refused");
    /* every byte changed to a few values: no crash, and the outer length still holds */
    for (size_t k = 0; k < len[0]; k++) {
        static const unsigned char vals[] = {0x00, 0x01, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0xff, 0x30, 0x1f};
        for (size_t v = 0; v < sizeof vals; v++) {
            unsigned char m[2048];
            memcpy(m, der[0], len[0]);
            m[k] = vals[v];
            (void)cert_info(m, len[0], &c, &why);
            if (cert_read(m, len[0], der + 1, len + 1, 1, &why) == 1) free(der[1]);
        }
    }
    /* random changes, many at once */
    srand(78);
    for (int t = 0; t < 20000; t++) {
        unsigned char m[2048];
        memcpy(m, der[0], len[0]);
        for (int j = rand() % 8; j >= 0; j--) m[rand() % len[0]] = (unsigned char)rand();
        if (cert_info(m, len[0], &c, &why) == 0) CHECK(strlen(c.subject) < sizeof c.subject, "subject stays in bounds");
    }
    free(der[0]);

    /* the server certificate: read, but not a root */
    n = cert_read((const unsigned char *)LEAF, strlen(LEAF), der, len, 4, &why);
    CHECK(n == 1 && cert_info(der[0], len[0], &c, &why) == 0 && !c.self_signed && !c.ca, "a server certificate is not a root");
    CHECK(!strcmp(c.subject, "sip.test") && !strcmp(c.issuer, "T48 Test Root"), "server certificate's names");
    if (n == 1) free(der[0]);

    /* two in one file, with text round them and another kind of block between */
    snprintf(buf, sizeof buf, "Root:\n%s\n-----BEGIN X509 CRL-----\nAAAA\n-----END X509 CRL-----\n%sbye\n", ROOT, LEAF);
    CHECK(read_count(buf, strlen(buf), 4, &why) == 2, "two certificates, other text skipped");
    CHECK(read_count(buf, strlen(buf), 1, &why) == -1 && !strcmp(why, "Too many certificates in file"), "more than asked for");

    /* refused files */
    CHECK(read_count("", 0, 4, &why) == -1, "an empty file");
    CHECK(read_count("hello", 5, 4, &why) == -1 && !strcmp(why, "No certificate in file"), "no certificate");
    snprintf(buf, sizeof buf, "%s-----BEGIN PRIVATE KEY-----\nMIIB\n-----END PRIVATE KEY-----\n", ROOT);
    CHECK(read_count(buf, strlen(buf), 4, &why) == -1 && strstr(why, "private key"), "a private key beside it");
    snprintf(buf, sizeof buf, "-----BEGIN EC PRIVATE KEY-----\nMIIB\n-----END EC PRIVATE KEY-----\n%s", ROOT);
    CHECK(read_count(buf, strlen(buf), 4, &why) == -1 && strstr(why, "private key"), "an EC private key before it");
    CHECK(read_count("-----BEGIN CERTIFICATE-----\nMIIB", 32, 4, &why) == -1, "no END");
    CHECK(read_count("-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n", 58, 4, &why) == -1, "not base64");
    CHECK(read_count("-----BEGIN CERTIFICATE-----\nAA==AA\n-----END CERTIFICATE-----\n", 60, 4, &why) == -1, "data after padding");
    CHECK(read_count("-----BEGIN CERTIFICATE-----\n\n-----END CERTIFICATE-----\n", 54, 4, &why) == -1, "empty block");
    CHECK(read_count("-----BEGIN CERTIFICATE", 22, 4, &why) == -1, "BEGIN cut short");
    {
        /* a label that never ends, and NULs in the middle */
        char z[300];
        memset(z, 'A', sizeof z);
        memcpy(z, "-----BEGIN ", 11);
        CHECK(read_count(z, sizeof z, 4, &why) == -1, "a label with no end");
        snprintf(buf, sizeof buf, "%s", ROOT);
        buf[100] = 0;
        CHECK(read_count(buf, strlen(ROOT), 4, &why) == -1, "a NUL inside the base64");
    }
    {
        /* lengths that claim more than there is, and long forms past 16 MB */
        static const unsigned char lie1[] = {0x30, 0x84, 0xff, 0xff, 0xff, 0xff, 0x30, 0x00};
        static const unsigned char lie2[] = {0x30, 0x83, 0xff, 0xff, 0xff, 0x30, 0x00};
        static const unsigned char lie3[] = {0x30, 0x80, 0x30, 0x00, 0x00, 0x00};
        static const unsigned char lie4[] = {0x30, 0x06, 0x30, 0x7f, 0x02, 0x01, 0x00, 0x00};
        static const unsigned char deep[] = {0x30, 0x04, 0x30, 0x02, 0x30, 0x00};
        CHECK(cert_info(lie1, sizeof lie1, &c, &why) < 0, "length over 16 MB");
        CHECK(cert_info(lie2, sizeof lie2, &c, &why) < 0, "length past the end");
        CHECK(cert_info(lie3, sizeof lie3, &c, &why) < 0, "indefinite length");
        CHECK(cert_info(lie4, sizeof lie4, &c, &why) < 0, "inner length past the end");
        CHECK(cert_info(deep, sizeof deep, &c, &why) < 0, "empty sequences");
    }
    {
        /* too big a file */
        char *big = malloc(CERT_FILE_MAX + 2);
        memset(big, ' ', CERT_FILE_MAX + 1);
        CHECK(read_count(big, CERT_FILE_MAX + 1, 4, &why) == -1 && !strcmp(why, "File too large"), "a file over the limit");
        free(big);
    }

    /* the folder: save, list, remove; ids that are not ids */
    {
        char dir[] = "/tmp/t48-certs-XXXXXX";
        struct cert list[CERT_MAX];
        CHECK(mkdtemp(dir) != NULL, "a folder");
        n = cert_read((const unsigned char *)ROOT, strlen(ROOT), der, len, 1, &why);
        cert_info(der[0], len[0], &c, &why);
        CHECK(cert_save(dir, der[0], len[0], c.id) == 0, "saved");
        CHECK(cert_save(dir, der[0], len[0], "../../etc/passwd") < 0, "an id with a path refused");
        CHECK(cert_list(dir, list, CERT_MAX) == 1 && !strcmp(list[0].id, c.id) && !strcmp(list[0].subject, "T48 Test Root"), "listed");
        CHECK(cert_remove(dir, "../x") < 0 && cert_remove(dir, "0123456789ABCDEF") < 0, "bad ids not removed");
        CHECK(cert_remove(dir, c.id) == 0 && cert_list(dir, list, CERT_MAX) == 0, "removed");
        free(der[0]);
        rmdir(dir);
    }

    printf("%s\n", fails ? "test_certs: FAILED" : "test_certs: all passed");
    return fails ? 1 : 0;
}
