# BearSSL subset (MIT)

SHA-1, HMAC and AES decryption for the WPA2 key primitives in `../wpa_crypto.c` (docs/WIFI.md stage 8).

- Source: BearSSL by Thomas Pornin, <https://www.bearssl.org/>, git commit
  `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f` (the head of master on 2026-10-25).
- Licence: MIT, `LICENSE.txt` (unchanged). OPLBOX as a whole stays GPL-3.0-only; MIT code combines with it.

| File here | Upstream | Changes |
| --- | --- | --- |
| `sha1.c` | `src/hash/sha1.c` | SPDX and provenance comment only |
| `hmac.c` | `src/mac/hmac.c` | SPDX and provenance comment only |
| `aes_common.c` | `src/symcipher/aes_common.c` | SPDX and provenance comment only |
| `aes_small_dec.c` | `src/symcipher/aes_small_dec.c` | SPDX and provenance comment only |
| `dec32be.c`, `enc32be.c` | `src/codec/` | SPDX and provenance comment only |
| `bearssl_hmac.h` | `inc/bearssl_hmac.h` | SPDX and provenance comment only |
| `bearssl_hash.h` | `inc/bearssl_hash.h` | `#include <string.h>` replaced by declarations of `memcpy` and `memset` (the firmware has no C library headers; `firmware/src/libc.c` defines both) |
| `inner.h` | `src/inner.h` | reduced to the declarations and inline helpers the files above use; the big-endian helpers keep only their portable branch |

## Why BearSSL and not wpa_supplicant's `src/crypto`

docs/WIFI.md stage 8 says to take whichever is smaller. Both were compiled with the firmware's compiler and flags
(JieLi clang, `-target pi32v2 -Os -ffunction-sections -fno-builtin`). These are the sizes of the code and read-only
data in the object files:

| | BearSSL (this directory) | wpa_supplicant 2.9 `src/crypto` |
| --- | --- | --- |
| SHA-1 | 1,190 B (`sha1.c`) | 4,394 B (`sha1-internal.c`, unrolled `SHA1Transform` 3,946 B) |
| HMAC-SHA1 | 372 B (`hmac.c`) | 274 B (`sha1.c`) |
| AES decryption + key schedule | 1,308 B (`aes_common.c` 522, `aes_small_dec.c` 786) | 4,246 B (`aes-internal.c` 3,028 with `AES_SMALL_TABLES`, `aes-internal-dec.c` 1,218) |
| byte-order helpers | 62 B (`dec32be.c`, `enc32be.c`) | inline |
| **total** | **2,932 B** | **about 8,914 B** |

wpa_supplicant's PBKDF2, PRF and key unwrap (`sha1-pbkdf2.c`, `sha1-prf.c`, `aes-unwrap.c`, 604 B together) are not
used: its PBKDF2 runs all 4096 iterations in one call and recomputes the HMAC key pads on every iteration. The
replacements are in `../wpa_crypto.c` (own code, 1,177 B). `tools/build.py` compiles everything in `firmware/wifi/`
on each build and prints its size. Nothing links it yet.

`aes_small` is BearSSL's smallest AES and is not constant-time: it uses table lookups indexed by the key and the
data. Here it only decrypts EAPOL-Key key data (the GTK) under the KEK. The bulk CCMP encryption is meant to run in
the MAC's hardware (stage 8). BearSSL's constant-time `aes_ct` is the replacement if this ever matters.
