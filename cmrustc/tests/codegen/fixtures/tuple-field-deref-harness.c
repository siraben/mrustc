#include <stdint.h>

uint32_t tuple_field_deref(uint32_t x);

int main(void)
{
    /* (3+5) + 3 */
    if (tuple_field_deref(3) != 11) return 1;
    return 0;
}
