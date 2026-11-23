// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-web: the phone's web setup page (issue #37, docs/web.md). This file is
 * the server: listening, reading a request, pairing a browser, sessions, and
 * sending the answer. The pages themselves are in pages.c.
 *
 * How it is kept safe enough for a phone on a home network:
 *  - it only runs while "web=on" is in /data/config/access, which the owner
 *    sets at the phone's own screen (Settings, Access). It looks every second
 *    and leaves when that is no longer so.
 *  - it listens on the LAN interface's own address only, and turns away
 *    callers whose address is not a private one.
 *  - a browser gets nothing but the pairing page until it has typed a six
 *    digit code that the phone shows on its screen. A code lasts two minutes
 *    and five tries; after fifteen wrong tries pairing closes for ten minutes.
 *    The browser that asked for the code is the only one that can use it.
 *  - a paired browser holds a random cookie (HttpOnly, SameSite=Strict). It
 *    is kept in memory only: a restart, or off and on at the screen, forgets
 *    every browser.
 *  - every change is a POST that must carry the session's own token, and an
 *    Origin, when the browser sends one, that is this page. No page can be
 *    put in a frame, and nothing is loaded from anywhere else.
 *  - values are never logged, only what was changed.
 *  - it is plain HTTP, and the page says so.
 *
 * One request at a time, each with a deadline: there is one owner and one
 * browser, and a phone with 124 MB has better uses for its memory.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "web.h"
#include "zones.h"

#ifndef T48_VERSION
#define T48_VERSION "development build"
#endif

#define MAX_SESSIONS 8
#define SESSION_IDLE_MS (12LL * 3600 * 1000)
#define PAIR_SECONDS 120
#define PAIR_TRIES 5
#define LOCK_AFTER 15
#define LOCK_SECONDS 600
#define START_GAP_MS 3000
#define REQUEST_MS 20000

struct options opt = {
    .apps = "/data/apps:/usr/share/t48/apps",
    .data = "/data/appdata",
    .config = "/data/config",
    .state = "/data/phone",
    .apply = "/usr/bin/t48-apply",
    .access = "/run/t48-access",
    .web = "/run/t48-web",
};
struct web_options wopt = {.iface = "eth0", .port = 80, .appcmd = "/usr/bin/t48-app", .keep = "/data/access"};

static volatile sig_atomic_t stopping;
static int stop_soon;
static struct session sessions[MAX_SESSIONS];
static char url[80];              /* how a browser reaches us; "" while there is no address */

/* ---------- small things ---------- */

int log_quiet;

void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (log_quiet) return;
    fputs("t48-web: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* config.c's t48-config wants it; the same answer the shell gives */
const char *settings_tz(void)
{
    const char *name = cfg_str("system", "timezone", "UTC");
    for (int i = 0; i < NZONES; i++)
        if (!strcmp(zones[i].name, name)) return zones[i].tz;
    return name[0] ? name : "UTC0";
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void random_bytes(unsigned char *p, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    while (fd >= 0 && got < n) {
        ssize_t k = read(fd, p + got, n - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    if (fd >= 0) close(fd);
    if (got != n) { logmsg("no random numbers: giving up"); exit(1); }
}

static void random_hex(char *out, int bytes)
{
    unsigned char b[32];
    random_bytes(b, (size_t)bytes);
    for (int i = 0; i < bytes; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

/* the same time whatever differs, so a guess learns nothing from how long it took */
static int same(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = (unsigned char)(la != lb);
    for (size_t i = 0; i < la; i++) d |= (unsigned char)(a[i] ^ b[lb ? i % lb : 0]);
    return d == 0 && la > 0;
}

char *load_file(const char *path, size_t max)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *t = malloc(max + 1);
    size_t n = t ? fread(t, 1, max, f) : 0;
    fclose(f);
    if (t) t[n] = 0;
    return t;
}

/* a new file beside the old one, synced, renamed over it: as the settings store does */
int save_file(const char *path, const char *text, int mode)
{
    char tmp[420];
    size_t len = strlen(text);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    if (fchmod(fd, (mode_t)mode) < 0 || write(fd, text, len) != (ssize_t)len || fsync(fd) < 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    if (rename(tmp, path) < 0) { unlink(tmp); return -1; }
    return 0;
}

/* ---------- running other programs: t48-apply, t48-app ---------- */

int run_program(char *const argv[], int wait_ms, char *out, size_t outn, int delay)
{
    int p[2] = {-1, -1}, status = -1;
    size_t got = 0;
    if (out && outn) out[0] = 0;
    if (wait_ms && pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { if (wait_ms) { close(p[0]); close(p[1]); } return -1; }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (!wait_ms) { setsid(); if (delay) sleep((unsigned)delay); }
        dup2(nul, 0);
        if (wait_ms) { dup2(p[1], 1); dup2(p[1], 2); }
        else dup2(2, 1);
        for (int fd = 3; fd < 256; fd++) close(fd);
        signal(SIGPIPE, SIG_DFL);
        execv(argv[0], argv);
        _exit(127);
    }
    if (!wait_ms) return 0;   /* reaped by the main loop when it ends */
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    long long end = now_ms() + wait_ms;
    int gone = 0;
    for (;;) {
        struct pollfd pf = {p[0], POLLIN, 0};
        char junk[512];
        poll(&pf, 1, 100);
        ssize_t k;
        /* what it says; anything past the room we have is read and dropped */
        while ((k = read(p[0], got + 1 < outn ? out + got : junk, got + 1 < outn ? outn - 1 - got : sizeof junk)) > 0)
            if (got + 1 < outn) { got += (size_t)k; out[got] = 0; }
        /* a program it started may hold the pipe open for ever: its own end is what counts */
        if (gone || k == 0) break;
        if (waitpid(pid, &status, WNOHANG) == pid) { gone = 1; continue; }
        if (now_ms() > end) { kill(pid, SIGKILL); break; }
    }
    close(p[0]);
    if (!gone) waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* make the running phone follow a settings file, as the shell does on leaving a page */
void run_apply(const char *what, int wait_ms, int delay)
{
    char *argv[] = {(char *)opt.apply, (char *)what, NULL};
    char out[200];
    if (!opt.apply || !opt.apply[0]) return;
    logmsg("apply %s", what);
    run_program(argv, wait_ms, out, sizeof out, delay);
}

/* ---------- the page being made ---------- */

static struct { char *p; size_t n, cap; } page;
static char extra_headers[900];

void page_reset(void)
{
    page.n = 0;
    if (page.p) page.p[0] = 0;
}

void raw(const char *s, size_t n)
{
    if (page.n + n + 1 > page.cap) {
        size_t cap = (page.n + n + 1) * 2 + 4096;
        char *p = realloc(page.p, cap);
        if (!p) { logmsg("out of memory"); exit(1); }
        page.p = p;
        page.cap = cap;
    }
    memcpy(page.p + page.n, s, n);
    page.n += n;
    page.p[page.n] = 0;
}

static void escaped(const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': raw("&amp;", 5); break;
        case '<': raw("&lt;", 4); break;
        case '>': raw("&gt;", 4); break;
        case '"': raw("&quot;", 6); break;
        case '\'': raw("&#39;", 5); break;
        default: raw(s, 1);
        }
    }
}

static void url_escaped(const char *s)
{
    char b[4];
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c)) raw(s, 1);
        else { snprintf(b, sizeof b, "%%%02X", c); raw(b, 3); }
    }
}

void H(const char *fmt, ...)
{
    va_list ap;
    char num[32];
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            size_t run = strcspn(p, "%");
            raw(p, run);
            p += run - 1;
            continue;
        }
        switch (*++p) {
        case 's': { const char *s = va_arg(ap, const char *); escaped(s ? s : ""); break; }
        case 'r': { const char *s = va_arg(ap, const char *); if (s) raw(s, strlen(s)); break; }
        case 'u': { const char *s = va_arg(ap, const char *); url_escaped(s ? s : ""); break; }
        case 'd': snprintf(num, sizeof num, "%d", va_arg(ap, int)); raw(num, strlen(num)); break;
        case 'l': snprintf(num, sizeof num, "%ld", va_arg(ap, long)); raw(num, strlen(num)); break;
        case '%': raw("%", 1); break;
        default: raw("?", 1);
        }
    }
    va_end(ap);
}

/* ---------- sending ---------- */

static void send_all(int fd, const char *p, size_t n)
{
    long long end = now_ms() + REQUEST_MS;
    while (n) {
        struct pollfd pf = {fd, POLLOUT, 0};
        if (poll(&pf, 1, 1000) <= 0) { if (now_ms() > end) return; continue; }
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (k <= 0) return;
        p += k;
        n -= (size_t)k;
    }
}

static void respond(struct req *r, int status, const char *type, const char *body, size_t len)
{
    char head[1800];
    const char *what = status == 200 ? "OK" : status == 303 ? "See Other" : status == 400 ? "Bad Request" :
                       status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" :
                       status == 411 ? "Length Required" : status == 413 ? "Too Large" : "Error";
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: same-origin\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'self'; script-src 'self'; font-src 'self'; form-action 'self'; "
        "frame-ancestors 'none'; base-uri 'none'\r\n"
        "%s\r\n", status, what, type, len, extra_headers);
    send_all(r->fd, head, (size_t)n);
    if (strcmp(r->method, "HEAD")) send_all(r->fd, body, len);
    extra_headers[0] = 0;
}

static void add_header(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void add_header(const char *fmt, ...)
{
    va_list ap;
    size_t at = strlen(extra_headers);
    va_start(ap, fmt);
    vsnprintf(extra_headers + at, sizeof extra_headers - at, fmt, ap);
    va_end(ap);
}

void send_page(struct req *r, int status)
{
    respond(r, status, "text/html; charset=utf-8", page.p ? page.p : "", page.n);
}

void send_file(struct req *r, const char *type, const char *filename)
{
    add_header("Content-Disposition: attachment; filename=\"%s\"\r\n", filename);
    respond(r, 200, type, page.p ? page.p : "", page.n);
}

void redirect(struct req *r, const char *where)
{
    add_header("Location: %s\r\n", where);
    respond(r, 303, "text/plain", "", 0);
}

/* The theme the phone is in (the store's screen, theme; Switchboard if it names none we
 * have): every page says it in <html data-theme>, and the style sheet does the rest. */
const char *web_theme(void)
{
    const char *name = cfg_str("screen", "theme", t48_themes[0].name);
    return t48_theme_named(name) ? name : t48_themes[0].name;
}

static void plain(struct req *r, int status, const char *text)
{
    page.n = 0;
    H("<!doctype html><html lang='en' data-theme='%s'><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>T48S</title><link rel='stylesheet' href='/style.css'><main class='lone'><div class='card'><h1>%s</h1>"
      "<p><a href='/'>Overview</a></p></div></main></html>", web_theme(), text);
    send_page(r, status);
}

/* ---------- the form, and the address's query ---------- */

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            int c = hexval(s[1]) * 16 + hexval(s[2]);
            *o++ = c ? (char)c : ' ';
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

const char *form(const struct req *r, const char *name)
{
    for (int i = 0; i < r->nf; i++)
        if (!strcmp(r->f[i].name, name)) return r->f[i].value;
    return "";
}

const char *query(const struct req *r, const char *name)
{
    static char v[128];
    size_t nl = strlen(name);
    v[0] = 0;
    for (const char *p = r->query; *p;) {
        size_t len = strcspn(p, "&");
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            snprintf(v, sizeof v, "%.*s", (int)(len - nl - 1), p + nl + 1);
            url_decode(v);
            break;
        }
        p += len + (p[len] == '&');
    }
    return v;
}

static void parse_urlencoded(struct req *r)
{
    for (char *p = r->body; p && *p && r->nf < MAX_FIELDS;) {
        char *amp = strchr(p, '&'), *eq;
        if (amp) *amp = 0;
        if ((eq = strchr(p, '='))) {
            *eq = 0;
            url_decode(p);
            url_decode(eq + 1);
            r->f[r->nf].name = p;
            r->f[r->nf++].value = eq + 1;
        }
        p = amp ? amp + 1 : NULL;
    }
}

static char *find_bytes(char *hay, size_t hn, const char *needle, size_t nn)
{
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (hay[i] == needle[0] && !memcmp(hay + i, needle, nn)) return hay + i;
    return NULL;
}

/* multipart/form-data: a file chosen in the browser arrives this way. Fields
 * are text; one with a zero byte in it is dropped. */
static void parse_multipart(struct req *r)
{
    char delim[120];
    const char *b = strstr(r->ctype, "boundary=");
    if (!b) return;
    b += 9;
    if (*b == '"') b++;
    snprintf(delim, sizeof delim, "--%.100s", b);
    delim[strcspn(delim, "\";\r\n ")] = 0;
    size_t dl = strlen(delim);
    char *end = r->body + r->blen;
    char *p = find_bytes(r->body, r->blen, delim, dl);
    while (p && r->nf < MAX_FIELDS) {
        p += dl;
        if (p + 2 > end || (p[0] == '-' && p[1] == '-')) break;
        char *head_end = find_bytes(p, (size_t)(end - p), "\r\n\r\n", 4);
        if (!head_end) break;
        *head_end = 0;
        char *value = head_end + 4;
        char *next = find_bytes(value, (size_t)(end - value), delim, dl);
        if (!next) break;
        char *vend = next >= value + 2 ? next - 2 : value;   /* the line break before the boundary */
        *vend = 0;
        char *name = strstr(p, "name=\"");
        if (name && strlen(value) == (size_t)(vend - value)) {
            name += 6;
            name[strcspn(name, "\"")] = 0;
            r->f[r->nf].name = name;
            r->f[r->nf++].value = value;
        }
        p = next;
    }
}

/* ---------- sessions ---------- */

static const char *cookie(const struct req *r, const char *name)
{
    static char v[2][80];
    static int at;
    char *out = v[at = !at];
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *p = r->cookie; *p;) {
        while (*p == ' ') p++;
        size_t len = strcspn(p, ";");
        if (!strncmp(p, name, nl) && p[nl] == '=') { snprintf(out, sizeof v[0], "%.*s", (int)(len - nl - 1), p + nl + 1); break; }
        p += len + (p[len] == ';');
    }
    return out;
}

static struct session *session_find(const struct req *r)
{
    const char *t = cookie(r, "t48s");
    long long now = now_ms();
    struct session *found = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        struct session *s = &sessions[i];
        if (!s->token[0]) continue;
        if (now - s->last > SESSION_IDLE_MS) { memset(s, 0, sizeof *s); continue; }
        if (same(s->token, t)) found = s;
    }
    if (found) found->last = now;
    return found;
}

static struct session *session_new(const char *ip)
{
    struct session *s = &sessions[0];
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].token[0]) { s = &sessions[i]; break; }
        if (sessions[i].last < s->last) s = &sessions[i];   /* full: the one unused longest goes */
    }
    memset(s, 0, sizeof *s);
    random_hex(s->token, 32);
    random_hex(s->csrf, 32);
    snprintf(s->ip, sizeof s->ip, "%s", ip);
    s->last = now_ms();
    return s;
}

int sessions_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) n += sessions[i].token[0] != 0;
    return n;
}

void sessions_forget_others(const struct session *keep)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (&sessions[i] != keep) memset(&sessions[i], 0, sizeof sessions[i]);
}

void flash(struct req *r, int bad, const char *fmt, ...)
{
    va_list ap;
    if (!r->s) return;
    va_start(ap, fmt);
    vsnprintf(r->s->flash, sizeof r->s->flash, fmt, ap);
    va_end(ap);
    r->s->flash_bad = bad;
}

void web_stop_soon(void) { stop_soon = 1; }
const char *web_url(void) { return url; }

/* ---------- pairing, and the status file the shell reads ---------- */

static struct {
    char code[8], nonce[33], from[48];
    long until;
    int tries;
} pair;
static int bad_total;
static long locked_until;
static long long last_start = -START_GAP_MS;

static void status_write(void)
{
    static char last[400];
    char text[400], path[300];
    int n = snprintf(text, sizeof text, "url=%s\npaired=%d\n", url, sessions_count());
    if (pair.code[0])
        snprintf(text + n, sizeof text - (size_t)n, "pair_code=%s\npair_until=%ld\npair_from=%s\n", pair.code, pair.until, pair.from);
    if (!strcmp(text, last)) return;
    snprintf(last, sizeof last, "%s", text);
    mkdir(opt.web, 0700);
    chmod(opt.web, 0700);
    snprintf(path, sizeof path, "%s/status", opt.web);
    if (save_file(path, text, 0600) < 0) logmsg("cannot write %s: %s", path, strerror(errno));
}

static void pair_clear(void)
{
    memset(&pair, 0, sizeof pair);
    status_write();
}

/* the code runs out; or the owner pressed X at the screen */
static void pair_tick(void)
{
    char path[300];
    if (pair.code[0] && pair.until <= (long)time(NULL)) { logmsg("pairing: the code ran out"); pair_clear(); }
    snprintf(path, sizeof path, "%s/refuse", opt.web);
    char *t = load_file(path, 32);
    if (!t) return;
    unlink(path);
    t[strcspn(t, "\r\n")] = 0;
    if (pair.code[0] && !strcmp(t, pair.code)) { logmsg("pairing: refused at the phone's screen"); pair_clear(); }
    free(t);
}

static int locked(void)
{
    if (locked_until && locked_until <= (long)time(NULL)) { locked_until = 0; bad_total = 0; }
    return locked_until != 0;
}

static void pair_start(struct req *r)
{
    unsigned char b[4];
    unsigned v;
    if (locked()) { redirect(r, "/?e=locked"); return; }
    if (now_ms() - last_start < START_GAP_MS) { redirect(r, "/?e=wait"); return; }
    last_start = now_ms();
    do {   /* six digits, each as likely as the next */
        random_bytes(b, 4);
        v = ((unsigned)b[0] << 24 | (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3]);
    } while (v >= 4294000000u);
    memset(&pair, 0, sizeof pair);
    snprintf(pair.code, sizeof pair.code, "%06u", v % 1000000u);
    random_hex(pair.nonce, 16);
    snprintf(pair.from, sizeof pair.from, "%s", r->peer);
    pair.until = (long)time(NULL) + PAIR_SECONDS;
    status_write();
    logmsg("pairing: a code is on the screen, asked for from %s", r->peer);
    add_header("Set-Cookie: t48p=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d\r\n", pair.nonce, PAIR_SECONDS + 60);
    redirect(r, "/pair");
}

static int pair_mine(const struct req *r)
{
    return pair.code[0] && pair.until > (long)time(NULL) && same(pair.nonce, cookie(r, "t48p"));
}

static void pair_finish(struct req *r)
{
    char typed[16];
    size_t n = 0;
    pair_tick();
    if (locked()) { redirect(r, "/?e=locked"); return; }
    if (!pair_mine(r)) { redirect(r, "/?e=gone"); return; }
    for (const char *p = form(r, "code"); *p && n < sizeof typed - 1; p++)
        if (*p != ' ' && *p != '-') typed[n++] = *p;
    typed[n] = 0;
    if (!same(pair.code, typed)) {
        pair.tries++;
        bad_total++;
        logmsg("pairing: a wrong code from %s (%d of %d for this code)", r->peer, pair.tries, PAIR_TRIES);
        if (bad_total >= LOCK_AFTER) {
            locked_until = (long)time(NULL) + LOCK_SECONDS;
            logmsg("pairing: too many wrong codes, closed for %d minutes", LOCK_SECONDS / 60);
            pair_clear();
            redirect(r, "/?e=locked");
        } else if (pair.tries >= PAIR_TRIES) {
            pair_clear();
            redirect(r, "/?e=tries");
        } else {
            redirect(r, "/pair?e=wrong");
        }
        return;
    }
    struct session *s = session_new(r->peer);
    bad_total = 0;
    pair_clear();
    status_write();
    logmsg("pairing: a browser at %s is paired (%d now)", r->peer, sessions_count());
    add_header("Set-Cookie: t48s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000\r\n", s->token);
    add_header("Set-Cookie: t48p=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0\r\n");
    redirect(r, "/");
}

#define HTTP_NOTE "Plain HTTP: readable by anyone on this network, passwords included."

static void lone_begin(const char *title)
{
    page.n = 0;
    H("<!doctype html><html lang='en' data-theme='%s'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>%s</title><link rel='stylesheet' href='/style.css'></head><body><main class='lone'>"
      "<div class='brand'><span class='dot'></span>T48S <span class='dim'>open firmware</span></div><div class='card'>", web_theme(), title);
}

static void lone_end(struct req *r)
{
    H("</div><p class='note warn'>%s</p></main></body></html>", HTTP_NOTE);
    send_page(r, 200);
}

static void page_welcome(struct req *r)
{
    const char *e = query(r, "e");
    lone_begin("T48S setup");
    H("<h1>Pair this browser</h1>");
    if (!strcmp(e, "locked")) H("<p class='flash bad'>Too many wrong codes. Pairing is locked for 10 minutes.</p>");
    else if (!strcmp(e, "wait")) H("<p class='flash bad'>Too soon. Try again in a few seconds.</p>");
    else if (!strcmp(e, "tries")) H("<p class='flash bad'>Five wrong tries. Request a new code.</p>");
    else if (!strcmp(e, "gone")) H("<p class='flash bad'>Code expired or refused. Request a new one.</p>");
    else if (!strcmp(e, "out")) H("<p class='flash'>Signed out.</p>");
    H("<p>The phone shows a six-digit code. Enter it here.</p>"
      "<form method='post' action='/pair/start'><button class='primary' type='submit'>Show code</button></form>"
      "<p class='dim small'>Not shown during a call or in an app.</p>");
    lone_end(r);
}

static void page_pair(struct req *r)
{
    pair_tick();
    if (!pair_mine(r)) { redirect(r, "/?e=gone"); return; }
    lone_begin("T48S setup");
    H("<h1>Enter code</h1>");
    if (!strcmp(query(r, "e"), "wrong"))
        H("<p class='flash bad'>Wrong code. %d %s left.</p>", PAIR_TRIES - pair.tries, PAIR_TRIES - pair.tries == 1 ? "try" : "tries");
    H("<p>On the phone's screen. Valid for 2 minutes.</p>"
      "<form method='post' action='/pair/finish' autocomplete='off'>"
      "<input class='code' name='code' inputmode='numeric' pattern='[0-9 ]*' maxlength='7' placeholder='000 000' autofocus required>"
      "<button class='primary' type='submit'>Pair</button></form>"
      "<p class='dim small'>No code? Close any open app on the phone (hold X), then <a href='/'>request another</a>.</p>");
    lone_end(r);
}

/* ---------- the frame around every page ---------- */

void page_begin(struct req *r, const char *title, const char *nav)
{
    static const struct { const char *path, *label; } items[] = {
        {"/", "Overview"}, {"/accounts", "Accounts"}, {"/network", "Network"}, {"/time", "Date and time"},
        {"/sound", "Sound and screen"}, {"/contacts", "Contacts"}, {"/history", "Calls"}, {"/apps", "Apps"},
        {"/access", "Access"}, {"/backup", "Backup"},
    };
    page.n = 0;
    H("<!doctype html><html lang='en' data-theme='%s'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>%s - T48S</title><link rel='stylesheet' href='/style.css'><script src='/t48.js' defer></script></head><body>"
      "<header><div class='brand'><span class='dot'></span>T48S <span class='dim'>open firmware</span></div>", web_theme(), title);
    form_begin(r, "/logout", "class='out'");
    H("<button type='submit' class='quiet'>Sign out</button></form></header><div class='frame'><nav>");
    for (unsigned i = 0; i < sizeof items / sizeof items[0]; i++)
        H("<a href='%s'%r>%s</a>", items[i].path, !strcmp(items[i].path, nav) ? " class='here'" : "", items[i].label);
    H("</nav><main><h1>%s</h1>", title);
    if (r->s && r->s->flash[0]) {
        H("<p class='flash%r' role='status'>%s</p>", r->s->flash_bad ? " bad" : "", r->s->flash);
        r->s->flash[0] = 0;
    }
}

void page_end(struct req *r)
{
    H("<footer><p class='note warn'>%s</p><p class='dim small'>Firmware %s. AGPL-3.0-or-later. "
      "<a href='https://github.com/M0LTE/yealink-t48s'>Source</a></p></footer>"
      "</main></div></body></html>", HTTP_NOTE, T48_VERSION);
    send_page(r, 200);
}

void form_begin(struct req *r, const char *action, const char *more)
{
    H("<form method='post' action='%s' %r><input type='hidden' name='csrf' value='%s'>", action, more ? more : "", r->s ? r->s->csrf : "");
}

/* ---------- one request ---------- */

/* 10/8, 172.16/12, 192.168/16, link-local, loopback, and 100.64/10 (a VPN's
 * addresses): a caller from anywhere else has been let in by a router that
 * should not have */
static int private_address(const struct in_addr *a)
{
    unsigned long v = ntohl(a->s_addr);
    return (v >> 24) == 10 || (v >> 24) == 127 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 16) == 0xA9FE ||
           (v >> 22) == (0x64400000ul >> 22);
}

static int header_is(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) || line[n] != ':') return 0;
    line += n + 1;
    while (*line == ' ' || *line == '\t') line++;
    *value = line;
    return 1;
}

/* 0, or the status to refuse with */
static int read_request(struct req *r, char **keep)
{
    static char head[8192];
    size_t got = 0;
    long clen = -1;
    char *end = NULL;
    long long deadline = now_ms() + REQUEST_MS;

    while (!end) {
        struct pollfd pf = {r->fd, POLLIN, 0};
        if (got >= sizeof head - 1) return 413;
        if (now_ms() > deadline || poll(&pf, 1, 2000) < 0) return 400;
        if (!pf.revents) continue;
        ssize_t k = recv(r->fd, head + got, sizeof head - 1 - got, 0);
        if (k <= 0) return 400;
        got += (size_t)k;
        head[got] = 0;
        end = find_bytes(head, got, "\r\n\r\n", 4);
    }
    *end = 0;
    size_t body_have = got - (size_t)(end + 4 - head);

    char target[400] = "", version[16] = "";
    char *line = head, *nl = strstr(head, "\r\n");
    if (nl) *nl = 0;
    if (sscanf(line, "%7s %399s %15s", r->method, target, version) != 3 || target[0] != '/' || strncmp(version, "HTTP/1.", 7)) return 400;
    char *q = strchr(target, '?');
    if (q) { *q = 0; snprintf(r->query, sizeof r->query, "%s", q + 1); }
    if (strlen(target) >= sizeof r->path) return 404;
    snprintf(r->path, sizeof r->path, "%s", target);
    for (line = nl ? nl + 2 : NULL; line && *line;) {
        const char *v;
        nl = strstr(line, "\r\n");
        if (nl) *nl = 0;
        if (header_is(line, "Host", &v)) snprintf(r->host, sizeof r->host, "%s", v);
        else if (header_is(line, "Origin", &v)) snprintf(r->origin, sizeof r->origin, "%s", v);
        else if (header_is(line, "Cookie", &v)) snprintf(r->cookie, sizeof r->cookie, "%s", v);
        else if (header_is(line, "Content-Type", &v)) snprintf(r->ctype, sizeof r->ctype, "%s", v);
        else if (header_is(line, "Content-Length", &v)) clen = atol(v);
        else if (header_is(line, "Transfer-Encoding", &v)) return 411;
        line = nl ? nl + 2 : NULL;
    }
    if (strcmp(r->method, "POST")) return strcmp(r->method, "GET") && strcmp(r->method, "HEAD") ? 405 : 0;
    if (clen < 0) clen = 0;   /* a form with nothing in it */
    if (clen > MAX_BODY) return 413;
    r->body = *keep = malloc((size_t)clen + 1);
    if (!r->body) return 413;
    if (body_have > (size_t)clen) body_have = (size_t)clen;
    memcpy(r->body, end + 4, body_have);
    while (body_have < (size_t)clen) {
        struct pollfd pf = {r->fd, POLLIN, 0};
        if (now_ms() > deadline || poll(&pf, 1, 2000) < 0) return 400;
        if (!pf.revents) continue;
        ssize_t k = recv(r->fd, r->body + body_have, (size_t)clen - body_have, 0);
        if (k <= 0) return 400;
        body_have += (size_t)k;
    }
    r->body[clen] = 0;
    r->blen = (size_t)clen;
    if (!strncasecmp(r->ctype, "multipart/form-data", 19)) parse_multipart(r);
    else if (strlen(r->body) == r->blen) parse_urlencoded(r);
    return 0;
}

static void handle(struct req *r)
{
    char *body = NULL;
    int bad = read_request(r, &body);
    if (bad) {
        plain(r, bad, bad == 413 ? "Too large" : "Bad request");
        free(body);
        return;
    }
    int post = !strcmp(r->method, "POST");

    /* nothing here needs pairing, or changes anything */
    if (!post && !strcmp(r->path, "/style.css")) {
        add_header("Cache-Control: max-age=600\r\n");
        respond(r, 200, "text/css; charset=utf-8", asset_css, strlen(asset_css));
        goto done;
    }
    if (!post && !strncmp(r->path, "/fonts/", 7)) {
        const struct asset_font *f = asset_fonts;
        while (f->name && strcmp(f->name, r->path + 7)) f++;
        if (!f->name) { plain(r, 404, "Not found"); goto done; }
        add_header("Cache-Control: max-age=86400\r\n");
        respond(r, 200, "font/woff2", (const char *)f->data, f->len);
        goto done;
    }
    if (!post && !strcmp(r->path, "/t48.js")) {
        add_header("Cache-Control: max-age=600\r\n");
        respond(r, 200, "text/javascript; charset=utf-8", asset_js, strlen(asset_js));
        goto done;
    }

    /* a change must come from one of our own pages: a browser says where a form was sent from */
    if (post && r->origin[0]) {
        char want[160];
        snprintf(want, sizeof want, "http://%s", r->host);
        if (strcmp(r->origin, want)) {
            logmsg("refused %s from %s: sent from another site", r->path, r->peer);
            plain(r, 403, "Refused: cross-site request");
            goto done;
        }
    }

    r->s = session_find(r);
    if (!r->s) {
        if (post && !strcmp(r->path, "/pair/start")) pair_start(r);
        else if (post && !strcmp(r->path, "/pair/finish")) pair_finish(r);
        else if (post) { logmsg("refused %s from %s: not paired", r->path, r->peer); plain(r, 403, "Not paired"); }
        else if (!strcmp(r->path, "/pair")) page_pair(r);
        else if (!strcmp(r->path, "/")) page_welcome(r);
        else redirect(r, "/");
        goto done;
    }
    if (post && !same(r->s->csrf, form(r, "csrf"))) {
        logmsg("refused %s from %s: the form's token is missing or wrong", r->path, r->peer);
        plain(r, 403, "Form expired. Reload the page and try again.");
        goto done;
    }
    if (post && !strcmp(r->path, "/logout")) {
        memset(r->s, 0, sizeof *r->s);
        status_write();
        add_header("Set-Cookie: t48s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0\r\n");
        redirect(r, "/?e=out");
        goto done;
    }
    if (!post && !strcmp(r->path, "/pair")) { redirect(r, "/"); goto done; }
    for (const struct route *rt = routes; rt->path; rt++) {
        if (strcmp(rt->path, r->path)) continue;
        if (strcmp(rt->method, post ? "POST" : "GET")) continue;
        if (post) logmsg("%s from %s", r->path, r->peer);   /* what was done, never what was typed */
        rt->handler(r);
        status_write();
        goto done;
    }
    plain(r, 404, "Not found");
done:
    free(body);
}

/* ---------- listening ---------- */

static int iface_address(struct in_addr *out)
{
    struct ifaddrs *list, *i;
    int found = 0;
    if (getifaddrs(&list) < 0) return 0;
    for (i = list; i && !found; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || strcmp(i->ifa_name, wopt.iface) || !(i->ifa_flags & IFF_UP)) continue;
        *out = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        found = 1;
    }
    freeifaddrs(list);
    return found;
}

static int listener(const struct in_addr *addr, int port, int to_device)
{
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port), .sin_addr = *addr};
    int one = 1, fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* and nothing that arrives by another interface, whatever address it was sent to */
    if (to_device && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, wopt.iface, (socklen_t)strlen(wopt.iface) + 1) < 0)
        logmsg("could not tie the socket to %s: %s", wopt.iface, strerror(errno));
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}

static void on_stop(int sig) { (void)sig; stopping = 1; }

static int enabled(void) { return !strcmp(cfg_str("access", "web", "off"), "on"); }

static int usage(void)
{
    fprintf(stderr,
        "t48-web [options]     the phone's web setup page (docs/web.md)\n"
        "  --interface NAME   listen on this interface's address only (%s)\n"
        "  --port N           (%d)\n"
        "  --listen ADDR:PORT listen here instead, for trying it on a PC\n"
        "  --config DIR       settings files (%s)\n"
        "  --state DIR        contacts and call history (%s)\n"
        "  --apps DIRS        installed apps (%s)\n"
        "  --data DIR         apps' own folders (%s)\n"
        "  --apply PROGRAM    run with a settings file's name when it has changed (%s)\n"
        "  --app PROGRAM      the app manager's command (%s)\n"
        "  --access DIR       where t48-access leaves its status file (%s)\n"
        "  --keys DIR         where t48-access keeps the keys that can log in (%s)\n"
        "  --run DIR          where this leaves its status file for the shell (%s)\n"
        "It runs only while \"web=on\" is in the settings file \"access\": the owner turns\n"
        "it on at the phone's screen (Settings, Access).\n",
        wopt.iface, wopt.port, opt.config, opt.state, opt.apps, opt.data, opt.apply, wopt.appcmd, opt.access, wopt.keep, opt.web);
    return 2;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char **dst = !strcmp(argv[i], "--interface") ? &wopt.iface : !strcmp(argv[i], "--listen") ? &wopt.listen :
                           !strcmp(argv[i], "--config") ? &opt.config : !strcmp(argv[i], "--state") ? &opt.state :
                           !strcmp(argv[i], "--apps") ? &opt.apps : !strcmp(argv[i], "--data") ? &opt.data :
                           !strcmp(argv[i], "--apply") ? &opt.apply : !strcmp(argv[i], "--app") ? &wopt.appcmd :
                           !strcmp(argv[i], "--access") ? &opt.access : !strcmp(argv[i], "--keys") ? &wopt.keep :
                           !strcmp(argv[i], "--run") ? &opt.web : NULL;
        if (!strcmp(argv[i], "--port") && i + 1 < argc) { wopt.port = atoi(argv[++i]); continue; }
        if (!dst || i + 1 >= argc) return usage();
        *dst = argv[++i];
    }
    if (!enabled()) {
        logmsg("the web page is off. The owner turns it on at the phone's screen: Settings, Access");
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);

    struct in_addr bound = {0}, fixed = {0};
    int fixed_port = wopt.port, fd = -1;
    if (wopt.listen) {
        char host[64];
        const char *colon = strrchr(wopt.listen, ':');
        snprintf(host, sizeof host, "%.*s", colon ? (int)(colon - wopt.listen) : 63, wopt.listen);
        if (colon) fixed_port = atoi(colon + 1);
        if (inet_pton(AF_INET, host, &fixed) != 1) return usage();
    }
    status_write();

    long long next_tick = 0;
    while (!stopping && !stop_soon) {
        long long now = now_ms();
        if (now >= next_tick) {
            next_tick = now + 1000;
            while (waitpid(-1, NULL, WNOHANG) > 0) {}
            if (!enabled()) { logmsg("turned off at the phone: leaving"); break; }
            pair_tick();
            /* follow the interface: no address yet, or a new one from DHCP or the Network page */
            struct in_addr want = fixed;
            int have = wopt.listen ? 1 : iface_address(&want);
            if (fd >= 0 && (!have || want.s_addr != bound.s_addr)) { close(fd); fd = -1; url[0] = 0; logmsg("the address has gone"); }
            if (fd < 0 && have) {
                fd = listener(&want, fixed_port, !wopt.listen);
                if (fd >= 0) {
                    char a[32];
                    bound = want;
                    inet_ntop(AF_INET, &want, a, sizeof a);
                    if (fixed_port == 80) snprintf(url, sizeof url, "http://%s/", a);
                    else snprintf(url, sizeof url, "http://%s:%d/", a, fixed_port);
                    logmsg("listening at %s", url);
                } else {
                    static int said;
                    if (!said++) logmsg("cannot listen on port %d: %s", fixed_port, strerror(errno));
                }
            }
            status_write();
        }
        struct pollfd pf = {fd, POLLIN, 0};
        int wait = (int)(next_tick - now_ms());
        if (poll(&pf, fd >= 0 ? 1 : 0, wait > 0 ? wait : 0) <= 0 || !(pf.revents & POLLIN)) continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int c = accept4(fd, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0) continue;
        if (private_address(&peer.sin_addr)) {
            struct req r;
            memset(&r, 0, sizeof r);
            r.fd = c;
            inet_ntop(AF_INET, &peer.sin_addr, r.peer, sizeof r.peer);
            handle(&r);
            shutdown(c, SHUT_WR);
        }
        close(c);
    }
    /* nothing for the shell to show once we are gone */
    char path[300];
    snprintf(path, sizeof path, "%s/status", opt.web);
    unlink(path);
    logmsg("stopped");
    return 0;
}
