#include <stdint.h>

uint32_t probe_chain(uint32_t left, uint32_t right);
uint32_t probe_after_call(uint32_t left, uint32_t right);

int main(void)
{
    static const struct {
        uint32_t left;
        uint32_t right;
        uint32_t expected;
    } cases[] = {
        { UINT32_C(0), UINT32_C(0), UINT32_C(6) },
        { UINT32_C(10), UINT32_C(20), UINT32_C(46) },
        { UINT32_C(0x10203040), UINT32_C(0x01020304),
            UINT32_C(0x2142638a) },
        { UINT32_MAX, UINT32_C(0), UINT32_C(4) },
        { UINT32_MAX, UINT32_C(1), UINT32_C(5) },
        { UINT32_C(0x80000000), UINT32_C(0), UINT32_C(6) },
        { UINT32_C(0x80000000), UINT32_C(0x80000000),
            UINT32_C(0x80000006) },
        { UINT32_C(0xffffff00), UINT32_C(0x000001ff), UINT32_C(5) }
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(cases) / sizeof(cases[0])); ++index) {
        if (probe_chain(cases[index].left, cases[index].right)
                != cases[index].expected
            || probe_after_call(cases[index].left, cases[index].right)
                != cases[index].expected) {
            return 1;
        }
    }
    return 0;
}
