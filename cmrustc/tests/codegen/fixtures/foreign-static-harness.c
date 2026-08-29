#include <stdint.h>
/* The Rust side declares `extern "C" { static HOST_LEVEL: u32; }`; the
 * host owns the storage.  Values are read as a first word, so give the
 * static a full word. */
long long HOST_LEVEL = 20;
uint32_t probe_foreign_static(uint32_t a);
int main(void)
{
    if (probe_foreign_static(2) != 42) return 1;
    HOST_LEVEL = 1;
    if (probe_foreign_static(0) != 2) return 2;
    return 0;
}
