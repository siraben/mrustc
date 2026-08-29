#include <stdint.h>

uint32_t box_unsize(uint32_t x);

int main(void)
{
    /* 3 + 7 */
    if (box_unsize(3) != 10) return 1;
    return 0;
}
