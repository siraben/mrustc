#include <stdint.h>

uint32_t extern_forward(uint32_t k);
uint32_t host_add(uint32_t a, uint32_t b) { return a + b; }
uint32_t host_scale(uint32_t v) { return v * 3; }

int main(void)
{
    if (extern_forward(2) != 7 + 600) return 1;
    if (extern_forward(10) != 15 + 3000) return 2;
    return 0;
}
