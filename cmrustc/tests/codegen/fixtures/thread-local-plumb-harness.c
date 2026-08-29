#include <stdint.h>

uint32_t thread_local_plumb(void);

int main(void)
{
    /* COUNT 7+100, Key.v 3, COUNT2 1000+100 */
    if (thread_local_plumb() != 1210) return 1;
    return 0;
}
