// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-web: shared declarations. See docs/web.md. */
#ifndef T48_WEB_H
#define T48_WEB_H

#include <stddef.h>
#include <stdint.h>
#include "shell.h"   /* the settings store (config.c), the manifests, and where things live (opt) */

/* where the web page's own things are, beside the shell's (opt) */
struct web_options {
    const char *listen;   /* ADDRESS:PORT to listen on, for tests; NULL: the interface's own address */
    const char *iface;    /* the LAN interface */
    int port;
    const char *appcmd;   /* t48-app */
    const char *keep;     /* t48-access's folder of things that are kept: ssh/authorized_keys */
    const char *update;   /* t48-update */
    const char *restart;  /* what restarts the phone */
};
extern struct web_options wopt;

#define MAX_FIELDS 96
#define MAX_BODY (512 * 1024)
#define MAX_RINGTONE_BODY (2 * 1024 * 1024)   /* /accounts/ringtone: ten seconds of 44.1 kHz stereo WAV */
#define DRAIN_BODY (16 * 1024 * 1024)         /* a ringtone bigger than that is read and dropped, to say so on the page */

struct session {
    char token[65];       /* the cookie; "" for a free slot */
    char csrf[65];        /* goes in every form */
    char ip[48];
    long long last;       /* when it was last used, ms */
    char flash[400];      /* what the last change did, shown once on the next page */
    int flash_bad;
};

struct req {
    int fd;
    char method[8], path[128], query[256], host[128], origin[200], cookie[600], ctype[200];
    char peer[48];
    char *body;
    size_t blen;
    struct { const char *name, *value; size_t len; } f[MAX_FIELDS];   /* len: of value, which may hold zero bytes (a file) */
    int nf;
    struct session *s;    /* NULL: not paired */
    int too_big;          /* a ringtone over its limit: read, dropped, and said so */
};

/* ---------- wav.c ---------- */

/* a WAV file as a custom ringtone: 16 kHz mono samples (malloc'd), ten
 * seconds at most (*cut says it was longer). 0, or -1 and a reason */
int wav_to_ringtone(const unsigned char *in, size_t len, int16_t **out, size_t *n, int *cut, const char **why);

/* ---------- http.c ---------- */

/* the page being made. %s is escaped for HTML, %r is put in as it is, %d an
 * int, %l a long, %u a string for a URL, %% a percent sign */
void H(const char *fmt, ...);
void raw(const char *s, size_t n);
void page_reset(void);                                     /* start again with nothing: for a download */
extern int log_quiet;                                      /* 1: say nothing, while the shell's own code is at work */
const char *form(const struct req *r, const char *name);   /* "" if the form has no such field, or it holds a zero byte */
const char *form_bytes(const struct req *r, const char *name, size_t *len);   /* a file as it came; NULL if there is none */
const char *query(const struct req *r, const char *name);  /* the same for the address's ?a=b; good until the next call */
void flash(struct req *r, int bad, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void send_page(struct req *r, int status);                 /* what H() has made, as HTML */
void send_file(struct req *r, const char *type, const char *filename);   /* the same, as a download */
void redirect(struct req *r, const char *where);
void page_refresh(int seconds);                            /* this page asks for itself again, for one that shows progress */
void page_begin(struct req *r, const char *title, const char *nav);
void page_end(struct req *r);
void form_begin(struct req *r, const char *action, const char *more);   /* with the CSRF field in it */
int sessions_count(void);
void sessions_forget_others(const struct session *keep);
void web_stop_soon(void);                                  /* after this answer has gone: leave */
const char *web_url(void);

/* run a program and wait up to wait_ms for it; its output (both kinds) in out.
 * The exit status, or -1. With wait_ms 0 it is left running by itself, after
 * delay seconds. */
int run_program(char *const argv[], int wait_ms, char *out, size_t outn, int delay);
void run_apply(const char *what, int wait_ms, int delay);
int save_file(const char *path, const char *text, int mode);   /* atomic; 0 or -1 */
char *load_file(const char *path, size_t max);                 /* malloc'd, or NULL */

/* ---------- pages.c ---------- */

struct route {
    const char *method, *path;
    void (*handler)(struct req *r);
};
extern const struct route routes[];
void page_overview(struct req *r);

const char *web_theme(void);   /* the theme the phone is in, for <html data-theme> */

/* ---------- assets.c ---------- */

extern const char asset_css[], asset_js[];
struct asset_font { const char *name; const unsigned char *data; unsigned len; };   /* served as /fonts/NAME */
extern const struct asset_font asset_fonts[];   /* the last has no name */

#endif
