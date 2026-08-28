#include <stdint.h>

uint32_t dyn_area(uint32_t sel, uint32_t k);

int main(void)
{
    if (dyn_area(0, 1) != 18) return 1;   /* 9*1 + 9 */
    if (dyn_area(0, 4) != 45) return 2;   /* 9*4 + 9 */
    if (dyn_area(1, 1) != 21) return 3;   /* 10*1+1 + 10 */
    if (dyn_area(1, 3) != 41) return 4;   /* 10*3+1 + 10 */
    return 0;
}
