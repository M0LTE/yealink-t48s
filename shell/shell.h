// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: shared declarations. See docs/shell.md. */
#ifndef SHELL_H
#define SHELL_H

#include <poll.h>
#include <sys/types.h>
#include "t48.h"
#include "t48_audio_ctl.h"

/* The screen, as the backend found it: 800x480 on the T48S, 480x272 on the
 * T46S. The shell draws its own screens at this size; below 800x480 they are
 * laid out for a small screen (docs/shell.md). */
extern int screen_w, screen_h;
#define SW screen_w
#define SH screen_h
/* The picture an app gets unless its manifest says "screen": "any": the T48S's,
 * which the backend fits to a smaller screen. */
#define APP_W 800
#define APP_H 480

struct layout {
    int small;      /* a screen smaller than the T48S's: the small layout, with the soft keys' labels along the bottom */
    int touch;      /* the screen takes touches */
    int softkeys;   /* there are four soft keys under it */
    int turned;     /* simulator: the panel is 16 bits and mounted upside down, as the T46S's is */
};
extern struct layout L;
#define SMALL (L.small)
#define BAR_H T48_UI_BAR_H                   /* the soft keys' labels */
#define BAR_Y (SH - BAR_H)
#define LINE_W 88                            /* small screens: kept clear at each side of the idle screen for the line keys' labels */
void shell_screen_is(int w, int h, int touch, int softkeys);   /* from a backend's init */
/* small screens: a list's rows, from under the header to the soft keys' labels */
#define SROW_STEP 35
#define SROW_H 32
#define SROW_Y(r) (HEAD_H + 3 + (r) * SROW_STEP)
#define SROWS ((BAR_Y - HEAD_H - 4) / SROW_STEP)

/* Phone keys, which never go to apps. App keys use the protocol's T48_KEY_*
 * codes; the X key arrives from a backend as T48_KEY_BACK. */
/* The four soft keys under the screen of a phone that has them (the T46S).
 * They press the buttons along the bottom of Calls and Contacts, left to
 * right, and never go to apps. */
enum { SK_SOFT1 = 0x1f0, SK_SOFT2, SK_SOFT3, SK_SOFT4 };
enum { SK_VOLDOWN = 0x200, SK_VOLUP, SK_HEADSET, SK_MUTE, SK_MESSAGE, SK_HOLD, SK_REDIAL, SK_TRANSFER, SK_SPEAKER };

/* ---------- the screen and input devices: the phone's, or the simulator's ---------- */

struct backend {
    const char *name;
    int (*init)(void);
    /* Part x, y, w, h of a picture sw x sh (rows `stride` pixels apart) to the screen. A picture
     * the size of the screen is copied; any other (an 800x480 app on a small screen) is fitted. */
    void (*flush)(const uint32_t *src, int stride, int sw, int sh, int x, int y, int w, int h);
    int (*fds)(struct pollfd *p, int max);        /* descriptors to wait on */
    void (*handle)(const struct pollfd *p, int n); /* after poll: read input, call shell_key() etc. */
    int (*timeout)(long long now);                /* ms until tick() is wanted, -1: never */
    void (*tick)(long long now);
    void (*fini)(void);
    /* A screen that can show an app's flip buffers itself, with no copy (NULL: it
     * cannot). flip_info: 0 and a descriptor opened for this one app (the caller
     * closes it), how many buffers, the bytes from one to the next and the refresh
     * period; -1: not this screen.
     * flip_show: buffer n at the next refresh, then shell_flipped(); -1: the
     * shell's own picture again. flip_clear: all buffers to black. */
    int (*flip_info)(int *fd, int *count, size_t *size, unsigned *period_us);
    void (*flip_show)(int n);
    void (*flip_clear)(void);
};
extern const struct backend be_fb, be_sim;

struct options {
    const char *fb;      /* framebuffer device */
    const char *apps;    /* folders of installed apps, colon separated */
    const char *data;    /* where each app's own folder is made */
    const char *config;  /* settings files */
    const char *script;  /* simulator: input script */
    const char *shot;    /* simulator: write one picture here and stop */
    const char *start;   /* id of an app to start straight away */
    const char *state;   /* things the shell remembers: call history */
    const char *sip;     /* baresip's control socket (t48ctrl), NULL: no SIP */
    const char *audio;   /* the audio service's control socket, NULL: no audio */
    const char *apply;   /* program run with a settings file's name when that file has changed, NULL: none */
    const char *update;  /* the updater, run as "PROGRAM info", "check" and so on (t48-update), NULL: none */
    const char *access;  /* folder where t48-access leaves its status file (docs/access.md) */
    const char *notice;  /* a file of one or two lines shown on the idle screen while it exists (t48-live), NULL: none */
    const char *web;     /* folder where the web setup page leaves its status file (docs/web.md), NULL: none */
    const char *runner;  /* simulator: a program to run apps with, such as qemu-arm for the phone's own builds */
    const char *screen;  /* simulator: the screen's size, "480x272" */
    const char *model;   /* simulator: "t46s" is that phone's screen and keys: 480x272, 16 bits, upside down, no touch, soft keys */
    int no_touch;        /* simulator: a screen that takes no touches, and four soft keys under it */
};
extern struct options opt;

/* input, from a backend. It sets shell_input_us first: when it happened,
 * microseconds of CLOCK_MONOTONIC. */
extern long long shell_input_us;
void shell_flipped(int n, unsigned frame, long long time_us);   /* flip_show(n) is on the screen */
void shell_key(int code, int down);
/* A key as the keypad sent it: its place in the matrix (the driver's MSC_SCAN, row * 8 + column + 1; 0 if
 * not known), the Linux key code, the shell's code for that (0: none) and 1 down, 0 up, or -1 for a place
 * that came with no key event. It goes on to shell_key() unless the key test has the screen. */
void shell_scan(int scan, int key, int code, int value);
void shell_touch(int state, int x, int y);
void shell_hook(int off_hook);

const struct backend *shell_backend(void);

/* Part x, y, w, h of a picture sw x sh onto a screen pw x ph of bpp bits (32, or 16 as RGB565), `line`
 * bytes a row, upside down if turned. The same size: copied. Another: each screen pixel is the average
 * of the picture's pixels under it (fit.c). */
void fit_put(void *fb, int line, int pw, int ph, int bpp, int turned,
             const uint32_t *src, int stride, int sw, int sh, int x, int y, int w, int h);

/* for the simulator's scripts */
void shell_stop(int status);
const char *shell_screen_name(void);
unsigned shell_app_frames(void);
const t48_surface *shell_app_surface(void);

/* a word about how things stand, for the simulator's checks (docs/shell.md) */
const char *shell_state(const char *key);

void shell_mkdirs(const char *path);
void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------- drawing shared by the shell's own screens ---------- */

/* The look is libt48's (theme.c, ui.c: docs/design/README.md), so that apps have it too:
 * a theme, and the parts screens are made of. Here they are under the names the screens
 * use, drawing on the shell's own picture. ui.c picks the theme and tells the app. */
#define T (t48_theme_now())
#define C_BG (T->bg)
#define C_PANEL (T->panel)
#define C_KEY (T->key)
#define C_LINE (T->line)
#define C_TEXT (T->text)
#define C_DIM (T->dim)
#define C_FAINT (T->faint)
#define C_ACCENT (T->accent)
#define C_GOOD (T->good)
#define C_BAD (T->bad)

#define HEAD_H (t48_ui_head_h())             /* the header */
#define TAG_H (t48_ui_tag_h())
enum { B_PLAIN = T48_UI_PLAIN, B_FIRST = T48_UI_FIRST, B_GO = T48_UI_GO, B_STOP = T48_UI_STOP, B_LIT = T48_UI_LIT,
       B_OFF = T48_UI_OFF };                 /* what a button is for */
#define row_ink t48_ui_ink

extern t48_surface ui;                       /* the shell's own picture of the screen */
#define ui_clear(ground) t48_ui_clear(&ui, ground)
#define ui_ground() t48_ui_ground()
/* lamp: 0, or B_GO, B_LIT, B_STOP. The way back is drawn only where it can be touched. */
#define ui_header(title, back, lamp) t48_ui_header(&ui, title, (back) && L.touch, lamp)
#define HEAD_NOTE_X (SW - (SMALL ? 70 : 100))   /* a count in the header ends here, left of the time */
#define ui_btn(x, y, w, h, label, role) t48_ui_btn(&ui, x, y, w, h, label, role)
#define ui_key(x, y, w, h, label) t48_ui_key(&ui, x, y, w, h, label)
#define ui_panel(x, y, w, h) t48_ui_panel(&ui, x, y, w, h)
#define ui_row(x, y, w, h, selected, ink) t48_ui_row(&ui, x, y, w, h, selected, ink)
#define ui_tag(x, y, text, role) t48_ui_tag(&ui, x, y, text, role)
#define ui_tag_width(text) t48_ui_tag_width(text)
#define ui_switch(rx, y, h, on, selected) t48_ui_switch(&ui, rx, y, h, on, selected)
#define ui_level(x, y, w, h, level, max, disabled, selected) t48_ui_level(&ui, x, y, w, h, level, max, disabled, selected)
#define ui_lamp(x, y, colour) t48_ui_lamp(&ui, x, y, colour)
#define ui_chevron(cx, cy, n, dir, t, c) t48_ui_chevron(&ui, cx, cy, n, dir, t, c)
#define ui_dialog(x, y, w, h, tone) t48_ui_dialog(&ui, x, y, w, h, tone)
#define ui_popup(x, y, w, h, tone) t48_ui_popup(&ui, x, y, w, h, tone)
#define ui_dialog_ink(ink) t48_ui_dialog_ink(ink)
#define ui_dialog_btn(x, y, w, h, label, role) t48_ui_dialog_btn(&ui, x, y, w, h, label, role)
#define ui_notice(x, y, text, role) t48_ui_notice(&ui, x, y, text, role)
#define ui_notice_width(text) t48_ui_notice_width(text)
#define ui_tile(x, y, w, h, selected, ink) t48_ui_tile(&ui, x, y, w, h, selected, ink)
#define ui_icon(x, y, n, letter) t48_ui_icon(&ui, x, y, n, letter)

void ui_init(void);                          /* the theme in the settings store */
void ui_header_idle(void);
int ui_theme_set(const char *name);          /* at once, here and in the open app; -1: no such theme */
const char *ui_theme_label(const char *name);   /* "Two-tone" */
int ui_theme_reload(void);                   /* the store may have changed: 1 if the theme did */
const t48_font_family *ui_fonts(const char *name);   /* a theme's family; B612 if we have not got it */
/* the font file an app is given (docs/app-protocol.md): every family, malloc'd; NULL: no memory */
unsigned char *ui_fonts_file(size_t *len);
/* a small picture of a theme, for choosing one */
void ui_theme_preview(const t48_theme *t, int x, int y, int w, int h, int current);
void ui_network_status(char *out, size_t n, int *up);
/* text over up to two lines, broken at spaces: from x, or each line centred on x */
void ui_two_lines(int x, int y, int w, const t48_font *f, uint32_t c, const char *text);
int ui_text_scaled(int x, int y, const t48_font *f, uint32_t c, const char *str, int num, int den);
void ui_two_lines_centred(int cx, int y, int w, const t48_font *f, uint32_t c, const char *text);
/* Small screens: what the four soft keys do on the screen being drawn, left to right. A label NULL or
 * "": that key does nothing. Shown along the bottom edge after the screen is drawn; with a touch
 * screen each is a button. */
void ui_bar(const char *l0, int r0, const char *l1, int r1, const char *l2, int r2, const char *l3, int r3);
/* a question in a box, for the small layout: the answers are on the soft keys */
void ui_ask(const char *title, const char *text, const char *extra, int tone);
#define UI_ASK_H 150                         /* that box's height when it has more than a title */
int ui_inside(int x, int y, int bx, int by, int bw, int bh);

/* ---------- the phone (phone.c): calls, the dialler, history ---------- */

void phone_init(void);
void phone_key(int code, int down);          /* any key while a phone screen shows, and phone keys always */
int phone_idle_key(int code);                /* a key on the idle screen: 1 if the phone took it */
void phone_touch(int x, int y);              /* a tap on a phone screen */
void phone_hook(int off_hook);
int phone_timeout(long long now);
void phone_tick(long long now);
void phone_draw(void);                       /* the call screen, whichever it is now */
void phone_draw_history(void);
void phone_history_key(int code);
void phone_history_touch(int x, int y);
void phone_history_soft(int b);   /* soft key b, 0 to 3 */
void phone_soft(int b);           /* the same on a call screen or the dialler (small screens) */
void phone_open_history(void);
void phone_draw_overlay(void);               /* the volume bar, over any of the shell's screens */
void phone_account_text(char *out, size_t n, uint32_t *colour);
int phone_missed(void);
const char *phone_state(const char *key);    /* NULL if the key is not the phone's */
/* from the two clients */
void phone_sip_link(int up);
void phone_sip_event(const char *type, const char *json, const char *end);
void phone_sip_response(const char *token, int ok, const char *data);
void phone_audio_link(int up);
void phone_audio_state(void);
/* from phone.c back to the shell */
void shell_phone_screen(int want);           /* the phone wants the screen, or is done with it */
void shell_phone_changed(void);              /* something a phone screen or the idle screen shows has changed */
void shell_show_history(int on);

void phone_reload_account(void);             /* a SIP settings file has changed */
void phone_reload_files(void);               /* settings changed by another program: accounts, volumes, do not disturb */
void phone_reload_history(void);             /* the history file changed by another program */
void phone_prefill(const char *number, int acct);   /* the dialler with a number in it; acct 0 to 5, or -1 for the usual one */
void phone_dial_from(int acct);              /* the dialler, set to call from that account */
void phone_toggle_dnd(void);
int phone_dnd(void);
int phone_forwarding(void);                  /* an account that is on forwards every call */
int phone_messages(void);                    /* new voice messages waiting, over all accounts */
int phone_accounts(void);                    /* how many accounts are switched on */
int phone_account_default(void);             /* the default account, from 0 */
int phone_account_line(int k, char *out, size_t n, uint32_t *colour, int *is_default);

/* ---------- the key test (keytest.c): every key's place in the matrix, on the screen ---------- */

#define KEYTEST_OPEN_MS 5000    /* a key held this long from the idle screen opens it (not a phone key: SK_VOLDOWN and up) */
#define KEYTEST_LEAVE_MS 3000   /* and any key held this long leaves it */
void keytest_open(void);
void keytest_event(int scan, int key, int value);
void keytest_draw(void);
int keytest_timeout(long long now);
int keytest_tick(long long now, int *scan, int *key);   /* 1: time to leave, and the key that is still held */
const char *keytest_state(const char *key);
void shell_show_keytest(void);               /* from Settings, About */
int phone_stand_down(void);                  /* a dialler goes away, whatever is typed in it, if the handset is down; 1 if the phone is idle */

/* ---------- contacts (contacts.c): a list of names and numbers ---------- */

void contacts_init(void);
int contacts_reload(void);                   /* the file changed by another program; 0: busy, ask again */
const char *contacts_name(const char *number);   /* NULL if the number is not in the list */
void contacts_open(const char *find);        /* show the list */
void contacts_open_add(const char *number, const char *name);   /* straight to adding this number */
void contacts_draw(void);
void contacts_key(int code, int down);
void contacts_soft(int b);   /* soft key b, 0 to 3 */
void contacts_touch(int state, int x, int y);
int contacts_timeout(long long now);
void contacts_tick(long long now);
const char *contacts_state(const char *key);
void shell_show_contacts(int on);
int phone_volume(int which);                 /* T48AC_VOL_* */
void phone_set_volume(int which, int level); /* stores it and tells the audio service; plays nothing */

/* ---------- the settings store (config.c): key=value files under opt.config ---------- */

int cfg_get(const char *file, const char *key, char *out, size_t n);       /* 1 if the key is there */
const char *cfg_str(const char *file, const char *key, const char *def);   /* good until four calls later */
int cfg_set(const char *file, const char *key, const char *value);         /* NULL removes; 0, or -1 with errno */
char *cfg_load_text(const char *file);                                     /* the whole file, malloc'd, or NULL */
int cfg_save_text(const char *file, const char *text);                     /* the whole file, atomically */
int cfg_secret(const char *key);                                           /* a password: never shown */
/* an account's custom ringtone (issue #77): 16 kHz mono s16le samples, kept
 * under the shell's state folder, by the account's file name (sip, sip2) */
void ringtone_path(const char *acct_file, char *out, size_t n);
/* root certificates added for SIP over TLS (issue #78, certs.h): certs under opt.config, as t48-sip reads them */
void certs_dir(char *out, size_t n);
int ringtone_code(const char *name);
int forward_to_self(const char *target, const char *user, const char *domain);   /* 1: a forwarding target that is the account itself */                                       /* "chime" -> T48AC_RINGTONE_CHIME; standard if unknown */
int sip_host_ok(const char *h, int scheme);   /* host, host:port, [IPv6]:port or empty; scheme: a sip: or sips: in front allowed */
#define T48_ACCOUNTS 16                       /* sip, sip2 ... sip16, as the stock firmware's 16 lines (issue #77) */
#define T48_REG_DEFAULT 600                   /* register_seconds when not set, as t48-sip has always had it */
int sip_regint_ok(const char *v);             /* register_seconds: empty, 0, or 60 to 86400 */
#define T48_CODECS 4
extern const char *const sip_codec_keys[T48_CODECS], *const sip_codec_names[T48_CODECS];
void sip_codecs(const char *stored, int order[T48_CODECS], int on[T48_CODECS]);   /* an account's codecs= */
void sip_codecs_str(const int order[T48_CODECS], const int on[T48_CODECS], char *out, size_t n);
int config_main(int argc, char **argv);                                    /* the t48-config command */

/* ---------- the app manager (pkg.c): the t48-app command ---------- */

#define T48_DEFAULT_FEED "https://m0lte.github.io/yealink-t48s-apps/index.json"
int pkg_main(int argc, char **argv);
const char *pkg_default_feed(void);

/* ---------- settings and app manager screens (settings.c) ---------- */

void settings_init(void);                    /* at start: the time zone */
void settings_open(int app_manager);         /* the settings list, or straight to the app manager */
void settings_close(void);
void settings_draw(void);
void settings_key(int code, int down);
void settings_touch(int state, int x, int y);
void settings_soft(int b);                   /* soft key b, 0 to 3 (small screens) */
int settings_timeout(long long now);
void settings_tick(long long now);
int settings_job_fd(void);                   /* the app manager's child process, -1 if none */
void settings_job_readable(void);
int settings_reaped(pid_t pid, int status);  /* 1 if that was ours */
const char *settings_state(const char *key);
int settings_type(const char *text);         /* the simulator's "type": into the open keyboard; -1 if none */
const char *settings_tz(void);               /* the TZ string for the zone in the store */
void settings_reload(void);                  /* the store changed under the screens: time zone, and the page showing */
void settings_open_page(const char *name);   /* "update" or "apps": that page, from a notice on the home screen */
void settings_update_watch(long long now);   /* once a second: what the daily check found */
const char *settings_update_notice(void);    /* "Update available" for the home screen, or "" */
int settings_app_updates(void);              /* installed apps the feed has newer */
/* from settings.c back to the shell */
void shell_theme_changed(void);              /* draw everything again */
void shell_settings_changed(void);           /* redraw, if the settings are showing */
void shell_settings_closed(int to_launcher);
void shell_settings_open_app(const char *id);

/* baresip's control socket (sipc.c) */
void sipc_init(const char *socket_path);
int sipc_fd(void);
int sipc_connected(void);
int sipc_timeout(long long now);
void sipc_tick(long long now);
void sipc_readable(void);
void sipc_cmd(const char *command, const char *params);
/* the same, for one account ("sip:user@domain"): the call goes out from it */
void sipc_cmd_from(const char *account, const char *command, const char *params);

/* the audio service's control socket (audioc.c) */
void audioc_init(const char *socket_path);
int audioc_fd(void);
int audioc_connected(void);
int audioc_timeout(long long now);
void audioc_tick(long long now);
void audioc_readable(void);
const struct t48ac_state *audioc_state(void);   /* NULL until the service has said */
void audioc_route(int route);
void audioc_volume(int which, int level);
void audioc_mute(int on);
void audioc_tone(int tone);
void audioc_ring(int on, int ringtone, const char *custom);   /* T48AC_RINGTONE_*; custom: the custom sound's file */
void audioc_key(int ch);
void audioc_app(int what, int ring_fd);       /* T48AC_APP_*; ring_fd: the app's sound ring to hand over, or -1 */

/* ---------- the backlight (backlight.c): bright, dimmed, off ---------- */

#define BL_LEVELS 10
void backlight_init(void);
void backlight_reload(void);                 /* the screen settings file changed */
int backlight_present(void);                 /* is there a backlight device at all */
int backlight_level(void);                   /* the brightness when bright, 1 to BL_LEVELS */
void backlight_set_level(int level);
void backlight_wake(const char *why);        /* light the screen: a call has arrived */
int backlight_key(int code, int down);       /* 1: the key only woke the screen and goes no further */
int backlight_touch(int state);              /* the same for a touch */
int backlight_timeout(long long now);
void backlight_tick(long long now);
const char *backlight_state(const char *key);

/* ---------- what other programs change (watch.c): settings files, contacts, history, apps, web pairing ---------- */

int watch_timeout(long long now);
void watch_tick(long long now);
void watch_draw_overlay(void);               /* a browser's pairing code, over the shell's own screens */
int watch_key(int code, int down);           /* 1: the key was for the pairing box */
int watch_touch(int state, int x, int y);
const char *watch_web_url(void);             /* "" until the web page is running */
int watch_web_paired(void);                  /* browsers paired with it */
const char *watch_state(const char *key);

/* ---------- lamps (leds.c) ---------- */

enum led { LAMP_MESSAGE, LAMP_SPEAKER, LAMP_HEADSET, LAMP_MUTE, LAMP_COUNT };
enum led_mode { LAMP_OFF, LAMP_ON, LAMP_BLINK, LAMP_SLOW };
void leds_set(enum led which, enum led_mode mode);   /* what the phone wants shown */
const char *leds_state(const char *name);    /* the phone's: "off", "on", "blink", "slow", or NULL for no such lamp */
/* The app in front may borrow lamps (docs/app-protocol.md). Masks and levels are the protocol's: bit T48_LAMP_*. */
void leds_app_set(unsigned mask, const uint8_t *level);
void leds_app_pattern(unsigned mask, int loop, const struct t48_lamp_key *key, int n);
void leds_app_release(unsigned mask);        /* back to what the phone wants */
const char *leds_shown(const char *name);    /* what is lit: the phone's word, or the app's level "0" to "255" */
int leds_timeout(long long now);
void leds_tick(long long now);

/* ---------- installed apps ---------- */

#define MAX_APPS 64
#define MAX_SETTINGS 16

struct manifest {
    char id[64], name[64], version[24];
    char dir[256];       /* the app's folder */
    char exec[128];      /* program to run, relative to dir */
    char perms[96];      /* permissions, space separated */
    int api;
    int any_screen;      /* "screen": "any": it lays itself out for whatever size HELLO says */
    char description[160];
    char category[16];   /* as the manifest gives it, "" without one: manifest_category() reads it */
    char *settings[MAX_SETTINGS];  /* "key=default" */
    char *setting_label[MAX_SETTINGS];
    char setting_type[MAX_SETTINGS][12];   /* text, number, bool, password */
    int nsettings;
    t48_surface icon;    /* px is NULL if the app has none we can read */
};
extern struct manifest apps[MAX_APPS];
extern int napps;
void manifests_scan(const char *dirs);
int manifest_id_ok(const char *id);
int manifest_may(const struct manifest *m, const char *perm);   /* a permission it asks for, such as "audio-out" */
/* read one app folder's manifest: 0, or -1 with the reason in err */
int manifest_read(const char *dir, struct manifest *m, char *err, size_t errn);
void manifest_free(struct manifest *m);
int manifest_version_cmp(const char *a, const char *b);   /* <0, 0, >0 as a is older, the same, newer */
/* the fixed set of app categories (issue 30), in the order the store shows them */
#define MANIFEST_CATEGORIES 5
extern const char *const manifest_categories[MANIFEST_CATEGORIES];
int manifest_category(const char *name);   /* its place in that list; 0 (General) for "" or a name not in it */
/* what the feed offers, as t48-app last read it: a file in the state folder (docs/apps.md) */
#define T48_OFFERS "app-offers"
/* the feeds (issue 81): the official one first, then up to seven added in the feed
 * settings file; key is the setting that holds it, "" for the official one */
#define T48_FEEDS 8
#define T48_FEED_APPS 256                          /* apps from all the feeds together */
#define T48_LIST_MAX (T48_FEED_APPS + MAX_APPS)    /* the most t48-app list gives: the Apps screen and page take all */
struct t48_feed { char url[256]; char key[8]; };
const char *feeds_official(void);
int feeds_url_ok(const char *url);                   /* https:// (file:// for the tests), one word, 200 at most */
int feeds_list(struct t48_feed out[T48_FEEDS]);      /* how many */
void feeds_names(const struct t48_feed *all, int n, char names[][64]);   /* each listed feed's name, all different */
void feeds_name(const struct t48_feed *all, int n, const char *url, char *out, size_t on);   /* Official, or the host */
int feeds_add(const char *url);                      /* 0, or -1 with errno EINVAL, EEXIST or ENOSPC */
int feeds_remove(const char *url);                   /* 0, or -1 with errno ENOENT */
/* the feed each installed app came from: a file in the state folder, "id url" a line */
#define T48_SOURCES "app-sources"
int app_source(const char *state, const char *id, char *url, size_t n);   /* 1 and "official", "file" or a URL; or 0 */
int manifest_builtin(const struct manifest *m);   /* part of the system image, not in the first of opt.apps */
int app_follows(const char *state, const struct manifest *m, const struct t48_feed *all, int n, int out[T48_FEEDS]);   /* feeds it updates from */
int manifest_updates(const char *state, const struct t48_feed *feeds, int nfeeds);   /* installed apps their feeds have newer */
extern int manifest_scans;

/* ---------- the one running app ---------- */

enum app_state { APP_NONE, APP_STARTING, APP_RUNNING, APP_PAUSING, APP_STOPPED, APP_QUITTING };

int app_launch(const struct manifest *m);  /* 0, or -1 with a reason in app_error() */
const char *app_error(void);
enum app_state app_state(void);
const char *app_id(void);                  /* "" if none */
int app_folder_changed(void);              /* the running app was removed or replaced */
void app_pause(void);
void app_resume(void);
void app_quit(void);
void app_kill_now(void);
void app_key(int code, int down);
void app_touch(int state, int x, int y);
void app_theme(void);                      /* tell it the look, which has changed */
int app_fd(void);                          /* -1 if none */
int app_sound(void);                       /* the app in front has sound: the volume keys are its */
void app_sound_link(void);                 /* the audio service is there (again): hand it the app's ring */
int app_flip(void);                        /* the flip buffer the app last asked for, -1: it draws by DAMAGE */
void app_flip_show(void);                  /* put that buffer on the screen (again) */
void app_flipped(int n, unsigned frame, long long time_us);   /* from the screen: buffer n is showing */
void app_readable(void);
int app_timeout(long long now);
void app_tick(long long now);
void app_reaped(pid_t pid, int status);
const uint32_t *app_pixels(void);
int app_w(void);                           /* the size of its picture: the screen's, or APP_W x APP_H */
int app_h(void);

/* from app.c back to the shell */
void shell_app_damage(int x, int y, int w, int h);
int shell_app_flip(void);                  /* the app has flipped: 1 if it is in front and the buffer should be shown */
void shell_screen_flush(const uint32_t *src, int w, int h);   /* a whole picture w x h to the screen: a flip, on a screen that copies */
/* the app has gone; why is NULL if that was expected, otherwise words for the owner */
void shell_app_gone(const char *name, const char *why);

#endif
