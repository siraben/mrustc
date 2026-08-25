#ifndef CMRUSTC_CM_RLIB_H
#define CMRUSTC_CM_RLIB_H

#include "cm/buf.h"

#define CM_RLIB_MAX_METADATA_SIZE ((size_t)67108864u)
#define CM_RLIB_MAX_MEMBER_SIZE ((size_t)134217728u)
#define CM_RLIB_MAX_ARCHIVE_SIZE ((size_t)268435456u)
#define CM_RLIB_MAX_MEMBER_COUNT ((size_t)16u)
#define CM_RLIB_MAX_MEMBER_NAME ((size_t)15u)

typedef enum CmRlibStatus {
    CM_RLIB_OK = 0,
    CM_RLIB_INVALID_ARGUMENT,
    CM_RLIB_LIMIT_EXCEEDED,
    CM_RLIB_WRONG_MAGIC,
    CM_RLIB_TRUNCATED,
    CM_RLIB_INVALID_HEADER,
    CM_RLIB_WRONG_MEMBER,
    CM_RLIB_INVALID_PADDING,
    CM_RLIB_TRAILING_BYTES,
    CM_RLIB_INVALID_MEMBER_NAME,
    CM_RLIB_TOO_MANY_MEMBERS,
    CM_RLIB_NONCANONICAL_ORDER
} CmRlibStatus;

typedef struct CmRlibMember {
    const char *name;
    const void *data;
    size_t length;
} CmRlibMember;

typedef struct CmRlibMemberView {
    char name[CM_RLIB_MAX_MEMBER_NAME + 1u];
    const unsigned char *data;
    size_t length;
} CmRlibMemberView;

typedef struct CmRlibArchiveView {
    CmRlibMemberView members[CM_RLIB_MAX_MEMBER_COUNT];
    size_t member_count;
} CmRlibArchiveView;

typedef struct CmRlibMetadataView {
    const unsigned char *data;
    size_t length;
} CmRlibMetadataView;

/*
 * Encode a deterministic short-name SysV archive. Members must be in strict
 * bytewise name order and names use only ASCII letters, digits, '.', '_', and
 * '-'. The archive is replaced only on success.
 */
CmRlibStatus cm_rlib_encode_members(CmByteBuf *output,
    const CmRlibMember *members, size_t member_count);

/*
 * Validate and expose every member without allocation. The returned views
 * borrow `archive`; member order is required to be canonical.
 */
CmRlibStatus cm_rlib_decode_members(const void *archive,
    size_t archive_length, CmRlibArchiveView *out_archive);

CmRlibStatus cm_rlib_find_member(const CmRlibArchiveView *archive,
    const char *name, CmRlibMemberView *out_member);

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
