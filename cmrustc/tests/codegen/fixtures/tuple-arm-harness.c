#include <stdint.h>

uint32_t tuple_arm(uint32_t x);

int main(void)
{
    /* (1 + 10) + 100 */
    if (tuple_arm(1) != 111) return 1;
    return 0;
}
