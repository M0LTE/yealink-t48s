// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * t48-shell: the phone (issue #26). The dialler, the call screens, call
 * history, and what the hook, Speakerphone, Headset, Mute, Hold, Redial,
 * Transfer and Volume keys do. See docs/phone.md.
 *
 * The call itself lives in baresip and the sound in the audio service; this
 * file is the glue and the face. It tells baresip what to do over sipc.c,
 * tells the audio service where the sound goes over audioc.c, and follows
 * baresip's events. It keeps one call. A second one arriving is told "busy".
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
static const char *const vol_labels[] = {"", "Handset volume", "Speaker volume", "Headset volume", "Ringer volume"};

enum reg { REG_NONE, REG_TRYING, REG_OK, REG_FAIL };

#define ENDED_MS 2500          /* how long "Call ended" stays up */
#define ENDED_TONE_MS 30000    /* a failed call's tone, if nobody hangs up */
#define SETTLE_MS 3000         /* after start: time for baresip and the audio service to say how things stand */
#define VOL_SHOW_MS 1500
#define HIST_MAX 50
#define HIST_ROWS 7

static struct {
    enum ph st;
    int hook_off;
    int route;                 /* T48AC_ROUTE_*: where we have asked for the sound to go */
    int tone, ring;            /* what we have asked the audio service to play */
    char number[64];           /* being typed: a number to call, or to transfer to */
    char call_id[160];         /* baresip's id for our call, "" before it has one */
    char peer[96], peer_name[64];
    int outgoing, answered, ringing, rejected;
    int muted, held, remote_held;
    int keypad;                /* in a call: the tone keypad is showing */
    int xfer;                  /* in a call: typing a number to transfer to */
    char dtmf[24];             /* tones sent in this call, as typed */
    char note[64];             /* a line for the call screen: "Transfer failed" */
    long long t_state, t_established;
    char ended[64], ended_detail[64];
    int ended_ms;
    enum reg reg;
    char reg_why[64];
    char account[96];          /* user@domain from the settings file, "" if there is none */
    int vol[T48AC_VOL_COUNT];
    int vol_shown;             /* which volume the bar is showing */
    long long vol_until;
    long long started;
    int settled, audio_seen;
    int missed;
    int hist_sel;
} P;

struct hist { long long when; char kind; int secs; char number[64], name[64]; };
static struct hist hist[HIST_MAX];   /* newest first */
static int nhist;
static long long hist_seen;          /* missed calls before this have been looked at */

/* ---------- small things ---------- */

static void changed(void) { shell_phone_changed(); }

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
    leds_set(LAMP_POWER, P.st == PH_INCOMING ? LAMP_BLINK : P.missed ? LAMP_ON : LAMP_OFF);
    leds_set(LAMP_MESSAGE, LAMP_OFF);   /* voicemail waiting: issue #27 */
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

static void load_settings(void)
{
    char path[300], line[200], user[64] = "", domain[64] = "";
    FILE *f;

    for (int i = 1; i < T48AC_VOL_COUNT; i++) P.vol[i] = 10;
    snprintf(path, sizeof path, "%s/audio", opt.config);
    if ((f = fopen(path, "r"))) {
        while (fgets(line, sizeof line, f)) {
            int v;
            for (int i = 1; i < T48AC_VOL_COUNT; i++) {
                const char *name = i < 4 ? route_names[i] : "ringer";
                size_t k = strlen(name);
                if (!strncmp(line, name, k) && line[k] == '=' && sscanf(line + k + 1, "%d", &v) == 1 && v >= 0 && v <= T48AC_VOL_MAX)
                    P.vol[i] = v;
            }
        }
        fclose(f);
    }
    /* the account, for the idle screen. The password in this file is baresip's business, not ours. */
    snprintf(path, sizeof path, "%s/sip", opt.config);
    if ((f = fopen(path, "r"))) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "user=", 5)) snprintf(user, sizeof user, "%s", line + 5);
            else if (!strncmp(line, "domain=", 7)) snprintf(domain, sizeof domain, "%s", line + 7);
        }
        fclose(f);
        if (user[0] && domain[0]) snprintf(P.account, sizeof P.account, "%s@%s", user, domain);
    }
}

void phone_reload_account(void)
{
    P.account[0] = 0;
    load_settings();
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
    static const char *const keys[] = {"", "handset", "speaker", "headset", "ringer"};
    for (int i = 1; i < T48AC_VOL_COUNT; i++) {
        char v[8];
        snprintf(v, sizeof v, "%d", P.vol[i]);
        cfg_set("audio", keys[i], v);
    }
}

static void count_missed(void)
{
    P.missed = 0;
    for (int i = 0; i < nhist; i++)
        if (hist[i].kind == 'm' && hist[i].when > hist_seen) P.missed++;
}

/* one call a line, newest last: when|kind|seconds|number|name. kind is o
 * (outgoing), i (incoming, answered or turned away) or m (missed). */
static void history_load(void)
{
    char path[300], line[300];
    snprintf(path, sizeof path, "%s/history", opt.state);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        struct hist h = {0};
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "seen %lld", &hist_seen) == 1) continue;
        if (sscanf(line, "%lld|%c|%d|%63[^|]|%63[^\n]", &h.when, &h.kind, &h.secs, h.number, h.name) < 4) continue;
        memmove(hist + 1, hist, sizeof hist[0] * (HIST_MAX - 1));
        hist[0] = h;
        if (nhist < HIST_MAX) nhist++;
    }
    fclose(f);
    count_missed();
}

static void history_save(void)
{
    char path[300], tmp[310];
    snprintf(path, sizeof path, "%s/history", opt.state);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    mkdir(opt.state, 0700);
    FILE *f = fopen(tmp, "w");
    if (!f) { logmsg("phone: cannot write %s", tmp); return; }
    fprintf(f, "seen %lld\n", hist_seen);
    for (int i = nhist - 1; i >= 0; i--)
        fprintf(f, "%lld|%c|%d|%s|%s\n", hist[i].when, hist[i].kind, hist[i].secs, hist[i].number, hist[i].name);
    if (fclose(f) == 0) rename(tmp, path);
}

static void history_add(char kind, const char *number, const char *name, int secs)
{
    struct hist h = {.when = (long long)time(NULL), .kind = kind, .secs = secs};
    snprintf(h.number, sizeof h.number, "%s", number[0] ? number : "unknown");
    snprintf(h.name, sizeof h.name, "%s", name);
    for (char *p = h.number; *p; p++) if (*p == '|') *p = '/';
    for (char *p = h.name; *p; p++) if (*p == '|') *p = '/';
    memmove(hist + 1, hist, sizeof hist[0] * (HIST_MAX - 1));
    hist[0] = h;
    if (nhist < HIST_MAX) nhist++;
    count_missed();
    history_save();
    logmsg("phone: history: %c %s %d s", kind, h.number, secs);
}

/* ---------- the call ---------- */

static void clear_call(void)
{
    P.call_id[0] = 0;
    P.outgoing = P.answered = P.ringing = P.rejected = 0;
    P.held = P.remote_held = P.keypad = P.xfer = 0;
    P.dtmf[0] = P.note[0] = 0;
}

static void go_idle(void)
{
    set_tone(T48AC_TONE_OFF);
    set_ring(0);
    set_route(T48AC_ROUTE_NONE);
    clear_call();
    P.number[0] = 0;
    set_state(PH_IDLE);
}

/* the call is over (or never began): write it down, say so for a moment */
static void finish(const char *text, const char *detail, int tone)
{
    char who[64];
    int secs = P.answered ? (int)((t48_now_ms() - P.t_established) / 1000) : 0;

    snprintf(who, sizeof who, "%s", P.peer);
    history_add(P.outgoing ? 'o' : (P.answered || P.rejected) ? 'i' : 'm', who, P.peer_name, secs);
    snprintf(P.ended, sizeof P.ended, "%s", text);
    if (detail) snprintf(P.ended_detail, sizeof P.ended_detail, "%s", detail);
    else if (P.answered) snprintf(P.ended_detail, sizeof P.ended_detail, "%d:%02d", secs / 60, secs % 60);
    else P.ended_detail[0] = 0;
    set_ring(0);
    if (P.muted) { P.muted = 0; audioc_mute(0); }
    /* a tone only where there is an ear to hear it: not from the loudspeaker of a phone on the hook */
    if (P.route == T48AC_ROUTE_NONE) tone = T48AC_TONE_OFF;
    set_tone(tone);
    P.ended_ms = tone != T48AC_TONE_OFF ? ENDED_TONE_MS : ENDED_MS;
    clear_call();
    P.number[0] = 0;
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

static void place_call(const char *number, int route)
{
    char n[64];
    snprintf(n, sizeof n, "%s", number);   /* number may be P.number, which is about to be emptied */
    clear_call();
    P.outgoing = 1;
    snprintf(P.peer, sizeof P.peer, "%s", n);
    P.peer_name[0] = 0;
    P.number[0] = 0;
    set_route(route);
    set_tone(T48AC_TONE_OFF);
    if (!sipc_connected()) {
        finish("No phone service", "The SIP service is not running", T48AC_TONE_ERROR);
        return;
    }
    sipc_cmd("dial", n);
    set_state(PH_OUTGOING);
}

static void answer(int route)
{
    set_ring(0);            /* before the route, so the owner never hears a call waiting beep */
    set_tone(T48AC_TONE_OFF);
    set_route(route);
    sipc_cmd("accept", P.call_id);
    changed();
}

static void hang_up(void)
{
    if (P.call_id[0]) sipc_cmd("hangup", P.call_id);
    else if (P.st == PH_OUTGOING) sipc_cmd("hangup", NULL);
    if (P.st == PH_INCOMING) P.rejected = 1;
    finish(P.st == PH_INCOMING ? "Call declined" : "Call ended", NULL, T48AC_TONE_OFF);
}

static void incoming(const char *id, const char *peer, const char *name)
{
    clear_call();
    snprintf(P.call_id, sizeof P.call_id, "%s", id);
    uri_user(peer, P.peer, sizeof P.peer);
    snprintf(P.peer_name, sizeof P.peer_name, "%s", name);
    P.number[0] = 0;
    set_tone(T48AC_TONE_OFF);
    set_ring(1);
    set_state(PH_INCOMING);
}

static void established(void)
{
    set_tone(T48AC_TONE_OFF);
    set_ring(0);
    P.answered = 1;
    P.t_established = t48_now_ms();
    if (P.route == T48AC_ROUTE_NONE) set_route(route_for_now());
    set_state(PH_INCALL);
}

static void call_closed(const char *why)
{
    int code = atoi(why);
    if (P.st == PH_INCOMING) { finish("Missed call", NULL, T48AC_TONE_OFF); return; }
    if (P.st == PH_INCALL) {
        finish(strstr(why, "ransfer") ? "Call transferred" : "Call ended", NULL, T48AC_TONE_OFF);
        return;
    }
    /* an outgoing call that never connected */
    if (code == 486 || code == 600) finish("Busy", NULL, T48AC_TONE_BUSY);
    else if (code == 404 || code == 604) finish("Number not found", why, T48AC_TONE_ERROR);
    else if (code == 408 || code == 480) finish("No answer", why, T48AC_TONE_BUSY);
    else if (code == 603) finish("Call declined", NULL, T48AC_TONE_BUSY);
    else finish("Call failed", why, T48AC_TONE_ERROR);
}

void phone_sip_link(int up)
{
    if (!up) {
        P.reg = REG_NONE;
        if (P.st == PH_OUTGOING || P.st == PH_INCOMING || P.st == PH_INCALL)
            finish("Call lost", "The SIP service stopped", T48AC_TONE_ERROR);
    }
    changed();
}

void phone_sip_response(const char *token, int ok, const char *data)
{
    if (ok) return;
    logmsg("sip: %s failed: %.80s", token, data);
    if (!strcmp(token, "dial") && P.st == PH_OUTGOING && !P.call_id[0]) {
        finish("Call failed", "That number cannot be called", T48AC_TONE_ERROR);
    } else if (!strcmp(token, "hold") || !strcmp(token, "resume")) {
        P.held = !P.held;
        changed();
    } else if (!strcmp(token, "transfer")) {
        snprintf(P.note, sizeof P.note, "Transfer failed");
        changed();
    }
}

void phone_sip_event(const char *type, const char *j, const char *e)
{
    char id[160] = "", peer[160] = "", name[64] = "", dir[12] = "", param[100] = "";
    double secs = 0;
    int sync = t48_json_true(t48_json_get(j, e, "sync"), e);

    t48_json_str(t48_json_get(j, e, "param"), e, param, sizeof param);
    if (!strncmp(type, "REGISTER", 8) || !strcmp(type, "UNREGISTERING")) {
        enum reg was = P.reg;
        P.reg = !strcmp(type, "REGISTER_OK") ? REG_OK : !strcmp(type, "REGISTER_FAIL") ? REG_FAIL : !strcmp(type, "REGISTERING") ? REG_TRYING : REG_NONE;
        snprintf(P.reg_why, sizeof P.reg_why, "%s", param);
        if (P.reg != was) logmsg("phone: registration %s %s", type + (type[0] == 'R' ? 9 : 0), param);
        changed();
        return;
    }
    if (strncmp(type, "CALL_", 5)) return;
    t48_json_str(t48_json_get(j, e, "id"), e, id, sizeof id);
    t48_json_str(t48_json_get(j, e, "peeruri"), e, peer, sizeof peer);
    t48_json_str(t48_json_get(j, e, "peerdisplayname"), e, name, sizeof name);
    t48_json_str(t48_json_get(j, e, "direction"), e, dir, sizeof dir);
    t48_json_num(t48_json_get(j, e, "duration"), e, &secs);
    type += 5;
    int ours = P.call_id[0] && !strcmp(id, P.call_id);
    int out = !strcmp(dir, "outgoing");
    int free_now = P.st == PH_IDLE || P.st == PH_ENDED || P.st == PH_DIAL;

    if (!strcmp(type, "INCOMING")) {
        if (ours) return;
        if (free_now) {
            incoming(id, peer, name);
        } else {
            /* one call at a time: the second caller hears busy, and we note that they tried */
            char arg[200], who[64];
            snprintf(arg, sizeof arg, "%s scode=486", id);
            sipc_cmd("hangup", arg);
            uri_user(peer, who, sizeof who);
            history_add('m', who, name, 0);
            update_lamps();
            changed();
        }
        return;
    }
    /* our own outgoing call gets its id */
    if (!ours && out && P.st == PH_OUTGOING && !P.call_id[0] && strcmp(type, "CLOSED")) {
        snprintf(P.call_id, sizeof P.call_id, "%s", id);
        ours = 1;
    }
    /* a call that was already up when the shell started: pick it up */
    if (!ours && sync && (P.st == PH_IDLE || (P.st == PH_DIAL && !P.number[0])) && strcmp(type, "CLOSED")) {
        clear_call();
        snprintf(P.call_id, sizeof P.call_id, "%s", id);
        uri_user(peer, P.peer, sizeof P.peer);
        snprintf(P.peer_name, sizeof P.peer_name, "%s", name);
        P.outgoing = out;
        P.held = t48_json_true(t48_json_get(j, e, "onhold"), e);
        logmsg("phone: picking up a call that was already there (%s)", type);
        if (!strcmp(type, "ESTABLISHED")) {
            established();
            P.t_established -= (long long)(secs * 1000);
        } else {
            set_tone(T48AC_TONE_OFF);
            if (P.route == T48AC_ROUTE_NONE) set_route(route_for_now());
            set_state(PH_OUTGOING);
        }
        return;
    }
    /* the far end has transferred us: baresip made the new call itself. Follow it. */
    if (!ours && out && P.st == PH_INCALL && !strcmp(type, "ESTABLISHED")) {
        snprintf(P.call_id, sizeof P.call_id, "%s", id);
        uri_user(peer, P.peer, sizeof P.peer);
        snprintf(P.peer_name, sizeof P.peer_name, "%s", name);
        P.held = P.remote_held = 0;
        changed();
        return;
    }
    if (!ours) return;

    if (!strcmp(type, "RINGING")) {
        P.ringing = 1;
        set_tone(T48AC_TONE_RINGBACK);
        changed();
    } else if (!strcmp(type, "PROGRESS")) {
        P.ringing = 1;
        set_tone(T48AC_TONE_OFF);   /* the far end is sending its own ringing sound */
        changed();
    } else if (!strcmp(type, "ESTABLISHED")) {
        if (P.st != PH_INCALL) established();
    } else if (!strcmp(type, "CLOSED")) {
        call_closed(param);
    } else if (!strcmp(type, "HOLD")) {
        P.remote_held = 1;
        changed();
    } else if (!strcmp(type, "RESUME")) {
        P.remote_held = 0;
        changed();
    } else if (!strcmp(type, "TRANSFER_FAILED")) {
        snprintf(P.note, sizeof P.note, "Transfer failed");
        P.held = 0;                 /* baresip takes the call off hold again */
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
    P.held = 1;      /* baresip holds the call while it asks */
    P.xfer = 0;
    P.number[0] = 0;
}

static void toggle_mute(void)
{
    if (P.st == PH_INCOMING) { set_ring(0); return; }   /* quiet, but still ringing on the screen */
    if (P.st != PH_INCALL && P.st != PH_OUTGOING) return;
    P.muted = !P.muted;
    audioc_mute(P.muted);
    logmsg("phone: microphone %s", P.muted ? "muted" : "live");
}

static void toggle_hold(void)
{
    if (P.st != PH_INCALL) return;
    P.held = !P.held;
    sipc_cmd(P.held ? "hold" : "resume", P.call_id);
}

static void redial(void)
{
    if (P.st != PH_IDLE && P.st != PH_DIAL && P.st != PH_ENDED) return;
    for (int i = 0; i < nhist; i++)
        if (hist[i].kind == 'o') { place_call(hist[i].number, route_for_now()); return; }
    logmsg("phone: nothing to redial");
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
        if (P.number[0] && P.route == T48AC_ROUTE_NONE) place_call(P.number, r);
        else if (P.route == r) { if (P.hook_off) set_route(T48AC_ROUTE_HANDSET); else go_idle(); }
        else { set_route(r); if (!P.number[0]) set_tone(T48AC_TONE_DIAL); }
        break;
    case PH_INCOMING:
        answer(r);
        break;
    case PH_OUTGOING:
    case PH_INCALL:
        if (P.route == r) { if (P.hook_off) set_route(T48AC_ROUTE_HANDSET); else hang_up(); }
        else set_route(r);
        break;
    }
}

static void volume_key(int step)
{
    int which = P.route != T48AC_ROUTE_NONE ? P.route : T48AC_VOL_RINGER;
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
    if (P.number[0]) place_call(P.number, route_for_now());
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
    case SK_MESSAGE: logmsg("phone: Message key (voicemail is issue #27)"); break;
    case SK_TRANSFER:
        if (P.st != PH_INCALL) break;
        if (P.xfer && P.number[0]) do_transfer();
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
            if (P.xfer) {
                if (digit) type_char(code);
                else if (code == T48_KEY_OK) do_transfer();
                else if (code == T48_KEY_BACK) { if (P.number[0]) delete_char(); else P.xfer = 0; }
            } else if (digit) {
                char d[2] = {(char)code, 0};
                size_t k = strlen(P.dtmf);
                if (k + 1 >= sizeof P.dtmf) { memmove(P.dtmf, P.dtmf + 1, k); k--; }
                P.dtmf[k] = (char)code;
                P.dtmf[k + 1] = 0;
                sipc_cmd("sndcode", d);
                audioc_key(code);
            } else if (code == T48_KEY_BACK) {
                P.keypad = 0;
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
    if (code == T48_KEY_UP || code == T48_KEY_DOWN) { phone_open_history(); return 1; }
    return 0;
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
            if (P.number[0] && P.route == T48AC_ROUTE_NONE) place_call(P.number, T48AC_ROUTE_HANDSET);
            else { set_route(T48AC_ROUTE_HANDSET); if (!P.number[0]) set_tone(T48AC_TONE_DIAL); }
            break;
        case PH_INCOMING:
            answer(T48AC_ROUTE_HANDSET);
            break;
        case PH_OUTGOING:
        case PH_INCALL:
            set_route(T48AC_ROUTE_HANDSET);
            break;
        }
    } else if (P.route == T48AC_ROUTE_HANDSET) {
        /* putting the handset down ends whatever was on it. A call on the speaker or headset carries on. */
        switch (P.st) {
        case PH_OUTGOING:
        case PH_INCALL:
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
        long long s = (now - P.t_established) / 1000;
        if (s != shown_secs) { shown_secs = s; changed(); }
    }
}

void phone_init(void)
{
    P.started = t48_now_ms();
    load_settings();
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

static void keypad_draw(void)
{
    for (int i = 0; i < 12; i++) {
        int x = KP_X + (i % 3) * (KP_W + KP_GAP), y = KP_Y + (i / 3) * (KP_H + KP_GAP);
        char s[2] = {kp_chars[i], 0};
        t48_round_fill(&ui, x, y, KP_W, KP_H, 10, C_KEY);
        t48_text_centre(&ui, x + KP_W / 2, y + (KP_H - t48_font_big.height) / 2, &t48_font_big, C_TEXT, s);
    }
}

static int keypad_hit(int x, int y)
{
    for (int i = 0; i < 12; i++)
        if (ui_inside(x, y, KP_X + (i % 3) * (KP_W + KP_GAP), KP_Y + (i / 3) * (KP_H + KP_GAP), KP_W, KP_H))
            return kp_chars[i] == '*' ? T48_KEY_STAR : kp_chars[i] == '#' ? T48_KEY_HASH : kp_chars[i];
    return 0;
}

/* a small label in a rounded box; returns the x after it */
static int chip(int x, int y, const char *text, uint32_t fill, uint32_t ink)
{
    int w = t48_text_width(&t48_font_bold, text) + 28;
    t48_round_fill(&ui, x, y, w, 34, 17, fill);
    t48_text(&ui, x + 14, y + (34 - t48_font_bold.height) / 2, &t48_font_bold, ink, text);
    return x + w + 10;
}

/* the chips that say where the sound is and what is switched off; centred on cx, or from x if cx < 0 */
static void chips(int cx, int x, int y)
{
    const char *t[4];
    uint32_t fill[4], ink[4];
    int n = 0, w = 0;

    t[n] = route_labels[P.route]; fill[n] = C_PANEL; ink[n++] = P.route ? C_GOOD : C_DIM;
    if (P.muted) { t[n] = "Muted"; fill[n] = C_BAD; ink[n++] = C_BG; }
    if (P.held) { t[n] = "On hold"; fill[n] = C_ACCENT; ink[n++] = C_BG; }
    if (P.remote_held) { t[n] = "Held by the other end"; fill[n] = C_PANEL; ink[n++] = C_ACCENT; }
    for (int i = 0; i < n; i++) w += t48_text_width(&t48_font_bold, t[i]) + 28 + (i ? 10 : 0);
    if (cx >= 0) x = cx - w / 2;
    for (int i = 0; i < n; i++) x = chip(x, y, t[i], fill[i], ink[i]);
}

/* a number or name as large as will fit in maxw, centred on cx; the end of a long number is the part kept */
static void big_text(int cx, int y, int maxw, const char *s, uint32_t c)
{
    const t48_font *f = &t48_font_big;
    if (t48_text_width(f, s) > maxw) { f = &t48_font_head; y += 8; }
    while (t48_text_width(f, s) > maxw && s[1]) s++;
    t48_text_centre(&ui, cx, y, f, c, s);
}

static const char *who(void) { return P.peer_name[0] ? P.peer_name : P.peer[0] ? P.peer : "Unknown"; }

static void draw_dial(void)
{
    ui_status_bar("Dial");
    t48_round_fill(&ui, 24, KP_Y, 448, 110, 14, C_PANEL);
    if (P.number[0]) big_text(248, KP_Y + 30, 420, P.number, C_TEXT);
    else t48_text_centre(&ui, 248, KP_Y + 40, &t48_font_head, C_FAINT, "Enter a number");
    chips(-1, 24, KP_Y + 130);
    const char *hint = !sipc_connected() ? "No phone service" :
        P.route == T48AC_ROUTE_NONE ? "Lift the handset, or press OK or Speaker" :
        P.number[0] ? "Press OK to call" : "Dial tone";
    t48_text(&ui, 28, KP_Y + 186, &t48_font_normal, sipc_connected() ? C_DIM : C_BAD, hint);
    keypad_draw();
    ui_button(24, ROW_Y, 216, ROW_H, "Call", P.number[0] ? C_GOOD : C_PANEL);
    ui_button(252, ROW_Y, 108, ROW_H, "Delete", C_PANEL);
    ui_button(372, ROW_Y, 100, ROW_H, "Cancel", C_PANEL);
}

static void draw_outgoing(void)
{
    ui_status_bar("Calling");
    t48_text_centre(&ui, SW / 2, 78, &t48_font_head, C_DIM, "Calling");
    big_text(SW / 2, 126, 720, who(), C_TEXT);
    t48_text_centre(&ui, SW / 2, 206, &t48_font_head, C_ACCENT, P.ringing ? "Ringing" : "Connecting");
    chips(SW / 2, 0, 276);
    ui_button(180, ROW_Y, 200, ROW_H, P.route == T48AC_ROUTE_SPEAKER ? "Speaker off" : "Speaker", P.route == T48AC_ROUTE_SPEAKER ? C_ACCENT : C_PANEL);
    ui_button(420, ROW_Y, 200, ROW_H, "End call", C_BAD);
}

static void draw_incoming(void)
{
    ui_status_bar("Incoming call");
    t48_text_centre(&ui, SW / 2, 78, &t48_font_head, C_ACCENT, "Incoming call");
    big_text(SW / 2, 126, 720, who(), C_TEXT);
    if (P.peer_name[0]) t48_text_centre(&ui, SW / 2, 192, &t48_font_head, C_DIM, P.peer);
    t48_text_centre(&ui, SW / 2, 268, &t48_font_normal, C_DIM,
                    P.route != T48AC_ROUTE_NONE ? "Press OK to answer" : "Lift the handset, or press Speaker or Headset, to answer");
    if (!P.ring) t48_text_centre(&ui, SW / 2, 304, &t48_font_normal, C_FAINT, "Ringer silenced");
    ui_button(120, 372, 260, 76, "Answer", C_GOOD);
    ui_button(420, 372, 260, 76, "Decline", C_BAD);
}

static void call_time(char *out, size_t n)
{
    long long s = (t48_now_ms() - P.t_established) / 1000;
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(out, n, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%02lld:%02lld", s / 60, s % 60);
}

static void draw_incall(void)
{
    char t[24];
    int side = P.keypad || P.xfer;       /* the keypad takes the right of the screen */
    int cx = side ? 248 : SW / 2, maxw = side ? 440 : 720;

    call_time(t, sizeof t);
    ui_status_bar(P.xfer ? "Transfer" : P.held ? "On hold" : "In call");
    if (P.xfer) {
        t48_text_centre(&ui, cx, 62, &t48_font_normal, C_DIM, "Transfer this call to");
        t48_round_fill(&ui, 24, 96, 448, 96, 14, C_PANEL);
        if (P.number[0]) big_text(cx, 120, 420, P.number, C_TEXT);
        else t48_text_centre(&ui, cx, 128, &t48_font_head, C_FAINT, "Enter a number");
        t48_text_centre(&ui, cx, 214, &t48_font_bold, C_TEXT, who());
        t48_text_centre(&ui, cx, 244, &t48_font_head, C_DIM, t);
    } else if (side) {
        big_text(cx, 66, maxw, who(), C_TEXT);
        t48_text_centre(&ui, cx, 132, &t48_font_big, P.held ? C_ACCENT : C_GOOD, t);
        if (P.dtmf[0]) {
            t48_round_fill(&ui, 24, 196, 448, 60, 12, C_PANEL);
            t48_text_centre(&ui, cx, 196 + (60 - t48_font_head.height) / 2, &t48_font_head, C_TEXT, P.dtmf);
        } else {
            t48_text_centre(&ui, cx, 214, &t48_font_normal, C_FAINT, "Keys send tones");
        }
    } else {
        big_text(cx, 60, maxw, who(), C_TEXT);
        if (P.peer_name[0]) t48_text_centre(&ui, cx, 112, &t48_font_normal, C_DIM, P.peer);
        t48_text_centre(&ui, cx, 142, &t48_font_huge, P.held ? C_ACCENT : C_TEXT, t);
    }
    if (side) keypad_draw();
    if (P.note[0]) t48_text_centre(&ui, cx, 274, &t48_font_normal, C_ACCENT, P.note);
    chips(cx, 0, 318);

    if (P.xfer) {
        ui_button(24, ROW_Y, 216, ROW_H, "Transfer", P.number[0] ? C_GOOD : C_PANEL);
        ui_button(252, ROW_Y, 108, ROW_H, "Delete", C_PANEL);
        ui_button(372, ROW_Y, 100, ROW_H, "Cancel", C_PANEL);
        return;
    }
    static const char *const label[6] = {"Mute", "Hold", "Keypad", "Speaker", "Transfer", "End"};
    for (int i = 0; i < 6; i++) {
        int on = (i == 0 && P.muted) || (i == 1 && P.held) || (i == 2 && P.keypad) || (i == 3 && P.route == T48AC_ROUTE_SPEAKER);
        const char *l = i == 0 && P.muted ? "Unmute" : i == 1 && P.held ? "Resume" : label[i];
        ui_button(24 + i * 127, ROW_Y, 117, ROW_H, l, i == 5 ? C_BAD : on ? C_ACCENT : C_PANEL);
    }
}

static void draw_ended(void)
{
    ui_status_bar("Call ended");
    t48_text_centre(&ui, SW / 2, 120, &t48_font_big, C_TEXT, P.ended);
    if (hist[0].number[0] && nhist) t48_text_centre(&ui, SW / 2, 196, &t48_font_head, C_DIM, hist[0].name[0] ? hist[0].name : hist[0].number);
    if (P.ended_detail[0]) t48_text_centre(&ui, SW / 2, 250, &t48_font_normal, C_DIM, P.ended_detail);
    if (P.tone != T48AC_TONE_OFF) t48_text_centre(&ui, SW / 2, 320, &t48_font_normal, C_FAINT, "Hang up to go back");
}

void phone_draw(void)
{
    t48_fill(&ui, 0, 0, SW, SH, C_BG);
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
    if (!P.vol_until) return;
    int x = 200, y = 300, w = 400, h = 76, v = P.vol[P.vol_shown];
    t48_round_fill(&ui, x - 2, y - 2, w + 4, h + 4, 16, C_LINE);
    t48_round_fill(&ui, x, y, w, h, 14, C_BAR);
    t48_text(&ui, x + 20, y + 10, &t48_font_bold, C_TEXT, vol_labels[P.vol_shown]);
    snprintf(b, sizeof b, "%d", v);
    t48_text_right(&ui, x + w - 20, y + 10, &t48_font_bold, C_ACCENT, b);
    for (int i = 0; i < T48AC_VOL_MAX; i++)
        t48_fill(&ui, x + 20 + i * 24, y + 44, 20, 16, i < v ? C_ACCENT : C_LINE);
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
            if (ui_inside(x, y, 24, ROW_Y, 216, ROW_H)) phone_key(T48_KEY_OK, 1);
            else if (ui_inside(x, y, 252, ROW_Y, 108, ROW_H)) { delete_char(); changed(); }
            else if (ui_inside(x, y, 372, ROW_Y, 100, ROW_H)) { P.xfer = 0; P.number[0] = 0; changed(); }
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
                if (P.route != T48AC_ROUTE_SPEAKER) set_route(T48AC_ROUTE_SPEAKER);
                else if (P.hook_off) set_route(T48AC_ROUTE_HANDSET);
                update_lamps();
                changed();
                break;
            case 4: phone_key(SK_TRANSFER, 1); break;
            case 5: hang_up(); changed(); break;
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

void phone_open_history(void)
{
    P.hist_sel = 0;
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
    t48_fill(&ui, 0, 0, SW, SH, C_BG);
    ui_status_bar("Calls");
    if (!nhist) {
        t48_text_centre(&ui, SW / 2, 200, &t48_font_head, C_DIM, "No calls yet");
        t48_text_centre(&ui, SW / 2, 244, &t48_font_normal, C_FAINT, "Press X to go back");
        return;
    }
    int top = P.hist_sel >= HIST_ROWS ? P.hist_sel - HIST_ROWS + 1 : 0;
    for (int r = 0; r < HIST_ROWS && top + r < nhist; r++) {
        const struct hist *h = &hist[top + r];
        int y = 54 + r * 52;
        char when[32], dur[16] = "";
        time_t t = (time_t)h->when;

        localtime_r(&t, &tm);
        strftime(when, sizeof when, tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year ? "%H:%M" : "%e %b %H:%M", &tm);
        if (h->secs) snprintf(dur, sizeof dur, "%d:%02d", h->secs / 60, h->secs % 60);
        t48_round_fill(&ui, 16, y, SW - 32, 46, 10, top + r == P.hist_sel ? C_KEY : C_PANEL);
        if (top + r == P.hist_sel) t48_fill(&ui, 16, y + 8, 5, 30, C_ACCENT);
        t48_text(&ui, 36, y + 12, &t48_font_bold, h->kind == 'm' ? C_BAD : h->kind == 'o' ? C_DIM : C_GOOD,
                 h->kind == 'm' ? "Missed" : h->kind == 'o' ? "Out" : "In");
        t48_text_fit(&ui, 130, y + 12, 380, &t48_font_normal, C_TEXT, h->name[0] ? h->name : h->number);
        t48_text_right(&ui, 620, y + 12, &t48_font_normal, C_DIM, dur);
        t48_text_right(&ui, SW - 34, y + 12, &t48_font_normal, C_DIM, when);
    }
    t48_text_centre(&ui, SW / 2, 438, &t48_font_small, C_FAINT, "Up and Down to choose, OK or a touch to put the number in the dialler, X to go back");
}

static void history_pick(int i)
{
    char n[64];
    if (i < 0 || i >= nhist) return;
    snprintf(n, sizeof n, "%s", hist[i].number);
    shell_show_history(0);
    start_dialler(P.hook_off ? T48AC_ROUTE_HANDSET : T48AC_ROUTE_NONE);
    set_tone(T48AC_TONE_OFF);
    snprintf(P.number, sizeof P.number, "%s", n);
    changed();
}

void phone_history_key(int code)
{
    if (code == T48_KEY_UP && P.hist_sel > 0) P.hist_sel--;
    else if (code == T48_KEY_DOWN && P.hist_sel < nhist - 1) P.hist_sel++;
    else if (code == T48_KEY_OK) { history_pick(P.hist_sel); return; }
    else if (code == T48_KEY_BACK) { shell_show_history(0); return; }
    changed();
}

void phone_history_touch(int x, int y)
{
    int top = P.hist_sel >= HIST_ROWS ? P.hist_sel - HIST_ROWS + 1 : 0;
    for (int r = 0; r < HIST_ROWS && top + r < nhist; r++)
        if (ui_inside(x, y, 16, 54 + r * 52, SW - 32, 46)) { history_pick(top + r); return; }
}

/* ---------- for the idle screen and the simulator ---------- */

int phone_missed(void) { return P.missed; }

void phone_account_text(char *out, size_t n, uint32_t *colour)
{
    const char *acc = P.account[0] ? P.account : "SIP account";
    *colour = C_DIM;
    if (!opt.sip || !sipc_connected()) { snprintf(out, n, "No phone service"); *colour = C_BAD; }
    else if (P.reg == REG_OK) { snprintf(out, n, "%s", acc); *colour = C_GOOD; }
    else if (P.reg == REG_FAIL) { snprintf(out, n, "%s: not registered", acc); *colour = C_BAD; }
    else if (P.reg == REG_TRYING) { snprintf(out, n, "%s: registering", acc); *colour = C_ACCENT; }
    else snprintf(out, n, "No SIP account");
}

const char *phone_state(const char *key)
{
    static char b[96];
    const struct t48ac_state *a = audioc_state();

    if (!strcmp(key, "phone")) return ph_names[P.st];
    if (!strcmp(key, "route")) return route_names[P.route];
    if (!strcmp(key, "mute")) return P.muted ? "1" : "0";
    if (!strcmp(key, "hold")) return P.held ? "1" : "0";
    if (!strcmp(key, "keypad")) return P.keypad ? "1" : "0";
    if (!strcmp(key, "xfer")) return P.xfer ? "1" : "0";
    if (!strcmp(key, "number")) return P.number[0] ? P.number : "-";
    if (!strcmp(key, "peer")) return P.peer[0] ? P.peer : "-";
    if (!strcmp(key, "dtmf")) return P.dtmf[0] ? P.dtmf : "-";
    if (!strcmp(key, "ended")) { snprintf(b, sizeof b, "%s", P.ended); for (char *p = b; *p; p++) if (*p == ' ') *p = '_'; return b; }
    if (!strcmp(key, "reg")) return P.reg == REG_OK ? "ok" : P.reg == REG_FAIL ? "fail" : P.reg == REG_TRYING ? "trying" : "none";
    if (!strcmp(key, "sip")) return sipc_connected() ? "up" : "down";
    if (!strcmp(key, "audio")) return audioc_connected() && a ? "up" : "down";
    if (!strcmp(key, "missed")) { snprintf(b, sizeof b, "%d", P.missed); return b; }
    if (!strcmp(key, "history")) { snprintf(b, sizeof b, "%d", nhist); return b; }
    if (!strcmp(key, "last")) { if (!nhist) return "-"; snprintf(b, sizeof b, "%c:%s", hist[0].kind, hist[0].number); return b; }
    if (!strncmp(key, "vol.", 4)) {
        for (int i = 1; i < T48AC_VOL_COUNT; i++)
            if (!strcmp(key + 4, i < 4 ? route_names[i] : "ringer")) { snprintf(b, sizeof b, "%d", P.vol[i]); return b; }
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
        else if (!strncmp(key, "vol.", 4)) {
            for (int i = 1; i < T48AC_VOL_COUNT; i++)
                if (!strcmp(key + 4, i < 4 ? route_names[i] : "ringer")) v = a->vol[i];
        }
        if (v < 0) return NULL;
        snprintf(b, sizeof b, "%d", v);
        return b;
    }
    return NULL;
}
