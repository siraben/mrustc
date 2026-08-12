#ifndef CM_CONFIG_H
#define CM_CONFIG_H

/* The bootstrap compiler deliberately uses a small, probed C99 subset. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
# error "cmrustc requires a C99 compiler"
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* Some bootstrap libcs provide the exact-width types but omit C99 macros. */
#ifndef UINT8_C
# define UINT8_C(value) value
#endif
#ifndef UINT16_C
# define UINT16_C(value) value
#endif
#ifndef UINT32_C
# define UINT32_C(value) value ## U
#endif
#ifndef UINT64_C
# define UINT64_C(value) value ## ULL
#endif
#ifndef UINTPTR_MAX
# if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
#  define UINTPTR_MAX UINT32_MAX
# elif defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#  define UINTPTR_MAX UINT64_MAX
# else
#  error "cmrustc requires a known 32-bit or 64-bit pointer width"
# endif
#endif

#define CM_STRINGIZE_INNER(value) #value
#define CM_STRINGIZE(value) CM_STRINGIZE_INNER(value)

#if defined(__TINYC__)
# define CM_COMPILED_WITH_TCC 1
# define CM_BUILD_COMPILER "tinycc (__TINYC__=" CM_STRINGIZE(__TINYC__) ")"
#elif defined(__clang__)
# define CM_COMPILED_WITH_TCC 0
# define CM_BUILD_COMPILER "clang " __clang_version__
#elif defined(__GNUC__)
# define CM_COMPILED_WITH_TCC 0
# define CM_BUILD_COMPILER "gcc " __VERSION__
#else
# define CM_COMPILED_WITH_TCC 0
# define CM_BUILD_COMPILER "unknown-c99"
#endif

#if defined(CM_REQUIRE_TCC) && !defined(__TINYC__)
# error "this build is required to be produced by TinyCC"
#endif

#ifndef CM_VERSION_STRING
# define CM_VERSION_STRING "0.0.0-dev"
#endif

#define CM_COMPILER_IDENTITY \
    "cmrustc " CM_VERSION_STRING "; built with " CM_BUILD_COMPILER

#if defined(__GNUC__) && !defined(__TINYC__)
# define CM_PRINTF_LIKE(format_index, first_argument) \
    __attribute__((format(printf, format_index, first_argument)))
# define CM_NORETURN __attribute__((noreturn))
#else
# define CM_PRINTF_LIKE(format_index, first_argument)
# define CM_NORETURN
#endif

#define CM_ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))
#define CM_STATIC_ASSERT(name, condition) \
    typedef char cm_static_assert_##name[(condition) ? 1 : -1]

CM_STATIC_ASSERT(char_is_eight_bits, CHAR_BIT == 8);
CM_STATIC_ASSERT(uint8_is_one_byte, sizeof(uint8_t) == 1u);
CM_STATIC_ASSERT(uint16_is_two_bytes, sizeof(uint16_t) == 2u);
CM_STATIC_ASSERT(uint32_is_four_bytes, sizeof(uint32_t) == 4u);
CM_STATIC_ASSERT(uint64_is_eight_bytes, sizeof(uint64_t) == 8u);

#endif
