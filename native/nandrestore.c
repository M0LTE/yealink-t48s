// SPDX-License-Identifier: AGPL-3.0-or-later
/* nandrestore [-s BLOCKS] /dev/mtdN < BACKUP      put a backup back into a partition
 * nandrestore -n [-o OUT] < BACKUP               on any PC: what a restore would write
 *
 * The other half of nandread -o (issue #30, docs/backup.md). BACKUP is a run of
 * pages, each 2048 data bytes followed by its 64 spare bytes; -s skips that many
 * 128 KB blocks of the partition first, and the restore covers as many whole blocks
 * as the input holds.
 *
 * For each block: erase it, check that it reads back erased, then write every page
 * the backup has data in, with the backup's free spare bytes (where Yealink's
 * filesystem keeps its tags). The kernel driver works out the ECC bytes itself and
 * reads every page back before it reports success; this program then reads each
 * page again and compares all 2112 bytes with the backup, ECC included. It stops at
 * the first difference, write error or bad block.
 *
 * Our kernel's driver only writes the ranges compiled into it
 * (kernel/drivers/t48s_nand.c), so this cannot touch the loader, U-Boot or anything
 * else outside them, whatever it is asked.
 *
 * -n needs no phone: it works out the bytes a restore would leave in the flash (the
 * data and free spare bytes from the backup, the ECC computed afresh the way the
 * driver does) and compares them with the backup. "identical" means a restore puts
 * back exactly what was read. -o also writes those bytes to a file. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PAGE 2048
#define SPARE 64
#define REC (PAGE + SPARE)
#define PPB 64                      /* pages per block */
#define STEP 512
#define GROUP 16
#define ECC_AT 9
#define ECC_LEN 7

/* from the kernel's mtd-abi.h (3.6) */
struct mtd_info_user { uint8_t type; uint32_t flags, size, erasesize, writesize, oobsize; uint64_t padding; };
struct erase_info_user { uint32_t start, length; };
struct mtd_oob_buf { uint32_t start, length; unsigned char *ptr; };
struct mtd_write_req { uint64_t start, len, ooblen, usr_data, usr_oob; uint8_t mode, padding[7]; };
#define MEMGETINFO     _IOR('M', 1, struct mtd_info_user)
#define MEMERASE       _IOW('M', 2, struct erase_info_user)
#define MEMREADOOB     _IOWR('M', 4, struct mtd_oob_buf)
#define MEMGETBADBLOCK _IOW('M', 11, long long)
#define MEMWRITE       _IOWR('M', 24, struct mtd_write_req)
#define MTD_OPS_PLACE_OOB 0

/* The phone's ECC: binary BCH over GF(2^13), polynomial 0x201b, 4 bits, 52 parity
 * bits; data most significant bit first, then four zero bits (tools/nandcheck.py,
 * kernel/drivers/t48s_nand.c). The generator polynomial without its top bit: */
#define BCH_LOW 0x4523043ab86abULL
#define BCH_MASK ((1ULL << 52) - 1)
static uint64_t table[256];

static uint64_t shift1(uint64_t r) { return (r >> 51 & 1) ? ((r << 1) ^ BCH_LOW) & BCH_MASK : (r << 1) & BCH_MASK; }

static void ecc_init(void)
{
    for (int b = 0; b < 256; b++) {
        uint64_t r = (uint64_t)b << 44;
        for (int j = 0; j < 8; j++) r = shift1(r);
        table[b] = r;
    }
}

static void ecc(const uint8_t *d, uint8_t *e)
{
    uint64_t r = 0;
    for (int i = 0; i < STEP; i++) r = ((r << 8) & BCH_MASK) ^ table[(r >> 44) ^ d[i]];
    for (int i = 0; i < 4; i++) r = shift1(r);
    for (int i = 0; i < ECC_LEN; i++) e[i] = r >> (8 * (ECC_LEN - 1 - i));
}

static int all_ff(const uint8_t *p, int n) { while (n--) if (*p++ != 0xff) return 0; return 1; }

/* one block of the backup from stdin: 1 read, 0 clean end of input */
static int get_block(uint8_t *b)
{
    size_t got = 0;
    while (got < PPB * REC) {
        ssize_t r = read(0, b + got, PPB * REC - got);
        if (r < 0) { perror("read"); exit(1); }
        if (r == 0) break;
        got += r;
    }
    if (got == 0) return 0;
    if (got != PPB * REC) { fprintf(stderr, "the backup does not end on a block boundary\n"); exit(1); }
    return 1;
}

/* what the flash holds after the page is written: the ECC is the driver's, not the backup's */
static void as_written(const uint8_t *rec, uint8_t *out)
{
    memcpy(out, rec, REC);
    if (all_ff(rec, REC)) return;               /* never written: left erased */
    for (int s = 0; s < PAGE / STEP; s++)
        ecc(rec + s * STEP, out + PAGE + s * GROUP + ECC_AT);
}

static int dry_run(const char *outpath)
{
    static uint8_t blk[PPB * REC], out[REC];
    FILE *o = outpath ? fopen(outpath, "wb") : NULL;
    unsigned blocks = 0, written = 0, erased = 0, differ = 0;
    if (outpath && !o) { perror(outpath); return 1; }
    while (get_block(blk)) {
        for (int p = 0; p < PPB; p++) {
            const uint8_t *rec = blk + p * REC;
            as_written(rec, out);
            if (all_ff(rec, REC)) erased++; else written++;
            if (rec[PAGE] != 0xff) { fprintf(stderr, "block %u page %d: bad block marker in the backup\n", blocks, p); differ++; }
            if (memcmp(out, rec, REC)) {
                if (differ++ < 10) fprintf(stderr, "block %u page %d: a restore would not give back the backup's bytes\n", blocks, p);
            }
            if (o) fwrite(out, 1, REC, o);
        }
        blocks++;
    }
    if (o && fclose(o)) { perror(outpath); return 1; }
    printf("%u blocks: %u pages to write, %u left erased, %u would differ from the backup: %s\n",
           blocks, written, erased, differ, differ ? "NOT IDENTICAL" : "identical");
    return differ ? 1 : 0;
}

static int read_page(int fd, unsigned a, uint8_t *rec)
{
    struct mtd_oob_buf ob = { a, SPARE, rec + PAGE };
    if (pread(fd, rec, PAGE, a) != PAGE) { fprintf(stderr, "read at 0x%x: %s\n", a, strerror(errno)); return -1; }
    if (ioctl(fd, MEMREADOOB, &ob)) { fprintf(stderr, "spare read at 0x%x: %s\n", a, strerror(errno)); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    int dry = 0, c;
    unsigned skip = 0;
    const char *outpath = NULL;
    while ((c = getopt(argc, argv, "no:s:")) != -1) {
        if (c == 'n') dry = 1;
        else if (c == 'o') outpath = optarg;
        else if (c == 's') skip = strtoul(optarg, NULL, 0);
        else argc = 0;
    }
    ecc_init();
    if (dry && argc == optind) return dry_run(outpath);
    if (dry || outpath || argc - optind != 1) {
        fprintf(stderr, "usage: nandrestore [-s BLOCKS] /dev/mtdN < BACKUP\n       nandrestore -n [-o OUT] < BACKUP\n");
        return 2;
    }
    int fd = open(argv[optind], O_RDWR);
    if (fd < 0) { perror(argv[optind]); return 1; }
    struct mtd_info_user mi;
    if (ioctl(fd, MEMGETINFO, &mi)) { perror("MEMGETINFO"); return 1; }
    if (mi.writesize != PAGE || mi.oobsize != SPARE || mi.erasesize != PPB * PAGE) {
        fprintf(stderr, "not the flash this tool knows\n");
        return 1;
    }
    static uint8_t blk[PPB * REC], now[REC];
    unsigned blocks = 0, written = 0;
    for (unsigned b = skip * mi.erasesize; get_block(blk); b += mi.erasesize, blocks++) {
        long long off = b;
        struct erase_info_user ei = { b, mi.erasesize };
        if (b + mi.erasesize > mi.size) { fprintf(stderr, "the backup is longer than the partition\n"); return 1; }
        if (ioctl(fd, MEMGETBADBLOCK, &off) > 0) { fprintf(stderr, "block at 0x%x is bad: stopping\n", b); return 1; }
        if (ioctl(fd, MEMERASE, &ei)) { fprintf(stderr, "erase at 0x%x: %s\n", b, strerror(errno)); return 1; }
        for (int p = 0; p < PPB; p++) {
            if (read_page(fd, b + p * PAGE, now)) return 1;
            if (!all_ff(now, REC)) { fprintf(stderr, "block at 0x%x page %d is not erased after the erase: stopping\n", b, p); return 1; }
        }
        for (int p = 0; p < PPB; p++) {
            uint8_t *rec = blk + p * REC;
            unsigned a = b + p * PAGE;
            if (all_ff(rec, REC)) continue;
            struct mtd_write_req wr = { a, PAGE, SPARE, (uintptr_t)rec, (uintptr_t)(rec + PAGE), MTD_OPS_PLACE_OOB, {0} };
            if (ioctl(fd, MEMWRITE, &wr)) { fprintf(stderr, "write at 0x%x: %s\n", a, strerror(errno)); return 1; }
            if (read_page(fd, a, now)) return 1;
            if (memcmp(now, rec, REC)) { fprintf(stderr, "page at 0x%x does not read back as the backup: stopping\n", a); return 1; }
            written++;
        }
    }
    printf("%u blocks restored and read back identical to the backup (%u pages written, the rest erased)\n", blocks, written);
    return 0;
}
