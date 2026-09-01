#include <stdint.h>

uint32_t slice_get_unchecked_width(void);

int main(void)
{
    return slice_get_unchecked_width() == 49u ? 0 : 1;
}
