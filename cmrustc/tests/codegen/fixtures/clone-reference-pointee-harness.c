#include <stdint.h>

uint32_t clone_reference_pointee(uint32_t start, uint32_t end);

int main(void)
{
    return clone_reference_pointee(4, 11) == 411 ? 0 : 1;
}
