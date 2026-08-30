#include <stdint.h>

uint32_t fnptr_bound(uint32_t x);

int main(void)
{
    /* three() + 1 + x */
    if (fnptr_bound(5) != 9) return 1;
    return 0;
}
