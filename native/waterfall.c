// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * waterfall: live receive waterfall of a pdn-soundmodem station on the T48S
 * screen (issue #13).
 *
 *   waterfall [-n NAME] ws://host:port/            draw on /dev/fb0
 *   waterfall [-n NAME] --shot F.ppm --seconds N ws://host:port/
 *                                                  host test: render N seconds into memory, write a PPM
 *
 * Data: the station page WebSocket. Listen only: after the HTTP upgrade we send
 * nothing but what WebSocket itself requires (pong to a ping, close in reply
 * to a close) and {"type":"pong"}, the answer to the page's own keep-alive
 * {"type":"ping"} (a viewer that stays silent for 60 s is dropped). The page
 * protocol also has commands that key the transmitter; none is ever sent.
 * Binary messages are spectrum lines (0x01 heard, 0x03 our own transmission:
 * type, line index u32 LE, then lineLength bytes); text messages are JSON and
 * only "config", "frame", "history" and "tx" are read.
 *
 * Drawing: spectrum lines are folded to 600 columns, a few lines per row, and
 * each finished row is coloured once (a 256-entry table) into a ring of rows.
 * A screen update copies the ring into the back buffer, draws the frame tags
 * on top and copies that region to the framebuffer: no per-pixel maths on
 * the scrolling part. Header and recent-frames panel are redrawn only when
 * they change.
 *
 * Protocol and design follow M0LTE/esp32-knob-pages (AGPL-3.0): ws_parse.c is
 * taken from it unchanged and wf_fold.c is adapted from it.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "gen/wf_fonts.h"
#include "wf_fold.h"
#include "ws_parse.h"

#define W 800
#define H 480

/* layout */
#define HDR_H 36
#define SCALE_Y 36
#define SPEC_TOP 58
#define SPEC_BOT 122
#define BAR_Y 125
#define WF_TOP 131
#define WF_VIS (H - WF_TOP)
#define PANEL_X 602
#define PANEL_W (W - PANEL_X)

#define RGB(r, g, b) ((unsigned)(((r) << 16) | ((g) << 8) | (b)))
#define C_BG RGB(8, 10, 16)
#define C_HDR RGB(18, 24, 36)
#define C_PANEL RGB(14, 19, 29)
#define C_LINE RGB(40, 52, 72)
#define C_GRID RGB(24, 31, 44)
#define C_TEXT RGB(226, 232, 240)
#define C_DIM RGB(128, 142, 162)
#define C_FAINT RGB(78, 90, 110)
#define C_AMBER RGB(255, 176, 60)
#define C_GOOD RGB(110, 214, 130)
#define C_BAD RGB(240, 96, 86)
#define C_COOL RGB(120, 196, 255)
#define C_TAG RGB(240, 244, 250)

static const unsigned band_rgb[] = {0x4CC2FF, 0xB68CFF, 0xE8C27A, 0x6FE3C1, 0xFF8FA3, 0x9AD0FF, 0xD7E36F, 0xFFB86B};

/* ---------- screen ---------- */

static unsigned *back;            /* we draw here */
static unsigned *fb;              /* the real screen, NULL on the host */
static unsigned fb_stride = W;    /* in pixels */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    time_t t = time(0);
    struct tm tm;
    gmtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* copy a region of the back buffer to the screen */
static void flush(int x, int y, int w, int h)
{
    if (!fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) memcpy(fb + (y + j) * fb_stride + x, back + (y + j) * W + x, (size_t)w * 4);
}

static void fill(int x, int y, int w, int h, unsigned c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        unsigned *p = back + (y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

/* a dark see-through box: three quarters background, one quarter what is under it */
static void shade(int x, int y, int w, int h)
{
    const unsigned bg = ((C_BG >> 2) & 0x3F3F3F) * 3;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        unsigned *p = back + (y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = ((p[i] >> 2) & 0x3F3F3F) + bg;
    }
}

static inline unsigned blend(unsigned d, unsigned s, unsigned a)
{
    a += a >> 7;  /* 0..256 */
    unsigned rb = ((s & 0xFF00FF) * a + (d & 0xFF00FF) * (256 - a)) >> 8;
    unsigned g = ((s & 0x00FF00) * a + (d & 0x00FF00) * (256 - a)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

/* text with its top at y, clipped to the screen; returns the x after the last glyph */
static int text(int x, int y, const struct font *f, unsigned c, const char *s)
{
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const struct glyph *g = &f->g[ch - 32];
        const unsigned char *a = f->a + g->off;
        for (int j = 0; j < g->h; j++) {
            int py = y + g->yoff + j;
            if (py < 0 || py >= H) continue;
            for (int i = 0; i < g->w; i++) {
                int px = x + g->xoff + i;
                unsigned al = a[j * g->w + i];
                if (px < 0 || px >= W || !al) continue;
                unsigned *d = back + py * W + px;
                *d = al == 255 ? c : blend(*d, c, al);
            }
        }
        x += g->adv;
    }
    return x;
}

static int text_width(const struct font *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        w += f->g[(ch < 32 || ch > 126 ? '?' : ch) - 32].adv;
    }
    return w;
}

/* text cut to a maximum width, with ".." if cut */
static void text_fit(int x, int y, int maxw, const struct font *f, unsigned c, const char *s)
{
    char buf[96];
    size_t n = (size_t)snprintf(buf, sizeof buf - 3, "%s", s);
    if (n > sizeof buf - 4) n = sizeof buf - 4;
    if (text_width(f, buf) > maxw) {
        while (n > 0) {
            buf[--n] = 0;
            strcpy(buf + n, "..");
            if (text_width(f, buf) <= maxw) break;
        }
        if (text_width(f, buf) > maxw) return;
    }
    text(x, y, f, c, buf);
}

static void text_centre(int cx, int y, const struct font *f, unsigned c, const char *s)
{
    text(cx - text_width(f, s) / 2, y, f, c, s);
}

static unsigned dim(unsigned c, int num, int den)  /* scale a colour towards black */
{
    return RGB(((c >> 16) & 255) * num / den, ((c >> 8) & 255) * num / den, (c & 255) * num / den);
}

/* ---------- a small JSON reader (enough for the station page's messages) ---------- */

static const char *jws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

static const char *jskip_str(const char *p, const char *e)  /* p at the opening quote */
{
    for (p++; p < e; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return e;
}

static const char *jskip(const char *p, const char *e)  /* past one value */
{
    p = jws(p, e);
    if (p >= e) return e;
    if (*p == '"') return jskip_str(p, e);
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (p < e) {
            if (*p == '"') { p = jskip_str(p, e); continue; }
            if (*p == '{' || *p == '[') d++;
            else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
            p++;
        }
        return e;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* the value of `key` in the object at o, or NULL */
static const char *jget(const char *o, const char *e, const char *key)
{
    if (!o) return NULL;
    o = jws(o, e);
    if (o >= e || *o != '{') return NULL;
    o++;
    size_t kl = strlen(key);
    for (;;) {
        o = jws(o, e);
        if (o >= e || *o != '"') return NULL;
        const char *ks = o + 1, *ke = jskip_str(o, e);
        int match = (size_t)(ke - 1 - ks) == kl && !memcmp(ks, key, kl);
        o = jws(ke, e);
        if (o >= e || *o != ':') return NULL;
        o = jws(o + 1, e);
        if (match) return o;
        o = jws(jskip(o, e), e);
        if (o < e && *o == ',') o++;
    }
}

static int jnum(const char *v, const char *e, double *out)
{
    if (!v || v >= e || !(*v == '-' || (*v >= '0' && *v <= '9'))) return 0;
    *out = strtod(v, NULL);  /* the message buffer is NUL terminated */
    return 1;
}

static int jstr(const char *v, const char *e, char *out, size_t n)
{
    if (!v || v >= e || *v != '"' || !n) return 0;
    size_t k = 0;
    for (v++; v < e && *v != '"'; v++) {
        char c = *v;
        if (c == '\\' && v + 1 < e) {
            v++;
            if (*v == 'u' && v + 4 < e) {
                unsigned u = (unsigned)strtoul((char[5]){v[1], v[2], v[3], v[4], 0}, NULL, 16);
                c = u >= 32 && u < 127 ? (char)u : '?';
                v += 4;
            } else {
                c = *v == 'n' ? ' ' : *v == 't' ? ' ' : *v;
            }
        }
        if (k + 1 < n) out[k++] = c;
    }
    out[k] = 0;
    return 1;
}

static int jtrue(const char *v, const char *e) { return v && e - v >= 4 && !memcmp(v, "true", 4); }

/* array iteration: first element after '[', then the next one; NULL at the end */
static const char *jfirst(const char *a, const char *e)
{
    if (!a || a >= e || *a != '[') return NULL;
    a = jws(a + 1, e);
    return a < e && *a != ']' ? a : NULL;
}

static const char *jnext(const char *v, const char *e)
{
    v = jws(jskip(v, e), e);
    if (v >= e || *v != ',') return NULL;
    v = jws(v + 1, e);
    return v < e ? v : NULL;
}

/* ---------- station state ---------- */

#define MAX_BANDS 8
#define NTAGS 64
#define NO_SNR (-100000)

typedef struct {
    float lo_hz, hi_hz;
    int sub;
    char name[24];
} band_t;

typedef struct {
    uint32_t row;      /* first row of the burst */
    uint16_t rows;
    uint8_t placed;    /* row/rows are valid (live frames only) */
    uint8_t tx;
    int sub;
    int snr10;         /* SNR in tenths of a dB, NO_SNR if none */
    char from[12], to[12], at[9];
} tag_t;

static struct {
    char name[32];          /* -n, else the page's title, else the host */
    int name_from_arg;
    char url_host[64];
    int url_port;
    /* config */
    int have_config;
    int line_len, lpr;
    float bin_hz, lines_per_s, lo_hz, hi_hz;
    double dial_hz;
    int lsb;
    char version[24];
    band_t bands[MAX_BANDS];
    int n_bands;
    /* connection */
    int up;                 /* socket open and upgraded */
    int attempts;
    long retry_at;          /* when the next try starts, while down */
    long last_line_ms;
    int tx_keyed;
} st;

static wf_map_t map;
static wf_rows_t rows;
static uint8_t cols[WF_W];
static unsigned ring[WF_HIST][WF_W];   /* coloured rows; row r lives at r % WF_HIST */
static uint32_t row_seq;               /* next row number */
static uint32_t row_first;             /* oldest row with data */
static int have_rows;
static int floor_q8, top_q8, have_levels;  /* auto levels, byte scale x 256 */
static unsigned ramp_rx[256], ramp_tx[256], lut[256];
static int spec16[WF_W];               /* smoothed spectrum trace, byte scale x 16 */
static unsigned spec_bg[WF_W];         /* spectrum background per column (band tint) */
static tag_t tags[NTAGS];
static uint32_t tag_seq;
static int dirty_wf, dirty_hdr, dirty_panel, dirty_static;

static int hz_x(float hz)
{
    if (st.hi_hz <= st.lo_hz) return 0;
    return (int)((hz - st.lo_hz) * WF_W / (st.hi_hz - st.lo_hz));
}

static const band_t *band_of(int sub)
{
    for (int i = 0; i < st.n_bands; i++)
        if (st.bands[i].sub == sub) return &st.bands[i];
    return NULL;
}

static unsigned band_colour(int sub) { return band_rgb[(unsigned)sub % 8]; }

static void build_ramp(unsigned *out, const unsigned *stops, int n)
{
    for (int i = 0; i < 256; i++) {
        int x = i * (n - 1) * 256 / 255, a = x >> 8, f = x & 255;
        if (a >= n - 1) { a = n - 2; f = 256; }
        unsigned c0 = stops[a], c1 = stops[a + 1];
        int r = (((c0 >> 16) & 255) * (256 - f) + ((c1 >> 16) & 255) * f) >> 8;
        int g = (((c0 >> 8) & 255) * (256 - f) + ((c1 >> 8) & 255) * f) >> 8;
        int b = ((c0 & 255) * (256 - f) + (c1 & 255) * f) >> 8;
        out[i] = RGB(r, g, b);
    }
}

/* the colour table for the current levels */
static void build_lut(const unsigned *ramp)
{
    int span = top_q8 - floor_q8;
    if (span < 20 * 256) span = 20 * 256;
    for (int b = 0; b < 256; b++) {
        int v = (b * 256 - floor_q8) * 255 / span;
        lut[b] = ramp[v < 0 ? 0 : v > 255 ? 255 : v];
    }
}

/* a finished row: levels, colour, into the ring */
static void on_row(void *ctx, uint32_t row, const uint8_t c[WF_W], int tx)
{
    (void)ctx;
    int hist[256] = {0};
    for (int i = 0; i < WF_W; i++) hist[c[i]]++;
    int acc = 0, q = -1, mx = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (q < 0 && acc >= WF_W / 4) q = i;
        if (hist[i]) mx = i;
    }
    if (!tx) {
        /* floor a little under the quarter point, top over the peak; eased */
        int f = (q + 1) * 256, t = (mx + 6) * 256;
        if (t < f + 56 * 256) t = f + 56 * 256;
        if (!have_levels) { floor_q8 = f; top_q8 = t; have_levels = 1; }
        floor_q8 += (f - floor_q8) / 20;
        top_q8 += (t > top_q8 ? (t - top_q8) / 5 : (t - top_q8) / 100);
    }
    if (tx) {
        /* our own transmission: levels of its own, so it shows at any strength */
        int keep_f = floor_q8, keep_t = top_q8;
        floor_q8 = q * 256;
        top_q8 = mx * 256 > floor_q8 + 30 * 256 ? mx * 256 : floor_q8 + 30 * 256;
        build_lut(ramp_tx);
        floor_q8 = keep_f;
        top_q8 = keep_t;
    } else {
        build_lut(ramp_rx);
    }
    unsigned *px = ring[row % WF_HIST];
    for (int i = 0; i < WF_W; i++) px[i] = lut[c[i]];
    if (!have_rows) { row_first = row; have_rows = 1; }
    row_seq = row + 1;
    if (row_seq - row_first > WF_HIST) row_first = row_seq - WF_HIST;
    dirty_wf = 1;
}

static void on_row_cb(void *ctx, uint32_t row, const uint8_t c[WF_W], bool tx) { on_row(ctx, row, c, tx); }

/* a dim marker row where the feed was lost */
static void gap_row(void)
{
    if (!have_rows) return;
    unsigned *px = ring[row_seq % WF_HIST];
    for (int i = 0; i < WF_W; i++) px[i] = (i / 4) % 2 ? C_LINE : C_BG;
    row_seq++;
    if (row_seq - row_first > WF_HIST) row_first = row_seq - WF_HIST;
    dirty_wf = 1;
}

/* ---------- messages ---------- */

static void on_config(const char *p, const char *e)
{
    double d;
    float sr = jnum(jget(p, e, "sampleRate"), e, &d) ? (float)d : 48000;
    st.bin_hz = jnum(jget(p, e, "binWidthHz"), e, &d) ? (float)d : 0;
    st.line_len = jnum(jget(p, e, "lineLength"), e, &d) ? (int)d : 0;
    st.lines_per_s = jnum(jget(p, e, "linesPerSecond"), e, &d) ? (float)d : 0;
    st.dial_hz = jnum(jget(p, e, "dialHz"), e, &d) ? d : 0;
    char sb[8] = "";
    jstr(jget(p, e, "sideband"), e, sb, sizeof sb);
    st.lsb = !strcmp(sb, "lsb");
    st.version[0] = 0;
    jstr(jget(jget(p, e, "daemonVersion"), e, "version"), e, st.version, sizeof st.version);
    char title[32];
    if (!st.name_from_arg && jstr(jget(p, e, "title"), e, title, sizeof title) && title[0])
        snprintf(st.name, sizeof st.name, "%s", title);

    float lo[MAX_BANDS], hi[MAX_BANDS];
    st.n_bands = 0;
    for (const char *m = jfirst(jget(p, e, "modems"), e); m && st.n_bands < MAX_BANDS; m = jnext(m, e)) {
        double l, h, s;
        if (!jnum(jget(m, e, "lowHz"), e, &l) || !jnum(jget(m, e, "highHz"), e, &h)) continue;
        band_t *b = &st.bands[st.n_bands];
        memset(b, 0, sizeof *b);
        b->lo_hz = lo[st.n_bands] = (float)l;
        b->hi_hz = hi[st.n_bands] = (float)h;
        b->sub = jnum(jget(m, e, "sub"), e, &s) ? (int)s : 0;
        if (!jstr(jget(m, e, "modeName"), e, b->name, sizeof b->name)) jstr(jget(m, e, "mode"), e, b->name, sizeof b->name);
        st.n_bands++;
    }
    wf_span(lo, hi, st.n_bands, sr, &st.lo_hz, &st.hi_hz);
    st.have_config = st.bin_hz > 0 && st.line_len > 0 && st.lines_per_s > 0;
    if (st.have_config) {
        wf_map_build(&map, st.lo_hz, st.hi_hz, st.bin_hz, st.line_len);
        /* about 8 rows a second whatever the line rate */
        st.lpr = (int)(st.lines_per_s / 8 + 0.5f);
        if (st.lpr < 1) st.lpr = 1;
        if (st.lpr > 16) st.lpr = 16;
    }
    wf_rows_reset(&rows, row_seq, st.lpr);
    for (int x = 0; x < WF_W; x++) spec_bg[x] = C_BG;
    for (int i = 0; i < st.n_bands; i++) {
        unsigned tint = blend(C_BG, band_colour(st.bands[i].sub), 22);
        for (int x = hz_x(st.bands[i].lo_hz); x < hz_x(st.bands[i].hi_hz); x++)
            if (x >= 0 && x < WF_W) spec_bg[x] = tint;
    }
    dirty_static = dirty_hdr = dirty_panel = dirty_wf = 1;
    logf_("config: %d bands, %.0f to %.0f Hz, %.0f lines/s, %d lines per row, %d bins of %.3f Hz", st.n_bands,
          st.lo_hz, st.hi_hz, st.lines_per_s, st.lpr, st.line_len, st.bin_hz);
}

static void read_frame(const char *f, const char *e, tag_t *t)
{
    double d;
    memset(t, 0, sizeof *t);
    t->sub = jnum(jget(f, e, "sub"), e, &d) ? (int)d : -1;
    t->tx = jtrue(jget(f, e, "tx"), e);
    t->snr10 = jnum(jget(f, e, "snrDb"), e, &d) ? (int)(d * 10 + (d < 0 ? -0.5 : 0.5)) : NO_SNR;
    jstr(jget(f, e, "from"), e, t->from, sizeof t->from);
    jstr(jget(f, e, "to"), e, t->to, sizeof t->to);
    char at[40] = "";
    if (jstr(jget(f, e, "at"), e, at, sizeof at) && strlen(at) >= 19) memcpy(t->at, at + 11, 8);
    if (!t->at[0]) {
        time_t now = time(0);
        struct tm tm;
        gmtime_r(&now, &tm);
        snprintf(t->at, sizeof t->at, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

static void on_frame(const char *p, const char *e)
{
    tag_t t;
    read_frame(p, e, &t);
    double d;
    if (jnum(jget(p, e, "line"), e, &d)) {
        double b;
        uint32_t burst = jnum(jget(p, e, "burstLines"), e, &b) && b > 0 ? (uint32_t)b : 0;
        t.placed = wf_tag_rows(&rows, (uint32_t)d, burst, &t.row, &t.rows);
    }
    tags[tag_seq % NTAGS] = t;
    tag_seq++;
    dirty_panel = dirty_wf = 1;
}

/* the station's recent log: fills the panel before anything new is heard */
static void on_history(const char *p, const char *e)
{
    if (tag_seq) return;  /* only on a fresh start */
    const char *fr = jget(p, e, "frames");
    int n = 0;
    for (const char *f = jfirst(fr, e); f; f = jnext(f, e)) n++;
    int skip = n > NTAGS ? n - NTAGS : 0, i = 0;
    for (const char *f = jfirst(fr, e); f; f = jnext(f, e), i++) {
        if (i < skip) continue;
        read_frame(f, e, &tags[tag_seq % NTAGS]);
        tag_seq++;
    }
    dirty_panel = 1;
}

static int sock = -1, closing;

static void send_frame(uint8_t op, const void *p, size_t len)
{
    uint8_t frame[6 + 125], mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)rand();
    size_t n = ws_control_frame(op, p, len, mask, frame);
    send(sock, frame, n, MSG_NOSIGNAL);
}

static void on_msg(void *ctx, uint8_t op, const uint8_t *p, size_t len)
{
    (void)ctx;
    if (op == WS_OP_BINARY) {
        if (st.have_config && len == (size_t)st.line_len + 5 && (p[0] == 0x01 || p[0] == 0x03)) {
            uint32_t line = p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24;
            wf_fold_line(&map, p + 5, cols);
            for (int i = 0; i < WF_W; i++) {  /* the trace: quick up, slow down */
                int v = cols[i] * 16, s = spec16[i];
                spec16[i] = s + (v > s ? (v - s) / 4 : (v - s) / 16);
            }
            wf_rows_feed(&rows, line, cols, p[0] == 0x03, on_row_cb, NULL);
            st.last_line_ms = now_ms();
        }
    } else if (op == WS_OP_TEXT) {
        const char *s = (const char *)p, *e = s + len;
        size_t head = len < 48 ? len : 48;
        ((uint8_t *)p)[len] = 0;  /* the buffer has a spare byte for this */
        if (memmem(s, head, "\"type\":\"ping\"", 13)) {
            static const char pong[] = "{\"type\":\"pong\"}";
            send_frame(WS_OP_TEXT, pong, sizeof pong - 1);
        } else if (memmem(s, head, "\"type\":\"config\"", 15)) {
            on_config(s, e);
        } else if (memmem(s, head, "\"type\":\"frame\"", 14)) {
            on_frame(s, e);
        } else if (memmem(s, head, "\"type\":\"history\"", 16)) {
            on_history(s, e);
        } else if (memmem(s, head, "\"type\":\"tx\"", 11)) {
            int k = jtrue(jget(s, e, "keyed"), e);
            if (k != st.tx_keyed) { st.tx_keyed = k; dirty_hdr = 1; }
        }
    } else if (op == WS_OP_PING) {
        send_frame(WS_OP_PONG, p, len);
    } else if (op == WS_OP_CLOSE) {
        if (!closing) send_frame(WS_OP_CLOSE, p, len >= 2 ? 2 : 0);
        closing = 1;
    }
}

/* ---------- drawing the parts ---------- */

static void draw_header(void)
{
    char b[96];
    fill(0, 0, W, HDR_H, C_HDR);
    fill(0, HDR_H - 1, W, 1, C_LINE);
    int x = text(12, 8, &f_head, C_TEXT, st.name);
    if (st.dial_hz > 0) {
        snprintf(b, sizeof b, "%.3f kHz %s", st.dial_hz / 1000.0, st.lsb ? "LSB" : "USB");
        x = text(x + 16, 11, &f_mono, C_AMBER, b);
    }
    if (st.version[0]) {
        snprintf(b, sizeof b, "pdn-soundmodem %s", st.version);
        text(x + 16, 12, &f_small, C_FAINT, b);
    }
    /* right: TX pill, link state, UTC clock */
    time_t now = time(0);
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(b, sizeof b, "%02d:%02d:%02d UTC", tm.tm_hour, tm.tm_min, tm.tm_sec);
    int rx = W - 12 - text_width(&f_mono, b);
    text(rx, 11, &f_mono, C_DIM, b);
    long t = now_ms();
    const char *s;
    unsigned c;
    if (st.up && t - st.last_line_ms < 3000) s = "LIVE", c = C_GOOD;
    else if (st.up) s = st.have_config ? "NO SPECTRUM" : "WAITING", c = C_AMBER;
    else if (st.retry_at > t) {
        snprintf(b, sizeof b, "RETRY IN %ld s", (st.retry_at - t + 999) / 1000);
        s = b, c = C_BAD;
    } else s = "CONNECTING", c = C_AMBER;
    int w = text_width(&f_bold, s);
    rx -= w + 34;
    fill(rx - 4, 0, w + 30, HDR_H - 1, C_HDR);
    for (int j = -4; j <= 4; j++)
        for (int i = -4; i <= 4; i++)
            if (i * i + j * j <= 17) back[(18 + j) * W + rx + 6 + i] = c;
    text(rx + 16, 9, &f_bold, c, s);
    if (st.tx_keyed) {
        int pw = text_width(&f_bold, "TX") + 16;
        rx -= pw + 12;
        fill(rx, 7, pw, 22, C_BAD);
        text(rx + 8, 9, &f_bold, RGB(255, 255, 255), "TX");
    }
    flush(0, 0, W, HDR_H);
}

/* frequency scale and band bars: change only with the config */
static void draw_static(void)
{
    char b[16];
    fill(0, SCALE_Y, PANEL_X, WF_TOP - SCALE_Y, C_BG);
    fill(PANEL_X - 2, HDR_H, 2, H - HDR_H, C_LINE);
    if (st.hi_hz > st.lo_hz) {
        float span = st.hi_hz - st.lo_hz;
        int step = span > 6000 ? 1000 : span > 2500 ? 500 : 250;
        for (int hz = ((int)st.lo_hz / step + 1) * step; hz < st.hi_hz; hz += step) {
            int x = hz_x((float)hz), major = hz % 1000 == 0;
            fill(x, SPEC_TOP - (major ? 7 : 4), 1, major ? 7 : 4, major ? C_DIM : C_FAINT);
            if (major) {
                snprintf(b, sizeof b, "%d", hz / 1000);
                text_centre(x, SCALE_Y + 3, &f_mono, C_DIM, b);
            }
        }
        text(PANEL_X - 34, SCALE_Y + 4, &f_tiny, C_FAINT, "kHz");
    }
    fill(0, BAR_Y - 2, PANEL_X - 2, 6, C_BG);
    for (int i = 0; i < st.n_bands; i++) {
        int x0 = hz_x(st.bands[i].lo_hz), x1 = hz_x(st.bands[i].hi_hz);
        fill(x0, BAR_Y, x1 - x0, 3, band_colour(st.bands[i].sub));
    }
    fill(0, WF_TOP - 1, PANEL_X - 2, 1, C_LINE);
    flush(0, SCALE_Y, W, WF_TOP - SCALE_Y);
}

static void draw_spectrum(void)
{
    const int h = SPEC_BOT - SPEC_TOP;
    int lo = floor_q8 - 10 * 256, span = top_q8 - lo;  /* the trace shows the noise too */
    if (span < 20 * 256) span = 20 * 256;
    for (int y = SPEC_TOP; y < SPEC_BOT; y++) memcpy(back + y * W, spec_bg, WF_W * 4);
    for (int g = 1; g < 4; g++) {
        unsigned *p = back + (SPEC_TOP + g * h / 4) * W;
        for (int x = 0; x < WF_W; x += 3) p[x] = C_GRID;
    }
    if (have_levels) {
        int prev = -1;
        for (int x = 0; x < WF_W; x++) {
            int v = (spec16[x] * 16 - lo) * h / span;
            v = v < 0 ? 0 : v > h - 1 ? h - 1 : v;
            int y = SPEC_BOT - 1 - v;
            for (int j = y + 1; j < SPEC_BOT; j++) {
                unsigned *d = back + j * W + x;
                *d = blend(*d, C_AMBER, 50);
            }
            int a = prev < 0 ? y : prev < y ? prev : y, z = prev < 0 ? y : prev > y ? prev : y;
            for (int j = a; j <= z; j++) back[j * W + x] = C_AMBER;
            prev = y;
        }
    }
    for (int i = 0; i < st.n_bands; i++) {
        const band_t *bd = &st.bands[i];
        int x0 = hz_x(bd->lo_hz), x1 = hz_x(bd->hi_hz);
        /* the full name, else its first word, else cut */
        /* (a label may run on past its band, up to where the next one starts) */
        int room = WF_W - x0;
        for (int k = 0; k < st.n_bands; k++) {
            int xk = hz_x(st.bands[k].lo_hz);
            if (xk > x0 && xk - x0 < room) room = xk - x0;
        }
        (void)x1;
        char nm[24];
        snprintf(nm, sizeof nm, "%s", bd->name);
        if (text_width(&f_tiny, nm) > room - 6 && strchr(nm, ' ')) *strchr(nm, ' ') = 0;
        text_fit(x0 + 3, SPEC_TOP + 2, room - 6, &f_tiny, band_colour(bd->sub), nm);
    }
    flush(0, SPEC_TOP, WF_W, h);
}

typedef struct { int x, y, w, h; } rect_t;

static int overlaps(const rect_t *a, const rect_t *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void tag_label(const tag_t *t, char *b, size_t n)
{
    if (t->tx) snprintf(b, n, "TX > %s", t->to[0] ? t->to : "?");
    else if (t->snr10 != NO_SNR) snprintf(b, n, "%s %d dB", t->from[0] ? t->from : "?", (t->snr10 + (t->snr10 < 0 ? -5 : 5)) / 10);
    else snprintf(b, n, "%s", t->from[0] ? t->from : "?");
}

static void draw_waterfall(void)
{
    /* rows, newest at the top */
    for (int y = 0; y < WF_VIS; y++) {
        uint32_t r = row_seq - 1 - (uint32_t)y;
        unsigned *d = back + (WF_TOP + y) * W;
        if (have_rows && (uint32_t)y < row_seq - row_first) memcpy(d, ring[r % WF_HIST], WF_W * 4);
        else for (int x = 0; x < WF_W; x++) d[x] = C_BG;
    }
    /* band edges, dotted */
    for (int i = 0; i < st.n_bands; i++) {
        unsigned c = dim(band_colour(st.bands[i].sub), 1, 2);
        int xs[2] = {hz_x(st.bands[i].lo_hz), hz_x(st.bands[i].hi_hz) - 1};
        for (int k = 0; k < 2; k++)
            if (xs[k] >= 0 && xs[k] < WF_W)
                for (int y = WF_TOP; y < H; y += 4) back[y * W + xs[k]] = c;
    }
    /* time marks every 10 s down the left */
    if (st.have_config) {
        float rps = st.lines_per_s / st.lpr;
        char b[12];
        for (int s = 10;; s += 10) {
            int y = WF_TOP + (int)(s * rps);
            if (y > H - 8) break;
            fill(0, y, 5, 1, C_DIM);
            snprintf(b, sizeof b, "-%ds", s);
            int w = text_width(&f_tiny, b);
            shade(6, y - 7, w + 4, 14);
            text(8, y - 7, &f_tiny, C_DIM, b);
        }
    }
    /* frame tags: a bracket on each burst, a label on the newest ones */
    rect_t used[12];
    int nu = 0, labels = 0;
    int n = tag_seq < NTAGS ? (int)tag_seq : NTAGS;
    for (int k = 0; k < n; k++) {
        const tag_t *t = &tags[(tag_seq - 1 - k) % NTAGS];
        if (!t->placed) continue;
        const band_t *bd = band_of(t->sub);
        if (!bd) continue;
        int y_new = WF_TOP + (int)(row_seq - 1 - (t->row + t->rows - 1));
        int y_old = WF_TOP + (int)(row_seq - 1 - t->row);
        if ((int32_t)(row_seq - 1 - (t->row + t->rows - 1)) < 0) y_new = WF_TOP;
        if (y_new >= H) continue;
        if (y_old >= H) y_old = H - 1;
        int x0 = hz_x(bd->lo_hz), x1 = hz_x(bd->hi_hz);
        unsigned c = t->tx ? C_COOL : C_TAG;
        fill(x0 - 4, y_new, 2, y_old - y_new + 1, c);
        fill(x0 - 4, y_new, 6, 2, c);
        if (y_old < H - 1) fill(x0 - 4, y_old - 1, 6, 2, c);
        if (labels >= 10) continue;
        char b[40];
        tag_label(t, b, sizeof b);
        int w = text_width(&f_mono, b) + 10;
        rect_t r = {x0 - 8 - w, y_new, w, 18};
        if (r.x < 0) r.x = x1 + 4;
        if (r.x + w > WF_W) r.x = x0 + 4;
        for (int tries = 0; tries < 6; tries++) {
            int hit = 0;
            for (int u = 0; u < nu; u++)
                if (overlaps(&r, &used[u])) { r.y = used[u].y + used[u].h + 1; hit = 1; }
            if (!hit) break;
        }
        if (r.y + r.h > H) continue;
        shade(r.x, r.y, r.w, r.h);
        fill(r.x, r.y, 2, r.h, t->tx ? C_COOL : band_colour(t->sub));
        text(r.x + 6, r.y + 1, &f_mono, c, b);
        if (nu < 12) used[nu++] = r;
        labels++;
    }
    /* state over the picture */
    long now = now_ms();
    const char *big = NULL;
    char small[96] = "";
    if (!st.up) {
        big = have_rows ? "FEED LOST" : "CONNECTING";
        if (st.retry_at > now)
            snprintf(small, sizeof small, "%s:%d  -  retry %d in %ld s", st.url_host, st.url_port, st.attempts + 1,
                     (st.retry_at - now + 999) / 1000);
        else
            snprintf(small, sizeof small, "%s:%d", st.url_host, st.url_port);
    } else if (!st.have_config) {
        big = "WAITING";
        snprintf(small, sizeof small, "connected, waiting for the station's settings");
    } else if (now - st.last_line_ms > 3000) {
        big = "NO SPECTRUM";
        snprintf(small, sizeof small, "connected, no spectrum lines for %ld s", (now - st.last_line_ms) / 1000);
    }
    if (big) {
        int bw = text_width(&f_big, big), sw = text_width(&f_small, small);
        int w = (bw > sw ? bw : sw) + 48, cy = WF_TOP + WF_VIS / 2;
        shade(WF_W / 2 - w / 2, cy - 40, w, 80);
        fill(WF_W / 2 - w / 2, cy - 40, w, 2, st.up ? C_AMBER : C_BAD);
        text_centre(WF_W / 2, cy - 28, &f_big, st.up || !have_rows ? C_AMBER : C_BAD, big);
        text_centre(WF_W / 2, cy + 12, &f_small, C_DIM, small);
    }
    flush(0, WF_TOP, WF_W, WF_VIS);
}

static void draw_panel(void)
{
    char b[64];
    fill(PANEL_X, HDR_H, PANEL_W, H - HDR_H, C_PANEL);
    text(PANEL_X + 10, HDR_H + 8, &f_tiny, C_FAINT, "RECENT FRAMES");
    int y = HDR_H + 28;
    const int row_h = 46;
    int n = tag_seq < NTAGS ? (int)tag_seq : NTAGS;
    if (!n) text(PANEL_X + 10, y + 4, &f_small, C_FAINT, "none yet");
    for (int k = 0; k < n && y + row_h <= H; k++) {
        const tag_t *t = &tags[(tag_seq - 1 - k) % NTAGS];
        unsigned c = t->tx ? C_COOL : band_colour(t->sub);
        fill(PANEL_X + 8, y + 3, 3, row_h - 10, c);
        if (t->tx) snprintf(b, sizeof b, "TX %s > %s", t->from, t->to);
        else snprintf(b, sizeof b, "%s > %s", t->from[0] ? t->from : "?", t->to[0] ? t->to : "?");
        text_fit(PANEL_X + 18, y + 2, PANEL_W - 24, &f_bold, t->tx ? C_COOL : C_TEXT, b);
        const band_t *bd = band_of(t->sub);
        char snr[12] = "";
        if (t->snr10 != NO_SNR) snprintf(snr, sizeof snr, "  %d dB", (t->snr10 + (t->snr10 < 0 ? -5 : 5)) / 10);
        snprintf(b, sizeof b, "%s  %s%s", t->at, bd ? bd->name : "", snr);
        text_fit(PANEL_X + 18, y + 22, PANEL_W - 24, &f_tiny, C_DIM, b);
        fill(PANEL_X + 8, y + row_h - 4, PANEL_W - 16, 1, C_GRID);
        y += row_h;
    }
    flush(PANEL_X, HDR_H, PANEL_W, H - HDR_H);
}

static void redraw(int force)
{
    if (force || dirty_static) draw_static();
    if (force || dirty_hdr) draw_header();
    if (force || dirty_panel) draw_panel();
    if (force || dirty_wf) {
        draw_spectrum();
        draw_waterfall();
    }
    dirty_static = dirty_hdr = dirty_panel = dirty_wf = 0;
}

/* ---------- network ---------- */

static int parse_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn)
{
    if (strncmp(url, "ws://", 5)) return 0;
    const char *h = url + 5, *slash = strchr(h, '/'), *colon = strchr(h, ':');
    const char *hend = slash ? slash : h + strlen(h);
    if (colon && colon < hend) { *port = atoi(colon + 1); hend = colon; }
    else *port = 80;
    if (hend == h || (size_t)(hend - h) >= hn) return 0;
    memcpy(host, h, (size_t)(hend - h));
    host[hend - h] = 0;
    snprintf(path, pn, "%s", slash ? slash : "/");
    return *port > 0;
}

/* connect and upgrade; leftover bytes after the HTTP head go to rest */
static int connect_ws(const char *host, int port, const char *path, uint8_t *rest, size_t cap, size_t *rest_n)
{
    char ps[12];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { logf_("cannot resolve %s", host); return -1; }
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = {.tv_sec = 5};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r) { logf_("connect %s:%d: %s", host, port, strerror(errno)); close(s); return -1; }
    uint8_t rnd[16];
    char key[25], req[320];
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)rand();
    ws_key(rnd, key);
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     path, host, port, key);
    if (send(s, req, (size_t)n, MSG_NOSIGNAL) != n) { close(s); return -1; }
    size_t got = 0;
    char *end = NULL;
    while (!end && got < cap - 1) {
        ssize_t k = recv(s, rest + got, cap - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
        rest[got] = 0;
        end = strstr((char *)rest, "\r\n\r\n");
    }
    if (!end || strncmp((char *)rest, "HTTP/1.1 101", 12)) {
        logf_("upgrade refused: %.40s", got ? (char *)rest : "(nothing)");
        close(s);
        return -1;
    }
    size_t hl = (size_t)((end + 4) - (char *)rest);
    memmove(rest, rest + hl, got - hl);
    *rest_n = got - hl;
    return s;
}

/* ---------- main ---------- */

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

#define KDSETMODE 0x4B3A
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
struct fb_var_min { unsigned xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, rest[33]; };
struct fb_fix_min {
    char id[16];
    unsigned long smem_start;
    unsigned smem_len, type, type_aux, visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned line_length;
    unsigned long mmio_start;
    unsigned mmio_len, accel;
    unsigned short capabilities, reserved[2];
};

static int open_fb(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror(path); return -1; }
    struct fb_var_min v;
    struct fb_fix_min f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("fb ioctl"); return -1; }
    if (v.xres < W || v.yres < H || v.bits_per_pixel != 32) {
        fprintf(stderr, "%s is %ux%u %u bpp; need at least %dx%d at 32 bpp\n", path, v.xres, v.yres, v.bits_per_pixel, W, H);
        return -1;
    }
    fb_stride = f.line_length / 4;
    size_t len = (size_t)f.line_length * H;
    fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); fb = NULL; return -1; }
    /* keep the text console from drawing over us */
    int tty = open("/dev/tty0", O_RDWR);
    if (tty >= 0) { ioctl(tty, KDSETMODE, 1); close(tty); }
    return 0;
}

static void console_back(void)
{
    int tty = open("/dev/tty0", O_RDWR);
    if (tty >= 0) { ioctl(tty, KDSETMODE, 0); close(tty); }
}

static int write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned p = back[i];
        unsigned char c[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
        fwrite(c, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: waterfall [-n NAME] [--fb /dev/fb0] ws://host:port/\n"
                    "       waterfall [-n NAME] --shot OUT.ppm --seconds N ws://host:port/   (render to a file)\n");
    exit(2);
}

#define MSG_MAX (256 * 1024)

int main(int argc, char **argv)
{
    const char *url = NULL, *shot = NULL, *fbpath = "/dev/fb0";
    int seconds = 20;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            snprintf(st.name, sizeof st.name, "%s", argv[++i]);
            st.name_from_arg = 1;
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fbpath = argv[++i];
        else if (argv[i][0] != '-' && !url) url = argv[i];
        else usage();
    }
    char host[64], path[128];
    int port = 80;
    if (!url || !parse_url(url, host, sizeof host, &port, path, sizeof path)) usage();
    snprintf(st.url_host, sizeof st.url_host, "%s", host);
    st.url_port = port;
    if (!st.name[0]) snprintf(st.name, sizeof st.name, "%s", host);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    srand((unsigned)time(0) ^ (unsigned)getpid());

    back = calloc(W * H, 4);
    uint8_t *msg = malloc(MSG_MAX + 1), *rx = malloc(8192);
    static ws_dec_t dec;
    if (!back || !msg || !rx) return 1;
    if (!shot && open_fb(fbpath)) return 1;

    static const unsigned rxs[] = {0x080A10, 0x0B1030, 0x2A1450, 0x7A2448, 0xD9601E, 0xFF9F1C, 0xFFE8B8};
    static const unsigned txs[] = {0x080A10, 0x06182A, 0x0E3A5C, 0x2A7FB0, 0x9AD8FF};
    for (int x = 0; x < WF_W; x++) spec_bg[x] = C_BG;
    build_ramp(ramp_rx, rxs, 7);
    build_ramp(ramp_tx, txs, 5);
    wf_rows_reset(&rows, 0, 4);

    long start = now_ms(), last_draw = 0, last_sec = 0;
    int backoff = 1;
    redraw(1);
    while (!stop) {
        long t = now_ms();
        if (shot && t - start >= seconds * 1000L) break;
        if (!st.up && t >= st.retry_at) {
            st.retry_at = 0;
            dirty_hdr = dirty_wf = 1;
            redraw(0);
            size_t rest_n = 0;
            sock = connect_ws(host, port, path, rx, 8192, &rest_n);
            if (sock >= 0) {
                logf_("listening to %s:%d%s", host, port, path);
                st.up = 1;
                st.attempts = 0;
                backoff = 1;
                closing = 0;
                st.last_line_ms = now_ms();
                ws_dec_init(&dec, msg, MSG_MAX);
                wf_rows_reset(&rows, row_seq, st.lpr ? st.lpr : 4);
                if (ws_dec_feed(&dec, rx, rest_n, on_msg, NULL)) closing = 1;
            } else {
                st.attempts++;
                st.retry_at = now_ms() + backoff * 1000L;
                logf_("cannot connect, retry in %d s", backoff);
                backoff = backoff < 30 ? backoff * 2 : 30;
            }
            dirty_hdr = dirty_wf = 1;
        }
        if (st.up) {
            struct pollfd pf = {.fd = sock, .events = POLLIN};
            int r = poll(&pf, 1, 40);
            int drop = closing;
            static long last_rx;
            if (r > 0) {
                ssize_t k = recv(sock, rx, 8192, 0);
                if (k > 0) {
                    last_rx = now_ms();
                    if (ws_dec_feed(&dec, rx, (size_t)k, on_msg, NULL)) { logf_("protocol error"); drop = 1; }
                } else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
                    drop = 1;
                }
            }
            if (!last_rx) last_rx = now_ms();
            if (now_ms() - last_rx > 60000) { logf_("silent for 60 s"); drop = 1; }
            if (drop || closing) {
                close(sock);
                sock = -1;
                st.up = 0;
                last_rx = 0;
                st.retry_at = now_ms() + backoff * 1000L;
                logf_("disconnected, retry in %d s", backoff);
                backoff = backoff < 30 ? backoff * 2 : 30;
                gap_row();
                dirty_hdr = dirty_wf = 1;
            }
        } else {
            usleep(100000);
        }
        t = now_ms();
        if (t / 1000 != last_sec) {  /* clock, countdowns, stale states */
            last_sec = t / 1000;
            dirty_hdr = 1;
            if (!st.up || t - st.last_line_ms > 3000) dirty_wf = 1;
        }
        if (dirty_wf && t - last_draw < 33) {
            int keep = dirty_wf;  /* at most about 30 screen updates a second */
            dirty_wf = 0;
            redraw(0);
            dirty_wf = keep;
        } else {
            if (dirty_wf) last_draw = t;
            redraw(0);
        }
    }
    if (sock >= 0) close(sock);
    if (shot) {
        redraw(1);
        if (write_ppm(shot)) return 1;
        logf_("wrote %s", shot);
    } else {
        console_back();
    }
    return 0;
}
