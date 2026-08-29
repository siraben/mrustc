#include <stdint.h>

uint32_t static_mut_default(uint32_t x);

int main(void)
{
    /* COUNTER 5 -> 6 -> 8, plus x */
    if (static_mut_default(10) != 18) return 1;
    /* second call keeps mutating: 9 -> 11 */
    if (static_mut_default(1) != 12) return 2;
    return 0;
}
