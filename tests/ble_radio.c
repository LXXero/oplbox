/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the BLE baseband driver (firmware/ble/fm1ble.c, docs/BLUETOOTH.md "Cordio path as built" step 3) against the
 * register model (tests/ble/bb_model.c) and of the test run on it (firmware/ble/fm1ble_test.c): the power-up writes in the
 * order docs/BLUETOOTH_RADIO.md B and C list them, the baseband and RAM block (D.1, E), the advertising link (G: PDU
 * buffers, pointers, the indexed registers, interrupts, the armed event), polling events and received packets, stopping,
 * and the run's steps for a baseband that repeats events, one that needs re-arming, one that needs the shared radio's init
 * first, and one that is silent. The model comes from the same facts as the driver: this proves the driver does what the
 * document says, not that the chip does. Exits 1 when a check fails. */
#include <stdio.h>
#include <string.h>
#include "fm1ble.h"
#include "fm1ble_port.h"
#include "fm1ble_regs.h"
#include "fm1ble_agc.h"
#include "fm1ble_test.h"
#include "ble_midi.h"
#include "bb_model.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

static fm1ble B;
static uint32_t blk[FM1BLE_BLOCK_SIZE / 4u];

static uint32_t rd16(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t e16(uint32_t off) { return rd16(B.blk + B.entity + off); }
static uint32_t lo(uint32_t a) { return a; }

/* the log has these writes in this order (each found after the one before) */
static void order(const char *what, const uint32_t (*w)[2], int n)
{
    int at = 0, i;
    for (i = 0; i < n; i++) {
        at = bb_model_find(at, w[i][0], w[i][1]);
        if (at < 0) {
            char m[200];
            snprintf(m, sizeof m, "%s: write %d (%05x <- %x) missing or out of order", what, i, (unsigned)w[i][0], (unsigned)w[i][1]);
            check(0, m);
            return;
        }
        at++;
    }
    check(1, what);
}

static void power_up(void)
{
    fm1ble_cal cal = { 1, { 0x70, 0x71, 0x05 }, 0x123, 0x2AA };
    fm1ble_adv a;
    uint32_t n, ok;

    memset(&a, 0, sizeof a);
    bb_model_reset();
    memset(&B, 0, sizeof B);
    check(fm1ble_bb_init(&B, blk) == FM1BLE_ESTATE, "bb_init before radio_up: ESTATE");
    check(fm1ble_radio_up(&B, 0, NULL) == FM1BLE_OK, "radio_up");
    check(bbm.rf_common_calls == 0, "variant 0 does not call the shared radio's init");
    check(B.d.rb_ok == 8 && B.d.rb_bad == 0, "all eight readbacks read as written (the model's registers are memory)");
    check(bbm.clk_calls == 1 && bbm.clk_bits == 0, "CLK_CON1's bits 15-14 cleared once");
    check(bbm.now_us >= 1070, "the delays of B.1 (4 x 240 us), C.3 (100) and C.1 (10) ran");
    {   /* B.2 first, then B.1 steps 4-17, C.3 before C.1's constants, in the document's order */
        static const uint32_t w[][2] = {
            { FM1BLE_WL_CON1, 5u << 4 | 0x98u << 16 | 1u }, { FM1BLE_BSFR_50, 0x87 }, { FM1BLE_BSFR_50, 0xAC }, { FM1BLE_IOMAP, 0x101 },
            { FM1BLE_BSFR_BASE, 0xC0000000u }, { FM1BLE_BT_CLKCON, 0xFFFF }, { FM1BLE_BT_CLKCON, 0xFCFD },
            { FM1BLE_BT_ENA, 0xF }, { FM1BLE_BT_ENA, 0x3FF },
            { FM1BLE_AGC_TAB_AT, 0 }, { FM1BLE_AGC_TAB, 0x008000B6u }, { FM1BLE_AGC_TAB, 0x000000F0u },
            { FM1BLE_AGC_0, 0x000F0000u }, { FM1BLE_AGC_0 + 4, 0x1872BF14u }, { FM1BLE_AGC_0 + 8, 0x1872BF55u },
            { FM1BLE_ANA(0x48), 0xB9 }, { FM1BLE_ANA(0x00), 0x00FFD144u }, { FM1BLE_ANA(0x04), 0x4E143CDFu },
            { FM1BLE_ANA(0x08), 0x03100000u }, { FM1BLE_ANA(0x0C), 0x80808080u }, { FM1BLE_ANA(0x10), 0x80008080u },
            { FM1BLE_ANA(0x20), 0x36555537u }, { FM1BLE_ANA(0x24), 0x00034041u }, { FM1BLE_ANA(0x28), 0x0005C000u },
        };
        order("power-up writes in the order of B.2, B.1, C.3 (the gain table first), C.1", w, (int)(sizeof w / sizeof w[0]));
    }
    check(bbm.agc_n == (int)FM1BLE_AGC_WORDS && bbm.agc_bad == 0 && B.d.agc_table == 1 && FM1BLE_AGC_TABLE[0] == 0x008000B6u,
          "the AGC gain table: 0x2FD98 = 0, then fm1ble_agc.h's 128 words to 0x2FD9C (btagc_ram_init's entry 63 first)");
    check(bb_model_reg(FM1BLE_WL_CON0) == ((2u << 24) | (0x0Cu << 16) | 0x181u), "WL_CON0: B.2's field and bit 8, B.1's bit 7, field 20-16 and bit 0");
    check(bb_model_reg(FM1BLE_BT_PWRCON) == (0x8u | 0x2u), "0x2FC50: field 5-2 = 2, bit 1");
    check(bb_model_reg(FM1BLE_ANA(0x98)) == 0x02360236u && bb_model_reg(FM1BLE_ANA(0x9C)) == 0x36360008u &&
          bb_model_reg(FM1BLE_ANA(0xA0)) == 0x3636u, "the modem's fields at 0x2FC98, 0x2FC9C and 0x2FCA0");
    check(bb_model_reg(FM1BLE_ANA(0x10)) == 0x80008080u && bb_model_reg(FM1BLE_ANA(0x08)) == 0x03100000u,
          "without calibration the constants of C.1 stand");
    for (n = 0, ok = 1; n < 81; n++)             /* F.2 */
        ok &= B.ana_pll[n * 3] == ((n << 8) | n) && B.ana_pll[n * 3 + 1] == 0 && B.ana_pll[n * 3 + 2] == 0;
    check(ok, "the PLL table: 81 entries of (n << 8 | n, 0, 0)");
    check(bb_model_reg(FM1BLE_ANA_PLLTAB) == lo((uint32_t)(uintptr_t)B.ana_pll), "0x2FC88 holds the table's address");
    check(fm1ble_radio_up(&B, 1, &cal) == FM1BLE_OK && bbm.rf_common_calls == 1 && B.d.rf_common == 1,
          "variant 1 calls the shared radio's init first");
    check((bb_model_reg(FM1BLE_ANA(0x10)) & 0xFFFFFFu) == 0xFB7170u, "calibration: compensation 0x70, 0x71 and -5 as 0xFB");
    check((bb_model_reg(FM1BLE_ANA(0x08)) & 0xFFFFFu) == (0x123u | 0x2AAu << 10), "calibration: offsets 0x123 and 0x2AA");

    /* the baseband and its RAM block */
    check(fm1ble_bb_init(&B, NULL) == FM1BLE_EARG && fm1ble_bb_init(&B, (uint8_t *)blk + 1) == FM1BLE_EARG, "bb_init refuses NULL and a misaligned block");
    check(fm1ble_adv_start(&B, &a) == FM1BLE_ESTATE, "adv_start before bb_init: ESTATE");
    check(fm1ble_bb_init(&B, blk) == FM1BLE_OK, "bb_init");
    {
        uint32_t used = 0x10u + FM1BLE_ENTITY_SIZE + 2u * FM1BLE_RX_SIZE + 2u * FM1BLE_TX_SIZE;
        check(bb_model_reg(FM1BLE_BB_RAM_START) == lo((uint32_t)(uintptr_t)blk), "0x2FC84: the block's address");
        check(bb_model_reg(FM1BLE_BB_RAM_END) == lo((uint32_t)(uintptr_t)blk) + used, "0x2FCBC: the end of what is used");
        check(used <= FM1BLE_BLOCK_SIZE && used < 0xFFFFu, "the block fits and every offset is below 64 KB");
    }
    check(bb_model_reg(FM1BLE_BB_CON0) == (1u | 1u << 13 | 1u << 14) && bb_model_reg(FM1BLE_BB_T34) == 0x0A01u,
          "0x28000 = 1, bits 13 and 14 set, bit 4 clear; 0x28034 = 0x0A01");
    check(bb_model_reg(FM1BLE_BB_T08) == 0xF046u && bb_model_reg(FM1BLE_BB_T0C) == 0x46u && bb_model_reg(FM1BLE_BB_T10) == 0x3E3Eu &&
          bb_model_reg(FM1BLE_BB_T14) == 0x1Eu && bb_model_reg(FM1BLE_BB_T18) == 0x1414u, "D.1's timing words");
    check(bb_model_reg(FM1BLE_BB_RAMWIN_EN) & 1u, "0x2FC80 bit 0");
    check(((const uint8_t *)blk)[0] == 0x10 && ((const uint8_t *)blk)[1] == 0x80, "the instance table: entity 0 at offset 0x10, tagged 0x8000");
    check(e16(FM1BLE_E_ADVIDX) == 0xD388u && e16(FM1BLE_E_BDADDR0) == 0xBED6u && e16(FM1BLE_E_BDADDR1) == 0x8E89u &&
          e16(FM1BLE_E_OPTCNTL) == 0x228u && e16(FM1BLE_E_CRCWORD0) == 0x5555u && e16(FM1BLE_E_CRCWORD1) == 0x55u &&
          e16(FM1BLE_E_WIDEN1) == 0x51E7u && e16(FM1BLE_E_IFSCNT) == 0x8295u && e16(FM1BLE_E_RFPRIOCNTL) == 0x101u &&
          e16(FM1BLE_E_RFPRIOSTAT) == 1 && e16(FM1BLE_E_RXMAXBUF) == 0xFFu && e16(FM1BLE_E_TXPTR0) == 1 && e16(FM1BLE_E_TXDHDR1) == 1,
          "the entity at open: E.3's values (advertising access address 0x8E89BED6, CRC init 0x555555, ...)");
    check(e16(FM1BLE_E_TXPWR0) == 0x16u && e16(FM1BLE_E_TXPWR1) == 0x16u && B.txpwr == 6,
          "TX power: 0x10 + 6, the SDK's BLE level (0x10, level 0, is the lowest: the first FM-1 run's)");
    check(bb_model_reg(FM1BLE_BB_RAM_END) == B.d.ram_end, "ram_end: 0x2FCBC as read back");
    {
        const uint8_t *e = B.blk + B.entity;
        check(e[FM1BLE_E_CHTAB2 + 0] == 2 && e[FM1BLE_E_CHTAB2 + 10] == 22 && e[FM1BLE_E_CHTAB2 + 11] == 26 && e[FM1BLE_E_CHTAB2 + 36] == 76,
              "F.1: data channel 0 is 2404 MHz (offset 2), 10 is 22, 11 jumps to 26, 36 is 76");
        check(e[FM1BLE_E_CHTAB0 + 5] == 5 && e[FM1BLE_E_CHTAB0 + 37] == 37 && e[FM1BLE_E_CHTAB2 + 37] == 0 &&
              e[FM1BLE_E_CHTAB2 + 38] == 24 && e[FM1BLE_E_CHTAB2 + 39] == 78 && e[FM1BLE_E_CHTAB3 + 39] == 78,
              "F.1: advertising channels 37-39 at 2402, 2426 and 2480 MHz (offsets 0, 24, 78)");
        check(e16(FM1BLE_E_CHMAP0) == 0xFFFFu && e16(FM1BLE_E_CHMAP2) == (37u << 5 | 0x1Fu), "all 37 data channels in the map");
    }
    check(bbm.ax[FM1BLE_AX_CNT_HI] == 0 && !bbm.armed, "the link is left in standby: counter off");
    check((bb_model_reg(FM1BLE_BB_IRQ_EN) & 0x101u) == 0, "interrupts off");
}

static const uint8_t ADDR[6] = { 0x01, 0x00, 0x31, 0x4D, 0x46, 0xC2 };

static void make_adv(fm1ble_adv *a, uint8_t type)
{
    memset(a, 0, sizeof *a);
    a->type = type;
    a->own_random = 1;
    a->chmap = 7;
    memcpy(a->addr, ADDR, 6);
    a->interval_slots = 160;
    a->nadv = (uint8_t)ble_midi_adv_data(a->adv);
    a->nrsp = (uint8_t)ble_midi_scan_rsp(a->rsp, "OPLBOX");
}

static void advertising(void)
{
    fm1ble_adv a;
    const uint8_t *t0, *t1;
    int i;

    make_adv(&a, 1);
    check(fm1ble_adv_start(&B, &a) == FM1BLE_EARG, "type 1 (ADV_DIRECT_IND) is refused");
    make_adv(&a, 0); a.interval_slots = 31;
    check(fm1ble_adv_start(&B, &a) == FM1BLE_EARG, "an interval under 20 ms is refused");
    make_adv(&a, 0); a.interval_slots = 16385;
    check(fm1ble_adv_start(&B, &a) == FM1BLE_EARG, "an interval over 10.24 s is refused");
    make_adv(&a, 0); a.nadv = 32;
    check(fm1ble_adv_start(&B, &a) == FM1BLE_EARG, "data over 31 bytes is refused");
    make_adv(&a, 0); a.filter = 4;
    check(fm1ble_adv_start(&B, &a) == FM1BLE_EARG, "a filter policy over 3 is refused");
    check(fm1ble_adv_start(&B, NULL) == FM1BLE_EARG, "NULL is refused");
    check(fm1ble_adv_stop(&B) == FM1BLE_ESTATE, "stop when not advertising: ESTATE");

    make_adv(&a, 0);
    bbm.nlog = 0;
    check(fm1ble_adv_start(&B, &a) == FM1BLE_OK, "adv_start (ADV_IND, 100 ms)");
    check(fm1ble_adv_start(&B, &a) == FM1BLE_ESTATE, "adv_start while advertising: ESTATE");
    t0 = B.blk + B.tx[0];
    t1 = B.blk + B.tx[1];
    check(t0[0x10] == 0x40 && t0[0x11] == 0 && t0[0x12] == a.nadv + 6u, "buffer 0: header ADV_IND with TxAdd, length data + 6");
    check(!memcmp(t0 + 0x14, ADDR, 6) && !memcmp(t0 + 0x1A, a.adv, a.nadv), "buffer 0: the advertiser address, then the data");
    check(t1[0x10] == 0x44 && t1[0x12] == a.nrsp + 6u && !memcmp(t1 + 0x14, ADDR, 6) && !memcmp(t1 + 0x1A, a.rsp, a.nrsp),
          "buffer 1: SCAN_RSP (type 4, TxAdd), the address, the scan response data");
    check(e16(FM1BLE_E_TXPTR0) == B.tx[0] + 0x14u && e16(FM1BLE_E_TXPTR1) == B.tx[1] + 0x14u, "TXPTR0 and TXPTR1: the payloads' offsets");
    check(e16(FM1BLE_E_TXAHDR0) == 0x10u && e16(FM1BLE_E_TXAHDR1) == (((0x44u >> 2) & 0x30u) | 4u), "TXAHDR: ((hdr >> 2) & 0x30) | (hdr & 0xF)");
    check(e16(FM1BLE_E_TXDHDR0) == (a.nadv + 6u) << 8 && e16(FM1BLE_E_TXDHDR1) == (a.nrsp + 6u) << 8,
          "TXDHDR: the PDU's length in bits 15-8 for both advertising buffers (it stayed 1, length 0, at 48ea034)");
    check(e16(FM1BLE_E_RXPTR0) == B.rx[0] + 0x14u && e16(FM1BLE_E_RXPTR1) == B.rx[1] + 0x14u, "RXPTR0 and RXPTR1");
    check(e16(FM1BLE_E_LOCALADR) == 0x0001u && e16(FM1BLE_E_LOCALADR + 2) == 0x4D31u && e16(FM1BLE_E_LOCALADR + 4) == 0xC246u,
          "LOCALADR: the address as three little-endian words");
    check((e16(FM1BLE_E_FORMAT) & 0xCu) == 0xCu, "FORMAT bits 3 (local address) and 2 (advertising started)");
    check(e16(FM1BLE_E_WINCNTL0) == 0x32u && e16(FM1BLE_E_WINCNTL1) == 0 && e16(FM1BLE_E_WINCNTL2) == 0x32u && e16(FM1BLE_E_FILTERCNTL) == 0,
          "windows: WINCNTL0 0x32, WINCNTL2 0x32 (le_hw_advertising's; 0 up to 3b39ab4), no filter");
    check((e16(FM1BLE_E_OPTCNTL) & 0x10u) == 0, "OPTCNTL bit 4 clear: local address match on");
    check(bbm.ax[FM1BLE_AX_INTERVAL] == 160 && bbm.ax[FM1BLE_AX_INTERVAL_HI] == 0x8000u, "anchor 1 and 0xF: 160 slots, event enable");
    check(bbm.ax[FM1BLE_AX_STATE] == 0x2000u, "anchor 2: state 2 (advertising), no latency");
    check(bbm.ax[FM1BLE_AX_RANDSLOT] == 0xC000u, "anchor 8: random delay (interval over 10 slots, type not NONCONN)");
    check(bbm.ax[FM1BLE_AX_CHANNEL] == (0x4000u | 1u << 8 | 1u << 7 | 37u), "anchor 6: hop 1, channel 39 enabled, first channel 37");
    check(bbm.ax[FM1BLE_AX_CNT_LO] == 0 && bbm.ax[FM1BLE_AX_CNT_HI] == 0x8000u && bbm.armed, "the first event armed for the next slot");
    check((bb_model_reg(FM1BLE_BB_IRQ_EN) & 0x101u) == 0x101u, "interrupt enable bits 0 and 8");
    {   /* G's order: the interval, the state, the channel index, the interrupt clear and enable, then the counter */
        int p_int = -1, p_state = -1, p_chan = -1, p_en = -1, p_cnt = -1;
        for (i = 0; i < bbm.nlog; i++) {
            if (bbm.log[i].addr != FM1BLE_BB_ANCHOR_CMD) {
                if (bbm.log[i].addr == FM1BLE_BB_IRQ_EN && p_en < 0) p_en = i;
                continue;
            }
            if ((bbm.log[i].val >> 10) == FM1BLE_AX_INTERVAL && p_int < 0) p_int = i;
            if ((bbm.log[i].val >> 10) == FM1BLE_AX_STATE && p_state < 0) p_state = i;
            if ((bbm.log[i].val >> 10) == FM1BLE_AX_CHANNEL && p_chan < 0) p_chan = i;
            if ((bbm.log[i].val >> 10) == FM1BLE_AX_CNT_HI) p_cnt = i;
        }
        check(p_int >= 0 && p_int < p_state && p_state < p_chan && p_chan < p_en && p_en < p_cnt, "G's order: interval, state, channel, interrupts, counter");
    }
    check(fm1ble_adv_stop(&B) == FM1BLE_OK, "stop");
    check(!bbm.armed && bbm.ax[FM1BLE_AX_CNT_HI] == 0 && (bb_model_reg(FM1BLE_BB_IRQ_EN) & 0x101u) == 0, "stopped: counter off, interrupts off");

    /* TXTOG left odd by the hardware (an earlier link): the advertising PDU goes into slot 1, the scan response into 0 */
    B.blk[B.entity + FM1BLE_E_TXTOG] = 1;
    fm1ble_op_power(&B, 30);
    check(B.txpwr == FM1BLE_TXPWR_MAX && e16(FM1BLE_E_TXPWR0) == 0x10u + FM1BLE_TXPWR_MAX, "op_power: at most level 11");
    check(fm1ble_adv_start(&B, &a) == FM1BLE_OK && e16(FM1BLE_E_TXPTR1) == B.tx[0] + 0x14u && e16(FM1BLE_E_TXPTR0) == B.tx[1] + 0x14u &&
          e16(FM1BLE_E_TXDHDR1) == (a.nadv + 6u) << 8 && e16(FM1BLE_E_TXAHDR0) == 0x14u && B.d.tx_flips == 1,
          "TXTOG odd: ADV_IND in slot 1, SCAN_RSP in slot 0, the change counted");
    fm1ble_adv_stop(&B);
    B.blk[B.entity + FM1BLE_E_TXTOG] = 0;
    fm1ble_op_power(&B, FM1BLE_TXPWR_LEVEL);

    /* ADV_NONCONN_IND: no random delay, no second buffer */
    make_adv(&a, 2);
    bbm.ax[FM1BLE_AX_RANDSLOT] = 0;
    check(fm1ble_adv_start(&B, &a) == FM1BLE_OK && bbm.ax[FM1BLE_AX_RANDSLOT] == 0, "ADV_NONCONN_IND: no random delay");
    check(B.blk[B.tx[0] + 0x10] == 0x42, "its header is type 2 with TxAdd");
    fm1ble_adv_stop(&B);
    /* the filter policy and a single channel */
    make_adv(&a, 0); a.filter = 3; a.chmap = 2;
    check(fm1ble_adv_start(&B, &a) == FM1BLE_OK && e16(FM1BLE_E_FILTERCNTL) == 0x18u, "filter policy 3: FILTERCNTL bits 3 and 4");
    check(bbm.ax[FM1BLE_AX_CHANNEL] == (0x4000u | 1u << 8 | 1u << 6 | 38u), "channel 38 only: bit 6 (37 off), no bit 7, first channel 38");
    fm1ble_adv_stop(&B);
    /* a stop that waits: the hardware stays busy */
    make_adv(&a, 0);
    fm1ble_adv_start(&B, &a);
    bbm.never_idle = 1;
    check(fm1ble_adv_stop(&B) == FM1BLE_ETIMEOUT, "stop with the baseband busy for good: ETIMEOUT, bounded");
    bbm.never_idle = 0;
}

static void polling(void)
{
    fm1ble_adv a;
    uint8_t scan[12] = { 0xC0, 0xFF, 0xEE, 0, 0, 1, 0x01, 0x00, 0x31, 0x4D, 0x46, 0xC2 };
    uint8_t conn[34], n;
    const uint8_t *p;
    uint8_t type;
    int i;

    bb_model_reset();
    memset(&B, 0, sizeof B);
    fm1ble_radio_up(&B, 0, NULL);
    fm1ble_bb_init(&B, blk);
    make_adv(&a, 0);
    check(fm1ble_poll(&B) == 0, "poll when not advertising: nothing");
    fm1ble_adv_start(&B, &a);

    /* the hardware repeats events by itself: one flag per 100 ms (after the first, a slot in) */
    bbm.repeat = 1;
    for (i = 0; i < 1000; i++) {
        bb_model_advance_us(1000);
        fm1ble_poll(&B);
    }
    check(B.d.events >= 8 && B.d.events <= 11 && B.d.rearms == 0, "repeating hardware: about ten events in a second, none re-armed by the driver");
    check(B.d.tx_flips == B.d.events && B.d.tx_flips == bbm.tx_sent, "a TXTOG change after every event, counted");
    {
        uint32_t sl = e16(FM1BLE_E_TXTOG) & 1u;
        check(e16(sl ? FM1BLE_E_TXPTR1 : FM1BLE_E_TXPTR0) == B.tx[0] + 0x14u && e16(sl ? FM1BLE_E_TXPTR0 : FM1BLE_E_TXPTR1) == B.tx[1] + 0x14u,
              "after each event the ADV_IND is in the slot TXTOG names, the scan response in the other (JieLi's event handler)");
    }
    check(bb_model_reg(FM1BLE_BB_IRQ_PEND) == 0, "every flag cleared");

    /* a received packet: SCAN_REQ in buffer 0, CONNECT_IND in buffer 1 */
    bb_model_rx(0, 3, scan, 12);
    check(fm1ble_poll(&B) == 1 && B.d.rx_packets == 1 && B.d.rx_scan_req == 1, "a SCAN_REQ is counted");
    p = fm1ble_rx_last(&B, &n, &type);
    check(type == 3 && n == 12 && !memcmp(p, scan, 12), "its payload is kept");
    for (i = 0; i < 34; i++)
        conn[i] = (uint8_t)(0x20 + i);
    bb_model_rx(1, 5, conn, 34);
    check(fm1ble_poll(&B) == 1 && B.d.rx_conn_ind == 1, "a CONNECT_IND in the other buffer is counted");
    p = fm1ble_rx_last(&B, &n, &type);
    check(type == 5 && n == 34 && !memcmp(p, conn, 34) && B.d.rx_last_len == 34, "its 34 payload bytes are kept");
    bb_model_rx(0, 2, scan, 6);
    fm1ble_poll(&B);
    check(B.d.rx_other == 1 && B.d.rx_packets == 3, "another PDU type is counted as other");
    check(e16(FM1BLE_E_RXSTAT0) == 0 && e16(FM1BLE_E_RXSTAT1) == 0, "the status words are cleared after a read");

    /* hardware that raises one flag per arming: the driver re-arms when asked */
    fm1ble_adv_stop(&B);
    bb_model_reset();
    memset(&B, 0, sizeof B);
    fm1ble_radio_up(&B, 0, NULL);
    fm1ble_bb_init(&B, blk);
    fm1ble_adv_start(&B, &a);
    for (i = 0; i < 1000; i++) {
        bb_model_advance_us(1000);
        fm1ble_poll(&B);
    }
    check(B.d.events == 1 && B.d.rearms == 0, "one flag per arming, no re-arm by default: one event");
    fm1ble_set_rearm(&B, 1);
    bbm.armed = 0;
    bbm.now_us = 0;
    bb_model_advance_us(1);
    bbm.ax[FM1BLE_AX_CNT_LO] = 0;
    {   /* arm again as the driver does, and let it run on its own re-arms */
        uint32_t ev0 = B.d.events;
        fm1ble_set_rearm(&B, 1);
        bbm.armed = 1;
        bbm.armed_at_us = bbm.now_us;
        bbm.period_us = 625;
        for (i = 0; i < 1000; i++) {
            bb_model_advance_us(1000);
            fm1ble_poll(&B);
        }
        check(B.d.events - ev0 >= 8 && B.d.rearms == B.d.events - ev0, "re-arming: each event arms the next, 160 slots on");
    }
    fm1ble_radio_down(&B);
    check(bb_model_reg(FM1BLE_WL_CON0) == 0 && bbm.clk_bits == 0xC000u, "radio_down: WL_CON0 = 0, the clock bits back on");
    check(!B.adv_on && !B.on, "radio_down also stops advertising");
}

/* the receive setup (fm1ble_rx_setup) and the windows of the one-packet operations */
static void rx_setup(void)
{
    fm1ble_adv a;
    static const uint8_t pdu[8] = { 0x40, 6, 0x01, 0x00, 0x31, 0x4D, 0x46, 0xC2 };
    uint32_t v;

    bb_model_reset();
    memset(&B, 0, sizeof B);
    fm1ble_rx_setup(&B, 0, 0x96);
    fm1ble_radio_up(&B, 0, NULL);
    check(bb_model_nth_write(FM1BLE_AGC_TAB, 0, &v) < 0 && bb_model_nth_write(FM1BLE_AGC_TAB_AT, 0, &v) < 0 && B.d.agc_table == 0,
          "rx_setup without the table: the power-up writes none of it");
    fm1ble_bb_init(&B, blk);
    make_adv(&a, 0);
    check(fm1ble_adv_start(&B, &a) == FM1BLE_OK && e16(FM1BLE_E_WINCNTL2) == 0x96u, "rx_setup's window in WINCNTL2 (0x96)");
    fm1ble_adv_stop(&B);
    fm1ble_rx_setup(&B, 1, 0);
    check(fm1ble_adv_start(&B, &a) == FM1BLE_OK && e16(FM1BLE_E_WINCNTL2) == 0, "and 0, as the driver had it up to 3b39ab4");
    fm1ble_adv_stop(&B);
    fm1ble_radio_down(&B);
    check(fm1ble_radio_up(&B, 0, NULL) == FM1BLE_OK && B.d.agc_table == 1 && bbm.agc_n == (int)FM1BLE_AGC_WORDS && !bbm.agc_bad,
          "rx_setup with the table: the next power-up writes it");

    /* without rx_setup: the operations' windows as JieLi writes them */
    bb_model_reset();
    memset(&B, 0, sizeof B);
    fm1ble_radio_up(&B, 0, NULL);
    fm1ble_bb_init(&B, blk);
    check(fm1ble_op_chan(&B, FM1BLE_ACCESS_ADV, 0x555555u, 37) == FM1BLE_OK && fm1ble_op_adv(&B, 1000, pdu, 8, NULL, 0) == FM1BLE_OK &&
          e16(FM1BLE_E_WINCNTL0) == 0x32u && e16(FM1BLE_E_WINCNTL2) == FM1BLE_ADV_WIN_US, "op_adv: WINCNTL0 and WINCNTL2 0x32");
    fm1ble_op_stop(&B);
    check(fm1ble_op_chan(&B, 0x12345678u, 0x3A5C21u, 5) == FM1BLE_OK && fm1ble_op_conn(&B, 1000, 3750) == FM1BLE_OK &&
          e16(FM1BLE_E_WINCNTL0) == 0x32u && e16(FM1BLE_E_WINCNTL1) == 0 && e16(FM1BLE_E_WINCNTL2) == 3750u,
          "op_conn: WINCNTL0 0x32, the window in WINCNTL2 (le_hw_ioctrl's __set_winsize; it went into WINCNTL0 up to 3b39ab4)");
    fm1ble_op_stop(&B);
    check(fm1ble_op_conn(&B, 1000, 70000) == FM1BLE_OK && e16(FM1BLE_E_WINCNTL2) == 0xFFFFu, "a window over 16 bits: 65535");
    fm1ble_op_stop(&B);
    /* the IR's three (fm1ble.h FM1BLE_CONN_PADJ, PADB, PADW), at 2000 us (the padded window 625 us earlier: 1375 = 2 slots
     * and 125 us) and a 52 us window: the anchor file as __set_anchor_cnt writes it (the bit offset, the count cleared,
     * then the slots minus one with the enable last), bit 15 on the offset from padb, the window in WINCNTL0 in padw */
    {
        static const struct { uint8_t setup; uint16_t bitoff, win0; const char *what; } J[3] = {
            { FM1BLE_CONN_PADJ, 125u, 0x32u, "padj: the slots minus one (1), the bit offset first, WINCNTL0 0x32" },
            { FM1BLE_CONN_PADB, 0x8000u | 125u, 0x32u, "padb: as padj, the bit offset with bit 15" },
            { FM1BLE_CONN_PADW, 0x8000u | 125u, 52u + 1250u, "padw: as padb, the window (1302 us) in WINCNTL0 too" },
        };
        int i, j;
        for (i = 0; i < 3; i++) {
            int at_bo = -1, at_lo0 = -1, at_hi0 = -1, at_lo = -1, at_hi = -1;
            fm1ble_op_conn_setup(&B, J[i].setup);
            bbm.nlog = 0;
            check(fm1ble_op_conn(&B, 2000, 52) == FM1BLE_OK, J[i].what);
            for (j = 0; j + 1 < bbm.nlog; j++) {         /* the anchor writes: 0x28020 = value, then 0x2801C = index << 10 | 5 */
                uint32_t idx = bbm.log[j + 1].val >> 10 & 15u, v = bbm.log[j].val;
                if (bbm.log[j].addr != FM1BLE_BB_ANCHOR_WR || bbm.log[j + 1].addr != FM1BLE_BB_ANCHOR_CMD)
                    continue;
                if (idx == FM1BLE_AX_BITOFF && at_bo < 0 && v == J[i].bitoff)
                    at_bo = j;
                else if (idx == FM1BLE_AX_CNT_LO && v == 0 && at_lo0 < 0)
                    at_lo0 = j;
                else if (idx == FM1BLE_AX_CNT_HI && v == 0 && at_hi0 < 0)
                    at_hi0 = j;
                else if (idx == FM1BLE_AX_CNT_LO && v == 1u)
                    at_lo = j;
                else if (idx == FM1BLE_AX_CNT_HI && v == 0x8000u)
                    at_hi = j;
            }
            check(at_bo >= 0 && at_lo0 > at_bo && at_hi0 > at_lo0 && at_lo > at_hi0 && at_hi > at_lo && bbm.ax[FM1BLE_AX_CNT_LO] == 1u &&
                  e16(FM1BLE_E_WINCNTL0) == J[i].win0 && e16(FM1BLE_E_WINCNTL1) == 0 && e16(FM1BLE_E_WINCNTL2) == 52u + 1250u &&
                  e16(FM1BLE_E_WIDEN0) == 0 && e16(FM1BLE_E_WIDEN1) == 0x5000u, J[i].what);
            fm1ble_op_stop(&B);
        }
        fm1ble_op_conn_setup(&B, FM1BLE_CONN_PAD);
        check(fm1ble_op_conn(&B, 2000, 52) == FM1BLE_OK && bbm.ax[FM1BLE_AX_CNT_LO] == 2u && bbm.ax[FM1BLE_AX_BITOFF] == 125u &&
              e16(FM1BLE_E_WINCNTL0) == 0x32u, "pad as before: the slots themselves (2), the bit offset without bit 15, WINCNTL0 0x32");
        fm1ble_op_stop(&B);
        /* advertising after a connection event: the open's widening again (padw left 0x5000, no widening) */
        check(fm1ble_op_chan(&B, FM1BLE_ACCESS_ADV, 0x555555u, 37) == FM1BLE_OK && fm1ble_op_adv(&B, 1000, pdu, 8, NULL, 0) == FM1BLE_OK &&
              e16(FM1BLE_E_WIDEN0) == 0 && e16(FM1BLE_E_WIDEN1) == 0x51E7u && e16(FM1BLE_E_WINCNTL0) == 0x32u,
              "op_adv after a connection event: WIDEN back to the open's 0 and 0x51E7, WINCNTL0 0x32");
        fm1ble_op_stop(&B);
        /* the anchor file read back (D.2): 0x2801C = index << 10 | 2, the value from 0x28024 */
        bbm.ax[FM1BLE_AX_BITOFF] = 0x1234u;
        check(fm1ble_anchor_rd(&B, FM1BLE_AX_BITOFF) == 0x1234u && bb_model_reg(FM1BLE_BB_ANCHOR_CMD) == (FM1BLE_AX_BITOFF << 10 | 2u),
              "anchor_rd: the read command, the value from 0x28024");
    }
}

/* ---- the test run ---- */

static uint32_t g_t;
static fm1ble_run R;

static void run_to_end(uint32_t wait_ms, void (*inject)(uint32_t t))
{
    uint32_t i;
    g_t = 0;
    fm1ble_test_begin(&R, g_t, wait_ms);
    for (i = 0; i < 20000; i++) {
        g_t += 5;
        bb_model_advance_us(5000);
        if (inject)
            inject(g_t);
        if (fm1ble_test_poll(&R, g_t))
            return;
    }
}

static int g_injected;

/* a CONNECT_IND of made-up bytes once the stack advertises, while one of its operations is armed */
static void inject_conn(uint32_t t)
{
    static uint8_t conn[34];
    int i;
    (void)t;
    if (R.step[FM1BLE_STEP_HCI].result == FM1BLE_PASS && bbm.armed && !g_injected) {
        g_injected = 1;
        for (i = 0; i < 34; i++)
            conn[i] = (uint8_t)(0x40 + i);
        bb_model_rx(0, 5, conn, 34);
    }
}

static uint32_t raw16(int step, int k) { return R.step[step].raw[2 * k] | R.step[step].raw[2 * k + 1] << 8; }

/* a SCAN_REQ once, while the RX trial advertising from C2:46:4D:31:00:<g_scan_to> has an event armed */
static uint8_t g_scan_to;
static int g_scanned;
static void inject_scan(uint32_t t)
{
    static const uint8_t scan[12] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC6, 0, 0, 0x31, 0x4D, 0x46, 0xC2 };
    const uint8_t *b = (const uint8_t *)(bbm.hi | bb_model_reg(FM1BLE_BB_RAM_START)), *e;
    (void)t;
    if (g_scanned || !bbm.armed || !bb_model_reg(FM1BLE_BB_RAM_START) || bbm.ax[FM1BLE_AX_STATE] >> 12 != FM1BLE_STATE_ADV)
        return;
    e = b + (b[0] | (b[1] & 0x7F) << 8);
    if (e[FM1BLE_E_LOCALADR] == g_scan_to && e[FM1BLE_E_LOCALADR + 2] == 0x31) {
        g_scanned = 1;
        bb_model_rx(0, 3, scan, 12);
    }
}
static uint32_t rxt(int k, int w) { return R.ctr[FM1BLE_C_RXT + 4 * k + w]; }

static void test_runs(void)
{
    /* the baseband repeats its events and sends (TXTOG changes): every trial shows it */
    bb_model_reset();
    bbm.repeat = 1;
    run_to_end(1000, NULL);
    check(R.state == FM1BLE_DONE, "a run finishes");
    check(R.step[FM1BLE_STEP_RADIO].result == FM1BLE_PASS && R.step[FM1BLE_STEP_RADIO].nraw == 32, "radio: PASS with the eight readbacks as evidence");
    check(R.step[FM1BLE_STEP_BLOCK].result == FM1BLE_PASS && R.step[FM1BLE_STEP_BLOCK].nraw == 32, "block: PASS");
    check(R.step[FM1BLE_STEP_HCI].result == FM1BLE_PASS && R.bdaddr[0] == 0xC2 && R.bdaddr[5] == 0x01,
          "hci: PASS, NimBLE started Cordio over the PAL and read its address (C2:46:4D:31:00:01)");
    check(R.ctr[FM1BLE_C_PAL_ADV_OPS] >= 3 && R.ctr[FM1BLE_C_PAL_STUCK] == 0, "pal counters: advertising operations armed and ended, none stuck");
    check(R.ctr[FM1BLE_C_PAL_DONE_37] >= 1 && R.ctr[FM1BLE_C_PAL_DONE_38] >= 1 && R.ctr[FM1BLE_C_PAL_DONE_39] >= 1,
          "pal counters: operations ended on each advertising channel");
    check(R.step[FM1BLE_STEP_ADV_SET].result == FM1BLE_PASS && R.step[FM1BLE_STEP_ADV_SET].nraw == 21, "adv_set: PASS, the 21 bytes of advertising data");
    check(R.step[FM1BLE_STEP_ADV_AIR].result == FM1BLE_PASS && !strcmp(R.step[FM1BLE_STEP_ADV_AIR].note, "sent, trials 0 1 2"),
          "adv_air: PASS, every trial sent");
    check(R.step[FM1BLE_STEP_ADV_AIR].nraw == 34 && raw16(FM1BLE_STEP_ADV_AIR, 0) >= 25 && raw16(FM1BLE_STEP_ADV_AIR, 1) == raw16(FM1BLE_STEP_ADV_AIR, 0) &&
          raw16(FM1BLE_STEP_ADV_AIR, 3) >= 25 && raw16(FM1BLE_STEP_ADV_AIR, 6) >= 25,
          "adv_air's evidence: per trial events (about 30 in 3 s) and as many TXTOG changes, then the hardware's words");
    check(R.ctr[FM1BLE_C_AIR_TRIALS] == 7 && R.ctr[FM1BLE_C_VARIANT] == 1 && R.ctr[FM1BLE_C_TXPWR] == FM1BLE_TXPWR_LEVEL &&
          R.ctr[FM1BLE_C_RF_COMMON] == 1 && bbm.rf_common_calls == 2,
          "counters: all three trials, the stack on variant 1 at level 6 (the shared init: trial 1, then RX trial 1's power-up)");
    {
        int k, ok = 1;
        for (k = 0; k < FM1BLE_RX_TRIALS; k++)
            ok &= (rxt(k, FM1BLE_RXT_COUNTS) & 0xFFu) >= 25 && ((rxt(k, FM1BLE_RXT_COUNTS) >> 8) & 0xFFu) >= 25 &&
                  (rxt(k, FM1BLE_RXT_COUNTS) >> 16) == 0 && ((rxt(k, FM1BLE_RXT_PKT) >> 16) & 0xFFu) == 0xFFu;
        check(ok, "the four RX trials ran (events and TXTOG changes), nothing received, no buffer named");
    }
    check(R.ctr[FM1BLE_C_RX_STACK_TRIAL] == 2 && R.ctr[FM1BLE_C_RX_AGC_TABLE] == 1 && R.ctr[FM1BLE_C_RX_WIN] == 0x32u &&
          (R.ctr[FM1BLE_C_AGC_CTL] & 1u) && R.ctr[FM1BLE_C_AGC_RSSI] == 0x1872BF00u,
          "nothing received: the stack on RX trial 2's setup (the table, 0x32); the AGC block read back after the trials");
    check(R.step[FM1BLE_STEP_CONNECT].result == FM1BLE_FAIL && strstr(R.step[FM1BLE_STEP_CONNECT].note, "no SCAN_REQ"), "connect: FAIL, nobody came");
    check(R.step[FM1BLE_STEP_MIDI_IN].result == FM1BLE_SKIP && R.step[FM1BLE_STEP_MIDI_OUT].result == FM1BLE_SKIP, "the MIDI steps: SKIP");
    check(R.ctr[FM1BLE_C_REARM_USED] == 0, "no re-arm needed");
    check(bb_model_reg(FM1BLE_WL_CON0) == 0, "the run leaves the radio off");

    /* the hardware raises one flag per arming: the run turns re-arming on */
    bb_model_reset();
    run_to_end(0, NULL);
    check(R.step[FM1BLE_STEP_ADV_AIR].result == FM1BLE_PASS && strstr(R.step[FM1BLE_STEP_ADV_AIR].note, "re-armed") &&
          R.ctr[FM1BLE_C_REARM_USED] == 1, "one flag per arming: adv_air PASS, the driver re-arms (counter says so)");
    check(R.step[FM1BLE_STEP_CONNECT].result == FM1BLE_SKIP, "wait 0: connect SKIP");

    /* the baseband only runs after the shared radio's init: trials 1 and 2 */
    bb_model_reset();
    bbm.repeat = 1;
    bbm.needs_common = 1;
    run_to_end(0, NULL);
    check(R.step[FM1BLE_STEP_ADV_AIR].result == FM1BLE_PASS && strstr(R.step[FM1BLE_STEP_ADV_AIR].note, "trials 1 2") && R.ctr[FM1BLE_C_AIR_TRIALS] == 6 &&
          R.ctr[FM1BLE_C_VARIANT] == 1 && R.ctr[FM1BLE_C_RF_COMMON] == 1 && bbm.rf_common_calls == 2,
          "no events without the shared init: trials 1 and 2 send, the stack runs on variant 1");
    check(raw16(FM1BLE_STEP_ADV_AIR, 0) == 0 && raw16(FM1BLE_STEP_ADV_AIR, 3) >= 25, "trial 0's events 0, trial 1's counted");

    /* events, but nothing sent (the first FM-1 run's look): adv_air fails; the stack still runs for its evidence */
    bb_model_reset();
    bbm.repeat = 1;
    bbm.no_tx = 1;
    run_to_end(0, NULL);
    check(R.step[FM1BLE_STEP_ADV_AIR].result == FM1BLE_FAIL && strstr(R.step[FM1BLE_STEP_ADV_AIR].note, "TXTOG never changed") &&
          R.ctr[FM1BLE_C_AIR_TRIALS] == 0 && R.ctr[FM1BLE_C_TX_FLIPS] == 0,
          "events without a TXTOG change: adv_air FAIL (the event timer alone does not pass it)");
    check(raw16(FM1BLE_STEP_ADV_AIR, 0) >= 25 && raw16(FM1BLE_STEP_ADV_AIR, 1) == 0, "its evidence: events, no changes");
    check(R.step[FM1BLE_STEP_HCI].result == FM1BLE_PASS, "the stack still comes up (hci PASS)");

    /* a silent baseband: no trial has an event */
    bb_model_reset();
    bbm.silent = 1;
    run_to_end(1000, NULL);
    check(R.step[FM1BLE_STEP_ADV_AIR].result == FM1BLE_FAIL && strstr(R.step[FM1BLE_STEP_ADV_AIR].note, "any trial"), "silent: adv_air FAIL after every trial");
    check(R.step[FM1BLE_STEP_CONNECT].result == FM1BLE_SKIP && bbm.rf_common_calls == 1 && (R.ctr[FM1BLE_C_RXT] & 0xFFu) == 0,
          "connect SKIP; the shared init was tried once; no RX trial without an event");
    check(R.state == FM1BLE_DONE, "and the run ends");

    /* a central connects: the CONNECT_IND is the evidence */
    bb_model_reset();
    bbm.repeat = 1;
    g_injected = 0;
    run_to_end(5000, inject_conn);
    check(g_injected && R.step[FM1BLE_STEP_CONNECT].result == FM1BLE_FAIL && strstr(R.step[FM1BLE_STEP_CONNECT].note, "CONNECT_IND received"),
          "a CONNECT_IND Cordio refuses (made-up parameters): connect FAIL says it was received");
    check(R.step[FM1BLE_STEP_CONNECT].nraw == 34 && R.step[FM1BLE_STEP_CONNECT].raw[0] == 0x40 && R.step[FM1BLE_STEP_CONNECT].raw[33] == 0x40 + 33,
          "with its payload as raw evidence");
    check(R.ctr[FM1BLE_C_RX_CONN_IND] == 1 && R.ctr[FM1BLE_C_RX_TYPE] == 5 && R.ctr[FM1BLE_C_RX_LEN] == 34, "counters: one CONNECT_IND, type 5, length 34");
    check(R.step[FM1BLE_STEP_MIDI_IN].result == FM1BLE_SKIP, "the MIDI steps need a connection");

    /* a SCAN_REQ in RX trial 2 (:15): its counters say what came in, the stack keeps its setup; then one in trial 3 only */
    bb_model_reset();
    bbm.repeat = 1;
    g_scan_to = FM1BLE_RX_ADDR0 + 2;
    g_scanned = 0;
    run_to_end(0, inject_scan);
    check(g_scanned && ((rxt(2, FM1BLE_RXT_COUNTS) >> 16) & 0xFFu) == 1 && (rxt(2, FM1BLE_RXT_COUNTS) >> 24) == 1 &&
          (rxt(3, FM1BLE_RXT_COUNTS) >> 16) == 0, "a SCAN_REQ in RX trial 2: one packet, one SCAN_REQ, in that trial only");
    check(rxt(2, FM1BLE_RXT_STAT) == (1u | 3u << 16) && (rxt(2, FM1BLE_RXT_PKT) & 0xFFFFFFu) == 12u << 8 && rxt(2, FM1BLE_RXT_BYTES) == 0xA4A3A2A1u,
          "its words: RXSTAT 1, RXAHDR type 3, RXDHDR length 12, buffer 0, the payload's first four bytes");
    check(R.ctr[FM1BLE_C_RX_STACK_TRIAL] == 2, "the stack runs on trial 2's setup");
    bb_model_reset();
    bbm.repeat = 1;
    g_scan_to = FM1BLE_RX_ADDR0 + 3;
    g_scanned = 0;
    run_to_end(0, inject_scan);
    check(g_scanned && ((rxt(3, FM1BLE_RXT_COUNTS) >> 16) & 0xFFu) == 1 && R.ctr[FM1BLE_C_RX_STACK_TRIAL] == 3 && R.ctr[FM1BLE_C_RX_WIN] == 0x96u,
          "a SCAN_REQ in RX trial 3 only: the stack runs on its window (0x96)");

    /* names */
    check(!strcmp(fm1ble_step_name(FM1BLE_STEP_ADV_AIR), "adv_air") && fm1ble_step_name(8) == NULL && fm1ble_ctr_name(FM1BLE_N_CTR) == NULL &&
          !strcmp(fm1ble_ctr_name(FM1BLE_C_EVENTS), "events") && !strcmp(fm1ble_ctr_name(FM1BLE_C_TX_FLIPS), "tx_flips") &&
          !strcmp(fm1ble_ctr_name(FM1BLE_C_RAM_END), "ram_end"), "step and counter names");
}

int main(void)
{
    power_up();
    advertising();
    polling();
    rx_setup();
    test_runs();
    printf("ble_radio: %d checks, %s\n", checks, bad ? "FAILED" : "all passed");
    return bad;
}
