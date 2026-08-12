#ifndef CM_RESOLVE_DEPENDENCY_MACRO_H
#define CM_RESOLVE_DEPENDENCY_MACRO_H

#include "cm/resolve/imports.h"

typedef enum CmDependencyMacroStatus {
    CM_DEPENDENCY_MACRO_OK = 0,
    CM_DEPENDENCY_MACRO_INVALID_ARGUMENT,
    CM_DEPENDENCY_MACRO_FAILED_GRAPH,
    CM_DEPENDENCY_MACRO_IMPORT_ERROR,
    CM_DEPENDENCY_MACRO_STALE_REVISION,
    CM_DEPENDENCY_MACRO_NOT_FOUND,
    CM_DEPENDENCY_MACRO_AMBIGUOUS,
    CM_DEPENDENCY_MACRO_PRIVATE_PATH,
    CM_DEPENDENCY_MACRO_UNSUPPORTED_IMPORT,
    CM_DEPENDENCY_MACRO_UNSUPPORTED_DEFINITION
} CmDependencyMacroStatus;

typedef struct CmDependencyMacroArtifactResult {
    CmDependencyMacroStatus status;
    CmModuleGraphRevision revision;
    size_t import_error_count;
} CmDependencyMacroArtifactResult;

/*
 * One exact dependency-graph definition. The declaration's source ID belongs
 * only to dependency_graph at dependency_revision and must never be cast into
 * a consuming graph's source namespace. All pointers are borrowed from the
 * live artifact or dependency graph and expire on rebuild or destruction.
 */
typedef struct CmDependencyMacroDefinition {
    const CmModuleGraph *dependency_graph;
    CmModuleGraphRevision dependency_revision;
    CmResolveItemRef declaration;
    CmModuleId owner_module;
    const CmAst *definition_ast;
    CmAstMacroForm form;
    const char *extern_name;
    const char *crate_identifier;
} CmDependencyMacroDefinition;

/*
 * Exact cross-graph result for one consumer use-tree leaf. Consumer and
 * dependency source IDs remain in their respective graph namespaces.
 */
typedef struct CmDependencyMacroImport {
    const CmModuleGraph *consumer_graph;
    CmModuleGraphRevision consumer_revision;
    CmModuleId consumer_module;
    CmResolveItemRef import_declaration;
    CmDependencyMacroDefinition definition;
} CmDependencyMacroImport;

typedef struct CmDependencyMacroArtifact {
    void *state;
} CmDependencyMacroArtifact;

typedef struct CmDependencyMacroArtifactIdentity {
    const CmModuleGraph *dependency_graph;
    CmModuleGraphRevision dependency_revision;
    const char *extern_name;
    const char *crate_identifier;
} CmDependencyMacroArtifactIdentity;

void cm_dependency_macro_artifact_init(CmDependencyMacroArtifact *artifact);
void cm_dependency_macro_artifact_destroy(CmDependencyMacroArtifact *artifact);

/* Borrowed identity view; false if the artifact is empty or stale. */
int cm_dependency_macro_artifact_identity(
    const CmDependencyMacroArtifact *artifact,
    CmDependencyMacroArtifactIdentity *out_identity);

/*
 * Authenticates one live, successful dependency graph and a revision-bound
 * local import snapshot. Unresolved unrelated import leaves remain visible in
 * import_error_count; only a separately successful exact lookup certifies a
 * macro. extern_name is the first segment accepted by lookup;
 * crate_identifier is the explicit single identifier used for `$crate`.
 */
CmDependencyMacroArtifactResult cm_dependency_macro_artifact_build(
    CmDependencyMacroArtifact *artifact, const CmModuleGraph *dependency,
    CmModuleGraphRevision revision, const char *extern_name,
    const char *crate_identifier);

/*
 * Resolves one structured public path such as core::bstr::impl_partial_eq.
 * Every intermediate module or module reexport and the final macro binding
 * must be public. No path text is parsed and no lookup uses invocation names.
 */
CmDependencyMacroStatus cm_dependency_macro_artifact_lookup(
    const CmDependencyMacroArtifact *artifact,
    const CmResolvePathSegmentView *segments, size_t segment_count,
    CmDependencyMacroDefinition *out_definition);

/*
 * Resolves a qualified path produced from `$crate`, whose first segment must
 * be the artifact's distinct crate_identifier rather than its consumer extern
 * name. Remaining public-path checks and returned identity are identical.
 */
CmDependencyMacroStatus cm_dependency_macro_artifact_lookup_generated(
    const CmDependencyMacroArtifact *artifact,
    const CmResolvePathSegmentView *segments, size_t segment_count,
    CmDependencyMacroDefinition *out_definition);

/*
 * Certifies one unqualified consumer-visible name through one explicit,
 * unresolved external use-tree leaf such as
 * `use core::bstr::impl_partial_eq;`. Aliases and grouped trees are accepted;
 * glob imports and competing unresolved leaves fail closed.
 */
CmDependencyMacroStatus cm_dependency_macro_artifact_resolve_import(
    const CmDependencyMacroArtifact *artifact,
    const CmModuleGraph *consumer, CmModuleGraphRevision consumer_revision,
    CmModuleId consumer_module, const CmResolvePathSegmentView *local_name,
    CmDependencyMacroImport *out_import);

int cm_dependency_macro_artifact_matches(
    const CmDependencyMacroArtifact *artifact,
    const CmModuleGraph *dependency, CmModuleGraphRevision revision);

const char *cm_dependency_macro_status_name(CmDependencyMacroStatus status);

#endif
