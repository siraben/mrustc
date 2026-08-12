#include <stdint.h>

uint32_t add(uint32_t left, uint32_t right);

int main(void)
{
    static const struct {
        uint32_t left;
        uint32_t right;
        uint32_t expected;
    } cases[] = {
        { UINT32_C(0), UINT32_C(0), UINT32_C(0) },
        { UINT32_C(1), UINT32_C(2), UINT32_C(3) },
        { UINT32_C(0x10203040), UINT32_C(0x01020304),
            UINT32_C(0x11223344) },
        { UINT32_C(0x7fffffff), UINT32_C(1), UINT32_C(0x80000000) },
        { UINT32_MAX, UINT32_C(1), UINT32_C(0) },
        { UINT32_MAX, UINT32_MAX, UINT32_MAX - UINT32_C(1) },
        { UINT32_C(0xf0000000), UINT32_C(0x20000001),
            UINT32_C(0x10000001) }
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(cases) / sizeof(cases[0])); ++index) {
        if (add(cases[index].left, cases[index].right)
                != cases[index].expected) {
            return 1;
        }
    }
    return 0;
}
