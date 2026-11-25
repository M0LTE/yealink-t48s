// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the program that owns the phone's screen and input devices.
 * Idle screen, launcher, one app at a time over the app protocol, Home on a
 * long press of X, and a message when an app dies (issue #19). The phone
 * itself (dialler, call screens, history, the phone keys) is in phone.c
 * (issue #26); a call takes the screen from anything, and gives it back.
 *
 * The same program is the PC simulator: started as t48-sim (or with --sim) it
 * keeps the screen in memory, takes its input from a script and writes
 * pictures to files. See docs/shell.md.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "shell.h"

#define HOME_PRESS_MS 800   /* X held this long is Home */

struct options opt = {
    .fb = "/dev/fb0",
    .apps = "/data/apps:/usr/share/t48/apps",
    .data = "/data/appdata",
    .config = "/data/config",
    .state = "/data/phone",
    .sip = "/run/t48-sip/ctrl.sock",
    .audio = T48AC_SOCKET,
    .apply = "/usr/bin/t48-apply",
    .update = "t48-update",
    .access = "/run/t48-access",
    .notice = "/run/t48-notice",
    .web = "/run/t48-web",
};

enum screen { SCR_IDLE, SCR_LAUNCHER, SCR_APP, SCR_CALL, SCR_MESSAGE, SCR_HISTORY, SCR_SETTINGS, SCR_CONTACTS, SCR_KEYS };
static const char *const screen_names[] = {"idle", "launcher", "app", "call", "message", "history", "settings", "contacts", "keys"};

static const struct backend *be;
long long shell_input_us;
int screen_w = APP_W, screen_h = APP_H;
struct layout L = {.touch = 1};
static uint32_t *ui_px;
t48_surface ui;
static t48_surface app_surface;
/* small screens: what the soft keys do on the screen being drawn */
static char bar_label[4][16];
static int bar_role[4], bar_set;

static enum screen scr = SCR_IDLE;
static enum screen under_call;       /* where to go when the handset goes back down */
static int app_shown;                /* the app in front has drawn since it came to the front */
static unsigned app_frames;
static int sel;                      /* launcher: selected app */
static char pending_id[64];          /* app to start once the old one has gone */
static char msg_title[96], msg_text[200];
static long long x_deadline;         /* in an app: when a held X becomes Home; 0 if X is up */
/* The key test opens when any key is held from the idle screen, whatever the key table makes of it: the
 * key, and when it has been held long enough (0: none is). And the key still held when the test was left,
 * whose release is nobody's. */
static int hold_scan, hold_key, left_scan, left_key, left_held;
static int down_scan, down_key, down_held;   /* the key that went down last, and whether it is still down */
static long long hold_deadline;
static int idle_notices;             /* how many notices the idle screen's row had room for */
static char idle_shown[900];         /* time, date, network, accounts and notices as last drawn, to redraw only on a change */
static volatile sig_atomic_t stopping;
static int exit_status;
static int chld_pipe[2];
static int log_quiet;                /* the command-line tools say what they have to say themselves */

void logmsg(const char *fmt, ...)
{
    va_list ap;
    time_t t = time(NULL);
    struct tm tm;
    if (log_quiet) return;
    gmtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* make a folder, and those above it, for the shell's own files */
void shell_mkdirs(const char *path)
{
    char p[300];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0700);
}

/* The backend has found the screen. Smaller than the T48S's: the small layout (docs/shell.md). */
void shell_screen_is(int w, int h, int touch, int softkeys)
{
    free(ui_px);
    ui_px = calloc((size_t)w * (size_t)h, sizeof *ui_px);
    if (!ui_px) { perror("the screen's picture"); exit(1); }
    screen_w = w;
    screen_h = h;
    ui = (t48_surface){ui_px, w, h, w};
    L.small = w < APP_W || h < APP_H;
    L.touch = touch;
    L.softkeys = softkeys;
    t48_ui_small(L.small);
    t48_kbd_keys_only = !touch;
    logmsg("layout: %dx%d, %s%s%s", w, h, L.small ? "small" : "full", touch ? ", touch" : ", no touch", softkeys ? ", soft keys" : "");
}

const char *shell_screen_name(void) { return screen_names[scr]; }
const struct backend *shell_backend(void) { return be; }
unsigned shell_app_frames(void) { return app_frames; }
void shell_stop(int status) { stopping = 1; exit_status = status; }

/* ---------- drawing the shell's own screens ---------- */

void ui_network_status(char *out, size_t n, int *up)
{
    struct ifaddrs *list, *a;
    *up = 0;
    snprintf(out, n, "No network");
    if (getifaddrs(&list) < 0) return;
    for (a = list; a; a = a->ifa_next) {
        char ip[INET_ADDRSTRLEN];
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if ((a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_RUNNING)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof ip);
        snprintf(out, n, "%s", ip);
        *up = 1;
        break;
    }
    freeifaddrs(list);
}

void ui_bar(const char *l0, int r0, const char *l1, int r1, const char *l2, int r2, const char *l3, int r3)
{
    const char *const l[4] = {l0, l1, l2, l3};
    const int r[4] = {r0, r1, r2, r3};
    for (int i = 0; i < 4; i++) {
        snprintf(bar_label[i], sizeof bar_label[i], "%s", l[i] ? l[i] : "");
        bar_role[i] = r[i];
    }
    bar_set = 1;
}

/* a key that does something, by its label: for the simulator's checks */
static const char *bar_state(int i)
{
    static char b[16];
    snprintf(b, sizeof b, "%s", bar_set && bar_label[i][0] ? bar_label[i] : "-");
    for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
    return b;
}

void ui_ask(const char *title, const char *text, const char *extra, int tone)
{
    struct row_ink ink;
    int more = (text && text[0]) || (extra && extra[0]);
    int w = SW - 40, h = !more ? 76 : extra && extra[0] ? UI_ASK_H : UI_ASK_H - 26, x = 20, y = HEAD_H + (BAR_Y - HEAD_H - h) / 2;
    const t48_font *f = t48_text_width(&t48_font_head, title) > w - 24 ? &t48_font_bold : &t48_font_head;
    ui_dialog(x, y, w, h, tone);
    ui_dialog_ink(&ink);
    int ty = y + (more ? 16 : (h - t48_font_head.height) / 2 + 2) + (t48_font_head.height - f->height) / 2;
    if (t48_text_width(f, title) > w - 24) t48_text_fit(&ui, x + 12, ty, w - 24, f, ink.text, title);
    else t48_text_centre(&ui, SW / 2, ty, f, ink.text, title);
    if (text && text[0]) ui_two_lines_centred(SW / 2, y + 58, w - 32, &t48_font_normal, ink.dim, text);
    if (extra && extra[0]) {
        const t48_font *ef = t48_text_width(&t48_font_small, extra) > w - 16 ? &t48_font_tiny : &t48_font_small;
        if (t48_text_width(ef, extra) > w - 16) t48_text_fit(&ui, x + 8, y + h - 30, w - 16, ef, ink.dim, extra);
        else t48_text_centre(&ui, SW / 2, y + h - 30, ef, ink.dim, extra);
    }
}

/* five buttons along the bottom of the idle screen */
#define IDLE_BTN_Y 400
#define IDLE_BTN_W 144
#define IDLE_BTN_H 56
#define IDLE_BTN_X(i) (24 + (i) * (IDLE_BTN_W + 8))
enum { IB_APPS, IB_CALLS, IB_CONTACTS, IB_DND, IB_SETTINGS };
/* one line for each account, when there is more than one */
#define ACC_Y 292
#define ACC_H 36
#define ACC_MAX 6

/* The standing notice: up to two short lines that something else wants on the idle
 * screen for as long as its file exists. The first leads the row of notices, the second
 * takes the bottom line. t48-live uses it to say that the phone was started from a USB
 * stick and nothing is installed, and how to go back (docs/live-image.md). */
static void notice_read(char line[2][96])
{
    FILE *f = opt.notice ? fopen(opt.notice, "r") : NULL;
    line[0][0] = line[1][0] = 0;
    if (!f) return;
    for (int i = 0; i < 2 && fgets(line[i], 96, f); i++) {
        line[i][strcspn(line[i], "\r\n")] = 0;
        for (char *p = line[i]; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
    }
    fclose(f);
}

static void idle_text(char *out, size_t n)
{
    char net[80], acc[120];
    uint32_t c;
    int up;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    ui_network_status(net, sizeof net, &up);
    phone_account_text(acc, sizeof acc, &c);
    size_t k = strftime(out, n, "%H:%M|%A %e %B %Y|", &tm);
    k += (size_t)snprintf(out + k, n - k, "%s|%s|%d|%d|%d", net, acc, phone_missed(), phone_messages(), phone_dnd());
    for (int i = 0; i < ACC_MAX && k < n; i++) {
        char line[80];
        int def;
        if (!phone_account_line(i, line, sizeof line, &c, &def)) break;
        k += (size_t)snprintf(out + k, n - k, "|%s %x %d", line, (unsigned)c, def);
    }
    char file[2][96];
    notice_read(file);
    if (k < n) snprintf(out + k, n - k, "|%s|%s", file[0], file[1]);
}

/* where the k-th of n account lines goes: a row of up to three, or two rows */
static void acc_box(int k, int n, int *x, int *y, int *w)
{
    int per = n > 3 ? 3 : n, row = k / 3, in_row = row == 0 ? per : n - 3;
    *w = n > 3 ? 248 : n == 3 ? 248 : 300;
    *x = (SW - in_row * *w - (in_row - 1) * 8) / 2 + (k % 3) * (*w + 8);
    *y = ACC_Y - (n > 3 ? 40 : 0) + row * (ACC_H + 4);
}

/* Small screens: one line for all the accounts, a lamp and a name each, with as many words as there is room for */
static void idle_accounts_small(int y)
{
    char line[ACC_MAX][80];
    uint32_t c[ACC_MAX];
    int n = 0, def, total = 0;
    while (n < ACC_MAX && phone_account_line(n, line[n], sizeof line[0], &c[n], &def)) n++;
    for (int cut = 0; cut < 3; cut++) {
        total = 0;
        for (int i = 0; i < n; i++) {
            if (cut == 1) line[i][strcspn(line[i], ":")] = 0;   /* the name without its state: the lamp says it */
            if (cut == 2) line[i][0] = 0;
            total += 12 + (line[i][0] ? 6 + t48_text_width(&t48_font_small, line[i]) : 0) + (i ? 14 : 0);
        }
        if (total <= SW - 24) break;
    }
    for (int i = 0, x = (SW - total) / 2; i < n; i++) {
        ui_lamp(x, y + (t48_font_small.height - 12) / 2, c[i]);
        x += 12;
        if (line[i][0]) x = t48_text(&ui, x + 6, y, &t48_font_small, C_DIM, line[i]);
        x += 14;
    }
}

static void draw_idle(void)
{
    char hm[8], date[64];
    time_t t = time(NULL);
    struct tm tm;

    localtime_r(&t, &tm);
    strftime(hm, sizeof hm, "%H:%M", &tm);
    strftime(date, sizeof date, "%A %e %B %Y", &tm);
    ui_clear(C_BG);
    ui_header_idle();
    /* the accounts, when there are several: which are registered, and a touch dials from one */
    int naccts = phone_accounts(), several = naccts > 1, ny, y2;
    if (SMALL) {
        /* The clock, the date, the accounts on one line, the notices: down the middle, between where
         * the line keys' labels will go. 214 rows between the header and the soft keys on 480x272. */
        const t48_glyph *d = &t48_font_huge.g['0' - 32];
        int top = HEAD_H + (BAR_Y - HEAD_H - 214) / 2 + (several ? 6 : 18);
        t48_text_centre(&ui, SW / 2, top - d->yoff, &t48_font_huge, C_TEXT, hm);
        int y = top + d->h + 9;
        if (t48_text_width(&t48_font_normal, date) > SW - 2 * LINE_W) t48_text_fit(&ui, LINE_W, y, SW - 2 * LINE_W, &t48_font_normal, C_DIM, date);
        else t48_text_centre(&ui, SW / 2, y, &t48_font_normal, C_DIM, date);
        y += t48_font_normal.height + 2;
        if (several) { idle_accounts_small(y); y += 22; }
        ny = y + 3;
        y2 = ny + 34;
    } else {
        t48_text_centre(&ui, SW / 2, several ? 72 : 112, &t48_font_huge, C_TEXT, hm);
        t48_text_centre(&ui, SW / 2, several ? 200 : 248, &t48_font_head, C_DIM, date);
        ny = several ? 340 : 320;
        y2 = 372;
    }
    for (int i = 0; !SMALL && several && i < ACC_MAX; i++) {
        char line[80];
        uint32_t c;
        int def, x, y, w;
        struct row_ink ink;
        if (!phone_account_line(i, line, sizeof line, &c, &def)) break;
        acc_box(i, naccts, &x, &y, &w);
        ui_tile(x, y, w, ACC_H, 0, &ink);
        ui_lamp(x + 12, y + ACC_H / 2 - 6, c);
        t48_text_fit(&ui, x + 34, y + (ACC_H - t48_font_normal.height) / 2, w - (def ? 112 : 44), &t48_font_normal, ink.text, line);
        if (def) t48_text_right(&ui, x + w - 12, y + (ACC_H - t48_font_small.height) / 2, &t48_font_small, ink.dim, "Default");
    }
    /* One row of notices, the most important first: the standing notice from a file
     * (t48-live: nothing is installed, which the owner must not lose sight of), missed
     * calls, voice messages, do not disturb. What does not fit the row is left out from
     * the end: do not disturb shows on its button too, and messages on the lamp. A small
     * screen shows do not disturb in the header instead, so a notice never crowds it out. */
    char note[4][96], file[2][96];
    int role[4];
    int nn = 0, total = 0;
    notice_read(file);
    if (file[0][0]) role[nn++] = B_FIRST;
    if (phone_missed()) {
        snprintf(note[nn], sizeof note[0], "%d missed call%s", phone_missed(), phone_missed() == 1 ? "" : "s");
        role[nn++] = B_STOP;
    }
    if (phone_messages()) {
        snprintf(note[nn], sizeof note[0], "%d new message%s", phone_messages(), phone_messages() == 1 ? "" : "s");
        role[nn++] = B_LIT;
    }
    if (phone_dnd() && !SMALL) {
        snprintf(note[nn], sizeof note[0], "Do not disturb");
        role[nn++] = B_LIT;
    }
    /* a small screen sets the row in the small face when that is what it takes to show it all */
    for (int tight = 0; tight <= SMALL; tight++) {
        t48_ui_notice_tight(tight);
        if (file[0][0]) {
            snprintf(note[0], sizeof note[0], "%s", file[0]);
            for (size_t l = strlen(note[0]); l && ui_notice_width(note[0]) > SW - 24; ) note[0][--l] = 0;
        }
        idle_notices = total = 0;
        for (int i = 0; i < nn; i++) {
            int w = ui_notice_width(note[i]) + (i ? (SMALL ? 8 : 12) : 0);
            if (total + w > SW - 24) break;
            total += w;
            idle_notices++;
        }
        if (idle_notices == nn && (!file[0][0] || !strcmp(note[0], file[0]))) break;
    }
    for (int i = 0, x = (SW - total) / 2; i < idle_notices; i++) x = ui_notice(x, ny, note[i], role[i]);
    t48_ui_notice_tight(0);
    if (SMALL) {
        /* the soft keys; do not disturb is Right, and shows in the header */
        ui_bar("Apps", B_PLAIN, "Calls", B_PLAIN, "Contacts", B_PLAIN, "Settings", B_PLAIN);
        if (file[1][0] && several) t48_text_fit(&ui, 12, y2, SW - 24, &t48_font_small, C_DIM, file[1]);
        else if (file[1][0]) ui_two_lines_centred(SW / 2, y2, SW - 24, &t48_font_small, C_DIM, file[1]);
        return;
    }
    static const char *const labels[5] = {"Apps", "Calls", "Contacts", "DND", "Settings"};
    for (int i = 0; i < 5; i++)
        ui_btn(IDLE_BTN_X(i), IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H, labels[i], i == IB_DND && phone_dnd() ? B_LIT : B_PLAIN);
    /* the notice's second line (how to go back to Yealink), above the buttons */
    if (file[1][0] && t48_text_width(&t48_font_small, file[1]) > SW - 20) t48_text_fit(&ui, 10, y2, SW - 20, &t48_font_small, C_DIM, file[1]);
    else if (file[1][0]) t48_text_centre(&ui, SW / 2, y2, &t48_font_small, C_DIM, file[1]);
}

/* launcher grid: 4 across, 2 down, a page at a time. On a small screen the tiles and their pictures are smaller. */
#define COLS 4
#define ROWS 2
#define TILE_W (SMALL ? 108 : 170)
#define TILE_H (SMALL ? 98 : 188)
#define TILE_GAP (SMALL ? 8 : 20)
#define GRID_X ((SW - COLS * TILE_W - (COLS - 1) * TILE_GAP) / 2)
#define GRID_Y (SMALL ? HEAD_H + (BAR_Y - HEAD_H - ROWS * TILE_H - TILE_GAP) / 2 : 66)
#define ICON (SMALL ? 48 : 96)

/* an app's picture at half its size, each pixel the average of four */
static void blit_half(int x, int y, const t48_surface *img)
{
    for (int j = 0; j + 1 < img->h; j += 2)
        for (int i = 0; i + 1 < img->w; i += 2) {
            const uint32_t *p = img->px + j * img->stride + i, *q = p + img->stride;
            uint32_t rb = ((p[0] & 0xFF00FF) + (p[1] & 0xFF00FF) + (q[0] & 0xFF00FF) + (q[1] & 0xFF00FF)) >> 2 & 0xFF00FF;
            uint32_t g = ((p[0] & 0x00FF00) + (p[1] & 0x00FF00) + (q[0] & 0x00FF00) + (q[1] & 0x00FF00)) >> 2 & 0x00FF00;
            t48_fill(&ui, x + i / 2, y + j / 2, 1, 1, rb | g);
        }
}

static void tile_box(int slot, int *x, int *y)
{
    *x = GRID_X + (slot % COLS) * (TILE_W + TILE_GAP);
    *y = GRID_Y + (slot / COLS) * (TILE_H + TILE_GAP);
}

/* the launcher's tiles: every installed app, then "Get apps" (the app manager) */
#define NTILES (napps + 1)

static void draw_launcher(void)
{
    ui_clear(C_BG);
    ui_header("Apps", 1, 0);
    if (sel >= NTILES) sel = NTILES - 1;
    int per = COLS * ROWS, page = sel / per, pages = (NTILES + per - 1) / per;
    for (int slot = 0; slot < per; slot++) {
        int i = page * per + slot, x, y;
        struct row_ink ink;
        if (i >= NTILES) break;
        tile_box(slot, &x, &y);
        ui_tile(x, y, TILE_W, TILE_H, i == sel, &ink);
        int ix = x + (TILE_W - ICON) / 2, iy = y + (SMALL ? 10 : 16), ty = y + (SMALL ? 66 : 124), pad = SMALL ? 4 : 8;
        if (i == napps) {
            ui_icon(ix, iy, ICON, "+");
            t48_text_centre(&ui, x + TILE_W / 2, ty, &t48_font_bold, ink.text, "Get apps");
            continue;
        }
        const struct manifest *m = &apps[i];
        if (m->icon.px && SMALL) {
            blit_half(ix + (ICON - m->icon.w / 2) / 2, iy + (ICON - m->icon.h / 2) / 2, &m->icon);
        } else if (m->icon.px) {
            t48_blit(&ui, ix + (96 - m->icon.w) / 2, iy + (96 - m->icon.h) / 2, m->icon.px, m->icon.w, m->icon.h, m->icon.stride);
        } else {
            char letter[2] = {m->name[0], 0};
            if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
            ui_icon(ix, iy, ICON, letter);
        }
        int tw = t48_text_width(&t48_font_bold, m->name);
        if (tw > TILE_W - 2 * pad) t48_text_fit(&ui, x + pad, ty, TILE_W - 2 * pad, &t48_font_bold, ink.text, m->name);
        else t48_text_centre(&ui, x + TILE_W / 2, ty, &t48_font_bold, ink.text, m->name);
        if (SMALL) {
            /* open: a lamp in the corner, as there is no room for the word */
            if (!strcmp(app_id(), m->id)) ui_lamp(x + TILE_W - 20, y + 8, C_GOOD);
        } else if (!strcmp(app_id(), m->id)) {
            int w = t48_text_width(&t48_font_small, "Open") + 18;
            ui_lamp(x + (TILE_W - w) / 2, y + 156, C_GOOD);
            t48_text(&ui, x + (TILE_W - w) / 2 + 18, y + 153, &t48_font_small, ink.dim, "Open");
        }
    }
    if (pages > 1) {
        char b[24];
        snprintf(b, sizeof b, "%d/%d", page + 1, pages);
        t48_text_right(&ui, HEAD_NOTE_X, (HEAD_H - t48_font_bold.height) / 2, &t48_font_bold, T->on_head_dim, b);
    }
    ui_bar(sel == napps ? "Get apps" : "Open", B_FIRST, NULL, 0, NULL, 0, "Back", B_PLAIN);
}

#define MSG_BTN_X 320
#define MSG_BTN_Y 300
#define MSG_BTN_W 160
#define MSG_BTN_H 56

static void draw_message(void)
{
    struct row_ink ink;
    ui_clear(C_BG);
    ui_header("Apps", 1, 0);
    if (SMALL) {
        ui_ask(msg_title, msg_text, NULL, B_STOP);
        ui_bar(NULL, 0, NULL, 0, NULL, 0, "OK", B_FIRST);
        return;
    }
    ui_dialog(80, 96, 640, 290, B_STOP);
    ui_dialog_ink(&ink);
    t48_text_centre(&ui, SW / 2, 132, &t48_font_head, ink.text, msg_title);
    int w = t48_text_width(&t48_font_normal, msg_text);
    if (w > 600) t48_text_fit(&ui, 100, 196, 600, &t48_font_normal, ink.dim, msg_text);
    else t48_text_centre(&ui, SW / 2, 196, &t48_font_normal, ink.dim, msg_text);
    ui_dialog_btn(MSG_BTN_X, MSG_BTN_Y, MSG_BTN_W, MSG_BTN_H, "OK", B_FIRST);
}

static void draw_starting(void)
{
    char b[96];
    ui_clear(C_BG);
    snprintf(b, sizeof b, "Starting %s", app_id());
    t48_text_centre(&ui, SW / 2, SMALL ? SH / 2 - t48_font_head.height / 2 : 210, &t48_font_head, C_DIM, b);
}

/* The screen shows the shell's own frame again, not an app's flip buffer.
 * Called after that frame has been drawn, so nothing stale is seen. */
static void screen_home(void)
{
    if (be->flip_show) be->flip_show(-1);
}

void shell_screen_flush(const uint32_t *src, int w, int h)
{
    be->flush(src, w, w, h, 0, 0, w, h);
    screen_home();
}

static void redraw(void)
{
    idle_text(idle_shown, sizeof idle_shown);
    if (scr == SCR_APP && app_shown && app_pixels()) {
        if (app_flip() >= 0) app_flip_show();   /* its last whole picture, wherever that is kept */
        else shell_screen_flush(app_pixels(), app_w(), app_h());
        return;
    }
    bar_set = 0;
    switch (scr) {
    case SCR_IDLE: draw_idle(); break;
    case SCR_LAUNCHER: draw_launcher(); break;
    case SCR_CALL: phone_draw(); break;
    case SCR_HISTORY: phone_draw_history(); break;
    case SCR_MESSAGE: draw_message(); break;
    case SCR_APP: draw_starting(); break;
    case SCR_SETTINGS: settings_draw(); break;
    case SCR_CONTACTS: contacts_draw(); break;
    case SCR_KEYS: keytest_draw(); break;
    }
    phone_draw_overlay();
    if (scr != SCR_CALL) watch_draw_overlay();
    if (!SMALL) bar_set = 0;
    if (bar_set) {
        const char *const label[4] = {bar_label[0], bar_label[1], bar_label[2], bar_label[3]};
        t48_ui_softkeys(&ui, label, bar_role);
    }
    shell_screen_flush(ui_px, SW, SH);
}

static void show(enum screen s)
{
    if (s != scr) logmsg("screen: %s", screen_names[s]);
    scr = s;
    x_deadline = 0;
    redraw();
}

static void message(const char *title, const char *text)
{
    snprintf(msg_title, sizeof msg_title, "%s", title);
    snprintf(msg_text, sizeof msg_text, "%s", text);
}

/* ---------- apps ---------- */

const t48_surface *shell_app_surface(void)
{
    app_surface = (t48_surface){(uint32_t *)app_pixels(), app_w(), app_h(), app_w()};
    return app_surface.px ? &app_surface : NULL;
}

static const struct manifest *find_app(const char *id)
{
    for (int i = 0; i < napps; i++)
        if (!strcmp(apps[i].id, id)) return &apps[i];
    return NULL;
}

static void start_app(const struct manifest *m)
{
    char title[96];
    if (app_launch(m) < 0) {
        logmsg("cannot start %s: %s", m->id, app_error());
        snprintf(title, sizeof title, "%s did not start", m->name);
        message(title, app_error());
        show(SCR_MESSAGE);
        return;
    }
    app_shown = 0;
    show(SCR_APP);
}

/* the owner picked an app in the launcher */
static void open_app(const struct manifest *m)
{
    if (!strcmp(app_id(), m->id) && (app_state() == APP_PAUSING || app_state() == APP_STOPPED)) {
        app_resume();   /* still there from last time: bring it back */
        app_shown = 1;
        show(SCR_APP);
    } else if (app_state() != APP_NONE) {
        /* one app at a time: close the other, start this one when it has gone */
        snprintf(pending_id, sizeof pending_id, "%s", m->id);
        app_quit();
    } else {
        start_app(m);
    }
}

static void open_tile(int i)
{
    if (i < napps) { open_app(&apps[i]); return; }
    settings_open(1);   /* "Get apps" */
    show(SCR_SETTINGS);
}

/* the look has changed: an app in front draws itself again when it is told, the shell's screens here */
void shell_theme_changed(void)
{
    if (scr != SCR_APP || !app_shown) redraw();
}

void shell_settings_changed(void)
{
    if (scr == SCR_SETTINGS) redraw();
}

void shell_settings_closed(int to_launcher)
{
    if (scr != SCR_SETTINGS) return;
    show(to_launcher ? SCR_LAUNCHER : SCR_IDLE);
}

void shell_settings_open_app(const char *id)
{
    const struct manifest *m = find_app(id);
    if (!m || scr != SCR_SETTINGS) return;
    settings_close();
    for (int i = 0; i < napps; i++)
        if (&apps[i] == m) sel = i;
    show(SCR_LAUNCHER);
    open_app(m);
}

static void go_home(void)
{
    if (app_state() == APP_RUNNING) app_pause();
    else app_quit();  /* still starting: not worth keeping */
    show(SCR_LAUNCHER);
}

void shell_app_damage(int x, int y, int w, int h)
{
    app_frames++;
    if (scr != SCR_APP) return;
    if (!app_shown) {  /* first picture: replace the whole "Starting" screen */
        app_shown = 1;
        redraw();
        return;
    }
    be->flush(app_pixels(), app_w(), app_w(), app_h(), x, y, w, h);
    screen_home();
}

/* The app has sent a whole picture to show. 1: it is in front, show it. */
int shell_app_flip(void)
{
    app_frames++;
    if (scr != SCR_APP) return 0;
    app_shown = 1;
    return 1;
}

void shell_flipped(int n, unsigned frame, long long time_us)
{
    app_flipped(n, frame, time_us);
}

void shell_app_gone(const char *name, const char *why)
{
    enum screen next = SCR_LAUNCHER;
    int was_front = scr == SCR_APP || (scr == SCR_CALL && under_call == SCR_APP);

    if (why) {
        snprintf(msg_title, sizeof msg_title, "%s stopped", name);
        snprintf(msg_text, sizeof msg_text, "%s", why);
        if (was_front) next = SCR_MESSAGE;
    }
    if (pending_id[0]) {
        const struct manifest *m = find_app(pending_id);
        pending_id[0] = 0;
        if (m && scr == SCR_LAUNCHER) { start_app(m); return; }
    }
    if (scr == SCR_CALL) {
        if (under_call == SCR_APP) under_call = next;
    } else if (scr == SCR_APP) {
        show(next);
    } else if (scr == SCR_LAUNCHER) {
        redraw();  /* its "open" mark goes */
    }
}

/* ---------- input ---------- */

static void launcher_key(int code)
{
    int per = COLS * ROWS;
    switch (code) {
    case T48_KEY_LEFT: if (sel > 0) sel--; break;
    case T48_KEY_RIGHT: if (sel < NTILES - 1) sel++; break;
    case T48_KEY_UP: if (sel >= COLS) sel -= COLS; break;
    case T48_KEY_DOWN:
        if (sel + COLS < NTILES) sel += COLS;
        else if (sel / per < (NTILES - 1) / per) sel = NTILES - 1;
        break;
    case T48_KEY_OK:
        if (sel < NTILES) open_tile(sel);
        return;
    case T48_KEY_BACK: show(SCR_IDLE); return;
    default: return;
    }
    redraw();
}

static void open_keytest(void)
{
    if (scr == SCR_APP || scr == SCR_KEYS || !phone_stand_down()) return;
    if (scr == SCR_SETTINGS) settings_close();
    keytest_open();
    show(SCR_KEYS);
}

/* From Settings: the key that chose the row is still down, and its release is not the test's first key */
void shell_show_keytest(void)
{
    open_keytest();
    if (scr != SCR_KEYS || !down_held) return;
    left_scan = down_scan;
    left_key = down_key;
    left_held = 1;
}

/* Is this the release of that key? Two keys the table does not hold share one Linux key, so the release of
 * the one held longer comes as its place alone, with no key event. */
static int released(int scan, int key, int value, int was_scan, int was_key)
{
    if (value < 0) return scan && scan == was_scan;
    return !value && scan == was_scan && key == was_key;
}

void shell_scan(int scan, int key, int code, int value)
{
    if (value > 0) { down_scan = scan; down_key = key; down_held = 1; }
    else if (released(scan, key, value, down_scan, down_key)) down_held = 0;
    if (hold_deadline && released(scan, key, value, hold_scan, hold_key)) hold_deadline = 0;
    if (left_held && released(scan, key, value, left_scan, left_key)) {
        left_held = 0;
        if (code) backlight_key(code, 0);
        return;
    }
    if (scr == SCR_KEYS) {
        /* every key is the test's, and does nothing else */
        if (code) backlight_key(code, value > 0);
        else if (value) backlight_wake("a key");
        keytest_event(scan, key, value);
        redraw();
        return;
    }
    if (value < 0) return;
    /* not the phone keys, which act on any screen and may well be held: volume, Speaker, Mute and the rest */
    if (value && scr == SCR_IDLE && !hold_deadline && code < SK_VOLDOWN) {
        hold_scan = scan;
        hold_key = key;
        hold_deadline = t48_now_ms() + KEYTEST_OPEN_MS;
    }
    if (code) shell_key(code, value);
    else if (value) logmsg("key code %d has no meaning yet", key);
}

/* Small screens: soft key b, 0 to 3, does what its label says */
static void soft_key(int b)
{
    static const int idle[4] = {T48_KEY_OK, T48_KEY_UP, T48_KEY_LEFT, T48_KEY_DOWN};   /* Apps, Calls, Contacts, Settings */
    switch (scr) {
    case SCR_IDLE: shell_key(idle[b], 1); break;
    case SCR_LAUNCHER: if (b == 0) shell_key(T48_KEY_OK, 1); else if (b == 3) shell_key(T48_KEY_BACK, 1); break;
    case SCR_MESSAGE: if (b == 3) show(SCR_LAUNCHER); break;
    case SCR_CALL: phone_soft(b); break;
    case SCR_HISTORY: phone_history_soft(b); break;
    case SCR_SETTINGS: settings_soft(b); break;
    case SCR_CONTACTS: contacts_soft(b); break;
    default: break;
    }
}

void shell_key(int code, int down)
{
    if (backlight_key(code, down)) return;   /* it woke a dark screen, and that is all it does */
    if (code >= SK_VOLDOWN) {
        /* phone keys stay with the shell, whatever is on the screen */
        phone_key(code, down);
        return;
    }
    if (code >= SK_SOFT1 && code <= SK_SOFT4) {
        /* In an app, the app's (level 3). On a small screen, what the label above the key says. On the
         * T48S's layout, the bottom row of buttons, where a screen has one. */
        if (scr == SCR_APP) app_key(T48_KEY_SOFT1 + code - SK_SOFT1, down);
        else if (down && SMALL) soft_key(code - SK_SOFT1);
        else if (down && scr == SCR_HISTORY) phone_history_soft(code - SK_SOFT1);
        else if (down && scr == SCR_CONTACTS) contacts_soft(code - SK_SOFT1);
        return;
    }
    if (scr != SCR_APP && scr != SCR_CALL && watch_key(code, down)) return;   /* X on a browser's pairing code */
    switch (scr) {
    case SCR_APP:
        if (code == T48_KEY_BACK) {
            /* short press: "back" for the app, sent when X comes up. Long press: Home. */
            if (down) {
                x_deadline = t48_now_ms() + HOME_PRESS_MS;
            } else if (x_deadline) {
                x_deadline = 0;
                app_key(T48_KEY_BACK, 1);
                app_key(T48_KEY_BACK, 0);
            }
        } else {
            app_key(code, down);
        }
        break;
    case SCR_IDLE:
        if (down && code == T48_KEY_OK) {
            if (app_state() == APP_NONE) manifests_scan(opt.apps);
            show(SCR_LAUNCHER);
        } else if (down && code == T48_KEY_DOWN) {
            settings_open(0);
            show(SCR_SETTINGS);
        } else if (down) {
            phone_idle_key(code);   /* a digit starts the dialler */
        }
        break;
    case SCR_LAUNCHER:
        if (down) launcher_key(code);
        break;
    case SCR_MESSAGE:
        if (down && (code == T48_KEY_OK || code == T48_KEY_BACK)) show(SCR_LAUNCHER);
        break;
    case SCR_CALL:
        phone_key(code, down);
        break;
    case SCR_HISTORY:
        if (down) phone_history_key(code);
        break;
    case SCR_SETTINGS:
        settings_key(code, down);
        break;
    case SCR_CONTACTS:
        contacts_key(code, down);
        break;
    case SCR_KEYS:   /* its keys come by shell_scan() */
        break;
    }
}

int ui_inside(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void shell_touch(int state, int x, int y)
{
    if (backlight_touch(state)) return;
    if (scr == SCR_KEYS) return;
    if (scr == SCR_APP) { app_touch(state, x, y); return; }
    if (SMALL && bar_set && y >= BAR_Y) {
        /* a small screen that takes touches: each label is a button */
        int b = x * 4 / SW;
        if (state == T48_TOUCH_DOWN && bar_label[b][0]) soft_key(b);
        return;
    }
    if (scr != SCR_CALL && watch_touch(state, x, y)) return;
    if (scr == SCR_SETTINGS) { settings_touch(state, x, y); return; }
    if (scr == SCR_CONTACTS) { contacts_touch(state, x, y); return; }
    if (state != T48_TOUCH_DOWN) return;
    if ((scr == SCR_LAUNCHER || scr == SCR_HISTORY) && y < HEAD_H && x < 130) {
        shell_key(T48_KEY_BACK, 1);   /* the way back in the header */
    } else if (scr == SCR_CALL) {
        phone_touch(x, y);
    } else if (scr == SCR_HISTORY) {
        phone_history_touch(x, y);
    } else if (SMALL && scr != SCR_LAUNCHER) {
        /* the idle screen and a message have nothing else to touch */
    } else if (scr == SCR_IDLE && ui_inside(x, y, IDLE_BTN_X(IB_APPS), IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H)) {
        shell_key(T48_KEY_OK, 1);
    } else if (scr == SCR_IDLE && ui_inside(x, y, IDLE_BTN_X(IB_CALLS), IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H)) {
        phone_open_history();
    } else if (scr == SCR_IDLE && ui_inside(x, y, IDLE_BTN_X(IB_CONTACTS), IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H)) {
        contacts_open(NULL);
    } else if (scr == SCR_IDLE && ui_inside(x, y, IDLE_BTN_X(IB_DND), IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H)) {
        phone_toggle_dnd();
    } else if (scr == SCR_IDLE && ui_inside(x, y, IDLE_BTN_X(IB_SETTINGS), IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H)) {
        shell_key(T48_KEY_DOWN, 1);
    } else if (scr == SCR_IDLE && phone_accounts() > 1) {
        for (int i = 0; i < ACC_MAX; i++) {
            char line[80];
            uint32_t c;
            int def, ax, ay, aw, acct = phone_account_line(i, line, sizeof line, &c, &def);
            if (!acct) break;
            acc_box(i, phone_accounts(), &ax, &ay, &aw);
            if (ui_inside(x, y, ax, ay, aw, ACC_H)) { phone_dial_from(acct - 1); return; }
        }
    } else if (scr == SCR_MESSAGE && ui_inside(x, y, MSG_BTN_X, MSG_BTN_Y, MSG_BTN_W, MSG_BTN_H)) {
        show(SCR_LAUNCHER);
    } else if (scr == SCR_LAUNCHER) {
        int per = COLS * ROWS, page = sel / per;
        for (int slot = 0; slot < per && page * per + slot < NTILES; slot++) {
            int tx, ty;
            tile_box(slot, &tx, &ty);
            if (ui_inside(x, y, tx, ty, TILE_W, TILE_H)) {
                sel = page * per + slot;
                redraw();
                open_tile(sel);
                return;
            }
        }
    }
}

void shell_hook(int off_hook)
{
    backlight_wake("the handset");
    phone_hook(off_hook);
}

/* ---------- the phone takes the screen, and gives it back ---------- */

void shell_phone_screen(int want)
{
    if (want) backlight_wake("the phone");
    if (want && scr != SCR_CALL) {
        /* calls come first: whatever is showing gives way at once */
        under_call = scr == SCR_HISTORY || scr == SCR_CONTACTS || scr == SCR_KEYS ? SCR_IDLE : scr;
        if (scr == SCR_APP) {
            if (app_state() == APP_RUNNING) app_pause();
            else { app_quit(); under_call = SCR_LAUNCHER; }
        }
        show(SCR_CALL);
    } else if (!want && scr == SCR_CALL) {
        if (under_call == SCR_APP) {
            if (app_state() == APP_PAUSING || app_state() == APP_STOPPED) app_resume();
            else under_call = SCR_LAUNCHER;
        }
        show(under_call);
    } else if (scr == SCR_CALL) {
        redraw();
    }
}

void shell_phone_changed(void)
{
    if (scr != SCR_APP) redraw();
}

void shell_show_history(int on)
{
    if (on && scr != SCR_CALL && scr != SCR_APP) show(SCR_HISTORY);
    else if (!on && scr == SCR_HISTORY) show(SCR_IDLE);
}

void shell_show_contacts(int on)
{
    if (on && scr != SCR_CALL && scr != SCR_APP) show(SCR_CONTACTS);
    else if (!on && scr == SCR_CONTACTS) show(SCR_IDLE);
}

const char *shell_state(const char *key)
{
    static const char *const app_states[] = {"none", "starting", "running", "pausing", "stopped", "quitting"};
    if (!strcmp(key, "screen")) return screen_names[scr];
    if (!strcmp(key, "app")) return app_states[app_state()];
    if (!strcmp(key, "appid")) return app_id()[0] ? app_id() : "-";
    if (!strcmp(key, "flip")) return app_flip() >= 0 ? "on" : "off";
    if (!strncmp(key, "lamp.", 5)) return leds_shown(key + 5);
    if (!strncmp(key, "soft", 4) && key[4] >= '1' && key[4] <= '4' && !key[5]) return bar_state(key[4] - '1');
    if (!strcmp(key, "layout")) return SMALL ? "small" : "full";
    if (!strcmp(key, "appsize")) {
        static char size[16];
        snprintf(size, sizeof size, "%dx%d", app_w(), app_h());
        return app_state() == APP_NONE ? "-" : size;
    }
    if (!strcmp(key, "theme")) return T->name;
    if (!strcmp(key, "font")) return T->font;
    const char *v = contacts_state(key);
    if (!strcmp(key, "notice")) {
        static char note[2][96];
        notice_read(note);
        for (char *p = note[0]; *p; p++) if (*p == ' ') *p = '_';
        return note[0][0] ? note[0] : "-";
    }
    if (!strcmp(key, "notices")) {
        static char n[4];
        snprintf(n, sizeof n, "%d", idle_notices);
        return n;
    }
    if (!v) v = keytest_state(key);
    if (!v) v = watch_state(key);
    if (!v) v = settings_state(key);
    if (!v) v = backlight_state(key);
    return v ? v : phone_state(key);
}

/* ---------- main loop ---------- */

static void on_stop(int sig) { (void)sig; stopping = 1; }

static void on_chld(int sig)
{
    int e = errno;
    (void)sig;
    if (write(chld_pipe[1], "c", 1) < 0) {}
    errno = e;
}

static void usage(void)
{
    fprintf(stderr,
        "t48-shell [options]      the phone's shell: screen, input, launcher, apps\n"
        "t48-sim [options]        the same on a PC: screen in memory, input from a script\n"
        "  --sim            run as the simulator (the default when started as t48-sim)\n"
        "  --apps DIRS      folders of installed apps, colon separated (%s)\n"
        "  --data DIR       where each app's own folder is made (%s)\n"
        "  --config DIR     settings files (%s)\n"
        "  --state DIR      what the phone remembers, such as call history (%s)\n"
        "  --sip SOCKET     baresip's control socket (%s; none in the simulator unless given)\n"
        "  --audio SOCKET   the audio service's control socket (%s; the same)\n"
        "  --fb DEVICE      framebuffer (%s)\n"
        "  --apply PROGRAM  run with a settings file's name when it has changed (%s)\n"
        "  --update PROGRAM the updater behind Settings, Update (%s)\n"
        "  --access DIR     where t48-access leaves its status file (%s)\n"
        "  --notice FILE    one or two lines to show on the idle screen while it exists (%s)\n"
        "  --web DIR        where the web setup page leaves its status file (%s)\n"
        "  --app ID         start this app straight away\n"
        "simulator only:\n"
        "  --script FILE    input script, - for standard input (see docs/shell.md)\n"
        "  --shot FILE      write one picture (.png or .ppm) of the first screen and stop\n"
        "  --runner PROGRAM run apps with this, such as qemu-arm for apps built for the phone\n"
        "  --screen WxH     the screen's size (800x480); smaller gets the small layout\n"
        "  --no-touch       a screen that takes no touches, with four soft keys under it\n"
        "  --model t46s     that phone: 480x272 at 16 bits, upside down, no touch, soft keys\n"
        "also: t48-app (install, update and remove apps) and t48-config (settings), each with its own help\n",
        opt.apps, opt.data, opt.config, opt.state, opt.sip ? opt.sip : "none", opt.audio ? opt.audio : "none", opt.fb,
        opt.apply ? opt.apply : "none", opt.update ? opt.update : "none", opt.access, opt.notice ? opt.notice : "none",
        opt.web ? opt.web : "none");
}

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/');
    int sim = !strcmp(me ? me + 1 : argv[0], "t48-sim");
    static char sim_data[64];

    /* the same program is the two command-line tools */
    me = me ? me + 1 : argv[0];
    if (!strcmp(me, "t48-app") || !strcmp(me, "t48-config")) {
        log_quiet = !getenv("T48_DEBUG");
        return me[4] == 'a' ? pkg_main(argc, argv) : config_main(argc, argv);
    }

    for (int i = 1; i < argc; i++) sim |= !strcmp(argv[i], "--sim");
    if (sim) opt.sip = opt.audio = opt.apply = opt.update = opt.notice = opt.web = NULL;   /* a PC's own services stay out of it unless asked */
    for (int i = 1; i < argc; i++) {
        const char **dst = NULL;
        if (!strcmp(argv[i], "--sim")) continue;
        if (!strcmp(argv[i], "--no-touch")) { opt.no_touch = 1; continue; }
        else if (!strcmp(argv[i], "--apps")) dst = &opt.apps;
        else if (!strcmp(argv[i], "--data")) dst = &opt.data;
        else if (!strcmp(argv[i], "--config")) dst = &opt.config;
        else if (!strcmp(argv[i], "--state")) dst = &opt.state;
        else if (!strcmp(argv[i], "--sip")) dst = &opt.sip;
        else if (!strcmp(argv[i], "--audio")) dst = &opt.audio;
        else if (!strcmp(argv[i], "--fb")) dst = &opt.fb;
        else if (!strcmp(argv[i], "--app")) dst = &opt.start;
        else if (!strcmp(argv[i], "--script")) dst = &opt.script;
        else if (!strcmp(argv[i], "--shot")) dst = &opt.shot;
        else if (!strcmp(argv[i], "--apply")) dst = &opt.apply;
        else if (!strcmp(argv[i], "--update")) dst = &opt.update;
        else if (!strcmp(argv[i], "--runner")) dst = &opt.runner;
        else if (!strcmp(argv[i], "--access")) dst = &opt.access;
        else if (!strcmp(argv[i], "--notice")) dst = &opt.notice;
        else if (!strcmp(argv[i], "--web")) dst = &opt.web;
        else if (!strcmp(argv[i], "--screen")) dst = &opt.screen;
        else if (!strcmp(argv[i], "--model")) dst = &opt.model;
        if (!dst || i + 1 >= argc) { usage(); return 2; }
        *dst = argv[++i];
    }
    if (sim) {
        /* keep a PC's /data out of it unless asked */
        int data_set = 0, apps_set = 0, config_set = 0, state_set = 0, access_set = 0;
        for (int i = 1; i < argc; i++) {
            data_set |= !strcmp(argv[i], "--data");
            apps_set |= !strcmp(argv[i], "--apps");
            config_set |= !strcmp(argv[i], "--config");
            state_set |= !strcmp(argv[i], "--state");
            access_set |= !strcmp(argv[i], "--access");
        }
        snprintf(sim_data, sizeof sim_data, "/tmp/t48-sim-%d", (int)getuid());
        if (!data_set) opt.data = sim_data;
        if (!config_set) opt.config = sim_data;
        if (!state_set) opt.state = sim_data;
        if (!access_set) opt.access = sim_data;
        if (!apps_set) opt.apps = "apps";
    }
    be = sim ? &be_sim : &be_fb;

    struct sigaction sa = {0};
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (pipe(chld_pipe) < 0) { perror("pipe"); return 1; }
    fcntl(chld_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(chld_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(chld_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(chld_pipe[1], F_SETFD, FD_CLOEXEC);

    ui_init();
    settings_init();
    phone_init();
    if (be->init() < 0) return 1;
    backlight_init();
    logmsg("t48-shell on the %s, API level %d", be->name, T48_API_LEVEL);
    manifests_scan(opt.apps);
    redraw();
    if (opt.start) {
        const struct manifest *m = find_app(opt.start);
        if (m) start_app(m);
        else { logmsg("no app with id %s in %s", opt.start, opt.apps); shell_stop(1); }
    }

    long long next_clock = t48_now_ms() + 1000;
    while (!stopping) {
        struct pollfd pf[16];
        long long now = t48_now_ms();
        int n = 0, t, wait = (int)(next_clock > now ? next_clock - now : 0);

        if ((t = app_timeout(now)) >= 0 && t < wait) wait = t;
        if ((t = be->timeout(now)) >= 0 && t < wait) wait = t;
        if ((t = phone_timeout(now)) >= 0 && t < wait) wait = t;
        if ((t = sipc_timeout(now)) >= 0 && t < wait) wait = t;
        if ((t = audioc_timeout(now)) >= 0 && t < wait) wait = t;
        if (scr == SCR_SETTINGS && (t = settings_timeout(now)) >= 0 && t < wait) wait = t;
        if (scr == SCR_CONTACTS && (t = contacts_timeout(now)) >= 0 && t < wait) wait = t;
        if ((t = backlight_timeout(now)) >= 0 && t < wait) wait = t;
        if ((t = watch_timeout(now)) < wait) wait = t;
        if ((t = leds_timeout(now)) >= 0 && t < wait) wait = t;
        if (x_deadline && x_deadline - now < wait) wait = (int)(x_deadline > now ? x_deadline - now : 0);
        if (hold_deadline && hold_deadline - now < wait) wait = (int)(hold_deadline > now ? hold_deadline - now : 0);
        if (scr == SCR_KEYS && (t = keytest_timeout(now)) >= 0 && t < wait) wait = t;

        pf[n].fd = chld_pipe[0]; pf[n++].events = POLLIN;
        int app_at = -1;
        if (app_fd() >= 0) { app_at = n; pf[n].fd = app_fd(); pf[n++].events = POLLIN; }
        int sip_at = -1, audio_at = -1;
        if (sipc_fd() >= 0) { sip_at = n; pf[n].fd = sipc_fd(); pf[n++].events = POLLIN; }
        if (audioc_fd() >= 0) { audio_at = n; pf[n].fd = audioc_fd(); pf[n++].events = POLLIN; }
        int job_at = -1;
        if (settings_job_fd() >= 0) { job_at = n; pf[n].fd = settings_job_fd(); pf[n++].events = POLLIN; }
        int be_at = n;
        n += be->fds(pf + n, 16 - n);
        for (int i = 0; i < n; i++) pf[i].revents = 0;
        if (poll(pf, (nfds_t)n, wait) < 0) {
            if (errno != EINTR) { perror("poll"); break; }
            continue;
        }

        /* input first: a key must beat a busy app */
        if (n > be_at) be->handle(pf + be_at, n - be_at);
        if (sip_at >= 0 && pf[sip_at].revents && pf[sip_at].fd == sipc_fd()) sipc_readable();
        if (audio_at >= 0 && pf[audio_at].revents && pf[audio_at].fd == audioc_fd()) audioc_readable();
        if (app_at >= 0 && pf[app_at].revents && pf[app_at].fd == app_fd()) app_readable();
        if (job_at >= 0 && pf[job_at].revents && pf[job_at].fd == settings_job_fd()) settings_job_readable();
        if (pf[0].revents) {
            char junk[32];
            int status;
            pid_t pid;
            while (read(chld_pipe[0], junk, sizeof junk) > 0) {}
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
                if (!settings_reaped(pid, status)) app_reaped(pid, status);
        }

        now = t48_now_ms();
        app_tick(now);
        sipc_tick(now);
        audioc_tick(now);
        phone_tick(now);
        if (scr == SCR_SETTINGS) settings_tick(now);
        if (scr == SCR_CONTACTS) contacts_tick(now);
        be->tick(now);
        backlight_tick(now);
        watch_tick(now);
        leds_tick(now);
        if (x_deadline && now >= x_deadline && scr == SCR_APP) go_home();
        if (hold_deadline && now >= hold_deadline) { hold_deadline = 0; open_keytest(); }
        if (scr == SCR_KEYS && keytest_tick(now, &left_scan, &left_key)) { left_held = 1; show(SCR_IDLE); }
        if (now >= next_clock) {
            next_clock = now + 1000;
            char b[sizeof idle_shown];
            if (scr != SCR_APP && (idle_text(b, sizeof b), strcmp(b, idle_shown))) redraw();
        }
    }

    app_kill_now();
    be->fini();
    return exit_status;
}
