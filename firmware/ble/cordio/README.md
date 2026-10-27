# Arm Cordio LE controller, peripheral subset (Apache-2.0)

The Bluetooth LE link layer under `../cordio_port/` (docs/BLUETOOTH.md "Cordio path as built", docs/BLUETOOTH_CORDIO.md):
the controller's link control, baseband manager, scheduler and HCI command handler, Cordio's WSF utilities and the PAL
(platform abstraction) headers the chip's hardware layer has to implement. Our configuration and glue are in
`../cordio_port/`; this directory is Cordio's own.

- Source: Packetcraft / Arm Cordio, <https://github.com/packetcraft-inc/stacks>, `master` at commit
  `3656312d6b73e2a2c1c8b33ee0385bc199dd97e6` (2020-11-11). Copied from a shallow clone with `cp`, at the same relative paths.
- Licence: Apache-2.0 (`LICENSE.md`, unchanged; the source files carry their own Apache headers). OPLBOX as a whole stays
  GPL-3.0-only; Apache-2.0 code combines with GPL-3.0 (not GPL-2.0-only).
- **Unchanged**: every file here is byte for byte the upstream's. `SHA256SUMS` lists them (`sha256sum` format);
  `tests/run_host.sh` checks it, so an edit here fails the host tests.
- Not taken: the host stack, the profiles and apps, the mesh, the platform targets (`platform/targets/*` is Nordic's and
  other vendors' register code: none of it is read or used), `thirdparty/`, the build trees, and the controller's scan,
  initiator/master, extended and periodic advertising, ISO, encryption and PAST sources: our configuration
  (`../cordio_port/DEFINES.txt`) is advertiser and peripheral only, one connection. `../cordio_port/SOURCES.txt` is the list
  of files compiled; the headers of the others stay because the compiled ones include them.
