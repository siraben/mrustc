#include <stdint.h>

uint32_t local_impl(uint32_t x);

int main(void)
{
    /* 7: steps 2,2,2,1 = 4 steps; written 7 */
    if (local_impl(7) != 47) return 1;
    return 0;
}
