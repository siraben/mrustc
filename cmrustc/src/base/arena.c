#include "cm/arena.h"

#include "cm/alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef union CmArenaAlignment {
    void *pointer_value;
    long double long_double_value;
    long long long_long_value;
} CmArenaAlignment;

struct CmArenaBlock {
    CmArenaBlock *next;
    size_t capacity;
    size_t used;
    CmArenaAlignment force_alignment;
    unsigned char data[1];
};

typedef struct CmArenaMarkRecord {
    CmArenaMark mark;
} CmArenaMarkRecord;

static uint64_t cm_arena_lifetime_counter;

static uint64_t cm_arena_new_lifetime(void)
{
    if (cm_arena_lifetime_counter == UINT64_MAX) {
        abort();
    }
    cm_arena_lifetime_counter += 1u;
    return cm_arena_lifetime_counter;
}

static size_t cm_arena_data_offset(void)
{
    return offsetof(CmArenaBlock, data);
}

static int cm_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static CmArenaBlock *cm_arena_new_block(size_t capacity)
{
    CmArenaBlock *block;
    size_t allocation_size;

    if (!cm_size_add(cm_arena_data_offset(), capacity, &allocation_size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    block = (CmArenaBlock *)cm_alloc(allocation_size);
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;
    return block;
}

static void *cm_arena_try_alloc(
    CmArenaBlock *block,
    size_t size,
    size_t alignment
)
{
    uintptr_t base;
    uintptr_t current;
    uintptr_t aligned;
    size_t offset;

    base = (uintptr_t)block->data;
    if (block->used > block->capacity || base > (uintptr_t)-1 - block->used) {
        return NULL;
    }
    current = base + block->used;
    if (current > (uintptr_t)-1 - (alignment - 1)) {
        return NULL;
    }
    aligned = (current + alignment - 1) & ~((uintptr_t)alignment - 1);
    if (aligned < base) {
        return NULL;
    }
    offset = (size_t)(aligned - base);
    if (offset > block->capacity || size > block->capacity - offset) {
        return NULL;
    }
    block->used = offset + size;
    return (void *)aligned;
}

void cm_arena_init(CmArena *arena, size_t block_size)
{
    if (arena == NULL) {
        abort();
    }
    if (block_size == 0) {
        block_size = 4096;
    }
    arena->blocks = NULL;
    arena->initial_block_size = block_size;
    arena->next_block_size = block_size;
    cm_vec_init(&arena->active_marks, sizeof(CmArenaMarkRecord));
    arena->lifetime_id = cm_arena_new_lifetime();
    arena->next_mark_id = 1u;
}

void cm_arena_destroy(CmArena *arena)
{
    CmArenaBlock *block;
    CmArenaBlock *next;
    uint64_t new_lifetime;

    if (arena == NULL) {
        return;
    }
    new_lifetime = cm_arena_new_lifetime();
    block = arena->blocks;
    while (block != NULL) {
        next = block->next;
        cm_free(block);
        block = next;
    }
    arena->blocks = NULL;
    arena->next_block_size = arena->initial_block_size;
    cm_vec_destroy(&arena->active_marks);
    arena->lifetime_id = new_lifetime;
    arena->next_mark_id = 1u;
}

void cm_arena_reset(CmArena *arena)
{
    cm_arena_destroy(arena);
}

CmArenaMark cm_arena_mark(CmArena *arena)
{
    CmArenaMark mark;
    CmArenaMarkRecord record;

    if (arena == NULL) {
        abort();
    }
    if (arena->next_mark_id == 0u) {
        abort();
    }
    mark.owner = arena;
    mark.head = arena->blocks;
    mark.head_used = arena->blocks == NULL ? 0 : arena->blocks->used;
    mark.next_block_size = arena->next_block_size;
    mark.lifetime_id = arena->lifetime_id;
    mark.mark_id = arena->next_mark_id;
    if (arena->next_mark_id == UINT64_MAX) {
        arena->next_mark_id = 0u;
    } else {
        arena->next_mark_id += 1u;
    }
    record.mark = mark;
    (void)cm_vec_push(&arena->active_marks, &record);
    return mark;
}

static int cm_arena_find_mark(
    const CmArena *arena,
    CmArenaMark mark,
    size_t *out_index
)
{
    const CmArenaMarkRecord *record;
    size_t index;

    if (arena == NULL || mark.owner != arena
        || mark.lifetime_id != arena->lifetime_id
        || mark.mark_id == 0u) {
        return 0;
    }
    for (index = 0u; index < arena->active_marks.len; index += 1u) {
        record = (const CmArenaMarkRecord *)cm_vec_at_const(
            &arena->active_marks,
            index
        );
        if (record->mark.mark_id == mark.mark_id) {
            if (record->mark.owner != mark.owner
                || record->mark.head != mark.head
                || record->mark.head_used != mark.head_used
                || record->mark.next_block_size != mark.next_block_size
                || record->mark.lifetime_id != mark.lifetime_id) {
                return 0;
            }
            if (out_index != NULL) {
                *out_index = index;
            }
            return 1;
        }
    }
    return 0;
}

int cm_arena_mark_is_valid(const CmArena *arena, CmArenaMark mark)
{
    CmArenaBlock *block;

    if (!cm_arena_find_mark(arena, mark, NULL)) {
        return 0;
    }
    block = arena->blocks;
    while (block != NULL && block != mark.head) {
        block = block->next;
    }
    if (block != mark.head
        || (mark.head == NULL && mark.head_used != 0)
        || (mark.head != NULL && mark.head_used > mark.head->used)) {
        return 0;
    }
    return 1;
}

void cm_arena_rewind(CmArena *arena, CmArenaMark mark)
{
    CmArenaBlock *block;
    CmArenaBlock *next;
    size_t mark_index;

    if (!cm_arena_find_mark(arena, mark, &mark_index)
        || !cm_arena_mark_is_valid(arena, mark)) {
        abort();
    }

    block = arena->blocks;
    while (block != mark.head) {
        next = block->next;
        cm_free(block);
        block = next;
    }
    arena->blocks = mark.head;
    if (arena->blocks != NULL) {
        arena->blocks->used = mark.head_used;
    }
    arena->next_block_size = mark.next_block_size;
    cm_vec_resize(&arena->active_marks, mark_index + 1u);
}

void cm_arena_discard_mark(CmArena *arena, CmArenaMark mark)
{
    size_t mark_index;

    if (!cm_arena_find_mark(arena, mark, &mark_index)
        || !cm_arena_mark_is_valid(arena, mark)) {
        abort();
    }
    cm_vec_resize(&arena->active_marks, mark_index);
}

void *cm_arena_alloc(CmArena *arena, size_t size, size_t alignment)
{
    CmArenaBlock *block;
    void *allocation;
    size_t minimum_capacity;
    size_t block_capacity;
    size_t doubled;

    if (arena == NULL) {
        abort();
    }
    if (alignment == 0) {
        alignment = 1;
    }
    if (!cm_is_power_of_two(alignment)) {
        abort();
    }
    if (size == 0) {
        size = 1;
    }

    block = arena->blocks;
    if (block != NULL) {
        allocation = cm_arena_try_alloc(block, size, alignment);
        if (allocation != NULL) {
            return allocation;
        }
    }

    if (!cm_size_add(size, alignment - 1, &minimum_capacity)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    block_capacity = arena->next_block_size;
    if (block_capacity < minimum_capacity) {
        block_capacity = minimum_capacity;
    }
    block = cm_arena_new_block(block_capacity);
    block->next = arena->blocks;
    arena->blocks = block;

    if (cm_size_add(block_capacity, block_capacity, &doubled)) {
        arena->next_block_size = doubled;
    } else {
        arena->next_block_size = (size_t)-1;
    }

    allocation = cm_arena_try_alloc(block, size, alignment);
    if (allocation == NULL) {
        abort();
    }
    return allocation;
}

void *cm_arena_alloc_zeroed(
    CmArena *arena,
    size_t count,
    size_t element_size,
    size_t alignment
)
{
    void *allocation;
    size_t size;

    if (!cm_size_mul(count, element_size, &size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    allocation = cm_arena_alloc(arena, size, alignment);
    memset(allocation, 0, size == 0 ? 1 : size);
    return allocation;
}

size_t cm_arena_block_count(const CmArena *arena)
{
    const CmArenaBlock *block;
    size_t count;

    count = 0;
    block = arena->blocks;
    while (block != NULL) {
        count += 1;
        block = block->next;
    }
    return count;
}

size_t cm_arena_bytes_used(const CmArena *arena)
{
    const CmArenaBlock *block;
    size_t total;

    total = 0;
    block = arena->blocks;
    while (block != NULL) {
        if (!cm_size_add(total, block->used, &total)) {
            return (size_t)-1;
        }
        block = block->next;
    }
    return total;
}

size_t cm_arena_capacity(const CmArena *arena)
{
    const CmArenaBlock *block;
    size_t total;

    total = 0;
    block = arena->blocks;
    while (block != NULL) {
        if (!cm_size_add(total, block->capacity, &total)) {
            return (size_t)-1;
        }
        block = block->next;
    }
    return total;
}
