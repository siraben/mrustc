#include <stdint.h>

uint32_t tuple_unsize(uint32_t x);

int main(void)
{
    /* 300 + 30*1000 */
    if (tuple_unsize(3) != 30300) return 1;
    return 0;
}
