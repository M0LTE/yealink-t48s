// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The dial plan (issue #77, docs/phone.md#dial-plan): a digit map, in the
 * syntax of MGCP (RFC 3435, 2.1.5) as Polycom and others take it, says when a
 * number being typed is complete, so that the dialler can call it without
 * the Call key.
 *
 *   999|112|07xxxxxxxxx|00x.T|x.T
 *
 * Patterns are separated by |, and the whole may be in ( ). In a pattern:
 * 0-9 * # + and A-D are themselves; x is any digit; [2-9], [*#x] or [0158]
 * is one of a set; a dot after an element means any number of it, none
 * included; T at the end means "when the owner stops typing", after 4 s, or
 * T2 for 2 s (1 to 99). Spaces are ignored, and x and T may be in either case.
 *
 * What the dialler does with a number, from all the patterns at once:
 *   - one without T matches it whole, and no pattern without T could take
 *     more digits: dial now (DP_NOW). Patterns with T do not hold it up: they
 *     are the "otherwise" of the map, and x.T would otherwise hold up every
 *     number there is;
 *   - one without T matches it but another could take more: dial after the
 *     short timer, 4 s (DP_TIMER);
 *   - one with T matches up to its T: dial after its time (DP_TIMER);
 *   - more digits could still make one match: wait (DP_PARTIAL);
 *   - none can: DP_NONE. The Call key still calls it.
 *
 * This file is plain C with no state, so the shell, the web setup page and
 * the unit test (tests/test_dialplan.c) share it.
 */
#include <string.h>
#include "dialplan.h"

#define MAXTOK 63   /* positions 0 to MAXTOK fit in 64 bits */

/* one element of a pattern: the characters it takes, whether it repeats, or the timer */
struct tok {
    unsigned set;        /* bit per character in KEYS */
    int repeat;
    int timer_ms;        /* > 0 or 0: this is the T at the end */
    int is_timer;
};

static const char KEYS[] = "0123456789*#+ABCD";

static int key_bit(char c)
{
    if (c >= 'a' && c <= 'd') c = (char)(c - 'a' + 'A');
    const char *p = c ? strchr(KEYS, c) : NULL;
    return p ? 1 << (p - KEYS) : 0;
}

#define DIGITS 0x3ffu

/* one pattern, from s to end (spaces already allowed for); the number of elements, or -1 */
static int parse(const char *s, const char *end, struct tok *t, const char *map, int *at)
{
    int n = 0;
#define FAIL(p) do { if (at) *at = (int)((p) - map); return -1; } while (0)
    while (s < end) {
        char c = *s;
        if (c == ' ' || c == '\t') { s++; continue; }
        if (n && t[n - 1].is_timer) FAIL(s);          /* nothing after T */
        if (n >= MAXTOK) FAIL(s);
        memset(&t[n], 0, sizeof t[n]);
        if (c == '.') {
            if (!n || t[n - 1].repeat) FAIL(s);
            t[n - 1].repeat = 1;
            s++;
            continue;
        }
        if (c == 'T' || c == 't') {
            int secs = 0, digits = 0;
            s++;
            while (s < end && *s >= '0' && *s <= '9' && digits < 2) { secs = secs * 10 + (*s++ - '0'); digits++; }
            if (s < end && *s >= '0' && *s <= '9') FAIL(s);
            if (digits && secs < 1) FAIL(s - digits);   /* T0 would dial the first digit typed */
            t[n].is_timer = 1;
            t[n].timer_ms = digits ? secs * 1000 : DP_TIMER_MS;
            n++;
            continue;
        }
        if (c == 'x' || c == 'X') {
            t[n++].set = DIGITS;
            s++;
            continue;
        }
        if (c == '[') {
            const char *open = s++;
            unsigned set = 0;
            while (s < end && *s != ']') {
                if (*s == ' ') { s++; continue; }
                if (*s == 'x' || *s == 'X') { set |= DIGITS; s++; continue; }
                if (!key_bit(*s)) FAIL(s);
                if (s + 2 < end && s[1] == '-' && s[2] != ']') {
                    if (*s < '0' || *s > '9' || s[2] < '0' || s[2] > '9' || s[2] < *s) FAIL(s);
                    for (char d = *s; d <= s[2]; d++) set |= (unsigned)key_bit(d);
                    s += 3;
                    continue;
                }
                set |= (unsigned)key_bit(*s++);
            }
            if (s >= end || !set) FAIL(open);
            s++;
            t[n++].set = set;
            continue;
        }
        if (!key_bit(c)) FAIL(s);
        t[n++].set = (unsigned)key_bit(c);
        s++;
    }
    if (!n) FAIL(s);
    return n;
#undef FAIL
}

/* the pattern list: where each one starts and ends, without the ( ) around the whole */
static int split(const char *map, const char **from, const char **to, int max, int *at)
{
    const char *s = map, *end = map + strlen(map);
    int n = 0;
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (s < end && *s == '(') {
        if (end[-1] != ')') { if (at) *at = (int)(end - map); return -1; }
        s++;
        end--;
    }
    if (s >= end) { if (at) *at = (int)(s - map); return -1; }
    for (;;) {
        const char *bar = memchr(s, '|', (size_t)(end - s));
        if (n >= max) { if (at) *at = (int)(s - map); return -1; }
        from[n] = s;
        to[n++] = bar ? bar : end;
        if (!bar) break;
        s = bar + 1;
    }
    return n;
}

int dp_check(const char *map, int *at)
{
    const char *from[64], *to[64];
    struct tok t[MAXTOK];
    if (at) *at = 0;
    if (strlen(map) > DP_MAX) { if (at) *at = DP_MAX; return -1; }
    int n = split(map, from, to, 64, at);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++)
        if (parse(from[i], to[i], t, map, at) < 0) return -1;
    return 0;
}

/* add position p, and every position a repeating element lets the match skip to */
static void reach(unsigned long long *set, const struct tok *t, int n, int p)
{
    for (; p <= n; p++) {
        *set |= 1ull << p;
        if (p == n || !t[p].repeat) break;
    }
}

enum dp_result dp_match(const char *map, const char *number, int *ms)
{
    const char *from[64], *to[64];
    struct tok t[MAXTOK];
    int exact = 0, grow = 0, partial = 0, timer = -1;

    if (ms) *ms = 0;
    if (dp_check(map, NULL) < 0) return DP_NONE;
    int np = split(map, from, to, 64, NULL);
    for (int i = 0; i < np; i++) {
        int n = parse(from[i], to[i], t, map, NULL);
        int timed = t[n - 1].is_timer;
        unsigned long long pos = 0;
        reach(&pos, t, n, 0);
        for (const char *c = number; *c && pos; c++) {
            unsigned bit = (unsigned)key_bit(*c);
            unsigned long long nx = 0;
            for (int p = 0; p < n; p++) {
                if (!(pos >> p & 1) || t[p].is_timer || !(t[p].set & bit)) continue;
                if (t[p].repeat) reach(&nx, t, n, p);
                else reach(&nx, t, n, p + 1);
            }
            pos = nx;
        }
        if (!pos) continue;
        for (int p = 0; p <= n; p++) {
            if (!(pos >> p & 1)) continue;
            if (p == n) exact = 1;                                      /* only a pattern with no T gets here */
            else if (t[p].is_timer) { if (timer < 0 || t[p].timer_ms < timer) timer = t[p].timer_ms; }
            else { partial = 1; if (!timed) grow = 1; }
        }
    }
    if (exact && !grow) return DP_NOW;
    if (exact && (timer < 0 || DP_TIMER_MS < timer)) timer = DP_TIMER_MS;
    if (timer >= 0) {
        if (ms) *ms = timer;
        return DP_TIMER;
    }
    return partial ? DP_PARTIAL : DP_NONE;
}
