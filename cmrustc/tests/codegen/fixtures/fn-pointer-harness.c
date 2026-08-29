#include <stdint.h>

uint32_t fn_pointer(uint32_t v);

int main(void)
{
    if (fn_pointer(0) != 116) return 1;   /* 1+100 + 15 + 0 */
    if (fn_pointer(7) != 137) return 2;   /* 8+100 + 15 + 14 */
    return 0;
}
