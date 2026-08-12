#include <stdint.h>

uint32_t select(uint32_t left, uint32_t right);

static int check(uint32_t left, uint32_t right, uint32_t expected)
{
    return select(left, right) == expected;
}

int main(void)
{
    if (!check(UINT32_C(7), UINT32_C(7), UINT32_C(8))) return 1;
    if (!check(UINT32_MAX, UINT32_MAX, UINT32_C(0))) return 2;
    if (!check(UINT32_C(7), UINT32_C(3), UINT32_C(4))) return 3;
    if (!check(UINT32_C(0), UINT32_C(1), UINT32_MAX)) return 4;
    if (!check(UINT32_C(0), UINT32_MAX, UINT32_C(1))) return 5;
    if (!check(UINT32_MAX, UINT32_C(0), UINT32_MAX)) return 6;
    return 0;
}
