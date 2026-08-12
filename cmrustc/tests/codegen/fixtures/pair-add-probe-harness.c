#include <stdint.h>

uint32_t probe_pair(uint32_t left, uint32_t right);

int main(void)
{
    static const struct {
        uint32_t left;
        uint32_t right;
        uint32_t expected;
    } cases[] = {
        { UINT32_C(0), UINT32_C(0), UINT32_C(3) },
        { UINT32_C(10), UINT32_C(20), UINT32_C(33) },
        { UINT32_C(0x10203040), UINT32_C(0x01020304),
            UINT32_C(0x11223347) },
        { UINT32_MAX, UINT32_C(0), UINT32_C(2) },
        { UINT32_MAX, UINT32_C(1), UINT32_C(3) },
        { UINT32_C(0x80000000), UINT32_C(0x80000000), UINT32_C(3) },
        { UINT32_C(0xffffff00), UINT32_C(0x000001ff), UINT32_C(0x102) }
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(cases) / sizeof(cases[0])); ++index) {
        if (probe_pair(cases[index].left, cases[index].right)
                != cases[index].expected) {
            return 1;
        }
    }
    return 0;
}
