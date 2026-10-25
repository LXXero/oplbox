/* SPDX-License-Identifier: GPL-3.0-only
 * FM-1 WiFi HAL: the register accessors the driver in firmware/wifi/ calls (docs/WIFI.md stage 1).
 * Own code. The addresses come from fm1_wifi_regs.h (see its provenance note).
 *
 * The CPU-side accessors are complete: they bound the index and do one 32-bit access. The internal SDIO bus
 * (CMD52/CMD53) is stage 2 and returns FM1_WIFI_ENOTYET until then, so nothing here talks to the WiFi
 * core yet. Nothing in this file has run on an FM-1.
 *
 * Built with -DFM1_WIFI_HOST (tests/wifi_regs.c), every access goes to fm1_wifi_host_rd/wr instead of the
 * bus, so the host test sees the address of each access.
 */
#include "fm1_wifi_regs.h"

#ifdef FM1_WIFI_HOST
uint32_t fm1_wifi_host_rd(uint32_t addr);
void fm1_wifi_host_wr(uint32_t addr, uint32_t v);
#define RD(a) fm1_wifi_host_rd(a)
#define WR(a, v) fm1_wifi_host_wr((a), (v))
#else
#define RD(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define WR(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (v))
#endif

uint32_t fm1_wla_rd(unsigned n) { return n < FM1_WLA_CON_N ? RD(FM1_WLA_CON(n)) : 0; }
void fm1_wla_wr(unsigned n, uint32_t v) { if (n < FM1_WLA_CON_RO_FIRST) WR(FM1_WLA_CON(n), v); }

uint32_t fm1_wl_rd(unsigned n) { return n < FM1_WL_CON_N ? RD(FM1_WL_CON(n)) : 0; }
void fm1_wl_wr(unsigned n, uint32_t v) { if (n < FM1_WL_CON_N) WR(FM1_WL_CON(n), v); }

uint32_t fm1_wl_anl_rd(unsigned n) { return n < FM1_WL_ANL_CON_N ? RD(FM1_WL_ANL_CON(n)) : 0; }
void fm1_wl_anl_wr(unsigned n, uint32_t v) { if (n < FM1_WL_ANL_CON_N) WR(FM1_WL_ANL_CON(n), v); }

uint32_t fm1_wf_rd(unsigned n) { return n < FM1_WF_CON_N ? RD(FM1_WF_CON(n)) : 0; }
void fm1_wf_wr(unsigned n, uint32_t v) { if (n < FM1_WF_CON_N) WR(FM1_WF_CON(n), v); }

static int wl30_off_ok(uint32_t off) { return off <= FM1_WL30_HOST_LAST - FM1_WL30_HOST_BASE && !(off & 3u); }
uint32_t fm1_wl30_host_rd(uint32_t off) { return wl30_off_ok(off) ? RD(FM1_WL30_HOST(off)) : 0; }
void fm1_wl30_host_wr(uint32_t off, uint32_t v) { if (wl30_off_ok(off)) WR(FM1_WL30_HOST(off), v); }

/* Stage 2: CMD52 (IO_RW_DIRECT) and CMD53 (IO_RW_EXTENDED) on the internal SDIO host, polled from the main
 * loop with a bounded wait, never from an interrupt. */
int fm1_wifi_cmd52(unsigned fn, uint32_t addr, int write, uint8_t *data)
{
    (void)fn; (void)addr; (void)write; (void)data;
    return FM1_WIFI_ENOTYET;
}

int fm1_wifi_cmd53(unsigned fn, uint32_t addr, int write, void *buf, unsigned len)
{
    (void)fn; (void)addr; (void)write; (void)buf; (void)len;
    return FM1_WIFI_ENOTYET;
}

/* A MAC register is a 4-byte CMD53 on function 1 (docs/WIFI.md "WiFi core registers"). The byte order on
 * the bus is assumed little-endian, unchecked until stage 2 reads MAC_CSR0. */
int fm1_wifi_mac_rd(uint32_t addr, uint32_t *v)
{
    uint8_t b[4];
    int r = fm1_wifi_cmd53(FM1_WIFI_SDIO_FN, addr, 0, b, 4);
    if (r == FM1_WIFI_OK)
        *v = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return r;
}

int fm1_wifi_mac_wr(uint32_t addr, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return fm1_wifi_cmd53(FM1_WIFI_SDIO_FN, addr, 1, b, 4);
}
