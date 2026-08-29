#include "cm/resolve/dependency_macro.h"

#include "cm/alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CmDependencyMacroArtifactState {
    CmImportResolver imports;
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    char *extern_name;
    char *crate_identifier;
} CmDependencyMacroArtifactState;

static CmDependencyMacroArtifactState *cm_dependency_state(
    CmDependencyMacroArtifact *artifact)
{
    return artifact == NULL ? NULL
        : (CmDependencyMacroArtifactState *)artifact->state;
}

static const CmDependencyMacroArtifactState *cm_dependency_state_const(
    const CmDependencyMacroArtifact *artifact)
{
    return artifact == NULL ? NULL
        : (const CmDependencyMacroArtifactState *)artifact->state;
}

static void cm_dependency_state_invalidate(
    CmDependencyMacroArtifactState *state)
{
    if (state == NULL) return;
    cm_free(state->extern_name);
    cm_free(state->crate_identifier);
    state->extern_name = NULL;
    state->crate_identifier = NULL;
    state->graph = NULL;
    state->revision = CM_MODULE_GRAPH_REVISION_NONE;
}

void cm_dependency_macro_artifact_init(CmDependencyMacroArtifact *artifact)
{
    CmDependencyMacroArtifactState *state;

    if (artifact == NULL) return;
    state = (CmDependencyMacroArtifactState *)cm_alloc_zeroed(1u,
        sizeof(*state));
    cm_import_resolver_init(&state->imports);
    artifact->state = state;
}

void cm_dependency_macro_artifact_destroy(CmDependencyMacroArtifact *artifact)
{
    CmDependencyMacroArtifactState *state;

    state = cm_dependency_state(artifact);
    if (state == NULL) return;
    cm_dependency_state_invalidate(state);
    cm_import_resolver_destroy(&state->imports);
    cm_free(state);
    artifact->state = NULL;
}

int cm_dependency_macro_artifact_identity(
    const CmDependencyMacroArtifact *artifact,
    CmDependencyMacroArtifactIdentity *out_identity)
{
    const CmDependencyMacroArtifactState *state;

    if (out_identity != NULL) memset(out_identity, 0, sizeof(*out_identity));
    state = cm_dependency_state_const(artifact);
    if (state == NULL || out_identity == NULL || state->graph == NULL
        || !cm_dependency_macro_artifact_matches(artifact, state->graph,
            state->revision)) return 0;
    out_identity->dependency_graph = state->graph;
    out_identity->dependency_revision = state->revision;
    out_identity->extern_name = state->extern_name;
    out_identity->crate_identifier = state->crate_identifier;
    return 1;
}

static int cm_dependency_identifier_valid(const char *identifier)
{
    const unsigned char *bytes;
    size_t index;

    if (identifier == NULL || identifier[0] == 0) return 0;
    bytes = (const unsigned char *)identifier;
    if (!((bytes[0] >= (unsigned char)'a'
                && bytes[0] <= (unsigned char)'z')
            || (bytes[0] >= (unsigned char)'A'
                && bytes[0] <= (unsigned char)'Z')
            || bytes[0] == (unsigned char)'_')) return 0;
    for (index = 1u; bytes[index] != 0; ++index) {
        if (!((bytes[index] >= (unsigned char)'a'
                    && bytes[index] <= (unsigned char)'z')
                || (bytes[index] >= (unsigned char)'A'
                    && bytes[index] <= (unsigned char)'Z')
                || (bytes[index] >= (unsigned char)'0'
                    && bytes[index] <= (unsigned char)'9')
                || bytes[index] == (unsigned char)'_')) return 0;
    }
    return 1;
}

static char *cm_dependency_copy_string(const char *source)
{
    size_t length;
    char *copy;

    length = strlen(source);
    copy = (char *)cm_alloc(length + 1u);
    memcpy(copy, source, length + 1u);
    return copy;
}

CmDependencyMacroArtifactResult cm_dependency_macro_artifact_build(
    CmDependencyMacroArtifact *artifact, const CmModuleGraph *dependency,
    CmModuleGraphRevision revision, const char *extern_name,
    const char *crate_identifier)
{
    CmDependencyMacroArtifactResult result;
    CmDependencyMacroArtifactState *state;
    CmImportResult imports;
    CmModuleId root;

    memset(&result, 0, sizeof(result));
    result.status = CM_DEPENDENCY_MACRO_INVALID_ARGUMENT;
    state = cm_dependency_state(artifact);
    if (state == NULL) return result;
    cm_dependency_state_invalidate(state);
    cm_import_resolver_destroy(&state->imports);
    cm_import_resolver_init(&state->imports);
    if (dependency == NULL || revision == CM_MODULE_GRAPH_REVISION_NONE
        || !cm_dependency_identifier_valid(extern_name)
        || !cm_dependency_identifier_valid(crate_identifier)) return result;
    if (cm_module_graph_revision(dependency) != revision
        || cm_module_graph_error_count(dependency) != 0u
        || !cm_module_graph_get_root(dependency, &root)) {
        result.status = CM_DEPENDENCY_MACRO_FAILED_GRAPH;
        return result;
    }
    imports = cm_import_resolve(&state->imports, dependency, revision);
    result.import_error_count = imports.error_count;
    if (imports.revision != revision) {
        result.status = CM_DEPENDENCY_MACRO_IMPORT_ERROR;
        return result;
    }
    state->extern_name = cm_dependency_copy_string(extern_name);
    state->crate_identifier = cm_dependency_copy_string(crate_identifier);
    state->graph = dependency;
    state->revision = revision;
    result.status = CM_DEPENDENCY_MACRO_OK;
    result.revision = revision;
    return result;
}

int cm_dependency_macro_artifact_matches(
    const CmDependencyMacroArtifact *artifact,
    const CmModuleGraph *dependency, CmModuleGraphRevision revision)
{
    const CmDependencyMacroArtifactState *state;

    state = cm_dependency_state_const(artifact);
    return state != NULL && dependency != NULL
        && revision != CM_MODULE_GRAPH_REVISION_NONE
        && state->graph == dependency && state->revision == revision
        && cm_module_graph_revision(dependency) == revision
        && cm_module_graph_error_count(dependency) == 0u
        && cm_import_resolver_matches_graph(&state->imports, dependency);
}

static int cm_dependency_segment_is(
    const CmResolvePathSegmentView *segment, const char *expected)
{
    size_t length;

    if (segment == NULL || segment->bytes == NULL || expected == NULL)
        return 0;
    length = strlen(expected);
    return segment->length == length
        && memcmp(segment->bytes, expected, length) == 0;
}

static CmDependencyMacroStatus cm_dependency_lookup_status(
    CmImportLookupStatus status)
{
    if (status == CM_IMPORT_LOOKUP_NOT_FOUND)
        return CM_DEPENDENCY_MACRO_NOT_FOUND;
    if (status == CM_IMPORT_LOOKUP_AMBIGUOUS
        || status == CM_IMPORT_LOOKUP_CYCLE)
        return CM_DEPENDENCY_MACRO_AMBIGUOUS;
    if (status == CM_IMPORT_LOOKUP_STALE_REVISION
        || status == CM_IMPORT_LOOKUP_FAILED_BUILD)
        return CM_DEPENDENCY_MACRO_STALE_REVISION;
    return CM_DEPENDENCY_MACRO_INVALID_ARGUMENT;
}

CmDependencyMacroStatus cm_dependency_macro_artifact_lookup(
    const CmDependencyMacroArtifact *artifact,
    const CmResolvePathSegmentView *segments, size_t segment_count,
    CmDependencyMacroDefinition *out_definition)
{
    const CmDependencyMacroArtifactState *state;
    CmResolveMacroDeclaration declaration;
    CmResolvedBinding binding;
    CmImportLookupStatus lookup;
    const CmAst *definition_ast;
    size_t prefix_count;

    if (out_definition != NULL)
        memset(out_definition, 0, sizeof(*out_definition));
    state = cm_dependency_state_const(artifact);
    if (state == NULL || out_definition == NULL || segments == NULL
        || segment_count < 2u || state->graph == NULL) {
        return CM_DEPENDENCY_MACRO_INVALID_ARGUMENT;
    }
    if (!cm_dependency_segment_is(&segments[0], state->extern_name))
        return CM_DEPENDENCY_MACRO_NOT_FOUND;
    if (!cm_dependency_macro_artifact_matches(artifact, state->graph,
            state->revision)) return CM_DEPENDENCY_MACRO_STALE_REVISION;
    for (prefix_count = 1u; prefix_count + 1u < segment_count;
            ++prefix_count) {
        memset(&binding, 0, sizeof(binding));
        lookup = cm_import_resolve_path_checked(&state->imports,
            state->graph, state->revision, CM_MODULE_NONE, 1,
            segments + 1u, prefix_count, CM_RESOLVE_NAMESPACE_TYPE,
            &binding);
        if (lookup != CM_IMPORT_LOOKUP_OK)
            return cm_dependency_lookup_status(lookup);
        if (!binding.is_public || binding.target_module == CM_MODULE_NONE)
            return CM_DEPENDENCY_MACRO_PRIVATE_PATH;
    }
    memset(&binding, 0, sizeof(binding));
    lookup = cm_import_resolve_path_checked(&state->imports, state->graph,
        state->revision, CM_MODULE_NONE, 1, segments + 1u,
        segment_count - 1u, CM_RESOLVE_NAMESPACE_MACRO, &binding);
    if (lookup != CM_IMPORT_LOOKUP_OK)
        return cm_dependency_lookup_status(lookup);
    if (!binding.is_public) return CM_DEPENDENCY_MACRO_PRIVATE_PATH;
    if (binding.item_kind != CM_AST_ITEM_MACRO
        || binding.declaration.source == 0u
        || binding.declaration.item == CM_AST_ITEM_NONE
        || cm_module_graph_get_macro_declaration(state->graph,
            state->revision, binding.declaration, &declaration)
                != CM_RESOLVE_VIEW_OK
        || declaration.form != CM_AST_MACRO_RULES_DEFINITION
        || declaration.is_generated
        || !cm_module_graph_borrow_ast(state->graph,
            declaration.owner_module, &definition_ast)
        || cm_ast_get_item(definition_ast, binding.declaration.item)
            == NULL) {
        return CM_DEPENDENCY_MACRO_UNSUPPORTED_DEFINITION;
    }
    out_definition->dependency_graph = state->graph;
    out_definition->dependency_revision = state->revision;
    out_definition->declaration = binding.declaration;
    out_definition->owner_module = declaration.owner_module;
    out_definition->definition_ast = definition_ast;
    out_definition->form = declaration.form;
    out_definition->extern_name = state->extern_name;
    out_definition->crate_identifier = state->crate_identifier;
    return CM_DEPENDENCY_MACRO_OK;
}

CmDependencyMacroStatus cm_dependency_macro_artifact_lookup_generated(
    const CmDependencyMacroArtifact *artifact,
    const CmResolvePathSegmentView *segments, size_t segment_count,
    CmDependencyMacroDefinition *out_definition)
{
    const CmDependencyMacroArtifactState *state;
    CmResolvePathSegmentView *extern_path;
    CmDependencyMacroStatus status;

    if (out_definition != NULL)
        memset(out_definition, 0, sizeof(*out_definition));
    state = cm_dependency_state_const(artifact);
    if (state == NULL || out_definition == NULL || segments == NULL
        || segment_count < 2u || state->graph == NULL) {
        return CM_DEPENDENCY_MACRO_INVALID_ARGUMENT;
    }
    if (!cm_dependency_segment_is(&segments[0], state->crate_identifier))
        return CM_DEPENDENCY_MACRO_NOT_FOUND;
    extern_path = (CmResolvePathSegmentView *)cm_alloc_zeroed(segment_count,
        sizeof(*extern_path));
    memcpy(extern_path, segments, segment_count * sizeof(*extern_path));
    extern_path[0].bytes = (const unsigned char *)state->extern_name;
    extern_path[0].length = strlen(state->extern_name);
    status = cm_dependency_macro_artifact_lookup(artifact, extern_path,
        segment_count, out_definition);
    cm_free(extern_path);
    return status;
}

static int cm_dependency_segment_identifier_valid(
    const CmResolvePathSegmentView *segment)
{
    size_t index;

    if (segment == NULL || segment->bytes == NULL || segment->length == 0u)
        return 0;
    if (!((segment->bytes[0] >= (unsigned char)'a'
                && segment->bytes[0] <= (unsigned char)'z')
            || (segment->bytes[0] >= (unsigned char)'A'
                && segment->bytes[0] <= (unsigned char)'Z')
            || segment->bytes[0] == (unsigned char)'_')) return 0;
    for (index = 1u; index < segment->length; ++index) {
        if (!((segment->bytes[index] >= (unsigned char)'a'
                    && segment->bytes[index] <= (unsigned char)'z')
                || (segment->bytes[index] >= (unsigned char)'A'
                    && segment->bytes[index] <= (unsigned char)'Z')
                || (segment->bytes[index] >= (unsigned char)'0'
                    && segment->bytes[index] <= (unsigned char)'9')
                || segment->bytes[index] == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_dependency_import_name_is(const CmImportResolver *imports,
    CmResolveStringId name, const CmResolvePathSegmentView *expected)
{
    size_t length;
    char *buffer;
    int matches;

    if (name == CM_RESOLVE_STRING_NONE) return 0;
    length = cm_import_string_length(imports, name);
    if (length != expected->length) return 0;
    buffer = (char *)cm_alloc(length + 1u);
    matches = cm_import_copy_string(imports, name, buffer, length + 1u)
        && memcmp(buffer, expected->bytes, length) == 0;
    cm_free(buffer);
    return matches;
}

CmDependencyMacroStatus cm_dependency_macro_artifact_resolve_import(
    const CmDependencyMacroArtifact *artifact,
    const CmModuleGraph *consumer, CmModuleGraphRevision consumer_revision,
    CmModuleId consumer_module, const CmResolvePathSegmentView *local_name,
    CmDependencyMacroImport *out_import)
{
    const CmDependencyMacroArtifactState *state;
    CmImportResolver imports;
    CmImportResult import_result;
    CmResolveModuleInfo module_info;
    CmResolvedBinding local_binding;
    CmImportLookupStatus local_lookup;
    CmDependencyMacroDefinition candidate;
    CmResolveItemRef candidate_import;
    CmDependencyMacroStatus candidate_status;
    size_t contender_count;
    size_t leaf_index;
    int unsupported_glob;

    if (out_import != NULL) memset(out_import, 0, sizeof(*out_import));
    state = cm_dependency_state_const(artifact);
    if (state == NULL || consumer == NULL || out_import == NULL
        || consumer_revision == CM_MODULE_GRAPH_REVISION_NONE
        || consumer_module == CM_MODULE_NONE
        || !cm_dependency_segment_identifier_valid(local_name)) {
        return CM_DEPENDENCY_MACRO_INVALID_ARGUMENT;
    }
    if (state->graph == NULL || !cm_dependency_macro_artifact_matches(
            artifact, state->graph, state->revision)) {
        return CM_DEPENDENCY_MACRO_STALE_REVISION;
    }
    if (cm_module_graph_revision(consumer) != consumer_revision
        || cm_module_graph_error_count(consumer) != 0u
        || !cm_module_graph_get_module(consumer, consumer_module,
            &module_info)) return CM_DEPENDENCY_MACRO_STALE_REVISION;
    (void)module_info;
    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, consumer,
        consumer_revision);
    if (import_result.revision != consumer_revision) {
        cm_import_resolver_destroy(&imports);
        return CM_DEPENDENCY_MACRO_STALE_REVISION;
    }
    memset(&local_binding, 0, sizeof(local_binding));
    local_lookup = cm_import_resolve_path_checked(&imports, consumer,
        consumer_revision, consumer_module, 0, local_name, 1u,
        CM_RESOLVE_NAMESPACE_MACRO, &local_binding);
    /* A binding the consumer's resolver delegated into a dependency
     * (`use cfg_if::cfg_if;` resolved through cfg_if's macro namespace)
     * is the very import this artifact certifies, not a local shadow. */
    if (getenv("CM_MACRO_DEBUG") != NULL)
        fprintf(stderr, "MACRO dep-import artifact=%s name=%.*s "
            "local_lookup=%d local_dep=%u local_kind=%d\n",
            state->extern_name == NULL ? "?" : state->extern_name,
            (int)local_name->length, (const char *)local_name->bytes,
            (int)local_lookup, (unsigned)local_binding.dependency,
            (int)local_binding.item_kind);
    if (local_lookup == CM_IMPORT_LOOKUP_OK && local_binding.dependency != 0u)
        local_lookup = CM_IMPORT_LOOKUP_NOT_FOUND;
    if (local_lookup == CM_IMPORT_LOOKUP_OK
        || local_lookup == CM_IMPORT_LOOKUP_AMBIGUOUS
        || local_lookup == CM_IMPORT_LOOKUP_CYCLE) {
        cm_import_resolver_destroy(&imports);
        return CM_DEPENDENCY_MACRO_AMBIGUOUS;
    }
    if (local_lookup != CM_IMPORT_LOOKUP_NOT_FOUND) {
        cm_import_resolver_destroy(&imports);
        return CM_DEPENDENCY_MACRO_STALE_REVISION;
    }
    memset(&candidate, 0, sizeof(candidate));
    memset(&candidate_import, 0, sizeof(candidate_import));
    candidate_status = CM_DEPENDENCY_MACRO_NOT_FOUND;
    contender_count = 0u;
    unsupported_glob = 0;
    for (leaf_index = 0u; leaf_index < cm_import_leaf_count(&imports);
            ++leaf_index) {
        CmImportLeafView leaf;
        CmResolvePathSegmentView first;

        if (!cm_import_get_leaf(&imports, (uint32_t)leaf_index, &leaf)
            || leaf.module != consumer_module || leaf.is_resolved)
            continue;
        if (leaf.is_glob) {
            if (leaf.segment_count != 0u
                && cm_import_get_leaf_segment(&imports,
                    (uint32_t)leaf_index, 0u, &first)
                && cm_dependency_segment_is(&first,
                    state->extern_name)) unsupported_glob = 1;
            continue;
        }
        if (leaf.is_anonymous || leaf.segment_count < 2u
            || !cm_dependency_import_name_is(&imports, leaf.import_name,
                local_name)) continue;
        contender_count += 1u;
        if (contender_count != 1u
            || !cm_import_get_leaf_segment(&imports,
                (uint32_t)leaf_index, 0u, &first)
            || !cm_dependency_segment_is(&first, state->extern_name))
            continue;
        {
            CmResolvePathSegmentView *path;
            size_t segment_index;
            int path_ok;

            path = (CmResolvePathSegmentView *)cm_alloc_zeroed(
                leaf.segment_count, sizeof(*path));
            path_ok = 1;
            for (segment_index = 0u; segment_index < leaf.segment_count;
                    ++segment_index) {
                if (!cm_import_get_leaf_segment(&imports,
                        (uint32_t)leaf_index, (uint32_t)segment_index,
                        &path[segment_index])) {
                    path_ok = 0;
                    break;
                }
            }
            candidate_status = path_ok
                ? cm_dependency_macro_artifact_lookup(artifact, path,
                    leaf.segment_count, &candidate)
                : CM_DEPENDENCY_MACRO_INVALID_ARGUMENT;
            cm_free(path);
            candidate_import = leaf.declaration;
        }
    }
    if (unsupported_glob) {
        candidate_status = CM_DEPENDENCY_MACRO_UNSUPPORTED_IMPORT;
    } else if (contender_count > 1u) {
        candidate_status = CM_DEPENDENCY_MACRO_AMBIGUOUS;
    } else if (contender_count == 0u) {
        candidate_status = CM_DEPENDENCY_MACRO_NOT_FOUND;
    }
    if (candidate_status == CM_DEPENDENCY_MACRO_OK) {
        out_import->consumer_graph = consumer;
        out_import->consumer_revision = consumer_revision;
        out_import->consumer_module = consumer_module;
        out_import->import_declaration = candidate_import;
        out_import->definition = candidate;
    }
    cm_import_resolver_destroy(&imports);
    return candidate_status;
}

const char *cm_dependency_macro_status_name(CmDependencyMacroStatus status)
{
    switch (status) {
    case CM_DEPENDENCY_MACRO_OK: return "ok";
    case CM_DEPENDENCY_MACRO_INVALID_ARGUMENT: return "invalid argument";
    case CM_DEPENDENCY_MACRO_FAILED_GRAPH: return "failed graph";
    case CM_DEPENDENCY_MACRO_IMPORT_ERROR: return "import error";
    case CM_DEPENDENCY_MACRO_STALE_REVISION: return "stale revision";
    case CM_DEPENDENCY_MACRO_NOT_FOUND: return "not found";
    case CM_DEPENDENCY_MACRO_AMBIGUOUS: return "ambiguous";
    case CM_DEPENDENCY_MACRO_PRIVATE_PATH: return "private path";
    case CM_DEPENDENCY_MACRO_UNSUPPORTED_IMPORT:
        return "unsupported import";
    case CM_DEPENDENCY_MACRO_UNSUPPORTED_DEFINITION:
        return "unsupported definition";
    }
    return "unknown";
}
