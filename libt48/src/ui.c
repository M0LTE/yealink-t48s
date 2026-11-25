// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * libt48: parts in the theme's look (docs/design/README.md, docs/style.md):
 * the header, buttons, keypad keys, list rows, switches, level bars, lamps,
 * tags, panels and the box of a question. The shell's own screens are made of
 * these, and an app that uses them looks like the shell. A screen says what a
 * thing is (a button that starts a call, a selected row) and this file decides
 * what that looks like in the theme in use, so the screens cannot drift apart.
 *
 * Flat drawing only, with fills and the built-in fonts: the phone has no GPU.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "t48.h"

#define T (t48_theme_now())

static uint32_t filled;
static int cleared;
/* what t48_ui_clear last filled the screen with; the theme's own ground if it never has */
#define ground (cleared ? filled : T->bg)

void t48_ui_clear(t48_surface *s, uint32_t c)
{
    filled = c;
    cleared = 1;
    t48_fill(s, 0, 0, s->w, s->h, c);
}

uint32_t t48_ui_ground(void) { return ground; }

/* a small screen: the parts in the sizes that suit it (t48.h) */
static int small;
void t48_ui_small(int on) { small = on; }
int t48_ui_is_small(void) { return small; }
int t48_ui_head_h(void) { return small ? 30 : T48_UI_HEAD_H; }
int t48_ui_tag_h(void) { return small ? 26 : 34; }
#define HEAD_H (t48_ui_head_h())
#define TAG_H (t48_ui_tag_h())

void t48_ui_ground_is(uint32_t c)
{
    filled = c;
    cleared = 1;
}

/* a filled shape in the theme's manner: plain, outlined, or with a lip below it like a key cap */
static void shape(t48_surface *s, int x, int y, int w, int h, int r, uint32_t fill, int edged)
{
    if (r * 2 > h) r = h / 2;
    if (edged && T->edge == T48_EDGE_OUTLINE) {
        t48_round_fill(s, x, y, w, h, r, T->edge_colour);
        t48_round_fill(s, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : r, fill);
    } else if (edged && T->edge == T48_EDGE_LIP) {
        t48_round_fill(s, x, y + 3, w, h - 3, r, t48_mix(fill, 0x000000, 70));
        t48_round_fill(s, x, y, w, h - 3, r, fill);
    } else {
        t48_round_fill(s, x, y, w, h, r, fill);
    }
}

/* ---------- small marks, drawn from fills ---------- */

/* dir: -1 points left, 1 right, -2 up, 2 down; n is half its length, t its weight */
void t48_ui_chevron(t48_surface *s, int cx, int cy, int n, int dir, int t, uint32_t c)
{
    for (int j = -n; j <= n; j++) {
        int a = j < 0 ? -j : j;
        if (dir == -1) t48_fill(s, cx - n / 2 + a, cy + j, t, 1, c);
        else if (dir == 1) t48_fill(s, cx + n / 2 - a - t + 1, cy + j, t, 1, c);
        else if (dir == -2) t48_fill(s, cx + j, cy - n / 2 + a, 1, t, c);
        else t48_fill(s, cx + j, cy + n / 2 - a - t + 1, 1, t, c);
    }
}

/* rub out: an arrow pointing left */
static void mark_rubout(t48_surface *s, int cx, int cy, uint32_t c)
{
    t48_ui_chevron(s, cx - 8, cy, 8, -1, 4, c);
    t48_fill(s, cx - 10, cy - 1, 26, 3, c);
}

static void mark_plus(t48_surface *s, int cx, int cy, int n, int t, uint32_t c)
{
    t48_fill(s, cx - n, cy - t / 2, 2 * n, t, c);
    t48_fill(s, cx - t / 2, cy - n, t, 2 * n, c);
}

void t48_ui_lamp(t48_surface *s, int x, int y, uint32_t c)
{
    t48_round_fill(s, x, y, 12, 12, T->key_radius >= 100 || T->radius > 3 ? 6 : 2, c);
}

/* ---------- the header ---------- */

void t48_ui_header_ground(t48_surface *s)
{
    t48_fill(s, 0, 0, s->w, HEAD_H, T->head);
    if (T->head_rule) t48_fill(s, 0, HEAD_H - 2, s->w, 2, T->line);
}

static void header_clock(t48_surface *s)
{
    char hm[8];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(hm, sizeof hm, "%H:%M", &tm);
    t48_text_right(s, s->w - (small ? 10 : 24), (HEAD_H - t48_font_bold.height) / 2, &t48_font_bold, T->on_head_dim, hm);
}

/* a screen's name, the time, and a way back if there is one (a touch left of x 130) */
void t48_ui_header(t48_surface *s, const char *title, int back, int lamp)
{
    int x = small ? 10 : 24;
    const t48_font *f = small ? &t48_font_bold : &t48_font_head;
    uint32_t mark = lamp == T48_UI_GO ? T->good : lamp == T48_UI_STOP ? T->bad : T->accent;
    t48_ui_header_ground(s);
    if (back) {
        t48_ui_chevron(s, small ? 14 : 30, HEAD_H / 2 - 1, small ? 7 : 9, -1, small ? 3 : 4, T->on_head);
        x = small ? 30 : 56;
    } else if (lamp) {
        t48_ui_lamp(s, x, HEAD_H / 2 - 6, mark);
        x = small ? 30 : 48;
    }
    t48_text_fit(s, x, (HEAD_H - f->height) / 2, s->w - x - (small ? 70 : 110), f, T->on_head, title);
    header_clock(s);
}

/* ---------- buttons and keys ---------- */

static void role_colours(int role, uint32_t *fill, uint32_t *ink)
{
    switch (role) {
    case T48_UI_FIRST: *fill = T->first; *ink = T->on_first; break;
    case T48_UI_GO: *fill = T->good; *ink = T->on_good; break;
    case T48_UI_STOP: *fill = T->bad; *ink = T->on_bad; break;
    case T48_UI_LIT: *fill = T->accent; *ink = T->on_accent; break;
    case T48_UI_OFF:
        *fill = T->edge == T48_EDGE_LIP ? t48_mix(ground, T->key, 60) : T->edge == T48_EDGE_OUTLINE ? T->bg : T->panel;
        *ink = T->edge == T48_EDGE_LIP ? t48_mix(ground, T->key, 140) : T->faint;
        break;
    default:
        *fill = T->key;
        *ink = T->on_key;
    }
}

/* what a button says, centred on cx, cy: words, or a mark */
static void btn_label(t48_surface *s, int cx, int cy, const char *label, uint32_t ink)
{
    if (!strcmp(label, "<x")) mark_rubout(s, cx, cy, ink);
    else if (!strcmp(label, "+")) mark_plus(s, cx, cy, 8, 4, ink);
    else if (!strcmp(label, "-")) t48_fill(s, cx - 8, cy - 2, 16, 4, ink);
    else if (!strcmp(label, "^") || !strcmp(label, "v")) t48_ui_chevron(s, cx, cy, 9, label[0] == '^' ? -2 : 2, 4, ink);
    else t48_text_centre(s, cx, cy - t48_font_bold.height / 2, &t48_font_bold, ink, label);
}

/* A button. The label "<x" is the rub-out mark and "+" a plus. */
void t48_ui_btn(t48_surface *s, int x, int y, int w, int h, const char *label, int role)
{
    uint32_t fill, ink;
    role_colours(role, &fill, &ink);
    shape(s, x, y, w, h, T->radius, fill, role == T48_UI_PLAIN || role == T48_UI_OFF || T->edge == T48_EDGE_LIP);
    btn_label(s, x + w / 2, y + (h - (T->edge == T48_EDGE_LIP ? 3 : 0)) / 2, label, ink);
}

/* a key of the number pad */
void t48_ui_key(t48_surface *s, int x, int y, int w, int h, const char *label)
{
    uint32_t ink = T->on_key;
    int lip = T->edge == T48_EDGE_LIP ? 3 : 0;
    if (T->key_radius >= 100) {
        int d = (h < w ? h : w) - 2;
        shape(s, x + (w - d) / 2, y + (h - d) / 2, d, d, d / 2, T->key, 1);
    } else {
        shape(s, x, y, w, h, T->key_radius, T->key, 1);
    }
    t48_text_centre(s, x + w / 2, y + (h - lip - t48_font_big.height) / 2, &t48_font_big, ink, label);
}

/* ---------- panels, rows, tags ---------- */

/* a quiet box that holds something: the number being dialled */
void t48_ui_panel(t48_surface *s, int x, int y, int w, int h)
{
    if (T->edge == T48_EDGE_OUTLINE) shape(s, x, y, w, h, T->radius, T->panel, 1);
    else t48_round_fill(s, x, y, w, h, T->radius, T->edge == T48_EDGE_LIP ? t48_mix(ground, 0x000000, 60) : T->panel);
}

/* The ground of one row of a list, and the inks to write on it. */
void t48_ui_row(t48_surface *s, int x, int y, int w, int h, int selected, t48_ui_ink *ink)
{
    ink->text = T->text; ink->dim = T->dim; ink->faint = T->faint;
    if (selected) {
        t48_round_fill(s, x, y, w, h, T->rows == T48_ROWS_SOFT ? T->radius - 4 : T->rows == T48_ROWS_RULED ? 4 : T->radius, T->sel);
        ink->text = T->on_sel; ink->dim = ink->faint = T->on_sel_dim;
    } else if (T->rows == T48_ROWS_BLOCK) {
        t48_round_fill(s, x, y, w, h, T->radius, T->panel);
    } else if (T->rows == T48_ROWS_SOFT) {
        t48_round_fill(s, x, y, w, h, T->radius - 4, t48_mix(ground, 0xFFFFFF, 22));
    } else {
        t48_fill(s, x, y + h + 2, w, 1, T->line);
    }
}

/* a short word in a box: where the sound is, what is switched off. Returns the x after it. */
/* A state, not something to press: a lamp in the role's colour and the word, with no box
 * round it, as the header shows the network and the account. Plain is a quiet lamp. */
int t48_ui_tag(t48_surface *s, int x, int y, const char *text, int role)
{
    uint32_t lamp, ink;
    role_colours(role, &lamp, &ink);
    ink = role == T48_UI_OFF ? T->dim : T->text;
    if (role == T48_UI_PLAIN) lamp = T->dim;
    else if (role == T48_UI_OFF) lamp = T->faint;
    t48_ui_lamp(s, x, y + TAG_H / 2 - 6, lamp);
    t48_text(s, x + 20, y + (TAG_H - t48_font_bold.height) / 2, &t48_font_bold, ink, text);
    return x + t48_ui_tag_width(text) + (small ? 14 : 20);
}

int t48_ui_tag_width(const char *text) { return 20 + t48_text_width(&t48_font_bold, text); }

/* an On/Off switch, its right edge at rx */
void t48_ui_switch(t48_surface *s, int rx, int y, int h, int on, int selected)
{
    int w = small ? 48 : 64, sh = small ? 24 : 32, sy = y + (h - sh) / 2, x = rx - w;
    if (T->radius < 6) {
        /* a lamp and a word */
        uint32_t ink = selected ? T->on_sel : on ? T->text : T->dim;
        t48_round_fill(s, rx - 16, y + h / 2 - 8, 16, 16, 2, on ? T->good : selected ? T->on_sel_dim : T->line);
        t48_text_right(s, rx - 26, y + (h - t48_font_bold.height) / 2, &t48_font_bold, ink, on ? "On" : "Off");
        return;
    }
    uint32_t track = on ? T->good : selected ? t48_mix(T->sel, T->on_sel, 90) : T->edge == T48_EDGE_LIP ? t48_mix(ground, 0x000000, 90) : T->line;
    t48_round_fill(s, x, sy, w, sh, sh / 2, track);
    t48_round_fill(s, on ? x + w - sh + 3 : x + 3, sy + 3, sh - 6, sh - 6, (sh - 6) / 2, 0xFFFFFF);
}

/* a level from 0 to max, in a box w wide: minus, the bar, plus */
void t48_ui_level(t48_surface *s, int x, int y, int w, int h, int level, int max, int disabled, int selected)
{
    int bw = small ? 36 : 50, in = small ? 3 : 6, cell = (w - 2 * bw - 20) / (max ? max : 1), by = y + h / 2 - 8;
    uint32_t on = disabled ? T->line : selected ? T->on_sel : T->rows == T48_ROWS_RULED ? T->text : T->accent;
    uint32_t off = selected ? t48_mix(T->sel, T->on_sel, 60) : T->edge == T48_EDGE_LIP ? t48_mix(ground, 0x000000, 90) : T->line;
    t48_ui_btn(s, x, y + in, bw, h - 2 * in, "-", disabled ? T48_UI_OFF : T48_UI_PLAIN);
    t48_ui_btn(s, x + w - bw, y + in, bw, h - 2 * in, "+", disabled ? T48_UI_OFF : T48_UI_PLAIN);
    for (int i = 0; i < max; i++)
        t48_round_fill(s, x + bw + 10 + i * cell, by, cell - 3, 16, T->radius < 6 ? 0 : 3, i < level ? on : off);
}

/* ---------- a box over the screen: a question, a notice, a code ---------- */

/* tone: T48_UI_PLAIN, T48_UI_LIT (wants attention) or T48_UI_STOP (destroys something) */
void t48_ui_dialog(t48_surface *s, int x, int y, int w, int h, int tone)
{
    t48_fill_alpha(s, 0, 0, s->w, s->h, T->edge == T48_EDGE_OUTLINE ? 0x000000 : T->bg, T->edge == T48_EDGE_OUTLINE ? 120 : 190);
    t48_ui_popup(s, x, y, w, h, tone);
}

/* the same box with the screen left showing around it: the volume, a pairing code */
void t48_ui_popup(t48_surface *s, int x, int y, int w, int h, int tone)
{
    uint32_t mark = tone == T48_UI_STOP ? T->bad : tone == T48_UI_LIT ? T->accent : T->line;
    if (T->edge == T48_EDGE_NONE) {
        t48_round_fill(s, x - 2, y - 2, w + 4, h + 4, T->radius + 1, tone == T48_UI_PLAIN ? T->text : mark);
        t48_round_fill(s, x, y, w, h, T->radius, T->bg);
    } else if (T->edge == T48_EDGE_OUTLINE) {
        t48_round_fill(s, x - 1, y - 1, w + 2, h + 2, T->radius + 3, T->edge_colour);
        t48_round_fill(s, x, y, w, h, T->radius + 2, T->panel);
        t48_fill(s, x + T->radius + 2, y, w - 2 * T->radius - 4, 6, tone == T48_UI_PLAIN ? T->accent : mark);
    } else {
        t48_round_fill(s, x, y, w, h, T->radius, T->key);
        if (tone != T48_UI_PLAIN) t48_round_fill(s, x + w / 2 - 30, y + 12, 60, 6, 3, mark);
    }
}

/* the inks to use inside ui_dialog */
void t48_ui_dialog_ink(t48_ui_ink *ink)
{
    if (T->edge == T48_EDGE_LIP) { ink->text = T->on_key; ink->dim = ink->faint = T->on_key_dim; }
    else { ink->text = T->text; ink->dim = T->dim; ink->faint = T->faint; }
}

/* a button inside ui_dialog: on the ivory of Two-tone a plain button has to be the dark one */
void t48_ui_dialog_btn(t48_surface *s, int x, int y, int w, int h, const char *label, int role)
{
    if (T->edge == T48_EDGE_LIP && role == T48_UI_PLAIN) {
        t48_round_fill(s, x, y, w, h, T->radius, t48_mix(T->key, 0x000000, 40));
        t48_text_centre(s, x + w / 2, y + (h - t48_font_bold.height) / 2, &t48_font_bold, T->on_key, label);
    } else if (T->edge == T48_EDGE_LIP && role == T48_UI_FIRST) {
        t48_round_fill(s, x, y, w, h, T->radius, T->bg);
        t48_text_centre(s, x + w / 2, y + (h - t48_font_bold.height) / 2, &t48_font_bold, T->text, label);
    } else {
        t48_ui_btn(s, x, y, w, h, label, role);
    }
}

/* notices in the small face: a small screen with more to say than a row of the usual face holds */
static int notice_tight;
void t48_ui_notice_tight(int on) { notice_tight = on; }
#define NOTICE_FONT (notice_tight ? &t48_font_small : &t48_font_bold)

/* a notice on the idle screen: a wide tag. Returns the x after it. */
int t48_ui_notice(t48_surface *s, int x, int y, const char *text, int role)
{
    uint32_t fill, ink;
    int w = t48_ui_notice_width(text);
    role_colours(role, &fill, &ink);
    int h = small ? 30 : 42;
    t48_round_fill(s, x, y, w, h, T->radius < 6 ? T->radius : h / 2, fill);
    t48_text_centre(s, x + w / 2, y + (h - NOTICE_FONT->height) / 2, NOTICE_FONT, ink, text);
    return x + w + (small ? 8 : 12);
}

int t48_ui_notice_width(const char *text) { return t48_text_width(NOTICE_FONT, text) + (small ? 24 : 44); }

/* a tile of the launcher */
void t48_ui_tile(t48_surface *s, int x, int y, int w, int h, int selected, t48_ui_ink *ink)
{
    ink->text = T->text; ink->dim = T->dim; ink->faint = T->faint;
    /* the chosen one has a frame: its picture may be any colour, so the tile's own must not change under it */
    if (selected) t48_round_fill(s, x - 4, y - 4, w + 8, h + 8, T->radius + 4, T->sel);
    if (T->edge == T48_EDGE_OUTLINE && !selected) shape(s, x, y, w, h, T->radius, T->panel, 1);
    else t48_round_fill(s, x, y, w, h, T->radius, T->rows == T48_ROWS_SOFT ? t48_mix(ground, 0xFFFFFF, 22) : T->panel);
}

/* the square an app's picture sits in, when it has none of its own: its first letter, or a plus */
void t48_ui_icon(t48_surface *s, int x, int y, int n, const char *letter)
{
    uint32_t fill = T->edge == T48_EDGE_LIP ? T->bg : T->line, ink = T->text;
    t48_round_fill(s, x, y, n, n, T->key_radius >= 100 ? n / 2 : T->radius * 2, fill);
    if (!strcmp(letter, "+")) mark_plus(s, x + n / 2, y + n / 2, 20, 6, ink);
    else t48_text_centre(s, x + n / 2, y + (n - t48_font_big.height) / 2, &t48_font_big, ink, letter);
}

/* ---------- the soft keys ---------- */

/* Each label is a tab that runs off the bottom edge, so it reads as belonging to
 * the key under it. A key that does nothing has none. */
void t48_ui_softkeys(t48_surface *s, const char *const label[4], const int role[4])
{
    int y = s->h - T48_UI_BAR_H, cw = s->w / 4;
    for (int i = 0; i < 4; i++) {
        uint32_t fill, ink;
        int r = role ? role[i] : T48_UI_PLAIN, x = i * cw + 2, w = cw - 4;
        if (!label[i] || !label[i][0]) continue;
        role_colours(r, &fill, &ink);
        shape(s, x, y, w, T48_UI_BAR_H + T->radius + 6, T->radius, fill, r == T48_UI_PLAIN || r == T48_UI_OFF);
        int cy = y + T48_UI_BAR_H / 2 + 1;
        if (t48_text_width(&t48_font_bold, label[i]) <= w - 6) btn_label(s, x + w / 2, cy, label[i], ink);
        else if (t48_text_width(&t48_font_small, label[i]) <= w - 6) t48_text_centre(s, x + w / 2, cy - t48_font_small.height / 2, &t48_font_small, ink, label[i]);
        else t48_text_fit(s, x + 3, cy - t48_font_small.height / 2, w - 6, &t48_font_small, ink, label[i]);
    }
}
