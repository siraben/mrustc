#ifndef CMRUSTC_CM_HIR_LIBRARY_H
#define CMRUSTC_CM_HIR_LIBRARY_H

#include "cm/hir/model.h"
#include "cm/hir/module_map.h"
#include "cm/resolve/imports.h"

typedef enum CmHirLibraryStatus {
    CM_HIR_LIBRARY_OK = 0,
    CM_HIR_LIBRARY_INVALID_ARGUMENT,
    CM_HIR_LIBRARY_FAILED_GRAPH,
    CM_HIR_LIBRARY_STALE_REVISION,
    CM_HIR_LIBRARY_INVALID_HIR,
    CM_HIR_LIBRARY_NOT_FOUND,
    CM_HIR_LIBRARY_WRONG_NAMESPACE,
    CM_HIR_LIBRARY_UNSUPPORTED_IMPORT,
    CM_HIR_LIBRARY_AMBIGUOUS
} CmHirLibraryStatus;

typedef struct CmHirLibraryArtifactResult {
    CmHirLibraryStatus status;
    size_t module_count;
    size_t public_type_entry_count;
    size_t public_value_entry_count;
} CmHirLibraryArtifactResult;

typedef struct CmHirLibraryPathSegment {
    const unsigned char *bytes;
    size_t length;
} CmHirLibraryPathSegment;

typedef struct CmHirLibraryType {
    CmHirDefId definition;
    CmHirTypeKind kind;
    CmHirPrimitiveKind primitive_kind;
} CmHirLibraryType;

typedef enum CmHirLibraryValueKind {
    CM_HIR_LIBRARY_VALUE_NONE = 0,
    CM_HIR_LIBRARY_VALUE_FUNCTION,
    CM_HIR_LIBRARY_VALUE_CONST,
    CM_HIR_LIBRARY_VALUE_STATIC
} CmHirLibraryValueKind;

/*
 * Declaration-only callable shape. Parameter patterns and bodies are not
 * part of a cross-crate function signature. `parameter_types`, the generic
 * parameter range, predicate arrays, and the ABI intern ID are borrowed from
 * the artifact's identity context.
 */
typedef struct CmHirLibraryFunctionSignature {
    const CmHirTypeId *parameter_types;
    uint32_t parameter_count;
    CmHirTypeId return_type;
    CmHirGenericParamId generic_parameter_start;
    uint32_t generic_parameter_count;
    const CmHirPredicateScope *predicate_scopes;
    uint32_t predicate_scope_count;
    const CmHirTraitPredicate *predicates;
    uint32_t predicate_count;
    const CmHirOutlivesPredicate *outlives_predicates;
    uint32_t outlives_predicate_count;
    CmInternId abi;
    CmHirSafety safety;
    int is_const;
    int is_async;
    int is_variadic;
} CmHirLibraryFunctionSignature;

/*
 * Authenticated value-namespace declaration. This first declaration slice
 * never carries a body, MIR, initializer, evaluated const, or link object.
 */
typedef struct CmHirLibraryValue {
    CmHirDefId definition;
    CmHirLibraryValueKind kind;
    union {
        CmHirLibraryFunctionSignature function;
        struct {
            CmHirTypeId type;
            CmHirMutability mutability;
        } value;
    } data;
} CmHirLibraryValue;

typedef enum CmHirLibraryBindingKind {
    CM_HIR_LIBRARY_BINDING_TYPE = 0,
    CM_HIR_LIBRARY_BINDING_MODULE,
    CM_HIR_LIBRARY_BINDING_TRAIT,
    CM_HIR_LIBRARY_BINDING_PRIMITIVE,
    CM_HIR_LIBRARY_BINDING_VALUE
} CmHirLibraryBindingKind;

typedef struct CmHirLibraryBinding {
    CmHirLibraryBindingKind kind;
    CmHirDefId definition;
    CmHirTypeKind type_kind;
    CmHirPrimitiveKind primitive_kind;
    CmHirLibraryValueKind value_kind;
} CmHirLibraryBinding;

typedef struct CmHirLibraryImport {
    CmModuleId consumer_module;
    CmResolveItemRef import_declaration;
    CmHirLibraryBinding binding;
} CmHirLibraryImport;

typedef struct CmHirLibraryArtifact {
    void *state;
} CmHirLibraryArtifact;

typedef struct CmHirLibraryArtifactIdentity {
    const CmHirContext *context;
    CmHirCrateId crate_id;
    CmHirDefId root_definition;
    const char *extern_name;
} CmHirLibraryArtifactIdentity;

void cm_hir_library_artifact_init(CmHirLibraryArtifact *artifact);
void cm_hir_library_artifact_destroy(CmHirLibraryArtifact *artifact);

/*
 * Copies the exact public module/type namespace from one successfully lowered
 * graph revision. The graph, its ASTs, imports, and the module map may then be
 * destroyed. Referenced DefIds remain owned by `context`, which must outlive
 * the artifact and every consumer that uses it.
 *
 * This legacy artifact slice publishes modules, traits, ADTs, type aliases,
 * extern types, and builtin primitives, including same-crate public
 * type-namespace reexports. Values are intentionally omitted.
 */
CmHirLibraryArtifactResult cm_hir_library_artifact_build(
    CmHirLibraryArtifact *artifact, const CmHirContext *context,
    CmHirCrateId crate_id, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirModuleMap *modules,
    const char *extern_name);

/*
 * Builds the declaration-v2 library view. In addition to the legacy type
 * namespace, it authenticates predicate-free public free functions, consts,
 * and statics plus same-crate value reexports. Bodies, evaluated consts,
 * external reexports, globs, macros, predicates, and link objects are not
 * represented.
 */
CmHirLibraryArtifactResult cm_hir_library_declaration_artifact_build(
    CmHirLibraryArtifact *artifact, const CmHirContext *context,
    CmHirCrateId crate_id, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirModuleMap *modules,
    const char *extern_name);

/* Returns a borrowed identity view for a live, nonempty artifact. */
int cm_hir_library_artifact_identity(const CmHirLibraryArtifact *artifact,
    CmHirLibraryArtifactIdentity *out_identity);

/* Resolves any exact public type-namespace binding. */
CmHirLibraryStatus cm_hir_library_artifact_lookup_binding(
    const CmHirLibraryArtifact *artifact,
    const CmHirLibraryPathSegment *segments, size_t segment_count,
    CmHirLibraryBinding *out_binding);

/* Resolves an exact public path beginning with the configured extern name. */
CmHirLibraryStatus cm_hir_library_artifact_lookup_type(
    const CmHirLibraryArtifact *artifact,
    const CmHirLibraryPathSegment *segments, size_t segment_count,
    CmHirLibraryType *out_type);

/* Resolves one exact public value-namespace declaration. */
CmHirLibraryStatus cm_hir_library_artifact_lookup_value(
    const CmHirLibraryArtifact *artifact,
    const CmHirLibraryPathSegment *segments, size_t segment_count,
    CmHirLibraryValue *out_value);

/*
 * Authenticates one unresolved, non-glob consumer type-namespace use-tree
 * leaf by its local name. The exact external path must resolve to a type,
 * trait, or module through this artifact. Local bindings and competing
 * unresolved leaves reject as ambiguous.
 */
CmHirLibraryStatus cm_hir_library_artifact_resolve_import(
    const CmHirLibraryArtifact *artifact, const CmImportResolver *imports,
    const CmModuleGraph *consumer,
    CmModuleGraphRevision consumer_revision, CmModuleId consumer_module,
    const CmHirLibraryPathSegment *local_name,
    CmHirLibraryImport *out_import);

/* Resolves a type below one exactly authenticated imported module alias. */
CmHirLibraryStatus cm_hir_library_artifact_resolve_imported_type(
    const CmHirLibraryArtifact *artifact, const CmImportResolver *imports,
    const CmModuleGraph *consumer,
    CmModuleGraphRevision consumer_revision, CmModuleId consumer_module,
    const CmHirLibraryPathSegment *local_module_name,
    const CmHirLibraryPathSegment *suffix, size_t suffix_count,
    CmHirLibraryType *out_type);

/* Resolves any binding below one authenticated imported module alias. */
CmHirLibraryStatus cm_hir_library_artifact_resolve_imported_binding(
    const CmHirLibraryArtifact *artifact, const CmImportResolver *imports,
    const CmModuleGraph *consumer,
    CmModuleGraphRevision consumer_revision, CmModuleId consumer_module,
    const CmHirLibraryPathSegment *local_module_name,
    const CmHirLibraryPathSegment *suffix, size_t suffix_count,
    CmHirLibraryBinding *out_binding);

const char *cm_hir_library_status_name(CmHirLibraryStatus status);

#endif
