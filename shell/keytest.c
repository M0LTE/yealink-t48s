// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the key test. For a phone whose key table is in doubt and which
 * has no console (the T46S, issue 10): every press and release is shown with
 * its place in the key matrix as the kernel's driver numbers it
 * (kernel6/drivers/t48s_keypad.c: code = row * 8 + column + 1, sent as
 * MSC_SCAN) and the Linux key the table makes of it, large enough to read in
 * a photo of the screen, above the codes seen so far in the order they were
 * first pressed. A table can be put right from those photos alone.
 *
 * Nothing here trusts the table. It opens from Settings, About, or by holding
 * any key for KEYTEST_OPEN_MS at the idle screen (main.c); it is left by
 * holding any key for KEYTEST_LEAVE_MS. While it shows, no key does anything
 * else.
 */
#include <stdio.h>
#include <string.h>
#include "shell.h"

#define KEY_UNKNOWN_ 240       /* what the driver sends for a place its table does not hold */
#define MAX_SEEN 45            /* what the grid has room for; the T46S has 42 keys */
#define GRID_COLS 9
#define GRID_Y 164
#define CELL_W 86
#define CELL_H 54
#define GRID_X ((SW - GRID_COLS * CELL_W) / 2)

static struct { int scan, mapped; } seen[MAX_SEEN];
static int nseen, total;
static int last_scan, last_key, last_value, any;
static int held_scan, held_key;
static long long leave_at;     /* when the key now held has been held long enough to leave; 0: none is */

/* Linux key codes by number: the phone's kernel headers are older than some of the names */
static const char *key_name(int key)
{
    static const struct { int key; const char *name; } names[] = {
        {11, "KEY_0"}, {2, "KEY_1"}, {3, "KEY_2"}, {4, "KEY_3"}, {5, "KEY_4"}, {6, "KEY_5"}, {7, "KEY_6"},
        {8, "KEY_7"}, {9, "KEY_8"}, {10, "KEY_9"}, {55, "KEY_KPASTERISK"}, {0x20b, "KEY_NUMERIC_POUND"},
        {103, "KEY_UP"}, {108, "KEY_DOWN"}, {105, "KEY_LEFT"}, {106, "KEY_RIGHT"}, {28, "KEY_ENTER"},
        {14, "KEY_BACKSPACE"}, {114, "KEY_VOLUMEDOWN"}, {115, "KEY_VOLUMEUP"}, {148, "KEY_PROG1"},
        {113, "KEY_MUTE"}, {430, "KEY_MESSENGER"}, {119, "KEY_PAUSE"}, {129, "KEY_AGAIN"},
        {159, "KEY_FORWARD"}, {149, "KEY_PROG2"},
    };
    static char b[24];
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].key == key) return names[i].name;
    if (key >= 0x290 && key < 0x290 + 30) snprintf(b, sizeof b, "KEY_MACRO%d", key - 0x290 + 1);
    else snprintf(b, sizeof b, "Key %d", key);
    return b;
}

static int mapped(int key, int value) { return value >= 0 && key != KEY_UNKNOWN_; }

void keytest_open(void)
{
    nseen = total = any = 0;
    leave_at = 0;
}

void keytest_event(int scan, int key, int value)
{
    int i;
    logmsg("key test: code %d key %d %s", scan, key, value > 0 ? "down" : value ? "no key" : "up");
    last_scan = scan;
    last_key = key;
    last_value = value;
    any = 1;
    if (value > 0) {
        held_scan = scan;
        held_key = key;
        leave_at = t48_now_ms() + KEYTEST_LEAVE_MS;
    } else if (!value && scan == held_scan && key == held_key) {
        leave_at = 0;
    }
    for (i = 0; i < nseen; i++)
        if (seen[i].scan == scan && (scan || seen[i].mapped == key)) break;
    if (i < nseen) {
        if (scan) seen[i].mapped = mapped(key, value);
        return;
    }
    total++;
    if (nseen == MAX_SEEN) memmove(seen, seen + 1, sizeof seen[0] * (size_t)--nseen);
    seen[nseen].scan = scan;
    seen[nseen++].mapped = scan ? mapped(key, value) : key;   /* no place known: told apart by the key */
}

int keytest_timeout(long long now)
{
    return !leave_at ? -1 : leave_at > now ? (int)(leave_at - now) : 0;
}

int keytest_tick(long long now, int *scan, int *key)
{
    if (!leave_at || now < leave_at) return 0;
    leave_at = 0;
    *scan = held_scan;
    *key = held_key;
    return 1;
}

void keytest_draw(void)
{
    char a[64], b[40];
    const t48_font *f = &t48_font_big;

    ui_clear(C_BG);
    ui_header("Key test", 0, 0);
    if (total) {
        snprintf(a, sizeof a, "%d seen", total);
        t48_text_right(&ui, SW - 100, (HEAD_H - t48_font_bold.height) / 2, &t48_font_bold, T->on_head_dim, a);
    }
    if (!any) {
        t48_text_centre(&ui, SW / 2, 82, f, C_DIM, "Press a key");
    } else {
        /* the place, then the key and which way it went */
        if (last_scan) snprintf(a, sizeof a, "Row %d  Col %d  Code %d", (last_scan - 1) / 8, (last_scan - 1) % 8, last_scan);
        else snprintf(a, sizeof a, "No code");
        t48_text_centre(&ui, SW / 2, 56, f, C_TEXT, a);
        int known = mapped(last_key, last_value);
        snprintf(a, sizeof a, "%s", known ? key_name(last_key) : "Not mapped");
        snprintf(b, sizeof b, "%s", last_value > 0 ? "  Down" : last_value ? "" : "  Up");
        int x = (SW - t48_text_width(f, a) - t48_text_width(f, b)) / 2;
        x = t48_text(&ui, x, 106, f, known ? C_TEXT : C_BAD, a);
        t48_text(&ui, x, 106, f, last_value > 0 ? C_GOOD : C_DIM, b);
    }
    for (int i = 0; i < nseen; i++) {
        int x = GRID_X + (i % GRID_COLS) * CELL_W, y = GRID_Y + (i / GRID_COLS) * CELL_H;
        int now = any && seen[i].scan == last_scan && (last_scan || seen[i].mapped == last_key);
        struct row_ink ink;
        ui_tile(x + 2, y + 2, CELL_W - 4, CELL_H - 4, now, &ink);
        if (seen[i].scan) snprintf(a, sizeof a, "%d", seen[i].scan);
        else snprintf(a, sizeof a, "-");
        t48_text_centre(&ui, x + CELL_W / 2, y + (CELL_H - f->height) / 2, f, seen[i].scan && !seen[i].mapped ? C_BAD : ink.text, a);
    }
    t48_text_centre(&ui, SW / 2, 448, &t48_font_normal, C_DIM, "Hold any key 3 s to leave");
}

const char *keytest_state(const char *key)
{
    static char b[48];
    if (!strcmp(key, "keys.seen")) { snprintf(b, sizeof b, "%d", total); return b; }
    if (!strcmp(key, "keys.last")) {
        if (!any) return "-";
        snprintf(b, sizeof b, "%d/%d/%s", last_scan, last_key, last_value > 0 ? "down" : last_value ? "none" : "up");
        return b;
    }
    return NULL;
}
