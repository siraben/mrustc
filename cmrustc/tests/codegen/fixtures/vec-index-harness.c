#include <stdint.h>

uint32_t vec_index(uint32_t x);

int main(void)
{
    /* 2*100 + 5 + 4*1000 */
    if (vec_index(3) != 4205) return 1;
    return 0;
}
