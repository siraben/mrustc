#include <stdint.h>

uint32_t dyn_forward(uint32_t b);

int main(void)
{
    if (dyn_forward(3) != 207) return 1;   /* len 2, bytes 3 + 4 */
    if (dyn_forward(10) != 221) return 2;  /* len 2, bytes 10 + 11 */
    return 0;
}
