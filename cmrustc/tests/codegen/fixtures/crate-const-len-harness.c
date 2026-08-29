#include <stdint.h>
uint32_t probe_crate_const_len(uint8_t a);
int main(void)
{
    if (probe_crate_const_len(6) != 42) return 1;
    return 0;
}
