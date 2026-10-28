/* SPDX-License-Identifier: GPL-3.0-only
 * The BLE baseband's registers and RAM layout, as numbers the driver hands to its port (docs/BLUETOOTH_RADIO.md: every
 * value here is a fact from tools/ble_ir.py's reading of btctrler.a, with the section named; the names are ours except
 * the parameter block's, which are the IR's value names). Addresses are numbers only: the access is the port's
 * (firmware/hal/fm1ble_port_bsp.c), so this header is not a register macro for the register check.
 */
#pragma once

/* B: power-up */
#define FM1BLE_WL_CON0          0x14000u
#define FM1BLE_WL_CON1          0x14004u   /* the coexistence command register (B.4) */
#define FM1BLE_BT_CLKCON        0x2FC40u   /* B.1 steps 11, 14, 15 */
#define FM1BLE_BT_PWRCON        0x2FC50u   /* B.2 */
#define FM1BLE_BT_ENA           0x2FC78u   /* B.1 steps 16-17 */
#define FM1BLE_BSFR_BASE        0x20000u   /* bsfr_base, B.1 step 12 */
#define FM1BLE_BSFR_50          0x20050u   /* B.2 */
#define FM1BLE_IOMAP            0x50048u   /* B.2: 0x101 */
#define FM1BLE_WLA_CON9         0x11924u   /* B.3, test paths only: not written here */
#define FM1BLE_WLA_CON20        0x11950u

/* C: the analog and modem block */
#define FM1BLE_ANA(n)           (0x2FC00u + (n))
#define FM1BLE_ANA_PLLTAB       0x2FC88u   /* the PLL table's address (F.2) */
#define FM1BLE_AGC_0            0x2FD80u   /* C.3 */
#define FM1BLE_AGC_RSSI         0x2FD8C
#define FM1BLE_AGC_TAB_AT       0x2FD98u   /* C.3 btagc_ram_init: 0 first, then the gain table's words go to ... */
#define FM1BLE_AGC_TAB          0x2FD9Cu   /* ... this register one after another (fm1ble_agc.h) */

/* D: the baseband control block */
#define FM1BLE_BB_CON0          0x28000u
#define FM1BLE_BB_T08           0x28008u
#define FM1BLE_BB_T0C           0x2800Cu
#define FM1BLE_BB_T10           0x28010u
#define FM1BLE_BB_T14           0x28014u
#define FM1BLE_BB_T18           0x28018u
#define FM1BLE_BB_ANCHOR_CMD    0x2801Cu   /* D.2: index << 10 | hw_id << 4 | 5 (write) or 2 (read) */
#define FM1BLE_BB_ANCHOR_WR     0x28020u   /* the value to write */
#define FM1BLE_BB_ANCHOR_RD     0x28024u   /* the value read */
#define FM1BLE_BB_IRQ_EN        0x28028u   /* D.4: bit id = event done, bit id + 8 = packet received */
#define FM1BLE_BB_IRQ_CLR       0x2802Cu
#define FM1BLE_BB_IRQ_PEND      0x28030u
#define FM1BLE_BB_T34           0x28034u
#define FM1BLE_BB_BUSY          0x28038u   /* bit 1 busy */
#define FM1BLE_BB_RAMWIN_EN     0x2FC80u   /* D.1 step 1 */
#define FM1BLE_BB_RAM_START     0x2FC84u
#define FM1BLE_BB_RAM_END       0x2FCBCu

/* the anchor register file's indexes (D.2) and the state codes (D.3) */
#define FM1BLE_AX_CNT_LO        0
#define FM1BLE_AX_INTERVAL      1
#define FM1BLE_AX_STATE         2
#define FM1BLE_AX_WINOFF        4
#define FM1BLE_AX_CHANNEL       6
#define FM1BLE_AX_BITOFF        7
#define FM1BLE_AX_RANDSLOT      8
#define FM1BLE_AX_CNT_HI        0xE
#define FM1BLE_AX_INTERVAL_HI   0xF
#define FM1BLE_STATE_ADV        2

/* E.1: the RAM block. Offsets from its start S; every pointer the hardware reads is a 16-bit offset into it. */
#define FM1BLE_BLK_INSTANCES    0x000u     /* 8 x u16: ((entity - S) | 0x8000), 0 unused */
#define FM1BLE_ENTITY_SIZE      0x268u
#define FM1BLE_RX_SIZE          (0x14u + 0xFFu + 1u)   /* a receive buffer: 0x14 of header and the 0xFF payload, rounded up */
#define FM1BLE_TX_SIZE          0x40u      /* 0x18 + the 0x25-byte payload, rounded up */
#define FM1BLE_RX_HDR           0x14u      /* the payload's offset in a receive buffer (E.5) */
#define FM1BLE_TX_HDR           0x14u      /* ... in a transmit buffer (E.4) */

/* E.2: the per-link parameter block, offsets in the entity, 16-bit words unless said (the IR's names) */
#define FM1BLE_E_ANCHOR         0x000u
#define FM1BLE_E_BITOFF         0x002u
#define FM1BLE_E_ADVIDX         0x004u
#define FM1BLE_E_FORMAT         0x006u
#define FM1BLE_E_OPTCNTL        0x008u
#define FM1BLE_E_BDADDR0        0x00Au     /* the access address, low 16 bits (the IR's name; its use is the access address) */
#define FM1BLE_E_BDADDR1        0x00Cu
#define FM1BLE_E_TXTOG          0x00Eu
#define FM1BLE_E_RXTOG          0x010u
#define FM1BLE_E_TXPTR0         0x012u
#define FM1BLE_E_TXPTR1         0x014u
#define FM1BLE_E_TXAHDR0        0x016u
#define FM1BLE_E_TXAHDR1        0x018u
#define FM1BLE_E_TXDHDR0        0x01Au
#define FM1BLE_E_TXDHDR1        0x01Cu
#define FM1BLE_E_RXPTR0         0x01Eu
#define FM1BLE_E_RXPTR1         0x020u
#define FM1BLE_E_WINCNTL0       0x022u
#define FM1BLE_E_WINCNTL1       0x024u
#define FM1BLE_E_RXAHDR0        0x026u
#define FM1BLE_E_RXAHDR1        0x028u
#define FM1BLE_E_RXDHDR0        0x02Au
#define FM1BLE_E_RXDHDR1        0x02Cu
#define FM1BLE_E_CHMAP0         0x02Eu
#define FM1BLE_E_CHMAP1         0x030u
#define FM1BLE_E_CHMAP2         0x032u
#define FM1BLE_E_LASTCHMAP      0x034u
#define FM1BLE_E_RXMAXBUF       0x036u
#define FM1BLE_E_RXSTAT0        0x038u
#define FM1BLE_E_RXSTAT1        0x03Au
#define FM1BLE_E_FILTERCNTL     0x03Cu
#define FM1BLE_E_CRCWORD0       0x044u
#define FM1BLE_E_CRCWORD1       0x046u
#define FM1BLE_E_WIDEN0         0x048u
#define FM1BLE_E_WIDEN1         0x04Au
#define FM1BLE_E_TARGETADR      0x04Cu     /* 3 words */
#define FM1BLE_E_LOCALADR       0x052u     /* 3 words */
#define FM1BLE_E_EVTCOUNT       0x058u
#define FM1BLE_E_IFSCNT         0x05Cu
#define FM1BLE_E_RFPRIOSTAT     0x05Eu
#define FM1BLE_E_RFPRIOCNTL     0x060u
#define FM1BLE_E_INTFRAME       0x062u
#define FM1BLE_E_TXBUF0CNTL     0x064u     /* bytes: 064, 065 TX, 066, 067 RX */
#define FM1BLE_E_CHTAB0         0x068u     /* four 40-byte tables (F.1) */
#define FM1BLE_E_CHTAB1         0x090u
#define FM1BLE_E_CHTAB2         0x0B8u
#define FM1BLE_E_CHTAB3         0x0E0u
#define FM1BLE_E_WINCNTL2       0x112u     /* the receive window, us from use (E.2): 0x32 advertising, the scan window
                                              scanning, a connection's first transmit window */
#define FM1BLE_E_TXPWR0         0x120u     /* u32: 0x10 + level (E.7) */
#define FM1BLE_E_TXPWR1         0x128u
#define FM1BLE_E_PLLCOMP        0x130u     /* i32 */

#define FM1BLE_ACCESS_ADV       0x8E89BED6ul   /* the advertising access address (Core Specification Vol 6 Part B 2.1.2) */
