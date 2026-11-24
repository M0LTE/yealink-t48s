// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the contact list (issue #27). Names and numbers, kept on the
 * phone, added, changed and removed on the phone, and used to put a name to
 * a number wherever one is shown: a call coming in or going out, the
 * dialler, the call history. See docs/phone.md.
 *
 * The file is "contacts" beside the call history (/data/phone/contacts): one
 * contact a line, name|number, so a list made elsewhere can be copied in.
 * It is read at start and written whole, beside the old one and renamed
 * over it, whenever the list changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "shell.h"

#define MAX_CONTACTS 200
#define ROWS 6
#define ROW_Y0 54
#define ROW_STEP 52
#define BTN_Y 400
#define BTN_H 56

struct contact { char name[64], number[64]; };
static struct contact list[MAX_CONTACTS];   /* sorted by name */
static int n, sel;

static enum { M_LIST, M_NAME, M_NUMBER, M_CONFIRM } mode;
static const char *const mode_names[] = {"list", "name", "number", "confirm"};
static t48_kbd kbd;
static int editing;                 /* index being changed, -1 for a new one */
static char new_name[64];
static char add_number[64];         /* a new contact's number, when it is already known */

static const struct { int x, w; const char *label; } btn[4] = {
    {16, 180, "Dial"}, {206, 200, "Add"}, {416, 170, "Edit"}, {596, 188, "Delete"},
};

static int by_name(const void *a, const void *b)
{
    const struct contact *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->number, y->number);
}

static void load(void)
{
    char path[300], line[200];
    snprintf(path, sizeof path, "%s/contacts", opt.state);
    FILE *f = fopen(path, "r");
    n = 0;
    if (!f) return;
    while (n < MAX_CONTACTS && fgets(line, sizeof line, f)) {
        char *bar;
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || !(bar = strrchr(line, '|')) || bar == line || !bar[1]) continue;
        *bar = 0;
        snprintf(list[n].name, sizeof list[n].name, "%s", line);
        snprintf(list[n].number, sizeof list[n].number, "%s", bar + 1);
        n++;
    }
    fclose(f);
    qsort(list, (size_t)n, sizeof list[0], by_name);
}

static void save(void)
{
    char path[300], tmp[310];
    snprintf(path, sizeof path, "%s/contacts", opt.state);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    shell_mkdirs(opt.state);
    FILE *f = fopen(tmp, "w");
    if (!f) { logmsg("contacts: cannot write %s", tmp); return; }
    for (int i = 0; i < n; i++) fprintf(f, "%s|%s\n", list[i].name, list[i].number);
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) == 0) rename(tmp, path);
}

void contacts_init(void) { load(); }

/* the file has been changed by something else (the web page): read it again,
 * unless a contact is being typed just now. 1 when it has been read. */
int contacts_reload(void)
{
    if (mode != M_LIST) return 0;
    load();
    if (sel >= n) sel = n ? n - 1 : 0;
    return 1;
}

const char *contacts_name(const char *number)
{
    if (!number || !number[0]) return NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].number, number)) return list[i].name;
    return NULL;
}

/* ---------- changing the list ---------- */

static void ask_name(const char *start)
{
    t48_kbd_open(&kbd, "Name", start, 40, 0);
    mode = M_NAME;
}

static void ask_number(const char *start)
{
    t48_kbd_open(&kbd, "Number", start, 40, T48_KBD_DIGITS);
    mode = M_NUMBER;
}

static void store(const char *name, const char *number)
{
    struct contact c;
    snprintf(c.name, sizeof c.name, "%s", name);
    snprintf(c.number, sizeof c.number, "%s", number);
    /* the file's own separator, and anything that is not a line of text, cannot be kept */
    for (char *p = c.name; *p; p++) if (*p == '|' || (unsigned char)*p < 32) *p = ' ';
    for (char *p = c.number; *p; p++) if (*p == '|' || (unsigned char)*p <= 32) *p = '-';
    if (editing >= 0 && editing < n) list[editing] = c;
    else if (n < MAX_CONTACTS) list[n++] = c;
    else { logmsg("contacts: the list is full"); return; }
    qsort(list, (size_t)n, sizeof list[0], by_name);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].name, c.name) && !strcmp(list[i].number, c.number)) sel = i;
    save();
    logmsg("contacts: %s, %d in the list", editing >= 0 ? "one changed" : "one added", n);
}

static void remove_one(int i)
{
    if (i < 0 || i >= n) return;
    memmove(list + i, list + i + 1, sizeof list[0] * (size_t)(n - i - 1));
    n--;
    if (sel >= n && sel > 0) sel--;
    save();
    logmsg("contacts: one removed, %d in the list", n);
}

static void kbd_result(int r)
{
    const char *text = t48_kbd_text(&kbd);
    if (r == T48_KBD_CANCEL) { mode = M_LIST; return; }
    if (r != T48_KBD_DONE) return;
    if (mode == M_NAME) {
        if (!text[0]) { mode = M_LIST; return; }
        snprintf(new_name, sizeof new_name, "%s", text);
        ask_number(editing >= 0 ? list[editing].number : add_number);
    } else {
        mode = M_LIST;
        if (text[0]) store(new_name, text);
    }
}

void contacts_open(const char *find)
{
    mode = M_LIST;
    if (sel >= n) sel = n ? n - 1 : 0;
    if (find)
        for (int i = 0; i < n; i++)
            if (!strcmp(list[i].number, find)) sel = i;
    shell_show_contacts(1);
}

/* from the call history: this number, and the name the caller gave if any */
void contacts_open_add(const char *number, const char *name)
{
    contacts_open(NULL);
    editing = -1;
    snprintf(add_number, sizeof add_number, "%s", number);
    ask_name(name ? name : "");
}

static void do_button(int b)
{
    switch (b) {
    case 0:
        if (sel < n) {
            char num[64];
            snprintf(num, sizeof num, "%s", list[sel].number);
            shell_show_contacts(0);
            phone_prefill(num, -1);
        }
        break;
    case 1: editing = -1; add_number[0] = 0; ask_name(""); break;
    case 2: if (sel < n) { editing = sel; ask_name(list[sel].name); } break;
    case 3: if (sel < n) mode = M_CONFIRM; break;
    }
}

/* ---------- input ---------- */

void contacts_key(int code, int down)
{
    if (mode == M_NAME || mode == M_NUMBER) {
        int r = t48_kbd_key(&kbd, code, down);
        kbd_result(r);
        if (r != T48_KBD_NOTHING) shell_phone_changed();
        return;
    }
    if (!down) return;
    if (mode == M_CONFIRM) {
        if (code == T48_KEY_OK) remove_one(sel);
        if (code == T48_KEY_OK || code == T48_KEY_BACK) mode = M_LIST;
        shell_phone_changed();
        return;
    }
    if (code == T48_KEY_UP && sel > 0) sel--;
    else if (code == T48_KEY_DOWN && sel < n - 1) sel++;
    else if (code == T48_KEY_OK) { do_button(n ? 0 : 1); if (mode == M_LIST && !n) return; }
    else if (code == T48_KEY_BACK) { shell_show_contacts(0); return; }
    shell_phone_changed();
}

void contacts_touch(int state, int x, int y)
{
    if (mode == M_NAME || mode == M_NUMBER) {
        int r = t48_kbd_touch(&kbd, state, x, y);
        kbd_result(r);
        if (r != T48_KBD_NOTHING) shell_phone_changed();
        return;
    }
    if (state != T48_TOUCH_DOWN) return;
    if (mode == M_CONFIRM) {
        if (ui_inside(x, y, 596, BTN_Y, 188, BTN_H)) contacts_key(T48_KEY_OK, 1);
        else if (ui_inside(x, y, 416, BTN_Y, 170, BTN_H)) contacts_key(T48_KEY_BACK, 1);
        return;
    }
    int top = sel >= ROWS ? sel - ROWS + 1 : 0;
    for (int r = 0; r < ROWS && top + r < n; r++)
        if (ui_inside(x, y, 16, ROW_Y0 + r * ROW_STEP, SW - 32, 46)) {
            /* a touch chooses; a touch on the one already chosen puts its number in the dialler */
            if (top + r == sel) do_button(0);
            else sel = top + r;
            shell_phone_changed();
            return;
        }
    if (y < HEAD_H && x < 130) { shell_show_contacts(0); return; }
    for (int b = 0; b < 4; b++)
        if (ui_inside(x, y, btn[b].x, BTN_Y, btn[b].w, BTN_H)) {
            do_button(b);
            if (b != 0) shell_phone_changed();
            return;
        }
}

int contacts_timeout(long long now)
{
    (void)now;
    return mode == M_NAME || mode == M_NUMBER ? t48_kbd_timeout(&kbd) : -1;
}

void contacts_tick(long long now)
{
    (void)now;
    if ((mode == M_NAME || mode == M_NUMBER) && t48_kbd_tick(&kbd) != T48_KBD_NOTHING) shell_phone_changed();
}

/* ---------- drawing ---------- */

void contacts_draw(void)
{
    if (mode == M_NAME || mode == M_NUMBER) { t48_kbd_draw(&kbd, &ui); return; }
    ui_clear(C_BG);
    ui_header("Contacts", 1, 0);
    if (!n) t48_text_centre(&ui, SW / 2, 200, &t48_font_head, C_DIM, "No contacts");
    int top = sel >= ROWS ? sel - ROWS + 1 : 0;
    for (int r = 0; r < ROWS && top + r < n; r++) {
        const struct contact *c = &list[top + r];
        int y = ROW_Y0 + r * ROW_STEP;
        struct row_ink ink;
        ui_row(16, y, SW - 32, 46, top + r == sel, &ink);
        t48_text_fit(&ui, 36, y + 12, 440, &t48_font_bold, ink.text, c->name);
        t48_text_right(&ui, SW - 34, y + 12, &t48_font_normal, ink.dim, c->number);
    }
    if (n > ROWS) {
        char b[24];
        snprintf(b, sizeof b, "%d/%d", sel + 1, n);
        t48_text_right(&ui, SW - 100, (HEAD_H - t48_font_bold.height) / 2, &t48_font_bold, T->on_head_dim, b);
    }
    if (mode == M_CONFIRM && sel < n) {
        char b[120];
        snprintf(b, sizeof b, "Delete %.40s?", list[sel].name);
        t48_text_fit(&ui, 24, BTN_Y + (BTN_H - t48_font_bold.height) / 2, 380, &t48_font_bold, C_TEXT, b);
        ui_btn(416, BTN_Y, 170, BTN_H, "Cancel", B_PLAIN);
        ui_btn(596, BTN_Y, 188, BTN_H, "Delete", B_STOP);
    } else {
        for (int b = 0; b < 4; b++) {
            int usable = b == 1 || n > 0;
            ui_btn(btn[b].x, BTN_Y, btn[b].w, BTN_H, btn[b].label, !usable ? B_OFF : b == 0 ? B_GO : B_PLAIN);
        }
    }
}

/* ---------- for the simulator's checks ---------- */

const char *contacts_state(const char *key)
{
    static char b[100];
    if (!strcmp(key, "contacts")) { snprintf(b, sizeof b, "%d", n); return b; }
    if (!strcmp(key, "contacts.mode")) return mode_names[mode];
    if (!strcmp(key, "contacts.sel") || !strcmp(key, "contacts.number")) {
        if (sel >= n) return "-";
        snprintf(b, sizeof b, "%s", key[9] == 's' ? list[sel].name : list[sel].number);
        for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
        return b;
    }
    if (mode != M_NAME && mode != M_NUMBER) return NULL;
    if (!strcmp(key, "kbd")) return "open";
    if (!strcmp(key, "kbd.text")) {
        snprintf(b, sizeof b, "%s", t48_kbd_text(&kbd));
        for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
        return b[0] ? b : "-";
    }
    return NULL;
}

/* a soft key is a touch on the button above it: Dial, Add, Edit, Delete (or Cancel and the verb, when asking) */
void contacts_soft(int b)
{
    if (mode == M_NAME || mode == M_NUMBER) return;   /* the keyboard has the screen */
    b &= 3;
    contacts_touch(T48_TOUCH_DOWN, btn[b].x + btn[b].w / 2, BTN_Y + BTN_H / 2);
}
