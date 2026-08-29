#include <stdint.h>

uintptr_t str_len(uint32_t sel);
uint8_t str_byte(uint32_t sel, uintptr_t i);
uint32_t ctor_call(uint32_t v);

int main(void)
{
    if (str_len(0) != 5) return 1;
    if (str_len(1) != 6) return 2;
    if (str_byte(0, 1) != 'e') return 3;
    if (str_byte(1, 1) != '\t') return 4;
    if (str_byte(1, 4) != 0xC3 || str_byte(1, 5) != 0xA9) return 5;
    if (ctor_call(35) != 42 + 1000 + 100 + 30000) return 6;
    return 0;
}
