#include <stdint.h>

uint32_t body_local_trait(uint32_t start, uint32_t value);

int main(void)
{
    if (body_local_trait(UINT32_C(0), UINT32_C(0)) != UINT32_C(1)) return 1;
    if (body_local_trait(UINT32_C(10), UINT32_C(5)) != UINT32_C(16)) return 2;
    if (body_local_trait(UINT32_C(1000), UINT32_C(24)) != UINT32_C(1025))
        return 3;
    return 0;
}
