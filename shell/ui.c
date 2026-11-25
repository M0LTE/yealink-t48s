// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the look (docs/design/README.md, docs/style.md). The themes and
 * the parts screens are made of are libt48's, so that apps have them too
 * (libt48/src/theme.c, ui.c). Here: which theme is in use (the `theme` key of
 * the settings file `screen`: Settings, Screen, Theme, or the web setup
 * page), changing it while everything runs, the font file apps are given, the
 * idle screen's header, and a small picture of a theme for choosing one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell.h"

static const t48_font_family *const families[] = {&t48_fonts_b612, &t48_fonts_plex, &t48_fonts_figtree};
#define NFAMILIES ((int)(sizeof families / sizeof families[0]))

const t48_font_family *ui_fonts(const char *name)
{
    for (int i = 0; i < NFAMILIES; i++)
        if (!strcmp(families[i]->name, name)) return families[i];
    return families[0];
}

/* a theme's name as the owner reads it */
const char *ui_theme_label(const char *name)
{
    static const char *const labels[][2] = {{"switchboard", "Switchboard"}, {"directory", "Directory"}, {"twotone", "Two-tone"}};
    for (unsigned i = 0; i < sizeof labels / sizeof labels[0]; i++)
        if (!strcmp(labels[i][0], name)) return labels[i][1];
    return name;
}

int ui_theme_set(const char *name)
{
    const t48_theme *t = t48_theme_named(name);
    if (!t) return -1;
    if (!strcmp(T->name, t->name)) return 0;
    t48_theme_use(t, ui_fonts(t->font));
    logmsg("theme: %s", t->name);
    app_theme();
    return 0;
}

/* the theme the store names; Switchboard if it names none, or one we have not got */
static const char *stored(void)
{
    const char *name = cfg_str("screen", "theme", t48_themes[0].name);
    return t48_theme_named(name) ? name : t48_themes[0].name;
}

int ui_theme_reload(void)
{
    const char *name = stored();
    if (!strcmp(T->name, name)) return 0;
    ui_theme_set(name);
    return 1;
}

void ui_init(void)
{
    const t48_theme *t = t48_theme_named(stored());
    t48_theme_use(t, ui_fonts(t->font));
}

/* ---------- the font file for apps ---------- */

/* how much coverage a face has: to the end of its last glyph */
static size_t coverage(const t48_font *f)
{
    size_t n = 0;
    for (int c = 0; c < 95; c++) {
        size_t end = f->g[c].off + (size_t)f->g[c].w * (size_t)f->g[c].h;
        if (end > n) n = end;
    }
    return n;
}

/* Every family, as struct t48_fonts_hdr describes (libt48/include/t48_proto.h).
 * Tables that two families share (the fixed-width faces) are there once. */
unsigned char *ui_fonts_file(size_t *len)
{
    struct { const void *p; uint32_t at; } seen[NFAMILIES * T48_FACES * 2];
    int nseen = 0;
    size_t cap = sizeof(struct t48_fonts_hdr) + NFAMILIES * sizeof(struct t48_fonts_family), n = cap;

    for (int i = 0; i < NFAMILIES; i++)
        for (int k = 0; k < T48_FACES; k++) cap += 95 * sizeof(t48_glyph) + coverage(&families[i]->face[k]) + 8;
    unsigned char *b = calloc(1, cap);
    if (!b) return NULL;
    struct t48_fonts_hdr h = {.version = 1, .families = NFAMILIES};
    memcpy(h.magic, T48_FONTS_MAGIC, 4);
    memcpy(b, &h, sizeof h);
    for (int i = 0; i < NFAMILIES; i++) {
        struct t48_fonts_family f;
        memset(&f, 0, sizeof f);
        snprintf(f.name, sizeof f.name, "%s", families[i]->name);
        for (int k = 0; k < T48_FACES; k++) {
            const t48_font *face = &families[i]->face[k];
            const void *part[2] = {face->g, face->a};
            size_t size[2] = {95 * sizeof(t48_glyph), coverage(face)};
            uint32_t at[2];
            for (int j = 0; j < 2; j++) {
                int k2 = 0;
                while (k2 < nseen && seen[k2].p != part[j]) k2++;
                if (k2 == nseen) {
                    seen[nseen].p = part[j];
                    seen[nseen++].at = (uint32_t)n;
                    memcpy(b + n, part[j], size[j]);
                    n += (size[j] + 3) & ~(size_t)3;
                }
                at[j] = seen[k2].at;
            }
            f.face[k].ascent = face->ascent;
            f.face[k].height = face->height;
            f.face[k].glyphs = at[0];
            f.face[k].coverage = at[1];
        }
        memcpy(b + sizeof h + (size_t)i * sizeof f, &f, sizeof f);
    }
    *len = n;
    return b;
}

/* ---------- the idle screen's header ---------- */

/* the network on the left, the default account on the right, a lamp for each */
void ui_header_idle(void)
{
    char net[80], acc[120];
    uint32_t acc_colour;
    int up, y = (HEAD_H - t48_font_normal.height) / 2, m = SMALL ? 10 : 24;

    t48_ui_header_ground(&ui);
    ui_network_status(net, sizeof net, &up);
    ui_lamp(m, HEAD_H / 2 - 6, up ? C_GOOD : C_BAD);
    int nx = t48_text(&ui, m + 22, y, &t48_font_normal, T->on_head, net);
    phone_account_text(acc, sizeof acc, &acc_colour);
    int w = t48_text_width(&t48_font_normal, acc);
    if (SMALL && w > SW - m - 22 - nx - 16) {
        /* a small screen: the account gives way to the address */
        w = SW - m - 22 - nx - 16;
        t48_text_fit(&ui, SW - m - w, y, w, &t48_font_normal, T->on_head, acc);
    } else {
        t48_text_right(&ui, SW - m, y, &t48_font_normal, T->on_head, acc);
    }
    ui_lamp(SW - m - w - 22, HEAD_H / 2 - 6, acc_colour);
}

/* ---------- words over two lines ---------- */

void ui_two_lines(int x, int y, int w, const t48_font *f, uint32_t c, const char *text)
{
    char line[220];
    size_t n = strlen(text), cut = n;
    snprintf(line, sizeof line, "%s", text);
    while (cut > 0 && (line[cut] = 0, t48_text_width(f, line) > w)) {
        do cut--; while (cut > 0 && text[cut] != ' ');
    }
    if (cut == 0) { t48_text_fit(&ui, x, y + f->height / 2, w, f, c, text); return; }
    if (cut == n) { t48_text(&ui, x, y + f->height / 2 + 1, f, c, line); return; }
    t48_text(&ui, x, y, f, c, line);
    t48_text_fit(&ui, x, y + f->height + 2, w, f, c, text + cut + 1);
}

void ui_two_lines_centred(int cx, int y, int w, const t48_font *f, uint32_t c, const char *text)
{
    char line[220];
    size_t n = strlen(text), cut = n;
    snprintf(line, sizeof line, "%s", text);
    while (cut > 0 && (line[cut] = 0, t48_text_width(f, line) > w)) {
        do cut--; while (cut > 0 && text[cut] != ' ');
    }
    if (cut == 0 || cut == n) { t48_text_centre(&ui, cx, y + f->height / 2, f, c, cut ? line : text); return; }
    t48_text_centre(&ui, cx, y, f, c, line);
    if (t48_text_width(f, text + cut + 1) > w) t48_text_fit(&ui, cx - w / 2, y + f->height + 2, w, f, c, text + cut + 1);
    else t48_text_centre(&ui, cx, y + f->height + 2, f, c, text + cut + 1);
}

/* ---------- a theme, small ---------- */

/* What the phone looks like in theme t, in a box: its header, its figures, a
 * list with a row chosen, a key and the button that starts a call. Drawn with
 * the real parts, by being in that theme for as long as it takes. */
void ui_theme_preview(const t48_theme *t, int x, int y, int w, int h, int current)
{
    t48_theme was = *T;
    uint32_t ground = ui_ground();
    t48_surface box = {ui.px + y * ui.stride + x, w, h, ui.stride};
    t48_ui_ink ink;

    t48_theme_use(t, ui_fonts(t->font));
    t48_ui_clear(&box, t->bg);
    if (SMALL) {
        /* the same parts, closer together */
        int ty = (HEAD_H - t48_font_bold.height) / 2;
        t48_ui_header_ground(&box);
        if (current) t48_ui_lamp(&box, 8, HEAD_H / 2 - 6, t->good);
        t48_text(&box, current ? 26 : 8, ty, &t48_font_bold, t->on_head, "Calls");
        t48_text_right(&box, w - 8, ty, &t48_font_bold, t->on_head_dim, "16:25");
        t48_ui_row(&box, 6, 36, w - 12, 28, 0, &ink);
        t48_text(&box, 14, 36 + (28 - t48_font_normal.height) / 2, &t48_font_normal, ink.text, "Alice");
        t48_ui_row(&box, 6, 68, w - 12, 28, 1, &ink);
        t48_text(&box, 14, 68 + (28 - t48_font_normal.height) / 2, &t48_font_normal, ink.text, "Bob");
        t48_ui_btn(&box, 6, 104, w - 12, h - 110, "Call", T48_UI_GO);
        t48_rect(&box, 0, 0, w, h, t->line);
        t48_theme_use(&was, ui_fonts(was.font));
        t48_ui_ground_is(ground);
        return;
    }
    t48_ui_header_ground(&box);
    if (current) t48_ui_lamp(&box, 14, HEAD_H / 2 - 6, t->good);
    t48_text(&box, current ? 36 : 14, (HEAD_H - t48_font_bold.height) / 2, &t48_font_bold, t->on_head, "Calls");
    t48_text_right(&box, w - 14, (HEAD_H - t48_font_bold.height) / 2, &t48_font_bold, t->on_head_dim, "16:25");
    t48_ui_row(&box, 10, 58, w - 20, 38, 0, &ink);
    t48_text(&box, 22, 58 + (38 - t48_font_normal.height) / 2, &t48_font_normal, ink.text, "Alice");
    t48_ui_row(&box, 10, 102, w - 20, 38, 1, &ink);
    t48_text(&box, 22, 102 + (38 - t48_font_normal.height) / 2, &t48_font_normal, ink.text, "Bob");
    t48_text_right(&box, w - 22, 102 + (38 - t48_font_small.height) / 2, &t48_font_small, ink.dim, "1002");
    t48_ui_key(&box, 10, 152, 64, 60, "5");
    t48_ui_btn(&box, 84, 154, w - 94, 56, "Call", T48_UI_GO);
    t48_rect(&box, 0, 0, w, h, t->line);
    t48_theme_use(&was, ui_fonts(was.font));
    t48_ui_ground_is(ground);
}
