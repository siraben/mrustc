#include <stdint.h>
long long host_add(long long a, long long b) { return a + b; }
uint32_t probe_extern_block_macro(uint32_t a);
int main(void)
{
    if (probe_extern_block_macro(40) != 42) return 1;
    return 0;
}
