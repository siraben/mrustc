#include <stdint.h>

uint32_t dot_value(uint32_t input, uint32_t other);

int main(void)
{
    static const struct {
        uint32_t input;
        uint32_t other;
        uint32_t expected;
    } cases[] = {
        { UINT32_C(0), UINT32_C(7), UINT32_C(7) },
        { UINT32_C(1), UINT32_C(29), UINT32_C(29) },
        { UINT32_C(0x10203040), UINT32_C(0xa0b0c0d0),
            UINT32_C(0xa0b0c0d0) },
        { UINT32_MAX, UINT32_C(0), UINT32_C(0) }
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(cases) / sizeof(cases[0])); ++index) {
        if (dot_value(cases[index].input, cases[index].other)
                != cases[index].expected) {
            return 1;
        }
    }
    return 0;
}
