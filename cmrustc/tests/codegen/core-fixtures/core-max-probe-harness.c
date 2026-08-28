#include <stdint.h>
extern uint32_t probe_core_max(uint32_t a, uint32_t b);
extern uint32_t probe_core_option(uint32_t value);
int main(void)
{
    if (probe_core_max(3u, 9u) != 9u) return 1;
    if (probe_core_max(12u, 5u) != 12u) return 2;
    if (probe_core_option(41u) != 42u) return 3;
    return 0;
}
