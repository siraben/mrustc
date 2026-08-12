#include "cm/alloc.h"

#include <stdlib.h>

static CmOomHandler cm_oom_handler;
static void *cm_oom_context;
static size_t cm_calls_before_failure = (size_t)-1;

static int cm_allocation_should_fail(void)
{
    if (cm_calls_before_failure == (size_t)-1) {
        return 0;
    }
    if (cm_calls_before_failure == 0) {
        return 1;
    }
    cm_calls_before_failure -= 1;
    return 0;
}

void cm_alloc_set_oom_handler(CmOomHandler handler, void *context)
{
    cm_oom_handler = handler;
    cm_oom_context = context;
}

void cm_alloc_out_of_memory(size_t requested_size)
{
    CmOomHandler handler;
    void *context;

    handler = cm_oom_handler;
    context = cm_oom_context;
    if (handler != NULL) {
        handler(requested_size, context);
    }
    abort();
}

int cm_size_add(size_t left, size_t right, size_t *result)
{
    if (right > (size_t)-1 - left) {
        return 0;
    }
    if (result != NULL) {
        *result = left + right;
    }
    return 1;
}

int cm_size_mul(size_t left, size_t right, size_t *result)
{
    if (left != 0 && right > (size_t)-1 / left) {
        return 0;
    }
    if (result != NULL) {
        *result = left * right;
    }
    return 1;
}

void *cm_alloc(size_t size)
{
    void *allocation;
    size_t actual_size;

    actual_size = size == 0 ? 1 : size;
    allocation = NULL;
    if (!cm_allocation_should_fail()) {
        allocation = malloc(actual_size);
    }
    if (allocation == NULL) {
        cm_alloc_out_of_memory(size);
    }
    return allocation;
}

void *cm_alloc_zeroed(size_t count, size_t element_size)
{
    void *allocation;
    size_t size;
    size_t actual_size;

    if (!cm_size_mul(count, element_size, &size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    actual_size = size == 0 ? 1 : size;
    allocation = NULL;
    if (!cm_allocation_should_fail()) {
        allocation = calloc(1, actual_size);
    }
    if (allocation == NULL) {
        cm_alloc_out_of_memory(size);
    }
    return allocation;
}

void *cm_realloc(void *allocation, size_t size)
{
    void *new_allocation;
    size_t actual_size;

    actual_size = size == 0 ? 1 : size;
    new_allocation = NULL;
    if (!cm_allocation_should_fail()) {
        new_allocation = realloc(allocation, actual_size);
    }
    if (new_allocation == NULL) {
        cm_alloc_out_of_memory(size);
    }
    return new_allocation;
}

void cm_free(void *allocation)
{
    free(allocation);
}

void cm_alloc_fail_after(size_t successful_calls)
{
    cm_calls_before_failure = successful_calls;
}

void cm_alloc_fail_never(void)
{
    cm_calls_before_failure = (size_t)-1;
}
