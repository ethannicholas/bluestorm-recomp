/* Portable spellings of the compiler builtins and attributes used by the runtime
   and by the generated code. Must stay C: recomp.h includes it.

   Clang and GCC provide these natively. MSVC needs intrinsics, so keep every use
   behind one of these macros rather than naming a __builtin_ directly. */
#pragma once
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#include <stdlib.h>

#define WR_BSWAP16(x) _byteswap_ushort((unsigned short)(x))
#define WR_BSWAP32(x) _byteswap_ulong((unsigned long)(x))
#define WR_BSWAP64(x) _byteswap_uint64((unsigned __int64)(x))

/* MSVC has no branch-probability hint; the condition still has to evaluate. */
#define WR_LIKELY(x) (!!(x))
#define WR_UNLIKELY(x) (!!(x))

#define WR_PRINTF_FMT(fmt_idx, first_arg)
/* COFF has no weak symbols in the ELF sense. Nothing overrides these today, so a
   plain definition is correct; see WR_WEAK's use in gx/fifo.cpp. */
#define WR_WEAK

static __inline uint32_t WR_CLZ32(uint32_t v) {
    unsigned long i;
    return _BitScanReverse(&i, (unsigned long)v) ? (uint32_t)(31 - i) : 32u;
}

#else /* Clang / GCC */

#define WR_BSWAP16(x) __builtin_bswap16((uint16_t)(x))
#define WR_BSWAP32(x) __builtin_bswap32((uint32_t)(x))
#define WR_BSWAP64(x) __builtin_bswap64((uint64_t)(x))

#define WR_LIKELY(x) __builtin_expect(!!(x), 1)
#define WR_UNLIKELY(x) __builtin_expect(!!(x), 0)

#define WR_PRINTF_FMT(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))

/* Clang on COFF only emulates weak via /alternatename, which we don't need. */
#if defined(_WIN32)
#define WR_WEAK
#else
#define WR_WEAK __attribute__((weak))
#endif

static inline uint32_t WR_CLZ32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32u; }

#endif
