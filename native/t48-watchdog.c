// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-watchdog: the SoC's watchdog (issues #17, #52).
 *
 *   t48-watchdog         arm it and feed it every 5 seconds, until told to stop
 *   t48-watchdog off     turn it off once (start-up: the loader arms it for the boot)
 *   t48-watchdog reset   reset the phone through it, about 2 seconds from now
 *
 * Feeding: if this program stops being scheduled for 30 seconds (the kernel has
 * hung, or the phone is too far gone to run it) the watchdog resets the phone. A
 * crash of this program alone does no harm: runit starts it again within a second.
 * On SIGTERM or SIGINT ("sv down watchdog") it turns the watchdog off and exits.
 *
 * Two ways to the same timer, chosen when the program starts:
 *   /dev/watchdog   where the kernel has a driver for it (Linux 6.18: the ARM
 *                   SP805 driver). The normal way.
 *   /dev/mem        where it has none (Linux 3.6): the registers themselves. The
 *                   timer counts down at 25 MHz, raises an interrupt nobody takes
 *                   at the first zero and resets the phone at the second.
 * Never both: with a driver in the kernel the registers are the driver's. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/watchdog.h>

#define WDT_PAGE    0x80017000
#define WDT_LOAD    (0x800 / 4)
#define WDT_CONTROL (0x808 / 4)     /* bit 0: interrupt, bit 1: reset */
#define WDT_INTCLR  (0x80c / 4)
#define WDT_LOCK    (0xc00 / 4)
#define UNLOCK      0x1ACCE551
#define HZ          25000000u
#define TIMEOUT     30              /* seconds without a feed before the reset */
#define FEED        5

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static int dev = -1;                /* /dev/watchdog, when the kernel has it */
static volatile uint32_t *reg;      /* else the registers */

static int wd_open(void)
{
    dev = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);  /* opening it arms it */
    if (dev >= 0)
        return 0;
    if (errno == EBUSY) {           /* the driver is there and someone else holds it */
        perror("/dev/watchdog");
        return -1;
    }
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) { perror("/dev/mem"); return -1; }
    reg = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, WDT_PAGE);
    if (reg == MAP_FAILED) { perror("mmap"); return -1; }
    return 0;
}

/* arm, or feed: the phone resets `seconds` from now unless this is called again */
static void wd_set(int seconds, int first)
{
    if (dev >= 0) {
        if (first && ioctl(dev, WDIOC_SETTIMEOUT, &seconds) < 0)
            perror("WDIOC_SETTIMEOUT");
        if (ioctl(dev, WDIOC_KEEPALIVE, 0) < 0)
            perror("WDIOC_KEEPALIVE");
        return;
    }
    reg[WDT_LOCK] = UNLOCK;
    reg[WDT_LOAD] = HZ / 2 * seconds;       /* two count-downs make the timeout */
    reg[WDT_INTCLR] = 1;
    reg[WDT_CONTROL] = 3;
    reg[WDT_LOCK] = 0;
}

static void wd_off(void)
{
    if (dev >= 0) {
        /* the letter V tells the driver this close is meant: it stops the timer */
        if (write(dev, "V", 1) != 1)
            perror("/dev/watchdog");
        close(dev);
        dev = -1;
        return;
    }
    reg[WDT_LOCK] = UNLOCK;
    reg[WDT_CONTROL] = 0;
    reg[WDT_LOCK] = 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "feed";

    if (strcmp(cmd, "feed") && strcmp(cmd, "off") && strcmp(cmd, "reset")) {
        fprintf(stderr, "usage: t48-watchdog [off | reset]\n");
        return 2;
    }
    if (wd_open() < 0)
        return 1;
    if (!strcmp(cmd, "off")) {
        wd_off();
        return 0;
    }
    if (!strcmp(cmd, "reset")) {
        wd_set(2, 1);
        for (;;)
            pause();                /* held open and never fed */
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    printf("watchdog on (%s): the phone resets after %d s without a feed\n",
           dev >= 0 ? "/dev/watchdog" : "/dev/mem", TIMEOUT);
    fflush(stdout);
    for (int first = 1; !stop; first = 0) {
        wd_set(TIMEOUT, first);
        sleep(FEED);
    }
    wd_off();
    printf("watchdog off\n");
    return 0;
}
