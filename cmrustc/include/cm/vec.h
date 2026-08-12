#ifndef CMRUSTC_CM_VEC_H
#define CMRUSTC_CM_VEC_H

#include "cm/config.h"

/*
 * Raw storage for typed vector wrappers.  elem_size is fixed at init time.
 * Values added by resize are zeroed; push_uninit is the explicit exception.
 */
typedef struct CmVec {
    unsigned char *data;
    size_t len;
    size_t cap;
    size_t elem_size;
} CmVec;

void cm_vec_init(CmVec *vector, size_t element_size);
void cm_vec_destroy(CmVec *vector);
void cm_vec_clear(CmVec *vector);
void cm_vec_reserve(CmVec *vector, size_t minimum_capacity);
void cm_vec_resize(CmVec *vector, size_t length);
void cm_vec_append(CmVec *vector, const void *items, size_t count);
void *cm_vec_push(CmVec *vector, const void *item);
void *cm_vec_push_uninit(CmVec *vector);
int cm_vec_pop(CmVec *vector, void *item_out);
void *cm_vec_at(CmVec *vector, size_t index);
const void *cm_vec_at_const(const CmVec *vector, size_t index);

#endif
