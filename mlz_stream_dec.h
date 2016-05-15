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

#ifndef MLZ_STREAM_DEC_H
#define MLZ_STREAM_DEC_H

#include "mlz_stream_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TODO?: come up with multithreaded interface for streaming decompression of independent blocks */
typedef struct
{
	/* 64k previous context, nk block size, 1kb unpack reserve */
	mlz_byte            *buffer;
	MLZ_CONST mlz_byte  *ptr;
	MLZ_CONST mlz_byte  *top;
	mlz_stream_params    params;
	mlz_int              block_size;
	mlz_int              context_size;
	mlz_bool             is_eof;
	mlz_bool             first_block;
} mlz_in_stream;

/* simple adler32 checksum (Mark Adler's Fletcher variant) */
MLZ_API mlz_uint
mlz_adler32(
	MLZ_CONST void *buf,
	size_t size,
	mlz_uint checksum
);

/* simple block variant of the above */
MLZ_API mlz_uint
mlz_adler32_simple(
	MLZ_CONST void *buf,
	size_t size
);

/* returns new stream or MLZ_NULL on failure */
MLZ_API mlz_in_stream *
mlz_in_stream_open(
	MLZ_CONST mlz_stream_params *params
);

/* returns -1 on error, otherwise number of bytes read */
MLZ_API mlz_intptr
mlz_stream_read(
	mlz_in_stream *stream,
	void          *buf,
	mlz_intptr     size
);

/* returns MLZ_TRUE on success */
MLZ_API mlz_bool
mlz_in_stream_rewind(
	mlz_in_stream *stream
);

/* returns MLZ_TRUE on success */
MLZ_API mlz_bool
mlz_in_stream_close(
	mlz_in_stream *stream
);

#ifdef __cplusplus
}
#endif

#endif
