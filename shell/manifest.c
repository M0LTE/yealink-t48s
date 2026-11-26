// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: find installed apps and read their manifest.json files, and count
 * the ones the feeds have newer (issue 80). Categories: issue 30, docs/app-protocol.md.
 * The list of feeds and the feed each installed app came from: issue 81, docs/apps.md. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "shell.h"

struct manifest apps[MAX_APPS];
int napps;
int manifest_scans;   /* how many times the list has been read: a count made from it is stale when this moves */

int manifest_id_ok(const char *id)
{
    if (!*id) return 0;
    for (; *id; id++)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= '0' && *id <= '9') || *id == '-' || *id == '.' || *id == '_'))
            return 0;
    return 1;
}

const char *const manifest_categories[MANIFEST_CATEGORIES] = {"General", "Games", "Radio", "Tools", "Media"};

/* An app without a category, or with one this firmware does not know (a newer
 * feed's, say), is General: it is still listed, just not in a group of its own. */
int manifest_category(const char *name)
{
    for (int i = 0; i < MANIFEST_CATEGORIES; i++)
        if (!strcasecmp(name, manifest_categories[i])) return i;
    return 0;
}

/* does the manifest ask for this permission? */
int manifest_may(const struct manifest *m, const char *perm)
{
    size_t n = strlen(perm);
    for (const char *p = m->perms; (p = strstr(p, perm)); p += n)
        if ((p == m->perms || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
    return 0;
}

void manifest_free(struct manifest *m)
{
    free(m->icon.px);
    for (int k = 0; k < m->nsettings; k++) { free(m->settings[k]); free(m->setting_label[k]); }
    memset(m, 0, sizeof *m);
}

/* Read the manifest of the app folder dir. 0, or -1 with the reason in err. */
int manifest_read(const char *dir, struct manifest *m, char *err, size_t errn)
{
    char path[400], *txt;
    FILE *f;
    long n;

    memset(m, 0, sizeof *m);
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "no manifest.json"); return -1; }
    txt = malloc(65536);
    n = txt ? (long)fread(txt, 1, 65535, f) : 0;
    fclose(f);
    if (n <= 0) { free(txt); snprintf(err, errn, "manifest.json is empty"); return -1; }
    txt[n] = 0;
    const char *e = txt + n;

    /* a full path: the app is started from inside its folder */
    char *full = realpath(dir, NULL);
    snprintf(m->dir, sizeof m->dir, "%s", full ? full : dir);
    free(full);
    double api = 0;
    if (!t48_json_str(t48_json_get(txt, e, "id"), e, m->id, sizeof m->id) || !manifest_id_ok(m->id) ||
        !t48_json_str(t48_json_get(txt, e, "exec"), e, m->exec, sizeof m->exec) ||
        m->exec[0] == '/' || strstr(m->exec, "..") ||
        !t48_json_num(t48_json_get(txt, e, "api"), e, &api)) {
        snprintf(err, errn, "manifest needs \"id\" (a-z, 0-9, - . _), \"exec\" (inside the app's folder) and \"api\"");
        free(txt);
        return -1;
    }
    m->api = (int)api;
    if (m->api < 1 || m->api > T48_API_LEVEL) {
        snprintf(err, errn, "needs app API %d, this phone has %d", m->api, T48_API_LEVEL);
        free(txt);
        return -1;
    }
    if (!t48_json_str(t48_json_get(txt, e, "name"), e, m->name, sizeof m->name) || !m->name[0])
        memcpy(m->name, m->id, sizeof m->name);
    t48_json_str(t48_json_get(txt, e, "version"), e, m->version, sizeof m->version);
    t48_json_str(t48_json_get(txt, e, "description"), e, m->description, sizeof m->description);
    t48_json_str(t48_json_get(txt, e, "category"), e, m->category, sizeof m->category);
    /* "screen": "any": the app takes whatever size it is told. Without it, it is promised 800x480. */
    char screen[16];
    m->any_screen = t48_json_str(t48_json_get(txt, e, "screen"), e, screen, sizeof screen) && !strcmp(screen, "any");

    for (const char *v = t48_json_first(t48_json_get(txt, e, "permissions"), e); v; v = t48_json_next(v, e)) {
        char p[32];
        size_t at = strlen(m->perms);
        if (t48_json_str(v, e, p, sizeof p) && at + strlen(p) + 2 < sizeof m->perms) {
            if (at) strcat(m->perms, " ");
            strcat(m->perms, p);
        }
    }
    for (const char *v = t48_json_first(t48_json_get(txt, e, "settings"), e); v && m->nsettings < MAX_SETTINGS;
         v = t48_json_next(v, e)) {
        char key[48], def[160] = "", kv[212], label[96] = "";
        const char *d = t48_json_get(v, e, "default");
        double num;
        if (!t48_json_str(t48_json_get(v, e, "key"), e, key, sizeof key) || !key[0] || strchr(key, '=')) continue;
        if (!t48_json_str(d, e, def, sizeof def)) {
            if (t48_json_num(d, e, &num)) snprintf(def, sizeof def, "%g", num);
            else if (d) snprintf(def, sizeof def, "%s", t48_json_true(d, e) ? "true" : "false");
        }
        snprintf(kv, sizeof kv, "%s=%s", key, def);
        if (!t48_json_str(t48_json_get(v, e, "label"), e, label, sizeof label) || !label[0])
            snprintf(label, sizeof label, "%s", key);
        if (!t48_json_str(t48_json_get(v, e, "type"), e, m->setting_type[m->nsettings], sizeof m->setting_type[0]))
            strcpy(m->setting_type[m->nsettings], "text");
        m->setting_label[m->nsettings] = strdup(label);
        m->settings[m->nsettings++] = strdup(kv);
    }

    char icon[128];
    if (t48_json_str(t48_json_get(txt, e, "icon"), e, icon, sizeof icon) && icon[0] && icon[0] != '/' &&
        !strstr(icon, "..")) {
        snprintf(path, sizeof path, "%s/%s", dir, icon);
        if (t48_load_ppm(path, &m->icon) < 0 || m->icon.w > 96 || m->icon.h > 96) {
            logmsg("%s: icon must be a binary PPM up to 96x96; using a letter instead", path);
            free(m->icon.px);
            m->icon.px = NULL;
        }
    }
    free(txt);
    return 0;
}

static void read_manifest(const char *dir)
{
    char err[200], path[400];
    struct manifest *m = &apps[napps];

    snprintf(path, sizeof path, "%s/manifest.json", dir);
    if (access(path, F_OK) < 0) return;
    if (manifest_read(dir, m, err, sizeof err) < 0) {
        logmsg("%s: %s; skipped", dir, err);
        manifest_free(m);
        return;
    }
    for (int i = 0; i < napps; i++)
        if (!strcmp(apps[i].id, m->id)) { manifest_free(m); return; }  /* an earlier folder wins */
    napps++;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const struct manifest *)a)->name, ((const struct manifest *)b)->name);
}

void manifests_scan(const char *dirs)
{
    for (int i = 0; i < napps; i++) manifest_free(&apps[i]);
    napps = 0;
    manifest_scans++;

    char *list = strdup(dirs), *save = NULL;
    for (char *d = strtok_r(list, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        DIR *dp = opendir(d);
        struct dirent *de;
        if (!dp) continue;
        while ((de = readdir(dp)) && napps < MAX_APPS) {
            char path[300];
            if (de->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s", d, de->d_name);
            read_manifest(path);
        }
        closedir(dp);
    }
    free(list);
    qsort(apps, (size_t)napps, sizeof apps[0], by_name);
    logmsg("%d app%s installed", napps, napps == 1 ? "" : "s");
}

/* compare versions number by number: 0.10.0 is newer than 0.9.1 */
int manifest_version_cmp(const char *a, const char *b)
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

/* ---------- feeds (issue 81, docs/apps.md#feeds) ---------- */

/* The official feed, always first. T48_OFFICIAL_FEED stands in for it in the tests,
 * which never reach the internet. */
const char *feeds_official(void)
{
    const char *e = getenv("T48_OFFICIAL_FEED");
    return e && e[0] ? e : T48_DEFAULT_FEED;
}

int feeds_url_ok(const char *url)
{
    if (strncmp(url, "https://", 8) && strncmp(url, "file://", 7)) return 0;   /* file:// for the tests */
    if (strlen(url) > 200 || !url[strncmp(url, "https://", 8) ? 7 : 8]) return 0;
    for (const char *p = url; *p; p++)
        if ((unsigned char)*p <= 32 || *p == 127) return 0;
    return 1;
}

/* The official feed, then the added ones in the order they were added: "url" (the one
 * feed URL of firmware before issue 81, which replaced the official feed; now just the
 * first added, and left where it is so that older firmware still finds it), then url1
 * to url7. Empty, repeated and unusable entries are passed over. */
int feeds_list(struct t48_feed out[T48_FEEDS])
{
    int n = 0;
    snprintf(out[n].url, sizeof out[0].url, "%s", feeds_official());
    out[n++].key[0] = 0;
    for (int k = 0; k < T48_FEEDS && n < T48_FEEDS; k++) {
        char key[8], url[256];
        if (k) snprintf(key, sizeof key, "url%d", k);
        else snprintf(key, sizeof key, "url");
        if (!cfg_get("feed", key, url, sizeof url) || !feeds_url_ok(url)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) dup |= !strcmp(out[i].url, url);
        if (dup) continue;
        snprintf(out[n].url, sizeof out[0].url, "%s", url);
        snprintf(out[n++].key, sizeof out[0].key, "%s", key);
    }
    return n;
}

/* the host, or for file:// the folder the index is in */
static void feed_host(const char *url, char *out, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    if (!strncmp(url, "file://", 7)) {
        const char *end = strrchr(p, '/'), *b = end;
        while (b && b > p && b[-1] != '/') b--;
        snprintf(out, n, "%.*s", end && b ? (int)(end - b) : 0, b ? b : "");
        if (!out[0]) snprintf(out, n, "file");
        return;
    }
    size_t len = strcspn(p, "/?#");
    const char *at = memchr(p, '@', len);   /* user@host: the host */
    if (at) { len -= (size_t)(at + 1 - p); p = at + 1; }
    snprintf(out, n, "%.*s", (int)len, p);
}

/* What the owner sees as an app's source: Official, or the feed's host. Never anything
 * the feed says about itself, so another feed cannot call itself Official. Two feeds on
 * one host are told apart by the first part of the path. */
void feeds_name(const struct t48_feed *all, int n, const char *url, char *out, size_t on)
{
    char host[128], other[128];
    if (!strcmp(url, feeds_official())) { snprintf(out, on, "Official"); return; }
    feed_host(url, host, sizeof host);
    int shared = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(all[i].url, url) || !strcmp(all[i].url, feeds_official())) continue;
        feed_host(all[i].url, other, sizeof other);
        shared |= !strcmp(host, other);
    }
    const char *path = strstr(url, "://");
    path = path ? path + 3 + strcspn(path + 3, "/") : "";
    if (shared && *path == '/' && strncmp(url, "file://", 7)) snprintf(out, on, "%s/%.*s", host, (int)strcspn(path + 1, "/?#"), path + 1);
    else snprintf(out, on, "%s", host);
    for (char *p = out; *p; p++)
        if ((unsigned char)*p <= 32 || *p == 127 || *p == ',') *p = '_';   /* one word: lists are split on spaces and commas */
}

/* the added feeds as they are, then written back as url1, url2... in that order */
static int feeds_write(struct t48_feed *f, int n)
{
    int bad = 0;
    for (int k = 1; k < T48_FEEDS; k++) {
        char key[8];
        snprintf(key, sizeof key, "url%d", k);
        bad |= cfg_set("feed", key, k - 1 < n ? f[k - 1].url : NULL) < 0;
    }
    return bad ? -1 : 0;
}

static int feeds_added(struct t48_feed out[T48_FEEDS])
{
    struct t48_feed all[T48_FEEDS];
    int n = feeds_list(all), k = 0;
    for (int i = 1; i < n; i++)
        if (strcmp(all[i].key, "url")) out[k++] = all[i];   /* url stays where it is */
    return k;
}

/* add a feed at the end: 0, or -1 and errno EINVAL (not a feed URL), EEXIST, ENOSPC (T48_FEEDS reached) */
int feeds_add(const char *url)
{
    struct t48_feed all[T48_FEEDS], added[T48_FEEDS];
    if (!feeds_url_ok(url)) { errno = EINVAL; return -1; }
    int n = feeds_list(all);
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i].url, url)) { errno = EEXIST; return -1; }
    if (n >= T48_FEEDS) { errno = ENOSPC; return -1; }
    int k = feeds_added(added);
    snprintf(added[k++].url, sizeof added[0].url, "%s", url);
    return feeds_write(added, k);
}

/* take an added feed out: 0, or -1 and errno ENOENT. The official feed stays. */
int feeds_remove(const char *url)
{
    struct t48_feed all[T48_FEEDS], added[T48_FEEDS];
    int n = feeds_list(all), found = 0;
    for (int i = 1; i < n; i++)
        if (!strcmp(all[i].url, url)) {
            found = 1;
            if (!strcmp(all[i].key, "url")) return cfg_set("feed", "url", NULL);
        }
    if (!found) { errno = ENOENT; return -1; }
    int k = feeds_added(added), m = 0;
    for (int i = 0; i < k; i++)
        if (strcmp(added[i].url, url)) added[m++] = added[i];
    return feeds_write(added, m);
}

/* The feed an installed app came from, as t48-app wrote it down when it installed it
 * (T48_SOURCES in the state folder, a line "id url" each): 1, or 0 for an app with
 * none (built in, copied by hand, or installed before issue 81). */
int app_source(const char *state, const char *id, char *url, size_t n)
{
    char path[400], line[600];
    size_t il = strlen(id);
    int found = 0;
    snprintf(path, sizeof path, "%s/" T48_SOURCES, state);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, id, il) && line[il] == ' ') { snprintf(url, n, "%s", line + il + 1); found = 1; }
    }
    fclose(f);
    return found;
}

/* what the offers file says feed `url` has of app `id`: its version and API level, 1 if any */
static int offered_in(const char *text, const char *url, const char *id, char *version, size_t vn, int *api)
{
    int in = 0;
    for (const char *p = text; *p; p += strcspn(p, "\n"), p += *p == '\n') {
        int len = (int)strcspn(p, "\n");
        char line[600], one[64], ver[64];
        snprintf(line, sizeof line, "%.*s", len, p);
        if (!strncmp(line, "feed=", 5)) in = !strcmp(line + 5, url);
        else if (in && sscanf(line, "app %63s %63s %d", one, ver, api) == 3 && !strcmp(one, id)) {
            snprintf(version, vn, "%s", ver);
            return 1;
        }
    }
    return 0;
}

/* How many of the installed apps have a newer version, that this shell can run, in the
 * feed each follows, from what t48-app last read of the feeds (T48_OFFERS in the state
 * folder). An app follows the feed it was installed from (app_source), else the first
 * of these feeds that has it; a feed no longer in the list counts for nothing. Counted
 * against what is installed now, so an update or a removal counts at once. */
int manifest_updates(const char *state, const struct t48_feed *feeds, int nfeeds)
{
    char path[400];
    int n = 0;
    snprintf(path, sizeof path, "%s/" T48_OFFERS, state);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char *text = malloc(256 * 1024 + 1);
    size_t len = text ? fread(text, 1, 256 * 1024, f) : 0;
    fclose(f);
    if (!text) return 0;
    text[len] = 0;
    for (int i = 0; i < napps; i++) {
        char url[256], version[64];
        int api = 0, listed = 0, has = 0;
        if (app_source(state, apps[i].id, url, sizeof url)) {
            for (int k = 0; k < nfeeds; k++) listed |= !strcmp(feeds[k].url, url);
            has = listed && offered_in(text, url, apps[i].id, version, sizeof version, &api);
        } else {
            for (int k = 0; k < nfeeds && !has; k++) has = offered_in(text, feeds[k].url, apps[i].id, version, sizeof version, &api);
        }
        if (has && api <= T48_API_LEVEL && manifest_version_cmp(version, apps[i].version) > 0) n++;
    }
    free(text);
    return n;
}
