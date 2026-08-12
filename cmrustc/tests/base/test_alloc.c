#include "cm/alloc.h"

#include <assert.h>
#include <setjmp.h>
#include <stddef.h>

static jmp_buf oom_jump;
static size_t oom_request;

static void catch_oom(size_t requested_size, void *context)
{
    int *calls;

    calls = (int *)context;
    *calls += 1;
    oom_request = requested_size;
    longjmp(oom_jump, 1);
}

int main(void)
{
    unsigned char *bytes;
    size_t value;
    int oom_calls;
    size_t index;

    assert(cm_size_add(10, 20, &value));
    assert(value == 30);
    assert(!cm_size_add((size_t)-1, 1, &value));
    assert(cm_size_mul(12, 11, &value));
    assert(value == 132);
    assert(!cm_size_mul((size_t)-1, 2, &value));

    bytes = (unsigned char *)cm_alloc_zeroed(32, 1);
    for (index = 0; index < 32; index += 1) {
        assert(bytes[index] == 0);
        bytes[index] = (unsigned char)index;
    }
    bytes = (unsigned char *)cm_realloc(bytes, 64);
    for (index = 0; index < 32; index += 1) {
        assert(bytes[index] == (unsigned char)index);
    }
    cm_free(bytes);
    cm_free(cm_alloc(0));

    oom_calls = 0;
    cm_alloc_set_oom_handler(catch_oom, &oom_calls);
    cm_alloc_fail_after(0);
    if (setjmp(oom_jump) == 0) {
        (void)cm_alloc(123);
        assert(0);
    }
    assert(oom_calls == 1);
    assert(oom_request == 123);

    cm_alloc_fail_never();
    if (setjmp(oom_jump) == 0) {
        (void)cm_alloc_zeroed((size_t)-1, 2);
        assert(0);
    }
    assert(oom_calls == 2);
    assert(oom_request == (size_t)-1);

    cm_alloc_set_oom_handler(NULL, NULL);
    cm_alloc_fail_never();
    return 0;
}
