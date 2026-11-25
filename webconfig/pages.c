// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-web: the pages (issue #37, docs/web.md). Each page is drawn from the
 * settings store the phone's own screens use (shell/config.c, linked in), and
 * each form writes to it the same way, so the two stay in step: the shell
 * notices a changed file within a second (shell/watch.c).
 *
 * Passwords only ever go one way. A password field is always empty; leaving it
 * empty keeps the one that is set. Nothing typed into any form is logged.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "web.h"
#include "zones.h"

#ifndef T48_VERSION
#define T48_VERSION "development build"
#endif

#define MAX_ACCTS 6
#define MAX_CONTACTS 200
#define MAX_KEYS 16

/* ---------- small things ---------- */

/* a value from the store, good for long enough to build one line of a page
 * from several of them (cfg_str keeps only four) */
static const char *val(const char *file, const char *key, const char *def)
{
    static char b[24][256];
    static int at;
    char *s = b[at = (at + 1) % 24];
    if (!cfg_get(file, key, s, sizeof b[0])) snprintf(s, sizeof b[0], "%s", def ? def : "");
    return s;
}

static const char *sip_file(int n)   /* 1 to 6 */
{
    static char f[16][8];
    static int at;
    char *o = f[at = (at + 1) % 16];
    if (n <= 1) return "sip";
    snprintf(o, sizeof f[0], "sip%d", n);
    return o;
}

/* one line of text from a form: control characters become spaces, none at the
 * ends. 0 if it does not fit. */
static int line(const struct req *r, const char *name, char *out, size_t n)
{
    const char *v = form(r, name);
    while (*v && (unsigned char)*v <= 32) v++;
    size_t len = strlen(v);
    while (len && (unsigned char)v[len - 1] <= 32) len--;
    if (len >= n) { out[0] = 0; return 0; }
    memcpy(out, v, len);
    out[len] = 0;
    for (char *p = out; *p; p++) if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
    return 1;
}

#define UNSURE "Tick Confirm first."
static int sure(const struct req *r) { return !strcmp(form(r, "sure"), "yes"); }

/* the tick box of a form that destroys something; the button beside it names the action */
static void sure_box(void)
{
    H("<label class='check'><input type='checkbox' name='sure' value='yes' required> Confirm</label>");
}

static void field(const char *label, const char *name, const char *value, const char *attrs, const char *hint)
{
    H("<label class='field'><span>%s</span><input name='%s' value='%s' maxlength='200' %r>", label, name, value, attrs ? attrs : "");
    if (hint) H("<small>%s</small>", hint);
    H("</label>");
}

/* the store says no for one reason only that a person can cause */
static int set(const char *file, const char *key, const char *value)
{
    return cfg_set(file, key, value && value[0] ? value : NULL);
}

static void use_phone_time(void)
{
    setenv("TZ", settings_tz(), 1);
    tzset();
}

static int ipv4(const char *s)
{
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

/* ---------- contacts: the shell's file, name|number a line (shell/contacts.c) ---------- */

struct contact { char name[64], number[64]; };
static struct contact cl[MAX_CONTACTS];
static int cn;

static int by_name(const void *a, const void *b)
{
    const struct contact *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->number, y->number);
}

static void contacts_path(char *out, size_t n) { snprintf(out, n, "%s/contacts", opt.state); }

static void contacts_load(void)
{
    char path[300];
    contacts_path(path, sizeof path);
    char *t = load_file(path, 64 * 1024);
    cn = 0;
    for (char *p = t; p && *p && cn < MAX_CONTACTS;) {
        char *nl = strchr(p, '\n'), *bar;
        if (nl) *nl = 0;
        p[strcspn(p, "\r")] = 0;
        if (p[0] != '#' && (bar = strrchr(p, '|')) && bar != p && bar[1]) {
            *bar = 0;
            snprintf(cl[cn].name, sizeof cl[0].name, "%s", p);
            snprintf(cl[cn].number, sizeof cl[0].number, "%s", bar + 1);
            cn++;
        }
        p = nl ? nl + 1 : NULL;
    }
    free(t);
    qsort(cl, (size_t)cn, sizeof cl[0], by_name);
}

static int contacts_save(void)
{
    char path[300], *t = malloc((size_t)cn * 130 + 1);
    size_t o = 0;
    if (!t) return -1;
    t[0] = 0;
    qsort(cl, (size_t)cn, sizeof cl[0], by_name);
    for (int i = 0; i < cn; i++) o += (size_t)sprintf(t + o, "%s|%s\n", cl[i].name, cl[i].number);
    mkdir(opt.state, 0755);
    contacts_path(path, sizeof path);
    int r = save_file(path, t, 0644);
    free(t);
    return r;
}

/* as the phone's own screen stores one. 0: not a contact, or no room, or there already */
static int contact_put(struct contact *c, const char *name, const char *number)
{
    size_t o = 0;
    char num[64];
    while (*name == ' ') name++;
    for (const char *p = number; *p && o < sizeof num - 1; p++)
        if ((unsigned char)*p > 32) num[o++] = *p == '|' ? '-' : *p;   /* spaces in a number are only for reading */
    num[o] = 0;
    if (!name[0] || !num[0]) return 0;
    snprintf(c->name, sizeof c->name, "%s", name);
    for (char *p = c->name; *p; p++) if (*p == '|' || (unsigned char)*p < 32) *p = ' ';
    for (size_t l = strlen(c->name); l && c->name[l - 1] == ' ';) c->name[--l] = 0;
    snprintf(c->number, sizeof c->number, "%s", num);
    return c->name[0] != 0;
}

static int contact_add(const char *name, const char *number)
{
    struct contact c;
    if (cn >= MAX_CONTACTS || !contact_put(&c, name, number)) return 0;
    for (int i = 0; i < cn; i++)
        if (!strcmp(cl[i].name, c.name) && !strcmp(cl[i].number, c.number)) return 0;
    cl[cn++] = c;
    return 1;
}

static int contact_find(const char *name, const char *number)
{
    for (int i = 0; i < cn; i++)
        if (!strcmp(cl[i].name, name) && !strcmp(cl[i].number, number)) return i;
    return -1;
}

static const char *contact_name(const char *number)
{
    for (int i = 0; i < cn; i++)
        if (!strcmp(cl[i].number, number)) return cl[i].name;
    return NULL;
}

/* ---------- overview ---------- */

static int account_used(int n) { return val(sip_file(n), "user", "")[0] && val(sip_file(n), "domain", "")[0]; }

static int account_on(int n)
{
    const char *v = val(sip_file(n), "enabled", "on");
    return account_used(n) && strcmp(v, "off") && strcmp(v, "no") && strcmp(v, "false") && strcmp(v, "0");
}

static int history_count(void)
{
    char path[300];
    int n = 0;
    snprintf(path, sizeof path, "%s/history", opt.state);
    char *t = load_file(path, 64 * 1024);
    for (char *p = t; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (strncmp(p, "seen ", 5) && strchr(p, '|')) n++;
    free(t);
    return n;
}

static int keys_count(void);

static void scan_apps(void)
{
    /* the shell's own reader of manifests: it says how many it found each time */
    log_quiet = 1;
    manifests_scan(opt.apps);
    log_quiet = 0;
}

static int ssh_is_on(void);

void page_overview(struct req *r)
{
    char now[80];
    time_t t = time(NULL);
    struct tm tm;
    int used = 0;

    page_begin(r, "Overview", "/");
    H("<div class='grid'>");

    H("<a class='card tile' href='/accounts'><h2>Accounts</h2>");
    for (int n = 1; n <= MAX_ACCTS; n++) {
        if (!account_used(n)) continue;
        const char *label = val(sip_file(n), "label", "");
        used++;
        H("<p>%s@%s %r%s%r%r</p>", val(sip_file(n), "user", ""), val(sip_file(n), "domain", ""), label[0] ? "<span class='dim'>" : "", label,
          label[0] ? "</span>" : "", account_on(n) ? "" : " <span class='pill'>Off</span>");
    }
    if (!used) H("<p class='dim'>None</p>");
    H("</a>");

    H("<a class='card tile' href='/network'><h2>Network</h2><p>%s</p><p class='dim'>%s</p></a>",
      !strcmp(val("network", "mode", "dhcp"), "static") ? "Static" : "DHCP", web_url());

    use_phone_time();
    localtime_r(&t, &tm);
    strftime(now, sizeof now, "%H:%M, %A %e %B %Y", &tm);
    H("<a class='card tile' href='/time'><h2>Date and time</h2><p>%s</p><p class='dim'>%s</p></a>", now, val("system", "timezone", "UTC"));

    H("<a class='card tile' href='/sound'><h2>Sound and screen</h2><p>Ringer %s of 15</p><p>Brightness %s of 10</p></a>",
      val("audio", "ringer", "13"), val("screen", "brightness", "10"));

    contacts_load();
    H("<a class='card tile' href='/contacts'><h2>Contacts</h2><p>%d</p></a>", cn);
    H("<a class='card tile' href='/history'><h2>Calls</h2><p>%d</p></a>", history_count());

    scan_apps();
    H("<a class='card tile' href='/apps'><h2>Apps</h2><p>%d installed</p></a>", napps);

    int ssh = ssh_is_on();
    H("<a class='card tile' href='/access'><h2>Access</h2><p><span class='pill %r'>SSH %r</span> %d key%r</p><p>%d browser%r paired</p></a>",
      ssh ? "good" : "", ssh ? "on" : "off", keys_count(), keys_count() == 1 ? "" : "s", sessions_count(), sessions_count() == 1 ? "" : "s");

    H("<a class='card tile' href='/backup'><h2>Backup</h2><p>Download or restore</p></a>");
    H("<a class='card tile' href='/update'><h2>Update</h2><p>Check or install</p></a>");
    H("</div>");
    page_end(r);
}

/* ---------- phone accounts ---------- */

static const char *const ringtones[][2] = {{"standard", "Standard"}, {"long", "Long"}, {"chime", "Chime"}, {"pulse", "Pulse"}, {"custom", "Custom"}};
static const char *const fwd_kinds[][2] = {{"always", "Always"}, {"busy", "Busy"}, {"noanswer", "No answer"}};

static int custom_ringtone(int n)
{
    char path[400];
    struct stat st;
    ringtone_path(sip_file(n), path, sizeof path);
    return stat(path, &st) == 0;
}

/* issue #77: the ringtone, a custom sound, and call forwarding, inside an account's form */
static void account_calls(int n)
{
    const char *f = sip_file(n), *rt = val(f, "ringtone", "standard");
    char key[40];
    H("<label class='field'><span>Ringtone</span><select name='ringtone'>");
    for (int i = 0; i < 5; i++)
        if (i < 4 || custom_ringtone(n))
            H("<option value='%s'%r>%s</option>", ringtones[i][0], !strcmp(rt, ringtones[i][0]) ? " selected" : "", ringtones[i][1]);
    H("</select></label></div><h3>Call forwarding</h3><input type='hidden' name='fwd' value='1'><div class='fields'>");
    for (int k = 0; k < 3; k++) {
        char name[40];
        snprintf(key, sizeof key, "forward_%s", fwd_kinds[k][0]);
        snprintf(name, sizeof name, "%s_to", key);
        H("<label class='field'><span><input type='checkbox' name='%s' value='on'%r> %s</span>"
          "<input name='%s' value='%s' maxlength='60' inputmode='tel' autocapitalize='none' spellcheck='false' placeholder='Number'></label>",
          key, !strcmp(val(f, key, "off"), "on") ? " checked" : "", fwd_kinds[k][1], name, val(f, name, ""));
    }
    const char *after = val(f, "forward_noanswer_after", "20");
    H("<label class='field'><span>No answer after</span><select name='forward_noanswer_after'>");
    for (const char *const *t = (const char *const[]){"5", "10", "15", "20", "25", "30", "45", "60", NULL}; *t; t++)
        H("<option value='%s'%r>%s s</option>", *t, !strcmp(after, *t) ? " selected" : "", *t);
    H("</select></label>");
}

/* the custom sound's own form: a WAV to upload, or the one there to remove */
static void account_sound(struct req *r, int n)
{
    form_begin(r, "/accounts/ringtone", "enctype='multipart/form-data' class='sub'");
    H("<input type='hidden' name='n' value='%d'><label class='field'><span>Custom ringtone</span>"
      "<input type='file' name='sound' accept='.wav,audio/wav,audio/x-wav' required><small>WAV, up to 2 MB, 10 s at most</small></label>"
      "<button type='submit'>Upload</button></form>", n);
    if (custom_ringtone(n)) {
        form_begin(r, "/accounts/ringtone-remove", "class='inline'");
        H("<input type='hidden' name='n' value='%d'><button type='submit'>Remove custom ringtone</button></form>", n);
    }
}

static void page_accounts(struct req *r)
{
    int used = 0, def = atoi(val("phone", "default_account", "1"));
    page_begin(r, "Accounts", "/accounts");
    for (int n = 1; n <= MAX_ACCTS; n++) used += account_on(n);
    if (used > 1) {
        H("<div class='card'>");
        form_begin(r, "/accounts/default", "class='row'");
        H("<label class='field'><span>Default account</span><select name='n'>");
        for (int n = 1; n <= MAX_ACCTS; n++)
            if (account_on(n))
                H("<option value='%d'%r>%d: %s@%s</option>", n, n == def ? " selected" : "", n, val(sip_file(n), "user", ""), val(sip_file(n), "domain", ""));
        H("</select></label><button type='submit'>Save</button></form></div>");
    }
    /* issue #77: calls only through the accounts' servers */
    H("<div class='card'>");
    form_begin(r, "/accounts/block", "class='row'");
    H("<label class='check'><input type='checkbox' name='block_direct' value='on'%r> Block direct calls</label>"
      "<button type='submit'>Save</button></form>"
      "<p class='dim'>Only calls through an account's server or outbound proxy ring.</p></div>",
      !strcmp(val("phone", "block_direct", "off"), "on") ? " checked" : "");
    for (int n = 1; n <= MAX_ACCTS; n++) {
        const char *f = sip_file(n);
        int has = val(f, "user", "")[0] || val(f, "domain", "")[0];
        int pw = val(f, "password", "")[0] != 0;
        const char *tr = val(f, "transport", "udp");
        H("<details class='card' id='a%d'%r><summary><h2>Account %d</h2>", n, has || n == 1 ? " open" : "", n);
        if (account_used(n))
            H("<span class='sum'>%s@%s</span><span class='pill %r'>%r</span>", val(f, "user", ""), val(f, "domain", ""), account_on(n) ? "good" : "", account_on(n) ? "On" : "Off");
        else H("<span class='sum dim'>Not set</span>");
        H("</summary>");
        form_begin(r, "/accounts/save", NULL);
        H("<input type='hidden' name='n' value='%d'><div class='fields'>", n);
        field("User", "user", val(f, "user", ""), "autocomplete='off' autocapitalize='none' spellcheck='false'", NULL);
        field("Server host", "domain", val(f, "domain", ""), "autocomplete='off' autocapitalize='none' spellcheck='false' placeholder='host or host:port'", NULL);
        H("<label class='field'><span>Password</span><input type='password' name='password' value='' maxlength='200' autocomplete='new-password' placeholder='%r'></label>", pw ? "Set. Empty keeps it" : "Not set");
        field("Outbound proxy", "outbound_proxy", val(f, "outbound_proxy", ""), "autocapitalize='none' spellcheck='false' placeholder='Optional'", NULL);
        field("Auth user", "auth_user", val(f, "auth_user", ""), "autocomplete='off' autocapitalize='none' spellcheck='false' placeholder='Optional'", NULL);
        field("Display name", "display_name", val(f, "display_name", ""), "placeholder='Optional'", NULL);
        H("<label class='field'><span>Transport</span><select name='transport'>");
        for (const char *const *t = (const char *const[]){"udp", "tcp", "tls", NULL}; *t; t++)
            H("<option value='%s'%r>%r</option>", *t, !strcmp(tr, *t) ? " selected" : "", !strcmp(*t, "udp") ? "UDP" : !strcmp(*t, "tcp") ? "TCP" : "TLS");
        H("</select></label>");
        field("Label", "label", val(f, "label", ""), "placeholder='Optional'", "Name on the phone's screens");
        field("Voicemail", "voicemail", val(f, "voicemail", ""), "inputmode='tel' placeholder='Optional'", "Number the Message key calls");
        account_calls(n);
        H("</div><label class='check'><input type='checkbox' name='enabled' value='on'%r> Enabled</label>"
          "<div class='actions'><button class='primary' type='submit'>Save</button></div></form>",
          strcmp(val(f, "enabled", "on"), "off") ? " checked" : "");
        account_sound(r, n);
        if (has) {
            form_begin(r, "/accounts/remove", "class='danger'");
            H("<input type='hidden' name='n' value='%d'>", n);
            sure_box();
            H("<button type='submit' class='bad'>Remove account %d</button></form>", n);
        }
        H("</details>");
    }
    page_end(r);
}

static void post_account_save(struct req *r)
{
    static const char *const keys[] = {"user", "domain", "outbound_proxy", "auth_user", "display_name", "label", "voicemail"};
    char v[7][201], pw[201], where[24];
    int n = atoi(form(r, "n")), ok = 1;
    const char *tr = form(r, "transport");
    if (n < 1 || n > MAX_ACCTS) { redirect(r, "/accounts"); return; }
    const char *f = sip_file(n);
    snprintf(where, sizeof where, "/accounts#a%d", n);
    for (int i = 0; i < 7; i++) ok &= line(r, keys[i], v[i], sizeof v[0]);
    /* a password is taken as typed, spaces and all, but must be one line */
    snprintf(pw, sizeof pw, "%s", form(r, "password"));
    if (strlen(form(r, "password")) > 200 || strpbrk(pw, "\r\n")) ok = 0;
    if (!ok) { flash(r, 1, "Account %d not saved: a value is too long.", n); redirect(r, where); return; }
    if (!sip_host_ok(v[1], 0) || !sip_host_ok(v[2], 1)) {
        flash(r, 1, "Account %d not saved: %s must be host or host:port, IPv6 in brackets.", n, sip_host_ok(v[1], 0) ? "outbound proxy" : "server host");
        redirect(r, where);
        return;
    }
    if (strcmp(tr, "udp") && strcmp(tr, "tcp") && strcmp(tr, "tls")) tr = "udp";
    /* forwarding, when the form had it: a box not ticked is off. All checked before any is kept */
    char fwd_to[3][64];
    for (int k = 0; k < 3 && form(r, "fwd")[0]; k++) {
        char key[48];
        snprintf(key, sizeof key, "forward_%s_to", fwd_kinds[k][0]);
        const char *why = !line(r, key, fwd_to[k], sizeof fwd_to[k]) || strpbrk(fwd_to[k], " <>\",;") ? "forwarding to %s is not a number or an address" :
                          forward_to_self(fwd_to[k], v[0], v[1]) ? "%s forwarding is to this account" :
                          (snprintf(key, sizeof key, "forward_%s", fwd_kinds[k][0]), !strcmp(form(r, key), "on") && !fwd_to[k][0]) ? "%s forwarding needs a number" : NULL;
        if (why) {
            char text[120];
            snprintf(text, sizeof text, why, fwd_kinds[k][1]);
            flash(r, 1, "Account %d not saved: %s.", n, text);
            redirect(r, where);
            return;
        }
    }
    for (int i = 0; i < 7 && ok; i++) ok = set(f, keys[i], v[i]) == 0;
    if (ok) ok = cfg_set(f, "transport", tr) == 0 && cfg_set(f, "enabled", !strcmp(form(r, "enabled"), "on") ? "on" : "off") == 0;
    if (ok && pw[0]) ok = cfg_set(f, "password", pw) == 0;
    memset(pw, 0, sizeof pw);
    if (ok && form(r, "ringtone")[0]) {
        const char *rt = form(r, "ringtone");
        int known = 0;
        for (int i = 0; i < 5; i++) known |= !strcmp(rt, ringtones[i][0]);
        ok = cfg_set(f, "ringtone", known ? rt : "standard") == 0;
    }
    for (int k = 0; k < 3 && ok && form(r, "fwd")[0]; k++) {
        char key[40], to_key[48];
        snprintf(key, sizeof key, "forward_%s", fwd_kinds[k][0]);
        snprintf(to_key, sizeof to_key, "%s_to", key);
        ok = cfg_set(f, key, !strcmp(form(r, key), "on") ? "on" : "off") == 0 && set(f, to_key, fwd_to[k]) == 0;
    }
    if (ok && form(r, "fwd")[0]) {
        int a = atoi(form(r, "forward_noanswer_after"));
        char v[8];
        snprintf(v, sizeof v, "%d", a >= 5 && a <= 60 ? a : 20);
        ok = cfg_set(f, "forward_noanswer_after", v) == 0;
    }
    if (!ok) { flash(r, 1, "Account %d not saved: %s.", n, errno == EINVAL ? "invalid characters" : strerror(errno)); redirect(r, where); return; }
    logmsg("account %d saved", n);
    run_apply("sip", 0, 0);
    flash(r, 0, "Account %d saved.", n);
    redirect(r, where);
}

static void post_account_remove(struct req *r)
{
    char path[400];
    int n = atoi(form(r, "n"));
    if (n < 1 || n > MAX_ACCTS || !sure(r)) { flash(r, 1, UNSURE); redirect(r, "/accounts"); return; }
    snprintf(path, sizeof path, "%s/%s", opt.config, sip_file(n));
    unlink(path);
    ringtone_path(sip_file(n), path, sizeof path);
    unlink(path);
    logmsg("account %d removed", n);
    run_apply("sip", 0, 0);
    flash(r, 0, "Account %d removed.", n);
    redirect(r, "/accounts");
}

static void post_account_block(struct req *r)
{
    int on = !strcmp(form(r, "block_direct"), "on");
    if (cfg_set("phone", "block_direct", on ? "on" : "off") < 0) {
        flash(r, 1, "Not saved: %s.", strerror(errno));
    } else {
        run_apply("sip", 0, 0);
        flash(r, 0, "Direct calls %s.", on ? "blocked" : "allowed");
    }
    redirect(r, "/accounts");
}

/* a WAV for an account's custom ringtone, made into the phone's format and kept */
static void post_account_ringtone(struct req *r)
{
    char path[400], tmp[410], dir[400], where[24];
    int n = atoi(form(r, "n")), cut = 0;
    size_t len = 0, ns = 0;
    const unsigned char *wav = (const unsigned char *)form_bytes(r, "sound", &len);
    int16_t *pcm = NULL;
    const char *why = "no file";
    if (n < 1 || n > MAX_ACCTS) { redirect(r, "/accounts"); return; }
    snprintf(where, sizeof where, "/accounts#a%d", n);
    if (!wav || !len || wav_to_ringtone(wav, len, &pcm, &ns, &cut, &why) < 0) {
        flash(r, 1, "Ringtone not saved: %s.", why);
        redirect(r, where);
        return;
    }
    ringtone_path(sip_file(n), path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    mkdir(opt.state, 0700);
    mkdir(dir, 0700);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *o = fopen(tmp, "wb");
    int ok = o && fwrite(pcm, 2, ns, o) == ns;
    if (o && fclose(o)) ok = 0;
    free(pcm);
    if (!ok || rename(tmp, path) < 0 || cfg_set(sip_file(n), "ringtone", "custom") < 0) {
        unlink(tmp);
        flash(r, 1, "Ringtone not saved: %s.", strerror(errno));
        redirect(r, where);
        return;
    }
    logmsg("account %d: custom ringtone, %zu samples", n, ns);
    flash(r, 0, cut ? "Ringtone saved, cut to 10 s." : "Ringtone saved.");
    redirect(r, where);
}

static void post_account_ringtone_remove(struct req *r)
{
    char path[400], where[24];
    int n = atoi(form(r, "n"));
    if (n < 1 || n > MAX_ACCTS) { redirect(r, "/accounts"); return; }
    snprintf(where, sizeof where, "/accounts#a%d", n);
    ringtone_path(sip_file(n), path, sizeof path);
    unlink(path);
    if (!strcmp(val(sip_file(n), "ringtone", ""), "custom")) cfg_set(sip_file(n), "ringtone", "standard");
    flash(r, 0, "Custom ringtone removed.");
    redirect(r, where);
}

static void post_account_default(struct req *r)
{
    char v[4];
    int n = atoi(form(r, "n"));
    if (n >= 1 && n <= MAX_ACCTS && account_on(n)) {
        snprintf(v, sizeof v, "%d", n);
        cfg_set("phone", "default_account", v);
        flash(r, 0, "Default account: %d.", n);
    }
    redirect(r, "/accounts");
}

/* ---------- network ---------- */

static void page_network(struct req *r)
{
    int fixed = !strcmp(val("network", "mode", "dhcp"), "static");
    page_begin(r, "Network", "/network");
    H("<div class='card'>");
    form_begin(r, "/network", NULL);
    H("<label class='field'><span>Mode</span><select name='mode'><option value='dhcp'%r>DHCP</option>"
      "<option value='static'%r>Static</option></select></label><div class='fields'>", fixed ? "" : " selected", fixed ? " selected" : "");
    field("Address", "address", val("network", "address", ""), "inputmode='decimal' placeholder='192.168.1.50'", NULL);
    field("Netmask", "netmask", val("network", "netmask", "255.255.255.0"), "inputmode='decimal'", NULL);
    field("Gateway", "gateway", val("network", "gateway", ""), "inputmode='decimal' placeholder='Optional'", NULL);
    field("DNS", "dns", val("network", "dns", ""), "inputmode='decimal' placeholder='Optional'", NULL);
    H("</div><p class='note'>Now at <b>%s</b>. If the address changes, this page moves with it.</p>", web_url());
    H("<div class='actions'><button class='primary' type='submit'>Save</button></div></form></div>");
    page_end(r);
}

static void post_network(struct req *r)
{
    char a[4][64];
    static const char *const keys[] = {"address", "netmask", "gateway", "dns"};
    int fixed = !strcmp(form(r, "mode"), "static"), ok = 1;
    for (int i = 0; i < 4; i++) ok &= line(r, keys[i], a[i], sizeof a[0]);
    if (!a[1][0]) snprintf(a[1], sizeof a[1], "255.255.255.0");
    if (fixed && (!ok || !ipv4(a[0]) || !ipv4(a[1]) || (a[2][0] && !ipv4(a[2])) || (a[3][0] && !ipv4(a[3])))) {
        flash(r, 1, "Not saved: static needs an IPv4 address and netmask. Gateway and DNS, if given, must be IPv4.");
        redirect(r, "/network");
        return;
    }
    /* with DHCP the boxes are kept as they were unless what is in them is good */
    for (int i = 0; i < 4; i++)
        if (fixed || !a[i][0] || ipv4(a[i])) set("network", keys[i], a[i]);
    cfg_set("network", "mode", fixed ? "static" : "dhcp");
    /* in two seconds, so that this answer gets back to the browser first */
    run_apply("network", 0, 2);
    flash(r, 0, "Saved. Applying now.");
    redirect(r, "/network");
}

/* ---------- date and time ---------- */

static void page_time(struct req *r)
{
    char now[80];
    const char *cur = val("system", "timezone", "UTC");
    time_t t = time(NULL);
    struct tm tm;
    int listed = 0;
    use_phone_time();
    localtime_r(&t, &tm);
    strftime(now, sizeof now, "%H:%M, %A %e %B %Y", &tm);
    page_begin(r, "Date and time", "/time");
    H("<div class='card'><p class='big'>%s</p><p class='dim'>Set by NTP</p>", now);
    form_begin(r, "/time", NULL);
    H("<label class='field'><span>Time zone</span><select name='zone'>");
    for (int i = 0; i < NZONES; i++) {
        int here = !strcmp(cur, zones[i].name);
        listed |= here;
        H("<option%r>%s</option>", here ? " selected" : "", zones[i].name);
    }
    H("<option value='other'%r>Custom TZ string</option></select></label>", listed ? "" : " selected");
    field("TZ string", "custom", listed ? "" : cur, "autocapitalize='none' spellcheck='false' placeholder='XYZ-3'",
          "POSIX form, such as CET-1CEST,M3.5.0,M10.5.0/3");
    H("<div class='actions'><button class='primary' type='submit'>Save</button></div></form></div>");
    page_end(r);
}

static void post_time(struct req *r)
{
    char z[64];
    int ok = line(r, strcmp(form(r, "zone"), "other") ? "zone" : "custom", z, sizeof z);
    for (char *p = z; *p; p++) if (*p == ' ') ok = 0;
    if (!ok || !z[0] || cfg_set("system", "timezone", z) < 0) flash(r, 1, "Not saved: choose a zone, or enter a TZ string without spaces.");
    else flash(r, 0, "Time zone: %s.", z);
    redirect(r, "/time");
}

/* ---------- sound and screen ---------- */

static void level(const char *label, const char *name, const char *value, int min, int max, const char *hint)
{
    H("<label class='field level'><span>%s</span><span class='slide'><input type='range' name='%s' min='%d' max='%d' value='%s'><output>%s</output></span>",
      label, name, min, max, value, value);
    if (hint) H("<small>%s</small>", hint);
    H("</label>");
}

static void page_sound(struct req *r)
{
    page_begin(r, "Sound and screen", "/sound");
    H("<div class='card'><h2>Sound</h2>");
    form_begin(r, "/sound", NULL);
    level("Ringer", "ringer", val("audio", "ringer", "13"), 0, 15, NULL);
    level("Handset", "handset", val("audio", "handset", "10"), 0, 15, NULL);
    level("Speaker", "speaker", val("audio", "speaker", "10"), 0, 15, NULL);
    level("Headset", "headset", val("audio", "headset", "10"), 0, 15, NULL);
    level("Apps", "apps", val("audio", "apps", "12"), 0, 15, NULL);
    H("<div class='actions'><button class='primary' type='submit'>Save</button></div></form></div>");
    H("<div class='card'><h2>Screen</h2>");
    form_begin(r, "/screen", NULL);
    level("Brightness", "brightness", val("screen", "brightness", "10"), 1, 10, NULL);
    H("<div class='fields'>");
    field("Dim after (seconds)", "dim_seconds", val("screen", "dim_seconds", "120"), "inputmode='numeric' pattern='[0-9]*'", "0: never");
    field("Off after (seconds)", "off_seconds", val("screen", "off_seconds", "1800"), "inputmode='numeric' pattern='[0-9]*'", "0: never");
    H("</div><div class='actions'><button class='primary' type='submit'>Save</button></div></form></div>");
    /* the theme: the phone's screen, this page and apps wear the same one */
    static const char *const labels[] = {"Switchboard", "Directory", "Two-tone"};
    H("<div class='card'><h2>Theme</h2>");
    form_begin(r, "/theme", NULL);
    H("<div class='themes'>");
    for (int i = 0; i < t48_theme_count && i < 3; i++) {
        const char *name = t48_themes[i].name;
        H("<label class='theme'><input type='radio' name='theme' value='%s'%r><span class='pick'><span class='swatch' data-theme='%s'>"
          "<span class='band'>Calls</span><span class='line'>Alice</span><span class='line here'>Bob</span>"
          "<span class='keys'><span>5</span><span class='go'>Call</span></span></span></span><span class='name'>%s</span></label>",
          name, !strcmp(name, web_theme()) ? " checked" : "", name, labels[i]);
    }
    H("</div><div class='actions'><button class='primary' type='submit'>Save</button></div></form></div>");
    page_end(r);
}

/* a whole number from a form, between two bounds; -1 if it is not one */
static long whole(const struct req *r, const char *name, long min, long max)
{
    char v[16], *end;
    if (!line(r, name, v, sizeof v) || !v[0]) return -1;
    long n = strtol(v, &end, 10);
    return *end || n < min || n > max ? -1 : n;
}

static void post_sound(struct req *r)
{
    static const char *const keys[] = {"ringer", "handset", "speaker", "headset", "apps"};
    long v[5];
    char b[8];
    /* apps came later: a form without it leaves that level as it is */
    int n = line(r, "apps", b, sizeof b) && b[0] ? 5 : 4;
    for (int i = 0; i < n; i++)
        if ((v[i] = whole(r, keys[i], 0, 15)) < 0) { flash(r, 1, "Not saved: levels are 0 to 15."); redirect(r, "/sound"); return; }
    for (int i = 0; i < n; i++) { snprintf(b, sizeof b, "%ld", v[i]); cfg_set("audio", keys[i], b); }
    flash(r, 0, "Sound saved.");
    redirect(r, "/sound");
}

/* the shell sees the store change within a second, and tells the open app */
static void post_theme(struct req *r)
{
    char name[24];
    if (!line(r, "theme", name, sizeof name) || !t48_theme_named(name)) { flash(r, 1, "Not saved: no such theme."); redirect(r, "/sound"); return; }
    cfg_set("screen", "theme", name);
    flash(r, 0, "Theme saved.");
    redirect(r, "/sound");
}

static void post_screen(struct req *r)
{
    long b = whole(r, "brightness", 1, 10), dim = whole(r, "dim_seconds", 0, 86400), off = whole(r, "off_seconds", 0, 604800);
    char v[16];
    if (b < 0 || dim < 0 || off < 0) { flash(r, 1, "Not saved: brightness is 1 to 10, times are whole seconds."); redirect(r, "/sound"); return; }
    snprintf(v, sizeof v, "%ld", b); cfg_set("screen", "brightness", v);
    snprintf(v, sizeof v, "%ld", dim); cfg_set("screen", "dim_seconds", v);
    snprintf(v, sizeof v, "%ld", off); cfg_set("screen", "off_seconds", v);
    flash(r, 0, "Screen saved.");
    redirect(r, "/sound");
}

/* ---------- contacts ---------- */

static void page_contacts(struct req *r)
{
    int edit = -1;
    contacts_load();
    if (query(r, "edit")[0]) edit = atoi(query(r, "edit"));
    if (edit < 0 || edit >= cn) edit = -1;
    page_begin(r, "Contacts", "/contacts");
    H("<div class='card' id='edit'><h2>%r</h2>", edit >= 0 ? "Edit contact" : "Add contact");
    form_begin(r, "/contacts/save", NULL);
    if (edit >= 0) H("<input type='hidden' name='was_name' value='%s'><input type='hidden' name='was_number' value='%s'>", cl[edit].name, cl[edit].number);
    H("<div class='fields'>");
    H("<label class='field'><span>Name</span><input name='name' value='%s' maxlength='60' required></label>", edit >= 0 ? cl[edit].name : "");
    H("<label class='field'><span>Number</span><input name='number' value='%s' maxlength='60' inputmode='tel' required></label>", edit >= 0 ? cl[edit].number : "");
    H("</div><div class='actions'><button class='primary' type='submit'>%r</button>%r</div></form></div>",
      edit >= 0 ? "Save" : "Add", edit >= 0 ? " <a class='button quiet' href='/contacts'>Cancel</a>" : "");

    H("<div class='card'><h2>%d contact%r</h2>", cn, cn == 1 ? "" : "s");
    if (!cn) H("<p class='dim'>No contacts</p>");
    else H("<table class='list'><thead><tr><th>Name</th><th>Number</th><th></th></tr></thead><tbody>");
    for (int i = 0; i < cn; i++) {
        H("<tr><td>%s</td><td class='mono'>%s</td><td class='acts'><a class='button small' href='/contacts?edit=%d#edit'>Edit</a>", cl[i].name, cl[i].number, i);
        form_begin(r, "/contacts/delete", "class='inline'");
        H("<input type='hidden' name='name' value='%s'><input type='hidden' name='number' value='%s'>"
          "<button type='submit' class='small bad' data-ask='Delete %s?'>Delete</button></form></td></tr>", cl[i].name, cl[i].number, cl[i].name);
    }
    if (cn) H("</tbody></table>");
    H("</div>");

    H("<div class='card'><h2>Import and export</h2><p class='dim'>CSV (<span class='mono'>name,number</span> per line) or vCard.</p>");
    H("<div class='actions'><a class='button' href='/contacts/export.csv'>Export CSV</a> <a class='button' href='/contacts/export.vcf'>Export vCard</a></div>");
    form_begin(r, "/contacts/import", "enctype='multipart/form-data' class='sub'");
    H("<label class='field'><span>File</span><input type='file' name='file' accept='.csv,.vcf,.txt,text/csv,text/vcard' required></label>"
      "<label class='check'><input type='checkbox' name='replace' value='yes'> Replace the current list</label>"
      "<div class='actions'><button type='submit'>Import</button></div></form></div>");
    page_end(r);
}

static void post_contact_save(struct req *r)
{
    char name[61], number[61];
    struct contact c;
    contacts_load();
    if (!line(r, "name", name, sizeof name) || !line(r, "number", number, sizeof number) || !contact_put(&c, name, number)) {
        flash(r, 1, "Not saved: name and number are required, 60 characters at most.");
        redirect(r, "/contacts");
        return;
    }
    int at = form(r, "was_number")[0] ? contact_find(form(r, "was_name"), form(r, "was_number")) : -1;
    if (at >= 0) cl[at] = c;
    else if (cn >= MAX_CONTACTS) { flash(r, 1, "Not saved: the list is full (%d).", MAX_CONTACTS); redirect(r, "/contacts"); return; }
    else if (contact_find(c.name, c.number) < 0) cl[cn++] = c;
    if (contacts_save() < 0) flash(r, 1, "Not saved: %s.", strerror(errno));
    else flash(r, 0, "%s saved.", c.name);
    redirect(r, "/contacts");
}

static void post_contact_delete(struct req *r)
{
    contacts_load();
    int at = contact_find(form(r, "name"), form(r, "number"));
    if (at < 0) { flash(r, 1, "Contact not found."); redirect(r, "/contacts"); return; }
    struct contact was = cl[at];
    memmove(cl + at, cl + at + 1, sizeof cl[0] * (size_t)(cn - at - 1));
    cn--;
    if (contacts_save() < 0) flash(r, 1, "Not deleted: %s.", strerror(errno));
    else flash(r, 0, "%s deleted.", was.name);
    redirect(r, "/contacts");
}

/* one field of a CSV line, quotes and all; returns where the next starts, or NULL at the end */
static char *csv_field(char *p, char sep, char *out, size_t n)
{
    size_t o = 0;
    while (*p == ' ') p++;
    if (*p == '"') {
        for (p++; *p; p++) {
            if (*p == '"' && p[1] == '"') p++;
            else if (*p == '"') { p++; break; }
            if (o < n - 1) out[o++] = *p;
        }
        p += strcspn(p, (char[]){sep, 0});
    } else {
        size_t len = strcspn(p, (char[]){sep, 0});
        o = len < n - 1 ? len : n - 1;
        memcpy(out, p, o);
        p += len;
    }
    out[o] = 0;
    return *p ? p + 1 : NULL;
}

static void vcard_text(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o < n - 1; in++) {
        if (*in == '\\' && in[1]) { in++; out[o++] = *in == 'n' || *in == 'N' ? ' ' : *in; }
        else out[o++] = *in == ';' ? ' ' : *in;
    }
    out[o] = 0;
}

/* how many were added; in "seen", how many the file held */
static int import_contacts(char *text, int *seen)
{
    int added = 0, vcard = strstr(text, "BEGIN:VCARD") || strstr(text, "begin:vcard"), first = 1;
    char name[128] = "", tel[4][128];
    int ntel = 0;
    *seen = 0;
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3;   /* the mark some programs put at the top of a file */
    for (char *p = text; p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        p[strcspn(p, "\r")] = 0;
        if (vcard) {
            /* the property's name: before the first : or ;, after any "item1." in front */
            char prop[16], *colon = strchr(p, ':');
            size_t pl = strcspn(p, ":;");
            const char *dot = memchr(p, '.', pl);
            if (dot) { pl -= (size_t)(dot + 1 - p); }
            snprintf(prop, sizeof prop, "%.*s", (int)(pl < 15 ? pl : 15), dot ? dot + 1 : p);
            if (!strcasecmp(p, "BEGIN:VCARD")) { name[0] = 0; ntel = 0; }
            else if (!strcasecmp(p, "END:VCARD")) {
                for (int i = 0; i < ntel; i++) { (*seen)++; added += contact_add(name, tel[i]); }
                ntel = 0;
            }
            else if (colon && !strcasecmp(prop, "FN")) vcard_text(colon + 1, name, sizeof name);
            else if (colon && !strcasecmp(prop, "N") && !name[0]) vcard_text(colon + 1, name, sizeof name);
            else if (colon && !strcasecmp(prop, "TEL") && ntel < 4) vcard_text(colon + 1, tel[ntel++], sizeof tel[0]);
        } else if (p[0] && p[0] != '#') {
            char number[128], sep = strchr(p, ',') ? ',' : strchr(p, ';') ? ';' : strchr(p, '\t') ? '\t' : ',';
            char *next = csv_field(p, sep, name, sizeof name);
            number[0] = 0;
            if (next) csv_field(next, sep, number, sizeof number);
            /* a heading line, as a spreadsheet writes one */
            if (!(first && !strcasecmp(name, "name"))) { (*seen)++; added += contact_add(name, number); }
            first = 0;
        }
        p = nl ? nl + 1 : NULL;
    }
    return added;
}

static void post_contacts_import(struct req *r)
{
    int seen, added;
    char *text = strdup(form(r, "file"));
    contacts_load();
    if (!strcmp(form(r, "replace"), "yes") && text && text[0]) cn = 0;
    added = text ? import_contacts(text, &seen) : 0;
    free(text);
    if (!added && !seen) flash(r, 1, "Nothing imported: no contacts found. Use CSV (name,number) or vCard.");
    else if (!added) flash(r, 1, "Nothing imported: %d skipped (duplicate, no number, or list full).", seen);
    else if (contacts_save() < 0) flash(r, 1, "Not imported: %s.", strerror(errno));
    else if (seen > added) flash(r, 0, "%d contact%s added, %d skipped (duplicate, no number, or list full).", added, added == 1 ? "" : "s", seen - added);
    else flash(r, 0, "%d contact%s added.", added, added == 1 ? "" : "s");
    redirect(r, "/contacts");
}

static void csv_out(const char *s)
{
    raw("\"", 1);
    for (; *s; s++) { if (*s == '"') raw("\"", 1); raw(s, 1); }
    raw("\"", 1);
}

static void get_contacts_csv(struct req *r)
{
    contacts_load();
    page_reset();
    H("name,number\r\n");
    for (int i = 0; i < cn; i++) { csv_out(cl[i].name); raw(",", 1); csv_out(cl[i].number); raw("\r\n", 2); }
    send_file(r, "text/csv; charset=utf-8", "t48s-contacts.csv");
}

static void vcard_out(const char *s)
{
    for (; *s; s++) { if (strchr("\\,;", *s)) raw("\\", 1); raw(s, 1); }
}

static void get_contacts_vcf(struct req *r)
{
    contacts_load();
    page_reset();
    for (int i = 0; i < cn; i++) {
        H("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:");
        vcard_out(cl[i].name);
        H("\r\nN:");
        vcard_out(cl[i].name);
        H(";;;;\r\nTEL;TYPE=VOICE:");
        vcard_out(cl[i].number);
        H("\r\nEND:VCARD\r\n");
    }
    send_file(r, "text/vcard; charset=utf-8", "t48s-contacts.vcf");
}

/* ---------- call history: the shell's file, when|kind|seconds|number|name|account ---------- */

static void page_history(struct req *r)
{
    char path[300];
    char *lines[64];
    int n = 0;
    snprintf(path, sizeof path, "%s/history", opt.state);
    char *t = load_file(path, 64 * 1024);
    for (char *p = t; p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (strncmp(p, "seen ", 5) && strchr(p, '|')) {
            if (n == 64) { memmove(lines, lines + 1, sizeof lines[0] * 63); n--; }
            lines[n++] = p;
        }
        p = nl ? nl + 1 : NULL;
    }
    contacts_load();
    use_phone_time();
    page_begin(r, "Calls", "/history");
    H("<div class='card'><h2>%d call%r</h2>", n, n == 1 ? "" : "s");
    if (!n) H("<p class='dim'>No calls</p>");
    else H("<table class='list'><thead><tr><th>Time</th><th></th><th>Number</th><th>Duration</th><th>Account</th></tr></thead><tbody>");
    for (int i = n - 1; i >= 0; i--) {   /* the file has the newest last */
        char *fld[6] = {0}, *p = lines[i], when[40], len[24] = "";
        for (int k = 0; k < 6 && p; k++) {
            fld[k] = p;
            p = strchr(p, '|');
            if (p) *p++ = 0;
        }
        if (!fld[3] || !fld[3][0] || !fld[1][0]) continue;
        time_t at = (time_t)atoll(fld[0]);
        struct tm tm;
        int secs = atoi(fld[2]), acct = fld[5] ? atoi(fld[5]) : 0;
        localtime_r(&at, &tm);
        strftime(when, sizeof when, "%a %e %b, %H:%M", &tm);
        if (secs > 0) snprintf(len, sizeof len, "%d:%02d", secs / 60, secs % 60);
        const char *name = contact_name(fld[3]) ? contact_name(fld[3]) : fld[4] ? fld[4] : "";
        char k = fld[1][0];
        H("<tr><td>%s</td><td><span class='pill %r'>%r</span></td><td>%s%r<span class='mono'>%s</span></td><td>%s</td><td>",
          when, k == 'm' ? "bad" : k == 'o' ? "" : "good", k == 'm' ? "Missed" : k == 'o' ? "Out" : "In", name, name[0] ? " " : "", fld[3], len);
        if (acct >= 1 && acct <= MAX_ACCTS) {
            const char *label = val(sip_file(acct), "label", "");
            H("%s", label[0] ? label : val(sip_file(acct), "user", ""));
        }
        H("</td></tr>");
    }
    if (n) {
        H("</tbody></table>");
        form_begin(r, "/history/clear", "class='danger'");
        sure_box();
        H("<button type='submit' class='bad'>Clear all</button></form>");
    }
    H("</div>");
    free(t);
    page_end(r);
}

static void post_history_clear(struct req *r)
{
    char path[300], keep[64] = "";
    if (!sure(r)) { flash(r, 1, UNSURE); redirect(r, "/history"); return; }
    snprintf(path, sizeof path, "%s/history", opt.state);
    char *t = load_file(path, 64 * 1024);
    /* the line that says which missed calls have been looked at stays */
    if (t && !strncmp(t, "seen ", 5)) snprintf(keep, sizeof keep, "%.*s\n", (int)strcspn(t, "\r\n"), t);
    free(t);
    mkdir(opt.state, 0755);
    if (save_file(path, keep, 0644) < 0) flash(r, 1, "Not cleared: %s.", strerror(errno));
    else flash(r, 0, "Calls cleared.");
    redirect(r, "/history");
}

/* ---------- apps: t48-app does the work (shell/pkg.c, docs/apps.md) ---------- */

static int app_command(const char *verb, const char *id, char *out, size_t n, int wait_ms)
{
    char *argv[12];
    int k = 0;
    argv[k++] = (char *)wopt.appcmd;
    argv[k++] = "--apps"; argv[k++] = (char *)opt.apps;
    argv[k++] = "--config"; argv[k++] = (char *)opt.config;
    argv[k++] = "--data"; argv[k++] = (char *)opt.data;
    argv[k++] = "--machine";
    argv[k++] = (char *)verb;
    if (id) argv[k++] = (char *)id;
    argv[k] = NULL;
    return run_program(argv, wait_ms, out, n, 0);
}

static const struct manifest *installed(const char *id)
{
    for (int i = 0; i < napps; i++)
        if (!strcmp(apps[i].id, id)) return &apps[i];
    return NULL;
}

static void app_settings_form(struct req *r, const struct manifest *m)
{
    char file[80], name[64];
    snprintf(file, sizeof file, "apps/%s", m->id);
    H("<details class='sub'><summary>Settings</summary>");
    form_begin(r, "/apps/settings", NULL);
    H("<input type='hidden' name='id' value='%s'><div class='fields'>", m->id);
    for (int k = 0; k < m->nsettings; k++) {
        char key[48], *def = strchr(m->settings[k], '=');
        const char *type = m->setting_type[k];
        snprintf(key, sizeof key, "%.*s", (int)(def - m->settings[k]), m->settings[k]);
        snprintf(name, sizeof name, "s_%s", key);
        const char *v = val(file, key, def + 1);
        if (!strcmp(type, "bool"))
            H("<label class='check'><input type='checkbox' name='%s' value='true'%r> %s</label>", name, !strcmp(v, "true") ? " checked" : "", m->setting_label[k]);
        else if (!strcmp(type, "password"))
            H("<label class='field'><span>%s</span><input type='password' name='%s' value='' maxlength='200' autocomplete='new-password' placeholder='%r'></label>",
              m->setting_label[k], name, val(file, key, "")[0] ? "Set. Empty keeps it" : "Not set");
        else
            H("<label class='field'><span>%s</span><input name='%s' value='%s' maxlength='200' %r></label>", m->setting_label[k], name, v,
              !strcmp(type, "number") ? "inputmode='decimal'" : "");
    }
    H("</div><div class='actions'><button class='primary' type='submit'>Save</button> "
      "<button type='submit' name='defaults' value='yes' formnovalidate>Reset to defaults</button></div></form></details>");
}

static void page_apps(struct req *r)
{
    static char out[32 * 1024];
    char err[200] = "";
    int shown = 0;
    app_command("list", NULL, out, sizeof out, 25000);
    scan_apps();
    page_begin(r, "Apps", "/apps");
    for (char *p = out; p && *p;) {
        char *nl = strchr(p, '\n'), *f[10] = {0};
        if (nl) *nl = 0;
        if (!strncmp(p, "feederror ", 10)) snprintf(err, sizeof err, "%s", p + 10);
        if (!strncmp(p, "app\t", 4)) {
            char *q = p;
            for (int k = 0; k < 10 && q; k++) { f[k] = q; q = strchr(q, '\t'); if (q) *q++ = 0; }
        }
        p = nl ? nl + 1 : NULL;
        if (!f[9] || !manifest_id_ok(f[1])) continue;
        /* id, name, installed, in the feed, download size, size on the phone, state, permissions, description */
        const char *id = f[1], *st = f[7];
        const struct manifest *m = installed(id);
        long kb = (atol(f[6]) ? atol(f[6]) : atol(f[5])) / 1024 + 1;
        shown++;
        H("<div class='card app'><div class='apphead'><h2>%s</h2><span class='pill %r'>%r</span></div>", f[2],
          !strcmp(st, "available") ? "" : !strcmp(st, "update") ? "warn" : !strcmp(st, "unsupported") ? "bad" : "good",
          !strcmp(st, "available") ? "Available" : !strcmp(st, "update") ? "Update" : !strcmp(st, "unsupported") ? "Needs newer firmware" :
          !strcmp(st, "builtin") ? "Built in" : "Installed");
        if (strcmp(f[9], "-")) H("<p>%s</p>", f[9]);
        H("<p class='dim small'>");
        if (strcmp(f[3], "-")) H("Installed %s. ", f[3]);
        if (strcmp(f[4], "-")) H("In the feed %s. ", f[4]);
        H("%l KB. %r%s</p><div class='actions'>", kb, strcmp(f[8], "-") ? "Permissions: " : "", strcmp(f[8], "-") ? f[8] : "");
        if (!strcmp(st, "available") || !strcmp(st, "update")) {
            form_begin(r, !strcmp(st, "update") ? "/apps/update" : "/apps/install", "class='inline'");
            H("<input type='hidden' name='id' value='%s'><button class='primary' type='submit'>%r</button></form>", id, !strcmp(st, "update") ? "Update" : "Install");
        }
        if (!strcmp(st, "installed") || !strcmp(st, "update")) {
            form_begin(r, "/apps/remove", "class='inline'");
            H("<input type='hidden' name='id' value='%s'><button type='submit' class='bad' data-ask='Remove %s? Deletes its data and settings.'>Remove</button></form>", id, f[2]);
        }
        H("</div>");
        if (m && m->nsettings) app_settings_form(r, m);
        H("</div>");
    }
    if (!shown) H("<div class='card'><p class='dim'>No apps</p></div>");
    if (err[0]) H("<p class='flash bad'>Feed error: %s</p>", err);

    H("<div class='card'><h2>Feed</h2>");
    form_begin(r, "/apps/feed", NULL);
    H("<label class='field'><span>Feed URL</span><input name='url' type='url' value='%s' maxlength='200' placeholder='%s'><small>Empty: default feed</small></label>"
      "<div class='actions'><button type='submit'>Save</button></div></form></div>", val("feed", "url", ""), T48_DEFAULT_FEED);
    page_end(r);
}

static void post_app_change(struct req *r)
{
    static char out[8192];
    char id[64], why[200] = "";
    const char *verb = !strcmp(r->path, "/apps/install") ? "install" : !strcmp(r->path, "/apps/update") ? "update" : "remove";
    if (!line(r, "id", id, sizeof id) || !manifest_id_ok(id)) { redirect(r, "/apps"); return; }
    int st = app_command(verb, id, out, sizeof out, 180000);
    for (char *p = out; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, "error ", 6)) snprintf(why, sizeof why, "%.*s", (int)strcspn(p + 6, "\n"), p + 6);
    logmsg("app %s %s: %s", verb, id, st == 0 ? "done" : "failed");
    if (st == 0) flash(r, 0, "%s %s.", id, !strcmp(verb, "remove") ? "removed" : !strcmp(verb, "update") ? "updated" : "installed");
    else flash(r, 1, "%s not %s: %s.", id, !strcmp(verb, "remove") ? "removed" : !strcmp(verb, "update") ? "updated" : "installed",
               why[0] ? why : "installer did not finish");
    redirect(r, "/apps");
}

static void post_app_settings(struct req *r)
{
    char id[64], file[80], name[64], v[201];
    int bad = 0;
    scan_apps();
    const struct manifest *m = line(r, "id", id, sizeof id) ? installed(id) : NULL;
    if (!m) { redirect(r, "/apps"); return; }
    snprintf(file, sizeof file, "apps/%s", m->id);
    if (!strcmp(form(r, "defaults"), "yes")) {
        char path[400];
        snprintf(path, sizeof path, "%s/apps/%s.conf", opt.config, m->id);
        unlink(path);
        flash(r, 0, "%s reset to defaults.", m->name);
        redirect(r, "/apps");
        return;
    }
    for (int k = 0; k < m->nsettings; k++) {
        char key[48];
        const char *type = m->setting_type[k];
        snprintf(key, sizeof key, "%.*s", (int)strcspn(m->settings[k], "="), m->settings[k]);
        snprintf(name, sizeof name, "s_%s", key);
        if (!strcmp(type, "bool")) { bad |= cfg_set(file, key, !strcmp(form(r, name), "true") ? "true" : "false") < 0; continue; }
        if (!strcmp(type, "password")) {
            const char *pw = form(r, name);
            if (pw[0]) bad |= strlen(pw) > 200 || cfg_set(file, key, pw) < 0;
            continue;
        }
        if (!line(r, name, v, sizeof v)) { bad = 1; continue; }
        bad |= cfg_set(file, key, v) < 0;
    }
    if (bad) flash(r, 1, "Some %s settings not saved: too long, or invalid characters.", m->name);
    else flash(r, 0, "%s settings saved.", m->name);
    redirect(r, "/apps");
}

static void post_app_feed(struct req *r)
{
    char u[201];
    if (!line(r, "url", u, sizeof u) || (u[0] && strncmp(u, "https://", 8) && strncmp(u, "file://", 7)) || strchr(u, ' ')) flash(r, 1, "Not saved: the feed URL must start with https://");
    else { set("feed", "url", u); flash(r, 0, u[0] ? "Feed saved." : "Using the default feed."); }
    redirect(r, "/apps");
}

/* ---------- access: SSH and its keys (docs/access.md), and this page itself ---------- */

struct key { char type[40], *blob, comment[84], *line; };

static void keys_path(char *out, size_t n) { snprintf(out, n, "%s/ssh/authorized_keys", wopt.keep); }

/* the type, the key and the name of one line; 0 if it is not a key, or has options in front */
static int key_parse(char *l, struct key *k)
{
    static const char *const types[] = {"ssh-ed25519", "ssh-rsa", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
                                        "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com"};
    while (*l == ' ' || *l == '\t') l++;
    size_t tl = strcspn(l, " \t"), ok = 0;
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) ok |= strlen(types[i]) == tl && !strncmp(l, types[i], tl);
    if (!ok || !l[tl]) return 0;
    snprintf(k->type, sizeof k->type, "%.*s", (int)tl, l);
    char *b = l + tl;
    while (*b == ' ' || *b == '\t') b++;
    size_t bl = strspn(b, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=");
    if (bl < 60 || bl > 1500 || strncmp(b, "AAAA", 4) || (b[bl] && b[bl] != ' ' && b[bl] != '\t')) return 0;
    char *c = b + bl;
    if (*c) *c++ = 0;
    while (*c == ' ' || *c == '\t') c++;
    snprintf(k->comment, sizeof k->comment, "%.80s", c);
    for (size_t l = strlen(k->comment); l && (k->comment[l - 1] == ' ' || k->comment[l - 1] == '\t');) k->comment[--l] = 0;
    for (char *p = k->comment; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
    k->blob = b;
    return 1;
}

static int keys_load(struct key *k, int max, char **text)
{
    char path[300];
    int n = 0;
    keys_path(path, sizeof path);
    *text = load_file(path, 64 * 1024);
    for (char *p = *text; p && *p && n < max;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        p[strcspn(p, "\r")] = 0;
        if (key_parse(p, &k[n])) n++;
        p = nl ? nl + 1 : NULL;
    }
    return n;
}

static int keys_save(const struct key *k, int n)
{
    char path[300], dir[300], *t = malloc((size_t)n * 1700 + 1);
    size_t o = 0;
    if (!t) return -1;
    t[0] = 0;
    for (int i = 0; i < n; i++) o += (size_t)sprintf(t + o, "%s %s%s%s\n", k[i].type, k[i].blob, k[i].comment[0] ? " " : "", k[i].comment);
    mkdir(wopt.keep, 0700);
    snprintf(dir, sizeof dir, "%s/ssh", wopt.keep);
    mkdir(dir, 0700);
    keys_path(path, sizeof path);
    int r = save_file(path, t, 0600);
    free(t);
    return r;
}

static int keys_count(void)
{
    struct key k[MAX_KEYS];
    char *text;
    int n = keys_load(k, MAX_KEYS, &text);
    free(text);
    return n;
}

/* what t48-access last said: one value from its status file */
static const char *access_status(const char *key, const char *def)
{
    static char v[2][120];
    static int at;
    char path[300], *out = v[at = !at];
    size_t kl = strlen(key);
    snprintf(out, sizeof v[0], "%s", def);
    snprintf(path, sizeof path, "%s/status", opt.access);
    char *t = load_file(path, 4096);
    for (char *p = t; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, key, kl) && p[kl] == '=') { snprintf(out, sizeof v[0], "%.*s", (int)strcspn(p + kl + 1, "\r\n"), p + kl + 1); break; }
    free(t);
    return out;
}

/* SSH takes logins: turned on by the owner, or a development build, where it always is */
static int ssh_is_on(void)
{
    return !strcmp(access_status("dev", "no"), "yes") || !strcmp(val("access", "ssh", "off"), "on");
}

static void page_access(struct req *r)
{
    struct key k[MAX_KEYS];
    char *text;
    int n = keys_load(k, MAX_KEYS, &text);
    int dev = !strcmp(access_status("dev", "no"), "yes");
    int on = ssh_is_on();
    char host[64];
    snprintf(host, sizeof host, "%s", web_url()[0] ? web_url() + 7 : "the-phone");
    host[strcspn(host, ":/")] = 0;

    page_begin(r, "Access", "/access");
    H("<div class='card'><div class='apphead'><h2>SSH</h2><span class='pill %r'>%r</span></div>", on ? "good" : "", on ? "On" : "Off");
    H("<p class='dim'>Root shell. Key login only.</p>");
    if (dev) H("<p class='note'>Development build: SSH always on, built-in keys accepted.</p>");
    if (on) H("<p><span class='mono'>ssh root@%s</span></p>", host);
    H("<p class='dim small'>Host key <span class='mono'>%s</span></p>", access_status("host_key", "unknown until SSH has run"));
    if (!dev) {
        form_begin(r, "/access/ssh", NULL);
        H("<input type='hidden' name='to' value='%r'><div class='actions'><button type='submit' class='%r'>Turn %r</button></div></form>",
          on ? "off" : "on", on ? "" : "primary", on ? "off" : "on");
    }
    H("</div>");

    H("<div class='card'><h2>Keys</h2>");
    if (!n) H("<p class='dim'>None</p>");
    else H("<table class='list'><tbody>");
    for (int i = 0; i < n; i++) {
        H("<tr><td><span class='mono'>%s</span></td><td>%s<br><span class='dim mono small'>...%s</span></td><td class='acts'>", k[i].type,
          k[i].comment[0] ? k[i].comment : "Unnamed", k[i].blob + (strlen(k[i].blob) > 16 ? strlen(k[i].blob) - 16 : 0));
        form_begin(r, "/access/key-remove", "class='inline'");
        H("<input type='hidden' name='key' value='%s'><button type='submit' class='small bad' data-ask='Remove this key?'>Remove</button></form></td></tr>", k[i].blob);
    }
    if (n) H("</tbody></table>");
    form_begin(r, "/access/key-add", "class='sub'");
    H("<label class='field wide'><span>Add key</span><textarea name='keys' rows='4' spellcheck='false' autocapitalize='none' "
      "placeholder='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAA... you@computer' required></textarea>"
      "<small>Public keys, one per line, as in <span class='mono'>~/.ssh/id_ed25519.pub</span></small></label>"
      "<div class='actions'><button class='primary' type='submit'>Add key</button></div></form>");
    if (n) {
        form_begin(r, "/access/keys-forget", "class='danger'");
        sure_box();
        H("<button type='submit' class='bad'>%r</button></form>", dev ? "Remove added keys" : "Remove all keys");
    }
    H("</div>");
    free(text);

    H("<div class='card'><h2>Web setup</h2><p>%d browser%r paired. <span class='dim'>Pairing ends after 12 idle hours, a phone restart, or turning this off.</span></p>",
      sessions_count(), sessions_count() == 1 ? "" : "s");
    if (sessions_count() > 1) {
        form_begin(r, "/access/browsers", NULL);
        H("<div class='actions'><button type='submit'>Sign out other browsers</button></div></form>");
    }
    form_begin(r, "/access/web-off", "class='danger'");
    sure_box();
    H("<button type='submit' class='bad'>Turn off web setup</button><small class='dim'> Back on only at the phone: Settings, Access</small></form></div>");
    page_end(r);
}

static void post_access_ssh(struct req *r)
{
    int on = !strcmp(form(r, "to"), "on");
    cfg_set("access", "ssh", on ? "on" : "off");
    logmsg("SSH turned %s", on ? "on" : "off");
    run_apply("access", 8000, 0);
    flash(r, 0, on ? "SSH on." : "SSH off. Open sessions closed.");
    redirect(r, "/access");
}

static void post_key_add(struct req *r)
{
    struct key k[MAX_KEYS];
    char *text, *add = strdup(form(r, "keys"));
    int n = keys_load(k, MAX_KEYS, &text), added = 0, refused = 0;
    for (char *p = add; p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        p[strcspn(p, "\r")] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#') {
            int dup = 0;
            if (n >= MAX_KEYS || !key_parse(p, &k[n])) refused++;
            else {
                for (int i = 0; i < n; i++) dup |= !strcmp(k[i].blob, k[n].blob);
                if (!dup) { n++; added++; }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    if (added && keys_save(k, n) < 0) flash(r, 1, "Not added: %s.", strerror(errno));
    else if (refused) flash(r, 1, "%d key%s added, %d line%s refused. A key is one line: type, key, optional name. Limit %d keys.",
                            added, added == 1 ? "" : "s", refused, refused == 1 ? "" : "s", MAX_KEYS);
    else if (!added) flash(r, 1, "Key already added.");
    else flash(r, 0, "%d key%s added.%s", added, added == 1 ? "" : "s", ssh_is_on() ? "" : " SSH is off.");
    if (added) { logmsg("%d SSH key%s added", added, added == 1 ? "" : "s"); run_apply("access-status", 4000, 0); }
    free(text);
    free(add);
    redirect(r, "/access");
}

static void post_key_remove(struct req *r)
{
    struct key k[MAX_KEYS];
    char *text;
    int n = keys_load(k, MAX_KEYS, &text), at = -1;
    for (int i = 0; i < n; i++) if (!strcmp(k[i].blob, form(r, "key"))) at = i;
    if (at < 0) flash(r, 1, "Key not found.");
    else {
        memmove(k + at, k + at + 1, sizeof k[0] * (size_t)(n - at - 1));
        if (keys_save(k, n - 1) < 0) flash(r, 1, "Not removed: %s.", strerror(errno));
        else { flash(r, 0, "Key removed. Open sessions stay open."); logmsg("an SSH key removed"); run_apply("access-status", 4000, 0); }
    }
    free(text);
    redirect(r, "/access");
}

static void post_keys_forget(struct req *r)
{
    if (!sure(r)) flash(r, 1, UNSURE);
    else {
        /* t48-access empties the list too, and puts a development build's own keys back */
        keys_save(NULL, 0);
        logmsg("SSH keys forgotten");
        run_apply("access-forget-keys", 4000, 0);
        flash(r, 0, "Keys removed.");
    }
    redirect(r, "/access");
}

static void post_browsers(struct req *r)
{
    sessions_forget_others(r->s);
    flash(r, 0, "Other browsers signed out.");
    redirect(r, "/access");
}

static void post_web_off(struct req *r)
{
    if (!sure(r)) { flash(r, 1, UNSURE); redirect(r, "/access"); return; }
    cfg_set("access", "web", "off");
    logmsg("turned off from the page itself");
    run_apply("access", 0, 1);
    page_reset();
    H("<!doctype html><html lang='en' data-theme='%s'><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>T48S</title>"
      "<link rel='stylesheet' href='/style.css'><main class='lone'><div class='card'><h1>Web setup is off</h1>"
      "<p>Turn it on at the phone: Settings, Access.</p></div></main></html>", web_theme());
    send_page(r, 200);
    web_stop_soon();
}

/* ---------- backup and restore ---------- */

#define BACKUP_FIRST "# T48S settings backup, format 1"
static const char *const backup_files[] = {"sip", "sip2", "sip3", "sip4", "sip5", "sip6", "phone", "network", "system", "audio", "screen", "feed"};
#define NBACKUP ((int)(sizeof backup_files / sizeof backup_files[0]))

/* is this key of this file a password: by its name, or because the app's manifest says so */
static int secret_key(const char *file, const char *key)
{
    if (cfg_secret(key)) return 1;
    const struct manifest *m = !strncmp(file, "apps/", 5) ? installed(file + 5) : NULL;
    for (int k = 0; m && k < m->nsettings; k++)
        if (!strcmp(m->setting_type[k], "password") && !strncmp(m->settings[k], key, strlen(key)) && m->settings[k][strlen(key)] == '=') return 1;
    return 0;
}

static void backup_section(const char *file, int passwords)
{
    /* an app that is not installed has no manifest to say which of its settings
     * are passwords: without passwords, none of them go in the file */
    if (!passwords && !strncmp(file, "apps/", 5) && !installed(file + 5)) return;
    char *t = cfg_load_text(file);
    if (!t) return;
    H("[config %r]\n", file);
    for (char *p = t; *p;) {
        size_t len = strcspn(p, "\n");
        char key[80], *eq = memchr(p, '=', len);
        snprintf(key, sizeof key, "%.*s", eq ? (int)(eq - p) : 0, p);
        if (len && p[0] != '[' && (passwords || p[0] == '#' || !eq || !secret_key(file, key))) { raw(p, len); raw("\n", 1); }
        p += len + (p[len] == '\n');
    }
    free(t);
}

static int has_app_confs(char names[][80], int max)
{
    char dir[300];
    int n = 0;
    snprintf(dir, sizeof dir, "%s/apps", opt.config);
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d)) && n < max) {
        size_t l = strlen(de->d_name);
        if (l < 6 || l > 60 || strcmp(de->d_name + l - 5, ".conf")) continue;
        snprintf(names[n], 80, "apps/%.*s", (int)(l - 5), de->d_name);
        if (manifest_id_ok(names[n] + 5)) n++;
    }
    if (d) closedir(d);
    return n;
}

static void page_backup(struct req *r)
{
    page_begin(r, "Backup", "/backup");
    H("<div class='card'><h2>Download</h2><p class='dim'>Settings and contacts, as one text file. Not SSH keys, access switches, calls or apps.</p>");
    form_begin(r, "/backup/download", NULL);
    H("<label class='check'><input type='checkbox' name='passwords' value='yes'> Include passwords</label>"
      "<div class='actions'><button class='primary' type='submit'>Download</button></div></form></div>");
    H("<div class='card'><h2>Restore</h2><p class='dim'>Replaces each section the file contains. Passwords missing from the file are kept.</p>");
    form_begin(r, "/backup/restore", "enctype='multipart/form-data'");
    H("<label class='field'><span>Backup file</span><input type='file' name='file' accept='.txt,.t48backup,text/plain' required></label>");
    H("<div class='actions'>");
    sure_box();
    H("<button type='submit' class='bad'>Restore</button></div></form></div>");
    page_end(r);
}

static void post_backup_download(struct req *r)
{
    char names[32][80], when[40], file[80], path[300];
    int passwords = !strcmp(form(r, "passwords"), "yes");
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M UTC", &tm);
    scan_apps();
    page_reset();
    H("%r\n# made: %r\n# passwords: %r\n", BACKUP_FIRST, when, passwords ? "included" : "left out");
    if (passwords) H("# SENSITIVE: contains passwords\n");
    for (int i = 0; i < NBACKUP; i++) backup_section(backup_files[i], passwords);
    int n = has_app_confs(names, 32);
    for (int i = 0; i < n; i++) backup_section(names[i], passwords);
    contacts_path(path, sizeof path);
    char *c = load_file(path, 64 * 1024);
    if (c) {
        H("[contacts]\n");
        for (char *p = c; *p;) {
            size_t len = strcspn(p, "\n");
            if (len && p[0] != '[') { raw(p, len); raw("\n", 1); }
            p += len + (p[len] == '\n');
        }
        free(c);
    }
    H("[end]\n");
    strftime(when, sizeof when, "%Y%m%d", &tm);
    snprintf(file, sizeof file, "t48s-settings-%s%s.txt", when, passwords ? "-SENSITIVE-with-passwords" : "");
    logmsg("backup downloaded, passwords %s", passwords ? "included" : "left out");
    send_file(r, "text/plain; charset=utf-8", file);
}

struct section { char name[80]; char *text; size_t len; };

static int backup_name_ok(const char *name)
{
    if (!strcmp(name, "contacts")) return 1;
    if (strncmp(name, "config ", 7)) return 0;
    name += 7;
    for (int i = 0; i < NBACKUP; i++) if (!strcmp(name, backup_files[i])) return 1;
    return !strncmp(name, "apps/", 5) && strlen(name) < 70 && manifest_id_ok(name + 5);
}

/* a line a settings file may hold: a comment, nothing, or key=value with a plain key */
static int setting_line_ok(const char *l)
{
    if (!l[0] || l[0] == '#') return 1;
    const char *eq = strchr(l, '=');
    if (!eq || eq == l) return 0;
    for (const char *p = l; p < eq; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._", *p))) return 0;
    return 1;
}

static void post_backup_restore(struct req *r)
{
    static struct section s[64];
    char *text = strdup(form(r, "file")), why[160] = "";
    int ns = 0, cur = -1, ended = 0, ln = 0, restored = 0, sip = 0, net = 0;
    if (!sure(r)) { flash(r, 1, UNSURE); goto out; }
    if (!text || strncmp(text, BACKUP_FIRST, strlen(BACKUP_FIRST))) { flash(r, 1, "Not restored: not a T48S backup file."); goto out; }
    scan_apps();
    /* read all of it and check all of it before anything on the phone is touched */
    for (char *p = text; p && *p && !why[0];) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        p[strcspn(p, "\r")] = 0;
        ln++;
        for (char *q = p; *q; q++) if ((unsigned char)*q < 32 || *q == 127) snprintf(why, sizeof why, "line %d is not text", ln);
        if (ended && p[0]) snprintf(why, sizeof why, "data after [end]");
        else if (!strcmp(p, "[end]")) ended = 1;
        else if (p[0] == '[') {
            size_t l = strlen(p);
            if (l < 3 || p[l - 1] != ']' || ns >= 64) snprintf(why, sizeof why, "line %d: bad section header", ln);
            else {
                p[l - 1] = 0;
                if (!backup_name_ok(p + 1)) snprintf(why, sizeof why, "line %d: unknown section", ln);
                else {
                    cur = ns++;
                    snprintf(s[cur].name, sizeof s[0].name, "%s", p + 1);
                    s[cur].text = calloc(1, 20000);
                    s[cur].len = 0;
                }
            }
        } else if (cur >= 0 && s[cur].text) {
            int contacts = !strcmp(s[cur].name, "contacts");
            size_t l = strlen(p);
            if (!contacts && !setting_line_ok(p)) snprintf(why, sizeof why, "line %d: not key=value", ln);
            else if (s[cur].len + l + 400 > (contacts ? 20000 : 15000)) snprintf(why, sizeof why, "section %s is too large", s[cur].name);
            else if (l) { memcpy(s[cur].text + s[cur].len, p, l); s[cur].len += l; s[cur].text[s[cur].len++] = '\n'; }
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!why[0] && !ended) snprintf(why, sizeof why, "file is cut short, [end] is missing");
    if (why[0]) { flash(r, 1, "Not restored: %s.", why); goto out; }

    for (int i = 0; i < ns; i++) {
        if (!strcmp(s[i].name, "contacts")) {
            char path[300];
            mkdir(opt.state, 0755);
            contacts_path(path, sizeof path);
            if (save_file(path, s[i].text, 0644) == 0) restored++;
            continue;
        }
        const char *file = s[i].name + 7;
        char *old = cfg_load_text(file);
        /* a password the phone has and the file does not say: kept */
        for (char *p = old; p && *p;) {
            size_t l = strcspn(p, "\n");
            char key[80], line_[600], *eq = memchr(p, '=', l);
            snprintf(key, sizeof key, "%.*s", eq ? (int)(eq - p) : 0, p);
            snprintf(line_, sizeof line_, "\n%s=", key);
            if (eq && p[0] != '#' && secret_key(file, key) && l < 500 && s[i].len + l + 2 < 20000 && strncmp(s[i].text, line_ + 1, strlen(line_ + 1)) && !strstr(s[i].text, line_)) {
                memcpy(s[i].text + s[i].len, p, l);
                s[i].len += l;
                s[i].text[s[i].len++] = '\n';
                s[i].text[s[i].len] = 0;
            }
            p += l + (p[l] == '\n');
        }
        if (!old || strcmp(old, s[i].text)) {
            if (cfg_save_text(file, s[i].text) == 0) restored++;
            sip |= !strncmp(file, "sip", 3);
            net |= !strcmp(file, "network");
        } else restored++;
        free(old);
    }
    logmsg("backup restored: %d parts", restored);
    if (sip) run_apply("sip", 0, 0);
    if (net) run_apply("network", 0, 2);
    flash(r, restored != ns, "Restored %d of %d sections.%s", restored, ns, net ? " Applying network settings." : "");
out:
    for (int i = 0; i < ns; i++) { free(s[i].text); s[i].text = NULL; }
    if (text) memset(text, 0, strlen(text));
    free(text);
    redirect(r, "/backup");
}

/* ---------- update: t48-update does the work (docs/install.md) ---------- */

struct update_info {
    char installed[8], slot[8], version[64], channel[204], latest[64], update[12], kernel[12], checked[24];
    char state[16], message[160], result[160], result_time[24];
};

static int update_command(const char *verb, const char *arg, char *out, size_t n, int wait_ms)
{
    char *argv[] = {(char *)wopt.update, (char *)verb, (char *)arg, NULL};
    return run_program(argv, wait_ms, out, n, 0);
}

/* what "t48-update info" says, NAME=VALUE a line. Asking never goes to the network. */
static void update_read(struct update_info *u)
{
    const struct { const char *name; char *to; size_t n; } f[] = {
        {"installed", u->installed, sizeof u->installed}, {"slot", u->slot, sizeof u->slot}, {"version", u->version, sizeof u->version},
        {"channel", u->channel, sizeof u->channel}, {"latest", u->latest, sizeof u->latest}, {"update", u->update, sizeof u->update},
        {"kernel", u->kernel, sizeof u->kernel}, {"checked", u->checked, sizeof u->checked}, {"state", u->state, sizeof u->state},
        {"message", u->message, sizeof u->message}, {"result", u->result, sizeof u->result}, {"result_time", u->result_time, sizeof u->result_time},
    };
    char out[2048];
    memset(u, 0, sizeof *u);
    update_command("info", NULL, out, sizeof out, 10000);
    for (char *p = out; p && *p;) {
        char *nl = strchr(p, '\n'), *eq;
        if (nl) *nl = 0;
        p[strcspn(p, "\r")] = 0;
        if ((eq = strchr(p, '='))) {
            *eq = 0;
            for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++)
                if (!strcmp(p, f[i].name)) snprintf(f[i].to, f[i].n, "%s", eq + 1);
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!u->version[0]) snprintf(u->version, sizeof u->version, "%s", T48_VERSION);
}

static int update_busy(const struct update_info *u)
{
    return !strcmp(u->state, "checking") || !strcmp(u->state, "downloading") || !strcmp(u->state, "writing");
}

/* A run is started and left to itself, and takes a moment to say that it is
 * running: until what it says has changed, for three seconds at most, the
 * page takes it to be on its first step, which is the check. */
static time_t update_started;
static char update_before[360];

static void update_mark(const struct update_info *u, char *out, size_t n)
{
    snprintf(out, n, "%s|%s|%s", u->state, u->message, u->checked);
}

static int update_pending(const struct update_info *u)
{
    char now[360];
    update_mark(u, now, sizeof now);
    if (strcmp(now, update_before) || time(NULL) - update_started >= 3) update_started = 0;
    return update_started != 0;
}

/* 23 Nov 16:25, from seconds as text; "" if it is not a time */
static const char *update_when(const char *epoch)
{
    static char b[2][32];
    static int at;
    char *o = b[at = !at];
    time_t t = (time_t)atoll(epoch);
    struct tm tm;
    o[0] = 0;
    if (t <= 0) return o;
    localtime_r(&t, &tm);
    strftime(o, sizeof b[0], "%e %b %H:%M", &tm);
    return o + (o[0] == ' ');
}

static void page_update(struct req *r)
{
    struct update_info u;
    update_read(&u);
    int inst = !strcmp(u.installed, "yes"), pending = inst && update_pending(&u), busy = inst && (pending || update_busy(&u));
    int restart = !strcmp(u.state, "restart"), offer = !strcmp(u.update, "yes");
    int named = !strcmp(u.channel, "stable") || !strcmp(u.channel, "testing");
    const char *slot = !strcmp(u.slot, "a") ? ", slot A" : !strcmp(u.slot, "b") ? ", slot B" : "";

    use_phone_time();
    page_begin(r, "Update", "/update");
    if (!inst) {
        H("<div class='card'><dl class='facts'><dt>Version</dt><dd>%s</dd></dl><p class='dim'>Not installed</p></div>", u.version);
        page_end(r);
        return;
    }
    /* while something runs the page asks for itself again */
    if (busy) page_refresh(pending ? 1 : 3);

    H("<div class='card'><dl class='facts'><dt>Version</dt><dd>%s%r</dd>", u.version, slot);
    if (busy) H("<dt>Channel</dt><dd>%s</dd>", !strcmp(u.channel, "stable") ? "Stable" : !strcmp(u.channel, "testing") ? "Testing" : u.channel);
    H("<dt>Latest</dt><dd>");
    if (!u.latest[0]) H("Not checked");
    else H("%s%r%s", u.latest, update_when(u.checked)[0] ? ", checked " : "", update_when(u.checked));
    H("</dd>");
    const char *st = pending || !strcmp(u.state, "checking") ? "Checking" : !strcmp(u.state, "downloading") ? "Downloading" :
                     !strcmp(u.state, "writing") ? "Writing" : restart ? "Restart to finish" :
                     !strcmp(u.state, "failed") ? (u.message[0] ? u.message : "Failed") :
                     offer ? "Update available" : !strcmp(u.update, "no") ? "Up to date" : NULL;
    if (st)
        H("<dt>Status</dt><dd><span class='pill %r'>%s</span></dd>",
          busy ? "" : !strcmp(u.state, "failed") ? "bad" : restart || offer ? "warn" : "good", st);
    H("<dt>Last result</dt><dd>%s%r%s</dd></dl>", u.result[0] ? u.result : "None",
      u.result[0] && update_when(u.result_time)[0] ? ", " : "", u.result[0] ? update_when(u.result_time) : "");
    if (!busy) {
        H("<div class='actions'>");
        form_begin(r, "/update/check", "class='inline'");
        H("<button type='submit'%r>Check now</button></form>", offer && !restart ? "" : " class='primary'");
        if (offer && !restart) {
            form_begin(r, "/update/install", "class='inline'");
            H("<button class='primary' type='submit'>Install</button></form>");
        }
        H("</div>");
        if (restart) {
            form_begin(r, "/update/restart", "class='danger'");
            sure_box();
            H("<button type='submit' class='bad'>Restart</button></form>");
        }
        if (!strcmp(u.kernel, "differs")) {
            form_begin(r, "/update/kernel", "class='danger'");
            sure_box();
            H("<button type='submit' class='bad'>Install kernel</button><small class='dim'> A failed kernel needs recovery mode.</small></form>");
        }
    }
    H("</div>");

    if (!busy) {
        H("<div class='card'>");
        form_begin(r, "/update/channel", NULL);
        H("<div class='fields'><label class='field'><span>Channel</span><select name='channel'><option value='stable'%r>Stable</option>"
          "<option value='testing'%r>Testing</option><option value='custom'%r>Custom</option></select></label>",
          !strcmp(u.channel, "stable") ? " selected" : "", !strcmp(u.channel, "testing") ? " selected" : "", named ? "" : " selected");
        field("Custom URL", "url", named ? "" : u.channel, "type='url' autocapitalize='none' spellcheck='false' placeholder='https://'", NULL);
        H("</div><div class='actions'><button type='submit'>Save</button></div></form></div>");
    }
    page_end(r);
}

/* check, install and kernel take from seconds to minutes: started, left to
 * themselves, and watched by the page through "info" */
static void post_update_start(struct req *r)
{
    const char *verb = strrchr(r->path, '/') + 1;
    struct update_info u;
    update_read(&u);
    if (strcmp(u.installed, "yes")) flash(r, 1, "Not installed.");
    else if (update_busy(&u) || update_pending(&u)) flash(r, 1, "Update already running.");
    else if (!strcmp(verb, "kernel") && !sure(r)) flash(r, 1, UNSURE);
    else if (!strcmp(verb, "kernel") && strcmp(u.kernel, "differs")) flash(r, 1, "No kernel to install.");
    else if (!strcmp(verb, "install") && strcmp(u.update, "yes")) flash(r, 1, "No update to install.");
    else {
        char *argv[] = {(char *)wopt.update, (char *)verb, NULL};
        update_mark(&u, update_before, sizeof update_before);
        if (run_program(argv, 0, NULL, 0, 0) < 0) flash(r, 1, "Not started: %s.", strerror(errno));
        else { update_started = time(NULL); logmsg("update %s started", verb); }
    }
    redirect(r, "/update");
}

static void post_update_channel(struct req *r)
{
    char to[201], out[400];
    struct update_info u;
    int custom = !strcmp(form(r, "channel"), "custom");
    if (!line(r, custom ? "url" : "channel", to, sizeof to) ||
        (custom ? (strncmp(to, "https://", 8) && strncmp(to, "http://", 7)) || strchr(to, ' ') : strcmp(to, "stable") && strcmp(to, "testing"))) {
        flash(r, 1, "Not saved: a custom channel is an https:// or http:// URL.");
        redirect(r, "/update");
        return;
    }
    update_read(&u);
    if (update_busy(&u) || update_pending(&u)) { flash(r, 1, "Update already running."); redirect(r, "/update"); return; }
    /* the updater has the last word on what a channel may be, and says why not */
    if (update_command("channel", to, out, sizeof out, 10000) != 0) {
        out[strcspn(out, "\r\n")] = 0;
        flash(r, 1, "Not saved: %s.", out[0] ? out : "updater did not finish");
    } else flash(r, 0, "Channel saved.");
    redirect(r, "/update");
}

static void post_update_restart(struct req *r)
{
    struct update_info u;
    update_read(&u);
    if (!sure(r)) flash(r, 1, UNSURE);
    else if (strcmp(u.state, "restart")) flash(r, 1, "No update to restart for.");
    else {
        char *argv[] = {(char *)wopt.restart, NULL};
        logmsg("restart for the update");
        /* in a second, so that this answer gets back to the browser first */
        run_program(argv, 0, NULL, 0, 1);
        flash(r, 0, "Restarting.");
    }
    redirect(r, "/update");
}

/* ---------- which address is which page ---------- */

const struct route routes[] = {
    {"GET", "/", page_overview},
    {"GET", "/accounts", page_accounts},
    {"POST", "/accounts/save", post_account_save},
    {"POST", "/accounts/remove", post_account_remove},
    {"POST", "/accounts/default", post_account_default},
    {"POST", "/accounts/block", post_account_block},
    {"POST", "/accounts/ringtone", post_account_ringtone},
    {"POST", "/accounts/ringtone-remove", post_account_ringtone_remove},
    {"GET", "/network", page_network},
    {"POST", "/network", post_network},
    {"GET", "/time", page_time},
    {"POST", "/time", post_time},
    {"GET", "/sound", page_sound},
    {"POST", "/sound", post_sound},
    {"POST", "/screen", post_screen},
    {"POST", "/theme", post_theme},
    {"GET", "/contacts", page_contacts},
    {"POST", "/contacts/save", post_contact_save},
    {"POST", "/contacts/delete", post_contact_delete},
    {"POST", "/contacts/import", post_contacts_import},
    {"GET", "/contacts/export.csv", get_contacts_csv},
    {"GET", "/contacts/export.vcf", get_contacts_vcf},
    {"GET", "/history", page_history},
    {"POST", "/history/clear", post_history_clear},
    {"GET", "/apps", page_apps},
    {"POST", "/apps/install", post_app_change},
    {"POST", "/apps/update", post_app_change},
    {"POST", "/apps/remove", post_app_change},
    {"POST", "/apps/settings", post_app_settings},
    {"POST", "/apps/feed", post_app_feed},
    {"GET", "/access", page_access},
    {"POST", "/access/ssh", post_access_ssh},
    {"POST", "/access/key-add", post_key_add},
    {"POST", "/access/key-remove", post_key_remove},
    {"POST", "/access/keys-forget", post_keys_forget},
    {"POST", "/access/browsers", post_browsers},
    {"POST", "/access/web-off", post_web_off},
    {"GET", "/backup", page_backup},
    {"POST", "/backup/download", post_backup_download},
    {"POST", "/backup/restore", post_backup_restore},
    {"GET", "/update", page_update},
    {"POST", "/update/check", post_update_start},
    {"POST", "/update/install", post_update_start},
    {"POST", "/update/kernel", post_update_start},
    {"POST", "/update/channel", post_update_channel},
    {"POST", "/update/restart", post_update_restart},
    {NULL, NULL, NULL},
};
