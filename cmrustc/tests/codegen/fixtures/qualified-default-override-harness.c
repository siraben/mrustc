#include <stdint.h>

uint32_t qualified_override_u32(uint32_t input);
uint32_t qualified_inherited_usize(uint32_t input);

int main(void)
{
    if (qualified_override_u32(UINT32_C(40)) != UINT32_C(42)
        || qualified_inherited_usize(UINT32_C(40)) != UINT32_C(41)) {
        return 1;
    }
    return 0;
}
