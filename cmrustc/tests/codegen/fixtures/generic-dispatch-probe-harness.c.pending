#include <stdint.h>
extern uint32_t probe_u32(uint32_t value);
extern uint32_t probe_u8(uint8_t value);
int main(void)
{
    if (probe_u32(4u) != 8u) return 1;
    if (probe_u8(4u) != 12u) return 2;
    return 0;
}
