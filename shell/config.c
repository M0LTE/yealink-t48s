// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * The settings store (issue #23): plain text files of key=value lines under
 * /data/config, one file per subject (sip, network, system, audio, access,
 * screen, feed, and apps/<id> for each app's own settings). Lines starting
 * with # are comments and are kept when a value is changed. The last line for
 * a key is the one that counts.
 *
 * Everything goes through two calls, load a whole file and save a whole
 * file, so the place the files live can change (RAM today, flash with issue
 * #29) without touching anything above. Saving is atomic: write a new file
 * beside the old one, sync it, rename it over, sync the folder. A power cut
 * leaves either the old file or the new one, never half of each.
 *
 * Files are made readable by root only: one of them holds the SIP password,
 * and apps run as other users.
 *
 * Also the command line: t48-config get | set | unset | list.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "shell.h"

#define CFG_MAX 16384   /* a settings file bigger than this is not one of ours */

/* ---------- where the files live: the part to swap for another kind of storage ---------- */

struct cfg_storage {
    char *(*load)(const char *name);                   /* malloc'd text, or NULL if there is none */
    int (*save)(const char *name, const char *text);   /* 0, or -1 with errno */
};

static void file_path(const char *name, char *out, size_t n)
{
    int k = snprintf(out, n, "%s/%s", opt.config, name);
    /* app settings keep the name the shell has always read: apps/<id>.conf */
    if (!strncmp(name, "apps/", 5) && k > 0 && (size_t)k < n) snprintf(out + k, n - (size_t)k, ".conf");
}

static char *file_load(const char *name)
{
    char path[400];
    file_path(name, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *t = malloc(CFG_MAX + 1);
    size_t n = t ? fread(t, 1, CFG_MAX, f) : 0;
    fclose(f);
    if (t) t[n] = 0;
    return t;
}

static void sync_dir(const char *path)
{
    char dir[400];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

static int file_save(const char *name, const char *text)
{
    char path[400], tmp[410], dir[400];
    size_t len = strlen(text);

    file_path(name, path, sizeof path);
    /* the folders above it, the apps/ one included */
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (fchmod(fd, 0600) < 0 || write(fd, text, len) != (ssize_t)len || fsync(fd) < 0) {
        int e = errno;
        close(fd);
        unlink(tmp);
        errno = e;
        return -1;
    }
    close(fd);
    if (rename(tmp, path) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    sync_dir(path);
    return 0;
}

static const struct cfg_storage files = {file_load, file_save};
static const struct cfg_storage *storage = &files;

/* ---------- names ---------- */

static int word_ok(const char *s, const char *extra)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr(extra, *s)))
            return 0;
    return 1;
}

/* a file is one word, or apps/<id> */
static int name_ok(const char *name)
{
    if (!strncmp(name, "apps/", 5)) name += 5;
    return word_ok(name, "-._") && name[0] != '.';
}

/* a whole file, for the web page's backup and restore (webconfig/): malloc'd text or
 * NULL; and 0, or -1 with errno */
char *cfg_load_text(const char *file) { return name_ok(file) ? storage->load(file) : NULL; }

int cfg_save_text(const char *file, const char *text)
{
    if (!name_ok(file) || strlen(text) > CFG_MAX) { errno = EINVAL; return -1; }
    return storage->save(file, text);
}

/* ---------- get and set ---------- */

/* the line for key: where it starts and where its value starts; the last one in the text */
static const char *find(const char *text, const char *key, const char **line_start)
{
    size_t kl = strlen(key);
    const char *value = NULL;
    for (const char *p = text; *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            value = p + kl + 1;
            if (line_start) *line_start = p;
        }
        p += strcspn(p, "\n");
        if (*p) p++;
    }
    return value;
}

int cfg_get(const char *file, const char *key, char *out, size_t n)
{
    if (n) out[0] = 0;
    if (!name_ok(file) || !word_ok(key, "-._")) return 0;
    char *text = storage->load(file);
    if (!text) return 0;
    const char *v = find(text, key, NULL);
    if (v) snprintf(out, n, "%.*s", (int)strcspn(v, "\r\n"), v);
    free(text);
    return v != NULL;
}

const char *cfg_str(const char *file, const char *key, const char *def)
{
    static char b[4][256];
    static int at;
    char *s = b[at = (at + 1) % 4];
    if (!cfg_get(file, key, s, sizeof b[0])) snprintf(s, sizeof b[0], "%s", def ? def : "");
    return s;
}

/* value NULL takes the key out. Never logs the value: it may be a password. */
int cfg_set(const char *file, const char *key, const char *value)
{
    if (!name_ok(file) || !word_ok(key, "-._")) { errno = EINVAL; return -1; }
    if (value)
        for (const char *p = value; *p; p++)
            if ((unsigned char)*p < 32 || *p == 127) { errno = EINVAL; return -1; }

    char *old = storage->load(file);
    const char *text = old ? old : "";
    size_t kl = strlen(key), room = strlen(text) + kl + (value ? strlen(value) : 0) + 8;
    char *out = malloc(room);
    if (!out) { free(old); return -1; }
    size_t o = 0;
    int placed = 0;
    for (const char *p = text; *p;) {
        size_t len = strcspn(p, "\n");
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            /* the first line for the key takes the new value; any others go */
            if (value && !placed) o += (size_t)sprintf(out + o, "%s=%s\n", key, value);
            placed = 1;
        } else {
            memcpy(out + o, p, len);
            o += len;
            out[o++] = '\n';
        }
        p += len;
        if (*p) p++;
    }
    if (value && !placed) o += (size_t)sprintf(out + o, "%s=%s\n", key, value);
    out[o] = 0;
    int r = strcmp(out, text) ? storage->save(file, out) : 0;
    free(out);
    free(old);
    return r;
}

/* ---------- t48-config ---------- */

/* a key whose value is never shown */
int cfg_secret(const char *key) { return strstr(key, "password") || strstr(key, "secret") || strstr(key, "token"); }

/* an account's server host or outbound proxy, as t48-sip takes it: a name or
 * address with :port after it if not the usual one, an IPv6 address in
 * brackets. The proxy may have sip: or sips: in front, in any case. */
int sip_host_ok(const char *h, int scheme)
{
    if (!strncasecmp(h, "sip:", 4) || !strncasecmp(h, "sips:", 5)) {
        if (!scheme) return 0;
        h = strchr(h, ':') + 1;
        if (!*h) return 0;
    }
    if (!*h) return 1;
    const char *port;
    if (*h == '[') {
        const char *end = strchr(h, ']');
        if (!end || end == h + 1) return 0;
        port = end + 1;
        if (!*port) return 1;
        if (*port != ':') return 0;
    } else {
        port = strchr(h, ':');
        if (!port) return 1;
        if (strchr(port + 1, ':') || port == h) return 0;      /* bare IPv6, or no host */
    }
    port++;
    size_t n = strlen(port);
    if (!n || n > 5 || strspn(port, "0123456789") != n) return 0;
    long v = atol(port);
    return v >= 1 && v <= 65535;
}

static int list_file(const char *name, int with_name)
{
    char *text = storage->load(name);
    if (!text) return 0;
    for (char *p = text; *p;) {
        size_t len = strcspn(p, "\n");
        char *next = p + len + (p[len] == '\n');
        p[len] = 0;
        p[strcspn(p, "\r")] = 0;
        char *eq = strchr(p, '=');
        if (*p != '#' && eq) {
            *eq = 0;
            if (with_name) printf("%s ", name);
            printf("%s=%s\n", p, cfg_secret(p) ? (eq[1] ? "(set)" : "") : eq + 1);
        }
        p = next;
    }
    free(text);
    return 1;
}

int config_main(int argc, char **argv)
{
    static const char *const all[] = {"sip", "network", "system", "audio", "screen", "access", "feed"};
    int i = 1;
    char v[256];

    if (i + 1 < argc && !strcmp(argv[i], "--config")) { opt.config = argv[i + 1]; i += 2; }
    const char *cmd = i < argc ? argv[i] : "";
    int n = argc - i - 1;
    char **a = argv + i + 1;

    if (!strcmp(cmd, "get") && n == 2) {
        if (!cfg_get(a[0], a[1], v, sizeof v)) return 1;
        puts(v);
        return 0;
    }
    if ((!strcmp(cmd, "set") && n == 3) || (!strcmp(cmd, "unset") && n == 2)) {
        if (cfg_set(a[0], a[1], n == 3 ? a[2] : NULL) < 0) {
            fprintf(stderr, "t48-config: cannot set %s %s: %s\n", a[0], a[1],
                    errno == EINVAL ? "names are letters, digits, - . _ and values are one line" : strerror(errno));
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "tz") && n == 0) { puts(settings_tz()); return 0; }
    if (!strcmp(cmd, "list") && n == 1) return name_ok(a[0]) && list_file(a[0], 0) ? 0 : 1;
    if (!strcmp(cmd, "list") && n == 0) {
        for (unsigned k = 0; k < sizeof all / sizeof all[0]; k++) list_file(all[k], 1);
        return 0;
    }
    fprintf(stderr,
        "t48-config [--config DIR] get FILE KEY | set FILE KEY VALUE | unset FILE KEY | list [FILE] | tz\n"
        "The phone's settings: key=value text files in %s.\n"
        "Files: sip, network, system, audio, screen, access, feed, and apps/ID for an app's own.\n"
        "list shows passwords only as (set). tz prints the time zone as a TZ string.\n"
        "Most changes take effect after: t48-apply FILE\n", opt.config);
    return 2;
}
