#include <stdint.h>

uint32_t box_field_deref(uint32_t x, uint32_t y);

int main(void)
{
    return box_field_deref(3u, 7u) == 37u ? 0 : 1;
}
