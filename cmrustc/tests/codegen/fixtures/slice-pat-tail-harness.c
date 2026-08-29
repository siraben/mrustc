#include <stdint.h>

uint32_t slice_pat_tail(uint32_t x);

int main(void)
{
    /* 5 + 100 + (30+5) + 8000 + 7 + 4 + 200 */
    if (slice_pat_tail(3) != 8351) return 1;
    return 0;
}
