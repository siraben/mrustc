#include <stdint.h>

uint32_t niche_option(uint32_t n);

int main(void)
{
    if (niche_option(0) != 1) return 1;
    if (niche_option(7) != 7107) return 2;
    return 0;
}
