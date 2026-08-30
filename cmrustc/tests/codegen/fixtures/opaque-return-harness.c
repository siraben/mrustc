#include <stdint.h>

uint32_t opaque_return(uint32_t x);

int main(void)
{
    if (opaque_return(10) != 12) return 1;
    if (opaque_return(3) != 3) return 1;
    return 0;
}
