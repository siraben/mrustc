#include <stdint.h>

uint8_t supertrait_output(uint32_t x);

int main(void)
{
    /* 1234 % 10 = 4, plus the decoy 200 */
    if (supertrait_output(1234) != 204) return 1;
    return 0;
}
