// SPDX-License-Identifier: GPL-2.0
/*
 * t48s_secure: make the first serial port an ordinary (non-secure) device.
 *
 * The BCM11107 family has a TrustZone block register at 0x80008000: one bit per
 * on-chip device, 0 for secure and 1 for non-secure. A secure device's
 * interrupt goes to the secure interrupt controller, where Linux never sees
 * it; a non-secure one's goes to INTC1. The boot loader leaves UART A (bit 3)
 * secure. Its interrupt (INTC1 line 14) then never arrives: the kernel's own
 * console messages still come out, because they are written by polling, but
 * anything a program writes stops when the 16 byte FIFO is full, and nothing
 * typed is ever read. That is issue 59: the serial console stopping part way
 * through a start from U-Boot, on the live image and from flash alike.
 *
 * Linux 3.6's mach-bcmring did this in its machine init (secHw_setUnsecure,
 * arch/arm/mach-bcmring/core.c), for every device at once, which is why a
 * kernel started by kexec from Yealink's firmware never showed it. We set the
 * one bit that was seen to matter and proved on the phone: with the port dead
 * after a start from flash, writing it from user space brought the interrupt
 * and the shell back at once (2026-11-24).
 *
 * Registers, 32 bits each (low 16 device bits):
 *   0x00  status       1 = non-secure. After U-Boot: 0x3f1
 *   0x04  set non-secure, write 1 bits
 *   0x08  set secure, write 1 bits (never written here)
 *
 * GPL v2.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>

#define TZPC_BASE		0x80008000
#define TZPC_STATUS		0x00
#define TZPC_SET_NONSECURE	0x04
#define TZPC_UART_A		BIT(3)

static int __init t48s_secure_init(void)
{
	void __iomem *base;
	u32 was;

	if (!of_machine_is_compatible("yealink,sip-t4xs"))
		return 0;
	base = ioremap(TZPC_BASE, 0x10);
	if (!base)
		return -ENOMEM;
	was = readl(base + TZPC_STATUS);
	if (!(was & TZPC_UART_A)) {
		writel(TZPC_UART_A, base + TZPC_SET_NONSECURE);
		pr_info("t48s_secure: serial port made non-secure (0x%03x, was 0x%03x)\n",
			readl(base + TZPC_STATUS), was);
	}
	iounmap(base);
	return 0;
}
arch_initcall(t48s_secure_init);
