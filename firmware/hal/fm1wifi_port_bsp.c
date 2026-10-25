/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi's port (firmware/wifi/fm1wifi_port.h) on felucca-bsp (firmware/bsp/): the register accesses the WiFi
 * library makes, its delays, and the WiFi bits of the shared CLK_CON1. Nothing in it is OPLBOX's, so any FM-1
 * firmware on felucca-bsp can compile it as is next to firmware/wifi/fm1wifi.c (firmware/wifi/README.md).
 * Main loop only. Not run on an FM-1 yet.
 */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1wifi_port.h"
#include "fm1wifi_regs.h"

static int wifi_addr(uint32_t a)   /* the blocks fm1wifi_port.h names, word-aligned; nothing else */
{
    return !(a & 3u) && ((a >= FM1_WLA_CON(0) && a <= FM1_WLA_CON(FM1_WLA_CON_N - 1u)) ||
                         (a >= FM1_JL_WL_BASE && a <= FM1_WL_SD_DPTR) ||
                         (a >= FM1_WIFI_CORE_BASE && a <= FM1_WIFI_CORE_LAST));
}

uint32_t fm1wifi_port_rd32(uint32_t addr) { return wifi_addr(addr) ? *(volatile uint32_t *)(uintptr_t)addr : 0; }

void fm1wifi_port_wr32(uint32_t addr, uint32_t v)
{
    if (wifi_addr(addr)) *(volatile uint32_t *)(uintptr_t)addr = v;
}

uint32_t fm1wifi_port_clk(uint32_t bits)   /* CLK_CON1 is shared (USB, UART): the read-modify-write with interrupts off */
{
    volatile uint32_t *c = (volatile uint32_t *)(uintptr_t)FM1_CLK_CON1_ADDR;
    uint32_t old;
    __asm__ volatile("cli" ::: "memory");              /* as fm1_irq.h's fm1_irq_off / fm1_irq_on */
    old = *c & FM1_CLK_CON1_WIFI;
    *c = (*c & ~FM1_CLK_CON1_WIFI) | (bits & FM1_CLK_CON1_WIFI);
    __asm__ volatile("csync\n\tsti" ::: "memory");
    return old;
}

void fm1wifi_port_delay_us(uint32_t us) { fm1_delay_us(us); }
