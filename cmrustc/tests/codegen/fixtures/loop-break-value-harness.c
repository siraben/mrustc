#include <stdint.h>

uint32_t loop_break_value(uint32_t n);

int main(void)
{
    if (loop_break_value(1) != 411) return 1;
    if (loop_break_value(9) != 916) return 2;
    return 0;
}
