#include <stdint.h>

uint32_t alias_variant(uint32_t x);

int main(void)
{
    if (alias_variant(9) != 10 + 100000) return 1;
    if (alias_variant(1) != 10 + 1000) return 2;
    return 0;
}
