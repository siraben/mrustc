#include <stdint.h>

uint32_t qualified_const(uint32_t x);

int main(void)
{
    /* Small: LIMIT 100; Large: LIMIT 210; shifted 50 */
    if (qualified_const(5) != 5 + 5000 + 50) return 1;
    if (qualified_const(150) != 100 + 150000 + 50) return 2;
    if (qualified_const(300) != 100 + 210000 + 50) return 3;
    return 0;
}
