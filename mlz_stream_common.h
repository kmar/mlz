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

#ifndef MLZ_STREAM_COMMON_H
#define MLZ_STREAM_COMMON_H

#include "mlz_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/* user data (handle) */
	void       *handle;
	/* note: in stream doesn't need write func while outstream doesn't need read func */
	/* low level read function, returns -1 on error, otherwise number of bytes read    */
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
	/* decompress reserve */
	MLZ_BLOCK_DEC_RESERVE		= 1 << 10
};

#ifdef __cplusplus
}
#endif

#endif
