// SPDX-License-Identifier: AGPL-3.0-or-later
/* nandrestore [-s BLOCKS] /dev/mtdN < BACKUP      put a backup back into a partition
 * nandrestore -b /dev/mtdN < FILE                 write a plain file (a kernel image)
 * nandrestore -e BLOCKS [-s BLOCKS] /dev/mtdN     erase blocks, and check them erased
 * nandrestore -c [-s BLOCKS] /dev/mtdN < BACKUP   compare only: nothing is written
 * nandrestore -n [-o OUT] < BACKUP                on any PC: what a restore would write
 * nandrestore -f CHIP -p NAME ...                 on any PC: any of the above against a
 *                                                 file that stands in for the flash
 *
 * The other half of nandread -o (issues #30 and #8, docs/backup.md, docs/install.md).
 * BACKUP is a run of pages, each 2048 data bytes followed by its 64 spare bytes; -s
 * skips that many 128 KB blocks of the partition first, and the restore covers as
 * many whole blocks as the input holds.
 *
 * For each block: erase it, check that it reads back erased, then write every page
 * the backup has data in, with the backup's free spare bytes (where Yealink's
 * filesystem keeps its tags). The kernel driver works out the ECC bytes itself and
 * reads every page back before it reports success; this program then reads each
 * page again and compares all 2112 bytes with the backup, ECC included. It stops at
 * the first difference, write error or bad block.
 *
 * -b takes a plain file instead (pages without spare bytes): the whole partition is
 * erased, the file written from its start, and each page compared with the file and
 * with the ECC bytes this program works out for it. For the kernel partition.
 *
 * -e erases without writing: BLOCKS blocks, each checked erased, spare bytes too.
 *
 * -c opens the flash read-only and compares it with BACKUP, all 2112 bytes of every
 * page. It prints how many pages differ.
 *
 * Our kernel's driver only writes the ranges compiled into it, and only while its
 * lock for that range is open (kernel6/drivers/t48s_nand.c), so this cannot touch
 * the loader, U-Boot or anything else outside them, whatever it is asked.
 *
 * -n needs no phone: it works out the bytes a restore would leave in the flash (the
 * data and free spare bytes from the backup, the ECC computed afresh the way the
 * driver does) and compares them with the backup. "identical" means a restore puts
 * back exactly what was read. -o also writes those bytes to a file.
 *
 * -f CHIP -p NAME needs no phone either. CHIP is a file holding the whole flash as
 * nandread -o would dump it (the twelve partitions of a backup, one after another),
 * and NAME is a partition of it. The file is treated as the driver treats the
 * flash: only kernel and app can be erased or written, a page must be erased before
 * it is written, and the ECC bytes are worked out here. For rehearsing an install
 * and the way back on a PC (tools/test-install.sh). */
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

/* ---- the flash: a real MTD device, or a file standing in for the whole chip ---- */

static const struct { const char *name; unsigned off, size; int ours; } parts[] = {
    { "ubl", 0x00000000, 0x00020000, 0 }, { "uboot", 0x00020000, 0x00100000, 0 },
    { "env", 0x00120000, 0x00100000, 0 }, { "userenv", 0x00220000, 0x00100000, 0 },
    { "logo", 0x00320000, 0x00100000, 0 }, { "smkern", 0x00420000, 0x00280000, 0 },
    { "smrfs", 0x006a0000, 0x00f00000, 0 }, { "kernel", 0x015a0000, 0x00280000, 1 },
    { "rfs", 0x01820000, 0x00f00000, 0 }, { "app", 0x02720000, 0x04ae0000, 1 },
    { "cfg", 0x07200000, 0x00400000, 0 }, { "data", 0x07600000, 0x00a00000, 0 },
};

static int fd = -1;
static int model;                   /* fd is a chip file, not an MTD device */
static unsigned part_off, part_size, model_ours;

static void as_written(const uint8_t *rec, uint8_t *out);

static int dev_read(unsigned a, uint8_t *rec)
{
    if (model) {
        if (pread(fd, rec, REC, (off_t)((part_off + a) / PAGE) * REC) != REC) { perror("chip file"); return -1; }
        return 0;
    }
    struct mtd_oob_buf ob = { a, SPARE, rec + PAGE };
    if (pread(fd, rec, PAGE, a) != PAGE) { fprintf(stderr, "read at 0x%x: %s\n", a, strerror(errno)); return -1; }
    if (ioctl(fd, MEMREADOOB, &ob)) { fprintf(stderr, "spare read at 0x%x: %s\n", a, strerror(errno)); return -1; }
    return 0;
}

static int dev_bad(unsigned b)
{
    if (model) {
        uint8_t rec[REC];
        return dev_read(b, rec) ? -1 : rec[PAGE] != 0xff;
    }
    long long off = b;
    return ioctl(fd, MEMGETBADBLOCK, &off) > 0;
}

static int dev_erase(unsigned b)
{
    if (model) {
        static uint8_t ff[PPB * REC];
        if (!model_ours) { fprintf(stderr, "erase at 0x%x refused: not ours to write\n", part_off + b); errno = EROFS; return -1; }
        memset(ff, 0xff, sizeof(ff));
        if (pwrite(fd, ff, sizeof(ff), (off_t)((part_off + b) / PAGE) * REC) != sizeof(ff)) { perror("chip file"); return -1; }
        return 0;
    }
    struct erase_info_user ei = { b, PPB * PAGE };
    if (ioctl(fd, MEMERASE, &ei)) { fprintf(stderr, "erase at 0x%x: %s\n", b, strerror(errno)); return -1; }
    return 0;
}

/* one page with its free spare bytes; the ECC bytes are the driver's to work out */
static int dev_write(unsigned a, const uint8_t *rec)
{
    if (model) {
        uint8_t now[REC], out[REC];
        if (!model_ours) { fprintf(stderr, "write at 0x%x refused: not ours to write\n", part_off + a); errno = EROFS; return -1; }
        if (dev_read(a, now)) return -1;
        if (!all_ff(now, REC)) { fprintf(stderr, "page at 0x%x is not erased; not written\n", part_off + a); errno = EIO; return -1; }
        as_written(rec, out);
        if (pwrite(fd, out, REC, (off_t)((part_off + a) / PAGE) * REC) != REC) { perror("chip file"); return -1; }
        return 0;
    }
    struct mtd_write_req wr = { a, PAGE, SPARE, (uintptr_t)rec, (uintptr_t)(rec + PAGE), MTD_OPS_PLACE_OOB, {0} };
    if (ioctl(fd, MEMWRITE, &wr)) { fprintf(stderr, "write at 0x%x: %s\n", a, strerror(errno)); return -1; }
    return 0;
}

static int dev_open(const char *path, const char *part, int rw)
{
    fd = open(path, rw ? O_RDWR : O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    if (part) {
        model = 1;
        for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
            if (!strcmp(parts[i].name, part)) {
                part_off = parts[i].off; part_size = parts[i].size; model_ours = parts[i].ours;
                if (lseek(fd, 0, SEEK_END) != (off_t)0x08000000 / PAGE * REC) { fprintf(stderr, "%s is not a whole chip\n", path); return -1; }
                return 0;
            }
        fprintf(stderr, "no partition called %s\n", part);
        return -1;
    }
    struct mtd_info_user mi;
    if (ioctl(fd, MEMGETINFO, &mi)) { perror("MEMGETINFO"); return -1; }
    if (mi.writesize != PAGE || mi.oobsize != SPARE || mi.erasesize != PPB * PAGE) {
        fprintf(stderr, "not the flash this tool knows\n");
        return -1;
    }
    part_size = mi.size;
    return 0;
}

/* erase one block and see that all of it, spare bytes included, reads ff */
static int erase_checked(unsigned b)
{
    uint8_t now[REC];
    if (dev_bad(b)) { fprintf(stderr, "block at 0x%x is bad: stopping\n", b); return -1; }
    if (dev_erase(b)) return -1;
    for (int p = 0; p < PPB; p++) {
        if (dev_read(b + p * PAGE, now)) return -1;
        if (!all_ff(now, REC)) { fprintf(stderr, "block at 0x%x page %d is not erased after the erase: stopping\n", b, p); return -1; }
    }
    return 0;
}

/* one page of plain input: 1 read (the tail padded with ff), 0 end of input */
static int get_plain(uint8_t *rec)
{
    size_t got = 0;
    memset(rec, 0xff, REC);
    while (got < PAGE) {
        ssize_t r = read(0, rec + got, PAGE - got);
        if (r < 0) { perror("read"); exit(1); }
        if (r == 0) break;
        got += r;
    }
    return got != 0;
}

int main(int argc, char **argv)
{
    int dry = 0, plain = 0, compare = 0, c;
    long erase = -1;
    unsigned skip = 0;
    const char *outpath = NULL, *chip = NULL, *part = NULL;
    while ((c = getopt(argc, argv, "no:s:be:cf:p:")) != -1) {
        if (c == 'n') dry = 1;
        else if (c == 'o') outpath = optarg;
        else if (c == 's') skip = strtoul(optarg, NULL, 0);
        else if (c == 'b') plain = 1;
        else if (c == 'e') erase = strtol(optarg, NULL, 0);
        else if (c == 'c') compare = 1;
        else if (c == 'f') chip = optarg;
        else if (c == 'p') part = optarg;
        else argc = 0;
    }
    ecc_init();
    if (dry && argc == optind && !chip) return dry_run(outpath);
    if (dry || outpath || plain + compare + (erase >= 0) > 1 || !chip != !part ||
        argc - optind != (chip ? 0 : 1) || (plain && skip)) {
        fprintf(stderr, "usage: nandrestore [-s BLOCKS] [-b | -c | -e BLOCKS] /dev/mtdN < BACKUP\n"
                        "       nandrestore -n [-o OUT] < BACKUP\n"
                        "       nandrestore -f CHIP -p NAME [-s BLOCKS] [-b | -c | -e BLOCKS] < BACKUP\n");
        return 2;
    }
    if (dev_open(chip ? chip : argv[optind], part, !compare)) return 1;
    static uint8_t blk[PPB * REC], now[REC], want[REC];
    unsigned blocks = 0, written = 0, differ = 0;
    unsigned start = skip * PPB * PAGE;

    if (erase >= 0) {
        if (start + (unsigned long long)erase * PPB * PAGE > part_size) { fprintf(stderr, "more blocks than the partition has\n"); return 1; }
        for (unsigned b = start; blocks < (unsigned)erase; b += PPB * PAGE, blocks++)
            if (erase_checked(b)) return 1;
        printf("%u blocks erased, and every page of them reads back erased\n", blocks);
        return 0;
    }
    if (compare) {
        unsigned pages = 0;
        for (unsigned b = start; get_block(blk); b += PPB * PAGE, blocks++) {
            if (b + PPB * PAGE > part_size) { fprintf(stderr, "the backup is longer than the partition\n"); return 1; }
            for (int p = 0; p < PPB; p++, pages++) {
                if (dev_read(b + p * PAGE, now)) return 1;
                if (memcmp(now, blk + p * REC, REC) && differ++ < 5)
                    fprintf(stderr, "page at 0x%x differs\n", b + p * PAGE);
            }
        }
        printf("%u pages compared, data and spare bytes: %u differ: %s\n", pages, differ, differ ? "NOT IDENTICAL" : "identical");
        return differ ? 1 : 0;
    }
    if (plain) {
        /* the whole partition erased first, so nothing of the old contents is left */
        for (unsigned b = 0; b < part_size; b += PPB * PAGE, blocks++)
            if (erase_checked(b)) return 1;
        for (unsigned a = 0; get_plain(blk); a += PAGE) {
            if (a + PAGE > part_size) { fprintf(stderr, "the file is larger than the partition\n"); return 1; }
            if (all_ff(blk, PAGE)) { fprintf(stderr, "the page at 0x%x of the file is all ff: not a file this can write\n", a); return 1; }
            as_written(blk, want);
            if (dev_write(a, blk) || dev_read(a, now)) return 1;
            if (memcmp(now, want, REC)) { fprintf(stderr, "page at 0x%x does not read back as written: stopping\n", a); return 1; }
            written++;
        }
        printf("%u blocks erased, %u pages written and read back identical, ECC bytes included\n", blocks, written);
        return 0;
    }
    for (unsigned b = start; get_block(blk); b += PPB * PAGE, blocks++) {
        if (b + PPB * PAGE > part_size) { fprintf(stderr, "the backup is longer than the partition\n"); return 1; }
        if (erase_checked(b)) return 1;
        for (int p = 0; p < PPB; p++) {
            uint8_t *rec = blk + p * REC;
            unsigned a = b + p * PAGE;
            if (all_ff(rec, REC)) continue;
            if (dev_write(a, rec) || dev_read(a, now)) return 1;
            if (memcmp(now, rec, REC)) { fprintf(stderr, "page at 0x%x does not read back as the backup: stopping\n", a); return 1; }
            written++;
        }
    }
    printf("%u blocks restored and read back identical to the backup (%u pages written, the rest erased)\n", blocks, written);
    return 0;
}
