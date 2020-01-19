/*    
 * Copyright (C) 2020, Twinkle Labs, LLC.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "lisp.h"

#include "win32/w32_compat.h"
#include "microtime.h"

#ifdef WIN32
#include <direct.h>
#include "win32/dirent.h"
#else
#include <dirent.h>
#endif
#include <sys/stat.h>


#ifndef __linux
#define MSG_NOSIGNAL 0
#endif

/*
 * PLATFORM ENDIANESS
 *
 * Windows always use little endian.
 */
#ifdef _WIN32
#  define PLATFORM_LITTLE_ENDIAN
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define PLATFORM_BIG_ENDIAN
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define PLATFORM_LITTLE_ENDIAN
#else
#  error "Unknown endianness"
#endif

#if (__LP64__==1) || (defined _WIN64)
#  define PLATFORM_BIT64
#else
#  define PLATFORM_BIT32
#endif

#define assert_e(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "Assertion error: %s@%s:%d: (%s)\n", __func__, __FILE__, __LINE__, #expr); \
        goto Error;\
    } \
} while (0)

#define CAR(p)  lisp_car((Lisp_Pair*)(p))
#define CDR(p)  lisp_cdr((Lisp_Pair*)(p))
#define CADR(p) CAR(CDR(p))
#define CDDR(p) CDR(CDR(p))
#define CADDR(p) CAR(CDDR(p))
#define PUSHX(vm,o) lisp_push(vm,(Lisp_Object*)o)
#define CHECK(vm, expr, msg) do { if (!(expr)) lisp_err(vm, msg); } while (0)


#define MAX(a,b) ((a)<(b)?(b):(a))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_COUNT(a) (sizeof(a)/sizeof(a[0]))

#define MAX_PUB_KEY 128 // We actually only use 64
#define MAX_PRI_KEY 128

/*
 * Shared read-only VM instance.
 * Hosting immutable variables and stateless procedures.
 * Any VM can inherits it.
 */
extern Lisp_VM *g_vm;
