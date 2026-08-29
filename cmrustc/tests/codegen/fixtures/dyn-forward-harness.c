#include <stdint.h>

uint32_t dyn_forward(uint32_t b);

int main(void)
{
    if (dyn_forward(3) != 315) return 1;   /* len 3, bytes 3 + 4 + 8 */
    if (dyn_forward(10) != 336) return 2;  /* len 3, bytes 10 + 11 + 15 */
    return 0;
}
