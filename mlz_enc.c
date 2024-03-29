/*
   mini-LZ library (mlz)
   (c) Martin Sedlak 2016-2018

   Boost Software License - Version 1.0 - August 17th, 2003

   Permission is hereby granted, free of charge, to any person or organization
   obtaining a copy of the software and accompanying documentation covered by
   this license (the "Software") to use, reproduce, display, distribute,
   execute, and transmit the Software, and to prepare derivative works of the
   Software, and to permit third-parties to whom the Software is furnished to
   do so, all subject to the following:

   The copyright notices in the Software and this entire statement, including
   the above license grant, this restriction and the following disclaimer,
   must be included in all copies of the Software, in whole or in part, and
   all derivative works of the Software, unless such copies or derivative
   works are solely in the form of machine-executable object code generated by
   a source language processor.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
   SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
   FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

#include "mlz_enc.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

enum mlz_match_constants
{
	/* because we don't clear hash list, setting this too high will actually slow things down */
	/* 4096 seems like a best tradeoff between compression speed and quality for "fast" modes */
	MLZ_HASH_SIZE      = 4096,
	MLZ_HASH_LIST_SIZE = 65536,
	MLZ_DICT_MASK      = MLZ_HASH_LIST_SIZE-1,
	MLZ_HASH_MASK      = MLZ_HASH_SIZE-1,

	/* 3 tested best */
	MLZ_SHORT_LEN_BITS = 3
};

MLZ_INLINE void *mlz_malloc_wrapper(size_t size)
{
	return malloc(size);
}

MLZ_INLINE void mlz_free_wrapper(void *ptr)
{
	free(ptr);
}

void *(*mlz_malloc)(size_t) = mlz_malloc_wrapper;
void (*mlz_free)(void *)    = mlz_free_wrapper;

/* experimental naive, SLOW optimal parsing */
typedef struct
{
	mlz_int cost;
	mlz_int dist;
	mlz_int len;
	mlz_int lcost;
} mlz_optimal;

/* simple hash-list (or hash-chain)                  */
/* note that this helper structure uses 137kB of RAM */
struct mlz_matcher
{
	mlz_ushort hash[MLZ_HASH_SIZE];
	mlz_ushort list[MLZ_HASH_LIST_SIZE];
	mlz_optimal *optimal;
	size_t     optimal_size;
	mlz_byte   pad [MLZ_CACHELINE_ALIGN];
};

mlz_bool mlz_matcher_alloc_opt(struct mlz_matcher *matcher, size_t size)
{
	MLZ_ASSERT(matcher);

	if (matcher->optimal_size >= size)
		return MLZ_TRUE;

	if (matcher->optimal)
		mlz_free(matcher->optimal);

	matcher->optimal = (mlz_optimal *)mlz_malloc(size*sizeof(mlz_optimal));
	matcher->optimal_size = matcher->optimal ? size : 0;

	return matcher->optimal != MLZ_NULL;
}

mlz_bool mlz_matcher_init(struct mlz_matcher **matcher)
{
	if (!matcher)
		return MLZ_FALSE;

	*matcher = (struct mlz_matcher *)mlz_malloc(sizeof(struct mlz_matcher));

	if (*matcher) {
		(*matcher)->optimal = MLZ_NULL;
		(*matcher)->optimal_size = 0;
	}

	return *matcher != MLZ_NULL;
}

static mlz_bool mlz_matcher_clear(struct mlz_matcher *matcher)
{
	MLZ_RET_FALSE(matcher != MLZ_NULL);
	memset(matcher->hash, 0, sizeof(matcher->hash));
	matcher->list[0] = 0;
	return MLZ_TRUE;
}

mlz_bool mlz_matcher_free(struct mlz_matcher *matcher)
{
	if (matcher) {
		if (matcher->optimal)
			mlz_free(matcher->optimal);

		mlz_free(matcher);
	}

	return matcher ? MLZ_TRUE : MLZ_FALSE;
}

/* bit accumulator */
typedef struct {
	mlz_uint  bits;
	mlz_int   count;
	mlz_byte *ptr;
} mlz_accumulator;

static mlz_bool mlz_flush_accum(mlz_accumulator *accum, mlz_byte **db, MLZ_CONST mlz_byte *de)
{
	int i;
	MLZ_RET_FALSE(*db + MLZ_ACCUM_BYTES <= de);

	if (!accum->count)
		return MLZ_TRUE;

	for (i=0; i<MLZ_ACCUM_BYTES; i++)
		accum->ptr[i] = (mlz_byte)((accum->bits >> 8*i) & 255);

	accum->ptr = *db;
	(*db) += MLZ_ACCUM_BYTES;
	accum->bits  = 0;
	accum->count = 0;

	for (i=0; i<MLZ_ACCUM_BYTES; i++)
		accum->ptr[i] = (mlz_byte)0;

	return MLZ_TRUE;
}

MLZ_INLINE mlz_bool mlz_add_bit(mlz_accumulator *accum, mlz_byte **db, MLZ_CONST mlz_byte *de, int bit)
{
	accum->bits |= (mlz_uint)bit << accum->count;
	if (++accum->count < MLZ_ACCUM_BITS)
		return MLZ_TRUE;

	return mlz_flush_accum(accum, db, de);
}

/* for optimal parsing */
MLZ_INLINE mlz_int mlz_compute_cost(mlz_int dist, mlz_int len)
{
	mlz_bool tiny_len;
	if (!dist)
		return 9;
	if (len < MLZ_MIN_MATCH)
		return 9*len;
	/* compute cost now */
	tiny_len = len >= MLZ_MIN_MATCH && len < MLZ_MIN_MATCH + (1 << MLZ_SHORT_LEN_BITS);
	if (tiny_len && dist < 256)
		return 3 + MLZ_SHORT_LEN_BITS + 8;
	if (tiny_len && (dist < (1 << 13)))
		return 3 + 16;
	if (tiny_len)
		return 3 + MLZ_SHORT_LEN_BITS + 16;
	if (len <= MLZ_MIN_MATCH)
		return 9*len;
	return 3 + 8 + 16*((len - MLZ_MIN_MATCH) >= 255) + 16;
}

/* for max compression mode */
MLZ_INLINE mlz_int mlz_compute_savings(mlz_int dist, mlz_int len)
{
	/* compute cost now */
	mlz_bool tiny_len = len >= MLZ_MIN_MATCH && len < MLZ_MIN_MATCH + (1 << MLZ_SHORT_LEN_BITS);
	/* note: we don't check for tiny_len && dist < (1 << 13)          */
	/* here because it has no impact due to the way the matcher works */
	mlz_int  bit_cost = tiny_len ? 3 + MLZ_SHORT_LEN_BITS + 8 + 8*(dist > 255) : 3 + 8 + 16 + 16*((len - MLZ_MIN_MATCH) >= 255);

	return 9*len - bit_cost;
}

MLZ_INLINE mlz_int mlz_min(mlz_int x, mlz_int y)
{
	return x < y ? x : y;
}

MLZ_INLINE mlz_int mlz_clamp(mlz_int x, mlz_int y, mlz_int z)
{
	return x < y ? x : (x > z ? z : x);
}

/* matcher */

#define MLZ_MATCH_BEST_COMMON \
	*best_len = i; \
	best_dist = cyc_dist; \
	if (i >= max_len) { \
		*best_len = max_len; \
		return best_dist; \
	}

#define MLZ_MATCH_BEST_SAVINGS \
	mlz_int save = mlz_compute_savings(cyc_dist, i); \
	if (i > *best_len && save > *best_save) { \
		*best_save = save; \
		MLZ_MATCH_BEST_COMMON \
	}

#define MLZ_MATCH_BEST \
	if (i > *best_len) { \
		MLZ_MATCH_BEST_COMMON \
	}

#define MLZ_MATCH(precond, best) \
	mlz_int cyc_dist; \
	mlz_int best_dist       = 0; \
	mlz_int mbest_len       = *best_len; \
	MLZ_CONST mlz_byte *src = buf + pos; \
	size_t opos             = pos; \
 \
	MLZ_RET_FALSE(best_len && max_len > 0 && *best_len < max_len); \
 \
	pos = m->hash[hash]; \
	cyc_dist = (mlz_int)(mlz_ushort)opos - (mlz_ushort)pos; \
	if (cyc_dist < 0) \
		cyc_dist += MLZ_DICT_MASK+1; \
 \
	while (precond && (mlz_uint)cyc_dist <= (mlz_uint)max_dist) { \
		mlz_int tmp; \
		mlz_ushort npos; \
 \
		/* micro-optimization: match at bestlen first */ \
		if (src[mbest_len] == src[mbest_len - cyc_dist]) { \
			mlz_int i; \
 \
			for (i=0; i<max_len; i++) \
				if (src[i] != src[i - cyc_dist]) \
					break; \
 \
			if (i > mbest_len) { \
				mbest_len = i; \
 \
				{ best } \
			} \
		} \
		npos = m->list[pos & MLZ_DICT_MASK]; \
 \
		/* recompute cyclic distance... */ \
		tmp = (mlz_int)pos - npos; \
		if (tmp <= 0) \
			tmp += MLZ_DICT_MASK+1; \
 \
		cyc_dist += tmp; \
		pos = npos; \
	} \
	return best_dist;

static mlz_int
mlz_match_loops_save(
	struct mlz_matcher *m,
	size_t              pos,
	mlz_uint            hash,
	MLZ_CONST mlz_byte *buf,
	mlz_int             max_dist,
	mlz_int             max_len,
	mlz_int            *best_len,
	mlz_int            *best_save,
	mlz_int             loops
)
{
	MLZ_MATCH(loops-- > 0, MLZ_MATCH_BEST_SAVINGS)
}

#if 0
/* keeping this in case it's useful to someone playing around */
static mlz_int
mlz_match_loops_nosave(
	struct mlz_matcher *m,
	size_t              pos,
	mlz_uint            hash,
	MLZ_CONST mlz_byte *buf,
	mlz_int             max_dist,
	mlz_int             max_len,
	mlz_int *           best_len,
	mlz_int             loops
)
{
	MLZ_MATCH(loops-- > 0, MLZ_MATCH_BEST)
}
#endif

static mlz_int
mlz_match(
	struct mlz_matcher *m,
	size_t              pos,
	mlz_uint            hash,
	MLZ_CONST mlz_byte *buf,
	mlz_int             max_dist,
	mlz_int             max_len,
	mlz_int *           best_len,
	mlz_int *           best_save,
	mlz_int             loops
)
{
	mlz_int dummy = -1;

	if (!best_save)
		best_save = &dummy;

	return mlz_match_loops_save(m, pos, hash, buf, max_dist, max_len, best_len, best_save, loops);
}

MLZ_INLINE void mlz_match_hash_next_byte(struct mlz_matcher *m, mlz_uint hash, size_t pos)
{
	mlz_ushort *idx;

	MLZ_ASSERT(m && hash <= MLZ_HASH_MASK);

	idx = m->hash + hash;
	m->list[pos & MLZ_DICT_MASK] = *idx;
	*idx = (mlz_ushort)(pos & MLZ_DICT_MASK);
}

static mlz_bool mlz_output_match(
	mlz_accumulator    *accum,
	MLZ_CONST mlz_byte *lb,
	MLZ_CONST mlz_byte *le,
	mlz_byte          **db,
	MLZ_CONST mlz_byte *de,
	mlz_int             dist,
	mlz_int             len
)
{
	mlz_int i, j, nlit, dlen;
	mlz_bool tiny_len;

	MLZ_RET_FALSE(*db < de);

	nlit = (mlz_int)(le - lb);

	/* literal run cost:                                                                         */
	/* 3 bits + 3 bits + byte + byte => 22 bits static cost + 8*lit => minimum literal run is 22 */
	/* normally single literal costs 9 bits                                                      */
	/* so we only do this for 23+ literals (because 23 gave better results than 22)              */
	while (nlit >= MLZ_MIN_LIT_RUN) {
		mlz_int  enc_run;
		mlz_int  run = mlz_min(65535 + MLZ_MIN_LIT_RUN, nlit);
		mlz_bool long_run = run > 255 + MLZ_MIN_LIT_RUN;

		MLZ_RET_FALSE(mlz_output_match(accum, MLZ_NULL, MLZ_NULL, db, de, 0, MLZ_MIN_MATCH + long_run));

		enc_run = run - MLZ_MIN_LIT_RUN;

		MLZ_RET_FALSE(*db + 1 < de);
		*(*db)++ = (mlz_byte)(enc_run & 255);
		if (long_run)
			*(*db)++ = (mlz_byte)(enc_run >> 8);

		MLZ_RET_FALSE(*db + run <= de);
		for (i=0; i<run; i++)
			*(*db)++ = lb[i];

		nlit -= run;
		lb += run;
	}

	/* encode literals */
	while (lb < le) {
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 0));
		MLZ_RET_FALSE(*db < de);
		*(*db)++ = *lb++;
	}
	if (len < MLZ_MIN_MATCH) {
		MLZ_ASSERT(!len);
		return MLZ_TRUE;
	}

	/* encode match */

	MLZ_ASSERT(len <= MLZ_MAX_MATCH);

	/*
	bit 0: byte literal
	match:
	100: tiny match + 3 bits len-min_match + byte dist
	101: short match + word dist (3 msbits encoded as short length)
	110: short match + 3 bits len-min match + word dist
	111: full match + byte len (255 => word len follows) + word dist
	dist = 0 => literal run (then word follows if len > MIN_MATCH, byte otherwise): number of literals
	            len above MIN_MATCH + 1 is illegal)
	*/

	#define MLZ_ADD_SHORT_LEN() \
		len -= MLZ_MIN_MATCH; \
		for (j=0; j<MLZ_SHORT_LEN_BITS; j++) \
			MLZ_RET_FALSE(mlz_add_bit(accum, db, de, (len >> j) & 1));

	tiny_len = len >= MLZ_MIN_MATCH && len < MLZ_MIN_MATCH + (1<<MLZ_SHORT_LEN_BITS);

	if (dist < 256 && tiny_len) {
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 0));
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 0));
		MLZ_ADD_SHORT_LEN()

		MLZ_RET_FALSE(*db < de);
		*(*db)++ = (mlz_byte)(dist & 255);
		return MLZ_TRUE;
	}

	if (tiny_len && (dist < (1 << 13))) {
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 0));
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));

		dist |= (len - MLZ_MIN_MATCH) << 13;

		MLZ_RET_FALSE(*db + 1 < de);
		*(*db)++ = (mlz_byte)(dist & 255);
		*(*db)++ = (mlz_byte)(dist >> 8);
		return MLZ_TRUE;
	}

	if (tiny_len) {
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 0));

		MLZ_ADD_SHORT_LEN()

		MLZ_RET_FALSE(*db + 1 < de);
		*(*db)++ = (mlz_byte)(dist & 255);
		*(*db)++ = (mlz_byte)(dist >> 8);
		return MLZ_TRUE;
	}

	if (len <= 3) {
		/* cost optimization: encode match as literals */
		for (i=0; i<len; i++) {
			MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 0));
			MLZ_RET_FALSE(*db < de);
			*(*db)++ = le[i];
		}
		return MLZ_TRUE;
	}

	MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));
	MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));
	MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));

	MLZ_RET_FALSE(*db+2 < de);
	dlen = len - MLZ_MIN_MATCH;
	*(*db)++ = (mlz_byte)mlz_min(dlen, 255);
	if (dlen >= 255) {
		MLZ_RET_FALSE(*db+3 < de);
		*(*db)++ = (mlz_byte)(dlen & 255);
		*(*db)++ = (mlz_byte)((dlen >> 8));
	}
	*(*db)++ = (mlz_byte)(dist & 255);
	*(*db)++ = (mlz_byte)((dist >> 8));

	return MLZ_TRUE;
}

MLZ_INLINE mlz_uint mlz_compute_hash(mlz_uint hash_data)
{
	hash_data ^= hash_data >> 11;
	hash_data ^= hash_data << 7;
	return hash_data & MLZ_HASH_MASK;
}

size_t
mlz_compress_optimal(
	struct mlz_matcher *matcher,
	void               *dst,
	size_t              dst_size,
	MLZ_CONST void     *src,
	size_t              src_size,
	size_t              bytes_before_src,
	int                 level
);

size_t
mlz_compress(
	struct mlz_matcher *matcher,
	void               *dst,
	size_t              dst_size,
	MLZ_CONST void     *src,
	size_t              src_size,
	size_t              bytes_before_src,
	int                 level
)
{
	mlz_accumulator accum;
	mlz_uint hdata, hash;
	mlz_int  loops;

	MLZ_CONST mlz_byte *sb = (MLZ_CONST mlz_byte *)src;
	MLZ_CONST mlz_byte *osb = sb - bytes_before_src;
	MLZ_CONST mlz_byte *se = sb + src_size;
	/* we need a reserve for faster decompression so that we can round match length up to 8-bytes */
	MLZ_CONST mlz_byte *se_match = se - MLZ_LAST_LITERALS;
	MLZ_CONST mlz_byte *match_start_max = se - MLZ_MIN_MATCH;
	mlz_byte *db = (mlz_byte *)dst;
	MLZ_CONST mlz_byte *odb = db;
	MLZ_CONST mlz_byte *de = db + dst_size;
	MLZ_CONST mlz_byte *lit_start = sb;
	MLZ_CONST mlz_byte *tmp = osb;

	if (level >= MLZ_LEVEL_OPTIMAL)
		return mlz_compress_optimal(matcher, dst, dst_size, src, src_size, bytes_before_src, level);

	MLZ_RET_FALSE(mlz_matcher_clear(matcher) && dst && src);

	/* cannot handle blocks larger than 2G - 64k - 1 */
	MLZ_RET_FALSE(se - osb < INT_MAX);

	level = mlz_clamp(level, 1, 10);
	loops = 1 << level;

	accum.bits  = 0;
	accum.count = 0;

	MLZ_RET_FALSE(db + MLZ_ACCUM_BYTES <= de);
	accum.ptr = db;
	db       += MLZ_ACCUM_BYTES;

	memset(accum.ptr, 0, MLZ_ACCUM_BYTES);

#define MLZ_HASHBYTE(sb)	\
	hdata = sb[0] + (sb+1<se ? (sb[1] << 8) : 0) + (sb+2<se ? sb[2] << 16 : 0); \
	hash = mlz_compute_hash(hdata);

	while (tmp < sb) {
		MLZ_HASHBYTE(tmp);
		mlz_match_hash_next_byte(matcher, hash, (size_t)(tmp - osb));
		tmp++;
	}

	while (sb < se) {
		mlz_int i, best_dist, firstlen, firstdist;
		mlz_int lazy_ofs, lazy_count;
		MLZ_CONST mlz_byte *firstsb;
		mlz_int best_savings = -1;
		mlz_int best_len = 0;
		mlz_int max_dist = mlz_min(MLZ_MAX_DIST,  (mlz_int)(sb - osb));
		mlz_int max_len  = mlz_min(MLZ_MAX_MATCH, (mlz_int)(se_match - sb));

		/* compute hash at sb */
		MLZ_HASHBYTE(sb);

		if (!max_dist || max_len < MLZ_MIN_MATCH || sb >= se_match) {
			mlz_match_hash_next_byte(matcher, hash, (size_t)(sb - osb));
			sb++;
			continue;
		}

		/* try to find a match now */
		best_dist = sb > match_start_max ? 0 :
			mlz_match(matcher, (mlz_int)(sb - osb), hash, osb, max_dist, max_len, &best_len,
				&best_savings, loops);

		if (!best_dist || best_len < MLZ_MIN_MATCH) {
			mlz_match_hash_next_byte(matcher, hash, (size_t)(sb - osb));
			sb++;
			continue;
		}

		firstsb = sb;
		firstlen = best_len;
		firstdist = best_dist;

		/* try lazy matching now */
		lazy_ofs = 1;
		lazy_count = level > 5 ? 30 : 0;
		while (best_len < max_len && sb+lazy_ofs < se && lazy_count-- > 0) {
			mlz_int lmax_dist, lmax_len, lbestLen, ldist;
			mlz_int best_len2, best_dist2;
			mlz_uint ohash = hash;
			MLZ_CONST mlz_byte *sb2 = sb+lazy_ofs;
			mlz_int max_dist2 = mlz_min(MLZ_MAX_DIST, (mlz_int)(sb2 - osb));
			mlz_int max_len2  = mlz_min(MLZ_MAX_MATCH, (mlz_int)(se_match - sb2));

			/* trying to speed things up using Yann Collet's advanced parsing strategies:                                        */
			/* just try to look for MINMATCH at P+ML+2-MINMATCH first (helps, in some cases ~15% but in others even 50% speedup) */
			MLZ_CONST mlz_byte *lazysb = sb + best_len + 2 - MLZ_MIN_MATCH;
			if (lazysb + MLZ_MIN_MATCH > se || sb2 >= se_match)
				break;

			lmax_dist = mlz_min(MLZ_MAX_DIST, (mlz_int)(lazysb - osb));
			lmax_len  = mlz_min(MLZ_MAX_MATCH, (mlz_int)(se - lazysb));
			MLZ_HASHBYTE(lazysb);
			lbestLen = 0;
			lmax_len = mlz_min(lmax_len, MLZ_MIN_MATCH);
			ldist = mlz_match(matcher, (mlz_int)(lazysb - osb), hash, osb, lmax_dist, lmax_len,
				&lbestLen, MLZ_NULL, loops);
			if (!ldist || lbestLen < MLZ_MIN_MATCH)
				break;

			MLZ_HASHBYTE(sb2);

			/* FIXME: for some reason, initializing best_len2 with 0 performs significantly faster (cache or bug?) */
			best_len2 = 0; /*best_len*/;
			best_dist2 = sb2 > match_start_max ? 0 :
				mlz_match(matcher, (mlz_int)(sb2 - osb), hash, osb, max_dist2, max_len2, &best_len2,
					&best_savings, loops);
			if (!best_dist2 || best_len2 <= best_len)
				break;

			MLZ_ASSERT(lazy_ofs == 1);
			mlz_match_hash_next_byte(matcher, ohash, (size_t)(sb - osb));
			sb += lazy_ofs;

			best_dist = best_dist2;
			best_len = best_len2;
			max_len  = max_len2;
		}

		MLZ_ASSERT(sb - best_dist >= osb);

		if (sb >= firstsb + MLZ_MIN_MATCH) {
			/* a pathetic attempt to save some bits... */
			firstlen = mlz_min(firstlen, (mlz_int)(sb - firstsb));
			MLZ_RET_FALSE(mlz_output_match(&accum, lit_start, firstsb, &db, de, firstdist, firstlen));
			lit_start = firstsb + firstlen;
		}

		MLZ_RET_FALSE(mlz_output_match(&accum, lit_start, sb, &db, de, best_dist, best_len));
		for (i=0; i<best_len; i++) {
			MLZ_HASHBYTE(sb);
			mlz_match_hash_next_byte(matcher, hash, (size_t)(sb - osb));
			sb++;
		}
		lit_start = sb;
	}

	/* flush last lit chunk */
	if (lit_start < sb && !mlz_output_match(&accum, lit_start, sb, &db, de, 0, 0))
		return 0;

	MLZ_RET_FALSE(mlz_flush_accum(&accum, &db, de));

	if (accum.ptr == db-MLZ_ACCUM_BYTES && accum.count == 0)
		/* don't waste extra space */
		db -= MLZ_ACCUM_BYTES;

	return (size_t)(db - odb);
}

size_t
mlz_compress_simple(
	void               *dst,
	size_t              dst_size,
	MLZ_CONST void     *src,
	size_t              src_size,
	int                 level
)
{
	size_t res;
	struct mlz_matcher *m;

	MLZ_RET_FALSE(mlz_matcher_init(&m));

	res = mlz_compress(m, dst, dst_size, src, src_size, 0, level);

	(void)mlz_matcher_free(m);
	return res;
}

/* very slow naive optimal parsing; uses extra 16*src_size bytes */
/* up to 3 orders of magnitude slower!! */
size_t
mlz_compress_optimal(
	struct mlz_matcher *matcher,
	void               *dst,
	size_t              dst_size,
	MLZ_CONST void     *src,
	size_t              src_size,
	size_t              bytes_before_src,
	int                 level
)
{
	mlz_accumulator accum;
	mlz_uint hdata, hash;
	mlz_int  loops;

	mlz_optimal *opt, *optimal;

	MLZ_CONST mlz_byte *sb = (MLZ_CONST mlz_byte *)src;
	MLZ_CONST mlz_byte *osb = sb - bytes_before_src;
	MLZ_CONST mlz_byte *se = sb + src_size;
	/* we need a reserve for faster decompression so that we can round match length up to 8-bytes */
	MLZ_CONST mlz_byte *se_match = se - MLZ_LAST_LITERALS;
	MLZ_CONST mlz_byte *match_start_max = se - MLZ_MIN_MATCH;
	mlz_byte *db = (mlz_byte *)dst;
	MLZ_CONST mlz_byte *odb = db;
	MLZ_CONST mlz_byte *de = db + dst_size;
	MLZ_CONST mlz_byte *lit_start = sb;
	MLZ_CONST mlz_byte *tmp = osb;

	/* out of memory for optimal parse temp buffer? */
	MLZ_RET_FALSE(mlz_matcher_alloc_opt(matcher, src_size));

	opt = optimal = matcher->optimal;

	MLZ_RET_FALSE(mlz_matcher_clear(matcher) && dst && src);

	/* cannot handle blocks larger than 2G - 64k - 1 */
	MLZ_RET_FALSE(se - osb < INT_MAX);

	level = mlz_clamp(level, 1, 10);
	loops = 1 << level;

	accum.bits  = 0;
	accum.count = 0;

	MLZ_RET_FALSE(db + MLZ_ACCUM_BYTES <= de);
	accum.ptr = db;
	db       += MLZ_ACCUM_BYTES;

	memset(accum.ptr, 0, MLZ_ACCUM_BYTES);

	while (tmp < sb) {
		MLZ_HASHBYTE(tmp);
		mlz_match_hash_next_byte(matcher, hash, (size_t)(tmp - osb));
		tmp++;
	}

	while (sb < se) {
		mlz_int best_dist;
		mlz_int best_savings = -1;
		mlz_int best_len = 0;
		mlz_int max_dist = mlz_min(MLZ_MAX_DIST,  (mlz_int)(sb - osb));
		mlz_int max_len  = mlz_min(MLZ_MAX_MATCH, (mlz_int)(se_match - sb));

		/* compute hash at sb */
		MLZ_HASHBYTE(sb);

		if (!max_dist || max_len < MLZ_MIN_MATCH || sb >= se_match) {
			opt->len  = 1;
			opt->cost = 0;
			opt->lcost = mlz_compute_cost(0, 1);
			opt->dist = 0;
			opt++;
			mlz_match_hash_next_byte(matcher, hash, (size_t)(sb - osb));
			sb++;
			continue;
		}

		/* try to find a match now */
		best_dist = sb > match_start_max ? 0 :
			mlz_match(matcher, (mlz_int)(sb - osb), hash, osb, max_dist, max_len, &best_len,
			&best_savings, loops);

		if (!best_dist || best_len < MLZ_MIN_MATCH) {
			opt->len  = 1;
			opt->cost = 0;
			opt->lcost = mlz_compute_cost(0, 1);
			opt->dist = 0;
			opt++;
			mlz_match_hash_next_byte(matcher, hash, (size_t)(sb - osb));
			sb++;
			continue;
		}

		/* this optimization is intended for skipping long runs of the same byte */
		if (opt > optimal && opt[-1].dist && best_dist == opt[-1].dist && best_len+1 == opt[-1].len && sb[-1] == sb[0] && sb-2 >= osb && sb[-2] == sb[0]) {
			/* skip chain... */
			while (best_len > 0) {
				opt->len  = best_len;
				opt->cost = 0;
				opt->lcost = mlz_compute_cost(best_dist, best_len);
				if (best_len < MLZ_MIN_MATCH) {
					opt->dist = 0;
					opt->len = 1;
					opt->lcost = mlz_compute_cost(opt->dist, opt->len);
				} else {
					opt->dist = best_dist;
				}
				opt++;

				best_len--;

				MLZ_HASHBYTE(sb);
				mlz_match_hash_next_byte(matcher, hash, (size_t)(sb - osb));
				sb++;
			}
			continue;
		}

		opt->len  = best_len;
		opt->cost = 0;
		opt->lcost = mlz_compute_cost(best_dist, best_len);
		opt->dist = best_dist;
		opt++;

		MLZ_ASSERT(sb - best_dist >= osb);

		mlz_match_hash_next_byte(matcher, hash, (size_t)(sb - osb));
		sb++;
	}

#undef MLZ_HASHBYTE

	{
		/* optimal parse backward pass */
		mlz_int i;
		mlz_int size = (mlz_int)(opt - optimal);
		for (i=size-1; i>=0; i--) {
			mlz_optimal *o = optimal + i;
			/* okay, so now: compute cost... */
			if (i == size-1)
				o->cost = o->lcost;
			else {
				mlz_int j;
				o->cost = o->lcost + (o+o->len >= opt ? 0 : o[o->len].cost);
				/* we can do better: virtually try to reduce match len!!! */
				/* this performs slightly better than forward iteration, even if literal runs are disabled... */
				for (j=o->len-1; j>=MLZ_MIN_MATCH; j--) {
					mlz_int tmpcost = mlz_compute_cost(o->dist, j);
					mlz_int ncost = tmpcost + (o+j >= opt ? 0 : o[j].cost);
					if (ncost < o->cost) {
						o->lcost = tmpcost;
						o->cost = ncost;
						o->len = j;
					}
				}
				{
					mlz_int tmpcost = 9;
					mlz_int ncost = tmpcost + (o+1 >= opt ? 0 : o[1].cost);
					if (ncost < o->cost) {
						o->lcost = tmpcost;
						o->cost = ncost;
						o->len  = 0;
						o->dist = 0;
					}
				}
			}
		}

		/* okay, fwd-flush literals */
		sb = (MLZ_CONST mlz_byte *)src;
		lit_start = sb;
		for (i=0; i<size; i++) {
			if (!optimal[i].dist) {
				sb++;
				continue;
			}
			MLZ_RET_FALSE(mlz_output_match(&accum, lit_start, sb, &db, de, optimal[i].dist, optimal[i].len));
			sb += optimal[i].len;
			i  += optimal[i].len-1;
			lit_start = sb;
		}
	}

	/* flush last lit chunk */
	if (lit_start < sb && !mlz_output_match(&accum, lit_start, sb, &db, de, 0, 0))
		return 0;

	MLZ_RET_FALSE(mlz_flush_accum(&accum, &db, de));

	if (accum.ptr == db-MLZ_ACCUM_BYTES && accum.count == 0)
		/* don't waste extra space */
		db -= MLZ_ACCUM_BYTES;

	return (size_t)(db - odb);
}

#undef MLZ_MATCH_BEST_COMMON
#undef MLZ_MATCH_BEST_SAVINGS
#undef MLZ_MATCH_BEST
#undef MLZ_MATCH
#undef MLZ_ADD_SHORT_LEN
