#ifndef CMRUSTC_CM_HIR_METADATA_H
#define CMRUSTC_CM_HIR_METADATA_H

#include "cm/buf.h"
#include "cm/hir/library.h"

/*
 * Status for the semantic, declaration-only cmhir-meta-v1 boundary.  The
 * lower-level envelope codec deliberately remains an implementation detail.
 */
typedef enum CmHirMetadataArtifactStatus {
    CM_HIR_METADATA_ARTIFACT_OK = 0,
    CM_HIR_METADATA_ARTIFACT_INVALID_ARGUMENT,
    CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT,
    CM_HIR_METADATA_ARTIFACT_LIMIT_EXCEEDED,
    CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR,
    CM_HIR_METADATA_ARTIFACT_INVALID_HIR
} CmHirMetadataArtifactStatus;

typedef struct CmHirMetadataArtifactResult {
    CmHirMetadataArtifactStatus status;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    size_t module_count;
    size_t public_entry_count;
} CmHirMetadataArtifactResult;

/*
 * Encode one live library artifact into deterministic cmhir-meta-v1 bytes.
 * The declaration slice accepts modules, extern types, structs, unions,
 * enums, free type aliases, lifetime/type/const generics and type defaults,
 * their supported structural types, public aliases/reexports, and builtin
 * primitive bindings. Traits, impls, bodies, const-generic defaults,
 * projections, unevaluated constants, and dependency archives remain outside
 * this boundary. `output` is replaced on success and unchanged on failure.
 */
CmHirMetadataArtifactResult cm_hir_metadata_encode_artifact(
    CmByteBuf *output, const CmHirLibraryArtifact *artifact);

/*
 * Load one declaration-only artifact into `context`.  Runtime IDs are newly
 * allocated in that context; the wire contains file-local handles only.
 * `metadata_source` supplies normalized decoded spans and is not read from the
 * file.  Success replaces `artifact` under the caller's extern alias.  Any
 * failure leaves both the context and artifact unchanged.
 */
CmHirMetadataArtifactResult cm_hir_metadata_decode_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source);

/*
 * Explicit cmhir-meta-v1.1 semantic boundary. It adds authenticated,
 * monomorphic trait/impl facts to the v1.0 declaration payload. The semantic
 * API accepts exact v1.1 only. The encoded universe is always open:
 * transported presence may prove a goal, but absence never proves that an
 * implementation does not exist.
 */
CmHirMetadataArtifactResult cm_hir_metadata_encode_semantic_artifact(
    CmByteBuf *output, const CmHirLibraryArtifact *artifact);
CmHirMetadataArtifactResult cm_hir_metadata_decode_semantic_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source);

/*
 * Exact cmhir-meta-v2.6 declaration encoder, with strict v2.6, v2.5, v2.4,
 * then v2.3 decoder dispatch. In addition to v2.3 values, v2.4 transports the
 * bounded predicate shape used by generic public free functions: scope-free
 * trait predicates on direct function type parameters with type
 * arguments, zero-GAT associated-type
 * equalities, predicate-owned late-bound input regions, and type-subject
 * `'static` outlives predicates whose subjects are direct function type
 * parameters. v2.5 preserves the complete closed trait-predicate modifier
 * enum; modifierless v2.4 records decode as REQUIRED. v2.6 permits a direct
 * predicate identity to be an opaque trait alias, but transports no alias
 * expansion or alias-associated equality. Referenced traits, trait aliases,
 * and
 * associated types are restored as opaque REFERENCE_ONLY identities with
 * schemas and ownership facts; no fake item or namespace binding is created.
 * Predicate modifiers are structural declaration facts only: const-trait
 * eligibility, solving, projection, trait-alias expansion, scoped HRTBs,
 * bodies, MIR, evaluated constants, and link objects remain deferred or
 * unsupported.
 * Unsupported shapes reject the complete transaction, and output/context/
 * artifact sentinels remain unchanged on failure.
 */
CmHirMetadataArtifactResult cm_hir_metadata_encode_declaration_artifact(
    CmByteBuf *output, const CmHirLibraryArtifact *artifact);
CmHirMetadataArtifactResult cm_hir_metadata_decode_declaration_artifact(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const void *encoded, size_t encoded_length, const char *extern_name,
    CmSourceId metadata_source);

const char *cm_hir_metadata_artifact_status_name(
    CmHirMetadataArtifactStatus status);

#endif
