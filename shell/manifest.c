// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: find installed apps and read their manifest.json files. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "shell.h"

struct manifest apps[MAX_APPS];
int napps;

int manifest_id_ok(const char *id)
{
    if (!*id) return 0;
    for (; *id; id++)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= '0' && *id <= '9') || *id == '-' || *id == '.' || *id == '_'))
            return 0;
    return 1;
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
