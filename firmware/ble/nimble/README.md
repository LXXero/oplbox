# Apache NimBLE host, peripheral subset (Apache-2.0)

The Bluetooth LE host under `../ble_nimble.c` (docs/BLUETOOTH.md "Cordio path as built", step 1): L2CAP, ATT, GATT
(server), GAP (advertising, one connection) and the HCI layer that talks to a controller. The BLE-MIDI profile, the
transport to the controller, the OS port and the configuration are ours (`../ble_nimble.c`, `../ble_nimble_npl.c`,
`../nimble_port/`); this directory is NimBLE's own.

- Source: Apache NimBLE, <https://github.com/apache/mynewt-nimble>, `master` at commit
  `e00b85ccfc49abbaf1731bb3eddaa6340d07ab52` (2026-10-23). Copied from a shallow clone with `cp`, at the same relative paths.
- Licence: Apache-2.0 (`LICENSE`, `NOTICE`, both unchanged). OPLBOX as a whole stays GPL-3.0-only; Apache-2.0 code
  combines with GPL-3.0 (not with GPL-2.0-only). Each source file keeps its own Apache header; `NOTICE` is kept as the
  licence asks.
- **Unchanged**: every file here is byte for byte the upstream's, except that files were left out (below).
  `SHA256SUMS` lists them (`sha256sum` format); `tests/run_host.sh` checks it, so an edit here fails the host tests.

## What was taken

| Here | What |
| --- | --- |
| `nimble/host/src/`, `nimble/host/include/` | the host: HCI, L2CAP (LE signalling), ATT server and client, GATT, GAP, the Security Manager's source (compiled out: `BLE_SM_LEGACY` and `BLE_SM_SC` are 0 in our configuration), the RAM store |
| `nimble/host/services/gap/`, `nimble/host/services/gatt/` | the GAP service (device name, appearance) and the GATT service |
| `nimble/host/util/` | address helpers |
| `nimble/include/nimble/` | `ble.h`, `hci_common.h`, the NPL interface `nimble_npl.h` and the option headers |
| `nimble/transport/include/`, `nimble/transport/src/transport.c` | the host/controller transport's buffer pools and its function pair (we implement the controller side in `../ble_nimble.c`) |
| `porting/nimble/include/`, `porting/nimble/src/` | the OS abstraction's headers and its portable parts: `os_mbuf`, `os_mempool`, `os_msys`, `endian`, `mem`, `nimble_port.c`, and `syscfg.h` (the option defaults, which `../nimble_port/syscfg/syscfg.h` overrides before including it) |

Not taken: the controller (`nimble/controller`), the drivers, the other transports, the NPL ports (`porting/npl/*`:
ours is `../ble_nimble_npl.c`), `hal_timer.c` and `os_cputime*` (the controller's), the mesh, the apps and examples, the
build files (`pkg.yml`, `syscfg.yml`, `Makefile*`) and `ble_gatts_lcl.c` (a console-based test helper).

## Our configuration

`../nimble_port/syscfg/syscfg.h`: peripheral and broadcaster roles only, one connection, no pairing (`BLE_SM_*` 0),
no GATT client procedures, no flow control towards the controller, ATT MTU 185, one 8-block mbuf pool of 224 bytes.
`../nimble_port/nimble/nimble_npl_os.h` are the NPL types of our port (one thread, polled).

## Provenance of the selection

Which files compile and link for the FM-1 CPU is checked by `tools/build.py` (every file is compiled with the toolchain's
clang, sized, and the linked result is measured with the controller in docs/BLUETOOTH.md "Size").
