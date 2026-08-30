#include <stdint.h>

uint32_t param_default(uint32_t x);

int main(void)
{
    if (param_default(2) != 42) return 1;
    return 0;
}
