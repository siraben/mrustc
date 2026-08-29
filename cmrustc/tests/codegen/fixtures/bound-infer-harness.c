#include <stdint.h>

uint32_t bound_infer(uint32_t x);

int main(void)
{
    /* seen 10 -> 11, + 3 */
    if (bound_infer(3) != 14) return 1;
    /* Err path: seen stays 10, + 200 */
    if (bound_infer(200) != 210) return 1;
    return 0;
}
