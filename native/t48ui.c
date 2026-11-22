/*
 * t48ui: our own screen for the Yealink T48S, alongside Yealink's phone UI.
 *
 * Yealink's dskPhone keeps running with fbswap.so loaded. When our screen is
 * shown we tell fbswap to move dskPhone's drawing to a hidden buffer, grab the
 * touchscreen and keys so dskPhone does not see them, and draw on /dev/fb0.
 * Hiding reverses all three.
 *
 * Control: HTTP on port 8123: /show, /hide, /toggle, /status. Yealink's Action
 * URLs (incoming call, off hook...) can call http://127.0.0.1:8123/hide so the
 * phone always gets its screen back for calls.
 *
 * Page: GB7RDG status from LinBPQ's web server (links, sessions, neighbours).
 *
 * Build: static with musl, see Makefile.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "gen/fonts.h"

#define W 800
#define H 480
#define FB_BYTES (W * H * 4)
#define FBSWAP_SIG 44
#define CTL_PORT 8123
#define EVIOCGRAB 0x40044590

static const char *node_host = "10.45.0.121";
static int node_port = 8008;
static const char *node_name = "GB7RDG";

static unsigned *fb;    /* the real screen */
static unsigned *back;  /* we draw here, then copy to fb */
static int shown;

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

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* ---------- drawing ---------- */

#define RGB(r, g, b) (((r) << 16) | ((g) << 8) | (b))
#define C_BG RGB(14, 20, 30)
#define C_PANEL RGB(26, 36, 52)
#define C_LINE RGB(44, 58, 80)
#define C_TEXT RGB(225, 232, 240)
#define C_DIM RGB(130, 145, 165)
#define C_ACCENT RGB(255, 190, 60)
#define C_GOOD RGB(110, 210, 130)
#define C_BAD RGB(235, 100, 90)
#define C_BTN RGB(48, 96, 160)

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

static unsigned blend(unsigned d, unsigned s, unsigned a)
{
    unsigned r = (((s >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    unsigned g = (((s >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    unsigned b = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
    return (r << 16) | (g << 8) | b;
}

/* draws text with its top at y; returns the x after the last glyph */
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

/* text clipped to a maximum width, with ".." if cut */
static void text_fit(int x, int y, int maxw, const struct font *f, unsigned c, const char *s)
{
    char buf[132];
    size_t n = snprintf(buf, sizeof buf - 3, "%s", s);
    if (n > sizeof buf - 4) n = sizeof buf - 4;
    if (text_width(f, buf) > maxw) {
        while (n > 0) {
            buf[--n] = 0;
            strcpy(buf + n, "..");
            if (text_width(f, buf) <= maxw) break;
        }
    }
    text(x, y, f, c, buf);
}

static void present(void)
{
    if (shown) memcpy(fb, back, FB_BYTES);
}

/* ---------- HTTP fetch and BPQ table parsing ---------- */

static const char *find_ci(const char *s, const char *end, const char *needle)
{
    size_t k = strlen(needle);
    for (; s + k <= end; s++)
        if (!strncasecmp(s, needle, k)) return s;
    return 0;
}

static char page_buf[3][65536];

static int http_get(const char *path, char *out, int max)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    fcntl(s, F_SETFL, O_NONBLOCK);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(node_port)};
    inet_pton(AF_INET, node_host, &a.sin_addr);
    connect(s, (struct sockaddr *)&a, sizeof a);
    struct pollfd p = {s, POLLOUT, 0};
    if (poll(&p, 1, 3000) <= 0) { close(s); return -1; }
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { close(s); return -1; }
    char req[256];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, node_host);
    if (write(s, req, n) != n) { close(s); return -1; }
    int len = 0;
    long deadline = now_ms() + 5000;
    while (len < max - 1) {
        p.events = POLLIN;
        int left = deadline - now_ms();
        if (left <= 0 || poll(&p, 1, left) <= 0) break;
        int r = read(s, out + len, max - 1 - len);
        if (r <= 0) break;
        len += r;
        out[len] = 0;
        /* LinBPQ keeps the connection open, so stop once the page is complete */
        char *body = strstr(out, "\r\n\r\n");
        if (body) {
            char *cl = find_ci(out, body, "Content-Length:") ? (char *)find_ci(out, body, "Content-Length:") : 0;
            if (cl && out + len - (body + 4) >= atol(cl + 15)) break;
            if (find_ci(body, out + len, "</html>")) break;
        }
    }
    close(s);
    out[len] = 0;
    return strstr(out, "</html>") || strstr(out, "</HTML>") || len > 0 ? len : -1;
}

#define MAXROWS 24
#define MAXCOLS 16
struct table { int rows, cols[MAXROWS]; char cell[MAXROWS][MAXCOLS][40]; };

/* copy cell text without tags or entities, trimmed */
static void cell_text(const char *s, const char *e, char *out, int max)
{
    int n = 0, intag = 0;
    for (; s < e && n < max - 1; s++) {
        if (*s == '<') { intag = 1; continue; }
        if (*s == '>' && intag) { intag = 0; continue; }
        if (intag) continue;
        if (*s == '&') {
            const char *semi = memchr(s, ';', e - s);
            if (semi && semi - s < 8) { if (n && out[n - 1] != ' ') out[n++] = ' '; s = semi; continue; }
        }
        char c = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
        if (c == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
        out[n++] = c;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}


/* parse the table that follows `marker`; header rows (th cells) are skipped */
static void parse_table(const char *html, const char *marker, struct table *t)
{
    t->rows = 0;
    const char *end = html + strlen(html);
    const char *s = marker ? find_ci(html, end, marker) : html;
    if (!s) return;
    s = find_ci(s, end, "<table");
    if (!s) return;
    const char *te = find_ci(s, end, "</table");
    if (!te) te = end;
    while (t->rows < MAXROWS) {
        const char *tr = find_ci(s, te, "<tr");
        if (!tr) break;
        const char *trend = find_ci(tr + 3, te, "<tr");
        if (!trend) trend = te;
        int c = 0;
        const char *p = tr;
        while (c < MAXCOLS) {
            const char *td = find_ci(p, trend, "<td");
            if (!td) break;
            const char *gt = memchr(td, '>', trend - td);
            if (!gt) break;
            const char *tdend = find_ci(gt, trend, "</td");
            const char *next = find_ci(gt, trend, "<td");
            if (!tdend || (next && next < tdend)) tdend = next ? next : trend;
            cell_text(gt + 1, tdend, t->cell[t->rows][c], sizeof t->cell[0][0]);
            c++;
            p = tdend;
        }
        if (c > 0) t->cols[t->rows++] = c;
        s = trend;
    }
}

static struct table links, users, routes;
static char status_line[128] = "not fetched yet";
static int fetch_ok;
static long last_fetch;

static void fetch_all(void)
{
    int ok = http_get("/Node/Links.html", page_buf[0], sizeof page_buf[0]) > 0 &&
             http_get("/Node/Users.html", page_buf[1], sizeof page_buf[1]) > 0 &&
             http_get("/Node/Routes.html", page_buf[2], sizeof page_buf[2]) > 0;
    last_fetch = now_ms();
    time_t t = time(0);
    struct tm tm;
    gmtime_r(&t, &tm);
    if (!ok) {
        fetch_ok = 0;
        snprintf(status_line, sizeof status_line, "can't reach %s:%d (%02d:%02d UTC)", node_host, node_port, tm.tm_hour, tm.tm_min);
        return;
    }
    fetch_ok = 1;
    parse_table(page_buf[0], "Links", &links);
    parse_table(page_buf[1], "Sessions", &users);
    parse_table(page_buf[2], "Routes", &routes);
    snprintf(status_line, sizeof status_line, "updated %02d:%02d:%02d UTC", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ---------- the page ---------- */

struct button { int x, y, w, h; const char *label; int id; };
enum { BTN_PHONE = 1, BTN_REFRESH };
static struct button buttons[] = {
    {16, 424, 150, 48, "Phone", BTN_PHONE},
    {178, 424, 150, 48, "Refresh", BTN_REFRESH},
};

static void draw_button(const struct button *b)
{
    fill(b->x, b->y, b->w, b->h, C_BTN);
    fill(b->x, b->y, b->w, 2, RGB(90, 140, 210));
    int tw = text_width(&f_bold, b->label);
    text(b->x + (b->w - tw) / 2, b->y + (b->h - f_bold.height) / 2, &f_bold, C_TEXT, b->label);
}

static int section(int y, const char *title, int count)
{
    char t[64];
    snprintf(t, sizeof t, "%s (%d)", title, count);
    text(16, y, &f_bold, C_ACCENT, t);
    fill(16, y + 28, W - 32, 1, C_LINE);
    return y + 34;
}

static void draw_page(void)
{
    fill(0, 0, W, H, C_BG);
    fill(0, 0, W, 56, C_PANEL);
    int x = text(16, 8, &f_big, C_TEXT, node_name);
    text(x + 14, 20, &f_small, C_DIM, "packet node");
    time_t t = time(0);
    struct tm tm;
    gmtime_r(&t, &tm);
    char clock[32];
    snprintf(clock, sizeof clock, "%02d:%02d UTC", tm.tm_hour, tm.tm_min);
    text(W - 16 - text_width(&f_bold, clock), 16, &f_bold, C_TEXT, clock);

    int y = 64;
    if (!fetch_ok) {
        text(16, y, &f_bold, C_BAD, status_line);
        y += 34;
    }
    const int row = 24, bottom = 408;
    /* rows each section may use: what it needs, sharing what is left fairly */
    int need[3] = {links.rows ? links.rows : 1, users.rows ? users.rows : 1, routes.rows ? routes.rows : 1};
    int avail = (bottom - y - 3 * 38) / row, give[3] = {0, 0, 0}, left = avail;
    for (int pass = 0; pass < 3 && left > 0; pass++)
        for (int k = 0; k < 3 && left > 0; k++)
            if (give[k] < need[k]) { int add = need[k] - give[k]; int fair = left / (3 - k > 0 ? 3 - k : 1); if (fair < 1) fair = 1; if (add > fair) add = fair; give[k] += add; left -= add; }
    char l[200];

    /* Links: Far Call, Our Call, Port, ax.25 state, Link Type, ax.25 Version */
    y = section(y, "Links", links.rows);
    if (!links.rows) { text(24, y, &f_small, C_DIM, "no AX.25 links open"); y += row; }
    for (int r = 0; r < links.rows; r++, y += row) {
        if (r == give[0] - 1 && links.rows > give[0]) { snprintf(l, sizeof l, "+%d more", links.rows - r); text(24, y, &f_small, C_DIM, l); y += row; break; }
        const char *ours = links.cell[r][1];
        int x = text(24, y, &f_mono, C_GOOD, links.cell[r][0]);
        if (strcasecmp(ours, node_name)) { snprintf(l, sizeof l, " to %s", ours); text(x, y, &f_mono, C_DIM, l); }
        snprintf(l, sizeof l, "port %-3s %-8s %s", links.cell[r][2], links.cell[r][3], links.cell[r][4]);
        text_fit(330, y, W - 346, &f_mono, C_TEXT, l);
    }
    y += 6;

    /* Sessions: the row as BPQ gives it */
    y = section(y, "Sessions", users.rows);
    if (!users.rows) { text(24, y, &f_small, C_DIM, "nobody connected"); y += row; }
    for (int r = 0; r < users.rows; r++, y += row) {
        if (r == give[1] - 1 && users.rows > give[1]) { snprintf(l, sizeof l, "+%d more", users.rows - r); text(24, y, &f_small, C_DIM, l); y += row; break; }
        l[0] = 0;
        for (int c = 0; c < users.cols[r]; c++) {
            if (!users.cell[r][c][0]) continue;
            if (l[0]) strncat(l, "  <->  ", sizeof l - strlen(l) - 1);
            strncat(l, users.cell[r][c], sizeof l - strlen(l) - 1);
        }
        text_fit(24, y, W - 48, &f_mono, C_TEXT, l);
    }
    y += 6;

    /* Neighbours: Port Call Quality NodeCount FrameCount Retries Percent Maxframe Frack LastHeard ... */
    y = section(y, "Neighbours", routes.rows);
    for (int r = 0; r < routes.rows; r++, y += row) {
        if (r == give[2] - 1 && routes.rows > give[2]) { snprintf(l, sizeof l, "+%d more", routes.rows - r); text(24, y, &f_small, C_DIM, l); break; }
        const char *port = routes.cell[r][0];
        int linked = 0;
        for (int k = 0; k < links.rows; k++)
            if (!strcasecmp(links.cell[k][0], routes.cell[r][1])) linked = 1;
        if (port[0] == '>') port++;
        text(24, y, &f_mono, linked ? C_GOOD : C_TEXT, routes.cell[r][1]);
        snprintf(l, sizeof l, "port %-3s q %-4s heard %-6s retries %s", port, routes.cell[r][2],
                 routes.cols[r] > 9 ? routes.cell[r][9] : "", routes.cols[r] > 6 ? routes.cell[r][6] : "");
        text_fit(330, y, W - 346, &f_mono, C_TEXT, l);
    }

    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; i++) draw_button(&buttons[i]);
    text(W - 16 - text_width(&f_small, status_line), 438, &f_small, fetch_ok ? C_DIM : C_BAD, status_line);
    present();
}

/* ---------- switching the screen and input ---------- */

static int in_fd[3] = {-1, -1, -1}; /* keypad, expansion, touch */

static void open_inputs(void)
{
    /* the evdev nodes for the keypad (event0) and expansion port (event1) are not in /dev */
    mknod("/tmp/t48ui-event0", S_IFCHR | 0600, makedev(13, 64));
    mknod("/tmp/t48ui-event1", S_IFCHR | 0600, makedev(13, 65));
    in_fd[0] = open("/tmp/t48ui-event0", O_RDWR | O_NONBLOCK);
    in_fd[1] = open("/tmp/t48ui-event1", O_RDWR | O_NONBLOCK);
    in_fd[2] = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    logf_("inputs: keypad %d, expansion %d, touch %d", in_fd[0], in_fd[1], in_fd[2]);
}

static void grab_inputs(int on)
{
    for (int i = 0; i < 3; i++)
        if (in_fd[i] >= 0 && ioctl(in_fd[i], EVIOCGRAB, on) < 0)
            logf_("grab %d on input %d: %s", on, i, strerror(errno));
}

/* ---------- backlight (via the keypad driver) ---------- */

#define BL_ON 80
#define BL_DIM 10
#define DIM_AFTER_MS (10 * 60 * 1000L)
static int bl_saved = -1, dimmed;
static long last_activity;

static int bl_get(void)
{
    char b[32] = {0};
    int fd = open("/proc/keypad/lcd_bklight", O_RDONLY);
    if (fd < 0) return -1;
    read(fd, b, sizeof b - 1);
    close(fd);
    char *eq = strchr(b, '=');
    return eq ? atoi(eq + 1) : -1;
}

static void bl_set(int level)
{
    char b[16];
    int n = snprintf(b, sizeof b, "%d\n", level);
    int fd = open("/proc/keypad/lcd_bklight", O_WRONLY);
    if (fd < 0) return;
    write(fd, b, n);
    close(fd);
}

/* while shown: keep the screen lit, dim after a while without touches */
static void bl_tick(void)
{
    int want = now_ms() - last_activity > DIM_AFTER_MS ? BL_DIM : BL_ON;
    dimmed = want == BL_DIM;
    if (bl_get() != want) bl_set(want);
}

static int dsk_pid(void)
{
    char b[16] = {0};
    int fd = open("/tmp/fbswap.pid", O_RDONLY);
    if (fd < 0) return 0;
    read(fd, b, sizeof b - 1);
    close(fd);
    int p = atoi(b);
    return p > 0 && kill(p, 0) == 0 ? p : 0;
}

static int swapped_pid;

static int tell_fbswap(char mode)
{
    int fd = open("/tmp/fbswap.mode", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    write(fd, &mode, 1);
    close(fd);
    int p = dsk_pid();
    if (!p) return -1;
    kill(p, FBSWAP_SIG);
    swapped_pid = mode == 'o' ? p : 0;
    return 0;
}

static void show(void)
{
    if (shown) return;
    if (tell_fbswap('o') < 0) {
        logf_("show refused: Yealink UI with fbswap.so not found");
        return;
    }
    usleep(100000); /* let dskPhone handle the signal before we draw */
    grab_inputs(1);
    shown = 1;
    bl_saved = bl_get();
    last_activity = now_ms();
    bl_tick();
    logf_("shown");
    draw_page(); /* with what we have; the main loop fetches fresh data straight after */
    if (now_ms() - last_fetch > 5000) last_fetch = 0;
}

static void hide(void)
{
    if (!shown) return;
    grab_inputs(0);
    shown = 0;
    if (bl_saved > 0) bl_set(bl_saved);
    tell_fbswap('y');
    logf_("hidden");
}

/* ---------- touch ---------- */

static long cal[7] = {14798, 13, -4134128, 53, 10207, -5441552, 65536}; /* overwritten from /etc/pointercal */

static void load_cal(void)
{
    FILE *f = fopen("/etc/pointercal", "r");
    if (!f) return;
    long c[7];
    if (fscanf(f, "%ld %ld %ld %ld %ld %ld %ld", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6]) == 7 && c[6])
        memcpy(cal, c, sizeof cal);
    fclose(f);
}

static int raw_x, raw_y, touching;

static void on_tap(int x, int y)
{
    logf_("tap %d,%d", x, y);
    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; i++) {
        struct button *b = &buttons[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            if (b->id == BTN_PHONE) hide();
            if (b->id == BTN_REFRESH) { fetch_all(); draw_page(); }
            return;
        }
    }
}

/* the kernel's 32-bit layout; musl's struct timeval is 64-bit, so spell it out */
struct input_event { unsigned sec, usec; unsigned short type, code; int value; };

static void read_input(int which)
{
    struct input_event ev[32];
    int n = read(in_fd[which], ev, sizeof ev);
    for (int i = 0; i < n / (int)sizeof ev[0]; i++) {
        struct input_event *e = &ev[i];
        if (which < 2 && e->type == 1) {
            /* logged so keys can be mapped later */
            logf_("key %s code %d value %d", which ? "exp" : "keypad", e->code, e->value);
            if (shown) {
                /* Physical keys (and very likely the hook switch) belong to the phone:
                 * hand the screen back and pass this key on, so Yealink sees it. */
                struct input_event k = *e, syn = {0, 0, 0, 0, 0};
                hide();
                write(in_fd[which], &k, sizeof k);
                write(in_fd[which], &syn, sizeof syn);
                logf_("key passed to Yealink");
            }
            continue;
        }
        if (which != 2 || !shown) continue;
        if (e->type == 3 && e->code == 0) raw_x = e->value;
        if (e->type == 3 && e->code == 1) raw_y = e->value;
        if (e->type == 1 && e->code == 330) {
            if (e->value) touching = 1;
            else if (touching) {
                touching = 0;
                long idle = now_ms() - last_activity;
                last_activity = now_ms();
                if (dimmed || idle > DIM_AFTER_MS) { bl_tick(); continue; } /* this touch only wakes the screen */
                int x = (cal[0] * raw_x + cal[1] * raw_y + cal[2]) / cal[6];
                int y = (cal[3] * raw_x + cal[4] * raw_y + cal[5]) / cal[6];
                on_tap(x, y);
            }
        }
    }
}

/* ---------- control server ---------- */

static void handle_client(int c)
{
    char req[512];
    struct pollfd p = {c, POLLIN, 0};
    int n = 0;
    if (poll(&p, 1, 1000) > 0) n = read(c, req, sizeof req - 1);
    if (n < 0) n = 0;
    req[n] = 0;
    char *eol = strpbrk(req, "\r\n");
    logf_("control: %.*s", eol ? (int)(eol - req) : n, req);
    const char *reply = "unknown\n";
    if (!strncmp(req, "GET /show", 9)) { show(); reply = shown ? "shown\n" : "refused\n"; }
    else if (!strncmp(req, "GET /hide", 9)) { hide(); reply = "hidden\n"; }
    else if (!strncmp(req, "GET /toggle", 11)) { if (shown) hide(); else show(); reply = shown ? "shown\n" : "hidden\n"; }
    else if (!strncmp(req, "GET /status", 11)) reply = shown ? "shown\n" : "hidden\n";
    char out[256];
    int len = snprintf(out, sizeof out, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n%s",
                       strlen(reply), reply);
    write(c, out, len);
    close(c);
}

int main(int argc, char **argv)
{
    if (argc > 1) node_host = argv[1];
    if (argc > 2) node_port = atoi(argv[2]);
    if (argc > 3) node_name = argv[3];
    signal(SIGPIPE, SIG_IGN);

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("/dev/fb0"); return 1; }
    fb = mmap(0, FB_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    back = malloc(FB_BYTES);
    if (!back) return 1;

    load_cal();
    open_inputs();

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CTL_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) { perror("control port"); return 1; }
    logf_("t48ui up; node %s:%d; control on port %d", node_host, node_port, CTL_PORT);

    long last_draw = 0;
    for (;;) {
        struct pollfd p[4];
        int n = 0;
        p[n++] = (struct pollfd){ls, POLLIN, 0};
        for (int i = 0; i < 3; i++) if (in_fd[i] >= 0) p[n++] = (struct pollfd){in_fd[i], POLLIN, 0};
        poll(p, n, 1000);
        if (p[0].revents & POLLIN) {
            int c = accept(ls, 0, 0);
            if (c >= 0) handle_client(c);
        }
        for (int i = 1, k = 0; i < n; i++) {
            while (k < 3 && in_fd[k] < 0) k++;
            if (p[i].revents & POLLIN) read_input(k);
            k++;
        }
        if (!shown) continue;
        /* dskPhone restarted while we were shown: its new instance draws on the real screen */
        int dp = dsk_pid();
        if (dp && dp != swapped_pid) { logf_("Yealink UI restarted; swapping again"); tell_fbswap('o'); usleep(100000); }
        bl_tick();
        if (now_ms() - last_fetch > 15000) { fetch_all(); draw_page(); last_draw = now_ms(); }
        else if (now_ms() - last_draw > 10000) { draw_page(); last_draw = now_ms(); }
    }
}
