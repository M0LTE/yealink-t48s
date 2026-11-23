// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: shared declarations. See docs/shell.md. */
#ifndef SHELL_H
#define SHELL_H

#include <poll.h>
#include <sys/types.h>
#include "t48.h"

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

void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------- installed apps ---------- */

#define MAX_APPS 64
#define MAX_SETTINGS 16

struct manifest {
    char id[64], name[64], version[24];
    char dir[256];       /* the app's folder */
    char exec[128];      /* program to run, relative to dir */
    char perms[96];      /* permissions, space separated */
    int api;
    char *settings[MAX_SETTINGS];  /* "key=default" */
    int nsettings;
    t48_surface icon;    /* px is NULL if the app has none we can read */
};
extern struct manifest apps[MAX_APPS];
extern int napps;
void manifests_scan(const char *dirs);

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
