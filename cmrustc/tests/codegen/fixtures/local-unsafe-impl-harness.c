#include <stdint.h>
uint32_t probe_local_unsafe_impl(uint32_t a);
int main(void)
{
    if (probe_local_unsafe_impl(41) != 42) return 1;
    return 0;
}
