#include <stdint.h>

uint32_t trait_default_self(uint32_t x);

int main(void)
{
    /* Lock { n: 100 }.write(3) + 1 */
    if (trait_default_self(3) != 104) return 1;
    return 0;
}
