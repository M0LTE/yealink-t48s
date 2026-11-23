// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-app: the app manager (issue #22). list, install, update, remove.
 *
 * A feed is an index.json at an https address (docs/apps.md); each entry
 * gives a package's address, size and SHA-256. A package (.t48app) is a
 * gzipped tar holding one folder named after the app's id. Installing is:
 * fetch the index, refuse an app written for a newer API level than this
 * shell speaks, download the package with curl, check its size and SHA-256
 * against the index, unpack it beside the installed apps (plain files and
 * folders only, all inside the one folder), read its manifest, and only then
 * move it into place with a rename. Nothing half-installed is ever visible.
 *
 * The shell's Apps screen runs this same code as a child process with
 * --machine and reads its lines, so the screen never waits on the network.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "shell.h"

#define MAX_INDEX (512 * 1024)
#define MAX_PACKAGE (32L * 1024 * 1024)
#define MAX_UNPACKED (96L * 1024 * 1024)
#define MAX_FILES 4000
#define MAX_FEED_APPS 128
#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

static int machine;              /* --machine: lines for the shell instead of words for a person */
static char apps_dir[300];       /* where apps are installed: the first folder of --apps */
static char errtxt[300];

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errtxt, sizeof errtxt, fmt, ap);
    va_end(ap);
}

static void progress(const char *what, int percent)
{
    if (!machine) return;
    if (percent >= 0) printf("progress %s %d\n", what, percent);
    else printf("progress %s\n", what);
    fflush(stdout);
}

/* ---------- SHA-256 ---------- */

struct sha256 { uint32_t h[8]; unsigned char buf[64]; size_t n; unsigned long long total; };

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha256 *s, const unsigned char *p)
{
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + sha_k[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof a[0]);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}

static void sha_init(struct sha256 *s)
{
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0);
    s->n = 0;
    s->total = 0;
}

static void sha_add(struct sha256 *s, const unsigned char *p, size_t len)
{
    s->total += len;
    while (len) {
        size_t k = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->buf + s->n, p, k);
        s->n += k; p += k; len -= k;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}

static void sha_hex(struct sha256 *s, char out[65])
{
    unsigned long long bits = s->total * 8;
    unsigned char pad[72] = {0x80}, len[8];
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_add(s, pad, (s->n < 56 ? 56 : 120) - s->n);
    sha_add(s, len, 8);
    for (int i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", s->h[i]);
}

/* size and SHA-256 of a file: 0, or -1 */
static int sha_file(const char *path, char hex[65], long *size)
{
    unsigned char b[8192];
    struct sha256 s;
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha_init(&s);
    while ((n = fread(b, 1, sizeof b, f)) > 0) sha_add(&s, b, n);
    fclose(f);
    *size = (long)s.total;
    sha_hex(&s, hex);
    return 0;
}

/* ---------- small things ---------- */

static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st; (void)flag; (void)f;
    return remove(p) < 0 && errno != ENOENT ? -1 : 0;
}

static void rm_rf(const char *path) { nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS); }

static long tree_bytes;
static int add_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)p; (void)f;
    if (flag == FTW_F) tree_bytes += (long)st->st_size;
    return 0;
}

static long dir_size(const char *path)
{
    tree_bytes = 0;
    nftw(path, add_one, 16, FTW_PHYS);
    return tree_bytes;
}

/* compare versions number by number: 0.10.0 is newer than 0.9.1 */
static int version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (ea == a || eb == b) return strcmp(a, b);
        if (x != y) return x < y ? -1 : 1;
        a = *ea ? ea + 1 : ea;
        b = *eb ? eb + 1 : eb;
        if ((*ea && *ea != '.') || (*eb && *eb != '.')) return strcmp(ea, eb);
    }
    return 0;
}

/* Run curl and wait, saying how far it has got. 0, or -1 with the reason. */
static int fetch(const char *url, const char *dest, long max_bytes, long expect, const char *what)
{
    char maxs[24];
    int https = !strncmp(url, "https://", 8);
    snprintf(maxs, sizeof maxs, "%ld", max_bytes);
    unlink(dest);
    pid_t pid = fork();
    if (pid < 0) { fail("cannot start a download (%s)", strerror(errno)); return -1; }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        /* the phone's curl is built without knowing where the CA bundle is */
        if (access(CA_BUNDLE, R_OK) == 0) setenv("CURL_CA_BUNDLE", CA_BUNDLE, 0);
        execlp("curl", "curl", "-fsSL", "--proto", "=https,http,file", "--proto-redir", https ? "=https" : "=https,http",
               "--connect-timeout", "20", "--max-time", "600", "--max-filesize", maxs, "-o", dest, "--", url, (char *)NULL);
        _exit(127);
    }
    int status, last = -1;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) { fail("lost track of the download"); return -1; }
        struct stat st;
        if (expect > 0 && stat(dest, &st) == 0) {
            int pc = (int)((long long)st.st_size * 100 / expect);
            if (pc > 99) pc = 99;
            if (pc != last) progress(what, last = pc);
            if (st.st_size > max_bytes) kill(pid, SIGKILL);
        }
        usleep(100000);
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 0) return 0;
    unlink(dest);
    switch (code) {
    case 127: fail("curl is not installed"); break;
    case 6: fail("the server's name was not found: is the network up?"); break;
    case 7: fail("could not connect to the server"); break;
    case 22: case 37: fail("the server has no such file"); break;
    case 28: fail("the download took too long"); break;
    case 35: case 51: case 60: case 77: fail("the secure connection was refused: is the phone's clock right?"); break;
    case 63: fail("the file is bigger than allowed"); break;
    default: fail("the download failed (curl error %d)", code);
    }
    return -1;
}

/* ---------- unpacking ---------- */

static int read_full(int fd, unsigned char *b, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, b + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    return (int)got;
}

static int mkdirs(char *path)
{
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            int r = mkdir(path, 0755);
            *p = '/';
            if (r < 0 && errno != EEXIST) return -1;
        }
    return 0;
}

/* Unpack a .t48app into dest (an empty folder of ours). Only plain files and
 * folders, every one inside a single top folder with a proper app id for a
 * name; that id is returned. Anything else and the package is refused. */
static int unpack(const char *pkg, const char *dest, char *id, size_t idn)
{
    int pfd[2], ok = -1, files = 0;
    long total = 0;
    unsigned char h[512];

    id[0] = 0;
    if (pipe(pfd) < 0) { fail("out of descriptors"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { fail("cannot start gzip (%s)", strerror(errno)); close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(pfd[1], 1);
        if (null >= 0) { dup2(null, 0); dup2(null, 2); }
        close(pfd[0]);
        execlp("gzip", "gzip", "-dc", pkg, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    for (;;) {
        char name[300], path[700];
        if (read_full(pfd[0], h, 512) != 512) { fail("the package is cut short or not a .t48app"); break; }
        int empty = 1;
        for (int i = 0; i < 512 && empty; i++) empty = !h[i];
        if (empty) { ok = id[0] ? 0 : -1; if (ok) fail("the package is empty"); break; }
        if (memcmp(h + 257, "ustar", 5)) { fail("the package is not a tar file"); break; }
        long size = strtol((char *)h + 124, NULL, 8);
        long mode = strtol((char *)h + 100, NULL, 8);
        int type = h[156];
        if (h[345]) snprintf(name, sizeof name, "%.155s/%.100s", h + 345, h);
        else snprintf(name, sizeof name, "%.100s", h);
        size_t nl = strlen(name);
        while (nl && name[nl - 1] == '/') name[--nl] = 0;

        if (type == 'x' || type == 'g') {   /* extra headers some tar programs add: skip */
            for (long left = (size + 511) / 512; left > 0; left--)
                if (read_full(pfd[0], h, 512) != 512) break;
            continue;
        }
        if ((type != '0' && type != 0 && type != '5') || size < 0) {
            fail("the package holds something that is not a plain file or folder (%s)", name);
            break;
        }
        /* every name: id/..., no empty, "." or ".." parts, nothing odd */
        int bad = !nl || name[0] == '/' || nl > 200;
        for (const char *p = name; *p && !bad;) {
            size_t k = strcspn(p, "/");
            if (k == 0 || (k == 1 && p[0] == '.') || (k == 2 && p[0] == '.' && p[1] == '.')) bad = 1;
            for (size_t i = 0; i < k; i++)
                if ((unsigned char)p[i] < 32 || p[i] == 127 || p[i] == '\\') bad = 1;
            p += k;
            if (*p == '/') p++;
        }
        char top[80];
        snprintf(top, sizeof top, "%.*s", (int)strcspn(name, "/"), name);
        if (!bad && !id[0] && manifest_id_ok(top) && top[0] != '.') snprintf(id, idn, "%s", top);
        if (bad || strcmp(top, id)) { fail("the package is not one app folder (%s)", name); break; }
        if (++files > MAX_FILES || (total += size) > MAX_UNPACKED) { fail("the package is too big when unpacked"); break; }

        snprintf(path, sizeof path, "%s/%s", dest, name);
        if (type == '5') {
            if (mkdirs(path) < 0 || (mkdir(path, 0755) < 0 && errno != EEXIST)) { fail("cannot make %s (%s)", name, strerror(errno)); break; }
            continue;
        }
        if (mkdirs(path) < 0) { fail("cannot make the folder for %s (%s)", name, strerror(errno)); break; }
        int out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, (mode & 0111) ? 0755 : 0644);
        if (out < 0) { fail("cannot write %s (%s)", name, strerror(errno)); break; }
        long left = size;
        int wrote = 1;
        while (left > 0 && wrote) {
            int want = left > 512 ? 512 : (int)left;
            if (read_full(pfd[0], h, 512) != 512) { fail("the package is cut short"); wrote = 0; break; }
            if (write(out, h, (size_t)want) != want) { fail("cannot write %s (%s)", name, strerror(errno)); wrote = 0; }
            left -= want;
        }
        fchmod(out, (mode & 0111) ? 0755 : 0644);   /* never set-user-id, whatever the package says */
        close(out);
        if (!wrote) break;
    }
    if (ok < 0) kill(pid, SIGKILL);
    close(pfd[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (ok == 0 && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        fail(WIFEXITED(status) && WEXITSTATUS(status) == 127 ? "gzip is not installed" : "the package is damaged");
        ok = -1;
    }
    return ok;
}

/* ---------- the feed ---------- */

struct feed_app {
    char id[64], name[64], version[24], url[400], sha[72], perms[96], description[160];
    long size;
    int api;
};
static struct feed_app feed[MAX_FEED_APPS];
static int nfeed;
static char feed_url[400];

const char *pkg_default_feed(void) { return "https://m0lte.github.io/yealink-t48s-apps/index.json"; }

static void work_dir(char *out, size_t n)
{
    snprintf(out, n, "%s/.tmp-%d", apps_dir, (int)getpid());
}

/* fetch and read the index: 0, or -1 with the reason */
static int load_feed(void)
{
    char tmp[340], path[360];

    nfeed = 0;
    if (strncmp(feed_url, "https://", 8) && strncmp(feed_url, "file://", 7)) {
        fail("the feed address must start with https://");
        return -1;
    }
    work_dir(tmp, sizeof tmp);
    if (mkdir(tmp, 0700) < 0 && errno != EEXIST) { fail("cannot write to %s (%s)", apps_dir, strerror(errno)); return -1; }
    snprintf(path, sizeof path, "%s/index.json", tmp);
    progress("index", -1);
    if (fetch(feed_url, path, MAX_INDEX, 0, "index") < 0) return -1;
    FILE *f = fopen(path, "rb");
    char *txt = malloc(MAX_INDEX + 1);
    size_t n = f && txt ? fread(txt, 1, MAX_INDEX, f) : 0;
    if (f) fclose(f);
    unlink(path);
    if (!n) { free(txt); fail("the feed is empty"); return -1; }
    txt[n] = 0;
    const char *e = txt + n, *list = t48_json_get(txt, e, "apps");
    double v;
    if (!t48_json_num(t48_json_get(txt, e, "feed"), e, &v) || !list) { free(txt); fail("that address is not an app feed"); return -1; }
    if ((int)v != 1) { free(txt); fail("the feed is a newer kind (%d) than this phone understands", (int)v); return -1; }

    for (const char *a = t48_json_first(list, e); a && nfeed < MAX_FEED_APPS; a = t48_json_next(a, e)) {
        struct feed_app *fa = &feed[nfeed];
        char rel[300];
        double num;
        memset(fa, 0, sizeof *fa);
        if (!t48_json_str(t48_json_get(a, e, "id"), e, fa->id, sizeof fa->id) || !manifest_id_ok(fa->id)) continue;
        if (!t48_json_str(t48_json_get(a, e, "url"), e, rel, sizeof rel) || !rel[0]) continue;
        if (!t48_json_str(t48_json_get(a, e, "name"), e, fa->name, sizeof fa->name) || !fa->name[0])
            memcpy(fa->name, fa->id, sizeof fa->name);
        t48_json_str(t48_json_get(a, e, "version"), e, fa->version, sizeof fa->version);
        t48_json_str(t48_json_get(a, e, "sha256"), e, fa->sha, sizeof fa->sha);
        t48_json_str(t48_json_get(a, e, "description"), e, fa->description, sizeof fa->description);
        if (t48_json_num(t48_json_get(a, e, "size"), e, &num)) fa->size = (long)num;
        if (t48_json_num(t48_json_get(a, e, "api"), e, &num)) fa->api = (int)num;
        for (const char *p = t48_json_first(t48_json_get(a, e, "permissions"), e); p; p = t48_json_next(p, e)) {
            char one[32];
            size_t at = strlen(fa->perms);
            if (t48_json_str(p, e, one, sizeof one) && at + strlen(one) + 2 < sizeof fa->perms) {
                if (at) strcat(fa->perms, " ");
                strcat(fa->perms, one);
            }
        }
        /* nothing in these may break the one-line-per-app list */
        for (char *p = fa->name; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
        for (char *p = fa->description; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
        for (char *p = fa->version; *p; p++) if ((unsigned char)*p <= 32) *p = '_';
        /* the package's address: as given, or beside the index */
        if (strstr(rel, "://")) snprintf(fa->url, sizeof fa->url, "%s", rel);
        else {
            const char *slash = strrchr(feed_url, '/');
            snprintf(fa->url, sizeof fa->url, "%.*s%s", (int)(slash - feed_url + 1), feed_url, rel);
        }
        nfeed++;
    }
    free(txt);
    return 0;
}

static const struct feed_app *feed_find(const char *id)
{
    for (int i = 0; i < nfeed; i++)
        if (!strcmp(feed[i].id, id)) return &feed[i];
    return NULL;
}

static const struct manifest *installed_find(const char *id)
{
    for (int i = 0; i < napps; i++)
        if (!strcmp(apps[i].id, id)) return &apps[i];
    return NULL;
}

/* is this installed app in the folder we install to, rather than part of the system image? */
static int ours(const struct manifest *m)
{
    char *full = realpath(apps_dir, NULL);
    size_t n = full ? strlen(full) : 0;
    int r = full && !strncmp(m->dir, full, n) && m->dir[n] == '/';
    free(full);
    return r;
}

/* ---------- the commands ---------- */

static void clean_leftovers(void)
{
    DIR *d = opendir(apps_dir);
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d))) {
        char p[600];
        if (strncmp(de->d_name, ".tmp-", 5) && strncmp(de->d_name, ".old-", 5)) continue;
        snprintf(p, sizeof p, "%s/%s", apps_dir, de->d_name);
        rm_rf(p);
    }
    closedir(d);
}

/* Unpack pkg, check it and move it into place. want_id: what the feed said it is, or NULL. */
static int install_file(const char *pkg, const char *want_id, char *id, size_t idn, char *version, size_t vn)
{
    char tmp[340], stage[400], from[500], final[400], old[440], err[200];
    struct manifest m;

    work_dir(tmp, sizeof tmp);
    snprintf(stage, sizeof stage, "%s/stage", tmp);
    rm_rf(stage);
    if ((mkdir(tmp, 0700) < 0 && errno != EEXIST) || mkdir(stage, 0700) < 0) {
        fail("cannot write to %s (%s)", apps_dir, strerror(errno));
        return -1;
    }
    progress("unpack", -1);
    if (unpack(pkg, stage, id, idn) < 0) return -1;
    if (want_id && strcmp(want_id, id)) { fail("the package holds %s, not %s", id, want_id); return -1; }
    snprintf(from, sizeof from, "%s/%s", stage, id);
    if (manifest_read(from, &m, err, sizeof err) < 0) { manifest_free(&m); fail("the app cannot be used here: %s", err); return -1; }
    int same = !strcmp(m.id, id);
    char prog[500];
    snprintf(prog, sizeof prog, "%s/%s", from, m.exec);
    snprintf(version, vn, "%s", m.version[0] ? m.version : "-");
    manifest_free(&m);
    if (!same) { fail("the package's folder and its manifest disagree about the app's id"); return -1; }
    if (access(prog, X_OK) < 0) { fail("the package has no program to run"); return -1; }

    chmod(stage, 0755);
    snprintf(final, sizeof final, "%s/%s", apps_dir, id);
    snprintf(old, sizeof old, "%s/.old-%s-%d", apps_dir, id, (int)getpid());
    int had = rename(final, old) == 0;
    if (rename(from, final) < 0) {
        fail("cannot put the app in place (%s)", strerror(errno));
        if (had) rename(old, final);
        return -1;
    }
    if (had) rm_rf(old);
    sync();
    return 0;
}

static int cmd_install(const char *what, int only_if_newer)
{
    char tmp[340], pkg[360], id[64], version[24], hex[65];
    long size;
    int r;

    if (strchr(what, '/') || (strlen(what) > 7 && !strcmp(what + strlen(what) - 7, ".t48app"))) {
        /* a file copied to the phone by hand: nothing to check it against */
        r = install_file(what, NULL, id, sizeof id, version, sizeof version);
    } else {
        if (load_feed() < 0) return -1;
        const struct feed_app *fa = feed_find(what);
        const struct manifest *have = installed_find(what);
        if (!fa) { fail("the feed has no app called %s", what); return -1; }
        if (only_if_newer && have && version_cmp(fa->version, have->version) <= 0) {
            if (machine) printf("done %s %s\n", fa->id, have->version);
            else printf("%s is up to date (%s)\n", fa->id, have->version);
            return 0;
        }
        if (fa->api > T48_API_LEVEL) {
            fail("%s needs API level %d and this phone speaks %d: update the phone's firmware first", fa->name, fa->api, T48_API_LEVEL);
            return -1;
        }
        if (strlen(fa->sha) != 64) { fail("the feed gives no SHA-256 for %s, so it cannot be checked", fa->name); return -1; }
        if (fa->size <= 0 || fa->size > MAX_PACKAGE) { fail("the feed gives no sensible size for %s", fa->name); return -1; }
        struct statvfs vfs;
        if (statvfs(apps_dir, &vfs) == 0 && vfs.f_blocks > 0 &&
            (unsigned long long)vfs.f_bavail * vfs.f_frsize < (unsigned long long)fa->size * 5 + 262144) {
            fail("not enough room for %s", fa->name);
            return -1;
        }
        work_dir(tmp, sizeof tmp);
        snprintf(pkg, sizeof pkg, "%s/package", tmp);
        progress("download", 0);
        if (fetch(fa->url, pkg, fa->size, fa->size, "download") < 0) return -1;
        progress("verify", -1);
        if (sha_file(pkg, hex, &size) < 0) { fail("the download went missing"); return -1; }
        if (size != fa->size || strcasecmp(hex, fa->sha)) {
            unlink(pkg);
            fail("the download does not match the checksum in the feed, so it was thrown away");
            return -1;
        }
        r = install_file(pkg, fa->id, id, sizeof id, version, sizeof version);
    }
    if (r < 0) return -1;
    if (machine) printf("done %s %s\n", id, version);
    else printf("installed %s %s\n", id, version);
    return 0;
}

static int cmd_update_all(void)
{
    int failed = 0, n = 0;
    if (load_feed() < 0) return -1;
    for (int i = 0; i < napps; i++) {
        const struct feed_app *fa = feed_find(apps[i].id);
        if (!fa || version_cmp(fa->version, apps[i].version) <= 0) continue;
        char id[64];
        snprintf(id, sizeof id, "%s", apps[i].id);
        n++;
        if (cmd_install(id, 1) < 0) { fprintf(stderr, "t48-app: %s: %s\n", id, errtxt); failed++; }
        manifests_scan(opt.apps);
        i = -1;   /* the list was read again: start over; what is now current is skipped */
        if (failed) break;
    }
    if (!n && !machine) printf("everything is up to date\n");
    if (failed) { fail("an update failed"); return -1; }
    return 0;
}

static int cmd_remove(const char *id)
{
    char p[600];
    const struct manifest *m = installed_find(id);
    if (!manifest_id_ok(id) || !m) { fail("%s is not installed", id); return -1; }
    if (!ours(m)) { fail("%s is part of the phone's firmware and cannot be removed", m->name); return -1; }
    snprintf(p, sizeof p, "%s/%s", apps_dir, id);
    rm_rf(p);
    if (access(p, F_OK) == 0) { fail("could not remove %s", id); return -1; }
    /* and what it kept, and its settings */
    snprintf(p, sizeof p, "%s/%s", opt.data, id);
    rm_rf(p);
    snprintf(p, sizeof p, "%s/apps/%s.conf", opt.config, id);
    unlink(p);
    sync();
    if (machine) printf("done %s -\n", id);
    else printf("removed %s\n", id);
    return 0;
}

static void list_line(const char *id, const char *name, const char *have, const char *offered, long pkg_bytes, long disk_bytes,
                      const char *state, const char *perms, const char *description)
{
    if (machine) {
        printf("app\t%s\t%s\t%s\t%s\t%ld\t%ld\t%s\t%s\t%s\n", id, name, have, offered, pkg_bytes, disk_bytes, state,
               perms[0] ? perms : "-", description[0] ? description : "-");
    } else {
        char size[24] = "";
        long b = disk_bytes ? disk_bytes : pkg_bytes;
        if (b) snprintf(size, sizeof size, "%ld KB", (b + 1023) / 1024);
        printf("%-14s %-10s %-10s %8s  %-11s %s\n", id, have, offered, size, state, name);
    }
}

static int cmd_list(void)
{
    int feed_ok = load_feed() == 0;
    if (!machine) printf("%-14s %-10s %-10s %8s  %-11s %s\n", "ID", "INSTALLED", "IN FEED", "SIZE", "", "NAME");
    for (int i = 0; i < napps; i++) {
        const struct manifest *m = &apps[i];
        const struct feed_app *fa = feed_ok ? feed_find(m->id) : NULL;
        const char *state = !ours(m) ? "builtin" : "installed";
        if (fa && version_cmp(fa->version, m->version) > 0) state = fa->api > T48_API_LEVEL ? "unsupported" : "update";
        list_line(m->id, m->name, m->version[0] ? m->version : "-", fa ? fa->version : "-", fa ? fa->size : 0, dir_size(m->dir),
                  state, m->perms, m->description[0] ? m->description : fa ? fa->description : "");
    }
    for (int i = 0; i < nfeed; i++) {
        const struct feed_app *fa = &feed[i];
        if (installed_find(fa->id)) continue;
        list_line(fa->id, fa->name, "-", fa->version[0] ? fa->version : "-", fa->size, 0,
                  fa->api > T48_API_LEVEL ? "unsupported" : "available", fa->perms, fa->description);
    }
    if (!feed_ok) {
        if (machine) printf("feederror %s\n", errtxt);
        else fprintf(stderr, "t48-app: the feed could not be read: %s\n", errtxt);
    }
    fflush(stdout);
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
        "t48-app [options] list                 installed apps and what the feed offers\n"
        "t48-app [options] install ID | FILE    from the feed, or a .t48app file copied to the phone\n"
        "t48-app [options] update [ID]          one app, or every app the feed has a newer version of\n"
        "t48-app [options] remove ID            the app, what it kept and its settings\n"
        "  --feed URL     the feed's index.json (default: \"url\" in %s/feed, else\n"
        "                 %s)\n"
        "  --apps DIRS    installed apps; new ones go in the first folder (%s)\n"
        "  --config DIR   settings (%s)      --data DIR   apps' own folders (%s)\n"
        "  --machine      output for the shell's Apps screen\n",
        opt.config, pkg_default_feed(), opt.apps, opt.config, opt.data);
    return 2;
}

int pkg_main(int argc, char **argv)
{
    const char *feed_opt = NULL;
    int i = 1, r;

    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--machine")) { machine = 1; continue; }
        const char **dst = !strcmp(argv[i], "--feed") ? &feed_opt : !strcmp(argv[i], "--apps") ? &opt.apps :
                           !strcmp(argv[i], "--config") ? &opt.config : !strcmp(argv[i], "--data") ? &opt.data : NULL;
        if (!dst || i + 1 >= argc) return usage();
        *dst = argv[++i];
    }
    if (i >= argc) return usage();
    const char *cmd = argv[i], *arg = i + 1 < argc ? argv[i + 1] : NULL;

    snprintf(apps_dir, sizeof apps_dir, "%.*s", (int)strcspn(opt.apps, ":"), opt.apps);
    snprintf(feed_url, sizeof feed_url, "%s", feed_opt ? feed_opt : cfg_str("feed", "url", pkg_default_feed()));
    if (!feed_url[0]) snprintf(feed_url, sizeof feed_url, "%s", pkg_default_feed());
    signal(SIGPIPE, SIG_IGN);

    /* One at a time: a second copy waits for the first. The lock is a file
     * holding the owner's process number (the phone's kernel is built without
     * flock); one left by a process that has gone is taken over. */
    char lock[340], pidtxt[24];
    snprintf(lock, sizeof lock, "%s/", apps_dir);
    mkdirs(lock);
    snprintf(lock, sizeof lock, "%s/.lock", apps_dir);
    for (int tries = 0;; tries++) {
        int lfd = open(lock, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (lfd >= 0) {
            int n = snprintf(pidtxt, sizeof pidtxt, "%d\n", (int)getpid());
            if (write(lfd, pidtxt, (size_t)n) < 0) {}
            close(lfd);
            break;
        }
        if (errno != EEXIST) {
            fprintf(stderr, "t48-app: cannot use %s: %s\n", apps_dir, strerror(errno));
            if (machine) printf("error cannot write to %s\n", apps_dir);
            return 1;
        }
        FILE *f = fopen(lock, "r");
        int owner = 0;
        if (f) { if (fscanf(f, "%d", &owner) != 1) owner = 0; fclose(f); }
        if ((owner > 0 && kill(owner, 0) < 0 && errno == ESRCH) || (owner <= 0 && tries > 10)) unlink(lock);
        else usleep(200000);
    }
    clean_leftovers();
    manifests_scan(opt.apps);

    if (!strcmp(cmd, "list") && !arg) r = cmd_list();
    else if (!strcmp(cmd, "install") && arg) r = cmd_install(arg, 0);
    else if (!strcmp(cmd, "update") && arg) r = installed_find(arg) ? cmd_install(arg, 1) : (fail("%s is not installed", arg), -1);
    else if (!strcmp(cmd, "update")) r = cmd_update_all();
    else if (!strcmp(cmd, "remove") && arg) r = cmd_remove(arg);
    else { unlink(lock); return usage(); }

    clean_leftovers();
    unlink(lock);
    if (r < 0) {
        if (machine) printf("error %s\n", errtxt);
        else fprintf(stderr, "t48-app: %s\n", errtxt);
        return 1;
    }
    return 0;
}
