#include <stdint.h>

uint32_t reborrow(uint32_t v);

int main(void)
{
    if (reborrow(0) != 6) return 1;     /* 0 + 0 + (1+2+3) */
    if (reborrow(10) != 56) return 2;   /* 10 + 10 + (11+12+13) */
    return 0;
}
