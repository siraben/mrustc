#include "cm/alloc.h"
#include "cm/arena.h"
#include "cm/buf.h"
#include "cm/vec.h"

#include <assert.h>
#include <setjmp.h>

static jmp_buf oom_jump;
static int oom_seen;

static void jump_on_oom(size_t requested_size, void *context)
{
    (void)requested_size;
    (void)context;
    oom_seen += 1;
    longjmp(oom_jump, 1);
}

static void expect_arena_oom(void)
{
    CmArena arena;

    cm_arena_init(&arena, 16);
    cm_alloc_fail_after(0);
    if (setjmp(oom_jump) == 0) {
        (void)cm_arena_alloc(&arena, 4, 4);
        assert(0);
    }
    cm_alloc_fail_never();
    cm_arena_destroy(&arena);
}

static void expect_byte_buf_oom(void)
{
    CmByteBuf buffer;

    cm_byte_buf_init(&buffer);
    cm_alloc_fail_after(0);
    if (setjmp(oom_jump) == 0) {
        cm_byte_buf_push(&buffer, 1);
        assert(0);
    }
    cm_alloc_fail_never();
    cm_byte_buf_destroy(&buffer);
}

static void expect_string_buf_oom(void)
{
    CmStrBuf buffer;

    cm_str_buf_init(&buffer);
    cm_alloc_fail_after(0);
    if (setjmp(oom_jump) == 0) {
        cm_str_buf_append(&buffer, "failure");
        assert(0);
    }
    cm_alloc_fail_never();
    cm_str_buf_destroy(&buffer);
}

static void expect_vec_oom(void)
{
    CmVec vector;
    int value;

    value = 7;
    cm_vec_init(&vector, sizeof(value));
    cm_alloc_fail_after(0);
    if (setjmp(oom_jump) == 0) {
        (void)cm_vec_push(&vector, &value);
        assert(0);
    }
    cm_alloc_fail_never();
    cm_vec_destroy(&vector);
}

int main(void)
{
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    expect_arena_oom();
    expect_byte_buf_oom();
    expect_string_buf_oom();
    expect_vec_oom();
    cm_alloc_set_oom_handler(NULL, NULL);
    cm_alloc_fail_never();
    assert(oom_seen == 4);
    return 0;
}
