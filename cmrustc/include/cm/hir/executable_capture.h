#ifndef CMRUSTC_CM_HIR_EXECUTABLE_CAPTURE_H
#define CMRUSTC_CM_HIR_EXECUTABLE_CAPTURE_H

#include "cm/hir/admission.h"
#include "cm/hir/artifact_config.h"
#include "cm/hir/executable_metadata.h"

typedef enum CmHirExecutableCaptureStatus {
    CM_HIR_EXEC_CAPTURE_OK = 0,
    CM_HIR_EXEC_CAPTURE_INVALID_ARGUMENT,
    CM_HIR_EXEC_CAPTURE_INVALID_AUTHORITY,
    CM_HIR_EXEC_CAPTURE_UNSUPPORTED_HIR,
    CM_HIR_EXEC_CAPTURE_METADATA_FAILURE
} CmHirExecutableCaptureStatus;

typedef struct CmHirExecutableCaptureInput {
    const CmHirContext *hir;
    CmHirCrateId crate_id;
    /* Must be a current whole-local REGIONS admission for hir/crate_id. */
    const CmSemanticAdmission *regions_admission;
    const CmHirArtifactConfig *configuration;
    CmHirArtifactBytes crate_disambiguator;
    const CmHirArtifactSourceEntry *source_entries;
    size_t source_entry_count;
    CmHirArtifactBytes archive_member_name;
    CmHirArtifactBytes object_bytes;
} CmHirExecutableCaptureInput;

typedef struct CmHirExecutableCaptureResult {
    CmHirExecutableCaptureStatus status;
    CmHirExecutableMetadataStatus metadata_status;
    CmHirItemId rejected_item;
    CmHirTypeId rejected_type;
    size_t trait_count;
    size_t impl_count;
    size_t recipe_count;
    size_t native_object_value_count;
} CmHirExecutableCaptureResult;

/*
 * Capture the exact bounded cmhir-meta-v3.2 executable profile from live HIR.
 * The admitted shape is deliberately structural: one root module, public
 * empty marker traits, positive primitive implementations, public one-type-
 * generic RETURN_ARGUMENT recipes, and public no_mangle C-ABI functions.
 * No declaration name has special meaning.  Every other local item, module,
 * reexport/import, type, ABI, attribute, or body shape is rejected.
 *
 * On success output owns its descriptor arrays and strings, while its source
 * entries and object bytes continue to borrow input storage.  Failure leaves
 * an already initialized output unchanged.
 */
CmHirExecutableCaptureResult cm_hir_executable_metadata_capture(
    const CmHirExecutableCaptureInput *input,
    CmHirExecutableMetadata *output);

const char *cm_hir_executable_capture_status_name(
    CmHirExecutableCaptureStatus status);

#endif
