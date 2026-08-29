#include <stdint.h>

uint32_t autoref_step(uint32_t x);

int main(void)
{
    /* x=4: a=b=5, c=d=6 -> 5 + 500 + 120000 */
    if (autoref_step(4) != 120505) return 1;
    if (autoref_step(0) != 1 + 100 + 40000) return 2;
    return 0;
}
