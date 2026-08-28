#include <stdint.h>
extern uint32_t probe_guard(uint32_t value, uint32_t limit);
extern uint32_t probe_or(uint32_t value);
int main(void)
{
    if (probe_guard(3u, 10u) != 3u) return 1;
    if (probe_guard(30u, 10u) != 130u) return 2;
    if (probe_or(5u) != 14u) return 3;
    return 0;
}
