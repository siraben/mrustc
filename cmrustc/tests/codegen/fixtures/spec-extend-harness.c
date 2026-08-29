#include <stdint.h>

uint32_t spec_extend(uint32_t x);

int main(void)
{
    /* x=1: total 6, s=100, calls 101 */
    if (spec_extend(1) != 6 + 100000 + 10100000) return 1;
    if (spec_extend(10) != 15 + 100000 + 10100000) return 2;
    return 0;
}
