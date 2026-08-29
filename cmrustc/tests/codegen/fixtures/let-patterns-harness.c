#include <stdint.h>

uint32_t let_patterns(uint32_t n);

int main(void)
{
    /* (n+1) + 100 + (n + ... + 1) + 30 + n + 5 */
    if (let_patterns(0) != 136) return 1;
    if (let_patterns(3) != 148) return 2;
    return 0;
}
