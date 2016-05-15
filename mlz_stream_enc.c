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

#include "mlz_stream_enc.h"
#include "mlz_enc.h"
#include <string.h>

mlz_bool
mlz_out_stream_open(
	mlz_out_stream             **stream,
	MLZ_CONST mlz_stream_params *params,
	mlz_int                      level
)
{
	mlz_byte       *buf;
	mlz_out_stream *outs;
	mlz_int         context_size;

	MLZ_RET_FALSE(stream && params);
	/* block size test */
	MLZ_RET_FALSE(params->block_size >= MLZ_MIN_BLOCK_SIZE && params->block_size < MLZ_MAX_BLOCK_SIZE);
	/* power of two test */
	MLZ_RET_FALSE(!((mlz_uint)params->block_size & ~(mlz_uint)params->block_size));
	/* write function test */
	MLZ_RET_FALSE(params->write_func);

	*stream = (mlz_out_stream *)mlz_malloc(sizeof(mlz_out_stream));
	MLZ_RET_FALSE(*stream);

	context_size = MLZ_BLOCK_CONTEXT_SIZE;
	if (params->block_size < context_size)
		context_size = params->block_size;

	if (params->independent_blocks)
		context_size = 0;

	(*stream)->buffer = buf = (mlz_byte *)mlz_malloc(context_size + params->block_size*2);
	if (!buf) {
	out_stream_error:
		mlz_free(*stream);
		*stream = MLZ_NULL;
		return MLZ_FALSE;
	}

	outs = *stream;

	if (!mlz_matcher_init(&outs->matcher)) {
		mlz_free(buf);
		goto out_stream_error;
	}

	outs->block_size   = params->block_size;
	outs->context_size = context_size;
	outs->out_buffer   = buf + context_size + params->block_size;
	outs->ptr          = 0;
	outs->level        = level < 0 ? 0 : (level > MLZ_LEVEL_MAX ? MLZ_LEVEL_MAX : level);
	outs->first_block  = MLZ_TRUE;
	outs->params       = *params;

	return MLZ_TRUE;
}

static mlz_bool mlz_write_little_endian(mlz_out_stream *stream, mlz_uint val)
{
	mlz_byte buf[4];
	buf[0] = (mlz_byte)(val & 255);
	buf[1] = (mlz_byte)((val >> 8) & 255);
	buf[2] = (mlz_byte)((val >> 16) & 255);
	buf[3] = (mlz_byte)((val >> 24) & 255);
	return stream->params.write_func(stream->params.handle, buf, 4) == 4;
}

static mlz_bool mlz_out_stream_flush_block(mlz_out_stream *stream)
{
	size_t out_len, real_out_len;
	mlz_bool partial_block = MLZ_FALSE;

	MLZ_ASSERT(stream);
	/* if nothing to do => success */
	if (!stream->ptr)
		return MLZ_TRUE;

	/* flush (compress) */
	out_len = mlz_compress(
		stream->matcher,
		stream->out_buffer,
		stream->block_size,
		stream->buffer + stream->context_size,
		stream->ptr,
		stream->context_size*(stream->first_block != MLZ_TRUE),
		stream->level
	);

	stream->first_block = MLZ_FALSE;

	real_out_len = out_len;

	MLZ_ASSERT(out_len <= (size_t)MLZ_MAX_BLOCK_SIZE && stream->ptr <= MLZ_MAX_BLOCK_SIZE);

	if (!out_len || out_len >= (size_t)stream->ptr) {
		memcpy(stream->out_buffer, stream->buffer + stream->context_size, stream->ptr);
		real_out_len = (size_t)stream->ptr;
		/* mark as uncompressed */
		real_out_len |= MLZ_UNCOMPRESSED_BLOCK_MASK;
	}

	/* mark as partial block if necessary */
	if (stream->ptr != stream->block_size) {
		real_out_len |= MLZ_PARTIAL_BLOCK_MASK;
		partial_block = MLZ_TRUE;
	}

	/* execute block callback if desired */
	if (stream->params.block_func)
		stream->params.block_func(stream->params.handle);

	/* write block len + flags */
	MLZ_RET_FALSE(mlz_write_little_endian(stream, (mlz_uint)real_out_len));

	real_out_len &= MLZ_BLOCK_LEN_MASK;

	/* compute and write compressed block checksum if needed */
	if (stream->params.block_checksum) {
		mlz_uint checksum = stream->params.block_checksum(stream->out_buffer, real_out_len);
		MLZ_RET_FALSE(mlz_write_little_endian(stream, (mlz_uint)checksum));
	}

	if (partial_block)
		MLZ_RET_FALSE(mlz_write_little_endian(stream, (mlz_uint)stream->ptr));

	/* write block */
	MLZ_RET_FALSE(stream->params.write_func(stream->params.handle, stream->out_buffer,
		(mlz_intptr)real_out_len) == (mlz_intptr)real_out_len);

	/* update incremental checksum if needed */
	if (stream->params.incremental_checksum) {
		stream->params.initial_checksum =
			stream->params.incremental_checksum(stream->buffer + stream->context_size,
				stream->ptr, stream->params.initial_checksum);
	}

	/* copy block context unless last block */
	if (stream->context_size > 0 && stream->ptr >= stream->context_size)
		memcpy(stream->buffer, stream->buffer + stream->ptr, stream->context_size );

	/* reset pointer */
	stream->ptr = 0;

	return MLZ_TRUE;
}

mlz_bool
mlz_out_stream_close(
	mlz_out_stream *stream
)
{
	MLZ_RET_FALSE(stream);
	MLZ_RET_FALSE(mlz_out_stream_flush_block(stream));

	/* encode end of stream */
	MLZ_RET_FALSE(mlz_write_little_endian(stream, 0u));

	/* write final checksum if needed */
	if (stream->params.incremental_checksum)
		MLZ_RET_FALSE(mlz_write_little_endian(stream, stream->params.initial_checksum));

	if (stream->params.close_func)
		MLZ_RET_FALSE(stream->params.close_func(stream->params.handle));

	MLZ_RET_FALSE(mlz_matcher_free(stream->matcher));
	mlz_free(stream->buffer);
	mlz_free(stream);
	return MLZ_TRUE;
}

mlz_intptr
mlz_stream_write(
	mlz_out_stream *stream,
	MLZ_CONST void *buf,
	mlz_intptr      size
)
{
	MLZ_CONST mlz_byte *src = (MLZ_CONST mlz_byte *)buf;
	mlz_intptr nwritten = 0;

	MLZ_ASSERT(stream);
	while (size > 0) {
		/* filling buffer */
		mlz_intptr to_fill  = size;
		mlz_intptr capacity = stream->block_size - stream->ptr;
		if (to_fill > capacity)
			to_fill = capacity;
		if (to_fill > 0) {
			memcpy(stream->buffer + stream->context_size + stream->ptr, src, to_fill);
			stream->ptr += (mlz_int)to_fill;
			size        -= to_fill;
			src         += to_fill;
			nwritten    += to_fill;
		}
		if (stream->ptr >= stream->block_size && !mlz_out_stream_flush_block(stream))
			return -1;
	}

	return nwritten;
}
