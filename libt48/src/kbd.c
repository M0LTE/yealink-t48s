// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48: the on-screen keyboard. A full-screen text editor driven by touch
 * (three pages of keys covering printable ASCII) and by the phone's keypad
 * (multi-tap, as on an old mobile). The caller owns the event loop: it passes
 * keys and touches in, draws when told, and reads the text when done.
 * A password is never drawn in full: only the character just typed shows. */
#include <stdio.h>
#include <string.h>
#include "t48.h"

#define TAP_MS 900      /* a second press of the same keypad key within this cycles the letter */
#define REVEAL_MS 1200  /* how long the last character of a password shows */
#define FLASH_MS 120

#define K_BG T48_RGB(8, 10, 16)
#define K_FIELD T48_RGB(22, 29, 43)
#define K_KEY T48_RGB(34, 44, 64)
#define K_SIDE T48_RGB(24, 31, 46)
#define K_TEXT T48_RGB(226, 232, 240)
#define K_DIM T48_RGB(128, 142, 162)
#define K_ACCENT T48_RGB(255, 176, 60)

enum { A_NONE = 0, A_SHIFT = 1, A_BKSP, A_PAGE, A_LEFT, A_RIGHT, A_CANCEL, A_DONE };  /* above 31: that character */

static const char *const rows[3][3] = {
    {"qwertyuiop", "asdfghjkl.", "zxcvbnm"},
    {"1234567890", "@.-_/:;()&", "!?,'\"+="},
    {"[]{}#%^*<>", "$\\|~`", ""},
};
static const char *const page_label[3] = {"123", "#+=", "abc"};
static const char *const tap_abc[12] = {" 0", ".,@-_/:1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9",
                                        "*#+=!?", ""};
static const char *const mode_label[3] = {"abc", "ABC", "123"};

struct box { int x, y, w, h, act; };

/* the keys of the current page, laid out for a w x h screen */
static int layout(const t48_kbd *k, struct box *b)
{
    int W = k->w ? k->w : 800, H = k->h ? k->h : 480;
    int gap = 6, m = 8, top = H * 116 / 480, rh = (H - top - 10 - 3 * gap) / 4;
    int u = (W - 2 * m - 9 * gap) / 10, n = 0;

    for (int r = 0; r < 3; r++) {
        const char *s = rows[k->page][r];
        int cnt = (int)strlen(s), y = top + r * (rh + gap);
        int x = r == 2 ? m + u + u / 2 + gap + (7 - cnt) * (u + gap) / 2 : (W - cnt * u - (cnt - 1) * gap) / 2;
        for (int i = 0; i < cnt; i++, x += u + gap) b[n++] = (struct box){x, y, u, rh, s[i]};
    }
    int y2 = top + 2 * (rh + gap), y3 = top + 3 * (rh + gap), side = u + u / 2;
    if (k->page == 0) b[n++] = (struct box){m, y2, side, rh, A_SHIFT};
    b[n++] = (struct box){W - m - side, y2, side, rh, A_BKSP};
    int x = m;
    b[n++] = (struct box){x, y3, side, rh, A_PAGE}; x += side + gap;
    b[n++] = (struct box){x, y3, u, rh, A_LEFT}; x += u + gap;
    int done_w = 2 * u, cancel_w = side + u / 2;
    int space_w = W - m - done_w - gap - cancel_w - gap - u - gap - x;
    b[n++] = (struct box){x, y3, space_w, rh, ' '}; x += space_w + gap;
    b[n++] = (struct box){x, y3, u, rh, A_RIGHT}; x += u + gap;
    b[n++] = (struct box){x, y3, cancel_w, rh, A_CANCEL}; x += cancel_w + gap;
    b[n++] = (struct box){x, y3, done_w, rh, A_DONE};
    return n;
}

void t48_kbd_open(t48_kbd *k, const char *title, const char *text, int max, int flags)
{
    memset(k, 0, sizeof *k);
    snprintf(k->title, sizeof k->title, "%s", title ? title : "");
    snprintf(k->text, sizeof k->text, "%s", text ? text : "");
    k->len = k->cur = (int)strlen(k->text);
    k->max = max > 0 && max < (int)sizeof k->text ? max : (int)sizeof k->text - 1;
    k->flags = flags;
    if (flags & T48_KBD_DIGITS) { k->page = 1; k->mode = 2; }
}

const char *t48_kbd_text(const t48_kbd *k) { return k->text; }

static void commit_tap(t48_kbd *k) { k->tap_key = 0; k->tap_until = 0; }

static void insert(t48_kbd *k, int ch)
{
    if (k->len >= k->max) return;
    memmove(k->text + k->cur + 1, k->text + k->cur, (size_t)(k->len - k->cur) + 1);
    k->text[k->cur++] = (char)ch;
    k->len++;
    k->reveal_until = t48_now_ms() + REVEAL_MS;
}

static void backspace(t48_kbd *k)
{
    if (!k->cur) return;
    memmove(k->text + k->cur - 1, k->text + k->cur, (size_t)(k->len - k->cur) + 1);
    k->cur--;
    k->len--;
    k->reveal_until = 0;
}

static int act(t48_kbd *k, int a)
{
    commit_tap(k);
    switch (a) {
    case A_NONE: return T48_KBD_NOTHING;
    case A_SHIFT: k->shift = (k->shift + 1) % 3; break;
    case A_BKSP: backspace(k); break;
    case A_PAGE: k->page = (k->page + 1) % 3; break;
    case A_LEFT: if (k->cur > 0) k->cur--; k->reveal_until = 0; break;
    case A_RIGHT: if (k->cur < k->len) k->cur++; k->reveal_until = 0; break;
    case A_CANCEL: return T48_KBD_CANCEL;
    case A_DONE: return T48_KBD_DONE;
    default:
        if (k->page == 0 && k->shift && a >= 'a' && a <= 'z') {
            a -= 32;
            if (k->shift == 1) k->shift = 0;
        }
        insert(k, a);
    }
    return T48_KBD_REDRAW;
}

int t48_kbd_touch(t48_kbd *k, int state, int x, int y)
{
    struct box b[48];
    if (state != T48_TOUCH_DOWN) return T48_KBD_NOTHING;
    int n = layout(k, b);
    for (int i = 0; i < n; i++)
        if (x >= b[i].x && x < b[i].x + b[i].w && y >= b[i].y && y < b[i].y + b[i].h) {
            k->flash = b[i].act;
            k->flash_until = t48_now_ms() + FLASH_MS;
            return act(k, b[i].act);
        }
    return T48_KBD_NOTHING;
}

int t48_kbd_key(t48_kbd *k, int code, int down)
{
    long long now = t48_now_ms();
    if (!down) return T48_KBD_NOTHING;
    if (code == T48_KEY_OK) return act(k, A_DONE);
    if (code == T48_KEY_LEFT) return act(k, A_LEFT);
    if (code == T48_KEY_RIGHT) return act(k, A_RIGHT);
    if (code == T48_KEY_BACK) return k->len ? act(k, A_BKSP) : T48_KBD_CANCEL;
    if (code == T48_KEY_HASH) {            /* abc, ABC, 123 */
        commit_tap(k);
        k->mode = (k->mode + 1) % 3;
        return T48_KBD_REDRAW;
    }
    int idx = code >= '0' && code <= '9' ? code - '0' : code == T48_KEY_STAR ? 10 : -1;
    if (idx < 0) return T48_KBD_NOTHING;
    if (k->mode == 2 && idx < 10) {        /* digits go straight in */
        commit_tap(k);
        insert(k, code);
        return T48_KBD_REDRAW;
    }
    const char *set = k->mode == 2 ? ".*+:-" : tap_abc[idx];
    int again = k->tap_key == code && now < k->tap_until && k->cur > 0;
    k->tap_n = again ? (k->tap_n + 1) % (int)strlen(set) : 0;
    int ch = set[k->tap_n];
    if (k->mode == 1 && ch >= 'a' && ch <= 'z') ch -= 32;
    if (again) {
        k->text[k->cur - 1] = (char)ch;
        k->reveal_until = now + REVEAL_MS;
    } else {
        insert(k, ch);
    }
    k->tap_key = code;
    k->tap_until = now + TAP_MS;
    return T48_KBD_REDRAW;
}

int t48_kbd_timeout(const t48_kbd *k)
{
    long long now = t48_now_ms(), t = -1;
    const long long due[3] = {k->tap_until, k->flash_until, (k->flags & T48_KBD_PASSWORD) ? k->reveal_until : 0};
    for (int i = 0; i < 3; i++)
        if (due[i] && (t < 0 || due[i] - now < t)) t = due[i] > now ? due[i] - now : 0;
    return (int)t;
}

int t48_kbd_tick(t48_kbd *k)
{
    long long now = t48_now_ms();
    int r = T48_KBD_NOTHING;
    if (k->tap_until && now >= k->tap_until) { commit_tap(k); r = T48_KBD_REDRAW; }
    if (k->flash_until && now >= k->flash_until) { k->flash_until = 0; k->flash = 0; r = T48_KBD_REDRAW; }
    if (k->reveal_until && now >= k->reveal_until) { k->reveal_until = 0; r = T48_KBD_REDRAW; }
    return r;
}

void t48_kbd_draw(t48_kbd *k, t48_surface *s)
{
    struct box b[48];
    char shown[sizeof k->text], one[2] = {0, 0};
    const t48_font *f = &t48_font_head;
    long long now = t48_now_ms();

    k->w = s->w;
    k->h = s->h;
    t48_fill(s, 0, 0, s->w, s->h, K_BG);
    t48_text_fit(s, 16, 10, s->w - 260, &t48_font_bold, K_DIM, k->title);
    char m[40];
    snprintf(m, sizeof m, "keypad: %s  (# changes)", mode_label[k->mode]);
    t48_text_right(s, s->w - 16, 12, &t48_font_small, K_DIM, m);

    /* what to show: a password is stars, but for the character just typed */
    memcpy(shown, k->text, (size_t)k->len + 1);
    if (k->flags & T48_KBD_PASSWORD)
        for (int i = 0; i < k->len; i++)
            if (!(i == k->cur - 1 && k->reveal_until && now < k->reveal_until)) shown[i] = '*';

    /* the field, scrolled so the cursor is in view */
    int fx = 16, fy = 44, fw = s->w - 32, fh = 58, pad = 14, room = fw - 2 * pad, first = 0;
    t48_round_fill(s, fx, fy, fw, fh, 10, K_FIELD);
    for (;;) {
        char c = shown[k->cur];
        shown[k->cur] = 0;
        int w = t48_text_width(f, shown + first);
        shown[k->cur] = c;
        if (w <= room - 4 || first >= k->cur) break;
        first++;
    }
    int x = fx + pad, ty = fy + (fh - f->height) / 2, cx = x;
    for (int i = first; i <= k->len; i++) {
        if (i == k->cur) cx = x;
        if (i == k->len) break;
        one[0] = shown[i];
        if (x + t48_text_width(f, one) > fx + fw - pad) break;
        /* the letter a keypad press may still change is underlined */
        if (i == k->cur - 1 && k->tap_until) t48_fill(s, x, ty + f->height, t48_text_width(f, one), 2, K_ACCENT);
        x = t48_text(s, x, ty, f, K_TEXT, one);
    }
    t48_fill(s, cx, fy + 10, 2, fh - 20, K_ACCENT);
    if (!k->len && (k->flags & T48_KBD_PASSWORD))
        t48_text(s, fx + pad + 10, fy + (fh - t48_font_normal.height) / 2, &t48_font_normal, K_DIM, "hidden as you type");

    int n = layout(k, b);
    for (int i = 0; i < n; i++) {
        int a = b[i].act, hot = k->flash == a && k->flash_until;
        uint32_t fill = a == A_DONE ? K_ACCENT : a < 32 ? K_SIDE : K_KEY;
        const char *label = one;
        const t48_font *lf = &t48_font_head;
        if (a == A_SHIFT && k->shift) fill = k->shift == 2 ? K_ACCENT : K_KEY;
        if (hot) fill = t48_mix(fill, K_TEXT, 90);
        one[0] = (char)a;
        if (k->page == 0 && k->shift && a >= 'a' && a <= 'z') one[0] = (char)(a - 32);
        switch (a) {
        case A_SHIFT: label = k->shift == 2 ? "CAPS" : "Shift"; lf = &t48_font_bold; break;
        case A_BKSP: label = "Del"; lf = &t48_font_bold; break;
        case A_PAGE: label = page_label[k->page]; lf = &t48_font_bold; break;
        case A_LEFT: label = "<"; break;
        case A_RIGHT: label = ">"; break;
        case A_CANCEL: label = "Cancel"; lf = &t48_font_bold; break;
        case A_DONE: label = "Done"; lf = &t48_font_bold; break;
        case ' ': label = "space"; lf = &t48_font_normal; break;
        }
        t48_round_fill(s, b[i].x, b[i].y, b[i].w, b[i].h, 8, fill);
        int dark = a == A_DONE || (a == A_SHIFT && k->shift == 2);
        t48_text_centre(s, b[i].x + b[i].w / 2, b[i].y + (b[i].h - lf->height) / 2, lf,
                        dark ? K_BG : a == ' ' ? K_DIM : K_TEXT, label);
    }
}
