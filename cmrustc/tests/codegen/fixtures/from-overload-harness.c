#include <stdint.h>

uint32_t from_overload(uint32_t x);

int main(void)
{
    if (from_overload(5) != 5 + 1 + 11 + 1000 + 300) return 1;
    if (from_overload(0) != 12 + 1000 + 300) return 2;
    return 0;
}
