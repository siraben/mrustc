#ifndef CMRUSTC_HIR_LIBRARY_INTERNAL_H
#define CMRUSTC_HIR_LIBRARY_INTERNAL_H

#include "cm/hir/library.h"

/*
 * Owned, graph-independent namespace data.  A future metadata decoder can
 * construct this representation using only remapped, same-context DefIds.
 * Names are copied into `names`; callers never lend string storage.
 */
typedef struct CmHirLibraryOwnedEntry {
    CmInternId name;
    CmHirDefId target;
    CmHirTypeKind type_kind;
    CmHirPrimitiveKind primitive_kind;
    CmHirLibraryBindingKind kind;
} CmHirLibraryOwnedEntry;

typedef struct CmHirLibraryOwnedModule {
    CmHirDefId definition;
    CmVec entries;

    /* Capture-only identities.  Restored metadata leaves both as none. */
    CmModuleId capture_graph_module;
    CmHirModuleId capture_hir_module;
} CmHirLibraryOwnedModule;

typedef struct CmHirLibraryOwnedData {
    CmInterner names;
    CmVec modules;
} CmHirLibraryOwnedData;

void cm_hir_library_owned_data_init(CmHirLibraryOwnedData *data);
void cm_hir_library_owned_data_destroy(CmHirLibraryOwnedData *data);

/* Adds one unique module and returns its zero-based owned-data index. */
CmHirLibraryStatus cm_hir_library_owned_data_add_module(
    CmHirLibraryOwnedData *data, CmHirDefId definition,
    size_t *out_module_index);

/*
 * Copies `name` and adds one exact entry.  Repeating an identical entry is
 * idempotent; a same-name conflict is retained for restore-time rejection.
 */
CmHirLibraryStatus cm_hir_library_owned_data_add_entry(
    CmHirLibraryOwnedData *data, size_t module_index,
    const unsigned char *name, size_t name_length,
    const CmHirLibraryBinding *binding);

/*
 * Validate and install `data` over an already initialized artifact.  Success
 * moves all owned data and resets `data` to an empty initialized value.
 * Failure leaves both the artifact and `data` unchanged.
 */
CmHirLibraryArtifactResult cm_hir_library_artifact_restore_owned(
    CmHirLibraryArtifact *artifact, const CmHirContext *context,
    CmHirCrateId crate_id, CmHirDefId root_definition,
    const char *extern_name, CmHirLibraryOwnedData *data);

/* Borrowed semantic payload for process-independent metadata encoding. */
const CmHirLibraryOwnedData *cm_hir_library_artifact_owned_data_const(
    const CmHirLibraryArtifact *artifact);

#endif
