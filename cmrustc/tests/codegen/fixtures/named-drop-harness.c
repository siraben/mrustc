#include <stdint.h>

uint32_t named_drop(void);

int main(void)
{
    return named_drop() == 3 ? 0 : 1;
}
