#include "cm/vec.h"

#include "cm/alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t cm_vec_grow_capacity(size_t current, size_t minimum)
{
    size_t capacity;

    capacity = current == 0 ? 8 : current;
    while (capacity < minimum) {
        if (capacity > (size_t)-1 / 2) {
            capacity = minimum;
            break;
        }
        capacity *= 2;
    }
    return capacity;
}

void cm_vec_init(CmVec *vector, size_t element_size)
{
    if (element_size == 0) {
        abort();
    }
    vector->data = NULL;
    vector->len = 0;
    vector->cap = 0;
    vector->elem_size = element_size;
}

void cm_vec_destroy(CmVec *vector)
{
    size_t element_size;

    element_size = vector->elem_size;
    cm_free(vector->data);
    vector->data = NULL;
    vector->len = 0;
    vector->cap = 0;
    vector->elem_size = element_size;
}

void cm_vec_clear(CmVec *vector)
{
    vector->len = 0;
}

void cm_vec_reserve(CmVec *vector, size_t minimum_capacity)
{
    size_t capacity;
    size_t allocation_size;

    if (minimum_capacity <= vector->cap) {
        return;
    }
    capacity = cm_vec_grow_capacity(vector->cap, minimum_capacity);
    if (!cm_size_mul(capacity, vector->elem_size, &allocation_size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    vector->data = (unsigned char *)cm_realloc(vector->data, allocation_size);
    vector->cap = capacity;
}

void cm_vec_resize(CmVec *vector, size_t length)
{
    size_t old_length;
    size_t new_bytes;

    old_length = vector->len;
    cm_vec_reserve(vector, length);
    if (length > old_length) {
        if (!cm_size_mul(length - old_length, vector->elem_size, &new_bytes)) {
            cm_alloc_out_of_memory((size_t)-1);
        }
        memset(
            vector->data + old_length * vector->elem_size,
            0,
            new_bytes
        );
    }
    vector->len = length;
}

void cm_vec_append(CmVec *vector, const void *items, size_t count)
{
    size_t new_length;
    size_t old_bytes;
    size_t append_bytes;
    size_t source_offset;
    int source_is_internal;
    uintptr_t source_address;
    uintptr_t vector_address;

    if (count == 0) {
        return;
    }
    if (items == NULL) {
        abort();
    }
    if (!cm_size_add(vector->len, count, &new_length)
        || !cm_size_mul(vector->len, vector->elem_size, &old_bytes)
        || !cm_size_mul(count, vector->elem_size, &append_bytes)) {
        cm_alloc_out_of_memory((size_t)-1);
    }

    source_address = (uintptr_t)items;
    vector_address = (uintptr_t)vector->data;
    source_is_internal = vector->data != NULL
        && source_address >= vector_address
        && source_address < vector_address + old_bytes;
    source_offset = source_is_internal
        ? (size_t)(source_address - vector_address)
        : 0;
    if (source_is_internal && append_bytes > old_bytes - source_offset) {
        abort();
    }

    cm_vec_reserve(vector, new_length);
    if (source_is_internal) {
        items = vector->data + source_offset;
    }
    memmove(vector->data + old_bytes, items, append_bytes);
    vector->len = new_length;
}

void *cm_vec_push(CmVec *vector, const void *item)
{
    size_t index;

    index = vector->len;
    cm_vec_append(vector, item, 1);
    return vector->data + index * vector->elem_size;
}

void *cm_vec_push_uninit(CmVec *vector)
{
    size_t new_length;
    void *slot;

    if (!cm_size_add(vector->len, 1, &new_length)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    cm_vec_reserve(vector, new_length);
    slot = vector->data + vector->len * vector->elem_size;
    vector->len = new_length;
    return slot;
}

int cm_vec_pop(CmVec *vector, void *item_out)
{
    if (vector->len == 0) {
        return 0;
    }
    vector->len -= 1;
    if (item_out != NULL) {
        memcpy(
            item_out,
            vector->data + vector->len * vector->elem_size,
            vector->elem_size
        );
    }
    return 1;
}

void *cm_vec_at(CmVec *vector, size_t index)
{
    if (index >= vector->len) {
        return NULL;
    }
    return vector->data + index * vector->elem_size;
}

const void *cm_vec_at_const(const CmVec *vector, size_t index)
{
    if (index >= vector->len) {
        return NULL;
    }
    return vector->data + index * vector->elem_size;
}
