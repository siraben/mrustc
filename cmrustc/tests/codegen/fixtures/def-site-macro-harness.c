#include <stdint.h>

uint32_t def_site_macro(void);

int main(void)
{
    if (def_site_macro() != 7) return 1;
    return 0;
}
