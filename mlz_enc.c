/*
   mini-LZ library (mlz)
   Martin Sedlak 2016

   This is free and unencumbered software released into the public domain.

   Anyone is free to copy, modify, publish, use, compile, sell, or
   distribute this software, either in source code form or as a compiled
   binary, for any purpose, commercial or non-commercial, and by any
   means.

   In jurisdictions that recognize copyright laws, the author or authors
   of this software dedicate any and all copyright interest in the
   software to the public domain. We make this dedication for the benefit
   of the public at large and to the detriment of our heirs and
   successors. We intend this dedication to be an overt act of
   relinquishment in perpetuity of all present and future rights to this
   software under copyright law.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
   OTHER DEALINGS IN THE SOFTWARE.

   For more information, please refer to <http://unlicense.org/>
*/

#include "mlz_enc.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

enum mlz_match_constants
{
	/* because we don't clear hash list, setting this too high will actually slow things down */
	/* value of 2048 entries works best for Silesia corpus, YMMV                              */
	MLZ_HASH_SIZE      = 2048,
	MLZ_HASH_LIST_SIZE = 65536,
	MLZ_DICT_MASK      = MLZ_HASH_LIST_SIZE-1,
	MLZ_HASH_MASK      = MLZ_HASH_SIZE-1
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

/* simple hash-list (or hash-chain)                  */
/* note that this helper structure uses 132kB of RAM */
struct mlz_matcher
{
	mlz_ushort hash[MLZ_HASH_SIZE];
	mlz_ushort list[MLZ_HASH_LIST_SIZE];
};

mlz_bool mlz_matcher_init(struct mlz_matcher **matcher)
{
	if (!matcher)
		return MLZ_FALSE;

	*matcher = (struct mlz_matcher *)mlz_malloc(sizeof(struct mlz_matcher));
	return *matcher != MLZ_NULL;
}

static mlz_bool mlz_matcher_clear(struct mlz_matcher *matcher)
{
	MLZ_RET_FALSE(matcher != MLZ_NULL);
	memset(matcher, 0, sizeof(struct mlz_matcher));
	return MLZ_TRUE;
}

mlz_bool mlz_matcher_free(struct mlz_matcher *matcher)
{
	if (matcher)
		mlz_free(matcher);

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

	for (i=0; i<MLZ_ACCUM_BYTES; i++) {
		accum->ptr[i] = (mlz_byte)((accum->bits >> 8*i) & 255);
	}
	accum->ptr = *db;
	(*db) += MLZ_ACCUM_BYTES;
	accum->bits  = 0;
	accum->count = 0;

	return MLZ_TRUE;
}

MLZ_INLINE mlz_bool mlz_add_bit(mlz_accumulator *accum, mlz_byte **db, MLZ_CONST mlz_byte *de, int bit)
{
	accum->bits |= (mlz_uint)bit << accum->count;
	if (MLZ_LIKELY(++accum->count < MLZ_ACCUM_BITS))
		return MLZ_TRUE;

	return mlz_flush_accum(accum, db, de);
}

/* for max compression mode */
MLZ_INLINE mlz_int mlz_compute_savings(mlz_int dist, mlz_int len)
{
	/* compute cost now */
	mlz_int  bit_cost = 0;
	mlz_bool tiny_len = len >= MLZ_MIN_MATCH && len < MLZ_MIN_MATCH + (1 << 3);

	if (tiny_len && dist <= 256) {
		bit_cost = 3 + 3 + 8;
	} else if (tiny_len && dist < (1 << 13)) {
		bit_cost = 3 + 3 + 16;
	} else if (tiny_len) {
		bit_cost = 3 + 3 + 16;
	} else {
		bit_cost = 3 + 8 + 16 + 16*(len >= 255);
	}
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
	if (i >= max_len) \
		return best_dist; \

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

static mlz_int
mlz_match_save(
	struct mlz_matcher *m,
	size_t              pos,
	mlz_uint            hash,
	MLZ_CONST mlz_byte *buf,
	mlz_int             max_dist,
	mlz_int             max_len,
	mlz_int *           best_len,
	mlz_int *           best_save
)
{
	MLZ_MATCH(1, MLZ_MATCH_BEST_SAVINGS)
}

static mlz_int
mlz_match_nosave(
	struct mlz_matcher *m,
	size_t              pos,
	mlz_uint            hash,
	MLZ_CONST mlz_byte *buf,
	mlz_int             max_dist,
	mlz_int             max_len,
	mlz_int *           best_len
)
{
	MLZ_MATCH(1, MLZ_MATCH_BEST)
}

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
	if (loops == INT_MAX) {
		if (best_save)
			return mlz_match_save(m, pos, hash, buf, max_dist, max_len, best_len, best_save);
		return mlz_match_nosave(m, pos, hash, buf, max_dist, max_len, best_len);
	}
	if (best_save)
		return mlz_match_loops_save(m, pos, hash, buf, max_dist, max_len, best_len, best_save, loops);
	return mlz_match_loops_nosave(m, pos, hash, buf, max_dist, max_len, best_len, loops);
}

MLZ_INLINE void mlz_match_hash_next_byte(struct mlz_matcher *m, mlz_uint hash, size_t pos)
{
	mlz_ushort *idx;

	MLZ_ASSERT(m && hash <= MLZ_HASH_MASK);

	idx = m->hash + hash;
	m->list[pos & MLZ_DICT_MASK] = *idx;
	*idx = pos & MLZ_DICT_MASK;
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

	/* 3 tested best */
	MLZ_CONST mlz_int len_bits = 3;

	MLZ_RET_FALSE(*db < de);

	nlit = (mlz_int)(le - lb);

	/* literal run cost:                                                                       */
	/* 3 bits + two words => 32 + 3 = 35 bits static cost + 8*lit => minimum literal run is 36 */
	/* normally single literal costs 9 bits                                                    */
	/* so we only do this for 36+ literals                                                     */
	while (nlit >= MLZ_MIN_LIT_RUN) {
		mlz_int run = mlz_min(65535, nlit);
		MLZ_RET_FALSE(mlz_output_match(accum, MLZ_NULL, MLZ_NULL, db, de, 0, MLZ_MIN_MATCH));

		MLZ_RET_FALSE(*db + 1 < de);
		*(*db)++ = (mlz_byte)(run & 255);
		*(*db)++ = (mlz_byte)(run >> 8);

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
	100: tiny match + 3 bits len-min_match + byte dist-1
	101: short match + word dist (3 msbits decoded as short length)
	110: short match + 3 bits len-min match + word dist
	111: full match + byte len (255 = word len follows) + word dist
	dist = 0 => literal run (then word follows: number of literals, values < 36 are illegal)
	*/

	#define MLZ_ADD_SHORT_LEN() \
		len -= MLZ_MIN_MATCH; \
		for (j=0; j<len_bits; j++) \
			MLZ_RET_FALSE(mlz_add_bit(accum, db, de, (len >> j) & 1));

	tiny_len = len >= MLZ_MIN_MATCH && len < MLZ_MIN_MATCH + (1<<len_bits);

	if (dist > 0 && dist-1 < 256 && tiny_len) {
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 1));
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 0));
		MLZ_RET_FALSE(mlz_add_bit(accum, db, de, 0));
		MLZ_ADD_SHORT_LEN()

		MLZ_RET_FALSE(*db < de);
		*(*db)++ = (mlz_byte)((dist-1) & 255);
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
	MLZ_CONST mlz_byte *match_start_max = se - MLZ_MIN_MATCH;
	mlz_byte *db = (mlz_byte *)dst;
	MLZ_CONST mlz_byte *odb = db;
	MLZ_CONST mlz_byte *de = db + dst_size;
	MLZ_CONST mlz_byte *lit_start = sb;
	MLZ_CONST mlz_byte *tmp = osb;

	MLZ_RET_FALSE(mlz_matcher_clear(matcher) && dst && src);

#if !defined(__BORLANDC__)
	/* cannot handle blocks larger than 2G - 64k - 1 */
	MLZ_RET_FALSE(sb - osb <= INT_MAX && se - sb <= INT_MAX);
#endif

	level = mlz_clamp(level, 0, 10);
	loops = INT_MAX;
	if (level < 9)
		loops = 1 << level;

	accum.bits  = 0;
	accum.count = 0;

	MLZ_RET_FALSE(db + MLZ_ACCUM_BYTES <= de);
	accum.ptr = db;
	db       += MLZ_ACCUM_BYTES;

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
		mlz_int max_len  = mlz_min(MLZ_MAX_MATCH, (mlz_int)(se - sb));

		/* compute hash at sb */
		MLZ_HASHBYTE(sb);

		if (!max_dist || max_len < MLZ_MIN_MATCH) {
			mlz_match_hash_next_byte(matcher, hash, (size_t)(sb - osb));
			sb++;
			continue;
		}

		/* try to find a match now */
		best_dist = sb > match_start_max ? 0 :
			mlz_match(matcher, (mlz_int)(sb - osb), hash, osb, max_dist, max_len, &best_len,
				level >= 10 ? &best_savings : MLZ_NULL, loops);

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
			mlz_int max_len2  = mlz_min(MLZ_MAX_MATCH, (mlz_int)(se - sb2));

			/* trying to speed things up using Yann Collet's advanced parsing strategies:                                        */
			/* just try to look for MINMATCH at P+ML+2-MINMATCH first (helps, in some cases ~15% but in others even 50% speedup) */
			MLZ_CONST mlz_byte *lazysb = sb + best_len + 2 - MLZ_MIN_MATCH;
			if (lazysb + MLZ_MIN_MATCH > se)
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
					level >=10 ? &best_savings : MLZ_NULL, loops);
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

#undef MLZ_HASHBYTE

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

	mlz_matcher_free(m);
	return res;
}

#undef MLZ_MATCH_BEST_COMMON
#undef MLZ_MATCH_BEST_SAVINGS
#undef MLZ_MATCH_BEST
#undef MLZ_MATCH
#undef MLZ_ADD_SHORT_LEN
