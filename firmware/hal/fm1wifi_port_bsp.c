/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi's port (firmware/wifi/fm1wifi_port.h) on felucca-bsp (firmware/bsp/): the register accesses the WiFi
 * library makes, its delays, the WiFi bits of the shared CLK_CON1, and the radio's trim. Nothing in it is OPLBOX's,
 * so any FM-1 firmware on felucca-bsp can compile it as is next to firmware/wifi/fm1wifi.c (firmware/wifi/README.md).
 * Main loop only. Stage 2 ran on an FM-1 (2026-10-26), stage 3 too; the frame FIFOs (stages 4-5) are not run on one yet.
 *
 * The trim must not read the flash (fm1wifi_port.h): the register access and the delay are in RAM
 * (.ram_code.fm1wifi_port, which felucca's app.ld copies there), use only inline helpers, and the delay is the BSP's
 * RAM-safe busy wait, which also feeds the watchdog; during the trim interrupts are masked, so no handler runs from
 * the flash.
 */
#include <stdint.h>
#include "fm1_sys.h"
#include "fm1wifi_port.h"
#include "fm1wifi_regs.h"

#define PORT_RAM __attribute__((section(".ram_code.fm1wifi_port"), noinline))

static inline __attribute__((always_inline)) int wifi_addr(uint32_t a)   /* the blocks fm1wifi_port.h names */
{
    return !(a & 3u) && ((a >= FM1_WLA_CON(0) && a <= FM1_WLA_CON(FM1_WLA_CON_N - 1u)) ||
                         (a >= FM1_JL_WL_BASE && a <= FM1_WL_SD_DPTR) ||
                         (a >= FM1_WIFI_CORE_BASE && a <= FM1_WIFI_CORE_LAST));
}

PORT_RAM uint32_t fm1wifi_port_rd32(uint32_t addr)
{
    return wifi_addr(addr) ? *(volatile uint32_t *)(uintptr_t)addr : 0;
}

PORT_RAM void fm1wifi_port_wr32(uint32_t addr, uint32_t v)
{
    if (!wifi_addr(addr)) return;
    *(volatile uint32_t *)(uintptr_t)addr = v;
    if (addr >= FM1_WL30_HOST_BASE && addr <= FM1_WL30_HOST_LAST)   /* the DMA block: as JieLi, a csync after each */
        __asm__ volatile("csync" ::: "memory");
}

uint32_t fm1wifi_port_bus(const void *p) { return (uint32_t)(uintptr_t)p; }   /* the DMA takes CPU addresses */

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

PORT_RAM void fm1wifi_port_delay_us(uint32_t us) { fm1_busy_us_inl(us); }

PORT_RAM void fm1wifi_port_trim(int on)   /* the trim: interrupts masked from its start to its end */
{
    if (on) __asm__ volatile("cli" ::: "memory");
    else __asm__ volatile("csync\n\tsti" ::: "memory");
}
