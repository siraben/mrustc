#include <stdint.h>
uint32_t probe_decl_macro_brace(void);
int main(void)
{
    /* the unix arm of a brace-invoked paren-parameter macro */
    if (probe_decl_macro_brace() != 42) return 1;
    return 0;
}
