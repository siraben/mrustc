#include <stdint.h>

uint32_t newtype_array_param_probe(void);

int main(void)
{
    return newtype_array_param_probe() == 42u ? 0 : 1;
}
