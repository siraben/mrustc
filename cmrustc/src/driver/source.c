#include "cm/source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cm_size_add_overflow(size_t left, size_t right, size_t *result)
{
    if (left > (size_t)-1 - right) {
        return 1;
    }
    *result = left + right;
    return 0;
}

static char *cm_duplicate_string(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL) {
        return NULL;
    }
    length = strlen(value);
    if (length == (size_t)-1) {
        return NULL;
    }
    copy = malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, value, length + 1u);
    }
    return copy;
}

static void cm_source_file_destroy(CmSourceFile *file)
{
    free(file->line_starts);
    free(file->bytes);
    free(file->path);
    memset(file, 0, sizeof(*file));
}

static CmSourceStatus cm_build_line_starts(CmSourceFile *file)
{
    size_t index;
    size_t count;
    size_t write_index;

    count = 1u;
    for (index = 0u; index < file->length; ++index) {
        if (file->bytes[index] == (unsigned char)'\n') {
            if (count == (size_t)-1) {
                return CM_SOURCE_TOO_LARGE;
            }
            ++count;
        }
    }
    if (count > (size_t)UINT32_MAX) {
        return CM_SOURCE_TOO_LARGE;
    }
    if (count > (size_t)-1 / sizeof(file->line_starts[0])) {
        return CM_SOURCE_TOO_LARGE;
    }
    file->line_starts = malloc(count * sizeof(file->line_starts[0]));
    if (file->line_starts == NULL) {
        return CM_SOURCE_OUT_OF_MEMORY;
    }
    file->line_starts[0] = 0u;
    write_index = 1u;
    for (index = 0u; index < file->length; ++index) {
        if (file->bytes[index] == (unsigned char)'\n') {
            file->line_starts[write_index] = (uint32_t)(index + 1u);
            ++write_index;
        }
    }
    file->line_count = count;
    return CM_SOURCE_OK;
}

static CmSourceStatus cm_source_reserve(CmSourceSet *set, size_t needed)
{
    size_t capacity;
    CmSourceFile *files;

    if (needed <= set->capacity) {
        return CM_SOURCE_OK;
    }
    capacity = set->capacity == 0u ? 4u : set->capacity;
    while (capacity < needed) {
        if (capacity > (size_t)-1 / 2u) {
            return CM_SOURCE_TOO_LARGE;
        }
        capacity *= 2u;
    }
    if (capacity > (size_t)-1 / sizeof(set->files[0])) {
        return CM_SOURCE_TOO_LARGE;
    }
    files = realloc(set->files, capacity * sizeof(set->files[0]));
    if (files == NULL) {
        return CM_SOURCE_OUT_OF_MEMORY;
    }
    set->files = files;
    set->capacity = capacity;
    return CM_SOURCE_OK;
}

void cm_source_set_init(CmSourceSet *set)
{
    if (set != NULL) {
        memset(set, 0, sizeof(*set));
    }
}

void cm_source_set_destroy(CmSourceSet *set)
{
    size_t index;

    if (set == NULL) {
        return;
    }
    for (index = 0u; index < set->length; ++index) {
        cm_source_file_destroy(&set->files[index]);
    }
    free(set->files);
    memset(set, 0, sizeof(*set));
}

CmSourceStatus cm_source_add_memory(CmSourceSet *set, const char *path,
    const unsigned char *bytes, size_t length, CmSourceId *out_id)
{
    CmSourceStatus status;
    CmSourceFile file;
    size_t allocation_size;

    if (set == NULL || path == NULL || out_id == NULL ||
        (bytes == NULL && length != 0u)) {
        return CM_SOURCE_INVALID_ARGUMENT;
    }
    if (length > (size_t)UINT32_MAX || set->length >= (size_t)UINT32_MAX) {
        return CM_SOURCE_TOO_LARGE;
    }
    status = cm_source_reserve(set, set->length + 1u);
    if (status != CM_SOURCE_OK) {
        return status;
    }

    memset(&file, 0, sizeof(file));
    file.id = (CmSourceId)(set->length + 1u);
    file.path = cm_duplicate_string(path);
    if (file.path == NULL) {
        return CM_SOURCE_OUT_OF_MEMORY;
    }
    if (cm_size_add_overflow(length, 1u, &allocation_size)) {
        cm_source_file_destroy(&file);
        return CM_SOURCE_TOO_LARGE;
    }
    file.bytes = malloc(allocation_size);
    if (file.bytes == NULL) {
        cm_source_file_destroy(&file);
        return CM_SOURCE_OUT_OF_MEMORY;
    }
    if (length != 0u) {
        memcpy(file.bytes, bytes, length);
    }
    file.bytes[length] = 0u;
    file.length = length;
    status = cm_build_line_starts(&file);
    if (status != CM_SOURCE_OK) {
        cm_source_file_destroy(&file);
        return status;
    }

    set->files[set->length] = file;
    ++set->length;
    *out_id = file.id;
    return CM_SOURCE_OK;
}

CmSourceStatus cm_source_load_file_bounded(CmSourceSet *set,
    const char *path, size_t maximum_bytes, CmSourceId *out_id)
{
    FILE *stream;
    unsigned char *bytes;
    size_t length;
    size_t capacity;
    CmSourceStatus status;

    if (set == NULL || path == NULL || out_id == NULL) {
        return CM_SOURCE_INVALID_ARGUMENT;
    }
    if (maximum_bytes > (size_t)UINT32_MAX) {
        maximum_bytes = (size_t)UINT32_MAX;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        return CM_SOURCE_IO_ERROR;
    }
    bytes = NULL;
    length = 0u;
    capacity = 0u;
    status = CM_SOURCE_OK;
    for (;;) {
        size_t available;
        size_t amount;

        if (length == maximum_bytes) {
            int extra;

            extra = fgetc(stream);
            if (extra != EOF) {
                status = CM_SOURCE_TOO_LARGE;
            } else if (ferror(stream)) {
                status = CM_SOURCE_IO_ERROR;
            }
            break;
        }
        if (capacity == length) {
            size_t new_capacity;
            unsigned char *new_bytes;

            new_capacity = capacity == 0u ? 4096u : capacity * 2u;
            if (new_capacity < capacity || new_capacity > maximum_bytes) {
                new_capacity = maximum_bytes;
            }
            new_bytes = realloc(bytes, new_capacity);
            if (new_bytes == NULL) {
                status = CM_SOURCE_OUT_OF_MEMORY;
                break;
            }
            bytes = new_bytes;
            capacity = new_capacity;
        }
        available = capacity - length;
        amount = fread(bytes + length, 1u, available, stream);
        length += amount;
        if (amount < available) {
            if (ferror(stream)) {
                status = CM_SOURCE_IO_ERROR;
            }
            break;
        }
    }
    if (fclose(stream) != 0 && status == CM_SOURCE_OK) {
        status = CM_SOURCE_IO_ERROR;
    }
    if (status == CM_SOURCE_OK) {
        status = cm_source_add_memory(set, path, bytes, length, out_id);
    }
    free(bytes);
    return status;
}

CmSourceStatus cm_source_load_file(CmSourceSet *set, const char *path,
    CmSourceId *out_id)
{
    return cm_source_load_file_bounded(set, path, (size_t)UINT32_MAX,
        out_id);
}

const CmSourceFile *cm_source_get(const CmSourceSet *set, CmSourceId id)
{
    size_t index;

    if (set == NULL || id == 0u) {
        return NULL;
    }
    index = (size_t)(id - 1u);
    if (index >= set->length || set->files[index].id != id) {
        return NULL;
    }
    return &set->files[index];
}

int cm_span_is_valid(const CmSourceSet *set, CmSpan span)
{
    const CmSourceFile *file;

    file = cm_source_get(set, span.source);
    if (file == NULL || span.start > span.end) {
        return 0;
    }
    return (size_t)span.end <= file->length;
}

int cm_source_line_column(const CmSourceSet *set, CmSpan span,
    uint32_t *out_line, uint32_t *out_column)
{
    const CmSourceFile *file;
    size_t low;
    size_t high;
    size_t line_index;

    if (out_line == NULL || out_column == NULL ||
        !cm_span_is_valid(set, span)) {
        return 0;
    }
    file = cm_source_get(set, span.source);
    if (file == NULL) {
        return 0;
    }
    low = 0u;
    high = file->line_count;
    while (low + 1u < high) {
        size_t middle;

        middle = low + (high - low) / 2u;
        if (file->line_starts[middle] <= span.start) {
            low = middle;
        } else {
            high = middle;
        }
    }
    line_index = low;
    *out_line = (uint32_t)(line_index + 1u);
    *out_column = span.start - file->line_starts[line_index] + 1u;
    return 1;
}

const char *cm_source_status_name(CmSourceStatus status)
{
    switch (status) {
    case CM_SOURCE_OK:
        return "ok";
    case CM_SOURCE_OUT_OF_MEMORY:
        return "out of memory";
    case CM_SOURCE_IO_ERROR:
        return "I/O error";
    case CM_SOURCE_TOO_LARGE:
        return "source too large";
    case CM_SOURCE_INVALID_ARGUMENT:
        return "invalid argument";
    }
    return "unknown source error";
}
