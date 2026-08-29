#include <stdint.h>
#include <stdarg.h>

uint32_t fn_ptr_abi(uint32_t x);

long long host_sum(long long count, ...)
{
    va_list args;
    long long total = 0;
    long long i;
    va_start(args, count);
    for (i = 0; i < count; ++i) total += va_arg(args, long long);
    va_end(args);
    return total;
}

int main(void)
{
    /* x=4: plus_one 5; sum 4+10+100 = 114 -> 5 + 114000 */
    if (fn_ptr_abi(4) != 114005) return 1;
    return 0;
}
