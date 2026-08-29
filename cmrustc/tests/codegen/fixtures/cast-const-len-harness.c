#include <stdint.h>
uint64_t probe_cast_const_len(uint64_t a);
int main(void)
{
    /* 16 words of 3 */
    if (probe_cast_const_len(3) != 48) return 1;
    return 0;
}
