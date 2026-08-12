#ifndef CMRUSTC_CM_ARENA_H
#define CMRUSTC_CM_ARENA_H

#include "cm/config.h"
#include "cm/vec.h"

typedef struct CmArenaBlock CmArenaBlock;

typedef struct CmArena {
    CmArenaBlock *blocks;
    size_t initial_block_size;
    size_t next_block_size;
    CmVec active_marks;
    uint64_t lifetime_id;
    uint64_t next_mark_id;
} CmArena;

typedef struct CmArenaMark {
    const CmArena *owner;
    CmArenaBlock *head;
    size_t head_used;
    size_t next_block_size;
    uint64_t lifetime_id;
    uint64_t mark_id;
} CmArenaMark;

void cm_arena_init(CmArena *arena, size_t block_size);
void cm_arena_destroy(CmArena *arena);
void cm_arena_reset(CmArena *arena);
/*
 * Marks are stack-like capabilities owned by one arena lifetime.  Rewind
 * preserves the selected mark for repeated rollback and invalidates every
 * descendant mark.  Discard commits the selected checkpoint and invalidates
 * it plus its descendants without changing allocated storage.
 */
CmArenaMark cm_arena_mark(CmArena *arena);
int cm_arena_mark_is_valid(const CmArena *arena, CmArenaMark mark);
void cm_arena_rewind(CmArena *arena, CmArenaMark mark);
void cm_arena_discard_mark(CmArena *arena, CmArenaMark mark);

/* Alignment must be zero, one, or a power of two.  Zero means one. */
void *cm_arena_alloc(CmArena *arena, size_t size, size_t alignment);
void *cm_arena_alloc_zeroed(
    CmArena *arena,
    size_t count,
    size_t element_size,
    size_t alignment
);

size_t cm_arena_block_count(const CmArena *arena);
size_t cm_arena_bytes_used(const CmArena *arena);
size_t cm_arena_capacity(const CmArena *arena);

#endif
