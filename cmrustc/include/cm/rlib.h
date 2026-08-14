#ifndef CMRUSTC_CM_RLIB_H
#define CMRUSTC_CM_RLIB_H

#include "cm/buf.h"

#define CM_RLIB_MAX_METADATA_SIZE ((size_t)67108864u)

typedef enum CmRlibStatus {
    CM_RLIB_OK = 0,
    CM_RLIB_INVALID_ARGUMENT,
    CM_RLIB_LIMIT_EXCEEDED,
    CM_RLIB_WRONG_MAGIC,
    CM_RLIB_TRUNCATED,
    CM_RLIB_INVALID_HEADER,
    CM_RLIB_WRONG_MEMBER,
    CM_RLIB_INVALID_PADDING,
    CM_RLIB_TRAILING_BYTES
} CmRlibStatus;

typedef struct CmRlibMetadataView {
    const unsigned char *data;
    size_t length;
} CmRlibMetadataView;

CmRlibStatus cm_rlib_encode_metadata(
    CmByteBuf *output,
    const void *metadata,
    size_t metadata_length
);
CmRlibStatus cm_rlib_decode_metadata(
    const void *archive,
    size_t archive_length,
    CmRlibMetadataView *out_metadata
);
const char *cm_rlib_status_name(CmRlibStatus status);

#endif
