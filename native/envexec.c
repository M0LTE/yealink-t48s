// SPDX-License-Identifier: AGPL-3.0-or-later
/* envexec ENVFILE [VAR=VAL ...] -- PROG [ARGS...]
 * Run PROG with the NUL-separated environment in ENVFILE (e.g. a copy of
 * /proc/PID/environ), plus or overriding the given VAR=VAL pairs. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: envexec ENVFILE [VAR=VAL ...] -- PROG [ARGS]\n"); return 2; }
    static char buf[65536];
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 1; }
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    static char *env[512];
    int ne = 0, i = 2;
    for (; i < argc && strcmp(argv[i], "--"); i++) env[ne++] = argv[i];
    if (i >= argc - 1) { fprintf(stderr, "envexec: missing -- PROG\n"); return 2; }
    for (long p = 0; p < n && ne < 510; p += strlen(buf + p) + 1) {
        if (!buf[p]) continue;
        char *eq = strchr(buf + p, '=');
        int dup = 0;
        for (int k = 0; eq && k < ne; k++)
            if (!strncmp(env[k], buf + p, eq - (buf + p) + 1)) dup = 1;
        if (!dup) env[ne++] = buf + p;
    }
    env[ne] = 0;
    execve(argv[i + 1], argv + i + 1, env);
    perror(argv[i + 1]);
    return 127;
}
