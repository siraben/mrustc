#include <stdint.h>

uint32_t deref_method(uint32_t x);

int main(void)
{
    /* x=3: bump -> a=4, total 6: 600 + 6 + 70000 */
    if (deref_method(3) != 600 + 6 + 70000) return 1;
    if (deref_method(0) != 300 + 3 + 70000) return 2;
    return 0;
}
