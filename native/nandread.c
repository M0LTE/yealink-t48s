// SPDX-License-Identifier: AGPL-3.0-or-later
/* nandread [-o|-O] [-r] [-b] /dev/mtdN > FILE
 *
 * Read-only dump of a NAND partition, for comparing what two kernels read (issue #5).
 * Runs under Yealink's 2.6.27 and under our 3.6 alike.
 *   (no option)  page data, as the kernel's error correction returns it
 *   -o           each page's data followed by its 64 spare ("OOB") bytes
 *   -O           the spare bytes only
 *   -r           raw: page data without error correction
 *   -b           no dump; list the blocks the kernel calls bad
 * A summary (pages, bad blocks, the kernel's count of corrected and failed reads)
 * goes to stderr.
 *
 * The device is opened read-only and the only ioctls used are queries and reads, so
 * this program cannot change the flash: the kernel refuses erase and write requests
 * on a file opened this way. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* from the kernel's mtd-abi.h; the same in 2.6.27 and 3.6 */
struct mtd_info_user { unsigned char type; unsigned flags, size, erasesize, writesize, oobsize, ecctype, eccsize; };
struct mtd_oob_buf { unsigned start, length; unsigned char *ptr; };
struct mtd_ecc_stats { unsigned corrected, failed, badblocks, bbtblocks; };
#define MEMGETINFO     _IOR('M', 1, struct mtd_info_user)
#define MEMREADOOB     _IOWR('M', 4, struct mtd_oob_buf)
#define MEMGETBADBLOCK _IOW('M', 11, long long)
#define ECCGETSTATS    _IOR('M', 18, struct mtd_ecc_stats)
#define MTDFILEMODE    _IO('M', 19)
#define MTD_MODE_RAW   3

static void put(const unsigned char *p, unsigned n)
{
    while (n) {
        ssize_t w = write(1, p, n);
        if (w <= 0) { perror("write"); exit(1); }
        p += w; n -= w;
    }
}

int main(int argc, char **argv)
{
    int data = 1, oob = 0, raw = 0, listbad = 0, c;
    while ((c = getopt(argc, argv, "oOrb")) != -1) {
        if (c == 'o') oob = 1;
        else if (c == 'O') { oob = 1; data = 0; }
        else if (c == 'r') raw = 1;
        else if (c == 'b') listbad = 1;
        else argc = 0;
    }
    if (argc - optind != 1) { fprintf(stderr, "usage: nandread [-o|-O] [-r] [-b] /dev/mtdN > FILE\n"); return 2; }
    int fd = open(argv[optind], O_RDONLY);
    if (fd < 0) { perror(argv[optind]); return 1; }
    struct mtd_info_user mi;
    if (ioctl(fd, MEMGETINFO, &mi)) { perror("MEMGETINFO"); return 1; }
    fprintf(stderr, "%s: size 0x%x, block 0x%x, page %u, spare %u, flags 0x%x\n",
            argv[optind], mi.size, mi.erasesize, mi.writesize, mi.oobsize, mi.flags);
    if (mi.writesize == 0 || mi.writesize > 8192 || mi.oobsize > 512) { fprintf(stderr, "not a NAND partition\n"); return 1; }
    struct mtd_ecc_stats s0, s1;
    int stats = ioctl(fd, ECCGETSTATS, &s0) == 0;
    unsigned nbad = 0, pages = 0, rderr = 0;
    for (unsigned b = 0; b < mi.size; b += mi.erasesize) {
        long long off = b;
        if (ioctl(fd, MEMGETBADBLOCK, &off) > 0) {
            nbad++;
            fprintf(stderr, "bad block at 0x%x\n", b);
        }
    }
    if (listbad) { fprintf(stderr, "%u bad blocks\n", nbad); return 0; }
    if (raw && ioctl(fd, MTDFILEMODE, MTD_MODE_RAW)) { perror("MTDFILEMODE raw"); return 1; }
    static unsigned char page[8192], spare[512];
    for (unsigned a = 0; a < mi.size; a += mi.writesize, pages++) {
        if (data) {
            ssize_t r = pread(fd, page, mi.writesize, a);
            if (r != (ssize_t)mi.writesize) {
                /* an uncorrectable page still returns its bytes, with EBADMSG, on 3.6 */
                if (rderr++ < 20) fprintf(stderr, "read at 0x%x: %s\n", a, r < 0 ? strerror(errno) : "short");
                if (r < 0 && errno != EBADMSG && errno != EUCLEAN) memset(page, 0xee, mi.writesize);
            }
            put(page, mi.writesize);
        }
        if (oob) {
            struct mtd_oob_buf ob = { a, mi.oobsize, spare };
            if (ioctl(fd, MEMREADOOB, &ob)) {
                if (rderr++ < 20) fprintf(stderr, "spare read at 0x%x: %s\n", a, strerror(errno));
                memset(spare, 0xee, mi.oobsize);
            }
            put(spare, mi.oobsize);
        }
    }
    fprintf(stderr, "%u pages, %u bad blocks, %u read errors\n", pages, nbad, rderr);
    if (stats && ioctl(fd, ECCGETSTATS, &s1) == 0)
        fprintf(stderr, "kernel ECC during this dump: %u bit errors corrected, %u failed\n",
                s1.corrected - s0.corrected, s1.failed - s0.failed);
    return 0;
}
