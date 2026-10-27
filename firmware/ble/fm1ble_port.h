/* SPDX-License-Identifier: GPL-3.0-only
 * fm1ble's port: what the BLE baseband driver needs from the board, implemented by the firmware that links it
 * (firmware/hal/fm1ble_port_bsp.c on felucca-bsp; the host tests' is tests/ble/bb_model.c). Everything is called from
 * the main loop, never from an interrupt: the driver polls the baseband's pending flags instead of attaching IRQs 45 and
 * 29 (docs/BLUETOOTH_RADIO.md D.4), the way the WiFi driver polls its FIFO.
 */
#pragma once
#include <stdint.h>

/* One aligned 32-bit access to a Bluetooth register: CLK_CON1's two shared bits are fm1ble_port_clk's, the rest are
 * JL_WL (0x14000-0x14004), the baseband and analog blocks 0x20000-0x2FFFF, JL_IOMAP (0x50048) and the two WLA words
 * of B.3. A port may refuse any other address (read 0, write nothing). */
uint32_t fm1ble_port_rd32(uint32_t addr);
void fm1ble_port_wr32(uint32_t addr, uint32_t v);

/* The address the baseband is given for p, a word-aligned RAM block (0x2FC84, 0x2FCBC, 0x2FC88): on the FM-1 the CPU
 * address itself. */
uint32_t fm1ble_port_bus(const void *p);

/* Make the CPU's writes to RAM visible to the baseband before it reads them (the data cache: csync on the FM-1). */
void fm1ble_port_sync(void);

/* CLK_CON1 bits 15-14 (shared with the WiFi driver, which has the same bits as FM1_CLK_CON1_WIFI): set them to
 * bits & 0xC000 in one read-modify-write that leaves the others alone; returns what they were. */
uint32_t fm1ble_port_clk(uint32_t bits);

/* Busy-wait at least us microseconds. JieLi's delay(n) is taken as microseconds, as in the WiFi driver [unchecked]. */
void fm1ble_port_delay_us(uint32_t us);

/* Step 1 of JieLi's bt_rf_init, wf_rf_common_init: the WiFi core's clocks and the shared analog radio (docs/BLUETOOTH_RADIO.md
 * "not settled" 1). Called first by fm1ble_radio_up with `variant` 1 and not at all with variant 0. The port decides what
 * it does (the FM-1's: fm1wifi_radio_up when WiFi is linked); returns nonzero when it ran and the coexistence arbiter's
 * wlc_main would follow in JieLi's driver (not done here: its tables were not read). */
int fm1ble_port_rf_common(void);
