#include <stdint.h>
uint32_t probe_assoc_const_len(uint32_t a);
int main(void)
{
    if (probe_assoc_const_len(32) != 42) return 1;
    return 0;
}
