#ifndef CMRUSTC_CM_HIR_EXECUTABLE_RLIB_H
#define CMRUSTC_CM_HIR_EXECUTABLE_RLIB_H

#include "cm/hir/executable_metadata.h"
#include "cm/rlib.h"

#define CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER "cmrustc.rmeta"
#define CM_HIR_EXECUTABLE_RLIB_MAX_OBJECTS \
    (CM_RLIB_MAX_MEMBER_COUNT - (size_t)1u)

typedef enum CmHirExecutableRlibStatus {
    CM_HIR_EXECUTABLE_RLIB_OK = 0,
    CM_HIR_EXECUTABLE_RLIB_INVALID_ARGUMENT,
    CM_HIR_EXECUTABLE_RLIB_LIMIT_EXCEEDED,
    CM_HIR_EXECUTABLE_RLIB_UNSUPPORTED_DESCRIPTOR,
    CM_HIR_EXECUTABLE_RLIB_INVALID_ARCHIVE,
    CM_HIR_EXECUTABLE_RLIB_INVALID_METADATA,
    CM_HIR_EXECUTABLE_RLIB_IDENTITY_MISMATCH,
    CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH,
    CM_HIR_EXECUTABLE_RLIB_OBJECT_MISMATCH
} CmHirExecutableRlibStatus;

/*
 * The name is stored inline, so an individual view can be copied safely.
 * `data` borrows the archive passed to cm_hir_executable_rlib_decode and
 * remains valid only while those archive bytes remain unchanged and alive.
 */
typedef struct CmHirExecutableRlibObjectView {
    char archive_member_name[CM_RLIB_MAX_MEMBER_NAME + 1u];
    const unsigned char *data;
    size_t length;
} CmHirExecutableRlibObjectView;

/*
 * The decoded metadata is owning. Object views borrow the input archive and
 * are ordered exactly as the metadata LINK object records. Initialize before
 * first use. A shallow copy of this owning aggregate must not be destroyed.
 */
typedef struct CmHirExecutableRlib {
    CmHirExecutableMetadata metadata;
    CmHirExecutableRlibObjectView
        objects[CM_HIR_EXECUTABLE_RLIB_MAX_OBJECTS];
    size_t object_count;
} CmHirExecutableRlib;

void cm_hir_executable_rlib_init(CmHirExecutableRlib *rlib);
void cm_hir_executable_rlib_destroy(CmHirExecutableRlib *rlib);

/*
 * Encode exactly `cmrustc.rmeta` and the object bytes declared by LINK.
 * Metadata/object validation is delegated to the v3.2 codec; `output` is
 * replaced only after both metadata and the deterministic archive succeed.
 */
CmHirExecutableRlibStatus cm_hir_executable_rlib_encode(
    const CmHirExecutableMetadata *metadata, CmByteBuf *output);

/*
 * Validate the deterministic archive, decode v3.2 metadata using
 * `expectation`, and verify the exact member set, lengths, and SHA-256
 * digests. `output` is replaced only after every check succeeds.
 */
CmHirExecutableRlibStatus cm_hir_executable_rlib_decode(
    const void *archive, size_t archive_length,
    const CmHirExecutableMetadataExpectation *expectation,
    CmHirExecutableRlib *output);

const char *cm_hir_executable_rlib_status_name(
    CmHirExecutableRlibStatus status);

#endif
