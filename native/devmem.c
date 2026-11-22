// SPDX-License-Identifier: AGPL-3.0-or-later
/* devmem ADDR [COUNT]: print 32-bit words of physical memory (read only). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: devmem ADDR [COUNT]\n"); return 2; }
    unsigned long a = strtoul(argv[1], 0, 0), n = argc > 2 ? strtoul(argv[2], 0, 0) : 1;
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    unsigned long base = a & ~0xfffUL;
    volatile unsigned *p = mmap(0, 0x2000, PROT_READ, MAP_SHARED, fd, base);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    for (unsigned long i = 0; i < n; i++)
        printf("%08lx: %08x\n", a + 4 * i, p[(a - base) / 4 + i]);
    return 0;
}
