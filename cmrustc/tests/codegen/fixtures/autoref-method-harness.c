#include <stdint.h>

uint32_t shared_autoref(uint32_t input, uint32_t other);
uint32_t mutable_autoref(uint32_t input, uint32_t other);

int main(void)
{
    static const struct {
        uint32_t input;
        uint32_t other;
        uint32_t expected;
    } cases[] = {
        { UINT32_C(0), UINT32_C(7), UINT32_C(8) },
        { UINT32_C(1), UINT32_C(28), UINT32_C(29) },
        { UINT32_C(0x10203040), UINT32_C(99), UINT32_C(100) }
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(cases) / sizeof(cases[0])); ++index) {
        if (shared_autoref(cases[index].input, cases[index].other)
                != cases[index].expected
            || mutable_autoref(cases[index].input, cases[index].other)
                != cases[index].expected) {
            return 1;
        }
    }
    return 0;
}
