/*	$OpenBSD: ssl_seclevel.c,v 1.9 2022/06/29 21:10:20 tb Exp $ */
/*
 * Copyright (c) 2020 Theo Buehler <tb@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stddef.h>

#include <openssl/asn1.h>
#include <openssl/dh.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/objects.h>
#include <openssl/ossl_typ.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/x509.h>

#include "ssl_locl.h"

static int
ssl_security_normalize_level(const SSL_CTX *ctx, const SSL *ssl, int *out_level)
{
	int security_level;

	if (ctx != NULL)
		security_level = SSL_CTX_get_security_level(ctx);
	else
		security_level = SSL_get_security_level(ssl);

	if (security_level < 0)
		security_level = 0;
	if (security_level > 5)
		security_level = 5;

	*out_level = security_level;

	return 1;
}

static int
ssl_security_level_to_minimum_bits(int security_level, int *out_minimum_bits)
{
	if (security_level < 0)
		return 0;

	if (security_level == 0)
		*out_minimum_bits = 0;
	else if (security_level == 1)
		*out_minimum_bits = 80;
	else if (security_level == 2)
		*out_minimum_bits = 112;
	else if (security_level == 3)
		*out_minimum_bits = 128;
	else if (security_level == 4)
		*out_minimum_bits = 192;
	else if (security_level >= 5)
		*out_minimum_bits = 256;

	return 1;
}

static int
ssl_security_level_and_minimum_bits(const SSL_CTX *ctx, const SSL *ssl,
    int *out_level, int *out_minimum_bits)
{
	int security_level = 0, minimum_bits = 0;

	if (!ssl_security_normalize_level(ctx, ssl, &security_level))
		return 0;
	if (!ssl_security_level_to_minimum_bits(security_level, &minimum_bits))
		return 0;

	if (out_level != NULL)
		*out_level = security_level;
	if (out_minimum_bits != NULL)
		*out_minimum_bits = minimum_bits;

	return 1;
}

static int
ssl_security_secop_cipher(const SSL_CTX *ctx, const SSL *ssl, int bits,
    void *arg)
{
	const SSL_CIPHER *cipher = arg;
	int security_level, minimum_bits;

	if (!ssl_security_level_and_minimum_bits(ctx, ssl, &security_level,
	    &minimum_bits))
		return 0;

	if (security_level <= 0)
		return 1;

	if (bits < minimum_bits)
		return 0;

	/* No unauthenticated ciphersuites. */
	if (cipher->algorithm_auth & SSL_aNULL)
		return 0;

	if (security_level <= 1)
		return 1;

	if (cipher->algorithm_enc == SSL_RC4)
		return 0;

	if (security_level <= 2)
		return 1;

	/* Security level >= 3 requires a cipher with forward secrecy. */
	if ((cipher->algorithm_mkey & (SSL_kDHE | SSL_kECDHE)) == 0 &&
	    cipher->algorithm_ssl != SSL_TLSV1_3)
		return 0;

	return 1;
}

static int
ssl_security_secop_version(const SSL_CTX *ctx, const SSL *ssl, int version)
{
	int min_version = TLS1_2_VERSION;
	int security_level;

	if (!ssl_security_level_and_minimum_bits(ctx, ssl, &security_level, NULL))
		return 0;

	if (security_level < 4)
		min_version = TLS1_1_VERSION;
	if (security_level < 3)
		min_version = TLS1_VERSION;

	return ssl_tls_version(version) >= min_version;
}

static int
ssl_security_secop_compression(const SSL_CTX *ctx, const SSL *ssl)
{
	return 0;
}

static int
ssl_security_secop_tickets(const SSL_CTX *ctx, const SSL *ssl)
{
	int security_level;

	if (!ssl_security_level_and_minimum_bits(ctx, ssl, &security_level, NULL))
		return 0;

	return security_level < 3;
}

static int
ssl_security_secop_tmp_dh(const SSL_CTX *ctx, const SSL *ssl, int bits)
{
	int security_level, minimum_bits;

	if (!ssl_security_level_and_minimum_bits(ctx, ssl, &security_level,
	    &minimum_bits))
		return 0;

	/* Disallow DHE keys weaker than 1024 bits even at security level 0. */
	if (security_level <= 0 && bits < 80)
		return 0;

	return bits >= minimum_bits;
}

static int
ssl_security_secop_default(const SSL_CTX *ctx, const SSL *ssl, int bits)
{
	int minimum_bits;

	if (!ssl_security_level_and_minimum_bits(ctx, ssl, NULL, &minimum_bits))
		return 0;

	return bits >= minimum_bits;
}

int
ssl_security_default_cb(const SSL *ssl, const SSL_CTX *ctx, int op, int bits,
    int version, void *cipher, void *ex_data)
{
	switch (op) {
	case SSL_SECOP_CIPHER_SUPPORTED:
	case SSL_SECOP_CIPHER_SHARED:
	case SSL_SECOP_CIPHER_CHECK:
		return ssl_security_secop_cipher(ctx, ssl, bits, cipher);
	case SSL_SECOP_VERSION:
		return ssl_security_secop_version(ctx, ssl, version);
	case SSL_SECOP_COMPRESSION:
		return ssl_security_secop_compression(ctx, ssl);
	case SSL_SECOP_TICKET:
		return ssl_security_secop_tickets(ctx, ssl);
	case SSL_SECOP_TMP_DH:
		return ssl_security_secop_tmp_dh(ctx, ssl, bits);
	default:
		return ssl_security_secop_default(ctx, ssl, bits);
	}
}

int
ssl_security_dummy_cb(const SSL *ssl, const SSL_CTX *ctx, int op, int bits,
    int version, void *cipher, void *ex_data)
{
	return 1;
}

int
ssl_ctx_security(const SSL_CTX *ctx, int op, int bits, int nid, void *other)
{
	return ctx->internal->cert->security_cb(NULL, ctx, op, bits, nid, other,
	    ctx->internal->cert->security_ex_data);
}

int
ssl_security(const SSL *ssl, int op, int bits, int nid, void *other)
{
	return ssl->cert->security_cb(ssl, NULL, op, bits, nid, other,
	    ssl->cert->security_ex_data);
}

int
ssl_ctx_security_dh(const SSL_CTX *ctx, DH *dh)
{
#if defined(LIBRESSL_HAS_SECURITY_LEVEL)
	return ssl_ctx_security(ctx, SSL_SECOP_TMP_DH, DH_security_bits(dh), 0,
	    dh);
#else
	return 1;
#endif
}

int
ssl_security_dh(const SSL *ssl, DH *dh)
{
#if defined(LIBRESSL_HAS_SECURITY_LEVEL)
	return ssl_security(ssl, SSL_SECOP_TMP_DH, DH_security_bits(dh), 0, dh);
#else
	return 1;
#endif
}

#if defined(LIBRESSL_HAS_SECURITY_LEVEL)
static int
ssl_cert_pubkey_security_bits(const X509 *x509)
{
	EVP_PKEY *pkey;

	if ((pkey = X509_get0_pubkey(x509)) == NULL)
		return -1;

	/*
	 * XXX: DSA_security_bits() returns -1 on keys without parameters and
	 * cause the default security callback to fail.
	 */

	return EVP_PKEY_security_bits(pkey);
}

static int
ssl_security_cert_key(const SSL_CTX *ctx, const SSL *ssl, X509 *x509, int op)
{
	int security_bits;

	security_bits = ssl_cert_pubkey_security_bits(x509);

	if (ssl != NULL)
		return ssl_security(ssl, op, security_bits, 0, x509);

	return ssl_ctx_security(ctx, op, security_bits, 0, x509);
}

static int
ssl_cert_signature_md_nid(const X509 *x509)
{
	int md_nid, signature_nid;

	if ((signature_nid = X509_get_signature_nid(x509)) == NID_undef)
		return NID_undef;

	if (!OBJ_find_sigid_algs(signature_nid, &md_nid, NULL))
		return NID_undef;

	return md_nid;
}

static int
ssl_cert_md_nid_security_bits(int md_nid)
{
	const EVP_MD *md;

	if (md_nid == NID_undef)
		return -1;

	if ((md = EVP_get_digestbynid(md_nid)) == NULL)
		return -1;

	/* Assume 4 bits of collision resistance for each hash octet. */
	return EVP_MD_size(md) * 4;
}

static int
ssl_security_cert_sig(const SSL_CTX *ctx, const SSL *ssl, X509 *x509, int op)
{
	int md_nid, security_bits;

	md_nid = ssl_cert_signature_md_nid(x509);
	security_bits = ssl_cert_md_nid_security_bits(md_nid);

	if (ssl != NULL)
		return ssl_security(ssl, op, security_bits, md_nid, x509);

	return ssl_ctx_security(ctx, op, security_bits, md_nid, x509);
}
#endif

int
ssl_security_cert(const SSL_CTX *ctx, const SSL *ssl, X509 *x509,
    int is_ee, int *out_error)
{
#if defined(LIBRESSL_HAS_SECURITY_LEVEL)
	int key_error, operation;

	*out_error = 0;

	if (is_ee) {
		operation = SSL_SECOP_EE_KEY;
		key_error = SSL_R_EE_KEY_TOO_SMALL;
	} else {
		operation = SSL_SECOP_CA_KEY;
		key_error = SSL_R_CA_KEY_TOO_SMALL;
	}

	if (!ssl_security_cert_key(ctx, ssl, x509, operation)) {
		*out_error = key_error;
		return 0;
	}

	if (!ssl_security_cert_sig(ctx, ssl, x509, SSL_SECOP_CA_MD)) {
		*out_error = SSL_R_CA_MD_TOO_WEAK;
		return 0;
	}

#endif
	return 1;
}

/*
 * Check security of a chain. If |sk| includes the end entity certificate
 * then |x509| must be NULL.
 */
int
ssl_security_cert_chain(const SSL *ssl, STACK_OF(X509) *sk, X509 *x509,
    int *out_error)
{
	int start_idx = 0;
	int is_ee;
	int i;

	if (x509 == NULL) {
		x509 = sk_X509_value(sk, 0);
		start_idx = 1;
	}

	if (!ssl_security_cert(NULL, ssl, x509, is_ee = 1, out_error))
		return 0;

	for (i = start_idx; i < sk_X509_num(sk); i++) {
		x509 = sk_X509_value(sk, i);

		if (!ssl_security_cert(NULL, ssl, x509, is_ee = 0,
		    out_error))
			return 0;
	}

	return 1;
}