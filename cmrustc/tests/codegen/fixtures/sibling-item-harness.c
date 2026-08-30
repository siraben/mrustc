#include <stdint.h>

uint32_t sibling_item(uint32_t x);

int main(void)
{
    if (sibling_item(1) != 43) return 1;
    return 0;
}
