#include <stdint.h>

uint32_t dyn_fn_output(uint32_t x);

int main(void)
{
    /* y = 6, f(6) = 12 -> r = 18; total = 3 + 6 = 9; seen = 3 + 4 = 7 */
    if (dyn_fn_output(3) != 34) return 1;
    return 0;
}
