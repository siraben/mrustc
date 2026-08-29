#include <stdint.h>

uint32_t glob_mod_macro(void);

int main(void)
{
    if (glob_mod_macro() != 11) return 1;
    return 0;
}
