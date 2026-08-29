#include <stdint.h>

uint32_t range_index(uint32_t x);

int main(void)
{
    /* 3 + 5 + 99 + (2*10 + 4) + 2 */
    if (range_index(3) != 133) return 1;
    return 0;
}
