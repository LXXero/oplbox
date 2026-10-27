/* SPDX-License-Identifier: GPL-3.0-only
 * See bb_model.h. */
#include <string.h>
#include "bb_model.h"
#include "fm1ble_port.h"
#include "fm1ble_regs.h"

bb_model bbm;

void bb_model_reset(void) { memset(&bbm, 0, sizeof bbm); }

static uint32_t *slot(uint32_t addr, int create)
{
    for (int i = 0; i < bbm.nregs; i++)
        if (bbm.regs[i][0] == addr)
            return &bbm.regs[i][1];
    if (!create || bbm.nregs >= 512)
        return 0;
    bbm.regs[bbm.nregs][0] = addr;
    bbm.regs[bbm.nregs][1] = 0;
    return &bbm.regs[bbm.nregs++][1];
}

uint32_t bb_model_reg(uint32_t addr) { uint32_t *p = slot(addr, 0); return p ? *p : 0; }

static void pend(uint32_t bit) { *slot(FM1BLE_BB_IRQ_PEND, 1) |= bit; }

static void arm(void)
{
    uint32_t slots = (uint32_t)bbm.ax[FM1BLE_AX_CNT_LO] + 1u;
    bbm.armed = 1;
    bbm.armed_at_us = bbm.now_us;
    bbm.period_us = slots * 625u + (bbm.air ? bbm.ax[FM1BLE_AX_BITOFF] : 0u);
}

uint32_t bb_model_air_us(int pdu_len) { return (uint32_t)(1 + 4 + pdu_len + 3) * 8u; }

/* ---- the air (bb_air) */

static uint8_t *air_blk(void) { return (uint8_t *)(bbm.hi | *slot(FM1BLE_BB_RAM_START, 1)); }
static uint8_t *air_ent(void) { uint8_t *b = air_blk(); return b + (b[0] | (b[1] & 0x7F) << 8); }
static uint16_t air16(uint32_t off) { uint8_t *e = air_ent(); return (uint16_t)(e[off] | e[off + 1] << 8); }
static void air16w(uint32_t off, uint32_t v) { uint8_t *e = air_ent(); e[off] = (uint8_t)v; e[off + 1] = (uint8_t)(v >> 8); }

/* a TX buffer as the hardware would send it: header byte, length, payload (E.4: the header word 4 bytes before the
 * payload pointer, the length 2 before) */
static int air_tx(int k, int adv, uint8_t *pdu)
{
    uint16_t ptr = air16(k ? FM1BLE_E_TXPTR1 : FM1BLE_E_TXPTR0);
    uint8_t *p;
    if (ptr == 1 || ptr < 4)
        return 0;
    p = air_blk() + ptr;
    if (adv) {
        pdu[0] = p[-4];
        pdu[1] = p[-2];
    } else {
        uint16_t dh = air16(k ? FM1BLE_E_TXDHDR1 : FM1BLE_E_TXDHDR0);
        pdu[0] = (uint8_t)(dh & 0x1Fu);
        pdu[1] = (uint8_t)(dh >> 8);
    }
    memcpy(pdu + 2, p, pdu[1]);
    return 2 + pdu[1];
}

static void air_rx(const uint8_t *pdu, int n, int adv)
{
    uint8_t *blk = air_blk();
    uint16_t ptr = air16(FM1BLE_E_RXPTR0);
    memcpy(blk + ptr, pdu + 2, (size_t)(n - 2));
    air16w(FM1BLE_E_RXAHDR0, adv ? pdu[0] : 0u);
    air16w(FM1BLE_E_RXDHDR0, (uint32_t)pdu[1] << 8 | (adv ? 0u : (pdu[0] & 0x1Fu)));
    air16w(FM1BLE_E_RXSTAT0, 1);
}

static void air_irq(uint32_t bit)
{
    pend(bit);
    if (bbm.air->irq && (bb_model_reg(FM1BLE_BB_IRQ_EN) & bit))
        bbm.air->irq(bbm.air->ctx);
}

static void air_event(void)
{
    const bb_air *a = bbm.air;
    uint32_t state = (uint32_t)bbm.ax[FM1BLE_AX_STATE] >> 12, chan = bbm.ax[FM1BLE_AX_CHANNEL] & 0x3Fu;
    uint64_t t = bbm.now_us;
    uint8_t pdu[260], req[260];
    int n, r;

    bbm.air_events++;
    if (state == FM1BLE_STATE_ADV) {
        n = air_tx(0, 1, pdu);
        r = a->adv ? a->adv(a->ctx, t, (uint8_t)chan, pdu, n, req) : 0;
        t += bb_model_air_us(n) + 150u;
        if (r > 0) {
            bbm.now_us = t + bb_model_air_us(r);
            air_rx(req, r, 1);
            air_irq(1u << 8);
            if ((req[0] & 0xFu) == 3u) {         /* SCAN_REQ: the scan response T_IFS after it */
                bbm.now_us += 150u;
                n = air_tx(1, 1, pdu);
                if (n && a->scan_rsp)
                    a->scan_rsp(a->ctx, bbm.now_us, pdu, n);
                bbm.now_us += bb_model_air_us(n);
            }
        } else {
            bbm.now_us = t + 80u;               /* the receive window after T_IFS, closed */
        }
    } else if (state == 7u) {
        uint32_t win = (uint32_t)air16(FM1BLE_E_WINCNTL0) | (uint32_t)air16(FM1BLE_E_WINCNTL1) << 16;
        uint32_t aa = (uint32_t)air16(FM1BLE_E_BDADDR0) | (uint32_t)air16(FM1BLE_E_BDADDR1) << 16;
        uint32_t crc = (uint32_t)air16(FM1BLE_E_CRCWORD0) | (uint32_t)(air16(FM1BLE_E_CRCWORD1) & 0xFFu) << 16;
        uint64_t at = 0;
        r = a->conn ? a->conn(a->ctx, t, win, (uint8_t)chan, aa, crc, req, &at) : 0;
        if (r > 0 && at >= t && at <= t + win) {
            bbm.now_us = at + bb_model_air_us(r);
            air_rx(req, r, 0);
            air_irq(1u << 8);
            bbm.now_us += 150u;
            n = air_tx(0, 0, pdu);
            if (n && a->conn_rsp)
                a->conn_rsp(a->ctx, bbm.now_us, pdu, n);
            bbm.now_us += bb_model_air_us(n);
        } else {
            if (r > 0)
                bbm.air_missed++;
            bbm.now_us = t + win;
        }
    }
    air_irq(1u);
}

uint32_t fm1ble_port_rd32(uint32_t addr)
{
    if (addr == FM1BLE_BB_BUSY) {
        if (bbm.never_idle)
            return 2;
        if (bbm.busy_reads) {
            bbm.busy_reads--;
            return 2;
        }
        return 0;
    }
    if (addr == FM1BLE_BB_ANCHOR_RD)
        return bbm.ax[(bb_model_reg(FM1BLE_BB_ANCHOR_CMD) >> 10) & 15u];
    return bb_model_reg(addr);
}

void fm1ble_port_wr32(uint32_t addr, uint32_t v)
{
    if (bbm.nlog < BB_LOG) {
        bbm.log[bbm.nlog].addr = addr;
        bbm.log[bbm.nlog].val = v;
        bbm.nlog++;
    }
    if (addr == FM1BLE_BB_ANCHOR_WR) {
        bbm.ax_last = (uint16_t)v;
    } else if (addr == FM1BLE_BB_ANCHOR_CMD) {
        uint32_t idx = (v >> 10) & 15u;
        if ((v & 15u) == 5u) {
            bbm.ax[idx] = bbm.ax_last;
            if (idx == FM1BLE_AX_CNT_HI) {
                if (bbm.ax[idx] & 0x8000u)
                    arm();
                else {
                    bbm.armed = 0;
                    bbm.busy_reads = 3;
                }
            }
        }
    } else if (addr == FM1BLE_BB_IRQ_CLR) {
        *slot(FM1BLE_BB_IRQ_PEND, 1) &= ~v;
    } else if (addr == FM1BLE_BB_RAM_START) {
        bbm.blk = 0;                    /* resolved by the test through the address it hands the driver */
    }
    *slot(addr, 1) = v;
}

uint32_t fm1ble_port_bus(const void *p)
{
    uintptr_t u = (uintptr_t)p;
    bbm.hi = u & ~(uintptr_t)0xFFFFFFFFu;
    return (uint32_t)u;
}

void fm1ble_port_sync(void) { bbm.sync_calls++; }
uint32_t fm1ble_port_clk(uint32_t bits) { uint32_t o = bbm.clk_bits; bbm.clk_bits = bits & 0xC000u; bbm.clk_calls++; return o; }

void fm1ble_port_delay_us(uint32_t us) { bb_model_advance_us(us); }
uint32_t fm1ble_port_now_us(void) { return (uint32_t)bbm.now_us; }

int fm1ble_port_rf_common(void) { bbm.rf_common_calls++; bbm.common_ran = 1; return 1; }

void bb_model_advance_us(uint64_t us)
{
    static int in_event;                        /* a delay inside the handler moves the clock, never starts an event */
    uint64_t end = bbm.now_us + us;
    while (bbm.air && !in_event && bbm.armed && bbm.armed_at_us + bbm.period_us <= end) {
        bbm.now_us = bbm.armed_at_us + bbm.period_us;
        bbm.armed = 0;                          /* one-shot: the handler arms the next one */
        in_event = 1;
        air_event();
        in_event = 0;
    }
    if (bbm.air) {
        if (bbm.now_us < end)
            bbm.now_us = end;
        return;
    }
    while (bbm.armed && !bbm.silent && (!bbm.needs_common || bbm.common_ran) &&
           bbm.armed_at_us + bbm.period_us <= end) {
        bbm.now_us = bbm.armed_at_us + bbm.period_us;
        pend(1u);
        if (bbm.repeat) {                /* later events come every interval (anchor index 1), as the hardware's own */
            bbm.armed_at_us = bbm.now_us;
            bbm.period_us = (uint32_t)bbm.ax[FM1BLE_AX_INTERVAL] * 625u;
        }
        else
            bbm.armed = 0;
    }
    bbm.now_us = end;
}

void bb_model_rx(int k, uint8_t type, const uint8_t *payload, uint8_t n)
{
    uint32_t start = *slot(FM1BLE_BB_RAM_START, 1);
    uint8_t *blk = (uint8_t *)(bbm.hi | start);
    uint8_t *ent = blk + (blk[0] | (blk[1] & 0x7F) << 8);
    uint16_t ptr = (uint16_t)(ent[FM1BLE_E_RXPTR0 + 2 * k] | ent[FM1BLE_E_RXPTR0 + 2 * k + 1] << 8);
    uint16_t ahdr = type, dhdr = (uint16_t)(n << 8);
    memcpy(blk + ptr, payload, n);
    ent[FM1BLE_E_RXAHDR0 + 2 * k] = (uint8_t)ahdr; ent[FM1BLE_E_RXAHDR0 + 2 * k + 1] = (uint8_t)(ahdr >> 8);
    ent[FM1BLE_E_RXDHDR0 + 2 * k] = (uint8_t)dhdr; ent[FM1BLE_E_RXDHDR0 + 2 * k + 1] = (uint8_t)(dhdr >> 8);
    ent[FM1BLE_E_RXSTAT0 + 2 * k] = 1; ent[FM1BLE_E_RXSTAT0 + 2 * k + 1] = 0;
    pend(1u << 8);
}

int bb_model_nth_write(uint32_t addr, int k, uint32_t *val)
{
    for (int i = 0; i < bbm.nlog; i++)
        if (bbm.log[i].addr == addr && k-- == 0) {
            *val = bbm.log[i].val;
            return i;
        }
    return -1;
}

int bb_model_find(int from, uint32_t addr, uint32_t val)
{
    for (int i = from; i < bbm.nlog; i++)
        if (bbm.log[i].addr == addr && bbm.log[i].val == val)
            return i;
    return -1;
}
