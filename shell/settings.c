// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the settings screens (issue #23) and the app manager's screens
 * (issue #22). One list engine draws every page: rows that open another page,
 * text (edited with the on-screen keyboard), choices, switches, levels,
 * information and actions. Values live in the settings store (config.c).
 *
 * Installing, updating and removing apps is done by t48-app (pkg.c), run as a
 * child process of this same program; its progress lines come back on a pipe,
 * so the screen and the phone never wait on a download.
 *
 * Passwords: typed blind (the keyboard shows only the last character), shown
 * afterwards only as "set", and never written to the log.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "shell.h"

#ifndef T48_VERSION
#define T48_VERSION "development build"
#endif

enum page { P_ROOT, P_ACCOUNTS, P_SIP, P_NET, P_TIME, P_TZ, P_SOUND, P_SCREEN, P_ACCESS, P_ABOUT, P_APPSETTINGS, P_APPCONF, P_STORE, P_STOREAPP };
static const char *const page_names[] = {"root", "accounts", "sip", "network", "time", "timezone", "sound", "screen", "access", "about",
                                         "appsettings", "appconf", "store", "storeapp"};

enum row_kind { R_MENU, R_TEXT, R_CHOICE, R_TOGGLE, R_LEVEL, R_INFO, R_NOTE, R_ACTION };
enum { F_DISABLED = 1, F_PASSWORD = 2, F_DIGITS = 4, F_IP = 8, F_DANGER = 16 };
enum action { A_NONE, A_SET_TZ, A_REFRESH, A_INSTALL, A_UPDATE, A_REMOVE, A_OPEN, A_APP_DEFAULTS, A_FEED_DEFAULT, A_UPDATE_ALL,
              A_SSH, A_WEB, A_DOOR_OPEN, A_DOOR_SHOW, A_DOOR_CLOSE, A_FORGET_KEYS, A_ACCOUNT_REMOVE };

struct row {
    int kind, flags;
    char label[96];
    char value[200];       /* what shows on the right; for R_NOTE the whole text */
    char file[80], key[48];
    char def[160];         /* what the value is when the store has none */
    const char *choices;   /* R_CHOICE: "dhcp|static" */
    int target;            /* R_MENU: page. R_ACTION: action. R_LEVEL: which volume, or -1 the backlight */
    char arg[64];
    int level, level_max;
};

#define MAX_ROWS 96
#define VISIBLE 7
#define ROW_H 60
#define LIST_Y 52
#define LIST_X 16

static struct row rows[MAX_ROWS];
static int nrows;
static char title[96];
static struct { enum page page; char arg[64]; int sel, top; } stack[8];
static int depth = -1;
static int came_from_launcher;
static char dirty[24];            /* settings file changed on this page, applied on leaving it */

static t48_kbd kbd;
static int kbd_open;
static struct row kbd_row;

enum overlay_kind { O_NONE, O_BUSY, O_MSG, O_CONFIRM, O_DOOR };
static struct {
    enum overlay_kind kind;
    char title[96], text[300];
    char extra[120];              /* O_CONFIRM: one more line in small print, such as a key's fingerprint */
    const char *yes, *no;         /* O_CONFIRM: what the two buttons say */
    int percent;
    enum action act;
    char arg[64];
} ov;

/* ---------- time zones ---------- */

#include "zones.h"

/* the TZ string for what the store holds: a listed name, or the string itself */
const char *settings_tz(void)
{
    const char *name = cfg_str("system", "timezone", "UTC");
    for (int i = 0; i < NZONES; i++)
        if (!strcmp(zones[i].name, name)) return zones[i].tz;
    return name[0] ? name : "UTC0";
}

static void apply_tz(void)
{
    setenv("TZ", settings_tz(), 1);   /* apps started from now on get it too */
    tzset();
}

/* ---------- making the running system follow a changed file ---------- */

/* input, if given, is one line for the program's standard input: a secret
 * goes that way, never on a command line, which any program can read */
static void run_apply_with(const char *what, const char *input)
{
    int p[2] = {-1, -1};
    logmsg("settings: %s", what);
    if (!strcmp(what, "sip") || !strcmp(what, "phone")) phone_reload_account();
    if (!strcmp(what, "phone")) return;   /* the shell's own file: nothing else follows it */
    if (!opt.apply || !opt.apply[0]) return;
    if (input && pipe(p) < 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        if (input) dup2(p[0], 0);
        for (int fd = 3; fd < 256; fd++) close(fd);
        execl(opt.apply, opt.apply, what, (char *)NULL);
        _exit(127);
    }
    if (input) {
        close(p[0]);
        if (pid > 0 && (write(p[1], input, strlen(input)) < 0 || write(p[1], "\n", 1) < 0)) {}
        close(p[1]);
    }
}

static void run_apply(const char *what) { run_apply_with(what, NULL); }

void settings_init(void)
{
    apply_tz();
}

static void rebuild(void);

/* the store has been changed by another program (watch.c): the web page, or t48-config */
void settings_reload(void)
{
    apply_tz();
    if (depth >= 0 && !kbd_open) rebuild();
}

/* ---------- access: SSH, the host key, the owner's keys (issue #28) ---------- */

/* What t48-access last wrote in its status file (docs/access.md). Nothing is
 * known until it has run, which in the simulator is never unless --apply
 * names a program that plays its part. */
#define MAX_KEYS 8
static struct {
    char raw[1500];
    int known, dev, running, door, nkeys;
    long door_until;
    char host_key[100];
    char key[MAX_KEYS][80];
} acc;
static char door_password[20];    /* shown while password login is open; never logged, never stored */
static int door_keys_before;
static long long door_asked;      /* when password login was asked for */
static long long acc_checked;

/* 1 if it has changed since the last look */
static int access_read(void)
{
    char path[300], raw[sizeof acc.raw] = "";
    snprintf(path, sizeof path, "%s/status", opt.access);
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(raw, 1, sizeof raw - 1, f);
        raw[n] = 0;
        fclose(f);
    }
    if (acc.known == (f != NULL) && !strcmp(raw, acc.raw)) return 0;
    memset(&acc, 0, sizeof acc);
    snprintf(acc.raw, sizeof acc.raw, "%s", raw);
    acc.known = f != NULL;
    int listed = 0;
    for (char *line = strtok(raw, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strcmp(line, "dev=yes")) acc.dev = 1;
        else if (!strcmp(line, "ssh=on")) acc.running = 1;
        else if (!strcmp(line, "door=open")) acc.door = 1;
        else if (!strncmp(line, "door_until=", 11)) acc.door_until = atol(line + 11);
        else if (!strncmp(line, "host_key=", 9)) snprintf(acc.host_key, sizeof acc.host_key, "%s", line + 9);
        else if (!strncmp(line, "keys=", 5)) acc.nkeys = atoi(line + 5);
        else if (!strncmp(line, "key=", 4) && listed < MAX_KEYS) snprintf(acc.key[listed++], sizeof acc.key[0], "%s", line + 4);
    }
    return 1;
}

/* a password to read off the screen and type once: 12 letters and digits
 * that cannot be mistaken for each other, in three groups */
static int make_password(char *out)
{
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";   /* 31 of them */
    unsigned char r[12];
    FILE *f = fopen("/dev/urandom", "r");
    if (!f) return -1;
    size_t got = fread(r, 1, sizeof r, f);
    fclose(f);
    if (got != sizeof r) return -1;
    char *p = out;
    for (int i = 0; i < 12; i++) {
        if (i && i % 4 == 0) *p++ = '-';
        *p++ = alphabet[r[i] % 31];
    }
    *p = 0;
    return 0;
}

static void door_left(char *out, size_t n)
{
    long left = acc.door_until - (long)time(NULL);
    if (left < 0) left = 0;
    snprintf(out, n, "%ld:%02ld", left / 60, left % 60);
}

/* ---------- the app manager's child process ---------- */

struct store_app {
    char id[64], name[64], have[24], offered[24], state[16], perms[96], description[160];
    long pkg_bytes, disk_bytes;
};
static struct store_app store[128];
static int nstore;
static enum { ST_NONE, ST_LOADING, ST_LOADED } store_state;
static char feed_error[200];

static struct {
    pid_t pid;
    int fd;
    char buf[2048];
    size_t n;
    enum { J_LIST, J_CHANGE } kind;
    char verb[24], name[64];
    char result[300];
    int ok, listed, blank;   /* blank: nothing was listed when this list was asked for */
} job = {.fd = -1};

static void rebuild(void);
static void redraw_me(void) { shell_settings_changed(); }

static const char *feed_now(void) { return cfg_str("feed", "url", pkg_default_feed()); }

static int job_start(int kind, const char *cmd, const char *arg)
{
    int p[2];
    if (job.pid > 0) return -1;
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], 1);
        for (int fd = 3; fd < 256; fd++) close(fd);
        signal(SIGPIPE, SIG_DFL);
        execl("/proc/self/exe", "t48-app", "--machine", "--apps", opt.apps, "--config", opt.config, "--data", opt.data,
              cmd, arg, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    job.pid = pid;
    job.fd = p[0];
    job.n = 0;
    job.kind = kind;
    job.ok = job.listed = 0;
    job.result[0] = 0;
    if (kind == J_LIST) { feed_error[0] = 0; job.blank = !nstore; }
    logmsg("apps: t48-app %s %s", cmd, arg ? arg : "");
    return 0;
}

static void store_refresh(void)
{
    if (job.pid > 0) return;
    /* what was listed before stays on the screen until the new list starts to arrive */
    store_state = job_start(J_LIST, "list", NULL) == 0 ? ST_LOADING : ST_NONE;
}

static const struct store_app *store_find(const char *id)
{
    for (int i = 0; i < nstore; i++)
        if (!strcmp(store[i].id, id)) return &store[i];
    return NULL;
}

static void job_line(char *line)
{
    if (!strncmp(line, "app\t", 4) && job.kind == J_LIST) {
        if (!job.listed) { nstore = 0; job.listed = 1; }
        if (nstore >= 128) return;
        struct store_app *a = &store[nstore];
        char *f[9] = {0}, *p = line + 4;
        for (int i = 0; i < 9 && p; i++) {
            f[i] = p;
            p = strchr(p, '\t');
            if (p) *p++ = 0;
        }
        if (!f[8]) return;
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", f[0]);
        snprintf(a->name, sizeof a->name, "%s", f[1]);
        snprintf(a->have, sizeof a->have, "%s", f[2]);
        snprintf(a->offered, sizeof a->offered, "%s", f[3]);
        a->pkg_bytes = atol(f[4]);
        a->disk_bytes = atol(f[5]);
        snprintf(a->state, sizeof a->state, "%s", f[6]);
        snprintf(a->perms, sizeof a->perms, "%s", strcmp(f[7], "-") ? f[7] : "");
        snprintf(a->description, sizeof a->description, "%s", strcmp(f[8], "-") ? f[8] : "");
        nstore++;
    } else if (!strncmp(line, "feederror ", 10)) {
        snprintf(feed_error, sizeof feed_error, "%s", line + 10);
    } else if (!strncmp(line, "progress ", 9) && job.kind == J_CHANGE && ov.kind == O_BUSY) {
        char what[24] = "";
        int pc = -1;
        sscanf(line + 9, "%23s %d", what, &pc);
        ov.percent = pc;
        const char *doing = !strcmp(what, "index") ? "Reading the feed" : !strcmp(what, "download") ? "Downloading" :
                            !strcmp(what, "verify") ? "Checking it is the real thing" : !strcmp(what, "unpack") ? "Unpacking" : what;
        snprintf(ov.text, sizeof ov.text, "%s", doing);
        redraw_me();
    } else if (!strncmp(line, "done ", 5)) {
        job.ok = 1;
        snprintf(job.result, sizeof job.result, "%s", line + 5);
    } else if (!strncmp(line, "error ", 6)) {
        snprintf(job.result, sizeof job.result, "%s", line + 6);
    }
}

int settings_job_fd(void) { return job.pid > 0 ? job.fd : -1; }

void settings_job_readable(void)
{
    for (;;) {
        size_t at = job.n < sizeof job.buf - 1 ? job.n : 0;   /* a line too long to be ours is dropped */
        ssize_t k = read(job.fd, job.buf + at, sizeof job.buf - 1 - at);
        job.n = at;
        if (k <= 0) {
            if (k == 0) { close(job.fd); job.fd = -1; }
            break;
        }
        job.n += (size_t)k;
        job.buf[job.n] = 0;
        char *start = job.buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            job_line(start);
            start = nl + 1;
        }
        job.n = strlen(start);
        memmove(job.buf, start, job.n + 1);
    }
}

static void message(const char *t, const char *text)
{
    memset(&ov, 0, sizeof ov);
    ov.kind = O_MSG;
    snprintf(ov.title, sizeof ov.title, "%s", t);
    snprintf(ov.text, sizeof ov.text, "%s", text);
}

int settings_reaped(pid_t pid, int status)
{
    if (job.pid <= 0 || pid != job.pid) return 0;
    if (job.fd >= 0) { settings_job_readable(); if (job.fd >= 0) close(job.fd); }
    job.fd = -1;
    job.pid = 0;
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (job.kind == J_LIST) {
        store_state = ST_LOADED;
        if (!job.listed) nstore = 0;
        if (!ok && !feed_error[0]) snprintf(feed_error, sizeof feed_error, "the app manager stopped unexpectedly");
        logmsg("apps: %d listed%s%s", nstore, feed_error[0] ? "; feed: " : "", feed_error);
        /* the list has just appeared where there was none: start at its top */
        if (depth >= 0 && stack[depth].page == P_STORE && job.blank) stack[depth].sel = stack[depth].top = 0;
    } else {
        char t[120];
        ok = ok && job.ok;
        logmsg("apps: %s %s: %s", job.verb, job.name, ok ? "done" : job.result[0] ? job.result : "failed");
        if (ok) {
            snprintf(t, sizeof t, "%s %s", job.name, job.verb);
            message(t, !strcmp(job.verb, "removed") ? "The app, what it kept and its settings have gone." :
                       "It is in the Apps list, ready to open.");
        } else {
            snprintf(t, sizeof t, "%s was not %s", job.name, job.verb);
            if (job.result[0] >= 'a' && job.result[0] <= 'z') job.result[0] -= 32;
            message(t, job.result[0] ? job.result : "The app manager stopped unexpectedly.");
        }
        manifests_scan(opt.apps);
        store_refresh();
        /* a removed app's page has nothing left to show */
        if (ok && !strcmp(job.verb, "removed") && depth > 0 && stack[depth].page == P_STOREAPP) depth--;
    }
    if (depth >= 0) { rebuild(); redraw_me(); }
    return 1;
}

static void change_app(enum action act, const char *id)
{
    const struct store_app *a = store_find(id);
    const char *cmd = act == A_REMOVE ? "remove" : act == A_UPDATE ? "update" : "install";
    if (job.pid > 0) { message("Busy", "Another app is being installed. Try again in a moment."); return; }
    /* an app cannot be changed under its own feet */
    if (!strcmp(app_id(), id)) app_quit();
    snprintf(job.verb, sizeof job.verb, "%s", act == A_REMOVE ? "removed" : act == A_UPDATE ? "updated" : "installed");
    snprintf(job.name, sizeof job.name, "%s", a ? a->name : id);
    if (job_start(J_CHANGE, cmd, id) < 0) { message("Could not start", "The app manager could not be run."); return; }
    memset(&ov, 0, sizeof ov);
    ov.kind = O_BUSY;
    ov.percent = -1;
    snprintf(ov.title, sizeof ov.title, "%s %s", act == A_REMOVE ? "Removing" : act == A_UPDATE ? "Updating" : "Installing", job.name);
    snprintf(ov.text, sizeof ov.text, "Starting");
}

/* ---------- building pages ---------- */

static struct row *add(int kind, const char *label)
{
    struct row *r = &rows[nrows < MAX_ROWS ? nrows++ : MAX_ROWS - 1];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    snprintf(r->label, sizeof r->label, "%s", label);
    return r;
}

static struct row *add_menu(const char *label, enum page target, const char *value)
{
    struct row *r = add(R_MENU, label);
    r->target = target;
    snprintf(r->value, sizeof r->value, "%s", value ? value : "");
    return r;
}

static struct row *add_stored(int kind, const char *label, const char *file, const char *key, const char *def, int flags)
{
    struct row *r = add(kind, label);
    snprintf(r->file, sizeof r->file, "%s", file);
    snprintf(r->key, sizeof r->key, "%s", key);
    snprintf(r->def, sizeof r->def, "%s", def);
    r->flags = flags;
    if (flags & F_PASSWORD) snprintf(r->value, sizeof r->value, "%s", cfg_str(file, key, "")[0] ? "set" : "not set");
    else snprintf(r->value, sizeof r->value, "%s", cfg_str(file, key, def));
    return r;
}

static struct row *add_info(const char *label, const char *value)
{
    struct row *r = add(R_INFO, label);
    snprintf(r->value, sizeof r->value, "%s", value);
    return r;
}

static struct row *add_action(const char *label, enum action act, const char *arg)
{
    struct row *r = add(R_ACTION, label);
    r->target = act;
    snprintf(r->arg, sizeof r->arg, "%s", arg ? arg : "");
    return r;
}

static void kb(char *out, size_t n, long bytes)
{
    if (bytes >= 1024 * 1024) snprintf(out, n, "%ld.%ld MB", bytes / 1048576, bytes % 1048576 * 10 / 1048576);
    else snprintf(out, n, "%ld KB", (bytes + 1023) / 1024);
}

static const struct manifest *installed(const char *id)
{
    for (int i = 0; i < napps; i++)
        if (!strcmp(apps[i].id, id)) return &apps[i];
    return NULL;
}

static void build_root(void)
{
    char v[200], user[120];
    int up;
    snprintf(title, sizeof title, "Settings");
    snprintf(user, sizeof user, "%s", cfg_str("sip", "user", ""));
    if (user[0]) snprintf(v, sizeof v, "%s@%s", user, cfg_str("sip", "domain", "?"));
    else snprintf(v, sizeof v, "not set");
    int extra = 0;
    for (int i = 2; i <= 6; i++) {
        char f[8];
        snprintf(f, sizeof f, "sip%d", i);
        if (cfg_str(f, "user", "")[0]) extra++;
    }
    if (extra && user[0]) { size_t at = strlen(v); snprintf(v + at, sizeof v - at, " and %d more", extra); }
    else if (extra) snprintf(v, sizeof v, "%d set", extra);
    add_menu("Phone accounts", P_ACCOUNTS, v);
    ui_network_status(v, sizeof v, &up);
    add_menu("Network", P_NET, v);
    add_menu("Date and time", P_TIME, cfg_str("system", "timezone", "UTC"));
    add_menu("Sound", P_SOUND, "");
    add_menu("Screen", P_SCREEN, "");
    access_read();
    add_menu("Access", P_ACCESS, acc.dev || !strcmp(cfg_str("access", "ssh", "off"), "on") ? "SSH on" : "SSH off");
    snprintf(v, sizeof v, "%d installed", napps);
    add_menu("Apps", P_STORE, v);
    add_menu("App settings", P_APPSETTINGS, "");
    add_menu("About", P_ABOUT, "");
}

/* the settings file of account 1 to 6: sip, sip2, ... sip6 */
static const char *sip_file(int n)
{
    static char f[4][8];
    static int at;
    char *o = f[at++ & 3];
    if (n <= 1) snprintf(o, 8, "sip");
    else snprintf(o, 8, "sip%d", n);
    return o;
}

static int sip_usable(int n)
{
    const char *f = sip_file(n);
    char user[64] = "", domain[64] = "", on[8] = "on";
    cfg_get(f, "user", user, sizeof user);
    cfg_get(f, "domain", domain, sizeof domain);
    cfg_get(f, "enabled", on, sizeof on);
    return user[0] && domain[0] && strcmp(on, "off");
}

/* up to six accounts, and which one calls go out from when nothing else is said */
static void build_accounts(void)
{
    char label[40], v[200], choices[24] = "";
    snprintf(title, sizeof title, "Phone accounts");
    for (int n = 1; n <= 6; n++) {
        const char *f = sip_file(n);
        char user[64] = "", domain[64] = "?", name[40] = "", on[8] = "on";
        cfg_get(f, "user", user, sizeof user);
        cfg_get(f, "domain", domain, sizeof domain);
        cfg_get(f, "label", name, sizeof name);
        cfg_get(f, "enabled", on, sizeof on);
        snprintf(label, sizeof label, "Account %d", n);
        if (!user[0]) snprintf(v, sizeof v, "not set");
        else snprintf(v, sizeof v, "%s%s%s@%s%s", name, name[0] ? ": " : "", user, domain, strcmp(on, "off") ? "" : " (off)");
        snprintf(add_menu(label, P_SIP, v)->arg, sizeof rows[0].arg, "%d", n);
        if (sip_usable(n)) { size_t at = strlen(choices); snprintf(choices + at, sizeof choices - at, "%s%d", at ? "|" : "", n); }
    }
    if (strchr(choices, '|')) {
        static char keep[24];
        struct row *r;
        snprintf(keep, sizeof keep, "%s", choices);
        snprintf(v, sizeof v, "%d", atoi(phone_state("acct.default")));
        r = add_stored(R_CHOICE, "Usual account for calls", "phone", "default_account", v, 0);
        r->choices = keep;
        if (!strstr(keep, r->value)) snprintf(r->value, sizeof r->value, "%s", v);
    }
    snprintf(add(R_NOTE, "")->value, sizeof rows[0].value,
             "Each account rings this phone. In the dialler, Left and Right choose which one a call goes out from.");
}

static void build_sip(const char *arg)
{
    char acc[120];
    int n = atoi(arg);
    const char *f = sip_file(n < 1 ? 1 : n);
    if (n < 1) n = 1;
    snprintf(title, sizeof title, "Account %d", n);
    add_stored(R_TEXT, "User", f, "user", "", 0);
    add_stored(R_TEXT, "Password", f, "password", "", F_PASSWORD);
    add_stored(R_TEXT, "Domain", f, "domain", "", 0);
    add_stored(R_TEXT, "Server or proxy (optional)", f, "outbound_proxy", "", 0);
    add_stored(R_TEXT, "Login name, if not the user", f, "auth_user", "", 0);
    add_stored(R_TEXT, "Name shown to others", f, "display_name", "", 0);
    add_stored(R_CHOICE, "Transport", f, "transport", "udp", 0)->choices = "udp|tcp|tls";
    add_stored(R_TEXT, "Name on this phone (optional)", f, "label", "", 0);
    add_stored(R_TEXT, "Voicemail number (the Message key)", f, "voicemail", "", F_DIGITS);
    add_stored(R_TOGGLE, "In use", f, "enabled", "on", 0);
    snprintf(acc, sizeof acc, "reg.%d", n);
    const char *reg = phone_state(acc);
    add_info("Now", !strcmp(phone_state("sip"), "up") ? (!strcmp(reg, "ok") ? "registered" : !strcmp(reg, "fail") ? "not registered" :
             !strcmp(reg, "trying") ? "registering" : "not in use") : "no phone service");
    if (cfg_str(f, "user", "")[0] || cfg_str(f, "domain", "")[0])
        snprintf(add_action("Remove this account", A_ACCOUNT_REMOVE, "")->def, sizeof rows[0].def, "%s", f);
    rows[nrows - 1].flags |= rows[nrows - 1].kind == R_ACTION ? F_DANGER : 0;
}

static void build_net(void)
{
    char v[100];
    int up, fixed = !strcmp(cfg_str("network", "mode", "dhcp"), "static");
    snprintf(title, sizeof title, "Network");
    add_stored(R_CHOICE, "Address from", "network", "mode", "dhcp", 0)->choices = "dhcp|static";
    add_stored(R_TEXT, "Address", "network", "address", "", F_DIGITS | F_IP | (fixed ? 0 : F_DISABLED));
    add_stored(R_TEXT, "Netmask", "network", "netmask", "255.255.255.0", F_DIGITS | F_IP | (fixed ? 0 : F_DISABLED));
    add_stored(R_TEXT, "Gateway", "network", "gateway", "", F_DIGITS | F_IP | (fixed ? 0 : F_DISABLED));
    add_stored(R_TEXT, "DNS server", "network", "dns", "", F_DIGITS | F_IP | (fixed ? 0 : F_DISABLED));
    ui_network_status(v, sizeof v, &up);
    add_info("Now", v);
    add(R_NOTE, "")->value[0] = 0;
    snprintf(rows[nrows - 1].value, sizeof rows[0].value, "%s",
             fixed ? "Changes take effect when you leave this page." :
                     "The address, netmask, gateway and DNS come from the network (DHCP).");
}

static void build_time(void)
{
    char now[80];
    time_t t = time(NULL);
    struct tm tm;
    snprintf(title, sizeof title, "Date and time");
    add_menu("Time zone", P_TZ, cfg_str("system", "timezone", "UTC"));
    localtime_r(&t, &tm);
    strftime(now, sizeof now, "%H:%M, %A %e %B %Y", &tm);
    add_info("Now", now);
    snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "The clock is set from the network (NTP).");
}

static void build_tz(void)
{
    const char *cur = cfg_str("system", "timezone", "UTC");
    snprintf(title, sizeof title, "Time zone");
    for (int i = 0; i < NZONES; i++) {
        struct row *r = add_action(zones[i].name, A_SET_TZ, "");
        snprintf(r->def, sizeof r->def, "%s", zones[i].name);
        if (!strcmp(cur, zones[i].name)) snprintf(r->value, sizeof r->value, "current");
    }
}

static void build_sound(void)
{
    static const char *const names[] = {"", "Handset", "Speakerphone", "Headset", "Ringer"};
    static const int order[] = {T48AC_VOL_RINGER, T48AC_VOL_HANDSET, T48AC_VOL_SPEAKER, T48AC_VOL_HEADSET};
    snprintf(title, sizeof title, "Sound");
    for (int i = 0; i < 4; i++) {
        struct row *r = add(R_LEVEL, names[order[i]]);
        r->target = order[i];
        r->level = phone_volume(order[i]);
        r->level_max = T48AC_VOL_MAX;
    }
    add_info("Audio service", audioc_connected() ? "running" : "not running");
    snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Ringer at 0 is silent. The volume keys change the same levels.");
}

static void build_screen(void)
{
    int have = backlight_present();
    snprintf(title, sizeof title, "Screen");
    struct row *r = add(R_LEVEL, "Brightness");
    r->target = -1;
    r->level_max = BL_LEVELS;
    r->level = backlight_level();
    if (!have) r->flags = F_DISABLED;
    snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "%s",
             have ? "The screen dims when nothing has been touched for a while, and later switches off. Any key or a touch brings it back." :
                    "Not available here: there is no backlight to set.");
}

static void build_access(void)
{
    char v[200];
    int up;
    access_read();
    snprintf(title, sizeof title, "Access");
    int on = acc.dev || !strcmp(cfg_str("access", "ssh", "off"), "on");
    struct row *r = add_action("SSH", A_SSH, "");
    r->kind = R_TOGGLE;
    snprintf(r->value, sizeof r->value, "%s", on ? "on" : "off");
    if (acc.dev) {
        r->flags = F_DISABLED;
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value,
                 "A development build: SSH is always on, and the keys built into this image can always log in.");
    }
    if (on) {
        char net[80];
        ui_network_status(net, sizeof net, &up);
        const char *ip = strrchr(net, ' ');
        snprintf(v, sizeof v, "ssh root@%s", up && ip ? ip + 1 : "this phone's address");
        add_info("Log in with", !acc.known || acc.running ? v : "SSH is not running");
    }
    snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "This phone's host key: %s",
             acc.host_key[0] ? acc.host_key : "not known yet");
    if (!acc.nkeys) add_info("Keys that can log in", on ? "none yet: nobody can log in" : "none");
    for (int i = 0; i < acc.nkeys && i < MAX_KEYS && acc.key[i][0]; i++) {
        snprintf(v, sizeof v, "Key %d", i + 1);
        add_info(v, acc.key[i]);
    }
    if (acc.nkeys > MAX_KEYS) { snprintf(v, sizeof v, "and %d more", acc.nkeys - MAX_KEYS); add_info("", v); }
    if (acc.door) {
        door_left(v, sizeof v);
        snprintf(add_action("Password login is open", A_DOOR_SHOW, "")->value, sizeof rows[0].value, "%s left", v);
        add_action("Close password login now", A_DOOR_CLOSE, "");
    } else {
        r = add_action("Add a key from your computer", A_DOOR_OPEN, "");
        if (!on) r->flags = F_DISABLED;
    }
    if (acc.nkeys) add_action(acc.dev ? "Forget the keys that were added" : "Forget all keys", A_FORGET_KEYS, "")->flags = F_DANGER;
    if (!on) snprintf(add(R_NOTE, "")->value, sizeof rows[0].value,
                      "SSH gives full control of the phone over the network. It is off until you turn it on here.");
    /* the web setup page (issue #37, docs/web.md) */
    int web = !strcmp(cfg_str("access", "web", "off"), "on");
    r = add_action("Web setup page", A_WEB, "");
    r->kind = R_TOGGLE;
    snprintf(r->value, sizeof r->value, "%s", web ? "on" : "off");
    if (web) {
        char net[80];
        ui_network_status(net, sizeof net, &up);
        const char *ip = strrchr(net, ' ');
        if (watch_web_url()[0]) snprintf(v, sizeof v, "%s", watch_web_url());
        else if (up && ip) snprintf(v, sizeof v, "http://%s/", ip + 1);
        else snprintf(v, sizeof v, "this phone's address");
        add_info("Open in a browser", v);
        snprintf(v, sizeof v, "%d", watch_web_paired());
        add_info("Browsers paired", v);
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value,
                 "A new browser must type a code that is shown on this screen. Off and on again forgets every browser.");
    } else {
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value,
                 "Set the phone up from a computer's browser. It is off until you turn it on here.");
    }
}

static void build_about(void)
{
    char v[200], path[300];
    struct utsname u;
    struct statvfs vfs;
    FILE *f;
    int up;
    snprintf(title, sizeof title, "About");
    add_info("Phone", "Yealink SIP-T48S, open firmware");
    snprintf(v, sizeof v, "%s", T48_VERSION);
    if ((f = fopen("/etc/t48-release", "r"))) {
        if (fgets(v, sizeof v, f)) v[strcspn(v, "\r\n")] = 0;
        fclose(f);
    }
    add_info("Firmware", v);
    add_info("Shell", T48_VERSION ", built " __DATE__);
    snprintf(v, sizeof v, "%d", T48_API_LEVEL);
    add_info("App API level", v);
    if (uname(&u) == 0) { snprintf(v, sizeof v, "%s %s", u.sysname, u.release); add_info("Kernel", v); }
    ui_network_status(v, sizeof v, &up);
    add_info("Network", v);
    if ((f = fopen("/sys/class/net/eth0/address", "r"))) {
        if (fgets(v, sizeof v, f)) { v[strcspn(v, "\r\n")] = 0; add_info("MAC address", v); }
        fclose(f);
    }
    if ((f = fopen("/proc/meminfo", "r"))) {
        long total = 0, avail = 0, fr = 0;
        char line[120];
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %ld", &total);
            sscanf(line, "MemFree: %ld", &fr);
            sscanf(line, "MemAvailable: %ld", &avail);
        }
        fclose(f);
        snprintf(v, sizeof v, "%ld MB free of %ld MB", (avail ? avail : fr) / 1024, total / 1024);
        add_info("Memory", v);
    }
    snprintf(path, sizeof path, "%.*s", (int)strcspn(opt.apps, ":"), opt.apps);
    if (statvfs(path, &vfs) == 0 && vfs.f_blocks > 0) {
        snprintf(v, sizeof v, "%llu MB free", (unsigned long long)vfs.f_bavail * vfs.f_frsize / 1048576);
        add_info("Storage for apps", v);
    } else {
        add_info("Storage for apps", "in memory: lost at power off");
    }
    add_info("Licence", "AGPL-3.0-or-later");
    add_info("Source", "github.com/M0LTE/yealink-t48s");
}

static void build_appsettings(void)
{
    int n = 0;
    snprintf(title, sizeof title, "App settings");
    for (int i = 0; i < napps; i++) {
        if (!apps[i].nsettings) continue;
        char v[40];
        snprintf(v, sizeof v, "%d setting%s", apps[i].nsettings, apps[i].nsettings == 1 ? "" : "s");
        snprintf(add_menu(apps[i].name, P_APPCONF, v)->arg, sizeof rows[0].arg, "%s", apps[i].id);
        n++;
    }
    if (!n) snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "No installed app has settings.");
}

static void build_appconf(const char *id)
{
    const struct manifest *m = installed(id);
    char file[80];
    if (!m) { snprintf(title, sizeof title, "App settings"); return; }
    snprintf(title, sizeof title, "%s", m->name);
    snprintf(file, sizeof file, "apps/%s", m->id);
    for (int i = 0; i < m->nsettings; i++) {
        char key[48];
        const char *eq = strchr(m->settings[i], '='), *type = m->setting_type[i];
        snprintf(key, sizeof key, "%.*s", (int)(eq - m->settings[i]), m->settings[i]);
        if (!strcmp(type, "bool")) {
            struct row *r = add_stored(R_TOGGLE, m->setting_label[i], file, key, eq + 1, 0);
            snprintf(r->value, sizeof r->value, "%s", !strcmp(r->value, "true") || !strcmp(r->value, "on") ? "on" : "off");
            r->choices = "true|false";
        } else {
            add_stored(R_TEXT, m->setting_label[i], file, key, eq + 1,
                       !strcmp(type, "password") ? F_PASSWORD : !strcmp(type, "number") ? F_DIGITS : 0);
        }
    }
    snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "The app picks changes up the next time it is opened.");
    snprintf(add_action("Back to the app's own defaults", A_APP_DEFAULTS, m->id)->def, sizeof rows[0].def, "%s", m->id);
}

static void build_store(void)
{
    int updates = 0;
    snprintf(title, sizeof title, "Apps");
    if (store_state == ST_NONE) store_refresh();
    if (store_state == ST_LOADING && !nstore) {
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Reading the feed...");
    } else {
        if (feed_error[0] && store_state == ST_LOADED) {
            struct row *r = add(R_NOTE, "");
            snprintf(r->value, sizeof r->value, "The feed could not be read: %s", feed_error);
            r->flags = F_DANGER;
        }
        for (int i = 0; i < nstore; i++) {
            const struct store_app *a = &store[i];
            char v[100], size[24];
            kb(size, sizeof size, a->disk_bytes ? a->disk_bytes : a->pkg_bytes);
            if (!strcmp(a->state, "update")) { snprintf(v, sizeof v, "%s installed, %s available", a->have, a->offered); updates++; }
            else if (!strcmp(a->state, "installed")) snprintf(v, sizeof v, "%s installed, %s", a->have, size);
            else if (!strcmp(a->state, "builtin")) snprintf(v, sizeof v, "%s, part of the firmware", a->have);
            else if (!strcmp(a->state, "unsupported")) snprintf(v, sizeof v, "%s needs newer firmware", a->offered);
            else snprintf(v, sizeof v, "%s, %s", a->offered, size);
            snprintf(add_menu(a->name, P_STOREAPP, v)->arg, sizeof rows[0].arg, "%s", a->id);
        }
        if (updates > 1) add_action("Update all", A_UPDATE_ALL, "");
        add_action("Look again", A_REFRESH, "");
    }
    add_stored(R_TEXT, "Feed address", "feed", "url", pkg_default_feed(), 0);
    if (strcmp(feed_now(), pkg_default_feed())) add_action("Use the standard feed", A_FEED_DEFAULT, "");
}

static void build_storeapp(const char *id)
{
    const struct store_app *a = store_find(id);
    const struct manifest *m = installed(id);
    char v[200];
    snprintf(title, sizeof title, "%s", a ? a->name : m ? m->name : id);
    if (!a) { snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, store_state == ST_LOADING ? "Reading the feed..." : "No longer listed."); return; }
    int inst = strcmp(a->have, "-") != 0, update = !strcmp(a->state, "update");
    if (!strcmp(a->state, "available")) add_action("Install", A_INSTALL, id);
    if (update) add_action("Update", A_UPDATE, id);
    if (inst) add_action("Open", A_OPEN, id);
    if (inst && m && m->nsettings) snprintf(add_menu("Settings", P_APPCONF, "")->arg, sizeof rows[0].arg, "%s", id);
    if (a->description[0]) snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "%s", a->description);
    if (!strcmp(a->state, "unsupported")) {
        struct row *r = add(R_NOTE, "");
        snprintf(r->value, sizeof r->value, "This version needs a newer app API than this phone's (level %d). Update the phone's firmware to install it.", T48_API_LEVEL);
        r->flags = F_DANGER;
    }
    if (inst) add_info("Installed version", a->have);
    if (strcmp(a->offered, "-")) add_info(inst ? "Version in the feed" : "Version", a->offered);
    if (a->disk_bytes) { kb(v, sizeof v, a->disk_bytes); add_info("Size on the phone", v); }
    if (a->pkg_bytes) { kb(v, sizeof v, a->pkg_bytes); add_info("Download size", v); }
    if (!a->perms[0]) snprintf(v, sizeof v, "nothing special");
    else {
        v[0] = 0;
        char perms[96], *save = NULL;
        snprintf(perms, sizeof perms, "%s", a->perms);
        for (char *p = strtok_r(perms, " ", &save); p; p = strtok_r(NULL, " ", &save)) {
            size_t at = strlen(v);
            snprintf(v + at, sizeof v - at, "%s%s", at ? ", " : "",
                     !strcmp(p, "network") ? "the network" : !strcmp(p, "audio-out") ? "playing sound" : p);
        }
    }
    add_info("May use", v);
    if (!strcmp(a->state, "builtin")) snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Part of the firmware: it cannot be removed.");
    else if (inst) add_action("Remove", A_REMOVE, id)->flags = F_DANGER;
}

static void rebuild(void)
{
    if (depth < 0) return;
    nrows = 0;
    switch (stack[depth].page) {
    case P_ROOT: build_root(); break;
    case P_ACCOUNTS: build_accounts(); break;
    case P_SIP: build_sip(stack[depth].arg); break;
    case P_NET: build_net(); break;
    case P_TIME: build_time(); break;
    case P_TZ: build_tz(); break;
    case P_SOUND: build_sound(); break;
    case P_SCREEN: build_screen(); break;
    case P_ACCESS: build_access(); break;
    case P_ABOUT: build_about(); break;
    case P_APPSETTINGS: build_appsettings(); break;
    case P_APPCONF: build_appconf(stack[depth].arg); break;
    case P_STORE: build_store(); break;
    case P_STOREAPP: build_storeapp(stack[depth].arg); break;
    }
    if (stack[depth].sel >= nrows) stack[depth].sel = nrows ? nrows - 1 : 0;
    if (stack[depth].top > stack[depth].sel) stack[depth].top = stack[depth].sel;
    if (stack[depth].sel >= stack[depth].top + VISIBLE) stack[depth].top = stack[depth].sel - VISIBLE + 1;
    if (stack[depth].top + VISIBLE > nrows) stack[depth].top = nrows > VISIBLE ? nrows - VISIBLE : 0;
}

/* ---------- moving between pages ---------- */

static void push(enum page p, const char *arg)
{
    if (depth >= 7) return;
    depth++;
    memset(&stack[depth], 0, sizeof stack[0]);
    stack[depth].page = p;
    snprintf(stack[depth].arg, sizeof stack[0].arg, "%s", arg ? arg : "");
    logmsg("settings: page %s", page_names[p]);
    rebuild();
    /* start on the first row that does something */
    while (stack[depth].sel < nrows - 1 && (rows[stack[depth].sel].kind == R_NOTE || rows[stack[depth].sel].kind == R_INFO)) stack[depth].sel++;
    if (p == P_TZ)
        for (int i = 0; i < nrows; i++)
            if (rows[i].value[0]) stack[depth].sel = i;
    rebuild();
}

static void pop(void)
{
    if (dirty[0]) { run_apply(dirty); dirty[0] = 0; }
    depth--;
    if (depth < 0) {
        kbd_open = 0;
        ov.kind = O_NONE;
        shell_settings_closed(came_from_launcher);
        return;
    }
    rebuild();
}

void settings_open(int app_manager)
{
    depth = -1;
    dirty[0] = 0;
    kbd_open = 0;
    ov.kind = O_NONE;
    came_from_launcher = app_manager;
    if (app_manager) { store_state = ST_NONE; nstore = 0; }   /* a fresh look at the feed each time */
    push(app_manager ? P_STORE : P_ROOT, NULL);
}

/* leave at once, for the shell to show something else */
void settings_close(void)
{
    if (dirty[0]) { run_apply(dirty); dirty[0] = 0; }
    depth = -1;
    kbd_open = 0;
    ov.kind = O_NONE;
}

/* ---------- changing things ---------- */

static void mark_dirty(const char *file)
{
    if (!strcmp(file, "network") || !strcmp(file, "access")) snprintf(dirty, sizeof dirty, "%s", file);
    /* any account's file: baresip is restarted once, with all of them */
    if (!strncmp(file, "sip", 3)) snprintf(dirty, sizeof dirty, "sip");
    if (!strcmp(file, "phone")) run_apply("phone");
    if (!strncmp(file, "apps/", 5) && !strcmp(app_id(), file + 5)) app_quit();   /* it restarts with the new values */
}

static void store_value(const struct row *r, const char *value)
{
    if (cfg_set(r->file, r->key, value) < 0) {
        message("Not saved", errno == EINVAL ? "That value has something in it that cannot be stored." : strerror(errno));
        return;
    }
    logmsg("settings: %s %s set", r->file, r->key);   /* never the value */
    mark_dirty(r->file);
    if (!strcmp(r->file, "feed") && !strcmp(r->key, "url")) store_refresh();
}

static void set_level(struct row *r, int v)
{
    if (r->flags & F_DISABLED) return;
    if (v < 0) v = 0;
    if (v > r->level_max) v = r->level_max;
    if (v == r->level) return;
    r->level = v;
    if (r->target >= 0) {
        phone_set_volume(r->target, v);   /* stored, and told to the audio service; nothing is played */
    } else {
        char b[8];
        if (v < 1) v = r->level = 1;   /* never fully dark: the screen is how you get back */
        snprintf(b, sizeof b, "%d", v);
        cfg_set("screen", "brightness", b);
        backlight_set_level(v);
    }
}

static void do_action(const struct row *r)
{
    switch ((enum action)r->target) {
    case A_SET_TZ:
        cfg_set("system", "timezone", r->def);
        apply_tz();
        logmsg("settings: time zone %s", r->def);
        run_apply("system");
        pop();
        return;
    case A_REFRESH: store_refresh(); break;
    case A_INSTALL:
    case A_UPDATE: change_app((enum action)r->target, r->arg); break;
    case A_UPDATE_ALL:
        for (int i = 0; i < nstore; i++)
            if (!strcmp(store[i].state, "update")) { change_app(A_UPDATE, store[i].id); break; }
        break;
    case A_REMOVE: {
        const struct store_app *a = store_find(r->arg);
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_REMOVE;
        snprintf(ov.arg, sizeof ov.arg, "%s", r->arg);
        snprintf(ov.title, sizeof ov.title, "Remove %s?", a ? a->name : r->arg);
        snprintf(ov.text, sizeof ov.text, "What it kept and its settings go too.");
        break;
    }
    case A_OPEN: shell_settings_open_app(r->arg); return;
    case A_APP_DEFAULTS: {
        char path[400];
        snprintf(path, sizeof path, "%s/apps/%s.conf", opt.config, r->def);
        unlink(path);
        if (!strcmp(app_id(), r->def)) app_quit();
        break;
    }
    case A_FEED_DEFAULT:
        cfg_set("feed", "url", NULL);
        store_refresh();
        break;
    case A_SSH:
        if (!strcmp(r->value, "on")) {
            /* off is the safe way: no question asked */
            cfg_set("access", "ssh", "off");
            door_password[0] = 0;
            run_apply("access");
        } else {
            memset(&ov, 0, sizeof ov);
            ov.kind = O_CONFIRM;
            ov.act = A_SSH;
            ov.yes = "Turn on";
            ov.no = "Keep off";
            snprintf(ov.title, sizeof ov.title, "Turn SSH on?");
            snprintf(ov.text, sizeof ov.text, "Anyone on your network holding a key this phone accepts gets full control of it.");
            snprintf(ov.extra, sizeof ov.extra, "Host key: %s", acc.host_key[0] ? acc.host_key : "not known yet");
        }
        break;
    case A_WEB:
        if (!strcmp(r->value, "on")) {
            cfg_set("access", "web", "off");
            run_apply("access");
        } else {
            memset(&ov, 0, sizeof ov);
            ov.kind = O_CONFIRM;
            ov.act = A_WEB;
            ov.yes = "Turn on";
            ov.no = "Keep off";
            snprintf(ov.title, sizeof ov.title, "Turn the web page on?");
            snprintf(ov.text, sizeof ov.text, "Anyone on your network can open it, but only a browser that types a code shown on this screen gets in.");
            snprintf(ov.extra, sizeof ov.extra, "It is plain HTTP: use it on a network you trust.");
        }
        break;
    case A_DOOR_OPEN:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_DOOR_OPEN;
        ov.yes = "Open";
        ov.no = "Cancel";
        snprintf(ov.title, sizeof ov.title, "Allow a password login?");
        snprintf(ov.text, sizeof ov.text, "For ten minutes, with a password shown here, so ssh-copy-id can put your key on the phone.");
        snprintf(ov.extra, sizeof ov.extra, "It closes by itself as soon as a key arrives.");
        break;
    case A_DOOR_SHOW:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_DOOR;
        break;
    case A_DOOR_CLOSE:
        door_password[0] = 0;
        run_apply("access-door-close");
        break;
    case A_ACCOUNT_REMOVE:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_ACCOUNT_REMOVE;
        snprintf(ov.arg, sizeof ov.arg, "%s", r->def);
        snprintf(ov.title, sizeof ov.title, "Remove this account?");
        snprintf(ov.text, sizeof ov.text, "Its user, password and server are forgotten.");
        break;
    case A_FORGET_KEYS:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_FORGET_KEYS;
        ov.yes = "Forget";
        ov.no = "Keep";
        snprintf(ov.title, sizeof ov.title, "Forget %s?", acc.dev ? "the added keys" : "all keys");
        snprintf(ov.text, sizeof ov.text, "%s", acc.dev ? "The keys built into this development build stay." :
                 "Nobody will be able to log in until a key is added again.");
        break;
    case A_NONE: break;
    }
    rebuild();
}

/* the owner said yes to a question */
static void confirmed(enum action act, const char *arg)
{
    switch (act) {
    case A_REMOVE: change_app(act, arg); break;
    case A_SSH:
        cfg_set("access", "ssh", "on");
        run_apply("access");
        break;
    case A_WEB:
        cfg_set("access", "web", "on");
        run_apply("access");
        break;
    case A_DOOR_OPEN:
        if (make_password(door_password) < 0) { message("Not opened", "The phone could not make up a password."); break; }
        door_keys_before = acc.nkeys;
        door_asked = t48_now_ms();
        run_apply_with("access-door-open", door_password);
        memset(&ov, 0, sizeof ov);
        ov.kind = O_DOOR;
        break;
    case A_FORGET_KEYS: run_apply("access-forget-keys"); break;
    case A_ACCOUNT_REMOVE: {
        char path[400];
        if (strncmp(arg, "sip", 3) || strchr(arg, '/')) break;
        snprintf(path, sizeof path, "%s/%s", opt.config, arg);
        unlink(path);
        logmsg("settings: account file %s removed", arg);
        snprintf(dirty, sizeof dirty, "sip");
        pop();
        break;
    }
    default: break;
    }
}

static void activate(int i, int dir)
{
    struct row *r = &rows[i];
    if (r->flags & F_DISABLED) return;
    switch (r->kind) {
    case R_MENU: push(r->target, r->arg); break;
    case R_TEXT:
        kbd_row = *r;
        /* a password starts empty: what is stored is never put on the screen */
        t48_kbd_open(&kbd, (r->flags & F_PASSWORD) ? "New password (leave empty to keep the one set)" : r->label,
                     (r->flags & F_PASSWORD) ? "" : r->value, 160,
                     ((r->flags & F_PASSWORD) ? T48_KBD_PASSWORD : 0) | ((r->flags & F_DIGITS) ? T48_KBD_DIGITS : 0));
        kbd_open = 1;
        break;
    case R_CHOICE: {
        /* step to the next (or the one before) of a|b|c */
        char list[100], *opts[8], *save = NULL;
        int n = 0, at = 0;
        snprintf(list, sizeof list, "%s", r->choices);
        for (char *p = strtok_r(list, "|", &save); p && n < 8; p = strtok_r(NULL, "|", &save)) {
            if (!strcmp(p, r->value)) at = n;
            opts[n++] = p;
        }
        store_value(r, opts[(at + (dir < 0 ? n - 1 : 1)) % n]);
        rebuild();
        break;
    }
    case R_TOGGLE: {
        int on = !strcmp(r->value, "on");
        if (r->target) { do_action(r); break; }   /* a switch that asks first */
        if (r->choices) store_value(r, on ? "false" : "true");   /* an app's bool */
        else store_value(r, on ? "off" : "on");
        rebuild();
        break;
    }
    case R_LEVEL: set_level(r, r->level + (dir < 0 ? -1 : 1)); break;
    case R_ACTION: do_action(r); break;
    default: break;
    }
}

static void kbd_done(void)
{
    const char *text = t48_kbd_text(&kbd);
    struct in_addr a;
    kbd_open = 0;
    if ((kbd_row.flags & F_PASSWORD) && !text[0]) return;
    if ((kbd_row.flags & F_IP) && text[0] && inet_pton(AF_INET, text, &a) != 1) {
        message("Not an address", "It should look like 192.168.1.20: four numbers with dots between.");
        return;
    }
    if (!strcmp(kbd_row.file, "feed") && !strcmp(kbd_row.key, "url") && strncmp(text, "https://", 8) &&
        strncmp(text, "file://", 7)) {
        message("Not a feed address", "A feed's address starts with https://");
        return;
    }
    /* emptied: the setting goes back to not being set (an app's own may really be empty) */
    store_value(&kbd_row, text[0] || !strncmp(kbd_row.file, "apps/", 5) ? text : NULL);
    rebuild();
}

/* ---------- input ---------- */

static void overlay_answer(int yes)
{
    enum overlay_kind k = ov.kind;
    if (k == O_BUSY) return;   /* nothing to press: it ends by itself */
    ov.kind = O_NONE;
    if (k == O_DOOR && yes) { door_password[0] = 0; run_apply("access-door-close"); }   /* "Close now" */
    if (k == O_CONFIRM && yes) confirmed(ov.act, ov.arg);
}

void settings_key(int code, int down)
{
    if (depth < 0) return;
    if (kbd_open) {
        int r = t48_kbd_key(&kbd, code, down);
        if (r == T48_KBD_DONE) kbd_done();
        else if (r == T48_KBD_CANCEL) kbd_open = 0;
        if (r != T48_KBD_NOTHING) redraw_me();
        return;
    }
    if (!down) return;
    if (ov.kind != O_NONE) {
        /* on the password screen both keys only hide it: closing it early is a touch on "Close now" */
        if (code == T48_KEY_OK) overlay_answer(ov.kind != O_DOOR);
        else if (code == T48_KEY_BACK) overlay_answer(0);
        redraw_me();
        return;
    }
    int *sel = &stack[depth].sel;
    switch (code) {
    case T48_KEY_UP: if (*sel > 0) (*sel)--; break;
    case T48_KEY_DOWN: if (*sel < nrows - 1) (*sel)++; break;
    case T48_KEY_OK: if (*sel < nrows) activate(*sel, 1); break;
    case T48_KEY_RIGHT:
        if (*sel < nrows && (rows[*sel].kind == R_LEVEL || rows[*sel].kind == R_CHOICE || rows[*sel].kind == R_MENU)) activate(*sel, 1);
        break;
    case T48_KEY_LEFT:
        if (*sel < nrows && (rows[*sel].kind == R_LEVEL || rows[*sel].kind == R_CHOICE)) activate(*sel, -1);
        else { pop(); if (depth < 0) return; }
        break;
    case T48_KEY_BACK: pop(); if (depth < 0) return; break;
    default: return;
    }
    if (depth >= 0 && !kbd_open) rebuild();
    redraw_me();
}

#define OV_X 110
#define OV_Y 120
#define OV_W 580
#define OV_H 250
#define SCROLL_W 60
#define DOOR_X 60
#define DOOR_Y 40
#define DOOR_W 680
#define DOOR_H 410

static int list_w(void) { return SW - 2 * LIST_X - (nrows > VISIBLE ? SCROLL_W : 0); }

void settings_touch(int state, int x, int y)
{
    if (depth < 0) return;
    if (kbd_open) {
        int r = t48_kbd_touch(&kbd, state, x, y);
        if (r == T48_KBD_DONE) kbd_done();
        else if (r == T48_KBD_CANCEL) kbd_open = 0;
        if (r != T48_KBD_NOTHING) redraw_me();
        return;
    }
    if (state != T48_TOUCH_DOWN) return;
    if (ov.kind != O_NONE) {
        if (ov.kind == O_MSG && ui_inside(x, y, OV_X + OV_W / 2 - 80, OV_Y + OV_H - 76, 160, 56)) overlay_answer(1);
        else if (ov.kind == O_CONFIRM && ui_inside(x, y, OV_X + OV_W / 2 - 170, OV_Y + OV_H - 76, 160, 56)) overlay_answer(0);
        else if (ov.kind == O_CONFIRM && ui_inside(x, y, OV_X + OV_W / 2 + 10, OV_Y + OV_H - 76, 160, 56)) overlay_answer(1);
        else if (ov.kind == O_DOOR && ui_inside(x, y, SW / 2 - 190, DOOR_Y + DOOR_H - 70, 180, 54)) overlay_answer(0);
        else if (ov.kind == O_DOOR && ui_inside(x, y, SW / 2 + 10, DOOR_Y + DOOR_H - 70, 180, 54)) overlay_answer(1);
        redraw_me();
        return;
    }
    if (y < 46) {
        if (x < 130) { pop(); if (depth < 0) return; }
        redraw_me();
        return;
    }
    int w = list_w();
    if (nrows > VISIBLE && x >= SW - LIST_X - SCROLL_W + 6) {
        /* the two arrows down the right: a page at a time */
        int step = y < LIST_Y + VISIBLE * ROW_H / 2 ? -(VISIBLE - 1) : VISIBLE - 1;
        int top = stack[depth].top + step;
        if (top > nrows - VISIBLE) top = nrows - VISIBLE;
        if (top < 0) top = 0;
        stack[depth].top = top;
        if (stack[depth].sel < top) stack[depth].sel = top;
        if (stack[depth].sel >= top + VISIBLE) stack[depth].sel = top + VISIBLE - 1;
        redraw_me();
        return;
    }
    int i = stack[depth].top + (y - LIST_Y) / ROW_H;
    if (y < LIST_Y || i >= nrows || x < LIST_X || x >= LIST_X + w) return;
    stack[depth].sel = i;
    if (rows[i].kind == R_LEVEL) {
        /* minus at the left of the bar, plus at the right */
        int mid = LIST_X + w - 170;
        if (x >= LIST_X + w - 340) activate(i, x < mid ? -1 : 1);
    } else {
        activate(i, 1);
    }
    if (depth >= 0 && !kbd_open) rebuild();
    redraw_me();
}

#define ACCESS_CHECK_MS 500   /* how often the Access page looks at what t48-access has done */

int settings_timeout(long long now)
{
    if (depth >= 0 && kbd_open) return t48_kbd_timeout(&kbd);
    if (depth >= 0 && stack[depth].page == P_ACCESS)
        return acc_checked + ACCESS_CHECK_MS > now ? (int)(acc_checked + ACCESS_CHECK_MS - now) : 0;
    return -1;
}

void settings_tick(long long now)
{
    if (depth >= 0 && kbd_open && t48_kbd_tick(&kbd) != T48_KBD_NOTHING) redraw_me();
    if (depth < 0 || kbd_open || stack[depth].page != P_ACCESS || now < acc_checked + ACCESS_CHECK_MS) return;
    static long shown_left = -1;
    int was_open = acc.door, changed = access_read();
    acc_checked = now;
    if (was_open && !acc.door) {
        /* password login has closed: because a key came, the time ran out, or it was closed by hand */
        int had_password = door_password[0] != 0;
        door_password[0] = 0;
        if (ov.kind == O_DOOR || had_password) {
            if (acc.nkeys > door_keys_before) message("Your key is on the phone", "Password login is closed again. From now on only keys can log in.");
            else message("Password login is closed", "No key arrived. Only keys can log in.");
        }
    }
    if (door_password[0] && !acc.door && !was_open && now > door_asked + 5000) {
        /* asked for, and five seconds later still not open */
        door_password[0] = 0;
        if (ov.kind == O_DOOR) message("Password login did not open", "Nothing has changed: only keys can log in.");
        changed = 1;
    }
    long left = acc.door ? acc.door_until - (long)time(NULL) : -1;
    if (changed || left != shown_left) { shown_left = left; rebuild(); redraw_me(); }
}

/* ---------- drawing ---------- */

/* text over up to two lines, broken at spaces */
static void two_lines(int x, int y, int w, const t48_font *f, uint32_t c, const char *text)
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

static void draw_row(const struct row *r, int x, int y, int w, int selected)
{
    int h = ROW_H - 6, dis = r->flags & F_DISABLED;
    uint32_t text = dis ? C_FAINT : C_TEXT;
    const t48_font *f = &t48_font_normal;
    int ty = y + (h - f->height) / 2;

    if (r->kind == R_NOTE) {
        if (selected) t48_fill(&ui, x, y + 8, 4, h - 16, C_ACCENT);
        two_lines(x + 16, y + 6, w - 32, &t48_font_small, (r->flags & F_DANGER) ? C_BAD : C_DIM, r->value);
        return;
    }
    t48_round_fill(&ui, x, y, w, h, 10, selected ? C_KEY : C_PANEL);
    if (selected) t48_fill(&ui, x, y + 8, 4, h - 16, C_ACCENT);

    if (r->kind == R_ACTION) {
        t48_text(&ui, x + 18, ty, &t48_font_bold, dis ? C_FAINT : (r->flags & F_DANGER) ? C_BAD : C_ACCENT, r->label);
        if (r->value[0]) t48_text_right(&ui, x + w - 18, ty, f, C_GOOD, r->value);
        return;
    }
    int right = x + w - 18;
    if (r->kind == R_MENU) { t48_text_right(&ui, right, ty, &t48_font_bold, C_DIM, ">"); right -= 26; }
    if (r->kind == R_LEVEL) {
        int bx = x + w - 340, bw = 340 - 18, cell = (bw - 120) / (r->level_max ? r->level_max : 1);
        ui_button(bx, y + 6, 50, h - 12, "-", dis ? C_PANEL : C_LINE);
        ui_button(bx + bw - 50, y + 6, 50, h - 12, "+", dis ? C_PANEL : C_LINE);
        for (int i = 0; i < r->level_max; i++)
            t48_fill(&ui, bx + 60 + i * cell, y + h / 2 - 9, cell - 3, 18, i < r->level && !dis ? C_ACCENT : C_LINE);
        t48_text(&ui, x + 18, ty, f, text, r->label);
        if (!dis) {
            char n[8];
            snprintf(n, sizeof n, "%d", r->level);
            t48_text_right(&ui, bx - 14, ty, f, C_DIM, n);
        }
        return;
    }
    if (r->kind == R_TOGGLE) {
        int on = !strcmp(r->value, "on");
        t48_round_fill(&ui, right - 76, y + 11, 76, h - 22, (h - 22) / 2, on ? C_GOOD : C_LINE);
        t48_text_centre(&ui, right - 38, ty, &t48_font_bold, on ? C_BG : C_DIM, on ? "On" : "Off");
        t48_text_fit(&ui, x + 18, ty, w - 140, f, text, r->label);
        return;
    }
    /* label on the left, value on the right; the value gives way first */
    int lw = t48_text_width(f, r->label), room = right - (x + 18);
    if (lw > room * 2 / 3 && r->value[0]) lw = room * 2 / 3;
    t48_text_fit(&ui, x + 18, ty, lw + 2, f, text, r->label);
    if (r->value[0]) {
        int vw = t48_text_width(f, r->value), max = room - lw - 24;
        uint32_t vc = r->kind == R_INFO ? (dis ? C_FAINT : C_TEXT) : C_DIM;
        if (dis) vc = C_FAINT;
        if (vw <= max) t48_text_right(&ui, right, ty, f, vc, r->value);
        else if (max > 40) t48_text_fit(&ui, right - max, ty, max, f, vc, r->value);
    }
}

static void draw_overlay(void)
{
    t48_fill_alpha(&ui, 0, 0, SW, SH, C_BG, 170);
    t48_round_fill(&ui, OV_X, OV_Y, OV_W, OV_H, 16, C_PANEL);
    t48_fill(&ui, OV_X, OV_Y + 16, 6, OV_H - 32, ov.kind == O_CONFIRM ? C_BAD : C_ACCENT);
    t48_text_centre(&ui, SW / 2, OV_Y + 24, &t48_font_head, C_TEXT, ov.title);
    if (ov.kind == O_BUSY) {
        t48_text_centre(&ui, SW / 2, OV_Y + 88, &t48_font_normal, C_DIM, ov.text);
        int bx = OV_X + 60, bw = OV_W - 120, by = OV_Y + 150;
        t48_round_fill(&ui, bx, by, bw, 16, 8, C_LINE);
        if (ov.percent >= 0) {
            char pc[8];
            t48_round_fill(&ui, bx, by, bw * ov.percent / 100 > 16 ? bw * ov.percent / 100 : 16, 16, 8, C_ACCENT);
            snprintf(pc, sizeof pc, "%d%%", ov.percent);
            t48_text_centre(&ui, SW / 2, by + 30, &t48_font_small, C_DIM, pc);
        }
        return;
    }
    two_lines(OV_X + 40, OV_Y + 70, OV_W - 80, &t48_font_normal, C_DIM, ov.text);
    if (ov.extra[0]) t48_text_centre(&ui, SW / 2, OV_Y + 136, &t48_font_small, C_TEXT, ov.extra);
    if (ov.kind == O_MSG) {
        ui_button(OV_X + OV_W / 2 - 80, OV_Y + OV_H - 76, 160, 56, "OK", C_ACCENT);
    } else {
        ui_button(OV_X + OV_W / 2 - 170, OV_Y + OV_H - 76, 160, 56, ov.no ? ov.no : "Keep", C_LINE);
        ui_button(OV_X + OV_W / 2 + 10, OV_Y + OV_H - 76, 160, 56, ov.yes ? ov.yes : "Remove", ov.yes && ov.act != A_FORGET_KEYS ? C_ACCENT : C_BAD);
    }
}

/* password login is open: what to type on the computer, and for how much longer */
static void draw_door(void)
{
    char b[200], net[80];
    int up;
    t48_fill_alpha(&ui, 0, 0, SW, SH, C_BG, 200);
    t48_round_fill(&ui, DOOR_X, DOOR_Y, DOOR_W, DOOR_H, 16, C_PANEL);
    t48_fill(&ui, DOOR_X, DOOR_Y + 16, 6, DOOR_H - 32, C_ACCENT);
    t48_text_centre(&ui, SW / 2, DOOR_Y + 16, &t48_font_head, C_TEXT, "Add a key from your computer");
    t48_text_centre(&ui, SW / 2, DOOR_Y + 66, &t48_font_small, C_DIM, "On your computer, run:");
    ui_network_status(net, sizeof net, &up);
    const char *ip = strrchr(net, ' ');
    snprintf(b, sizeof b, "ssh-copy-id root@%s", up && ip ? ip + 1 : "the-phone");
    t48_text_centre(&ui, SW / 2, DOOR_Y + 90, &t48_font_head, C_TEXT, b);
    t48_text_centre(&ui, SW / 2, DOOR_Y + 140, &t48_font_small, C_DIM, "It shows this host key. If it shows another, stop:");
    t48_text_centre(&ui, SW / 2, DOOR_Y + 162, &t48_font_small, C_TEXT, acc.host_key[0] ? acc.host_key : "not known yet");
    t48_text_centre(&ui, SW / 2, DOOR_Y + 194, &t48_font_small, C_DIM, "Then it asks for this password:");
    if (door_password[0]) t48_text_centre(&ui, SW / 2, DOOR_Y + 216, &t48_font_big, C_ACCENT, door_password);
    else t48_text_centre(&ui, SW / 2, DOOR_Y + 228, &t48_font_normal, C_DIM, "(no longer shown: close this and open it again for a new one)");
    if (acc.door) {
        char left[16];
        door_left(left, sizeof left);
        snprintf(b, sizeof b, "Password login closes when your key arrives, or in %s.", left);
    } else {
        snprintf(b, sizeof b, "Opening...");
    }
    t48_text_centre(&ui, SW / 2, DOOR_Y + DOOR_H - 104, &t48_font_small, C_DIM, b);
    ui_button(SW / 2 - 190, DOOR_Y + DOOR_H - 70, 180, 54, "Hide", C_LINE);
    ui_button(SW / 2 + 10, DOOR_Y + DOOR_H - 70, 180, 54, "Close now", C_BAD);
}

void settings_draw(void)
{
    if (depth < 0) return;
    if (kbd_open) { t48_kbd_draw(&kbd, &ui); return; }
    rebuild();   /* what the rows show may have changed underneath: the clock, the network, a download */

    char hm[8];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(hm, sizeof hm, "%H:%M", &tm);
    t48_fill(&ui, 0, 0, SW, SH, C_BG);
    t48_fill(&ui, 0, 0, SW, 44, C_BAR);
    t48_fill(&ui, 0, 44, SW, 1, C_LINE);
    t48_text(&ui, 18, 11, &t48_font_bold, C_ACCENT, "< Back");
    t48_text(&ui, 130, 6, &t48_font_head, C_TEXT, title);
    t48_text_right(&ui, SW - 18, 11, &t48_font_bold, C_DIM, hm);

    int w = list_w(), top = stack[depth].top;
    for (int i = top; i < nrows && i < top + VISIBLE; i++)
        draw_row(&rows[i], LIST_X, LIST_Y + (i - top) * ROW_H, w, i == stack[depth].sel);
    if (nrows > VISIBLE) {
        int x = SW - LIST_X - SCROLL_W + 8, hh = VISIBLE * ROW_H / 2 - 6;
        ui_button(x, LIST_Y, SCROLL_W - 8, hh, "^", top > 0 ? C_KEY : C_PANEL);
        ui_button(x, LIST_Y + hh + 6, SCROLL_W - 8, hh, "v", top + VISIBLE < nrows ? C_KEY : C_PANEL);
    }
    if (ov.kind == O_DOOR) draw_door();
    else if (ov.kind != O_NONE) draw_overlay();
}

/* ---------- for the simulator's checks ---------- */

const char *settings_state(const char *key)
{
    static char b[220];
    if (!strcmp(key, "settings.page")) return depth >= 0 ? page_names[stack[depth].page] : "none";
    if (!strcmp(key, "settings.rows")) { snprintf(b, sizeof b, "%d", depth >= 0 ? nrows : 0); return b; }
    if (!strcmp(key, "settings.row") || !strcmp(key, "settings.value")) {
        if (depth < 0 || stack[depth].sel >= nrows) return "-";
        const struct row *r = &rows[stack[depth].sel];
        if (key[9] == 'v' && r->kind == R_LEVEL) snprintf(b, sizeof b, "%d", r->level);
        else snprintf(b, sizeof b, "%s", key[9] == 'r' ? r->label : r->value);
        for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
        return b[0] ? b : "-";
    }
    if (!strcmp(key, "kbd")) return depth >= 0 && kbd_open ? "open" : "closed";
    if (!strcmp(key, "kbd.text")) {
        if (depth < 0 || !kbd_open) return "-";
        if (kbd.flags & T48_KBD_PASSWORD) return "(hidden)";
        snprintf(b, sizeof b, "%s", t48_kbd_text(&kbd));
        for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
        return b[0] ? b : "-";
    }
    if (!strcmp(key, "overlay")) return ov.kind == O_BUSY ? "busy" : ov.kind == O_MSG ? "message" : ov.kind == O_CONFIRM ? "confirm" :
                                        ov.kind == O_DOOR ? "door" : "none";
    if (!strcmp(key, "door.password")) return door_password[0] ? "(shown)" : "(none)";   /* never the password */
    if (!strcmp(key, "overlay.title")) {
        snprintf(b, sizeof b, "%s", ov.kind != O_NONE ? ov.title : "-");
        for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
        return b;
    }
    if (!strcmp(key, "store")) return store_state == ST_LOADING ? "loading" : store_state == ST_LOADED ? (feed_error[0] ? "error" : "loaded") : "none";
    if (!strncmp(key, "store.", 6)) {
        const struct store_app *a = store_find(key + 6);
        return a ? a->state : "none";
    }
    if (!strncmp(key, "installed.", 10)) {
        const struct manifest *m = installed(key + 10);
        return m ? (m->version[0] ? m->version : "yes") : "no";
    }
    if (!strncmp(key, "cfg.", 4)) {
        /* cfg.FILE.KEY, straight from the store; passwords only as set or not */
        char file[80];
        const char *dot = strchr(key + 4, '.');
        if (!dot) return NULL;
        snprintf(file, sizeof file, "%.*s", (int)(dot - key - 4), key + 4);
        for (char *p = file; *p; p++) if (*p == ':') *p = '/';
        if (!cfg_get(file, dot + 1, b, sizeof b)) return "(unset)";
        if (strstr(dot + 1, "password")) return b[0] ? "(set)" : "(empty)";
        for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
        return b[0] ? b : "(empty)";
    }
    if (!strcmp(key, "tz")) return getenv("TZ") ? getenv("TZ") : "-";
    return NULL;
}
