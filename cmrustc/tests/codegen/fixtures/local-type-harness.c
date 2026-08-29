#include <stdint.h>
uintptr_t probe_local_type(uintptr_t a);
int main(void)
{
    if (probe_local_type(41) != 42) return 1;
    return 0;
}
