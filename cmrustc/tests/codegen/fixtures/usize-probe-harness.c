#include <stdint.h>

#if !defined(UINTPTR_MAX) || UINTPTR_MAX != UINT64_MAX
# error "usize probe requires a 64-bit execution target"
#endif

extern uintptr_t probe_usize(uintptr_t left, uintptr_t right);

int main(void)
{
    if (probe_usize((uintptr_t)UINT64_C(10),
            (uintptr_t)UINT64_C(3)) != (uintptr_t)UINT64_C(8)) {
        return 1;
    }
    if (probe_usize((uintptr_t)UINT64_C(3),
            (uintptr_t)UINT64_C(5))
            != (uintptr_t)UINT64_C(4294967301)) {
        return 2;
    }
    if (probe_usize((uintptr_t)UINT64_C(0),
            (uintptr_t)UINT64_C(1)) != (uintptr_t)UINT64_C(0)) {
        return 3;
    }
    return 0;
}
