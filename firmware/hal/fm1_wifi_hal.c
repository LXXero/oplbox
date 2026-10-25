/* SPDX-License-Identifier: GPL-3.0-only
 * FM-1 WiFi HAL: the register accessors the driver in firmware/wifi/ calls (docs/WIFI.md stage 1).
 * Own code. The addresses come from fm1_wifi_regs.h (see its provenance note).
 *
 * The CPU-side accessors are complete: they bound the index and do one 32-bit access. The internal SDIO bus
 * (CMD52/CMD53) is stage 2 and returns FM1_WIFI_ENOTYET until then, so nothing here talks to the WiFi
 * core yet. Nothing in this file has run on an FM-1.
 *
 * Built with -DFM1_WIFI_HOST (tests/wifi_regs.c), every access goes to fm1_wifi_host_rd/wr instead of the
 * bus, so the host test sees the address of each access. Built with -DFM1_WIFI_SDIO_MODEL too
 * (tests/wifi_model.c), the SDIO commands go to the register model in tests/wifi/sdio_model.c.
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

static int wl30_off_ok(uint32_t off)
{
    switch (off) {
    case FM1_WL30_HOST_00 - FM1_WL30_HOST_BASE:
    case FM1_WL30_HOST_04 - FM1_WL30_HOST_BASE:
    case FM1_WL30_HOST_10 - FM1_WL30_HOST_BASE:
    case FM1_WL30_HOST_14 - FM1_WL30_HOST_BASE:
    case FM1_WL30_HOST_18 - FM1_WL30_HOST_BASE:
    case FM1_WL30_HOST_1C - FM1_WL30_HOST_BASE:
        return 1;
    default:
        return 0;
    }
}
uint32_t fm1_wl30_host_rd(uint32_t off) { return wl30_off_ok(off) ? RD(FM1_WL30_HOST(off)) : 0; }
void fm1_wl30_host_wr(uint32_t off, uint32_t v) { if (wl30_off_ok(off)) WR(FM1_WL30_HOST(off), v); }

#ifndef FM1_WIFI_SDIO_MODEL
/* Stage 2: commands, CMD52 (IO_RW_DIRECT) and CMD53 (IO_RW_EXTENDED) on the internal SDIO host, polled from
 * the main loop with a bounded wait, never from an interrupt. */
int fm1_wifi_sdio_cmd(unsigned idx, uint32_t arg, uint32_t *resp)
{
    (void)idx; (void)arg; (void)resp;
    return FM1_WIFI_ENOTYET;
}

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
#endif

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

static int cmd52_wr(unsigned fn, uint32_t addr, uint8_t v) { return fm1_wifi_cmd52(fn, addr, 1, &v); }

/* docs/WIFI.md "Internal SDIO enumeration", in the SDIO Simplified Specification's order: the first CMD5 asks
 * for the card's conditions, the next ones repeat until it reports ready (wl30_sdio_enum sends two, so a card
 * ready at once gets the same two). CMD3 gets the card's RCA, which CMD7 selects it with (JieLi's fixed
 * 0x00010000 is RCA 1). Then the CCCR writes and function 1's four, in wl30_sdio_enum's order. Not run on an
 * FM-1. The specification has the host read I/O Ready before using a function; wl30_sdio_enum, as decoded,
 * does not, but this does: after I/O Enable it reads I/O Ready (at most FM1_WIFI_IO_READY_TRIES times) and
 * touches function 1 only once its bit is set, else FM1_WIFI_ETIMEOUT. */
int fm1_wifi_sdio_enum(void)
{
    uint32_t r4 = 0, r6 = 0, rca = 0;
    int r = fm1_wifi_sdio_cmd(FM1_SDIO_CMD5, FM1_WIFI_CMD5_ARG, &r4);
    if (r != FM1_WIFI_OK) return r;
    if (FM1_SDIO_R4_NFN(r4) < FM1_WIFI_SDIO_FN) return FM1_WIFI_EBUS;   /* no function 1: not the WiFi core */
    for (unsigned i = 0;; i++) {
        if (i == FM1_WIFI_CMD5_TRIES) return FM1_WIFI_ETIMEOUT;
        if ((r = fm1_wifi_sdio_cmd(FM1_SDIO_CMD5, FM1_WIFI_CMD5_ARG, &r4)) != FM1_WIFI_OK) return r;
        if (r4 & FM1_SDIO_R4_READY) break;
    }
    for (unsigned i = 0; rca == 0; i++) {
        if (i == FM1_WIFI_CMD3_TRIES) return FM1_WIFI_EBUS;
        if ((r = fm1_wifi_sdio_cmd(FM1_SDIO_CMD3, 0, &r6)) != FM1_WIFI_OK) return r;
        rca = r6 >> 16;
    }
    if ((r = fm1_wifi_sdio_cmd(FM1_SDIO_CMD7, rca << 16, &r6)) != FM1_WIFI_OK) return r;
    if ((r = cmd52_wr(0, FM1_SDIO_CCCR_IO_ENABLE, FM1_WIFI_IO_ENABLE)) != FM1_WIFI_OK) return r;
    for (unsigned i = 0;; i++) {
        uint8_t rdy = 0;
        if (i == FM1_WIFI_IO_READY_TRIES) return FM1_WIFI_ETIMEOUT;
        if ((r = fm1_wifi_cmd52(0, FM1_SDIO_CCCR_IO_READY, 0, &rdy)) != FM1_WIFI_OK) return r;
        if (rdy & (1u << FM1_WIFI_SDIO_FN)) break;
    }
    if ((r = cmd52_wr(0, FM1_SDIO_CCCR_BUS_IF, FM1_SDIO_BUS_WIDTH_4)) != FM1_WIFI_OK) return r;
    if ((r = cmd52_wr(0, FM1_SDIO_CCCR_INT_ENABLE, FM1_WIFI_INT_ENABLE)) != FM1_WIFI_OK) return r;
    for (unsigned i = 0; i < FM1_WIFI_FN1_INIT_N; i++)
        if ((r = cmd52_wr(FM1_WIFI_SDIO_FN, FM1_WIFI_FN1_INIT_FIRST + i, 0)) != FM1_WIFI_OK) return r;
    return FM1_WIFI_OK;
}
