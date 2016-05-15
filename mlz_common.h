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

#ifndef MLZ_COMMON_H
#define MLZ_COMMON_H

/* boilerplate macros */

/* you can override this externally */
#if !defined(MLZ_API)
#	define MLZ_API
#endif

#if defined(_MSC_VER) || defined(__BORLANDC__)
#	define MLZ_INLINE static __inline
#else
#	define MLZ_INLINE static inline
#endif

#if !defined MLZ_CONST
#	define MLZ_CONST  const
#endif

#if !defined(MLZ_DEBUG) && (defined(DEBUG) || defined(_DEBUG)) && !defined(NDEBUG)
#	define MLZ_DEBUG 1
#endif

#if !defined(MLZ_ASSERT)
#	if defined(MLZ_DEBUG)
#		include <assert.h>
#		define MLZ_ASSERT(expr) assert(expr)
#	else
#		define MLZ_ASSERT(expr)
#	endif
#endif

#if !defined(MLZ_LIKELY)
#	if defined(__clang__) || defined(__GNUC__)
#		define MLZ_LIKELY(x)   __builtin_expect(!!(x), 1)
#		define MLZ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#	else
#		define MLZ_LIKELY(x) x
#		define MLZ_UNLIKELY(x) x
#	endif
#endif

/* types */

#include <stddef.h>

#if defined(__BORLANDC__)
typedef long          intptr_t;
typedef unsigned long uintptr_t;
#endif

/* the following should be compatible with vs2008 and up */
#if (defined(_MSC_VER) && _MSC_VER < 1900) || defined(__BORLANDC__)
	typedef unsigned __int8  mlz_byte;
	typedef signed   __int8  mlz_sbyte;
	typedef unsigned __int16 mlz_ushort;
	typedef signed   __int16 mlz_short;
	typedef unsigned __int32 mlz_uint;
	typedef signed   __int32 mlz_int;
	typedef unsigned __int64 mlz_ulong;
	typedef signed   __int64 mlz_long;
#	if defined(__clang__) || defined(__GNUC__)
#		include <stdint.h>
#	endif
#else
	/* do it the standard way */
#	include <stdint.h>
	typedef uint8_t     mlz_byte;
	typedef int8_t      mlz_sbyte;
	typedef uint16_t    mlz_ushort;
	typedef int16_t     mlz_short;
	typedef uint32_t    mlz_uint;
	typedef int32_t     mlz_int;
	typedef uint64_t    mlz_ulong;
	typedef int64_t     mlz_long;
#endif

typedef intptr_t   mlz_intptr;
typedef uintptr_t  mlz_uintptr;
typedef size_t     mlz_size;
typedef char       mlz_char;
typedef mlz_int    mlz_bool;

/* constants */

#ifndef NULL
#	define MLZ_NULL  0
#else
#	define MLZ_NULL NULL
#endif
#define MLZ_TRUE  ((mlz_bool)1)
#define MLZ_FALSE ((mlz_bool)0)

/* helper macros */

#define MLZ_RET_FALSE(expr) if (!(expr)) return 0

/* compression-specific constants */

typedef enum {
	MLZ_MAX_DIST    = 65535,
	/* min match len of 3 tested better than 2 */
	MLZ_MIN_MATCH   = 3,
	MLZ_MAX_MATCH   = 65535,
	MLZ_MIN_LIT_RUN = 36,
	MLZ_ACCUM_BITS  = 24,
	MLZ_ACCUM_BYTES = 3
} mlz_constants;

#endif
