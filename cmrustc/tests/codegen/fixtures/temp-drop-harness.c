#include <stdint.h>

uint32_t temp_drop(uint32_t x);

int main(void)
{
    /* 103 + 104 + 0 borrows + 2 drops */
    if (temp_drop(3) != 20207) return 1;
    return 0;
}
