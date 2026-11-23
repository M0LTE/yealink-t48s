// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-shell: the client for baresip's control socket (the t48ctrl module,
 * docs/sip.md). JSON in netstrings: commands out, answers and events in.
 * Keeps trying once a second while baresip is not there. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "shell.h"

static const char *path;
static int fd = -1;
static long long retry_at;
static char rx[8192];
static size_t rxn;

void sipc_init(const char *socket_path) { path = socket_path; }
int sipc_fd(void) { return fd; }
int sipc_connected(void) { return fd >= 0; }

static void drop(const char *why)
{
    if (fd < 0) return;
    logmsg("sip: lost baresip (%s)", why);
    close(fd);
    fd = -1;
    rxn = 0;
    retry_at = t48_now_ms() + 1000;
    phone_sip_link(0);
}

static void try_connect(long long now)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    retry_at = now + 1000;
    if (!path || strlen(path) >= sizeof sa.sun_path) return;
    strcpy(sa.sun_path, path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) { close(s); return; }
    fd = s;
    rxn = 0;
    logmsg("sip: connected to baresip at %s", path);
    phone_sip_link(1);
}

int sipc_timeout(long long now)
{
    if (fd >= 0 || !path) return -1;
    return retry_at > now ? (int)(retry_at - now) : 0;
}

void sipc_tick(long long now)
{
    if (fd < 0 && path && now >= retry_at) try_connect(now);
}

/* Send a command. The token comes back with the answer: we use the command's
 * own name, which is all phone_sip_response() needs to know. */
void sipc_cmd(const char *command, const char *params)
{
    char json[512], msg[540];
    size_t k;

    if (fd < 0) return;
    k = (size_t)snprintf(json, sizeof json, "{\"command\":\"%s\",\"token\":\"%s\"", command, command);
    if (params) {
        k += (size_t)snprintf(json + k, sizeof json - k, ",\"params\":\"");
        for (const char *p = params; *p && k < sizeof json - 8; p++) {
            if (*p == '"' || *p == '\\') json[k++] = '\\';
            if ((unsigned char)*p >= 32) json[k++] = *p;
        }
        json[k++] = '"';
    }
    json[k++] = '}';
    json[k] = 0;
    int n = snprintf(msg, sizeof msg, "%zu:%s,", k, json);
    logmsg("sip: > %s%s%s", command, params ? " " : "", params ? params : "");
    if (send(fd, msg, (size_t)n, MSG_NOSIGNAL) != n) drop("cannot send");
}

static void message(char *json, size_t len)
{
    const char *e = json + len;
    char type[40] = "", tok[40] = "", data[200] = "";

    if (t48_json_true(t48_json_get(json, e, "event"), e)) {
        t48_json_str(t48_json_get(json, e, "type"), e, type, sizeof type);
        phone_sip_event(type, json, e);
    } else if (t48_json_true(t48_json_get(json, e, "response"), e)) {
        t48_json_str(t48_json_get(json, e, "token"), e, tok, sizeof tok);
        t48_json_str(t48_json_get(json, e, "data"), e, data, sizeof data);
        phone_sip_response(tok, t48_json_true(t48_json_get(json, e, "ok"), e), data);
    }
}

void sipc_readable(void)
{
    if (fd < 0) return;
    ssize_t n = recv(fd, rx + rxn, sizeof rx - 1 - rxn, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n <= 0) { drop(n ? strerror(errno) : "closed"); return; }
    rxn += (size_t)n;
    while (fd >= 0) {
        size_t i = 0, len = 0;
        while (i < rxn && rx[i] >= '0' && rx[i] <= '9' && i < 6) len = len * 10 + (size_t)(rx[i++] - '0');
        if (i == rxn) break;
        if (!i || rx[i] != ':' || len > sizeof rx - 16) { drop("bad framing"); return; }
        if (rxn < i + len + 2) break;
        rx[i + 1 + len] = 0;
        message(rx + i + 1, len);
        memmove(rx, rx + i + len + 2, rxn - (i + len + 2));
        rxn -= i + len + 2;
    }
}
