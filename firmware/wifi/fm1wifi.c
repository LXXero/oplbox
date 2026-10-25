/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi, stage 2: the core's power-up and power-down, its MAC and BBP registers, a diagnostic snapshot and the
 * RAM pattern check (fm1wifi.h). Own code. Every register access goes through the board's port
 * (fm1wifi_port.h); this file includes nothing outside its directory.
 *
 * The power-up is a table of register steps, the facts docs/WIFI.md "Stage 2 bring-up" lists (which register,
 * which bits, in which order, the delays), read from wl_rf_common.a's wf_clk_rst_init and wdma_init as that
 * document allows: addresses and values, no code. Nothing in this file has run on an FM-1.
 */
#include "fm1wifi.h"
#include "fm1wifi_port.h"

const char *fm1wifi_strerror(int err)
{
    switch (err) {
    case FM1WIFI_OK: return "OK";
    case FM1WIFI_ENOTYET: return "NOT YET";
    case FM1WIFI_ERANGE: return "RANGE";
    case FM1WIFI_EBUS: return "NO ANSWER";
    case FM1WIFI_ETIMEOUT: return "TIMEOUT";
    case FM1WIFI_ESTATE: return "OFF";
    default: return "?";
    }
}

/* ---- CPU-side registers by index */

#define RD(a) fm1wifi_port_rd32(a)
#define WR(a, v) fm1wifi_port_wr32((a), (v))

uint32_t fm1wifi_wla_rd(unsigned n) { return n < FM1_WLA_CON_N ? RD(FM1_WLA_CON(n)) : 0; }
void fm1wifi_wla_wr(unsigned n, uint32_t v) { if (n < FM1_WLA_CON_RO_FIRST) WR(FM1_WLA_CON(n), v); }
uint32_t fm1wifi_wl_rd(unsigned n) { return n < FM1_WL_CON_N ? RD(FM1_WL_CON(n)) : 0; }
void fm1wifi_wl_wr(unsigned n, uint32_t v) { if (n < FM1_WL_CON_N) WR(FM1_WL_CON(n), v); }
uint32_t fm1wifi_wl_anl_rd(unsigned n) { return n < FM1_WL_ANL_CON_N ? RD(FM1_WL_ANL_CON(n)) : 0; }
void fm1wifi_wl_anl_wr(unsigned n, uint32_t v) { if (n < FM1_WL_ANL_CON_N) WR(FM1_WL_ANL_CON(n), v); }
uint32_t fm1wifi_wf_rd(unsigned n) { return n < FM1_WF_CON_N ? RD(FM1_WF_CON(n)) : 0; }
void fm1wifi_wf_wr(unsigned n, uint32_t v) { if (n < FM1_WF_CON_N) WR(FM1_WF_CON(n), v); }

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
uint32_t fm1wifi_wl30_rd(uint32_t off) { return wl30_off_ok(off) ? RD(FM1_WL30_HOST(off)) : 0; }
void fm1wifi_wl30_wr(uint32_t off, uint32_t v) { if (wl30_off_ok(off)) WR(FM1_WL30_HOST(off), v); }

/* ---- power-up and power-down */

/* One step: SET ors bits in, CLR clears them, FIELD replaces the bits of a mask, ZERO writes 0, DELAY waits
 * (val microseconds). Each register step is one read and one write, one change at a time, in the order the
 * SDK makes them. */
enum { SET, CLR, FIELD, ZERO, DELAY };
typedef struct { uint8_t op; uint32_t addr, mask, val; } step;

static const step POWER_UP[] = {
    /* wf_clk_rst_init: (CLK_CON1 bits 15-14 cleared first, by the port) WF_CON0-4 to 0 */
    { ZERO, FM1_WF_CON(0), 0, 0 }, { ZERO, FM1_WF_CON(1), 0, 0 }, { ZERO, FM1_WF_CON(2), 0, 0 },
    { ZERO, FM1_WF_CON(3), 0, 0 }, { ZERO, FM1_WF_CON(4), 0, 0 },
    /* WF_CON0: bits 8, 6, 0 on, then bits 2, 3 and 1, each after 1 us; 50 us after the last */
    { SET, FM1_WF_CON(0), 0x100u, 0 }, { SET, FM1_WF_CON(0), 0x040u, 0 }, { SET, FM1_WF_CON(0), 0x001u, 0 },
    { DELAY, 0, 0, 1 }, { SET, FM1_WF_CON(0), 0x004u, 0 },
    { DELAY, 0, 0, 1 }, { SET, FM1_WF_CON(0), 0x008u, 0 },
    { DELAY, 0, 0, 1 }, { SET, FM1_WF_CON(0), 0x002u, 0 },
    { DELAY, 0, 0, 50 },
    /* WF_CON7: bits 7-0 = 5, bits 15-8 and 23-16 cleared, bits 26-24 and 30 set, bit 31 cleared */
    { FIELD, FM1_WF_CON(7), 0x000000FFu, 5u }, { CLR, FM1_WF_CON(7), 0x0000FF00u, 0 },
    { CLR, FM1_WF_CON(7), 0x00FF0000u, 0 }, { SET, FM1_WF_CON(7), 0x07000000u, 0 },
    { SET, FM1_WF_CON(7), 0x40000000u, 0 }, { CLR, FM1_WF_CON(7), 0x80000000u, 0 },
    /* wdma_init: WF_CON3 bit 24, then the WL30 host block's 0x04 and 0x00 */
    { SET, FM1_WF_CON(3), 0x01000000u, 0 },
    { SET, FM1_WL30_HOST_04, 0x800u, 0 }, { SET, FM1_WL30_HOST_04, 0x400u, 0 }, { SET, FM1_WL30_HOST_04, 0x200u, 0 },
    { SET, FM1_WL30_HOST_04, 0x020u, 0 }, { SET, FM1_WL30_HOST_04, 0x010u, 0 }, { CLR, FM1_WL30_HOST_04, 0x008u, 0 },
    { SET, FM1_WL30_HOST_04, 0x004u, 0 }, { SET, FM1_WL30_HOST_04, 0x002u, 0 }, { SET, FM1_WL30_HOST_04, 0x001u, 0 },
    { SET, FM1_WL30_HOST_00, 0x400u, 0 }, { CLR, FM1_WL30_HOST_00, 0x200u, 0 }, { CLR, FM1_WL30_HOST_00, 0x100u, 0 },
    { FIELD, FM1_WL30_HOST_00, 0x0E0u, 0x060u }, { CLR, FM1_WL30_HOST_00, 0x010u, 0 },
    { SET, FM1_WL30_HOST_00, 0x008u, 0 }, { SET, FM1_WL30_HOST_00, 0x004u, 0 }, { SET, FM1_WL30_HOST_00, 0x002u, 0 },
    { SET, FM1_WL30_HOST_00, 0x001u, 0 },
    /* (wdma_init then enables the external PA only when WIFI_PA_ENABLE is set; it is 0 on the FM-1) */
};

/* What power-up changes, in the order power-down puts it back: the DMA block first, the clock bits last. */
static const uint32_t SAVED[] = { FM1_WL30_HOST_00, FM1_WL30_HOST_04, FM1_WF_CON(7), FM1_WF_CON(4), FM1_WF_CON(3),
                                  FM1_WF_CON(2), FM1_WF_CON(1), FM1_WF_CON(0) };
#define N_SAVED (sizeof SAVED / sizeof SAVED[0])
static uint32_t saved[N_SAVED], saved_clk;
static int up;

int fm1wifi_is_up(void) { return up; }

int fm1wifi_power_up(void)
{
    unsigned i;
    if (up) return FM1WIFI_ESTATE;
    for (i = 0; i < N_SAVED; i++) saved[i] = RD(SAVED[i]);
    saved_clk = fm1wifi_port_clk(0);
    for (i = 0; i < sizeof POWER_UP / sizeof POWER_UP[0]; i++) {
        const step *s = &POWER_UP[i];
        uint32_t v;
        if (s->op == DELAY) { fm1wifi_port_delay_us(s->val); continue; }
        if (s->op == ZERO) { WR(s->addr, 0); continue; }
        v = RD(s->addr);
        v = s->op == SET ? v | s->mask : s->op == CLR ? v & ~s->mask : (v & ~s->mask) | s->val;
        WR(s->addr, v);
    }
    up = 1;
    return FM1WIFI_OK;
}

void fm1wifi_power_down(void)
{
    unsigned i;
    if (!up) return;
    for (i = 0; i < N_SAVED; i++) WR(SAVED[i], saved[i]);
    fm1wifi_port_clk(saved_clk);
    up = 0;
}

/* ---- the MAC registers, through the window */

static int mac_addr(uint32_t addr)
{
    if (!up) return FM1WIFI_ESTATE;
    if ((addr & 3u) || addr > FM1_WIFI_CORE_LAST - FM1_WIFI_CORE_BASE) return FM1WIFI_ERANGE;
    if (addr == FM1_WF_TX_FIFO || addr == FM1_WF_RX_FIFO) return FM1WIFI_ENOTYET;   /* DMA, stages 4-5 */
    return FM1WIFI_OK;
}

int fm1wifi_mac_rd(uint32_t addr, uint32_t *v)
{
    int r = mac_addr(addr);
    if (r == FM1WIFI_OK) *v = RD(FM1_WIFI_CORE(addr));
    return r;
}

int fm1wifi_mac_wr(uint32_t addr, uint32_t v)
{
    int r = mac_addr(addr);
    if (r == FM1WIFI_OK) WR(FM1_WIFI_CORE(addr), v);
    return r;
}

/* ---- the baseband through BBP_CSR_CFG */

static int bbp_idle(uint32_t *v)
{
    for (unsigned i = 0; i < FM1WIFI_BBP_TRIES; i++) {
        int r = fm1wifi_mac_rd(BBP_CSR_CFG, v);
        if (r != FM1WIFI_OK) return r;
        if (!(*v & BBP_CSR_CFG_BUSY)) return FM1WIFI_OK;
        fm1wifi_port_delay_us(1);
    }
    return FM1WIFI_ETIMEOUT;
}

static int bbp_access(uint32_t req, uint32_t *out)
{
    uint32_t v;
    int r = bbp_idle(&v);
    if (r == FM1WIFI_OK) r = fm1wifi_mac_wr(BBP_CSR_CFG, req);
    if (r == FM1WIFI_OK) r = fm1wifi_mac_wr(BBP_CSR_CFG, req | BBP_CSR_CFG_BUSY);
    if (r == FM1WIFI_OK) r = bbp_idle(&v);
    if (r == FM1WIFI_OK && out) *out = v;
    return r;
}

int fm1wifi_bbp_rd(uint8_t reg, uint8_t *val)
{
    uint32_t v = 0;
    int r = bbp_access((uint32_t)reg << 8 | BBP_CSR_CFG_READ_CONTROL | BBP_CSR_CFG_BBP_RW_MODE, &v);
    if (r == FM1WIFI_OK) *val = (uint8_t)(v & BBP_CSR_CFG_VALUE);
    return r;
}

int fm1wifi_bbp_wr(uint8_t reg, uint8_t val)
{
    return bbp_access((uint32_t)reg << 8 | val | BBP_CSR_CFG_BBP_RW_MODE, 0);
}

/* ---- stage 2's snapshot */

int fm1wifi_diag_read(fm1wifi_diag *d)
{
    int r = FM1WIFI_OK;
    d->mac_csr0 = d->mac_sys_ctrl = 0; d->bbp0 = 0;
    if (!up) r = fm1wifi_power_up();
    if (r == FM1WIFI_OK) r = fm1wifi_mac_rd(MAC_CSR0, &d->mac_csr0);
    if (r == FM1WIFI_OK) r = fm1wifi_mac_rd(MAC_SYS_CTRL, &d->mac_sys_ctrl);
    if (r == FM1WIFI_OK && (d->mac_csr0 == 0 || d->mac_csr0 == 0xFFFFFFFFu)) r = FM1WIFI_EBUS;
    d->bbp_err = up ? fm1wifi_bbp_rd(0, &d->bbp0) : FM1WIFI_ESTATE;
    d->err = r != FM1WIFI_OK ? r : d->bbp_err;
    return d->err;
}

/* ---- the RAM pattern check */

static uint32_t pattern(uintptr_t a) { return (uint32_t)a * 0x9E3779B1u ^ 0xA5C3965Au; }

static void words(const void *p, uint32_t n, uintptr_t *a, uintptr_t *e)
{
    *a = ((uintptr_t)p + 3u) & ~(uintptr_t)3u;
    *e = ((uintptr_t)p + n) & ~(uintptr_t)3u;
    if (*e < *a) *e = *a;
}

void fm1wifi_pattern_fill(void *p, uint32_t n)
{
    uintptr_t a, e;
    words(p, n, &a, &e);
    for (; a < e; a += 4u) *(uint32_t *)a = pattern(a);
}

fm1wifi_ramcheck fm1wifi_pattern_check(const void *p, uint32_t n)
{
    fm1wifi_ramcheck c = { 0, 0 };
    uintptr_t a, e;
    words(p, n, &a, &e);
    for (; a < e; a += 4u) {
        uint32_t x = *(const uint32_t *)a ^ pattern(a);
        for (unsigned b = 0; x && b < 4u; b++, x >>= 8)
            if (x & 0xFFu) { if (!c.changed) c.first = a + b; c.changed++; }
    }
    return c;
}
