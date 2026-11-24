// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-meter: copy standard input to standard output and say how far it has got.
 *
 *   nandread -o /dev/mtd10 | t48-meter 3>progress | sha256sum
 *
 * Every megabyte, and once at the end, the number of bytes passed so far goes
 * out on descriptor 3 as a line. t48-install turns those into the progress the
 * Install page shows (docs/installer.md). The bytes themselves are not looked
 * at. A read or write error ends it with status 1, and the reader downstream
 * then sees a short file. */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#define EVERY (1024 * 1024)

static void say(unsigned long long n)
{
    char b[32];
    int k = snprintf(b, sizeof b, "%llu\n", n);
    if (write(3, b, (size_t)k) < 0) {}   /* nobody listening is not an error */
}

int main(void)
{
    static char buf[65536];
    unsigned long long done = 0, said = 0;
    for (;;) {
        ssize_t r = read(0, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("t48-meter: read");
            return 1;
        }
        if (r == 0) break;
        for (ssize_t at = 0; at < r;) {
            ssize_t w = write(1, buf + at, (size_t)(r - at));
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("t48-meter: write");
                return 1;
            }
            at += w;
        }
        done += (unsigned long long)r;
        if (done - said >= EVERY) { say(done); said = done; }
    }
    say(done);
    return 0;
}
