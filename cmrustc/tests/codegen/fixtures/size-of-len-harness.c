#include <stdint.h>
uint32_t probe_size_of_len(uint8_t a);
int main(void)
{
    /* slots[7] = 8, + 34 */
    if (probe_size_of_len(8) != 42) return 1;
    return 0;
}
