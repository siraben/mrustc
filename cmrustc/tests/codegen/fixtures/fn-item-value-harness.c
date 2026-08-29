#include <stdint.h>

uint32_t fn_item_value(uint32_t x);

int main(void)
{
    /* x=4: 8 + 7*100; x=9: 18 + 18*100 */
    if (fn_item_value(4) != 708) return 1;
    if (fn_item_value(9) != 1818) return 2;
    return 0;
}
