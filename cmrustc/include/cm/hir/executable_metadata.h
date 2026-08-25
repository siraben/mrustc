#ifndef CMRUSTC_CM_HIR_EXECUTABLE_METADATA_H
#define CMRUSTC_CM_HIR_EXECUTABLE_METADATA_H

#include "cm/buf.h"
#include "cm/hir/artifact_identity.h"

#include <stdint.h>

#define CM_HIR_EXEC_METADATA_MAJOR UINT16_C(3)
#define CM_HIR_EXEC_METADATA_MINOR UINT16_C(2)
#define CM_HIR_EXEC_METADATA_PROFILE UINT32_C(3)
#define CM_HIR_EXEC_METADATA_MAX_RECORDS ((size_t)131072u)
#define CM_HIR_EXEC_METADATA_MAX_PARAMETERS ((size_t)32u)

typedef enum CmHirExecutableMetadataStatus {
    CM_HIR_EXEC_METADATA_OK = 0,
    CM_HIR_EXEC_METADATA_INVALID_ARGUMENT,
    CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED,
    CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR,
    CM_HIR_EXEC_METADATA_INVALID_FORMAT,
    CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH
} CmHirExecutableMetadataStatus;

typedef struct CmHirExecutableString {
    unsigned char *data;
    size_t length;
} CmHirExecutableString;

typedef enum CmHirExecutablePrimitive {
    CM_HIR_EXEC_PRIMITIVE_BOOL = 1,
    CM_HIR_EXEC_PRIMITIVE_I8 = 2,
    CM_HIR_EXEC_PRIMITIVE_U8 = 3,
    CM_HIR_EXEC_PRIMITIVE_I16 = 4,
    CM_HIR_EXEC_PRIMITIVE_U16 = 5,
    CM_HIR_EXEC_PRIMITIVE_I32 = 6,
    CM_HIR_EXEC_PRIMITIVE_U32 = 7,
    CM_HIR_EXEC_PRIMITIVE_I64 = 8,
    CM_HIR_EXEC_PRIMITIVE_U64 = 9,
    CM_HIR_EXEC_PRIMITIVE_ISIZE = 10,
    CM_HIR_EXEC_PRIMITIVE_USIZE = 11,
    CM_HIR_EXEC_PRIMITIVE_F32 = 12,
    CM_HIR_EXEC_PRIMITIVE_F64 = 13
} CmHirExecutablePrimitive;

typedef enum CmHirExecutableTypeKind {
    CM_HIR_EXEC_TYPE_PRIMITIVE = 1,
    CM_HIR_EXEC_TYPE_VALUE_GENERIC = 2
} CmHirExecutableTypeKind;

typedef struct CmHirExecutableModule {
    uint32_t parent_module;
    CmHirExecutableString name;
} CmHirExecutableModule;

typedef struct CmHirExecutableTrait {
    uint32_t owner_module;
    CmHirExecutableString name;
    uint32_t source_ordinal;
} CmHirExecutableTrait;

typedef struct CmHirExecutableType {
    uint8_t kind;
    uint8_t primitive;
    uint32_t owner_value;
    uint32_t generic_index;
} CmHirExecutableType;

typedef struct CmHirExecutableImpl {
    uint32_t owner_module;
    uint32_t source_ordinal;
    uint32_t trait_local;
    uint32_t self_type;
} CmHirExecutableImpl;

typedef enum CmHirExecutableValueKind {
    CM_HIR_EXEC_VALUE_RECIPE = 1,
    CM_HIR_EXEC_VALUE_NATIVE_OBJECT = 2
} CmHirExecutableValueKind;

typedef struct CmHirExecutableValue {
    uint32_t owner_module;
    CmHirExecutableString name;
    uint32_t source_ordinal;
    uint8_t kind;
    CmHirExecutableString generic_name;
    uint32_t parameter_count;
    uint32_t *parameter_types;
    uint32_t return_type;
    uint32_t predicate_start;
    uint32_t predicate_count;
    uint32_t execution_local;
} CmHirExecutableValue;

typedef struct CmHirExecutablePredicate {
    uint32_t owner_value;
    uint32_t ordinal;
    uint32_t subject_type;
    uint32_t trait_local;
} CmHirExecutablePredicate;

typedef enum CmHirExecutableNamespaceKind {
    CM_HIR_EXEC_NAMESPACE_TYPE = 1,
    CM_HIR_EXEC_NAMESPACE_VALUE = 2
} CmHirExecutableNamespaceKind;

typedef enum CmHirExecutableNamespaceTarget {
    CM_HIR_EXEC_NAMESPACE_MODULE = 1,
    CM_HIR_EXEC_NAMESPACE_TRAIT = 2,
    CM_HIR_EXEC_NAMESPACE_VALUE_TARGET = 3
} CmHirExecutableNamespaceTarget;

typedef struct CmHirExecutableNamespaceEntry {
    uint32_t owner_module;
    uint8_t namespace_kind;
    CmHirExecutableString name;
    uint8_t target_kind;
    uint32_t target_local;
    uint32_t export_ordinal;
} CmHirExecutableNamespaceEntry;

typedef struct CmHirExecutableBody {
    uint32_t owner_value;
    uint32_t parameter_index;
    uint32_t parameter_type;
    uint32_t return_type;
} CmHirExecutableBody;

typedef struct CmHirExecutableLinkObject {
    CmHirExecutableString archive_member_name;
    uint64_t byte_length;
    const void *object_bytes;
    size_t object_bytes_length;
    CmHirArtifactDigest object_digest;
    uint32_t symbol_start;
    uint32_t symbol_count;
} CmHirExecutableLinkObject;

typedef struct CmHirExecutableLinkSymbol {
    uint32_t owner_value;
    uint32_t object_local;
    CmHirExecutableString external_symbol;
} CmHirExecutableLinkSymbol;

typedef struct CmHirExecutableMetadata {
    CmHirExecutableString crate_name;
    CmHirExecutableString crate_disambiguator;
    uint32_t edition;
    CmHirExecutableString target_descriptor;
    CmHirExecutableString panic_strategy;
    CmHirExecutableString *cfgs;
    size_t cfg_count;
    const CmHirArtifactSourceEntry *source_entries;
    size_t source_entry_count;
    CmHirArtifactDigest source_digest;
    CmHirArtifactDigest link_manifest_digest;
    CmHirArtifactDigest artifact_identity;
    CmHirExecutableModule *modules;
    size_t module_count;
    CmHirExecutableTrait *traits;
    size_t trait_count;
    CmHirExecutableType *types;
    size_t type_count;
    CmHirExecutableImpl *impls;
    size_t impl_count;
    CmHirExecutableValue *values;
    size_t value_count;
    CmHirExecutablePredicate *predicates;
    size_t predicate_count;
    CmHirExecutableNamespaceEntry *namespace_entries;
    size_t namespace_count;
    CmHirExecutableBody *bodies;
    size_t body_count;
    CmHirExecutableLinkObject *objects;
    size_t object_count;
    CmHirExecutableLinkSymbol *symbols;
    size_t symbol_count;
    int owns_storage;
} CmHirExecutableMetadata;

typedef struct CmHirExecutableMetadataExpectation {
    CmHirExecutableString crate_name;
    CmHirExecutableString crate_disambiguator;
    uint32_t edition;
    CmHirExecutableString target_descriptor;
    CmHirExecutableString panic_strategy;
    const CmHirExecutableString *cfgs;
    size_t cfg_count;
    CmHirArtifactDigest source_digest;
    CmHirArtifactDigest artifact_identity;
} CmHirExecutableMetadataExpectation;

void cm_hir_executable_metadata_init(CmHirExecutableMetadata *metadata);
void cm_hir_executable_metadata_destroy(CmHirExecutableMetadata *metadata);

/*
 * The descriptor uses one-based local references. Inputs must already be in
 * the canonical structural order specified by cmhir-meta-v3.2. Encoding and
 * decoding replace their output only after complete validation. This exact
 * executable slice admits only one defining namespace entry per transported
 * module, trait, and value; a producer that encounters a reexport rejects the
 * descriptor instead of silently dropping or synthesizing that binding.
 */
CmHirExecutableMetadataStatus cm_hir_executable_metadata_encode(
    const CmHirExecutableMetadata *metadata, CmByteBuf *output);
CmHirExecutableMetadataStatus cm_hir_executable_metadata_compute_identity(
    const CmHirExecutableMetadata *metadata,
    CmHirArtifactDigest *out_link_manifest_digest,
    CmHirArtifactDigest *out_artifact_identity);
CmHirExecutableMetadataStatus cm_hir_executable_metadata_decode(
    const void *encoded, size_t encoded_length,
    const CmHirExecutableMetadataExpectation *expectation,
    CmHirExecutableMetadata *output);

const char *cm_hir_executable_metadata_status_name(
    CmHirExecutableMetadataStatus status);

#endif
