/* SPDX-License-Identifier: GPL-3.0-only
 * A register model of the BLE baseband for the host build (docs/BLUETOOTH.md "Cordio path as built" step 3): the port of
 * firmware/ble/fm1ble.c (fm1ble_port.h) with memory behind every register the driver uses, the indexed anchor register file
 * (D.2), the busy bit, a log of every write, and a baseband that runs advertising events as docs/BLUETOOTH_RADIO.md describes
 * them: when the anchor counter is armed, an event-done flag comes after the slots counted (once, or every interval when
 * `repeat` is set), and a packet can be injected into the receive buffer. Written from the same facts the driver is, so it
 * proves the driver does what the document says, not that the chip does. Own code. Host only. */
#ifndef OPLBOX_TESTS_BLE_BB_MODEL_H
#define OPLBOX_TESTS_BLE_BB_MODEL_H

#include <stdint.h>

#define BB_LOG 1024

typedef struct { uint32_t addr, val; } bb_write;

typedef struct {
    uint32_t regs[512][2];              /* sparse: addr, value */
    int nregs;
    bb_write log[BB_LOG];
    int nlog;
    uint16_t ax[16];                    /* the anchor register file, hw id 0 */
    uint16_t ax_last;                   /* the last value written to 0x28020 */
    uint32_t busy_reads;                /* BUSY reads as busy this many times after the counter is stopped */
    int never_idle;                     /* BUSY stays set */
    int repeat;                         /* the hardware repeats events by itself */
    int silent;                         /* the hardware raises no flag at all */
    int needs_common;                   /* flags only come after fm1ble_port_rf_common ran (the second variant) */
    int common_ran;
    uint64_t now_us, armed_at_us;
    int armed;
    uint32_t period_us;
    uint32_t clk_calls, rf_common_calls, sync_calls;
    uint32_t clk_bits;
    uint8_t *blk;                       /* the RAM block the driver gave the baseband (from 0x2FC84) */
    uintptr_t hi;                       /* the high half of host pointers, since the bus address is 32 bits */
} bb_model;

extern bb_model bbm;

void bb_model_reset(void);
/* Advance the baseband's clock; raises pending flags as the armed counter says. */
void bb_model_advance_us(uint64_t us);
/* A packet arrives in receive buffer k: PDU type, payload; sets RXAHDR/RXDHDR/RXSTAT as E.5 says and the receive flag. */
void bb_model_rx(int k, uint8_t type, const uint8_t *payload, uint8_t n);
uint32_t bb_model_reg(uint32_t addr);
/* The k-th write to addr (0 based), or -1; value out. */
int bb_model_nth_write(uint32_t addr, int k, uint32_t *val);
/* Index of the first log entry at or after `from` that writes addr with val, or -1. */
int bb_model_find(int from, uint32_t addr, uint32_t val);

#endif
