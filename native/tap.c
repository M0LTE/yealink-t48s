/* tap X Y: inject a touchscreen tap at screen pixel X,Y (for testing without a finger).
 * Writes events into /dev/input/event2, which Linux passes on as if from the hardware.
 * Raw values come from inverting the tslib calibration in /etc/pointercal. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct ev { unsigned sec, usec; unsigned short type, code; int value; };

static void put(int fd, int type, int code, int value)
{
    struct ev e = {0, 0, type, code, value};
    write(fd, &e, sizeof e);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: tap X Y\n"); return 2; }
    double x = atof(argv[1]), y = atof(argv[2]);
    double a = 14798, b = 13, c = -4134128, d = 53, e = 10207, f = -5441552, s = 65536;
    FILE *pc = fopen("/etc/pointercal", "r");
    if (pc) { fscanf(pc, "%lf %lf %lf %lf %lf %lf %lf", &a, &b, &c, &d, &e, &f, &s); fclose(pc); }
    /* solve x*s = a*X + b*Y + c and y*s = d*X + e*Y + f for X, Y */
    double det = a * e - b * d;
    int X = ((x * s - c) * e - b * (y * s - f)) / det;
    int Y = (a * (y * s - f) - d * (x * s - c)) / det;
    int fd = open("/dev/input/event2", O_WRONLY);
    if (fd < 0) { perror("event2"); return 1; }
    put(fd, 3, 0, X); put(fd, 3, 1, Y); put(fd, 3, 24, 200); put(fd, 1, 330, 1); put(fd, 0, 0, 0);
    usleep(80000);
    put(fd, 3, 24, 0); put(fd, 1, 330, 0); put(fd, 0, 0, 0);
    printf("tapped %g,%g (raw %d,%d)\n", x, y, X, Y);
    return 0;
}
