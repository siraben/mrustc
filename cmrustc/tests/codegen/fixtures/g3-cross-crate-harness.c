#include <stdint.h>

uint32_t consumer_probe(uint32_t value);
uint32_t object_probe(uint32_t value);

int main(void)
{
    static const uint32_t identity_values[] = {
        UINT32_C(0),
        UINT32_C(37),
        UINT32_C(0x89abcdef),
        UINT32_MAX
    };
    static const uint32_t object_values[] = {
        UINT32_C(0),
        UINT32_C(41),
        UINT32_MAX
    };
    static const uint32_t object_results[] = {
        UINT32_C(1),
        UINT32_C(42),
        UINT32_C(0)
    };
    unsigned int index;

    for (index = 0u; index <
            (unsigned int)(sizeof(identity_values)
                / sizeof(identity_values[0])); ++index) {
        if (consumer_probe(identity_values[index])
                != identity_values[index]) {
            return 1;
        }
    }
    for (index = 0u; index <
            (unsigned int)(sizeof(object_values)
                / sizeof(object_values[0])); ++index) {
        if (object_probe(object_values[index]) != object_results[index]) {
            return 2;
        }
    }
    return 0;
}
