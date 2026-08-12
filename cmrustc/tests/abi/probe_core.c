#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct ProbePair {
    uint32_t first;
    uint32_t second;
} ProbePair;

typedef struct ProbeWide {
    uint64_t first;
    uint64_t second;
    uint64_t third;
} ProbeWide;

typedef struct ProbeAlign64 {
    unsigned char prefix;
    uint64_t value;
} ProbeAlign64;

static jmp_buf probe_jump;

static ProbePair probe_pair_make(uint32_t first, uint32_t second)
{
    ProbePair result;

    result.first = first;
    result.second = second;
    return result;
}

static uint32_t probe_pair_sum(ProbePair value)
{
    return value.first + value.second;
}

static ProbeWide probe_wide_rotate(ProbeWide value)
{
    ProbeWide result;

    result.first = value.second;
    result.second = value.third;
    result.third = value.first;
    return result;
}

static uint64_t probe_wide_sum(ProbeWide value)
{
    return value.first + value.second + value.third;
}

static uint64_t probe_varargs(unsigned int count, ...)
{
    va_list args;
    uint64_t sum;
    unsigned int index;

    sum = 0;
    va_start(args, count);
    for (index = 0; index < count; index += 1) {
        sum += va_arg(args, uint64_t);
    }
    va_end(args);
    return sum;
}

static void probe_jump_now(void)
{
    longjmp(probe_jump, 37);
}

int main(void)
{
    ProbePair pair;
    ProbeWide wide;
    uint64_t dividend;
    uint64_t quotient;
    int jump_value;

    if (sizeof(uint8_t) != 1u
        || sizeof(uint16_t) != 2u
        || sizeof(uint32_t) != 4u
        || sizeof(uint64_t) != 8u
        || sizeof(void *) != sizeof(uintptr_t)) {
        return 1;
    }
    if (offsetof(ProbeAlign64, value) == 0u
        || offsetof(ProbeAlign64, value) % sizeof(uint32_t) != 0u) {
        return 2;
    }

    pair = probe_pair_make(UINT32_C(0x10203040), UINT32_C(0x01020304));
    if (probe_pair_sum(pair) != UINT32_C(0x11223344)) {
        return 3;
    }

    wide.first = UINT64_C(0x0102030405060708);
    wide.second = UINT64_C(0x1112131415161718);
    wide.third = UINT64_C(0x2122232425262728);
    wide = probe_wide_rotate(wide);
    if (wide.first != UINT64_C(0x1112131415161718)
        || wide.second != UINT64_C(0x2122232425262728)
        || wide.third != UINT64_C(0x0102030405060708)
        || probe_wide_sum(wide) != UINT64_C(0x3336393c3f424548)) {
        return 4;
    }

    if (probe_varargs(
        3,
        UINT64_C(0x100000002),
        UINT64_C(0x300000004),
        UINT64_C(0x500000006)
    ) != UINT64_C(0x90000000c)) {
        return 5;
    }

    dividend = UINT64_C(0xfedcba9876543210);
    quotient = dividend / UINT64_C(0x10203);
    if (quotient != UINT64_C(0xfce003f0896f)
        || quotient * UINT64_C(0x10203)
            + dividend % UINT64_C(0x10203) != dividend) {
        return 6;
    }

    jump_value = setjmp(probe_jump);
    if (jump_value == 0) {
        probe_jump_now();
        return 7;
    }
    if (jump_value != 37) {
        return 8;
    }

    printf(
        "core pass pointer=%lu align64=%lu\n",
        (unsigned long)(sizeof(void *) * 8u),
        (unsigned long)offsetof(ProbeAlign64, value)
    );
    return 0;
}
