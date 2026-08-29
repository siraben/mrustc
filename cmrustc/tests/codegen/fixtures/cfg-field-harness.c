#include <stdint.h>

uint32_t cfg_field(uint32_t x);

int main(void)
{
    if (cfg_field(0) != 1) return 1;
    if (cfg_field(4) != 45) return 2;
    return 0;
}
