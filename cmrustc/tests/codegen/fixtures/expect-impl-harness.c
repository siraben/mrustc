#include <stdint.h>

uint32_t expect_impl(uint32_t x);

int main(void)
{
    /* 2x + 2x */
    if (expect_impl(5) != 20) return 1;
    return 0;
}
