#include <stdint.h>

static uint32_t slot;
uint32_t *cm_host_errno_slot(void) { return &slot; }
uint32_t cm_host_base = 40;
uint32_t link_name(uint32_t x);

int main(void)
{
    if (link_name(3) != 43) return 1;
    return 0;
}
