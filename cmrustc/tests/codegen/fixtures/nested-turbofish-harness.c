#include <stdint.h>

uint32_t nested_turbofish(uint32_t x);

int main(void)
{
    if (nested_turbofish(2) != 10) return 1;
    return 0;
}
