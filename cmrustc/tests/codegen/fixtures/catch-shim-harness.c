#include <stdint.h>

uint32_t catch_shim(uint32_t x);

int main(void)
{
    if (catch_shim(3) != 8) return 1;
    return 0;
}
