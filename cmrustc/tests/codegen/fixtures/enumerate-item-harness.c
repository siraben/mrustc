#include <stdint.h>

uint32_t enumerate(uint32_t v);

int main(void)
{
    if (enumerate(5) != 11) return 1;   /* (5+0) + (5+1) */
    if (enumerate(0) != 1) return 2;
    return 0;
}
