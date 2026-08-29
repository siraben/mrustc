#include <stdint.h>

uint32_t nested_const_fn(uint32_t x);

int main(void)
{
    /* twice(x) = 2x, plus(x) = x + 1 */
    if (nested_const_fn(5) != 10 + 6) return 1;
    if (nested_const_fn(0) != 1) return 2;
    return 0;
}
