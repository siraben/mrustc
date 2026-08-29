#include <stdint.h>

uint32_t derive_probe(uint32_t i);

int main(void)
{
    if (derive_probe(1) != 155) return 1;
    if (derive_probe(3) != 355) return 2;
    return 0;
}
