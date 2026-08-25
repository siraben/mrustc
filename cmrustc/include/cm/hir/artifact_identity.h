#ifndef CMRUSTC_CM_HIR_ARTIFACT_IDENTITY_H
#define CMRUSTC_CM_HIR_ARTIFACT_IDENTITY_H

#include "cm/hir/model.h"
#include "cm/sha256.h"

#include <stdint.h>

#define CM_HIR_ARTIFACT_MAX_NAME_SIZE ((size_t)255u)
#define CM_HIR_ARTIFACT_MAX_DISAMBIGUATOR_SIZE ((size_t)255u)
#define CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE ((size_t)65535u)
#define CM_HIR_ARTIFACT_MAX_CFG_SIZE ((size_t)65535u)
#define CM_HIR_ARTIFACT_MAX_CFG_COUNT ((size_t)4096u)
#define CM_HIR_ARTIFACT_MAX_DEPENDENCY_COUNT ((size_t)4096u)
#define CM_HIR_ARTIFACT_MAX_SOURCE_COUNT ((size_t)65536u)
#define CM_HIR_ARTIFACT_MAX_SOURCE_PATH_SIZE ((size_t)4096u)
#define CM_HIR_ARTIFACT_MAX_SOURCE_FILE_SIZE ((size_t)134217728u)
#define CM_HIR_ARTIFACT_MAX_SOURCE_CLOSURE_SIZE ((size_t)536870912u)

typedef enum CmHirArtifactIdentityStatus {
    CM_HIR_ARTIFACT_IDENTITY_OK = 0,
    CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT,
    CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED,
    CM_HIR_ARTIFACT_IDENTITY_INVALID_SOURCE_PATH,
    CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_CFG,
    CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_DEPENDENCIES,
    CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_SOURCES
} CmHirArtifactIdentityStatus;

typedef struct CmHirArtifactBytes {
    const void *data;
    size_t length;
} CmHirArtifactBytes;

typedef struct CmHirArtifactDigest {
    unsigned char bytes[CM_HIR_ARTIFACT_IDENTITY_SIZE];
} CmHirArtifactDigest;

typedef struct CmHirArtifactSourceEntry {
    CmHirArtifactBytes logical_path;
    CmHirArtifactBytes contents;
} CmHirArtifactSourceEntry;

typedef struct CmHirArtifactIdentityInput {
    uint32_t schema_major;
    uint32_t schema_minor;
    uint32_t profile;
    CmHirArtifactBytes crate_name;
    CmHirArtifactBytes crate_disambiguator;
    uint32_t edition;
    CmHirArtifactBytes target_descriptor;
    CmHirArtifactBytes panic_strategy;
    const CmHirArtifactBytes *cfgs;
    size_t cfg_count;
    CmHirArtifactDigest source_closure;
    const CmHirArtifactDigest *dependency_identities;
    size_t dependency_count;
} CmHirArtifactIdentityInput;

/*
 * Hash a source closure using normalized, relative logical paths and exact
 * file bytes. Entries must be in strict bytewise path order. Physical roots,
 * mtimes, ownership, and other filesystem metadata are intentionally absent.
 * Every hashed field is encoded as a one-byte tag, a big-endian u64 payload
 * length, then the payload; integer payloads are fixed-width big-endian.
 * `out_digest` is changed only on success.
 */
CmHirArtifactIdentityStatus cm_hir_artifact_source_closure_digest(
    const CmHirArtifactSourceEntry *sources, size_t source_count,
    CmHirArtifactDigest *out_digest);

/*
 * Hash the complete canonical G3 artifact identity. Cfg strings and dependency
 * digests must each be in strict bytewise order. `out_identity` is changed
 * only on success. It uses the same explicit field framing as the source
 * closure and hashes the fields in structure declaration order.
 */
CmHirArtifactIdentityStatus cm_hir_artifact_identity_compute(
    const CmHirArtifactIdentityInput *input,
    CmHirArtifactDigest *out_identity);

const char *cm_hir_artifact_identity_status_name(
    CmHirArtifactIdentityStatus status);

#endif
