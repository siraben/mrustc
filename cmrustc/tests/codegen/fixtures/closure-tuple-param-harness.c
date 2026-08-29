#include <stdint.h>

uint32_t closure_tuple_param(uint32_t k);

int main(void)
{
    /* k=3: first=30, second=6, third=307 */
    if (closure_tuple_param(3) != 30 + 6000 + 30700000) return 1;
    if (closure_tuple_param(1) != 10 + 6000 + 10700000) return 2;
    return 0;
}
