#include <stdint.h>

uint32_t match_literal(uint32_t which);

int main(void)
{
    if (match_literal(0) != 100 + 1 + 20 + 1 + 10000 + 0 + 400) return 1;   /* 122 */
    if (match_literal(1) != 200 + 2 + 20 + 0 + 11000 + 100000 + 400) return 2;   /* 222 */
    if (match_literal(2) != 200 + 3 + 10 + 0 + 12000 + 200000 + 400) return 3;   /* 213 */
    if (match_literal(16) != 300 + 4 + 10 + 0 + 200000 + 600000 + 400) return 4;  /* 314 */
    if (match_literal(25) != 400 + 4 + 10 + 26 + 200000 + 500000 + 400) return 5; /* 440 */
    return 0;
}
