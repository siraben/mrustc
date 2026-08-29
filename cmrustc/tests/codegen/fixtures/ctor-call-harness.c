#include <stdint.h>

int32_t ctor_call(int32_t x);

int main(void)
{
    /* 300 + 13 */
    if (ctor_call(3) != 313) return 1;
    return 0;
}
