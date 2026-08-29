#include <stdint.h>

uint32_t meta_cascade(uint32_t x);

int main(void)
{
    /* the linux arm is branch 11 of 20 */
    if (meta_cascade(1) != 12) return 1;
    return 0;
}
