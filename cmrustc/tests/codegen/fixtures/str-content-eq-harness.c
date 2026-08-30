#include <stdint.h>

uint32_t str_content_eq_probe(void);

int main(void)
{
    return str_content_eq_probe() == 47u ? 0 : 1;
}
