#include <stdint.h>
uint32_t probe_radix_len(uint8_t x);
int main(void)
{
    /* five arrays, last element of each = 3 */
    if (probe_radix_len(3) != 15) return 1;
    return 0;
}
