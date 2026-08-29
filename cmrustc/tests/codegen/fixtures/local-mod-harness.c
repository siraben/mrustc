#include <stdint.h>

uint8_t local_mod(uint8_t x);

int main(void)
{
    if (local_mod(0) != 10) return 1;
    if (local_mod(1) != 22) return 2;
    if (local_mod(7) != 32) return 3;
    return 0;
}
