#include <stdint.h>
extern uint32_t probe_sum(uint32_t n);
extern uint32_t probe_count(uint32_t start);
int main(void)
{
    if (probe_sum(0u) != 0u) return 1;
    if (probe_sum(5u) != 10u) return 2;
    if (probe_count(7u) != 7u) return 3;
    return 0;
}
