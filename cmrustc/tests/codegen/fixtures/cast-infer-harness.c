#include <stdint.h>

uint32_t cast_infer(uint32_t x);

int main(void)
{
    if (cast_infer(1) != 12) return 1;
    if (cast_infer(10) != 30) return 2;
    return 0;
}
