#include <stdint.h>

uint32_t into_probe(uint32_t i);

int main(void)
{
    if (into_probe(3) != 3) return 1;
    if (into_probe(9) != 77) return 2;
    return 0;
}
