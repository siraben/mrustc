#include <stdint.h>

uint32_t block_use_macro(void);

int main(void)
{
    if (block_use_macro() != 10) return 1;
    return 0;
}
