#include <stdint.h>

uint32_t trait_args(uint32_t i);

int main(void)
{
    if (trait_args(0) != 1021) return 1;   /* 10 + 1001 + 10 */
    if (trait_args(2) != 1043) return 2;   /* 30 + 1003 + 10 */
    return 0;
}
