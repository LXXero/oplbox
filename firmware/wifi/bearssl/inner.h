/* SPDX-License-Identifier: MIT */
/* Reduced from BearSSL (https://www.bearssl.org/), commit 7bea48e5e850ab4cafbe68d3765cdaba13a86d6f,
 * src/inner.h: only the declarations and inline helpers that sha1.c, hmac.c, aes_common.c,
 * aes_small_dec.c, dec32be.c and enc32be.c use, copied unchanged, except that the big-endian helpers keep
 * only their portable byte-by-byte branch (the BR_BE_UNALIGNED fast path is dropped), and bearssl.h (every
 * BearSSL header) is replaced by the two this code needs. Licence: LICENSE.txt in this directory. */
/*
 * Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining 
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be 
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef INNER_H__
#define INNER_H__

#include <stddef.h>
#include <stdint.h>

#include "bearssl_hash.h"
#include "bearssl_hmac.h"

/*
 * Encoding/decoding functions (big-endian, portable form).
 */

static inline void
br_enc32be(void *dst, uint32_t x)
{
	unsigned char *buf;

	buf = dst;
	buf[0] = (unsigned char)(x >> 24);
	buf[1] = (unsigned char)(x >> 16);
	buf[2] = (unsigned char)(x >> 8);
	buf[3] = (unsigned char)x;
}

static inline uint32_t
br_dec32be(const void *src)
{
	const unsigned char *buf;

	buf = src;
	return ((uint32_t)buf[0] << 24)
		| ((uint32_t)buf[1] << 16)
		| ((uint32_t)buf[2] << 8)
		| (uint32_t)buf[3];
}

static inline void
br_enc64be(void *dst, uint64_t x)
{
	unsigned char *buf;

	buf = dst;
	br_enc32be(buf, (uint32_t)(x >> 32));
	br_enc32be(buf + 4, (uint32_t)x);
}

/*
 * Range decoding and encoding (for several successive values).
 */
void br_range_dec32be(uint32_t *v, size_t num, const void *src);
void br_range_enc32be(void *dst, const uint32_t *v, size_t num);

/* ==================================================================== */
/*
 * Support code for hash functions.
 */

extern const uint32_t br_sha1_IV[];

void br_sha1_round(const unsigned char *buf, uint32_t *val);

static inline size_t
br_digest_size(const br_hash_class *digest_class)
{
	return (size_t)(digest_class->desc >> BR_HASHDESC_OUT_OFF)
		& BR_HASHDESC_OUT_MASK;
}

/* ==================================================================== */
/*
 * AES support functions.
 */

/*
 * The AES S-box (256-byte table).
 */
extern const unsigned char br_aes_S[];

/*
 * AES key schedule. skey[] is filled with n+1 128-bit subkeys, where n
 * is the number of rounds (10 to 14, depending on key size). The number
 * of rounds is returned. If the key size is invalid (not 16, 24 or 32),
 * then 0 is returned.
 *
 * This implementation uses a 256-byte table and is NOT constant-time.
 */
unsigned br_aes_keysched(uint32_t *skey, const void *key, size_t key_len);


/*
 * AES block decryption with the 'aes_small' implementation (small, but
 * slow and not constant-time). This function decrypts a single block
 * "in place".
 */
void br_aes_small_decrypt(unsigned num_rounds,
	const uint32_t *skey, void *data);

#endif
