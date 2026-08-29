#include <stdint.h>

uint32_t early_return(uint32_t x);

int main(void)
{
    if (early_return(0) != 7 + 100 + 90000) return 1;       /* None -> 7, f(0) = 0 */
    if (early_return(5) != 10 + 600 + 100000) return 2;      /* 5 + 5 */
    if (early_return(50) != 90 + 5100 + 1000000) return 3;     /* 50 + 40 */
    if (early_return(500) != 590 + 50100 + 10000000) return 4;   /* 100 + 490 */
    return 0;
}
