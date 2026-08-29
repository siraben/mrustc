#include <stdint.h>

uint32_t op_traits(uint32_t x);

int main(void)
{
    if (op_traits(5) != 1146) return 1;   /* 40+6 + 100 + 1000 */
    if (op_traits(4) != 1146) return 2;
    return 0;
}
