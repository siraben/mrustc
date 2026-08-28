#include <stdint.h>
extern uint32_t probe_fmt_len(uint32_t value);
int main(void)
{
    if (probe_fmt_len(7u) != 1u) return 1;
    if (probe_fmt_len(1234u) != 4u) return 2;
    return 0;
}
