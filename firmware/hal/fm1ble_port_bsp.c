/* SPDX-License-Identifier: GPL-3.0-only
 * fm1ble's port (firmware/ble/fm1ble_port.h) on felucca-bsp (firmware/bsp/): the register accesses the BLE baseband driver
 * makes, its delay and the Bluetooth bits of the shared CLK_CON1. Main loop only. Not run on an FM-1.
 *
 * rf_common (JieLi's wf_rf_common_init, docs/BLUETOOTH_RADIO.md "not settled" 1) is OPLBOX's own WiFi power-up and radio
 * bring-up, which does the same job for the same analog block: the test run's second variant uses it.
 */
#include <stdint.h>
#include "fm1_sys.h"
#include "fm1ble_port.h"
#include "fm1wifi.h"

#define BLE_CLK_CON1 0x10010u               /* LSFR_BASE + 0x10: bits 15-14 are the radio clock gates */
#define BLE_CLK_BITS 0x0000C000u

static int ble_addr(uint32_t a)
{
    return !(a & 3u) && ((a >= 0x14000u && a <= 0x14004u) || (a >= 0x20000u && a <= 0x2FFFFu) || a == 0x50048u);
}

uint32_t fm1ble_port_rd32(uint32_t addr) { return ble_addr(addr) ? *(volatile uint32_t *)(uintptr_t)addr : 0; }

void fm1ble_port_wr32(uint32_t addr, uint32_t v)
{
    if (ble_addr(addr))
        *(volatile uint32_t *)(uintptr_t)addr = v;
}

uint32_t fm1ble_port_bus(const void *p) { return (uint32_t)(uintptr_t)p; }

void fm1ble_port_sync(void) { __asm__ volatile("csync" ::: "memory"); }

uint32_t fm1ble_port_clk(uint32_t bits)     /* CLK_CON1 is shared (USB, UART, WiFi): the read-modify-write with interrupts off */
{
    volatile uint32_t *c = (volatile uint32_t *)(uintptr_t)BLE_CLK_CON1;
    uint32_t old;
    __asm__ volatile("cli" ::: "memory");
    old = *c & BLE_CLK_BITS;
    *c = (*c & ~BLE_CLK_BITS) | (bits & BLE_CLK_BITS);
    __asm__ volatile("csync\n\tsti" ::: "memory");
    return old;
}

void fm1ble_port_delay_us(uint32_t us) { fm1_busy_us_inl(us); }

int fm1ble_port_rf_common(void)
{
    fm1wifi_radio r;
    if (fm1wifi_power_up() != FM1WIFI_OK)
        return 0;
    return fm1wifi_radio_up(&fm1wifi_cal_default, &r) == FM1WIFI_OK ? 1 : 0;
}
