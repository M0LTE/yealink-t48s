// SPDX-License-Identifier: AGPL-3.0-or-later
/* sertest DEV: set DEV to 115200 8N1 raw, send a marker, then print what arrives for 10 s. */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/ttyAMA0";
    int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror(dev); return 1; }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    char m[64];
    int n = snprintf(m, sizeof m, "\r\nMARKER-FROM-PHONE-%s\r\n", dev);
    write(fd, m, n);
    tcdrain(fd);
    printf("sent marker on %s; listening 30 s\n", dev);
    fflush(stdout);
    char buf[256];
    int total = 0;
    for (int i = 0; i < 300; i++) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) {
            int r = read(fd, buf, sizeof buf - 1);
            if (r > 0) { buf[r] = 0; printf("received: %s\n", buf); total += r; fflush(stdout); }
        }
    }
    printf("received %d bytes in total\n", total);
    return 0;
}
