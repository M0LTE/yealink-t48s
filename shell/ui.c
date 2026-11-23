// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the look (docs/design/README.md, docs/style.md). The palette, and
 * the parts every screen is made of: the header, buttons, keypad keys, list
 * rows, switches, level bars, lamps, tags, panels and the box of a question.
 * A screen says what a thing is (a button that starts a call, a selected row)
 * and this file decides what that looks like, so the screens cannot drift
 * apart.
 *
 * Flat drawing only, with libt48's fills and fonts: the phone has no GPU.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "shell.h"

#ifndef T48_THEME
#define T48_THEME 0
#endif

static const struct theme themes[] = {
    {   /* 0: Switchboard. Black to the bezel, white type, the selection in inverse, colour only in lamps. */
        .name = "switchboard",
        .bg = 0x000000, .panel = 0x171715, .key = 0x24241F, .line = 0x3A3A35,
        .text = 0xF4F3EE, .dim = 0xA3A299, .faint = 0x66655E,
        .accent = 0xFFB000, .on_accent = 0x000000,
        .good = 0x35D07F, .on_good = 0x000000, .bad = 0xFF5A47, .on_bad = 0x000000,
        .sel = 0xF4F3EE, .on_sel = 0x000000, .on_sel_dim = 0x55544E,
        .head = 0x000000, .on_head = 0xF4F3EE, .on_head_dim = 0xA3A299,
        .first = 0xF4F3EE, .on_first = 0x000000,
        .ground = {0x000000, 0x000000, 0x000000},
        .r = 3, .r_key = 3, .edge = EDGE_NONE, .rows = ROWS_BLOCK, .head_rule = 1,
    },
    {   /* 1: Directory. Paper, ink, and one telephone yellow: the header, and the line you have your finger on. */
        .name = "directory",
        .bg = 0xF7F7F4, .panel = 0xFFFFFF, .key = 0xFFFFFF, .line = 0xCFD0CA,
        .text = 0x151514, .dim = 0x5C5D58, .faint = 0x9B9C96,
        .accent = 0xFFCE00, .on_accent = 0x151514,
        .good = 0x0B8F4C, .on_good = 0xFFFFFF, .bad = 0xD63A22, .on_bad = 0xFFFFFF,
        .sel = 0xFFCE00, .on_sel = 0x151514, .on_sel_dim = 0x5A4A00,
        .head = 0xFFCE00, .on_head = 0x151514, .on_head_dim = 0x5A4A00,
        .first = 0x151514, .on_first = 0xFFFFFF,
        .ground = {0xF7F7F4, 0xF7F7F4, 0xF7F7F4},
        .r = 6, .r_key = 6, .edge = EDGE_OUTLINE, .edge_c = 0xB9BAB4, .rows = ROWS_RULED,
    },
    {   /* 2: Two-tone. The coloured body and ivory keys of a 1960s telephone; the ground itself changes with a call. */
        .name = "twotone",
        .bg = 0x1C3557, .panel = 0x28466F, .key = 0xEFE8D8, .line = 0x3F5F8B,
        .text = 0xF3EEE2, .dim = 0xB1BFD2, .faint = 0x7C93B1,
        .accent = 0xFFC94D, .on_accent = 0x1B2333,
        .good = 0x46CF86, .on_good = 0x0B2A1A, .bad = 0xF2664F, .on_bad = 0x2B0B06,
        .sel = 0xEFE8D8, .on_sel = 0x1B2A40, .on_sel_dim = 0x5C6A7D,
        .head = 0x1C3557, .on_head = 0xF3EEE2, .on_head_dim = 0xB1BFD2,
        .first = 0xFFC94D, .on_first = 0x1B2333,
        .ground = {0x0F5A40, 0x1C3557, 0x6A4A14},
        .r = 16, .r_key = 100, .edge = EDGE_LIP, .edge_c = 0xBDB49E, .rows = ROWS_SOFT,
    },
};
const struct theme *T = &themes[T48_THEME];

static uint32_t ground;   /* what ui_clear last filled the screen with */

void ui_init(void)
{
    /* the keyboard is libt48's, so that apps have it too: dress it as the shell is dressed */
    t48_kbd_style = (t48_kbd_look){
        .bg = T->bg, .field = T->panel, .key = T->edge == EDGE_LIP ? T->key : T->rows == ROWS_RULED ? T->key : T->key,
        .side = T->edge == EDGE_LIP ? t48_mix(T->key, T->bg, 60) : T->rows == ROWS_RULED ? t48_mix(T->key, T->line, 110) : T->panel,
        .text = T->edge == EDGE_NONE ? T->text : T->edge == EDGE_LIP ? T->on_sel : T->text,
        .dim = T->edge == EDGE_LIP ? T->on_sel_dim : T->dim, .label = T->dim, .field_text = T->text,
        .accent = T->first, .on_accent = T->on_first, .cursor = T->edge == EDGE_OUTLINE ? T->text : T->accent,
        .edge = T->edge_c, .edge_kind = T->edge, .radius = T->r < 8 ? T->r : 10,
    };
    ground = T->bg;
}

void ui_clear(uint32_t c)
{
    ground = c;
    t48_fill(&ui, 0, 0, SW, SH, c);
}

uint32_t ui_ground(void) { return ground; }

/* a filled shape in the theme's manner: plain, outlined, or with a lip below it like a key cap */
static void shape(int x, int y, int w, int h, int r, uint32_t fill, int edged)
{
    if (r * 2 > h) r = h / 2;
    if (edged && T->edge == EDGE_OUTLINE) {
        t48_round_fill(&ui, x, y, w, h, r, T->edge_c);
        t48_round_fill(&ui, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : r, fill);
    } else if (edged && T->edge == EDGE_LIP) {
        t48_round_fill(&ui, x, y + 3, w, h - 3, r, t48_mix(fill, 0x000000, 70));
        t48_round_fill(&ui, x, y, w, h - 3, r, fill);
    } else {
        t48_round_fill(&ui, x, y, w, h, r, fill);
    }
}

/* ---------- small marks, drawn from fills ---------- */

/* dir: -1 points left, 1 right, -2 up, 2 down; s is half its length, t its weight */
void ui_chevron(int cx, int cy, int s, int dir, int t, uint32_t c)
{
    for (int j = -s; j <= s; j++) {
        int a = j < 0 ? -j : j;
        if (dir == -1) t48_fill(&ui, cx - s / 2 + a, cy + j, t, 1, c);
        else if (dir == 1) t48_fill(&ui, cx + s / 2 - a - t + 1, cy + j, t, 1, c);
        else if (dir == -2) t48_fill(&ui, cx + j, cy - s / 2 + a, 1, t, c);
        else t48_fill(&ui, cx + j, cy + s / 2 - a - t + 1, 1, t, c);
    }
}

/* rub out: an arrow pointing left */
static void mark_rubout(int cx, int cy, uint32_t c)
{
    ui_chevron(cx - 8, cy, 8, -1, 4, c);
    t48_fill(&ui, cx - 10, cy - 1, 26, 3, c);
}

static void mark_plus(int cx, int cy, int s, int t, uint32_t c)
{
    t48_fill(&ui, cx - s, cy - t / 2, 2 * s, t, c);
    t48_fill(&ui, cx - t / 2, cy - s, t, 2 * s, c);
}

void ui_lamp(int x, int y, uint32_t c)
{
    t48_round_fill(&ui, x, y, 12, 12, T->r_key >= 100 || T->r > 3 ? 6 : 2, c);
}

/* ---------- the header ---------- */

static void header_ground(void)
{
    t48_fill(&ui, 0, 0, SW, HEAD_H, T->head);
    if (T->head_rule) t48_fill(&ui, 0, HEAD_H - 2, SW, 2, T->line);
}

static void header_clock(void)
{
    char hm[8];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(hm, sizeof hm, "%H:%M", &tm);
    t48_text_right(&ui, SW - 24, (HEAD_H - t48_font_bold.height) / 2, &t48_font_bold, T->on_head_dim, hm);
}

/* a screen's name, the time, and a way back if there is one (a touch left of x 130) */
void ui_header(const char *title, int back, int lamp)
{
    int x = 24;
    header_ground();
    if (back) {
        ui_chevron(30, HEAD_H / 2 - 1, 9, -1, 4, T->on_head);
        x = 56;
    } else if (lamp) {
        ui_lamp(24, HEAD_H / 2 - 6, lamp == B_GO ? T->good : lamp == B_STOP ? T->bad : T->accent);
        x = 48;
    }
    t48_text_fit(&ui, x, (HEAD_H - t48_font_head.height) / 2, SW - x - 110, &t48_font_head, T->on_head, title);
    header_clock();
}

/* the idle screen's: the network on the left, the default account on the right, a lamp for each */
void ui_header_idle(void)
{
    char net[80], acc[120];
    uint32_t acc_colour;
    int up, y = (HEAD_H - t48_font_normal.height) / 2;

    header_ground();
    ui_network_status(net, sizeof net, &up);
    ui_lamp(24, HEAD_H / 2 - 6, up ? C_GOOD : C_BAD);
    t48_text(&ui, 46, y, &t48_font_normal, T->on_head, net);
    phone_account_text(acc, sizeof acc, &acc_colour);
    int w = t48_text_width(&t48_font_normal, acc);
    t48_text_right(&ui, SW - 24, y, &t48_font_normal, T->on_head, acc);
    ui_lamp(SW - 24 - w - 22, HEAD_H / 2 - 6, acc_colour);
}

/* ---------- buttons and keys ---------- */

static void role_colours(int role, uint32_t *fill, uint32_t *ink)
{
    switch (role) {
    case B_FIRST: *fill = T->first; *ink = T->on_first; break;
    case B_GO: *fill = T->good; *ink = T->on_good; break;
    case B_STOP: *fill = T->bad; *ink = T->on_bad; break;
    case B_LIT: *fill = T->accent; *ink = T->on_accent; break;
    case B_OFF:
        *fill = T->edge == EDGE_LIP ? t48_mix(ground, T->key, 60) : T->edge == EDGE_OUTLINE ? T->bg : T->panel;
        *ink = T->edge == EDGE_LIP ? t48_mix(ground, T->key, 140) : T->faint;
        break;
    default:
        *fill = T->key;
        *ink = T->edge == EDGE_LIP ? T->on_sel : T->text;
    }
}

/* A button. The label "<x" is the rub-out mark and "+" a plus. */
void ui_btn(int x, int y, int w, int h, const char *label, int role)
{
    uint32_t fill, ink;
    role_colours(role, &fill, &ink);
    shape(x, y, w, h, T->r, fill, role == B_PLAIN || role == B_OFF || T->edge == EDGE_LIP);
    int cy = y + (h - (T->edge == EDGE_LIP ? 3 : 0)) / 2;
    if (!strcmp(label, "<x")) mark_rubout(x + w / 2, cy, ink);
    else if (!strcmp(label, "+")) mark_plus(x + w / 2, cy, 8, 4, ink);
    else if (!strcmp(label, "-")) t48_fill(&ui, x + w / 2 - 8, cy - 2, 16, 4, ink);
    else if (!strcmp(label, "^") || !strcmp(label, "v")) ui_chevron(x + w / 2, cy, 9, label[0] == '^' ? -2 : 2, 4, ink);
    else t48_text_centre(&ui, x + w / 2, cy - t48_font_bold.height / 2, &t48_font_bold, ink, label);
}

/* a key of the number pad */
void ui_key(int x, int y, int w, int h, const char *label)
{
    uint32_t ink = T->edge == EDGE_LIP ? T->on_sel : T->text;
    int lip = T->edge == EDGE_LIP ? 3 : 0;
    if (T->r_key >= 100) {
        int d = (h < w ? h : w) - 2;
        shape(x + (w - d) / 2, y + (h - d) / 2, d, d, d / 2, T->key, 1);
    } else {
        shape(x, y, w, h, T->r_key, T->key, 1);
    }
    t48_text_centre(&ui, x + w / 2, y + (h - lip - t48_font_big.height) / 2, &t48_font_big, ink, label);
}

/* ---------- panels, rows, tags ---------- */

/* a quiet box that holds something: the number being dialled */
void ui_panel(int x, int y, int w, int h)
{
    if (T->edge == EDGE_OUTLINE) shape(x, y, w, h, T->r, T->panel, 1);
    else t48_round_fill(&ui, x, y, w, h, T->r, T->edge == EDGE_LIP ? t48_mix(ground, 0x000000, 60) : T->panel);
}

/* The ground of one row of a list, and the inks to write on it. */
void ui_row(int x, int y, int w, int h, int selected, struct row_ink *ink)
{
    ink->text = T->text; ink->dim = T->dim; ink->faint = T->faint;
    if (selected) {
        t48_round_fill(&ui, x, y, w, h, T->rows == ROWS_SOFT ? T->r - 4 : T->rows == ROWS_RULED ? 4 : T->r, T->sel);
        ink->text = T->on_sel; ink->dim = ink->faint = T->on_sel_dim;
    } else if (T->rows == ROWS_BLOCK) {
        t48_round_fill(&ui, x, y, w, h, T->r, T->panel);
    } else if (T->rows == ROWS_SOFT) {
        t48_round_fill(&ui, x, y, w, h, T->r - 4, t48_mix(ground, 0xFFFFFF, 22));
    } else {
        t48_fill(&ui, x, y + h + 2, w, 1, T->line);
    }
}

/* a short word in a box: where the sound is, what is switched off. Returns the x after it. */
int ui_tag(int x, int y, const char *text, int role)
{
    uint32_t fill, ink;
    int w = t48_text_width(&t48_font_bold, text) + 28;
    if (role == B_PLAIN || role == B_OFF) {
        fill = T->edge == EDGE_LIP ? t48_mix(ground, 0x000000, 60) : T->edge == EDGE_OUTLINE ? T->panel : T->panel;
        ink = role == B_OFF ? T->dim : T->text;
        if (T->edge == EDGE_OUTLINE) { t48_round_fill(&ui, x, y, w, TAG_H, TAG_H / 2, T->edge_c); t48_round_fill(&ui, x + 1, y + 1, w - 2, TAG_H - 2, TAG_H / 2 - 1, fill); }
        else t48_round_fill(&ui, x, y, w, TAG_H, T->r < 6 ? T->r : TAG_H / 2, fill);
    } else {
        role_colours(role, &fill, &ink);
        t48_round_fill(&ui, x, y, w, TAG_H, T->r < 6 ? T->r : TAG_H / 2, fill);
    }
    t48_text(&ui, x + 14, y + (TAG_H - t48_font_bold.height) / 2, &t48_font_bold, ink, text);
    return x + w + 8;
}

int ui_tag_width(const char *text) { return t48_text_width(&t48_font_bold, text) + 28; }

/* an On/Off switch, its right edge at rx */
void ui_switch(int rx, int y, int h, int on, int selected)
{
    int w = 64, sh = 32, sy = y + (h - sh) / 2, x = rx - w;
    if (T->r < 6) {
        /* a lamp and a word */
        uint32_t ink = selected ? T->on_sel : on ? T->text : T->dim;
        t48_round_fill(&ui, rx - 16, y + h / 2 - 8, 16, 16, 2, on ? T->good : selected ? T->on_sel_dim : T->line);
        t48_text_right(&ui, rx - 26, y + (h - t48_font_bold.height) / 2, &t48_font_bold, ink, on ? "On" : "Off");
        return;
    }
    uint32_t track = on ? T->good : selected ? t48_mix(T->sel, T->on_sel, 90) : T->edge == EDGE_LIP ? t48_mix(ground, 0x000000, 90) : T->line;
    t48_round_fill(&ui, x, sy, w, sh, sh / 2, track);
    t48_round_fill(&ui, on ? x + w - sh + 3 : x + 3, sy + 3, sh - 6, sh - 6, (sh - 6) / 2, 0xFFFFFF);
}

/* a level from 0 to max, in a box w wide: minus, the bar, plus */
void ui_level(int x, int y, int w, int h, int level, int max, int disabled, int selected)
{
    int cell = (w - 120) / (max ? max : 1), by = y + h / 2 - 8;
    uint32_t on = disabled ? T->line : selected && T->sel == T->accent ? T->on_sel : selected ? T->on_sel : T->rows == ROWS_RULED ? T->text : T->accent;
    uint32_t off = selected ? t48_mix(T->sel, T->on_sel, 60) : T->edge == EDGE_LIP ? t48_mix(ground, 0x000000, 90) : T->line;
    ui_btn(x, y + 6, 50, h - 12, "-", disabled ? B_OFF : B_PLAIN);
    ui_btn(x + w - 50, y + 6, 50, h - 12, "+", disabled ? B_OFF : B_PLAIN);
    for (int i = 0; i < max; i++)
        t48_round_fill(&ui, x + 60 + i * cell, by, cell - 3, 16, T->r < 6 ? 0 : 3, i < level ? on : off);
}

/* ---------- a box over the screen: a question, a notice, a code ---------- */

/* tone: B_PLAIN, B_LIT (wants attention) or B_STOP (destroys something) */
void ui_dialog(int x, int y, int w, int h, int tone)
{
    t48_fill_alpha(&ui, 0, 0, SW, SH, T->edge == EDGE_OUTLINE ? 0x000000 : T->bg, T->edge == EDGE_OUTLINE ? 120 : 190);
    ui_popup(x, y, w, h, tone);
}

/* the same box with the screen left showing around it: the volume, a pairing code */
void ui_popup(int x, int y, int w, int h, int tone)
{
    uint32_t mark = tone == B_STOP ? T->bad : tone == B_LIT ? T->accent : T->line;
    if (T->edge == EDGE_NONE) {
        t48_round_fill(&ui, x - 2, y - 2, w + 4, h + 4, T->r + 1, tone == B_PLAIN ? T->text : mark);
        t48_round_fill(&ui, x, y, w, h, T->r, T->bg);
    } else if (T->edge == EDGE_OUTLINE) {
        t48_round_fill(&ui, x - 1, y - 1, w + 2, h + 2, T->r + 3, T->edge_c);
        t48_round_fill(&ui, x, y, w, h, T->r + 2, T->panel);
        t48_fill(&ui, x + T->r + 2, y, w - 2 * T->r - 4, 6, tone == B_PLAIN ? T->accent : mark);
    } else {
        t48_round_fill(&ui, x, y, w, h, T->r, T->key);
        if (tone != B_PLAIN) t48_round_fill(&ui, x + w / 2 - 30, y + 12, 60, 6, 3, mark);
    }
}

/* the inks to use inside ui_dialog */
void ui_dialog_ink(struct row_ink *ink)
{
    if (T->edge == EDGE_LIP) { ink->text = T->on_sel; ink->dim = ink->faint = T->on_sel_dim; }
    else { ink->text = T->text; ink->dim = T->dim; ink->faint = T->faint; }
}

/* a button inside ui_dialog: on the ivory of Two-tone a plain button has to be the dark one */
void ui_dialog_btn(int x, int y, int w, int h, const char *label, int role)
{
    if (T->edge == EDGE_LIP && role == B_PLAIN) {
        t48_round_fill(&ui, x, y, w, h, T->r, t48_mix(T->key, 0x000000, 40));
        t48_text_centre(&ui, x + w / 2, y + (h - t48_font_bold.height) / 2, &t48_font_bold, T->on_sel, label);
    } else if (T->edge == EDGE_LIP && role == B_FIRST) {
        t48_round_fill(&ui, x, y, w, h, T->r, T->bg);
        t48_text_centre(&ui, x + w / 2, y + (h - t48_font_bold.height) / 2, &t48_font_bold, T->text, label);
    } else {
        ui_btn(x, y, w, h, label, role);
    }
}

/* a notice on the idle screen: a wide tag. Returns the x after it. */
int ui_notice(int x, int y, const char *text, int role)
{
    uint32_t fill, ink;
    int w = ui_notice_width(text);
    role_colours(role, &fill, &ink);
    t48_round_fill(&ui, x, y, w, 42, T->r < 6 ? T->r : 21, fill);
    t48_text_centre(&ui, x + w / 2, y + (42 - t48_font_bold.height) / 2, &t48_font_bold, ink, text);
    return x + w + 12;
}

int ui_notice_width(const char *text) { return t48_text_width(&t48_font_bold, text) + 44; }

/* a tile of the launcher */
void ui_tile(int x, int y, int w, int h, int selected, struct row_ink *ink)
{
    ink->text = T->text; ink->dim = T->dim; ink->faint = T->faint;
    /* the chosen one has a frame: its picture may be any colour, so the tile's own must not change under it */
    if (selected) t48_round_fill(&ui, x - 4, y - 4, w + 8, h + 8, T->r + 4, T->sel);
    if (T->edge == EDGE_OUTLINE && !selected) shape(x, y, w, h, T->r, T->panel, 1);
    else t48_round_fill(&ui, x, y, w, h, T->r, T->rows == ROWS_SOFT ? t48_mix(ground, 0xFFFFFF, 22) : T->panel);
}

/* the square an app's picture sits in, when it has none of its own: its first letter, or a plus */
void ui_icon(int x, int y, int s, const char *letter)
{
    uint32_t fill = T->edge == EDGE_LIP ? T->bg : T->line, ink = T->text;
    t48_round_fill(&ui, x, y, s, s, T->r_key >= 100 ? s / 2 : T->r * 2, fill);
    if (!strcmp(letter, "+")) mark_plus(x + s / 2, y + s / 2, 20, 6, ink);
    else t48_text_centre(&ui, x + s / 2, y + (s - t48_font_big.height) / 2, &t48_font_big, ink, letter);
}
