#include "cm/buf.h"

#include "cm/alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t cm_grow_capacity(size_t current, size_t minimum)
{
    size_t capacity;

    capacity = current == 0 ? 16 : current;
    while (capacity < minimum) {
        if (capacity > (size_t)-1 / 2) {
            capacity = minimum;
            break;
        }
        capacity *= 2;
    }
    return capacity;
}

void cm_byte_buf_init(CmByteBuf *buffer)
{
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

void cm_byte_buf_destroy(CmByteBuf *buffer)
{
    cm_free(buffer->data);
    cm_byte_buf_init(buffer);
}

void cm_byte_buf_clear(CmByteBuf *buffer)
{
    buffer->len = 0;
}

void cm_byte_buf_reserve(CmByteBuf *buffer, size_t minimum_capacity)
{
    size_t capacity;

    if (minimum_capacity <= buffer->cap) {
        return;
    }
    capacity = cm_grow_capacity(buffer->cap, minimum_capacity);
    buffer->data = (unsigned char *)cm_realloc(buffer->data, capacity);
    buffer->cap = capacity;
}

void cm_byte_buf_resize(CmByteBuf *buffer, size_t length)
{
    size_t old_length;

    old_length = buffer->len;
    cm_byte_buf_reserve(buffer, length);
    if (length > old_length) {
        memset(buffer->data + old_length, 0, length - old_length);
    }
    buffer->len = length;
}

void cm_byte_buf_append(CmByteBuf *buffer, const void *data, size_t length)
{
    size_t new_length;
    size_t source_offset;
    int source_is_internal;
    uintptr_t source_address;
    uintptr_t buffer_address;

    if (length == 0) {
        return;
    }
    if (data == NULL) {
        abort();
    }
    if (!cm_size_add(buffer->len, length, &new_length)) {
        cm_alloc_out_of_memory((size_t)-1);
    }

    source_address = (uintptr_t)data;
    buffer_address = (uintptr_t)buffer->data;
    source_is_internal = buffer->data != NULL
        && source_address >= buffer_address
        && source_address < buffer_address + buffer->len;
    source_offset = source_is_internal
        ? (size_t)(source_address - buffer_address)
        : 0;
    if (source_is_internal && length > buffer->len - source_offset) {
        abort();
    }

    cm_byte_buf_reserve(buffer, new_length);
    if (source_is_internal) {
        data = buffer->data + source_offset;
    }
    memmove(buffer->data + buffer->len, data, length);
    buffer->len = new_length;
}

void cm_byte_buf_push(CmByteBuf *buffer, unsigned char byte)
{
    cm_byte_buf_append(buffer, &byte, 1);
}

void cm_str_buf_init(CmStrBuf *buffer)
{
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

void cm_str_buf_destroy(CmStrBuf *buffer)
{
    cm_free(buffer->data);
    cm_str_buf_init(buffer);
}

void cm_str_buf_clear(CmStrBuf *buffer)
{
    buffer->len = 0;
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

void cm_str_buf_reserve(CmStrBuf *buffer, size_t minimum_capacity)
{
    size_t capacity;
    size_t allocation_size;

    if (minimum_capacity <= buffer->cap && buffer->data != NULL) {
        return;
    }
    capacity = cm_grow_capacity(buffer->cap, minimum_capacity);
    if (!cm_size_add(capacity, 1, &allocation_size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    buffer->data = (char *)cm_realloc(buffer->data, allocation_size);
    buffer->cap = capacity;
    buffer->data[buffer->len] = '\0';
}

void cm_str_buf_append_n(CmStrBuf *buffer, const char *text, size_t length)
{
    size_t new_length;
    size_t source_offset;
    int source_is_internal;
    uintptr_t source_address;
    uintptr_t buffer_address;

    if (length == 0) {
        return;
    }
    if (text == NULL) {
        abort();
    }
    if (!cm_size_add(buffer->len, length, &new_length)) {
        cm_alloc_out_of_memory((size_t)-1);
    }

    source_address = (uintptr_t)text;
    buffer_address = (uintptr_t)buffer->data;
    source_is_internal = buffer->data != NULL
        && source_address >= buffer_address
        && source_address <= buffer_address + buffer->len;
    source_offset = source_is_internal
        ? (size_t)(source_address - buffer_address)
        : 0;
    if (source_is_internal && length > buffer->len - source_offset) {
        abort();
    }

    cm_str_buf_reserve(buffer, new_length);
    if (source_is_internal) {
        text = buffer->data + source_offset;
    }
    memmove(buffer->data + buffer->len, text, length);
    buffer->len = new_length;
    buffer->data[buffer->len] = '\0';
}

void cm_str_buf_append(CmStrBuf *buffer, const char *text)
{
    if (text == NULL) {
        abort();
    }
    cm_str_buf_append_n(buffer, text, strlen(text));
}

void cm_str_buf_push(CmStrBuf *buffer, char byte)
{
    cm_str_buf_append_n(buffer, &byte, 1);
}

const char *cm_str_buf_c_str(const CmStrBuf *buffer)
{
    return buffer->data == NULL ? "" : buffer->data;
}
