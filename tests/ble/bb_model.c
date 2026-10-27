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
    bbm.period_us = slots * 625u;
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

int fm1ble_port_rf_common(void) { bbm.rf_common_calls++; bbm.common_ran = 1; return 1; }

void bb_model_advance_us(uint64_t us)
{
    uint64_t end = bbm.now_us + us;
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
