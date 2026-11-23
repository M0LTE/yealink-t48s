// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: the one running app. Starts it with a socket and a shared screen
 * buffer, speaks the shell side of the app protocol (docs/app-protocol.md),
 * pauses, freezes, resumes and stops it, and notices when it dies. */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "shell.h"

#define START_MS 5000   /* an app that has not answered START by then is killed */
#define GRACE_MS 1000   /* between PAUSE and freezing, and between QUIT and killing */
#define BUF_BYTES (SW * SH * 4)

static struct {
    enum app_state state;
    pid_t pid;
    int sock;
    uint32_t *buf;
    char id[64], name[64];
    char dir[256];            /* its folder, and which folder that was when it started */
    dev_t dir_dev;
    ino_t dir_ino;
    long long deadline;       /* for STARTING, PAUSING and QUITTING */
    const char *kill_reason;  /* set when the shell kills it for misbehaving */
    unsigned char rx[1024];
    size_t rx_n;
} app = {.sock = -1};

static char errbuf[200];

const char *app_error(void) { return errbuf; }
enum app_state app_state(void) { return app.state; }
const char *app_id(void) { return app.state == APP_NONE ? "" : app.id; }
int app_fd(void) { return app.state == APP_NONE ? -1 : app.sock; }

/* Has the running app been removed or replaced (by the web page, or t48-app
 * from SSH)? Both rename its folder away, so the folder of that name is then
 * missing or another one. */
int app_folder_changed(void)
{
    struct stat st;
    if (app.state == APP_NONE) return 0;
    return stat(app.dir, &st) < 0 || st.st_dev != app.dir_dev || st.st_ino != app.dir_ino;
}
const uint32_t *app_pixels(void) { return app.buf; }

/* An unprivileged user for each app, the same one every time: there is no
 * account database to keep, the number comes from the id. */
static uid_t uid_for(const char *id)
{
    unsigned h = 2166136261u;
    for (; *id; id++) h = (h ^ (unsigned char)*id) * 16777619u;
    return 20000 + h % 10000;
}

/* A file nobody else can open, to share with the app: the screen buffer, the
 * fonts. Kernel 3.6 has no memfd, so: a temporary file on a RAM disk, unlinked
 * at once. */
static int make_file(const char *what, size_t size)
{
    static const char *dirs[] = {NULL, "/dev/shm", "/run", "/tmp"};
    dirs[0] = getenv("T48_RUNDIR");
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char path[200];
        if (!dirs[i]) continue;
        snprintf(path, sizeof path, "%s/t48-%s-XXXXXX", dirs[i], what);
        int fd = mkstemp(path);
        if (fd < 0) continue;
        unlink(path);
        if (ftruncate(fd, (off_t)size) == 0) return fd;
        close(fd);
    }
    return -1;
}

/* the font file, for the app to map (docs/app-protocol.md): -1 if it cannot be made */
static int make_fonts(void)
{
    size_t n;
    unsigned char *b = ui_fonts_file(&n);
    int fd = b ? make_file("fonts", n) : -1;
    if (fd >= 0 && pwrite(fd, b, n, 0) != (ssize_t)n) { close(fd); fd = -1; }
    free(b);
    return fd;
}

static void kill_app(int sig)
{
    if (app.pid > 0) {
        kill(-app.pid, sig);
        if (sig == SIGKILL) kill(-app.pid, SIGCONT);
    }
}

static void misbehaved(const char *why)
{
    if (!app.kill_reason) app.kill_reason = why;
    logmsg("app %s %s: killing it", app.id, why);
    kill_app(SIGKILL);
}

static void send_msg(int type, const void *payload, size_t len)
{
    unsigned char b[300];
    struct t48_msg_hdr h = {(uint16_t)type, (uint16_t)len};
    if (app.state == APP_NONE || app.sock < 0 || len > sizeof b - sizeof h) return;
    memcpy(b, &h, sizeof h);
    if (len) memcpy(b + sizeof h, payload, len);
    ssize_t k = send(app.sock, b, sizeof h + len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (k == (ssize_t)(sizeof h + len)) return;
    /* The socket's queue is full: the app has not read its events for a long
     * time. (A partial write cannot be told apart and is as bad.) */
    if (k >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) misbehaved("Not responding");
    /* any other error: the app has gone, and SIGCHLD will tell us */
}

int app_launch(const struct manifest *m)
{
    int sv[2], buf_fd, fonts_fd;
    char path[400], data[400], conf[400];

    if (app.state != APP_NONE) { snprintf(errbuf, sizeof errbuf, "Another app is running"); return -1; }
    snprintf(path, sizeof path, "%s/%s", m->dir, m->exec);
    if (access(path, X_OK) < 0) {
        snprintf(errbuf, sizeof errbuf, "Program missing: %s", m->exec);
        return -1;
    }
    buf_fd = make_file("screen", BUF_BYTES);
    if (buf_fd < 0) { snprintf(errbuf, sizeof errbuf, "No memory for the screen"); return -1; }
    fonts_fd = make_fonts();   /* an app that gets none still has B612 */
    uint32_t *buf = mmap(NULL, BUF_BYTES, PROT_READ, MAP_SHARED, buf_fd, 0);
    if (buf == MAP_FAILED || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        snprintf(errbuf, sizeof errbuf, "Out of memory");
        if (buf != MAP_FAILED) munmap(buf, BUF_BYTES);
        close(buf_fd);
        if (fonts_fd >= 0) close(fonts_fd);
        return -1;
    }

    /* the app's own folder */
    uid_t uid = uid_for(m->id);
    mkdir(opt.data, 0755);
    snprintf(data, sizeof data, "%s/%s", opt.data, m->id);
    mkdir(data, 0700);
    if (geteuid() == 0 && chown(data, uid, uid) < 0) logmsg("chown %s: %s", data, strerror(errno));

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(errbuf, sizeof errbuf, "Cannot start: %s", strerror(errno));
        munmap(buf, BUF_BYTES);
        close(buf_fd); close(sv[0]); close(sv[1]);
        if (fonts_fd >= 0) close(fonts_fd);
        return -1;
    }
    if (pid == 0) {
        char num[16];
        setpgid(0, 0);
        /* socket on 3, buffer on 4, fonts on 5, nothing else of ours */
        int s = fcntl(sv[1], F_DUPFD, 10), b = fcntl(buf_fd, F_DUPFD, 10), f = fonts_fd < 0 ? -1 : fcntl(fonts_fd, F_DUPFD, 10);
        dup2(s, 3);
        dup2(b, 4);
        if (f >= 0) dup2(f, 5); else close(5);
        for (int fd = 6; fd < 256; fd++) close(fd);
        if (geteuid() == 0 && (setgroups(0, NULL) < 0 || setgid(uid) < 0 || setuid(uid) < 0)) _exit(126);
        prctl(PR_SET_PDEATHSIG, SIGKILL);  /* an app is no use without its shell (set after the change of user, which clears it) */
        if (chdir(m->dir) < 0) _exit(126);
        snprintf(num, sizeof num, "%d", 3);
        setenv(T48_ENV_SOCKET, num, 1);
        snprintf(num, sizeof num, "%d", 4);
        setenv(T48_ENV_BUFFER, num, 1);
        if (f >= 0) setenv(T48_ENV_FONTS, "5", 1);
        setenv(T48_ENV_APP_ID, m->id, 1);
        setenv(T48_ENV_APP_DATA, data, 1);
        setenv("HOME", data, 1);
        signal(SIGPIPE, SIG_DFL);
        if (opt.runner) execlp(opt.runner, opt.runner, path, (char *)NULL);
        execl(path, m->exec, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    close(sv[1]);
    close(buf_fd);
    if (fonts_fd >= 0) close(fonts_fd);   /* the app's copy keeps the file for as long as the app lives */

    memset(&app, 0, sizeof app);
    app.state = APP_STARTING;
    app.pid = pid;
    app.sock = sv[0];
    app.buf = buf;
    snprintf(app.id, sizeof app.id, "%s", m->id);
    snprintf(app.name, sizeof app.name, "%s", m->name);
    struct stat st;
    snprintf(app.dir, sizeof app.dir, "%s", m->dir);
    if (stat(app.dir, &st) == 0) { app.dir_dev = st.st_dev; app.dir_ino = st.st_ino; }
    else { app.dir_dev = 0; app.dir_ino = 0; }
    app.deadline = t48_now_ms() + START_MS;
    logmsg("app %s started, pid %d%s", m->id, (int)pid, geteuid() == 0 ? " as its own user" : "");

    struct t48_msg_hello h = {T48_API_LEVEL, SW, SH, T48_FORMAT_XRGB8888, SW * 4, BUF_BYTES};
    send_msg(T48_MSG_HELLO, &h, sizeof h);

    /* settings: the manifest's defaults, unless the owner has set them (config.c) */
    snprintf(conf, sizeof conf, "apps/%s", m->id);
    for (int i = 0; i < m->nsettings; i++) {
        char key[64], val[200], kv[280];
        size_t kl = strcspn(m->settings[i], "=");
        snprintf(key, sizeof key, "%.*s", (int)kl, m->settings[i]);
        if (cfg_get(conf, key, val, sizeof val)) snprintf(kv, sizeof kv, "%s=%s", key, val);
        else snprintf(kv, sizeof kv, "%s", m->settings[i]);
        send_msg(T48_MSG_SETTING, kv, strlen(kv));
    }
    app_theme();
    send_msg(T48_MSG_START, NULL, 0);
    return 0;
}

/* The look: before START, and again whenever the owner changes it. An app that
 * is frozen finds it waiting, ahead of RESUME. A level 1 app skips it. */
void app_theme(void)
{
    send_msg(T48_MSG_THEME, T, sizeof *T);
}

void app_pause(void)
{
    if (app.state != APP_RUNNING) return;
    send_msg(T48_MSG_PAUSE, NULL, 0);
    app.state = APP_PAUSING;
    app.deadline = t48_now_ms() + GRACE_MS;
}

static void freeze(void)
{
    kill_app(SIGSTOP);
    app.state = APP_STOPPED;
    logmsg("app %s frozen", app.id);
}

void app_resume(void)
{
    if (app.state == APP_STOPPED) kill_app(SIGCONT);
    else if (app.state != APP_PAUSING) return;
    app.state = APP_RUNNING;
    send_msg(T48_MSG_RESUME, NULL, 0);
    logmsg("app %s resumed", app.id);
}

void app_quit(void)
{
    if (app.state == APP_NONE || app.state == APP_QUITTING) return;
    if (app.state == APP_STOPPED) kill_app(SIGCONT);
    send_msg(T48_MSG_QUIT, NULL, 0);
    app.state = APP_QUITTING;
    app.deadline = t48_now_ms() + GRACE_MS;
}

void app_kill_now(void)
{
    if (app.state == APP_NONE) return;
    app.state = APP_QUITTING;
    kill_app(SIGKILL);
}

void app_key(int code, int down)
{
    struct t48_msg_key k = {(uint16_t)code, (uint8_t)down, 0};
    if (app.state == APP_RUNNING) send_msg(T48_MSG_KEY, &k, sizeof k);
}

void app_touch(int state, int x, int y)
{
    struct t48_msg_touch t = {(uint8_t)state, 0, (int16_t)x, (int16_t)y};
    if (app.state == APP_RUNNING) send_msg(T48_MSG_TOUCH, &t, sizeof t);
}

void app_readable(void)
{
    if (app.state == APP_NONE) return;
    ssize_t k = recv(app.sock, app.rx + app.rx_n, sizeof app.rx - app.rx_n, MSG_DONTWAIT);
    if (k <= 0) {
        /* closed: the exit will be reported by SIGCHLD; stop polling the socket */
        if (k == 0 || (errno != EAGAIN && errno != EINTR)) { close(app.sock); app.sock = -1; }
        return;
    }
    app.rx_n += (size_t)k;
    for (;;) {
        struct t48_msg_hdr h;
        if (app.rx_n < sizeof h) break;
        memcpy(&h, app.rx, sizeof h);
        if (h.len > sizeof app.rx - sizeof h) { misbehaved("Protocol error"); return; }
        if (app.rx_n < sizeof h + h.len) break;
        const unsigned char *p = app.rx + sizeof h;
        if (h.type == T48_MSG_READY && h.len >= sizeof(struct t48_msg_ready)) {
            struct t48_msg_ready r;
            memcpy(&r, p, sizeof r);
            if (app.state == APP_STARTING) {
                app.state = APP_RUNNING;
                logmsg("app %s ready (API level %d)", app.id, r.api);
            }
        } else if (h.type == T48_MSG_DAMAGE && h.len >= sizeof(struct t48_msg_rect)) {
            struct t48_msg_rect r;
            memcpy(&r, p, sizeof r);
            if (app.state == APP_RUNNING) shell_app_damage(r.x, r.y, r.w, r.h);
        } else if (h.type == T48_MSG_PAUSED) {
            if (app.state == APP_PAUSING) freeze();
        }
        /* anything else: from a newer library, skip it */
        app.rx_n -= sizeof h + h.len;
        memmove(app.rx, app.rx + sizeof h + h.len, app.rx_n);
    }
}

int app_timeout(long long now)
{
    if (app.state != APP_STARTING && app.state != APP_PAUSING && app.state != APP_QUITTING) return -1;
    return app.deadline > now ? (int)(app.deadline - now) : 0;
}

void app_tick(long long now)
{
    if (app_timeout(now) != 0) return;
    if (app.state == APP_STARTING) {
        misbehaved("Did not start");
        app.deadline = now + GRACE_MS;
    } else if (app.state == APP_PAUSING) {
        freeze();
    } else if (app.state == APP_QUITTING) {
        logmsg("app %s did not quit when asked: killing it", app.id);
        kill_app(SIGKILL);
        app.deadline = now + GRACE_MS;
    }
}

void app_reaped(pid_t pid, int status)
{
    static char why[160];
    char name[64];
    const char *reason = NULL;

    if (app.state == APP_NONE || pid != app.pid) return;
    kill(-app.pid, SIGKILL);  /* anything it started goes with it */
    if (app.kill_reason) {
        reason = app.kill_reason;
    } else if (app.state == APP_QUITTING) {
        reason = NULL;
    } else if (WIFSIGNALED(status)) {
        snprintf(why, sizeof why, "Crashed: %s", strsignal(WTERMSIG(status)));
        reason = why;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        reason = "Could not start";
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        snprintf(why, sizeof why, "Exit status %d", WEXITSTATUS(status));
        reason = why;
    }
    logmsg("app %s gone: %s", app.id, reason ? reason : "closed");
    snprintf(name, sizeof name, "%s", app.name);
    if (app.sock >= 0) close(app.sock);
    munmap(app.buf, BUF_BYTES);
    memset(&app, 0, sizeof app);
    app.sock = -1;
    shell_app_gone(name, reason);
}
