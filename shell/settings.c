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
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "shell.h"
#include "dialplan.h"

#ifndef T48_VERSION
#define T48_VERSION "development build"
#endif

enum page { P_ROOT, P_ACCOUNTS, P_SIP, P_SIPADV, P_CODECS, P_NET, P_TIME, P_TZ, P_SOUND, P_SCREEN, P_ACCESS, P_ABOUT, P_APPSETTINGS, P_APPCONF, P_STORE,
             P_STOREAPP, P_THEME, P_UPDATE, P_INSTALL, P_ADVANCED, P_FWD };
static const char *const page_names[] = {"root", "accounts", "sip", "sipadvanced", "codecs", "network", "time", "timezone", "sound", "screen", "access", "about",
                                         "appsettings", "appconf", "store", "storeapp", "theme", "update", "install", "advanced", "forward"};

enum row_kind { R_MENU, R_TEXT, R_CHOICE, R_TOGGLE, R_LEVEL, R_INFO, R_NOTE, R_ACTION };
enum { F_DISABLED = 1, F_PASSWORD = 2, F_DIGITS = 4, F_IP = 8, F_DANGER = 16, F_HOST = 32, F_PROXY = 64,
       F_SECONDS = 128,   /* the value is shown with " s" after it */
       F_REGINT = 256,    /* register_seconds: 0, 60 to 86400 or empty */
       F_CODEC = 512 };   /* a row of the Codecs page: a switch that also moves up and down */
enum action { A_NONE, A_SET_TZ, A_REFRESH, A_INSTALL, A_UPDATE, A_REMOVE, A_OPEN, A_APP_DEFAULTS, A_FEED_DEFAULT, A_UPDATE_ALL,
              A_SSH, A_WEB, A_DOOR_OPEN, A_DOOR_SHOW, A_DOOR_CLOSE, A_FORGET_KEYS, A_ACCOUNT_REMOVE, A_SET_THEME,
              A_UPD_CHANNEL, A_UPD_CHECK, A_UPD_INSTALL, A_UPD_KERNEL, A_UPD_RESTART,
              A_FW_BACKUP, A_FW_INSTALL, A_FW_RESTORE, A_KEYTEST, A_DND };

struct row {
    int kind, flags;
    char label[96];
    char value[200];       /* what shows on the right; for R_NOTE the whole text */
    char file[80], key[48];
    char def[160];         /* what the value is when the store has none */
    const char *choices;   /* R_CHOICE: "dhcp|static" */
    int target;            /* R_MENU: page. R_ACTION: action. R_LEVEL: which volume, or -1 the backlight.
                              R_CHOICE: an action that takes the choice, or 0 for the store */
    char arg[64];
    int level, level_max;
};

#define MAX_ROWS 96
/* the list: seven rows on the T48S; on a small screen as many as fit above the soft keys' labels */
#define VISIBLE (SMALL ? SROWS : 7)
#define ROW_H (SMALL ? SROW_STEP : 60)
#define LIST_Y (SMALL ? SROW_Y(0) : 54)
#define LIST_X (SMALL ? 8 : 16)

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
    int plain;                    /* O_BUSY: words only, no bar (waiting for the stick to come out) */
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

/* ---------- updates (issue #31): the front end of t48-update ---------- */

/* What "t48-update info" last said. The updater is a separate program
 * (--update), run as a child and never waited for: one child reads info, one
 * does the work (check, install, kernel, channel), and while work is going on
 * info is asked for again about once a second. Nothing here starts by itself. */
static struct {
    int loaded, installed;
    char slot[8], version[64], channel[160], latest[64], update[12], kernel[12], state[16], message[120], result[120];
    long result_time;
    pid_t info_pid, cmd_pid;
    int info_fd, again;
    char doing[16];               /* the state to show until the updater's own says the same */
    long long asked;
} upd = {.info_fd = -1};

#define UPDATE_CHECK_MS 1000

static void update_select(void);
static void message(const char *t, const char *text);
static void redraw_me(void);

static void firmware_version(char *v, size_t n)
{
    FILE *f;
    snprintf(v, n, "%s", T48_VERSION);
    if ((f = fopen("/etc/t48-release", "r"))) {
        if (fgets(v, (int)n, f)) v[strcspn(v, "\r\n")] = 0;
        fclose(f);
    }
}

static int update_working(const char *state)
{
    return !strcmp(state, "checking") || !strcmp(state, "downloading") || !strcmp(state, "writing");
}

static int update_busy(void) { return upd.cmd_pid > 0 || update_working(upd.state); }

/* without an updater, or one that says nothing: a phone that is not installed */
static void update_none(void)
{
    upd.loaded = 1;
    upd.installed = 0;
    firmware_version(upd.version, sizeof upd.version);
}

static void update_info(void)
{
    int p[2];
    if (upd.info_pid > 0) { upd.again = 1; return; }
    if (!opt.update || !opt.update[0] || pipe(p) < 0) { update_none(); return; }
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); update_none(); return; }
    if (pid == 0) {
        dup2(p[1], 1);
        for (int fd = 3; fd < 256; fd++) close(fd);
        execlp(opt.update, opt.update, "info", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    upd.info_pid = pid;
    upd.info_fd = p[0];
    upd.asked = t48_now_ms();
}

static void update_parse(char *text)
{
    int seen = 0;
    upd.installed = 0;
    upd.slot[0] = upd.version[0] = upd.channel[0] = upd.latest[0] = upd.message[0] = upd.result[0] = 0;
    upd.result_time = 0;
    snprintf(upd.update, sizeof upd.update, "unknown");
    snprintf(upd.kernel, sizeof upd.kernel, "unknown");
    snprintf(upd.state, sizeof upd.state, "idle");
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        seen = 1;
        if (!strcmp(line, "installed")) upd.installed = !strcmp(v, "yes");
        else if (!strcmp(line, "slot")) snprintf(upd.slot, sizeof upd.slot, "%s", v);
        else if (!strcmp(line, "version")) snprintf(upd.version, sizeof upd.version, "%s", v);
        else if (!strcmp(line, "channel")) snprintf(upd.channel, sizeof upd.channel, "%s", v);
        else if (!strcmp(line, "latest")) snprintf(upd.latest, sizeof upd.latest, "%s", v);
        else if (!strcmp(line, "update")) snprintf(upd.update, sizeof upd.update, "%s", v);
        else if (!strcmp(line, "kernel")) snprintf(upd.kernel, sizeof upd.kernel, "%s", v);
        else if (!strcmp(line, "state") && v[0]) snprintf(upd.state, sizeof upd.state, "%s", v);
        else if (!strcmp(line, "message")) snprintf(upd.message, sizeof upd.message, "%s", v);
        else if (!strcmp(line, "result")) snprintf(upd.result, sizeof upd.result, "%s", v);
        else if (!strcmp(line, "result_time")) upd.result_time = atol(v);
    }
    upd.loaded = 1;
    if (!seen) upd.installed = 0;
    if (!upd.version[0]) firmware_version(upd.version, sizeof upd.version);
    /* work just started may not have reached the updater's own state yet */
    if (upd.cmd_pid > 0 && upd.doing[0] && !update_working(upd.state)) snprintf(upd.state, sizeof upd.state, "%s", upd.doing);
}

/* check, install, kernel, or channel NAME */
static void update_run(const char *cmd, const char *arg)
{
    if (update_busy() || !opt.update || !opt.update[0]) return;
    pid_t pid = fork();
    if (pid < 0) { message("Not started", "Could not run the updater"); return; }
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 1);   /* the cause of a failure comes back through info */
        for (int fd = 3; fd < 256; fd++) close(fd);
        execlp(opt.update, opt.update, cmd, arg, (char *)NULL);
        _exit(127);
    }
    upd.cmd_pid = pid;
    snprintf(upd.doing, sizeof upd.doing, "%s", !strcmp(cmd, "check") ? "checking" : !strcmp(cmd, "channel") ? "" : "downloading");
    if (upd.doing[0]) snprintf(upd.state, sizeof upd.state, "%s", upd.doing);
    logmsg("update: %s %s", cmd, arg ? arg : "");
}


static int update_reaped(pid_t pid, int status)
{
    int on_page = depth >= 0 && stack[depth].page == P_UPDATE;
    if (upd.info_pid > 0 && pid == upd.info_pid) {
        char text[1500];
        ssize_t n = read(upd.info_fd, text, sizeof text - 1);
        int first = !upd.loaded;
        text[n > 0 ? n : 0] = 0;
        close(upd.info_fd);
        upd.info_fd = -1;
        upd.info_pid = 0;
        update_parse(text);
        if (upd.again) { upd.again = 0; update_info(); }
        if (on_page) { rebuild(); if (first) update_select(); redraw_me(); }
        return 1;
    }
    if (upd.cmd_pid > 0 && pid == upd.cmd_pid) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        upd.cmd_pid = 0;
        logmsg("update: %s", code == 0 ? "done" : "failed");
        if (code == 127) message("Not started", "Could not run the updater");
        else if (code != 0 && !upd.doing[0]) message("Channel not saved", "");
        /* what it left behind: the new state, or the cause of a failure */
        update_info();
        if (on_page) { rebuild(); redraw_me(); }
        return 1;
    }
    return 0;
}

/* ---------- the home screen's notices of updates (issue #80) ---------- */

/* The daily check (t48-update daily) leaves what it found in update-latest in the
 * state folder. When that file changes, what the updater says is asked for again;
 * every two seconds while it works, and once a minute while an update is waiting,
 * to see it installed from the web setup page or over SSH. */
#define UPDATE_WATCH_MS 60000
#define UPDATE_WORK_MS 2000
void settings_update_watch(long long now)
{
    static char seen[64] = "?";
    static long long last;
    char path[400], sig[64] = "-";
    struct stat st;
    snprintf(path, sizeof path, "%s/update-latest", opt.state);
    if (stat(path, &st) == 0) snprintf(sig, sizeof sig, "%ld.%ld.%ld", (long)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec, (long)st.st_size);
    long long every = update_working(upd.state) ? UPDATE_WORK_MS : !strcmp(upd.update, "yes") ? UPDATE_WATCH_MS : 0;
    if (!strcmp(sig, seen) && (!every || now < last + every)) return;
    snprintf(seen, sizeof seen, "%s", sig);
    last = now;
    if (upd.cmd_pid <= 0) update_info();
}

/* "Update available", with the channel when it is not the usual one; "" when there is none */
const char *settings_update_notice(void)
{
    if (!upd.loaded || !upd.installed || strcmp(upd.update, "yes") || !strcmp(upd.state, "restart")) return "";
    return !strcmp(upd.channel, "stable") ? "Update available" : !strcmp(upd.channel, "testing") ? "Testing: update available"
                                                                                                 : "Custom: update available";
}

static const char *feed_now(void);

/* installed apps the feed has newer, as t48-app last found */
int settings_app_updates(void)
{
    const char *feed = feed_now();
    return manifest_updates(opt.state, feed[0] ? feed : pkg_default_feed());
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
    enum { J_LIST, J_CHANGE, J_FIRMWARE } kind;
    char verb[24], name[64];
    char result[300];
    int ok, listed, blank;   /* blank: nothing was listed when this list was asked for */
} job = {.fd = -1};

static void rebuild(void);
static void redraw_me(void)
{
    shell_settings_changed();
}

static const char *feed_now(void) { return cfg_str("feed", "url", pkg_default_feed()); }

/* the installer on a phone started from a USB stick (t48-install, docs/installer.md) */
static const char *fw_tool(void) { return getenv("T48_INSTALL") ? getenv("T48_INSTALL") : "/usr/bin/t48-install"; }
static int fw_live(void)
{
    const char *f = getenv("T48_LIVE") ? getenv("T48_LIVE") : "/run/t48-live";
    return access(f, F_OK) == 0;
}

/* what t48-install status last said. Asked for when the page opens, when one
 * of its jobs ends and every few seconds while the page waits: never while a
 * job runs, as status mounts the stick */
#define FW_CHECK_MS 3000
static struct {
    int loaded, stick, files, ours, backup, free_mb;   /* backup: 0 none, 1 unfinished, 2 checked */
    char date[12];
    char again[12];               /* a job that stopped part way: it is offered again */
    long long asked;
} fw;

static int fw_status(void)
{
    char cmd[300], line[120], date[12] = "";
    int stick = 0, files = 0, ours = 0, backup = 0, free_mb = 0;
    FILE *f;
    snprintf(cmd, sizeof cmd, "%s status 2>/dev/null", fw_tool());
    if ((f = popen(cmd, "r"))) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strcmp(line, "stick=yes")) stick = 1;
            else if (!strcmp(line, "files=yes")) files = 1;
            else if (!strcmp(line, "flash=ours")) ours = 1;
            else if (!strcmp(line, "backup=checked")) backup = 2;
            else if (!strcmp(line, "backup=unfinished")) backup = 1;
            else if (!strncmp(line, "free_mb=", 8)) free_mb = atoi(line + 8);
            else if (!strncmp(line, "backup_date=", 12)) snprintf(date, sizeof date, "%.10s", line + 12);
        }
        pclose(f);
    }
    int changed = !fw.loaded || stick != fw.stick || files != fw.files || ours != fw.ours || backup != fw.backup ||
                  free_mb != fw.free_mb || strcmp(date, fw.date);
    fw.loaded = 1;
    fw.stick = stick; fw.files = files; fw.ours = ours; fw.backup = backup; fw.free_mb = free_mb;
    snprintf(fw.date, sizeof fw.date, "%s", date);
    fw.asked = t48_now_ms();
    return changed;
}

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
        if (kind == J_FIRMWARE) execl(fw_tool(), "t48-install", "--machine", cmd, (char *)NULL);
        else execl("/proc/self/exe", "t48-app", "--machine", "--apps", opt.apps, "--config", opt.config, "--data", opt.data,
                   "--state", opt.state, cmd, arg, (char *)NULL);
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
    logmsg("%s %s %s", kind == J_FIRMWARE ? "install: t48-install" : "apps: t48-app", cmd, arg ? arg : "");
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
        const char *doing = !strcmp(what, "index") ? "Reading feed" : !strcmp(what, "download") ? "Downloading" :
                            !strcmp(what, "verify") ? "Verifying" : !strcmp(what, "unpack") ? "Unpacking" : what;
        snprintf(ov.text, sizeof ov.text, "%s", doing);
        redraw_me();
    } else if (!strncmp(line, "step ", 5) && job.kind == J_FIRMWARE && ov.kind == O_BUSY) {
        char *text = NULL;
        ov.percent = (int)strtol(line + 5, &text, 10);
        snprintf(ov.text, sizeof ov.text, "%s", *text == ' ' ? text + 1 : text);
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

static void fw_eject(const char *done);

int settings_reaped(pid_t pid, int status)
{
    if (update_reaped(pid, status)) return 1;
    if (job.pid <= 0 || pid != job.pid) return 0;
    if (job.fd >= 0) { settings_job_readable(); if (job.fd >= 0) close(job.fd); }
    job.fd = -1;
    job.pid = 0;
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (job.kind == J_LIST) {
        store_state = ST_LOADED;
        if (!job.listed) nstore = 0;
        if (!ok && !feed_error[0]) snprintf(feed_error, sizeof feed_error, "installer stopped");
        logmsg("apps: %d listed%s%s", nstore, feed_error[0] ? "; feed: " : "", feed_error);
        /* the list has just appeared where there was none: start at its top */
        if (depth >= 0 && stack[depth].page == P_STORE && job.blank) stack[depth].sel = stack[depth].top = 0;
    } else if (job.kind == J_FIRMWARE) {
        ok = ok && job.ok;
        logmsg("install: %s: %s", job.verb, ok ? job.result : job.result[0] ? job.result : "stopped");
        int wrote = !strcmp(job.verb, "install") || !strcmp(job.verb, "restore");
        snprintf(fw.again, sizeof fw.again, "%s", wrote && !ok ? job.verb : "");
        fw_status();
        if (ok && wrote) fw_eject(job.result);
        else if (ok) message(job.result, "");
        else message("Stopped", job.result[0] ? job.result : "Installer stopped");
    } else {
        char t[120];
        ok = ok && job.ok;
        logmsg("apps: %s %s: %s", job.verb, job.name, ok ? "done" : job.result[0] ? job.result : "failed");
        if (ok) {
            snprintf(t, sizeof t, "%s %s", job.name, job.verb);
            message(t, "");
        } else {
            snprintf(t, sizeof t, "%s not %s", job.name, job.verb);
            if (job.result[0] >= 'a' && job.result[0] <= 'z') job.result[0] -= 32;
            message(t, job.result[0] ? job.result : "Installer stopped");
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
    if (job.pid > 0) { message("Busy", "Another install is running"); return; }
    /* an app cannot be changed under its own feet */
    if (!strcmp(app_id(), id)) app_quit();
    snprintf(job.verb, sizeof job.verb, "%s", act == A_REMOVE ? "removed" : act == A_UPDATE ? "updated" : "installed");
    snprintf(job.name, sizeof job.name, "%s", a ? a->name : id);
    if (job_start(J_CHANGE, cmd, id) < 0) { message("Not started", "Could not run the installer"); return; }
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
    if (flags & F_PASSWORD) snprintf(r->value, sizeof r->value, "%s", cfg_str(file, key, "")[0] ? "Set" : "Not set");
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
    else snprintf(v, sizeof v, "Not set");
    int extra = 0;
    for (int i = 2; i <= T48_ACCOUNTS; i++) {
        char f[8];
        snprintf(f, sizeof f, "sip%d", i);
        if (cfg_str(f, "user", "")[0]) extra++;
    }
    if (extra && user[0]) { size_t at = strlen(v); snprintf(v + at, sizeof v - at, " +%d", extra); }
    else if (extra) snprintf(v, sizeof v, "%d set", extra);
    add_menu("Accounts", P_ACCOUNTS, v);
    /* the same switch as Right on the home screen */
    struct row *dnd = add_action("Do not disturb", A_DND, "");
    dnd->kind = R_TOGGLE;
    snprintf(dnd->value, sizeof dnd->value, "%s", phone_dnd() ? "on" : "off");
    ui_network_status(v, sizeof v, &up);
    add_menu("Network", P_NET, v);
    add_menu("Date and time", P_TIME, cfg_str("system", "timezone", "UTC"));
    add_menu("Sound", P_SOUND, "");
    add_menu("Screen", P_SCREEN, "");
    access_read();
    add_menu("Access", P_ACCESS, acc.dev || !strcmp(cfg_str("access", "ssh", "off"), "on") ? "SSH on" : "SSH off");
    int ups = settings_app_updates();
    if (ups) snprintf(v, sizeof v, "%d installed, %d update%s", napps, ups, ups == 1 ? "" : "s");
    else snprintf(v, sizeof v, "%d installed", napps);
    add_menu("Apps", P_STORE, v);
    add_menu("App settings", P_APPSETTINGS, "");
    add_menu("Update", P_UPDATE, settings_update_notice()[0] ? "Update available" : "");
    add_menu("About", P_ABOUT, "");
    if (fw_live()) add_menu("Install", P_INSTALL, "");
}

/* the settings file of account 1 to 16: sip, sip2, ... sip16 */
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

/* Up to 16 accounts, and which one calls go out from when nothing else is
 * said. Those that are set, and the first that is not, to add one. */
static void build_accounts(void)
{
    char label[40], v[200], choices[64] = "";
    int spare = 0;
    snprintf(title, sizeof title, "Accounts");
    for (int n = 1; n <= T48_ACCOUNTS; n++) {
        const char *f = sip_file(n);
        char user[64] = "", domain[64] = "?", name[40] = "", on[8] = "on";
        if (!cfg_str(f, "user", "")[0] && !cfg_str(f, "domain", "")[0] && spare++) continue;
        cfg_get(f, "user", user, sizeof user);
        cfg_get(f, "domain", domain, sizeof domain);
        cfg_get(f, "label", name, sizeof name);
        cfg_get(f, "enabled", on, sizeof on);
        snprintf(label, sizeof label, "Account %d", n);
        if (!user[0]) snprintf(v, sizeof v, "Not set");
        else snprintf(v, sizeof v, "%s%s%s@%s%s", name, name[0] ? ": " : "", user, domain, strcmp(on, "off") ? "" : " (off)");
        snprintf(add_menu(label, P_SIP, v)->arg, sizeof rows[0].arg, "%d", n);
        if (sip_usable(n)) { size_t at = strlen(choices); snprintf(choices + at, sizeof choices - at, "%s%d", at ? "|" : "", n); }
    }
    if (strchr(choices, '|')) {
        static char keep[64];
        struct row *r;
        snprintf(keep, sizeof keep, "%s", choices);
        snprintf(v, sizeof v, "%d", atoi(phone_state("acct.default")));
        r = add_stored(R_CHOICE, "Default account", "phone", "default_account", v, 0);
        r->choices = keep;
        /* a whole item of 1|2|...: 1 is not in 10|11 */
        char item[8];
        snprintf(item, sizeof item, "%s", r->value);
        size_t il = strlen(item);
        const char *at = keep;
        int found = 0;
        while (!found && *at) {
            size_t k = strcspn(at, "|");
            found = il && k == il && !strncmp(at, item, k);
            at += k + (at[k] == '|');
        }
        if (!found) snprintf(r->value, sizeof r->value, "%s", v);
    }
    add_menu("Advanced", P_ADVANCED, "");
}

/* what is set for all the accounts and seldom changed */
static void build_advanced(void)
{
    snprintf(title, sizeof title, "Advanced");
    /* the digit map that says when a number is complete (docs/phone.md#dial-plan); emptied, the default again */
    add_stored(R_TEXT, "Dial plan", "phone", "dialplan", DP_DEFAULT, 0);
    snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "%s999 and 112 always dial at once",
             cfg_str("phone", "dialplan", "")[0] ? "Empty for the default. " : "");
    /* issue #77: calls only from the accounts' servers; baresip reads it at start */
    add_stored(R_TOGGLE, "Block direct calls", "phone", "block_direct", "off", 0);
}

/* what an account's forwarding comes to, for its row: Off, or the first kind that is on */
static void forward_summary(const char *f, char *out, size_t n)
{
    static const char *const kinds[][2] = {{"always", "Always"}, {"busy", "Busy"}, {"noanswer", "No answer"}};
    char key[40];
    out[0] = 0;
    for (int k = 0; k < 3; k++) {
        snprintf(key, sizeof key, "forward_%s", kinds[k][0]);
        if (strcmp(cfg_str(f, key, "off"), "on")) continue;
        snprintf(key, sizeof key, "forward_%s_to", kinds[k][0]);
        if (!cfg_str(f, key, "")[0]) continue;
        size_t at = strlen(out);
        snprintf(out + at, n - at, "%s%s", at ? ", " : "", kinds[k][1]);
    }
    if (!out[0]) snprintf(out, n, "Off");
}

/* an account's call forwarding (issue #77): each kind on or off, and where to */
static void build_fwd(const char *arg)
{
    int n = atoi(arg) < 1 ? 1 : atoi(arg);
    const char *f = sip_file(n);
    static const char *const kinds[][2] = {{"always", "Always"}, {"busy", "Busy"}, {"noanswer", "No answer"}};
    char key[40], label[40];
    snprintf(title, sizeof title, "Call forwarding %d", n);
    for (int k = 0; k < 3; k++) {
        snprintf(key, sizeof key, "forward_%s", kinds[k][0]);
        int on = !strcmp(add_stored(R_TOGGLE, kinds[k][1], f, key, "off", 0)->value, "on");
        snprintf(key, sizeof key, "forward_%s_to", kinds[k][0]);
        snprintf(label, sizeof label, "%s to", kinds[k][1]);
        add_stored(R_TEXT, label, f, key, "", F_DIGITS | (on ? 0 : F_DISABLED));
        if (k == 2)
            add_stored(R_CHOICE, "No answer after", f, "forward_noanswer_after", "20", F_SECONDS | (on ? 0 : F_DISABLED))->choices =
                "5|10|15|20|25|30|45|60";
    }
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
    add_stored(R_TEXT, "Server host", f, "domain", "", F_HOST);
    add_stored(R_TEXT, "Outbound proxy", f, "outbound_proxy", "", F_PROXY);
    add_stored(R_TEXT, "Auth user", f, "auth_user", "", 0);
    add_stored(R_TEXT, "Display name", f, "display_name", "", 0);
    add_stored(R_CHOICE, "Transport", f, "transport", "udp", 0)->choices = "udp|tcp|tls";
    add_stored(R_TEXT, "Account name", f, "label", "", 0);
    add_stored(R_TEXT, "Voicemail", f, "voicemail", "", F_DIGITS);
    {
        /* issue #77: the ringtone, and Custom once a sound has been given on the web setup page */
        char path[300], fwd[60];
        struct stat st;
        ringtone_path(f, path, sizeof path);
        add_stored(R_CHOICE, "Ringtone", f, "ringtone", "standard", 0)->choices =
            stat(path, &st) == 0 ? "standard|long|chime|pulse|custom" : "standard|long|chime|pulse";
        forward_summary(f, fwd, sizeof fwd);
        snprintf(add_menu("Call forwarding", P_FWD, fwd)->arg, sizeof rows[0].arg, "%d", n);
    }
    add_stored(R_TOGGLE, "Enabled", f, "enabled", "on", 0);
    snprintf(add_menu("Advanced", P_SIPADV, "")->arg, sizeof rows[0].arg, "%d", n);
    snprintf(acc, sizeof acc, "reg.%d", n);
    const char *reg = phone_state(acc);
    add_info("Status", !strcmp(phone_state("sip"), "up") ? (!strcmp(reg, "ok") ? "Registered" : !strcmp(reg, "fail") ? "Not registered" :
             !strcmp(reg, "trying") ? "Registering" : "Off") : "No service");
    if (cfg_str(f, "user", "")[0] || cfg_str(f, "domain", "")[0])
        snprintf(add_action("Remove account", A_ACCOUNT_REMOVE, "")->def, sizeof rows[0].def, "%s", f);
    rows[nrows - 1].flags |= rows[nrows - 1].kind == R_ACTION ? F_DANGER : 0;
}

/* the account's settings that are seldom changed (issue #77) */
static void build_sipadv(const char *arg)
{
    char v[60];
    int n = atoi(arg) < 1 ? 1 : atoi(arg), order[T48_CODECS], on[T48_CODECS];
    const char *f = sip_file(n);
    snprintf(title, sizeof title, "Advanced");
    snprintf(v, sizeof v, "%d", T48_REG_DEFAULT);
    add_stored(R_TEXT, "Registration expiry", f, "register_seconds", v, F_DIGITS | F_SECONDS | F_REGINT);
    add_stored(R_CHOICE, "DTMF", f, "dtmf", "rfc2833", 0)->choices = "rfc2833|inband|info";
    sip_codecs(cfg_str(f, "codecs", ""), order, on);
    v[0] = 0;
    for (int i = 0; i < T48_CODECS; i++)
        if (on[i]) { size_t at = strlen(v); snprintf(v + at, sizeof v - at, "%s%s", at ? ", " : "", sip_codec_names[order[i]]); }
    snprintf(add_menu("Codecs", P_CODECS, v)->arg, sizeof rows[0].arg, "%d", n);
    add_stored(R_TOGGLE, "NAT keepalive", f, "keepalive", "on", 0);
    /* interoperability (docs/sip.md#interoperability); 100rel's required is offered only to a file that already says it */
    add_stored(R_CHOICE, "Session timer", f, "session_timer", "off", 0)->choices = "off|300|600|1800|3600";
    struct row *r = add_stored(R_CHOICE, "100rel", f, "100rel", "off", 0);
    r->choices = !strcmp(r->value, "required") ? "off|on|required" : "off|on";
}

/* the codecs an account offers, in order: each a switch, moved with Up and Down */
static void build_codecs(const char *arg)
{
    int n = atoi(arg) < 1 ? 1 : atoi(arg), order[T48_CODECS], on[T48_CODECS];
    const char *f = sip_file(n);
    snprintf(title, sizeof title, "Codecs");
    sip_codecs(cfg_str(f, "codecs", ""), order, on);
    for (int i = 0; i < T48_CODECS; i++) {
        struct row *r = add(R_TOGGLE, sip_codec_names[order[i]]);
        snprintf(r->file, sizeof r->file, "%s", f);
        snprintf(r->key, sizeof r->key, "codecs");
        snprintf(r->value, sizeof r->value, "%s", on[i] ? "on" : "off");
        r->flags = F_CODEC;
    }
}

static void build_net(void)
{
    char v[100];
    int up, fixed = !strcmp(cfg_str("network", "mode", "dhcp"), "static");
    snprintf(title, sizeof title, "Network");
    add_stored(R_CHOICE, "Mode", "network", "mode", "dhcp", 0)->choices = "dhcp|static";
    add_stored(R_TEXT, "Address", "network", "address", "", F_DIGITS | F_IP | (fixed ? 0 : F_DISABLED));
    add_stored(R_TEXT, "Netmask", "network", "netmask", "255.255.255.0", F_DIGITS | F_IP | (fixed ? 0 : F_DISABLED));
    add_stored(R_TEXT, "Gateway", "network", "gateway", "", F_DIGITS | F_IP | (fixed ? 0 : F_DISABLED));
    add_stored(R_TEXT, "DNS", "network", "dns", "", F_DIGITS | F_IP | (fixed ? 0 : F_DISABLED));
    ui_network_status(v, sizeof v, &up);
    add_info("Status", v);
    if (fixed) snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Applied when you leave this page");
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
    add_info("Clock source", "NTP");
}

static void build_tz(void)
{
    const char *cur = cfg_str("system", "timezone", "UTC");
    snprintf(title, sizeof title, "Time zone");
    for (int i = 0; i < NZONES; i++) {
        struct row *r = add_action(zones[i].name, A_SET_TZ, "");
        snprintf(r->def, sizeof r->def, "%s", zones[i].name);
        if (!strcmp(cur, zones[i].name)) snprintf(r->value, sizeof r->value, "Current");
    }
}

static void build_sound(void)
{
    static const char *const names[] = {"", "Handset", "Speaker", "Headset", "Ringer", "Apps"};
    static const int order[] = {T48AC_VOL_RINGER, T48AC_VOL_HANDSET, T48AC_VOL_SPEAKER, T48AC_VOL_HEADSET, T48AC_VOL_APP};
    snprintf(title, sizeof title, "Sound");
    for (int i = 0; i < 5; i++) {
        struct row *r = add(R_LEVEL, names[order[i]]);
        r->target = order[i];
        r->level = phone_volume(order[i]);
        r->level_max = T48AC_VOL_MAX;
    }
    if (!audioc_connected()) add_info("Audio service", "Not running");
}

/* "2 min", "45 s", "Never" */
static const char *idle_time(int secs)
{
    static char b[2][16];
    static int at;
    char *o = b[at = !at];
    if (secs <= 0) snprintf(o, sizeof b[0], "Never");
    else if (secs % 60) snprintf(o, sizeof b[0], "%d s", secs);
    else snprintf(o, sizeof b[0], "%d min", secs / 60);
    return o;
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
    add_menu("Theme", P_THEME, ui_theme_label(T->name));
    if (!have) { add_info("Backlight", "None"); return; }
    add_info("Dim after", idle_time(atoi(cfg_str("screen", "dim_seconds", "120"))));
    add_info("Off after", idle_time(atoi(cfg_str("screen", "off_seconds", "1800"))));
}

/* the themes, side by side, each as a small picture of itself */
static void build_theme(void)
{
    snprintf(title, sizeof title, "Theme");
    for (int i = 0; i < t48_theme_count; i++) {
        struct row *r = add_action(ui_theme_label(t48_themes[i].name), A_SET_THEME, "");
        snprintf(r->def, sizeof r->def, "%s", t48_themes[i].name);
        if (!strcmp(T->name, t48_themes[i].name)) snprintf(r->value, sizeof r->value, "Current");
    }
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
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Development build: SSH always on, built-in keys accepted");
    }
    if (on) {
        char net[80];
        ui_network_status(net, sizeof net, &up);
        snprintf(v, sizeof v, "ssh root@%s", up ? net : "ADDRESS");
        add_info("Login", !acc.known || acc.running ? v : "Not running");
    }
    snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Host key %s", acc.host_key[0] ? acc.host_key : "unknown");
    if (!acc.nkeys) add_info("Keys", "None");
    for (int i = 0; i < acc.nkeys && i < MAX_KEYS && acc.key[i][0]; i++) {
        snprintf(v, sizeof v, "Key %d", i + 1);
        add_info(v, acc.key[i]);
    }
    if (acc.nkeys > MAX_KEYS) { snprintf(v, sizeof v, "+%d more", acc.nkeys - MAX_KEYS); add_info("", v); }
    if (acc.door) {
        door_left(v, sizeof v);
        snprintf(add_action("Password login", A_DOOR_SHOW, "")->value, sizeof rows[0].value, "Open, %s left", v);
        add_action("Close password login", A_DOOR_CLOSE, "");
    } else {
        r = add_action("Add key", A_DOOR_OPEN, "");
        if (!on) r->flags = F_DISABLED;
    }
    if (acc.nkeys) add_action(acc.dev ? "Remove added keys" : "Remove all keys", A_FORGET_KEYS, "")->flags = F_DANGER;
    /* the web setup page (issue #37, docs/web.md) */
    int web = !strcmp(cfg_str("access", "web", "off"), "on");
    r = add_action("Web setup", A_WEB, "");
    r->kind = R_TOGGLE;
    snprintf(r->value, sizeof r->value, "%s", web ? "on" : "off");
    if (web) {
        char net[80];
        ui_network_status(net, sizeof net, &up);
        if (watch_web_url()[0]) snprintf(v, sizeof v, "%s", watch_web_url());
        else if (up) snprintf(v, sizeof v, "http://%s/", net);
        else snprintf(v, sizeof v, "No network");
        add_info("Address", v);
        snprintf(v, sizeof v, "%d", watch_web_paired());
        add_info("Paired browsers", v);
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Turning off signs out every browser");
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
    add_action("Key test", A_KEYTEST, "");   /* first: a phone whose keys are in doubt gets there on Down and OK alone */
    add_info("Phone", "Yealink SIP-T48S, open firmware");
    firmware_version(v, sizeof v);
    add_info("Firmware", v);
    add_info("Shell", T48_VERSION ", built " __DATE__);
    snprintf(v, sizeof v, "%d", T48_API_LEVEL);
    add_info("App API", v);
    if (uname(&u) == 0) { snprintf(v, sizeof v, "%s %s", u.sysname, u.release); add_info("Kernel", v); }
    ui_network_status(v, sizeof v, &up);
    add_info("Address", v);
    if ((f = fopen("/sys/class/net/eth0/address", "r"))) {
        if (fgets(v, sizeof v, f)) { v[strcspn(v, "\r\n")] = 0; add_info("MAC", v); }
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
        snprintf(v, sizeof v, "%ld of %ld MB free", (avail ? avail : fr) / 1024, total / 1024);
        add_info("Memory", v);
    }
    snprintf(path, sizeof path, "%.*s", (int)strcspn(opt.apps, ":"), opt.apps);
    if (statvfs(path, &vfs) == 0 && vfs.f_blocks > 0) {
        snprintf(v, sizeof v, "%llu MB free", (unsigned long long)vfs.f_bavail * vfs.f_frsize / 1048576);
        add_info("App storage", v);
    } else {
        add_info("App storage", "RAM, lost at power off");
    }
    add_info("Licence", "AGPL-3.0-or-later");
    add_info("Source", "github.com/M0LTE/yealink-t48s");
}

/* "23 Nov 16:25", as the call list has it */
static void list_time(char *out, size_t n, long when)
{
    time_t t = when;
    struct tm tm;
    char b[40];
    localtime_r(&t, &tm);
    strftime(b, sizeof b, "%e %b %H:%M", &tm);
    snprintf(out, n, "%s", b[0] == ' ' ? b + 1 : b);
}

static void build_update(void)
{
    char v[200];
    struct row *r;
    snprintf(title, sizeof title, "Update");
    if (!upd.loaded) { snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Loading"); return; }
    if (upd.installed && upd.slot[0]) snprintf(v, sizeof v, "%s, slot %s", upd.version, upd.slot);
    else snprintf(v, sizeof v, "%s", upd.version);
    add_info("Version", v);
    if (!upd.installed) { add_info("Status", "Not installed"); return; }
    int busy = update_busy(), restart = !strcmp(upd.state, "restart");
    if (!strcmp(upd.channel, "stable") || !strcmp(upd.channel, "testing")) {
        r = add(R_CHOICE, "Channel");
        snprintf(r->value, sizeof r->value, "%s", upd.channel);
        r->choices = "stable|testing";
        r->target = A_UPD_CHANNEL;
        if (busy) r->flags = F_DISABLED;
    } else {
        /* the owner's own builds: set from the command line or the web setup page */
        add_info("Channel", "Custom");
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "%s", upd.channel);
    }
    /* firmware and apps, once a night (issue #80): it finds, and never installs */
    add_stored(R_TOGGLE, "Daily check", "update", "daily", "on", 0);
    add_info("Latest", upd.latest[0] ? upd.latest : "Not checked");
    const char *st = !strcmp(upd.state, "checking") ? "Checking" : !strcmp(upd.state, "downloading") ? "Downloading" :
                     !strcmp(upd.state, "writing") ? "Writing" : restart ? "Restart to finish" :
                     !strcmp(upd.state, "failed") ? (upd.message[0] ? upd.message : "Failed") :
                     !strcmp(upd.update, "yes") ? "Update available" : !strcmp(upd.update, "no") ? "Up to date" : "";
    if (st[0]) add_info("Status", st);
    r = add_action("Check now", A_UPD_CHECK, "");
    if (busy) r->flags = F_DISABLED;
    if (!strcmp(upd.update, "yes") && !restart) {
        r = add_action("Install", A_UPD_INSTALL, "");
        if (busy) r->flags = F_DISABLED;
    }
    if (restart) add_action("Restart", A_UPD_RESTART, "");
    if (!strcmp(upd.kernel, "differs")) {
        r = add_action("Install kernel", A_UPD_KERNEL, "");
        if (busy) r->flags = F_DISABLED;
    }
    if (upd.result[0]) {
        size_t at = snprintf(v, sizeof v, "%s", upd.result);
        if (upd.result_time > 0 && at < sizeof v - 20) { snprintf(v + at, sizeof v - at, ", "); list_time(v + at + 2, sizeof v - at - 2, upd.result_time); }
        add_info("Last result", v);
    }
}

/* start on Check now: the first row, the channel, changes at a press */
static void update_select(void)
{
    for (int i = 0; i < nrows; i++)
        if (rows[i].kind == R_ACTION) { stack[depth].sel = i; break; }
    rebuild();
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
    if (!n) snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "No apps with settings");
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
    snprintf(add_action("Reset to defaults", A_APP_DEFAULTS, m->id)->def, sizeof rows[0].def, "%s", m->id);
}

static void build_store(void)
{
    int updates = 0;
    snprintf(title, sizeof title, "Apps");
    if (store_state == ST_NONE) store_refresh();
    if (store_state == ST_LOADING && !nstore) {
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Loading");
    } else {
        if (feed_error[0] && store_state == ST_LOADED) {
            struct row *r = add(R_NOTE, "");
            snprintf(r->value, sizeof r->value, "Feed error: %s", feed_error);
            r->flags = F_DANGER;
        }
        for (int i = 0; i < nstore; i++) {
            const struct store_app *a = &store[i];
            char v[100], size[24];
            kb(size, sizeof size, a->disk_bytes ? a->disk_bytes : a->pkg_bytes);
            if (!strcmp(a->state, "update")) { snprintf(v, sizeof v, "Update %s", a->offered); updates++; }
            else if (!strcmp(a->state, "installed")) snprintf(v, sizeof v, "Installed %s", a->have);
            else if (!strcmp(a->state, "builtin")) snprintf(v, sizeof v, "Built in %s", a->have);
            else if (!strcmp(a->state, "unsupported")) snprintf(v, sizeof v, "Needs newer firmware");
            else snprintf(v, sizeof v, "%s, %s", a->offered, size);
            snprintf(add_menu(a->name, P_STOREAPP, v)->arg, sizeof rows[0].arg, "%s", a->id);
        }
        if (updates > 1) add_action("Update all", A_UPDATE_ALL, "");
        add_action("Refresh", A_REFRESH, "");
    }
    add_stored(R_TEXT, "Feed URL", "feed", "url", pkg_default_feed(), 0);
    if (strcmp(feed_now(), pkg_default_feed())) add_action("Use default feed", A_FEED_DEFAULT, "");
}

static void build_storeapp(const char *id)
{
    const struct store_app *a = store_find(id);
    const struct manifest *m = installed(id);
    char v[200];
    snprintf(title, sizeof title, "%s", a ? a->name : m ? m->name : id);
    if (!a) { snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, store_state == ST_LOADING ? "Loading" : "Not in the feed"); return; }
    int inst = strcmp(a->have, "-") != 0, update = !strcmp(a->state, "update");
    if (!strcmp(a->state, "available")) add_action("Install", A_INSTALL, id);
    if (update) add_action("Update", A_UPDATE, id);
    if (inst) add_action("Open", A_OPEN, id);
    if (inst && m && m->nsettings) snprintf(add_menu("Settings", P_APPCONF, "")->arg, sizeof rows[0].arg, "%s", id);
    if (a->description[0]) snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "%s", a->description);
    if (!strcmp(a->state, "unsupported")) {
        struct row *r = add(R_NOTE, "");
        snprintf(r->value, sizeof r->value, "Needs an app API above %d: update the firmware", T48_API_LEVEL);
        r->flags = F_DANGER;
    }
    if (inst) add_info("Installed", a->have);
    if (strcmp(a->offered, "-")) add_info(inst ? "In the feed" : "Version", a->offered);
    if (a->disk_bytes) { kb(v, sizeof v, a->disk_bytes); add_info("Size", v); }
    if (a->pkg_bytes) { kb(v, sizeof v, a->pkg_bytes); add_info("Download", v); }
    if (!a->perms[0]) snprintf(v, sizeof v, "None");
    else {
        v[0] = 0;
        char perms[96], *save = NULL;
        snprintf(perms, sizeof perms, "%s", a->perms);
        for (char *p = strtok_r(perms, " ", &save); p; p = strtok_r(NULL, " ", &save)) {
            size_t at = strlen(v);
            snprintf(v + at, sizeof v - at, "%s%s", at ? ", " : "",
                     !strcmp(p, "network") ? "Network" : !strcmp(p, "audio-out") ? "Sound" : !strcmp(p, "lamps") ? "Lamps" : p);
        }
    }
    add_info("Permissions", v);
    if (!strcmp(a->state, "builtin")) add_info("Source", "Built in");
    else if (inst) add_action("Remove", A_REMOVE, id)->flags = F_DANGER;
}

/* Install: one step at a time. A phone on Yealink's firmware is backed up, then
 * installed; an installed one can be restored. Only the next step is offered */
static void build_install(void)
{
    char v[120];
    snprintf(title, sizeof title, "Install");
    if (!fw.loaded) fw_status();
    int restore = fw.ours || !strcmp(fw.again, "restore");
    add_info("Phone", fw.ours ? "Open firmware" : "Yealink firmware");
    if (fw.stick) snprintf(v, sizeof v, "%d MB free", fw.free_mb); else snprintf(v, sizeof v, "Not found");
    add_info("Stick", v);
    if (!fw.stick) return;
    if (fw.backup == 2) snprintf(v, sizeof v, "Checked, %s", fw.date); else snprintf(v, sizeof v, "%s", fw.backup ? "Unfinished" : "None");
    add_info("Backup", v);
    if (restore) {
        if (fw.backup == 2) add_action("Restore Yealink firmware", A_FW_RESTORE, "")->flags = F_DANGER;
        else snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Restore needs the stick that holds this phone's backup");
    } else if (fw.backup != 2) {
        snprintf(add_action("Back up", A_FW_BACKUP, "")->value, sizeof rows[0].value, "Step 1 of 2");
    } else {
        struct row *r = add_action("Install", A_FW_INSTALL, "");
        snprintf(r->value, sizeof r->value, "%s", fw.files ? "Step 2 of 2" : "No install files");
        if (!fw.files) r->flags = F_DISABLED;
    }
    if (restore ? fw.backup == 2 : 1)
        snprintf(add(R_NOTE, "")->value, sizeof rows[0].value, "Power supply: 5 V, 1.5 A or more. Do not unplug while writing");
}

static void fw_job(const char *cmd, const char *doing)
{
    if (job.pid > 0) { message("Busy", "Another install is running"); return; }
    snprintf(job.verb, sizeof job.verb, "%s", cmd);
    if (job_start(J_FIRMWARE, cmd, NULL) < 0) { message("Not started", "Could not run the installer"); return; }
    memset(&ov, 0, sizeof ov);
    ov.kind = O_BUSY;
    ov.percent = 0;
    snprintf(ov.title, sizeof ov.title, "%s", doing);
    snprintf(ov.text, sizeof ov.text, "Starting");
}

/* installed, or restored: the stick is let go, and the phone restarts when it is
 * taken out. Nothing to press, so nothing to press in the wrong order: Yealink's
 * firmware must not find the stick at its next start */
static void fw_eject(const char *done)
{
    char t[96];
    snprintf(t, sizeof t, "%s", done);   /* done is the last job's result, which starting a job clears */
    snprintf(job.verb, sizeof job.verb, "eject");
    if (job_start(J_FIRMWARE, "eject", NULL) < 0) { message(t, "Take the stick out, then unplug the power"); return; }
    memset(&ov, 0, sizeof ov);
    ov.kind = O_BUSY;
    ov.plain = 1;
    snprintf(ov.title, sizeof ov.title, "%s", t);
    snprintf(ov.text, sizeof ov.text, "Take the stick out to restart");
}

static void rebuild(void)
{
    char keep[96] = "";
    if (depth < 0) return;
    /* the Update page gains and loses rows as work goes on: the chosen action stays chosen */
    if (stack[depth].page == P_UPDATE && stack[depth].sel < nrows && rows[stack[depth].sel].kind == R_ACTION)
        snprintf(keep, sizeof keep, "%s", rows[stack[depth].sel].label);
    nrows = 0;
    switch (stack[depth].page) {
    case P_ROOT: build_root(); break;
    case P_ACCOUNTS: build_accounts(); break;
    case P_SIP: build_sip(stack[depth].arg); break;
    case P_SIPADV: build_sipadv(stack[depth].arg); break;
    case P_CODECS: build_codecs(stack[depth].arg); break;
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
    case P_THEME: build_theme(); break;
    case P_UPDATE: build_update(); break;
    case P_INSTALL: build_install(); break;
    case P_FWD: build_fwd(stack[depth].arg); break;
    case P_ADVANCED: build_advanced(); break;
    }
    for (int i = 0; keep[0] && i < nrows; i++)
        if (rows[i].kind == R_ACTION && !strcmp(rows[i].label, keep)) stack[depth].sel = i;
    /* the Install page has one thing to choose, and it stays chosen as the steps change */
    for (int i = 0; stack[depth].page == P_INSTALL && i < nrows; i++)
        if (rows[i].kind == R_ACTION) stack[depth].sel = i;
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
    if (p == P_UPDATE) { upd.loaded = 0; update_info(); }   /* what the updater says now; no check of the channel */
    if (p == P_INSTALL) { fw.loaded = 0; fw.again[0] = 0; }
    rebuild();
    /* start on the first row that does something */
    while (stack[depth].sel < nrows - 1 && (rows[stack[depth].sel].kind == R_NOTE || rows[stack[depth].sel].kind == R_INFO)) stack[depth].sel++;
    if (p == P_TZ || p == P_THEME)
        for (int i = 0; i < nrows; i++)
            if (rows[i].value[0]) stack[depth].sel = i;
    rebuild();
    if (p == P_UPDATE) update_select();
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
    /* with an update noticed on the home screen, OK opens it */
    enum page want = app_manager ? P_ROOT : settings_update_notice()[0] ? P_UPDATE : settings_app_updates() ? P_STORE : P_ROOT;
    for (int i = 0; want != P_ROOT && i < nrows; i++)
        if (rows[i].kind == R_MENU && rows[i].target == (int)want) { stack[depth].sel = i; rebuild(); break; }
}

/* a touch on a notice: Settings, Update or Settings, Apps, with the list behind it */
void settings_open_page(const char *name)
{
    settings_open(0);
    if (!strcmp(name, "apps")) { store_state = ST_NONE; nstore = 0; push(P_STORE, NULL); }
    else push(P_UPDATE, NULL);
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
    /* a call forwarded to the account itself would come straight back */
    if (value && !strncmp(r->file, "sip", 3) && !strncmp(r->key, "forward_", 8) && strstr(r->key, "_to") &&
        forward_to_self(value, cfg_str(r->file, "user", ""), cfg_str(r->file, "domain", ""))) {
        message("Not saved", "That is this account");
        return;
    }
    if (cfg_set(r->file, r->key, value) < 0) {
        message("Not saved", errno == EINVAL ? "Invalid characters" : strerror(errno));
        return;
    }
    logmsg("settings: %s %s set", r->file, r->key);   /* never the value */
    /* the shell's own account keys (issue #77): no need to restart baresip */
    if (!strncmp(r->file, "sip", 3) && (!strcmp(r->key, "ringtone") || !strncmp(r->key, "forward_", 8))) {
        phone_reload_account();
        return;
    }
    /* read by t48-sip when baresip starts */
    if (!strcmp(r->file, "phone") && !strcmp(r->key, "block_direct")) snprintf(dirty, sizeof dirty, "sip");
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
    case A_SET_THEME:
        /* at once, here and in an open app; the web setup page follows the store */
        cfg_set("screen", "theme", r->def);
        ui_theme_set(r->def);
        shell_theme_changed();
        break;
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
        snprintf(ov.text, sizeof ov.text, "Deletes its data and settings");
        break;
    }
    case A_OPEN: shell_settings_open_app(r->arg); return;
    case A_KEYTEST: shell_show_keytest(); return;
    case A_APP_DEFAULTS: {
        char path[400];
        snprintf(path, sizeof path, "%s/apps/%s.conf", opt.config, r->def);
        unlink(path);
        if (!strcmp(app_id(), r->def)) app_quit();
        break;
    }
    case A_FW_BACKUP: fw_job("backup", "Backing up"); break;
    case A_FW_INSTALL:
    case A_FW_RESTORE:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = (enum action)r->target;
        if (r->target == A_FW_INSTALL) {
            ov.yes = "Install";
            snprintf(ov.title, sizeof ov.title, "Install?");
            snprintf(ov.text, sizeof ov.text, "Replaces Yealink's firmware. The backup on the stick restores it");
        } else {
            ov.yes = "Restore";
            snprintf(ov.title, sizeof ov.title, "Restore Yealink firmware?");
            snprintf(ov.text, sizeof ov.text, "Removes the open firmware and its settings");
        }
        snprintf(ov.extra, sizeof ov.extra, "About 3 min. Do not unplug");
        break;
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
            snprintf(ov.title, sizeof ov.title, "Turn on SSH?");
            snprintf(ov.text, sizeof ov.text, "Root access for anyone on the network with an accepted key");
            snprintf(ov.extra, sizeof ov.extra, "Host key %s", acc.host_key[0] ? acc.host_key : "unknown");
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
            snprintf(ov.title, sizeof ov.title, "Turn on web setup?");
            snprintf(ov.text, sizeof ov.text, "Plain HTTP. A browser pairs with a code shown here");
        }
        break;
    case A_DOOR_OPEN:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_DOOR_OPEN;
        ov.yes = "Open";
        snprintf(ov.title, sizeof ov.title, "Open password login?");
        snprintf(ov.text, sizeof ov.text, "For ssh-copy-id. Closes after 10 minutes, or when a key arrives");
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
        snprintf(ov.title, sizeof ov.title, "Remove account %d?", atoi(stack[depth].arg) < 1 ? 1 : atoi(stack[depth].arg));
        break;
    case A_FORGET_KEYS:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_FORGET_KEYS;
        snprintf(ov.title, sizeof ov.title, "Remove %s?", acc.dev ? "added keys" : "all keys");
        snprintf(ov.text, sizeof ov.text, "%s", acc.dev ? "Built-in keys stay" : "");
        break;
    case A_UPD_CHECK: update_run("check", NULL); break;
    case A_UPD_INSTALL:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_UPD_INSTALL;
        ov.yes = "Install";
        snprintf(ov.title, sizeof ov.title, "Install %s?", upd.latest);
        break;
    case A_UPD_KERNEL:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_UPD_KERNEL;
        ov.yes = "Install";
        snprintf(ov.title, sizeof ov.title, "Install kernel?");
        snprintf(ov.text, sizeof ov.text, "A failed kernel needs recovery mode");
        break;
    case A_UPD_RESTART:
        memset(&ov, 0, sizeof ov);
        ov.kind = O_CONFIRM;
        ov.act = A_UPD_RESTART;
        ov.yes = "Restart";
        snprintf(ov.title, sizeof ov.title, "Restart phone?");
        break;
    case A_DND: phone_toggle_dnd(); break;
    case A_UPD_CHANNEL:
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
        if (make_password(door_password) < 0) { message("Not opened", "No random source"); break; }
        door_keys_before = acc.nkeys;
        door_asked = t48_now_ms();
        run_apply_with("access-door-open", door_password);
        memset(&ov, 0, sizeof ov);
        ov.kind = O_DOOR;
        break;
    case A_FORGET_KEYS: run_apply("access-forget-keys"); break;
    case A_UPD_INSTALL: update_run("install", NULL); break;
    case A_UPD_KERNEL: update_run("kernel", NULL); break;
    case A_UPD_RESTART:
        run_apply("restart");
        message("Restarting", "");
        break;
    case A_FW_INSTALL: fw_job("install", "Installing"); break;
    case A_FW_RESTORE: fw_job("restore", "Restoring"); break;
    case A_ACCOUNT_REMOVE: {
        char path[400];
        if (strncmp(arg, "sip", 3) || strchr(arg, '/')) break;
        snprintf(path, sizeof path, "%s/%s", opt.config, arg);
        unlink(path);
        ringtone_path(arg, path, sizeof path);   /* its custom ringtone, if it had one */
        unlink(path);
        logmsg("settings: account file %s removed", arg);
        snprintf(dirty, sizeof dirty, "sip");
        pop();
        break;
    }
    default: break;
    }
}

/* The Codecs page: the codec in row i turned on or off (move 0), or moved
 * up (-1) or down (1) the order. The page's rows are the stored order. */
static void codec_change(int i, int move)
{
    int order[T48_CODECS], on[T48_CODECS], j = i + move, n = 0;
    char v[64];
    struct row r = rows[i];
    if (i < 0 || i >= T48_CODECS || j < 0 || j >= T48_CODECS) return;
    sip_codecs(cfg_str(r.file, "codecs", ""), order, on);
    if (!move) {
        for (int k = 0; k < T48_CODECS; k++) n += on[k];
        if (on[i] && n == 1) { message("Not turned off", "One codec must stay on"); return; }
        on[i] = !on[i];
    } else {
        int o = order[i], f = on[i];
        order[i] = order[j]; on[i] = on[j];
        order[j] = o; on[j] = f;
        stack[depth].sel = j;
    }
    sip_codecs_str(order, on, v, sizeof v);
    store_value(&r, v);
    rebuild();
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
        t48_kbd_open(&kbd, (r->flags & F_PASSWORD) ? "New password (empty: no change)" : r->label,
                     (r->flags & F_PASSWORD) ? "" : r->value, 160,
                     ((r->flags & F_PASSWORD) ? T48_KBD_PASSWORD : 0) | ((r->flags & F_DIGITS) ? T48_KBD_DIGITS : 0));
        kbd_open = 1;
        break;
    case R_CHOICE: {
        /* step to the next (or the one before) of a|b|c */
        char list[100], *opts[T48_ACCOUNTS], *save = NULL;
        int n = 0, at = 0;
        snprintf(list, sizeof list, "%s", r->choices);
        for (char *p = strtok_r(list, "|", &save); p && n < T48_ACCOUNTS; p = strtok_r(NULL, "|", &save)) {
            if (!strcmp(p, r->value)) at = n;
            opts[n++] = p;
        }
        if (r->target == A_UPD_CHANNEL) update_run("channel", opts[(at + (dir < 0 ? n - 1 : 1)) % n]);
        else store_value(r, opts[(at + (dir < 0 ? n - 1 : 1)) % n]);
        rebuild();
        break;
    }
    case R_TOGGLE: {
        int on = !strcmp(r->value, "on");
        if (r->flags & F_CODEC) { codec_change(i, 0); break; }
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
        message("Not an address", "Use the form 192.168.1.20");
        return;
    }
    if (((kbd_row.flags & F_HOST) && !sip_host_ok(text, 0)) || ((kbd_row.flags & F_PROXY) && !sip_host_ok(text, 1))) {
        message("Not a host", "Use host or host:port, IPv6 in [ ]");
        return;
    }
    if ((kbd_row.flags & F_REGINT) && !sip_regint_ok(text)) {
        message("Out of range", "Use 0, 60 to 86400, or empty for 600");
        return;
    }
    if (!strcmp(kbd_row.file, "phone") && !strcmp(kbd_row.key, "dialplan") && text[0] && dp_check(text, NULL) < 0) {
        message("Not a dial plan", "Use the form 07xxxxxxxxx|x.T");
        return;
    }
    /* the default as it stands is not stored, so a better one in a later release reaches this phone */
    if (!strcmp(kbd_row.file, "phone") && !strcmp(kbd_row.key, "dialplan") && !strcmp(text, DP_DEFAULT)) {
        store_value(&kbd_row, NULL);
        rebuild();
        return;
    }
    if (!strcmp(kbd_row.file, "feed") && !strcmp(kbd_row.key, "url") && strncmp(text, "https://", 8) &&
        strncmp(text, "file://", 7)) {
        message("Not a feed URL", "Must start with https://");
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
    if (stack[depth].page == P_THEME && (code == T48_KEY_LEFT || code == T48_KEY_RIGHT))
        code = code == T48_KEY_LEFT ? T48_KEY_UP : T48_KEY_DOWN;   /* they are side by side */
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
#define SCROLL_W (SMALL ? 10 : 60)
#define LEVEL_W (SMALL ? 190 : 340)   /* a level's bar, at the right of its row */
#define DOOR_X 60
#define DOOR_Y 40
#define DOOR_W 680
#define DOOR_H 410

static int list_w(void) { return SW - 2 * LIST_X - (nrows > VISIBLE ? SCROLL_W : 0); }

/* a codec's Up and Down buttons, left of its switch */
#define CODEC_BTN_W 64
#define CODEC_UP_X(x, w) ((x) + (w) - 150 - 2 * CODEC_BTN_W)

/* the Theme page: a picture of each, its name under it */
#define TH_W (SMALL ? 144 : 240)
#define TH_H (SMALL ? 150 : 222)
#define TH_GAP (SMALL ? 12 : 16)
#define TH_Y (SMALL ? HEAD_H + 8 : 92)
#define TH_X(i) ((SW - t48_theme_count * TH_W - (t48_theme_count - 1) * TH_GAP) / 2 + (i) * (TH_W + TH_GAP))
#define TH_ALL_H (TH_H + 84)   /* with the name and "Current" */

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
    if (ov.kind != O_NONE && SMALL) return;   /* its answers are the soft keys' labels */
    if (ov.kind != O_NONE) {
        if (ov.kind == O_MSG && ui_inside(x, y, OV_X + OV_W / 2 - 80, OV_Y + OV_H - 76, 160, 56)) overlay_answer(1);
        else if (ov.kind == O_CONFIRM && ui_inside(x, y, OV_X + OV_W / 2 - 170, OV_Y + OV_H - 76, 160, 56)) overlay_answer(0);
        else if (ov.kind == O_CONFIRM && ui_inside(x, y, OV_X + OV_W / 2 + 10, OV_Y + OV_H - 76, 160, 56)) overlay_answer(1);
        else if (ov.kind == O_DOOR && ui_inside(x, y, SW / 2 - 190, DOOR_Y + DOOR_H - 70, 180, 54)) overlay_answer(0);
        else if (ov.kind == O_DOOR && ui_inside(x, y, SW / 2 + 10, DOOR_Y + DOOR_H - 70, 180, 54)) overlay_answer(1);
        redraw_me();
        return;
    }
    if (y < HEAD_H) {
        if (x < 130) { pop(); if (depth < 0) return; }
        redraw_me();
        return;
    }
    if (stack[depth].page == P_THEME) {
        for (int i = 0; i < nrows; i++)
            if (ui_inside(x, y, TH_X(i), TH_Y, TH_W, TH_ALL_H)) {
                stack[depth].sel = i;
                activate(i, 1);
                rebuild();
                redraw_me();
            }
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
    if ((rows[i].flags & F_CODEC) && x >= CODEC_UP_X(LIST_X, w) && x < CODEC_UP_X(LIST_X, w) + 2 * CODEC_BTN_W + 8) {
        codec_change(i, x < CODEC_UP_X(LIST_X, w) + CODEC_BTN_W + 4 ? -1 : 1);
    } else if (rows[i].kind == R_LEVEL) {
        /* minus at the left of the bar, plus at the right */
        int mid = LIST_X + w - LEVEL_W / 2;
        if (x >= LIST_X + w - LEVEL_W) activate(i, x < mid ? -1 : 1);
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
    if (depth >= 0 && stack[depth].page == P_UPDATE && update_busy())
        return upd.asked + UPDATE_CHECK_MS > now ? (int)(upd.asked + UPDATE_CHECK_MS - now) : UPDATE_CHECK_MS;
    if (depth >= 0 && stack[depth].page == P_INSTALL && job.pid <= 0 && ov.kind == O_NONE)
        return fw.asked + FW_CHECK_MS > now ? (int)(fw.asked + FW_CHECK_MS - now) : 0;
    return -1;
}

void settings_tick(long long now)
{
    if (depth >= 0 && kbd_open && t48_kbd_tick(&kbd) != T48_KBD_NOTHING) redraw_me();
    /* work is going on: how far it has got, about once a second */
    if (depth >= 0 && stack[depth].page == P_UPDATE && update_busy() && upd.info_pid <= 0 && now >= upd.asked + UPDATE_CHECK_MS)
        update_info();
    /* a stick put in or taken out while the Install page waits */
    if (depth >= 0 && stack[depth].page == P_INSTALL && job.pid <= 0 && ov.kind == O_NONE && now >= fw.asked + FW_CHECK_MS &&
        fw_status()) { rebuild(); redraw_me(); }
    if (depth < 0 || kbd_open || stack[depth].page != P_ACCESS || now < acc_checked + ACCESS_CHECK_MS) return;
    static long shown_left = -1;
    int was_open = acc.door, changed = access_read();
    acc_checked = now;
    if (was_open && !acc.door) {
        /* password login has closed: because a key came, the time ran out, or it was closed by hand */
        int had_password = door_password[0] != 0;
        door_password[0] = 0;
        if (ov.kind == O_DOOR || had_password) {
            if (acc.nkeys > door_keys_before) message("Key added", "Password login closed");
            else message("Password login closed", "No key arrived");
        }
    }
    if (door_password[0] && !acc.door && !was_open && now > door_asked + 5000) {
        /* asked for, and five seconds later still not open */
        door_password[0] = 0;
        if (ov.kind == O_DOOR) message("Password login failed", "");
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

/* the same, each line centred on cx: the words of a question */
static void two_lines_centred(int cx, int y, int w, const t48_font *f, uint32_t c, const char *text)
{
    char line[220];
    size_t n = strlen(text), cut = n;
    snprintf(line, sizeof line, "%s", text);
    while (cut > 0 && (line[cut] = 0, t48_text_width(f, line) > w)) {
        do cut--; while (cut > 0 && text[cut] != ' ');
    }
    if (cut == 0 || cut == n) { t48_text_centre(&ui, cx, y + f->height / 2, f, c, cut ? line : text); return; }
    t48_text_centre(&ui, cx, y, f, c, line);
    t48_text_centre(&ui, cx, y + f->height + 2, f, c, text + cut + 1);
}

/* a stored choice as it is shown: the files keep "dhcp", the screen says DHCP */
static const char *choice_text(const char *v)
{
    static const char *const map[][2] = {{"dhcp", "DHCP"}, {"static", "Static"}, {"udp", "UDP"}, {"tcp", "TCP"}, {"tls", "TLS"},
                                             {"stable", "Stable"}, {"testing", "Testing"}, {"rfc2833", "RFC 2833"},
                                             {"inband", "In-band"}, {"info", "SIP INFO"},
                                             {"standard", "Standard"}, {"long", "Long"}, {"chime", "Chime"}, {"pulse", "Pulse"},
                                             {"custom", "Custom"}, {"off", "Off"}, {"on", "On"}, {"required", "Required"},
                                             {"300", "5 min"}, {"600", "10 min"}, {"1800", "30 min"}, {"3600", "1 h"}};
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(v, map[i][0])) return map[i][1];
    return v;
}

static void draw_row(const struct row *r, int x, int y, int w, int selected)
{
    int h = SMALL ? SROW_H : ROW_H - 6, dis = r->flags & F_DISABLED, pad = SMALL ? 10 : 18;
    const t48_font *f = &t48_font_normal;
    int ty = y + (h - f->height) / 2;
    struct row_ink ink;

    if (r->kind == R_NOTE && SMALL) {
        /* one line where it fits; two in the smallest face where it does not */
        uint32_t c = (r->flags & F_DANGER) ? C_BAD : C_DIM;
        if (selected) t48_fill(&ui, x, y + 4, 3, h - 8, C_TEXT);
        if (t48_text_width(&t48_font_small, r->value) <= w - 2 * pad) t48_text(&ui, x + pad, y + (h - t48_font_small.height) / 2, &t48_font_small, c, r->value);
        else ui_two_lines(x + pad, y, w - 2 * pad, &t48_font_tiny, c, r->value);
        return;
    }
    if (r->kind == R_NOTE) {
        /* words between rows: they are not chosen, so they are never dressed as a row */
        if (selected) t48_fill(&ui, x, y + 8, 4, h - 16, C_TEXT);
        two_lines(x + 18, y + 6, w - 36, &t48_font_small, (r->flags & F_DANGER) ? C_BAD : C_DIM, r->value);
        return;
    }
    ui_row(x, y, w, h, selected, &ink);
    uint32_t text = dis ? ink.faint : ink.text;

    if (r->kind == R_ACTION) {
        t48_text(&ui, x + pad, ty, &t48_font_bold, dis ? ink.faint : (r->flags & F_DANGER) && !selected ? C_BAD : ink.text, r->label);
        if (r->value[0]) t48_text_right(&ui, x + w - pad, ty, f, ink.dim, r->value);
        return;
    }
    int right = x + w - pad;
    if (r->kind == R_MENU) { ui_chevron(right - 6, y + h / 2, 7, 1, 3, ink.dim); right -= 26; }
    if (r->kind == R_LEVEL) {
        int bx = x + w - LEVEL_W, bw = LEVEL_W - pad;
        ui_level(bx, y, bw, h, r->level, r->level_max, dis, selected);
        t48_text(&ui, x + pad, ty, f, text, r->label);
        if (!dis) {
            char n[8];
            snprintf(n, sizeof n, "%d", r->level);
            t48_text_right(&ui, bx - 14, ty, f, ink.dim, n);
        }
        return;
    }
    if (r->kind == R_TOGGLE) {
        ui_switch(right, y, h, !strcmp(r->value, "on"), selected);
        t48_text_fit(&ui, x + pad, ty, w - 140, f, text, r->label);
        if ((r->flags & F_CODEC) && !SMALL) {
            /* touch: up and down the order, left of the switch (soft keys on a small screen) */
            int i = (int)(r - rows);
            ui_btn(CODEC_UP_X(x, w), y + 7, CODEC_BTN_W, h - 14, "^", i > 0 ? B_PLAIN : B_OFF);
            ui_btn(CODEC_UP_X(x, w) + CODEC_BTN_W + 8, y + 7, CODEC_BTN_W, h - 14, "v", i < nrows - 1 ? B_PLAIN : B_OFF);
        }
        return;
    }
    /* label on the left, value on the right; the value gives way first */
    int lw = t48_text_width(f, r->label), room = right - (x + pad);
    if (lw > room * 2 / 3 && r->value[0]) lw = room * 2 / 3;
    t48_text_fit(&ui, x + pad, ty, lw + 2, f, r->kind == R_INFO && !selected ? ink.dim : text, r->label);
    if (r->value[0]) {
        char secs[220];
        const char *shown = r->kind == R_CHOICE ? choice_text(r->value) : r->value;
        if (r->flags & F_SECONDS) { snprintf(secs, sizeof secs, "%s s", r->value); shown = secs; }
        int vw = t48_text_width(f, shown), max = room - lw - 24;
        uint32_t vc = dis ? ink.faint : r->kind == R_INFO ? ink.text : ink.dim;
        if (vw <= max) t48_text_right(&ui, right, ty, f, vc, shown);
        else if (max > 40) t48_text_fit(&ui, right - max, ty, max, f, vc, shown);
    }
}

static void draw_overlay(void)
{
    struct row_ink ink;
    int danger = ov.kind == O_CONFIRM && (!ov.yes || ov.act == A_FORGET_KEYS || ov.act == A_UPD_KERNEL || ov.act == A_FW_INSTALL || ov.act == A_FW_RESTORE);
    if (SMALL) {
        /* the same box, smaller; its answers are the soft keys */
        int busy = ov.kind == O_BUSY && !ov.plain, y = HEAD_H + (BAR_Y - HEAD_H - UI_ASK_H) / 2;
        ui_ask(ov.title, busy ? " " : ov.text, busy ? " " : ov.kind == O_BUSY ? NULL : ov.extra, danger ? B_STOP : B_PLAIN);
        if (busy) {
            int bx = 60, bw = SW - 120, by = y + 100;
            ui_dialog_ink(&ink);
            t48_text_centre(&ui, SW / 2, y + 62, &t48_font_normal, ink.dim, ov.text);
            t48_round_fill(&ui, bx, by, bw, 12, T->radius < 6 ? 0 : 6, t48_mix(ink.text, ui_ground(), 215));
            if (ov.percent >= 0) {
                char pc[8];
                t48_round_fill(&ui, bx, by, bw * ov.percent / 100 > 12 ? bw * ov.percent / 100 : 12, 12, T->radius < 6 ? 0 : 6, ink.text);
                snprintf(pc, sizeof pc, "%d%%", ov.percent);
                t48_text_centre(&ui, SW / 2, by + 18, &t48_font_small, ink.dim, pc);
            }
        }
        if (ov.kind == O_MSG) ui_bar(NULL, 0, NULL, 0, NULL, 0, "OK", B_FIRST);
        else if (ov.kind == O_CONFIRM) ui_bar(NULL, 0, NULL, 0, ov.no ? ov.no : "Cancel", B_PLAIN, ov.yes ? ov.yes : "Remove", danger ? B_STOP : B_FIRST);
        else ui_bar(NULL, 0, NULL, 0, NULL, 0, NULL, 0);
        return;
    }
    ui_dialog(OV_X, OV_Y, OV_W, OV_H, danger ? B_STOP : B_PLAIN);
    ui_dialog_ink(&ink);
    t48_text_centre(&ui, SW / 2, OV_Y + 30, &t48_font_head, ink.text, ov.title);
    if (ov.kind == O_BUSY && ov.plain) {
        t48_text_centre(&ui, SW / 2, OV_Y + 118, &t48_font_normal, ink.text, ov.text);
        return;
    }
    if (ov.kind == O_BUSY) {
        t48_text_centre(&ui, SW / 2, OV_Y + 88, &t48_font_normal, ink.dim, ov.text);
        int bx = OV_X + 60, bw = OV_W - 120, by = OV_Y + 150;
        t48_round_fill(&ui, bx, by, bw, 12, T->radius < 6 ? 0 : 6, t48_mix(ink.text, ui_ground(), 215));
        if (ov.percent >= 0) {
            char pc[8];
            t48_round_fill(&ui, bx, by, bw * ov.percent / 100 > 12 ? bw * ov.percent / 100 : 12, 12, T->radius < 6 ? 0 : 6, ink.text);
            snprintf(pc, sizeof pc, "%d%%", ov.percent);
            t48_text_centre(&ui, SW / 2, by + 28, &t48_font_small, ink.dim, pc);
        }
        return;
    }
    two_lines_centred(SW / 2, OV_Y + 76, OV_W - 80, &t48_font_normal, ink.dim, ov.text);
    if (ov.extra[0]) t48_text_centre(&ui, SW / 2, OV_Y + 138, &t48_font_small, ink.dim, ov.extra);
    if (ov.kind == O_MSG) {
        ui_dialog_btn(OV_X + OV_W / 2 - 80, OV_Y + OV_H - 76, 160, 56, "OK", B_FIRST);
    } else {
        ui_dialog_btn(OV_X + OV_W / 2 - 170, OV_Y + OV_H - 76, 160, 56, ov.no ? ov.no : "Cancel", B_PLAIN);
        ui_dialog_btn(OV_X + OV_W / 2 + 10, OV_Y + OV_H - 76, 160, 56, ov.yes ? ov.yes : "Remove", danger ? B_STOP : B_FIRST);
    }
}

/* password login is open: what to type on the computer, and for how much longer */
static void draw_door(void)
{
    char b[200], net[80];
    int up;
    struct row_ink ink;
    if (SMALL) {
        /* the same words in the room there is: the command, the host key, the password, the time left */
        int x = 8, y = HEAD_H + 4, w = SW - 16, h = BAR_Y - HEAD_H - 10;
        ui_dialog(x, y, w, h, B_LIT);
        ui_dialog_ink(&ink);
        ui_network_status(net, sizeof net, &up);
        snprintf(b, sizeof b, "ssh-copy-id root@%s", up ? net : "ADDRESS");
        t48_text_centre(&ui, SW / 2, y + 14, &t48_font_bold, ink.text, b);
        t48_text_centre(&ui, SW / 2, y + 44, &t48_font_small, ink.dim, "Host key must match");
        const char *hk = acc.host_key[0] ? acc.host_key : "unknown";
        const t48_font *kf = t48_text_width(&t48_font_small, hk) > w - 16 ? &t48_font_tiny : &t48_font_small;
        if (t48_text_width(kf, hk) > w - 16) t48_text_fit(&ui, x + 8, y + 64, w - 16, kf, ink.text, hk);
        else t48_text_centre(&ui, SW / 2, y + 64, kf, ink.text, hk);
        t48_text_centre(&ui, SW / 2, y + 90, &t48_font_small, ink.dim, "Password");
        if (door_password[0]) t48_text_centre(&ui, SW / 2, y + 106, &t48_font_big, ink.text, door_password);
        else t48_text_centre(&ui, SW / 2, y + 120, &t48_font_small, ink.dim, "Hidden. Close and reopen for a new one");
        if (acc.door) {
            char left[16];
            door_left(left, sizeof left);
            snprintf(b, sizeof b, "Closes in %s, or when a key arrives", left);
        } else {
            snprintf(b, sizeof b, "Opening");
        }
        t48_text_centre(&ui, SW / 2, y + h - 28, &t48_font_small, ink.dim, b);
        ui_bar(NULL, 0, NULL, 0, "Hide", B_PLAIN, "Close", B_STOP);
        return;
    }
    ui_dialog(DOOR_X, DOOR_Y, DOOR_W, DOOR_H, B_LIT);
    ui_dialog_ink(&ink);
    t48_text_centre(&ui, SW / 2, DOOR_Y + 22, &t48_font_head, ink.text, "Add key");
    t48_text_centre(&ui, SW / 2, DOOR_Y + 66, &t48_font_small, ink.dim, "Run");
    ui_network_status(net, sizeof net, &up);
    snprintf(b, sizeof b, "ssh-copy-id root@%s", up ? net : "ADDRESS");
    t48_text_centre(&ui, SW / 2, DOOR_Y + 88, &t48_font_head, ink.text, b);
    t48_text_centre(&ui, SW / 2, DOOR_Y + 140, &t48_font_small, ink.dim, "Host key must match");
    t48_text_centre(&ui, SW / 2, DOOR_Y + 162, &t48_font_small, ink.text, acc.host_key[0] ? acc.host_key : "unknown");
    t48_text_centre(&ui, SW / 2, DOOR_Y + 196, &t48_font_small, ink.dim, "Password");
    if (door_password[0]) t48_text_centre(&ui, SW / 2, DOOR_Y + 218, &t48_font_big, ink.text, door_password);
    else t48_text_centre(&ui, SW / 2, DOOR_Y + 230, &t48_font_normal, ink.dim, "Hidden. Close and reopen for a new one");
    if (acc.door) {
        char left[16];
        door_left(left, sizeof left);
        snprintf(b, sizeof b, "Closes in %s, or when a key arrives", left);
    } else {
        snprintf(b, sizeof b, "Opening");
    }
    t48_text_centre(&ui, SW / 2, DOOR_Y + DOOR_H - 104, &t48_font_small, ink.dim, b);
    ui_dialog_btn(SW / 2 - 190, DOOR_Y + DOOR_H - 70, 180, 54, "Hide", B_PLAIN);
    ui_dialog_btn(SW / 2 + 10, DOOR_Y + DOOR_H - 70, 180, 54, "Close", B_STOP);
}

static void draw_themes(void)
{
    for (int i = 0; i < nrows && i < t48_theme_count; i++) {
        int x = TH_X(i), current = rows[i].value[0] != 0, selected = i == stack[depth].sel;
        if (selected) t48_round_fill(&ui, x - 6, TH_Y - 6, TH_W + 12, TH_H + 12, T->radius + 2, T->sel);
        ui_theme_preview(&t48_themes[i], x, TH_Y, TH_W, TH_H, current);
        t48_text_centre(&ui, x + TH_W / 2, TH_Y + TH_H + (SMALL ? 8 : 20), &t48_font_bold, C_TEXT, rows[i].label);
        if (current) t48_text_centre(&ui, x + TH_W / 2, TH_Y + TH_H + (SMALL ? 32 : 50), &t48_font_small, C_DIM, "Current");
    }
}

/* Small screens: what the soft keys do to the row chosen */
static void bar_for_row(void)
{
    const struct row *r = stack[depth].sel < nrows ? &rows[stack[depth].sel] : NULL;
    const char *first = NULL;
    if (!SMALL) return;
    if (r && !(r->flags & F_DISABLED)) {
        switch (r->kind) {
        case R_MENU: first = "Open"; break;
        case R_TEXT: first = "Edit"; break;
        case R_CHOICE: first = "Change"; break;
        case R_TOGGLE: first = !strcmp(r->value, "on") ? "Turn off" : "Turn on"; break;
        case R_ACTION: first = "Select"; break;
        case R_LEVEL: ui_bar("-", B_PLAIN, "+", B_PLAIN, NULL, 0, "Back", B_PLAIN); return;
        default: break;
        }
    }
    if (stack[depth].page == P_THEME) first = "Select";
    if (r && (r->flags & F_CODEC)) {
        int i = stack[depth].sel;
        ui_bar(first, B_FIRST, "Up", i > 0 ? B_PLAIN : B_OFF, "Down", i < nrows - 1 ? B_PLAIN : B_OFF, "Back", B_PLAIN);
        return;
    }
    ui_bar(first, B_FIRST, NULL, 0, NULL, 0, "Back", B_PLAIN);
}

void settings_soft(int b)
{
    if (depth < 0) return;
    if (kbd_open) { settings_key(T48_KEY_SOFT1 + b, 1); return; }
    if (ov.kind != O_NONE) {
        if (ov.kind == O_MSG && b == 3) overlay_answer(1);
        else if ((ov.kind == O_CONFIRM || ov.kind == O_DOOR) && b >= 2) overlay_answer(b == 3);
        else return;
        redraw_me();
        return;
    }
    if (stack[depth].sel < nrows && (rows[stack[depth].sel].flags & F_CODEC) && (b == 1 || b == 2)) {
        codec_change(stack[depth].sel, b == 1 ? -1 : 1);
        redraw_me();
    } else if (stack[depth].sel < nrows && rows[stack[depth].sel].kind == R_LEVEL && b < 2) settings_key(b ? T48_KEY_RIGHT : T48_KEY_LEFT, 1);
    else if (b == 0) settings_key(T48_KEY_OK, 1);
    else if (b == 3) settings_key(T48_KEY_BACK, 1);
}

void settings_draw(void)
{
    if (depth < 0) return;
    if (kbd_open) { t48_kbd_draw(&kbd, &ui); return; }
    rebuild();   /* what the rows show may have changed underneath: the clock, the network, a download */

    ui_clear(C_BG);
    ui_header(title, 1, 0);
    bar_for_row();
    if (stack[depth].page == P_THEME) { draw_themes(); return; }
    int w = list_w(), top = stack[depth].top;
    for (int i = top; i < nrows && i < top + VISIBLE; i++)
        draw_row(&rows[i], LIST_X, LIST_Y + (i - top) * ROW_H, w, i == stack[depth].sel);
    if (nrows > VISIBLE && SMALL) {
        /* where the rows showing are in the whole list */
        int x = SW - LIST_X - 4, hh = VISIBLE * ROW_H - 3, th = hh * VISIBLE / nrows;
        t48_fill(&ui, x, LIST_Y, 4, hh, C_LINE);
        t48_fill(&ui, x, LIST_Y + (hh - th) * top / (nrows - VISIBLE), 4, th, C_DIM);
    } else if (nrows > VISIBLE) {
        int x = SW - LIST_X - SCROLL_W + 8, hh = VISIBLE * ROW_H / 2 - 6;
        ui_btn(x, LIST_Y, SCROLL_W - 8, hh, "^", top > 0 ? B_PLAIN : B_OFF);
        ui_btn(x, LIST_Y + hh + 6, SCROLL_W - 8, hh, "v", top + VISIBLE < nrows ? B_PLAIN : B_OFF);
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
    if (!strcmp(key, "overlay.text")) {
        snprintf(b, sizeof b, "%s", ov.kind != O_NONE && ov.text[0] ? ov.text : "-");
        for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
        return b;
    }
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
    if (!strcmp(key, "update.state")) return !upd.loaded ? "loading" : !upd.installed ? "notinstalled" : upd.state;
    if (!strcmp(key, "update.busy")) return update_busy() ? "yes" : "no";
    if (!strcmp(key, "tz")) return getenv("TZ") ? getenv("TZ") : "-";
    return NULL;
}
