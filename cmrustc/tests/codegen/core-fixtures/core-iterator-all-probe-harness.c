#include <stdlib.h>
#include <stdint.h>

long long panic_impl(long long info) { (void)info; abort(); }
uint32_t core_iterator_all_probe(void);

int main(void)
{
    return core_iterator_all_probe() == 1u ? 0 : 1;
}
