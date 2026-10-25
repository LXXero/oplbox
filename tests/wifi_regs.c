/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of firmware/hal/fm1_wifi_regs.h and fm1_wifi_hal.c (built with -DFM1_WIFI_HOST).
 *   wifi_regs           checks the accessors against a fake bus, then prints "NAME 0xVALUE" for every
 *                       register and mask, which tests/wifi_regs.py compares with docs/WIFI.md.
 * Exits 1 when an accessor check fails.
 */
#include <stdio.h>
#include "fm1_wifi_regs.h"

static uint32_t last_addr, last_val, n_wr, n_rd;
uint32_t fm1_wifi_host_rd(uint32_t addr) { last_addr = addr; n_rd++; return addr ^ 0x5A5A5A5Au; }
void fm1_wifi_host_wr(uint32_t addr, uint32_t v) { last_addr = addr; last_val = v; n_wr++; }

static int bad;
static void check(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

static void wr_hits(void (*wr)(unsigned, uint32_t), unsigned n, uint32_t addr, const char *what)
{
    uint32_t before = n_wr;
    wr(n, 0x12345678u + n);
    check(n_wr == before + 1 && last_addr == addr && last_val == 0x12345678u + n, what);
}

static void wr_refused(void (*wr)(unsigned, uint32_t), unsigned n, const char *what)
{
    uint32_t before = n_wr;
    wr(n, 1);
    check(n_wr == before, what);
}

static void accessors(void)
{
    wr_hits(fm1_wla_wr, 0, 0x11900u, "WLA_CON0 at 0x11900");
    wr_hits(fm1_wla_wr, 12, 0x11930u, "WLA_CON12 (crystal caps) at 0x11930");
    wr_hits(fm1_wla_wr, 30, 0x11978u, "WLA_CON30 at 0x11978");
    wr_refused(fm1_wla_wr, 31, "WLA_CON31 is read-only");
    wr_refused(fm1_wla_wr, 40, "WLA_CON40 does not exist");
    check(fm1_wla_rd(39) == (0x1199Cu ^ 0x5A5A5A5Au) && last_addr == 0x1199Cu, "WLA_CON39 read at 0x1199C");
    { uint32_t before = n_rd; check(fm1_wla_rd(40) == 0 && n_rd == before, "WLA_CON40 read touches nothing"); }

    wr_hits(fm1_wl_wr, 0, 0x14000u, "WL_CON0 at 0x14000");
    wr_hits(fm1_wl_wr, 3, 0x1400Cu, "WL_CON3 at 0x1400C");
    wr_refused(fm1_wl_wr, 8, "WL_CON8 does not exist (LOFC_CON is there)");
    wr_hits(fm1_wl_anl_wr, 0, 0x14028u, "WL_ANL_CON0 at 0x14028");
    wr_hits(fm1_wl_anl_wr, 3, 0x14034u, "WL_ANL_CON3 at 0x14034");
    wr_refused(fm1_wl_anl_wr, 4, "WL_ANL_CON4 does not exist");
    wr_hits(fm1_wf_wr, 0, 0x14040u, "WF_CON0 at 0x14040");
    wr_hits(fm1_wf_wr, 9, 0x14064u, "WF_CON9 at 0x14064");
    wr_refused(fm1_wf_wr, 10, "WF_CON10 does not exist");
    check(fm1_wf_rd(4) == (0x14050u ^ 0x5A5A5A5Au), "WF_CON4 (interrupt) read at 0x14050");

    {
      static const uint32_t allowed[] = {0x00u, 0x04u, 0x10u, 0x14u, 0x18u, 0x1Cu};
      static const uint32_t refused[] = {0x02u, 0x08u, 0x0Cu, 0x20u};
      for (unsigned i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
        uint32_t off = allowed[i], before_wr = n_wr, before_rd = n_rd;
        fm1_wl30_host_wr(off, 7);
        check(n_wr == before_wr + 1 && last_addr == FM1_WL30_HOST(off) && last_val == 7,
              "documented SDIO host offset is writable");
        check(fm1_wl30_host_rd(off) == (FM1_WL30_HOST(off) ^ 0x5A5A5A5Au) &&
              n_rd == before_rd + 1 && last_addr == FM1_WL30_HOST(off),
              "documented SDIO host offset is readable");
      }
      for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        uint32_t off = refused[i], before_wr = n_wr, before_rd = n_rd;
        fm1_wl30_host_wr(off, 7);
        check(n_wr == before_wr, "undocumented SDIO host offset write touches nothing");
        check(fm1_wl30_host_rd(off) == 0 && n_rd == before_rd,
              "undocumented SDIO host offset read touches nothing");
      }
    }

    { uint32_t v = 0xDEADBEEFu, before = n_wr + n_rd;
      check(fm1_wifi_mac_rd(MAC_CSR0, &v) == FM1_WIFI_ENOTYET && v == 0xDEADBEEFu, "mac_rd not built yet");
      check(fm1_wifi_mac_wr(MAC_SYS_CTRL, 3) == FM1_WIFI_ENOTYET, "mac_wr not built yet");
      check(n_wr + n_rd == before, "the SDIO stubs touch no register"); }

    check(PAIRWISE_KEY_ENTRY(255) + 32u == MAC_IVEIV_TABLE_BASE, "pairwise key table: 256 x 32 bytes");
    check(MAC_IVEIV_ENTRY(256) == MAC_WCID_ATTRIBUTE_BASE, "IV/EIV table: 256 x 8 bytes");
    check(MAC_WCID_ATTR_ENTRY(256) == 0x6C00u, "WCID attribute table: 256 x 4 bytes");
    check(SHARED_KEY_ENTRY(16) == 0x6E00u && SHARED_KEY_ENTRY(16) <= SHARED_KEY_MODE_BASE,
          "shared key table: 16 x 32 bytes, below the shared key mode table");
    check(SHARED_KEY_MODE_ENTRY(15) == 0x703Cu, "shared key mode table: 16 x 4 bytes");
}

#define R(x) { #x, (uint32_t)(x) },
static const struct { const char *name; uint32_t v; } regs[] = {
    R(FM1_JL_ANA_BASE) R(FM1_JL_WL_BASE) R(FM1_WL30_HOST_BASE) R(FM1_WL30_HOST_LAST) R(FM1_IRQ_WF)
    R(FM1_WL30_HOST_00) R(FM1_WL30_HOST_04) R(FM1_WL30_HOST_10) R(FM1_WL30_HOST_14) R(FM1_WL30_HOST_18)
    R(FM1_WL30_HOST_1C)
    R(FM1_WLA_CON(0)) R(FM1_WLA_CON(30)) R(FM1_WF_CON(9)) R(FM1_WL_ANL_CON(3))
    R(MAC_CSR0) R(MAC_SYS_CTRL) R(MAC_SYS_CTRL_RESET_CSR) R(MAC_SYS_CTRL_RESET_BBP) R(MAC_SYS_CTRL_ENABLE_TX)
    R(MAC_SYS_CTRL_ENABLE_RX) R(MAC_ADDR_DW0) R(MAC_ADDR_DW1) R(MAX_LEN_CFG)
    R(BBP_CSR_CFG) R(BBP_CSR_CFG_VALUE) R(BBP_CSR_CFG_REGNUM) R(BBP_CSR_CFG_READ_CONTROL) R(BBP_CSR_CFG_BUSY)
    R(BBP_CSR_CFG_BBP_RW_MODE)
    R(PBF_CFG) R(PBF_MAX_PCNT) R(BCN_OFFSET0) R(BCN_OFFSET1) R(TXRXQ_PCNT) R(PBF_DBG)
    R(XIFS_TIME_CFG) R(BKOFF_SLOT_CFG) R(PWR_PIN_CFG) R(TX_SW_CFG0) R(TX_SW_CFG1) R(TX_SW_CFG2)
    R(TXOP_CTRL_CFG) R(TX_RTS_CFG) R(TX_TIMEOUT_CFG) R(TX_RTY_CFG) R(TX_LINK_CFG) R(CCK_PROT_CFG)
    R(OFDM_PROT_CFG) R(MM20_PROT_CFG) R(MM40_PROT_CFG) R(GF20_PROT_CFG) R(GF40_PROT_CFG) R(EXP_ACK_TIME)
    R(RX_FILTER_CFG) R(AUTO_RSP_CFG) R(LEGACY_BASIC_RATE) R(HT_BASIC_RATE)
    R(TX_SEC_CNT0) R(RX_SEC_CNT0) R(TXOP_HLDR_ET)
    R(PAIRWISE_KEY_TABLE_BASE) R(MAC_IVEIV_TABLE_BASE) R(MAC_WCID_ATTRIBUTE_BASE) R(SHARED_KEY_TABLE_BASE)
    R(SHARED_KEY_MODE_BASE)
    R(FM1_WF_REG_0308) R(FM1_WF_REG_030C) R(FM1_WF_REG_0324) R(FM1_WF_REG_0328) R(FM1_WF_REG_0434)
    R(FM1_WF_TX_FIFO) R(FM1_WF_RX_FIFO)
    R(FM1_BBP_CCA_STROBE) R(FM1_BBP_CCA_ADDR) R(FM1_BBP_CCA_DATA)
    R(FM1_BBP_RX11N_STROBE) R(FM1_BBP_RX11N_ADDR) R(FM1_BBP_RX11N_DATA)
    R(FM1_BBP_TXBE_STROBE) R(FM1_BBP_TXBE_ADDR) R(FM1_BBP_TXBE_DATA)
    R(FM1_BBP_RXFE_STROBE) R(FM1_BBP_RXFE_ADDR) R(FM1_BBP_RXFE_DATA)
};

int main(void)
{
    accessors();
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        printf("%s 0x%X\n", regs[i].name, (unsigned)regs[i].v);
    return bad;
}
