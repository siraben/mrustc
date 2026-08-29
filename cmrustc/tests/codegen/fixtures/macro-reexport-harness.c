#include <stdint.h>
uint32_t probe_macro_reexport(void);
int main(void)
{
    if (probe_macro_reexport() != 5) return 1;
    return 0;
}
