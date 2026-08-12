#include "cm/arena.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_mark_generations(void)
{
    CmArena arena;
    CmArena other;
    CmArenaMark outer;
    CmArenaMark stale_inner;
    CmArenaMark current;
    unsigned char *bytes;

    cm_arena_init(&arena, 64);
    cm_arena_init(&other, 64);
    bytes = (unsigned char *)cm_arena_alloc(&arena, 5, 1);
    memcpy(bytes, "seed", 5);
    outer = cm_arena_mark(&arena);
    bytes = (unsigned char *)cm_arena_alloc(&arena, 2, 1);
    memcpy(bytes, "x", 2);
    stale_inner = cm_arena_mark(&arena);

    cm_arena_rewind(&arena, outer);
    bytes = (unsigned char *)cm_arena_alloc(&arena, 12, 1);
    memcpy(bytes, "replacement", 12);
    assert(!cm_arena_mark_is_valid(&arena, stale_inner));
    assert(!cm_arena_mark_is_valid(&other, outer));
    assert(cm_arena_mark_is_valid(&arena, outer));

    current = cm_arena_mark(&arena);
    assert(cm_arena_mark_is_valid(&arena, current));
    cm_arena_discard_mark(&arena, current);
    assert(!cm_arena_mark_is_valid(&arena, current));
    assert(cm_arena_mark_is_valid(&arena, outer));

    cm_arena_rewind(&arena, outer);
    cm_arena_rewind(&arena, outer);
    cm_arena_discard_mark(&arena, outer);
    assert(!cm_arena_mark_is_valid(&arena, outer));

    current = cm_arena_mark(&arena);
    cm_arena_reset(&arena);
    assert(!cm_arena_mark_is_valid(&arena, current));
    current = cm_arena_mark(&arena);
    assert(cm_arena_mark_is_valid(&arena, current));
    cm_arena_discard_mark(&arena, current);

    cm_arena_destroy(&other);
    cm_arena_destroy(&arena);
}

int main(void)
{
    CmArena arena;
    CmArenaMark empty_mark;
    CmArenaMark first_mark;
    CmArenaMark nested_mark;
    unsigned char *bytes;
    unsigned char *first;
    unsigned char *nested;
    void *allocation;
    size_t alignment;
    size_t first_block_count;
    size_t first_bytes_used;
    size_t first_next_block_size;
    size_t index;

    cm_arena_init(&arena, 32);
    empty_mark = cm_arena_mark(&arena);
    first = (unsigned char *)cm_arena_alloc(&arena, 7, 1);
    memcpy(first, "stable", 7);
    first_mark = cm_arena_mark(&arena);
    first_block_count = cm_arena_block_count(&arena);
    first_bytes_used = cm_arena_bytes_used(&arena);
    first_next_block_size = arena.next_block_size;
    nested = (unsigned char *)cm_arena_alloc(&arena, 5, 1);
    memcpy(nested, "inner", 5);
    nested_mark = cm_arena_mark(&arena);
    (void)cm_arena_alloc(&arena, 200, 16);
    assert(cm_arena_block_count(&arena) > first_block_count);

    cm_arena_rewind(&arena, nested_mark);
    assert(memcmp(first, "stable", 7) == 0);
    assert(memcmp(nested, "inner", 5) == 0);
    cm_arena_rewind(&arena, first_mark);
    assert(cm_arena_block_count(&arena) == first_block_count);
    assert(cm_arena_bytes_used(&arena) == first_bytes_used);
    assert(arena.next_block_size == first_next_block_size);
    assert(memcmp(first, "stable", 7) == 0);

    for (alignment = 1; alignment <= 128; alignment *= 2) {
        allocation = cm_arena_alloc(&arena, 3, alignment);
        assert(((uintptr_t)allocation % alignment) == 0);
        memset(allocation, (int)alignment, 3);
    }
    assert(cm_arena_block_count(&arena) >= 2);

    for (index = 0; index < 100; index += 1) {
        bytes = (unsigned char *)cm_arena_alloc(&arena, 13, 8);
        assert(((uintptr_t)bytes % 8) == 0);
        bytes[0] = (unsigned char)index;
        bytes[12] = (unsigned char)(index + 1);
    }
    assert(cm_arena_block_count(&arena) >= 3);
    assert(cm_arena_capacity(&arena) >= cm_arena_bytes_used(&arena));

    bytes = (unsigned char *)cm_arena_alloc_zeroed(&arena, 37, 1, 16);
    for (index = 0; index < 37; index += 1) {
        assert(bytes[index] == 0);
    }

    cm_arena_rewind(&arena, empty_mark);
    assert(cm_arena_block_count(&arena) == 0);
    assert(cm_arena_bytes_used(&arena) == 0);
    assert(arena.next_block_size == arena.initial_block_size);

    cm_arena_reset(&arena);
    assert(cm_arena_block_count(&arena) == 0);
    assert(cm_arena_bytes_used(&arena) == 0);
    assert(cm_arena_capacity(&arena) == 0);

    allocation = cm_arena_alloc(&arena, 1, 0);
    assert(allocation != NULL);
    assert(cm_arena_block_count(&arena) == 1);
    cm_arena_destroy(&arena);
    test_mark_generations();
    return 0;
}
