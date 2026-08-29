#include <stdint.h>
#include <stdlib.h>

/* The extern-block declaration forwards to this host symbol. */
long long host_abort(long long info) { (void)info; abort(); }

uint32_t body_local_extern(uint32_t code);

int main(void)
{
    if (body_local_extern(UINT32_C(0)) != UINT32_C(2)) return 1;
    if (body_local_extern(UINT32_C(40)) != UINT32_C(42)) return 2;
    if (body_local_extern(UINT32_C(1000)) != UINT32_C(1002)) return 3;
    return 0;
}
