/* SPDX-License-Identifier: GPL-3.0-only
 * lwIP 2.1.3's compiler and platform port for fm1wifi_net (lwip/arch.h includes it first): the firmware has no C
 * library headers beyond the compiler's own (stddef.h, stdint.h, limits.h), so no inttypes.h or ctype.h, and lwIP
 * prints nothing. LWIP_RAND (DHCP's transaction id) is fm1wifi_net_rand(), in ../../fm1wifi_net.c.
 *
 * FM1WIFI_NET_DEBUG (the host tests): a failed lwIP assertion prints where and aborts. Without it lwipopts.h sets
 * LWIP_NOASSERT and the assertions compile to nothing.
 */
#pragma once
#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H    1
#define LWIP_NO_UNISTD_H   1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "lu"
#define S32_F "ld"
#define X32_F "lx"
#define SZT_F "u"

uint32_t fm1wifi_net_rand(void);
#define LWIP_RAND() fm1wifi_net_rand()

#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#ifdef FM1WIFI_NET_DEBUG
#include <stdio.h>
#include <stdlib.h>
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "lwIP assertion \"%s\" failed at %s:%d\n", x, __FILE__, __LINE__); \
                                     abort(); } while (0)
#else
#define LWIP_PLATFORM_ASSERT(x) do { } while (0)
#endif
