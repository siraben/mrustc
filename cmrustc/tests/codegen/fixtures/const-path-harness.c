#include <stdint.h>

uint32_t const_sum(uint32_t x);

int main(void)
{
    if (const_sum(0) != 27) return 1;
    if (const_sum(10) != 57) return 2;
    return 0;
}
