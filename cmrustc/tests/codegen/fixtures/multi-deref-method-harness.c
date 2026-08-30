#include <stdint.h>

uint32_t multi_deref_method_probe(void);

int main(void)
{
    return multi_deref_method_probe() == 73u ? 0 : 1;
}
