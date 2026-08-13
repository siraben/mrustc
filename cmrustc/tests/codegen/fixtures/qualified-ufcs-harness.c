#include <stdint.h>

uint32_t qualified_value(uint32_t input);

int main(void)
{
    static const struct {
        uint32_t input;
        uint32_t expected;
    } cases[] = {
        { UINT32_C(0), UINT32_C(1) },
        { UINT32_C(1), UINT32_C(2) },
        { UINT32_C(0x10203040), UINT32_C(0x10203041) },
        { UINT32_MAX, UINT32_C(0) }
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(cases) / sizeof(cases[0])); ++index) {
        if (qualified_value(cases[index].input) != cases[index].expected) {
            return 1;
        }
    }
    return 0;
}
