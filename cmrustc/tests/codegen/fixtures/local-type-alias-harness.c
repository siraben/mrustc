#include <stdint.h>

uint32_t local_type_alias(uint32_t x);

int main(void)
{
    /* 3 + 4 + 103 */
    if (local_type_alias(3) != 110) return 1;
    return 0;
}
