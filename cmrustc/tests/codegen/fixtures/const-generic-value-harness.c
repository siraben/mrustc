#include <stdint.h>

uint32_t const_generic_value_probe(void);

int main(void)
{
    return const_generic_value_probe() == 34u ? 0 : 1;
}
