/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi: a driver for the WiFi core inside the FM-1's JieLi AC79 (WL82) chip, written to be shared: no
 * OPLBOX code, headers or globals, only this directory and the board's port (fm1wifi_port.h). README.md here
 * has the API and how another firmware links it; docs/WIFI.md the hardware and the plan.
 *
 * Stage 2 (this): power the core up and down, read and write its MAC registers and its baseband (BBP)
 * registers, a diagnostic snapshot, and a RAM pattern check. Not run on an FM-1 yet.
 */
#pragma once
#include <stdint.h>
#include "fm1wifi_regs.h"

/* OK; ENOTYET: not built yet (the frame FIFOs, stages 4-5); ERANGE: an argument out of range, nothing done;
 * EBUS: the core answered nothing a working one would (reads of 0 or all ones); ETIMEOUT: a bounded wait ran
 * out; ESTATE: the core is not powered up (its window is never touched then). */
enum { FM1WIFI_OK = 0, FM1WIFI_ENOTYET = -1, FM1WIFI_ERANGE = -2, FM1WIFI_EBUS = -3, FM1WIFI_ETIMEOUT = -4,
       FM1WIFI_ESTATE = -5 };
const char *fm1wifi_strerror(int err);   /* "OK", "NOT YET", "RANGE", "NO ANSWER", "TIMEOUT", "OFF" */

/* CPU-side registers by index; an index past the block reads 0 / writes nothing. Usable without power-up. */
uint32_t fm1wifi_wla_rd(unsigned n);
void fm1wifi_wla_wr(unsigned n, uint32_t v);   /* refuses the read-only WLA_CON31..39 */
uint32_t fm1wifi_wl_rd(unsigned n);
void fm1wifi_wl_wr(unsigned n, uint32_t v);
uint32_t fm1wifi_wl_anl_rd(unsigned n);
void fm1wifi_wl_anl_wr(unsigned n, uint32_t v);
uint32_t fm1wifi_wf_rd(unsigned n);
void fm1wifi_wf_wr(unsigned n, uint32_t v);
uint32_t fm1wifi_wl30_rd(uint32_t off);        /* the WL30 host block, off 0x00, 0x04, 0x10-0x1C */
void fm1wifi_wl30_wr(uint32_t off, uint32_t v);

/* Power-up, as JieLi's wf_clk_rst_init then wdma_init (docs/WIFI.md "Stage 2 bring-up"): the WiFi clock bits,
 * WF_CON0-4 and WF_CON7 (clock enables, then the resets released one by one with 1 us between and 50 us
 * after), and the WL30 host block's DMA set-up. The values it replaces are kept, so power-down puts them back.
 * About 55 us, no interrupts, no polling. FM1WIFI_OK, or FM1WIFI_ESTATE when already up. */
int fm1wifi_power_up(void);
/* Back to how power-up found it: the WL30 host block, WF_CON7, WF_CON3, WF_CON1-4, WF_CON0 (the core held in
 * reset again, if it was before), then the clock bits. Does nothing when not up. */
void fm1wifi_power_down(void);
int fm1wifi_is_up(void);

/* The MAC registers (function 1 addresses: MAC_CSR0, MAC_SYS_CTRL, ...), through the core's window. Only once
 * powered up (else FM1WIFI_ESTATE); an address not word-aligned or past the window is FM1WIFI_ERANGE. The two
 * frame FIFO addresses go through the DMA engine instead: FM1WIFI_ENOTYET until stages 4-5. */
int fm1wifi_mac_rd(uint32_t addr, uint32_t *v);
int fm1wifi_mac_wr(uint32_t addr, uint32_t v);

/* A baseband register through BBP_CSR_CFG (rt2800's protocol, as docs/WIFI.md gives it): wait until BUSY is
 * clear, write the request, then BUSY with it, and wait until BUSY clears again; a read's value is then in
 * bits 7-0. Each wait reads at most FM1WIFI_BBP_TRIES times, 1 us apart: FM1WIFI_ETIMEOUT, never a hang. */
#define FM1WIFI_BBP_TRIES 1000u
int fm1wifi_bbp_rd(uint8_t reg, uint8_t *v);
int fm1wifi_bbp_wr(uint8_t reg, uint8_t v);

/* Stage 2's first contact: power up (if not up yet) and read MAC_CSR0 (the ASIC version), MAC_SYS_CTRL and
 * BBP register 0. err is the first error; EBUS when MAC_CSR0 reads 0 or all ones (no core answered). */
typedef struct {
    int err;              /* FM1WIFI_OK or the first error */
    int bbp_err;          /* the BBP read's own result */
    uint32_t mac_csr0, mac_sys_ctrl;
    uint8_t bbp0;
} fm1wifi_diag;
int fm1wifi_diag_read(fm1wifi_diag *d);

/* The RAM pattern check (stage 2, docs/WIFI.md: fm1-base saw the radio write somewhere into its RAM): fill a
 * free region with a pattern that depends on each word's address before power-up, then count the bytes that
 * changed and the first one's address. p and n are rounded inward to whole words. */
typedef struct {
    uint32_t changed;     /* bytes that differ from the pattern */
    uintptr_t first;      /* the lowest such byte's address (0: none) */
} fm1wifi_ramcheck;
void fm1wifi_pattern_fill(void *p, uint32_t n);
fm1wifi_ramcheck fm1wifi_pattern_check(const void *p, uint32_t n);
