#include <stdint.h>

uint32_t param_attr(uint32_t x);

int main(void)
{
    if (param_attr(5) != 15) return 1;
    return 0;
}
