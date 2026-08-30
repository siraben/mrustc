#include <stdint.h>

uint32_t foreign_never(uint32_t x);

int main(void)
{
    if (foreign_never(5) != 6) return 1;
    /* exits the process with status 0 */
    foreign_never(150);
    return 1;
}
