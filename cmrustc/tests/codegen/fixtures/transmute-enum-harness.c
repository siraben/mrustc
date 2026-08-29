#include <stdint.h>
#include <stddef.h>

size_t transmute_enum(size_t v);

int main(void)
{
    /* 4*100 + 4 + 8 */
    if (transmute_enum(4) != 412) return 1;
    /* 16*100 + 16 + 8 */
    if (transmute_enum(16) != 1624) return 2;
    return 0;
}
