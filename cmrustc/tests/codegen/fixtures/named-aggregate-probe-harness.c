#include <stdint.h>

uint32_t probe_aggregate(uint32_t x);

static int check(uint32_t x)
{
    return probe_aggregate(x) == (uint32_t)(x + UINT32_C(2));
}

int main(void)
{
    if (!check(UINT32_C(0))) return 1;
    if (!check(UINT32_C(7))) return 2;
    if (!check(UINT32_MAX)) return 3;
    if (!check(UINT32_MAX - UINT32_C(1))) return 4;
    if (!check(UINT32_C(0x89abcdef))) return 5;
    return 0;
}
