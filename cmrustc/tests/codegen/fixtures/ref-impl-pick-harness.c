#include <stdint.h>

uint32_t ref_impl_pick(uint32_t x);

int main(void)
{
    /* 100 + 3 */
    if (ref_impl_pick(3) != 103) return 1;
    return 0;
}
