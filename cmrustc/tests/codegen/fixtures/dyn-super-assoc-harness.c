#include <stdint.h>
uint32_t probe_dyn_super_assoc(uint32_t a);
int main(void)
{
    /* produce(10) = 12, twice(10) = 20 */
    if (probe_dyn_super_assoc(10) != 32) return 1;
    return 0;
}
