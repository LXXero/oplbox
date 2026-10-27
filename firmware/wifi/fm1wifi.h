/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi: a driver for the WiFi core inside the FM-1's JieLi AC79 (WL82) chip, written to be shared: no
 * OPLBOX code, headers or globals, only this directory and the board's port (fm1wifi_port.h). README.md here
 * has the API and how another firmware links it; docs/WIFI.md the hardware and the plan.
 *
 * Stage 2: power the core up and down, read and write its MAC registers and its baseband (BBP) registers, a
 * diagnostic snapshot, and a RAM pattern check (run on xero's FM-1 on 2026-10-26: MAC_CSR0 00300100).
 * Stage 3: the radio's power-up and calibration (docs/WIFI_STAGE3.md), a channel, the TX power, the receiver's
 * ADC peak, a channel survey (the false CCA count and busy time JieLi's channel select reads). Its first run on an FM-1 (2026-10-26) crashed (docs/WIFI_STAGE3.md "First run on an FM-1").
 * Stages 4-6 (docs/WIFI_STAGE4.md): the MAC's defaults and address, the frame FIFOs (one received frame, one frame
 * sent), a passive or active scan of one channel, and joining an open network as a station; the frames themselves
 * are fm1wifi_frame.h's. Not run on an FM-1 yet.
 */
#pragma once
#include <stdint.h>
#include "fm1wifi_regs.h"

/* OK; ENOTYET: not built yet; ERANGE: an argument out of range, nothing done; EBUS: the core answered nothing a
 * working one would (reads of 0 or all ones); ETIMEOUT: a bounded wait ran out; ESTATE: the core (or the radio, or
 * the link) is not up (its window is never touched then); EFRAME: a frame the FIFO gave does not hold together
 * (a length word of 0 or past 4088, a descriptor longer than the frame). */
enum { FM1WIFI_OK = 0, FM1WIFI_ENOTYET = -1, FM1WIFI_ERANGE = -2, FM1WIFI_EBUS = -3, FM1WIFI_ETIMEOUT = -4,
       FM1WIFI_ESTATE = -5, FM1WIFI_EFRAME = -6 };
const char *fm1wifi_strerror(int err);   /* "OK", "NOT YET", "RANGE", "NO ANSWER", "TIMEOUT", "OFF", "BAD FRAME" */

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
 * frame FIFO addresses go through the DMA engine instead (fm1wifi_rx_read, fm1wifi_tx_send): FM1WIFI_ENOTYET here. */
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

/* ---- stage 3: the radio (docs/WIFI_STAGE3.md) */

/* The calibration the radio starts from. On a stock FM-1 these would sit in M-VAVE's settings store (docs/WIFI.md
 * "Where the tuning values come from"); fm1wifi_cal_default is the SDK board's / fm1-base's values. */
typedef struct {
    uint8_t xosc[2];          /* the crystal's load caps (WLA_CON12): 11, 11 */
    uint8_t pa[7];            /* PA trim: 1, 7, 4, 7, 11, 1, 7 */
    uint8_t mcs_gain[20];     /* per-MCS digital gains, 0 = the built-in one; the SDK's cfg_tool.bin values */
} fm1wifi_cal;
extern const fm1wifi_cal fm1wifi_cal_default;

/* What the bring-up found, for the diagnostic page. */
typedef struct {
    int err;                  /* FM1WIFI_OK or the first error (TIMEOUT: a bounded wait ran out) */
    const char *stage;        /* where it stopped: "PHY", "PLL", "TRIM", "RF", "TABLE", or "DONE" */
    uint32_t mac_sys_ctrl;    /* MAC_SYS_CTRL once the MAC and BBP resets are released (0000000C expected) */
    uint8_t bbp0;             /* BBP register 0 then */
    uint8_t pll_mid, pll_bank;   /* the PLL scan's middle bank, and the bank it set for the reference divider */
    uint8_t bw[3];            /* the RX filter tune's codes for 6, 10 and 20 MHz */
    uint8_t txlo[2];          /* TX trim: LO leakage I, Q (0-127) */
    int8_t tx_dc[2];          /* TX trim: DC I, Q */
    int8_t tx_gain;           /* TX trim: gain imbalance (< 0: gain I lowered, else gain Q) */
    int8_t tx_phase;          /* TX trim: the phase, on Q when tx_gain >= 0, else on I */
    uint16_t rx_dc[4][2];     /* RX DC trim: the analog DC codes I, Q per gain range (0x200 | value: positive) */
    int8_t rx_gain, rx_phase; /* RX IQ trim: as tx_gain, tx_phase */
    uint8_t vga[3];           /* the VGA gain each trim's ADC search settled on (TX LO, TX DC, RX IQ); 36 = its limit */
    uint8_t vga_peak[3];      /* the ADC peak each search read last (at that gain): above 47 ends a search early */
    uint8_t channel, power;
    uint32_t got;             /* FM1WIFI_GOT_*: the values above that were read; one whose bit is clear was never reached */
} fm1wifi_radio;
#define FM1WIFI_GOT_BW(k)     (1u << (k))          /* bw[k], k 0-2 */
#define FM1WIFI_GOT_TXLO_I    (1u << 3)
#define FM1WIFI_GOT_TXLO_Q    (1u << 4)
#define FM1WIFI_GOT_TXDC_I    (1u << 5)
#define FM1WIFI_GOT_TXDC_Q    (1u << 6)
#define FM1WIFI_GOT_TX_GAIN   (1u << 7)
#define FM1WIFI_GOT_TX_PHASE  (1u << 8)
#define FM1WIFI_GOT_RXDC(g, k) (1u << ((k) ? 16u + (g) : 9u + (g)))   /* rx_dc[g][k], g 0-3, k 0 I, 1 Q */
#define FM1WIFI_GOT_RX_GAIN   (1u << 13)
#define FM1WIFI_GOT_RX_PHASE  (1u << 14)
#define FM1WIFI_GOT_CHANNEL   (1u << 15)           /* channel and power set */
#define FM1WIFI_GOT_VGA(k)    (1u << (20u + (k)))  /* vga[k] and vga_peak[k], k 0-2 */
#define FM1WIFI_GOT_ALL       0x7FFFFFu            /* every bit above */

/* Power up (stage 2, if not up yet), then the radio: the MAC/PHY reset and tables, the PLL bank scan, a fresh trim
 * (TX LO and IQ, RX DC, RX IQ), the PA configuration, the RF RAM table, channel 6 and TX power 6, as JieLi's
 * wfhw_init does them. About a second; the trim part (about half of it) runs from RAM with the port told
 * (fm1wifi_port_trim) and must have the sound off: the caller stops it first. Every wait is bounded:
 * FM1WIFI_ETIMEOUT, never a hang. r gets what it found, whatever the result. */
int fm1wifi_radio_up(const fm1wifi_cal *cal, fm1wifi_radio *r);
int fm1wifi_radio_is_up(void);
/* Channel 1-13 (WF_CON7 bits 7-0 = channel + 101), TX power 0-6 (bits 23-16). FM1WIFI_ESTATE before
 * fm1wifi_radio_up, FM1WIFI_ERANGE outside those. */
int fm1wifi_set_channel(unsigned ch);
int fm1wifi_set_power(unsigned level);
/* The receiver's ADC peak (0-255, 1 ms): what rd_adc_peak reads, a relative level, not dBm. JieLi reads it only inside
 * the trim (adc_pp_tune, from txiq_tune_soft and rxiq_tune_soft), with the PHY in loopback and the RX front end set
 * for the trim; read after fm1wifi_radio_up it is outside any state JieLi reads it in (docs/WIFI_STAGE3.md "ADC
 * peak 0 on every channel"). */
int fm1wifi_adc_peak(uint8_t *peak);
/* A diagnostic of the receive state after fm1wifi_radio_up, on the current channel: the registers the trim sets
 * differently from the receive state, and the ADC peak at VGA gain 0 (where the bring-up leaves it) and at 41 (the
 * table's highest). Leaves the VGA gain at 0 again. FM1WIFI_ESTATE before fm1wifi_radio_up. */
typedef struct {
    uint8_t peak, peak_vga41; /* the ADC peak (second of two readings, as adc_pp_tune counts) at VGA gain 0 and 41 */
    uint8_t idle80;           /* RX front end 0x80 before the peak's start (RX front end 23 = 2) */
    uint8_t rxfe23;           /* RX front end 23 read back with the peak started (0x82 written) */
    uint8_t rxfe0, rxfe19, rxfe20;   /* the RX front end set-up the trim changes (trim: 62, 30, 52 or 4) */
    uint8_t bbp25;            /* BBP 25: 0x18 is the PHY's loopback (the trim), 0 its receive state */
} fm1wifi_rxprobe;
int fm1wifi_rx_probe(fm1wifi_rxprobe *p);

/* The receive level on channel ch (1-13) as JieLi's own channel survey measures it: the channel select of its AP
 * code (wl_wifi.a: APAutoSelectChannel, then UpdateChannelInfo; docs/WIFI_STAGE3.md "Receive level per channel").
 * Sets the channel as RT30xx_ChipSwitchChannel does (WF_CON7 bits 7-0 = ch + 101, TX_BAND_CFG to 2.4 GHz,
 * TX_PIN_CFG = 0x00050302: one PA, the LNAs, the RF and T/R switch controls on), reads both counters once to clear
 * them, waits ms milliseconds (1-1000; JieLi waits FM1WIFI_SURVEY_MS) and reads them again. Counts, not dBm: more
 * means more energy on the channel. Leaves the radio on ch. FM1WIFI_ESTATE before fm1wifi_radio_up, FM1WIFI_ERANGE
 * for ch or ms out of range. */
#define FM1WIFI_SURVEY_MS 200u    /* wifi_ap_auto_channel_wait_time() */
typedef struct {
    uint32_t false_cca;       /* RX_STA_CNT1 bits 15-0: the false CCA count, what JieLi's channel select ranks by */
    uint32_t busy;            /* CH_BUSY_STA: the channel busy time (rt2800: microseconds); JieLi reads it, uses no value */
} fm1wifi_survey;
int fm1wifi_channel_survey(unsigned ch, unsigned ms, fm1wifi_survey *s);

/* ---- stages 4-6: the frames (docs/WIFI_STAGE4.md; the formats and the station are fm1wifi_frame.h's)
 *
 * After fm1wifi_radio_up. Every wait is bounded (FM1WIFI_DMA_TRIES reads 1 us apart for a DMA, FM1WIFI_TXQ_TRIES
 * 10 us apart for room in the TX queue): FM1WIFI_ETIMEOUT, never a hang. The library's clock for the scan and the
 * station is the time it spends in the port's delay. */
#include "fm1wifi_frame.h"
#define FM1WIFI_DMA_TRIES 1000u
#define FM1WIFI_TXQ_TRIES 1000u

/* The link: the MAC's 22 defaults (MACRegTable, as JieLi's NICInitializeAsic writes them after wfhw_init; docs/
 * WIFI_STAGE4.md "The MAC's defaults"), the address mac in MAC_ADDR_DW0/1 (the MAC keeps unicast frames to it and
 * ACKs them), no BSSID, channel ch. FM1WIFI_ESTATE before fm1wifi_radio_up. */
int fm1wifi_link_up(const uint8_t mac[6], unsigned ch);
int fm1wifi_link_is_up(void);
int fm1wifi_set_bssid(const uint8_t bssid[6]);      /* MAC_BSSID_DW0/1 (0: all zero) */
/* Channel 1-13 as the survey switches it (WF_CON7, TX_BAND_CFG, TX_PIN_CFG; fm1wifi_channel_survey) */
int fm1wifi_tune(unsigned ch);

/* 1 when a received frame waits in the RX FIFO (the WL30 host block's 0x04 bit 17, what JieLi's receive handler
 * tests), 0 when not or the link is not up. */
int fm1wifi_rx_ready(void);
/* Read one frame from the RX FIFO into buf (word-aligned, cap >= FM1WIFI_RX_BUF bytes): JieLi's register 0x0324 = 2,
 * a 4-byte DMA for the length word N, then a DMA of N + 4 (the frame and its trailer); the length word and the
 * trailer are put into CPU order. *n gets N + 8, what fm1wifi_rx_parse takes. FM1WIFI_EFRAME for a length word of 0
 * or past FM1WIFI_RX_LEN_MAX (the frame is then not read: the FIFO's state is unknown); ETIMEOUT for a DMA that does
 * not finish. */
int fm1wifi_rx_read(uint8_t *buf, uint32_t cap, uint32_t *n);
/* Send the image fm1wifi_tx_image built (img word-aligned, dma_len its return value): wait for room in the TX queue
 * as JieLi's usb_submit_urb does (docs/WIFI_STAGE4.md "The TX FIFO"), then the DMA from img + 4. */
int fm1wifi_tx_send(uint8_t *img, uint32_t dma_len);

/* Evidence for the WiFi page: the WL30 host block's 0x04 and TXRXQ_PCNT as they read now, and the statistics
 * counters (which clear when read) added up since the last fm1wifi_counters_clear. */
typedef struct {
    uint32_t host04, txrxq;
    uint32_t crc_err, phy_err, rx_dup, rx_overflow;    /* RX_STA_CNT0, RX_STA_CNT2 */
    uint32_t tx_fail, tx_ok, tx_retry, tx_underflow;   /* TX_STA_CNT0, CNT1, CNT2 */
} fm1wifi_counters;
void fm1wifi_counters_clear(fm1wifi_counters *c);  /* reads the counters once to clear them */
int fm1wifi_counters_add(fm1wifi_counters *c);

/* What the frame functions below saw: counts, the first frames' first FM1WIFI_RAW_LEN bytes as the FIFO gave them
 * (length word, RXWI, the frame, the trailer; a beacon fits whole), whatever they parse to. Clear it (all zero) before
 * the first call. */
#define FM1WIFI_RAW_N 4u
#define FM1WIFI_RAW_LEN 512u
typedef struct {
    uint32_t frames;          /* frames read from the FIFO */
    uint32_t bad;             /* of those, ones fm1wifi_rx_parse refused */
    uint32_t beacons, probe_resps, to_us;   /* beacons, probe responses, frames for our address */
    uint32_t tx, tx_err;      /* frames sent; sends that failed */
    int rx_err, tx_errno;     /* the first receive / send error (FM1WIFI_OK: none) */
    uint8_t n_raw;
    uint8_t raw_ch[FM1WIFI_RAW_N];
    uint16_t raw_n[FM1WIFI_RAW_N];   /* bytes the frame had (N + 8) */
    uint8_t raw[FM1WIFI_RAW_N][FM1WIFI_RAW_LEN];
} fm1wifi_frames;

/* One channel of a scan: tune to ch, for an active scan send a probe request for any network (and one for ssid, if
 * given: a hidden network answers only that), then listen for ms milliseconds (1-1000), every beacon and probe
 * response into l. FM1WIFI_OK, or the first error (a failed probe request does not stop the listening). */
int fm1wifi_scan_channel(unsigned ch, unsigned ms, int active, const char *ssid, fm1wifi_scan_list *l, fm1wifi_frames *fr);
/* Join b as the station s (fm1wifi_sta_start): tune to b's channel, set the BSSID, then run the station until it is
 * UP or DOWN, at most ms milliseconds (still AUTH or ASSOC then). FM1WIFI_OK unless the hardware failed. */
int fm1wifi_join(fm1wifi_sta *s, const fm1wifi_bss *b, unsigned ms, fm1wifi_frames *fr);
/* Stay joined for ms milliseconds: the station counts the BSS's beacons and goes DOWN on a deauthentication,
 * a disassociation or a lost beacon. */
int fm1wifi_listen(fm1wifi_sta *s, unsigned ms, fm1wifi_frames *fr);
/* Leave the BSS: the deauthentication frame, the station DOWN (fm1wifi_sta_leave). */
int fm1wifi_leave(fm1wifi_sta *s, fm1wifi_frames *fr);
/* The address the link uses (fm1wifi_link_up's mac). */
const uint8_t *fm1wifi_link_mac(void);

/* ---- where a crash happened: the step the library is in and its last register write
 *
 * fm1wifi_power_up, _power_down, _diag_read, _radio_up, _set_channel, _set_power, _adc_peak, _rx_probe,
 * _channel_survey, _link_up, _set_bssid, _tune, _rx_read, _tx_send and _counters_* set fm1wifi_where
 * before each of their steps (step, then magic) and clear magic when they return; every CPU-side register write
 * the library makes (the radio's analog controls, JL_WL, the core's window) records its address and value just
 * before it is made. So a crash handler that finds magic ==
 * FM1WIFI_WHERE_MAGIC knows the crash happened inside the library, in that step, after (or at) that write.
 * It lives in section .noinit (FM1WIFI_NOINIT): the linker script must place that in RAM and leave it out of the
 * start-up zeroing; it then also survives the reboot after the crash. So it holds what the last run left: call
 * fm1wifi_where_clear() once at start, before the first crash that could read it; otherwise only read it. */
#ifndef FM1WIFI_NOINIT
#define FM1WIFI_NOINIT __attribute__((section(".noinit")))
#endif
#define FM1WIFI_WHERE_MAGIC 0x57494649u   /* "WIFI" */
typedef struct {
    uint32_t magic;           /* FM1WIFI_WHERE_MAGIC while a call is running */
    uint32_t step;            /* FM1WIFI_STEP_* */
    uint32_t addr, val;       /* the last CPU-side register write: address and value (0, 0: none yet in this call) */
} fm1wifi_where_t;
extern volatile fm1wifi_where_t fm1wifi_where;
void fm1wifi_where_clear(void);
/* The steps, in fm1wifi_radio_up's order (docs/WIFI_STAGE3.md "The sequence"); stage 2's and the later calls first. */
enum {
    FM1WIFI_STEP_NONE, FM1WIFI_STEP_POWER_UP, FM1WIFI_STEP_DIAG, FM1WIFI_STEP_POWER_DOWN, FM1WIFI_STEP_PHY_MAC_RST,
    FM1WIFI_STEP_SIFS, FM1WIFI_STEP_MAC_0308, FM1WIFI_STEP_CFR0, FM1WIFI_STEP_PHY_INIT_A, FM1WIFI_STEP_AGC,
    FM1WIFI_STEP_PHY_INIT_B, FM1WIFI_STEP_RF_INIT, FM1WIFI_STEP_PLL_SCAN, FM1WIFI_STEP_RF_PLL,
    FM1WIFI_STEP_FILTER_TUNE, FM1WIFI_STEP_TRIM_TX, FM1WIFI_STEP_TX_START, FM1WIFI_STEP_TX_LO_I, FM1WIFI_STEP_TX_LO_Q,
    FM1WIFI_STEP_TX_DC_I, FM1WIFI_STEP_TX_DC_Q, FM1WIFI_STEP_TX_GAIN, FM1WIFI_STEP_TX_PHASE, FM1WIFI_STEP_TRIM_RXDC,
    FM1WIFI_STEP_RXDC, FM1WIFI_STEP_TRIM_RXIQ, FM1WIFI_STEP_RXIQ_START, FM1WIFI_STEP_RX_GAIN, FM1WIFI_STEP_RX_PHASE,
    FM1WIFI_STEP_RF_DONE, FM1WIFI_STEP_PA, FM1WIFI_STEP_RF_GAIN, FM1WIFI_STEP_VGA0, FM1WIFI_STEP_PHY_INIT_A2,
    FM1WIFI_STEP_AGC2, FM1WIFI_STEP_PHY_INIT_B2, FM1WIFI_STEP_CFR1, FM1WIFI_STEP_RAPP, FM1WIFI_STEP_RAM_TABLE,
    FM1WIFI_STEP_BSB_AUTO, FM1WIFI_STEP_CHANNEL6, FM1WIFI_STEP_MCS_GAIN, FM1WIFI_STEP_SET_CHANNEL,
    FM1WIFI_STEP_SET_POWER, FM1WIFI_STEP_ADC_PEAK, FM1WIFI_STEP_RX_PROBE, FM1WIFI_STEP_SURVEY,
    FM1WIFI_STEP_LINK_UP, FM1WIFI_STEP_SET_BSSID, FM1WIFI_STEP_TUNE, FM1WIFI_STEP_RX_READ, FM1WIFI_STEP_TX_SEND,
    FM1WIFI_STEP_COUNTERS, FM1WIFI_STEP_N
};
const char *fm1wifi_step_name(uint32_t step);   /* "RF_INIT", "TX LO I", ...: at most 18 characters; "?" past the list */
