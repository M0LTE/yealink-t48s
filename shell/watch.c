// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: noticing what other programs change (issue #37).
 *
 * The web setup page (webconfig/, docs/web.md) and t48-config write the same
 * settings files, contact list and call history as the screens do. Once a
 * second the shell looks at those files; when one is not the file it last
 * saw, the part of the shell that keeps it in memory reads it again, and
 * whatever is on the screen is drawn again. Every writer saves by writing a
 * new file and renaming it over the old one, so a change always shows as a
 * new file, and a look costs a couple of dozen stat calls.
 *
 * Also here: the web page's status file (opt.web), and the box that shows a
 * browser's pairing code on the screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "shell.h"

#define WATCH_MS 1000

/* what a file was when last looked at; all zero when it is not there */
struct sig { unsigned long long ino, size, sec, nsec; };

static int sig_changed(struct sig *s, const char *path)
{
    struct stat st;
    struct sig now = {0, 0, 0, 0};
    if (stat(path, &st) == 0) {
        now.ino = (unsigned long long)st.st_ino;
        now.size = (unsigned long long)st.st_size;
        now.sec = (unsigned long long)st.st_mtim.tv_sec;
        now.nsec = (unsigned long long)st.st_mtim.tv_nsec;
    }
    if (!memcmp(&now, s, sizeof now)) return 0;
    *s = now;
    return 1;
}

static const char *const phone_files[] = {"sip", "sip2", "sip3", "sip4", "sip5", "sip6", "phone", "audio"};
static const char *const other_files[] = {"network", "system", "screen", "access", "feed", "apps"};
#define NPHONE ((int)(sizeof phone_files / sizeof phone_files[0]))
#define NOTHER ((int)(sizeof other_files / sizeof other_files[0]))

static struct sig s_phone[NPHONE], s_other[NOTHER], s_contacts, s_history, s_apps[8], s_appconf, s_web;
static char appconf_of[64];
static int started, contacts_late, apps_late;
static long long next_look;

/* ---------- the web page's status file ---------- */

static struct {
    char url[80], from[48], code[12];
    int paired;
    long until;
} web;
static char dismissed[12];      /* a code the owner has refused: not shown again */

static void web_read(void)
{
    char path[300], line[160];
    memset(&web, 0, sizeof web);
    if (!opt.web) return;
    snprintf(path, sizeof path, "%s/status", opt.web);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        for (char *p = line; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
        if (!strncmp(line, "url=", 4)) snprintf(web.url, sizeof web.url, "%s", line + 4);
        else if (!strncmp(line, "paired=", 7)) web.paired = atoi(line + 7);
        else if (!strncmp(line, "pair_code=", 10)) snprintf(web.code, sizeof web.code, "%s", line + 10);
        else if (!strncmp(line, "pair_until=", 11)) web.until = atol(line + 11);
        else if (!strncmp(line, "pair_from=", 10)) snprintf(web.from, sizeof web.from, "%s", line + 10);
    }
    fclose(f);
}

const char *watch_web_url(void) { return web.url; }
int watch_web_paired(void) { return web.paired; }

/* is there a code to show right now */
static int pairing(void)
{
    return web.code[0] && strcmp(web.code, dismissed) && web.until > (long)time(NULL);
}

#define BOX_X 90
#define BOX_Y 96
#define BOX_W 620
#define BOX_H 288

void watch_draw_overlay(void)
{
    char spaced[16], from[96];
    struct row_ink ink;
    if (!pairing()) return;
    ui_dialog(BOX_X, BOX_Y, BOX_W, BOX_H, B_LIT);
    ui_dialog_ink(&ink);
    t48_text_centre(&ui, SW / 2, BOX_Y + 28, &t48_font_head, ink.text, "Pairing code");
    /* in two groups, as it is easier to carry across the room */
    size_t n = strlen(web.code), half = (n + 1) / 2;
    snprintf(spaced, sizeof spaced, "%.*s %s", (int)half, web.code, web.code + half);
    t48_text_centre(&ui, SW / 2, BOX_Y + 76, &t48_font_huge, ink.text, spaced);
    snprintf(from, sizeof from, "Requested by %s. X to refuse", web.from[0] ? web.from : "a browser");
    t48_text_centre(&ui, SW / 2, BOX_Y + BOX_H - 48, &t48_font_normal, ink.dim, from);
}

/* the owner says no: the server throws the code away when it sees this file */
static void refuse(void)
{
    char path[300];
    snprintf(dismissed, sizeof dismissed, "%s", web.code);
    snprintf(path, sizeof path, "%s/refuse", opt.web);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", dismissed); fclose(f); }
    logmsg("web: pairing refused at the screen");
    shell_phone_changed();
}

int watch_key(int code, int down)
{
    if (code != T48_KEY_BACK || !pairing()) return 0;
    if (down) refuse();
    return 1;
}

int watch_touch(int state, int x, int y)
{
    if (!pairing() || !ui_inside(x, y, BOX_X, BOX_Y, BOX_W, BOX_H)) return 0;
    if (state == T48_TOUCH_DOWN) refuse();
    return 1;
}

const char *watch_state(const char *key)
{
    static char b[16];
    if (!strcmp(key, "web.pairing")) return pairing() ? "shown" : "none";
    if (!strcmp(key, "web.paired")) { snprintf(b, sizeof b, "%d", web.paired); return b; }
    return NULL;
}

/* ---------- the look, once a second ---------- */

int watch_timeout(long long now)
{
    return next_look > now ? (int)(next_look - now) : 0;
}

void watch_tick(long long now)
{
    char path[400];
    int phone = 0, any = 0, contacts = 0, history = 0, apps = 0, k = 0;
    static int was_pairing;

    if (now < next_look) return;
    next_look = now + WATCH_MS;

    for (int i = 0; i < NPHONE; i++) {
        snprintf(path, sizeof path, "%s/%s", opt.config, phone_files[i]);
        phone |= sig_changed(&s_phone[i], path);
    }
    for (int i = 0; i < NOTHER; i++) {
        snprintf(path, sizeof path, "%s/%s", opt.config, other_files[i]);
        any |= sig_changed(&s_other[i], path);
    }
    snprintf(path, sizeof path, "%s/contacts", opt.state);
    contacts = sig_changed(&s_contacts, path);
    snprintf(path, sizeof path, "%s/history", opt.state);
    history = sig_changed(&s_history, path);
    /* the folders apps are installed in: installing and removing rename into and out of them */
    for (const char *d = opt.apps; *d && k < 8; k++) {
        size_t len = strcspn(d, ":");
        snprintf(path, sizeof path, "%.*s", (int)len, d);
        apps |= sig_changed(&s_apps[k], path);
        d += len + (d[len] == ':');
    }
    /* the settings of the app that is open: it is closed, and starts with the new ones next time */
    int same_app = !strcmp(appconf_of, app_id());
    snprintf(appconf_of, sizeof appconf_of, "%s", app_id());
    snprintf(path, sizeof path, "%s/apps/%s.conf", opt.config, appconf_of[0] ? appconf_of : ".none");
    int appconf = sig_changed(&s_appconf, path) && same_app && appconf_of[0];
    if (opt.web) {
        snprintf(path, sizeof path, "%s/status", opt.web);
        if (sig_changed(&s_web, path)) { web_read(); any = 1; }
    }
    /* a code runs out by the clock, not by a change to the file */
    int is_pairing = pairing();
    if (is_pairing != was_pairing) {
        if (is_pairing) backlight_wake("a browser asks to pair");
        was_pairing = is_pairing;
        shell_phone_changed();
    }

    if (!started) { started = 1; return; }   /* the first look only notes how things are */

    if (phone) phone_reload_files();
    if (history) phone_reload_history();
    if (contacts || contacts_late) contacts_late = !contacts_reload();
    if (apps || apps_late) {
        /* the app that is open was removed or updated: it is closed, as its files have gone */
        if (app_state() != APP_NONE && app_state() != APP_QUITTING && app_folder_changed()) {
            logmsg("watch: %s was removed or replaced while open: closing it", app_id());
            app_quit();
        }
        apps_late = app_state() != APP_NONE;   /* the list is not changed under a running app */
        if (!apps_late) manifests_scan(opt.apps);
    }
    if (appconf && app_state() != APP_NONE) {
        logmsg("watch: the settings of %s have changed: closing it", appconf_of);
        app_quit();
    }
    if (phone || any || contacts || history || apps) {
        logmsg("watch: files have changed, read again:%s%s%s%s%s", phone ? " phone" : "", any ? " settings" : "",
               contacts ? " contacts" : "", history ? " history" : "", apps ? " apps" : "");
        backlight_reload();
        int theme = ui_theme_reload();   /* before the screens are drawn again */
        settings_reload();
        if (theme) shell_theme_changed();
        shell_phone_changed();
    }
}
