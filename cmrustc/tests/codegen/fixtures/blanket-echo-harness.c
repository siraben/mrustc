#include <stdint.h>

extern uint32_t echo_qualified_u32(uint32_t receiver, uint32_t value);
extern uint8_t echo_dot_u8(uint8_t receiver, uint8_t value);

int main(void)
{
    if (echo_qualified_u32(UINT32_C(1), UINT32_C(0x89abcdef))
            != UINT32_C(0x89abcdef)) {
        return 1;
    }
    if (echo_qualified_u32(UINT32_C(0xffffffff), UINT32_C(7))
            != UINT32_C(7)) {
        return 2;
    }
    if (echo_dot_u8(UINT8_C(1), UINT8_C(0xa5)) != UINT8_C(0xa5)) {
        return 3;
    }
    if (echo_dot_u8(UINT8_C(0xff), UINT8_C(0)) != UINT8_C(0)) {
        return 4;
    }
    return 0;
}
