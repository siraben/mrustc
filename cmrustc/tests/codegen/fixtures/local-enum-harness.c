#include <stdint.h>

uint8_t local_enum(uint8_t x);

int main(void)
{
    if (local_enum(0) != 0) return 1;
    if (local_enum(1) != 11) return 2;
    if (local_enum(5) != 29) return 3;
    return 0;
}
