#include <stdint.h>

uint32_t impl_bind(uint32_t k);

int main(void)
{
    if (impl_bind(0) != 10) return 1;
    if (impl_bind(1) != 20) return 2;
    if (impl_bind(4) != 50) return 3;
    return 0;
}
