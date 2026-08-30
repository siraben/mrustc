#include <stdint.h>

uint32_t pattern_generic(uint32_t);

int main(void)
{
    return pattern_generic(20) == 41 ? 0 : 1;
}
