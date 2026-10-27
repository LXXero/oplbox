/* SPDX-License-Identifier: GPL-3.0-only
 * The Bluetooth test run's state machine (fm1ble_test.h). Stage: the steps and their names are fixed; the radio
 * driver and the controller are not linked yet, so every step reports SKIP with the reason. Own code. */
#include <string.h>
#include "fm1ble_test.h"

static const char *const g_steps[FM1BLE_STEPS] = {
    "radio", "block", "hci", "adv_set", "adv_air", "connect", "midi_in", "midi_out"
};

static const char *const g_ctrs[FM1BLE_N_CTR] = {
    "radio_regs_ok", "radio_regs_bad", "bb_irq", "bb_irq_tx", "bb_irq_rx", "hci_cmds", "hci_events", "hci_bad",
    "hci_status", "hci_op", "adv_events", "connects", "disconnects", "acl_in", "acl_out", "att_mtu",
    "interval", "latency", "timeout", "midi_in", "midi_out", "notify_failed", "nocp", "reserved"
};

const char *fm1ble_step_name(int step) { return step >= 0 && step < FM1BLE_STEPS ? g_steps[step] : 0; }
const char *fm1ble_ctr_name(int ctr) { return ctr >= 0 && ctr < FM1BLE_N_CTR ? g_ctrs[ctr] : 0; }

void fm1ble_test_begin(fm1ble_run *r, uint32_t now_ms, uint32_t wait_ms)
{
    (void)wait_ms;
    memset(r, 0, sizeof *r);
    r->state = FM1BLE_RUNNING;
    r->t0 = r->now = now_ms;
}

int fm1ble_test_poll(fm1ble_run *r, uint32_t now_ms)
{
    int i;
    static const char why[] = "controller not built in yet";
    r->now = now_ms;
    for (i = 0; i < FM1BLE_STEPS; i++) {
        r->step[i].result = FM1BLE_SKIP;
        memcpy(r->step[i].note, why, sizeof why);
    }
    r->state = FM1BLE_DONE;
    return 1;
}
