#include <stdint.h>

uint32_t refmut_scope_drop_probe(void);

int main(void)
{
    return refmut_scope_drop_probe() == 10u ? 0 : 1;
}
