#include <stdint.h>

uint32_t never_turbofish(uint32_t x);

int main(void)
{
    if (never_turbofish(3) != 4) return 1;
    return 0;
}
