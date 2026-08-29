#include <stdint.h>

uint32_t deref_unary(uint32_t x);

int main(void)
{
    /* 3 + (3+5) + 100 */
    if (deref_unary(3) != 111) return 1;
    return 0;
}
