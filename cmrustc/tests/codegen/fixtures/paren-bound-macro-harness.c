#include <stdint.h>
uint32_t probe_paren_bound_macro(uint32_t a);
int main(void)
{
    /* run(&Box7) = 7, bump!(34) = 35 */
    if (probe_paren_bound_macro(34) != 42) return 1;
    return 0;
}
