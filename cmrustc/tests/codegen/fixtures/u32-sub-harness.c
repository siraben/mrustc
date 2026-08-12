#include <stdint.h>

uint32_t sub(uint32_t end, uint32_t start);

static int check(uint32_t end, uint32_t start, uint32_t expected)
{
    return sub(end, start) == expected;
}

int main(void)
{
    if (!check(UINT32_C(7), UINT32_C(3), UINT32_C(4))) return 1;
    if (!check(UINT32_C(0), UINT32_C(1), UINT32_MAX)) return 2;
    if (!check(UINT32_MAX, UINT32_MAX, UINT32_C(0))) return 3;
    if (!check(UINT32_MAX, UINT32_C(0), UINT32_MAX)) return 4;
    if (!check(UINT32_C(0), UINT32_MAX, UINT32_C(1))) return 5;
    return 0;
}
