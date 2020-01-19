/*    
 * Copyright (C) 2020, Twinkle Labs, LLC.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "lisp.h"

struct aes_cbc_stream;

#define AES_BLOCK_SIZE 16

typedef enum {
	AES_CBC_STREAM_NONE,
	AES_CBC_STREAM_DECRYPT,
	AES_CBC_STREAM_ENCRYPT,
	AES_CBC_STREAM_FINALIZED
} aes_cbc_stream_mode_t;

int
ecdh_shared_secret(uint8_t outbuf[], size_t outlen,
	const uint8_t *pubkey, size_t pubkey_len,
	const uint8_t *prikey, size_t prikey_len);

size_t aes_cbc_stream_decrypt(struct aes_cbc_stream *stream, const void *buf, size_t size);
struct aes_cbc_stream *aes_cbc_stream_new(aes_cbc_stream_mode_t mode, uint8_t *k, size_t klen, uint8_t salt[8]);
const uint8_t *aes_cbc_stream_get_bytes(struct aes_cbc_stream *stream, size_t *len);
aes_cbc_stream_mode_t aes_cbc_stream_get_mode(struct aes_cbc_stream *stream);
void aes_cbc_stream_clear_buffer(struct aes_cbc_stream *stream);
size_t aes_cbc_stream_encrypt(struct aes_cbc_stream *stream, const void *buf, size_t size);
size_t aes_cbc_stream_decrypt(struct aes_cbc_stream *stream, const void *buf, size_t size);
void aes_cbc_stream_delete(struct aes_cbc_stream *stream);

bool lisp_crypto_init(Lisp_VM *vm);
void ripemd160(const unsigned char *buf, size_t len, unsigned char hash[20]);
void sha256(const unsigned char *buf, size_t len, unsigned char hash[32]);
int hex_decode(const char *s, uint8_t *buf, int size);
bool aes_cbc_encrypt(
	const void *data,
	size_t size,
	const uint8_t *k,
	int klen,
	const uint8_t salt[8],
	uint8_t *outbuf);
bool aes_cbc_decrypt(
	const void *data,
	size_t size,
	const uint8_t *k,
	int klen,
	const uint8_t salt[8],
	uint8_t *outbuf);

