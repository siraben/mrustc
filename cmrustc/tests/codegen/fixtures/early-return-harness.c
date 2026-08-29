#include <stdint.h>

uint32_t early_return(uint32_t x);

int main(void)
{
    if (early_return(0) != 7) return 1;       /* None -> 7, f(0) = 0 */
    if (early_return(5) != 10) return 2;      /* 5 + 5 */
    if (early_return(50) != 90) return 3;     /* 50 + 40 */
    if (early_return(500) != 590) return 4;   /* 100 + 490 */
    return 0;
}
