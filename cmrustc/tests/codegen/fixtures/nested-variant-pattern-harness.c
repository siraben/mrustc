#include <stdint.h>

uint32_t nested_variant_pattern_probe(uint32_t which);

int main(void)
{
    if (nested_variant_pattern_probe(0u) != 7u) return 1;
    if (nested_variant_pattern_probe(1u) != 38u) return 2;
    return 0;
}
