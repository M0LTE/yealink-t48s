// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: shared declarations. See docs/shell.md. */
#ifndef SHELL_H
#define SHELL_H

#include <poll.h>
#include <sys/types.h>
#include "t48.h"
#include "t48_audio_ctl.h"

#define SW 800
#define SH 480

/* Phone keys, which never go to apps. App keys use the protocol's T48_KEY_*
 * codes; the X key arrives from a backend as T48_KEY_BACK. */
enum { SK_VOLDOWN = 0x200, SK_VOLUP, SK_HEADSET, SK_MUTE, SK_MESSAGE, SK_HOLD, SK_REDIAL, SK_TRANSFER, SK_SPEAKER };

/* ---------- the screen and input devices: the phone's, or the simulator's ---------- */

struct backend {
    const char *name;
    int (*init)(void);
    /* copy part of a full-screen picture (rows `stride` pixels apart) to the screen */
    void (*flush)(const uint32_t *src, int stride, int x, int y, int w, int h);
    int (*fds)(struct pollfd *p, int max);        /* descriptors to wait on */
    void (*handle)(const struct pollfd *p, int n); /* after poll: read input, call shell_key() etc. */
    int (*timeout)(long long now);                /* ms until tick() is wanted, -1: never */
    void (*tick)(long long now);
    void (*fini)(void);
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
    const char *access;  /* folder where t48-access leaves its status file (docs/access.md) */
    const char *runner;  /* simulator: a program to run apps with, such as qemu-arm for the phone's own builds */
};
extern struct options opt;

/* input, from a backend */
void shell_key(int code, int down);
void shell_touch(int state, int x, int y);
void shell_hook(int off_hook);

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

#define C_BG T48_RGB(8, 10, 16)
#define C_BAR T48_RGB(18, 24, 36)
#define C_PANEL T48_RGB(22, 29, 43)
#define C_KEY T48_RGB(34, 44, 64)
#define C_LINE T48_RGB(40, 52, 72)
#define C_TEXT T48_RGB(226, 232, 240)
#define C_DIM T48_RGB(128, 142, 162)
#define C_FAINT T48_RGB(78, 90, 110)
#define C_ACCENT T48_RGB(255, 176, 60)
#define C_GOOD T48_RGB(110, 214, 130)
#define C_BAD T48_RGB(240, 96, 86)

extern t48_surface ui;                       /* the shell's own picture of the screen */
void ui_status_bar(const char *title);
/* a rounded button; fill is its colour, and the label is dark on bright fills */
void ui_button(int x, int y, int w, int h, const char *label, uint32_t fill);
void ui_network_status(char *out, size_t n, int *up);
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
void phone_prefill(const char *number, int acct);   /* the dialler with a number in it; acct 0 to 5, or -1 for the usual one */
void phone_dial_from(int acct);              /* the dialler, set to call from that account */
void phone_toggle_dnd(void);
int phone_dnd(void);
int phone_messages(void);                    /* new voice messages waiting, over all accounts */
int phone_accounts(void);                    /* how many accounts are switched on */
int phone_account_line(int k, char *out, size_t n, uint32_t *colour, int *is_default);

/* ---------- contacts (contacts.c): a list of names and numbers ---------- */

void contacts_init(void);
const char *contacts_name(const char *number);   /* NULL if the number is not in the list */
void contacts_open(const char *find);        /* show the list */
void contacts_open_add(const char *number, const char *name);   /* straight to adding this number */
void contacts_draw(void);
void contacts_key(int code, int down);
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
int config_main(int argc, char **argv);                                    /* the t48-config command */

/* ---------- the app manager (pkg.c): the t48-app command ---------- */

int pkg_main(int argc, char **argv);
const char *pkg_default_feed(void);

/* ---------- settings and app manager screens (settings.c) ---------- */

void settings_init(void);                    /* at start: the time zone */
void settings_open(int app_manager);         /* the settings list, or straight to the app manager */
void settings_close(void);
void settings_draw(void);
void settings_key(int code, int down);
void settings_touch(int state, int x, int y);
int settings_timeout(long long now);
void settings_tick(long long now);
int settings_job_fd(void);                   /* the app manager's child process, -1 if none */
void settings_job_readable(void);
int settings_reaped(pid_t pid, int status);  /* 1 if that was ours */
const char *settings_state(const char *key);
const char *settings_tz(void);               /* the TZ string for the zone in the store */
/* from settings.c back to the shell */
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
void audioc_ring(int on);
void audioc_key(int ch);

/* ---------- the backlight (backlight.c): bright, dimmed, off ---------- */

#define BL_LEVELS 10
void backlight_init(void);
int backlight_present(void);                 /* is there a backlight device at all */
int backlight_level(void);                   /* the brightness when bright, 1 to BL_LEVELS */
void backlight_set_level(int level);
void backlight_wake(const char *why);        /* light the screen: a call has arrived */
int backlight_key(int code, int down);       /* 1: the key only woke the screen and goes no further */
int backlight_touch(int state);              /* the same for a touch */
int backlight_timeout(long long now);
void backlight_tick(long long now);
const char *backlight_state(const char *key);

/* ---------- lamps (leds.c): a place for the driver of issue #3 ---------- */

enum led { LAMP_MESSAGE, LAMP_SPEAKER, LAMP_HEADSET, LAMP_MUTE, LAMP_COUNT };
enum led_mode { LAMP_OFF, LAMP_ON, LAMP_BLINK, LAMP_SLOW };
void leds_set(enum led which, enum led_mode mode);
const char *leds_state(const char *name);    /* "off", "on", "blink", "slow", or NULL for no such lamp */

/* ---------- installed apps ---------- */

#define MAX_APPS 64
#define MAX_SETTINGS 16

struct manifest {
    char id[64], name[64], version[24];
    char dir[256];       /* the app's folder */
    char exec[128];      /* program to run, relative to dir */
    char perms[96];      /* permissions, space separated */
    int api;
    char description[160];
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
/* read one app folder's manifest: 0, or -1 with the reason in err */
int manifest_read(const char *dir, struct manifest *m, char *err, size_t errn);
void manifest_free(struct manifest *m);

/* ---------- the one running app ---------- */

enum app_state { APP_NONE, APP_STARTING, APP_RUNNING, APP_PAUSING, APP_STOPPED, APP_QUITTING };

int app_launch(const struct manifest *m);  /* 0, or -1 with a reason in app_error() */
const char *app_error(void);
enum app_state app_state(void);
const char *app_id(void);                  /* "" if none */
void app_pause(void);
void app_resume(void);
void app_quit(void);
void app_kill_now(void);
void app_key(int code, int down);
void app_touch(int state, int x, int y);
int app_fd(void);                          /* -1 if none */
void app_readable(void);
int app_timeout(long long now);
void app_tick(long long now);
void app_reaped(pid_t pid, int status);
const uint32_t *app_pixels(void);

/* from app.c back to the shell */
void shell_app_damage(int x, int y, int w, int h);
/* the app has gone; why is NULL if that was expected, otherwise words for the owner */
void shell_app_gone(const char *name, const char *why);

#endif
