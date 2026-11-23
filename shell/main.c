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
};

enum screen { SCR_IDLE, SCR_LAUNCHER, SCR_APP, SCR_CALL, SCR_MESSAGE, SCR_HISTORY };
static const char *const screen_names[] = {"idle", "launcher", "app", "call", "message", "history"};

static const struct backend *be;
static uint32_t ui_px[SW * SH];
t48_surface ui = {ui_px, SW, SH, SW};
static t48_surface app_surface = {NULL, SW, SH, SW};

static enum screen scr = SCR_IDLE;
static enum screen under_call;       /* where to go when the handset goes back down */
static int app_shown;                /* the app in front has drawn since it came to the front */
static unsigned app_frames;
static int sel;                      /* launcher: selected app */
static char pending_id[64];          /* app to start once the old one has gone */
static char msg_title[96], msg_text[200];
static long long x_deadline;         /* in an app: when a held X becomes Home; 0 if X is up */
static char idle_shown[320];         /* time, date, network and account as last drawn, to redraw only on a change */
static volatile sig_atomic_t stopping;
static int exit_status;
static int chld_pipe[2];

void logmsg(const char *fmt, ...)
{
    va_list ap;
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

const char *shell_screen_name(void) { return screen_names[scr]; }
unsigned shell_app_frames(void) { return app_frames; }
void shell_stop(int status) { stopping = 1; exit_status = status; }

/* ---------- drawing the shell's own screens ---------- */

static void network_status(char *out, size_t n, int *up)
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
        snprintf(out, n, "%s  %s", a->ifa_name, ip);
        *up = 1;
        break;
    }
    freeifaddrs(list);
}

void ui_status_bar(const char *title)
{
    char net[80], hm[8], acc[120];
    uint32_t acc_colour;
    int up;
    time_t t = time(NULL);
    struct tm tm;

    t48_fill(&ui, 0, 0, SW, 44, C_BAR);
    t48_fill(&ui, 0, 44, SW, 1, C_LINE);
    if (title) {
        localtime_r(&t, &tm);
        strftime(hm, sizeof hm, "%H:%M", &tm);
        t48_text(&ui, 18, 6, &t48_font_head, C_TEXT, title);
        t48_text_right(&ui, SW - 18, 11, &t48_font_bold, C_DIM, hm);
    } else {
        network_status(net, sizeof net, &up);
        t48_round_fill(&ui, 18, 17, 10, 10, 5, up ? C_GOOD : C_BAD);
        t48_text(&ui, 36, 11, &t48_font_normal, C_TEXT, net);
        phone_account_text(acc, sizeof acc, &acc_colour);
        int w = t48_text_width(&t48_font_normal, acc);
        t48_text_right(&ui, SW - 18, 11, &t48_font_normal, C_TEXT, acc);
        t48_round_fill(&ui, SW - 18 - w - 20, 17, 10, 10, 5, acc_colour);
    }
}

void ui_button(int x, int y, int w, int h, const char *label, uint32_t fill)
{
    /* dark lettering on the bright fills, light on the dark ones */
    int bright = ((fill >> 16 & 255) * 3 + (fill >> 8 & 255) * 6 + (fill & 255)) / 10 > 120;
    t48_round_fill(&ui, x, y, w, h, 10, fill);
    t48_text_centre(&ui, x + w / 2, y + (h - t48_font_bold.height) / 2, &t48_font_bold, bright ? C_BG : C_TEXT, label);
}

#define IDLE_BTN_X 214
#define IDLE_BTN_Y 392
#define IDLE_BTN_W 180
#define IDLE_BTN_H 56
#define IDLE_CALLS_X 406

static void idle_text(char *out, size_t n)
{
    char net[80], acc[120];
    uint32_t c;
    int up;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    network_status(net, sizeof net, &up);
    phone_account_text(acc, sizeof acc, &c);
    size_t k = strftime(out, n, "%H:%M|%A %e %B %Y|", &tm);
    snprintf(out + k, n - k, "%s|%s|%d", net, acc, phone_missed());
}

static void draw_idle(void)
{
    char hm[8], date[64];
    time_t t = time(NULL);
    struct tm tm;

    localtime_r(&t, &tm);
    strftime(hm, sizeof hm, "%H:%M", &tm);
    strftime(date, sizeof date, "%A %e %B %Y", &tm);
    t48_fill(&ui, 0, 0, SW, SH, C_BG);
    ui_status_bar(NULL);
    t48_text_centre(&ui, SW / 2, 106, &t48_font_huge, C_TEXT, hm);
    t48_text_centre(&ui, SW / 2, 248, &t48_font_head, C_DIM, date);
    if (phone_missed()) {
        char b[40];
        snprintf(b, sizeof b, "%d missed call%s", phone_missed(), phone_missed() == 1 ? "" : "s");
        int w = t48_text_width(&t48_font_bold, b) + 44;
        t48_round_fill(&ui, (SW - w) / 2, 312, w, 42, 21, C_BAD);
        t48_text_centre(&ui, SW / 2, 312 + (42 - t48_font_bold.height) / 2, &t48_font_bold, C_BG, b);
    }
    ui_button(IDLE_BTN_X, IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H, "Apps", C_PANEL);
    ui_button(IDLE_CALLS_X, IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H, "Calls", C_PANEL);
    t48_text_centre(&ui, SW / 2, 456, &t48_font_small, C_FAINT, "OK for apps, Up for calls, or just dial");
}

/* launcher grid: 4 across, 2 down, a page at a time */
#define COLS 4
#define ROWS 2
#define TILE_W 170
#define TILE_H 188
#define TILE_GAP 20
#define GRID_X ((SW - COLS * TILE_W - (COLS - 1) * TILE_GAP) / 2)
#define GRID_Y 66

static const uint32_t tile_colours[] = {0x4CC2FF, 0xB68CFF, 0xE8C27A, 0x6FE3C1, 0xFF8FA3, 0x9AD0FF, 0xD7E36F, 0xFFB86B};

static void tile_box(int slot, int *x, int *y)
{
    *x = GRID_X + (slot % COLS) * (TILE_W + TILE_GAP);
    *y = GRID_Y + (slot / COLS) * (TILE_H + TILE_GAP);
}

static void draw_launcher(void)
{
    t48_fill(&ui, 0, 0, SW, SH, C_BG);
    ui_status_bar("Apps");
    if (!napps) {
        t48_text_centre(&ui, SW / 2, 200, &t48_font_head, C_DIM, "No apps installed");
        t48_text_centre(&ui, SW / 2, 244, &t48_font_normal, C_FAINT, "Press X to go back");
        return;
    }
    if (sel >= napps) sel = napps - 1;
    int per = COLS * ROWS, page = sel / per, pages = (napps + per - 1) / per;
    for (int slot = 0; slot < per; slot++) {
        int i = page * per + slot, x, y;
        if (i >= napps) break;
        const struct manifest *m = &apps[i];
        tile_box(slot, &x, &y);
        if (i == sel) t48_round_fill(&ui, x - 3, y - 3, TILE_W + 6, TILE_H + 6, 15, C_ACCENT);
        t48_round_fill(&ui, x, y, TILE_W, TILE_H, 12, C_PANEL);
        int ix = x + (TILE_W - 96) / 2, iy = y + 16;
        if (m->icon.px) {
            t48_blit(&ui, ix + (96 - m->icon.w) / 2, iy + (96 - m->icon.h) / 2, m->icon.px, m->icon.w, m->icon.h, m->icon.stride);
        } else {
            unsigned h = 0;
            char letter[2] = {m->name[0], 0};
            for (const char *p = m->id; *p; p++) h = h * 31 + (unsigned char)*p;
            if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
            t48_round_fill(&ui, ix, iy, 96, 96, 20, tile_colours[h % 8]);
            t48_text_centre(&ui, ix + 48, iy + 48 - t48_font_big.height / 2, &t48_font_big, C_BG, letter);
        }
        int tw = t48_text_width(&t48_font_bold, m->name);
        if (tw > TILE_W - 16) t48_text_fit(&ui, x + 8, y + 122, TILE_W - 16, &t48_font_bold, C_TEXT, m->name);
        else t48_text_centre(&ui, x + TILE_W / 2, y + 122, &t48_font_bold, C_TEXT, m->name);
        if (!strcmp(app_id(), m->id))
            t48_text_centre(&ui, x + TILE_W / 2, y + 152, &t48_font_small, C_GOOD, "open");
        else if (m->version[0])
            t48_text_centre(&ui, x + TILE_W / 2, y + 152, &t48_font_small, C_FAINT, m->version);
    }
    if (pages > 1) {
        char b[24];
        snprintf(b, sizeof b, "page %d of %d", page + 1, pages);
        t48_text_centre(&ui, SW / 2, 14, &t48_font_small, C_DIM, b);
    }
}

#define MSG_BTN_X 320
#define MSG_BTN_Y 300
#define MSG_BTN_W 160
#define MSG_BTN_H 56

static void draw_message(void)
{
    t48_fill(&ui, 0, 0, SW, SH, C_BG);
    ui_status_bar("Apps");
    t48_round_fill(&ui, 80, 96, 640, 290, 16, C_PANEL);
    t48_fill(&ui, 80, 112, 6, 258, C_BAD);
    t48_text_centre(&ui, SW / 2, 126, &t48_font_head, C_TEXT, msg_title);
    int w = t48_text_width(&t48_font_normal, msg_text);
    if (w > 600) t48_text_fit(&ui, 100, 196, 600, &t48_font_normal, C_DIM, msg_text);
    else t48_text_centre(&ui, SW / 2, 196, &t48_font_normal, C_DIM, msg_text);
    ui_button(MSG_BTN_X, MSG_BTN_Y, MSG_BTN_W, MSG_BTN_H, "OK", C_ACCENT);
}

static void draw_starting(void)
{
    char b[96];
    t48_fill(&ui, 0, 0, SW, SH, C_BG);
    snprintf(b, sizeof b, "Starting %s", app_id());
    t48_text_centre(&ui, SW / 2, 210, &t48_font_head, C_DIM, b);
}

static void redraw(void)
{
    idle_text(idle_shown, sizeof idle_shown);
    if (scr == SCR_APP && app_shown && app_pixels()) {
        be->flush(app_pixels(), SW, 0, 0, SW, SH);
        return;
    }
    switch (scr) {
    case SCR_IDLE: draw_idle(); break;
    case SCR_LAUNCHER: draw_launcher(); break;
    case SCR_CALL: phone_draw(); break;
    case SCR_HISTORY: phone_draw_history(); break;
    case SCR_MESSAGE: draw_message(); break;
    case SCR_APP: draw_starting(); break;
    }
    phone_draw_overlay();
    be->flush(ui_px, SW, 0, 0, SW, SH);
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
    app_surface.px = (uint32_t *)app_pixels();
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
    if (x >= SW || y >= SH) return;
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    be->flush(app_pixels(), SW, x, y, w, h);
}

void shell_app_gone(const char *name, const char *why)
{
    enum screen next = SCR_LAUNCHER;
    int was_front = scr == SCR_APP || (scr == SCR_CALL && under_call == SCR_APP);

    if (why) {
        snprintf(msg_title, sizeof msg_title, "%s has stopped", name);
        snprintf(msg_text, sizeof msg_text, "It %s.", why);
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
    case T48_KEY_RIGHT: if (sel < napps - 1) sel++; break;
    case T48_KEY_UP: if (sel >= COLS) sel -= COLS; break;
    case T48_KEY_DOWN:
        if (sel + COLS < napps) sel += COLS;
        else if (sel / per < (napps - 1) / per) sel = napps - 1;
        break;
    case T48_KEY_OK:
        if (sel < napps) open_app(&apps[sel]);
        return;
    case T48_KEY_BACK: show(SCR_IDLE); return;
    default: return;
    }
    redraw();
}

void shell_key(int code, int down)
{
    if (code >= SK_VOLDOWN) {
        /* phone keys stay with the shell, whatever is on the screen */
        phone_key(code, down);
        return;
    }
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
    }
}

int ui_inside(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void shell_touch(int state, int x, int y)
{
    if (scr == SCR_APP) { app_touch(state, x, y); return; }
    if (state != T48_TOUCH_DOWN) return;
    if (scr == SCR_CALL) {
        phone_touch(x, y);
    } else if (scr == SCR_HISTORY) {
        phone_history_touch(x, y);
    } else if (scr == SCR_IDLE && ui_inside(x, y, IDLE_BTN_X, IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H)) {
        shell_key(T48_KEY_OK, 1);
    } else if (scr == SCR_IDLE && ui_inside(x, y, IDLE_CALLS_X, IDLE_BTN_Y, IDLE_BTN_W, IDLE_BTN_H)) {
        phone_open_history();
    } else if (scr == SCR_MESSAGE && ui_inside(x, y, MSG_BTN_X, MSG_BTN_Y, MSG_BTN_W, MSG_BTN_H)) {
        show(SCR_LAUNCHER);
    } else if (scr == SCR_LAUNCHER) {
        int per = COLS * ROWS, page = sel / per;
        for (int slot = 0; slot < per && page * per + slot < napps; slot++) {
            int tx, ty;
            tile_box(slot, &tx, &ty);
            if (ui_inside(x, y, tx, ty, TILE_W, TILE_H)) {
                sel = page * per + slot;
                redraw();
                open_app(&apps[sel]);
                return;
            }
        }
    }
}

void shell_hook(int off_hook) { phone_hook(off_hook); }

/* ---------- the phone takes the screen, and gives it back ---------- */

void shell_phone_screen(int want)
{
    if (want && scr != SCR_CALL) {
        /* calls come first: whatever is showing gives way at once */
        under_call = scr == SCR_HISTORY ? SCR_IDLE : scr;
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
    if (scr == SCR_CALL || scr == SCR_HISTORY || scr == SCR_IDLE || scr == SCR_LAUNCHER || scr == SCR_MESSAGE) redraw();
}

void shell_show_history(int on)
{
    if (on && scr != SCR_CALL && scr != SCR_APP) show(SCR_HISTORY);
    else if (!on && scr == SCR_HISTORY) show(SCR_IDLE);
}

const char *shell_state(const char *key)
{
    static const char *const app_states[] = {"none", "starting", "running", "pausing", "stopped", "quitting"};
    if (!strcmp(key, "screen")) return screen_names[scr];
    if (!strcmp(key, "app")) return app_states[app_state()];
    return phone_state(key);
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
        "  --app ID         start this app straight away\n"
        "simulator only:\n"
        "  --script FILE    input script, - for standard input (see docs/shell.md)\n"
        "  --shot FILE      write one picture (.png or .ppm) of the first screen and stop\n",
        opt.apps, opt.data, opt.config, opt.state, opt.sip ? opt.sip : "none", opt.audio ? opt.audio : "none", opt.fb);
}

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/');
    int sim = !strcmp(me ? me + 1 : argv[0], "t48-sim");
    static char sim_data[64];

    for (int i = 1; i < argc; i++) sim |= !strcmp(argv[i], "--sim");
    if (sim) opt.sip = opt.audio = NULL;   /* a PC's own services stay out of it unless asked */
    for (int i = 1; i < argc; i++) {
        const char **dst = NULL;
        if (!strcmp(argv[i], "--sim")) continue;
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
        if (!dst || i + 1 >= argc) { usage(); return 2; }
        *dst = argv[++i];
    }
    if (sim) {
        /* keep a PC's /data out of it unless asked */
        int data_set = 0, apps_set = 0, config_set = 0, state_set = 0;
        for (int i = 1; i < argc; i++) {
            data_set |= !strcmp(argv[i], "--data");
            apps_set |= !strcmp(argv[i], "--apps");
            config_set |= !strcmp(argv[i], "--config");
            state_set |= !strcmp(argv[i], "--state");
        }
        snprintf(sim_data, sizeof sim_data, "/tmp/t48-sim-%d", (int)getuid());
        if (!data_set) opt.data = sim_data;
        if (!config_set) opt.config = sim_data;
        if (!state_set) opt.state = sim_data;
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

    phone_init();
    if (be->init() < 0) return 1;
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
        if (x_deadline && x_deadline - now < wait) wait = (int)(x_deadline > now ? x_deadline - now : 0);

        pf[n].fd = chld_pipe[0]; pf[n++].events = POLLIN;
        int app_at = -1;
        if (app_fd() >= 0) { app_at = n; pf[n].fd = app_fd(); pf[n++].events = POLLIN; }
        int sip_at = -1, audio_at = -1;
        if (sipc_fd() >= 0) { sip_at = n; pf[n].fd = sipc_fd(); pf[n++].events = POLLIN; }
        if (audioc_fd() >= 0) { audio_at = n; pf[n].fd = audioc_fd(); pf[n++].events = POLLIN; }
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
        if (pf[0].revents) {
            char junk[32];
            int status;
            pid_t pid;
            while (read(chld_pipe[0], junk, sizeof junk) > 0) {}
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) app_reaped(pid, status);
        }

        now = t48_now_ms();
        app_tick(now);
        sipc_tick(now);
        audioc_tick(now);
        phone_tick(now);
        be->tick(now);
        if (x_deadline && now >= x_deadline && scr == SCR_APP) go_home();
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
