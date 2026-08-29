#include <stdint.h>
uint32_t probe_cfg_decl_field(uint32_t a);
int main(void)
{
    if (probe_cfg_decl_field(28) != 42) return 1;
    return 0;
}
