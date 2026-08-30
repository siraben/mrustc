#include <stdint.h>

uint32_t match_ref(uint32_t x);

int main(void)
{
    if (match_ref(1) != 9) return 1;
    return 0;
}
