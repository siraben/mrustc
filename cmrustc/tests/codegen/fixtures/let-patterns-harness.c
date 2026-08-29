#include <stdint.h>

uint32_t let_patterns(uint32_t n);

int main(void)
{
    /* (n+1) + 100 + (n + ... + 1) + 30 + n + 5 */
    if (let_patterns(0) != 136 + 700 + 1000) return 1;
    if (let_patterns(3) != 148 + 700 + 4000) return 2;
    return 0;
}
