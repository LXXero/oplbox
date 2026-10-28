/* SPDX-License-Identifier: GPL-3.0-only
 * See fm1ble.h. Every value below is a fact from docs/BLUETOOTH_RADIO.md (the section is named in the comments); where
 * the document says [measure on FM-1] or [unchecked], this file chooses and says so. Own code. */
#include <string.h>
#include "fm1ble.h"
#include "fm1ble_port.h"
#include "fm1ble_regs.h"
#include "fm1ble_agc.h"

#define HW_ID           0u      /* one link: hardware id 0 */
#define BIT(n)          (1u << (n))

static uint32_t rd(uint32_t a) { return fm1ble_port_rd32(a); }
static void wr(uint32_t a, uint32_t v) { fm1ble_port_wr32(a, v); }
static void set(uint32_t a, uint32_t m) { wr(a, rd(a) | m); }
static void clr(uint32_t a, uint32_t m) { wr(a, rd(a) & ~m); }
static void fld(uint32_t a, uint32_t mask, uint32_t v) { wr(a, (rd(a) & ~mask) | (v & mask)); }
static void us(uint32_t n) { fm1ble_port_delay_us(n); }

/* D.2: a register of the per-link file, written through 0x28020 and 0x2801C */
static void ax_wr(uint32_t idx, uint32_t v)
{
    wr(FM1BLE_BB_ANCHOR_WR, v & 0xFFFFu);
    wr(FM1BLE_BB_ANCHOR_CMD, idx << 10 | HW_ID << 4 | 5u);
}

/* D.2's __set_anchor_cnt: bit offset 0, then the slot count to the next event, minus one, with the counter enabled (bit 15 of index 0xE);
 * 0 slots stops it */
static void ax_arm(uint32_t slots)
{
    ax_wr(FM1BLE_AX_BITOFF, 0);
    if (slots) {
        ax_wr(FM1BLE_AX_CNT_LO, (slots - 1u) & 0xFFFFu);
        ax_wr(FM1BLE_AX_CNT_HI, ((slots - 1u) >> 16) | 0x8000u);
    } else {
        ax_wr(FM1BLE_AX_CNT_LO, 0);
        ax_wr(FM1BLE_AX_CNT_HI, 0);
    }
}

/* ---- the entity in the RAM block (E.2): 16-bit words little-endian, the CPU's byte order */

static uint8_t *ent(fm1ble *b) { return b->blk + b->entity; }
static void e16(fm1ble *b, uint32_t off, uint32_t v) { uint8_t *p = ent(b) + off; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t g16(fm1ble *b, uint32_t off) { const uint8_t *p = ent(b) + off; return (uint32_t)(p[0] | p[1] << 8); }
static void e32(fm1ble *b, uint32_t off, uint32_t v) { e16(b, off, v & 0xFFFFu); e16(b, off + 2u, v >> 16); }

/* ---- the bounded wait for the hardware to go idle (D.4: 0x28038 bit 1 is busy) */
static int wait_idle(void)
{
    int i;
    for (i = 0; i < 2000; i++) {
        if (!(rd(FM1BLE_BB_BUSY) & BIT(1)))
            return FM1BLE_OK;
        us(10);
    }
    return FM1BLE_ETIMEOUT;
}

/* ---- B and C: power-up */

void fm1ble_rx_setup(fm1ble *b, int agc_table, uint32_t adv_win_us)
{
    b->rx_set = 1;
    b->rx_agc = (uint8_t)(agc_table != 0);
    b->rx_win = (uint16_t)(adv_win_us > 0xFFFFu ? 0xFFFFu : adv_win_us);
}

static int rx_agc(const fm1ble *b) { return !b->rx_set || b->rx_agc; }
static uint32_t adv_win(const fm1ble *b) { return b->rx_set ? b->rx_win : FM1BLE_ADV_WIN_US; }

static void agc_init(fm1ble *b)                  /* C.3 btagc_init: btagc_ram_init (the gain table) first, then the block */
{
    uint32_t i;
    b->d.agc_table = (uint8_t)rx_agc(b);
    if (b->d.agc_table) {
        wr(FM1BLE_AGC_TAB_AT, 0);
        for (i = 0; i < FM1BLE_AGC_WORDS; i++)
            wr(FM1BLE_AGC_TAB, FM1BLE_AGC_TABLE[i]);
    }
    clr(FM1BLE_ANA(0x20), 0xC0000000u);
    wr(FM1BLE_AGC_0, 0x000F0000u);
    wr(FM1BLE_AGC_0 + 4u, 0x1872BF14u);
    wr(FM1BLE_AGC_0 + 8u, 0x1872BF55u);
    wr(FM1BLE_AGC_0 + 0xCu, 0x1872BF00u);
    wr(FM1BLE_AGC_0 + 0x10u, 0);
    wr(FM1BLE_AGC_0 + 0x14u, 0);
    us(100);
    set(FM1BLE_AGC_0, BIT(0));
    clr(FM1BLE_AGC_0, BIT(20));
    set(FM1BLE_ANA(0x7C), BIT(0));
}

static void analog_init(fm1ble *b)               /* C.1 bt_rf_analog_init */
{
    uint32_t n;
    /* F.2: 81 entries of three words: (n << 8) | n, 0, 0 */
    for (n = 0; n < 81; n++) {
        b->ana_pll[n * 3u] = (n << 8) | n;
        b->ana_pll[n * 3u + 1u] = 0;
        b->ana_pll[n * 3u + 2u] = 0;
    }
    fm1ble_port_sync();
    wr(FM1BLE_ANA_PLLTAB, fm1ble_port_bus(b->ana_pll));
    agc_init(b);
    wr(FM1BLE_ANA(0x48), 0xB9u);
    wr(FM1BLE_ANA(0x00), 0x00FFD144u);
    wr(FM1BLE_ANA(0x04), 0x4E143CDFu);
    wr(FM1BLE_ANA(0x08), 0x03100000u);
    wr(FM1BLE_ANA(0x0C), 0x80808080u);
    wr(FM1BLE_ANA(0x10), 0x80008080u);
    wr(FM1BLE_ANA(0x14), 0x80u);
    wr(FM1BLE_ANA(0x18), 0);
    wr(FM1BLE_ANA(0x1C), 0);
    wr(FM1BLE_ANA(0x20), 0x36555537u);
    wr(FM1BLE_ANA(0x24), 0x00034041u);
    wr(FM1BLE_ANA(0x28), 0x0005C000u);
    if (b->cal.have) {                           /* C.2 update_mdm_txiq_cmp */
        fld(FM1BLE_ANA(0x10), 0x000000FFu, b->cal.cmpuk[0]);
        fld(FM1BLE_ANA(0x10), 0x0000FF00u, (uint32_t)b->cal.cmpuk[1] << 8);
        fld(FM1BLE_ANA(0x10), 0x00FF0000u, ((uint32_t)(-(int)b->cal.cmpuk[2]) & 0xFFu) << 16);
        fld(FM1BLE_ANA(0x08), 0x000003FFu, (uint32_t)b->cal.ofsi & 0x3FFu);
        fld(FM1BLE_ANA(0x08), 0x000FFC00u, ((uint32_t)b->cal.ofsq & 0x3FFu) << 10);
    }
    fld(FM1BLE_ANA(0x9C), 0x0000000Fu, 8);
    fld(FM1BLE_ANA(0x98), 0x000000FFu, 0x36);
    fld(FM1BLE_ANA(0x98), 0x00FF0000u, 0x36u << 16);
    fld(FM1BLE_ANA(0x98), 0x0000FF00u, 0x02u << 8);
    fld(FM1BLE_ANA(0x98), 0xFF000000u, 0x02u << 24);
    fld(FM1BLE_ANA(0x9C), 0x00FF0000u, 0x36u << 16);
    fld(FM1BLE_ANA(0x9C), 0xFF000000u, 0x36u << 24);
    us(10);
    fld(FM1BLE_ANA(0xA0), 0x000000FFu, 0x36);
    fld(FM1BLE_ANA(0xA0), 0x0000FF00u, 0x36u << 8);
}

/* the readbacks the report shows, and what each should read as when the register holds what was written */
static const struct { uint32_t addr, mask, want; } RB[8] = {
    { FM1BLE_WL_CON0, 0x0F1F0181u, (2u << 24) | (0x0Cu << 16) | 0x181u },   /* B.2 field 27-24 = 2, bit 8; B.1 bit 7, field 20-16 = 0x0C, bit 0 */
    { FM1BLE_BT_CLKCON, 0xFFFFu, 0xFCFDu },         /* B.1 step 14 then 15 */
    { FM1BLE_BT_ENA, 0x3FFu, 0x3FFu },              /* B.1 steps 16-17 */
    { FM1BLE_ANA(0x00), 0xFFFFFFFFu, 0x00FFD144u },
    { FM1BLE_ANA(0x04), 0xFFFFFFFFu, 0x4E143CDFu },
    { FM1BLE_ANA(0x20), 0xFFFFFFFFu, 0x36555537u },
    { FM1BLE_ANA(0x98), 0xFFFFFFFFu, 0x02360236u },
    { FM1BLE_ANA(0x9C), 0xFFFF000Fu, 0x36360008u },
};

int fm1ble_radio_up(fm1ble *b, int variant, const fm1ble_cal *cal)
{
    fm1ble_diag *d = &b->d;
    int i;

    memset(d, 0, sizeof *d);
    b->cal.have = 0;
    if (cal)
        b->cal = *cal;
    d->variant = (uint8_t)(variant != 0);
    d->stage = "RF";
    if (variant)
        d->rf_common = fm1ble_port_rf_common();     /* B.1 step 1 */

    /* B.2 bt_process_before_init: run first, as its name says [unchecked when JieLi runs it]; wlc_wreg(5, 0x98) writes
     * 5 << 4 | 0x98 << 16 | 1 to the coexistence command register (B.4) */
    wr(FM1BLE_WL_CON1, 5u << 4 | 0x98u << 16 | 1u);
    fld(FM1BLE_WL_CON0, 0x0F000000u, 2u << 24);
    set(FM1BLE_WL_CON0, BIT(8));
    fld(FM1BLE_BT_PWRCON, 0x3Cu, 0x08u);
    wr(FM1BLE_BSFR_50, 0x87u);
    wr(FM1BLE_BSFR_50, 0xACu);
    set(FM1BLE_BT_PWRCON, BIT(1));
    wr(FM1BLE_IOMAP, 0x101u);

    /* B.1 steps 4-17: rf field 10 = 24 gives delay(240) in steps 8 */
    fm1ble_port_clk(0);
    us(240);
    set(FM1BLE_WL_CON0, BIT(7));
    fld(FM1BLE_WL_CON0, 0x1F0000u, 0x0Cu << 16);
    us(240);
    set(FM1BLE_WL_CON0, BIT(0));
    us(240);
    set(FM1BLE_BT_CLKCON, BIT(10));
    set(FM1BLE_BSFR_BASE, 0xC0000000u);
    us(240);
    wr(FM1BLE_BT_CLKCON, 0xFFFFu);
    clr(FM1BLE_BT_CLKCON, 0x302u);
    wr(FM1BLE_BT_ENA, 0xFu);
    set(FM1BLE_BT_ENA, 0x3F0u);
    d->stage = "ANALOG";
    analog_init(b);                                 /* step 18 */

    for (i = 0; i < 8; i++) {
        d->rb[i] = rd(RB[i].addr);
        d->rb_want[i] = RB[i].want;
        if ((d->rb[i] & RB[i].mask) == (RB[i].want & RB[i].mask))
            d->rb_ok++;
        else
            d->rb_bad++;
    }
    d->stage = "BB";
    b->on = 1;
    return FM1BLE_OK;
}

/* ---- D.1 and E: the baseband and its RAM block */

static void entity_open(fm1ble *b)               /* E.3: every word of E.2 to its value at open */
{
    uint32_t i;
    memset(ent(b), 0, FM1BLE_ENTITY_SIZE);
    e16(b, FM1BLE_E_ADVIDX, 0xD388u);
    e16(b, FM1BLE_E_OPTCNTL, 0x28u | BIT(9));
    e16(b, FM1BLE_E_BDADDR0, 0xBED6u);               /* the advertising access address, 0x8E89BED6 (Core Specification) */
    e16(b, FM1BLE_E_BDADDR1, 0x8E89u);
    e16(b, FM1BLE_E_TXPTR0, 1);
    e16(b, FM1BLE_E_TXPTR1, 1);
    e16(b, FM1BLE_E_TXDHDR0, 1);
    e16(b, FM1BLE_E_TXDHDR1, 1);
    e16(b, FM1BLE_E_RXMAXBUF, 0xFFu);
    e16(b, FM1BLE_E_CRCWORD0, 0x5555u);
    e16(b, FM1BLE_E_CRCWORD1, 0x0055u);
    e16(b, FM1BLE_E_WIDEN1, 0x51E7u);
    e16(b, FM1BLE_E_IFSCNT, 0x8295u);
    e16(b, FM1BLE_E_RFPRIOSTAT, 1);
    e16(b, FM1BLE_E_RFPRIOCNTL, 0x101u);
    e16(b, FM1BLE_E_INTFRAME, BIT(1) | BIT(2));
    ent(b)[FM1BLE_E_TXBUF0CNTL + 2u] &= (uint8_t)~1u;   /* RXBUF0CNTL, RXBUF1CNTL bit 0 cleared */
    ent(b)[FM1BLE_E_TXBUF0CNTL + 3u] &= (uint8_t)~1u;
    b->txpwr = FM1BLE_TXPWR_LEVEL;                   /* E.7: 0x10 + level; the SDK's level, not 0 (the lowest) */
    e32(b, FM1BLE_E_TXPWR0, 0x10u + b->txpwr);
    e32(b, FM1BLE_E_TXPWR1, 0x10u + b->txpwr);
    /* F.1: channel tables. Data channel i: table 0 = i, table 2 = its frequency offset from 2402 MHz; all 37 used, so the
     * compact tables (1 and 3) are the same; advertising channels at 37-39: 37, 38, 39 and 0, 24, 78 */
    for (i = 0; i < 37; i++) {
        uint32_t off = (i + 1u) * 2u + (i >= 11u ? 2u : 0u);
        ent(b)[FM1BLE_E_CHTAB0 + i] = (uint8_t)i;
        ent(b)[FM1BLE_E_CHTAB1 + i] = (uint8_t)i;
        ent(b)[FM1BLE_E_CHTAB2 + i] = (uint8_t)off;
        ent(b)[FM1BLE_E_CHTAB3 + i] = (uint8_t)off;
    }
    for (i = 0; i < 3; i++) {
        static const uint8_t mhz[3] = { 0, 24, 78 };
        ent(b)[FM1BLE_E_CHTAB0 + 37u + i] = (uint8_t)(37u + i);
        ent(b)[FM1BLE_E_CHTAB1 + 37u + i] = (uint8_t)(37u + i);
        ent(b)[FM1BLE_E_CHTAB2 + 37u + i] = mhz[i];
        ent(b)[FM1BLE_E_CHTAB3 + 37u + i] = mhz[i];
    }
    e16(b, FM1BLE_E_CHMAP0, 0xFFFFu);
    e16(b, FM1BLE_E_CHMAP1, 0xFFFFu);
    e16(b, FM1BLE_E_CHMAP2, 37u << 5 | 0x1Fu);
}

int fm1ble_bb_init(fm1ble *b, void *block)
{
    fm1ble_diag *d = &b->d;
    uint32_t used, off;

    if (!b->on || !block || ((uintptr_t)block & 3u))
        return b->on ? FM1BLE_EARG : FM1BLE_ESTATE;
    b->blk = block;
    b->blk_bus = fm1ble_port_bus(block);
    memset(block, 0, FM1BLE_BLOCK_SIZE);
    /* E.1's layout: the instance table, entity 0, two receive buffers, two transmit buffers */
    off = 0x10u;
    b->entity = (uint16_t)off;
    off += FM1BLE_ENTITY_SIZE;
    b->rx[0] = (uint16_t)off;
    b->rx[1] = (uint16_t)(off + FM1BLE_RX_SIZE);
    off += 2u * FM1BLE_RX_SIZE;
    b->tx[0] = (uint16_t)off;
    b->tx[1] = (uint16_t)(off + FM1BLE_TX_SIZE);
    off += 2u * FM1BLE_TX_SIZE;
    used = off;
    if (used > FM1BLE_BLOCK_SIZE)
        return FM1BLE_EARG;

    set(FM1BLE_BB_RAMWIN_EN, BIT(0));            /* D.1 */
    wr(FM1BLE_BB_CON0, 1);
    wr(FM1BLE_BB_T34, 0x0A01u);
    wr(FM1BLE_BB_RAM_START, b->blk_bus);
    wr(FM1BLE_BB_RAM_END, b->blk_bus + used);
    set(FM1BLE_BB_CON0, BIT(13));
    set(FM1BLE_BB_CON0, BIT(14));
    clr(FM1BLE_BB_CON0, BIT(4));
    fld(FM1BLE_BB_T08, 0x3FFu, 0x46u);
    set(FM1BLE_BB_T08, 0xF000u);
    fld(FM1BLE_BB_T0C, 0x3FFu, 0x46u);
    fld(FM1BLE_BB_T10, 0x00FFu, 0x3Eu);
    fld(FM1BLE_BB_T10, 0xFF00u, 0x3Eu << 8);
    fld(FM1BLE_BB_T14, 0x00FFu, 0x1Eu);
    fld(FM1BLE_BB_T18, 0x00FFu, 0x14u);
    fld(FM1BLE_BB_T18, 0xFF00u, 0x14u << 8);

    /* E.3: open entity 0: its offset, tagged 0x8000, in the instance table; every word to its value at open; standby */
    entity_open(b);
    b->blk[FM1BLE_BLK_INSTANCES] = (uint8_t)(b->entity | 0);
    b->blk[FM1BLE_BLK_INSTANCES + 1u] = (uint8_t)((b->entity >> 8) | 0x80u);
    fm1ble_port_sync();
    ax_arm(0);                                   /* ble_hw_disable: the counter off, wait for idle, interrupts off */
    d->err = wait_idle();
    clr(FM1BLE_BB_IRQ_EN, BIT(HW_ID) | BIT(HW_ID + 8u));
    b->link_open = 1;

    d->bb[0] = rd(FM1BLE_BB_CON0);
    d->bb[1] = rd(FM1BLE_BB_T08);
    d->bb[2] = rd(FM1BLE_BB_T10);
    d->bb[3] = rd(FM1BLE_BB_T34);
    d->bb[4] = rd(FM1BLE_BB_RAM_START);
    d->bb[5] = rd(FM1BLE_BB_RAM_END);
    d->bb[6] = rd(FM1BLE_BB_RAMWIN_EN);
    d->bb[7] = rd(FM1BLE_BB_BUSY);
    d->ram_end = d->bb[5];
    b->d.txtog = (uint16_t)g16(b, FM1BLE_E_TXTOG);
    d->stage = d->err ? "BLOCK" : "DONE";
    return d->err;
}

/* ---- G: advertising */

/* E.4: TX buffer `buf` (its header word at +0x10, its length at +0x12) into the hardware's TX slot `slot`: TXPTR = the
 * payload's offset (when the length is not 0), TXAHDR = ((hdr >> 2) & 0x30) | (hdr & 0xF), TXDHDR = length << 8, bit 2
 * kept, header bit 9 in bit 3, header bits 5-4 in bits 1-0. The length in TXDHDR is what the first FM-1 run left out
 * (TXDHDR stayed 1 from the open: zero-length advertising PDUs). */
static void tx_slot(fm1ble *b, uint32_t slot, int buf)
{
    const uint8_t *t = b->blk + b->tx[buf];
    uint32_t hdr = (uint32_t)(t[0x10] | t[0x11] << 8), len = t[0x12];
    uint32_t dh = slot ? FM1BLE_E_TXDHDR1 : FM1BLE_E_TXDHDR0;
    if (len)
        e16(b, slot ? FM1BLE_E_TXPTR1 : FM1BLE_E_TXPTR0, (uint32_t)b->tx[buf] + FM1BLE_TX_HDR);
    e16(b, slot ? FM1BLE_E_TXAHDR1 : FM1BLE_E_TXAHDR0, ((hdr >> 2) & 0x30u) | (hdr & 0xFu));
    e16(b, dh, len << 8 | (g16(b, dh) & 4u) | ((hdr >> 6) & 8u) | ((hdr >> 4) & 3u));
}

/* the TX slot the hardware sends next: bit 0 of TXTOG, which the hardware changes (docs/BLUETOOTH_RADIO.md "TX and RX
 * toggles"); a change since the last read is counted as a buffer sent */
static uint32_t tx_next(fm1ble *b)
{
    uint32_t t = g16(b, FM1BLE_E_TXTOG);
    if (t != b->d.txtog) {
        b->d.tx_flips++;
        b->d.txtog = (uint16_t)t;
    }
    return t & 1u;
}

/* G 9 after each event: the advertising PDU (buffer 0) in the slot sent next, the second PDU (the scan response, buffer 1;
 * for ADV_NONCONN_IND, which has none, buffer 0 again) in the other */
static void adv_slots(fm1ble *b)
{
    uint32_t s = tx_next(b);
    tx_slot(b, s, 0);
    tx_slot(b, s ^ 1u, b->adv_type == 2 ? 0 : 1);
}

static void build_pdu(fm1ble *b, int k, uint8_t type, uint8_t txadd, const uint8_t *addr, const uint8_t *data, uint8_t n)
{
    uint8_t *t = b->blk + b->tx[k];
    uint32_t hdr = (uint32_t)(type & 0xFu) | (uint32_t)(txadd ? BIT(6) : 0);
    memset(t, 0, FM1BLE_TX_SIZE);
    t[0x10] = (uint8_t)hdr;
    t[0x11] = (uint8_t)(hdr >> 8);
    t[0x12] = (uint8_t)(n + 6u);
    memcpy(t + FM1BLE_TX_HDR, addr, 6);              /* E.4: the payload begins with the advertiser address */
    memcpy(t + FM1BLE_TX_HDR + 6u, data, n);
}

/* G 3, 4, 6, 7, 12 and 14: the interval (slots, event enable in bit 15 of 0xF), the state (advertising, no latency), the
 * random delay on events (index 8) when asked, the channel index (hop increment 1, the map's two ends, the first channel,
 * bit 14), interrupts on (clear first), the first event armed `slots0` slots ahead (1: the next slot), FORMAT bit 2 last.
 * fm1ble_adv_start's advertising and the PAL's advertising events (fm1ble_op_adv) both arm this way: the sequence a central
 * heard from the FM-1 at 122afe3 (trials 1 and 2), where the PAL's own one-channel events (hop 0) were never heard. */
static void adv_arm(fm1ble *b, uint32_t map, uint32_t slots, int random_slot, uint32_t slots0)
{
    uint32_t first = (map & 1u) ? 37u : (map & 2u) ? 38u : 39u;

    ax_wr(FM1BLE_AX_INTERVAL, slots & 0xFFFFu);
    ax_wr(FM1BLE_AX_INTERVAL_HI, 0x8000u | (slots >> 16));
    ax_wr(FM1BLE_AX_STATE, (uint32_t)FM1BLE_STATE_ADV << 12);
    if (random_slot)
        ax_wr(FM1BLE_AX_RANDSLOT, 0xC000u);
    /* hop: [measure on FM-1], 1 */
    b->hop = 1;
    ax_wr(FM1BLE_AX_CHANNEL, (uint32_t)b->hop << 8 | (((map & 1u) ^ 1u) << 6) | ((map >> 2) << 7) | first | 0x4000u);
    wr(FM1BLE_BB_IRQ_CLR, BIT(HW_ID) | BIT(HW_ID + 8u));
    set(FM1BLE_BB_IRQ_EN, BIT(HW_ID) | BIT(HW_ID + 8u));
    ax_arm(slots0);
    e16(b, FM1BLE_E_FORMAT, g16(b, FM1BLE_E_FORMAT) | BIT(2));
    fm1ble_port_sync();
}

int fm1ble_adv_start(fm1ble *b, const fm1ble_adv *a)
{
    uint32_t map, i, slots;

    if (!b->link_open || b->adv_on || b->op_on)
        return FM1BLE_ESTATE;
    if (!a || (a->type != 0 && a->type != 2 && a->type != 6) || a->nadv > 31 || a->nrsp > 31 || a->filter > 3 ||
        a->interval_slots < 32 || a->interval_slots > 16384)
        return FM1BLE_EARG;
    map = a->chmap ? (a->chmap & 7u) : 7u;
    slots = a->interval_slots;

    for (i = 0; i < 3; i++)                          /* __set_local_addr (E.2: LOCALADR, FORMAT bit 3) */
        e16(b, FM1BLE_E_LOCALADR + 2u * i, (uint32_t)(a->addr[2u * i] | a->addr[2u * i + 1u] << 8));
    e16(b, FM1BLE_E_FORMAT, g16(b, FM1BLE_E_FORMAT) | BIT(3));
    /* G 2: the CRC init and the windows (WINCNTL0 0x32; WINCNTL2, the time the receiver listens: le_hw_advertising's 0x32
     * unless fm1ble_rx_setup says otherwise) */
    e16(b, FM1BLE_E_CRCWORD0, 0x5555u);
    e16(b, FM1BLE_E_CRCWORD1, 0x55u);
    e16(b, FM1BLE_E_WINCNTL0, FM1BLE_WIN0);
    e16(b, FM1BLE_E_WINCNTL1, 0);
    e16(b, FM1BLE_E_WINCNTL2, adv_win(b));
    /* G 5: the filter policy */
    e16(b, FM1BLE_E_FILTERCNTL, (a->filter & 1u ? BIT(3) : 0u) | (a->filter & 2u ? BIT(4) : 0u));
    /* G 8: the receive buffers */
    e16(b, FM1BLE_E_RXPTR0, (uint32_t)b->rx[0] + FM1BLE_RX_HDR);
    e16(b, FM1BLE_E_RXPTR1, (uint32_t)b->rx[1] + FM1BLE_RX_HDR);
    /* G 9: the advertising PDU in buffer 0; buffer 1 holds the scan response unless the type has none */
    build_pdu(b, 0, a->type, a->own_random, a->addr, a->adv, a->nadv);
    if (a->type != 2)
        build_pdu(b, 1, 4, a->own_random, a->addr, a->rsp, a->nrsp);
    b->adv_type = a->type;
    adv_slots(b);                                    /* TXTOG may be odd from an earlier link: the slot it names */
    /* G 10: local address match enabled (OPTCNTL bit 4 cleared) */
    e16(b, FM1BLE_E_OPTCNTL, g16(b, FM1BLE_E_OPTCNTL) & ~BIT(4));
    fm1ble_port_sync();

    /* G 3, 4, 6 (a random delay on events when the interval is over 10 slots and the type can be scanned or connected
     * to), 7, 12, 14 */
    adv_arm(b, map, slots, slots > 10 && a->type != 2, 1);
    b->slots = (uint16_t)slots;
    b->adv_on = 1;
    return FM1BLE_OK;
}

int fm1ble_adv_stop(fm1ble *b)
{
    int rc;
    if (!b->adv_on)
        return FM1BLE_ESTATE;
    ax_arm(0);
    rc = wait_idle();
    clr(FM1BLE_BB_IRQ_EN, BIT(HW_ID) | BIT(HW_ID + 8u));
    b->adv_on = 0;
    return rc;
}

void fm1ble_set_rearm(fm1ble *b, int on) { b->rearm = (uint8_t)(on != 0); }

static void rx_capture(fm1ble *b)
{
    fm1ble_diag *d = &b->d;
    uint32_t k, len, type;

    d->rx_hdr[0] = (uint16_t)g16(b, FM1BLE_E_RXAHDR0);
    d->rx_hdr[1] = (uint16_t)g16(b, FM1BLE_E_RXDHDR0);
    d->rx_stat[0] = (uint16_t)g16(b, FM1BLE_E_RXSTAT0);
    d->rx_stat[1] = (uint16_t)g16(b, FM1BLE_E_RXSTAT1);
    k = g16(b, FM1BLE_E_RXSTAT0) ? 0u : 1u;          /* the buffer the hardware used: [unchecked how it chooses] */
    d->rx_last_ahdr = (uint16_t)g16(b, k ? FM1BLE_E_RXAHDR1 : FM1BLE_E_RXAHDR0);
    type = d->rx_last_ahdr & 0xFu;
    d->rx_last_dhdr = (uint16_t)g16(b, k ? FM1BLE_E_RXDHDR1 : FM1BLE_E_RXDHDR0);
    d->rx_last_buf = (uint8_t)k;
    len = (d->rx_last_dhdr >> 8) & 0xFFu;
    d->rx_last_type = (uint8_t)type;
    d->rx_last_len = (uint8_t)len;
    d->rx_last_n = (uint8_t)(len < sizeof d->rx_last ? len : sizeof d->rx_last);
    memcpy(d->rx_last, b->blk + b->rx[k] + FM1BLE_RX_HDR, d->rx_last_n);
    e16(b, FM1BLE_E_RXSTAT0, 0);                     /* taken: the hardware writes the status of the next packet */
    e16(b, FM1BLE_E_RXSTAT1, 0);
    if (type == 5)
        d->rx_conn_ind++;
    else if (type == 3)
        d->rx_scan_req++;
    else
        d->rx_other++;
}

int fm1ble_poll(fm1ble *b)
{
    fm1ble_diag *d = &b->d;
    uint32_t pend, n = 0;

    if (!b->adv_on)
        return 0;
    pend = rd(FM1BLE_BB_IRQ_PEND) & rd(FM1BLE_BB_IRQ_EN);
    d->pending_last = pend;
    if (pend & BIT(HW_ID)) {
        wr(FM1BLE_BB_IRQ_CLR, BIT(HW_ID));
        d->events++;
        n++;
        adv_slots(b);
        fm1ble_port_sync();
        if (b->rearm) {
            ax_arm(b->slots);
            d->rearms++;
        }
    }
    if (pend & BIT(HW_ID + 8u)) {
        wr(FM1BLE_BB_IRQ_CLR, BIT(HW_ID + 8u));
        d->rx_packets++;
        n++;
        rx_capture(b);
    }
    return (int)n;
}

const uint8_t *fm1ble_rx_last(const fm1ble *b, uint8_t *n, uint8_t *type)
{
    *n = b->d.rx_last_n;
    *type = b->d.rx_last_type;
    return b->d.rx_last;
}

void fm1ble_radio_down(fm1ble *b)
{
    if (b->adv_on)
        fm1ble_adv_stop(b);
    fm1ble_op_stop(b);
    if (!b->on)
        return;
    clr(FM1BLE_WL_CON0, 0x1Fu);                  /* B.5 bt_rf_close */
    us(1000);
    wr(FM1BLE_WL_CON0, 0);
    us(1000);
    fm1ble_port_clk(0xC000u);
    us(1000);
    b->on = 0;
    b->link_open = 0;
}

/* ---- one packet at a time (fm1ble.h "for Cordio's PAL"): our design from the facts, every choice marked */

#define OP_MAX_PDU      (2u + 37u)               /* two header bytes and the TX buffers' 37-byte payload (E.4) */
#define OP_NEVER        0xFFFFu                  /* the interval of a one-shot operation: 41 s, the next one comes first */

int fm1ble_op_chan(fm1ble *b, uint32_t aa, uint32_t crc, uint8_t chan)
{
    if (!b->link_open)
        return FM1BLE_ESTATE;
    if (chan > 39)
        return FM1BLE_EARG;
    e16(b, FM1BLE_E_BDADDR0, aa & 0xFFFFu);          /* E.2: the access address in BDADDR1:BDADDR0 */
    e16(b, FM1BLE_E_BDADDR1, aa >> 16);
    e16(b, FM1BLE_E_CRCWORD0, crc & 0xFFFFu);        /* CRC init, 24 bits */
    e16(b, FM1BLE_E_CRCWORD1, (crc >> 16) & 0xFFu);
    b->op_chan = chan;
    return FM1BLE_OK;
}

void fm1ble_op_power(fm1ble *b, uint8_t level)
{
    if (!b->link_open)
        return;
    if (level > FM1BLE_TXPWR_MAX)
        level = FM1BLE_TXPWR_MAX;
    b->txpwr = level;
    e32(b, FM1BLE_E_TXPWR0, 0x10u + level);           /* E.7 */
    e32(b, FM1BLE_E_TXPWR1, 0x10u + level);
}

/* A PDU into TX buffer k as E.4 lays it out: the header word at +0x10, the length at +0x12, the payload at +0x14; an
 * advertising one into TX slot `slot` (tx_slot), a data one into slot k */
static void op_buf(fm1ble *b, int k, const uint8_t *pdu, uint16_t n, int adv, uint32_t slot)
{
    uint8_t *t = b->blk + b->tx[k];
    uint32_t len = n >= 2u ? pdu[1] : 0u;
    memset(t, 0, FM1BLE_TX_SIZE);
    t[0x10] = n ? pdu[0] : 0;
    t[0x12] = (uint8_t)len;
    if (len)
        memcpy(t + FM1BLE_TX_HDR, pdu + 2, len);
    if (adv) {
        tx_slot(b, slot, k);
    } else {
        /* a data PDU: length in 15-8, LLID/NESN/SN/MD in 4-0, as RXDHDR (E.5) [unchecked]; in both TX slots, so it is the
         * one sent whichever slot TXTOG names (the hardware picks by TXTOG bit 0, "TX and RX toggles"; which way it changes
         * it in a connection is [unchecked]) */
        uint32_t s, dh = len << 8 | (t[0x10] & 0x1Fu);
        (void)slot;
        for (s = 0; s < 2u; s++) {
            e16(b, s ? FM1BLE_E_TXPTR1 : FM1BLE_E_TXPTR0, (uint32_t)b->tx[k] + FM1BLE_TX_HDR);
            e16(b, s ? FM1BLE_E_TXAHDR1 : FM1BLE_E_TXAHDR0, 0);
            e16(b, s ? FM1BLE_E_TXDHDR1 : FM1BLE_E_TXDHDR0, dh);
        }
    }
}

/* how op_arm writes the counter (fm1ble.h FM1BLE_CONN_PADJ, PADB) */
#define ARM_JIELI       1u                       /* the slots minus one, the bit offset before them (__set_anchor_cnt) */
#define ARM_BIT15       2u                       /* the bit offset | 0x8000 (__hw_set_widen_drift) */

/* The common part of a connection event: the RX buffers, the state, a one-shot interval, the channel, interrupts, the
 * counter */
static void op_arm(fm1ble *b, uint32_t state, uint32_t delay_us, uint32_t how)
{
    /* the counter: whole slots, then the bit offset inside the slot. Where the slot boundaries lie against this write is
     * the baseband's own clock [measure on FM-1]: taken as a boundary at the write, so 0 slots and an offset is "this
     * slot" (Cordio's scheduler arms 300 us ahead, SCH_LOAD_DELAY_US, less than a slot) */
    uint32_t slots = delay_us / 625u, bitoff = delay_us % 625u;

    e16(b, FM1BLE_E_RXPTR0, (uint32_t)b->rx[0] + FM1BLE_RX_HDR);
    e16(b, FM1BLE_E_RXPTR1, (uint32_t)b->rx[1] + FM1BLE_RX_HDR);
    e16(b, FM1BLE_E_RXSTAT0, 0);
    e16(b, FM1BLE_E_RXSTAT1, 0);
    fm1ble_port_sync();
    ax_wr(FM1BLE_AX_INTERVAL, OP_NEVER);
    ax_wr(FM1BLE_AX_INTERVAL_HI, 0x8000u);
    ax_wr(FM1BLE_AX_STATE, state << 12);
    set(FM1BLE_BB_CON0, BIT(2));                     /* K 4: a fixed channel, latched by a pulse of 0x28000 bit 2 */
    us(10);
    ax_wr(FM1BLE_AX_CHANNEL, b->op_chan | 0x2000u);
    clr(FM1BLE_BB_CON0, BIT(2));
    wr(FM1BLE_BB_IRQ_CLR, BIT(HW_ID) | BIT(HW_ID + 8u));
    set(FM1BLE_BB_IRQ_EN, BIT(HW_ID) | BIT(HW_ID + 8u));
    if (how & ARM_JIELI) {                           /* __set_anchor_cnt's writes, the bit offset first */
        uint32_t cnt = slots ? slots - 1u : 0u;
        ax_wr(FM1BLE_AX_BITOFF, (how & ARM_BIT15) ? bitoff | 0x8000u : bitoff);
        ax_wr(FM1BLE_AX_CNT_LO, 0);
        ax_wr(FM1BLE_AX_CNT_HI, 0);
        ax_wr(FM1BLE_AX_CNT_LO, cnt & 0xFFFFu);
        ax_wr(FM1BLE_AX_CNT_HI, (cnt >> 16) | 0x8000u);
    } else {
        ax_wr(FM1BLE_AX_BITOFF, 0);                  /* D.2 __set_anchor_cnt, then the bit offset inside the slot */
        ax_wr(FM1BLE_AX_CNT_LO, slots & 0xFFFFu);
        ax_wr(FM1BLE_AX_BITOFF, bitoff);
        ax_wr(FM1BLE_AX_CNT_HI, (slots >> 16) | 0x8000u);
    }
    b->op_state = (uint8_t)state;
    b->op_on = 1;
}

int fm1ble_op_adv(fm1ble *b, uint32_t delay_us, const uint8_t *pdu, uint16_t n, const uint8_t *rsp, uint16_t nrsp)
{
    uint32_t i, type;

    if (!b->link_open || b->adv_on)
        return FM1BLE_ESTATE;
    if (b->op_chan < 37 || !pdu || n < 8 || n > OP_MAX_PDU || pdu[1] != n - 2u)
        return FM1BLE_EARG;
    if (rsp && nrsp && (nrsp < 8 || nrsp > OP_MAX_PDU || rsp[1] != nrsp - 2u))
        return FM1BLE_EARG;
    type = pdu[0] & 0xFu;
    /* fm1ble_adv_start's order: __set_local_addr from the PDU's AdvA (E.2: LOCALADR, FORMAT bit 3), G 2, G 5 (no filter:
     * Cordio filters), G 8 */
    for (i = 0; i < 3; i++)
        e16(b, FM1BLE_E_LOCALADR + 2u * i, (uint32_t)(pdu[2u + 2u * i] | pdu[3u + 2u * i] << 8));
    e16(b, FM1BLE_E_FORMAT, g16(b, FM1BLE_E_FORMAT) | BIT(3));
    e16(b, FM1BLE_E_WINCNTL0, FM1BLE_WIN0);
    e16(b, FM1BLE_E_WINCNTL1, 0);
    e16(b, FM1BLE_E_WINCNTL2, adv_win(b));
    e16(b, FM1BLE_E_WIDEN0, 0);                       /* the open's widening (E.3), which a connection event changes: the */
    e16(b, FM1BLE_E_WIDEN1, 0x51E7u);                 /* advertising after a connection as the first one */
    e16(b, FM1BLE_E_FILTERCNTL, 0);
    e16(b, FM1BLE_E_RXPTR0, (uint32_t)b->rx[0] + FM1BLE_RX_HDR);
    e16(b, FM1BLE_E_RXPTR1, (uint32_t)b->rx[1] + FM1BLE_RX_HDR);
    e16(b, FM1BLE_E_RXSTAT0, 0);
    e16(b, FM1BLE_E_RXSTAT1, 0);
    /* G 9: the PDU in buffer 0 at the slot the hardware sends next; the other slot holds this event's scan response
     * (buffer 1) or, with none and for ADV_NONCONN_IND, the PDU again. Nothing is kept from an earlier event: the caller
     * hands over the scan response as it is now, so a changed one goes out from the next event on */
    b->op_slot = (uint8_t)tx_next(b);                 /* the slot the hardware sends next */
    op_buf(b, 0, pdu, n, 1, b->op_slot);
    if (type != 2u && rsp && nrsp)
        op_buf(b, 1, rsp, nrsp, 1, b->op_slot ^ 1u);
    else
        tx_slot(b, b->op_slot ^ 1u, 0);
    e16(b, FM1BLE_E_OPTCNTL, g16(b, FM1BLE_E_OPTCNTL) & ~BIT(4));         /* G 10: local address match on */
    fm1ble_port_sync();
    /* one event over 37, 38 and 39 (the hardware hops), the first one in the slot delay_us falls in, one-shot (an
     * interval the next operation always comes before) */
    adv_arm(b, 7u, OP_NEVER, type != 2u, 1u + delay_us / 625u);
    b->op_state = FM1BLE_STATE_ADV;
    b->op_on = 1;
    return FM1BLE_OK;
}

int fm1ble_op_conn(fm1ble *b, uint32_t delay_us, uint32_t window_us)
{
    uint32_t how = 0, win0 = FM1BLE_WIN0;

    if (!b->link_open || b->adv_on)
        return FM1BLE_ESTATE;
    if (b->op_chan > 36)
        return FM1BLE_EARG;
    if (!b->op_on || b->op_state != 7u)              /* an empty PDU until the PAL puts the response in (LLID 1, length 0) */
        op_buf(b, 0, (const uint8_t *)"\x01\x00", 2, 0, 0);
    /* the windows as le_hw_ioctrl's __set_winsize writes them: WINCNTL0 0x32, the time the receiver listens in WINCNTL2
     * (taken as us from its uses; up to 3b39ab4 the window went into WINCNTL0 and WINCNTL2 stayed 0) */
    if (b->conn_setup == FM1BLE_CONN_KEEP) {          /* the open's values (E.3), which 58cadc5 left in place */
        e16(b, FM1BLE_E_WIDEN0, 0);
        e16(b, FM1BLE_E_WIDEN1, 0x51E7u);
    } else if (b->conn_setup == FM1BLE_CONN_PAD || b->conn_setup >= FM1BLE_CONN_PADJ) {   /* PAD, and PADJ, PADB, PADW */
        e16(b, FM1BLE_E_WIDEN0, 0);
        e16(b, FM1BLE_E_WIDEN1, 0x5000u);
        delay_us = delay_us > FM1BLE_CONN_PAD_US ? delay_us - FM1BLE_CONN_PAD_US : 0u;
        window_us += 2u * FM1BLE_CONN_PAD_US;
        if (b->conn_setup >= FM1BLE_CONN_PADJ)
            how |= ARM_JIELI;
        if (b->conn_setup >= FM1BLE_CONN_PADB)
            how |= ARM_BIT15;
        if (b->conn_setup == FM1BLE_CONN_PADW)
            win0 = window_us;
    } else if (b->conn_setup == FM1BLE_CONN_WIDEN) {
        uint32_t w = window_us / 2u;                 /* __set_widen(w): w / 625 slots, w % 625 | 0x5000 */
        e16(b, FM1BLE_E_WIDEN0, w / 625u);
        e16(b, FM1BLE_E_WIDEN1, (w % 625u) | 0x5000u);
        delay_us += w;
    } else if (b->conn_setup == FM1BLE_CONN_WIDE) {  /* as PAD, FM1BLE_CONN_WIDE_US each side */
        e16(b, FM1BLE_E_WIDEN0, 0);
        e16(b, FM1BLE_E_WIDEN1, 0x5000u);
        delay_us = delay_us > FM1BLE_CONN_WIDE_US ? delay_us - FM1BLE_CONN_WIDE_US : 0u;
        window_us += 2u * FM1BLE_CONN_WIDE_US;
    }
    e16(b, FM1BLE_E_WINCNTL0, win0 & 0xFFFFu);       /* PADW: the window, as __hw_set_widen_drift writes it; else 0x32 */
    e16(b, FM1BLE_E_WINCNTL1, win0 >> 16);
    e16(b, FM1BLE_E_WINCNTL2, window_us > 0xFFFFu ? 0xFFFFu : window_us);
    op_arm(b, 7u, delay_us, how);
    return FM1BLE_OK;
}

void fm1ble_op_conn_setup(fm1ble *b, uint8_t setup) { b->conn_setup = setup < FM1BLE_CONN_SETUPS ? setup : FM1BLE_CONN_KEEP; }

int fm1ble_op_resp(fm1ble *b, const uint8_t *pdu, uint16_t n)
{
    if (!b->link_open)
        return FM1BLE_ESTATE;
    if (!pdu || n < 2 || n > OP_MAX_PDU || pdu[1] != n - 2u)
        return FM1BLE_EARG;
    if (b->op_state == FM1BLE_STATE_ADV)
        op_buf(b, 1, pdu, n, 1, b->op_slot ^ 1u);    /* for this event; the next fm1ble_op_adv takes the one it is given */
    else
        op_buf(b, 0, pdu, n, 0, 0);
    fm1ble_port_sync();
    return FM1BLE_OK;
}

uint32_t fm1ble_anchor_rd(fm1ble *b, uint32_t index)
{
    if (!b->link_open)
        return 0;
    wr(FM1BLE_BB_ANCHOR_CMD, (index & 0x1Fu) << 10 | HW_ID << 4 | 2u);  /* D.2's read; widen_drift writes index 0x10 */
    return rd(FM1BLE_BB_ANCHOR_RD) & 0xFFFFu;
}

void fm1ble_op_stop(fm1ble *b)
{
    if (!b->op_on)
        return;
    ax_wr(FM1BLE_AX_CNT_HI, 0);                      /* the counter off (ble_hw_disable without its wait) */
    clr(FM1BLE_BB_IRQ_EN, BIT(HW_ID) | BIT(HW_ID + 8u));
    wr(FM1BLE_BB_IRQ_CLR, BIT(HW_ID) | BIT(HW_ID + 8u));
    b->op_on = 0;
}

uint32_t fm1ble_op_service(fm1ble *b, fm1ble_pkt *p)
{
    fm1ble_diag *d = &b->d;
    uint32_t pend, ev = 0;

    if (!b->op_on)
        return 0;
    pend = rd(FM1BLE_BB_IRQ_PEND) & rd(FM1BLE_BB_IRQ_EN);
    d->pending_last = pend;
    if (pend & BIT(HW_ID + 8u)) {                    /* the packet first: it came before the event's end */
        uint32_t k, ahdr, dhdr;
        wr(FM1BLE_BB_IRQ_CLR, BIT(HW_ID + 8u));
        d->rx_packets++;
        k = g16(b, FM1BLE_E_RXSTAT0) ? 0u : 1u;      /* the buffer the hardware used [unchecked how it chooses] */
        ahdr = g16(b, k ? FM1BLE_E_RXAHDR1 : FM1BLE_E_RXAHDR0);
        dhdr = g16(b, k ? FM1BLE_E_RXDHDR1 : FM1BLE_E_RXDHDR0);
        p->stat = (uint16_t)g16(b, k ? FM1BLE_E_RXSTAT1 : FM1BLE_E_RXSTAT0);
        p->ok = (p->stat & 0xFu) == 1u;
        /* an advertising PDU's header byte is RXAHDR's low byte (E.5 reads TxAdd and RxAdd from its bits 6 and 7); a data
         * PDU's is RXDHDR bits 4-0 (LLID, NESN, SN, MD) [unchecked]; the length is RXDHDR bits 15-8 either way */
        p->hdr[0] = (uint8_t)(b->op_state == FM1BLE_STATE_ADV ? ahdr & 0xFFu : dhdr & 0x1Fu);
        p->hdr[1] = (uint8_t)(dhdr >> 8);
        p->payload = b->blk + b->rx[k] + FM1BLE_RX_HDR;
        d->rx_hdr[0] = (uint16_t)ahdr;                   /* the report's evidence, as rx_capture keeps it */
        d->rx_hdr[1] = (uint16_t)dhdr;
        d->rx_last_ahdr = (uint16_t)ahdr;
        d->rx_last_dhdr = (uint16_t)dhdr;
        d->rx_last_buf = (uint8_t)k;
        d->rx_stat[k] = p->stat;
        d->rx_last_type = (uint8_t)(p->hdr[0] & 0xFu);
        d->rx_last_len = p->hdr[1];
        d->rx_last_n = (uint8_t)(p->hdr[1] < sizeof d->rx_last ? p->hdr[1] : sizeof d->rx_last);
        memcpy(d->rx_last, p->payload, d->rx_last_n);
        e16(b, FM1BLE_E_RXSTAT0, 0);
        e16(b, FM1BLE_E_RXSTAT1, 0);
        if (b->op_state == FM1BLE_STATE_ADV) {
            uint32_t type = p->hdr[0] & 0xFu;
            if (type == 5)
                d->rx_conn_ind++;
            else if (type == 3)
                d->rx_scan_req++;
            else
                d->rx_other++;
        } else {
            d->conn_stat = p->stat;
            d->conn_dhdr = (uint16_t)dhdr;
            if (p->stat & BIT(8))
                d->conn_acks++;
        }
        ev |= FM1BLE_EV_RX;
    }
    if (pend & BIT(HW_ID)) {
        wr(FM1BLE_BB_IRQ_CLR, BIT(HW_ID));
        d->events++;
        if (b->op_state == FM1BLE_STATE_ADV) {
            (void)tx_next(b);                        /* counts a TXTOG change */
            d->op_adv_done++;
        } else {
            uint32_t flips = d->tx_flips;
            (void)tx_next(b);                        /* a TXTOG change: the hardware sent a buffer in the event */
            d->conn_flips += d->tx_flips - flips;
            d->op_conn_done++;
        }
        ev |= FM1BLE_EV_DONE;
    }
    return ev;
}
