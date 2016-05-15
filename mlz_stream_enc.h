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

#ifndef MLZ_STREAM_ENC_H
#define MLZ_STREAM_ENC_H

#include "mlz_stream_common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mlz_matcher;

/* TODO?: come up with multithreaded interface for streaming compression */
typedef struct
{
	struct mlz_matcher  *matcher;
	/* 64k previous context, nk block size, nk output buffer */
	mlz_byte            *buffer;
	/* points into buffer */
	mlz_byte            *out_buffer;
	mlz_stream_params    params;
	mlz_int              ptr;
	mlz_int              block_size;
	mlz_int              context_size;
	mlz_int              level;
	mlz_bool             first_block;
} mlz_out_stream;

/* level = compression level, 0 = fastest, 10 = best */
/* returns MLZ_TRUE on success */
MLZ_API mlz_bool
mlz_out_stream_open(
	mlz_out_stream             **stream,
	MLZ_CONST mlz_stream_params *params,
	mlz_int                      level
);

/* returns -1 on error, otherwise number of bytes read */
MLZ_API mlz_intptr
mlz_stream_write(
	mlz_out_stream *stream,
	MLZ_CONST void *buf,
	mlz_intptr      size
);

/* returns MLZ_TRUE on success */
MLZ_API mlz_bool
mlz_out_stream_close(
	mlz_out_stream *stream
);

#ifdef __cplusplus
}
#endif

#endif
