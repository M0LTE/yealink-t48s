// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48's small JSON reader. Lifted out of native/waterfall.c. It walks the
 * text in place: no tree, no allocation. */
#include <stdlib.h>
#include <string.h>
#include "t48.h"

static const char *jws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

static const char *jskip_str(const char *p, const char *e)  /* p at the opening quote */
{
    for (p++; p < e; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return e;
}

static const char *jskip(const char *p, const char *e)  /* past one value */
{
    p = jws(p, e);
    if (p >= e) return e;
    if (*p == '"') return jskip_str(p, e);
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (p < e) {
            if (*p == '"') { p = jskip_str(p, e); continue; }
            if (*p == '{' || *p == '[') d++;
            else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
            p++;
        }
        return e;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* the value of `key` in the object at o, or NULL */
const char *t48_json_get(const char *o, const char *e, const char *key)
{
    if (!o) return NULL;
    o = jws(o, e);
    if (o >= e || *o != '{') return NULL;
    o++;
    size_t kl = strlen(key);
    for (;;) {
        o = jws(o, e);
        if (o >= e || *o != '"') return NULL;
        const char *ks = o + 1, *ke = jskip_str(o, e);
        int match = (size_t)(ke - 1 - ks) == kl && !memcmp(ks, key, kl);
        o = jws(ke, e);
        if (o >= e || *o != ':') return NULL;
        o = jws(o + 1, e);
        if (match) return o;
        o = jws(jskip(o, e), e);
        if (o < e && *o == ',') o++;
    }
}

int t48_json_num(const char *v, const char *e, double *out)
{
    if (!v || v >= e || !(*v == '-' || (*v >= '0' && *v <= '9'))) return 0;
    *out = strtod(v, NULL);  /* the message buffer is NUL terminated */
    return 1;
}

int t48_json_str(const char *v, const char *e, char *out, size_t n)
{
    if (!v || v >= e || *v != '"' || !n) return 0;
    size_t k = 0;
    for (v++; v < e && *v != '"'; v++) {
        char c = *v;
        if (c == '\\' && v + 1 < e) {
            v++;
            if (*v == 'u' && v + 4 < e) {
                unsigned u = (unsigned)strtoul((char[5]){v[1], v[2], v[3], v[4], 0}, NULL, 16);
                c = u >= 32 && u < 127 ? (char)u : '?';
                v += 4;
            } else {
                c = *v == 'n' ? ' ' : *v == 't' ? ' ' : *v;
            }
        }
        if (k + 1 < n) out[k++] = c;
    }
    out[k] = 0;
    return 1;
}

int t48_json_true(const char *v, const char *e) { return v && e - v >= 4 && !memcmp(v, "true", 4); }

/* array iteration: first element after '[', then the next one; NULL at the end */
const char *t48_json_first(const char *a, const char *e)
{
    if (!a || a >= e || *a != '[') return NULL;
    a = jws(a + 1, e);
    return a < e && *a != ']' ? a : NULL;
}

const char *t48_json_next(const char *v, const char *e)
{
    v = jws(jskip(v, e), e);
    if (v >= e || *v != ',') return NULL;
    v = jws(v + 1, e);
    return v < e ? v : NULL;
}

/* ---------- station state ---------- */
