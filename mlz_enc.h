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

#ifndef MLZ_ENC_H
#define MLZ_ENC_H

#include "mlz_common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern MLZ_API void *(*mlz_malloc)(size_t);
extern MLZ_API void (*mlz_free)(void *);

struct mlz_matcher;

/* compression level constants */
typedef enum {
	MLZ_LEVEL_FASTEST = 0,
	MLZ_LEVEL_MEDIUM  = 5,
	MLZ_LEVEL_MAX     = 10
} mlz_compression_level;

/* initialize matcher */
MLZ_API mlz_bool
mlz_matcher_init(
	struct mlz_matcher **matcher
);

/* free matcher */
MLZ_API mlz_bool
mlz_matcher_free(
	struct mlz_matcher *matcher
);

/* main compression function, can be used for block-based streaming    */
/* and reuse matcher for subsequent calls to avoid memory reallocation */
MLZ_API size_t
mlz_compress(
	struct mlz_matcher *matcher,
	void               *dst,
	size_t              dst_size,
	MLZ_CONST void     *src,
	size_t              src_size,
	size_t              bytes_before_src,
	int                 level
);

/* straightforward version, manages matcher internally, */
/* doesn't allow streaming                              */
MLZ_API size_t
mlz_compress_simple(
	void           *dst,
	size_t          dst_size,
	MLZ_CONST void *src,
	size_t          src_size,
	int             level
);

#ifdef __cplusplus
}
#endif

#endif
