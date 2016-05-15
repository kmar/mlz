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

#ifndef MLZ_DEC_H
#define MLZ_DEC_H

#include "mlz_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* safe decompression */
MLZ_API size_t
mlz_decompress(
	void           *dst,
	size_t          dst_size,
	MLZ_CONST void *src,
	size_t          src_size,
	size_t          bytes_before_dst
);

/* simple version of the above */
MLZ_API size_t
mlz_decompress_simple(
	void           *dst,
	size_t          dst_size,
	MLZ_CONST void *src,
	size_t          src_size
);

/* unsafe version of the above to squeeze a marginal gain out of it */
MLZ_API size_t
mlz_decompress_unsafe(
	void           *dst,
	MLZ_CONST void *src,
	size_t          src_size
);

#ifdef __cplusplus
}
#endif

#endif
