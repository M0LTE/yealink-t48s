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

static int sure(const struct req *r) { return !strcmp(form(r, "sure"), "yes"); }

static void sure_box(const char *text)
{
    H("<label class='check'><input type='checkbox' name='sure' value='yes' required> %s</label>", text);
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

void page_overview(struct req *r)
{
    char now[80];
    time_t t = time(NULL);
    struct tm tm;
    int used = 0;

    page_begin(r, "Overview", "/");
    H("<div class='grid'>");

    H("<a class='card tile' href='/accounts'><h2>Phone accounts</h2>");
    for (int n = 1; n <= MAX_ACCTS; n++) {
        if (!account_used(n)) continue;
        const char *label = val(sip_file(n), "label", "");
        used++;
        H("<p><span class='pill %r'>%r</span> %s@%s %r%s%r</p>", account_on(n) ? "good" : "", account_on(n) ? "in use" : "off",
          val(sip_file(n), "user", ""), val(sip_file(n), "domain", ""), label[0] ? "<span class='dim'>(" : "", label, label[0] ? ")</span>" : "");
    }
    if (!used) H("<p class='dim'>None yet. The phone cannot make calls until it has one.</p>");
    H("</a>");

    H("<a class='card tile' href='/network'><h2>Network</h2><p>%s</p><p class='dim'>%s</p></a>",
      !strcmp(val("network", "mode", "dhcp"), "static") ? "A fixed address" : "Address from the network (DHCP)", web_url());

    use_phone_time();
    localtime_r(&t, &tm);
    strftime(now, sizeof now, "%H:%M, %A %e %B %Y", &tm);
    H("<a class='card tile' href='/time'><h2>Date and time</h2><p>%s</p><p class='dim'>%s</p></a>", now, val("system", "timezone", "UTC"));

    H("<a class='card tile' href='/sound'><h2>Sound and screen</h2><p>Ringer %s of 15</p><p class='dim'>Brightness %s of 10</p></a>",
      val("audio", "ringer", "10"), val("screen", "brightness", "10"));

    contacts_load();
    H("<a class='card tile' href='/contacts'><h2>Contacts</h2><p>%d in the list</p><p class='dim'>Add, change, import and export</p></a>", cn);
    H("<a class='card tile' href='/history'><h2>Calls</h2><p>%d in the history</p><p class='dim'>The last 50 are kept</p></a>", history_count());

    scan_apps();
    H("<a class='card tile' href='/apps'><h2>Apps</h2><p>%d installed</p><p class='dim'>Install, update, remove, settings</p></a>", napps);

    int ssh = !strcmp(val("access", "ssh", "off"), "on");
    H("<a class='card tile' href='/access'><h2>Access</h2><p><span class='pill %r'>SSH %r</span> %d key%r</p><p class='dim'>%d browser%r paired with this page</p></a>",
      ssh ? "good" : "", ssh ? "on" : "off", keys_count(), keys_count() == 1 ? "" : "s", sessions_count(), sessions_count() == 1 ? "" : "s");

    H("<a class='card tile' href='/backup'><h2>Backup</h2><p>Save the settings to a file</p><p class='dim'>Or put a saved file back</p></a>");
    H("</div>");
    page_end(r);
}

/* ---------- phone accounts ---------- */

static void page_accounts(struct req *r)
{
    int used = 0, def = atoi(val("phone", "default_account", "1"));
    page_begin(r, "Phone accounts", "/accounts");
    H("<p class='lead'>Up to six SIP accounts. All of them ring; calls go out from the usual one unless you choose another in the dialler.</p>");
    for (int n = 1; n <= MAX_ACCTS; n++) used += account_on(n);
    if (used > 1) {
        H("<div class='card'>");
        form_begin(r, "/accounts/default", "class='row'");
        H("<label class='field'><span>Calls usually go out from</span><select name='n'>");
        for (int n = 1; n <= MAX_ACCTS; n++)
            if (account_on(n))
                H("<option value='%d'%r>Account %d: %s@%s</option>", n, n == def ? " selected" : "", n, val(sip_file(n), "user", ""), val(sip_file(n), "domain", ""));
        H("</select></label><button type='submit'>Save</button></form></div>");
    }
    for (int n = 1; n <= MAX_ACCTS; n++) {
        const char *f = sip_file(n);
        int has = val(f, "user", "")[0] || val(f, "domain", "")[0];
        int pw = val(f, "password", "")[0] != 0;
        const char *tr = val(f, "transport", "udp");
        H("<details class='card' id='a%d'%r><summary><h2>Account %d</h2>", n, has || n == 1 ? " open" : "", n);
        if (account_used(n))
            H("<span class='sum'>%s@%s</span><span class='pill %r'>%r</span>", val(f, "user", ""), val(f, "domain", ""), account_on(n) ? "good" : "", account_on(n) ? "in use" : "off");
        else H("<span class='sum dim'>not set</span>");
        H("</summary>");
        form_begin(r, "/accounts/save", NULL);
        H("<input type='hidden' name='n' value='%d'><div class='fields'>", n);
        field("User", "user", val(f, "user", ""), "autocomplete='off' autocapitalize='none' spellcheck='false'", "The part before the @, often a number");
        field("Domain", "domain", val(f, "domain", ""), "autocomplete='off' autocapitalize='none' spellcheck='false'", "The part after the @: your provider or PBX");
        H("<label class='field'><span>Password</span><input type='password' name='password' value='' maxlength='200' autocomplete='new-password' placeholder='%r'>"
          "<small>Never shown. Leave it empty to keep the one that is set.</small></label>", pw ? "set: leave empty to keep it" : "not set");
        field("Server or proxy", "outbound_proxy", val(f, "outbound_proxy", ""), "autocapitalize='none' spellcheck='false'", "Optional: only if calls must go through a server other than the domain");
        field("Login name", "auth_user", val(f, "auth_user", ""), "autocomplete='off' autocapitalize='none' spellcheck='false'", "Optional: only if it is not the user");
        field("Name shown to others", "display_name", val(f, "display_name", ""), NULL, "Optional");
        H("<label class='field'><span>Transport</span><select name='transport'>");
        for (const char *const *t = (const char *const[]){"udp", "tcp", "tls", NULL}; *t; t++)
            H("<option%r>%s</option>", !strcmp(tr, *t) ? " selected" : "", *t);
        H("</select></label>");
        field("Name on this phone", "label", val(f, "label", ""), NULL, "Optional: a short name for the phone's own screens");
        field("Voicemail number", "voicemail", val(f, "voicemail", ""), "inputmode='tel'", "Optional: what the Message key calls");
        H("</div><label class='check'><input type='checkbox' name='enabled' value='on'%r> In use</label>"
          "<div class='actions'><button class='primary' type='submit'>Save account %d</button></div></form>",
          strcmp(val(f, "enabled", "on"), "off") ? " checked" : "", n);
        if (has) {
            form_begin(r, "/accounts/remove", "class='danger'");
            H("<input type='hidden' name='n' value='%d'>", n);
            sure_box("Forget this account's user, password and server");
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
    if (!ok) { flash(r, 1, "Account %d was not saved: something in it is too long.", n); redirect(r, where); return; }
    if (strcmp(tr, "udp") && strcmp(tr, "tcp") && strcmp(tr, "tls")) tr = "udp";
    for (int i = 0; i < 7 && ok; i++) ok = set(f, keys[i], v[i]) == 0;
    if (ok) ok = cfg_set(f, "transport", tr) == 0 && cfg_set(f, "enabled", !strcmp(form(r, "enabled"), "on") ? "on" : "off") == 0;
    if (ok && pw[0]) ok = cfg_set(f, "password", pw) == 0;
    memset(pw, 0, sizeof pw);
    if (!ok) { flash(r, 1, "Account %d was not saved: %s.", n, errno == EINVAL ? "there is something in it that cannot be stored" : strerror(errno)); redirect(r, where); return; }
    logmsg("account %d saved", n);
    run_apply("sip", 0, 0);
    flash(r, 0, "Account %d is saved. The phone service is starting again with it.", n);
    redirect(r, where);
}

static void post_account_remove(struct req *r)
{
    char path[400];
    int n = atoi(form(r, "n"));
    if (n < 1 || n > MAX_ACCTS || !sure(r)) { flash(r, 1, "Nothing was removed: tick the box first."); redirect(r, "/accounts"); return; }
    snprintf(path, sizeof path, "%s/%s", opt.config, sip_file(n));
    unlink(path);
    logmsg("account %d removed", n);
    run_apply("sip", 0, 0);
    flash(r, 0, "Account %d is removed.", n);
    redirect(r, "/accounts");
}

static void post_account_default(struct req *r)
{
    char v[4];
    int n = atoi(form(r, "n"));
    if (n >= 1 && n <= MAX_ACCTS && account_on(n)) {
        snprintf(v, sizeof v, "%d", n);
        cfg_set("phone", "default_account", v);
        flash(r, 0, "Calls now go out from account %d unless you choose another.", n);
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
    H("<label class='field'><span>Address from</span><select name='mode'><option value='dhcp'%r>The network (DHCP)</option>"
      "<option value='static'%r>The fixed address below</option></select></label><div class='fields'>", fixed ? "" : " selected", fixed ? " selected" : "");
    field("Address", "address", val("network", "address", ""), "inputmode='decimal' placeholder='192.168.1.50'", NULL);
    field("Netmask", "netmask", val("network", "netmask", "255.255.255.0"), "inputmode='decimal'", NULL);
    field("Gateway", "gateway", val("network", "gateway", ""), "inputmode='decimal'", "Optional");
    field("DNS server", "dns", val("network", "dns", ""), "inputmode='decimal'", "Optional");
    H("</div><p class='note'>The four boxes are only used with a fixed address. The phone is at <b>%s</b> now. "
      "If its address changes, this page moves with it: the new address is on the phone's screen under Settings, Network.</p>", web_url());
    H("<div class='actions'><button class='primary' type='submit'>Save and use it now</button></div></form></div>");
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
        flash(r, 1, "Not saved: a fixed address needs an address and a netmask like 192.168.1.50 and 255.255.255.0, and the gateway and DNS server, if given, must look the same.");
        redirect(r, "/network");
        return;
    }
    /* with DHCP the boxes are kept as they were unless what is in them is good */
    for (int i = 0; i < 4; i++)
        if (fixed || !a[i][0] || ipv4(a[i])) set("network", keys[i], a[i]);
    cfg_set("network", "mode", fixed ? "static" : "dhcp");
    /* in two seconds, so that this answer gets back to the browser first */
    run_apply("network", 0, 2);
    flash(r, 0, "Saved. The phone is changing its network settings now.");
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
    H("<div class='card'><p class='big'>%s</p><p class='dim'>The clock is set from the network (NTP).</p>", now);
    form_begin(r, "/time", NULL);
    H("<label class='field'><span>Time zone</span><select name='zone'>");
    for (int i = 0; i < NZONES; i++) {
        int here = !strcmp(cur, zones[i].name);
        listed |= here;
        H("<option%r>%s</option>", here ? " selected" : "", zones[i].name);
    }
    H("<option value='other'%r>Another, as a TZ string</option></select></label>", listed ? "" : " selected");
    field("TZ string", "custom", listed ? "" : cur, "autocapitalize='none' spellcheck='false' placeholder='XYZ-3'",
          "Only for a place the list lacks: the form the C library takes, such as CET-1CEST,M3.5.0,M10.5.0/3");
    H("<div class='actions'><button class='primary' type='submit'>Save</button></div></form></div>");
    page_end(r);
}

static void post_time(struct req *r)
{
    char z[64];
    int ok = line(r, strcmp(form(r, "zone"), "other") ? "zone" : "custom", z, sizeof z);
    for (char *p = z; *p; p++) if (*p == ' ') ok = 0;
    if (!ok || !z[0] || cfg_set("system", "timezone", z) < 0) flash(r, 1, "Not saved: choose a place, or give a TZ string with no spaces in it.");
    else flash(r, 0, "The time zone is %s. The phone's clock follows within a second.", z);
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
    level("Ringer", "ringer", val("audio", "ringer", "10"), 0, 15, "0 is silent");
    level("Handset", "handset", val("audio", "handset", "10"), 0, 15, NULL);
    level("Speakerphone", "speaker", val("audio", "speaker", "10"), 0, 15, NULL);
    level("Headset", "headset", val("audio", "headset", "10"), 0, 15, NULL);
    H("<p class='note'>Nothing is played when you save. The phone's volume keys change the same levels.</p>"
      "<div class='actions'><button class='primary' type='submit'>Save sound</button></div></form></div>");
    H("<div class='card'><h2>Screen</h2>");
    form_begin(r, "/screen", NULL);
    level("Brightness", "brightness", val("screen", "brightness", "10"), 1, 10, NULL);
    H("<div class='fields'>");
    field("Dim after (seconds)", "dim_seconds", val("screen", "dim_seconds", "120"), "inputmode='numeric' pattern='[0-9]*'", "Of nothing being touched. 0 is never");
    field("Off after (seconds)", "off_seconds", val("screen", "off_seconds", "1800"), "inputmode='numeric' pattern='[0-9]*'", "0 is never. Any key or a touch brings it back");
    H("</div><div class='actions'><button class='primary' type='submit'>Save screen</button></div></form></div>");
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
    static const char *const keys[] = {"ringer", "handset", "speaker", "headset"};
    long v[4];
    char b[8];
    for (int i = 0; i < 4; i++)
        if ((v[i] = whole(r, keys[i], 0, 15)) < 0) { flash(r, 1, "Not saved: levels are 0 to 15."); redirect(r, "/sound"); return; }
    for (int i = 0; i < 4; i++) { snprintf(b, sizeof b, "%ld", v[i]); cfg_set("audio", keys[i], b); }
    flash(r, 0, "The sound levels are saved.");
    redirect(r, "/sound");
}

static void post_screen(struct req *r)
{
    long b = whole(r, "brightness", 1, 10), dim = whole(r, "dim_seconds", 0, 86400), off = whole(r, "off_seconds", 0, 604800);
    char v[16];
    if (b < 0 || dim < 0 || off < 0) { flash(r, 1, "Not saved: brightness is 1 to 10, and the times are whole seconds."); redirect(r, "/sound"); return; }
    snprintf(v, sizeof v, "%ld", b); cfg_set("screen", "brightness", v);
    snprintf(v, sizeof v, "%ld", dim); cfg_set("screen", "dim_seconds", v);
    snprintf(v, sizeof v, "%ld", off); cfg_set("screen", "off_seconds", v);
    flash(r, 0, "The screen settings are saved.");
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
    H("<p class='lead'>Names for numbers: shown when they call, in the dialler and in the call history. Up to %d.</p>", MAX_CONTACTS);

    H("<div class='card' id='edit'><h2>%r</h2>", edit >= 0 ? "Change a contact" : "Add a contact");
    form_begin(r, "/contacts/save", NULL);
    if (edit >= 0) H("<input type='hidden' name='was_name' value='%s'><input type='hidden' name='was_number' value='%s'>", cl[edit].name, cl[edit].number);
    H("<div class='fields'>");
    H("<label class='field'><span>Name</span><input name='name' value='%s' maxlength='60' required></label>", edit >= 0 ? cl[edit].name : "");
    H("<label class='field'><span>Number</span><input name='number' value='%s' maxlength='60' inputmode='tel' required></label>", edit >= 0 ? cl[edit].number : "");
    H("</div><div class='actions'><button class='primary' type='submit'>%r</button>%r</div></form></div>",
      edit >= 0 ? "Save the change" : "Add", edit >= 0 ? " <a class='button quiet' href='/contacts'>Cancel</a>" : "");

    H("<div class='card'><h2>%d contact%r</h2>", cn, cn == 1 ? "" : "s");
    if (!cn) H("<p class='dim'>The list is empty.</p>");
    else H("<table class='list'><thead><tr><th>Name</th><th>Number</th><th></th></tr></thead><tbody>");
    for (int i = 0; i < cn; i++) {
        H("<tr><td>%s</td><td class='mono'>%s</td><td class='acts'><a class='button small' href='/contacts?edit=%d#edit'>Edit</a>", cl[i].name, cl[i].number, i);
        form_begin(r, "/contacts/delete", "class='inline'");
        H("<input type='hidden' name='name' value='%s'><input type='hidden' name='number' value='%s'>"
          "<button type='submit' class='small bad' data-ask='Delete %s?'>Delete</button></form></td></tr>", cl[i].name, cl[i].number, cl[i].name);
    }
    if (cn) H("</tbody></table>");
    H("</div>");

    H("<div class='card'><h2>Import and export</h2><p>A CSV file has one contact a line, the name and then the number: <span class='mono'>Alice,1001</span>. "
      "A vCard file (.vcf) is what address books and mobile phones export.</p>");
    H("<div class='actions'><a class='button' href='/contacts/export.csv'>Download as CSV</a> <a class='button' href='/contacts/export.vcf'>Download as vCard</a></div>");
    form_begin(r, "/contacts/import", "enctype='multipart/form-data' class='sub'");
    H("<label class='field'><span>A CSV or vCard file to add to the list</span><input type='file' name='file' accept='.csv,.vcf,.txt,text/csv,text/vcard' required></label>"
      "<label class='check'><input type='checkbox' name='replace' value='yes'> Empty the list first, so that it holds only what is in the file</label>"
      "<div class='actions'><button type='submit'>Import</button></div></form></div>");
    page_end(r);
}

static void post_contact_save(struct req *r)
{
    char name[61], number[61];
    struct contact c;
    contacts_load();
    if (!line(r, "name", name, sizeof name) || !line(r, "number", number, sizeof number) || !contact_put(&c, name, number)) {
        flash(r, 1, "Not saved: a contact needs a name and a number, each up to 60 characters.");
        redirect(r, "/contacts");
        return;
    }
    int at = form(r, "was_number")[0] ? contact_find(form(r, "was_name"), form(r, "was_number")) : -1;
    if (at >= 0) cl[at] = c;
    else if (cn >= MAX_CONTACTS) { flash(r, 1, "Not saved: the list is full (%d).", MAX_CONTACTS); redirect(r, "/contacts"); return; }
    else if (contact_find(c.name, c.number) < 0) cl[cn++] = c;
    if (contacts_save() < 0) flash(r, 1, "Not saved: %s.", strerror(errno));
    else flash(r, 0, "%s is saved.", c.name);
    redirect(r, "/contacts");
}

static void post_contact_delete(struct req *r)
{
    contacts_load();
    int at = contact_find(form(r, "name"), form(r, "number"));
    if (at < 0) { flash(r, 1, "That contact is not in the list any more."); redirect(r, "/contacts"); return; }
    struct contact was = cl[at];
    memmove(cl + at, cl + at + 1, sizeof cl[0] * (size_t)(cn - at - 1));
    cn--;
    if (contacts_save() < 0) flash(r, 1, "Not deleted: %s.", strerror(errno));
    else flash(r, 0, "%s is deleted.", was.name);
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
    if (!added && !seen) flash(r, 1, "Nothing was imported: the file has no contacts this phone can read. It wants a CSV file of name,number lines, or a vCard file.");
    else if (!added) flash(r, 1, "Nothing was imported: the %d line%s in the file were already in the list, had no number, or there was no room.", seen, seen == 1 ? "" : "s");
    else if (contacts_save() < 0) flash(r, 1, "Not imported: %s.", strerror(errno));
    else flash(r, 0, "%d contact%s added. %d in the file were left out: already there, no number, or no room.", added, added == 1 ? "" : "s", seen - added);
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
    if (!n) H("<p class='dim'>No calls yet. The phone keeps the last 50.</p>");
    else H("<table class='list'><thead><tr><th>When</th><th></th><th>Who</th><th>Length</th><th>Account</th></tr></thead><tbody>");
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
          when, k == 'm' ? "bad" : k == 'o' ? "" : "good", k == 'm' ? "missed" : k == 'o' ? "out" : "in", name, name[0] ? " " : "", fld[3], len);
        if (acct >= 1 && acct <= MAX_ACCTS) {
            const char *label = val(sip_file(acct), "label", "");
            H("%s", label[0] ? label : val(sip_file(acct), "user", ""));
        }
        H("</td></tr>");
    }
    if (n) {
        H("</tbody></table>");
        form_begin(r, "/history/clear", "class='danger'");
        sure_box("Forget every call in the list");
        H("<button type='submit' class='bad'>Clear the history</button></form>");
    }
    H("</div>");
    free(t);
    page_end(r);
}

static void post_history_clear(struct req *r)
{
    char path[300], keep[64] = "";
    if (!sure(r)) { flash(r, 1, "Nothing was cleared: tick the box first."); redirect(r, "/history"); return; }
    snprintf(path, sizeof path, "%s/history", opt.state);
    char *t = load_file(path, 64 * 1024);
    /* the line that says which missed calls have been looked at stays */
    if (t && !strncmp(t, "seen ", 5)) snprintf(keep, sizeof keep, "%.*s\n", (int)strcspn(t, "\r\n"), t);
    free(t);
    mkdir(opt.state, 0755);
    if (save_file(path, keep, 0644) < 0) flash(r, 1, "Not cleared: %s.", strerror(errno));
    else flash(r, 0, "The call history is empty.");
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
              m->setting_label[k], name, val(file, key, "")[0] ? "set: leave empty to keep it" : "not set");
        else
            H("<label class='field'><span>%s</span><input name='%s' value='%s' maxlength='200' %r></label>", m->setting_label[k], name, v,
              !strcmp(type, "number") ? "inputmode='decimal'" : "");
    }
    H("</div><div class='actions'><button class='primary' type='submit'>Save settings</button> "
      "<button type='submit' name='defaults' value='yes' formnovalidate>Back to the app's own defaults</button></div></form></details>");
}

static void page_apps(struct req *r)
{
    static char out[32 * 1024];
    char err[200] = "";
    int shown = 0;
    app_command("list", NULL, out, sizeof out, 25000);
    scan_apps();
    page_begin(r, "Apps", "/apps");
    H("<p class='lead'>What is on the phone, and what the feed offers. An app that is open on the phone is closed when it is changed here.</p>");
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
          !strcmp(st, "available") ? "not installed" : !strcmp(st, "update") ? "update ready" : !strcmp(st, "unsupported") ? "needs newer firmware" :
          !strcmp(st, "builtin") ? "part of the firmware" : "installed");
        if (strcmp(f[9], "-")) H("<p>%s</p>", f[9]);
        H("<p class='dim small'>");
        if (strcmp(f[3], "-")) H("Version %s on the phone. ", f[3]);
        if (strcmp(f[4], "-")) H("Version %s in the feed. ", f[4]);
        H("About %l KB. %r%s</p><div class='actions'>", kb, strcmp(f[8], "-") ? "May use: " : "", strcmp(f[8], "-") ? f[8] : "");
        if (!strcmp(st, "available") || !strcmp(st, "update")) {
            form_begin(r, !strcmp(st, "update") ? "/apps/update" : "/apps/install", "class='inline'");
            H("<input type='hidden' name='id' value='%s'><button class='primary' type='submit'>%r</button></form>", id, !strcmp(st, "update") ? "Update" : "Install");
        }
        if (!strcmp(st, "installed") || !strcmp(st, "update")) {
            form_begin(r, "/apps/remove", "class='inline'");
            H("<input type='hidden' name='id' value='%s'><button type='submit' class='bad' data-ask='Remove %s? What it kept and its settings go too.'>Remove</button></form>", id, f[2]);
        }
        H("</div>");
        if (m && m->nsettings) app_settings_form(r, m);
        H("</div>");
    }
    if (!shown) H("<div class='card'><p class='dim'>No apps are installed.</p></div>");
    if (err[0]) H("<p class='flash bad'>The feed could not be read: %s</p>", err);

    H("<div class='card'><h2>The feed</h2><p>Where the list of apps comes from. It must be an https address.</p>");
    form_begin(r, "/apps/feed", NULL);
    H("<label class='field'><span>Feed address</span><input name='url' type='url' value='%s' maxlength='200' placeholder='%s'><small>Empty is the standard feed.</small></label>"
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
    if (st == 0) flash(r, 0, "%s: %s.", id, !strcmp(verb, "remove") ? "removed" : !strcmp(verb, "update") ? "updated" : "installed");
    else flash(r, 1, "%s was not %s: %s.", id, !strcmp(verb, "remove") ? "removed" : !strcmp(verb, "update") ? "updated" : "installed",
               why[0] ? why : "the app manager did not finish");
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
        flash(r, 0, "%s is back to its own defaults.", m->name);
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
    if (bad) flash(r, 1, "Some of %s's settings were not saved: too long, or with something in them that cannot be stored.", m->name);
    else flash(r, 0, "%s's settings are saved. It uses them the next time it is opened.", m->name);
    redirect(r, "/apps");
}

static void post_app_feed(struct req *r)
{
    char u[201];
    if (!line(r, "url", u, sizeof u) || (u[0] && strncmp(u, "https://", 8) && strncmp(u, "file://", 7)) || strchr(u, ' ')) flash(r, 1, "Not saved: the feed address must start with https://");
    else { set("feed", "url", u); flash(r, 0, u[0] ? "The feed address is saved." : "The phone uses the standard feed."); }
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

static void page_access(struct req *r)
{
    struct key k[MAX_KEYS];
    char *text;
    int n = keys_load(k, MAX_KEYS, &text);
    int dev = !strcmp(access_status("dev", "no"), "yes");
    int on = dev || !strcmp(val("access", "ssh", "off"), "on");
    char host[64];
    snprintf(host, sizeof host, "%s", web_url()[0] ? web_url() + 7 : "the-phone");
    host[strcspn(host, ":/")] = 0;

    page_begin(r, "Access", "/access");
    H("<div class='card'><div class='apphead'><h2>SSH</h2><span class='pill %r'>%r</span></div>", on ? "good" : "", on ? "on" : "off");
    H("<p>A command line on the phone, as root, with a key and never a password. It gives full control of the phone.</p>");
    if (dev) H("<p class='note'>This is a development build: SSH is always on, and the keys built into the image can always log in.</p>");
    if (on) H("<p>Log in with <span class='mono'>ssh root@%s</span></p>", host);
    H("<p class='dim small'>This phone's host key: <span class='mono'>%s</span><br>Check it against what ssh shows the first time you log in.</p>", access_status("host_key", "not known until SSH has been on once"));
    if (!dev) {
        form_begin(r, "/access/ssh", NULL);
        H("<input type='hidden' name='to' value='%r'><div class='actions'><button type='submit' class='%r'>Turn SSH %r</button></div></form>",
          on ? "off" : "on", on ? "" : "primary", on ? "off" : "on");
    }
    H("</div>");

    H("<div class='card'><h2>Keys that can log in</h2>");
    if (!n) H("<p class='dim'>None yet%r</p>", on ? ": nobody can log in." : ".");
    else H("<table class='list'><tbody>");
    for (int i = 0; i < n; i++) {
        H("<tr><td><span class='mono'>%s</span></td><td>%s<br><span class='dim mono small'>...%s</span></td><td class='acts'>", k[i].type,
          k[i].comment[0] ? k[i].comment : "(no name)", k[i].blob + (strlen(k[i].blob) > 16 ? strlen(k[i].blob) - 16 : 0));
        form_begin(r, "/access/key-remove", "class='inline'");
        H("<input type='hidden' name='key' value='%s'><button type='submit' class='small bad' data-ask='Remove this key?'>Remove</button></form></td></tr>", k[i].blob);
    }
    if (n) H("</tbody></table>");
    form_begin(r, "/access/key-add", "class='sub'");
    H("<label class='field wide'><span>Add a public key</span><textarea name='keys' rows='4' spellcheck='false' autocapitalize='none' "
      "placeholder='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAA... you@computer' required></textarea>"
      "<small>Paste the line from <span class='mono'>~/.ssh/id_ed25519.pub</span> on your computer (<span class='mono'>cat ~/.ssh/id_ed25519.pub</span> prints it). "
      "A public key is not a secret. Several lines add several keys.</small></label>"
      "<div class='actions'><button class='primary' type='submit'>Add</button></div></form>");
    if (n) {
        form_begin(r, "/access/keys-forget", "class='danger'");
        sure_box(dev ? "Forget the keys that were added (the ones built into this image stay)" : "Forget every key: nobody can log in until one is added again");
        H("<button type='submit' class='bad'>Forget all keys</button></form>");
    }
    H("</div>");
    free(text);

    H("<div class='card'><h2>This web page</h2><p>%d browser%r paired, this one included. A browser stays paired until it has not been used for twelve hours, "
      "the phone restarts, or the page is turned off.</p>", sessions_count(), sessions_count() == 1 ? " is" : "s are");
    if (sessions_count() > 1) {
        form_begin(r, "/access/browsers", NULL);
        H("<div class='actions'><button type='submit'>Sign out the other browsers</button></div></form>");
    }
    form_begin(r, "/access/web-off", "class='danger'");
    sure_box("Turn this page off. It can only be turned on again at the phone: Settings, Access");
    H("<button type='submit' class='bad'>Turn the web page off</button></form></div>");
    page_end(r);
}

static void post_access_ssh(struct req *r)
{
    int on = !strcmp(form(r, "to"), "on");
    cfg_set("access", "ssh", on ? "on" : "off");
    logmsg("SSH turned %s", on ? "on" : "off");
    run_apply("access", 8000, 0);
    flash(r, 0, on ? "SSH is on." : "SSH is off, and anyone who was logged in has been cut off.");
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
    else if (refused) flash(r, 1, "%d key%s added. %d line%s refused: a public key is one line that starts with its kind, such as ssh-ed25519, then the key, then a name. The list holds %d.",
                            added, added == 1 ? "" : "s", refused, refused == 1 ? " was" : "s were", MAX_KEYS);
    else if (!added) flash(r, 1, "Nothing was added: that key is already in the list.");
    else flash(r, 0, "%d key%s added.%s", added, added == 1 ? "" : "s", !strcmp(val("access", "ssh", "off"), "on") ? "" : " SSH is off: turn it on to use it.");
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
    if (at < 0) flash(r, 1, "That key is not in the list any more.");
    else {
        memmove(k + at, k + at + 1, sizeof k[0] * (size_t)(n - at - 1));
        if (keys_save(k, n - 1) < 0) flash(r, 1, "Not removed: %s.", strerror(errno));
        else { flash(r, 0, "The key is removed. Someone logged in with it now stays logged in until they leave."); logmsg("an SSH key removed"); run_apply("access-status", 4000, 0); }
    }
    free(text);
    redirect(r, "/access");
}

static void post_keys_forget(struct req *r)
{
    if (!sure(r)) flash(r, 1, "Nothing was forgotten: tick the box first.");
    else {
        /* t48-access empties the list too, and puts a development build's own keys back */
        keys_save(NULL, 0);
        logmsg("SSH keys forgotten");
        run_apply("access-forget-keys", 4000, 0);
        flash(r, 0, "The keys are forgotten.");
    }
    redirect(r, "/access");
}

static void post_browsers(struct req *r)
{
    sessions_forget_others(r->s);
    flash(r, 0, "The other browsers are signed out.");
    redirect(r, "/access");
}

static void post_web_off(struct req *r)
{
    if (!sure(r)) { flash(r, 1, "Nothing was changed: tick the box first."); redirect(r, "/access"); return; }
    cfg_set("access", "web", "off");
    logmsg("turned off from the page itself");
    run_apply("access", 0, 1);
    page_reset();
    H("<!doctype html><html lang='en'><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>T48S</title>"
      "<link rel='stylesheet' href='/style.css'><main class='lone'><div class='card'><h1>The web page is off</h1>"
      "<p>To use it again, turn it on at the phone: Settings, Access, Web setup page.</p></div></main></html>");
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
    H("<div class='card'><h2>Save the settings to a file</h2><p>The phone accounts, network, time zone, sound, screen, the feed address, apps' settings and the contact list, in one text file you can read.</p>"
      "<p class='dim small'>Not in it: whether SSH and this page are on, the SSH keys, the call history, and the apps themselves.</p>");
    form_begin(r, "/backup/download", NULL);
    H("<label class='check'><input type='checkbox' name='passwords' value='yes'> Include the passwords. The file is then marked as sensitive: keep it somewhere safe</label>"
      "<div class='actions'><button class='primary' type='submit'>Download the backup</button></div></form></div>");
    H("<div class='card'><h2>Put a saved file back</h2><p>Every part that is in the file replaces what the phone has. A file saved without passwords keeps the passwords the phone has now.</p>");
    form_begin(r, "/backup/restore", "enctype='multipart/form-data'");
    H("<label class='field'><span>A backup file from this page</span><input type='file' name='file' accept='.txt,.t48backup,text/plain' required></label>");
    sure_box("Replace this phone's settings with the file's");
    H("<div class='actions'><button type='submit' class='bad'>Restore</button></div></form></div>");
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
    if (passwords) H("# SENSITIVE: this file holds passwords. Keep it somewhere safe, and delete it when it is no longer needed.\n");
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
    if (!sure(r)) { flash(r, 1, "Nothing was restored: tick the box first."); goto out; }
    if (!text || strncmp(text, BACKUP_FIRST, strlen(BACKUP_FIRST))) { flash(r, 1, "Nothing was restored: that is not a backup file from this page."); goto out; }
    scan_apps();
    /* read all of it and check all of it before anything on the phone is touched */
    for (char *p = text; p && *p && !why[0];) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        p[strcspn(p, "\r")] = 0;
        ln++;
        for (char *q = p; *q; q++) if ((unsigned char)*q < 32 || *q == 127) snprintf(why, sizeof why, "line %d has something in it that is not text", ln);
        if (ended && p[0]) snprintf(why, sizeof why, "there is something after the end mark");
        else if (!strcmp(p, "[end]")) ended = 1;
        else if (p[0] == '[') {
            size_t l = strlen(p);
            if (l < 3 || p[l - 1] != ']' || ns >= 64) snprintf(why, sizeof why, "line %d is not a part this phone knows", ln);
            else {
                p[l - 1] = 0;
                if (!backup_name_ok(p + 1)) snprintf(why, sizeof why, "line %d names a part this phone does not know", ln);
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
            if (!contacts && !setting_line_ok(p)) snprintf(why, sizeof why, "line %d is not a setting", ln);
            else if (s[cur].len + l + 400 > (contacts ? 20000 : 15000)) snprintf(why, sizeof why, "the part %s is too big", s[cur].name);
            else if (l) { memcpy(s[cur].text + s[cur].len, p, l); s[cur].len += l; s[cur].text[s[cur].len++] = '\n'; }
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!why[0] && !ended) snprintf(why, sizeof why, "it stops before its end mark, so part of it is missing");
    if (why[0]) { flash(r, 1, "Nothing was restored: %s.", why); goto out; }

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
    flash(r, restored != ns, "%d of the file's %d parts are restored.%s", restored, ns, net ? " The phone is changing its network settings now." : "");
out:
    for (int i = 0; i < ns; i++) { free(s[i].text); s[i].text = NULL; }
    if (text) memset(text, 0, strlen(text));
    free(text);
    redirect(r, "/backup");
}

/* ---------- which address is which page ---------- */

const struct route routes[] = {
    {"GET", "/", page_overview},
    {"GET", "/accounts", page_accounts},
    {"POST", "/accounts/save", post_account_save},
    {"POST", "/accounts/remove", post_account_remove},
    {"POST", "/accounts/default", post_account_default},
    {"GET", "/network", page_network},
    {"POST", "/network", post_network},
    {"GET", "/time", page_time},
    {"POST", "/time", post_time},
    {"GET", "/sound", page_sound},
    {"POST", "/sound", post_sound},
    {"POST", "/screen", post_screen},
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
    {NULL, NULL, NULL},
};
