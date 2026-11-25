// SPDX-License-Identifier: AGPL-3.0-or-later
/* The dial plan's matcher (shell/dialplan.c, issue #77): the syntax it takes and refuses, and what it says of
 * numbers under the default map and others. Run by make check. */
#include <stdio.h>
#include <string.h>
#include "../dialplan.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const names[] = {"none", "partial", "timer", "now"};

/* what map says of number: "now", "partial", "none", or "timer" with the milliseconds, "timer4000" */
static const char *what(const char *map, const char *number)
{
    static char b[32];
    int ms = -1;
    enum dp_result r = dp_match(map, number, &ms);
    if (r == DP_TIMER) snprintf(b, sizeof b, "timer%d", ms);
    else snprintf(b, sizeof b, "%s", names[r]);
    return b;
}

#define IS(map, number, want) do { const char *got_ = what(map, number); \
    CHECK(!strcmp(got_, want), "\"%s\" with %s: %s, wanted %s", number, map, got_, want); } while (0)
#define D(number, want) IS(DP_DEFAULT, number, want)

#define GOOD(map) do { int at_; CHECK(dp_check(map, &at_) == 0, "%s refused at %d", map, at_); } while (0)
#define BAD(map, where) do { int at_ = -9; CHECK(dp_check(map, &at_) < 0, "%s taken", map); \
    CHECK((where) < 0 || at_ == (where), "%s: refused at %d, wanted %d", map, at_, where); } while (0)

int main(void)
{
    /* ---------- the syntax ---------- */
    GOOD(DP_DEFAULT);
    CHECK(strlen(DP_DEFAULT) <= DP_MAX, "the default is %zu long", strlen(DP_DEFAULT));
    GOOD("x.T");
    GOOD("999");
    GOOD("(999|112)");
    GOOD(" ( 999 | 112 ) ");
    GOOD("[2-9]xxxxxxxxx|07xxxxxxxxx|999|112|00x.T");
    GOOD("[2-9]11|0T|011xxx.T|[0-1][2-9]xx[2-9]xxxxxx|[2-9]xx[2-9]xxxxxx|[2-9]xxxT");   /* Polycom's own */
    GOOD("XxX.t");
    GOOD("xxxT2");
    GOOD("xxxT10");
    GOOD("xT0");
    GOOD("[*#x]");
    GOOD("[0158]x");
    GOOD("[x*]x.");
    GOOD("*xx#");
    GOOD("+x.T");
    GOOD("ABCD");
    BAD("[a-d]", 1);   /* a range is of digits only */
    BAD("", 0);
    BAD("   ", 3);
    BAD("()", 1);
    BAD("(999", 4);
    BAD("999|", 4);
    BAD("|999", 0);
    BAD("999||112", 4);
    BAD(".x", 0);
    BAD("x..", 2);
    BAD("xTx", 2);
    BAD("x.T.", 3);
    BAD("xT123", 4);
    BAD("[2-9", 0);
    BAD("[]", 0);
    BAD("[9-2]", 1);
    BAD("[-9]", 1);
    BAD("[2-]", 2);
    BAD("x?", 1);
    GOOD("07xxxxxxxxx|0a7");   /* a is A: a key the phone does not have, but a key */
    {
        int at;
        CHECK(dp_check("07xxxxxxxxx|0e7", &at) < 0 && at == 13, "a letter that is not a key: refused at %d", at);
    }
    {
        char big[DP_MAX + 2];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = 0;
        BAD(big, DP_MAX);
        big[DP_MAX] = 0;          /* DP_MAX long is taken, if not as many elements */
        BAD(big, -1);
        GOOD("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");   /* 63 elements */
        BAD("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 63);   /* 64 */
    }
    /* a map that does not check matches nothing */
    IS("999|", "999", "none");

    /* ---------- the default: emergency and service numbers ---------- */
    D("", "timer4000");        /* x. takes nothing too; the dialler never asks about an empty number */
    D("9", "timer4000");
    D("99", "timer4000");
    D("999", "now");
    D("1", "timer4000");
    D("11", "timer4000");
    D("112", "now");
    D("111", "now");
    D("101", "now");
    D("105", "now");
    D("100", "timer4000");     /* the operator: no pattern of its own */
    D("123", "timer4000");
    D("116", "timer4000");
    D("11612", "timer4000");
    D("116123", "now");
    D("118", "timer4000");
    D("118118", "now");
    D("141", "timer4000");     /* withhold the number: the rest follows */
    D("14107700900123", "timer4000");
    D("1471", "timer4000");
    D("9999", "timer4000");    /* not 999 with more: 999 went as soon as it was typed */

    /* UK geographic and non-geographic numbers, eleven digits */
    D("0", "timer4000");
    D("01", "timer4000");
    D("0118496", "timer4000");
    D("01184960000", "now");
    D("02079460000", "now");
    D("0207946000", "timer4000");
    D("020794600001", "timer4000");   /* typed on: the twelfth digit came before... not possible, but no harm */
    D("03001234567", "now");
    D("0169772000", "timer4000");     /* one of the ten-digit numbers: after the pause */
    D("07", "timer4000");
    D("0770090012", "timer4000");
    D("07700900123", "now");
    D("05512345678", "now");
    D("05612345678", "now");
    D("0571234567", "timer4000");
    D("08001111", "timer4000");       /* 0800 numbers are 8, 10 or 11 digits */
    D("0800123456", "timer4000");
    D("08001234567", "timer4000");
    D("08081234567", "now");
    D("08451234567", "now");
    D("08701234567", "now");
    D("09061234567", "now");
    D("06123456789", "timer4000");

    /* international, star codes, hash */
    D("00", "timer4000");
    D("0033123456789", "timer4000");
    D("+", "partial");
    D("+4", "timer4000");
    D("+44", "timer4000");
    D("*", "partial");
    D("*6", "timer4000");
    D("*21*07700900123#", "timer4000");
    D("#", "partial");
    D("#31#", "timer4000");
    D("*#06#", "timer4000");
    D("0770#", "none");               /* the dialler takes a # the map has no place for as Call */
    D("999#", "none");
    D("1*", "none");
    D("A", "none");

    /* ---------- other maps ---------- */
    /* the example in the issue */
    const char *uk = "[2-9]xxxxxxxxx|07xxxxxxxxx|999|112|00x.T";
    IS(uk, "2", "partial");
    IS(uk, "212345678", "partial");
    IS(uk, "2123456789", "now");
    IS(uk, "21234567890", "none");
    IS(uk, "999", "timer4000");       /* [2-9]xxxxxxxxx could take more: the short timer */
    IS(uk, "9991", "partial");
    IS(uk, "112", "now");
    IS(uk, "07700900123", "now");
    IS(uk, "00", "timer4000");
    IS(uk, "001", "timer4000");
    IS(uk, "0", "partial");
    IS(uk, "01", "none");
    /* T's own time; the shortest wins; an exact match beside a T is held for the shorter of the two */
    IS("xxxT2", "123", "timer2000");
    IS("xxxT2", "12", "partial");
    IS("xxxT2", "1234", "none");
    IS("x.T10|x.T3", "5", "timer3000");
    IS("x.T", "", "timer4000");       /* x. takes nothing too */
    IS("xx.T", "", "partial");
    IS("xT0", "5", "now");
    IS("101|1011", "101", "timer4000");
    IS("101|1011", "1011", "now");
    IS("101|101x.T9", "101", "now");  /* patterns with T do not hold up one without */
    IS("101|101x.T2", "1012", "timer2000");
    IS("101|101xT9|1011", "101", "timer4000");
    IS("1x.T1|101", "101", "now");
    /* repeats in the middle, sets, and the other keys */
    IS("0x.#", "0", "partial");
    IS("0x.#", "0#", "now");
    IS("0x.#", "0123#", "now");
    IS("0x.#", "0123#4", "none");
    IS("[*#][*#x][*#x].T", "*", "partial");
    IS("[*#][*#x][*#x].T", "*1", "timer4000");
    IS("[*#][*#x][*#x].T", "**#", "timer4000");
    IS("[13579]", "5", "now");
    IS("[13579]", "4", "none");
    IS("[0-35-9]x", "4", "none");
    IS("[0-35-9]x", "61", "now");
    IS("+x.T", "+", "timer4000");
    IS("+xx.T", "+", "partial");
    IS("+x.T", "+1", "timer4000");
    IS("[x*]x", "*1", "now");
    IS("(999|112)", "112", "now");
    IS(" 9 9 9 ", "999", "now");
    IS("ABCD", "abcd", "now");
    IS("07xxxxxxxxx", "07700 900123", "none");   /* a space is no key */
    IS("x.T", "07700900123456789012345678901234567890", "timer4000");

    printf("dial plan: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
