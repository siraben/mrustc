#ifndef CMRUSTC_CM_HIR_DECLARATION_CAPTURE_H
#define CMRUSTC_CM_HIR_DECLARATION_CAPTURE_H

#include "cm/hir/artifact_config.h"
#include "cm/hir/declaration_metadata.h"
#include "cm/hir/library.h"
#include "cm/hir/module_map.h"

typedef enum CmHirDeclarationCaptureStatus {
    CM_HIR_DECL_CAPTURE_OK = 0,
    CM_HIR_DECL_CAPTURE_INVALID_ARGUMENT,
    CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY,
    CM_HIR_DECL_CAPTURE_LIBRARY_FAILURE,
    CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR,
    CM_HIR_DECL_CAPTURE_METADATA_FAILURE
} CmHirDeclarationCaptureStatus;

typedef struct CmHirDeclarationCaptureInput {
    const CmHirContext *hir;
    CmHirCrateId crate_id;
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    const CmImportResolver *imports;
    const CmHirModuleMap *modules;
    const CmHirArtifactConfig *configuration;
    CmHirArtifactBytes crate_disambiguator;
    CmHirArtifactBytes target_triple;
    CmHirArtifactBytes data_layout;
} CmHirDeclarationCaptureInput;

typedef struct CmHirDeclarationCaptureResult {
    CmHirDeclarationCaptureStatus status;
    CmHirDeclarationMetadataStatus metadata_status;
    CmHirLibraryStatus library_status;
    CmHirItemId rejected_item;
    CmHirTypeId rejected_type;
    size_t module_count;
    size_t trait_count;
    size_t value_count;
    size_t predicate_count;
    size_t namespace_count;
} CmHirDeclarationCaptureResult;

/*
 * Capture the exact first bounded v3.0 LOWER_SAFE declaration slice from one
 * successfully lowered graph revision.  Graph/import/module-map provenance
 * is the completeness authority; unsupported active public facts reject the
 * complete transaction.  No declaration name has special meaning.
 *
 * On success output owns all descriptor storage.  Failure leaves an already
 * initialized output unchanged.
 */
CmHirDeclarationCaptureResult cm_hir_declaration_metadata_capture(
    const CmHirDeclarationCaptureInput *input,
    CmHirDeclarationMetadata *output);

const char *cm_hir_declaration_capture_status_name(
    CmHirDeclarationCaptureStatus status);

#endif
