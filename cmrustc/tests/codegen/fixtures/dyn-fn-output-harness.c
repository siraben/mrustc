#include <stdint.h>

uint32_t dyn_fn_output(uint32_t x);

int main(void)
{
    /* y = 6, f(6) = 12 -> r = 18; total = 3 + 6 = 9 */
    if (dyn_fn_output(3) != 27) return 1;
    return 0;
}
