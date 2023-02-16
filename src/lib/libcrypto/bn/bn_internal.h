/*	$OpenBSD: bn_internal.h,v 1.6 2023/02/16 10:02:02 jsing Exp $ */
/*
 * Copyright (c) 2023 Joel Sing <jsing@openbsd.org>
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

#include <openssl/bn.h>

#include "bn_arch.h"

#ifndef HEADER_BN_INTERNAL_H
#define HEADER_BN_INTERNAL_H

#ifndef HAVE_BN_CT_NE_ZERO
static inline int
bn_ct_ne_zero(BN_ULONG w)
{
	return (w | ~(w - 1)) >> (BN_BITS2 - 1);
}
#endif

#ifndef HAVE_BN_CT_NE_ZERO_MASK
static inline BN_ULONG
bn_ct_ne_zero_mask(BN_ULONG w)
{
	return 0 - bn_ct_ne_zero(w);
}
#endif

#ifndef HAVE_BN_CT_EQ_ZERO
static inline int
bn_ct_eq_zero(BN_ULONG w)
{
	return 1 - bn_ct_ne_zero(w);
}
#endif

#ifndef HAVE_BN_CT_EQ_ZERO_MASK
static inline BN_ULONG
bn_ct_eq_zero_mask(BN_ULONG w)
{
	return 0 - bn_ct_eq_zero(w);
}
#endif

/*
 * Big number primitives are named as the operation followed by a suffix
 * that indicates the number of words that it operates on, where 'w' means
 * single word, 'dw' means double word, 'tw' means triple word and 'qw' means
 * quadruple word. Unless otherwise noted, the size of the output is implied
 * based on its inputs, for example bn_mulw() takes two single word inputs
 * and is going to produce a double word result.
 *
 * Where a function implements multiple operations, these are listed in order.
 * For example, a function that computes (r1:r0) = a * b + c is named
 * bn_mulw_addw(), producing a double word result.
 */

/*
 * bn_addw() computes (r1:r0) = a + b, where both inputs are single words,
 * producing a double word result. The value of r1 is the carry from the
 * addition.
 */
#ifndef HAVE_BN_ADDW
#ifdef BN_LLONG
static inline void
bn_addw(BN_ULONG a, BN_ULONG b, BN_ULONG *out_r1, BN_ULONG *out_r0)
{
	BN_ULLONG r;

	r = (BN_ULLONG)a + (BN_ULLONG)b;

	*out_r1 = r >> BN_BITS2;
	*out_r0 = r & BN_MASK2;
}
#else

static inline void
bn_addw(BN_ULONG a, BN_ULONG b, BN_ULONG *out_r1, BN_ULONG *out_r0)
{
	BN_ULONG r1, r0, c1, c2;

	c1 = a | b;
	c2 = a & b;
	r0 = a + b;
	r1 = ((c1 & ~r0) | c2) >> (BN_BITS2 - 1); /* carry */

	*out_r1 = r1;
	*out_r0 = r0;
}
#endif
#endif

/*
 * bn_addw_addw() computes (r1:r0) = a + b + c, where all inputs are single
 * words, producing a double word result.
 */
#ifndef HAVE_BN_ADDW_ADDW
static inline void
bn_addw_addw(BN_ULONG a, BN_ULONG b, BN_ULONG c, BN_ULONG *out_r1,
    BN_ULONG *out_r0)
{
	BN_ULONG carry, r1, r0;

	bn_addw(a, b, &r1, &r0);
	bn_addw(r0, c, &carry, &r0);
	r1 += carry;

	*out_r1 = r1;
	*out_r0 = r0;
}
#endif

/*
 * bn_subw() computes r0 = a - b, where both inputs are single words,
 * producing a single word result and borrow.
 */
#ifndef HAVE_BN_SUBW
static inline void
bn_subw(BN_ULONG a, BN_ULONG b, BN_ULONG *out_borrow, BN_ULONG *out_r0)
{
	BN_ULONG borrow, r0;

	r0 = a - b;
	borrow = ((r0 | (b & ~a)) & (b | ~a)) >> (BN_BITS2 - 1);

	*out_borrow = borrow;
	*out_r0 = r0;
}
#endif

/*
 * bn_subw_subw() computes r0 = a - b - c, where all inputs are single words,
 * producing a single word result and borrow.
 */
#ifndef HAVE_BN_SUBW_SUBW
static inline void
bn_subw_subw(BN_ULONG a, BN_ULONG b, BN_ULONG c, BN_ULONG *out_borrow,
    BN_ULONG *out_r0)
{
	BN_ULONG b1, b2, r0;

	bn_subw(a, b, &b1, &r0);
	bn_subw(r0, c, &b2, &r0);

	*out_borrow = b1 + b2;
	*out_r0 = r0;
}
#endif

#ifndef HAVE_BN_UMUL_HILO
#ifdef BN_LLONG
static inline void
bn_umul_hilo(BN_ULONG a, BN_ULONG b, BN_ULONG *out_h, BN_ULONG *out_l)
{
	BN_ULLONG r;

	r = (BN_ULLONG)a * (BN_ULLONG)b;

	*out_h = r >> BN_BITS2;
	*out_l = r & BN_MASK2;
}

#else /* !BN_LLONG */
/*
 * Multiply two words (a * b) producing a double word result (h:l).
 *
 * This can be rewritten as:
 *
 *  a * b = (hi32(a) * 2^32 + lo32(a)) * (hi32(b) * 2^32 + lo32(b))
 *        = hi32(a) * hi32(b) * 2^64 +
 *          hi32(a) * lo32(b) * 2^32 +
 *          hi32(b) * lo32(a) * 2^32 +
 *          lo32(a) * lo32(b)
 *
 * The multiplication for each part of a and b can be calculated for each of
 * these four terms without overflowing a BN_ULONG, as the maximum value of a
 * 32 bit x 32 bit multiplication is 32 + 32 = 64 bits. Once these
 * multiplications have been performed the result can be partitioned and summed
 * into a double word (h:l). The same applies on a 32 bit system, substituting
 * 16 for 32 and 32 for 64.
 */
#if 1
static inline void
bn_umul_hilo(BN_ULONG a, BN_ULONG b, BN_ULONG *out_h, BN_ULONG *out_l)
{
	BN_ULONG ah, al, bh, bl, h, l, x, c1, c2;

	ah = a >> BN_BITS4;
	al = a & BN_MASK2l;
	bh = b >> BN_BITS4;
	bl = b & BN_MASK2l;

	h = ah * bh;
	l = al * bl;

	/* (ah * bl) << BN_BITS4, partition the result across h:l with carry. */
	x = ah * bl;
	h += x >> BN_BITS4;
	x <<= BN_BITS4;
	c1 = l | x;
	c2 = l & x;
	l += x;
	h += ((c1 & ~l) | c2) >> (BN_BITS2 - 1); /* carry */

	/* (bh * al) << BN_BITS4, partition the result across h:l with carry. */
	x = bh * al;
	h += x >> BN_BITS4;
	x <<= BN_BITS4;
	c1 = l | x;
	c2 = l & x;
	l += x;
	h += ((c1 & ~l) | c2) >> (BN_BITS2 - 1); /* carry */

	*out_h = h;
	*out_l = l;
}
#else

/*
 * XXX - this accumulator based version uses fewer instructions, however
 * requires more variables/registers. It seems to be slower on at least amd64
 * and i386, however may be faster on other architectures that have more
 * registers available. Further testing is required and one of the two
 * implementations should eventually be removed.
 */
static inline void
bn_umul_hilo(BN_ULONG a, BN_ULONG b, BN_ULONG *out_h, BN_ULONG *out_l)
{
	BN_ULONG ah, bh, al, bl, x, h, l;
	BN_ULONG acc0, acc1, acc2, acc3;

	ah = a >> BN_BITS4;
	bh = b >> BN_BITS4;
	al = a & BN_MASK2l;
	bl = b & BN_MASK2l;

	h = ah * bh;
	l = al * bl;

	acc0 = l & BN_MASK2l;
	acc1 = l >> BN_BITS4;
	acc2 = h & BN_MASK2l;
	acc3 = h >> BN_BITS4;

	/* (ah * bl) << BN_BITS4, partition the result across h:l. */
	x = ah * bl;
	acc1 += x & BN_MASK2l;
	acc2 += (acc1 >> BN_BITS4) + (x >> BN_BITS4);
	acc1 &= BN_MASK2l;
	acc3 += acc2 >> BN_BITS4;
	acc2 &= BN_MASK2l;

	/* (bh * al) << BN_BITS4, partition the result across h:l. */
	x = bh * al;
	acc1 += x & BN_MASK2l;
	acc2 += (acc1 >> BN_BITS4) + (x >> BN_BITS4);
	acc1 &= BN_MASK2l;
	acc3 += acc2 >> BN_BITS4;
	acc2 &= BN_MASK2l;

	*out_h = (acc3 << BN_BITS4) | acc2;
	*out_l = (acc1 << BN_BITS4) | acc0;
}
#endif
#endif /* !BN_LLONG */
#endif

#ifndef HAVE_BN_UMUL_LO
static inline BN_ULONG
bn_umul_lo(BN_ULONG a, BN_ULONG b)
{
	return a * b;
}
#endif

#ifndef HAVE_BN_UMUL_HI
static inline BN_ULONG
bn_umul_hi(BN_ULONG a, BN_ULONG b)
{
	BN_ULONG h, l;

	bn_umul_hilo(a, b, &h, &l);

	return h;
}
#endif

/*
 * bn_mulw_addw() computes (r1:r0) = a * b + c with all inputs being single
 * words, producing a double word result.
 */
#ifndef HAVE_BN_MULW_ADDW
static inline void
bn_mulw_addw(BN_ULONG a, BN_ULONG b, BN_ULONG c, BN_ULONG *out_r1,
    BN_ULONG *out_r0)
{
	BN_ULONG carry, r1, r0;

	bn_umul_hilo(a, b, &r1, &r0);
	bn_addw(r0, c, &carry, &r0);
	r1 += carry;

	*out_r1 = r1;
	*out_r0 = r0;
}
#endif

/*
 * bn_mulw_addw_addw() computes (r1:r0) = a * b + c + d with all inputs being
 * single words, producing a double word result.
 */
#ifndef HAVE_BN_MULW_ADDW_ADDW
static inline void
bn_mulw_addw_addw(BN_ULONG a, BN_ULONG b, BN_ULONG c, BN_ULONG d,
    BN_ULONG *out_r1, BN_ULONG *out_r0)
{
	BN_ULONG carry, r1, r0;

	bn_mulw_addw(a, b, c, &r1, &r0);
	bn_addw(r0, d, &carry, &r0);
	r1 += carry;

	*out_r1 = r1;
	*out_r0 = r0;
}
#endif

/*
 * bn_mulw_addtw() computes (r2:r1:r0) = a * b + (c2:c1:c0), where a and b are
 * single words and (c2:c1:c0) is a triple word, producing a triple word result.
 * The caller must ensure that the inputs provided do not result in c2
 * overflowing.
 */
#ifndef HAVE_BN_MULW_ADDTW
static inline void
bn_mulw_addtw(BN_ULONG a, BN_ULONG b, BN_ULONG c2, BN_ULONG c1, BN_ULONG c0,
    BN_ULONG *out_r2, BN_ULONG *out_r1, BN_ULONG *out_r0)
{
	BN_ULONG carry, r2, r1, r0, x1, x0;

	bn_umul_hilo(a, b, &x1, &x0);
	bn_addw(c0, x0, &carry, &r0);
	x1 += carry;
	bn_addw(c1, x1, &carry, &r1);
	r2 = c2 + carry;

	*out_r2 = r2;
	*out_r1 = r1;
	*out_r0 = r0;
}
#endif

#endif
