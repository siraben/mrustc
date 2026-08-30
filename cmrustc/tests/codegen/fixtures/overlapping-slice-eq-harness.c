#include <stdint.h>

uint32_t overlapping_slice_eq_probe(void);

int main(void)
{
    return overlapping_slice_eq_probe() == 41u ? 0 : 1;
}
