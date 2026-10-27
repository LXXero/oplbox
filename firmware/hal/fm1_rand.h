/* SPDX-License-Identifier: GPL-3.0-only
 * The chip's random number register: JL_RAND in JieLi's WL82.h (lsfr_base + map_adr(0x3b, 0x00) = 0x13B00: R64L, then
 * R64H, both read-only; the SDK's rand32() reads R64L). What it is on the AC79, a noise source or a free-running
 * counter, is not known [unchecked]: the WiFi page's report shows two reads of it. The WPA2 SNonces (oplbox.c
 * wifi_nonce) mix it with the timer and a count, so they stay new either way.
 */
#pragma once
#include <stdint.h>

#define FM1_RAND_R64L (*(volatile const uint32_t *)0x13B00u)

static inline uint32_t fm1_rand32(void) { return FM1_RAND_R64L; }
