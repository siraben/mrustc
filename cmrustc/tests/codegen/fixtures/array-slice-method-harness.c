#include <stddef.h>

size_t array_slice_method(void);

int main(void)
{
    return array_slice_method() == (size_t)('l' + 10) ? 0 : 1;
}
