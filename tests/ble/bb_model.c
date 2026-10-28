/* SPDX-License-Identifier: GPL-3.0-only
 * See bb_model.h. */
#include <string.h>
#include "bb_model.h"
#include "fm1ble_port.h"
#include "fm1ble_regs.h"
#include "fm1ble_agc.h"

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
    /* the event at the counter's slot: the register-level tests take the next slot boundary a whole slot after the
     * write, the air takes one at the write and adds the bit offset (fm1ble.c op_arm); which is right is the chip's */
    bbm.period_us = bbm.air ? (slots - 1u) * 625u + bbm.ax[FM1BLE_AX_BITOFF] : slots * 625u;
}

uint32_t bb_model_air_us(int pdu_len) { return (uint32_t)(1 + 4 + pdu_len + 3) * 8u; }

/* ---- the air (bb_air) */

static uint8_t *air_blk(void) { return (uint8_t *)(bbm.hi | *slot(FM1BLE_BB_RAM_START, 1)); }
static uint8_t *air_ent(void) { uint8_t *b = air_blk(); return b + (b[0] | (b[1] & 0x7F) << 8); }
static uint16_t air16(uint32_t off) { uint8_t *e = air_ent(); return (uint16_t)(e[off] | e[off + 1] << 8); }
static void air16w(uint32_t off, uint32_t v) { uint8_t *e = air_ent(); e[off] = (uint8_t)v; e[off + 1] = (uint8_t)(v >> 8); }

/* a TX slot as the hardware would send it: header byte, length, payload, from TX slot k (the caller names the slot: TXTOG
 * & 1 for the PDU an event sends, the other one for the SCAN_RSP). An advertising PDU's header comes from TXAHDR (type bits
 * 3-0, TxAdd and RxAdd in bits 5-4) and TXDHDR bits 1-0 (the header's bits 5-4: ChSel), a data PDU's from TXDHDR bits
 * 4-0; the length from TXDHDR bits 15-8 (docs/BLUETOOTH_RADIO.md "TX and RX toggles"; which of the length copies the chip
 * reads is [unchecked]: the model takes the register's, so a driver that leaves TXDHDR out sends nothing a central takes) */
static int air_tx(int k, int adv, uint8_t *pdu)
{
    uint16_t ptr = air16(k ? FM1BLE_E_TXPTR1 : FM1BLE_E_TXPTR0), dh = air16(k ? FM1BLE_E_TXDHDR1 : FM1BLE_E_TXDHDR0);
    uint8_t *p;
    if (ptr == 1 || ptr < 4)
        return 0;
    p = air_blk() + ptr;
    if (adv) {
        uint16_t ah = air16(k ? FM1BLE_E_TXAHDR1 : FM1BLE_E_TXAHDR0);
        pdu[0] = (uint8_t)((ah & 0xFu) | (ah & 0x30u) << 2 | (dh & 3u) << 4);
    } else {
        pdu[0] = (uint8_t)(dh & 0x1Fu);
    }
    pdu[1] = (uint8_t)(dh >> 8);
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

/* the receiver listens after its own TX (bb_air): the gain table written whole and a window in WINCNTL2 */
static int air_listens(void) { return bbm.agc_n == (int)FM1BLE_AGC_WORDS && !bbm.agc_bad && air16(FM1BLE_E_WINCNTL2) != 0; }

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
        /* index 6 (D.2): the hop increment, the map's two ends (bit 6: 37 not in it, bit 7: 39 in it), the first channel */
        uint32_t ch = bbm.ax[FM1BLE_AX_CHANNEL], hop = (ch >> 8) & 0x1Fu, last = (ch & 0x80u) ? 39u : 38u, s, c;
        if (!hop) {
            /* A one-channel event (hop 0, the map that channel alone): what the FM-1 did with Cordio's operations at
             * 122afe3 (tools/fm1_ble_report.py, 852 of 2556 ended, all of them on 37, nothing heard from any): on 37 the
             * event ends having sent nothing a central heard; on 38 and 39 it never ends. */
            bbm.one_chan++;
            if (chan != 37u)
                return;
            bbm.now_us = t + 400u;
            air_irq(1u);
            return;
        }
        /* the event: the PDU of the slot TXTOG & 1 names on each channel from the first, hop by hop, to the map's end; a
         * request answered T_IFS after it; a CONNECT_IND ends the event. TXTOG changes once, after the event (its change
         * is the only transmit evidence the IR names; the FM-1's heard trials changed it at most once in 29 events) */
        s = air16(FM1BLE_E_TXTOG) & 1u;
        for (c = chan; c >= 37u && c <= last; c += hop) {
            n = air_tx((int)s, 1, pdu);
            if (!n)
                break;
            bbm.tx_sent++;
            r = a->adv ? a->adv(a->ctx, t, (uint8_t)c, pdu, n, req) : 0;
            if (r > 0 && !air_listens()) {
                bbm.rx_deaf++;
                r = 0;
            }
            t += bb_model_air_us(n) + 150u;
            if (r > 0) {
                bbm.now_us = t + bb_model_air_us(r);
                air_rx(req, r, 1);
                air_irq(1u << 8);
                if ((req[0] & 0xFu) == 5u)       /* CONNECT_IND: advertising ends */
                    break;
                if ((req[0] & 0xFu) == 3u) {     /* SCAN_REQ: the scan response T_IFS after it, from the other slot */
                    bbm.now_us += 150u;
                    n = air_tx((int)(s ^ 1u), 1, pdu);
                    if (n && a->scan_rsp)
                        a->scan_rsp(a->ctx, bbm.now_us, pdu, n);
                    bbm.now_us += bb_model_air_us(n);
                }
                t = bbm.now_us;
            } else {
                t += 80u;                       /* the receive window after T_IFS, closed */
            }
            t += 200u;                          /* to the next channel's synthesiser [unchecked: the chip's gap] */
            bbm.now_us = t;
        }
        air16w(FM1BLE_E_TXTOG, air16(FM1BLE_E_TXTOG) ^ 1u);
    } else if (state == 7u) {
        uint32_t win = air16(FM1BLE_E_WINCNTL2);
        uint32_t aa = (uint32_t)air16(FM1BLE_E_BDADDR0) | (uint32_t)air16(FM1BLE_E_BDADDR1) << 16;
        uint32_t crc = (uint32_t)air16(FM1BLE_E_CRCWORD0) | (uint32_t)(air16(FM1BLE_E_CRCWORD1) & 0xFFu) << 16;
        uint64_t at = 0;
        r = a->conn ? a->conn(a->ctx, t, win, (uint8_t)chan, aa, crc, req, &at) : 0;
        if (r > 0 && !air_listens()) {
            bbm.rx_deaf++;
            r = 0;
        }
        if (r > 0 && at >= t && at <= t + win) {
            int64_t off = (int64_t)(at - t);
            if (!bbm.conn_in || off < bbm.conn_off_min)
                bbm.conn_off_min = off;
            if (!bbm.conn_in || off > bbm.conn_off_max)
                bbm.conn_off_max = off;
            bbm.conn_in++;
            if (!bbm.period_us)
                bbm.conn_at_once++;
            /* the packet, then the response T_IFS after its end (conn_respond): what is in the TX slot then goes */
            bbm.now_us = at + bb_model_air_us(r);
            air_rx(req, r, 0);
            bbm.resp_at = bbm.now_us + 150u;
            bbm.resp_pending = 1;
            air_irq(1u << 8);
            return;
        } else {
            if (r > 0)
                bbm.air_missed++;
            bbm.now_us = t + win;
        }
    }
    air_irq(1u);
}

/* a connection event's second half, at T_IFS after the central's packet: the PDU of the TX slot TXTOG & 1 names, as for
 * advertising, TXTOG changed after it [unchecked in a connection], the event flag */
static void conn_respond(void)
{
    const bb_air *a = bbm.air;
    uint8_t pdu[260];
    int n;
    bbm.resp_pending = 0;
    bbm.now_us = bbm.resp_at;
    n = air_tx(air16(FM1BLE_E_TXTOG) & 1, 0, pdu);
    if (n && a->conn_rsp)
        a->conn_rsp(a->ctx, bbm.now_us, pdu, n);
    bbm.now_us += bb_model_air_us(n);
    if (n)
        air16w(FM1BLE_E_TXTOG, air16(FM1BLE_E_TXTOG) ^ 1u);
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
    } else if (addr == FM1BLE_AGC_TAB_AT) {
        bbm.agc_n = 0;
        bbm.agc_bad = v != 0;
    } else if (addr == FM1BLE_AGC_TAB) {
        if (bbm.agc_n >= (int)FM1BLE_AGC_WORDS || v != FM1BLE_AGC_TABLE[bbm.agc_n])
            bbm.agc_bad++;
        bbm.agc_n++;
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
    while (bbm.air && !in_event && ((bbm.resp_pending && bbm.resp_at <= end) ||
                                    (bbm.armed && bbm.armed_at_us + bbm.period_us <= end))) {
        if (bbm.resp_pending && (!bbm.armed || bbm.resp_at <= bbm.armed_at_us + bbm.period_us)) {
            in_event = 1;
            conn_respond();
            in_event = 0;
            continue;
        }
        bbm.now_us = bbm.armed_at_us + bbm.period_us;
        bbm.armed = 0;                          /* one-shot: the handler arms the next one */
        if (bbm.silent)                         /* a baseband that runs nothing and raises no flag */
            continue;
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
        if (!bbm.no_tx && bbm.ax[FM1BLE_AX_STATE] >> 12 == FM1BLE_STATE_ADV && bb_model_reg(FM1BLE_BB_RAM_START)) {
            air16w(FM1BLE_E_TXTOG, air16(FM1BLE_E_TXTOG) ^ 1u);   /* the advertising PDU went: TXTOG changes */
            bbm.tx_sent++;
        }
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
