#include <stdint.h>

uint32_t maybe_uninit(uint8_t x);

int main(void)
{
    if (maybe_uninit(3) != 395) return 1;
    if (maybe_uninit(7) != 795) return 2;
    return 0;
}
