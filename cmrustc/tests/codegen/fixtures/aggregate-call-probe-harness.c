#include <stdint.h>

uint32_t probe_aggregate_call(uint32_t x);

int main(void)
{
    static const struct {
        uint32_t input;
        uint32_t expected;
    } cases[] = {
        { UINT32_C(0), UINT32_C(4) },
        { UINT32_C(1), UINT32_C(5) },
        { UINT32_C(42), UINT32_C(46) },
        { UINT32_MAX - UINT32_C(3), UINT32_C(0) },
        { UINT32_MAX, UINT32_C(3) }
    };
    unsigned int index;

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        if (probe_aggregate_call(cases[index].input)
                != cases[index].expected) {
            return 1;
        }
    }
    return 0;
}
