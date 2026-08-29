#include <stdint.h>

uint32_t byvalue_first(uint32_t x);

int main(void)
{
    if (byvalue_first(3) != 11) return 1;
    return 0;
}
