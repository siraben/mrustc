#include <stdint.h>
extern uint32_t probe_default(uint32_t value);
int main(void)
{
    if (probe_default(21u) != 42u) return 1;
    if (probe_default(0u) != 0u) return 2;
    return 0;
}
