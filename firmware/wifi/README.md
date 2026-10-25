# fm1wifi: a WiFi driver for the FM-1, as a library

A driver for the WiFi core inside the M-VAVE FM-1's JieLi AC79 (WL82) chip, written from the hardware's facts
(docs/WIFI.md), not from JieLi's binary WiFi libraries. It is OPLBOX's, but written to be shared: any firmware on
the FM-1 can link it. Nothing in this directory includes a file from outside it, uses an OPLBOX global, or touches
a register itself.

**Status: stage 2** (docs/WIFI.md "Plan"). It powers the core up and down, reads and writes its MAC and baseband
registers, takes a diagnostic snapshot and checks RAM for writes by the hardware. Host-tested; not run on an FM-1
yet. No frames, scanning or networking yet; those are stages 3-8, in the layers docs/WIFI.md "Standalone library"
lists (driver → raw frames → station MAC → a netif-style packet interface an IP stack such as lwIP plugs into).

## Files

| File | What |
| --- | --- |
| `fm1wifi.h` | the public API |
| `fm1wifi.c` | the driver (stage 2) |
| `fm1wifi_regs.h` | the register map: addresses and masks only (SDK `WL82.h`, Linux `rt2800.h`, facts from `wl_rf_common.a`; provenance at its top) |
| `fm1wifi_port.h` | what the library needs from the board: you implement it |
| `wpa_crypto.c`, `wpa_crypto.h`, `bearssl/` | WPA2 key primitives for stage 8 (PMK, PRF/PTK, AES key unwrap) on a BearSSL subset; not used by the driver yet |

## The API (stage 2)

```c
#include "fm1wifi.h"

int  fm1wifi_power_up(void);       /* clocks, resets, DMA set-up (about 55 us); keeps what it replaces */
void fm1wifi_power_down(void);     /* every register back to how power-up found it */
int  fm1wifi_mac_rd(uint32_t addr, uint32_t *v);   /* MAC registers: MAC_CSR0, MAC_SYS_CTRL, ... */
int  fm1wifi_mac_wr(uint32_t addr, uint32_t v);
int  fm1wifi_bbp_rd(uint8_t reg, uint8_t *v);      /* baseband, through BBP_CSR_CFG, bounded waits */
int  fm1wifi_bbp_wr(uint8_t reg, uint8_t v);
int  fm1wifi_diag_read(fm1wifi_diag *d);           /* power up if needed; MAC_CSR0, MAC_SYS_CTRL, BBP 0 */
void fm1wifi_pattern_fill(void *p, uint32_t n);    /* the RAM check: fill a free region before power-up ... */
fm1wifi_ramcheck fm1wifi_pattern_check(const void *p, uint32_t n);   /* ... count the bytes that changed */
const char *fm1wifi_strerror(int err);
```

Every call returns `FM1WIFI_OK` or an error (`FM1WIFI_ESTATE` when the core is not powered up, `FM1WIFI_ETIMEOUT`
when a bounded wait ran out, `FM1WIFI_EBUS` when the core answered nothing, ...; `fm1wifi.h`). Nothing blocks for
long and nothing needs an RTOS: call it from your main loop (or a task), never from an interrupt. The core's window
is never touched before `fm1wifi_power_up()`.

## Linking it into your firmware

1. Compile `fm1wifi.c` with `-I<this directory>` (C11; it needs only `<stdint.h>`).
2. Implement `fm1wifi_port.h`: four functions, 32-bit register access to the WiFi blocks, the two WiFi bits of the
   shared `CLK_CON1`, and a microsecond delay. On **felucca-bsp**, use OPLBOX's
   [`firmware/hal/fm1wifi_port_bsp.c`](../hal/fm1wifi_port_bsp.c) as it is: it has nothing of OPLBOX's in it
   (compile it with felucca-bsp's `chip/` on the include path). On anything else, write your own, the same size.
   The host tests' port (`tests/wifi/sdio_model.c`) is a register model, if you want to test on a computer.
3. Keep the sound (or whatever else is timing-critical) quiet while the core powers up if you can; OPLBOX stops its
   audio for the stage 2 page.

Interrupts (IRQ 52) join the port at stage 4. Each later stage keeps this directory's rule: no dependency on
OPLBOX, everything board-specific through the port.

## Licence

Today every file here is GPL-3.0-only, as OPLBOX is (BearSSL's files: MIT). Stage 6 will take its code basis from
GPL-2.0-or-later sources, so the library as a whole will be GPL-licensed; which GPL version is an open decision for
xero (docs/WIFI.md "Standalone library").
