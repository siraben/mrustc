#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

size_t probe_fmt2(uint32_t which, uint32_t value, uint8_t *out, size_t cap);

static int check(uint32_t which, uint32_t value, const char *want)
{
    uint8_t out[64];
    size_t n = probe_fmt2(which, value, out, sizeof out);
    if (n != strlen(want) || memcmp(out, want, n) != 0) {
        fprintf(stderr, "fmt2 which=%u value=%u got \"%.*s\" want \"%s\"\n",
            (unsigned)which, (unsigned)value, (int)n, (const char *)out, want);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (check(0, 7, "x=7")) return 1;
    if (check(1, 255, "ff")) return 2;
    if (check(2, 42, "[   42]")) return 3;
    if (check(3, 3, "-7")) return 4;
    if (check(4, 5, "true")) return 5;
    if (check(5, 9, "Some(9)")) return 6;
    if (check(5, 1, "None")) return 7;
    if (check(6, 5, "00000101")) return 8;
    return 0;
}
