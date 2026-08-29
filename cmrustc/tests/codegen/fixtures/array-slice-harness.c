#include <stdint.h>

uint32_t array_slice(uint32_t k);

int main(void)
{
    /* (6+k) + 3 + 3k + 10 */
    if (array_slice(0) != 19) return 1;
    if (array_slice(5) != 39) return 2;
    return 0;
}
