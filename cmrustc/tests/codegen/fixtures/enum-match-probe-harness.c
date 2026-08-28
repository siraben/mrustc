#include <stdint.h>

extern uint32_t probe_point(void);
extern uint32_t probe_square(uint32_t side);
extern uint32_t probe_rect(uint32_t w, uint32_t h);

int main(void)
{
    if (probe_point() != 0u) return 1;
    if (probe_square(7u) != 49u) return 2;
    if (probe_rect(3u, 5u) != 15u) return 3;
    return 0;
}
