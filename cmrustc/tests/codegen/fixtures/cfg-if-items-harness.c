#include <stdint.h>

uint32_t cfg_if_items(uint32_t x);

int main(void)
{
    /* linux branch: BASE 100, PRELUDE 7 */
    if (cfg_if_items(1) != 108) return 1;
    return 0;
}
