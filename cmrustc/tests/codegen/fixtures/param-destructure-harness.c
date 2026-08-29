#include <stdint.h>

uint32_t param_destructure(uint32_t x, uint32_t y);

int main(void)
{
    /* (3+4) + (30+4) */
    if (param_destructure(3, 4) != 41) return 1;
    return 0;
}
