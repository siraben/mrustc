#ifndef CMRUSTC_CM_BUF_H
#define CMRUSTC_CM_BUF_H

#include "cm/config.h"

typedef struct CmByteBuf {
    unsigned char *data;
    size_t len;
    size_t cap;
} CmByteBuf;

void cm_byte_buf_init(CmByteBuf *buffer);
void cm_byte_buf_destroy(CmByteBuf *buffer);
void cm_byte_buf_clear(CmByteBuf *buffer);
void cm_byte_buf_reserve(CmByteBuf *buffer, size_t minimum_capacity);
void cm_byte_buf_resize(CmByteBuf *buffer, size_t length);
void cm_byte_buf_append(CmByteBuf *buffer, const void *data, size_t length);
void cm_byte_buf_push(CmByteBuf *buffer, unsigned char byte);

/* String buffers always have a trailing NUL after the first allocation. */
typedef struct CmStrBuf {
    char *data;
    size_t len;
    size_t cap;
} CmStrBuf;

void cm_str_buf_init(CmStrBuf *buffer);
void cm_str_buf_destroy(CmStrBuf *buffer);
void cm_str_buf_clear(CmStrBuf *buffer);
void cm_str_buf_reserve(CmStrBuf *buffer, size_t minimum_capacity);
void cm_str_buf_append_n(CmStrBuf *buffer, const char *text, size_t length);
void cm_str_buf_append(CmStrBuf *buffer, const char *text);
void cm_str_buf_push(CmStrBuf *buffer, char byte);
const char *cm_str_buf_c_str(const CmStrBuf *buffer);

#endif
