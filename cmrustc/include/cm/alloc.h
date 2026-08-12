#ifndef CMRUSTC_CM_ALLOC_H
#define CMRUSTC_CM_ALLOC_H

#include "cm/config.h"

typedef void (*CmOomHandler)(size_t requested_size, void *context);

/*
 * All allocation functions either return usable storage or invoke the OOM
 * handler and abort.  A handler may escape with longjmp for a driver-owned
 * fatal-error boundary or for fault-injection tests.
 */
void cm_alloc_set_oom_handler(CmOomHandler handler, void *context);
void cm_alloc_out_of_memory(size_t requested_size) CM_NORETURN;

void *cm_alloc(size_t size);
void *cm_alloc_zeroed(size_t count, size_t element_size);
void *cm_realloc(void *allocation, size_t size);
void cm_free(void *allocation);

int cm_size_add(size_t left, size_t right, size_t *result);
int cm_size_mul(size_t left, size_t right, size_t *result);

/* Test hook: fail after this many successful allocation calls. */
void cm_alloc_fail_after(size_t successful_calls);
void cm_alloc_fail_never(void);

#endif
