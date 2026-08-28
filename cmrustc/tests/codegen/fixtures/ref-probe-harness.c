#include <stdint.h>
extern uint32_t probe_ref(uint32_t start, uint32_t by);
int main(void)
{
    if (probe_ref(1u, 2u) != 5u) return 1;
    if (probe_ref(10u, 0u) != 10u) return 2;
    return 0;
}
