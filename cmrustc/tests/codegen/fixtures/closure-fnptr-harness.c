#include <stdint.h>

uint32_t closure_fnptr(uint32_t x);

int main(void)
{
    /* 9 + 13 + 4 + 6 */
    if (closure_fnptr(3) != 32) return 1;
    return 0;
}
