#include <stdint.h>
extern uint32_t probe_closure(uint32_t base, uint32_t value);
extern uint32_t probe_direct(uint32_t value);
int main(void)
{
    if (probe_closure(3u, 4u) != 10u) return 1;
    if (probe_direct(21u) != 42u) return 2;
    return 0;
}
