#ifndef CMRUSTC_CM_HIR_EXECUTABLE_MATERIALIZE_H
#define CMRUSTC_CM_HIR_EXECUTABLE_MATERIALIZE_H

#include "cm/hir/executable_metadata.h"
#include "cm/hir/library.h"

typedef enum CmHirExecutableMaterializeStatus {
    CM_HIR_EXEC_MATERIALIZE_OK = 0,
    CM_HIR_EXEC_MATERIALIZE_INVALID_ARGUMENT,
    CM_HIR_EXEC_MATERIALIZE_INVALID_METADATA,
    CM_HIR_EXEC_MATERIALIZE_HIR_FAILURE,
    CM_HIR_EXEC_MATERIALIZE_ARTIFACT_FAILURE
} CmHirExecutableMaterializeStatus;

typedef struct CmHirExecutableMaterializeResult {
    CmHirExecutableMaterializeStatus status;
    CmHirExecutableMetadataStatus metadata_status;
    CmHirStatus hir_status;
    CmHirLibraryStatus library_status;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    size_t module_count;
    size_t public_type_entry_count;
    size_t public_value_entry_count;
} CmHirExecutableMaterializeResult;

/*
 * Revalidate and transactionally materialize one exact cmhir-meta-v3.2
 * executable descriptor. Success appends a foreign crate, its marker-trait
 * universe, executable recipe bodies, and declarations to `context`, then
 * atomically replaces `artifact`. Failure rewinds every HIR append and leaves
 * the caller's artifact unchanged.
 *
 * `metadata_source` is the nonzero synthetic source identity assigned to the
 * authenticated descriptor. The artifact identity remains the authority for
 * recipe provenance; no source expression identity is synthesized.
 */
CmHirExecutableMaterializeResult cm_hir_executable_metadata_materialize(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const CmHirExecutableMetadata *metadata, const char *extern_name,
    CmSourceId metadata_source);

const char *cm_hir_executable_materialize_status_name(
    CmHirExecutableMaterializeStatus status);

#endif
