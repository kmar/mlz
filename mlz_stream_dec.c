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

#include "mlz_stream_dec.h"
#include "mlz_dec.h"
/* mlz_malloc, mlz_free */
#include "mlz_enc.h"
#include <stdio.h>
#include <string.h>

static mlz_intptr mlz_stream_read_wrapper(void *handle, void *buf, mlz_intptr size)
{
	size_t res = (size_t)fread(buf, 1, size, (FILE *)handle);
	return ferror((FILE *)handle) ? -1 : (mlz_intptr)res;
}

static mlz_intptr mlz_stream_write_wrapper(void *handle, MLZ_CONST void *buf, mlz_intptr size)
{
	size_t res = (size_t)fwrite(buf, 1, size, (FILE *)handle);
	return ferror((FILE *)handle) ? -1 : (mlz_intptr)res;
}

mlz_bool mlz_stream_close_wrapper(void *handle)
{
	return fclose((FILE *)handle) == 0;
}

/* simple adler32 checksum (Mark Adler's Fletcher variant) */
mlz_uint
mlz_adler32(
	MLZ_CONST void *buf,
	size_t size,
	mlz_uint checksum
)
{
	MLZ_CONST mlz_byte *b = (MLZ_CONST mlz_byte *)buf;
	mlz_uint hi = (checksum >> 16);
	mlz_uint lo = checksum & 0xffffu;

	MLZ_ASSERT(buf);

	while (size >= 5552) {
		/* fast path */
		mlz_uint i;
		for ( i=5552/4; i>0; i--) {
			lo += *b++;
			hi += lo;
			lo += *b++;
			hi += lo;
			lo += *b++;
			hi += lo;
			lo += *b++;
			hi += lo;
		}
		lo %= 65521;
		hi %= 65521;
		size -= 5552;
	}

	while (size >= 4) {
		lo += *b++;
		hi += lo;
		lo += *b++;
		hi += lo;
		lo += *b++;
		hi += lo;
		lo += *b++;
		hi += lo;
		size -= 4;
	}

	while (size--) {
		lo += *b++;
		hi += lo;
	}

	lo %= 65521;
	hi %= 65521;

	return lo | (hi << 16);
}

mlz_uint
mlz_adler32_simple(
	MLZ_CONST void *buf,
	size_t size
)
{
	return mlz_adler32(buf, size, 1);
}

MLZ_CONST mlz_stream_params mlz_default_stream_params = {
	/* user data (handle) */
	MLZ_NULL,
	mlz_stream_read_wrapper,
	mlz_stream_write_wrapper,
	mlz_stream_close_wrapper,
	MLZ_NULL,
	/* compressed block checksum function, returns 32-bit hash */
	MLZ_NULL,
	/* incremental uncompressed checksum function */
	mlz_adler32,
	/* initial value for incremental checksum   */
	1,
	/* desired block size, 65536 is recommended */
	65536,
	/* independent blocks flag */
	MLZ_FALSE
};

mlz_bool
mlz_in_stream_open(
	mlz_in_stream              **stream,
	MLZ_CONST mlz_stream_params *params
)
{
	mlz_byte       *buf;
	mlz_in_stream  *ins;
	mlz_int         context_size;

	MLZ_RET_FALSE(stream && params);
	/* block size test */
	MLZ_RET_FALSE(params->block_size >= MLZ_MIN_BLOCK_SIZE && params->block_size < MLZ_MAX_BLOCK_SIZE);
	/* power of two test */
	MLZ_RET_FALSE(!((mlz_uint)params->block_size & ~(mlz_uint)params->block_size));
	/* read function test */
	MLZ_RET_FALSE(params->read_func);

	*stream = (mlz_in_stream *)mlz_malloc(sizeof(mlz_in_stream));
	MLZ_RET_FALSE(*stream);

	context_size = MLZ_BLOCK_CONTEXT_SIZE;
	if (context_size < params->block_size)
		context_size = params->block_size;

	if (params->independent_blocks)
		context_size = 0;

	(*stream)->buffer = buf = (mlz_byte *)mlz_malloc(context_size + params->block_size + MLZ_BLOCK_DEC_RESERVE);
	if (!buf) {
		mlz_free(*stream);
		*stream = MLZ_NULL;
		return MLZ_FALSE;
	}

	ins               = *stream;
	ins->params       = *params;
	ins->block_size   = params->block_size;
	ins->context_size = context_size;
	ins->ptr          = MLZ_NULL;
	ins->top          = MLZ_NULL;
	ins->is_eof       = MLZ_FALSE;
	ins->first_block  = MLZ_TRUE;
	return MLZ_TRUE;
}

static mlz_bool mlz_read_little_endian(mlz_in_stream *stream, mlz_uint *val)
{
	mlz_byte buf[4];
	MLZ_ASSERT(val);
	MLZ_RET_FALSE(stream->params.read_func(stream->params.handle, buf, 4) == 4);
	*val = buf[0] + (buf[1] << 8) + (buf[2] << 16) + ((mlz_uint)buf[3] << 24);
	return MLZ_TRUE;
}

static mlz_bool
mlz_in_stream_read_block(mlz_in_stream *stream)
{
	mlz_uint  blk_size;
	mlz_uint  usize;
	mlz_bool  partial, uncompressed;
	mlz_int   target_pos;
	mlz_byte *target;
	mlz_uint  block_checksum = 0;

	MLZ_RET_FALSE(mlz_read_little_endian(stream, &blk_size));

	partial      = (blk_size & MLZ_PARTIAL_BLOCK_MASK) != 0;
	uncompressed = (blk_size & MLZ_UNCOMPRESSED_BLOCK_MASK) != 0;

	blk_size &= MLZ_BLOCK_LEN_MASK;

	MLZ_RET_FALSE(blk_size <= (mlz_uint)stream->block_size);

	if (blk_size == 0) {
		if (stream->params.incremental_checksum) {
			mlz_uint checksum;
			MLZ_RET_FALSE(mlz_read_little_endian(stream, &checksum) &&
				checksum == stream->params.initial_checksum);
		}
		stream->ptr = stream->top = MLZ_NULL;
		stream->is_eof = MLZ_TRUE;
		return MLZ_TRUE;
	}

	usize = stream->block_size;

	/* load block checksum if needed */
	if (stream->params.block_checksum) {
		MLZ_RET_FALSE(mlz_read_little_endian(stream, &block_checksum));
	}

	if ( partial )
		MLZ_RET_FALSE(mlz_read_little_endian(stream, &usize) &&
			usize > 0 && usize <= (mlz_uint)stream->block_size);

	target_pos = stream->context_size + stream->block_size + MLZ_BLOCK_DEC_RESERVE - blk_size;
	/* make sure buffer is aligned, we have 1k reserve anyway */
	target_pos &= ~(mlz_uintptr)7;

	target = stream->buffer + target_pos;
	if ( uncompressed )
		/* special handling of uncompressed blocks */
		target = stream->buffer + stream->context_size;

	MLZ_RET_FALSE(stream->params.read_func(stream->params.handle, target,
		(mlz_intptr)blk_size) == (mlz_intptr)blk_size);

	/* validate compressed checksum */
	if (stream->params.block_checksum)
		MLZ_RET_FALSE(stream->params.block_checksum(target, blk_size) == block_checksum);

	stream->ptr = stream->buffer + stream->context_size;

	if ( !uncompressed ) {
		/* and finally: decompress (in-place) */
		size_t dlen = mlz_decompress(stream->buffer + stream->context_size, usize, target,
			blk_size, stream->block_size * (stream->first_block != MLZ_TRUE));
		MLZ_RET_FALSE(dlen == (size_t)usize);
	}

	/* compute incremental checksum if needed */
	if (stream->params.incremental_checksum)
		stream->params.initial_checksum =
			stream->params.incremental_checksum(stream->buffer + stream->context_size,
				usize, stream->params.initial_checksum);

	stream->top = stream->ptr + usize;

	/* copy context */
	if (stream->context_size > 0 && usize >= (size_t)stream->context_size)
		memcpy(stream->buffer, stream->buffer + usize, stream->context_size);

	stream->first_block = MLZ_FALSE;

	return MLZ_TRUE;
}

mlz_intptr
mlz_stream_read(
	mlz_in_stream *stream,
	void          *buf,
	mlz_intptr     size
)
{
	mlz_intptr nread = 0;
	mlz_byte *db = (mlz_byte *)buf;

	MLZ_ASSERT(stream);

	while (size > 0) {
		mlz_intptr to_fill, capacity;

		if (MLZ_UNLIKELY(stream->ptr >= stream->top)) {
			if (stream->is_eof)
				break;

			/* read next block */
			if (!mlz_in_stream_read_block(stream))
				return -1;
		}

		to_fill = size;
		capacity = (mlz_intptr)(stream->top - stream->ptr);
		if (to_fill > capacity)
			to_fill = capacity;

		if (to_fill > 0) {
			memcpy(db, stream->ptr, to_fill);
			stream->ptr += to_fill;
			size        -= to_fill;
			db          += to_fill;
			nread       += to_fill;
		}
	}

	return nread;
}

mlz_bool
mlz_in_stream_close(
	mlz_in_stream *stream
)
{
	MLZ_RET_FALSE(stream);

	if (stream->params.close_func)
		MLZ_RET_FALSE(stream->params.close_func(stream->params.handle));

	mlz_free(stream->buffer);
	mlz_free(stream);
	return MLZ_TRUE;
}
