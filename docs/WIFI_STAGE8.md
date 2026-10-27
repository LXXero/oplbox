# WiFi stage 8: joining a WPA2 network

Stage 8 of docs/WIFI.md's plan, on the WiFi test page: the run joins a WPA2-PSK network (xero's TESTWPA2) instead of
an open OPLBOX hotspot, runs the 4-way handshake, loads the agreed keys into the MAC's key tables so CCMP runs in
hardware, stays joined 2 s, leaves and joins again. **None of it has run on an FM-1 yet**; everything below was
checked on the host only (`tests/run_host.sh`), against the register model and its made-up WPA2 access point.

## Running it

From the computer the FM-1 is plugged into (xlap), in the oplbox checkout:

```
python3 tools/fm1_wifi_report.py --ssid TESTWPA2 --psk-from-nm <connection> --run --out wifi-wpa2
```

`<connection>` is the NetworkManager connection that holds TESTWPA2's passphrase (`nmcli connection show` lists them;
`nmcli -s` needs the right to read secrets, so run it as the desktop user). Or put the passphrase on the first line of
`~/.config/oplbox/wifi.psk`, `chmod 600` it, and leave out `--psk-from-nm` (`--psk-file FILE` for another file). The
tool sends the network, starts a run (the sound stops; EDIT must be closed), waits for it, prints the report and
writes it to `wifi-wpa2/`. `--forget` makes the runs join the open OPLBOX again.

The passphrase:
- is never an argument of the tool or of a program it starts (`nmcli` gets the connection's name), and the tool
  never prints it. A passphrase file anyone but its owner can read is refused;
- goes to the FM-1 in SysEx 0x64 (docs/API.md), which keeps it in RAM only, until the FM-1 restarts or the command
  comes again: not in flash, not on the screen (the SSID is), not in the report, not in the reply;
- is turned into the PMK (PBKDF2, 4096 iterations, `wpa_crypto.c`) once per network: the PMK stays beside it in RAM,
  so a second run says `PMK CACHED`. Setting another network or passphrase forgets the cached one.

The report holds the EAPOL frames of the handshake (as captures of any WPA2 join do). Messages 1 and 2 are enough to
test passphrase guesses offline, so keep a WPA2 run's `--out` directory private: do not commit it or post it.

## What the run does

After the survey, as before (docs/WIFI_STAGE4.md): the link up, a passive scan, an active scan that probes for the
network's SSID. Then, when a network is set over USB and the scan heard it as WPA2:

1. the PMK, in `WPA_PMK_STEP` (64) iterations per step with USB served between steps, or the cached one;
2. the open-system authentication and the association, the association request carrying our RSN element (CCMP
   pairwise and group, AKM 2 PSK, no management frame protection). A network whose RSN element asks for something
   else (a TKIP group cipher, no CCMP, no PSK, MFP required, as WPA3-only networks do) is not joined, and the JOIN line
   says which;
3. the 4-way handshake (`wpa_eapol.c`) over EAPOL data frames: msg 1 must come within 2 s of the association and the
   handshake must end within 5 s. Msg 4 goes out in the clear, then the PTK is loaded; then the GTK from msg 3;
4. 2 s joined: beacons counted, a group key handshake answered (encrypted, by the MAC, with the PTK), every protected
   frame from the AP counted as the MAC decrypted it or not;
5. leaving (the deauthentication, the key tables cleared) and the whole join again with the cached PMK.

With no network set the run joins the open OPLBOX as stage 6 did.

## The key tables

As JieLi's own station code writes them (its `wl_wifi_sta_sfc.a`, read as addresses and values: `cmm_asic.c`'s
`AsicAddPairwiseKeyEntry`, `AsicAddSharedKeyEntry`, `AsicUpdateWCIDIVEIV`, `AsicUpdateWcidAttributeEntry`,
`cmm_wpa.c`'s `WPAInstallPairwiseKey` and `WPAInstallSharedKey`, `AsicUpdateRxWCIDTable(pAd, 1, bssid)`), with Linux
rt2800.h's names for the addresses (`firmware/wifi/fm1wifi.c` `fm1wifi_key_*`):

| What | Where | Value |
| --- | --- | --- |
| the AP's address | `MAC_WCID_ENTRY(1)` = 0x1808 | address bytes 0-3, then 4-5 and FF FF (rt2800's reserved bytes) |
| the TK | `PAIRWISE_KEY_ENTRY(1)` = 0x4020 | 16 key bytes, then 16 bytes of MIC keys, 0 for CCMP |
| the AP's IV | `MAC_IVEIV_ENTRY(1)` = 0x6008 | IV `20000001` (PN 1, Ext IV, key ID 0), EIV 0 |
| the AP's attribute | `MAC_WCID_ATTR_ENTRY(1)` = 0x6804 | `00000009`: pairwise table, cipher 4 (AES) in bits 3-1, BSS 0 |
| the GTK | `SHARED_KEY_ENTRY(id)` = 0x6C00 + 32 id | 16 key bytes, 16 bytes of 0 (BSS 0: entry 4 x 0 + key ID) |
| its cipher | `SHARED_KEY_MODE_ENTRY(0)` = 0x7000 | 4 in the key ID's 4 bits (bits 4 x id to 4 x id + 3), the others kept |
| the group IV | `MAC_IVEIV_ENTRY(0)` = 0x6000 | IV `(20 \| id << 6) << 24 \| 1`, EIV 0 |
| the group attribute | `MAC_WCID_ATTR_ENTRY(0)` = 0x6800 | `00000008`: shared table, AES, BSS 0 |

JieLi's attribute word has no `RX_WIUDF` bits (Linux rt2800 sets them to the cipher) and its PN starts at 1; JieLi's
station puts the AP at WCID 1 and the group key on WCID 0 (Linux puts stations from WCID 33 and writes a WCID entry per
shared key, which would overwrite WCID 1 for key ID 1). Leaving clears all of it: the AP's address entry all FF, both
key entries, all four shared keys and their mode word, both IV and attribute words 0.

A frame we send protected (group msg 2, or msg 2 and 4 of a later PTK rekey) has its Protected bit set and no IV or
MIC; JieLi's word before the TXWI has WIV 0 (the MAC inserts the IV from its IV/EIV table) and the TXWI's WCID is 1,
the key to encrypt with (rt2800usb.c and rt2800lib.c do the same). A protected frame the MAC decrypted arrives with the
trailer's bit 16 (`DECRYPTED`) set, its IV and MIC taken off; bits 10-9 say why one was not (1 ICV, 2 MIC, 3 key).

**Not known yet [unchecked], what the FM-1 run shows:** whether the MAC counts the PN up in the IV/EIV table itself
(JieLi's driver writes it only when it loads a key, so it seems to); whether the RXD bits are rt2800usb's on this
core; what `TX_SEC_CNT0` / `RX_SEC_CNT0` count (the report has both before the join and at the end); whether the key
entries read back at all (the tables might be write-only: then the read-back says DIFF while the join still works).
Whether a decrypted frame's MPDU count and header come as rt2800usb's (IV and MIC taken out) is not known either. The
4-way handshake itself does not depend on any of this (msg 1-4 travel in the clear); GROUP KEY's read-back, page 11's
DECRYPTED counts and a group key handshake during the 2 s are what show the hardware CCMP working. The group key's
receive counter (the RSC from msg 3) is not loaded, as JieLi's driver does not.

## Reading the pages

Page 5 now has ten steps on lines 1-10 (no footer: PLAY still goes to page 6):

```
STEPS                 5/11
RADIO   OK
SURVEY  OK 13 OF 13 CH
LINK    OK
PASSIVE OK 12 NETWORKS
ACTIVE  OK 12 ANSWERS
JOIN    OK AID 1                  FAIL NO TESTWPA2 / TESTWPA2 IS WPA / NO CCMP GROUP / MFP REQUIRED / AUTH ... / ASSOC ...
4-WAY   OK 37 MS                  ms from the association to msg 4 sent. FAIL NO MSG 1 / NO MSG 3 / DEAUTH n (the AP's
                                  reason: 15 is its 4-way timeout, a wrong passphrase) / MIC / RSNE / ... (what
                                  wpa_eapol.c refused) / KEYS TIMEOUT (a key table write) / PTK READBACK
GROUP KEY OK KEY ID 1             the last GTK loaded; FAIL GTK READBACK. NOT NEEDED (OPEN) for an open network
STAY 2S OK 20 BEACONS             FAIL BEACON LOSS / DEAUTH n / MIC ... (a group key handshake refused)
REJOIN  OK
```

Page 10 is the join in detail as before, headed with the network's name. Page 11 is new:

```
WPA2                 11/11
NETWORK TESTWPA2
PMK  1234 MS                      how long the PMK's steps took (or CACHED): the number that sets WPA_PMK_STEP
EAPOL IN 3 OUT 3 REFUSED 0        EAPOL frames from and to the AP, and those wpa_eapol.c refused
SEEN M1 M2 M3 M4 G1 G2            the handshake's messages either way
PTK ATTR 09 IV 20000001 OK        the AP's entries as read back after loading: OK, or DIFF (the report has them all)
GTK ID 1 MODE 0040 IV 60 OK       the group key's
DECRYPTED 7/7 GROUP 6/6           protected frames from the AP the MAC decrypted, of all; of those, group addressed
REKEYS 0  RNG REG CHANGES         group key handshakes; whether two reads of the chip's random register differed
```

Line 8 says `NO MSG 3: WRONG PASSPHRASE?` when msg 2 went out and no msg 3 came (the AP checks msg 2's MIC, which
only the right passphrase gives), and `GROUP FRAMES NOT DECRYPTED` when the AP's broadcasts arrived but none
decrypted (the GTK's entries are wrong).

The report (docs/API.md) adds the key tables as read back, with each key's eight words only as "as written" or not:

```
PTK MAC_WCID_ENTRY 00000002 FFFF6600 ATTR 00000009 IV/EIV 20000001 00000000 KEY WORDS AS WRITTEN 8/8
GTK KEY ID 2 SHARED_KEY_MODE 00000440 ATTR 00000008 IV/EIV A0000001 00000000 KEY WORDS AS WRITTEN 8/8
TX_SEC_CNT0 xxxxxxxx > xxxxxxxx  RX_SEC_CNT0 xxxxxxxx > xxxxxxxx
RANDOM NUMBER REGISTER, TWO READS: xxxxxxxx xxxxxxxx
```

then 39 more counters (the station's, the read-back, the counters before the join) and the EAPOL frames as bytes:
`--out DIR` writes them as `frame-NN-eapol_from_ap-msg1.bin` and so on.

## SNonces

The SNonce must be random (IEEE 802.11 12.7.5). The FM-1's source is the chip's random number register (`JL_RAND` in
JieLi's `WL82.h`, read in `firmware/hal/fm1_rand.h`), mixed with the timer, a count and the previous nonce through
HMAC-SHA1 keyed with the flash's unique ID and the WiFi address (`oplbox.c` `wifi_nonce`). Whether that register is a
noise source on the AC79 is not known [unchecked]; page 11 shows whether two reads differ. With the count and the
clock each nonce is new either way, which is what the handshake needs to give a fresh PTK.

## Host tests

- `tests/wifi_wpa.c`: the AP's RSN element (what is joined, what is refused, ours byte for byte); the station alone
  against the made-up AP (`tests/wifi/wpa_ap.c`, the authenticator moved out of `tests/wifi_eapol.c`, whose replay of
  Wireshark's wpa-Induction handshake still checks `wpa_eapol.c` byte for byte): msg 1-4 as data frames, msg 4 in the
  clear, the group key handshake encrypted with WCID 1, the records, no msg 1, a wrong passphrase, a bad MIC, a
  deauthentication, leaving; then the driver against the register model, whose WPA2 AP plays the key tables as the
  MAC would use them (it decrypts a protected frame only when the entries above hold the right key, and takes our
  protected frames only when WIV is 0 and the TXWI's WCID holds the TK): every entry register by register, msg 4
  sent before the PTK is written, the read-back, a group key handshake and protected broadcasts under the old and the
  new GTK, the tables cleared on leaving, a rejoin, a wrong passphrase (the AP's deauthentication 15), an AP that never
  sends msg 1. Eight deliberate bugs in the key table code (no pairwise bit, a mode field 3 bits wide, the reserved
  bytes 0, the key words reversed, msg 4 protected, WCID 2, keys loaded late, no clearing) are each caught.
- `tests/slot_sysex_test.c`: the network command through ota.c's framing, every refusal, none taken while a run goes
  on, the passphrase not in the reply.
- `tests/script_wifi_wpa.txt` in the simulator: the network set over USB, a run against `tests/wifi/aps_wpa.txt`,
  every page, the cached PMK on the second run, a wrong passphrase, the network forgotten; `tests/run_host.sh` checks
  that neither passphrase, neither PMK and neither GTK is anywhere in the output or the four reports.
- `tests/wifi_report_tool.py`: `--ssid` with `--psk-file` (refused when others can read the file) and with
  `--psk-from-nm` (a stand-in `nmcli` checks its arguments), the refusals before anything is sent, `--forget`, the
  passphrase never printed.

## Open

- The FM-1 run: the key tables and the hardware CCMP are the part the host cannot check.
- `WPA_PMK_STEP` (64): page 11's PMK time on the FM-1 sets it.
- The PMK cache is in RAM, not the settings sector (stage 9 decides where the network lives).
- No PMF (802.11w), no WPA3/SAE, no TKIP: networks that need one are not joined, and the page says so.
