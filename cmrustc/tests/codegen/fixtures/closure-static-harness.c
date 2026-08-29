#include <stdint.h>

uint32_t closure_static(uint32_t x);

int main(void)
{
    if (closure_static(3) != 43) return 1;
    return 0;
}
