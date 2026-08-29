/* plat.h -- compiler and platform shims.
 *
 * The only place in doot allowed to know about a specific compiler. Everything
 * here is either a portable spelling of a compiler extension or a C99 stand-in
 * for a C11 feature. See docs/09-engineering.md.
 */
#ifndef DOOT_PLAT_H
#define DOOT_PLAT_H

#include <stddef.h>
#include <stdint.h>

/* ---- compiler identification ------------------------------------------- */

#if defined(__clang__)
#define DOOT_CC_CLANG 1
#elif defined(__GNUC__)
#define DOOT_CC_GCC 1
#elif defined(_MSC_VER)
#define DOOT_CC_MSVC 1
#endif

/* Computed goto drives interpreter dispatch (D001). MSVC has no equivalent, so
 * the interpreter keeps a `switch` fallback selected by this macro. */
#if defined(DOOT_CC_GCC) || defined(DOOT_CC_CLANG)
#define DOOT_HAVE_COMPUTED_GOTO 1
#else
#define DOOT_HAVE_COMPUTED_GOTO 0
#endif

/* ---- attributes -------------------------------------------------------- */

#if defined(DOOT_CC_GCC) || defined(DOOT_CC_CLANG)
#define DOOT_NORETURN __attribute__((noreturn))
#define DOOT_PRINTF(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
/* For functions taking a va_list: marks the format argument without checking
 * varargs. Without it, clang's -Wformat-nonliteral fires inside the callee. */
#define DOOT_VPRINTF(fmt_idx) __attribute__((format(printf, fmt_idx, 0)))
#define DOOT_LIKELY(x) __builtin_expect(!!(x), 1)
#define DOOT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define DOOT_INLINE static inline
#elif defined(DOOT_CC_MSVC)
#define DOOT_NORETURN __declspec(noreturn)
#define DOOT_PRINTF(fmt_idx, first_arg)
#define DOOT_VPRINTF(fmt_idx)
#define DOOT_LIKELY(x) (x)
#define DOOT_UNLIKELY(x) (x)
#define DOOT_INLINE static __inline
#else
#define DOOT_NORETURN
#define DOOT_PRINTF(fmt_idx, first_arg)
#define DOOT_VPRINTF(fmt_idx)
#define DOOT_LIKELY(x) (x)
#define DOOT_UNLIKELY(x) (x)
#define DOOT_INLINE static
#endif

#define DOOT_UNUSED(x) ((void)(x))

/* ---- alignment --------------------------------------------------------- */

/* Alignment satisfying every type doot uses, including double, uint64_t, and any
 * pointer.
 *
 * C99 has no _Alignof, and the usual stand-in -- declaring a struct inside
 * offsetof -- is a compiler extension rather than standard C99, which D046
 * forbids. Over-aligning is always safe and under-aligning never is, so typed
 * allocations use this constant instead of computing an exact alignment. The
 * cost is at most 15 bytes of padding per allocation, in an allocator whose
 * chunks are 32 KB. Callers that know their exact requirement, such as a byte
 * buffer, pass it directly to arena_alloc. */
#define DOOT_ALIGN_MAX 16u

/* ---- limits ------------------------------------------------------------ */

/* Source positions are 32-bit byte offsets throughout the compiler, which keeps
 * spans at 8 bytes. The cap is enforced when a source is loaded (DT0002). */
#define DOOT_MAX_SOURCE_BYTES ((size_t)64u * 1024u * 1024u)

#endif /* DOOT_PLAT_H */
