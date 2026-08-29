#include <stdint.h>

uint32_t slice_pat_tuple(uint32_t x);

int main(void)
{
    /* x=5: 1 + (2+5)*10 + 3*100 + 3*1000 = 3371 */
    if (slice_pat_tuple(5) != 3371) return 1;
    return 0;
}
