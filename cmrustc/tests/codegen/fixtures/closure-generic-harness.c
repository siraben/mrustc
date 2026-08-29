#include <stdint.h>

uint32_t closure_generic(uint32_t k);

int main(void)
{
    /* k=3: a=(1+3)*10=40, b=(1+3)+1000=1004, c=(3+5)*2=16, d=3*100*2=600 */
    if (closure_generic(3) != 40 + 1004 + 160000 + 600000000) return 1;
    /* k=0: a=10, b=1001, c=10, d=0 */
    if (closure_generic(0) != 10 + 1001 + 100000 + 0) return 2;
    return 0;
}
