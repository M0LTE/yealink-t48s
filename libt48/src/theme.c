// SPDX-License-Identifier: AGPL-3.0-or-later
/* libt48: the themes (docs/design/README.md). A theme is a table of colours by
 * what they are for, a few shapes and a font family; this file has the three
 * there are, and keeps the one in use. The shell picks it (Settings, Screen,
 * Theme) and tells each app (docs/app-protocol.md); what a theme does that the
 * others do not is in its row here, not in the screens. */
#include <string.h>
#include "t48.h"

const t48_theme t48_themes[] = {
    {   /* Switchboard. Black to the bezel, white type, the selection in inverse, colour only in lamps. */
        .name = "switchboard", .font = "b612", .dark = 1,
        .bg = 0x000000, .panel = 0x171715, .line = 0x3A3A35,
        .text = 0xF4F3EE, .dim = 0xA3A299, .faint = 0x66655E,
        .key = 0x24241F, .on_key = 0xF4F3EE, .on_key_dim = 0xA3A299,
        .accent = 0xFFB000, .on_accent = 0x000000,
        .good = 0x35D07F, .on_good = 0x000000, .bad = 0xFF5A47, .on_bad = 0x000000,
        .sel = 0xF4F3EE, .on_sel = 0x000000, .on_sel_dim = 0x55544E,
        .head = 0x000000, .on_head = 0xF4F3EE, .on_head_dim = 0xA3A299,
        .first = 0xF4F3EE, .on_first = 0x000000,
        .ground_ring = 0x000000, .ground_call = 0x000000, .ground_hold = 0x000000,
        .radius = 3, .key_radius = 3, .edge = T48_EDGE_NONE, .rows = T48_ROWS_BLOCK, .head_rule = 1,
    },
    {   /* Directory. Paper, ink, and one telephone yellow: the header band, and the line a finger is on. */
        .name = "directory", .font = "plex", .dark = 0,
        .bg = 0xF7F7F4, .panel = 0xFFFFFF, .line = 0xCFD0CA,
        .text = 0x151514, .dim = 0x5C5D58, .faint = 0x9B9C96,
        .key = 0xFFFFFF, .on_key = 0x151514, .on_key_dim = 0x5C5D58,
        .accent = 0xFFCE00, .on_accent = 0x151514,
        .good = 0x0B8F4C, .on_good = 0xFFFFFF, .bad = 0xD63A22, .on_bad = 0xFFFFFF,
        .sel = 0xFFCE00, .on_sel = 0x151514, .on_sel_dim = 0x5A4A00,
        .head = 0xFFCE00, .on_head = 0x151514, .on_head_dim = 0x5A4A00,
        .first = 0x151514, .on_first = 0xFFFFFF,
        .ground_ring = 0xF7F7F4, .ground_call = 0xF7F7F4, .ground_hold = 0xF7F7F4,
        .radius = 6, .key_radius = 6, .edge = T48_EDGE_OUTLINE, .edge_colour = 0xB9BAB4, .rows = T48_ROWS_RULED,
    },
    {   /* Two-tone. The coloured body and ivory keys of a 1960s telephone; the ground itself changes with a call. */
        .name = "twotone", .font = "figtree", .dark = 1,
        .bg = 0x1C3557, .panel = 0x28466F, .line = 0x3F5F8B,
        .text = 0xF3EEE2, .dim = 0xB1BFD2, .faint = 0x7C93B1,
        .key = 0xEFE8D8, .on_key = 0x1B2A40, .on_key_dim = 0x5C6A7D,
        .accent = 0xFFC94D, .on_accent = 0x1B2333,
        .good = 0x46CF86, .on_good = 0x0B2A1A, .bad = 0xF2664F, .on_bad = 0x2B0B06,
        .sel = 0xEFE8D8, .on_sel = 0x1B2A40, .on_sel_dim = 0x5C6A7D,
        .head = 0x1C3557, .on_head = 0xF3EEE2, .on_head_dim = 0xB1BFD2,
        .first = 0xFFC94D, .on_first = 0x1B2333,
        .ground_ring = 0x0F5A40, .ground_call = 0x1C3557, .ground_hold = 0x6A4A14,
        .radius = 16, .key_radius = 100, .edge = T48_EDGE_LIP, .edge_colour = 0xBDB49E, .rows = T48_ROWS_SOFT,
    },
};
const int t48_theme_count = sizeof t48_themes / sizeof t48_themes[0];

/* a copy, as the one an app is sent lives nowhere else */
static t48_theme now;
static int set;

const t48_theme *t48_theme_now(void)
{
    if (!set) t48_theme_use(&t48_themes[0], NULL);
    return &now;
}

const t48_theme *t48_theme_named(const char *name)
{
    for (int i = 0; i < t48_theme_count; i++)
        if (!strcmp(t48_themes[i].name, name)) return &t48_themes[i];
    return NULL;
}

void t48_fonts_use(const t48_font_family *f)
{
    t48_font_tiny = f->face[T48_FACE_TINY];
    t48_font_small = f->face[T48_FACE_SMALL];
    t48_font_normal = f->face[T48_FACE_NORMAL];
    t48_font_bold = f->face[T48_FACE_BOLD];
    t48_font_mono = f->face[T48_FACE_MONO];
    t48_font_head = f->face[T48_FACE_HEAD];
    t48_font_big = f->face[T48_FACE_BIG];
    t48_font_huge = f->face[T48_FACE_HUGE];
}

void t48_theme_use(const t48_theme *t, const t48_font_family *fonts)
{
    if (t != &now) now = *t;
    now.name[sizeof now.name - 1] = now.font[sizeof now.font - 1] = 0;
    set = 1;
    if (fonts) t48_fonts_use(fonts);
    /* the keyboard */
    t = &now;
    t48_kbd_style = (t48_kbd_look){
        .bg = t->bg, .field = t->panel, .field_text = t->text, .label = t->dim,
        .key = t->key, .text = t->on_key, .dim = t->on_key_dim,
        .side = t->edge == T48_EDGE_LIP ? t48_mix(t->key, t->bg, 60) :
                t->edge == T48_EDGE_OUTLINE ? t48_mix(t->key, t->line, 110) : t->panel,
        .accent = t->first, .on_accent = t->on_first, .cursor = t->edge == T48_EDGE_OUTLINE ? t->text : t->accent,
        .edge = t->edge_colour, .edge_kind = t->edge, .radius = t->radius < 8 ? t->radius : 10,
    };
}
