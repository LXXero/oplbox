/* SPDX-License-Identifier: GPL-3.0-only
 * See fm1ble.h. Every value below is a fact from docs/BLUETOOTH_RADIO.md (the section is named in the comments); where
 * the document says [measure on FM-1] or [unchecked], this file chooses and says so. Own code. */
#include <string.h>
#include "fm1ble.h"
#include "fm1ble_port.h"
#include "fm1ble_regs.h"

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

static void agc_init(fm1ble *b)                  /* C.3 btagc_init, without btagc_ram_init (0x2FD98 and 0x2FD9C: not read) */
{
    (void)b;
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
    e32(b, FM1BLE_E_TXPWR0, 0x10u);                   /* E.7: 0x10 + level (the level is 0: the chip's maximum is not known) */
    e32(b, FM1BLE_E_TXPWR1, 0x10u);
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
    d->stage = d->err ? "BLOCK" : "DONE";
    return d->err;
}

/* ---- G: advertising */

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
    e16(b, k ? FM1BLE_E_TXPTR1 : FM1BLE_E_TXPTR0, (uint32_t)b->tx[k] + FM1BLE_TX_HDR);
    e16(b, k ? FM1BLE_E_TXAHDR1 : FM1BLE_E_TXAHDR0, ((hdr >> 2) & 0x30u) | (hdr & 0xFu));
}

int fm1ble_adv_start(fm1ble *b, const fm1ble_adv *a)
{
    uint32_t map, first, i, slots;

    if (!b->link_open || b->adv_on)
        return FM1BLE_ESTATE;
    if (!a || (a->type != 0 && a->type != 2 && a->type != 6) || a->nadv > 31 || a->nrsp > 31 || a->filter > 3 ||
        a->interval_slots < 32 || a->interval_slots > 16384)
        return FM1BLE_EARG;
    map = a->chmap ? (a->chmap & 7u) : 7u;
    slots = a->interval_slots;

    for (i = 0; i < 3; i++)                          /* __set_local_addr (E.2: LOCALADR, FORMAT bit 3) */
        e16(b, FM1BLE_E_LOCALADR + 2u * i, (uint32_t)(a->addr[2u * i] | a->addr[2u * i + 1u] << 8));
    e16(b, FM1BLE_E_FORMAT, g16(b, FM1BLE_E_FORMAT) | BIT(3));
    /* G 2: the CRC init and the window (0x32 = 50) */
    e16(b, FM1BLE_E_CRCWORD0, 0x5555u);
    e16(b, FM1BLE_E_CRCWORD1, 0x55u);
    e16(b, FM1BLE_E_WINCNTL0, 0x32u);
    e16(b, FM1BLE_E_WINCNTL1, 0);
    /* G 5: the filter policy */
    e16(b, FM1BLE_E_FILTERCNTL, (a->filter & 1u ? BIT(3) : 0u) | (a->filter & 2u ? BIT(4) : 0u));
    /* G 8: the receive buffers */
    e16(b, FM1BLE_E_RXPTR0, (uint32_t)b->rx[0] + FM1BLE_RX_HDR);
    e16(b, FM1BLE_E_RXPTR1, (uint32_t)b->rx[1] + FM1BLE_RX_HDR);
    /* G 9: the advertising PDU in buffer 0; buffer 1 holds the scan response unless the type has none */
    build_pdu(b, 0, a->type, a->own_random, a->addr, a->adv, a->nadv);
    if (a->type != 2)
        build_pdu(b, 1, 4, a->own_random, a->addr, a->rsp, a->nrsp);
    /* G 10: local address match enabled (OPTCNTL bit 4 cleared) */
    e16(b, FM1BLE_E_OPTCNTL, g16(b, FM1BLE_E_OPTCNTL) & ~BIT(4));
    fm1ble_port_sync();

    /* G 3, 4: the interval (slots, in index 1 and 0xF, event enable in bit 15 of 0xF), the state: advertising, no latency */
    ax_wr(FM1BLE_AX_INTERVAL, slots & 0xFFFFu);
    ax_wr(FM1BLE_AX_INTERVAL_HI, 0x8000u | (slots >> 16));
    ax_wr(FM1BLE_AX_STATE, (uint32_t)FM1BLE_STATE_ADV << 12);
    /* G 6: a random delay on events when the interval is over 10 slots and the type can be scanned or connected to */
    if (slots > 10 && a->type != 2)
        ax_wr(FM1BLE_AX_RANDSLOT, 0xC000u);
    /* G 7: the channel index: hop increment, the map's two ends, the first channel, bit 14. hop: [measure on FM-1], 1 */
    first = (map & 1u) ? 37u : (map & 2u) ? 38u : 39u;
    b->hop = 1;
    ax_wr(FM1BLE_AX_CHANNEL, (uint32_t)b->hop << 8 | (((map & 1u) ^ 1u) << 6) | ((map >> 2) << 7) | first | 0x4000u);
    /* G 12: interrupts on (clear first), the first event armed for the next slot */
    wr(FM1BLE_BB_IRQ_CLR, BIT(HW_ID) | BIT(HW_ID + 8u));
    set(FM1BLE_BB_IRQ_EN, BIT(HW_ID) | BIT(HW_ID + 8u));
    ax_arm(1);
    /* G 14: FORMAT bit 2 last */
    e16(b, FM1BLE_E_FORMAT, g16(b, FM1BLE_E_FORMAT) | BIT(2));
    fm1ble_port_sync();
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
    type = g16(b, k ? FM1BLE_E_RXAHDR1 : FM1BLE_E_RXAHDR0) & 0xFu;
    len = (g16(b, k ? FM1BLE_E_RXDHDR1 : FM1BLE_E_RXDHDR0) >> 8) & 0xFFu;
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
