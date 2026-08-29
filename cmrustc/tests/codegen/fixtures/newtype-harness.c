#include <stdint.h>

uint32_t newtype(uint32_t x);

int main(void)
{
    if (newtype(1) != 4) return 1;    /* (1+1) + (1+1) */
    if (newtype(20) != 42) return 2;
    return 0;
}
