#include <stdint.h>

uint32_t nested_add(uint32_t left, uint32_t right);

int main(void)
{
    static const struct {
        uint32_t left;
        uint32_t right;
        uint32_t expected;
    } cases[] = {
        { UINT32_C(0), UINT32_C(0), UINT32_C(1) },
        { UINT32_C(10), UINT32_C(20), UINT32_C(31) },
        { UINT32_C(0x10203040), UINT32_C(0x01020304),
            UINT32_C(0x11223345) },
        { UINT32_MAX, UINT32_C(0), UINT32_C(0) },
        { UINT32_MAX, UINT32_C(1), UINT32_C(1) },
        { UINT32_C(0x80000000), UINT32_C(0x80000000), UINT32_C(1) },
        { UINT32_C(0xffffff00), UINT32_C(0x000001ff), UINT32_C(0x100) }
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(cases) / sizeof(cases[0])); ++index) {
        if (nested_add(cases[index].left, cases[index].right)
                != cases[index].expected) {
            return 1;
        }
    }
    return 0;
}
