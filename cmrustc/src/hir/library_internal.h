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
    CmHirLibraryValueKind value_kind;
    CmHirLibraryBindingKind kind;
} CmHirLibraryOwnedEntry;

typedef struct CmHirLibraryOwnedModule {
    CmHirDefId definition;
    CmVec entries;

    /* Capture-only identities.  Restored metadata leaves both as none. */
    CmModuleId capture_graph_module;
    CmHirModuleId capture_hir_module;
} CmHirLibraryOwnedModule;

typedef struct CmHirLibraryOwnedValue {
    CmHirLibraryValue declaration;
    CmHirLibraryValueKind storage_kind;
    CmHirTypeId *parameter_types;
    uint32_t parameter_count;
    CmHirPredicateScope *predicate_scopes;
    CmInternId **predicate_scope_lifetimes;
    uint32_t predicate_scope_count;
    CmHirTraitPredicate *predicates;
    CmHirGenericArg **predicate_arguments;
    CmHirAssociatedTypeEquality **predicate_equalities;
    CmInternId **predicate_lifetimes;
    uint32_t predicate_count;
    CmHirOutlivesPredicate *outlives_predicates;
    uint32_t outlives_predicate_count;
    CmHirLibraryNominalReference *nominal_references;
    CmInternId *nominal_reference_names;
    CmHirGenericParamKind **nominal_reference_generic_kinds;
    uint32_t nominal_reference_count;
    CmHirLibraryAssociatedAvailability *associated_availability;
    uint32_t associated_availability_count;
} CmHirLibraryOwnedValue;

typedef struct CmHirLibraryOwnedData {
    CmInterner names;
    CmVec modules;
    CmVec values;
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
 * Copies one unique declaration, including nominal-reference name bytes and
 * all other nested function-signature data.
 */
CmHirLibraryStatus cm_hir_library_owned_data_add_value(
    CmHirLibraryOwnedData *data, const CmHirLibraryValue *value);

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
