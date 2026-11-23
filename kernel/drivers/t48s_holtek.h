/* SPDX-License-Identifier: GPL-2.0 */
/* What t48s_holtek offers to other drivers of the Yealink SIP-T48S. */
#ifndef _LINUX_T48S_HOLTEK_H
#define _LINUX_T48S_HOLTEK_H

#include <linux/types.h>

/*
 * Turn the Holtek chip's one LED (the power lamp, top right of the phone) on
 * or off. Takes about 7 ms and may sleep. -ENODEV if the chip did not answer
 * at boot.
 */
int t48s_holtek_set_led(bool on);

#endif
