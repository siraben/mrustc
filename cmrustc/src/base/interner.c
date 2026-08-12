#include "cm/interner.h"

#include "cm/alloc.h"

#include <stdlib.h>
#include <string.h>

void cm_interner_init(CmInterner *interner, size_t arena_block_size)
{
    cm_arena_init(&interner->strings, arena_block_size);
    cm_map_init(&interner->by_text, sizeof(CmInternId));
    cm_vec_init(&interner->entries, sizeof(CmInternedString));
}

void cm_interner_destroy(CmInterner *interner)
{
    cm_vec_destroy(&interner->entries);
    cm_map_destroy(&interner->by_text);
    cm_arena_destroy(&interner->strings);
}

CmInternerMark cm_interner_mark(CmInterner *interner)
{
    CmInternerMark mark;

    if (interner == NULL) {
        abort();
    }
    mark.owner = interner;
    mark.strings = cm_arena_mark(&interner->strings);
    mark.entry_count = interner->entries.len;
    return mark;
}

int cm_interner_mark_is_valid(
    const CmInterner *interner,
    CmInternerMark mark
)
{
    return interner != NULL
        && mark.owner == interner
        && mark.strings.owner == &interner->strings
        && mark.entry_count <= interner->entries.len
        && cm_arena_mark_is_valid(&interner->strings, mark.strings);
}

void cm_interner_rewind(CmInterner *interner, CmInternerMark mark)
{
    const CmInternedString *entry;
    const CmInternId *stored_id;
    CmInternId expected_id;
    size_t index;

    /* Reject stale storage checkpoints before touching the map or vector. */
    if (!cm_interner_mark_is_valid(interner, mark)) {
        abort();
    }

    /* Validate all map bindings before removing any of them. */
    for (index = mark.entry_count; index < interner->entries.len; index += 1) {
        entry = (const CmInternedString *)cm_vec_at_const(
            &interner->entries,
            index
        );
        stored_id = (const CmInternId *)cm_map_get_const(
            &interner->by_text,
            entry->bytes,
            entry->len
        );
        expected_id = (CmInternId)(index + 1);
        if (stored_id == NULL || *stored_id != expected_id) {
            abort();
        }
    }

    for (index = interner->entries.len; index > mark.entry_count; index -= 1) {
        entry = (const CmInternedString *)cm_vec_at_const(
            &interner->entries,
            index - 1
        );
        if (!cm_map_remove(
            &interner->by_text,
            entry->bytes,
            entry->len,
            NULL
        )) {
            abort();
        }
    }
    cm_vec_resize(&interner->entries, mark.entry_count);
    cm_arena_rewind(&interner->strings, mark.strings);
}

void cm_interner_discard_mark(CmInterner *interner, CmInternerMark mark)
{
    if (!cm_interner_mark_is_valid(interner, mark)) {
        abort();
    }
    cm_arena_discard_mark(&interner->strings, mark.strings);
}

CmInternId cm_interner_lookup(
    const CmInterner *interner,
    const void *bytes,
    size_t length
)
{
    const CmInternId *id;

    id = (const CmInternId *)cm_map_get_const(
        &interner->by_text,
        bytes,
        length
    );
    return id == NULL ? CM_INTERN_ID_NONE : *id;
}

CmInternId cm_interner_intern(
    CmInterner *interner,
    const void *bytes,
    size_t length
)
{
    CmInternedString *entry;
    CmInternId id;
    unsigned char *copy;
    size_t allocation_size;
    size_t new_length;
    int inserted;

    if (bytes == NULL && length != 0) {
        abort();
    }
    id = cm_interner_lookup(interner, bytes, length);
    if (id != CM_INTERN_ID_NONE) {
        return id;
    }
    if (!cm_size_add(interner->entries.len, 1, &new_length)
        || new_length > (size_t)UINT32_MAX) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    id = (CmInternId)new_length;

    /* Reserve every fallible container growth before publishing the ID. */
    cm_vec_reserve(&interner->entries, new_length);
    cm_map_reserve(&interner->by_text, new_length);
    if (!cm_size_add(length, 1, &allocation_size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    copy = (unsigned char *)cm_arena_alloc(
        &interner->strings,
        allocation_size,
        1
    );
    if (length != 0) {
        memcpy(copy, bytes, length);
    }
    copy[length] = 0;

    (void)cm_map_insert(
        &interner->by_text,
        copy,
        length,
        &id,
        &inserted
    );
    if (!inserted) {
        abort();
    }
    entry = (CmInternedString *)cm_vec_push_uninit(&interner->entries);
    entry->bytes = copy;
    entry->len = length;
    return id;
}

CmInternId cm_interner_intern_c_str(CmInterner *interner, const char *text)
{
    if (text == NULL) {
        abort();
    }
    return cm_interner_intern(interner, text, strlen(text));
}

const CmInternedString *cm_interner_get(
    const CmInterner *interner,
    CmInternId id
)
{
    if (id == CM_INTERN_ID_NONE) {
        return NULL;
    }
    return (const CmInternedString *)cm_vec_at_const(
        &interner->entries,
        (size_t)id - 1
    );
}

size_t cm_interner_length(const CmInterner *interner)
{
    return interner->entries.len;
}
