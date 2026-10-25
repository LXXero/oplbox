/* SPDX-License-Identifier: GPL-3.0-only
 * FM-1 WiFi register map (docs/WIFI.md stage 1, header part). Addresses and bit masks only: no register
 * values, no code. Nothing here has been tried on an FM-1.
 *
 * Provenance, per block:
 *  - CPU side, JL_ANA / JL_WL and IRQ 52: the JieLi AC79 SDK's C headers (Apache-2.0),
 *    include_lib/driver/cpu/wl82/asm/WL82.h (JL_ANA_TypeDef, JL_WL_TypeDef, lsfr_base, map_adr) and
 *    asm/hwi.h (IRQ_WF_IDX), SDK commit d179b448 (tag AC79NN_SDK_V1.2.1_2023-12-13). The member order is
 *    turned into offsets here; the register names are the SDK's.
 *  - CPU side, the internal SDIO host at 0x30F00: not in any SDK header. The six addresses are the MMIO
 *    pointers in wl_rf_common.a's wl_sdio.c.o (docs/WIFI.md "CPU-side registers", reproduced in
 *    docs/WIFI_REVIEW.md). Addresses are facts, not code; no code from the archive is used. The SDK names
 *    neither the block nor its registers, so they are numbered by offset [no standard term].
 *  - WiFi core (SDIO function 1), MAC: names, addresses and masks from Linux
 *    drivers/net/wireless/ralink/rt2x00/rt2800.h at v6.6 (SPDX GPL-2.0-or-later, combined here under
 *    GPL-3.0; Copyright (C) 2004 - 2010 Ivo van Doorn, (C) 2010 Willow Garage, (C) 2009 Alban Browaeys,
 *    Felix Fietkau, Luis Correia, Mattias Nissler, Mark Asselstine, Xose Vazquez Perez, Bart
 *    Zolnierkiewicz; http://rt2x00.serialmonkey.com). The register values (MACRegTable) are deliberately
 *    not here: rt2800.h does not supply them (docs/WIFI_REVIEW.md), stage 5 takes them from rt2800lib.c or
 *    Ralink's staging driver.
 *  - WiFi core, JieLi's own registers (0x0300 range, the frame data addresses) and the BBP windows: the
 *    addresses docs/WIFI.md read from wl_rf_common.a. rt2800.h has no name for them.
 *  - The internal SDIO bus's command indexes, R4 fields and CCCR addresses: the SD Association's SDIO
 *    Simplified Specification; the CCCR addresses cross-checked against Linux include/linux/mmc/sdio.h v6.6
 *    (facts only). The values wl30_sdio_enum writes: docs/WIFI.md.
 * See LICENSING.md / CREDITS.md. tests/wifi_regs.py checks every address here against docs/WIFI.md.
 */
#pragma once
#include <stdint.h>

/* ---- CPU side, memory-mapped (WL82.h: lsfr_base 0x10000, map_adr(grp, adr) = (64*grp + adr)*4) ---- */

#define FM1_LSFR_BASE 0x10000u
#define FM1_MAP_ADR(grp, adr) ((64u * (grp) + (adr)) * 4u)

/* JL_ANA (lsfr_base + map_adr(0x19, 0)): the radio's analog controls. WLA_CON0..WLA_CON30 are read/write,
 * WLA_CON31..WLA_CON39 read-only. PLL_CON*, DAA_CON*, ADA_CON* that follow belong to the audio and the
 * system PLL, not WiFi, and are left out. */
#define FM1_JL_ANA_BASE (FM1_LSFR_BASE + FM1_MAP_ADR(0x19u, 0u)) /* 0x11900 */
#define FM1_WLA_CON_N 40u
#define FM1_WLA_CON_RO_FIRST 31u
#define FM1_WLA_CON(n) (FM1_JL_ANA_BASE + 4u * (n)) /* WLA_CON0..39 */

/* JL_WL (lsfr_base + map_adr(0x40, 0)): WiFi clock, reset, power, interrupt and the RF digital block's
 * indirect port. Offsets follow JL_WL_TypeDef's member order. */
#define FM1_JL_WL_BASE (FM1_LSFR_BASE + FM1_MAP_ADR(0x40u, 0u)) /* 0x14000 */
#define FM1_WL_CON(n) (FM1_JL_WL_BASE + 4u * (n))                /* WL_CON0..7, 0x14000-0x1401C */
#define FM1_WL_LOFC_CON (FM1_JL_WL_BASE + 0x20u)
#define FM1_WL_LOFC_RES (FM1_JL_WL_BASE + 0x24u)
#define FM1_WL_ANL_CON(n) (FM1_JL_WL_BASE + 0x28u + 4u * (n))    /* WL_ANL_CON0..3, 0x14028-0x14034 */
#define FM1_WF_CON(n) (FM1_JL_WL_BASE + 0x40u + 4u * (n))        /* WF_CON0..9, 0x14040-0x14064 */
#define FM1_WL_SD_CON(n) (FM1_JL_WL_BASE + 0x80u + 4u * (n))     /* SD_CON0..2; not on the WiFi path */
#define FM1_WL_SD_CTU_CON (FM1_JL_WL_BASE + 0x8Cu)
#define FM1_WL_SD_CTU_CNT (FM1_JL_WL_BASE + 0x90u)               /* write-only */
#define FM1_WL_SD_CPTR (FM1_JL_WL_BASE + 0x94u)                  /* write-only */
#define FM1_WL_SD_DPTR (FM1_JL_WL_BASE + 0x98u)                  /* write-only */
#define FM1_WL_CON_N 8u
#define FM1_WL_ANL_CON_N 4u
#define FM1_WF_CON_N 10u

/* The internal SDIO host between the CPU and the WiFi core ("WL30" in JieLi's names) [no standard term for
 * its registers]. wl30_mmc_io_rw_extended uses 0x04 and 0x10-0x1C, wdma_init 0x00 and 0x04,
 * sdio_recv_int_hdl 0x04. What each bit does is stage 2's to find. */
#define FM1_WL30_HOST_BASE 0x30F00u
#define FM1_WL30_HOST_LAST 0x30F1Cu
#define FM1_WL30_HOST(off) (FM1_WL30_HOST_BASE + (off))
#define FM1_WL30_HOST_00 FM1_WL30_HOST(0x00u)
#define FM1_WL30_HOST_04 FM1_WL30_HOST(0x04u)
#define FM1_WL30_HOST_10 FM1_WL30_HOST(0x10u)
#define FM1_WL30_HOST_14 FM1_WL30_HOST(0x14u)
#define FM1_WL30_HOST_18 FM1_WL30_HOST(0x18u)
#define FM1_WL30_HOST_1C FM1_WL30_HOST(0x1Cu)

/* IRQ_WF_IDX (hwi.h). It must run below the audio's ALNK0 and only set a flag (docs/WIFI.md "OPLBOX's
 * constraints"). */
#define FM1_IRQ_WF 52u

/* ---- WiFi core, SDIO function 1 addresses (not CPU addresses: reach them with fm1_wifi_mac_rd/wr) ---- */

#define FM1_WIFI_SDIO_FN 1u

/* rt2800.h names, addresses and masks (Linux v6.6). Only the registers docs/WIFI.md lists as used. */
#define MAC_CSR0 0x1000u
#define MAC_CSR0_REVISION 0x0000ffffu
#define MAC_CSR0_CHIPSET 0xffff0000u

#define MAC_SYS_CTRL 0x1004u
#define MAC_SYS_CTRL_RESET_CSR 0x00000001u
#define MAC_SYS_CTRL_RESET_BBP 0x00000002u
#define MAC_SYS_CTRL_ENABLE_TX 0x00000004u
#define MAC_SYS_CTRL_ENABLE_RX 0x00000008u
#define MAC_SYS_CTRL_CONTINUOUS_TX 0x00000010u
#define MAC_SYS_CTRL_LOOPBACK 0x00000020u
#define MAC_SYS_CTRL_WLAN_HALT 0x00000040u
#define MAC_SYS_CTRL_RX_TIMESTAMP 0x00000080u

#define MAC_ADDR_DW0 0x1008u
#define MAC_ADDR_DW1 0x100cu
#define MAX_LEN_CFG 0x1018u

#define BBP_CSR_CFG 0x101cu
#define BBP_CSR_CFG_VALUE 0x000000ffu
#define BBP_CSR_CFG_REGNUM 0x0000ff00u
#define BBP_CSR_CFG_READ_CONTROL 0x00010000u
#define BBP_CSR_CFG_BUSY 0x00020000u
#define BBP_CSR_CFG_BBP_PAR_DUR 0x00040000u
#define BBP_CSR_CFG_BBP_RW_MODE 0x00080000u

#define PBF_CFG 0x0408u
#define PBF_MAX_PCNT 0x040cu
#define BCN_OFFSET0 0x042cu
#define BCN_OFFSET1 0x0430u
#define TXRXQ_PCNT 0x0438u
#define TXRXQ_PCNT_TX0Q 0x000000ffu
#define TXRXQ_PCNT_TX1Q 0x0000ff00u
#define TXRXQ_PCNT_TX2Q 0x00ff0000u
#define TXRXQ_PCNT_RX0Q 0xff000000u
#define PBF_DBG 0x043cu

#define XIFS_TIME_CFG 0x1100u
#define BKOFF_SLOT_CFG 0x1104u
#define PWR_PIN_CFG 0x1204u
#define TX_SW_CFG0 0x1330u
#define TX_SW_CFG1 0x1334u
#define TX_SW_CFG2 0x1338u
#define TXOP_CTRL_CFG 0x1340u
#define TX_RTS_CFG 0x1344u
#define TX_TIMEOUT_CFG 0x1348u
#define TX_RTY_CFG 0x134cu
#define TX_LINK_CFG 0x1350u
#define CCK_PROT_CFG 0x1364u
#define OFDM_PROT_CFG 0x1368u
#define MM20_PROT_CFG 0x136cu
#define MM40_PROT_CFG 0x1370u
#define GF20_PROT_CFG 0x1374u
#define GF40_PROT_CFG 0x1378u
#define EXP_ACK_TIME 0x1380u

#define RX_FILTER_CFG 0x1400u
#define RX_FILTER_CFG_DROP_CRC_ERROR 0x00000001u
#define RX_FILTER_CFG_DROP_PHY_ERROR 0x00000002u
#define RX_FILTER_CFG_DROP_NOT_TO_ME 0x00000004u
#define RX_FILTER_CFG_DROP_NOT_MY_BSSD 0x00000008u
#define RX_FILTER_CFG_DROP_VER_ERROR 0x00000010u
#define RX_FILTER_CFG_DROP_MULTICAST 0x00000020u
#define RX_FILTER_CFG_DROP_BROADCAST 0x00000040u
#define RX_FILTER_CFG_DROP_DUPLICATE 0x00000080u
#define RX_FILTER_CFG_DROP_CF_END_ACK 0x00000100u
#define RX_FILTER_CFG_DROP_CF_END 0x00000200u
#define RX_FILTER_CFG_DROP_ACK 0x00000400u
#define RX_FILTER_CFG_DROP_CTS 0x00000800u
#define RX_FILTER_CFG_DROP_RTS 0x00001000u
#define RX_FILTER_CFG_DROP_PSPOLL 0x00002000u
#define RX_FILTER_CFG_DROP_BA 0x00004000u
#define RX_FILTER_CFG_DROP_BAR 0x00008000u
#define RX_FILTER_CFG_DROP_CNTL 0x00010000u
#define AUTO_RSP_CFG 0x1404u
#define LEGACY_BASIC_RATE 0x1408u
#define HT_BASIC_RATE 0x140cu

#define TX_SEC_CNT0 0x1500u
#define RX_SEC_CNT0 0x1504u
#define TXOP_HLDR_ET 0x1608u

/* The hardware key tables (rt2800.h: 32-byte x 256, 8-byte x 256, 4-byte x 256, 32-byte x 16, 4-byte x 16).
 * The entry sizes are sizeof(struct hw_key_entry) 32, sizeof(struct mac_iveiv_entry) 8 and sizeof(u32). */
#define PAIRWISE_KEY_TABLE_BASE 0x4000u
#define MAC_IVEIV_TABLE_BASE 0x6000u
#define MAC_WCID_ATTRIBUTE_BASE 0x6800u
#define SHARED_KEY_TABLE_BASE 0x6c00u
#define SHARED_KEY_MODE_BASE 0x7000u
#define PAIRWISE_KEY_ENTRY(i) (PAIRWISE_KEY_TABLE_BASE + 32u * (i))
#define MAC_IVEIV_ENTRY(i) (MAC_IVEIV_TABLE_BASE + 8u * (i))
#define MAC_WCID_ATTR_ENTRY(i) (MAC_WCID_ATTRIBUTE_BASE + 4u * (i))
#define SHARED_KEY_ENTRY(i) (SHARED_KEY_TABLE_BASE + 32u * (i))
#define SHARED_KEY_MODE_ENTRY(i) (SHARED_KEY_MODE_BASE + 4u * (i))
#define MAC_WCID_ATTRIBUTE_KEYTAB 0x00000001u
#define MAC_WCID_ATTRIBUTE_CIPHER 0x0000000eu
#define MAC_WCID_ATTRIBUTE_BSS_IDX 0x00000070u

/* JieLi's own WiFi-core addresses (docs/WIFI.md); no rt2800.h name, so named by address [no standard term].
 * 0x0434 is read with TXRXQ_PCNT in get_txrxq_pcnt; 0x0308, 0x030C, 0x0324, 0x0328 are written in
 * wfhw_init / wf_phy_mac_rst / sdio_recv_int_hdl / wfhw_iqr_init. */
#define FM1_WF_REG_0308 0x0308u
#define FM1_WF_REG_030C 0x030Cu
#define FM1_WF_REG_0324 0x0324u
#define FM1_WF_REG_0328 0x0328u
#define FM1_WF_REG_0434 0x0434u
/* Frame data: TX frames are written to function 1 address 0, RX frames read from address 12. */
#define FM1_WF_TX_FIFO 0x0000u
#define FM1_WF_RX_FIFO 0x000Cu

/* The BBP indirect windows, BBP register numbers reached through BBP_CSR_CFG (docs/WIFI.md) [no standard
 * term]: write the address register, then the data register, then 1 to the strobe register. */
#define FM1_BBP_CCA_STROBE 0xC8u      /* cca_reg_set */
#define FM1_BBP_CCA_ADDR 0xC9u
#define FM1_BBP_CCA_DATA 0xCAu
#define FM1_BBP_RX11N_STROBE 0xCCu    /* rx11n_reg_set */
#define FM1_BBP_RX11N_ADDR 0xCDu
#define FM1_BBP_RX11N_DATA 0xCEu
#define FM1_BBP_TXBE_STROBE 0xD0u     /* tx_backend_wr */
#define FM1_BBP_TXBE_ADDR 0xD1u
#define FM1_BBP_TXBE_DATA 0xD2u
#define FM1_BBP_RXFE_STROBE 0xD4u     /* rx_frontend_wr */
#define FM1_BBP_RXFE_ADDR 0xD5u
#define FM1_BBP_RXFE_DATA 0xD6u

/* ---- the internal SDIO bus, as the SDIO Simplified Specification defines it (docs/WIFI.md "Internal SDIO
 * enumeration"). Command indexes, the R4 response's fields and the CCCR addresses are the specification's;
 * the CCCR addresses were cross-checked against Linux include/linux/mmc/sdio.h v6.6 (facts only, no code). ---- */

#define FM1_SDIO_CMD3 3u   /* SEND_RELATIVE_ADDR, R6: RCA in bits 31-16 */
#define FM1_SDIO_CMD5 5u   /* IO_SEND_OP_COND, R4 */
#define FM1_SDIO_CMD7 7u   /* SELECT/DESELECT_CARD, argument RCA << 16 */
#define FM1_SDIO_CMD52 52u /* IO_RW_DIRECT */
#define FM1_SDIO_CMD53 53u /* IO_RW_EXTENDED */
#define FM1_SDIO_ADDR_MAX 0x1FFFFu  /* register address: 17 bits */
#define FM1_SDIO_FN_MAX 7u

#define FM1_SDIO_R4_READY 0x80000000u   /* C: initialisation done */
#define FM1_SDIO_R4_NFN(r4) (((r4) >> 28) & 7u) /* number of I/O functions */
#define FM1_SDIO_R4_MEM 0x08000000u     /* memory present */
#define FM1_SDIO_R4_OCR 0x00FFFFFFu

/* The CCCR: function 0's registers */
#define FM1_SDIO_CCCR_REV 0x00u       /* CCCR/SDIO revision, read-only */
#define FM1_SDIO_CCCR_SD_REV 0x01u    /* read-only */
#define FM1_SDIO_CCCR_IO_ENABLE 0x02u /* bit n enables function n */
#define FM1_SDIO_CCCR_IO_READY 0x03u  /* read-only: bit n, function n is ready */
#define FM1_SDIO_CCCR_INT_ENABLE 0x04u /* bit 0 master, bit n function n */
#define FM1_SDIO_CCCR_INT_PENDING 0x05u /* read-only */
#define FM1_SDIO_CCCR_ABORT 0x06u
#define FM1_SDIO_CCCR_BUS_IF 0x07u    /* Bus Interface Control: bits 1-0 bus width */
#define FM1_SDIO_BUS_WIDTH_4 0x02u
#define FM1_SDIO_CCCR_CAPS 0x08u      /* Card Capability, read-only */
#define FM1_SDIO_CCCR_CIS 0x09u       /* common CIS pointer, 3 bytes, read-only */
#define FM1_SDIO_CCCR_EXEC 0x0Eu      /* read-only */
#define FM1_SDIO_CCCR_READY 0x0Fu     /* read-only */

/* What wl30_sdio_enum sends (docs/WIFI.md), the values our enumeration uses */
#define FM1_WIFI_CMD5_ARG 0x1F000000u /* as decoded from the IR. The specification defines bits 23-0 (OCR) and
                                       * 24 (S18R) only; bits 25-28 are unexplained [unchecked] */
#define FM1_WIFI_IO_ENABLE 0xFFu      /* JieLi's value; bit 0 is reserved in the specification */
#define FM1_WIFI_INT_ENABLE 0x03u     /* master and function 1 */
#define FM1_WIFI_FN1_INIT_FIRST 0x10u /* function 1 registers 0x10-0x13 are written 0: WiFi-core specific */
#define FM1_WIFI_FN1_INIT_N 4u
#define FM1_WIFI_CMD5_TRIES 100u      /* CMD5 until C is set, at most this often */
#define FM1_WIFI_CMD3_TRIES 4u        /* again while the card offers RCA 0, which the specification reserves */

/* ---- accessors (fm1_wifi_hal.c) ---- */

/* ENOTYET: not built yet. ERANGE: an argument out of range, nothing sent. EBUS: the card answered with an
 * error or not at all. ETIMEOUT: a bounded wait ran out. */
enum { FM1_WIFI_OK = 0, FM1_WIFI_ENOTYET = -1, FM1_WIFI_ERANGE = -2, FM1_WIFI_EBUS = -3, FM1_WIFI_ETIMEOUT = -4 };

/* CPU-side registers by index; an index past the block returns 0 / does nothing. */
uint32_t fm1_wla_rd(unsigned n);
void fm1_wla_wr(unsigned n, uint32_t v);   /* refuses the read-only WLA_CON31..39 */
uint32_t fm1_wl_rd(unsigned n);
void fm1_wl_wr(unsigned n, uint32_t v);
uint32_t fm1_wl_anl_rd(unsigned n);
void fm1_wl_anl_wr(unsigned n, uint32_t v);
uint32_t fm1_wf_rd(unsigned n);
void fm1_wf_wr(unsigned n, uint32_t v);
uint32_t fm1_wl30_host_rd(uint32_t off);   /* off 0x00..0x1C */
void fm1_wl30_host_wr(uint32_t off, uint32_t v);

/* WiFi core over the internal SDIO bus. fm1_wifi_sdio_cmd sends a command without data (CMD3, CMD5, CMD7)
 * and returns its 32-bit response. Stage 2 writes these three; until then they return FM1_WIFI_ENOTYET and
 * touch nothing. Built with -DFM1_WIFI_SDIO_MODEL the HAL leaves them out and the host's register model
 * (tests/wifi/sdio_model.c) supplies them. */
int fm1_wifi_sdio_cmd(unsigned idx, uint32_t arg, uint32_t *resp);
int fm1_wifi_cmd52(unsigned fn, uint32_t addr, int write, uint8_t *data);
int fm1_wifi_cmd53(unsigned fn, uint32_t addr, int write, void *buf, unsigned len);
int fm1_wifi_mac_rd(uint32_t addr, uint32_t *v);
int fm1_wifi_mac_wr(uint32_t addr, uint32_t v);
/* Enumerate the WiFi core on the internal SDIO bus, as wl30_sdio_enum does (docs/WIFI.md): CMD5 until ready,
 * CMD3, CMD7, then the CCCR and function 1 writes. FM1_WIFI_OK or the first error. */
int fm1_wifi_sdio_enum(void);
