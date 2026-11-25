// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the phone (issue #26). The dialler, the call screens, call
 * history, and what the hook, Speakerphone, Headset, Mute, Hold, Redial,
 * Transfer and Volume keys do. See docs/phone.md.
 *
 * The call itself lives in baresip and the sound in the audio service; this
 * file is the glue and the face. It tells baresip what to do over sipc.c,
 * tells the audio service where the sound goes over audioc.c, and follows
 * baresip's events.
 *
 * Issue #27 added the rest of a desk phone: a second call (one that is
 * waiting, or one on hold while the owner talks to the other), asking first
 * before a transfer, do not disturb, the message lamp, and names from the
 * contact list (contacts.c). Issue #36 added several accounts: which one a
 * call goes out from, which one was called, and each one's registration.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "shell.h"

enum ph { PH_IDLE, PH_DIAL, PH_OUTGOING, PH_INCOMING, PH_INCALL, PH_ENDED };
static const char *const ph_names[] = {"idle", "dial", "outgoing", "incoming", "incall", "ended"};
static const char *const route_names[] = {"none", "handset", "speaker", "headset"};
static const char *const route_labels[] = {"On hook", "Handset", "Speaker", "Headset"};
static const char *const vol_labels[] = {"", "Handset", "Speaker", "Headset", "Ringer", "Apps"};
static const char *const vol_keys[] = {"", "handset", "speaker", "headset", "ringer", "apps"};   /* in the audio settings file */

enum reg { REG_NONE, REG_TRYING, REG_OK, REG_FAIL };

#define ENDED_MS 2500          /* how long "Call ended" stays up */
#define ENDED_TONE_MS 30000    /* a failed call's tone, if nobody hangs up */
#define SETTLE_MS 3000         /* after start: time for baresip and the audio service to say how things stand */
#define VOL_SHOW_MS 1500
#define HIST_MAX 50
#define HIST_ROWS 6

#define MAX_ACCTS 6

/* one call, as baresip has it */
struct call {
    char id[160];              /* baresip's id for the call, "" before it has one */
    char peer[96], name[64];
    int acct;                  /* which account it is on, 0 to MAX_ACCTS - 1 */
    int outgoing, answered, ringing, rejected;
    int held, remote_held;
    long long t_established;
    char dtmf[24];             /* tones sent in this call, as typed */
};

enum other { O_NONE, O_WAITING, O_HELD };
static const char *const other_names[] = {"none", "waiting", "held"};

static struct {
    enum ph st;
    int hook_off;
    int route;                 /* T48AC_ROUTE_*: where we have asked for the sound to go */
    int tone, ring;            /* what we have asked the audio service to play */
    char number[64];           /* being typed: a number to call, or to transfer to */
    struct call c;             /* the call the owner is on, or is making or being offered */
    struct call o;             /* a second call: ringing while the first goes on, or on hold */
    enum other o_st;
    int consult;               /* the current call is to ask someone before sending them the held one */
    int joining;               /* the two calls have been told to talk to each other */
    int parked;                /* the call is on hold because the handset went down with nowhere for the sound to go */
    int dial_acct;             /* the account the next call goes out from */
    int dnd;                   /* do not disturb */
    int muted;
    int keypad;                /* in a call: the tone keypad is showing */
    int xfer;                  /* in a call: typing a number to transfer to */
    char note[64];             /* a line for the call screen: "Transfer failed" */
    long long t_state;
    char ended[64], ended_detail[64];
    int ended_ms;
    int vol[T48AC_VOL_COUNT];
    int vol_shown;             /* which volume the bar is showing */
    long long vol_until;
    long long started;
    int settled, audio_seen;
    int missed;
    int hist_sel, hist_confirm;
} P;

struct hist { long long when; char kind; int secs; char number[64], name[64]; int acct; /* 1 to 6, 0 not known */ };
static struct hist hist[HIST_MAX];   /* newest first */
static int nhist;
static long long hist_seen;          /* missed calls before this have been looked at */

/* the accounts: /data/config/sip is the first, sip2 to sip6 the others */
struct acct {
    int used;                  /* has a user and a domain */
    int on;                    /* used, and not switched off */
    char user[64], domain[64], label[32], voicemail[40];
    char aor[140];             /* "sip:user@domain", as baresip names it */
    enum reg reg;
    char why[64];
    int msgs;                  /* new voice messages; -1: some, the server did not say how many */
};
static struct acct A[MAX_ACCTS];
static int def_acct;

/* ---------- small things ---------- */

static void update_lamps(void);

static void changed(void) { shell_phone_changed(); }

int phone_messages(void);
static void set_tone(int t) { P.tone = t; audioc_tone(t); }
static void set_ring(int on) { P.ring = on; audioc_ring(on); }

static void set_route(int r)
{
    if (r != P.route) logmsg("phone: sound to %s", route_names[r]);
    P.route = r;
    if (r == T48AC_ROUTE_NONE) P.muted = 0;   /* the service clears mute when the phone goes on hook */
    audioc_route(r);
}

static void update_lamps(void)
{
    leds_set(LAMP_SPEAKER, P.route == T48AC_ROUTE_SPEAKER ? LAMP_ON : LAMP_OFF);
    leds_set(LAMP_HEADSET, P.route == T48AC_ROUTE_HEADSET ? LAMP_ON : LAMP_OFF);
    leds_set(LAMP_MUTE, P.muted ? LAMP_ON : LAMP_OFF);
    /* the lamp at the top right, as Yealink has it: fast for a call ringing, slow for voicemail, on for missed calls */
    leds_set(LAMP_MESSAGE, P.st == PH_INCOMING || P.o_st == O_WAITING ? LAMP_BLINK :
                           phone_messages() ? LAMP_SLOW : P.missed ? LAMP_ON : LAMP_OFF);
}

static void set_state(enum ph st)
{
    if (st != P.st) logmsg("phone: %s", ph_names[st]);
    P.st = st;
    P.t_state = t48_now_ms();
    update_lamps();
    shell_phone_screen(st != PH_IDLE);
}

/* "sip:1002@example.com;transport=udp" as the owner would say it: "1002" */
static void uri_user(const char *uri, char *out, size_t n)
{
    const char *p = uri;
    if (!strncmp(p, "sip:", 4)) p += 4;
    else if (!strncmp(p, "sips:", 5)) p += 5;
    size_t k = strcspn(p, "@;>");
    snprintf(out, n, "%.*s", (int)k, p);
}

static int route_for_now(void)
{
    if (P.route != T48AC_ROUTE_NONE) return P.route;
    return P.hook_off ? T48AC_ROUTE_HANDSET : T48AC_ROUTE_SPEAKER;
}

/* ---------- settings and history files ---------- */

static const char *acct_file(int i)
{
    static char f[8];
    if (i == 0) return "sip";
    snprintf(f, sizeof f, "sip%d", i + 1);
    return f;
}

static int accts_on(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ACCTS; i++) n += A[i].on;
    return n;
}

/* what the owner calls an account: its label, or its user */
static const char *acct_label(int i)
{
    if (i < 0 || i >= MAX_ACCTS || !A[i].used) return "";
    return A[i].label[0] ? A[i].label : A[i].user;
}

/* the account baresip means by "sip:user@domain;..." */
static int acct_by_aor(const char *aor)
{
    int only = -1, n = 0;
    for (int i = 0; i < MAX_ACCTS; i++) {
        size_t k = strlen(A[i].aor);
        if (!A[i].used) continue;
        if (!strncmp(aor, A[i].aor, k) && (!aor[k] || aor[k] == ';')) return i;
        if (A[i].on) { only = i; n++; }
    }
    return n == 1 ? only : -1;
}

/* the accounts, for the screens. The passwords in these files are baresip's business, not ours. */
static void load_accounts(void)
{
    char v[64];
    for (int i = 0; i < MAX_ACCTS; i++) {
        struct acct was = A[i], *a = &A[i];
        const char *f = acct_file(i);
        memset(a, 0, sizeof *a);
        cfg_get(f, "user", a->user, sizeof a->user);
        cfg_get(f, "domain", a->domain, sizeof a->domain);
        cfg_get(f, "label", a->label, sizeof a->label);
        cfg_get(f, "voicemail", a->voicemail, sizeof a->voicemail);
        a->used = a->user[0] && a->domain[0];
        v[0] = 0;
        cfg_get(f, "enabled", v, sizeof v);
        a->on = a->used && strcmp(v, "off") && strcmp(v, "no") && strcmp(v, "false") && strcmp(v, "0");
        if (a->used) snprintf(a->aor, sizeof a->aor, "sip:%s@%s", a->user, a->domain);
        if (a->on && !strcmp(a->aor, was.aor)) {   /* the same account as before: what we know of it still holds */
            a->reg = was.reg;
            a->msgs = was.msgs;
            snprintf(a->why, sizeof a->why, "%s", was.why);
        }
    }
    def_acct = atoi(cfg_str("phone", "default_account", "1")) - 1;
    if (def_acct < 0 || def_acct >= MAX_ACCTS || !A[def_acct].on) {
        def_acct = 0;
        for (int i = MAX_ACCTS - 1; i >= 0; i--) if (A[i].on) def_acct = i;
    }
    if (P.dial_acct < 0 || P.dial_acct >= MAX_ACCTS || !A[P.dial_acct].on) P.dial_acct = def_acct;
}

static void load_settings(void)
{
    char path[300], line[200];
    FILE *f;

    for (int i = 1; i < T48AC_VOL_COUNT; i++) P.vol[i] = 10;
    P.vol[T48AC_VOL_RINGER] = 13;   /* the audio service's default: 4 dB under the loudspeaker's limit (docs/audiod.md) */
    P.vol[T48AC_VOL_APP] = 12;      /* likewise: a full-scale sample 6 dB under it */
    snprintf(path, sizeof path, "%s/audio", opt.config);
    if ((f = fopen(path, "r"))) {
        while (fgets(line, sizeof line, f)) {
            int v;
            for (int i = 1; i < T48AC_VOL_COUNT; i++) {
                const char *name = vol_keys[i];
                size_t k = strlen(name);
                if (!strncmp(line, name, k) && line[k] == '=' && sscanf(line + k + 1, "%d", &v) == 1 && v >= 0 && v <= T48AC_VOL_MAX)
                    P.vol[i] = v;
            }
        }
        fclose(f);
    }
    load_accounts();
    P.dnd = !strcmp(cfg_str("phone", "dnd", "off"), "on");
}

void phone_reload_account(void)
{
    load_accounts();
    P.dial_acct = def_acct;
    update_lamps();
    changed();
}

/* the settings files have been changed by something else (watch.c): the web
 * page, or t48-config. Volumes that differ are told to the audio service;
 * nothing is played. */
void phone_reload_files(void)
{
    int was[T48AC_VOL_COUNT];
    memcpy(was, P.vol, sizeof was);
    load_settings();
    for (int i = 1; i < T48AC_VOL_COUNT; i++)
        if (P.vol[i] != was[i]) audioc_volume(i, P.vol[i]);
    update_lamps();
    changed();
}

static void save_volumes(void);

int phone_volume(int which) { return which > 0 && which < T48AC_VOL_COUNT ? P.vol[which] : 0; }

void phone_set_volume(int which, int level)
{
    if (which <= 0 || which >= T48AC_VOL_COUNT || level < 0 || level > T48AC_VOL_MAX || P.vol[which] == level) return;
    P.vol[which] = level;
    audioc_volume(which, level);
    save_volumes();
}

static void save_volumes(void)
{
    for (int i = 1; i < T48AC_VOL_COUNT; i++) {
        char v[8];
        snprintf(v, sizeof v, "%d", P.vol[i]);
        cfg_set("audio", vol_keys[i], v);
    }
}

static void count_missed(void)
{
    P.missed = 0;
    for (int i = 0; i < nhist; i++)
        if (hist[i].kind == 'm' && hist[i].when > hist_seen) P.missed++;
}

/* one call a line, newest last: when|kind|seconds|number|name|account. kind
 * is o (outgoing), i (incoming, answered or turned away) or m (missed). The
 * account (1 to 6) was added for issue #36; older lines have none. */
static void history_load(void)
{
    char path[300], line[300];
    snprintf(path, sizeof path, "%s/history", opt.state);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        struct hist h = {0};
        char *fld[6] = {0}, *p = line;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "seen %lld", &hist_seen) == 1) continue;
        for (int i = 0; i < 6 && p; i++) {
            fld[i] = p;
            p = strchr(p, '|');
            if (p) *p++ = 0;
        }
        if (!fld[3] || !fld[3][0] || !fld[1][0]) continue;
        h.when = atoll(fld[0]);
        h.kind = fld[1][0];
        h.secs = atoi(fld[2]);
        snprintf(h.number, sizeof h.number, "%s", fld[3]);
        snprintf(h.name, sizeof h.name, "%s", fld[4] ? fld[4] : "");
        h.acct = fld[5] ? atoi(fld[5]) : 0;
        memmove(hist + 1, hist, sizeof hist[0] * (HIST_MAX - 1));
        hist[0] = h;
        if (nhist < HIST_MAX) nhist++;
    }
    fclose(f);
    count_missed();
}

/* the history file has been changed by something else: the web page clearing it */
void phone_reload_history(void)
{
    nhist = 0;
    history_load();
    count_missed();
    if (P.hist_sel >= nhist) P.hist_sel = nhist ? nhist - 1 : 0;
    update_lamps();
    changed();
}

static void history_save(void)
{
    char path[300], tmp[310];
    snprintf(path, sizeof path, "%s/history", opt.state);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    shell_mkdirs(opt.state);
    FILE *f = fopen(tmp, "w");
    if (!f) { logmsg("phone: cannot write %s", tmp); return; }
    fprintf(f, "seen %lld\n", hist_seen);
    for (int i = nhist - 1; i >= 0; i--)
        fprintf(f, "%lld|%c|%d|%s|%s|%d\n", hist[i].when, hist[i].kind, hist[i].secs, hist[i].number, hist[i].name, hist[i].acct);
    if (fclose(f) == 0) rename(tmp, path);
}

static void history_add(char kind, const char *number, const char *name, int secs, int acct)
{
    struct hist h = {.when = (long long)time(NULL), .kind = kind, .secs = secs, .acct = acct + 1};
    snprintf(h.number, sizeof h.number, "%s", number[0] ? number : "unknown");
    snprintf(h.name, sizeof h.name, "%s", name);
    for (char *p = h.number; *p; p++) if (*p == '|') *p = '/';
    for (char *p = h.name; *p; p++) if (*p == '|') *p = '/';
    memmove(hist + 1, hist, sizeof hist[0] * (HIST_MAX - 1));
    hist[0] = h;
    if (nhist < HIST_MAX) nhist++;
    count_missed();
    history_save();
    logmsg("phone: history: %c %s %d s, account %d", kind, h.number, secs, h.acct);
}

/* the name to show for a history line: the contact list's, if it has the number now */
static const char *hist_name(const struct hist *h)
{
    const char *n = contacts_name(h->number);
    return n ? n : h->name[0] ? h->name : h->number;
}

/* ---------- the call ---------- */

/* forget the current call, and what the call screen was in the middle of */
static void clear_cur(void)
{
    memset(&P.c, 0, sizeof P.c);
    P.keypad = P.xfer = 0;
    P.note[0] = 0;
}

static void clear_call(void)
{
    clear_cur();
    memset(&P.o, 0, sizeof P.o);
    P.o_st = O_NONE;
    P.consult = P.joining = P.parked = 0;
}

/* who a call is with: the contact list knows best, then what the caller said */
static void set_peer(struct call *c, const char *number, const char *said)
{
    const char *n;
    snprintf(c->peer, sizeof c->peer, "%s", number);
    n = contacts_name(c->peer);
    snprintf(c->name, sizeof c->name, "%s", n ? n : said ? said : "");
}

static const char *call_who(const struct call *c) { return c->name[0] ? c->name : c->peer[0] ? c->peer : "Unknown"; }

static void log_call(const struct call *c)
{
    int secs = c->answered ? (int)((t48_now_ms() - c->t_established) / 1000) : 0;
    history_add(c->outgoing ? 'o' : (c->answered || c->rejected) ? 'i' : 'm', c->peer, c->name, secs, c->acct);
}

static void go_idle(void)
{
    set_tone(T48AC_TONE_OFF);
    set_ring(0);
    set_route(T48AC_ROUTE_NONE);
    clear_call();
    P.number[0] = 0;
    P.dial_acct = def_acct;
    set_state(PH_IDLE);
}

/* The current call has gone and there is another: that one takes its place.
 * A call that was waiting becomes an ordinary incoming call. A call that was
 * on hold comes back if there is somewhere for its sound to go; with the
 * handset down it stays on hold, and says so on the screen. */
static void promote(const char *text)
{
    enum other was = P.o_st;
    char gone[64];
    int joining = P.joining;

    snprintf(gone, sizeof gone, "%s", call_who(&P.c));
    P.c = P.o;
    memset(&P.o, 0, sizeof P.o);
    P.o_st = O_NONE;
    P.keypad = P.xfer = P.consult = 0;
    P.number[0] = 0;
    set_tone(T48AC_TONE_OFF);
    if (P.muted) { P.muted = 0; audioc_mute(0); }
    if (was == O_WAITING) {
        P.note[0] = 0;
        set_state(PH_INCOMING);
        return;
    }
    if (!joining) snprintf(P.note, sizeof P.note, "%.30s: %.30s", gone, text);
    if (joining) {
        P.joining = 1;      /* it goes too, once the two of them are talking */
    } else if (P.route != T48AC_ROUTE_NONE) {
        sipc_cmd("resume", P.c.id);
        P.c.held = 0;
    } else {
        P.parked = 1;
        logmsg("phone: %s stays on hold", P.c.peer);
    }
    set_state(PH_INCALL);
}

/* the call is over (or never began): write it down, say so for a moment */
static void finish(const char *text, const char *detail, int tone)
{
    int secs = P.c.answered ? (int)((t48_now_ms() - P.c.t_established) / 1000) : 0;

    log_call(&P.c);
    if (P.o_st) { promote(text); return; }
    if (P.joining) text = "Transferred";
    snprintf(P.ended, sizeof P.ended, "%s", text);
    if (detail) snprintf(P.ended_detail, sizeof P.ended_detail, "%s", detail);
    else if (P.c.answered) snprintf(P.ended_detail, sizeof P.ended_detail, "%d:%02d", secs / 60, secs % 60);
    else P.ended_detail[0] = 0;
    set_ring(0);
    if (P.muted) { P.muted = 0; audioc_mute(0); }
    /* a tone only where there is an ear to hear it: not from the loudspeaker of a phone on the hook */
    if (P.route == T48AC_ROUTE_NONE) tone = T48AC_TONE_OFF;
    set_tone(tone);
    P.ended_ms = tone != T48AC_TONE_OFF ? ENDED_TONE_MS : ENDED_MS;
    clear_call();
    P.number[0] = 0;
    P.dial_acct = def_acct;
    set_state(PH_ENDED);
}

static void start_dialler(int route)
{
    P.number[0] = 0;
    clear_call();
    set_route(route);
    set_tone(route != T48AC_ROUTE_NONE ? T48AC_TONE_DIAL : T48AC_TONE_OFF);
    set_state(PH_DIAL);
}

/* ask baresip for a call from an account; the current call's slot must be clear */
static void dial_out(const char *number, int acct)
{
    if (acct < 0 || acct >= MAX_ACCTS || !A[acct].on) acct = def_acct;
    P.c.outgoing = 1;
    P.c.acct = acct;
    set_peer(&P.c, number, NULL);
    /* with one account baresip needs no telling, and an account the shell cannot name still works */
    sipc_cmd_from(accts_on() > 1 ? A[acct].aor : NULL, "dial", number);
}

static void place_call(const char *number, int route, int acct)
{
    char n[64];
    snprintf(n, sizeof n, "%s", number);   /* number may be P.number, which is about to be emptied */
    clear_call();
    P.number[0] = 0;
    set_route(route);
    set_tone(T48AC_TONE_OFF);
    if (!sipc_connected()) {
        P.c.outgoing = 1;
        P.c.acct = acct;
        set_peer(&P.c, n, NULL);
        finish("No service", NULL, T48AC_TONE_ERROR);
        return;
    }
    dial_out(n, acct);
    set_state(PH_OUTGOING);
}

static void answer(int route)
{
    set_ring(0);            /* before the route, so the owner never hears a call waiting beep */
    set_tone(T48AC_TONE_OFF);
    set_route(route);
    sipc_cmd("accept", P.c.id);
    changed();
}

static void hang_up(void)
{
    if (P.c.id[0]) sipc_cmd("hangup", P.c.id);
    else if (P.st == PH_OUTGOING && !P.o_st) sipc_cmd("hangup", NULL);
    else if (P.st == PH_OUTGOING) return;   /* baresip has not named the new call yet: a moment */
    if (P.st == PH_INCOMING) P.c.rejected = 1;
    finish(P.st == PH_INCOMING ? "Declined" : "Call ended", NULL, T48AC_TONE_OFF);
}

static void incoming(const char *id, const char *peer, const char *name, int acct)
{
    char who[96];
    clear_call();
    snprintf(P.c.id, sizeof P.c.id, "%s", id);
    uri_user(peer, who, sizeof who);
    set_peer(&P.c, who, name);
    P.c.acct = acct;
    P.number[0] = 0;
    set_tone(T48AC_TONE_OFF);
    set_ring(1);
    set_state(PH_INCOMING);
}

static void established(void)
{
    set_tone(T48AC_TONE_OFF);
    set_ring(0);
    P.c.answered = 1;
    P.c.t_established = t48_now_ms();
    if (P.route == T48AC_ROUTE_NONE) set_route(route_for_now());
    set_state(PH_INCALL);
}

static void call_closed(const char *why)
{
    int code = atoi(why);
    if (P.st == PH_INCOMING) { finish("Missed call", NULL, T48AC_TONE_OFF); return; }
    if (P.st == PH_INCALL) {
        finish(strstr(why, "ransfer") ? "Transferred" : "Call ended", NULL, T48AC_TONE_OFF);
        return;
    }
    /* an outgoing call that never connected */
    if (code == 486 || code == 600) finish("Busy", NULL, T48AC_TONE_BUSY);
    else if (code == 404 || code == 604) finish("Not found", why, T48AC_TONE_ERROR);
    else if (code == 408 || code == 480) finish("No answer", why, T48AC_TONE_BUSY);
    else if (code == 603) finish("Declined", NULL, T48AC_TONE_BUSY);
    else finish("Call failed", why, T48AC_TONE_ERROR);
}

/* ---------- two calls ---------- */

/* a second call has arrived while the owner is on the first */
static void call_waiting(const char *id, const char *peer, const char *name, int acct)
{
    char who[96];
    memset(&P.o, 0, sizeof P.o);
    snprintf(P.o.id, sizeof P.o.id, "%s", id);
    uri_user(peer, who, sizeof who);
    set_peer(&P.o, who, name);
    P.o.acct = acct;
    P.o_st = O_WAITING;
    set_ring(1);            /* with the sound on a route, the audio service makes this a quiet beep there */
    backlight_wake("a call waiting");
    logmsg("phone: call waiting from %s", P.o.peer);
}

/* answer the waiting call: the first goes on hold */
static void answer_waiting(void)
{
    struct call t;
    if (P.o_st != O_WAITING) return;
    set_ring(0);
    if (!P.c.held) sipc_cmd("hold", P.c.id);
    sipc_cmd("accept", P.o.id);
    t = P.c; P.c = P.o; P.o = t;
    P.o.held = 1;
    P.o_st = O_HELD;
    P.c.answered = 1;
    P.c.t_established = t48_now_ms();
    P.keypad = P.parked = 0;
    P.note[0] = 0;
    if (P.muted) { P.muted = 0; audioc_mute(0); }
}

static void decline_waiting(void)
{
    char arg[200];
    if (P.o_st != O_WAITING) return;
    snprintf(arg, sizeof arg, "%s scode=486", P.o.id);
    sipc_cmd("hangup", arg);
    P.o.rejected = 1;
    log_call(&P.o);
    memset(&P.o, 0, sizeof P.o);
    P.o_st = O_NONE;
    set_ring(0);
}

/* talk to the one on hold, and hold the one we were talking to */
static void swap_calls(void)
{
    struct call t;
    if (P.o_st != O_HELD || P.st != PH_INCALL || P.joining) return;
    if (!P.c.held) sipc_cmd("hold", P.c.id);
    sipc_cmd("resume", P.o.id);
    t = P.c; P.c = P.o; P.o = t;
    P.o.held = 1;
    P.c.held = 0;
    P.parked = P.keypad = 0;
    P.note[0] = 0;
    if (P.route == T48AC_ROUTE_NONE) set_route(route_for_now());
    logmsg("phone: now talking to %s, %s on hold", P.c.peer, P.o.peer);
}

/* connect the one on hold to the one we are talking to, and leave: an attended transfer */
static void join_calls(void)
{
    char arg[340];
    if (P.o_st != O_HELD || P.st != PH_INCALL || P.joining) return;
    snprintf(arg, sizeof arg, "%s %s", P.o.id, P.c.id);
    sipc_cmd("joincalls", arg);
    P.joining = 1;
    P.c.held = 1;           /* baresip holds it while the other end takes over */
    snprintf(P.note, sizeof P.note, "Connecting %.20s to %.20s", call_who(&P.o), call_who(&P.c));
    logmsg("phone: transfer %s to %s", P.o.peer, P.c.peer);
}

/* in the middle of a transfer, number typed: call them first, with the caller on hold */
static void consult(void)
{
    char n[64];
    int acct = P.c.acct;
    if (!P.number[0] || P.o_st) return;
    snprintf(n, sizeof n, "%s", P.number);
    if (!P.c.held) sipc_cmd("hold", P.c.id);
    P.o = P.c;
    P.o.held = 1;
    P.o_st = O_HELD;
    clear_cur();
    P.number[0] = 0;
    P.consult = 1;
    if (P.muted) { P.muted = 0; audioc_mute(0); }
    dial_out(n, acct);
    logmsg("phone: asking %s before the transfer", n);
    set_state(PH_OUTGOING);
}

/* the second call has ended at the far end */
static void other_closed(void)
{
    if (P.o_st == O_WAITING) set_ring(0);
    else if (!P.joining) snprintf(P.note, sizeof P.note, "%.40s hung up", call_who(&P.o));
    log_call(&P.o);
    memset(&P.o, 0, sizeof P.o);
    P.o_st = O_NONE;
    P.consult = 0;
}

void phone_sip_link(int up)
{
    if (!up) {
        for (int i = 0; i < MAX_ACCTS; i++) { A[i].reg = REG_NONE; A[i].msgs = 0; }
        if (P.st == PH_OUTGOING || P.st == PH_INCOMING || P.st == PH_INCALL) {
            if (P.o_st) { log_call(&P.o); memset(&P.o, 0, sizeof P.o); P.o_st = O_NONE; }
            finish("Call lost", "No service", T48AC_TONE_ERROR);
        }
        update_lamps();
    }
    changed();
}

void phone_sip_response(const char *token, int ok, const char *data)
{
    if (ok) return;
    logmsg("sip: %s failed: %.80s", token, data);
    if (!strcmp(token, "dial") && P.st == PH_OUTGOING && !P.c.id[0]) {
        finish("Call failed", "Invalid number", T48AC_TONE_ERROR);
    } else if ((!strcmp(token, "hold") || !strcmp(token, "resume")) && !P.o_st) {
        P.c.held = !P.c.held;
        changed();
    } else if (!strcmp(token, "transfer") || !strcmp(token, "joincalls")) {
        snprintf(P.note, sizeof P.note, "Transfer failed");
        P.joining = 0;
        changed();
    }
}

/* "Messages-Waiting: yes ... Voice-Message: 2/8": how many new ones; -1 for yes without a number */
static int mwi_count(const char *body)
{
    const char *p = strstr(body, "Messages-Waiting:"), *v = strstr(body, "Voice-Message:");
    if (!p) return 0;
    p += 17;
    while (*p == ' ') p++;
    if (strncmp(p, "yes", 3) && strncmp(p, "Yes", 3) && strncmp(p, "YES", 3)) return 0;
    if (v) {
        int n = atoi(v + 14);
        if (n > 0) return n;
    }
    return -1;
}

void phone_sip_event(const char *type, const char *j, const char *e)
{
    char id[160] = "", peer[160] = "", name[64] = "", dir[12] = "", param[400] = "", aor[160] = "";
    double secs = 0;
    int sync = t48_json_true(t48_json_get(j, e, "sync"), e);

    t48_json_str(t48_json_get(j, e, "param"), e, param, sizeof param);
    t48_json_str(t48_json_get(j, e, "accountaor"), e, aor, sizeof aor);
    int acct = acct_by_aor(aor);
    if (!strncmp(type, "REGISTER", 8) || !strcmp(type, "UNREGISTERING")) {
        if (acct < 0) return;
        enum reg was = A[acct].reg;
        A[acct].reg = !strcmp(type, "REGISTER_OK") ? REG_OK : !strcmp(type, "REGISTER_FAIL") ? REG_FAIL : !strcmp(type, "REGISTERING") ? REG_TRYING : REG_NONE;
        snprintf(A[acct].why, sizeof A[acct].why, "%s", param);
        if (A[acct].reg != REG_OK) A[acct].msgs = 0;
        if (A[acct].reg != was) logmsg("phone: account %d registration %s %s", acct + 1, type + (type[0] == 'R' ? 9 : 0), param);
        update_lamps();
        changed();
        return;
    }
    if (!strcmp(type, "MWI_NOTIFY")) {
        if (acct < 0) return;
        int n = mwi_count(param);
        if (n != A[acct].msgs) logmsg("phone: account %d: %d new message%s", acct + 1, n < 0 ? 1 : n, n == 1 || n < 0 ? "" : "s");
        A[acct].msgs = n;
        update_lamps();
        changed();
        return;
    }
    if (strncmp(type, "CALL_", 5)) return;
    if (acct < 0) acct = def_acct;
    t48_json_str(t48_json_get(j, e, "id"), e, id, sizeof id);
    t48_json_str(t48_json_get(j, e, "peeruri"), e, peer, sizeof peer);
    t48_json_str(t48_json_get(j, e, "peerdisplayname"), e, name, sizeof name);
    t48_json_str(t48_json_get(j, e, "direction"), e, dir, sizeof dir);
    t48_json_num(t48_json_get(j, e, "duration"), e, &secs);
    type += 5;
    int ours = P.c.id[0] && !strcmp(id, P.c.id);
    int others = P.o_st && !strcmp(id, P.o.id);
    int out = !strcmp(dir, "outgoing");
    int free_now = P.st == PH_IDLE || P.st == PH_ENDED || P.st == PH_DIAL;

    if (others) {
        if (!strcmp(type, "CLOSED")) {
            other_closed();
            update_lamps();
            changed();
        } else if (!strcmp(type, "TRANSFER_FAILED")) {
            snprintf(P.note, sizeof P.note, "Transfer failed");
            P.joining = 0;
            changed();
        }
        return;
    }
    if (!strcmp(type, "INCOMING")) {
        char arg[200], who[64];
        if (ours) return;
        if (P.dnd) {
            /* do not disturb: the caller is told we are not available, and we note that they tried */
            snprintf(arg, sizeof arg, "%s scode=480 reason=Unavailable", id);
            sipc_cmd("hangup", arg);
            uri_user(peer, who, sizeof who);
            history_add('m', who, contacts_name(who) ? contacts_name(who) : name, 0, acct);
            update_lamps();
            changed();
        } else if (free_now) {
            incoming(id, peer, name, acct);
        } else if (P.st == PH_INCALL && !P.o_st && !P.xfer && !sync) {
            call_waiting(id, peer, name, acct);
            update_lamps();
            changed();
        } else {
            /* no room for it: the caller hears busy, and we note that they tried */
            snprintf(arg, sizeof arg, "%s scode=486", id);
            sipc_cmd("hangup", arg);
            uri_user(peer, who, sizeof who);
            history_add('m', who, contacts_name(who) ? contacts_name(who) : name, 0, acct);
            update_lamps();
            changed();
        }
        return;
    }
    /* our own outgoing call gets its id */
    if (!ours && out && P.st == PH_OUTGOING && !P.c.id[0] && strcmp(type, "CLOSED")) {
        snprintf(P.c.id, sizeof P.c.id, "%s", id);
        ours = 1;
    }
    /* a call that was already up when the shell started: pick it up */
    if (!ours && sync && (P.st == PH_IDLE || (P.st == PH_DIAL && !P.number[0])) && strcmp(type, "CLOSED")) {
        char who[96];
        clear_call();
        snprintf(P.c.id, sizeof P.c.id, "%s", id);
        uri_user(peer, who, sizeof who);
        set_peer(&P.c, who, name);
        P.c.acct = acct;
        P.c.outgoing = out;
        P.c.held = t48_json_true(t48_json_get(j, e, "onhold"), e);
        logmsg("phone: picking up a call that was already there (%s)", type);
        if (!strcmp(type, "ESTABLISHED")) {
            established();
            P.c.t_established -= (long long)(secs * 1000);
        } else {
            set_tone(T48AC_TONE_OFF);
            if (P.route == T48AC_ROUTE_NONE) set_route(route_for_now());
            set_state(PH_OUTGOING);
        }
        return;
    }
    /* and a second one beside it, on hold */
    if (!ours && sync && P.st == PH_INCALL && !P.o_st && !strcmp(type, "ESTABLISHED")) {
        char who[96];
        memset(&P.o, 0, sizeof P.o);
        snprintf(P.o.id, sizeof P.o.id, "%s", id);
        uri_user(peer, who, sizeof who);
        set_peer(&P.o, who, name);
        P.o.acct = acct;
        P.o.outgoing = out;
        P.o.answered = 1;
        P.o.t_established = t48_now_ms() - (long long)(secs * 1000);
        P.o.held = 1;
        P.o_st = O_HELD;
        /* the one the owner was talking to is the one in front */
        if (P.c.held && !t48_json_true(t48_json_get(j, e, "onhold"), e)) {
            struct call t = P.c; P.c = P.o; P.o = t;
            P.c.held = 0;
        }
        changed();
        return;
    }
    /* the far end has transferred us: baresip made the new call itself. Follow it. */
    if (!ours && out && P.st == PH_INCALL && !P.o_st && !strcmp(type, "ESTABLISHED")) {
        char who[96];
        snprintf(P.c.id, sizeof P.c.id, "%s", id);
        uri_user(peer, who, sizeof who);
        set_peer(&P.c, who, name);
        P.c.held = P.c.remote_held = 0;
        changed();
        return;
    }
    if (!ours) return;

    if (!strcmp(type, "RINGING")) {
        P.c.ringing = 1;
        set_tone(T48AC_TONE_RINGBACK);
        changed();
    } else if (!strcmp(type, "PROGRESS")) {
        P.c.ringing = 1;
        set_tone(T48AC_TONE_OFF);   /* the far end is sending its own ringing sound */
        changed();
    } else if (!strcmp(type, "ESTABLISHED")) {
        if (P.st != PH_INCALL) established();
    } else if (!strcmp(type, "CLOSED")) {
        call_closed(param);
    } else if (!strcmp(type, "HOLD")) {
        P.c.remote_held = 1;
        changed();
    } else if (!strcmp(type, "RESUME")) {
        P.c.remote_held = 0;
        changed();
    } else if (!strcmp(type, "TRANSFER_FAILED")) {
        snprintf(P.note, sizeof P.note, "Transfer failed");
        P.c.held = 0;               /* baresip takes the call off hold again */
        P.joining = 0;
        changed();
    }
}

void phone_audio_link(int up)
{
    const struct t48ac_state *s = audioc_state();
    if (up && s) {
        for (int i = 1; i < T48AC_VOL_COUNT; i++) audioc_volume(i, P.vol[i]);
        if (!P.audio_seen && !P.settled && !P.hook_off && s->route != T48AC_ROUTE_NONE && s->route < T48AC_ROUTE_COUNT &&
            (P.st == PH_IDLE || P.st == PH_INCALL || P.st == PH_OUTGOING)) {
            /* the shell has just started and the service is in the middle of something: keep it */
            P.route = s->route;
            P.muted = s->mute;
            logmsg("phone: the audio service was already on %s", route_names[P.route]);
        } else {
            audioc_route(P.route);
            if (P.muted) audioc_mute(1);
            audioc_tone(P.tone);
            audioc_ring(P.ring);
        }
        P.audio_seen = 1;
        update_lamps();
        app_sound_link();
    }
    changed();
}

/* the service's state changed. Nothing on the screen is drawn from it: the
 * screens show what we asked for, and the simulator's checks read it direct. */
void phone_audio_state(void) {}

/* ---------- keys ---------- */

static void type_char(int ch)
{
    size_t k = strlen(P.number);
    if (k + 1 < sizeof P.number && k < 40) { P.number[k] = (char)ch; P.number[k + 1] = 0; }
    audioc_key(ch);
}

static void do_transfer(void)
{
    if (!P.number[0]) return;
    logmsg("phone: transfer to %s", P.number);
    sipc_cmd("transfer", P.number);
    snprintf(P.note, sizeof P.note, "Transferring to %s", P.number);
    P.c.held = 1;    /* baresip holds the call while it asks */
    P.xfer = 0;
    P.number[0] = 0;
}

static void toggle_mute(void)
{
    if (P.st == PH_INCOMING || (P.o_st == O_WAITING && P.ring)) { set_ring(0); return; }   /* quiet, but still ringing on the screen */
    if (P.st != PH_INCALL && P.st != PH_OUTGOING) return;
    P.muted = !P.muted;
    audioc_mute(P.muted);
    logmsg("phone: microphone %s", P.muted ? "muted" : "live");
}

/* a call left on hold when the handset went down comes back when there is somewhere for its sound */
static void unpark(void)
{
    if (!P.parked || P.st != PH_INCALL || P.route == T48AC_ROUTE_NONE) return;
    P.parked = 0;
    if (P.c.held) { P.c.held = 0; sipc_cmd("resume", P.c.id); }
}

static void toggle_hold(void)
{
    if (P.st != PH_INCALL || P.joining) return;
    if (P.c.held && P.route == T48AC_ROUTE_NONE) set_route(route_for_now());
    P.parked = 0;
    P.c.held = !P.c.held;
    sipc_cmd(P.c.held ? "hold" : "resume", P.c.id);
}

static void redial(void)
{
    if (P.st != PH_IDLE && P.st != PH_DIAL && P.st != PH_ENDED) return;
    for (int i = 0; i < nhist; i++)
        if (hist[i].kind == 'o') { place_call(hist[i].number, route_for_now(), hist[i].acct ? hist[i].acct - 1 : def_acct); return; }
    logmsg("phone: nothing to redial");
}

/* the next account that is switched on, for the dialler */
static void next_account(int step)
{
    if (accts_on() < 2) return;
    do P.dial_acct = (P.dial_acct + MAX_ACCTS + step) % MAX_ACCTS; while (!A[P.dial_acct].on);
}

/* the Message key: call the voicemail number of the account that has messages, or of the usual one */
static void message_key(void)
{
    int a = def_acct;
    if (P.st != PH_IDLE && P.st != PH_DIAL && P.st != PH_ENDED) return;
    for (int i = MAX_ACCTS - 1; i >= 0; i--)
        if (A[i].on && A[i].msgs && A[i].voicemail[0]) a = i;
    if (!A[a].on || !A[a].voicemail[0]) {
        logmsg("phone: Message key: no voicemail number is set (Settings, Accounts)");
        return;
    }
    place_call(A[a].voicemail, route_for_now(), a);
}

void phone_toggle_dnd(void)
{
    P.dnd = !P.dnd;
    cfg_set("phone", "dnd", P.dnd ? "on" : "off");
    logmsg("phone: do not disturb %s", P.dnd ? "on" : "off");
    changed();
}

int phone_dnd(void) { return P.dnd; }

int phone_messages(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ACCTS; i++)
        if (A[i].on && A[i].msgs) n += A[i].msgs < 0 ? 1 : A[i].msgs;
    return n;
}

/* Speakerphone or Headset: answer, start, move the sound there, or (pressed
 * again) end, as on any desk phone */
static void route_key(int r)
{
    switch (P.st) {
    case PH_ENDED:
        /* a failed call's tone is still playing there: pressing the key again gives up */
        if (P.route == r && !P.hook_off) { go_idle(); break; }
        /* fall through */
    case PH_IDLE:
        start_dialler(r);
        break;
    case PH_DIAL:
        if (P.number[0] && P.route == T48AC_ROUTE_NONE) place_call(P.number, r, P.dial_acct);
        else if (P.route == r) { if (P.hook_off) set_route(T48AC_ROUTE_HANDSET); else go_idle(); }
        else { set_route(r); if (!P.number[0]) set_tone(T48AC_TONE_DIAL); }
        break;
    case PH_INCOMING:
        answer(r);
        break;
    case PH_OUTGOING:
    case PH_INCALL:
        if (P.route == r) {
            if (P.hook_off) set_route(T48AC_ROUTE_HANDSET);
            else { if (P.o_st) set_route(T48AC_ROUTE_NONE); hang_up(); }
        } else {
            set_route(r);
            unpark();
        }
        break;
    }
}

static void volume_key(int step)
{
    /* the route in use; on hook the ringer, or the sound of the app in front if it has any */
    int which = P.route != T48AC_ROUTE_NONE ? P.route : app_sound() ? T48AC_VOL_APP : T48AC_VOL_RINGER;
    int v = P.vol[which] + step;
    if (v < 0) v = 0;
    if (v > T48AC_VOL_MAX) v = T48AC_VOL_MAX;
    if (v != P.vol[which]) {
        P.vol[which] = v;
        audioc_volume(which, v);
        save_volumes();
    }
    P.vol_shown = which;
    P.vol_until = t48_now_ms() + VOL_SHOW_MS;
}

static void delete_char(void)
{
    size_t k = strlen(P.number);
    if (k) P.number[k - 1] = 0;
}

static void dial_now(void)
{
    if (P.number[0]) place_call(P.number, route_for_now(), P.dial_acct);
}

void phone_key(int code, int down)
{
    int digit = (code >= '0' && code <= '9') || code == T48_KEY_STAR || code == T48_KEY_HASH;
    if (!down) return;
    switch (code) {
    case SK_SPEAKER: route_key(T48AC_ROUTE_SPEAKER); break;
    case SK_HEADSET: route_key(T48AC_ROUTE_HEADSET); break;
    case SK_MUTE: toggle_mute(); break;
    case SK_HOLD: toggle_hold(); break;
    case SK_REDIAL: redial(); break;
    case SK_VOLUP: volume_key(1); break;
    case SK_VOLDOWN: volume_key(-1); break;
    case SK_MESSAGE: message_key(); break;
    case SK_TRANSFER:
        if (P.st != PH_INCALL || P.joining) break;
        if (P.o_st == O_HELD) join_calls();   /* two calls: put them through to each other */
        else if (P.o_st) break;
        else if (P.xfer && P.number[0]) do_transfer();
        else { P.xfer = !P.xfer; P.number[0] = 0; P.note[0] = 0; }
        break;
    default:
        switch (P.st) {
        case PH_IDLE:
            break;
        case PH_DIAL:
            if (digit) {
                if (!P.number[0]) set_tone(T48AC_TONE_OFF);
                type_char(code);
            } else if (code == T48_KEY_OK) {
                dial_now();
            } else if (code == T48_KEY_LEFT || code == T48_KEY_RIGHT) {
                next_account(code == T48_KEY_LEFT ? -1 : 1);
            } else if (code == T48_KEY_BACK) {
                if (P.number[0]) {
                    delete_char();
                    if (!P.number[0] && P.route != T48AC_ROUTE_NONE) set_tone(T48AC_TONE_DIAL);
                } else {
                    go_idle();
                }
            }
            break;
        case PH_OUTGOING:
            if (code == T48_KEY_BACK) hang_up();
            break;
        case PH_INCOMING:
            if (code == T48_KEY_OK) answer(route_for_now());
            else if (code == T48_KEY_BACK) hang_up();
            break;
        case PH_INCALL:
            if (P.o_st == O_WAITING && (code == T48_KEY_OK || code == T48_KEY_BACK)) {
                if (code == T48_KEY_OK) answer_waiting(); else decline_waiting();
            } else if (P.xfer) {
                if (digit) type_char(code);
                else if (code == T48_KEY_OK) consult();
                else if (code == T48_KEY_BACK) { if (P.number[0]) delete_char(); else P.xfer = 0; }
            } else if (digit) {
                char d[2] = {(char)code, 0};
                size_t k = strlen(P.c.dtmf);
                if (k + 1 >= sizeof P.c.dtmf) { memmove(P.c.dtmf, P.c.dtmf + 1, k); k--; }
                P.c.dtmf[k] = (char)code;
                P.c.dtmf[k + 1] = 0;
                sipc_cmd("sndcode", d);
                audioc_key(code);
            } else if (code == T48_KEY_BACK) {
                P.keypad = 0;
            } else if (code == T48_KEY_OK && P.o_st == O_HELD) {
                swap_calls();
            }
            break;
        case PH_ENDED:
            if (digit) { start_dialler(P.hook_off ? T48AC_ROUTE_HANDSET : T48AC_ROUTE_NONE); set_tone(T48AC_TONE_OFF); type_char(code); }
            else if (code == T48_KEY_OK || code == T48_KEY_BACK) go_idle();
            break;
        }
    }
    update_lamps();
    changed();
}

int phone_idle_key(int code)
{
    if ((code >= '0' && code <= '9') || code == T48_KEY_STAR || code == T48_KEY_HASH) {
        /* typing a number with the phone on the hook: no dial tone, nothing is sent until OK or the handset */
        start_dialler(T48AC_ROUTE_NONE);
        type_char(code);
        changed();
        return 1;
    }
    if (code == T48_KEY_UP) { phone_open_history(); return 1; }
    if (code == T48_KEY_LEFT) { contacts_open(NULL); return 1; }
    if (code == T48_KEY_RIGHT) { phone_toggle_dnd(); return 1; }
    return 0;
}

/* put a number in the dialler, ready to call: from the history or the contact list */
void phone_prefill(const char *number, int acct)
{
    char n[64];
    snprintf(n, sizeof n, "%s", number);
    start_dialler(P.hook_off ? T48AC_ROUTE_HANDSET : T48AC_ROUTE_NONE);
    set_tone(T48AC_TONE_OFF);
    snprintf(P.number, sizeof P.number, "%.40s", n);
    P.dial_acct = acct >= 0 && acct < MAX_ACCTS && A[acct].on ? acct : def_acct;
    changed();
}

/* the dialler, set to call from one account: a touch on its line on the idle screen */
void phone_dial_from(int acct)
{
    if (acct < 0 || acct >= MAX_ACCTS || !A[acct].on || P.st != PH_IDLE) return;
    start_dialler(P.hook_off ? T48AC_ROUTE_HANDSET : T48AC_ROUTE_NONE);
    P.dial_acct = acct;
    changed();
}

void phone_hook(int off_hook)
{
    P.hook_off = off_hook;
    logmsg("phone: handset %s", off_hook ? "lifted" : "down");
    if (off_hook) {
        switch (P.st) {
        case PH_IDLE:
        case PH_ENDED:
            start_dialler(T48AC_ROUTE_HANDSET);
            break;
        case PH_DIAL:
            if (P.number[0] && P.route == T48AC_ROUTE_NONE) place_call(P.number, T48AC_ROUTE_HANDSET, P.dial_acct);
            else { set_route(T48AC_ROUTE_HANDSET); if (!P.number[0]) set_tone(T48AC_TONE_DIAL); }
            break;
        case PH_INCOMING:
            answer(T48AC_ROUTE_HANDSET);
            break;
        case PH_OUTGOING:
        case PH_INCALL:
            set_route(T48AC_ROUTE_HANDSET);
            unpark();
            break;
        }
    } else if (P.route == T48AC_ROUTE_HANDSET) {
        /* putting the handset down ends whatever was on it. A call on the speaker or headset carries on. */
        switch (P.st) {
        case PH_OUTGOING:
        case PH_INCALL:
            /* a second call is left as it is: one on hold stays on hold, one waiting rings out loud */
            if (P.o_st) set_route(T48AC_ROUTE_NONE);
            hang_up();
            break;
        case PH_INCOMING:
            set_route(T48AC_ROUTE_NONE);   /* still ringing: back to the loudspeaker */
            break;
        default:
            go_idle();
        }
    }
    update_lamps();
    changed();
}

int phone_timeout(long long now)
{
    long long t = -1;
    if (P.st == PH_ENDED) t = P.t_state + P.ended_ms - now;
    if (P.vol_until && (t < 0 || P.vol_until - now < t)) t = P.vol_until - now;
    if (!P.settled && (t < 0 || P.started + SETTLE_MS - now < t)) t = P.started + SETTLE_MS - now;
    if (P.st == PH_INCALL && (t < 0 || t > 250)) t = 250;   /* the call timer */
    return t < 0 ? -1 : t > 0 ? (int)t : 0;
}

void phone_tick(long long now)
{
    static long long shown_secs = -1;

    if (P.st == PH_ENDED && now >= P.t_state + P.ended_ms) go_idle();
    if (P.vol_until && now >= P.vol_until) { P.vol_until = 0; changed(); }
    if (!P.settled && now >= P.started + SETTLE_MS) {
        P.settled = 1;
        /* sound left switched on by a shell that died, and no call to go with it */
        if (P.st == PH_IDLE && P.route != T48AC_ROUTE_NONE) { set_route(T48AC_ROUTE_NONE); update_lamps(); }
    }
    if (P.st == PH_INCALL) {
        long long s = (now - P.c.t_established) / 1000;
        if (s != shown_secs) { shown_secs = s; changed(); }
    }
}

void phone_init(void)
{
    P.started = t48_now_ms();
    contacts_init();
    load_settings();
    P.dial_acct = def_acct;
    history_load();
    sipc_init(opt.sip);
    audioc_init(opt.audio);
    update_lamps();
}

/* ---------- drawing ---------- */

#define KP_X 496
#define KP_Y 58
#define KP_W 88
#define KP_H 73
#define KP_GAP 8
static const char kp_chars[] = "123456789*0#";

#define ROW_Y 392          /* the row of buttons along the bottom */
#define ROW_H 64
#define FROM_Y 328         /* the dialler's account line, when there is more than one account */
#define FROM_H 46
#define BAN_Y 258          /* in a call: the line about the second call */
#define BAN_H 46

static void keypad_draw(void)
{
    for (int i = 0; i < 12; i++) {
        char k[2] = {kp_chars[i], 0};
        ui_key(KP_X + (i % 3) * (KP_W + KP_GAP), KP_Y + (i / 3) * (KP_H + KP_GAP), KP_W, KP_H, k);
    }
}

static int keypad_hit(int x, int y)
{
    for (int i = 0; i < 12; i++)
        if (ui_inside(x, y, KP_X + (i % 3) * (KP_W + KP_GAP), KP_Y + (i / 3) * (KP_H + KP_GAP), KP_W, KP_H))
            return kp_chars[i] == '*' ? T48_KEY_STAR : kp_chars[i] == '#' ? T48_KEY_HASH : kp_chars[i];
    return 0;
}

/* the tags that say where the sound is and what is switched off; centred on cx, or from x if cx < 0 */
static void chips(int cx, int x, int y)
{
    const char *t[4];
    int role[4], n = 0, w = 0;

    if (P.route) { t[n] = route_labels[P.route]; role[n++] = B_PLAIN; }
    if (P.muted) { t[n] = "Muted"; role[n++] = B_STOP; }
    if (P.c.held) { t[n] = "On hold"; role[n++] = B_LIT; }
    if (P.c.remote_held) { t[n] = "Held remotely"; role[n++] = B_LIT; }
    for (int i = 0; i < n; i++) w += ui_tag_width(t[i]) + (i ? 8 : 0);
    if (cx >= 0) x = cx - w / 2;
    for (int i = 0; i < n; i++) x = ui_tag(x, y, t[i], role[i]);
}

/* a number or name as large as will fit in maxw, centred on cx; the end of a long number is the part kept */
static void big_text(int cx, int y, int maxw, const char *s, uint32_t c)
{
    const t48_font *f = &t48_font_big;
    if (t48_text_width(f, s) > maxw) { f = &t48_font_head; y += 8; }
    while (t48_text_width(f, s) > maxw && s[1]) s++;
    t48_text_centre(&ui, cx, y, f, c, s);
}

static const char *who(void) { return call_who(&P.c); }

/* "Work (1004@example.com)", or just the address if the account has no label */
static void acct_text(int i, char *out, size_t n)
{
    if (i < 0 || i >= MAX_ACCTS || !A[i].used) { snprintf(out, n, "?"); return; }
    if (A[i].label[0]) snprintf(out, n, "%s (%s@%s)", A[i].label, A[i].user, A[i].domain);
    else snprintf(out, n, "%s@%s", A[i].user, A[i].domain);
}

static void draw_dial(void)
{
    ui_header("Dial", 0, 0);
    ui_panel(24, KP_Y, 448, 110);
    if (P.number[0]) {
        const char *n = contacts_name(P.number);
        big_text(248, KP_Y + (n ? 18 : 30), 420, P.number, C_TEXT);
        if (n) t48_text_centre(&ui, 248, KP_Y + 74, &t48_font_normal, C_DIM, n);
    } else {
        t48_text_centre(&ui, 248, KP_Y + 40, &t48_font_head, C_FAINT, "Number");
    }
    chips(-1, 24, KP_Y + 126);
    if (!sipc_connected()) t48_text(&ui, 28, KP_Y + 182, &t48_font_normal, C_BAD, "No service");
    if (accts_on() > 1) {
        char b[120], a[100];
        enum reg r = A[P.dial_acct].reg;
        struct row_ink ink;
        acct_text(P.dial_acct, a, sizeof a);
        snprintf(b, sizeof b, "From %s", a);
        ui_tile(24, FROM_Y, 448, FROM_H, 0, &ink);
        ui_lamp(40, FROM_Y + FROM_H / 2 - 6, r == REG_OK ? C_GOOD : r == REG_TRYING ? C_ACCENT : C_BAD);
        t48_text_fit(&ui, 62, FROM_Y + (FROM_H - t48_font_normal.height) / 2, 340, &t48_font_normal, ink.text, b);
        ui_chevron(424, FROM_Y + FROM_H / 2, 7, -1, 3, ink.dim);
        ui_chevron(448, FROM_Y + FROM_H / 2, 7, 1, 3, ink.dim);
    }
    keypad_draw();
    ui_btn(24, ROW_Y, 216, ROW_H, "Call", P.number[0] ? B_GO : B_OFF);
    ui_btn(252, ROW_Y, 108, ROW_H, "<x", P.number[0] ? B_PLAIN : B_OFF);
    ui_btn(372, ROW_Y, 100, ROW_H, "Cancel", B_PLAIN);
}

static void draw_outgoing(void)
{
    char b[120];
    ui_header(P.consult ? "Asking first" : "Calling", 0, B_LIT);
    big_text(SW / 2, 104, 720, who(), C_TEXT);
    if (P.c.name[0]) t48_text_centre(&ui, SW / 2, 160, &t48_font_head, C_DIM, P.c.peer);
    t48_text_centre(&ui, SW / 2, 212, &t48_font_normal, C_DIM, P.c.ringing ? "Ringing" : "Connecting");
    if (P.o_st == O_HELD) {
        snprintf(b, sizeof b, "On hold: %s", call_who(&P.o));
        t48_text_centre(&ui, SW / 2, 246, &t48_font_normal, C_DIM, b);
    } else if (accts_on() > 1) {
        snprintf(b, sizeof b, "From %s", acct_label(P.c.acct));
        t48_text_centre(&ui, SW / 2, 246, &t48_font_normal, C_DIM, b);
    }
    chips(SW / 2, 0, 296);
    ui_btn(180, ROW_Y, 200, ROW_H, "Speaker", P.route == T48AC_ROUTE_SPEAKER ? B_LIT : B_PLAIN);
    ui_btn(420, ROW_Y, 200, ROW_H, "End", B_STOP);
}

static void draw_incoming(void)
{
    ui_header("Incoming call", 0, B_GO);
    big_text(SW / 2, 112, 720, who(), C_TEXT);
    if (P.c.name[0]) t48_text_centre(&ui, SW / 2, 172, &t48_font_head, C_DIM, P.c.peer);
    if (accts_on() > 1) {
        char b[120], a[100];
        acct_text(P.c.acct, a, sizeof a);
        snprintf(b, sizeof b, "For %s", a);
        t48_text_centre(&ui, SW / 2, 228, &t48_font_normal, C_DIM, b);
    }
    if (!P.ring) ui_tag(SW / 2 - ui_tag_width("Silenced") / 2, 280, "Silenced", B_OFF);
    ui_btn(120, 372, 260, 76, "Answer", B_GO);
    ui_btn(420, 372, 260, 76, "Decline", B_STOP);
}

static void call_time(char *out, size_t n)
{
    long long s = (t48_now_ms() - P.c.t_established) / 1000;
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(out, n, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%02lld:%02lld", s / 60, s % 60);
}

/* the line about the second call: who is waiting, or who is on hold */
static void draw_banner(int side)
{
    char b[120];
    int w = side ? 448 : SW - 48;
    struct row_ink ink;
    if (!P.o_st) return;
    snprintf(b, sizeof b, "%s: %s", P.o_st == O_WAITING ? "Call waiting" : "On hold", call_who(&P.o));
    ui_tile(24, BAN_Y, w, BAN_H, 0, &ink);
    ui_lamp(40, BAN_Y + BAN_H / 2 - 6, P.o_st == O_WAITING ? C_GOOD : C_ACCENT);
    t48_text_fit(&ui, 64, BAN_Y + (BAN_H - t48_font_bold.height) / 2, side ? w - 60 : 460, &t48_font_bold, ink.text, b);
    if (side) return;
    if (P.o_st == O_WAITING) {
        ui_btn(548, BAN_Y + 4, 108, BAN_H - 8, "Answer", B_GO);
        ui_btn(664, BAN_Y + 4, 108, BAN_H - 8, "Decline", B_STOP);
    } else if (!P.joining) {
        ui_btn(548, BAN_Y + 4, 108, BAN_H - 8, "Swap", B_FIRST);
        ui_btn(664, BAN_Y + 4, 108, BAN_H - 8, "Join", B_FIRST);
    }
}

static void draw_incall(void)
{
    char t[24];
    int side = P.keypad || P.xfer;       /* the keypad takes the right of the screen */
    int cx = side ? 248 : SW / 2, maxw = side ? 440 : 720;

    call_time(t, sizeof t);
    ui_header(P.xfer ? "Transfer to" : P.c.held ? "On hold" : "In call", 0, P.c.held ? B_LIT : B_GO);
    if (P.xfer) {
        ui_panel(24, KP_Y, 448, 110);
        if (P.number[0]) big_text(cx, KP_Y + 30, 420, P.number, C_TEXT);
        else t48_text_centre(&ui, cx, KP_Y + 40, &t48_font_head, C_FAINT, "Number");
        t48_text_centre(&ui, cx, 196, &t48_font_bold, C_TEXT, who());
        t48_text_centre(&ui, cx, 226, &t48_font_head, C_DIM, t);
    } else if (side) {
        big_text(cx, 66, maxw, who(), C_TEXT);
        t48_text_centre(&ui, cx, 128, &t48_font_big, P.c.held ? C_DIM : C_TEXT, t);
        if (P.c.dtmf[0]) {
            ui_panel(24, 196, 448, 56);
            t48_text_centre(&ui, cx, 196 + (56 - t48_font_head.height) / 2, &t48_font_head, C_TEXT, P.c.dtmf);
        }
    } else {
        big_text(cx, 60, maxw, who(), C_TEXT);
        if (P.c.name[0]) t48_text_centre(&ui, cx, 112, &t48_font_normal, C_DIM, P.c.peer);
        t48_text_centre(&ui, cx, 142, &t48_font_huge, P.c.held ? C_DIM : C_TEXT, t);
    }
    if (side) keypad_draw();
    if (P.o_st && !P.xfer) {
        draw_banner(side);
        if (P.note[0]) t48_text_centre(&ui, cx, 362, &t48_font_small, C_DIM, P.note);
    } else if (P.note[0]) {
        t48_text_centre(&ui, cx, 276, &t48_font_normal, C_DIM, P.note);
    } else if (P.parked && !side) {
        t48_text_centre(&ui, cx, 276, &t48_font_normal, C_DIM, "Lift handset to resume");
    }
    chips(cx, 0, 318);

    if (P.xfer) {
        ui_btn(24, ROW_Y, 132, ROW_H, "Transfer", P.number[0] ? B_GO : B_OFF);
        ui_btn(164, ROW_Y, 132, ROW_H, "Ask first", P.number[0] ? B_FIRST : B_OFF);
        ui_btn(304, ROW_Y, 80, ROW_H, "<x", P.number[0] ? B_PLAIN : B_OFF);
        ui_btn(392, ROW_Y, 80, ROW_H, "Cancel", B_PLAIN);
        return;
    }
    static const char *const label[6] = {"Mute", "Hold", "Keypad", "Speaker", "Transfer", "End"};
    for (int i = 0; i < 6; i++) {
        int on = (i == 0 && P.muted) || (i == 1 && P.c.held) || (i == 2 && P.keypad) || (i == 3 && P.route == T48AC_ROUTE_SPEAKER);
        ui_btn(24 + i * 127, ROW_Y, 117, ROW_H, label[i], i == 5 ? B_STOP : on ? B_LIT : B_PLAIN);
    }
}

static void draw_ended(void)
{
    ui_header("Call", 0, 0);
    t48_text_centre(&ui, SW / 2, 120, &t48_font_big, C_TEXT, P.ended);
    if (hist[0].number[0] && nhist) t48_text_centre(&ui, SW / 2, 196, &t48_font_head, C_DIM, hist_name(&hist[0]));
    if (P.ended_detail[0]) t48_text_centre(&ui, SW / 2, 250, &t48_font_normal, C_DIM, P.ended_detail);
}

void phone_draw(void)
{
    ui_clear(P.st == PH_INCOMING ? T->ground_ring : P.st == PH_INCALL && P.c.held ? T->ground_hold :
             P.st == PH_INCALL || P.st == PH_OUTGOING ? T->ground_call : C_BG);
    switch (P.st) {
    case PH_IDLE: break;
    case PH_DIAL: draw_dial(); break;
    case PH_OUTGOING: draw_outgoing(); break;
    case PH_INCOMING: draw_incoming(); break;
    case PH_INCALL: draw_incall(); break;
    case PH_ENDED: draw_ended(); break;
    }
}

void phone_draw_overlay(void)
{
    char b[8];
    struct row_ink ink;
    if (!P.vol_until) return;
    int x = 200, y = 300, w = 400, h = 76, v = P.vol[P.vol_shown];
    ui_popup(x, y, w, h, B_PLAIN);
    ui_dialog_ink(&ink);
    t48_text(&ui, x + 20, y + 12, &t48_font_bold, ink.text, vol_labels[P.vol_shown]);
    snprintf(b, sizeof b, "%d", v);
    t48_text_right(&ui, x + w - 20, y + 12, &t48_font_bold, ink.text, b);
    for (int i = 0; i < T48AC_VOL_MAX; i++)
        t48_fill(&ui, x + 20 + i * 24, y + 46, 20, 14, i < v ? ink.text : t48_mix(ink.text, ui_ground(), 200));
}

void phone_touch(int x, int y)
{
    int k;
    switch (P.st) {
    case PH_DIAL:
        if ((k = keypad_hit(x, y))) phone_key(k, 1);
        else if (ui_inside(x, y, 24, ROW_Y, 216, ROW_H)) phone_key(T48_KEY_OK, 1);
        else if (ui_inside(x, y, 252, ROW_Y, 108, ROW_H)) { if (P.number[0]) phone_key(T48_KEY_BACK, 1); }
        else if (ui_inside(x, y, 372, ROW_Y, 100, ROW_H)) { go_idle(); changed(); }
        else if (accts_on() > 1 && ui_inside(x, y, 24, FROM_Y, 448, FROM_H)) phone_key(T48_KEY_RIGHT, 1);
        break;
    case PH_OUTGOING:
        if (ui_inside(x, y, 180, ROW_Y, 200, ROW_H)) phone_key(SK_SPEAKER, 1);
        else if (ui_inside(x, y, 420, ROW_Y, 200, ROW_H)) { hang_up(); changed(); }
        break;
    case PH_INCOMING:
        if (ui_inside(x, y, 120, 372, 260, 76)) phone_key(T48_KEY_OK, 1);
        else if (ui_inside(x, y, 420, 372, 260, 76)) phone_key(T48_KEY_BACK, 1);
        break;
    case PH_INCALL:
        if ((P.keypad || P.xfer) && (k = keypad_hit(x, y))) { phone_key(k, 1); break; }
        if (P.xfer) {
            if (ui_inside(x, y, 24, ROW_Y, 132, ROW_H)) { do_transfer(); changed(); }
            else if (ui_inside(x, y, 164, ROW_Y, 132, ROW_H)) phone_key(T48_KEY_OK, 1);
            else if (ui_inside(x, y, 304, ROW_Y, 80, ROW_H)) { delete_char(); changed(); }
            else if (ui_inside(x, y, 392, ROW_Y, 80, ROW_H)) { P.xfer = 0; P.number[0] = 0; changed(); }
            break;
        }
        if (P.o_st && !P.keypad && ui_inside(x, y, 548, BAN_Y, 108, BAN_H)) {
            if (P.o_st == O_WAITING) answer_waiting(); else swap_calls();
            update_lamps();
            changed();
            break;
        }
        if (P.o_st && !P.keypad && ui_inside(x, y, 664, BAN_Y, 108, BAN_H)) {
            if (P.o_st == O_WAITING) decline_waiting(); else join_calls();
            update_lamps();
            changed();
            break;
        }
        for (int i = 0; i < 6; i++) {
            if (!ui_inside(x, y, 24 + i * 127, ROW_Y, 117, ROW_H)) continue;
            switch (i) {
            case 0: phone_key(SK_MUTE, 1); break;
            case 1: phone_key(SK_HOLD, 1); break;
            case 2: P.keypad = !P.keypad; changed(); break;
            case 3:
                /* the button moves the sound to the loudspeaker and back; it never hangs up */
                if (P.route != T48AC_ROUTE_SPEAKER) { set_route(T48AC_ROUTE_SPEAKER); unpark(); }
                else if (P.hook_off) set_route(T48AC_ROUTE_HANDSET);
                update_lamps();
                changed();
                break;
            case 4: phone_key(SK_TRANSFER, 1); break;
            case 5: hang_up(); update_lamps(); changed(); break;
            }
        }
        break;
    case PH_ENDED:
        go_idle();
        changed();
        break;
    case PH_IDLE:
        break;
    }
}

/* ---------- call history ---------- */

#define HIST_BTN_Y 400
#define HIST_BTN_H 56

void phone_open_history(void)
{
    P.hist_sel = 0;
    P.hist_confirm = 0;
    if (P.missed) {
        hist_seen = (long long)time(NULL);
        count_missed();
        history_save();
        update_lamps();
    }
    shell_show_history(1);
}

void phone_draw_history(void)
{
    time_t now = time(NULL);
    struct tm today, tm;

    localtime_r(&now, &today);
    ui_clear(C_BG);
    ui_header("Calls", 1, 0);
    if (!nhist) {
        t48_text_centre(&ui, SW / 2, 200, &t48_font_head, C_DIM, "No calls");
        return;
    }
    if (P.hist_sel >= nhist) P.hist_sel = nhist - 1;
    int top = P.hist_sel >= HIST_ROWS ? P.hist_sel - HIST_ROWS + 1 : 0, several = accts_on() > 1;
    for (int r = 0; r < HIST_ROWS && top + r < nhist; r++) {
        const struct hist *h = &hist[top + r];
        int y = 54 + r * 52, selected = top + r == P.hist_sel;
        char when[32], dur[16] = "";
        const char *name = hist_name(h);
        time_t t = (time_t)h->when;
        struct row_ink ink;

        localtime_r(&t, &tm);
        strftime(when, sizeof when, tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year ? "%H:%M" : "%e %b %H:%M", &tm);
        if (h->secs) snprintf(dur, sizeof dur, "%d:%02d", h->secs / 60, h->secs % 60);
        ui_row(16, y, SW - 32, 46, selected, &ink);
        t48_text(&ui, 36, y + 12, &t48_font_bold, selected ? ink.text : h->kind == 'm' ? C_BAD : h->kind == 'o' ? C_DIM : C_GOOD,
                 h->kind == 'm' ? "Missed" : h->kind == 'o' ? "Out" : "In");
        t48_text_fit(&ui, 130, y + 12, several ? 250 : 380, &t48_font_normal, ink.text, name);
        if (strcmp(name, h->number)) {
            int w = t48_text_width(&t48_font_normal, name);
            if (w < (several ? 150 : 260)) t48_text_fit(&ui, 130 + w + 14, y + 15, (several ? 250 : 380) - w - 14, &t48_font_small, ink.dim, h->number);
        }
        if (several && h->acct >= 1 && h->acct <= MAX_ACCTS && A[h->acct - 1].used)
            t48_text_fit(&ui, 392, y + 15, 150, &t48_font_small, ink.dim, acct_label(h->acct - 1));
        t48_text_right(&ui, 620, y + 12, &t48_font_normal, ink.dim, dur);
        t48_text_right(&ui, SW - 34, y + 12, &t48_font_normal, ink.dim, when);
    }
    if (P.hist_confirm) {
        t48_text(&ui, 24, HIST_BTN_Y + (HIST_BTN_H - t48_font_bold.height) / 2, &t48_font_bold, C_TEXT, "Clear all calls?");
        ui_btn(416, HIST_BTN_Y, 170, HIST_BTN_H, "Cancel", B_PLAIN);
        ui_btn(596, HIST_BTN_Y, 188, HIST_BTN_H, "Clear", B_STOP);
    } else {
        int known = contacts_name(hist[P.hist_sel].number) != NULL;
        ui_btn(16, HIST_BTN_Y, 180, HIST_BTN_H, "Dial", B_GO);
        ui_btn(206, HIST_BTN_Y, 200, HIST_BTN_H, known ? "In contacts" : "Add contact", known ? B_OFF : B_PLAIN);
        ui_btn(416, HIST_BTN_Y, 170, HIST_BTN_H, "Delete", B_PLAIN);
        ui_btn(596, HIST_BTN_Y, 188, HIST_BTN_H, "Clear all", B_PLAIN);
    }
}

static void history_pick(int i)
{
    char n[64];
    int acct;
    if (i < 0 || i >= nhist) return;
    snprintf(n, sizeof n, "%s", hist[i].number);
    acct = hist[i].acct ? hist[i].acct - 1 : def_acct;
    shell_show_history(0);
    phone_prefill(n, acct);
}

static void history_delete(int i)
{
    if (i < 0 || i >= nhist) return;
    memmove(hist + i, hist + i + 1, sizeof hist[0] * (size_t)(nhist - i - 1));
    nhist--;
    if (P.hist_sel >= nhist && P.hist_sel > 0) P.hist_sel--;
    count_missed();
    history_save();
    update_lamps();
}

void phone_history_key(int code)
{
    if (P.hist_confirm) {
        if (code == T48_KEY_OK) { nhist = 0; P.hist_sel = 0; count_missed(); history_save(); update_lamps(); }
        if (code == T48_KEY_OK || code == T48_KEY_BACK) P.hist_confirm = 0;
        changed();
        return;
    }
    if (code == T48_KEY_UP && P.hist_sel > 0) P.hist_sel--;
    else if (code == T48_KEY_DOWN && P.hist_sel < nhist - 1) P.hist_sel++;
    else if (code == T48_KEY_OK) { history_pick(P.hist_sel); return; }
    else if (code == T48_KEY_BACK) { shell_show_history(0); return; }
    changed();
}

void phone_history_touch(int x, int y)
{
    int top = P.hist_sel >= HIST_ROWS ? P.hist_sel - HIST_ROWS + 1 : 0;
    if (!nhist) return;
    if (P.hist_confirm) {
        if (ui_inside(x, y, 596, HIST_BTN_Y, 188, HIST_BTN_H)) phone_history_key(T48_KEY_OK);
        else if (ui_inside(x, y, 416, HIST_BTN_Y, 170, HIST_BTN_H)) phone_history_key(T48_KEY_BACK);
        return;
    }
    for (int r = 0; r < HIST_ROWS && top + r < nhist; r++)
        if (ui_inside(x, y, 16, 54 + r * 52, SW - 32, 46)) {
            /* a touch chooses; a touch on the one already chosen puts its number in the dialler */
            if (top + r == P.hist_sel) history_pick(top + r);
            else { P.hist_sel = top + r; changed(); }
            return;
        }
    if (ui_inside(x, y, 16, HIST_BTN_Y, 180, HIST_BTN_H)) history_pick(P.hist_sel);
    else if (ui_inside(x, y, 206, HIST_BTN_Y, 200, HIST_BTN_H)) {
        if (!contacts_name(hist[P.hist_sel].number)) {
            char n[64], name[64];
            snprintf(n, sizeof n, "%s", hist[P.hist_sel].number);
            snprintf(name, sizeof name, "%s", hist[P.hist_sel].name);
            shell_show_history(0);
            contacts_open_add(n, name);
        }
    }
    else if (ui_inside(x, y, 416, HIST_BTN_Y, 170, HIST_BTN_H)) { history_delete(P.hist_sel); changed(); }
    else if (ui_inside(x, y, 596, HIST_BTN_Y, 188, HIST_BTN_H)) { P.hist_confirm = 1; changed(); }
}

/* a soft key is a touch on the button above it: Dial, Add contact, Delete, Clear all */
void phone_history_soft(int b)
{
    static const int mid[4] = {16 + 90, 206 + 100, 416 + 85, 596 + 94};
    phone_history_touch(mid[b & 3], HIST_BTN_Y + HIST_BTN_H / 2);
}

/* ---------- for the idle screen and the simulator ---------- */

int phone_missed(void) { return P.missed; }

static void acct_status(int i, char *out, size_t n, uint32_t *colour, const char *name)
{
    *colour = C_DIM;
    if (A[i].reg == REG_OK) { snprintf(out, n, "%s", name); *colour = C_GOOD; }
    else if (A[i].reg == REG_FAIL) { snprintf(out, n, "%s: not registered", name); *colour = C_BAD; }
    else if (A[i].reg == REG_TRYING) { snprintf(out, n, "%s: registering", name); *colour = C_ACCENT; }
    else snprintf(out, n, "%s: off", name);
}

/* the usual account and how it stands, for the status bar */
void phone_account_text(char *out, size_t n, uint32_t *colour)
{
    char acc[140];
    *colour = C_DIM;
    if (A[def_acct].used) snprintf(acc, sizeof acc, "%s@%s", A[def_acct].user, A[def_acct].domain);
    else snprintf(acc, sizeof acc, "Account");
    if (!opt.sip || !sipc_connected()) { snprintf(out, n, "No service"); *colour = C_BAD; }
    else if (!A[def_acct].on || A[def_acct].reg == REG_NONE) snprintf(out, n, "No account");
    else acct_status(def_acct, out, n, colour, acc);
}

/* How many accounts are switched on, and for the k-th of them (from 0) a short
 * line for the idle screen and a colour for its dot. Returns the account's
 * number, 1 to 6, or 0 if there is no k-th. */
int phone_account_line(int k, char *out, size_t n, uint32_t *colour, int *is_default)
{
    for (int i = 0; i < MAX_ACCTS; i++) {
        if (!A[i].on || k--) continue;
        acct_status(i, out, n, colour, acct_label(i));
        if (!sipc_connected()) *colour = C_BAD;
        if (A[i].msgs) {
            size_t at = strlen(out);
            if (A[i].msgs > 0) snprintf(out + at, n - at, "  (%d new)", A[i].msgs);
            else snprintf(out + at, n - at, "  (new)");
        }
        *is_default = i == def_acct;
        return i + 1;
    }
    return 0;
}

int phone_accounts(void) { return accts_on(); }

static const char *under(char *b)
{
    for (char *p = b; *p; p++) if (*p == ' ') *p = '_';
    return b[0] ? b : "-";
}

const char *phone_state(const char *key)
{
    static char b[96];
    const struct t48ac_state *a = audioc_state();

    if (!strcmp(key, "phone")) return ph_names[P.st];
    if (!strcmp(key, "route")) return route_names[P.route];
    if (!strcmp(key, "mute")) return P.muted ? "1" : "0";
    if (!strcmp(key, "hold")) return P.c.held ? "1" : "0";
    if (!strcmp(key, "keypad")) return P.keypad ? "1" : "0";
    if (!strcmp(key, "xfer")) return P.xfer ? "1" : "0";
    if (!strcmp(key, "number")) return P.number[0] ? P.number : "-";
    if (!strcmp(key, "peer")) return P.c.peer[0] ? P.c.peer : "-";
    if (!strcmp(key, "name")) { snprintf(b, sizeof b, "%s", P.c.name); return under(b); }
    if (!strcmp(key, "dtmf")) return P.c.dtmf[0] ? P.c.dtmf : "-";
    if (!strcmp(key, "ended")) { snprintf(b, sizeof b, "%s", P.ended); return under(b); }
    if (!strcmp(key, "note")) { snprintf(b, sizeof b, "%s", P.note); return under(b); }
    /* the second call */
    if (!strcmp(key, "other")) return other_names[P.o_st];
    if (!strcmp(key, "other.peer")) return P.o_st && P.o.peer[0] ? P.o.peer : "-";
    if (!strcmp(key, "other.name")) { snprintf(b, sizeof b, "%s", P.o_st ? P.o.name : ""); return under(b); }
    if (!strcmp(key, "consult")) return P.consult ? "1" : "0";
    if (!strcmp(key, "parked")) return P.parked ? "1" : "0";
    if (!strcmp(key, "dnd")) return P.dnd ? "on" : "off";
    if (!strcmp(key, "messages")) { snprintf(b, sizeof b, "%d", phone_messages()); return b; }
    /* accounts: "reg" is the usual one's, "reg.2" the second's; "acct" the one in use, 1 to 6 */
    if (!strcmp(key, "reg") || !strncmp(key, "reg.", 4)) {
        int i = key[3] ? atoi(key + 4) - 1 : def_acct;
        if (i < 0 || i >= MAX_ACCTS || !A[i].on) return "none";
        return A[i].reg == REG_OK ? "ok" : A[i].reg == REG_FAIL ? "fail" : A[i].reg == REG_TRYING ? "trying" : "none";
    }
    if (!strcmp(key, "accounts")) { snprintf(b, sizeof b, "%d", accts_on()); return b; }
    if (!strcmp(key, "acct.default")) { snprintf(b, sizeof b, "%d", def_acct + 1); return b; }
    if (!strcmp(key, "acct")) {
        snprintf(b, sizeof b, "%d", (P.st == PH_IDLE || P.st == PH_DIAL || P.st == PH_ENDED ? P.dial_acct : P.c.acct) + 1);
        return b;
    }
    if (!strcmp(key, "sip")) return sipc_connected() ? "up" : "down";
    if (!strcmp(key, "audio")) return audioc_connected() && a ? "up" : "down";
    if (!strcmp(key, "missed")) { snprintf(b, sizeof b, "%d", P.missed); return b; }
    if (!strcmp(key, "history")) { snprintf(b, sizeof b, "%d", nhist); return b; }
    if (!strcmp(key, "history.sel")) { snprintf(b, sizeof b, "%d", P.hist_sel); return b; }
    if (!strcmp(key, "last")) { if (!nhist) return "-"; snprintf(b, sizeof b, "%c:%s", hist[0].kind, hist[0].number); return b; }
    if (!strcmp(key, "last.acct")) { snprintf(b, sizeof b, "%d", nhist ? hist[0].acct : 0); return b; }
    if (!strcmp(key, "last.name")) { snprintf(b, sizeof b, "%s", nhist ? hist_name(&hist[0]) : ""); return under(b); }
    if (!strncmp(key, "vol.", 4)) {
        for (int i = 1; i < T48AC_VOL_COUNT; i++)
            if (!strcmp(key + 4, vol_keys[i])) { snprintf(b, sizeof b, "%d", P.vol[i]); return b; }
        return NULL;
    }
    if (!strncmp(key, "led.", 4)) return leds_state(key + 4);
    /* what the audio service itself says, not what we asked for */
    if (!strncmp(key, "audio.", 6)) {
        int v = -1;
        if (!a) return "?";
        key += 6;
        if (!strcmp(key, "route")) return a->route < T48AC_ROUTE_COUNT ? route_names[a->route] : "?";
        if (!strcmp(key, "mute")) v = a->mute;
        else if (!strcmp(key, "tone")) v = a->tone;
        else if (!strcmp(key, "ring")) v = a->ring;
        else if (!strcmp(key, "call")) v = a->call;
        else if (!strcmp(key, "app")) v = a->app;
        else if (!strcmp(key, "app.sound")) v = a->app_sound;
        else if (!strncmp(key, "vol.", 4)) {
            for (int i = 1; i < T48AC_VOL_COUNT; i++)
                if (!strcmp(key + 4, vol_keys[i])) v = i == T48AC_VOL_APP ? a->vol_app : a->vol[i];
        }
        if (v < 0) return NULL;
        snprintf(b, sizeof b, "%d", v);
        return b;
    }
    return NULL;
}
