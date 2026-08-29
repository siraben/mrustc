#include <stdint.h>

uint32_t assoc_const(uint32_t x);

int main(void)
{
    if (assoc_const(255) != (3 + 10) * 100 + (2 + 16)) return 1;  /* 1318 */
    if (assoc_const(7) != (1 + 10) * 100 + (1 + 16)) return 2;    /* 1117 */
    if (assoc_const(0) != (0 + 10) * 100 + (0 + 16)) return 3;    /* 1016 */
    return 0;
}
