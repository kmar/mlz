/*
   mini-LZ library (mlz)
   (c) Martin Sedlak 2016

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

#include "mlz_stream_enc.h"
#include "mlz_enc.h"
#include <string.h>

static mlz_bool mlz_out_stream_free(mlz_out_stream *stream)
{
	mlz_int i;
	for (i=0; i<stream->num_threads; i++)
		MLZ_RET_FALSE(mlz_matcher_free(stream->matchers[i]));

#if defined(MLZ_THREADS)
	MLZ_RET_FALSE(mlz_mutex_destroy(stream->mutex));
#endif

	mlz_free(stream->buffer_unaligned);
	mlz_free(stream);

	return MLZ_TRUE;
}

mlz_out_stream *
mlz_out_stream_open(
	MLZ_CONST mlz_stream_params *params,
	mlz_int                      level
)
{
	mlz_byte       *buf;
	mlz_out_stream *outs;
	mlz_int         i, context_size;
	mlz_int         num_threads = 1;

	MLZ_RET_FALSE(params);
	/* block size test */
	MLZ_RET_FALSE(params->block_size >= MLZ_MIN_BLOCK_SIZE && params->block_size < MLZ_MAX_BLOCK_SIZE);
	/* power of two test */
	MLZ_RET_FALSE(!((mlz_uint)params->block_size & ((mlz_uint)params->block_size)-1));
	/* write function test */
	MLZ_RET_FALSE(params->write_func);

	outs = (mlz_out_stream *)mlz_malloc(sizeof(mlz_out_stream));
	MLZ_RET_FALSE(outs);
	memset(outs, 0, sizeof(mlz_out_stream));

#if defined(MLZ_THREADS)
	outs->mutex = mlz_mutex_create();
	if (!outs->mutex)
		goto out_stream_error;
#endif

	context_size = MLZ_BLOCK_CONTEXT_SIZE;
	if (params->block_size < context_size)
		context_size = params->block_size;

	if (params->independent_blocks)
		context_size = 0;

#if defined(MLZ_THREADS)
	if (params->jobs)
		num_threads += params->jobs->num_threads;
	if (num_threads < 1 || num_threads > MLZ_MAX_THREADS)
		goto out_stream_error;
#endif

	outs->buffer_unaligned = (mlz_byte *)mlz_malloc(context_size + params->block_size*2*num_threads + MLZ_CACHELINE_ALIGN-1);
	if (!outs->buffer_unaligned) {
out_stream_error:
		(void)mlz_out_stream_free(outs);
		return MLZ_NULL;
	}

	buf = outs->buffer_unaligned;
	buf = (mlz_byte *)((mlz_uintptr)buf & ~((mlz_uintptr)MLZ_CACHELINE_ALIGN-1));
	if (buf < outs->buffer_unaligned)
		buf += MLZ_CACHELINE_ALIGN;

	MLZ_ASSERT(buf >= outs->buffer_unaligned);

	outs->buffer = buf;

	for (i=0; i<num_threads; i++)
		if (!mlz_matcher_init(outs->matchers + i))
			goto out_stream_error;

	outs->block_size   = params->block_size;
	outs->context_size = context_size;
	outs->out_buffer   = buf + context_size + params->block_size*num_threads;
	outs->checksum     = params->initial_checksum;
	outs->ptr          = 0;
	outs->level        = level < 0 ? 0 : (level > MLZ_LEVEL_MAX ? MLZ_LEVEL_MAX : level);
	outs->num_threads  = num_threads;
	outs->first_block  = MLZ_TRUE;
	outs->params       = *params;

	/* prepare simple 2-byte block header        */
	/* bits 4-0: log2(block_size)                */
	/* bit 5   : independent                     */
	/* bit 6   : use block checksum (adler32)    */
	/* bit 7   : use incremental chsum (adler32) */
	/* 2nd byte = ~hdr (validation)              */

	if (params->use_header) {
		mlz_byte hdr[2];
		hdr[0] = hdr[1] = 0;

		i = 1;
		while (params->block_size > i) {
			hdr[0]++;
			i <<= 1;
		}

		MLZ_ASSERT( hdr[0] < (1 << 5) );

		if (params->independent_blocks)
			hdr[0] |= 0x20;
		if (params->block_checksum)
			hdr[0] |= 0x40;
		if (params->incremental_checksum)
			hdr[0] |= 0x80;
		hdr[1] = (mlz_byte)~hdr[0];

		if (params->write_func(params->handle, hdr, 2) != 2)
			goto out_stream_error;
	}

	return outs;
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

static void mlz_compress_block_job(int thread, void *param)
{
	mlz_int ptr;
	size_t  out_len;
	mlz_out_stream *stream = (mlz_out_stream *)param;
	mlz_int num_sub_blocks = (stream->ptr + stream->block_size-1)/stream->block_size;

	if (thread < num_sub_blocks-1)
		ptr = stream->block_size;
	else
		ptr = stream->ptr - thread*stream->block_size;

	/* flush (compress) */
	out_len = mlz_compress(
		stream->matchers[thread],
		stream->out_buffer + thread*stream->block_size,
		stream->block_size,
		stream->buffer + stream->context_size + thread*stream->block_size,
		ptr,
		stream->context_size,
		stream->level
	);
#if defined(MLZ_THREADS)
	(void)mlz_mutex_lock(stream->mutex);
	stream->out_lens[thread] = out_len;
	(void)mlz_mutex_unlock(stream->mutex);
#else
	stream->out_lens[thread] = out_len;
#endif
}

static mlz_bool mlz_out_stream_flush_block(mlz_out_stream *stream)
{
	size_t out_len;
	mlz_int i, num_sub_blocks;

	MLZ_ASSERT(stream);
	/* if nothing to do => success */
	if (!stream->ptr)
		return MLZ_TRUE;

	num_sub_blocks = (stream->ptr + stream->block_size-1)/stream->block_size;

#if defined(MLZ_THREADS)
	if (stream->params.jobs) {
		MLZ_RET_FALSE(mlz_jobs_prepare_batch(stream->params.jobs, num_sub_blocks-1));
		for (i=1; i<num_sub_blocks; i++) {
			mlz_job job;
			job.job   = mlz_compress_block_job;
			job.param = stream;
			job.idx   = i;
			MLZ_RET_FALSE(mlz_jobs_enqueue(stream->params.jobs, job));
		}
	}
#endif

	/* flush (compress) */
	out_len = mlz_compress(
		stream->matchers[0],
		stream->out_buffer,
		stream->block_size,
		stream->buffer + stream->context_size,
		num_sub_blocks > 1 ? stream->block_size : stream->ptr,
		stream->context_size*(stream->first_block != MLZ_TRUE),
		stream->level
	);

#if defined(MLZ_THREADS)
	if (stream->params.jobs)
		MLZ_RET_FALSE(mlz_jobs_wait(stream->params.jobs));
#endif

	stream->out_lens[0] = out_len;

	stream->first_block = MLZ_FALSE;

	for (i=0; i<num_sub_blocks; i++) {
		size_t real_out_len;
		mlz_int ptr;
		mlz_bool partial_block     = MLZ_FALSE;
		mlz_byte *out_ptr          = stream->out_buffer + i*stream->block_size;
		MLZ_CONST mlz_byte *in_ptr = stream->buffer + stream->context_size + i*stream->block_size;

		if (i < num_sub_blocks-1)
			ptr = stream->block_size;
		else
			ptr = stream->ptr - i*stream->block_size;

		out_len = stream->out_lens[i];
		real_out_len = out_len;

		MLZ_ASSERT(out_len <= (size_t)MLZ_MAX_BLOCK_SIZE && ptr <= MLZ_MAX_BLOCK_SIZE);

		if (!out_len || out_len >= (size_t)ptr) {
			memcpy(out_ptr, in_ptr, ptr);
			real_out_len = (size_t)ptr;
			/* mark as uncompressed */
			real_out_len |= MLZ_UNCOMPRESSED_BLOCK_MASK;
		}

		/* mark as partial block if necessary */
		if (ptr != stream->block_size) {
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
			mlz_uint checksum = stream->params.block_checksum(out_ptr, real_out_len);
			MLZ_RET_FALSE(mlz_write_little_endian(stream, (mlz_uint)checksum));
		}

		if (partial_block)
			MLZ_RET_FALSE(mlz_write_little_endian(stream, (mlz_uint)ptr));

		/* write block */
		MLZ_RET_FALSE(stream->params.write_func(stream->params.handle, out_ptr,
			(mlz_intptr)real_out_len) == (mlz_intptr)real_out_len);

		/* update incremental checksum if needed */
		if (stream->params.incremental_checksum) {
			stream->checksum =
				stream->params.incremental_checksum(in_ptr,
					ptr, stream->checksum);
		}
	}

	/* copy block context unless last block */
	if (stream->context_size > 0 && stream->ptr >= stream->context_size)
		memcpy(stream->buffer, stream->buffer + stream->ptr, stream->context_size);

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
		MLZ_RET_FALSE(mlz_write_little_endian(stream, stream->checksum));

	if (stream->params.close_func)
		MLZ_RET_FALSE(stream->params.close_func(stream->params.handle));

	return mlz_out_stream_free(stream);
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
		mlz_intptr capacity = stream->block_size*stream->num_threads - stream->ptr;
		if (to_fill > capacity)
			to_fill = capacity;
		if (to_fill > 0) {
			memcpy(stream->buffer + stream->context_size + stream->ptr, src, to_fill);
			stream->ptr += (mlz_int)to_fill;
			size        -= to_fill;
			src         += to_fill;
			nwritten    += to_fill;
		}
		if (stream->ptr >= stream->block_size*stream->num_threads && !mlz_out_stream_flush_block(stream))
			return -1;
	}

	return nwritten;
}
