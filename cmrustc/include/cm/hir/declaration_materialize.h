#ifndef CMRUSTC_CM_HIR_DECLARATION_MATERIALIZE_H
#define CMRUSTC_CM_HIR_DECLARATION_MATERIALIZE_H

#include "cm/hir/declaration_metadata.h"
#include "cm/hir/library.h"

typedef enum CmHirDeclarationMaterializeStatus {
    CM_HIR_DECL_MATERIALIZE_OK = 0,
    CM_HIR_DECL_MATERIALIZE_INVALID_ARGUMENT,
    CM_HIR_DECL_MATERIALIZE_INVALID_METADATA,
    CM_HIR_DECL_MATERIALIZE_HIR_FAILURE,
    CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
} CmHirDeclarationMaterializeStatus;

typedef struct CmHirDeclarationMaterializeResult {
    CmHirDeclarationMaterializeStatus status;
    CmHirDeclarationMetadataStatus metadata_status;
    CmHirStatus hir_status;
    CmHirLibraryStatus library_status;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    size_t module_count;
    size_t item_count;
    size_t public_type_entry_count;
    size_t public_value_entry_count;
} CmHirDeclarationMaterializeResult;

typedef struct CmHirDeclarationMaterializeExpectation {
    CmHirDeclarationString crate_name;
    CmHirDeclarationString crate_disambiguator;
    uint8_t edition;
    CmHirDeclarationString target_triple;
    CmHirDeclarationString data_layout;
    uint8_t panic_strategy;
    const CmHirDeclarationString *cfgs;
    size_t cfg_count;
} CmHirDeclarationMaterializeExpectation;

/*
 * Revalidate and transactionally materialize one exact bounded v3.0
 * declaration descriptor.  Success appends a complete foreign declaration
 * crate to `context` and atomically replaces `artifact`.  Failure rewinds all
 * HIR/interner appends and leaves the caller's artifact unchanged.
 */
CmHirDeclarationMaterializeResult cm_hir_declaration_metadata_materialize(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationMaterializeExpectation *expectation,
    const char *extern_name, CmSourceId metadata_source);

const char *cm_hir_declaration_materialize_status_name(
    CmHirDeclarationMaterializeStatus status);

#endif
