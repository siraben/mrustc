#include <stdint.h>

uint32_t let_chain(uint32_t x);

int main(void)
{
    /* (7 + 1) + 100 + 100 + (3 + 2 + 1) */
    if (let_chain(1) != 214) return 1;
    return 0;
}
