# WiFi stages 4-6: frames, scanning, joining an open network

What `firmware/wifi/fm1wifi.c` (the hardware side) and `firmware/wifi/fm1wifi_frame.c` (the frames, the scan list,
the station) do for docs/WIFI.md stages 4, 5 and 6, where each fact comes from, and how to read pages 5-10 of the
WiFi test page. All of it is host-tested (tests/wifi_frame.c, tests/wifi_link.c, the page against the register
model). **Stages 4 and 5 ran on xero's FM-1 on 2026-10-27** ("The first run on the FM-1" below): frames arrive and
parse, probe requests go out and are answered. The join (stage 6) has not run: no open OPLBOX network was up. The page
is built so that one run tests all three stages and a step that fails still shows what the next fix needs; its results
can also be read over USB ("Reading the results over USB").

## Where the facts come from

- **The frame FIFOs' registers, bits, order and limits**: read from `wl_rf_common.a`'s `wl_sdio.c.o`
  (`wl30_mmc_io_rw_extended`) and `wl_usb_to_sdio.c.o` (`sdio_recv_int_hdl`, `usb_submit_urb`, `get_txrxq_pcnt`,
  `RTUSBDoubleWrite`), turned into IR with the JieLi toolchain's clang 4.0.1 as docs/WIFI.md "Reproducing the
  measurements" does. Those two members carry Ralink and Linux lineage, so only addresses and values are taken from
  them (docs/WIFI.md "What stages 1 and 3 may take"); the code is written from the description below.
- **The RX and TX descriptors' fields** (RXWI, TXWI, the RX trailer's flags): Linux rt2x00's `rt2800.h` and
  `rt2800usb.h` (GPL-2.0-or-later), by field name. JieLi's structures are not used.
- **The MAC's defaults**: the 22 `MACRegTable` pairs docs/WIFI_REVIEW.md lists (JieLi's `rtmp_init.c.o`, equal to
  Ralink's own driver's).
- **The frames and the station**: IEEE 802.11-2020, clauses 9.3.3 (management frame bodies), 9.4.2 (elements) and
  11.3 (authentication and association).

## The RX FIFO

JieLi's receive thread (`sdio_irq_thread`) either waits on IRQ 52 or, without it, calls the receive handler over and
over; the handler tests bit 17 of the WL30 host block's 0x04 (0x30F04) and reads a frame when it is set. The library
polls the same bit from the main loop (`fm1wifi_rx_ready`), so no interrupt is hooked up yet.

One frame (`fm1wifi_rx_read`), as `sdio_recv_int_hdl` and `wl30_mmc_io_rw_extended` do it:

1. Function 1 register 0x0324 = 2.
2. 0x30F04 |= bit 11 (clears RX done); 0x30F18 = the buffer's address; 0x30F1C = 4 (bit 31, the increment flag,
   0). Wait for 0x30F04 bit 21 (RX done), then 0x30F04 |= bit 11 again.
3. The 4 bytes are the length word N, big-endian. JieLi refuses N = 0 or N >= 4089 (it resets the chip); the
   library returns `BAD FRAME` and does not read on (the FIFO's state is then unknown).
4. The same DMA with 0x30F18 = buffer + 4 and 0x30F1C = N + 4: N bytes of frame, then a 4-byte trailer, also
   big-endian (JieLi swaps it in place). On the FM-1, N is the RXWI (16) plus the MPDU byte count rounded up to a
   multiple of 4: 0 to 3 pad bytes end the N (all four frames of the first run).

JieLi's waits have no bound; the library's read bit 21 at most 1000 times, 1 us apart (`TIMEOUT`). The buffer must be
word-aligned (JieLi asserts that). After the read the buffer holds, as `fm1wifi_rx_parse` takes it:

| Offset | Bytes | What |
| --- | --- | --- |
| 0 | 4 | N, in CPU order |
| 4 | 16 | RXWI (rt2800.h): word 0 bits 7-0 WCID, 27-16 MPDU byte count; word 1 bits 22-16 MCS, 23 BW, 31-30 PHY mode; word 2 bits 7-0 RSSI0 |
| 20 | MPDU | the 802.11 frame, header first (JieLi's AGC hook reads the transmitter's address at offset 20 + 10) |
| 4 + N | 4 | the trailer (rt2800usb.h RXD word 0): bit 4 unicast to me, 5 multicast, 6 broadcast, 7 my BSS, 8 CRC error, 14 L2PAD |

RSSI0 is taken as a signed dBm value, because that is how JieLi's AGC (`WifiRecvPkgHook`, `adj_agc_max_gain`) uses
it: unconfirmed until the FM-1 shows real values. With L2PAD set, two pad bytes follow a header whose length is not a
multiple of 4 (QoS data); the MPDU byte count does not include them (as rt2x00 handles it).

## The TX FIFO

`usb_submit_urb` copies a frame into a 2560-byte buffer laid out as below and sends it with one DMA:

| Offset | What |
| --- | --- |
| 0 | a length word, frame + 22, that the DMA does not send |
| 4 | JieLi's header word: bit 0 = rt2800 TXINFO's WIV, bits 2-1 its QSEL |
| 8 | the TXWI, 16 bytes (rt2800.h): word 0 bits 9-8 TX_OP, 22-16 MCS, 31-30 PHY mode; word 1 bit 0 ACK, bit 1 NSEQ, bits 15-8 WCID, 27-16 MPDU byte count |
| 24 | the 802.11 frame |

The DMA (`fm1wifi_tx_send`): 0x30F04 |= bit 10 (clears TX done); 0x30F10 = buffer + 4; 0x30F14 = frame + 26; wait
for bit 20 (TX done, at most 1000 reads); 0x30F04 |= bit 10. Before it, JieLi waits until the queue has room: with
QSEL q, byte q of `TXRXQ_PCNT` (pages queued) must be below byte 3 - q of `PBF_MAX_PCNT` (JieLi keeps that register's
bytes in reverse order) with more than frame bytes of 128-byte pages left, bit 2 + 8q of 0x0434 clear, and
`PBF_DBG` bits 7-5 not all clear. The library makes the same checks, at most 1000 times 10 us apart (`TIMEOUT`).

The library fills the image as rt2x00 does for a management frame: QSEL 2 (EDCA, rt2800usb's only queue), WIV 1,
TX_OP 3 (backoff), CCK 1 Mbit/s with the long preamble (MCS 0, PHY mode 0; OFDM 6 Mbit/s on request), WCID 255
(no table entry), ACK for unicast frames. NSEQ is 0: the station counts its own sequence numbers into the header,
so a retry is recognised even if the MAC does not number frames.

## The link

`fm1wifi_link_up(mac, ch)`, after the radio is up: the 22 `MACRegTable` values in JieLi's order, `MAC_ADDR_DW0/1` =
the address (with `UNICAST_TO_ME_MASK` 0xFF, as rt2x00), the BSSID 0, then channel ch as the survey tunes
(`WF_CON7`, `TX_BAND_CFG`, `TX_PIN_CFG` 0x00050302). `RX_FILTER_CFG` 0x17F97 drops CRC and PHY errors, duplicates,
control frames and unicast frames to other addresses; it leaves `DROP_NOT_MY_BSSD`, `DROP_MULTICAST` and
`DROP_BROADCAST` clear, so every group-addressed frame of any network comes through, not only beacons (the first run's
fourth frame was another network's protected multicast data frame). The library counts what is not a beacon or a probe
response and drops it. `TX_RTY_CFG` is
JieLi's (and Ralink's) 0x47D01F0F, retry limits 31 and 15, not rt2x00's 2 and 2: it is what JieLi's driver ran on
this core, and a station joining at 1 Mbit/s wants the retries.

**The address**: the FM-1 has no WiFi address of its own that we know of. The page makes one from the flash part's
factory unique ID (16 bytes, `platform_uid`), hashed (FNV-1a) into a locally administered unicast address, so it is
the same on every start and differs between FM-1s, without writing flash. docs/WIFI.md planned to keep a made-up
address in a settings sector; the unique ID makes that unnecessary. Without an ID it falls back to 02:4F:50:4C:42:58
(page 9 then says `FIX` instead of `UID`).

## Scanning and joining

- **Passive scan** (`fm1wifi_scan_channel(ch, ms, 0, ...)`): tune, listen ms milliseconds, every beacon and probe
  response into the scan list (SSID, BSSID, the DS element's channel, the strongest RSSI, security from the RSN
  element / WPA's vendor element / the privacy bit, the rates, the RSN element whole for WPA2). A hidden network
  beacons with an empty SSID and shows as `(HIDDEN)`.
- **Active scan** (`active` 1): a probe request for any network, and one for the SSID given, then the same listening;
  probe responses mark the network `P`.
- **Station** (`fm1wifi_frame.h`): open-system authentication (3 tries, 300 ms apart), then association (the same),
  then UP with the AID; DOWN on a deauthentication or disassociation addressed to us, on 10 beacon intervals (at
  least 1 s) without a beacon, or on leaving (a deauthentication, reason 3, is sent). `fm1wifi_join` sets the BSSID
  and runs it until UP or DOWN; `fm1wifi_listen` keeps it running.
- The library's clock for all of this is the time it spends in the port's delay (100 us slices while nothing waits in
  the FIFO), so a scan of 13 channels at 120 ms takes about 1.6 s.

## The page's run

After stage 3's survey and RX check (pages 1-4, unchanged), PLAY goes on with:

1. the link up on channel 1 with the FM-1's address;
2. a passive scan, 120 ms on each of channels 1-13;
3. an active scan, 40 ms on each channel, probe requests for any network and for `OPLBOX`;
4. joining `OPLBOX` if the scan found it and it is open (an open phone hotspot named OPLBOX, on 2.4 GHz);
5. 2 s joined (four times 500 ms, USB kept alive between), counting its beacons;
6. leaving, joining again, leaving.

Each step runs only if the one before it worked. The whole run takes about 10-15 s. Steps 4-6 need the hotspot;
without it the page says `FAIL NO OPLBOX` and the scans still show what they heard.

## Reading pages 5-10

Page 5, each step's result: `OK` with what it found, `FAIL` with the reason, or `NOT REACHED` when an earlier step
failed.

```
STEPS                 5/10
RADIO   OK                        stage 2-3 (page 1 has the details)
SURVEY  OK 13 OF 13 CH            the channel survey (page 4)
LINK    OK                        the MAC's defaults and the address written
PASSIVE OK 5 NETWORKS             FAIL RX TIMEOUT / RX BAD FRAME: the FIFO read; FAIL NO FRAMES: nothing ever arrived
ACTIVE  OK 4 ANSWERS              FAIL TX TIMEOUT: no room in the queue or the DMA never finished; FAIL NO ANSWER
JOIN    OK AID 1                  FAIL NO OPLBOX / OPLBOX IS WPA2 / AUTH TIMEOUT / AUTH REFUSED / ASSOC ...
STAY 2S OK 19 BEACONS             FAIL BEACON LOSS / DEAUTH / DISASSOC
REJOIN  OK                        after leaving
```

Page 6, the scan list: SSID (10 characters), channel, RSSI (dBm, if RSSI0 is what JieLi takes it for), security, `P`
when a probe response came. Up to 7 or 8 lines; `+n MORE` below when there are more, `n NOT KEPT` when the list (16)
was full.

Pages 7 and 8, the first four frames the FIFO gave, whatever they parsed to: the channel, the bytes it had (N + 8),
then 36 bytes in memory order, four to a group (the library keeps the first 512 bytes of the first four frames of each
step; the USB report has them all):

```
1 CH  1  BYTES 104
60000000 FF004D00 00000000      group 1: N (little-endian: 0x60); groups 2-5: the RXWI's four words, each
C3000000 00000000 80000000      little-endian (FF = WCID, 4D00 = MPDU 0x4D bytes; C3 = RSSI0 -61)
FFFFFFFF FFFF0200 00000001      groups 6-9: the 802.11 header's first 16 bytes (80000000: a beacon;
                                then address 1, the broadcast, and the start of address 2)
```

These are what settles the RXWI's layout on the real chip: if the MPDU count, the RSSI byte or the header's place
differ from rt2800's, the bytes show where they actually are.

Page 9, the core's side:

```
RX/TX COUNTS          9/10
WL30 04 xxxxxxxx>xxxxxxxx         0x30F04 before the scans and after the run (bit 17: a frame waits)
TXRXQ xxxxxxxx>xxxxxxxx           TXRXQ_PCNT before and after (bits 31-24: frames in the RX queue)
FRAMES n BAD n TO ME n            frames read, those that did not parse, those for our address
CRC ERR n  PHY ERR n              RX_STA_CNT0 over the run (dropped by the MAC's filter)
DUP n  OVERFLOW n                 RX_STA_CNT2
SENT n ERR n                      frames the library sent / sends that failed (and the first error)
TX OK n FAIL n RETRY n            TX_STA_CNT0/1: what the MAC says about them (OK: acknowledged or broadcast)
MAC 16:C6:87:70:CF:3D UID         the FM-1's address, and where it came from
```

Page 10, the join: OPLBOX's BSSID, channel, RSSI and security, the authentication's and association's answers (OK,
no answer after 3 tries, or refused with the AP's status code), 2 s later (UP and the beacons, or why it went down
and the reason code), the rejoin, the frames sent and received.

## Reading the results over USB

The ten pages are also a report the FM-1 keeps in RAM until the next run, which `tools/fm1_wifi_report.py` reads over
USB-MIDI from the computer the FM-1 is plugged into (docs/API.md "WiFi test page over SysEx"):

```
python3 tools/fm1_wifi_report.py --run --out wifi-run      # start a run (as SAVE, PLAY), wait, read it
python3 tools/fm1_wifi_report.py                           # the last run's report again
```

It prints every line of pages 1-10, every network the scan kept (the whole SSID and the BSSID; page 6 has room for 7),
and the counters behind page 9. `--out DIR` also writes each frame the FIFO gave as it gave it (up to four per step,
the first 512 bytes of each: length word, RXWI, frame, trailer) as `frame-NN-<step>-chNN.bin`, for parser fixtures.
A run started this way stops the sound and leaves the page open on page 1, as PLAY does; SAVE leaves it. The FM-1
refuses a run while EDIT is open. The pages stay as they are, for when no computer is at hand. Before a frame from a
report becomes a fixture in this public repository, replace its transmitter and BSSID bytes (see below).

## The first run on the FM-1

xero ran main 82f7fa6 on his FM-1 on 2026-10-27 (flashed 02:20 UTC), with no open OPLBOX network up; the values were
read off photos of the ten pages, so a digit may be misread.

- Pages 1-4 (stages 2-3) as in the earlier runs: `RESULT: OK`, `MAC_CSR0` 00300100, `MAC_SYS_CTRL` 0000000C, BBP 0
  60, PLL bank 51 mid 63, filter 253 127 55, `RAM CHECK: 0 CHANGED`; trim TX LO 48 60, DC 6 0, TX gain +20 phase -15,
  RX gain +6 phase -6; false CCA counts 106-494 per 200 ms on every channel, busy time 0 everywhere.
- Page 5: `RADIO OK`, `SURVEY OK 13 OF 13 CH`, `LINK OK`, `PASSIVE OK 12 NETWORKS`, `ACTIVE OK 12 ANSWERS`,
  `JOIN FAIL NO OPLBOX`, `STAY 2S` and `REJOIN NOT REACHED`.
- Page 6: 15 networks (7 on the screen, `+8 MORE`, none lost to a full list); the seven shown are all WPA2, on
  channels 1 and 6, RSSI -57 to -88, four with `P` (a probe response came), two hidden.
- Page 9: `WL30 04 00000037>00010037`, `TXRXQ 00000000>00000000`, `FRAMES 34 BAD 0 TO ME 12`,
  `CRC ERR 44 PHY ERR 21`, `DUP 0 OVERFLOW 0`, `SENT 26 ERR 0`, `TX OK 26 FAIL 0 RETRY 0`,
  `MAC CE:62:BC:AE:4D:05 UID`.
- Page 10: `NO OPLBOX IN THE SCAN`.

Pages 7-8, the first four frames, all on channel 1. Each transmitter address (address 2, the six bytes after address
1) is shown here with its last three bytes replaced by 00 00 01, 02 or 03, one per address: real BSSIDs locate a place
in public WiFi databases, and this repository is public.

```
1 BYTES 360  60010000 FFE04E01 F0440040 A8A2E000 A800A0FE 80000000 FFFFFFFF FFFFC64F D5000001
2 BYTES 328  40010000 FFE02D01 10340040 BBA5EC00 BB0F94FF 80000000 FFFFFFFF FFFF82DA C2000002
3 BYTES 324  3C010000 FFE02B01 20340040 BBA3EE00 BB0F94FF 80000000 FFFFFFFF FFFF82DA C2000003
4 BYTES 336  48010000 FFE13701 50340000 B8B9EE00 B80EFEFF 08620000 33330000 00FB82DA C2000002
```

Against the layout above (rt2800's RXWI), field by field:

| Field | Where the doc has it | The FM-1's frames |
| --- | --- | --- |
| N | offset 0, CPU order after the swap | 0x160, 0x140, 0x13C, 0x148: BYTES - 8, as expected; and N = 16 + the MPDU count rounded up to 4 (new: the end padding) |
| WCID | word 0 bits 7-0 | FF on all four (no table entry), as expected |
| MPDU byte count | word 0 bits 27-16 | 334, 301, 299, 311: each fits N as above, so the field is where rt2800 has it |
| MCS, BW, PHY mode | word 1 bits 22-16, 23, 31-30 | MCS 0, BW 0; PHY mode 1 (OFDM, 6 Mbit/s) for the three beacons, 0 (CCK, 1 Mbit/s) for the data frame |
| RSSI0 | word 2 bits 7-0, signed dBm | A8 (-88), BB (-69), BB (-69), B8 (-72). Frame 1 is the network page 6 lists at -88 and frames 2-3 match the two at -69 on channel 1: the byte is the value the scan list shows; as dBm it is plausible, not checked against another receiver |
| 802.11 header | offset 20 | frame control 80 00 (beacon) on frames 1-3, broadcast address 1; frame 4: 08 62 (data, From DS, More Data, Protected) to 33:33:00:00:00:FB (IPv6 mDNS multicast) |

Every field the parser reads is where rt2800 has it; nothing on the page needs to move. Also seen, not read by the
library: word 0 bits 15-8 are E0 (E1 on the protected frame; rt2800 has the key index in bits 9-8 and the UDF in
15-13); word 1 bits 15-4 count up across frames 2, 3 and 4 (0x341, 0x342, 0x345), which fits rt2800's sequence number
there for one radio sending as two BSSIDs (unconfirmed until a whole frame shows the header's sequence control); the
other bytes of word 2 (A2 E0, A5 EC, A3 EE, B9 EE) and word 3 (its first byte equals RSSI0; then 00, 0F, 0F, 0E and
A0FE, 94FF, 94FF, FEFF) do not match rt2800's RSSI1/2 and SNR fields, and their meaning is open. tests/wifi_frame.c
(`fm1_frames`) parses these 36 bytes of each frame as fixtures and checks the fields in the table.

What it shows:

- **Stage 4 works on the FM-1**: frames reach the FIFO with the receive path as set up (no IRQ 52, no 0x0328), the
  length word, the RXWI and the header are where the library reads them (`BAD 0` of 34), and the scan list fills.
  `CRC ERR` 44 and `PHY ERR` 21 are what the MAC's filter dropped.
- **Stage 5 works on the FM-1**: the TX gate opened for every frame (`SENT 26 ERR 0`: two probe requests on each of
  13 channels), the MAC counts all 26 as sent (`TX OK 26`, no failures, no retries: broadcast frames are not
  acknowledged), and 12 networks answered, to our address (`TO ME 12`): the address filter with `MAC_ADDR_DW0/1` from
  the unique ID works, and APs hear the probe requests at CCK 1 Mbit/s.
- **Stage 6 is untested**: no open OPLBOX network was up, so the run stopped at `JOIN FAIL NO OPLBOX`, as it should.
- Open: 0x30F04 bit 16 was 0 before the scans and 1 after the run (bits 10, 11, 17, 20 and 21 are the ones the library
  uses; bit 16's meaning is unknown). `TXRXQ_PCNT` read 0 both times. The RAM check found nothing the DMA wrote outside
  its buffers (`RAM CHECK: 0 CHANGED` after the FIFOs ran).

## What a failure would mean (for the next fix)

- `PASSIVE FAIL NO FRAMES`, with `CRC ERR`/`PHY ERR` counting: the receiver hears frames but the MAC drops them
  (the descriptor or filter). With both 0 and 0x30F04 bit 17 never set: frames never reach the FIFO (the DMA block's
  set-up, the RX path's enable, or a register JieLi writes elsewhere in the receive path; the IRQ path's 0x0328 = 2
  is the first candidate). The survey's false CCA counts (page 4) show the receiver itself works.
- `PASSIVE FAIL RX BAD FRAME`: the length word is not where it is expected; page 7 is empty then, so the next round
  should dump the raw DMA bytes before checking N.
- Frames on pages 7-8 that do not parse (`BAD n` on page 9): compare their bytes with the RXWI layout above.
- `ACTIVE FAIL TX TIMEOUT`: the queue never had room (the gate's registers; `TXRXQ` on page 9) or the DMA never
  finished. `ACTIVE FAIL NO ANSWER` with `TX OK` counting: frames leave the MAC but no AP answers (the radio's TX
  path, the channel, or the frame); with `TX FAIL` counting: the MAC tried and got nothing back.
- `JOIN FAIL AUTH TIMEOUT` with the active scan answered: our unicast frames are not heard or the AP's answer is
  dropped (`TO ME` 0: the MAC's address filter); `AUTH REFUSED` / `ASSOC REFUSED` carry the AP's status code.
- `STAY 2S FAIL BEACON LOSS`: frames stop arriving after the BSSID is set.

## Not done here

- No interrupt (IRQ 52): the main loop polls.
- No hardware encryption (the key tables), no data frames on the page: stages 7 (IP, lwIP) and 8 (WPA2) are built
  as host code and join the page once an open join works on the FM-1.
- The station's rate is CCK 1 Mbit/s for every frame it sends; no rate control.
- The TX status FIFO (`TX_STA_FIFO`) is not read; the counters stand in for it.
