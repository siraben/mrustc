#include <stdint.h>

uint32_t index_place(uint32_t k);

int main(void)
{
    if (index_place(3) != 3 + 70 + 100 + 4000) return 1;
    if (index_place(9) != 9 + 70 + 100 + 10000) return 2;
    return 0;
}
