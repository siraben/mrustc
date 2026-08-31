#include <stdint.h>

uint32_t associated_item_closure_param(uint32_t k);

int main(void)
{
    if (associated_item_closure_param(3) != 3234) return 1;
    if (associated_item_closure_param(9) != 9234) return 2;
    return 0;
}
