#include <stdint.h>

uint32_t probe_let(uint32_t left, uint32_t right);

static int check(uint32_t left, uint32_t right)
{
    uint32_t expected;

    expected = (uint32_t)((uint32_t)(left + left)
        + (uint32_t)(right + UINT32_C(6)));
    return probe_let(left, right) == expected;
}

int main(void)
{
    if (!check(UINT32_C(0), UINT32_C(0))) return 1;
    if (!check(UINT32_C(7), UINT32_C(11))) return 2;
    if (!check(UINT32_MAX, UINT32_C(0))) return 3;
    if (!check(UINT32_MAX, UINT32_MAX)) return 4;
    if (!check(UINT32_C(0x89abcdef), UINT32_C(0x76543210))) return 5;
    return 0;
}
