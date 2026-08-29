#include <stdint.h>

uint32_t match_literal(uint32_t which);

int main(void)
{
    if (match_literal(0) != 100 + 1 + 20 + 1) return 1;   /* 122 */
    if (match_literal(1) != 200 + 2 + 20 + 0) return 2;   /* 222 */
    if (match_literal(2) != 200 + 3 + 10 + 0) return 3;   /* 213 */
    if (match_literal(16) != 300 + 4 + 10 + 0) return 4;  /* 314 */
    if (match_literal(25) != 400 + 4 + 10 + 26) return 5; /* 440 */
    return 0;
}
