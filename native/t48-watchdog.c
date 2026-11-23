// SPDX-License-Identifier: AGPL-3.0-or-later
/* t48-watchdog: arm the SoC's watchdog and feed it every 5 seconds (issue #17).
 *
 * If this program stops being scheduled for 30 seconds (the kernel has hung, or
 * the phone is too far gone to run it) the watchdog resets the phone. A crash
 * of this program alone does no harm: runit starts it again within a second.
 * On SIGTERM or SIGINT ("sv down watchdog") it turns the watchdog off and exits.
 *
 * The timer is the one the kexec loader arms and rcS disarms: it counts down
 * at 25 MHz, raises an interrupt nobody takes at the first zero and resets the
 * phone at the second. */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define WDT_PAGE    0x80017000
#define WDT_LOAD    (0x800 / 4)
#define WDT_CONTROL (0x808 / 4)     /* bit 0: interrupt, bit 1: reset */
#define WDT_INTCLR  (0x80c / 4)
#define WDT_LOCK    (0xc00 / 4)
#define UNLOCK      0x1ACCE551
#define HZ          25000000u
#define PERIOD      15              /* seconds per count-down; two of them reset */

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    volatile uint32_t *w = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, WDT_PAGE);
    if (w == MAP_FAILED) { perror("mmap"); return 1; }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    printf("watchdog on: the phone resets after %d s without a feed\n", 2 * PERIOD);
    fflush(stdout);
    while (!stop) {
        w[WDT_LOCK] = UNLOCK;
        w[WDT_LOAD] = HZ * PERIOD;
        w[WDT_INTCLR] = 1;
        w[WDT_CONTROL] = 3;
        w[WDT_LOCK] = 0;
        sleep(5);
    }
    w[WDT_LOCK] = UNLOCK;
    w[WDT_CONTROL] = 0;
    w[WDT_LOCK] = 0;
    printf("watchdog off\n");
    return 0;
}
