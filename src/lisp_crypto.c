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
/*
 * Extend LISP VM with crypto operators.
 *  - Keygen
 *  - Hashing
 *  - Encrypting
 */
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/bio.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>
#include <openssl/ecdh.h>
#include <openssl/ecdsa.h>
#include <openssl/buffer.h>
#include <openssl/rand.h>
#include <openssl/bn.h>

#include "base64.h"
#include "base58.h"
#include "lisp_crypto.h"
#include "common.h"

#define MAX_KEY_BUF 1024



struct keygen_info {
	unsigned char priv[MAX_KEY_BUF];
	int privlen;
	unsigned char pub[MAX_KEY_BUF];
	int publen;
};

typedef int (*keygen_callback)(void *ctx, EVP_PKEY*);

/*
   Generate keys until callback returns 0.
*/
static void secp256k1_keygen(keygen_callback callback, void *cb_ctx)
{
	EVP_PKEY_CTX *pctx=NULL, *kctx=NULL;
	EVP_PKEY *pkey = NULL, *params = NULL;
	
	/* Create the context for parameter generation */
	if(NULL == (pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL))) { 
		fprintf(stderr, "error: creating context for parameter generation\n");
		goto Error;
	}
	
	/* Initialise the parameter generation */
	if(1 != EVP_PKEY_paramgen_init(pctx)) {
		fprintf(stderr, "error: initializing parameter generation\n");
		goto Error;
	}
	
	/* We're going to use secp256k1 curve */
	if(1 != EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_secp256k1)) {
		fprintf(stderr, "error: can not use secp256k1\n");
		goto Error;
	}
	
	/* Create the parameter object params */
	if (!EVP_PKEY_paramgen(pctx, &params)) {
		fprintf(stderr, "parameter gen error\n");
		goto Error;
	}
	
	/* Create the context for the key generation */
	assert(NULL != (kctx = EVP_PKEY_CTX_new(params, NULL)));
	
	/* Generate the key */
	if (1 != EVP_PKEY_keygen_init(kctx))
		assert(0);

	while(1) {
		if (1 != EVP_PKEY_keygen(kctx, &pkey))
			assert(0);
		if (callback(cb_ctx, pkey) == 0)
			break;
	}
	
Error:
	if (params)
		EVP_PKEY_free(params);
	if (pctx)
		EVP_PKEY_CTX_free(pctx);
	if (kctx)
		EVP_PKEY_CTX_free(kctx);
	if (pkey)
		EVP_PKEY_free(pkey);
}

static void secp256k1_keygen_from_raw_vk(const uint8_t*vk, size_t vklen, struct keygen_info* info)
{
     EC_KEY *eckey = NULL;
     EC_POINT *pub_key = NULL;
     const EC_GROUP *group = NULL;
     BIGNUM *res = BN_bin2bn(vk, (int)vklen, NULL);

     BN_CTX *ctx;

     ctx = BN_CTX_new();
     eckey = EC_KEY_new_by_curve_name(NID_secp256k1);
     group = EC_KEY_get0_group(eckey);
     pub_key = EC_POINT_new(group);

     EC_KEY_set_private_key(eckey, res);

     /* pub_key is a new uninitialized `EC_POINT*`.  priv_key res is a `BIGNUM*`. */
     if (!EC_POINT_mul(group, pub_key, res, NULL, NULL, ctx))
       printf("Error at EC_POINT_mul.\n");

	info->publen = (int)EC_POINT_point2oct(group, pub_key, POINT_CONVERSION_UNCOMPRESSED,
			info->pub, sizeof(info->pub), NULL);
	
	BN_CTX_free(ctx);
	BN_free(res);
	EC_KEY_free(eckey);
	EC_POINT_free(pub_key);
}

void sha256(const unsigned char *buf, size_t len, unsigned char hash[SHA256_DIGEST_LENGTH])
{
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, buf, len);
    SHA256_Final(hash, &sha256);
}

/*
void sha1(const unsigned char *buf, size_t len, unsigned char hash[SHA1_DIGEST_LENGTH])
{
    SHA_CTX ctx;
    SHA_Init(&ctx);
    SHA_Update(&ctx, buf, len);
    SHA_Final(hash, &ctx);
}
*/

/* RIPE Message Digest, similar to SHA1 but double work  */
void ripemd160(const unsigned char *buf, size_t len, unsigned char hash[RIPEMD160_DIGEST_LENGTH])
{
	RIPEMD160_CTX c;
	RIPEMD160_Init(&c);
	RIPEMD160_Update(&c, buf, len);
	RIPEMD160_Final(hash, &c);
}

/* Callback to keygen */
static int on_keygen(void *ctx, EVP_PKEY *pkey)
{
	struct keygen_info *info = ctx;
	EC_KEY *ec_key = NULL;
	const EC_POINT *ec_point = NULL;
	const EC_GROUP *ec_group = NULL;
	const BIGNUM *bn = NULL;

	/* Get Public Key */
	if (NULL == (ec_key=EVP_PKEY_get1_EC_KEY(pkey))) {
		assert(0);
		return 0;
	}

	assert(NULL != (ec_point=EC_KEY_get0_public_key(ec_key)));
	assert(NULL != (ec_group=EC_KEY_get0_group(ec_key)));

	info->publen = (int)EC_POINT_point2oct(ec_group, ec_point, POINT_CONVERSION_UNCOMPRESSED,
			info->pub, sizeof(info->pub), NULL);

	if (NULL == (bn=EC_KEY_get0_private_key(ec_key))) {
		assert(0);
		return 0;
	}
	info->privlen = BN_bn2bin(bn, info->priv);
	return 0;
}

static int hex(int i)
{
	return i < 10? '0'+i : 'a'+(i-10);
}

/* out size should >= nbytes*2+1 */
static void hexify(const unsigned char *buf, size_t nbytes, char *out)
{
	for (unsigned i = 0; i < nbytes; i++) {
		*out++ = hex(buf[i]>>4);
		*out++ = hex(buf[i]&0xf);
	}
	*out = 0; 
}




static void *
KDF_SHA1(const void *in, size_t inlen, void *out, size_t * outlen)
{
	if (*outlen < SHA_DIGEST_LENGTH) {
		return NULL;
	} else {
		*outlen = SHA_DIGEST_LENGTH;
	}
	return SHA1(in, inlen, out);
}

static void *
KDF_SHA256(const void *in, size_t inlen, void *out, size_t * outlen)
{
	if (*outlen < SHA256_DIGEST_LENGTH) {
		return NULL;
	} else {
		*outlen = SHA256_DIGEST_LENGTH;
	}
	return SHA256(in, inlen, out);
}

//http://stackoverflow.com/questions/18155559/how-does-one-access-the-raw-ecdh-public-key-private-key-and-params-inside-opens

/*
 * ECDH: given two keys: A_pub, B_priv, and we should be able to
 * compute a shared secret between A, B.
 *
 * SharedSecret(A_pub,B_pri) == SharedSecret(A_pri,B_pub)
 *
 * Return the 32-byte secret in outbuf.
 */
int ecdh_shared_secret(uint8_t outbuf[], size_t outlen,
	const uint8_t *pubkey, size_t pubkey_len,
	const uint8_t *prikey, size_t prikey_len)
{
	EC_POINT *pub = NULL;
	EC_KEY *pri = EC_KEY_new_by_curve_name(NID_secp256k1);	
	EC_GROUP *ec_group = EC_GROUP_new_by_curve_name(NID_secp256k1);
	int n = 0;
	BIGNUM *bn = NULL;

	assert_e(pri != NULL && ec_group != NULL);
	assert_e(outlen >= 20);
	assert_e(NULL != (pub = EC_POINT_new(ec_group)));
	assert_e(0 != EC_POINT_oct2point(ec_group, pub, pubkey, pubkey_len, NULL));

	bn = BN_bin2bn(prikey, (int)prikey_len, NULL);
	assert_e(bn != NULL);
	assert_e(1 == EC_KEY_set_private_key(pri, bn));
	
	n = ECDH_compute_key(outbuf, outlen, pub, pri, KDF_SHA256);
	
Error:
	if (ec_group) {
		EC_GROUP_free(ec_group);
	}
	if (pub) {
		EC_POINT_free(pub);
	}
	if (pri) {
		EC_KEY_free(pri);
	}
	if (bn) {
		BN_free(bn);
	}
	return n;
}

static ECDSA_SIG *sign(const uint8_t *k, size_t klen, unsigned char *buf, size_t len)
{
	EC_KEY *pri = EC_KEY_new_by_curve_name(NID_secp256k1);	
	BIGNUM *bn = NULL;
	ECDSA_SIG *sig = NULL;
	unsigned char digest[SHA_DIGEST_LENGTH];
	
	bn = BN_bin2bn(k, (int)klen, NULL);
	assert_e(bn != NULL);
	assert_e(1 == EC_KEY_set_private_key(pri, bn));

	SHA1(buf, len, digest);
	sig = ECDSA_do_sign(digest, SHA_DIGEST_LENGTH, pri);
	assert_e(sig != NULL);
Error:
	if (pri)
		EC_KEY_free(pri);
	if (bn)
		BN_free(bn);
	return sig;	
}

static int hex_value(unsigned char c)
{
	if ('0' <= c && c <= '9') {
		return c - '0';
	} else if ('A' <= c && c <= 'F') {
		return c - 'A' + 10;
	} else if ('a' <= c && c <= 'f') {
		return c - 'a' + 10;
	} else {
		assert(0);
		return -1;
	}
}

static int verify(const uint8_t *k, size_t klen, unsigned char *buf, size_t len, const char *hex_sig)
{
	ECDSA_SIG *sig = NULL;
	int ret = 0;
	unsigned char digest[SHA_DIGEST_LENGTH];
	unsigned char * raw_sig = NULL;
	size_t i, n;
	EC_POINT *pub = NULL;
	EC_KEY *ec_key = EC_KEY_new_by_curve_name(NID_secp256k1);	
	EC_GROUP *ec_group = EC_GROUP_new_by_curve_name(NID_secp256k1);
	
	SHA1(buf, len, digest);

	n = strlen(hex_sig) / 2;

	assert_e(n > 0 && n * 2 == strlen(hex_sig));
	raw_sig = malloc(n);
	assert_e(raw_sig != NULL);
	
	for (i = 0; i < n; i++, hex_sig+=2) {
		raw_sig[i] = hex_value(hex_sig[0]) * 16 + hex_value(hex_sig[1]);
	}
	
	assert_e(NULL != (sig=d2i_ECDSA_SIG(NULL, (const unsigned char **)&raw_sig, n)));

	assert_e(NULL != (pub = EC_POINT_new(ec_group)));
	assert_e(0 != EC_POINT_oct2point(ec_group, pub, k, klen, NULL));
	assert_e(1 == EC_KEY_set_public_key(ec_key, pub));
	         
	ret = ECDSA_do_verify(digest, SHA_DIGEST_LENGTH, sig, ec_key);
	         
Error:
	         
	return ret;
}

/* Only string and buffer */
static const void *get_object_bytes(Lisp_VM *vm, Lisp_Object* obj, size_t *byte_count)
{
	if (lisp_string_p(obj)) {
		*byte_count = lisp_string_length((Lisp_String*)obj);
		return lisp_string_cstr((Lisp_String*)obj);
	} else if (lisp_buffer_p(obj)) {
		*byte_count = lisp_buffer_size((Lisp_Buffer*)obj);
		return lisp_buffer_data((Lisp_Buffer*)obj);
	} else {
		lisp_err(vm, "Expect string or buffer, got %s", lisp_object_type_name(obj));
		return NULL;
	}
}


/********************************************************/
static void op_sha1(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t len = 0;
	const uint8_t *bytes = get_object_bytes(vm, CAR(args), &len);
	Lisp_Buffer *buffer = lisp_buffer_new(vm, SHA_DIGEST_LENGTH);
	SHA1(bytes, len, lisp_buffer_bytes(buffer));
	lisp_buffer_set_size(buffer, SHA_DIGEST_LENGTH);
	PUSHX(vm, buffer);
}

struct sha256_stream {
	SHA256_CTX sha256;
	Lisp_Port *port;
};

size_t sha256_stream_write(void *context, const void *buf, size_t size)
{
	struct sha256_stream *s = context;
	if (s->port)
		lisp_port_put_bytes(s->port, buf, size);
	SHA256_Update(&s->sha256, buf, size);
	return size;
}

void sha256_stream_mark(void *context)
{
	struct sha256_stream *s = context;
	if (s->port)
		lisp_mark((Lisp_Object*)s->port);
}

struct lisp_stream_class_t sha256_stream_class = {
	.context_size = sizeof(struct sha256_stream),
	.write = sha256_stream_write,
	.mark = sha256_stream_mark
};

/* (open-sha256-output <output>)
 * Any bytes written to sha256 port will be passed through to <output>.
 * This allows on-the-fly hash calculation when writing data to disk.
 * Otherwise, we would need to reload the data from disk to do the hash calculation.
 * The sha256 of all written bytes can be read using
 * (sha256-output-finalize <sha256-port>).
 */
static void op_open_sha256_output(Lisp_VM *vm, Lisp_Pair *args)
{
	lisp_push_buffer(vm, NULL, 1024);
	Lisp_Stream *stream = lisp_push_stream(vm, &sha256_stream_class, NULL);
	struct sha256_stream *s = lisp_stream_context(stream);
	SHA256_Init(&s->sha256);
	if ((Lisp_Object*)args == lisp_nil) {
		// We don't have a sink
	} else if (!lisp_output_port_p(CAR(args))) {
		lisp_err(vm, "Bad output port");
	} else {
		s->port = (Lisp_Port*)CAR(args);
	}
	lisp_make_output_port(vm);
}

/* (sha256-output-finalize <sha256-port>)
 * Return the sha256 value of all written bytes as a buffer object.
 */
static void op_sha256_output_finalize(Lisp_VM *vm, Lisp_Pair *args)
{
	if (!lisp_output_port_p(CAR(args)))
		lisp_err(vm, "Bad output port");
	Lisp_Port *p = (Lisp_Port*)CAR(args);
	Lisp_Stream *stream = lisp_port_get_stream(p);
	if (lisp_stream_class(stream) != &sha256_stream_class)
		lisp_err(vm, "Bad sha256 port");
	lisp_port_flush(p); // Make sure all bytes are considered
	Lisp_Buffer *buffer = lisp_push_buffer(vm, NULL, SHA256_DIGEST_LENGTH);
	struct sha256_stream *s = lisp_stream_context(stream);
	SHA256_Final(lisp_buffer_bytes(buffer), &s->sha256);
	s->port = NULL;
	lisp_buffer_set_size(buffer, SHA256_DIGEST_LENGTH);
}

/* (sha256 <buffer|string|port>) */
static void op_sha256(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_Buffer *buffer = lisp_push_buffer(vm, NULL, SHA256_DIGEST_LENGTH);
	if (args == (Lisp_Pair*)lisp_nil || lisp_input_port_p(CAR(args))) {
		Lisp_Port *port;
		if (args == (Lisp_Pair*)lisp_nil)
			port = lisp_current_input(vm);
		else
			port = (Lisp_Port*)CAR(args);
		if (!port)
			lisp_err(vm, "sha256: missing arguments");
		SHA256_CTX sha256;
		SHA256_Init(&sha256);
		size_t n;
		while ((n = lisp_port_fill(port))) {
			SHA256_Update(&sha256, lisp_port_pending_bytes(port), n);
			lisp_port_drain(port, n);
		}
		SHA256_Final(lisp_buffer_bytes(buffer), &sha256);
	} else {
		size_t len = 0;
		const uint8_t *bytes = get_object_bytes(vm, CAR(args), &len);
		sha256(bytes, len, lisp_buffer_bytes(buffer));
	}
	lisp_buffer_set_size(buffer, SHA256_DIGEST_LENGTH);
}

static void op_rmd160(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t len = 0;
	const uint8_t *bytes = get_object_bytes(vm, CAR(args), &len);
	Lisp_Buffer *buffer = lisp_buffer_new(vm, RIPEMD160_DIGEST_LENGTH);
	ripemd160(bytes, len, lisp_buffer_bytes(buffer));
	lisp_buffer_set_size(buffer, RIPEMD160_DIGEST_LENGTH);
	PUSHX(vm, buffer);
}

/* Key needs to be stored in buffer, so that we can clear it */
static void op_secp256k1_keygen(Lisp_VM *vm, Lisp_Pair *args)
{
	struct keygen_info info;

	memset(&info, 0, sizeof(info));
	
	if ((Lisp_Object*)args == lisp_nil) {
		secp256k1_keygen(on_keygen, &info);
		Lisp_Buffer *priv = lisp_buffer_new(vm, info.privlen);
		lisp_buffer_add_bytes(priv, info.priv, info.privlen);
		PUSHX(vm, priv);
	} else {
		size_t vklen = 0;
		const uint8_t * vk = lisp_safe_bytes(vm, CAR(args), &vklen);
		secp256k1_keygen_from_raw_vk(vk, vklen, &info);
		PUSHX(vm, CAR(args));
	}
	
	Lisp_Buffer *pub = lisp_buffer_new(vm, info.publen);
	lisp_buffer_add_bytes(pub, info.pub, info.publen);
	PUSHX(vm, pub);
	lisp_cons(vm);
}

/* (ecdh <private-key> <public-kye>) */
static void op_ecdh(Lisp_VM *vm, Lisp_Pair *args)
{
	Lisp_Buffer *pri = (Lisp_Buffer*)CAR(args);
	Lisp_Buffer *pub = (Lisp_Buffer*)CADR(args);

	unsigned char secret[64];
	int n = ecdh_shared_secret(secret, sizeof(secret),
			lisp_buffer_bytes(pub), lisp_buffer_size(pub),
			lisp_buffer_bytes(pri), lisp_buffer_size(pri));
	if (n == 0)
		lisp_err(vm, "Invalid shared secret");
	lisp_push_buffer(vm, secret, n);
}

/*
 * (ecdsa-sign <message-digest> <pri>)
 */

static void op_ecdsa_sign(Lisp_VM *vm, Lisp_Pair *args)
{
	unsigned char *p = NULL;
	const char *md = lisp_safe_cstring(vm, CAR(args));
	Lisp_Buffer *pri = (Lisp_Buffer*)CADR(args);
	CHECK(vm, lisp_buffer_p(CADR(args)), "Not buffer");
	ECDSA_SIG *sig = sign(lisp_buffer_bytes(pri), lisp_buffer_size(pri), (void*)md, strlen(md));
	int len = i2d_ECDSA_SIG(sig, &p);
	char buf[256];
	hexify(p, len, buf);
    ECDSA_SIG_free(sig);
	lisp_push(vm, (Lisp_Object*)lisp_string_new(vm, buf, strlen(buf)));
}

/*
 * (ecdsa-verify <signature> <message-digest> <pub>)
 */
static void op_ecdsa_verify(Lisp_VM *vm, Lisp_Pair *args)
{
		const char *sig = lisp_safe_cstring(vm, CAR(args));
		const char *md = lisp_safe_cstring(vm, CADR(args));
		size_t publen = 0;
		const uint8_t *pub = lisp_safe_bytes(vm, CADDR(args), &publen);

		if (verify(pub, publen, (void*)md, strlen(md), sig)) {
			lisp_push(vm, lisp_true);
		} else {
			lisp_push(vm, lisp_false);
		}
}

static void op_hex_encode(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t len = 0;
	const void *data = get_object_bytes(vm, CAR(args), &len);
	Lisp_Buffer *b = lisp_buffer_new(vm, len*2+1);
	lisp_push(vm, (Lisp_Object*)b);
	hexify(data, len, lisp_buffer_data(b));
	lisp_push(vm, (Lisp_Object*)lisp_string_new(vm, lisp_buffer_data(b), len*2));
	lisp_push(vm, lisp_pop(vm, 2));
}

// TODO should ignore whitespaces
int hex_decode(const char *s, uint8_t *buf, int size)
{
	int i = 0;
	for (; i < size && *s; i++,s+=2)
	{
		int h = hex_value(s[0]);
		int l = hex_value(s[1]);
		if (h < 0 || l < 0)
			return -1;
		buf[i] = ((h<<4)|l);
	}
	return i;
}

static void op_hex_decode(Lisp_VM *vm, Lisp_Pair *args)
{
	if (!lisp_string_p(CAR(args))) {
		lisp_err(vm, "hex-decode: not a string");
	}
	Lisp_String *str = (Lisp_String*)CAR(args);
	const char *s = lisp_string_cstr(str);	
	size_t len = lisp_string_length(str);
	if (len % 2 != 0) {
		lisp_err(vm, "hex-decode: odd chars");
	}

	Lisp_Buffer *b = lisp_buffer_new(vm, len/2);
	lisp_push(vm, (Lisp_Object*)b);

	for (unsigned i = 0; i < len; i+=2) {
		int h = hex_value(s[i]);
		int l = hex_value(s[i+1]);
		lisp_buffer_add_byte(b, (h<<4)|l);
	}
}

/*
 * (base58-decode <b58string>)
 */
static void op_base58_decode(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *s = lisp_safe_cstring(vm, CAR(args));
	size_t n = base58_decode(s, NULL, 0);
	if (n > 0) {
		Lisp_Buffer *buffer = lisp_buffer_new(vm, n);
		n = base58_decode(s, lisp_buffer_bytes(buffer), n);
		if (n == 0) {
			lisp_push(vm, lisp_undef);
			return;
		}
		lisp_buffer_set_size(buffer, n);
		PUSHX(vm, buffer);
	} else {
		lisp_push(vm, lisp_undef);
	}
}

/*
 * (base58-encode <buffer>)
 */
static void op_base58_encode(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t len = 0;
	const void *bytes = get_object_bytes(vm, CAR(args), &len);
	size_t n = base58_encode(bytes, len, NULL, 0);
	if (n == 0) {
		lisp_push(vm, lisp_undef);
		return;
	}
	Lisp_Buffer *b = lisp_buffer_new(vm, n);
	n = base58_encode(bytes, len, lisp_buffer_bytes(b), n);
	if (n == 0) {
		lisp_err(vm, "base58 encode");
	}
	PUSHX(vm, b);
	lisp_push(vm, (Lisp_Object*)lisp_string_new(vm, lisp_buffer_bytes(b), n));
	lisp_push(vm, lisp_pop(vm,2));
}

/* (base58-check-encode <version> <payload>) */
static void op_base58_check_encode(Lisp_VM *vm, Lisp_Pair *args)
{
	int version = lisp_safe_int(vm, CAR(args));
	size_t len = 0;
	const void *bytes = get_object_bytes(vm, CADR(args), &len);
	
	Lisp_Buffer *buf = lisp_buffer_new(vm, 256);
	PUSHX(vm, buf);
	lisp_buffer_add_byte(buf, (uint8_t)version);
	lisp_buffer_add_bytes(buf, bytes, len);
	uint8_t hash1[SHA256_DIGEST_LENGTH];
	uint8_t hash2[SHA256_DIGEST_LENGTH];
	sha256(lisp_buffer_bytes(buf), len+1, hash1);
	sha256(hash1, sizeof(hash1), hash2);
	lisp_buffer_add_bytes(buf, hash2, 4);

	// Encode raw bytes in base58
	bytes = lisp_buffer_bytes(buf);
	len = lisp_buffer_size(buf);
	size_t n = base58_encode(bytes, len, NULL, 0);
	if (n == 0) {
		lisp_push(vm, lisp_undef);
		return;
	}
	Lisp_Buffer *b = lisp_buffer_new(vm, n);
	n = base58_encode(bytes, len, lisp_buffer_bytes(b), n);
	if (n == 0) {
		lisp_err(vm, "base58-encode");
	}
	PUSHX(vm, b);
	lisp_push(vm, (Lisp_Object*)lisp_string_new(vm, lisp_buffer_bytes(b), n));
	lisp_push(vm, lisp_pop(vm,3));
}

static void op_base58_check(Lisp_VM *vm, Lisp_Pair *args)
{
	assert(0);//Not implemented
}

static void op_base64_encode(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t len = 0;
	const void *data = get_object_bytes(vm, CAR(args), &len);
	size_t enclen = base64_enclen(len)+1;
	Lisp_Buffer *b = lisp_buffer_new(vm, enclen);
	lisp_push(vm, (Lisp_Object*)b);
	int n = base64_encode(data, len, lisp_buffer_data(b), enclen);
	if (n < 0) {
		lisp_err(vm, "base64_encode");
	}
	lisp_push(vm, (Lisp_Object*)lisp_string_new(vm, lisp_buffer_data(b), n));
	lisp_push(vm, lisp_pop(vm,2));
}

/* (base64-decode <b64string>) */
static void op_base64_decode(Lisp_VM *vm, Lisp_Pair *args)
{
	if (!lisp_string_p(CAR(args))) {
		lisp_err(vm, "base64-decode: not a string");
	}
	Lisp_String *str = (Lisp_String*)CAR(args);
	const char *s = lisp_string_cstr(str);	
	size_t len = lisp_string_length(str);
	size_t declen = base64_declen(len);
	Lisp_Buffer *b = lisp_buffer_new(vm, declen);
	lisp_push(vm, (Lisp_Object*)b);	
	int n = base64_decode(s, lisp_buffer_data(b), declen);
	if (n < 0) {
		lisp_err(vm, "base64_decode");
	}	
	lisp_buffer_set_size(b, n);
}

static const EVP_CIPHER * cipher_from_name(const char *name)
{
	if (strcmp(name, "aes-256-ecb") == 0)
		return EVP_aes_256_ecb();
	else if (strcmp(name, "aes-256-cbc") == 0)
		return EVP_aes_256_cbc();
	else if (strcmp(name, "aes-256-ofb") == 0)
		return EVP_aes_256_ofb();
	else if (strcmp(name, "aes-256-cfb8") == 0)
		return EVP_aes_256_cfb8();
	else if (strcmp(name, "aes-256-cfb128") == 0)
		return EVP_aes_256_cfb128();
	else
		return NULL;
}

#define AES_BLKSIZE 16

/* (encrypt <buffer|string> <cipher-type> <key> <iv>)
 */
static void op_encrypt(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t datalen = 0, keylen=0, ivlen=0;
	const uint8_t *data = (const uint8_t*)get_object_bytes(vm, CAR(args), &datalen);
	const char *cipher_name = lisp_safe_cstring(vm, CADR(args));
	const EVP_CIPHER *cipher = cipher_from_name(cipher_name);
	Lisp_Pair *params = (Lisp_Pair*)CDDR(args);
	const uint8_t *key = lisp_safe_bytes(vm, CAR(params), &keylen);
	const uint8_t *iv = lisp_safe_bytes(vm, CADR(params), &ivlen);

	if (cipher == NULL)
		lisp_err(vm, "Invalid cipher %s", cipher_name);
	if (keylen != 32 || ivlen != 32)
		lisp_err(vm, "Invalid key or iv: length must be 32");

	Lisp_Buffer *out = lisp_push_buffer(vm, NULL, datalen+16);

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();

	EVP_CIPHER_CTX_reset(ctx);
	EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv);
	int outlen = 0, flen=0;
	EVP_EncryptUpdate(ctx, lisp_buffer_bytes(out), &outlen, data, (int)datalen);
	EVP_EncryptFinal_ex(ctx, (uint8_t*)lisp_buffer_bytes(out)+outlen, &flen);
	lisp_buffer_set_size(out, outlen+flen);
	EVP_CIPHER_CTX_free(ctx);
}

/* (encrypt-from-input <in> <out> <size> <cipher-type> <key> <iv>)
 */
static void op_encrypt_from_input(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t keylen=0, ivlen=0;
	
	if (!lisp_input_port_p(CAR(args)) || !lisp_output_port_p(CADR(args)))
		lisp_err(vm, "Bad port");
	
	Lisp_Port *in = (Lisp_Port*)CAR(args);
	Lisp_Port *out = (Lisp_Port*)CADR(args);
	args = (Lisp_Pair*)CDDR(args);
    size_t in_size = lisp_safe_int(vm, CAR(args));
    size_t in_size_old = in_size;
	const char *cipher_name = lisp_safe_cstring(vm, CADR(args));
	const EVP_CIPHER *cipher = cipher_from_name(cipher_name);
	Lisp_Pair *params = (Lisp_Pair*)CDDR(args);
	const uint8_t *key = lisp_safe_bytes(vm, CAR(params), &keylen);
	const uint8_t *iv = lisp_safe_bytes(vm, CADR(params), &ivlen);

	if (cipher == NULL)
		lisp_err(vm, "Invalid cipher %s", cipher_name);
	if (keylen != 32 || ivlen != 32)
		lisp_err(vm, "Invalid key or iv: length must be 32");

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	EVP_CIPHER_CTX_reset(ctx);
	EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv);
	int outlen = 0, flen=0;
	
	size_t n, outcap;
	uint8_t outbuf[512];
	outcap = sizeof(outbuf);
	while (in_size > 0 && (n = lisp_port_fill(in))) {
		if (n > in_size)
			n = in_size;
		if (n > outcap)
			n = outcap;
		EVP_EncryptUpdate(ctx, outbuf, &outlen,
			lisp_port_pending_bytes(in), (int)n);
		assert(outlen == (int)n);
		lisp_port_put_bytes(out, outbuf, outlen);
		lisp_port_drain(in, n);
		in_size -= n;
	}

	EVP_EncryptFinal_ex(ctx, outbuf, &flen);
	if (flen > 0) {
		lisp_port_put_bytes(out, outbuf, flen);
	}
	EVP_CIPHER_CTX_free(ctx);
	
	if (in_size > 0)
		lisp_err(vm, "Input not processed in full");
	lisp_push_number(vm, (double)in_size_old);
}

/* (decrypt <buffer|string> <cipher-type> <key> <iv>)
 */
static void op_decrypt(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t datalen = 0, keylen=0, ivlen=0;
	const uint8_t *data = (const uint8_t*)get_object_bytes(vm, CAR(args), &datalen);
	const char *cipher_name = lisp_safe_cstring(vm, CADR(args));
	const EVP_CIPHER *cipher = cipher_from_name(cipher_name);
	Lisp_Pair *params = (Lisp_Pair*)CDDR(args);
	const uint8_t *key = lisp_safe_bytes(vm, CAR(params), &keylen);
	const uint8_t *iv = lisp_safe_bytes(vm, CADR(params), &ivlen);

	if (cipher == NULL)
		lisp_err(vm, "Invalid cipher %s", cipher_name);
	if (keylen != 32 || ivlen != 32)
		lisp_err(vm, "Invalid key or iv: length must be 32");
	Lisp_Buffer *out = lisp_push_buffer(vm, NULL, datalen+16);
	
	int outlen = 0, flen=0;
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	EVP_DecryptInit_ex(ctx, cipher, NULL, key, iv);
	EVP_DecryptUpdate(ctx, (uint8_t*)lisp_buffer_bytes(out), &outlen, data, (int)datalen);
	EVP_DecryptFinal_ex(ctx, (uint8_t*)lisp_buffer_bytes(out)+outlen, &flen);
	lisp_buffer_set_size(out, outlen+flen);
	EVP_CIPHER_CTX_free(ctx);
}

/* (decrypt-from-input <in> <out> <size> <cipher-type> <key> <iv>)
 */
static void op_decrypt_from_input(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t keylen=0, ivlen=0;
	
	if (!lisp_input_port_p(CAR(args)) || !lisp_output_port_p(CADR(args)))
		lisp_err(vm, "Bad port");
	
	Lisp_Port *in = (Lisp_Port*)CAR(args);
	Lisp_Port *out = (Lisp_Port*)CADR(args);
	args = (Lisp_Pair*)CDDR(args);
    size_t in_size = lisp_safe_int(vm, CAR(args));
    size_t in_size_old = in_size;
	const char *cipher_name = lisp_safe_cstring(vm, CADR(args));
	const EVP_CIPHER *cipher = cipher_from_name(cipher_name);
	Lisp_Pair *params = (Lisp_Pair*)CDDR(args);
	const uint8_t *key = lisp_safe_bytes(vm, CAR(params), &keylen);
	const uint8_t *iv = lisp_safe_bytes(vm, CADR(params), &ivlen);

	if (cipher == NULL)
		lisp_err(vm, "Invalid cipher %s", cipher_name);
	if (keylen != 32 || ivlen != 32)
		lisp_err(vm, "Invalid key or iv: length must be 32");

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	EVP_CIPHER_CTX_reset(ctx);
	int outlen = 0, flen=0;
	EVP_DecryptInit_ex(ctx, cipher, NULL, key, iv);

	size_t n, outcap;
	uint8_t outbuf[512];
	outcap = sizeof(outbuf);
	while (in_size > 0 && (n = lisp_port_fill(in))) {
		if (n > in_size)
			n = in_size;
		if (n > outcap)
			n = outcap;
		EVP_DecryptUpdate(ctx, outbuf, &outlen,
			lisp_port_pending_bytes(in), (int)n);
		assert(outlen == (int)n);
		lisp_port_put_bytes(out, outbuf, outlen);
		lisp_port_drain(in, n);
		in_size -= n;
	}
	EVP_DecryptFinal_ex(ctx, outbuf, &flen);
	if (flen > 0) {
		lisp_port_put_bytes(out, outbuf, flen);
	}
	EVP_CIPHER_CTX_free(ctx);
	
	if (in_size > 0)
		lisp_err(vm, "Input not processed in full");
	lisp_push_number(vm, (double)in_size_old);
}

/*
 * (pbkdf2-hmac-sha1 <pass> <salt> &optional <itercnt>)
 *
 * Derive key from plain text password and salt with iteration count.
 * By default, itercnt is 10000.
 */
static void op_pbkdf2_hmac_sha1(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t passlen = 0, saltlen = 0;
	const void *pass = get_object_bytes(vm, CAR(args), &passlen);
	const void *salt = get_object_bytes(vm, CADR(args), &saltlen);
	int itercnt = 10000;
	if (CDDR(args) != lisp_nil)
		itercnt = lisp_safe_int(vm, CAR(CDDR(args)));
	int dklen = 32;
	Lisp_Buffer *b = lisp_push_buffer(vm, NULL, dklen);
	uint8_t *out = lisp_buffer_bytes(b);
	PKCS5_PBKDF2_HMAC_SHA1(pass, (int)passlen, salt, (int)saltlen, itercnt, dklen, out);
	lisp_buffer_set_size(b, dklen);
}

#define AES_NUM_ROUNDS 5
#define AES_STREAM_BUFSIZE 4096


struct aes_cbc_stream {
	EVP_CIPHER_CTX* ctx;
	int mode;
	uint8_t buf[AES_STREAM_BUFSIZE+AES_BLOCK_SIZE];
	size_t buf_len;
};

struct aes_cbc_stream *aes_cbc_stream_new(aes_cbc_stream_mode_t mode, uint8_t *k, size_t klen, uint8_t salt[8])
{
	struct aes_cbc_stream *stream = calloc(1, sizeof(struct aes_cbc_stream));
	unsigned char key[32], iv[32];
	
	assert(stream != NULL);
	/*
	 * Gen key & IV for AES 256 CBC mode. A SHA1 digest is used to hash the supplied key material.
	 * nrounds is the number of times the we hash the material.
	 * More rounds are more secure but slower.
	 */
	size_t n = EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha1(), salt,
			      (const uint8_t*)k, (int)klen, AES_NUM_ROUNDS, key, iv);
	if (n != 32) {
		free(stream);
		return NULL;
	}

	stream->ctx = EVP_CIPHER_CTX_new();
	if (stream->ctx == NULL) {
		free(stream);
		return NULL;
	}
	EVP_CIPHER_CTX_reset(stream->ctx);
	if (mode == AES_CBC_STREAM_ENCRYPT) {
    		EVP_EncryptInit_ex(stream->ctx, EVP_aes_256_cbc(), NULL, key, iv);
    		stream->mode = mode;
	} else if (mode == AES_CBC_STREAM_DECRYPT) {
    		EVP_DecryptInit_ex(stream->ctx, EVP_aes_256_cbc(), NULL, key, iv);
    		stream->mode = mode;
	}
	EVP_CIPHER_CTX_set_padding(stream->ctx, 0);
	return stream;
}

const uint8_t *aes_cbc_stream_get_bytes(struct aes_cbc_stream *stream, size_t *len)
{
	if (len) *len = stream->buf_len;
	return stream->buf;
}

aes_cbc_stream_mode_t aes_cbc_stream_get_mode(struct aes_cbc_stream *stream)
{
	return stream->mode;
}

void aes_cbc_stream_clear_buffer(struct aes_cbc_stream *stream)
{
	stream->buf_len = 0;
}

// Return consumed bytes
size_t aes_cbc_stream_encrypt(struct aes_cbc_stream *stream, const void *buf, size_t size)
{
	assert(stream->mode == AES_CBC_STREAM_ENCRYPT);

	size_t avail = sizeof(stream->buf) - stream->buf_len;
	if (avail <= AES_BLOCK_SIZE) {
		return 0;
	}

	if (buf == NULL) { // finalize
		int f_len = 0;
	    	EVP_EncryptFinal_ex(stream->ctx, stream->buf+stream->buf_len, &f_len);
	    	stream->buf_len += f_len;
	    	size = 0;
	    	stream->mode = AES_CBC_STREAM_FINALIZED;
	} else {
		int c_len = 0;
		avail -= AES_BLOCK_SIZE;
		if (size > avail) {
			size = avail;
		}
		EVP_EncryptUpdate(stream->ctx, stream->buf+stream->buf_len, &c_len, buf, (int)size);
		stream->buf_len += c_len;
	}
	return size;
}

size_t aes_cbc_stream_decrypt(struct aes_cbc_stream *stream, const void *buf, size_t size)
{
	int len = 0;
	assert(stream->mode == AES_CBC_STREAM_DECRYPT);

	size_t avail = sizeof(stream->buf) - stream->buf_len;
	if (avail < AES_BLOCK_SIZE)
		return 0;
	
	if (buf == NULL) { // finalize
  		EVP_DecryptFinal_ex(stream->ctx, stream->buf+stream->buf_len, &len);
  		stream->buf_len += len;
		size = 0;
		stream->mode = AES_CBC_STREAM_FINALIZED;
	} else {
		if (size > avail) {
			size = avail;
		}
		EVP_DecryptUpdate(stream->ctx, stream->buf+stream->buf_len, &len, buf, (int)size);
		stream->buf_len += len;
	}
	return size;
}

void aes_cbc_stream_delete(struct aes_cbc_stream *stream)
{
	if (stream->ctx) {
	    EVP_CIPHER_CTX_reset(stream->ctx);
	    EVP_CIPHER_CTX_free(stream->ctx);
	    stream->ctx = NULL;
	}
}

bool aes_cbc_encrypt(
	const void *data,
	size_t size,
	const uint8_t *k,
	int klen,
	const uint8_t salt[8],
	uint8_t *outbuf)
{
    unsigned char key[32], iv[32];
	int f_len = 0;
	int outsize = 0;
	
	if (size % AES_BLOCK_SIZE != 0)
	{
		assert(0);
		return false;
	}
	
    /*
     * Gen key & IV for AES 256 CBC mode. A SHA1 digest is used to hash the supplied key material.
     * nrounds is the number of times the we hash the material. More rounds are more secure but
     * slower.
     */
    size_t n = EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha1(), salt,
                              k, klen, AES_NUM_ROUNDS, key, iv);
    assert(n == 32);
	
    EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_reset(e_ctx);
	EVP_CIPHER_CTX_set_padding(e_ctx, 0);
    EVP_EncryptInit_ex(e_ctx, EVP_aes_256_cbc(), NULL, key, iv);
	EVP_EncryptUpdate(e_ctx, outbuf, &outsize, data, (int)size);
	assert(outsize == size);
//	int ok = EVP_EncryptFinal_ex(e_ctx, outbuf+outsize, &f_len);
//	assert(ok && outsize+f_len==size);

    EVP_CIPHER_CTX_free(e_ctx);

	return true;
}

bool aes_cbc_decrypt(
	const void *data,
	size_t size,
	const uint8_t *k,
	int klen,
	const uint8_t salt[8],
	uint8_t *outbuf)
{
	int outsize = 0;
	int f_len = 0;
	
	if (size % AES_BLOCK_SIZE != 0)
	{
		assert(0);
		return false;
	}
	
    unsigned char key[32], iv[32];
	
    size_t n = EVP_BytesToKey(
    	EVP_aes_256_cbc(),
    	EVP_sha1(),
    	salt,
		k, klen,
		AES_NUM_ROUNDS,
		key, iv);
    assert(n == 32);

    EVP_CIPHER_CTX* d_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_reset(d_ctx);
	EVP_CIPHER_CTX_set_padding(d_ctx, 0);

    EVP_DecryptInit_ex(d_ctx, EVP_aes_256_cbc(), NULL, key, iv);
	EVP_DecryptUpdate(d_ctx, outbuf, &outsize, data, (int)size);
	//int ok = EVP_DecryptFinal_ex(d_ctx, outbuf+outsize, &f_len);
	//assert(ok);
	//assert(outsize + f_len == size);
    EVP_CIPHER_CTX_free(d_ctx);
	return true;
}

/* (aes-encrypt <buffer|string> <key> ) => buffer. PKCS padded */
static void op_aes_cbc_encrypt(Lisp_VM *vm, Lisp_Pair *args)
{
    size_t len = 0;
    const uint8_t *data = (const uint8_t*)get_object_bytes(vm, CAR(args), &len);
    const char *k = lisp_safe_cstring(vm, CADR(args));
    
    
    /* max ciphertext len for a n bytes of plaintext is n + AES_BLOCK_SIZE -1 bytes */
    int c_len = (int)len + AES_BLOCK_SIZE, f_len = 0;
    Lisp_Buffer *b = lisp_buffer_new(vm, c_len);
    unsigned char *ciphertext = lisp_buffer_data(b);
    unsigned char key[32], iv[32];
    
    /*
     * Gen key & IV for AES 256 CBC mode. A SHA1 digest is used to hash the supplied key material.
     * nrounds is the number of times the we hash the material. More rounds are more secure but
     * slower.
     */
    size_t n = EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha1(), NULL/*salt*/,
                              (const uint8_t*)k, (int)strlen(k), AES_NUM_ROUNDS, key, iv);
    assert(n == 32);
    
    EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_reset(e_ctx);

    EVP_EncryptInit_ex(e_ctx, EVP_aes_256_cbc(), NULL, key, iv);
    
    /* update ciphertext, c_len is filled with the length of ciphertext generated,
     *len is the size of plaintext in bytes */
    EVP_EncryptUpdate(e_ctx, ciphertext, &c_len, data, (int)len);
    
    /* update ciphertext with the final remaining bytes */
    EVP_EncryptFinal_ex(e_ctx, ciphertext+c_len, &f_len);
    EVP_CIPHER_CTX_reset(e_ctx);
    EVP_CIPHER_CTX_free(e_ctx);
    
    lisp_buffer_set_size(b, c_len + f_len);
    lisp_push(vm, (Lisp_Object*)b);
}


/* (aes-encrypt <buffer> <key>) => buffer */
static void op_aes_cbc_decrypt(Lisp_VM *vm, Lisp_Pair *args)
{
	if (!lisp_buffer_p(CAR(args))) {
		lisp_err(vm, "decrypt");
	}
	Lisp_Buffer *b1 = (Lisp_Buffer*)CAR(args);
	uint8_t *data = (uint8_t*)lisp_buffer_data(b1);
	size_t len = lisp_buffer_size(b1);
	if (len == 0 || len % AES_BLKSIZE != 0) {
		lisp_err(vm, "decrypt");
	}
    
	const char *k = lisp_safe_cstring(vm, CADR(args));
    unsigned char key[32], iv[32];
    
    size_t n = EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha1(), NULL/*salt*/,
                              (const uint8_t*)k, (int)strlen(k), AES_NUM_ROUNDS, key, iv);
    assert(n == 32);

  /* plaintext will always be equal to or lesser than length of ciphertext*/
    Lisp_Buffer *b2 = lisp_buffer_new(vm, len);
    uint8_t *plaintext = (uint8_t*)lisp_buffer_data(b2);

    EVP_CIPHER_CTX* d_ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_reset(d_ctx);
    EVP_DecryptInit_ex(d_ctx, EVP_aes_256_cbc(), NULL, key, iv);
  int p_len,f_len;
EVP_DecryptUpdate(d_ctx, plaintext, &p_len, data, (int)len);
  int ok = EVP_DecryptFinal_ex(d_ctx, plaintext+p_len, &f_len);
      EVP_CIPHER_CTX_reset(d_ctx);
    EVP_CIPHER_CTX_free(d_ctx);
   if (!ok) {
        lisp_err(vm, "decrypt: finalize error");
    }
	
	lisp_buffer_set_size(b2, p_len + f_len);
	lisp_push(vm, (Lisp_Object*)b2);
}

static void op_bin_decode(Lisp_VM *vm, Lisp_Pair *args)
{
	const char *s = lisp_safe_cstring(vm, CAR(args));
	int n = 0;
	for (const char *p = s; *p; p++)
		if (*p == '0' || *p == '1')
			n++;
	if (n % 8 != 0)
		lisp_err(vm, "Invalid bin string: not multiple of 8");
	Lisp_Buffer *r = lisp_buffer_new(vm, n/8);
	int t = 0;
	n = 0;
	for (const char *p = s; *p; p++)
	{
		if (*p == '0' || *p == '1')
		{
			t = (t << 1) | (*p - '0');
			if (++n == 8)
			{
				lisp_buffer_add_byte(r, t);
				t = 0;
				n = 0;
			}
		}
	}
	PUSHX(vm, r);
}

static void op_bin_encode(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len = 0;
	uint8_t *a =lisp_safe_bytes(vm, CAR(args), &a_len);
	Lisp_String *s = lisp_push_string(vm, NULL, a_len*8);
	char *p = (char*)lisp_string_cstr(s);
	for (unsigned i = 0; i < a_len; i++)
	{
		for (int j = 7; j >= 0;j--)
		{
			*p++ = '0' + ((a[i] >> j) & 1);
		}
	}
}

static void op_bitwise_clz(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	int n = 0;
	for (unsigned i = 0; i < a_len; i++)
	{
		if (a[i] == 0)
		{
			n+=8;
			continue;
		}
		
		if ((a[i] >> 1) == 0)
			n+=7;
		else if ((a[i] >> 2) == 0)
			n+=6;
		else if ((a[i] >> 3) == 0)
			n+=5;
		else if ((a[i] >> 4) == 0)
			n+=4;
		else if ((a[i] >> 5) == 0)
			n+=3;
		else if ((a[i] >> 6) == 0)
			n+=2;
		else if ((a[i] >> 7) == 0)
			n+=1;
		break;
	}
	PUSHX(vm, lisp_number_new(vm, n));
}

static void op_bitwise_not(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	Lisp_Buffer *r = lisp_buffer_new(vm, a_len);
	for (unsigned i = 0; i < a_len; i++) {
		lisp_buffer_add_byte(r, ~a[i]);
	}
	PUSHX(vm, r);
}

static void op_bitwise_and(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0,b_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	uint8_t *b = lisp_safe_bytes(vm, CADR(args), &b_len);
	if (a_len != b_len)
		lisp_err(vm, "Not equal bytes: %ld %ld", a_len, b_len);
	Lisp_Buffer *r = lisp_buffer_new(vm, a_len);
	for (unsigned i = 0; i < a_len; i++) {
		lisp_buffer_add_byte(r, a[i] & b[i]);
	}
	PUSHX(vm, r);
}

static void op_bitwise_add(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0,b_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	uint8_t *b = lisp_safe_bytes(vm, CADR(args), &b_len);
	if (a_len != b_len)
		lisp_err(vm, "Not equal bytes: %ld %ld", a_len, b_len);

	Lisp_Buffer *r = lisp_buffer_new(vm, a_len);
	int carry = 0;
	for (int i = (int)a_len-1; i>=0; i--)
	{
		int sum = a[i] + b[i] + carry;
		lisp_buffer_add_byte(r, sum);
		carry = (sum > 255 ? 1 : 0);
	}
	PUSHX(vm, r);
}

static void op_bitwise_or(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0,b_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	uint8_t *b = lisp_safe_bytes(vm, CADR(args), &b_len);
	if (a_len != b_len)
		lisp_err(vm, "Not equal bytes: %ld %ld", a_len, b_len);
	Lisp_Buffer *r = lisp_buffer_new(vm, a_len);
	for (unsigned i = 0; i < a_len; i++) {
		lisp_buffer_add_byte(r, a[i] | b[i]);
	}
	PUSHX(vm, r);
}


static void op_bitwise_xor(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0,b_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	uint8_t *b = lisp_safe_bytes(vm, CADR(args), &b_len);
	if (a_len != b_len)
		lisp_err(vm, "Not equal bytes: %ld %ld", a_len, b_len);
	Lisp_Buffer *r = lisp_buffer_new(vm, a_len);
	for (unsigned i = 0; i < a_len; i++) {
		lisp_buffer_add_byte(r, a[i] ^ b[i]);
	}
	PUSHX(vm, r);
}

static void setbits(uint8_t *s, size_t n, int begin, int end, int bit_value)
{
	int i = (begin & ~7);
	int mask;
	for (; i < end; i += 8) {
		if (i < begin)
		{
			if (i + 8 <= end) {
				mask = (0xff >> (begin - i));
			} else {
				mask = ((0xff >> (begin-i)) & (0xff << (i+8-end)));
			}
		}
		else
		{
			if (i+8 <= end) {
				mask = 0xff;
			} else {
				mask = (0xff <<  (i+8-end));
			}
		}
		if (bit_value == 0)
			s[i>>3] &= ~mask;
		else
			s[i>>3] |= mask;
	}
}

static void op_bitwise_setbits(Lisp_VM *vm, Lisp_Pair *args, int bit_value)
{
	size_t a_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	Lisp_Object *oStart = CADR(args);
	Lisp_Object *oLength = CAR(CDDR(args));
	int start;
	int len;
	if (oStart == lisp_undef)
		start = 0;
	else
		start = lisp_safe_int(vm, oStart);

	if (oLength == lisp_undef)
		len = (int)a_len*8;
	else
		len = lisp_safe_int(vm, oLength);
	
	if (start < 0 || start >= (int)a_len*8 || len < 0 || len > (int)a_len*8)
		lisp_err(vm, "Invalid start or length");

	Lisp_Buffer *r = lisp_buffer_copy(vm, a, a_len);
	setbits(lisp_buffer_bytes(r), lisp_buffer_size(r), start, start+len, bit_value);
	PUSHX(vm, r);
}

static void op_bitwise_clear(Lisp_VM *vm, Lisp_Pair *args)
{
	op_bitwise_setbits(vm, args, 0);
}

static void op_bitwise_set(Lisp_VM *vm, Lisp_Pair *args)
{
	op_bitwise_setbits(vm, args, 1);
}

static uint8_t extract8b(const uint8_t *s, size_t n, int bit_index, int default_bit)
{
	unsigned i = (bit_index & ~7) / 8;
	int w = 0;
	
	if (i < 0 || i >= n) {
		w = (default_bit ? 0xff00 : 0x0000);
	} else {
		w = ((int)s[i] << 8);
	}
	
	i++;
	if (i < 0 || i >= n) {
		w |= (default_bit ? 0x00ff : 0x0000);
	} else {
		w |= s[i];
	}

	w <<= (bit_index & 7);
	return (uint8_t)(w >> 8);
}

// (bitwise-lsl bit-buffer count)
// Logical shift to left
static void op_bitwise_lsl(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	int n = lisp_safe_int(vm, CADR(args));
	Lisp_Buffer *r = lisp_buffer_new(vm, a_len);
	for (unsigned i = 0; i < a_len; i++) {
		lisp_buffer_add_byte(r, extract8b(a, a_len, i*8+n, 0));
	}
	PUSHX(vm, r);
}

static void op_bitwise_lsr(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	int n = lisp_safe_int(vm, CADR(args));
	Lisp_Buffer *r = lisp_buffer_new(vm, a_len);
	for (unsigned i = 0; i < a_len; i++) {
		lisp_buffer_add_byte(r, extract8b(a, a_len, i*8-n, 0));
	}
	PUSHX(vm, r);
}

static void op_bitwise_asr(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	int n = lisp_safe_int(vm, CADR(args));
	int msb = 0;
	if (a_len > 0)
		msb = ((a[0] >> 7) & 1);
	Lisp_Buffer *r = lisp_buffer_new(vm, a_len);
	for (unsigned i = 0; i < a_len; i++) {
		lisp_buffer_add_byte(r, extract8b(a, a_len, i*8-n, msb));
	}
	PUSHX(vm, r);
}

static void op_bitwise_compare(Lisp_VM *vm, Lisp_Pair *args)
{
	size_t a_len=0,b_len=0;
	uint8_t *a = lisp_safe_bytes(vm, CAR(args), &a_len);
	uint8_t *b = lisp_safe_bytes(vm, CADR(args), &b_len);
	if (a_len != b_len)
		lisp_err(vm, "Not equal bytes");
	int r = memcmp(a, b, a_len);
	lisp_push(vm, (Lisp_Object*)lisp_number_new(vm, r));
}

// Strong random bytes for cryptography use
static void op_random_bytes(Lisp_VM *vm, Lisp_Pair *args)
{
	int n = lisp_safe_int(vm, CAR(args));
	if (n <= 0)
		lisp_err(vm, "Invalid count: %d", n);
	
	Lisp_Buffer *b = lisp_push_buffer(vm, NULL, n);
	int ret = RAND_bytes(lisp_buffer_bytes(b), n);
	assert(ret == 1);
	if (ret != 1)
		lisp_err(vm, "random-bytes: RAND_bytes() is not working");
	lisp_buffer_set_size(b, n);
}

/* (fill-bytes <count> &optional <byte-value>)
 * Default <byte-value> is 0.
 */
static void op_fill_bytes(Lisp_VM *vm, Lisp_Pair *args)
{
	int n = lisp_safe_int(vm, CAR(args));
	if (n <= 0)
		lisp_err(vm, "Invalid count");
	int b = 0;
	if (CDR(args) != lisp_nil) {
		b = lisp_safe_int(vm, CADR(args));
	}
	Lisp_Buffer *r = lisp_buffer_new(vm, n);
	for (int i = 0; i < n; i++) {
		lisp_buffer_add_byte(r, b);
	}
	PUSHX(vm, r);
}

bool lisp_crypto_init(Lisp_VM *vm)
{
	lisp_defn(vm, "keygen-secp256k1",    op_secp256k1_keygen);
	lisp_defn(vm, "sha256",              op_sha256);
	lisp_defn(vm, "rmd160",              op_rmd160);
	lisp_defn(vm, "sha1",                op_sha1);
	lisp_defn(vm, "ecdsa-sign",          op_ecdsa_sign);
	lisp_defn(vm, "ecdsa-verify",        op_ecdsa_verify);
	lisp_defn(vm, "ecdh",                op_ecdh);
	lisp_defn(vm, "hex-encode",          op_hex_encode);
	lisp_defn(vm, "hex-decode",          op_hex_decode);
	lisp_defn(vm, "base64-encode",       op_base64_encode);
	lisp_defn(vm, "base64-decode",       op_base64_decode);
	lisp_defn(vm, "base58-encode",       op_base58_encode);
	lisp_defn(vm, "base58-decode",       op_base58_decode);
	lisp_defn(vm, "base58-check-encode", op_base58_check_encode);
	lisp_defn(vm, "base58-check",        op_base58_check);
	lisp_defn(vm, "encrypt",             op_encrypt);
	lisp_defn(vm, "decrypt",             op_decrypt);
	lisp_defn(vm, "encrypt-from-input",  op_encrypt_from_input);
	lisp_defn(vm, "decrypt-from-input",  op_decrypt_from_input);
	lisp_defn(vm, "aes-cbc-encrypt",     op_aes_cbc_encrypt);
	lisp_defn(vm, "aes-cbc-decrypt",     op_aes_cbc_decrypt);
	lisp_defn(vm, "bitwise-not",         op_bitwise_not);
	lisp_defn(vm, "bitwise-and",         op_bitwise_and);
	lisp_defn(vm, "bitwise-add",         op_bitwise_add);
	lisp_defn(vm, "bitwise-or",          op_bitwise_or);
	lisp_defn(vm, "bitwise-xor",         op_bitwise_xor);
	lisp_defn(vm, "bitwise-set",         op_bitwise_set);
	lisp_defn(vm, "bitwise-clear",       op_bitwise_clear);
	lisp_defn(vm, "bitwise-compare",     op_bitwise_compare);
	lisp_defn(vm, "bitwise-lsr",         op_bitwise_lsr);
	lisp_defn(vm, "bitwise-asr",         op_bitwise_asr);
	lisp_defn(vm, "bitwise-lsl",         op_bitwise_lsl);
	lisp_defn(vm, "bitwise-clz",         op_bitwise_clz);
	lisp_defn(vm, "bin-encode",          op_bin_encode);
	lisp_defn(vm, "bin-decode",          op_bin_decode);
	lisp_defn(vm, "random-bytes",        op_random_bytes);
	lisp_defn(vm, "fill-bytes",          op_fill_bytes);
	lisp_defn(vm, "pbkdf2-hmac-sha1",    op_pbkdf2_hmac_sha1);
	lisp_defn(vm, "open-sha256-output",  op_open_sha256_output);
	lisp_defn(vm, "sha256-output-finalize", op_sha256_output_finalize);
	return true;
}


