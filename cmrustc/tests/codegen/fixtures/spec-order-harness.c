#include <stdint.h>

uint32_t spec_order(void);

int main(void)
{
    /* blanket default lower_bound 1, specialized upper_bound 0 */
    if (spec_order() != 10) return 1;
    return 0;
}
