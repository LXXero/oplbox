/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi's port: what the library needs from the board, implemented by the firmware that links it (README.md
 * here). OPLBOX's, on felucca-bsp, is firmware/hal/fm1wifi_port_bsp.c and can be used as is by any FM-1
 * firmware on felucca-bsp; the host tests' is tests/wifi/sdio_model.c. Everything is called from the main
 * loop (or a task), never from an interrupt, and the library never calls it before fm1wifi_power_up().
 *
 * Interrupt hookup (IRQ 52, FM1_IRQ_WF) joins this header at stage 4, when the library first needs it.
 */
#pragma once
#include <stdint.h>

/* One aligned 32-bit access to a WiFi register: JL_ANA (FM1_JL_ANA_BASE..), JL_WL (FM1_JL_WL_BASE..) or the
 * core's window (FM1_WIFI_CORE_BASE..FM1_WIFI_CORE_LAST, the WL30 host block included). A port may refuse any
 * other address (read 0, write nothing); the library never asks for one. */
uint32_t fm1wifi_port_rd32(uint32_t addr);
void fm1wifi_port_wr32(uint32_t addr, uint32_t v);

/* The WiFi bits of the shared CLK_CON1 (FM1_CLK_CON1_WIFI, bits 15-14): set them to bits & FM1_CLK_CON1_WIFI
 * in one read-modify-write that leaves the other bits alone, and return what they were. */
uint32_t fm1wifi_port_clk(uint32_t bits);

/* Busy-wait at least us microseconds (power-up: 1 and 50; the BBP poll: 1 each). */
void fm1wifi_port_delay_us(uint32_t us);
