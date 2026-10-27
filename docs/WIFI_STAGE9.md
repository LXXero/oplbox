# WiFi stage 9: an IP address and a ping over the WPA2 join

The WiFi test page's ninth step, on top of stage 8 (docs/WIFI_STAGE8.md): once the FM-1 has joined xero's WPA2
network, it gets an IPv4 address by DHCP, looks up the gateway by ARP, pings it 10 times and then answers echo
requests from the network for up to 10 s. All of it goes through the MAC's hardware CCMP keys that stage 8 loaded.
**None of it has run on an FM-1 yet**; everything below was checked on the host only (`tests/run_host.sh`), against
the register model, its made-up WPA2 access point and a DHCP server and gateway added to the model for this stage.

This is the test page's stage 9, the first IP on hardware: docs/WIFI.md's plan calls it the hardware half of stage 7
(lwIP, 7g in TODO.md). The plan's own "Stage 9" (a WiFi page in OPLBOX's UI, the settings sector, song transfer) is
not this.

## Running it

From the computer the FM-1 is plugged into, as for stage 8, plus `--ping`:

```
python3 tools/fm1_wifi_report.py --ssid TESTWPA2 --psk-file FILE --run --ping --out wifi-ip
```

The network and passphrase go over exactly as in stage 8 (SysEx 0x64, RAM only, never printed or in the report).
`--ping` asks the FM-1 what it is doing while the run goes on (the live command, SysEx 0x65, docs/API.md); once it
has an address and listens for echo requests, the tool runs `ping -c 5 -i 0.2 -W 1 ADDRESS` from this computer,
prints the output and writes it to `ping.txt` in `--out`. The computer must be on the same network as the FM-1: the
FM-1's address is on the router's LAN.

## What the run does

After stage 8's join and the 4-way handshake, inside the 2 s stay (so the leave and the rejoin come after it):

1. **DHCP** (`fm1wifi_ip_dhcp`, lwIP's client): DISCOVER, OFFER, REQUEST, ACK, at most 10 s; lwIP asks again 2 s and
   4 s after the first DISCOVER. Kept: address, mask, gateway, server, up to 2 DNS servers, lease time, ms to bound,
   whether the OFFER and ACK came broadcast or unicast.
2. **ARP for the gateway** (`fm1wifi_ip_arp`): a request every 500 ms until the reply, at most 2 s. Kept: the
   gateway's MAC and the ms it took. A run without the reply still pings: lwIP then asks again itself.
3. **Ping the gateway** (`fm1wifi_ip_ping`): 10 echo requests of 56 data bytes, 200 ms apart, 1 s for each reply.
   Kept: sent, answered, each round trip, min/avg/max, replies that came late.
4. **Pinged from the network** (`fm1wifi_ip_listen`): for 10 s, or until 5 have been answered, every echo request to
   the FM-1's address is answered and counted.
5. The lease released (DHCPRELEASE), lwIP's interface taken down.

USB is served throughout (`platform_poll` every 20 ms), so the live command and the report answer during the run.

Data frames go to lwIP only from the AP (FromDS, the BSSID as the transmitter) to us or a group address, and only as
the MAC decrypted them: a frame in the clear once the PTK is loaded is dropped and counted. EAPOL frames stay with
the station (`fm1wifi_wpa.c`). Frames out carry the Protected bit and go through the AP's WCID, so the MAC adds the
CCMP header and MIC, with sequence numbers from the station's one counter.

## The pages

| Page | What |
| --- | --- |
| 12 IP STEPS | one line per step: `DHCP`, `ARP GW`, `PING GW`, `PING IN` (`OK` with the time or count, `LOSS`, `FAIL` with why: `NO OFFER, n DISCOVER`, `NAK`, `NO ACK`, `NO GATEWAY`, `NO REPLY`, or the library's error), then the address/prefix, gateway, gateway MAC, DNS |
| 13 DHCP, DATA | the DHCP messages sent and received, the server, the lease, OFFER and ACK broadcast or unicast; data frames sent and send errors, received, decrypted before and after, ICV, MIC and key errors, frames in the clear |
| 14 IP FRAMES 1-2 | the first IP frames either way in hex (the MAC's plaintext: no IV, MIC or key) |

A step that was never reached says `NOT REACHED`, and a run that joined an open network says `NOT RUN (WPA2 ONLY)`.

## The report

`tools/fm1_wifi_report.py` prints the 14 pages as text, then the counters, then the frames. New counters (stage 9):
`steps_reached`, the step errors (`ip_start_err` .. `ip_err`, signed `FM1WIFI_*`: 0 OK, -4 timeout, -5 off), the
DHCP fields (`ip_addr`, `ip_mask`, `ip_gateway`, `dhcp_server`, `dns_1`, `dns_2`, `lease_s`, `t_dhcp_ms` and each
message's count), ARP (`t_arp_ms`, `arp_tx`, `arp_rx`, the gateway's MAC), pings (`pings_*`, `ping_*_ms`), the
listening window (`echo_in`, `echo_out`, `listen_*`), the data path (`data_rx` .. `data_tx_err`) and the protected
frame and MAC security counters before and after the IP steps. Up to 8 IP frames are kept (400 bytes each, enough
for a whole DHCP message), named `frame-NN-ip_from_ap-dhcp_offer.bin` and so on in `--out`.

The frames are plaintext and the report carries no key material, but they show the FM-1's and the router's
addresses: keep `--out` private like a stage 8 run's.

## Reading a failure

- `DHCP FAIL NO OFFER, n DISCOVER` with `data_tx` counting up and `data_rx` at 0: our frames go out but nothing
  decrypted comes back. Look at `data_rx_not_decrypted`, `data_rx_icv_err` and `data_rx_mic_err` (page 13): nonzero
  means frames arrive but the MAC's key or its decrypt settings are wrong; all zero means nothing arrives at all
  (the AP drops our frames: compare `tx_sec_*_ip`).
- `NO ACK` after an OFFER: our REQUEST did not get through or was refused; `NAK` says the server refused it.
- `ARP GW FAIL NO REPLY` but `PING GW` answered: lwIP learnt the gateway from its own ARP; harmless.
- `PING GW LOSS`: the round trips (page 12) and `pings_late` show whether replies came after the 1 s wait.
- `PING IN NONE IN 10 S`: nothing pinged the FM-1 in the window. That is expected without `--ping`, or when the
  computer is not on the same network.

## Size

`./build.sh`, against main b84bafd (stage 8): the image grows by 25,024 B to 431,612 of 581,564 B; `.data`+`.bss`
by 15,920 B to 75,180 of 98,304 B: the run's state (`wl`: the IP state and its 8 kept frames) 5,988 B, lwIP's heap
4,115 B and its receive pool 2,115 B, `fm1wifi_net.c`'s frame buffers in and out 1,546 B each, the rest lwIP's
smaller pools and tables; the pool by 6,556 B to 280,072 of 344,064 B; the RAM code (16,028 B)
is unchanged. RAM left: 23,124 B. Of the C library only `memcpy`, `memset` and `memcmp` are linked
(`firmware/bsp/chip/libc.c`).

## Checked on the host

`tests/run_host.sh`: `tests/wifi_ip.c` (80 checks: DHCP with a silent server retried after 2 and 4 s, OFFER, REQUEST,
ACK, ARP both ways, pings and echo answers with the checksums and 802.11 headers checked, a restart on another AP
with 300 pings, 100 of them 1,500 bytes), its 15 mutants in `tests/wifi_ip_mutants.py` all caught, the simulator's
WiFi page through pages 12-14 with the live command (`tests/script_wifi_wpa.txt`), and the report tool against it.
Not checked: anything on an FM-1, the stack depth of an echo reply sent from inside `fm1wifi_net_input`
[unchecked], and a real router's DHCP server.
