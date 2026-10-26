# lwIP 2.1.3 subset (BSD-3-Clause)

The IP stack under `../fm1wifi_net.c` (docs/WIFI.md stage 7): IPv4, ARP, DHCP, ICMP and UDP, with `NO_SYS=1` and
the raw API.

- Source: lwIP by Adam Dunkels and the lwIP developers, <https://savannah.nongnu.org/projects/lwip/>, release 2.1.3:
  git tag `STABLE-2_1_3_RELEASE` (tag object `f01ec11c7bd0eddc786d5d9068ecf2b941b061b7`, commit
  `6ca936f6b588cee702c638eee75c2436e6cf75de`, 2021-11-10, "Prepare 2.1.3 release"). Taken from a clone of
  <https://github.com/lwip-tcpip/lwip.git> on this machine (the Raspberry Pi Pico SDK's `lib/lwip` submodule in
  PlatformIO's `framework-arduinopico`) with `git archive STABLE-2_1_3_RELEASE`; each file here has the same git
  blob hash as at that tag.
- Licence: BSD-3-Clause, `COPYING` (unchanged). OPLBOX as a whole stays GPL-3.0-only; BSD-3-Clause code combines
  with it.
- **Unchanged**: every file under `src/` and `COPYING` is byte for byte the release's (no SPDX lines added).
  `SHA256SUMS` lists them; `tests/run_host.sh` checks it, so an edit here fails the host tests.
- Not JieLi's copy. The JieLi AC79 SDK has lwIP 2.1.3 too (`lib/net/lwip_2_1_3`, `include_lib/net/lwip_2_1_3`), but
  JieLi changed it: 27 of the 37 `.c` files under `src/core` differ from the release beyond line endings and
  whitespace, 10 of the 15 used here among them (for example `etharp.c` gained an address conflict check that calls
  JieLi's `lwip_renew`, and debug levels changed), and it adds `ip4_napt.c` and `lwip/_errno.h`. Nothing here comes
  from it.

## Files

Only what `lwipopts.h` compiles in, and the headers those files include (found with `cc -MM`):

| Here | What |
| --- | --- |
| `src/core/` `init.c def.c inet_chksum.c ip.c mem.c memp.c netif.c pbuf.c timeouts.c udp.c` | the core: start-up, byte order, checksums, the heap and pools, pbufs, the interface, timers, UDP |
| `src/core/ipv4/` `dhcp.c etharp.c icmp.c ip4.c ip4_addr.c` | DHCP client, ARP, ICMP (echo), IPv4 |
| `src/netif/ethernet.c` | Ethernet framing (lwIP sees the WiFi link as Ethernet) |
| `src/include/` | the 63 headers those include (`lwip/`, `lwip/priv/`, `lwip/prot/`, `netif/ethernet.h`, `netif/ppp/ppp_opts.h` and `ppp_impl.h`, which compile to nothing with PPP off) |
| `COPYING` | lwIP's licence |

Not taken: TCP (`tcp*.c`), IPv6, IP fragmentation and reassembly (`ip4_frag.c`), raw sockets, IGMP, DNS, AutoIP,
SNMP, the netconn and socket APIs, PPP, the apps. Each is a file from the same tag when it is wanted; TCP is the
likely first (song transfer, docs/WIFI.md stage 9).

Our own, not lwIP's (GPL-3.0-only like the rest of OPLBOX):

| Here | What |
| --- | --- |
| `port/lwipopts.h` | the options: what is compiled in and the RAM it takes (below) |
| `port/arch/cc.h` | the compiler port: no `inttypes.h`/`ctype.h`, no printing, `LWIP_RAND` = `fm1wifi_net_rand()`; with `FM1WIFI_NET_DEBUG` (the host tests) a failed assertion prints and aborts |

Include paths: `-Ifirmware/wifi -Ifirmware/wifi/lwip/port -Ifirmware/wifi/lwip/src/include`.

## Size on the FM-1

JieLi clang, `-target pi32v2 -Os -ffunction-sections -fno-builtin` (`tools/build.py`'s flags), each file compiled
alone and measured with the toolchain's `objdump -h` and `nm -S` (not linked):

| | code + read-only data | RAM (.data, .bss, common) |
| --- | --- | --- |
| lwIP (16 files) | 13,411 B | 6,671 B |
| `fm1wifi_net.c` | 1,548 B | 168 B |
| **total** | **14,959 B** (without `--gc-sections`: an upper bound) | **6,839 B** |

The RAM is lwIP's heap (`MEM_SIZE` 4,096 B; 4,115 with its headers), the receive pool (`PBUF_POOL_SIZE` 4 x
`PBUF_POOL_BUFSIZE` 512 B; 2,115 B), the pbuf, UDP and timeout pools (67 B each), the ARP table (4 entries, 96 B)
and fm1wifi_net's interface, DHCP state and counters (168 B); `port/lwipopts.h` says what each is for. All static:
lwIP allocates only from these, and nothing calls `malloc`.

## What the firmware build needs

- The C library's `string.h`: the lwIP `.c` files include it (`stddef.h`, `stdint.h`, `limits.h` too, which the
  compiler has). The firmware has no C library headers on its include path (JieLi's are in
  `pi32v2/include` of the toolchain); the sizes above were measured with `-isystem` on that directory.
- From the C library, when linked: `memcpy`, `memset`, `memcmp` (Ethernet address compares), and `memmove`,
  `strlen`, `strncmp`, `atoi` unless `--gc-sections` drops the functions that use them (`def.c`'s string helpers,
  `pbuf_strstr`, `netif_find`; nothing here calls them). `firmware/src/libc.c` has `memcpy` and `memset` only.
- `sys_now()`, the port's millisecond clock (`../fm1wifi_port.h`).
- Stack depth in `fm1wifi_net_input` (an echo reply is sent from inside it) is not measured [unchecked].

`tools/build.py` compiles `../fm1wifi_net.c` with these include paths on every build (no C library header needed
there), but not the files in `src/`, and nothing links either yet.
