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

#ifndef MLZ_STREAM_COMMON_H
#define MLZ_STREAM_COMMON_H

#include "mlz_common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mlz_jobs;

typedef struct {
	/* user data (handle) */
	void            *handle;
	/* for multi-threaded (de)compression, null if single-threaded */
	struct mlz_jobs *jobs;
	/* note: in stream doesn't need write func while outstream doesn't need read func */
	/* low level read function, returns -1 on error, otherwise number of bytes read   */
	mlz_intptr (*read_func)(void *handle, void *buf, mlz_intptr size);
	/* low level write function, returns -1 on error, otherwise number of bytes written */
	mlz_intptr (*write_func)(void *handle, MLZ_CONST void *buf, mlz_intptr size);
	/* optional rewind callback, in stream only */
	mlz_bool   (*rewind_func)(void *handle);
	/* optional low level close function, returns MLZ_TRUE on success   */
	mlz_bool   (*close_func)(void *handle);
	/* optional block callback, out stream only, called before new block is written */
	void       (*block_func)(void *handle);
	/* compressed block checksum function, returns 32-bit hash */
	/* buf is guaranteed to be 8-byte aligned                  */
	/* set to null to disable compressed block checksum        */
	mlz_uint   (*block_checksum)(MLZ_CONST void *buf, size_t size);
	/* incremental uncompressed checksum function, prev is previous checksum value */
	/* buf is guaranteed to be 8-byte aligned                                      */
	/* set to null to disable incremental checksum                                 */
	/* returns new checksum value                                                  */
	mlz_uint   (*incremental_checksum)(MLZ_CONST void *buf, size_t size, mlz_uint prev);
	/* initial value for incremental checksum   */
	mlz_uint     initial_checksum;
	/* desired block size, 65536 is recommended */
	/* must be power of two                     */
	mlz_int      block_size;
	/* set to MLZ_TRUE to use independent blocks        */
	/* hurts ratio but improves compression speed a lot */
	mlz_bool     independent_blocks;
	/* unsafe: no bounds check, skip checksum (if any)  */
	mlz_bool     unsafe;
	/* use simple stream header to identify block params automatically */
	mlz_bool     use_header;
} mlz_stream_params;

/* default params wrapped around stdio, just copy and assign handle */
extern MLZ_API MLZ_CONST mlz_stream_params mlz_default_stream_params;

enum mlz_stream_constants
{
	MLZ_MIN_BLOCK_SIZE          = 1 << 10,
	MLZ_MAX_BLOCK_SIZE          = 1 << 29,
	MLZ_UNCOMPRESSED_BLOCK_MASK = 1 << 30,
	MLZ_PARTIAL_BLOCK_MASK      = 1 << 31,
	MLZ_BLOCK_LEN_MASK          = MLZ_UNCOMPRESSED_BLOCK_MASK-1,
	/* to support dependent-block streaming */
	MLZ_BLOCK_CONTEXT_SIZE      = MLZ_MAX_DIST+1,
	/* maximum # of threads in multi-threaded mode */
	MLZ_MAX_THREADS             = 32
};

#ifdef __cplusplus
}
#endif

#endif
