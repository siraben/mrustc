#include <stdint.h>

uint32_t probe_generic_chain(uint32_t value);

int main(void)
{
    static const uint32_t values[] = {
        UINT32_C(0),
        UINT32_C(37),
        UINT32_C(0x89abcdef),
        UINT32_MAX
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(values) / sizeof(values[0])); ++index) {
        if (probe_generic_chain(values[index]) != values[index]) return 1;
    }
    return 0;
}
