#include "cm/resolve/module_graph.h"
#include "cm/resolve/derive_expand.h"

#include "cm/alloc.h"
#include "cm/arena.h"
#include "cm/interner.h"
#include "cm/resolve/dependency_macro.h"
#include "cm/resolve/imports.h"
#include "cm/syntax/parser.h"
#include "cm/vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

typedef uint32_t CmResolveUnitId;

typedef struct CmResolveUnit {
    CmSourceId source;
    CmAst ast;
    CmItemMacroPlan plan;
    /* Source provenance indexed by AST item ID minus one. */
    CmVec item_sources;
    /* Authenticated include nesting depth on the same item index. */
    CmVec item_include_depths;
    CmItemMacroItemRef *initial_scope;
    size_t initial_scope_count;
    /* Resolutions accumulated over staging rounds: every replan starts
     * from scratch, so a source invocation resolved earlier (`thread_local!`)
     * and a generated qualified path (`crate::thread::local_impl::
     * thread_local_inner`) resolved later must both be supplied. */
    CmVec sticky_resolved;
    CmVec sticky_paths;
    size_t parsed_item_count;
    int parse_ok;
    int plan_prepared;
    int plan_ok;
    int active;
    int has_identity;
    dev_t device;
    ino_t inode;
} CmResolveUnit;

typedef struct CmResolveEffectiveVariantRecord {
    CmResolveEffectiveVariant variant;
    CmResolveEffectiveAttribute *attributes;
} CmResolveEffectiveVariantRecord;

typedef struct CmResolveEffectiveItemRecord {
    CmResolveEffectiveItem item;
    CmResolveEffectiveAttribute *attributes;
    struct CmResolveEffectiveItemRecord *children;
    CmResolveEffectiveVariantRecord *variants;
} CmResolveEffectiveItemRecord;

typedef struct CmResolveModuleNode {
    CmResolveModuleInfo info;
    CmResolveStringId module_directory;
    CmResolveUnitId unit;
    const CmAstItemId *items;
    uint32_t item_count;
    CmModuleId *children;
    CmResolveItemRef *active_items;
    CmResolveEffectiveItemRecord *effective_items;
    CmResolveEffectiveAttribute *inner_attributes;
    CmResolveNamespaceEntry *type_entries;
    CmResolveNamespaceEntry *value_entries;
    CmResolveNamespaceEntry *macro_entries;
    CmResolveMacroScopeEntry *macro_scope_entries;
    CmResolveMacroScopeEntry *macro_scope_history;
    size_t macro_scope_history_count;
    CmResolveImport *imports;
} CmResolveModuleNode;

typedef struct CmResolveMacroDeclarationRecord {
    CmResolveMacroDeclaration declaration;
    CmResolveEffectiveAttribute *attributes;
} CmResolveMacroDeclarationRecord;

typedef struct CmResolveDependencyRegistration {
    CmResolveDependencyId id;
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
} CmResolveDependencyRegistration;

typedef struct CmResolveExternalAstOwner {
    CmItemMacroAstOwner owner;
    CmResolveDependencyId dependency;
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    CmModuleId owner_module;
    const CmAst *ast;
} CmResolveExternalAstOwner;

typedef struct CmResolveExternalMacro {
    CmItemMacroAstOwner owner;
    CmResolveDependencyCertificateId certificate;
    CmResolveDependencyId dependency;
    CmModuleGraphRevision revision;
    CmResolveItemRef declaration;
    int published;
} CmResolveExternalMacro;

typedef struct CmResolveExternalMacroImport {
    CmItemMacroAstOwner owner;
    CmAstItemId definition;
    CmModuleId module;
    CmResolveItemRef import_declaration;
    CmResolveItemRef invocation;
} CmResolveExternalMacroImport;

typedef struct CmPendingModule {
    CmResolveStringId name;
    CmResolveItemRef declaration;
    CmSpan span;
    int is_inline;
    CmAstItemId *items;
    uint32_t item_count;
    const CmItemMacroPlanNode *effective_items;
    size_t effective_item_count;
    const CmEffectiveAttribute *attributes;
    size_t attribute_count;
    const CmEffectiveAttribute *inner_attributes;
    size_t inner_attribute_count;
    int inner_attributes_generated;
    const CmItemMacroItemRef *external_scope;
    size_t external_scope_count;
} CmPendingModule;

typedef struct CmModuleGraphState {
    const CmModuleGraph *owner_graph;
    uint64_t lifetime_id;
    CmArena storage;
    CmInterner strings;
    CmVec modules;
    CmVec macro_declarations;
    CmVec dependencies;
    CmVec external_ast_owners;
    CmVec external_macros;
    CmVec external_macro_imports;
    /* Extern names of `#[macro_use] extern crate X;` items (std's
     * `alloc`): X's exported macros resolve unqualified. */
    CmVec macro_use_crates;
    /* Names of invocations deferred in the previous / current staging
     * round (sorted later): an unchanged multiset with no shrinking
     * pending set means the round made no progress. */
    CmVec previous_deferred_names;
    CmVec deferred_names;
    CmVec units;
    CmVec errors;
    CmSourceSet *building_sources;
    CmModuleGraphOptions options;
    int defer_non_macro_use_modules;
    size_t authenticated_include_files;
    /* Builtin derives may name `::core` (set once for the crate). */
    int derive_core_reachable;
    size_t authenticated_include_bytes;
    CmModuleGraphRevision revision;
    int revision_exhausted;
} CmModuleGraphState;

static int cm_graph_compact_item_cfg_fields(CmModuleGraphState *state,
    const CmAst *ast, CmAstItemId item_id, const CmAstItem *item);

static uint64_t cm_module_graph_lifetime_counter;

static uint64_t cm_module_graph_new_lifetime_id(void)
{
    if (cm_module_graph_lifetime_counter == UINT64_MAX) abort();
    cm_module_graph_lifetime_counter += UINT64_C(1);
    return cm_module_graph_lifetime_counter;
}

#define CM_INCLUDE_MAX_DEPTH 32u
#define CM_INCLUDE_MAX_FILES 256u
#define CM_INCLUDE_MAX_BYTES (16u * 1024u * 1024u)
#define CM_STAGING_MAX_ROUNDS 512u

typedef struct CmIncludeExpansion {
    CmVec active_sources;
    size_t file_count;
    size_t byte_count;
    int binding_ambiguous;
} CmIncludeExpansion;

static CmModuleGraphState *cm_graph_state(CmModuleGraph *graph)
{
    return graph == NULL ? NULL : (CmModuleGraphState *)graph->state;
}

static const CmModuleGraphState *cm_graph_state_const(
    const CmModuleGraph *graph)
{
    return graph == NULL ? NULL :
        (const CmModuleGraphState *)graph->state;
}

static int cm_resolve_item_ref_equal(CmResolveItemRef left,
    CmResolveItemRef right)
{
    return left.source == right.source && left.item == right.item;
}

static void cm_graph_vec_remove(CmVec *vector, size_t index)
{
    size_t trailing;

    if (vector == NULL || index >= vector->len) return;
    trailing = vector->len - index - 1u;
    if (trailing != 0u) {
        memmove(vector->data + index * vector->elem_size,
            vector->data + (index + 1u) * vector->elem_size,
            trailing * vector->elem_size);
    }
    vector->len -= 1u;
}

static size_t cm_remove_active_declaration(CmVec *items,
    CmResolveItemRef declaration)
{
    size_t index;
    size_t removed;

    index = 0u;
    removed = 0u;
    while (index < items->len) {
        const CmResolveItemRef *item;

        item = (const CmResolveItemRef *)cm_vec_at_const(items, index);
        if (item != NULL && cm_resolve_item_ref_equal(*item, declaration)) {
            cm_graph_vec_remove(items, index);
            removed += 1u;
        } else {
            index += 1u;
        }
    }
    return removed;
}

static size_t cm_remove_effective_declaration(CmVec *items,
    CmResolveItemRef declaration)
{
    size_t index;
    size_t removed;

    index = 0u;
    removed = 0u;
    while (index < items->len) {
        const CmResolveEffectiveItemRecord *item;

        item = (const CmResolveEffectiveItemRecord *)cm_vec_at_const(items,
            index);
        if (item != NULL && cm_resolve_item_ref_equal(
                item->item.declaration, declaration)) {
            cm_graph_vec_remove(items, index);
            removed += 1u;
        } else {
            index += 1u;
        }
    }
    return removed;
}

static size_t cm_remove_namespace_declaration(CmVec *entries,
    CmResolveItemRef declaration)
{
    size_t index;
    size_t removed;

    index = 0u;
    removed = 0u;
    while (index < entries->len) {
        const CmResolveNamespaceEntry *entry;

        entry = (const CmResolveNamespaceEntry *)cm_vec_at_const(entries,
            index);
        if (entry != NULL && cm_resolve_item_ref_equal(entry->declaration,
                declaration)) {
            cm_graph_vec_remove(entries, index);
            removed += 1u;
        } else {
            index += 1u;
        }
    }
    return removed;
}

static size_t cm_remove_import_declaration(CmVec *imports,
    CmResolveItemRef declaration)
{
    size_t index;
    size_t removed;

    index = 0u;
    removed = 0u;
    while (index < imports->len) {
        const CmResolveImport *import_value;

        import_value = (const CmResolveImport *)cm_vec_at_const(imports,
            index);
        if (import_value != NULL && cm_resolve_item_ref_equal(
                import_value->declaration, declaration)) {
            cm_graph_vec_remove(imports, index);
            removed += 1u;
        } else {
            index += 1u;
        }
    }
    return removed;
}

static void cm_graph_state_init(CmModuleGraphState *state)
{
    memset(state, 0, sizeof(*state));
    cm_arena_init(&state->storage, 4096u);
    cm_interner_init(&state->strings, 4096u);
    cm_vec_init(&state->modules, sizeof(CmResolveModuleNode));
    cm_vec_init(&state->macro_declarations,
        sizeof(CmResolveMacroDeclarationRecord));
    cm_vec_init(&state->dependencies,
        sizeof(CmResolveDependencyRegistration));
    cm_vec_init(&state->external_ast_owners,
        sizeof(CmResolveExternalAstOwner));
    cm_vec_init(&state->external_macros,
        sizeof(CmResolveExternalMacro));
    cm_vec_init(&state->macro_use_crates, 64u);
    cm_vec_init(&state->previous_deferred_names, 1u);
    cm_vec_init(&state->deferred_names, 1u);
    cm_vec_init(&state->external_macro_imports,
        sizeof(CmResolveExternalMacroImport));
    cm_vec_init(&state->units, sizeof(CmResolveUnit));
    cm_vec_init(&state->errors, sizeof(CmResolveError));
}

static void cm_unit_clear_sticky(CmResolveUnit *unit);

static void cm_graph_state_destroy(CmModuleGraphState *state)
{
    size_t index;

    if (state == NULL) return;
    for (index = 0u; index < state->units.len; ++index) {
        CmResolveUnit *unit;

        unit = (CmResolveUnit *)cm_vec_at(&state->units, index);
        if (unit != NULL) {
            cm_item_macro_plan_destroy(&unit->plan);
            cm_free(unit->initial_scope);
            cm_unit_clear_sticky(unit);
            cm_vec_destroy(&unit->sticky_paths);
            cm_vec_destroy(&unit->sticky_resolved);
            cm_vec_destroy(&unit->item_include_depths);
            cm_vec_destroy(&unit->item_sources);
            cm_ast_destroy(&unit->ast);
        }
    }
    cm_vec_destroy(&state->errors);
    cm_vec_destroy(&state->units);
    cm_vec_destroy(&state->macro_use_crates);
    cm_vec_destroy(&state->previous_deferred_names);
    cm_vec_destroy(&state->deferred_names);
    cm_vec_destroy(&state->external_macro_imports);
    cm_vec_destroy(&state->external_macros);
    cm_vec_destroy(&state->external_ast_owners);
    cm_vec_destroy(&state->dependencies);
    cm_vec_destroy(&state->macro_declarations);
    cm_vec_destroy(&state->modules);
    cm_interner_destroy(&state->strings);
    cm_arena_destroy(&state->storage);
    memset(state, 0, sizeof(*state));
}

void cm_module_graph_options_init(CmModuleGraphOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->edition = CM_EDITION_2024;
    options->include_expansion = CM_INCLUDE_EXPANSION_AUTHENTICATED;
}

static void cm_graph_clear_borrowed_build_inputs(CmModuleGraphState *state)
{
    state->building_sources = NULL;
    state->options.cfg = NULL;
    state->options.dependency_macros = NULL;
    state->options.dependency_macro_count = 0u;
}

void cm_module_graph_init(CmModuleGraph *graph)
{
    CmModuleGraphState *state;

    if (graph == NULL) return;
    state = (CmModuleGraphState *)cm_alloc_zeroed(1u, sizeof(*state));
    cm_graph_state_init(state);
    state->owner_graph = graph;
    state->lifetime_id = cm_module_graph_new_lifetime_id();
    graph->state = state;
}

void cm_module_graph_destroy(CmModuleGraph *graph)
{
    CmModuleGraphState *state;

    state = cm_graph_state(graph);
    if (state == NULL) return;
    cm_graph_state_destroy(state);
    cm_free(state);
    graph->state = NULL;
}

static uint32_t cm_count_u32(size_t count)
{
    if (count > (size_t)UINT32_MAX) cm_alloc_out_of_memory((size_t)-1);
    return (uint32_t)count;
}

static int cm_register_dependency_artifacts(CmModuleGraphState *state,
    const CmModuleGraph *consumer, const CmModuleGraphOptions *options)
{
    size_t index;

    if (options->dependency_macro_count > (size_t)UINT32_MAX
        || (options->dependency_macro_count != 0u
            && options->dependency_macros == NULL)) return 0;
    for (index = 0u; index < options->dependency_macro_count; ++index) {
        CmDependencyMacroArtifactIdentity identity;
        CmResolveDependencyRegistration registration;
        size_t earlier;

        memset(&identity, 0, sizeof(identity));
        if (options->dependency_macros[index] == NULL
            || !cm_dependency_macro_artifact_identity(
                options->dependency_macros[index], &identity)
            || identity.dependency_graph == consumer) return 0;
        for (earlier = 0u; earlier < index; ++earlier) {
            CmDependencyMacroArtifactIdentity prior;

            memset(&prior, 0, sizeof(prior));
            if (!cm_dependency_macro_artifact_identity(
                    options->dependency_macros[earlier], &prior)
                || prior.dependency_graph == identity.dependency_graph
                || strcmp(prior.extern_name, identity.extern_name) == 0
                || strcmp(prior.crate_identifier,
                    identity.crate_identifier) == 0) return 0;
        }
        memset(&registration, 0, sizeof(registration));
        registration.id = (CmResolveDependencyId)(index + 1u);
        registration.graph = identity.dependency_graph;
        registration.revision = identity.dependency_revision;
        (void)cm_vec_push(&state->dependencies, &registration);
    }
    return 1;
}

static int cm_register_external_macro(CmModuleGraphState *state,
    const CmDependencyMacroDefinition *definition,
    CmItemMacroAstOwner *out_owner)
{
    const CmResolveDependencyRegistration *dependency;
    CmResolveDependencyId dependency_id;
    CmResolveExternalAstOwner *owner_record;
    size_t index;

    if (out_owner != NULL) *out_owner = CM_ITEM_MACRO_AST_OWNER_NONE;
    if (state == NULL || definition == NULL || out_owner == NULL
        || definition->dependency_graph == NULL
        || definition->dependency_revision == CM_MODULE_GRAPH_REVISION_NONE
        || definition->declaration.source == 0u
        || definition->declaration.item == CM_AST_ITEM_NONE
        || definition->owner_module == CM_MODULE_NONE
        || definition->definition_ast == NULL) return 0;
    dependency_id = CM_RESOLVE_DEPENDENCY_NONE;
    for (index = 0u; index < state->dependencies.len; ++index) {
        dependency = (const CmResolveDependencyRegistration *)cm_vec_at_const(
            &state->dependencies, index);
        if (dependency != NULL
            && dependency->graph == definition->dependency_graph
            && dependency->revision == definition->dependency_revision) {
            dependency_id = dependency->id;
            break;
        }
    }
    if (dependency_id == CM_RESOLVE_DEPENDENCY_NONE) return 0;
    owner_record = NULL;
    for (index = 0u; index < state->external_ast_owners.len; ++index) {
        CmResolveExternalAstOwner *candidate;

        candidate = (CmResolveExternalAstOwner *)cm_vec_at(
            &state->external_ast_owners, index);
        if (candidate != NULL && candidate->dependency == dependency_id
            && candidate->revision == definition->dependency_revision
            && candidate->owner_module == definition->owner_module
            && candidate->ast == definition->definition_ast) {
            owner_record = candidate;
            break;
        }
    }
    if (owner_record == NULL) {
        CmResolveExternalAstOwner added;
        uint64_t offset;

        offset = (uint64_t)state->external_ast_owners.len + UINT64_C(1);
        if (offset > UINT64_MAX - (uint64_t)UINT32_MAX) return 0;
        memset(&added, 0, sizeof(added));
        added.owner = (CmItemMacroAstOwner)((uint64_t)UINT32_MAX + offset);
        added.dependency = dependency_id;
        added.graph = definition->dependency_graph;
        added.revision = definition->dependency_revision;
        added.owner_module = definition->owner_module;
        added.ast = definition->definition_ast;
        (void)cm_vec_push(&state->external_ast_owners, &added);
        owner_record = (CmResolveExternalAstOwner *)cm_vec_at(
            &state->external_ast_owners,
            state->external_ast_owners.len - 1u);
        if (owner_record == NULL) return 0;
    }
    for (index = 0u; index < state->external_macros.len; ++index) {
        const CmResolveExternalMacro *existing;

        existing = (const CmResolveExternalMacro *)cm_vec_at_const(
            &state->external_macros, index);
        if (existing != NULL && existing->owner == owner_record->owner
            && existing->declaration.source
                == definition->declaration.source
            && existing->declaration.item
                == definition->declaration.item) {
            *out_owner = owner_record->owner;
            return 1;
        }
    }
    {
        CmResolveExternalMacro added;

        if (state->external_macros.len >= (size_t)UINT32_MAX) return 0;
        memset(&added, 0, sizeof(added));
        added.owner = owner_record->owner;
        added.certificate = (CmResolveDependencyCertificateId)
            (state->external_macros.len + 1u);
        added.dependency = dependency_id;
        added.revision = definition->dependency_revision;
        added.declaration = definition->declaration;
        (void)cm_vec_push(&state->external_macros, &added);
    }
    *out_owner = owner_record->owner;
    return 1;
}

static int cm_register_external_macro_import(CmModuleGraphState *state,
    CmItemMacroAstOwner owner, CmAstItemId definition, CmModuleId module,
    CmResolveItemRef import_declaration, CmResolveItemRef invocation)
{
    size_t index;
    CmResolveExternalMacroImport added;

    if (state == NULL || owner <= (CmItemMacroAstOwner)UINT32_MAX
        || definition == CM_AST_ITEM_NONE || module == CM_MODULE_NONE
        || import_declaration.source == 0u
        || import_declaration.item == CM_AST_ITEM_NONE
        || invocation.source == 0u
        || invocation.item == CM_AST_ITEM_NONE) return 0;
    for (index = 0u; index < state->external_macro_imports.len; ++index) {
        const CmResolveExternalMacroImport *existing;

        existing = (const CmResolveExternalMacroImport *)cm_vec_at_const(
            &state->external_macro_imports, index);
        if (existing != NULL && existing->owner == owner
            && existing->definition == definition
            && existing->module == module
            && cm_resolve_item_ref_equal(existing->import_declaration,
                import_declaration)
            && cm_resolve_item_ref_equal(existing->invocation, invocation)) {
            return 1;
        }
    }
    memset(&added, 0, sizeof(added));
    added.owner = owner;
    added.definition = definition;
    added.module = module;
    added.import_declaration = import_declaration;
    added.invocation = invocation;
    (void)cm_vec_push(&state->external_macro_imports, &added);
    return 1;
}

static CmResolveExternalMacro *cm_external_macro_for_plan_ref(
    CmModuleGraphState *state, CmItemMacroItemRef reference)
{
    size_t index;

    if (reference.owner <= (CmItemMacroAstOwner)UINT32_MAX
        || reference.item == CM_AST_ITEM_NONE) return NULL;
    for (index = 0u; index < state->external_macros.len; ++index) {
        CmResolveExternalMacro *external;

        external = (CmResolveExternalMacro *)cm_vec_at(
            &state->external_macros, index);
        if (external != NULL && external->owner == reference.owner
            && external->declaration.item == reference.item) return external;
    }
    return NULL;
}

static CmItemMacroGeneratedLookupStatus
cm_resolve_generated_dependency_macro(void *context,
    const CmItemMacroPathSegment *segments, size_t segment_count,
    CmItemMacroResolvedGeneratedTarget *out_target)
{
    CmModuleGraphState *state;
    CmResolvePathSegmentView *path;
    CmDependencyMacroDefinition selected;
    size_t selected_count;
    size_t artifact_index;
    size_t index;
    int ambiguous;
    int invalid;

    if (out_target != NULL) memset(out_target, 0, sizeof(*out_target));
    state = (CmModuleGraphState *)context;
    if (state == NULL || out_target == NULL || segments == NULL
        || segment_count < 2u) return CM_ITEM_MACRO_GENERATED_LOOKUP_INVALID;
    path = (CmResolvePathSegmentView *)cm_alloc_zeroed(segment_count,
        sizeof(*path));
    for (index = 0u; index < segment_count; ++index) {
        path[index].bytes = segments[index].bytes;
        path[index].length = segments[index].length;
    }
    memset(&selected, 0, sizeof(selected));
    selected_count = 0u;
    ambiguous = 0;
    invalid = 0;
    for (artifact_index = 0u;
            artifact_index < state->options.dependency_macro_count;
            ++artifact_index) {
        CmDependencyMacroDefinition candidate;
        CmDependencyMacroStatus status;

        memset(&candidate, 0, sizeof(candidate));
        status = cm_dependency_macro_artifact_lookup_generated(
            state->options.dependency_macros[artifact_index], path,
            segment_count, &candidate);
        if (status == CM_DEPENDENCY_MACRO_OK) {
            selected = candidate;
            selected_count += 1u;
        } else if (status == CM_DEPENDENCY_MACRO_AMBIGUOUS) {
            ambiguous = 1;
        } else if (status != CM_DEPENDENCY_MACRO_NOT_FOUND) {
            invalid = 1;
        }
    }
    cm_free(path);
    if (ambiguous) return CM_ITEM_MACRO_GENERATED_LOOKUP_AMBIGUOUS;
    if (invalid) return CM_ITEM_MACRO_GENERATED_LOOKUP_INVALID;
    if (selected_count > 1u)
        return CM_ITEM_MACRO_GENERATED_LOOKUP_AMBIGUOUS;
    if (selected_count == 0u)
        return CM_ITEM_MACRO_GENERATED_LOOKUP_NOT_FOUND;
    if (!cm_register_external_macro(state, &selected,
            &out_target->definition.owner)) {
        return CM_ITEM_MACRO_GENERATED_LOOKUP_INVALID;
    }
    out_target->definition.item = selected.declaration.item;
    out_target->definition_ast = selected.definition_ast;
    out_target->crate_identifier = selected.crate_identifier;
    return CM_ITEM_MACRO_GENERATED_LOOKUP_OK;
}

static void *cm_graph_copy_array(CmModuleGraphState *state,
    const CmVec *values)
{
    size_t size;
    void *copy;

    if (values->len == 0u) return NULL;
    if (!cm_size_mul(values->len, values->elem_size, &size))
        cm_alloc_out_of_memory((size_t)-1);
    copy = cm_arena_alloc(&state->storage, size, sizeof(void *));
    memcpy(copy, values->data, size);
    return copy;
}

static CmResolveStringId cm_graph_intern(CmModuleGraphState *state,
    const void *bytes, size_t length)
{
    return (CmResolveStringId)cm_interner_intern(&state->strings, bytes,
        length);
}

static CmResolveStringId cm_graph_intern_c_str(CmModuleGraphState *state,
    const char *text)
{
    return (CmResolveStringId)cm_interner_intern_c_str(&state->strings, text);
}

static const CmInternedString *cm_graph_string(
    const CmModuleGraphState *state, CmResolveStringId id)
{
    return cm_interner_get(&state->strings, (CmInternId)id);
}

static CmResolveStringId cm_graph_copy_ast_string(CmModuleGraphState *state,
    const CmAst *ast, CmInternId id)
{
    const CmInternedString *string;

    string = cm_ast_get_string(ast, id);
    if (string == NULL) return CM_RESOLVE_STRING_NONE;
    return cm_graph_intern(state, string->bytes, string->len);
}

static CmSourceId cm_unit_item_source(const CmResolveUnit *unit,
    CmAstItemId item)
{
    const CmSourceId *source;

    if (unit == NULL || item == CM_AST_ITEM_NONE) return 0u;
    source = (const CmSourceId *)cm_vec_at_const(&unit->item_sources,
        (size_t)item - 1u);
    /* Macro reparsing appends generated nodes after include preprocessing. */
    return source == NULL ? unit->source : *source;
}

static uint32_t cm_unit_item_include_depth(const CmResolveUnit *unit,
    CmAstItemId item)
{
    const uint32_t *depth;

    if (unit == NULL || item == CM_AST_ITEM_NONE) return 0u;
    depth = (const uint32_t *)cm_vec_at_const(&unit->item_include_depths,
        (size_t)item - 1u);
    return depth == NULL ? 0u : *depth;
}

static char *cm_path_normalize(const char *path)
{
    size_t length;
    char *result;
    size_t *component_starts;
    size_t read_index;
    size_t write_index;
    size_t component_count;
    int absolute;

    length = strlen(path);
    result = (char *)cm_alloc(length + 3u);
    component_starts = (size_t *)cm_alloc((length + 1u) *
        sizeof(component_starts[0]));
    read_index = 0u;
    write_index = 0u;
    component_count = 0u;
    absolute = length != 0u && path[0] == '/';
    if (absolute) {
        result[write_index++] = '/';
        while (path[read_index] == '/') ++read_index;
    }
    while (read_index < length) {
        size_t start;
        size_t component_length;

        start = read_index;
        while (read_index < length && path[read_index] != '/') ++read_index;
        component_length = read_index - start;
        while (read_index < length && path[read_index] == '/') ++read_index;
        if (component_length == 0u ||
            (component_length == 1u && path[start] == '.')) {
            continue;
        }
        if (component_length == 2u && path[start] == '.' &&
            path[start + 1u] == '.') {
            if (component_count != 0u) {
                write_index = component_starts[--component_count];
                if (write_index > (absolute ? 1u : 0u) &&
                    result[write_index - 1u] == '/') {
                    --write_index;
                }
            } else if (!absolute) {
                if (write_index != 0u) result[write_index++] = '/';
                component_starts[component_count++] = write_index;
                result[write_index++] = '.';
                result[write_index++] = '.';
            }
            continue;
        }
        if (write_index != 0u && result[write_index - 1u] != '/')
            result[write_index++] = '/';
        component_starts[component_count++] = write_index;
        memcpy(result + write_index, path + start, component_length);
        write_index += component_length;
    }
    if (write_index == 0u) result[write_index++] = '.';
    result[write_index] = 0;
    cm_free(component_starts);
    return result;
}

static char *cm_path_directory(const char *path)
{
    const char *slash;
    size_t length;
    char *result;

    slash = strrchr(path, '/');
    if (slash == NULL) return cm_path_normalize(".");
    if (slash == path) return cm_path_normalize("/");
    length = (size_t)(slash - path);
    result = (char *)cm_alloc(length + 1u);
    memcpy(result, path, length);
    result[length] = 0;
    {
        char *normalized;

        normalized = cm_path_normalize(result);
        cm_free(result);
        return normalized;
    }
}

static char *cm_path_join_bytes(const char *directory,
    const unsigned char *name, size_t name_length, const char *suffix)
{
    size_t directory_length;
    size_t suffix_length;
    size_t total;
    char *result;
    char *normalized;

    directory_length = strlen(directory);
    suffix_length = strlen(suffix);
    if (!cm_size_add(directory_length, name_length, &total) ||
        !cm_size_add(total, suffix_length + 2u, &total))
        cm_alloc_out_of_memory((size_t)-1);
    result = (char *)cm_alloc(total);
    memcpy(result, directory, directory_length);
    result[directory_length] = '/';
    memcpy(result + directory_length + 1u, name, name_length);
    memcpy(result + directory_length + 1u + name_length, suffix,
        suffix_length + 1u);
    normalized = cm_path_normalize(result);
    cm_free(result);
    return normalized;
}

static int cm_path_is_regular_file(const char *path, struct stat *out_stat)
{
    struct stat information;

    if (stat(path, &information) != 0 || !S_ISREG(information.st_mode))
        return 0;
    if (out_stat != NULL) *out_stat = information;
    return 1;
}

static CmResolveStringId cm_join_module_path(CmModuleGraphState *state,
    CmResolveStringId parent, CmResolveStringId name)
{
    const CmInternedString *parent_string;
    const CmInternedString *name_string;
    size_t length;
    unsigned char *bytes;
    CmResolveStringId result;

    parent_string = cm_graph_string(state, parent);
    name_string = cm_graph_string(state, name);
    if (parent_string == NULL || name_string == NULL)
        return CM_RESOLVE_STRING_NONE;
    if (!cm_size_add(parent_string->len, name_string->len, &length) ||
        !cm_size_add(length, 2u, &length))
        cm_alloc_out_of_memory((size_t)-1);
    bytes = (unsigned char *)cm_alloc(length);
    memcpy(bytes, parent_string->bytes, parent_string->len);
    bytes[parent_string->len] = ':';
    bytes[parent_string->len + 1u] = ':';
    memcpy(bytes + parent_string->len + 2u, name_string->bytes,
        name_string->len);
    result = cm_graph_intern(state, bytes, length);
    cm_free(bytes);
    return result;
}

static void cm_graph_add_error(CmModuleGraphState *state,
    CmResolveErrorKind kind, CmSourceId source, uint32_t start, uint32_t end,
    CmResolveStringId module_path, CmResolveStringId detail_a,
    CmResolveStringId detail_b, uint32_t line, uint32_t column)
{
    CmResolveError error;

    memset(&error, 0, sizeof(error));
    error.kind = kind;
    error.span.source = source;
    error.span.start = start;
    error.span.end = end;
    error.module_path = module_path;
    error.detail_a = detail_a;
    error.detail_b = detail_b;
    error.line = line;
    error.column = column;
    (void)cm_vec_push(&state->errors, &error);
}

static CmResolveUnitId cm_find_unit(const CmModuleGraphState *state,
    CmSourceId source)
{
    size_t index;

    for (index = 0u; index < state->units.len; ++index) {
        const CmResolveUnit *unit;

        unit = (const CmResolveUnit *)cm_vec_at_const(&state->units, index);
        if (unit != NULL && unit->source == source)
            return (CmResolveUnitId)(index + 1u);
    }
    return 0u;
}

static CmResolveUnit *cm_get_unit(CmModuleGraphState *state,
    CmResolveUnitId id)
{
    if (id == 0u || (size_t)id > state->units.len) return NULL;
    return (CmResolveUnit *)cm_vec_at(&state->units, (size_t)id - 1u);
}

static const CmResolveUnit *cm_get_unit_const(
    const CmModuleGraphState *state, CmResolveUnitId id)
{
    if (id == 0u || (size_t)id > state->units.len) return NULL;
    return (const CmResolveUnit *)cm_vec_at_const(&state->units,
        (size_t)id - 1u);
}

static int cm_unit_source_invocation_span(const CmResolveUnit *unit,
    CmAstItemId source_invocation, CmAstSpan *out_span)
{
    const CmAstItem *item;

    if (out_span != NULL) memset(out_span, 0, sizeof(*out_span));
    if (unit == NULL || out_span == NULL
        || source_invocation == CM_AST_ITEM_NONE
        || (size_t)source_invocation > unit->parsed_item_count) return 0;
    item = cm_ast_get_item(&unit->ast, source_invocation);
    if (item == NULL) return 0;
    *out_span = item->span;
    return 1;
}

static int cm_graph_source_invocation_span(
    const CmModuleGraphState *state, CmItemMacroItemRef invocation,
    CmSourceId *out_source, CmAstSpan *out_span)
{
    CmResolveUnitId unit_id;
    const CmResolveUnit *unit;

    if (out_source != NULL) *out_source = 0u;
    if (out_span != NULL) memset(out_span, 0, sizeof(*out_span));
    if (out_source == NULL || out_span == NULL
        || invocation.owner == CM_ITEM_MACRO_AST_OWNER_NONE
        || invocation.owner > (CmItemMacroAstOwner)UINT32_MAX
        || invocation.item == CM_AST_ITEM_NONE) return 0;
    unit_id = cm_find_unit(state, (CmSourceId)invocation.owner);
    unit = cm_get_unit_const(state, unit_id);
    if (!cm_unit_source_invocation_span(unit, invocation.item, out_span))
        return 0;
    *out_source = unit->source;
    return 1;
}

static int cm_validate_generated_plan(CmModuleGraphState *state,
    CmResolveUnit *unit, const CmItemMacroPlanNode *nodes, size_t count)
{
    size_t index;

    if (count != 0u && nodes == NULL) return 0;
    for (index = 0u; index < count; ++index) {
        const CmAstItem *item;
        const char *detail;

        item = cm_ast_get_item(&unit->ast, nodes[index].item_id);
        if (item == NULL) return 0;
        detail = NULL;
        if (nodes[index].is_generated
            && item->kind == CM_AST_ITEM_EXTERN_CRATE) {
            detail = "generated extern crate items are not supported";
        }
        if (detail != NULL) {
            CmAstSpan anchor;
            CmSourceId source;

            source = 0u;
            (void)cm_graph_source_invocation_span(state,
                nodes[index].source_invocation, &source, &anchor);
            cm_graph_add_error(state,
                CM_RESOLVE_ERROR_UNSUPPORTED_GENERATED_ITEM, source,
                anchor.start, anchor.end, CM_RESOLVE_STRING_NONE,
                cm_graph_intern_c_str(state, detail),
                CM_RESOLVE_STRING_NONE, 0u, 0u);
            return 0;
        }
        if (!cm_validate_generated_plan(state, unit,
                nodes[index].children, nodes[index].child_count)) {
            return 0;
        }
    }
    return 1;
}

static void cm_graph_add_plan_error(CmModuleGraphState *state,
    const CmResolveUnit *unit, CmItemMacroPlanResult result)
{
    const CmAstItem *item;
    CmAstSpan anchor;
    CmSourceId span_source;
    uint32_t span_start;
    uint32_t span_end;
    const char *detail;

    item = cm_ast_get_item(&unit->ast, result.item_id);
    span_source = 0u;
    span_start = 0u;
    span_end = 0u;
    if (cm_graph_source_invocation_span(state, result.source_invocation,
            &span_source, &anchor)) {
        span_start = anchor.start;
        span_end = anchor.end;
    } else if (item != NULL && result.item_id != CM_AST_ITEM_NONE
        && (size_t)result.item_id <= unit->parsed_item_count) {
        span_source = cm_unit_item_source(unit, result.item_id);
        span_start = item->span.start;
        span_end = item->span.end;
    }
    detail = result.message;
    if (result.kind == CM_ITEM_MACRO_PLAN_DIAG_REPARSE
        && result.reparse.reparse.error_count != 0u
        && result.reparse.reparse.first_error.message[0] != '\0') {
        detail = result.reparse.reparse.first_error.message;
    }
    cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
        span_source, span_start, span_end, CM_RESOLVE_STRING_NONE,
        cm_graph_intern_c_str(state,
            cm_item_macro_plan_diagnostic_kind_name(result.kind)),
        cm_graph_intern_c_str(state, detail), 0u, 0u);
}

static int cm_prepare_unit_plan(CmModuleGraphState *state,
    CmResolveUnitId unit_id, const CmItemMacroItemRef *initial_scope,
    size_t initial_scope_count)
{
    CmResolveUnit *unit;
    CmItemMacroScopeSeed *seeds;
    size_t seed_index;
    CmExpandOptions expand_options;
    CmExpandedAst active;
    CmExpandResult expand;
    CmItemMacroPlanOptions plan_options;
    CmItemMacroPlanResult plan_result;

    unit = cm_get_unit(state, unit_id);
    if (unit != NULL && unit->plan_prepared) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
            unit->source, 0u, 0u, CM_RESOLVE_STRING_NONE,
            cm_graph_intern_c_str(state, "context-dependent macro plan"),
            cm_graph_intern_c_str(state,
                "source unit was requested with more than one lexical scope"),
            0u, 0u);
        return 0;
    }
    if (unit == NULL || !unit->parse_ok
        || (initial_scope_count != 0u && initial_scope == NULL)) return 0;
    unit->plan_prepared = 1;
    unit->initial_scope_count = initial_scope_count;
    if (initial_scope_count != 0u) {
        unit->initial_scope = (CmItemMacroItemRef *)cm_alloc(
            initial_scope_count * sizeof(CmItemMacroItemRef));
        memcpy(unit->initial_scope, initial_scope,
            initial_scope_count * sizeof(CmItemMacroItemRef));
    }
    seeds = NULL;
    if (initial_scope_count != 0u) {
        seeds = (CmItemMacroScopeSeed *)cm_alloc_zeroed(
            initial_scope_count, sizeof(CmItemMacroScopeSeed));
        for (seed_index = 0u; seed_index < initial_scope_count;
                ++seed_index) {
            CmResolveUnitId definition_unit_id;
            const CmResolveUnit *definition_unit;

            definition_unit_id = initial_scope[seed_index].owner
                    > (CmItemMacroAstOwner)UINT32_MAX
                ? 0u : cm_find_unit(state,
                    (CmSourceId)initial_scope[seed_index].owner);
            definition_unit = cm_get_unit_const(state, definition_unit_id);
            if (definition_unit == NULL) {
                cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                    unit->source, 0u, 0u, CM_RESOLVE_STRING_NONE,
                    cm_graph_intern_c_str(state, "invalid inherited scope"),
                    cm_graph_intern_c_str(state,
                        "macro definition owner is not in the graph"),
                    0u, 0u);
                cm_free(seeds);
                return 0;
            }
            seeds[seed_index].definition = initial_scope[seed_index];
            seeds[seed_index].definition_ast = &definition_unit->ast;
        }
    }
    cm_expanded_ast_init(&active);
    cm_expand_options_init(&expand_options, state->options.cfg);
    expand = cm_expand_cfg_view(&unit->ast, &expand_options, &active);
    if (expand.status != CM_MACRO_OK) {
        CmSourceId diagnostic_source;

        diagnostic_source = cm_unit_item_source(unit,
            expand.diagnostic.item_id);
        if (diagnostic_source == 0u) diagnostic_source = unit->source;
        cm_graph_add_error(state, CM_RESOLVE_ERROR_CFG_EXPANSION,
            diagnostic_source, expand.diagnostic.span.start,
            expand.diagnostic.span.end, CM_RESOLVE_STRING_NONE,
            cm_graph_intern_c_str(state,
                cm_expand_diagnostic_code_name(expand.diagnostic.code)),
            cm_graph_intern_c_str(state, expand.diagnostic.message), 0u, 0u);
        cm_expanded_ast_destroy(&active);
        cm_free(seeds);
        return 0;
    }
    cm_item_macro_plan_options_init(&plan_options, state->options.cfg);
    plan_options.current_owner = (CmItemMacroAstOwner)unit->source;
    plan_options.initial_scope = seeds;
    plan_options.initial_scope_count = initial_scope_count;
    plan_options.defer_source_invocations = 1;
    plan_result = cm_plan_item_macros(&active, &unit->ast, &plan_options,
        &unit->plan);
    cm_expanded_ast_destroy(&active);
    cm_free(seeds);
    if (plan_result.status != CM_MACRO_OK) {
        cm_graph_add_plan_error(state, unit, plan_result);
        return 0;
    }
    if (!cm_validate_generated_plan(state, unit, unit->plan.roots,
            unit->plan.root_count)) {
        return 0;
    }
    unit->plan_ok = 1;
    return 1;
}

static void cm_unit_clear_sticky(CmResolveUnit *unit)
{
    size_t index;

    for (index = 0u; index < unit->sticky_paths.len; ++index) {
        CmItemMacroResolvedGeneratedPath *entry =
            (CmItemMacroResolvedGeneratedPath *)cm_vec_at(
                &unit->sticky_paths, index);
        if (entry != NULL) cm_free((void *)entry->segments);
    }
    cm_vec_destroy(&unit->sticky_paths);
    cm_vec_destroy(&unit->sticky_resolved);
    cm_vec_init(&unit->sticky_resolved,
        sizeof(CmItemMacroResolvedInvocation));
    cm_vec_init(&unit->sticky_paths,
        sizeof(CmItemMacroResolvedGeneratedPath));
}

static int cm_replan_unit_with_resolved(CmModuleGraphState *state,
    CmResolveUnitId unit_id,
    const CmItemMacroResolvedInvocation *resolved,
    size_t resolved_count,
    const CmItemMacroResolvedGeneratedPath *generated_paths,
    size_t generated_path_count)
{
    CmResolveUnit *unit;
    CmItemMacroScopeSeed *seeds;
    size_t seed_index;
    CmExpandOptions expand_options;
    CmExpandedAst active;
    CmExpandResult expand;
    CmItemMacroPlanOptions plan_options;
    CmItemMacroPlanResult plan_result;

    unit = cm_get_unit(state, unit_id);
    if (unit == NULL || !unit->parse_ok || !unit->plan_prepared
        || (resolved_count != 0u && resolved == NULL)) return 0;
    seeds = NULL;
    if (unit->initial_scope_count != 0u) {
        seeds = (CmItemMacroScopeSeed *)cm_alloc_zeroed(
            unit->initial_scope_count, sizeof(CmItemMacroScopeSeed));
        for (seed_index = 0u; seed_index < unit->initial_scope_count;
                ++seed_index) {
            CmResolveUnitId definition_unit_id;
            const CmResolveUnit *definition_unit;

            definition_unit_id = unit->initial_scope[seed_index].owner
                    > (CmItemMacroAstOwner)UINT32_MAX
                ? 0u : cm_find_unit(state,
                    (CmSourceId)unit->initial_scope[seed_index].owner);
            definition_unit = cm_get_unit_const(state, definition_unit_id);
            if (definition_unit == NULL) {
                cm_free(seeds);
                return 0;
            }
            seeds[seed_index].definition = unit->initial_scope[seed_index];
            seeds[seed_index].definition_ast = &definition_unit->ast;
        }
    }
    cm_expanded_ast_init(&active);
    cm_expand_options_init(&expand_options, state->options.cfg);
    expand = cm_expand_cfg_view(&unit->ast, &expand_options, &active);
    if (expand.status != CM_MACRO_OK) {
        cm_expanded_ast_destroy(&active);
        cm_free(seeds);
        return 0;
    }
    unit->plan_ok = 0;
    cm_item_macro_plan_options_init(&plan_options, state->options.cfg);
    plan_options.current_owner = (CmItemMacroAstOwner)unit->source;
    plan_options.initial_scope = seeds;
    plan_options.initial_scope_count = unit->initial_scope_count;
    plan_options.resolved_invocations = resolved;
    plan_options.resolved_invocation_count = resolved_count;
    if (state->options.dependency_macro_count != 0u) {
        plan_options.resolve_generated_path =
            cm_resolve_generated_dependency_macro;
        plan_options.resolve_generated_path_context = state;
    }
    plan_options.resolved_generated_paths = generated_paths;
    plan_options.resolved_generated_path_count = generated_path_count;
    plan_options.defer_source_invocations = 1;
    plan_result = cm_plan_item_macros(&active, &unit->ast, &plan_options,
        &unit->plan);
    cm_expanded_ast_destroy(&active);
    cm_free(seeds);
    if (plan_result.status != CM_MACRO_OK) {
        cm_graph_add_plan_error(state, unit, plan_result);
        return 0;
    }
    if (!cm_validate_generated_plan(state, unit, unit->plan.roots,
            unit->plan.root_count)) return 0;
    unit->plan_ok = 1;
    return 1;
}

static int cm_unit_add_initial_scope(CmResolveUnit *unit,
    CmItemMacroItemRef definition)
{
    CmItemMacroItemRef *scope;
    size_t index;

    if (unit == NULL
        || definition.owner == CM_ITEM_MACRO_AST_OWNER_NONE
        || definition.owner > (CmItemMacroAstOwner)UINT32_MAX
        || definition.item == CM_AST_ITEM_NONE) return 0;
    for (index = 0u; index < unit->initial_scope_count; ++index) {
        if (unit->initial_scope[index].owner == definition.owner
            && unit->initial_scope[index].item == definition.item) return 1;
    }
    scope = (CmItemMacroItemRef *)cm_alloc(
        (unit->initial_scope_count + 1u) * sizeof(CmItemMacroItemRef));
    if (unit->initial_scope_count != 0u) {
        memcpy(scope, unit->initial_scope, unit->initial_scope_count
            * sizeof(CmItemMacroItemRef));
    }
    scope[unit->initial_scope_count] = definition;
    cm_free(unit->initial_scope);
    unit->initial_scope = scope;
    unit->initial_scope_count += 1u;
    return 1;
}

static int cm_expand_unit_includes(CmModuleGraphState *state,
    CmResolveUnitId unit_id);

static CmResolveUnitId cm_add_unit(CmModuleGraphState *state,
    CmSourceId source)
{
    const CmSourceFile *file;
    CmResolveUnit unit;
    CmParseResult parse_result;
    struct stat information;

    file = cm_source_get(state->building_sources, source);
    if (file == NULL) return 0u;
    memset(&unit, 0, sizeof(unit));
    unit.source = source;
    cm_ast_init(&unit.ast);
    cm_item_macro_plan_init(&unit.plan);
    cm_vec_init(&unit.item_sources, sizeof(CmSourceId));
    cm_vec_init(&unit.sticky_resolved, sizeof(CmItemMacroResolvedInvocation));
    cm_vec_init(&unit.sticky_paths, sizeof(CmItemMacroResolvedGeneratedPath));
    cm_vec_init(&unit.item_include_depths, sizeof(uint32_t));
    parse_result = cm_parse_crate(&unit.ast, (const char *)file->bytes,
        file->length, state->options.edition);
    unit.parse_ok = parse_result.error_count == 0u;
    /* Builtin derives: synthesized impls join the unit's items. */
    if (unit.parse_ok) {
        size_t compact_index;

        /* Derives read the fields, so cfg-inactive ones go first: core's
         * `#[derive(Debug)] struct BorrowError { #[cfg(feature =
         * "debug_refcell")] location: .. }` must not derive `location`. */
        for (compact_index = 0u; compact_index < unit.ast.items.len;
             ++compact_index) {
            const CmAstItem *parsed_item = cm_ast_get_item(&unit.ast,
                (CmAstItemId)(compact_index + 1u));
            if (parsed_item == NULL) continue;
            (void)cm_graph_compact_item_cfg_fields(state, &unit.ast,
                (CmAstItemId)(compact_index + 1u), parsed_item);
        }
        /* Crate-wide: a dependency provides `::core`, or the root unit
         * (parsed first) declares `extern crate self as core`. */
        if (!state->derive_core_reachable
            && cm_derive_unit_aliases_core(&unit.ast))
            state->derive_core_reachable = 1;
        (void)cm_derive_expand(&unit.ast, state->options.edition,
            state->options.dependency_macro_count != 0u
                || state->derive_core_reachable);
    }
    unit.parsed_item_count = unit.ast.items.len;
    if (unit.parse_ok) {
        size_t item_index;

        for (item_index = 0u; item_index < unit.ast.items.len;
                ++item_index) {
            uint32_t depth;

            depth = 0u;
            (void)cm_vec_push(&unit.item_sources, &source);
            (void)cm_vec_push(&unit.item_include_depths, &depth);
        }
    }
    if (cm_path_is_regular_file(file->path, &information)) {
        unit.has_identity = 1;
        unit.device = information.st_dev;
        unit.inode = information.st_ino;
    }
    (void)cm_vec_push(&state->units, &unit);
    if (!unit.parse_ok) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_PARSE, source,
            (uint32_t)parse_result.first_error.offset,
            (uint32_t)parse_result.first_error.offset,
            CM_RESOLVE_STRING_NONE,
            cm_graph_intern_c_str(state,
                parse_result.first_error.message),
            CM_RESOLVE_STRING_NONE,
            (uint32_t)parse_result.first_error.line,
            (uint32_t)parse_result.first_error.column);
    } else if (state->options.include_expansion
            == CM_INCLUDE_EXPANSION_SOURCE_FIXTURE
        && !cm_expand_unit_includes(state,
            (CmResolveUnitId)state->units.len)) {
        CmResolveUnit *stored_unit;

        stored_unit = cm_get_unit(state, (CmResolveUnitId)state->units.len);
        if (stored_unit != NULL) stored_unit->parse_ok = 0;
    }
    return (CmResolveUnitId)state->units.len;
}

static CmResolveUnitId cm_get_or_add_unit(CmModuleGraphState *state,
    CmSourceId source)
{
    CmResolveUnitId id;

    id = cm_find_unit(state, source);
    return id == 0u ? cm_add_unit(state, source) : id;
}

static CmResolveModuleNode *cm_get_module_node(CmModuleGraphState *state,
    CmModuleId id)
{
    if (id == 0u || (size_t)id > state->modules.len) return NULL;
    return (CmResolveModuleNode *)cm_vec_at(&state->modules,
        (size_t)id - 1u);
}

static const CmResolveModuleNode *cm_get_module_node_const(
    const CmModuleGraphState *state, CmModuleId id)
{
    if (id == 0u || (size_t)id > state->modules.len) return NULL;
    return (const CmResolveModuleNode *)cm_vec_at_const(&state->modules,
        (size_t)id - 1u);
}

static int cm_module_path_exists(const CmModuleGraphState *state,
    CmResolveStringId absolute_path)
{
    size_t index;

    for (index = 0u; index < state->modules.len; ++index) {
        const CmResolveModuleNode *node;

        node = (const CmResolveModuleNode *)cm_vec_at_const(&state->modules,
            index);
        if (node != NULL && node->info.absolute_path == absolute_path)
            return 1;
    }
    return 0;
}

/* Drop cfg-inactive fields in place (`#[cfg(gnu_time_bits64)] pub
 * tv_usec: __suseconds64_t` beside its `not(..)` twin in libc's
 * `timeval`): fields carry no effective view of their own, so the
 * unit-owned AST is compacted once and every consumer sees the active
 * set.  Idempotent across graph rounds. */
static int cm_graph_compact_cfg_fields(CmModuleGraphState *state,
    const CmAst *ast, CmAstItemId item_id, CmAstField *fields,
    uint32_t *count)
{
    CmExpandOptions expand_options;
    uint32_t read;
    uint32_t write = 0u;

    if (fields == NULL || count == NULL) return 1;
    if (getenv("CM_GRAPH_KEEP_CFG_FIELDS") != NULL) return 1;
    for (read = 0u; read < *count; ++read) {
        CmExpandedAttributeList expanded;
        CmExpandResult expand_result;
        int active = 1;

        if (fields[read].attribute_count != 0u) {
            cm_expand_options_init(&expand_options, state->options.cfg);
            cm_expanded_attribute_list_init(&expanded);
            expand_result = cm_expand_cfg_attribute_list(ast, item_id,
                fields[read].attributes, fields[read].attribute_count,
                &expand_options, &expanded);
            if (expand_result.status != CM_MACRO_OK) {
                cm_expanded_attribute_list_destroy(&expanded);
                return 0;
            }
            active = expanded.is_active;
            cm_expanded_attribute_list_destroy(&expanded);
        }
        if (!active) {
            if (getenv("CM_MACRO_DEBUG") != NULL) {
                const CmInternedString *fname = fields[read].name
                        == CM_INTERN_ID_NONE ? NULL
                    : cm_ast_get_string(ast, fields[read].name);
                fprintf(stderr, "MACRO cfg-inactive field dropped "
                    "item=%lu field=%.*s index=%lu\n",
                    (unsigned long)item_id, fname == NULL ? 1
                        : (int)fname->len,
                    fname == NULL ? "_" : (const char *)fname->bytes,
                    (unsigned long)read);
            }
            continue;
        }
        if (write != read) fields[write] = fields[read];
        write += 1u;
    }
    *count = write;
    return 1;
}

static int cm_graph_compact_item_cfg_fields(CmModuleGraphState *state,
    const CmAst *ast, CmAstItemId item_id, const CmAstItem *item)
{
    CmAstItem *mutable_item = (CmAstItem *)item;
    uint32_t index;

    if (item->kind == CM_AST_ITEM_STRUCT || item->kind == CM_AST_ITEM_UNION) {
        return cm_graph_compact_cfg_fields(state, ast, item_id,
            mutable_item->data.aggregate_item.fields,
            &mutable_item->data.aggregate_item.field_count);
    }
    if (item->kind == CM_AST_ITEM_ENUM) {
        for (index = 0u; index < item->data.enum_item.variant_count;
             ++index) {
            CmAstVariant *variant = &mutable_item->data.enum_item
                .variants[index];
            if (!cm_graph_compact_cfg_fields(state, ast, item_id,
                    variant->fields, &variant->field_count)) return 0;
        }
    }
    return 1;
}

static int cm_record_effective_plan_item(CmModuleGraphState *state,
    const CmResolveUnit *unit, const CmItemMacroPlanNode *node,
    CmResolveEffectiveItemId *next_id,
    CmResolveEffectiveItemRecord *out_record)
{
    CmSourceId source;
    const CmAst *ast;
    const CmAstItem *item;
    CmVec attributes;
    size_t index;

    source = cm_unit_item_source(unit, node->item_id);
    ast = &unit->ast;
    item = cm_ast_get_item(ast, node->item_id);
    if (source == 0u || item == NULL || next_id == NULL || out_record == NULL
        || *next_id == CM_RESOLVE_EFFECTIVE_ITEM_NONE) return 0;
    if (!cm_graph_compact_item_cfg_fields(state, ast, node->item_id, item))
        return 0;
    memset(out_record, 0, sizeof(*out_record));
    out_record->item.id = *next_id;
    *next_id += 1u;
    out_record->item.declaration.source = source;
    out_record->item.declaration.item = node->item_id;
    if (!node->is_generated) {
        out_record->item.provenance.source_item =
            out_record->item.declaration;
        out_record->item.span.source = source;
        out_record->item.span.start = node->span.start;
        out_record->item.span.end = node->span.end;
    } else {
        CmAstSpan anchor;
        CmSourceId anchor_source;

        if (cm_graph_source_invocation_span(state,
                node->source_invocation, &anchor_source, &anchor)) {
            out_record->item.span.source = anchor_source;
            out_record->item.span.start = anchor.start;
            out_record->item.span.end = anchor.end;
        }
    }
    if (node->invocation.owner != CM_ITEM_MACRO_AST_OWNER_NONE
        && node->invocation.item != CM_AST_ITEM_NONE) {
        if (node->invocation.owner > (CmItemMacroAstOwner)UINT32_MAX)
            return 0;
        out_record->item.provenance.macro_invocation.source =
            (CmSourceId)node->invocation.owner;
        out_record->item.provenance.macro_invocation.item =
            node->invocation.item;
    }
    if (node->definition.owner != CM_ITEM_MACRO_AST_OWNER_NONE
        && node->definition.item != CM_AST_ITEM_NONE) {
        if (node->definition.owner > (CmItemMacroAstOwner)UINT32_MAX) {
            CmResolveExternalMacro *external;

            external = cm_external_macro_for_plan_ref(state,
                node->definition);
            if (external == NULL) return 0;
            external->published = 1;
            out_record->item.provenance.dependency_macro_definition
                .consumer_graph = state->owner_graph;
            out_record->item.provenance.dependency_macro_definition
                .consumer_revision = state->revision;
            out_record->item.provenance.dependency_macro_definition
                .certificate = external->certificate;
            out_record->item.provenance.dependency_macro_definition
                .dependency = external->dependency;
            out_record->item.provenance.dependency_macro_definition
                .dependency_revision = external->revision;
            out_record->item.provenance.dependency_macro_definition
                .declaration = external->declaration;
        } else {
            out_record->item.provenance.macro_definition.source =
                (CmSourceId)node->definition.owner;
            out_record->item.provenance.macro_definition.item =
                node->definition.item;
        }
    }
    out_record->item.provenance.expansion_depth = node->expansion_depth;
    out_record->item.item_kind = item->kind;
    out_record->item.visibility = item->visibility.kind;
    out_record->item.is_generated = node->is_generated;
    cm_vec_init(&attributes, sizeof(CmResolveEffectiveAttribute));
    for (index = 0u; index < node->attribute_count; ++index) {
        const CmEffectiveAttribute *attribute;
        CmResolveEffectiveAttribute effective;

        attribute = &node->attributes[index];
        memset(&effective, 0, sizeof(effective));
        effective.source = source;
        effective.source_attribute = attribute->source_id;
        effective.owner = out_record->item.declaration;
        effective.style = attribute->style;
        if (!node->is_generated) {
            effective.span.source = source;
            effective.span.start = attribute->span.start;
            effective.span.end = attribute->span.end;
        } else {
            effective.span = out_record->item.span;
        }
        effective.metadata = cm_graph_intern(state, attribute->meta,
            attribute->meta_length);
        effective.expansion_depth = attribute->expansion_depth;
        (void)cm_vec_push(&attributes, &effective);
    }
    out_record->item.attribute_count = cm_count_u32(attributes.len);
    out_record->attributes = (CmResolveEffectiveAttribute *)cm_graph_copy_array(
        state, &attributes);
    cm_vec_destroy(&attributes);
    if (item->kind == CM_AST_ITEM_ENUM) {
        const CmAstEnum *enumeration;
        CmVec variants;

        enumeration = &item->data.enum_item;
        if ((enumeration->variant_count == 0u)
                != (enumeration->variants == NULL)) {
            return 0;
        }
        cm_vec_init(&variants, sizeof(CmResolveEffectiveVariantRecord));
        for (index = 0u; index < enumeration->variant_count; ++index) {
            const CmAstVariant *variant;
            CmExpandOptions expand_options;
            CmExpandedAttributeList expanded;
            CmExpandResult expand_result;
            CmResolveEffectiveVariantRecord effective_variant;
            size_t attribute_index;

            variant = &enumeration->variants[index];
            if (variant->name == CM_INTERN_ID_NONE
                || cm_ast_get_string(ast, variant->name) == NULL
                || variant->form > CM_AST_FIELDS_NAMED
                || variant->span.start > variant->span.end
                || variant->span.start < item->span.start
                || variant->span.end > item->span.end
                || ((variant->attribute_count == 0u)
                    != (variant->attributes == NULL))) {
                cm_vec_destroy(&variants);
                return 0;
            }
            for (attribute_index = 0u;
                 attribute_index < variant->attribute_count;
                 ++attribute_index) {
                const CmAstAttribute *source_attribute;

                source_attribute = cm_ast_get_attribute(ast,
                    variant->attributes[attribute_index]);
                if (source_attribute == NULL
                    || source_attribute->style != CM_AST_ATTR_OUTER
                    || source_attribute->span.start
                        < variant->span.start
                    || source_attribute->span.end > variant->span.end) {
                    cm_vec_destroy(&variants);
                    return 0;
                }
            }
            cm_expand_options_init(&expand_options, state->options.cfg);
            cm_expanded_attribute_list_init(&expanded);
            expand_result = cm_expand_cfg_attribute_list(ast, node->item_id,
                variant->attributes, variant->attribute_count,
                &expand_options, &expanded);
            if (expand_result.status != CM_MACRO_OK) {
                cm_expanded_attribute_list_destroy(&expanded);
                cm_vec_destroy(&variants);
                return 0;
            }
            if (!expanded.is_active) {
                cm_expanded_attribute_list_destroy(&expanded);
                continue;
            }
            memset(&effective_variant, 0, sizeof(effective_variant));
            effective_variant.variant.declaration.enumeration =
                out_record->item.declaration;
            effective_variant.variant.declaration.index = (uint32_t)index;
            effective_variant.variant.name = cm_graph_copy_ast_string(state,
                ast, variant->name);
            effective_variant.variant.form = variant->form;
            effective_variant.variant.field_count = variant->field_count;
            effective_variant.variant.attribute_count = cm_count_u32(
                expanded.attribute_count);
            effective_variant.variant.is_generated = node->is_generated;
            if (node->is_generated) {
                effective_variant.variant.span = out_record->item.span;
            } else {
                effective_variant.variant.span.source = source;
                effective_variant.variant.span.start = variant->span.start;
                effective_variant.variant.span.end = variant->span.end;
            }
            if (expanded.attribute_count != 0u) {
                CmVec variant_attributes;

                cm_vec_init(&variant_attributes,
                    sizeof(CmResolveEffectiveAttribute));
                for (attribute_index = 0u;
                     attribute_index < expanded.attribute_count;
                     ++attribute_index) {
                    const CmEffectiveAttribute *attribute;
                    const CmAstAttribute *source_attribute;
                    CmResolveEffectiveAttribute effective_attribute;

                    attribute = &expanded.attributes[attribute_index];
                    source_attribute = cm_ast_get_attribute(ast,
                        attribute->source_id);
                    if (source_attribute == NULL
                        || source_attribute->style != CM_AST_ATTR_OUTER
                        || source_attribute->span.start
                            < variant->span.start
                        || source_attribute->span.end > variant->span.end) {
                        cm_vec_destroy(&variant_attributes);
                        cm_expanded_attribute_list_destroy(&expanded);
                        cm_vec_destroy(&variants);
                        return 0;
                    }
                    memset(&effective_attribute, 0,
                        sizeof(effective_attribute));
                    effective_attribute.source = source;
                    effective_attribute.source_attribute =
                        attribute->source_id;
                    effective_attribute.owner = out_record->item.declaration;
                    effective_attribute.owner_variant =
                        effective_variant.variant.declaration;
                    effective_attribute.style = attribute->style;
                    effective_attribute.span = node->is_generated
                        ? out_record->item.span
                        : (CmSpan){ source, attribute->span.start,
                            attribute->span.end };
                    effective_attribute.metadata = cm_graph_intern(state,
                        attribute->meta, attribute->meta_length);
                    effective_attribute.expansion_depth =
                        attribute->expansion_depth;
                    (void)cm_vec_push(&variant_attributes,
                        &effective_attribute);
                }
                effective_variant.attributes =
                    (CmResolveEffectiveAttribute *)cm_graph_copy_array(state,
                        &variant_attributes);
                cm_vec_destroy(&variant_attributes);
            }
            cm_expanded_attribute_list_destroy(&expanded);
            (void)cm_vec_push(&variants, &effective_variant);
        }
        out_record->item.variant_count = cm_count_u32(variants.len);
        out_record->variants = (CmResolveEffectiveVariantRecord *)
            cm_graph_copy_array(state, &variants);
        cm_vec_destroy(&variants);
    }
    if (item->kind == CM_AST_ITEM_EXTERN_BLOCK
        || item->kind == CM_AST_ITEM_TRAIT
        || item->kind == CM_AST_ITEM_IMPL) {
        out_record->item.child_kind = node->child_kind;
        out_record->item.child_count = cm_count_u32(node->child_count);
        if (node->child_count != 0u) {
            out_record->children = (CmResolveEffectiveItemRecord *)
                cm_arena_alloc_zeroed(&state->storage, node->child_count,
                    sizeof(CmResolveEffectiveItemRecord), sizeof(void *));
            for (index = 0u; index < node->child_count; ++index) {
                if (!cm_record_effective_plan_item(state, unit,
                        &node->children[index], next_id,
                        &out_record->children[index])) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static CmResolveEffectiveAttribute *cm_record_effective_inner_attributes(
    CmModuleGraphState *state, const CmResolveUnit *unit,
    CmResolveItemRef owner, const CmEffectiveAttribute *attributes,
    size_t attribute_count, int generated, CmSpan generated_span)
{
    CmVec effective_attributes;
    size_t index;

    if (attribute_count == 0u) return NULL;
    if (attributes == NULL) return NULL;
    cm_vec_init(&effective_attributes, sizeof(CmResolveEffectiveAttribute));
    for (index = 0u; index < attribute_count; ++index) {
        CmResolveEffectiveAttribute effective;

        memset(&effective, 0, sizeof(effective));
        effective.source = unit->source;
        effective.source_attribute = attributes[index].source_id;
        effective.owner = owner;
        effective.style = attributes[index].style;
        /* A generated out-of-line `mod libunwind;` (unwind's cfg_if!)
         * still has source-written inner attributes in its own file:
         * anchor only what the invocation itself produced. */
        if (generated && generated_span.source == unit->source) {
            effective.span = generated_span;
        } else {
            effective.span.source = unit->source;
            effective.span.start = attributes[index].span.start;
            effective.span.end = attributes[index].span.end;
        }
        effective.metadata = cm_graph_intern(state, attributes[index].meta,
            attributes[index].meta_length);
        effective.expansion_depth = attributes[index].expansion_depth;
        (void)cm_vec_push(&effective_attributes, &effective);
    }
    {
        CmResolveEffectiveAttribute *result;

        result = (CmResolveEffectiveAttribute *)cm_graph_copy_array(state,
            &effective_attributes);
        cm_vec_destroy(&effective_attributes);
        return result;
    }
}

static int cm_effective_attribute_is_bare(
    const CmEffectiveAttribute *attribute, const char *name);

static void cm_record_namespace_entry_with_visibility(
    CmModuleGraphState *state,
    CmSourceId source, const CmAst *ast, CmAstItemId item_id,
    CmResolveNamespace namespace_kind, CmAstVisibilityKind visibility,
    CmVec *type_entries,
    CmVec *value_entries, CmVec *macro_entries)
{
    const CmAstItem *item;
    const CmInternedString *name;
    CmResolveNamespaceEntry entry;
    CmVec *destination;

    item = cm_ast_get_item(ast, item_id);
    if (item == NULL || item->name == CM_INTERN_ID_NONE) return;
    name = cm_ast_get_string(ast, item->name);
    /* `const _` is an item but deliberately introduces no value name. */
    if (item->kind == CM_AST_ITEM_CONST && name != NULL
        && name->len == 1u && name->bytes[0] == (unsigned char)'_') return;
    memset(&entry, 0, sizeof(entry));
    entry.name = cm_graph_copy_ast_string(state, ast,
        item->kind == CM_AST_ITEM_EXTERN_CRATE
            && item->data.extern_crate_item.alias != CM_INTERN_ID_NONE
        ? item->data.extern_crate_item.alias : item->name);
    entry.declaration.source = source;
    entry.declaration.item = item_id;
    entry.item_kind = item->kind;
    entry.visibility = visibility;
    destination = namespace_kind == CM_RESOLVE_NAMESPACE_TYPE ?
        type_entries : (namespace_kind == CM_RESOLVE_NAMESPACE_VALUE ?
        value_entries : macro_entries);
    (void)cm_vec_push(destination, &entry);
}

static void cm_record_namespace_entry(CmModuleGraphState *state,
    CmSourceId source, const CmAst *ast, CmAstItemId item_id,
    CmResolveNamespace namespace_kind, CmVec *type_entries,
    CmVec *value_entries, CmVec *macro_entries)
{
    const CmAstItem *item;

    item = cm_ast_get_item(ast, item_id);
    if (item == NULL) return;
    cm_record_namespace_entry_with_visibility(state, source, ast, item_id,
        namespace_kind, item->visibility.kind, type_entries, value_entries,
        macro_entries);
}

static CmAstVisibilityKind cm_struct_constructor_visibility(
    const CmAstItem *item, const CmEffectiveAttribute *attributes,
    size_t attribute_count)
{
    CmAstVisibilityKind visibility;
    size_t index;

    if (item == NULL) return CM_AST_VIS_INHERITED;
    if (item->visibility.kind == CM_AST_VIS_PUBLIC) {
        visibility = CM_AST_VIS_PUBLIC;
    } else if (item->visibility.kind == CM_AST_VIS_CRATE
        || item->visibility.kind == CM_AST_VIS_SUPER
        || item->visibility.kind == CM_AST_VIS_RESTRICTED) {
        visibility = CM_AST_VIS_CRATE;
    } else {
        visibility = CM_AST_VIS_INHERITED;
    }
    for (index = 0u; index < item->data.aggregate_item.field_count;
            ++index) {
        CmAstVisibilityKind field_visibility;

        if (item->data.aggregate_item.fields == NULL)
            return CM_AST_VIS_INHERITED;
        field_visibility =
            item->data.aggregate_item.fields[index].visibility.kind;
        if (field_visibility == CM_AST_VIS_PUBLIC) continue;
        if (field_visibility == CM_AST_VIS_CRATE) {
            if (visibility == CM_AST_VIS_PUBLIC)
                visibility = CM_AST_VIS_CRATE;
        } else {
            /*
             * `pub(super)` and `pub(in path)` meets need a path-aware schema.
             * Until then, retain local identity but conservatively record
             * those constructor restrictions as inherited.
             * TODO: retain the exact restriction-module identity here.
             */
            return CM_AST_VIS_INHERITED;
        }
    }
    for (index = 0u; index < attribute_count; ++index) {
        if (attributes == NULL) return CM_AST_VIS_INHERITED;
        if (cm_effective_attribute_is_bare(&attributes[index],
                "non_exhaustive") != 0
            && visibility == CM_AST_VIS_PUBLIC) {
            visibility = CM_AST_VIS_CRATE;
        }
    }
    return visibility;
}

static void cm_record_item_declarations(CmModuleGraphState *state,
    CmSourceId source, const CmAst *ast, CmAstItemId item_id,
    const CmEffectiveAttribute *attributes, size_t attribute_count,
    CmVec *type_entries, CmVec *value_entries, CmVec *macro_entries)
{
    const CmAstItem *item;

    item = cm_ast_get_item(ast, item_id);
    if (item == NULL) return;
    switch (item->kind) {
    case CM_AST_ITEM_FUNCTION:
    case CM_AST_ITEM_CONST:
    case CM_AST_ITEM_STATIC:
        cm_record_namespace_entry(state, source, ast, item_id,
            CM_RESOLVE_NAMESPACE_VALUE, type_entries, value_entries,
            macro_entries);
        break;
    case CM_AST_ITEM_STRUCT:
        cm_record_namespace_entry(state, source, ast, item_id,
            CM_RESOLVE_NAMESPACE_TYPE, type_entries, value_entries,
            macro_entries);
        if (item->data.aggregate_item.form != CM_AST_FIELDS_NAMED) {
            cm_record_namespace_entry_with_visibility(state, source, ast,
                item_id, CM_RESOLVE_NAMESPACE_VALUE,
                cm_struct_constructor_visibility(item, attributes,
                    attribute_count),
                type_entries, value_entries, macro_entries);
        }
        break;
    case CM_AST_ITEM_UNION:
        cm_record_namespace_entry(state, source, ast, item_id,
            CM_RESOLVE_NAMESPACE_TYPE, type_entries, value_entries,
            macro_entries);
        break;
    case CM_AST_ITEM_ENUM:
    case CM_AST_ITEM_TYPE_ALIAS:
    case CM_AST_ITEM_MODULE:
    case CM_AST_ITEM_EXTERN_CRATE:
    case CM_AST_ITEM_TRAIT:
        cm_record_namespace_entry(state, source, ast, item_id,
            CM_RESOLVE_NAMESPACE_TYPE, type_entries, value_entries,
            macro_entries);
        break;
    case CM_AST_ITEM_MACRO:
        cm_record_namespace_entry(state, source, ast, item_id,
            CM_RESOLVE_NAMESPACE_MACRO, type_entries, value_entries,
            macro_entries);
        break;
    case CM_AST_ITEM_EXTERN_BLOCK:
    case CM_AST_ITEM_USE:
    case CM_AST_ITEM_IMPL:
        break;
    }
}

static int cm_record_macro_declaration_metadata(
    CmModuleGraphState *state, const CmAst *ast, CmModuleId owner_module,
    const CmItemMacroDeclaration *declaration,
    const CmAstItem *item, CmSourceId source)
{
    CmResolveMacroDeclarationRecord record;
    CmVec attributes;
    size_t index;

    for (index = 0u; index < state->macro_declarations.len; ++index) {
        const CmResolveMacroDeclarationRecord *existing;

        existing = (const CmResolveMacroDeclarationRecord *)cm_vec_at_const(
            &state->macro_declarations, index);
        if (existing != NULL && existing->declaration.declaration.source
                == source
            && existing->declaration.declaration.item
                == declaration->item_id) return 0;
    }
    memset(&record, 0, sizeof(record));
    record.declaration.declaration.source = source;
    record.declaration.declaration.item = declaration->item_id;
    record.declaration.name = cm_graph_copy_ast_string(state,
        ast, item->name);
    record.declaration.owner_module = owner_module;
    record.declaration.form = declaration->form;
    record.declaration.visibility = item->visibility.kind;
    record.declaration.is_generated = declaration->is_generated;
    if (!declaration->is_generated) {
        record.declaration.span.source = source;
        record.declaration.span.start = declaration->span.start;
        record.declaration.span.end = declaration->span.end;
        record.declaration.provenance.source_item =
            record.declaration.declaration;
    } else {
        CmAstSpan anchor;
        CmSourceId anchor_source;

        if (cm_graph_source_invocation_span(state,
                declaration->source_invocation, &anchor_source, &anchor)) {
            record.declaration.span.source = anchor_source;
            record.declaration.span.start = anchor.start;
            record.declaration.span.end = anchor.end;
        }
    }
    if (declaration->invocation.owner != CM_ITEM_MACRO_AST_OWNER_NONE
        && declaration->invocation.item != CM_AST_ITEM_NONE) {
        if (declaration->invocation.owner
                > (CmItemMacroAstOwner)UINT32_MAX) return 0;
        record.declaration.provenance.macro_invocation.source =
            (CmSourceId)declaration->invocation.owner;
        record.declaration.provenance.macro_invocation.item =
            declaration->invocation.item;
    }
    if (declaration->definition.owner != CM_ITEM_MACRO_AST_OWNER_NONE
        && declaration->definition.item != CM_AST_ITEM_NONE) {
        if (declaration->definition.owner
                > (CmItemMacroAstOwner)UINT32_MAX) {
            CmResolveExternalMacro *external;

            external = cm_external_macro_for_plan_ref(state,
                declaration->definition);
            if (external == NULL) return 0;
            external->published = 1;
            record.declaration.provenance.dependency_macro_definition
                .consumer_graph = state->owner_graph;
            record.declaration.provenance.dependency_macro_definition
                .consumer_revision = state->revision;
            record.declaration.provenance.dependency_macro_definition
                .certificate = external->certificate;
            record.declaration.provenance.dependency_macro_definition
                .dependency = external->dependency;
            record.declaration.provenance.dependency_macro_definition
                .dependency_revision = external->revision;
            record.declaration.provenance.dependency_macro_definition
                .declaration = external->declaration;
        } else {
            record.declaration.provenance.macro_definition.source =
                (CmSourceId)declaration->definition.owner;
            record.declaration.provenance.macro_definition.item =
                declaration->definition.item;
        }
    }
    record.declaration.provenance.expansion_depth =
        declaration->expansion_depth;
    cm_vec_init(&attributes, sizeof(CmResolveEffectiveAttribute));
    for (index = 0u; index < declaration->attribute_count; ++index) {
        const CmEffectiveAttribute *attribute;
        CmResolveEffectiveAttribute effective;

        attribute = &declaration->attributes[index];
        memset(&effective, 0, sizeof(effective));
        effective.source = source;
        effective.source_attribute = attribute->source_id;
        effective.owner = record.declaration.declaration;
        effective.style = attribute->style;
        if (declaration->is_generated) {
            effective.span = record.declaration.span;
        } else {
            effective.span.source = source;
            effective.span.start = attribute->span.start;
            effective.span.end = attribute->span.end;
        }
        effective.metadata = cm_graph_intern(state, attribute->meta,
            attribute->meta_length);
        effective.expansion_depth = attribute->expansion_depth;
        (void)cm_vec_push(&attributes, &effective);
    }
    record.declaration.attribute_count = cm_count_u32(attributes.len);
    record.attributes = (CmResolveEffectiveAttribute *)cm_graph_copy_array(
        state, &attributes);
    cm_vec_destroy(&attributes);
    (void)cm_vec_push(&state->macro_declarations, &record);
    return 1;
}

static int cm_record_macro_declarations(CmModuleGraphState *state,
    const CmResolveUnit *unit, CmModuleId owner_module,
    CmAstItemId container_item, CmVec *macro_entries)
{
    size_t index;

    if (unit->plan.declaration_count != 0u
        && unit->plan.declarations == NULL) return 0;
    for (index = 0u; index < unit->plan.declaration_count; ++index) {
        const CmItemMacroDeclaration *declaration;
        const CmAstItem *item;
        CmSourceId source;

        declaration = &unit->plan.declarations[index];
            if (declaration->container_item != container_item) continue;
        item = cm_ast_get_item(&unit->ast, declaration->item_id);
        source = cm_unit_item_source(unit, declaration->item_id);
        if (item == NULL || item->kind != CM_AST_ITEM_MACRO
            || item->name == CM_INTERN_ID_NONE || source == 0u
            || item->data.macro_item.form != declaration->form
            || item->span.start != declaration->span.start
            || item->span.end != declaration->span.end) {
            return 0;
        }
        cm_record_namespace_entry(state, source, &unit->ast,
            declaration->item_id, CM_RESOLVE_NAMESPACE_MACRO,
            NULL, NULL, macro_entries);
        if (!cm_record_macro_declaration_metadata(state, &unit->ast,
                owner_module,
                declaration, item, source)) return 0;
    }
    return 1;
}

/* Returns 1 for an exact bare attribute, -1 for this head with arguments. */
static int cm_attribute_bytes_is_bare(const unsigned char *bytes,
    size_t length, const char *name)
{
    size_t start;
    size_t end;
    size_t name_length;

    if (length != 0u && bytes == NULL) return 0;
    start = 0u;
    end = length;
    while (start < end && (bytes[start] == ' '
        || bytes[start] == '\t'
        || bytes[start] == '\r'
        || bytes[start] == '\n')) ++start;
    name_length = strlen(name);
    if (end - start < name_length
        || memcmp(bytes + start, name, name_length) != 0) {
        return 0;
    }
    start += name_length;
    if (start < end && ((bytes[start] >= 'a'
            && bytes[start] <= 'z')
        || (bytes[start] >= 'A'
            && bytes[start] <= 'Z')
        || (bytes[start] >= '0'
            && bytes[start] <= '9')
        || bytes[start] == '_')) return 0;
    while (start < end && (bytes[start] == ' '
        || bytes[start] == '\t'
        || bytes[start] == '\r'
        || bytes[start] == '\n')) ++start;
    return start == end ? 1 : -1;
}

static int cm_effective_attribute_is_bare(
    const CmEffectiveAttribute *attribute, const char *name)
{
    if (attribute == NULL) return 0;
    return cm_attribute_bytes_is_bare(attribute->meta,
        attribute->meta_length, name);
}

static int cm_effective_attribute_simple_string(
    const CmEffectiveAttribute *attribute, const char *name,
    const unsigned char **out_bytes, size_t *out_length)
{
    const unsigned char *bytes;
    size_t length;
    size_t name_length;
    size_t index;
    size_t start;

    if (out_bytes != NULL) *out_bytes = NULL;
    if (out_length != NULL) *out_length = 0u;
    if (attribute == NULL || name == NULL || out_bytes == NULL
        || out_length == NULL) return 0;
    bytes = attribute->meta;
    length = attribute->meta_length;
    index = 0u;
    while (index < length && (bytes[index] == ' '
        || bytes[index] == '\t' || bytes[index] == '\r'
        || bytes[index] == '\n')) ++index;
    name_length = strlen(name);
    if (length - index < name_length
        || memcmp(bytes + index, name, name_length) != 0) return 0;
    index += name_length;
    if (index < length && ((bytes[index] >= 'a' && bytes[index] <= 'z')
        || (bytes[index] >= 'A' && bytes[index] <= 'Z')
        || (bytes[index] >= '0' && bytes[index] <= '9')
        || bytes[index] == '_')) return 0;
    while (index < length && (bytes[index] == ' '
        || bytes[index] == '\t' || bytes[index] == '\r'
        || bytes[index] == '\n')) ++index;
    if (index >= length || bytes[index] != '=') return -1;
    ++index;
    while (index < length && (bytes[index] == ' '
        || bytes[index] == '\t' || bytes[index] == '\r'
        || bytes[index] == '\n')) ++index;
    if (index >= length || bytes[index] != '"') return -1;
    ++index;
    start = index;
    while (index < length && bytes[index] != '"') {
        if (bytes[index] == 0u || bytes[index] == '\\'
            || bytes[index] == '\r' || bytes[index] == '\n') return -1;
        ++index;
    }
    if (index == start || index >= length || bytes[index] != '"'
        || bytes[start] == '/') return -1;
    *out_bytes = bytes + start;
    *out_length = index - start;
    ++index;
    while (index < length && (bytes[index] == ' '
        || bytes[index] == '\t' || bytes[index] == '\r'
        || bytes[index] == '\n')) ++index;
    return index == length ? 1 : -1;
}

static int cm_resolve_attribute_is_bare(const CmModuleGraphState *state,
    const CmResolveEffectiveAttribute *attribute, const char *name)
{
    const CmInternedString *metadata;

    if (attribute == NULL) return 0;
    metadata = cm_graph_string(state, attribute->metadata);
    if (metadata == NULL) return 0;
    return cm_attribute_bytes_is_bare(metadata->bytes, metadata->len, name);
}

static size_t cm_macro_declaration_module_count(
    const CmModuleGraphState *state, CmResolveItemRef declaration,
    CmResolveNamespaceEntry *out_entry)
{
    size_t declaration_index;
    size_t count;

    count = 0u;
    for (declaration_index = 0u;
            declaration_index < state->macro_declarations.len;
            ++declaration_index) {
        const CmResolveMacroDeclarationRecord *record;
        const CmResolveModuleNode *module;
        uint32_t entry_index;

        record = (const CmResolveMacroDeclarationRecord *)cm_vec_at_const(
            &state->macro_declarations, declaration_index);
        if (record == NULL || !cm_resolve_item_ref_equal(
                record->declaration.declaration, declaration)) continue;
        module = cm_get_module_node_const(state,
            record->declaration.owner_module);
        if (module == NULL) return count + 2u;
        for (entry_index = 0u; entry_index < module->info.macro_count;
                ++entry_index) {
            const CmResolveNamespaceEntry *entry;

            entry = &module->macro_entries[entry_index];
            if (cm_resolve_item_ref_equal(entry->declaration,
                    declaration)) {
                if (out_entry != NULL) *out_entry = *entry;
                count += 1u;
            }
        }
    }
    return count;
}

static int cm_apply_macro_exports(CmModuleGraphState *state,
    CmModuleId root_id)
{
    CmResolveModuleNode *root;
    CmVec root_macros;
    size_t unit_index;
    int ok;

    root = cm_get_module_node(state, root_id);
    if (root == NULL) return 0;
    cm_vec_init(&root_macros, sizeof(CmResolveNamespaceEntry));
    if (root->info.macro_count != 0u) {
        cm_vec_append(&root_macros, root->macro_entries,
            root->info.macro_count);
    }
    ok = 1;
    for (unit_index = 0u; unit_index < state->units.len; ++unit_index) {
        const CmResolveUnit *unit;
        size_t declaration_index;

        unit = (const CmResolveUnit *)cm_vec_at_const(&state->units,
            unit_index);
        if (unit == NULL || !unit->plan_ok) continue;
        for (declaration_index = 0u;
                declaration_index < unit->plan.declaration_count;
                ++declaration_index) {
            const CmItemMacroDeclaration *declaration;
            size_t attribute_index;
            size_t export_count;
            int malformed;
            CmResolveItemRef reference;
            CmResolveNamespaceEntry exported;
            size_t local_count;
            size_t existing_index;
            int already_present;

            declaration = &unit->plan.declarations[declaration_index];
            export_count = 0u;
            malformed = 0;
            for (attribute_index = 0u;
                    attribute_index < declaration->attribute_count;
                    ++attribute_index) {
                int match;

                match = cm_effective_attribute_is_bare(
                    &declaration->attributes[attribute_index],
                    "macro_export");
                if (match > 0) export_count += 1u;
                else if (match < 0) malformed = 1;
            }
            if (export_count == 0u && !malformed) continue;
            reference.source = cm_unit_item_source(unit,
                declaration->item_id);
            reference.item = declaration->item_id;
            if (reference.source == 0u || malformed || export_count != 1u
                || declaration->form
                    != CM_AST_MACRO_RULES_DEFINITION) {
                cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                    reference.source, declaration->span.start,
                    declaration->span.end, root->info.absolute_path,
                    cm_graph_intern_c_str(state,
                        "unsupported macro_export declaration"),
                    cm_graph_intern_c_str(state,
                        "macro_export must be one bare attribute on macro_rules"),
                    0u, 0u);
                ok = 0;
                continue;
            }
            memset(&exported, 0, sizeof(exported));
            local_count = cm_macro_declaration_module_count(state,
                reference, &exported);
            if (local_count != 1u) {
                cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                    reference.source, declaration->span.start,
                    declaration->span.end, root->info.absolute_path,
                    cm_graph_intern_c_str(state,
                        "macro_export declaration has no unique owner"),
                    CM_RESOLVE_STRING_NONE, 0u, 0u);
                ok = 0;
                continue;
            }
            exported.visibility = CM_AST_VIS_PUBLIC;
            already_present = 0;
            for (existing_index = 0u; existing_index < root_macros.len;
                    ++existing_index) {
                CmResolveNamespaceEntry *existing;

                existing = (CmResolveNamespaceEntry *)cm_vec_at(
                    &root_macros, existing_index);
                if (existing == NULL || existing->name != exported.name) {
                    continue;
                }
                if (cm_resolve_item_ref_equal(existing->declaration,
                        exported.declaration)) {
                    existing->visibility = CM_AST_VIS_PUBLIC;
                    already_present = 1;
                    break;
                }
                cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                    reference.source, declaration->span.start,
                    declaration->span.end, root->info.absolute_path,
                    cm_graph_intern_c_str(state,
                        "distinct macro_export declarations share a name"),
                    exported.name, 0u, 0u);
                ok = 0;
                already_present = 1;
                break;
            }
            if (!already_present) (void)cm_vec_push(&root_macros, &exported);
        }
    }
    if (ok) {
        root = cm_get_module_node(state, root_id);
        root->macro_entries = (CmResolveNamespaceEntry *)cm_graph_copy_array(
            state, &root_macros);
        root->info.macro_count = cm_count_u32(root_macros.len);
    }
    cm_vec_destroy(&root_macros);
    return ok;
}

typedef struct CmMacroScopeEvent {
    CmSpan span;
    size_t ordinal;
    const CmResolveMacroDeclarationRecord *local;
    CmModuleId child;
    CmResolveItemRef introduced_by;
} CmMacroScopeEvent;

static void cm_macro_scope_bind(CmVec *scope,
    const CmResolveMacroScopeEntry *entry)
{
    size_t index;

    for (index = 0u; index < scope->len; ++index) {
        const CmResolveMacroScopeEntry *existing;

        existing = (const CmResolveMacroScopeEntry *)cm_vec_at_const(
            scope, index);
        if (existing != NULL && existing->name == entry->name) {
            cm_graph_vec_remove(scope, index);
            break;
        }
    }
    (void)cm_vec_push(scope, entry);
}

static int cm_macro_scope_event_before(const CmMacroScopeEvent *left,
    const CmMacroScopeEvent *right)
{
    if (left->span.start != right->span.start)
        return left->span.start < right->span.start;
    if (left->span.end != right->span.end)
        return left->span.end < right->span.end;
    return left->ordinal < right->ordinal;
}

static CmModuleId cm_child_for_declaration(
    const CmModuleGraphState *state, const CmResolveModuleNode *parent,
    CmResolveItemRef declaration)
{
    uint32_t index;

    for (index = 0u; index < parent->info.child_count; ++index) {
        const CmResolveModuleNode *child;

        child = cm_get_module_node_const(state, parent->children[index]);
        if (child != NULL && cm_resolve_item_ref_equal(
                child->info.declaration, declaration)) return child->info.id;
    }
    return CM_MODULE_NONE;
}

static int cm_build_module_macro_scope(CmModuleGraphState *state,
    CmModuleId module_id)
{
    CmResolveModuleNode *module;
    CmVec events;
    CmVec scope;
    CmVec history;
    size_t index;
    int ok;

    module = cm_get_module_node(state, module_id);
    if (module == NULL) return 0;
    cm_vec_init(&events, sizeof(CmMacroScopeEvent));
    cm_vec_init(&scope, sizeof(CmResolveMacroScopeEntry));
    cm_vec_init(&history, sizeof(CmResolveMacroScopeEntry));
    ok = 1;
    for (index = 0u; index < state->macro_declarations.len; ++index) {
        const CmResolveMacroDeclarationRecord *record;
        CmMacroScopeEvent event;

        record = (const CmResolveMacroDeclarationRecord *)cm_vec_at_const(
            &state->macro_declarations, index);
        if (record == NULL
            || record->declaration.owner_module != module_id) continue;
        if (record->declaration.span.source != module->info.source
            || record->declaration.span.start
                > record->declaration.span.end) {
            cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                record->declaration.span.source,
                record->declaration.span.start,
                record->declaration.span.end, module->info.absolute_path,
                cm_graph_intern_c_str(state,
                    "unsupported macro declaration scope order"),
                CM_RESOLVE_STRING_NONE, 0u, 0u);
            ok = 0;
            continue;
        }
        memset(&event, 0, sizeof(event));
        event.span = record->declaration.span;
        event.ordinal = index;
        event.local = record;
        (void)cm_vec_push(&events, &event);
    }
    for (index = 0u; index < module->info.effective_item_count; ++index) {
        const CmResolveEffectiveItemRecord *item;
        uint32_t attribute_index;
        size_t macro_use_count;
        int malformed;

        item = &module->effective_items[index];
        macro_use_count = 0u;
        malformed = 0;
        for (attribute_index = 0u;
                attribute_index < item->item.attribute_count;
                ++attribute_index) {
            int match;

            match = cm_resolve_attribute_is_bare(state,
                &item->attributes[attribute_index], "macro_use");
            if (match > 0) macro_use_count += 1u;
            else if (match < 0) malformed = 1;
        }
        if (macro_use_count == 0u && !malformed) continue;
        if (!malformed && macro_use_count == 1u
            && item->item.item_kind == CM_AST_ITEM_EXTERN_CRATE) {
            /* `#[macro_use] extern crate alloc as alloc_crate;` (std):
             * remember the crate's extern name; its exported macros are
             * looked up unqualified through the dependency artifacts. */
            const CmResolveUnit *crate_unit = cm_get_unit_const(state,
                module->unit);
            const CmAstItem *crate_item = crate_unit == NULL ? NULL
                : cm_ast_get_item(&crate_unit->ast,
                    item->item.declaration.item);
            const CmInternedString *crate_name = crate_item == NULL ? NULL
                : cm_ast_get_string(&crate_unit->ast, crate_item->name);
            if (crate_name != NULL && crate_name->len < 64u) {
                char stored[64];
                size_t scan;
                int known = 0;
                memset(stored, 0, sizeof(stored));
                memcpy(stored, crate_name->bytes, crate_name->len);
                for (scan = 0u; scan < state->macro_use_crates.len; ++scan) {
                    const char *have = (const char *)cm_vec_at_const(
                        &state->macro_use_crates, scan);
                    if (have != NULL && strcmp(have, stored) == 0) known = 1;
                }
                if (!known) (void)cm_vec_push(&state->macro_use_crates,
                    stored);
            }
            continue;
        }
        if (malformed || macro_use_count != 1u
            || item->item.item_kind != CM_AST_ITEM_MODULE
            || item->item.is_generated) {
            cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                item->item.span.source, item->item.span.start,
                item->item.span.end, module->info.absolute_path,
                cm_graph_intern_c_str(state,
                    "unsupported macro_use declaration"),
                cm_graph_intern_c_str(state,
                    "macro_use must be one bare attribute on a source module"),
                0u, 0u);
            ok = 0;
        } else {
            CmMacroScopeEvent event;

            memset(&event, 0, sizeof(event));
            event.span = item->item.span;
            event.ordinal = state->macro_declarations.len + index;
            event.introduced_by = item->item.declaration;
            event.child = cm_child_for_declaration(state, module,
                item->item.declaration);
            if (event.child == CM_MODULE_NONE) {
                cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                    item->item.span.source, item->item.span.start,
                    item->item.span.end, module->info.absolute_path,
                    cm_graph_intern_c_str(state,
                        "macro_use module has no exact child"),
                    CM_RESOLVE_STRING_NONE, 0u, 0u);
                ok = 0;
            } else {
                (void)cm_vec_push(&events, &event);
            }
        }
    }
    for (index = 1u; index < events.len; ++index) {
        CmMacroScopeEvent value;
        size_t position;

        value = *(const CmMacroScopeEvent *)cm_vec_at_const(&events, index);
        position = index;
        while (position != 0u) {
            CmMacroScopeEvent *previous;

            previous = (CmMacroScopeEvent *)cm_vec_at(&events,
                position - 1u);
            if (previous == NULL
                || cm_macro_scope_event_before(previous, &value)) break;
            *(CmMacroScopeEvent *)cm_vec_at(&events, position) = *previous;
            position -= 1u;
        }
        *(CmMacroScopeEvent *)cm_vec_at(&events, position) = value;
    }
    for (index = 0u; index < events.len; ++index) {
        const CmMacroScopeEvent *event;

        event = (const CmMacroScopeEvent *)cm_vec_at_const(&events, index);
        if (event == NULL) continue;
        if (event->local != NULL) {
            CmResolveMacroScopeEntry entry;

            memset(&entry, 0, sizeof(entry));
            entry.name = event->local->declaration.name;
            entry.declaration = event->local->declaration.declaration;
            entry.introduced_by = entry.declaration;
            entry.introduction_span = event->local->declaration.span;
            entry.form = event->local->declaration.form;
            (void)cm_vec_push(&history, &entry);
            cm_macro_scope_bind(&scope, &entry);
        } else {
            const CmResolveModuleNode *child;
            uint32_t child_index;

            child = cm_get_module_node_const(state, event->child);
            if (child == NULL) {
                ok = 0;
                continue;
            }
            for (child_index = 0u;
                    child_index < child->info.macro_scope_count;
                    ++child_index) {
                CmResolveMacroScopeEntry entry;

                entry = child->macro_scope_entries[child_index];
                if (entry.form != CM_AST_MACRO_RULES_DEFINITION) continue;
                entry.introduced_by = event->introduced_by;
                entry.introduction_span = event->span;
                entry.is_macro_use = 1;
                (void)cm_vec_push(&history, &entry);
                cm_macro_scope_bind(&scope, &entry);
            }
        }
    }
    module = cm_get_module_node(state, module_id);
    module->macro_scope_entries = (CmResolveMacroScopeEntry *)
        cm_graph_copy_array(state, &scope);
    module->info.macro_scope_count = cm_count_u32(scope.len);
    module->macro_scope_history = (CmResolveMacroScopeEntry *)
        cm_graph_copy_array(state, &history);
    module->macro_scope_history_count = history.len;
    cm_vec_destroy(&history);
    cm_vec_destroy(&scope);
    cm_vec_destroy(&events);
    return ok;
}

/* The effective-item record declared by `declaration`, searching nested
 * generated children as well. */
static const CmResolveEffectiveItemRecord *cm_find_effective_by_declaration(
    const CmResolveEffectiveItemRecord *items, uint32_t count,
    CmResolveItemRef declaration)
{
    uint32_t index;

    if (items == NULL) return NULL;
    for (index = 0u; index < count; ++index) {
        const CmResolveEffectiveItemRecord *found;

        if (items[index].item.declaration.source == declaration.source
            && items[index].item.declaration.item == declaration.item) {
            return &items[index];
        }
        found = cm_find_effective_by_declaration(items[index].children,
            items[index].item.child_count, declaration);
        if (found != NULL) return found;
    }
    return NULL;
}

static int cm_propagate_inherited_macro_histories(
    CmModuleGraphState *state)
{
    size_t index;

    for (index = 0u; index < state->modules.len; ++index) {
        CmResolveModuleNode *child;
        const CmResolveModuleNode *parent;
        const CmResolveUnit *parent_unit;
        const CmAstItem *declaration;
        CmVec merged;
        size_t history_index;
        CmSourceId anchor_source;
        uint32_t anchor_start;

        child = (CmResolveModuleNode *)cm_vec_at(&state->modules, index);
        if (child == NULL || child->info.parent == CM_MODULE_NONE) continue;
        parent = cm_get_module_node_const(state, child->info.parent);
        parent_unit = parent == NULL ? NULL
            : cm_get_unit_const(state, parent->unit);
        declaration = parent_unit == NULL ? NULL : cm_ast_get_item(
            &parent_unit->ast, child->info.declaration.item);
        if (parent == NULL || declaration == NULL) return 0;
        /* A macro-generated `mod x;` (libc's `cfg_if! { .. mod primitives;
         * .. }`) sits at its source invocation's anchor, not at the
         * synthetic reparse offset its AST item carries. */
        anchor_source = child->info.declaration.source;
        anchor_start = declaration->span.start;
        if ((size_t)child->info.declaration.item
                > parent_unit->parsed_item_count) {
            const CmResolveEffectiveItemRecord *record;

            record = cm_find_effective_by_declaration(
                parent->effective_items, parent->info.effective_item_count,
                child->info.declaration);
            if (record != NULL && record->item.is_generated
                && record->item.span.source != 0u) {
                anchor_source = record->item.span.source;
                anchor_start = record->item.span.start;
            }
        }
        cm_vec_init(&merged, sizeof(CmResolveMacroScopeEntry));
        for (history_index = 0u;
                history_index < parent->macro_scope_history_count;
                ++history_index) {
            CmResolveMacroScopeEntry inherited;

            inherited = parent->macro_scope_history[history_index];
            if (inherited.introduction_span.source != anchor_source
                || inherited.introduction_span.end > anchor_start) continue;
            inherited.introduction_span.source = child->info.source;
            inherited.introduction_span.start = 0u;
            inherited.introduction_span.end = 0u;
            (void)cm_vec_push(&merged, &inherited);
        }
        if (child->macro_scope_history_count != 0u) {
            cm_vec_append(&merged, child->macro_scope_history,
                child->macro_scope_history_count);
        }
        child = (CmResolveModuleNode *)cm_vec_at(&state->modules, index);
        child->macro_scope_history = (CmResolveMacroScopeEntry *)
            cm_graph_copy_array(state, &merged);
        child->macro_scope_history_count = merged.len;
        cm_vec_destroy(&merged);
    }
    return 1;
}

static int cm_apply_macro_uses(CmModuleGraphState *state)
{
    size_t index;
    int ok;

    ok = 1;
    index = state->modules.len;
    while (index != 0u) {
        index -= 1u;
        if (!cm_build_module_macro_scope(state, (CmModuleId)(index + 1u)))
            ok = 0;
    }
    if (ok && !cm_propagate_inherited_macro_histories(state)) ok = 0;
    return ok;
}

static int cm_graph_ast_names_equal(const CmModuleGraphState *state,
    CmResolveStringId graph_name, const CmAst *ast, CmInternId ast_name)
{
    const CmInternedString *left;
    const CmInternedString *right;

    left = cm_graph_string(state, graph_name);
    right = cm_ast_get_string(ast, ast_name);
    return left != NULL && right != NULL && left->len == right->len
        && memcmp(left->bytes, right->bytes, left->len) == 0;
}

static int cm_pending_invocation_name(const CmResolveUnit *unit,
    const CmItemMacroPendingInvocation *pending, CmInternId *out_name)
{
    const CmAstItem *item;
    const CmAstPath *path;
    const CmAstPathSegment *segment;

    item = cm_ast_get_item(&unit->ast, pending->invocation.item);
    if (item == NULL || item->kind != CM_AST_ITEM_MACRO
        || item->data.macro_item.form != CM_AST_MACRO_INVOCATION) return 0;
    path = cm_ast_get_path(&unit->ast, item->data.macro_item.path);
    if (path == NULL || path->segment_count == 0u) return 0;
    segment = &path->segments[path->segment_count - 1u];
    if (cm_ast_get_string(&unit->ast, segment->name) == NULL) return 0;
    *out_name = segment->name;
    return 1;
}

static const CmResolveModuleNode *cm_pending_owner_module(
    const CmModuleGraphState *state, CmResolveUnitId unit_id,
    CmAstItemId container_item)
{
    size_t index;

    const CmResolveModuleNode *file_module = NULL;

    for (index = 0u; index < state->modules.len; ++index) {
        const CmResolveModuleNode *module;

        module = (const CmResolveModuleNode *)cm_vec_at_const(
            &state->modules, index);
        if (module == NULL || module->unit != unit_id) continue;
        if (!module->info.is_inline && file_module == NULL)
            file_module = module;
        if (container_item == CM_AST_ITEM_NONE) {
            if (!module->info.is_inline) return module;
        } else if (module->info.is_inline
            && module->info.declaration.item == container_item) {
            return module;
        }
    }
    /* A non-module container (std's `extern "C" { cfg_if::cfg_if! {..} }`
     * in cmath.rs) resolves names in the unit's file module. */
    return container_item == CM_AST_ITEM_NONE ? NULL : file_module;
}

/* "<reason>: <name>" for a macro invocation without a binding; the name
 * is what a reader needs to find the missing definition. */
static const char *cm_graph_missing_macro_detail(
    const CmModuleGraphState *state, const CmResolveUnit *unit,
    CmInternId name, int is_qualified)
{
    static char buffer[256];
    const CmInternedString *text;
    const char *reason;

    (void)state;
    reason = is_qualified ? "no unique public local-crate macro binding"
        : "no declaration-ordered macro binding";
    text = name == CM_INTERN_ID_NONE || is_qualified ? NULL
        : cm_ast_get_string(&unit->ast, name);
    if (text == NULL) return reason;
    snprintf(buffer, sizeof(buffer), "%s: %.*s", reason,
        (int)(text->len > 200u ? 200u : text->len),
        (const char *)text->bytes);
    return buffer;
}

static const CmResolveMacroScopeEntry *cm_resolve_pending_from_scope(
    const CmModuleGraphState *state, const CmResolveModuleNode *module,
    const CmResolveUnit *unit,
    const CmItemMacroPendingInvocation *pending, CmInternId name)
{
    const CmResolveMacroScopeEntry *resolved;
    size_t index;

    resolved = NULL;
    for (index = 0u; index < module->macro_scope_history_count; ++index) {
        const CmResolveMacroScopeEntry *entry;

        entry = &module->macro_scope_history[index];
        if (entry->introduction_span.source != unit->source
            || entry->introduction_span.end > pending->span.start
            || !cm_graph_ast_names_equal(state, entry->name,
                &unit->ast, name)) continue;
        resolved = entry;
    }
    return resolved;
}

static const CmResolveMacroScopeEntry *cm_resolve_pending_from_namespace(
    const CmModuleGraphState *state, const CmResolveModuleNode *module,
    const CmResolveUnit *unit, CmInternId name,
    CmResolveMacroScopeEntry *out_entry)
{
    uint32_t index;
    size_t matches;

    memset(out_entry, 0, sizeof(*out_entry));
    matches = 0u;
    for (index = 0u; index < module->info.macro_count; ++index) {
        const CmResolveNamespaceEntry *entry;

        entry = &module->macro_entries[index];
        if (!cm_graph_ast_names_equal(state, entry->name,
                &unit->ast, name)) continue;
        out_entry->name = entry->name;
        out_entry->declaration = entry->declaration;
        out_entry->introduced_by = entry->declaration;
        matches += 1u;
    }
    return matches == 1u ? out_entry : NULL;
}

static CmImportLookupStatus cm_resolve_pending_qualified(
    const CmImportResolver *imports, const CmModuleGraph *graph,
    const CmResolveModuleNode *module, const CmResolveUnit *unit,
    const CmItemMacroPendingInvocation *pending,
    CmResolveMacroScopeEntry *out_entry)
{
    const CmAstItem *item;
    const CmAstPath *path;
    CmResolvePathSegmentView *segments;
    CmResolvedBinding binding;
    CmImportLookupStatus status;
    size_t index;

    memset(out_entry, 0, sizeof(*out_entry));
    item = cm_ast_get_item(&unit->ast, pending->invocation.item);
    if (imports == NULL || graph == NULL || module == NULL || item == NULL
        || item->kind != CM_AST_ITEM_MACRO
        || item->data.macro_item.form != CM_AST_MACRO_INVOCATION) {
        return CM_IMPORT_LOOKUP_INVALID;
    }
    path = cm_ast_get_path(&unit->ast, item->data.macro_item.path);
    if (path == NULL || path->segment_count == 0u)
        return CM_IMPORT_LOOKUP_INVALID;
    segments = (CmResolvePathSegmentView *)cm_alloc_zeroed(
        path->segment_count, sizeof(*segments));
    for (index = 0u; index < path->segment_count; ++index) {
        const CmAstPathSegment *segment;
        const CmInternedString *name;

        segment = &path->segments[index];
        name = cm_ast_get_string(&unit->ast, segment->name);
        if (name == NULL || segment->argument_count != 0u) {
            cm_free(segments);
            return CM_IMPORT_LOOKUP_INVALID;
        }
        segments[index].bytes = name->bytes;
        segments[index].length = name->len;
    }
    memset(&binding, 0, sizeof(binding));
    status = cm_import_resolve_path_checked(imports, graph,
        cm_module_graph_revision(graph), module->info.id, path->absolute,
        segments, path->segment_count, CM_RESOLVE_NAMESPACE_MACRO,
        &binding);
    cm_free(segments);
    /* `$crate::sys::thread_local::local_pointer!` (std) names a
     * `pub(crate) macro`: crate-visible is enough within the crate. */
    if (status != CM_IMPORT_LOOKUP_OK
        || binding.item_kind != CM_AST_ITEM_MACRO
        || binding.dependency != 0u
        || (!binding.is_public && !binding.is_crate_visible)
        || binding.declaration.source == 0u
        || binding.declaration.item == CM_AST_ITEM_NONE) {
        if (getenv("CM_IMPORT_DEBUG") != NULL)
            fprintf(stderr, "IMPORT qualified-macro status=%d kind=%d "
                "public=%d crate=%d source=%u\n", (int)status,
                (int)binding.item_kind, binding.is_public,
                binding.is_crate_visible, (unsigned)binding.declaration.source);
        return status == CM_IMPORT_LOOKUP_OK
            ? CM_IMPORT_LOOKUP_INVALID : status;
    }
    out_entry->name = binding.name;
    out_entry->declaration = binding.declaration;
    out_entry->introduced_by = binding.is_import
        ? binding.import_declaration : binding.declaration;
    return CM_IMPORT_LOOKUP_OK;
}

/* `use crate::sys::thread_local::local_pointer; local_pointer! { .. }`
 * (std): an unqualified invocation whose name a `use` brought in
 * resolves through the import resolver's macro namespace to a
 * local-crate declaration (`pub(crate) macro local_pointer { .. }`). */
static CmImportLookupStatus cm_resolve_pending_unqualified_import(
    const CmImportResolver *imports, const CmModuleGraph *graph,
    const CmResolveModuleNode *module, const CmResolveUnit *unit,
    CmInternId name, CmResolveMacroScopeEntry *out_entry)
{
    const CmInternedString *text;
    CmResolvePathSegmentView segment;
    CmResolvedBinding binding;
    CmImportLookupStatus status;

    memset(out_entry, 0, sizeof(*out_entry));
    text = name == CM_INTERN_ID_NONE ? NULL
        : cm_ast_get_string(&unit->ast, name);
    if (imports == NULL || graph == NULL || module == NULL || text == NULL)
        return CM_IMPORT_LOOKUP_INVALID;
    segment.bytes = text->bytes;
    segment.length = text->len;
    memset(&binding, 0, sizeof(binding));
    status = cm_import_resolve_path_checked(imports, graph,
        cm_module_graph_revision(graph), module->info.id, 0, &segment, 1u,
        CM_RESOLVE_NAMESPACE_MACRO, &binding);
    if (status != CM_IMPORT_LOOKUP_OK
        || binding.item_kind != CM_AST_ITEM_MACRO
        || !binding.is_import
        || binding.dependency != 0u
        || binding.declaration.source == 0u
        || binding.declaration.item == CM_AST_ITEM_NONE) {
        return status == CM_IMPORT_LOOKUP_OK
            ? CM_IMPORT_LOOKUP_INVALID : status;
    }
    out_entry->name = binding.name;
    out_entry->declaration = binding.declaration;
    out_entry->introduced_by = binding.import_declaration;
    return CM_IMPORT_LOOKUP_OK;
}

static const CmResolveMacroDeclarationRecord *
cm_graph_find_macro_declaration_record(const CmModuleGraphState *state,
    CmResolveItemRef declaration)
{
    size_t index;

    for (index = 0u; index < state->macro_declarations.len; ++index) {
        const CmResolveMacroDeclarationRecord *record;

        record = (const CmResolveMacroDeclarationRecord *)cm_vec_at_const(
            &state->macro_declarations, index);
        if (record != NULL && cm_resolve_item_ref_equal(
                record->declaration.declaration, declaration)) return record;
    }
    return NULL;
}

static int cm_exact_declaration_is_builtin_include(
    const CmModuleGraphState *state, const CmResolveUnit *definition_unit,
    CmResolveItemRef declaration)
{
    const CmAstItem *item;
    const CmInternedString *name;
    const CmResolveMacroDeclarationRecord *record;
    size_t attribute_index;
    size_t builtin_count;
    int malformed;

    item = cm_ast_get_item(&definition_unit->ast, declaration.item);
    name = item == NULL ? NULL
        : cm_ast_get_string(&definition_unit->ast, item->name);
    if (item == NULL || name == NULL || name->len != 7u
        || memcmp(name->bytes, "include", 7u) != 0) return 0;
    record = cm_graph_find_macro_declaration_record(state, declaration);
    if (record == NULL || record->declaration.form
            != CM_AST_MACRO_RULES_DEFINITION
        || record->declaration.is_generated) return -1;
    builtin_count = 0u;
    malformed = 0;
    for (attribute_index = 0u;
            attribute_index < record->declaration.attribute_count;
            ++attribute_index) {
        int match;

        match = cm_resolve_attribute_is_bare(state,
            &record->attributes[attribute_index], "rustc_builtin_macro");
        if (match > 0) builtin_count += 1u;
        else if (match < 0) malformed = 1;
    }
    return !malformed && builtin_count == 1u ? 1 : -1;
}

static CmItemMacroResolvedBuiltin cm_exact_declaration_builtin_kind(
    const CmModuleGraphState *state, const CmResolveUnit *definition_unit,
    CmResolveItemRef declaration)
{
    const CmAstItem *item;
    const CmInternedString *name;
    const CmResolveMacroDeclarationRecord *record;
    size_t attribute_index;
    size_t builtin_count;
    int malformed;

    item = cm_ast_get_item(&definition_unit->ast, declaration.item);
    name = item == NULL ? NULL
        : cm_ast_get_string(&definition_unit->ast, item->name);
    if (item == NULL || name == NULL || name->len != 10u
        || memcmp(name->bytes, "cfg_select", 10u) != 0) {
        return CM_ITEM_MACRO_RESOLVED_BUILTIN_NONE;
    }
    record = cm_graph_find_macro_declaration_record(state, declaration);
    if (record == NULL || record->declaration.form
            != CM_AST_MACRO_DECLARATIVE_DEFINITION
        || record->declaration.is_generated) {
        return CM_ITEM_MACRO_RESOLVED_BUILTIN_NONE;
    }
    builtin_count = 0u;
    malformed = 0;
    for (attribute_index = 0u;
            attribute_index < record->declaration.attribute_count;
            ++attribute_index) {
        int match;

        match = cm_resolve_attribute_is_bare(state,
            &record->attributes[attribute_index], "rustc_builtin_macro");
        if (match > 0) builtin_count += 1u;
        else if (match < 0) malformed = 1;
    }
    return !malformed && builtin_count == 1u
        ? CM_ITEM_MACRO_RESOLVED_BUILTIN_CFG_SELECT
        : CM_ITEM_MACRO_RESOLVED_BUILTIN_NONE;
}

static int cm_splice_authenticated_include(CmModuleGraphState *state,
    CmResolveUnit *unit, const CmItemMacroPendingInvocation *pending);
static void cm_include_error(CmModuleGraphState *state,
    CmResolveErrorKind kind, CmSourceId source, const CmAstItem *item,
    const char *detail_a, const char *detail_b);

/* `cfg_if::cfg_if! { .. }` (unwind, std): a qualified invocation whose path
 * leads into a registered dependency resolves through that crate's macro
 * artifact (first segment = extern name, remaining = public path). */
static CmDependencyMacroStatus cm_resolve_pending_qualified_dependency_macro(
    CmModuleGraphState *state, const CmResolveUnit *unit,
    const CmItemMacroPendingInvocation *pending,
    CmDependencyMacroDefinition *out_definition)
{
    const CmAstItem *item;
    const CmAstPath *path;
    CmResolvePathSegmentView segments[16];
    size_t count;
    size_t index;
    size_t selected = 0u;
    CmDependencyMacroStatus status = CM_DEPENDENCY_MACRO_NOT_FOUND;

    memset(out_definition, 0, sizeof(*out_definition));
    item = cm_ast_get_item(&unit->ast, pending->invocation.item);
    if (item == NULL || item->kind != CM_AST_ITEM_MACRO
        || item->data.macro_item.form != CM_AST_MACRO_INVOCATION)
        return CM_DEPENDENCY_MACRO_NOT_FOUND;
    path = cm_ast_get_path(&unit->ast, item->data.macro_item.path);
    if (path == NULL || path->segment_count < 2u
        || path->segment_count > 16u) return CM_DEPENDENCY_MACRO_NOT_FOUND;
    count = path->segment_count;
    for (index = 0u; index < count; ++index) {
        const CmInternedString *name = cm_ast_get_string(&unit->ast,
            path->segments[index].name);
        if (name == NULL || path->segments[index].argument_count != 0u)
            return CM_DEPENDENCY_MACRO_NOT_FOUND;
        segments[index].bytes = name->bytes;
        segments[index].length = name->len;
    }
    for (index = 0u; index < state->options.dependency_macro_count; ++index) {
        CmDependencyMacroDefinition definition;
        CmDependencyMacroStatus one;

        memset(&definition, 0, sizeof(definition));
        one = cm_dependency_macro_artifact_lookup(
            state->options.dependency_macros[index], segments, count,
            &definition);
        if (one == CM_DEPENDENCY_MACRO_OK) {
            *out_definition = definition;
            selected += 1u;
        } else if (one != CM_DEPENDENCY_MACRO_NOT_FOUND
            && status == CM_DEPENDENCY_MACRO_NOT_FOUND) {
            status = one;
        }
    }
    if (selected > 1u) return CM_DEPENDENCY_MACRO_AMBIGUOUS;
    if (selected == 1u) return CM_DEPENDENCY_MACRO_OK;
    return status;
}

/* core's `#[rustc_builtin_macro] macro_rules! include { .. => {{ }} }`
 * reached through a dependency (std's `include!("keyword_docs.rs")`):
 * the compiler-provided include, not a transcription of `{}`. */
static int cm_dependency_definition_is_builtin_include(
    const CmDependencyMacroDefinition *definition)
{
    const CmAstItem *item;
    const CmInternedString *name;
    uint32_t index;
    size_t builtin_count = 0u;

    if (definition == NULL || definition->definition_ast == NULL
        || definition->declaration.item == CM_AST_ITEM_NONE) return 0;
    item = cm_ast_get_item(definition->definition_ast,
        definition->declaration.item);
    name = item == NULL ? NULL
        : cm_ast_get_string(definition->definition_ast, item->name);
    if (item == NULL || item->kind != CM_AST_ITEM_MACRO || name == NULL
        || name->len != 7u || memcmp(name->bytes, "include", 7u) != 0)
        return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmAstAttribute *attribute = cm_ast_get_attribute(
            definition->definition_ast, item->attributes[index]);
        const CmInternedString *text = attribute == NULL ? NULL
            : cm_ast_get_string(definition->definition_ast,
                attribute->text);
        /* Attribute text keeps its `#[..]` wrapper. */
        if (text != NULL && text->len == 22u
            && memcmp(text->bytes, "#[rustc_builtin_macro]", 22u) == 0)
            builtin_count += 1u;
    }
    return builtin_count == 1u;
}

static CmDependencyMacroStatus cm_resolve_pending_dependency_macro(
    CmModuleGraphState *state, const CmModuleGraph *graph_view,
    CmModuleId module, const CmResolvePathSegmentView *local_name,
    CmDependencyMacroImport *out_import)
{
    CmDependencyMacroStatus selected_status;
    size_t selected_count;
    size_t index;

    memset(out_import, 0, sizeof(*out_import));
    selected_status = CM_DEPENDENCY_MACRO_NOT_FOUND;
    selected_count = 0u;
    for (index = 0u; index < state->options.dependency_macro_count;
            ++index) {
        CmDependencyMacroImport imported;
        CmDependencyMacroStatus status;

        memset(&imported, 0, sizeof(imported));
        status = cm_dependency_macro_artifact_resolve_import(
            state->options.dependency_macros[index], graph_view,
            state->revision, module, local_name, &imported);
        if (status == CM_DEPENDENCY_MACRO_OK) {
            *out_import = imported;
            selected_count += 1u;
        } else if (status == CM_DEPENDENCY_MACRO_AMBIGUOUS) {
            selected_status = CM_DEPENDENCY_MACRO_AMBIGUOUS;
        } else if (status != CM_DEPENDENCY_MACRO_NOT_FOUND
            && selected_status == CM_DEPENDENCY_MACRO_NOT_FOUND) {
            selected_status = status;
        }
    }
    if (selected_count > 1u
        || selected_status == CM_DEPENDENCY_MACRO_AMBIGUOUS) {
        memset(out_import, 0, sizeof(*out_import));
        return CM_DEPENDENCY_MACRO_AMBIGUOUS;
    }
    if (selected_count == 1u) return CM_DEPENDENCY_MACRO_OK;
    return selected_status;
}

static int cm_graph_other_units_have_pending(
    const CmModuleGraphState *state, size_t unit_index)
{
    size_t index;

    for (index = 0u; index < state->units.len; ++index) {
        const CmResolveUnit *other;

        if (index == unit_index) continue;
        other = (const CmResolveUnit *)cm_vec_at_const(&state->units, index);
        if (other != NULL && other->plan.pending_invocation_count != 0u)
            return 1;
    }
    return 0;
}

static int cm_generated_paths_equal(
    const CmItemMacroResolvedGeneratedPath *a,
    const CmItemMacroResolvedGeneratedPath *b)
{
    size_t index;

    if (a->segment_count != b->segment_count) return 0;
    for (index = 0u; index < a->segment_count; ++index) {
        if (a->segments[index].length != b->segments[index].length
            || memcmp(a->segments[index].bytes, b->segments[index].bytes,
                a->segments[index].length) != 0) return 0;
    }
    return 1;
}

/* Fold this round's resolutions into the unit's accumulated set. A
 * merged path entry's segment array moves to the unit (the round entry
 * is emptied); a duplicate's array is freed here. */
static void cm_unit_merge_sticky(CmResolveUnit *unit, const CmVec *resolved,
    CmVec *resolved_paths)
{
    size_t index;
    size_t existing;

    for (index = 0u; index < resolved->len; ++index) {
        const CmItemMacroResolvedInvocation *value =
            (const CmItemMacroResolvedInvocation *)cm_vec_at_const(
                resolved, index);
        int duplicate = 0;
        for (existing = 0u; value != NULL
                && existing < unit->sticky_resolved.len; ++existing) {
            CmItemMacroResolvedInvocation *have =
                (CmItemMacroResolvedInvocation *)cm_vec_at(
                    &unit->sticky_resolved, existing);
            if (have != NULL && have->invocation.owner
                    == value->invocation.owner
                && have->invocation.item == value->invocation.item) {
                *have = *value;
                duplicate = 1;
                break;
            }
        }
        if (value != NULL && !duplicate)
            (void)cm_vec_push(&unit->sticky_resolved, value);
    }
    for (index = 0u; index < resolved_paths->len; ++index) {
        CmItemMacroResolvedGeneratedPath *entry =
            (CmItemMacroResolvedGeneratedPath *)cm_vec_at(resolved_paths,
                index);
        int duplicate = 0;
        for (existing = 0u; entry != NULL
                && existing < unit->sticky_paths.len; ++existing) {
            CmItemMacroResolvedGeneratedPath *have =
                (CmItemMacroResolvedGeneratedPath *)cm_vec_at(
                    &unit->sticky_paths, existing);
            if (have != NULL && cm_generated_paths_equal(have, entry)) {
                have->definition = entry->definition;
                have->definition_ast = entry->definition_ast;
                have->crate_identifier = entry->crate_identifier;
                duplicate = 1;
                break;
            }
        }
        if (entry == NULL) continue;
        if (duplicate) {
            cm_free((void *)entry->segments);
        } else {
            (void)cm_vec_push(&unit->sticky_paths, entry);
        }
        entry->segments = NULL;
        entry->segment_count = 0u;
    }
}

/* Sticky entries borrow unit ASTs, and `state->units` relocates when a
 * round appends generated units: re-derive every local definition's AST
 * pointer from its owning source before a replan. Returns 0 when an
 * owner vanished. */
static int cm_unit_refresh_sticky_asts(CmModuleGraphState *state,
    CmResolveUnit *unit)
{
    size_t index;

    for (index = 0u; index < unit->sticky_resolved.len; ++index) {
        CmItemMacroResolvedInvocation *entry =
            (CmItemMacroResolvedInvocation *)cm_vec_at(
                &unit->sticky_resolved, index);
        const CmResolveUnit *owner;
        if (entry == NULL
            || entry->definition.owner > (CmItemMacroAstOwner)UINT32_MAX)
            continue;
        owner = cm_get_unit_const(state, cm_find_unit(state,
            (CmSourceId)entry->definition.owner));
        if (owner == NULL) return 0;
        entry->definition_ast = &owner->ast;
    }
    for (index = 0u; index < unit->sticky_paths.len; ++index) {
        CmItemMacroResolvedGeneratedPath *entry =
            (CmItemMacroResolvedGeneratedPath *)cm_vec_at(
                &unit->sticky_paths, index);
        const CmResolveUnit *owner;
        if (entry == NULL
            || entry->definition.owner > (CmItemMacroAstOwner)UINT32_MAX)
            continue;
        owner = cm_get_unit_const(state, cm_find_unit(state,
            (CmSourceId)entry->definition.owner));
        if (owner == NULL) return 0;
        entry->definition_ast = &owner->ast;
    }
    return 1;
}

/* Path-keyed resolution for a generated qualified invocation: the
 * planner applies it to every generated invocation spelling that path,
 * across replans. Segment bytes borrow the unit AST. */
static void cm_graph_record_generated_path(const CmResolveUnit *unit,
    const CmItemMacroPendingInvocation *pending,
    const CmItemMacroResolvedInvocation *value, CmVec *resolved_paths)
{
    const CmAstItem *item;
    const CmAstPath *path;
    CmItemMacroPathSegment *segments;
    CmItemMacroResolvedGeneratedPath entry;
    size_t index;
    size_t existing;

    item = cm_ast_get_item(&unit->ast, pending->invocation.item);
    if (item == NULL || item->kind != CM_AST_ITEM_MACRO) return;
    path = cm_ast_get_path(&unit->ast, item->data.macro_item.path);
    if (path == NULL || path->absolute || path->segment_count < 2u) return;
    segments = (CmItemMacroPathSegment *)cm_alloc_zeroed(
        path->segment_count, sizeof(*segments));
    for (index = 0u; index < path->segment_count; ++index) {
        const CmInternedString *name = cm_ast_get_string(&unit->ast,
            path->segments[index].name);
        if (name == NULL || path->segments[index].argument_count != 0u) {
            cm_free(segments);
            return;
        }
        segments[index].bytes = name->bytes;
        segments[index].length = name->len;
    }
    for (existing = 0u; existing < resolved_paths->len; ++existing) {
        const CmItemMacroResolvedGeneratedPath *other =
            (const CmItemMacroResolvedGeneratedPath *)cm_vec_at_const(
                resolved_paths, existing);
        int same = other != NULL
            && other->segment_count == path->segment_count;
        for (index = 0u; same && index < path->segment_count; ++index) {
            same = other->segments[index].length == segments[index].length
                && memcmp(other->segments[index].bytes,
                    segments[index].bytes, segments[index].length) == 0;
        }
        if (same) {
            cm_free(segments);
            return;
        }
    }
    memset(&entry, 0, sizeof(entry));
    entry.segments = segments;
    entry.segment_count = path->segment_count;
    entry.definition = value->definition;
    entry.definition_ast = value->definition_ast;
    entry.crate_identifier = NULL;
    (void)cm_vec_push(resolved_paths, &entry);
    if (getenv("CM_MACRO_DEBUG") != NULL)
        fprintf(stderr, "MACRO generated-path segs=%lu def=%lu:%u\n",
            (unsigned long)entry.segment_count,
            (unsigned long)entry.definition.owner,
            (unsigned)entry.definition.item);
}

static int cm_replan_staged_macro_invocations(CmModuleGraphState *state,
    int *out_spliced_include)
{
    size_t unit_index;
    int progress = 0;
    size_t pending_before;

    /* Rotate the deferred-name signature: the current round's names are
     * compared with the previous round's at the end. */
    {
        CmVec swap = state->previous_deferred_names;
        state->previous_deferred_names = state->deferred_names;
        state->deferred_names = swap;
        state->deferred_names.len = 0u;
    }
    int deferred_any = 0;
    int deferred_saved = 0;
    CmSourceId deferred_source = 0u;
    uint32_t deferred_start = 0u;
    uint32_t deferred_end = 0u;
    CmResolveStringId deferred_path = CM_RESOLVE_STRING_NONE;
    CmResolveStringId deferred_kind = CM_RESOLVE_STRING_NONE;
    CmResolveStringId deferred_detail = CM_RESOLVE_STRING_NONE;

    if (out_spliced_include != NULL) *out_spliced_include = 0;
    if (out_spliced_include == NULL) return 0;
    for (unit_index = 0u; unit_index < state->units.len; ++unit_index) {
        CmResolveUnit *unit;
        CmVec resolved;
        /* Generated qualified invocations (`$crate::thread::local_impl::
         * thread_local_inner!` from std's thread_local!) are regenerated
         * by every replan, so their resolutions are keyed by path. */
        CmVec resolved_paths;
        CmVec authenticated_includes;
        CmImportResolver imports;
        CmModuleGraph graph_view;
        CmImportResult import_result;
        size_t pending_index;
        int imports_initialized;
        int imports_ready;
        int ok;

        unit = (CmResolveUnit *)cm_vec_at(&state->units, unit_index);
        if (unit == NULL || !unit->plan_ok
            || unit->plan.pending_invocation_count == 0u) continue;
            /* A unit whose module is still deferred (the root has a generated
         * pending invocation — libc's `prelude!()` — so non-macro_use
         * modules are not built yet) has no scope to resolve against:
         * its generated invocations wait for the round that builds it. */
        if (cm_pending_owner_module(state,
                (CmResolveUnitId)(unit_index + 1u), CM_AST_ITEM_NONE)
                == NULL) continue;
        cm_vec_init(&resolved, sizeof(CmItemMacroResolvedInvocation));
        cm_vec_init(&resolved_paths,
            sizeof(CmItemMacroResolvedGeneratedPath));
        cm_vec_init(&authenticated_includes,
            sizeof(CmItemMacroPendingInvocation));
        memset(&imports, 0, sizeof(imports));
        memset(&graph_view, 0, sizeof(graph_view));
        graph_view.state = state;
        memset(&import_result, 0, sizeof(import_result));
        imports_initialized = 0;
        imports_ready = 0;
        for (pending_index = 0u;
                pending_index < unit->plan.pending_invocation_count;
                ++pending_index) {
            if (unit->plan.pending_invocations[pending_index].is_qualified) {
                cm_import_resolver_init(&imports);
                imports_initialized = 1;
                import_result = cm_import_resolve(&imports, &graph_view,
                    state->revision);
                imports_ready = import_result.revision == state->revision;
                break;
            }
        }
        ok = 1;
        for (pending_index = 0u;
                pending_index < unit->plan.pending_invocation_count;
                ++pending_index) {
            const CmItemMacroPendingInvocation *pending;
            const CmResolveModuleNode *module;
            const CmResolveMacroScopeEntry *entry;
            CmResolveMacroScopeEntry namespace_entry;
            CmResolveMacroScopeEntry qualified_entry;
            CmResolveUnitId definition_unit_id;
            const CmResolveUnit *definition_unit;
            CmItemMacroResolvedInvocation value;
            CmDependencyMacroDefinition dependency_definition;
            CmDependencyMacroImport dependency_import;
            CmDependencyMacroStatus dependency_status;
            CmInternId name;
            CmSourceId invocation_source;
            CmAstSpan source_anchor;
            int has_name;
            int builtin_include;
            int has_dependency_definition;

            pending = &unit->plan.pending_invocations[pending_index];
            invocation_source = cm_unit_item_source(unit,
                pending->invocation.item);
            if (invocation_source == 0u) invocation_source = unit->source;
            if (pending->is_generated
                && (!cm_graph_source_invocation_span(state,
                        pending->source_invocation, &invocation_source,
                        &source_anchor)
                    || source_anchor.start != pending->span.start
                    || source_anchor.end != pending->span.end)) {
                cm_graph_add_error(state,
                    CM_RESOLVE_ERROR_ITEM_MACRO, invocation_source,
                    pending->span.start, pending->span.end,
                    CM_RESOLVE_STRING_NONE,
                    cm_graph_intern_c_str(state,
                        "invalid generated macro anchor"),
                    cm_graph_intern_c_str(state,
                        "generated pending invocation lost its source "
                        "identity"),
                    0u, 0u);
                ok = 0;
                continue;
            }
            name = CM_INTERN_ID_NONE;
            memset(&dependency_definition, 0,
                sizeof(dependency_definition));
            memset(&dependency_import, 0, sizeof(dependency_import));
            dependency_status = CM_DEPENDENCY_MACRO_NOT_FOUND;
            has_dependency_definition = 0;
            module = cm_pending_owner_module(state,
                (CmResolveUnitId)(unit_index + 1u),
                pending->container_item);
            has_name = cm_pending_invocation_name(unit, pending, &name);
            entry = NULL;
                    if (pending->is_qualified) {
                /* Generated invocations too: `thread_local!` expands to
                 * `$crate::thread::local_impl::thread_local_inner!`. */
                if (imports_ready
                    && cm_resolve_pending_qualified(&imports,
                        &graph_view, module, unit, pending,
                        &qualified_entry) == CM_IMPORT_LOOKUP_OK) {
                    entry = &qualified_entry;
                }
            } else if (module != NULL && has_name) {
                entry = cm_resolve_pending_from_scope(state, module, unit,
                    pending, name);
            }
            if (entry == NULL && module != NULL && !pending->is_qualified
                && name != CM_INTERN_ID_NONE) {
                entry = cm_resolve_pending_from_namespace(state, module,
                    unit, name, &namespace_entry);
            }
            if (entry == NULL && module != NULL && !pending->is_qualified
                && !pending->is_generated && has_name
                && name != CM_INTERN_ID_NONE) {
                /* A `use`-imported macro: consult the import resolver
                 * (initialized on demand). */
                if (!imports_initialized) {
                    cm_import_resolver_init(&imports);
                    imports_initialized = 1;
                    import_result = cm_import_resolve(&imports, &graph_view,
                        state->revision);
                    imports_ready = import_result.revision
                        == state->revision;
                }
                if (imports_ready
                    && cm_resolve_pending_unqualified_import(&imports,
                        &graph_view, module, unit, name, &qualified_entry)
                        == CM_IMPORT_LOOKUP_OK) {
                    entry = &qualified_entry;
                }
            }
            if (entry == NULL && module != NULL && has_name
                && !pending->is_qualified && !pending->is_generated
                && state->options.dependency_macro_count != 0u) {
                const CmInternedString *local_name_string;
                CmResolvePathSegmentView local_name;

                local_name_string = cm_ast_get_string(&unit->ast, name);
                if (local_name_string == NULL) {
                    dependency_status =
                        CM_DEPENDENCY_MACRO_INVALID_ARGUMENT;
                } else {
                    local_name.bytes = local_name_string->bytes;
                    local_name.length = local_name_string->len;
                    dependency_status = cm_resolve_pending_dependency_macro(
                        state, &graph_view, module->info.id, &local_name,
                        &dependency_import);
                    has_dependency_definition = dependency_status
                        == CM_DEPENDENCY_MACRO_OK;
                    if (has_dependency_definition) {
                        dependency_definition = dependency_import.definition;
                    }
                }
            }
            definition_unit_id = entry == NULL ? 0u : cm_find_unit(state,
                entry->declaration.source);
            definition_unit = cm_get_unit_const(state, definition_unit_id);
                    if (!has_dependency_definition && pending->is_qualified
                && (entry == NULL || definition_unit == NULL)
                && state->options.dependency_macro_count != 0u) {
                CmDependencyMacroDefinition qualified_definition;
                CmDependencyMacroStatus qualified_status;

                qualified_status =
                    cm_resolve_pending_qualified_dependency_macro(state,
                        unit, pending, &qualified_definition);
                if (qualified_status == CM_DEPENDENCY_MACRO_OK) {
                    has_dependency_definition = 1;
                    dependency_definition = qualified_definition;
                    /* No `use` leaf brought the name in: the invocation
                     * itself is the retained consumer import. */
                    dependency_import.consumer_graph = &graph_view;
                    dependency_import.consumer_revision = state->revision;
                    dependency_import.consumer_module = module == NULL
                        ? CM_MODULE_NONE : module->info.id;
                    dependency_import.import_declaration.source =
                        invocation_source;
                    dependency_import.import_declaration.item =
                        pending->invocation.item;
                    dependency_import.definition = qualified_definition;
                    entry = NULL;
                } else if (qualified_status
                        != CM_DEPENDENCY_MACRO_NOT_FOUND) {
                    dependency_status = qualified_status;
                }
            }
            if (!has_dependency_definition && !pending->is_qualified
                && has_name && name != CM_INTERN_ID_NONE
                && (entry == NULL || definition_unit == NULL)
                && state->options.dependency_macro_count != 0u) {
                /* `vec!` in std after `#[macro_use] extern crate alloc`:
                 * `alloc::vec` through the artifacts. */
                const CmInternedString *macro_name = cm_ast_get_string(
                    &unit->ast, name);
                size_t crate_index;
                size_t selected = 0u;
                CmDependencyMacroDefinition found;
                /* rustc injects `#[macro_use] extern crate core;` (and
                 * `std`) implicitly: their exported macros (`include!`,
                 * `panic!`) are always in scope. */
                static const char *const implicit_crates[] = { "core",
                    "std" };
                size_t total = state->macro_use_crates.len
                    + sizeof(implicit_crates) / sizeof(implicit_crates[0]);

                memset(&found, 0, sizeof(found));
                for (crate_index = 0u; macro_name != NULL
                        && crate_index < total; ++crate_index) {
                    const char *crate_name = crate_index
                            < state->macro_use_crates.len
                        ? (const char *)cm_vec_at_const(
                            &state->macro_use_crates, crate_index)
                        : implicit_crates[crate_index
                            - state->macro_use_crates.len];
                    CmResolvePathSegmentView segments[2];
                    size_t artifact_index;

                    if (crate_name == NULL) continue;
                    segments[0].bytes = (const unsigned char *)crate_name;
                    segments[0].length = strlen(crate_name);
                    segments[1].bytes = macro_name->bytes;
                    segments[1].length = macro_name->len;
                    for (artifact_index = 0u; artifact_index
                            < state->options.dependency_macro_count;
                            ++artifact_index) {
                        CmDependencyMacroDefinition candidate;
                        memset(&candidate, 0, sizeof(candidate));
                        if (cm_dependency_macro_artifact_lookup(
                                state->options.dependency_macros[
                                    artifact_index], segments, 2u,
                                &candidate) == CM_DEPENDENCY_MACRO_OK) {
                            found = candidate;
                            selected += 1u;
                        }
                    }
                }
                if (selected == 1u) {
                    has_dependency_definition = 1;
                    dependency_definition = found;
                    dependency_import.consumer_graph = &graph_view;
                    dependency_import.consumer_revision = state->revision;
                    dependency_import.consumer_module = module == NULL
                        ? CM_MODULE_NONE : module->info.id;
                    dependency_import.import_declaration.source =
                        invocation_source;
                    dependency_import.import_declaration.item =
                        pending->invocation.item;
                    dependency_import.definition = found;
                    entry = NULL;
                } else if (selected > 1u) {
                    dependency_status = CM_DEPENDENCY_MACRO_AMBIGUOUS;
                }
            }
            if (!has_dependency_definition
                && (entry == NULL || definition_unit == NULL)
                && (state->defer_non_macro_use_modules
                    || cm_graph_other_units_have_pending(state, unit_index)
                    || progress)) {
                /* `progress`: an earlier unit this round expanded (std's
                 * sys unit expands its cfg_if! whole; the generated
                 * `mod native { pub macro local_pointer }` is only built
                 * after the round), so the name may exist next round. */
                /* The name may come from an expansion or a module the
                 * next round builds (std's `use crate::sys::thread_local::
                 * local_pointer` reaches a cfg_if!-generated re-export):
                 * leave the invocation pending; error only when a whole
                 * round makes no progress. */
                if (!deferred_saved) {
                    deferred_saved = 1;
                    deferred_source = invocation_source;
                    deferred_start = pending->span.start;
                    deferred_end = pending->span.end;
                    deferred_path = module == NULL ? CM_RESOLVE_STRING_NONE
                        : module->info.absolute_path;
                    deferred_kind = cm_graph_intern_c_str(state,
                        pending->is_qualified ? "qualified macro"
                            : "unsupported macro");
                    deferred_detail = dependency_status
                            != CM_DEPENDENCY_MACRO_NOT_FOUND
                        ? cm_graph_intern_c_str(state,
                            cm_dependency_macro_status_name(
                                dependency_status))
                        : cm_graph_intern_c_str(state,
                            cm_graph_missing_macro_detail(state, unit,
                                name, pending->is_qualified));
                }
                if (getenv("CM_MACRO_DEBUG") != NULL) {
                    const CmInternedString *dn = name == CM_INTERN_ID_NONE
                        ? NULL : cm_ast_get_string(&unit->ast, name);
                    fprintf(stderr, "MACRO deferred name=%.*s unit=%lu "
                        "generated=%d defer_modules=%d others=%d "
                        "entry=%p defunit=%p\n",
                        dn == NULL ? 1 : (int)dn->len,
                        dn == NULL ? "?" : (const char *)dn->bytes,
                        (unsigned long)unit_index, pending->is_generated,
                        state->defer_non_macro_use_modules,
                        cm_graph_other_units_have_pending(state, unit_index),
                        (const void *)entry, (const void *)definition_unit);
                }
                {
                    const CmInternedString *dn = name == CM_INTERN_ID_NONE
                        ? NULL : cm_ast_get_string(&unit->ast, name);
                    size_t byte;
                    for (byte = 0u; dn != NULL && byte < dn->len; ++byte)
                        (void)cm_vec_push(&state->deferred_names,
                            &dn->bytes[byte]);
                    (void)cm_vec_push(&state->deferred_names, "\n");
                }
                deferred_any = 1;
                continue;
            }
            if (!has_dependency_definition
                && (entry == NULL || definition_unit == NULL)) {
                if (getenv("CM_MACRO_DEBUG") != NULL) {
                    size_t h;
                    const CmInternedString *pname = name == CM_INTERN_ID_NONE
                        ? NULL : cm_ast_get_string(&unit->ast, name);
                    fprintf(stderr, "MACRO missing binding name=%.*s "
                        "module=%lu unit_source=%lu generated=%d "
                        "span=%lu..%lu history=%lu entry=%p defunit=%p\n",
                        pname == NULL ? 1 : (int)pname->len,
                        pname == NULL ? "?" : (const char *)pname->bytes,
                        module == NULL ? 0ul : (unsigned long)module->info.id,
                        (unsigned long)unit->source, pending->is_generated,
                        (unsigned long)pending->span.start,
                        (unsigned long)pending->span.end,
                        module == NULL ? 0ul
                            : (unsigned long)module->macro_scope_history_count,
                        (const void *)entry, (const void *)definition_unit);
                    for (h = 0u; module != NULL
                            && h < module->macro_scope_history_count; ++h) {
                        const CmResolveMacroScopeEntry *he =
                            &module->macro_scope_history[h];
                        const CmInternedString *hn = cm_graph_string(state,
                            he->name);
                        fprintf(stderr, "  history[%lu] name=%.*s source=%lu "
                            "span=%lu..%lu decl_source=%lu item=%lu\n",
                            (unsigned long)h, hn == NULL ? 1 : (int)hn->len,
                            hn == NULL ? "?" : (const char *)hn->bytes,
                            (unsigned long)he->introduction_span.source,
                            (unsigned long)he->introduction_span.start,
                            (unsigned long)he->introduction_span.end,
                            (unsigned long)he->declaration.source,
                            (unsigned long)he->declaration.item);
                    }
                }
                cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                    invocation_source, pending->span.start,
                    pending->span.end,
                    module == NULL ? CM_RESOLVE_STRING_NONE
                        : module->info.absolute_path,
                    cm_graph_intern_c_str(state, pending->is_qualified
                        ? "qualified macro" : "unsupported macro"),
                    dependency_status != CM_DEPENDENCY_MACRO_NOT_FOUND
                        ? cm_graph_intern_c_str(state,
                            cm_dependency_macro_status_name(
                                dependency_status))
                        : cm_graph_intern_c_str(state,
                            cm_graph_missing_macro_detail(state, unit,
                                name, pending->is_qualified)),
                    0u, 0u);
                ok = 0;
                continue;
            }
            if (has_dependency_definition
                && cm_dependency_definition_is_builtin_include(
                    &dependency_definition)) {
                if (pending->is_generated || state->options.include_expansion
                        != CM_INCLUDE_EXPANSION_AUTHENTICATED) {
                    cm_include_error(state,
                        CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
                        invocation_source,
                        cm_ast_get_item(&unit->ast,
                            pending->invocation.item),
                        pending->is_generated
                            ? "generated authenticated include is unsupported"
                            : "authenticated include expansion is disabled",
                        NULL);
                    ok = 0;
                    continue;
                }
                (void)cm_vec_push(&authenticated_includes, pending);
                continue;
            }
            if (has_dependency_definition) {
                CmResolveItemRef invocation;

                memset(&value, 0, sizeof(value));
                value.invocation = pending->invocation;
                if (!cm_register_external_macro(state,
                        &dependency_definition, &value.definition.owner)) {
                    cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                        invocation_source, pending->span.start,
                        pending->span.end,
                        module == NULL ? CM_RESOLVE_STRING_NONE
                            : module->info.absolute_path,
                        cm_graph_intern_c_str(state,
                            "dependency macro"),
                        cm_graph_intern_c_str(state,
                            "could not register exact dependency owner"),
                        0u, 0u);
                    ok = 0;
                    continue;
                }
                invocation.source = pending->invocation.owner
                        <= (CmItemMacroAstOwner)UINT32_MAX
                    ? (CmSourceId)pending->invocation.owner : 0u;
                invocation.item = pending->invocation.item;
                if (!cm_register_external_macro_import(state,
                        value.definition.owner,
                        dependency_definition.declaration.item,
                        dependency_import.consumer_module,
                        dependency_import.import_declaration, invocation)) {
                    cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                        invocation_source, pending->span.start,
                        pending->span.end,
                        module == NULL ? CM_RESOLVE_STRING_NONE
                            : module->info.absolute_path,
                        cm_graph_intern_c_str(state,
                            "dependency macro import"),
                        cm_graph_intern_c_str(state,
                            "could not retain exact consumer import"),
                        0u, 0u);
                    ok = 0;
                    continue;
                }
                value.definition.item =
                    dependency_definition.declaration.item;
                value.definition_ast =
                    dependency_definition.definition_ast;
                value.crate_identifier =
                    dependency_definition.crate_identifier;
                (void)cm_vec_push(&resolved, &value);
                continue;
            }
            builtin_include = cm_exact_declaration_is_builtin_include(state,
                definition_unit, entry->declaration);
            if (builtin_include < 0) {
                cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
                    invocation_source,
                    cm_ast_get_item(&unit->ast, pending->invocation.item),
                    "resolved include declaration is not authenticated",
                    "expected one bare rustc_builtin_macro attribute");
                ok = 0;
                continue;
            }
            if (builtin_include > 0) {
                if (pending->is_generated) {
                    cm_include_error(state,
                        CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
                        invocation_source,
                        cm_ast_get_item(&unit->ast,
                            pending->invocation.item),
                        "generated authenticated include is unsupported",
                        NULL);
                    ok = 0;
                    continue;
                }
                if (state->options.include_expansion
                        != CM_INCLUDE_EXPANSION_AUTHENTICATED) {
                    cm_include_error(state,
                        CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
                        invocation_source,
                        cm_ast_get_item(&unit->ast,
                            pending->invocation.item),
                        "authenticated include expansion is disabled", NULL);
                    ok = 0;
                    continue;
                }
                (void)cm_vec_push(&authenticated_includes, pending);
                continue;
            }
            if (pending->is_generated && pending->is_qualified) {
                /* A scope seed cannot serve a qualified path: certify the
                 * path itself for the replan. */
                memset(&value, 0, sizeof(value));
                value.invocation = pending->invocation;
                value.definition.owner =
                    (CmItemMacroAstOwner)entry->declaration.source;
                value.definition.item = entry->declaration.item;
                value.definition_ast = &definition_unit->ast;
                cm_graph_record_generated_path(unit, pending, &value,
                    &resolved_paths);
                continue;
            }
                    if (pending->is_generated) {
                /* The seed scopes the whole unit, so an inline-module
                 * container (a fixture's `mod user { cfg_if! { ..
                 * thread_local! .. } }`) is served the same way. */
                if (module == NULL
                    || !cm_unit_add_initial_scope(unit,
                        (CmItemMacroItemRef) {
                            (CmItemMacroAstOwner)
                                entry->declaration.source,
                            entry->declaration.item
                        })) {
                    cm_graph_add_error(state,
                        CM_RESOLVE_ERROR_ITEM_MACRO,
                        invocation_source, pending->span.start,
                        pending->span.end,
                        module == NULL ? CM_RESOLVE_STRING_NONE
                            : module->info.absolute_path,
                        cm_graph_intern_c_str(state,
                            "generated macro scope"),
                        cm_graph_intern_c_str(state,
                            "generated external macros are supported only "
                            "at a source unit root"),
                        0u, 0u);
                    ok = 0;
                }
                continue;
            }
            memset(&value, 0, sizeof(value));
            value.invocation = pending->invocation;
            value.definition.owner =
                (CmItemMacroAstOwner)entry->declaration.source;
            value.definition.item = entry->declaration.item;
            value.definition_ast = &definition_unit->ast;
            value.builtin = cm_exact_declaration_builtin_kind(state,
                definition_unit, entry->declaration);
            (void)cm_vec_push(&resolved, &value);
                    if (pending->is_generated && pending->is_qualified)
                cm_graph_record_generated_path(unit, pending, &value,
                    &resolved_paths);
        }
        if (imports_initialized) cm_import_resolver_destroy(&imports);
        pending_before = unit->plan.pending_invocation_count;
        if (authenticated_includes.len != 0u) progress = 1;
        if (ok && authenticated_includes.len != 0u) {
            size_t include_index;

            for (include_index = 0u;
                    include_index < authenticated_includes.len;
                    ++include_index) {
                const CmItemMacroPendingInvocation *include;

                include = (const CmItemMacroPendingInvocation *)cm_vec_at_const(
                    &authenticated_includes, include_index);
                if (include == NULL || !cm_splice_authenticated_include(
                        state, unit, include)) {
                    ok = 0;
                    break;
                }
            }
            if (ok) *out_spliced_include = 1;
        } else if (ok) {
            const CmResolveUnit *replanned;

            cm_unit_merge_sticky(unit, &resolved, &resolved_paths);
            if (!cm_unit_refresh_sticky_asts(state, unit)) ok = 0;
            if (ok) ok = cm_replan_unit_with_resolved(state,
                (CmResolveUnitId)(unit_index + 1u),
                (const CmItemMacroResolvedInvocation *)
                    unit->sticky_resolved.data,
                unit->sticky_resolved.len,
                (const CmItemMacroResolvedGeneratedPath *)
                    unit->sticky_paths.data,
                unit->sticky_paths.len);
            /* A unit is re-planned from scratch: only a shrinking pending
             * set (or new units) is progress, not re-resolving the same
             * invocations while a deferred one waits. */
            replanned = cm_get_unit_const(state,
                (CmResolveUnitId)(unit_index + 1u));
            /* New units alone are not progress: a unit holding a
             * deferred invocation re-expands its other invocations every
             * round, regenerating units (std's thread_local plumbing
             * looped to the round limit that way). */
            if (ok && replanned != NULL
                && replanned->plan.pending_invocation_count < pending_before)
                progress = 1;
        }
        cm_vec_destroy(&authenticated_includes);
        cm_vec_destroy(&resolved);
        {
            /* Segment arrays not merged into the unit were freed by the
             * merge; merged ones live with the unit. */
            size_t path_index;
            for (path_index = 0u; path_index < resolved_paths.len;
                 ++path_index) {
                CmItemMacroResolvedGeneratedPath *entry =
                    (CmItemMacroResolvedGeneratedPath *)cm_vec_at(
                        &resolved_paths, path_index);
                if (entry != NULL && entry->segments != NULL)
                    cm_free((void *)entry->segments);
            }
            cm_vec_destroy(&resolved_paths);
        }
        if (!ok) return 0;
        if (*out_spliced_include) return 1;
    }
    if (deferred_any && !progress) {
        /* A changed set of deferred names (one resolved while another
         * appeared from its expansion) is still progress. */
        int same = state->deferred_names.len
                == state->previous_deferred_names.len
            && (state->deferred_names.len == 0u
                || memcmp(state->deferred_names.data,
                    state->previous_deferred_names.data,
                    state->deferred_names.len) == 0);
        if (same) {
            cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                deferred_source, deferred_start, deferred_end, deferred_path,
                deferred_kind, deferred_detail, 0u, 0u);
            return 0;
        }
    }
    return 1;
}

static int cm_graph_has_pending_invocations(
    const CmModuleGraphState *state)
{
    size_t index;

    for (index = 0u; index < state->units.len; ++index) {
        const CmResolveUnit *unit;

        unit = (const CmResolveUnit *)cm_vec_at_const(&state->units, index);
        if (unit != NULL && unit->plan.pending_invocation_count != 0u)
            return 1;
    }
    return 0;
}

static void cm_graph_reset_derived_resolution(CmModuleGraphState *state)
{
    size_t index;

    /* Unit ASTs and plans own their storage and survive this transaction. */
    for (index = 0u; index < state->external_macros.len; ++index) {
        CmResolveExternalMacro *external;

        external = (CmResolveExternalMacro *)cm_vec_at(
            &state->external_macros, index);
        if (external != NULL) external->published = 0;
    }
    cm_vec_clear(&state->errors);
    cm_vec_clear(&state->macro_declarations);
    cm_vec_clear(&state->modules);
    cm_interner_destroy(&state->strings);
    cm_interner_init(&state->strings, 4096u);
    cm_arena_destroy(&state->storage);
    cm_arena_init(&state->storage, 4096u);
}

static void cm_graph_reset_unit_plans(CmModuleGraphState *state)
{
    size_t index;

    for (index = 0u; index < state->units.len; ++index) {
        CmResolveUnit *unit;

        unit = (CmResolveUnit *)cm_vec_at(&state->units, index);
        if (unit == NULL) continue;
        cm_item_macro_plan_destroy(&unit->plan);
        cm_item_macro_plan_init(&unit->plan);
        cm_free(unit->initial_scope);
        unit->initial_scope = NULL;
        unit->initial_scope_count = 0u;
        cm_unit_clear_sticky(unit);
        unit->plan_prepared = 0;
        unit->plan_ok = 0;
        unit->active = 0;
    }
}

static void cm_record_extern_block(CmModuleGraphState *state,
    CmSourceId source, const CmAst *ast,
    const CmItemMacroPlanNode *block, CmVec *type_entries,
    CmVec *value_entries, CmVec *macro_entries)
{
    size_t index;

    for (index = 0u; index < block->child_count; ++index) {
        cm_record_item_declarations(state, source, ast,
            block->children[index].item_id,
            block->children[index].attributes,
            block->children[index].attribute_count,
            type_entries, value_entries, macro_entries);
    }
}

static int cm_active_unit_matches(CmModuleGraphState *state,
    const char *path, const struct stat *identity)
{
    size_t index;

    for (index = 0u; index < state->units.len; ++index) {
        const CmResolveUnit *unit;
        const CmSourceFile *file;
        char *normalized;
        int path_matches;

        unit = (const CmResolveUnit *)cm_vec_at_const(&state->units, index);
        if (unit == NULL || !unit->active) continue;
        if (identity != NULL && unit->has_identity &&
            unit->device == identity->st_dev && unit->inode == identity->st_ino)
            return 1;
        file = cm_source_get(state->building_sources, unit->source);
        if (file == NULL) continue;
        normalized = cm_path_normalize(file->path);
        path_matches = strcmp(normalized, path) == 0;
        cm_free(normalized);
        if (path_matches) return 1;
    }
    return 0;
}

static CmSourceId cm_find_loaded_source(CmSourceSet *sources,
    const char *path)
{
    size_t index;

    for (index = 0u; index < sources->length; ++index) {
        char *normalized;
        int matches;

        normalized = cm_path_normalize(sources->files[index].path);
        matches = strcmp(normalized, path) == 0;
        cm_free(normalized);
        if (matches) return sources->files[index].id;
    }
    return 0u;
}

static int cm_ast_path_has_final_name(const CmAst *ast, CmAstPathId path_id,
    const char *expected, int *out_unqualified)
{
    const CmAstPath *path;
    const CmInternedString *name;
    size_t expected_length;

    if (out_unqualified != NULL) *out_unqualified = 0;
    path = cm_ast_get_path(ast, path_id);
    if (path == NULL || path->segment_count == 0u) return 0;
    name = cm_ast_get_string(ast,
        path->segments[path->segment_count - 1u].name);
    expected_length = strlen(expected);
    if (name == NULL || name->len != expected_length
        || memcmp(name->bytes, expected, expected_length) != 0) return 0;
    if (out_unqualified != NULL) {
        *out_unqualified = !path->absolute && path->segment_count == 1u;
    }
    return 1;
}

static int cm_ast_attribute_has_head(const CmAst *ast,
    CmAstAttributeId attribute_id, const char *expected)
{
    const CmAstAttribute *attribute;
    const CmInternedString *text;
    size_t expected_length;
    size_t index;

    attribute = cm_ast_get_attribute(ast, attribute_id);
    if (attribute == NULL) return 0;
    text = cm_ast_get_string(ast, attribute->text);
    if (text == NULL) return 0;
    index = 0u;
    while (index < text->len
        && (text->bytes[index] == ' ' || text->bytes[index] == '\t'
            || text->bytes[index] == '\r' || text->bytes[index] == '\n')) {
        ++index;
    }
    if (index < text->len && text->bytes[index] == '#') {
        ++index;
        if (index < text->len && text->bytes[index] == '!') ++index;
        while (index < text->len
            && (text->bytes[index] == ' ' || text->bytes[index] == '\t'
                || text->bytes[index] == '\r'
                || text->bytes[index] == '\n')) ++index;
        if (index >= text->len || text->bytes[index] != '[') return 0;
        ++index;
        while (index < text->len
            && (text->bytes[index] == ' ' || text->bytes[index] == '\t'
                || text->bytes[index] == '\r'
                || text->bytes[index] == '\n')) ++index;
    }
    expected_length = strlen(expected);
    if (text->len - index < expected_length
        || memcmp(text->bytes + index, expected, expected_length) != 0) {
        return 0;
    }
    index += expected_length;
    return index == text->len || text->bytes[index] == '('
        || text->bytes[index] == ']'
        || text->bytes[index] == ' ' || text->bytes[index] == '\t'
        || text->bytes[index] == '\r' || text->bytes[index] == '\n';
}

static int cm_item_has_macro_use_attribute(const CmAst *ast,
    const CmAstItem *item)
{
    uint32_t index;

    for (index = 0u; index < item->attribute_count; ++index) {
        if (cm_ast_attribute_has_head(ast, item->attributes[index],
                "macro_use")) return 1;
    }
    return 0;
}

static int cm_include_literal_path(const CmAst *ast,
    const CmAstMacroInvocation *invocation, unsigned char **out_path,
    size_t *out_length)
{
    const CmInternedString *arguments;
    size_t start;
    size_t end;
    size_t index;
    unsigned char *copy;

    *out_path = NULL;
    *out_length = 0u;
    if (invocation->form != CM_AST_MACRO_INVOCATION) return 0;
    arguments = cm_ast_get_string(ast, invocation->arguments);
    if (arguments == NULL) return 0;
    start = 0u;
    while (start < arguments->len
        && (arguments->bytes[start] == ' '
            || arguments->bytes[start] == '\t'
            || arguments->bytes[start] == '\r'
            || arguments->bytes[start] == '\n')) ++start;
    end = arguments->len;
    while (end > start
        && (arguments->bytes[end - 1u] == ' '
            || arguments->bytes[end - 1u] == '\t'
            || arguments->bytes[end - 1u] == '\r'
            || arguments->bytes[end - 1u] == '\n')) --end;
    if (end - start < 3u || arguments->bytes[start] != '"'
        || arguments->bytes[end - 1u] != '"') return 0;
    start += 1u;
    end -= 1u;
    for (index = start; index < end; ++index) {
        unsigned char byte;

        byte = arguments->bytes[index];
        if (byte == 0u || byte == '\\' || byte == '"'
            || byte == '\r' || byte == '\n') return 0;
    }
    if (start == end || arguments->bytes[start] == '/') return 0;
    copy = (unsigned char *)cm_alloc(end - start + 1u);
    memcpy(copy, arguments->bytes + start, end - start);
    copy[end - start] = 0;
    *out_path = copy;
    *out_length = end - start;
    return 1;
}

static void cm_include_error(CmModuleGraphState *state,
    CmResolveErrorKind kind, CmSourceId source, const CmAstItem *item,
    const char *detail_a, const char *detail_b)
{
    cm_graph_add_error(state, kind, source,
        item == NULL ? 0u : item->span.start,
        item == NULL ? 0u : item->span.end, CM_RESOLVE_STRING_NONE,
        detail_a == NULL ? CM_RESOLVE_STRING_NONE
            : cm_graph_intern_c_str(state, detail_a),
        detail_b == NULL ? CM_RESOLVE_STRING_NONE
            : cm_graph_intern_c_str(state, detail_b), 0u, 0u);
}

static int cm_replace_authenticated_include_items(CmResolveUnit *unit,
    const CmItemMacroPendingInvocation *pending,
    const CmAstItemId *included_items, uint32_t included_count)
{
    const CmAstItemId *items;
    uint32_t item_count;
    CmAstItem *container;
    CmVec replacement;
    uint32_t index;
    size_t matches;

    container = NULL;
    if (pending->container_item == CM_AST_ITEM_NONE) {
        items = (const CmAstItemId *)unit->ast.root_items.data;
        item_count = cm_count_u32(unit->ast.root_items.len);
    } else {
        container = (CmAstItem *)cm_vec_at(&unit->ast.items,
            (size_t)pending->container_item - 1u);
        if (container == NULL || container->kind != CM_AST_ITEM_MODULE
            || !container->data.module_item.is_inline) return 0;
        items = container->data.module_item.items;
        item_count = container->data.module_item.item_count;
    }
    cm_vec_init(&replacement, sizeof(CmAstItemId));
    matches = 0u;
    for (index = 0u; index < item_count; ++index) {
        if (items[index] == pending->invocation.item) {
            cm_vec_append(&replacement, included_items, included_count);
            matches += 1u;
        } else {
            (void)cm_vec_push(&replacement, &items[index]);
        }
    }
    if (matches != 1u) {
        cm_vec_destroy(&replacement);
        return 0;
    }
    if (container == NULL) {
        cm_vec_clear(&unit->ast.root_items);
        cm_vec_append(&unit->ast.root_items, replacement.data,
            replacement.len);
    } else {
        CmAstItemId *copy;
        size_t size;

        copy = NULL;
        if (replacement.len != 0u) {
            if (!cm_size_mul(replacement.len, sizeof(CmAstItemId), &size))
                cm_alloc_out_of_memory((size_t)-1);
            copy = (CmAstItemId *)cm_arena_alloc(&unit->ast.storage, size,
                sizeof(void *));
            memcpy(copy, replacement.data, size);
        }
        container = (CmAstItem *)cm_vec_at(&unit->ast.items,
            (size_t)pending->container_item - 1u);
        if (container == NULL) {
            cm_vec_destroy(&replacement);
            return 0;
        }
        container->data.module_item.items = copy;
        container->data.module_item.item_count =
            cm_count_u32(replacement.len);
    }
    cm_vec_destroy(&replacement);
    return 1;
}

static int cm_splice_authenticated_include(CmModuleGraphState *state,
    CmResolveUnit *unit, const CmItemMacroPendingInvocation *pending)
{
    const CmAstItem *item;
    CmSourceId invoking_source;
    unsigned char *literal;
    size_t literal_length;
    const CmSourceFile *invoking_file;
    char *directory;
    char *selected;
    CmSourceId included_source;
    CmSourceStatus source_status;
    const CmSourceFile *included_file;
    size_t old_item_count;
    size_t item_index;
    uint32_t invoking_depth;
    uint32_t included_depth;
    CmItemListFragment fragment;

    item = cm_ast_get_item(&unit->ast, pending->invocation.item);
    invoking_source = cm_unit_item_source(unit, pending->invocation.item);
    invoking_depth = cm_unit_item_include_depth(unit,
        pending->invocation.item);
    if (item == NULL || invoking_source == 0u || pending->is_qualified
        || item->kind != CM_AST_ITEM_MACRO
        || item->data.macro_item.form != CM_AST_MACRO_INVOCATION
        || item->data.macro_item.delimiter != CM_AST_DELIMITER_PAREN
        || !item->data.macro_item.has_semicolon
        || item->visibility.kind != CM_AST_VIS_INHERITED
        || item->attribute_count != 0u) {
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
            invoking_source, item, "unsupported authenticated include form",
            "include must be an unqualified, unattributed item ending in ';'");
        return 0;
    }
    literal = NULL;
    literal_length = 0u;
    if (!cm_include_literal_path(&unit->ast, &item->data.macro_item,
            &literal, &literal_length)) {
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
            invoking_source, item, "unsupported include argument",
            "expected one simple ordinary string literal");
        return 0;
    }
    invoking_file = cm_source_get(state->building_sources, invoking_source);
    if (invoking_file == NULL) {
        cm_free(literal);
        return 0;
    }
    directory = cm_path_directory(invoking_file->path);
    selected = cm_path_join_bytes(directory, literal, literal_length, "");
    cm_free(directory);
    cm_free(literal);
    if (invoking_depth >= CM_INCLUDE_MAX_DEPTH
        || state->authenticated_include_files >= CM_INCLUDE_MAX_FILES) {
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_LIMIT,
            invoking_source, item, selected,
            "authenticated include depth or file-count limit exceeded");
        cm_free(selected);
        return 0;
    }
    included_source = cm_find_loaded_source(state->building_sources,
        selected);
    if (included_source == 0u) {
        source_status = cm_source_load_file_bounded(state->building_sources,
            selected, CM_INCLUDE_MAX_BYTES
                - state->authenticated_include_bytes, &included_source);
        if (source_status != CM_SOURCE_OK) {
            cm_include_error(state, source_status == CM_SOURCE_TOO_LARGE
                    ? CM_RESOLVE_ERROR_INCLUDE_LIMIT
                    : CM_RESOLVE_ERROR_SOURCE_IO,
                invoking_source, item, selected,
                cm_source_status_name(source_status));
            cm_free(selected);
            return 0;
        }
    }
    included_file = cm_source_get(state->building_sources, included_source);
    if (included_file == NULL
        || included_file->length > CM_INCLUDE_MAX_BYTES
        || state->authenticated_include_bytes
            > CM_INCLUDE_MAX_BYTES - included_file->length) {
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_LIMIT,
            invoking_source, item, selected,
            "authenticated include byte limit exceeded");
        cm_free(selected);
        return 0;
    }
    for (item_index = 0u; item_index < unit->item_sources.len;
            ++item_index) {
        const CmSourceId *source;

        source = (const CmSourceId *)cm_vec_at_const(&unit->item_sources,
            item_index);
        if (source != NULL && *source == included_source) {
            cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_CYCLE,
                invoking_source, item, selected,
                "repeated authenticated include source is unsupported");
            cm_free(selected);
            return 0;
        }
    }
    state->authenticated_include_files += 1u;
    state->authenticated_include_bytes += included_file->length;
    old_item_count = unit->ast.items.len;
    included_depth = invoking_depth + 1u;
    fragment = cm_parse_item_list_fragment(&unit->ast,
        (const char *)included_file->bytes, included_file->length,
        state->options.edition);
    for (item_index = old_item_count; item_index < unit->ast.items.len;
            ++item_index) {
        (void)cm_vec_push(&unit->item_sources, &included_source);
        (void)cm_vec_push(&unit->item_include_depths, &included_depth);
    }
    cm_free(selected);
    if (fragment.parse.error_count != 0u) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_PARSE, included_source,
            (uint32_t)fragment.parse.first_error.offset,
            (uint32_t)fragment.parse.first_error.offset,
            CM_RESOLVE_STRING_NONE,
            cm_graph_intern_c_str(state,
                fragment.parse.first_error.message),
            CM_RESOLVE_STRING_NONE,
            (uint32_t)fragment.parse.first_error.line,
            (uint32_t)fragment.parse.first_error.column);
        return 0;
    }
    if (!cm_replace_authenticated_include_items(unit, pending,
            fragment.items, fragment.item_count)) {
        item = cm_ast_get_item(&unit->ast, pending->invocation.item);
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
            invoking_source, item, "stale authenticated include invocation",
            NULL);
        return 0;
    }
    unit->parsed_item_count = unit->ast.items.len;
    return 1;
}

static int cm_include_stack_contains(CmModuleGraphState *state,
    const CmIncludeExpansion *expansion, CmSourceId source,
    const char *normalized_path)
{
    struct stat candidate_identity;
    int candidate_has_identity;
    size_t index;

    candidate_has_identity = cm_path_is_regular_file(normalized_path,
        &candidate_identity);
    for (index = 0u; index < expansion->active_sources.len; ++index) {
        const CmSourceId *active_source;
        const CmSourceFile *active_file;
        struct stat active_identity;
        char *active_path;
        int matches;

        active_source = (const CmSourceId *)cm_vec_at_const(
            &expansion->active_sources, index);
        if (active_source == NULL) continue;
        if (*active_source == source) return 1;
        active_file = cm_source_get(state->building_sources, *active_source);
        if (active_file == NULL) continue;
        if (candidate_has_identity
            && cm_path_is_regular_file(active_file->path, &active_identity)
            && candidate_identity.st_dev == active_identity.st_dev
            && candidate_identity.st_ino == active_identity.st_ino) {
            return 1;
        }
        active_path = cm_path_normalize(active_file->path);
        matches = strcmp(active_path, normalized_path) == 0;
        cm_free(active_path);
        if (matches) return 1;
    }
    return 0;
}

static int cm_expand_include_item_list(CmModuleGraphState *state,
    CmResolveUnit *unit, const CmAstItemId *items, uint32_t item_count,
    uint32_t depth,
    CmIncludeExpansion *expansion, CmVec *out_items);

static int cm_replace_nested_item_list(CmModuleGraphState *state,
    CmResolveUnit *unit, CmAstItemId item_id, const CmAstItemId *items,
    uint32_t item_count, uint32_t depth,
    CmIncludeExpansion *expansion)
{
    CmVec expanded;
    CmAstItemId *copy;
    size_t size;

    cm_vec_init(&expanded, sizeof(CmAstItemId));
    if (!cm_expand_include_item_list(state, unit, items, item_count, depth,
            expansion, &expanded)) {
        cm_vec_destroy(&expanded);
        return 0;
    }
    copy = NULL;
    if (expanded.len != 0u) {
        if (!cm_size_mul(expanded.len, sizeof(CmAstItemId), &size))
            cm_alloc_out_of_memory((size_t)-1);
        copy = (CmAstItemId *)cm_arena_alloc(&unit->ast.storage, size,
            sizeof(void *));
        memcpy(copy, expanded.data, size);
    }
    {
        CmAstItem *item;

        item = (CmAstItem *)cm_vec_at(&unit->ast.items,
            (size_t)item_id - 1u);
        if (item == NULL) {
            cm_vec_destroy(&expanded);
            return 0;
        }
        if (item->kind == CM_AST_ITEM_MODULE) {
            item->data.module_item.items = copy;
            item->data.module_item.item_count = cm_count_u32(expanded.len);
        } else if (item->kind == CM_AST_ITEM_TRAIT) {
            item->data.trait_item.items = copy;
            item->data.trait_item.item_count = cm_count_u32(expanded.len);
        } else if (item->kind == CM_AST_ITEM_IMPL) {
            item->data.impl_item.items = copy;
            item->data.impl_item.item_count = cm_count_u32(expanded.len);
        } else {
            cm_vec_destroy(&expanded);
            return 0;
        }
    }
    cm_vec_destroy(&expanded);
    return 1;
}

static int cm_expand_one_include(CmModuleGraphState *state,
    CmResolveUnit *unit, CmSourceId invoking_source, const CmAstItem *item,
    uint32_t depth, CmIncludeExpansion *expansion, CmVec *out_items)
{
    const CmSourceFile *invoking_file;
    unsigned char *literal;
    size_t literal_length;
    char *directory;
    char *selected;
    CmSourceId included_source;
    CmSourceStatus source_status;
    const CmSourceFile *included_file;
    size_t old_item_count;
    CmItemListFragment fragment;
    size_t item_index;
    int ok;

    literal = NULL;
    literal_length = 0u;
    if (!cm_include_literal_path(&unit->ast, &item->data.macro_item,
            &literal, &literal_length)) {
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
            invoking_source, item, "unsupported include argument",
            "expected one simple ordinary string literal");
        return 0;
    }
    invoking_file = cm_source_get(state->building_sources, invoking_source);
    if (invoking_file == NULL) {
        cm_free(literal);
        return 0;
    }
    directory = cm_path_directory(invoking_file->path);
    selected = cm_path_join_bytes(directory, literal, literal_length, "");
    cm_free(directory);
    cm_free(literal);
    if (depth >= CM_INCLUDE_MAX_DEPTH
        || expansion->file_count >= CM_INCLUDE_MAX_FILES) {
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_LIMIT,
            invoking_source, item, selected,
            "include depth or file-count limit exceeded");
        cm_free(selected);
        return 0;
    }
    included_source = cm_find_loaded_source(state->building_sources,
        selected);
    if (included_source == 0u) {
        source_status = cm_source_load_file_bounded(
            state->building_sources, selected,
            CM_INCLUDE_MAX_BYTES - expansion->byte_count,
            &included_source);
        if (source_status != CM_SOURCE_OK) {
            if (source_status == CM_SOURCE_TOO_LARGE) {
                cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_LIMIT,
                    invoking_source, item, selected,
                    "include byte limit exceeded while loading");
            } else {
                cm_include_error(state, CM_RESOLVE_ERROR_SOURCE_IO,
                    invoking_source, item, selected,
                    cm_source_status_name(source_status));
            }
            cm_free(selected);
            return 0;
        }
    }
    if (cm_include_stack_contains(state, expansion, included_source,
            selected)) {
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_CYCLE,
            invoking_source, item, selected, NULL);
        cm_free(selected);
        return 0;
    }
    included_file = cm_source_get(state->building_sources, included_source);
    if (included_file == NULL) {
        cm_free(selected);
        return 0;
    }
    if (included_file->length > CM_INCLUDE_MAX_BYTES
        || expansion->byte_count
            > CM_INCLUDE_MAX_BYTES - included_file->length) {
        cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_LIMIT,
            invoking_source, item, selected,
            "include depth, file count, or byte limit exceeded");
        cm_free(selected);
        return 0;
    }
    expansion->file_count += 1u;
    expansion->byte_count += included_file->length;
    (void)cm_vec_push(&expansion->active_sources, &included_source);
    old_item_count = unit->ast.items.len;
    fragment = cm_parse_item_list_fragment(&unit->ast,
        (const char *)included_file->bytes, included_file->length,
        state->options.edition);
    for (item_index = old_item_count; item_index < unit->ast.items.len;
            ++item_index) {
        uint32_t included_depth;

        included_depth = depth + 1u;
        (void)cm_vec_push(&unit->item_sources, &included_source);
        (void)cm_vec_push(&unit->item_include_depths, &included_depth);
    }
    if (fragment.parse.error_count != 0u) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_PARSE, included_source,
            (uint32_t)fragment.parse.first_error.offset,
            (uint32_t)fragment.parse.first_error.offset,
            CM_RESOLVE_STRING_NONE,
            cm_graph_intern_c_str(state,
                fragment.parse.first_error.message),
            CM_RESOLVE_STRING_NONE,
            (uint32_t)fragment.parse.first_error.line,
            (uint32_t)fragment.parse.first_error.column);
        (void)cm_vec_pop(&expansion->active_sources, NULL);
        cm_free(selected);
        return 0;
    }
    ok = cm_expand_include_item_list(state, unit, fragment.items,
        fragment.item_count, depth + 1u, expansion, out_items);
    (void)cm_vec_pop(&expansion->active_sources, NULL);
    cm_free(selected);
    return ok;
}

static int cm_expand_include_item_list(CmModuleGraphState *state,
    CmResolveUnit *unit, const CmAstItemId *items, uint32_t item_count,
    uint32_t depth,
    CmIncludeExpansion *expansion, CmVec *out_items)
{
    uint32_t index;

    if (item_count != 0u && items == NULL) return 0;
    for (index = 0u; index < item_count; ++index) {
        const CmAstItem *candidate;

        candidate = cm_ast_get_item(&unit->ast, items[index]);
        if (candidate == NULL) return 0;
        if (candidate->kind == CM_AST_ITEM_USE
            || cm_item_has_macro_use_attribute(&unit->ast, candidate)) {
            expansion->binding_ambiguous = 1;
        }
    }
    for (index = 0u; index < item_count; ++index) {
        CmAstItemId item_id;
        const CmAstItem *item;
        CmSourceId source;
        int is_include;
        int is_unqualified;

        item_id = items[index];
        item = cm_ast_get_item(&unit->ast, item_id);
        source = cm_unit_item_source(unit, item_id);
        if (item == NULL || source == 0u) return 0;
        is_unqualified = 0;
        is_include = item->kind == CM_AST_ITEM_MACRO
            && item->data.macro_item.form == CM_AST_MACRO_INVOCATION
            && cm_ast_path_has_final_name(&unit->ast,
                item->data.macro_item.path, "include", &is_unqualified);
        if (is_include) {
            if (state->options.include_expansion
                    != CM_INCLUDE_EXPANSION_SOURCE_FIXTURE) {
                cm_include_error(state,
                    CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED, source, item,
                    "include macro binding is unresolved",
                    "builtin include expansion is disabled");
                return 0;
            }
            if (expansion->binding_ambiguous) {
                cm_include_error(state,
                    CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED, source, item,
                    "include macro binding is ambiguous",
                    "local, imported, or macro-generated bindings are outside fixture expansion");
                return 0;
            }
            if (!is_unqualified
                || item->data.macro_item.delimiter != CM_AST_DELIMITER_PAREN
                || !item->data.macro_item.has_semicolon
                || item->visibility.kind != CM_AST_VIS_INHERITED
                || item->attribute_count != 0u) {
                cm_include_error(state,
                    CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED, source, item,
                    "unsupported include form",
                    "include must be unqualified, unattributed, and end in ';'");
                return 0;
            }
            if (!cm_expand_one_include(state, unit, source, item, depth,
                    expansion, out_items)) return 0;
            continue;
        }
        if (item->kind == CM_AST_ITEM_MACRO) {
            expansion->binding_ambiguous = 1;
        }
        if (source != unit->source
            && (item->kind == CM_AST_ITEM_MACRO
                || item->kind == CM_AST_ITEM_MODULE
                || item->kind == CM_AST_ITEM_USE
                || item->kind == CM_AST_ITEM_EXTERN_CRATE
                || item->kind == CM_AST_ITEM_EXTERN_BLOCK)) {
            cm_include_error(state, CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
                source, item, "unsupported item in included source",
                "macros, modules, uses, and extern items are outside the bounded include path");
            return 0;
        }
        if (item->kind == CM_AST_ITEM_MODULE && item->data.module_item.is_inline) {
            if (!cm_replace_nested_item_list(state, unit, item_id,
                    item->data.module_item.items,
                    item->data.module_item.item_count, depth, expansion)) {
                return 0;
            }
        } else if (item->kind == CM_AST_ITEM_TRAIT) {
            if (!cm_replace_nested_item_list(state, unit, item_id,
                    item->data.trait_item.items,
                    item->data.trait_item.item_count, depth, expansion)) {
                return 0;
            }
        } else if (item->kind == CM_AST_ITEM_IMPL) {
            if (!cm_replace_nested_item_list(state, unit, item_id,
                    item->data.impl_item.items,
                    item->data.impl_item.item_count, depth, expansion)) {
                return 0;
            }
        }
        (void)cm_vec_push(out_items, &item_id);
    }
    return 1;
}

static int cm_expand_unit_includes(CmModuleGraphState *state,
    CmResolveUnitId unit_id)
{
    CmResolveUnit *unit;
    CmIncludeExpansion expansion;
    CmVec expanded;
    int ok;

    unit = cm_get_unit(state, unit_id);
    if (unit == NULL) return 0;
    cm_vec_init(&expansion.active_sources, sizeof(CmSourceId));
    expansion.file_count = 0u;
    expansion.byte_count = 0u;
    expansion.binding_ambiguous = 0;
    (void)cm_vec_push(&expansion.active_sources, &unit->source);
    cm_vec_init(&expanded, sizeof(CmAstItemId));
    ok = cm_expand_include_item_list(state, unit,
        (const CmAstItemId *)unit->ast.root_items.data,
        cm_count_u32(unit->ast.root_items.len), 0u, &expansion, &expanded);
    if (ok) {
        cm_vec_clear(&unit->ast.root_items);
        cm_vec_append(&unit->ast.root_items, expanded.data, expanded.len);
        unit->parsed_item_count = unit->ast.items.len;
    }
    cm_vec_destroy(&expanded);
    cm_vec_destroy(&expansion.active_sources);
    return ok;
}

static CmModuleId cm_build_module(CmModuleGraphState *state,
    CmModuleId parent, CmResolveUnitId unit_id, CmResolveStringId name,
    CmResolveStringId absolute_path, CmResolveStringId module_directory,
    CmResolveItemRef declaration, int is_inline,
    const CmAstItemId *items, uint32_t item_count,
    const CmItemMacroPlanNode *effective_nodes,
    size_t effective_node_count,
    const CmEffectiveAttribute *inner_attributes,
    size_t inner_attribute_count, int inner_attributes_generated,
    CmSpan inner_attribute_span);

static CmModuleId cm_build_external_module(CmModuleGraphState *state,
    CmModuleId parent, CmResolveUnitId parent_unit,
    const CmPendingModule *pending, CmResolveStringId absolute_path,
    CmResolveStringId module_directory, int *out_inactive)
{
    const CmResolveModuleNode *parent_node;
    const CmSourceFile *parent_file;
    const CmInternedString *directory;
    const CmInternedString *name;
    char *candidate_file;
    char *candidate_mod;
    const unsigned char *path_bytes;
    size_t path_length;
    size_t path_count;
    size_t attribute_index;
    int path_malformed;
    struct stat file_stat;
    struct stat mod_stat;
    int has_file;
    int has_mod;
    const char *selected;
    const struct stat *selected_stat;
    char *selected_directory;
    CmSourceId source;
    CmSourceStatus source_status;
    CmResolveUnitId unit_id;
    CmResolveUnit *unit;
    const CmResolveUnit *parent_source_unit;
    CmResolveItemRef declaration;
    const CmAstItemId *root_items;
    uint32_t root_item_count;
    CmModuleId result;

    if (out_inactive != NULL) *out_inactive = 0;
    if (out_inactive == NULL) return CM_MODULE_NONE;
    parent_node = cm_get_module_node_const(state, parent);
    directory = parent_node == NULL ? NULL
        : cm_graph_string(state, parent_node->module_directory);
    name = cm_graph_string(state, pending->name);
    if (directory == NULL || name == NULL) return CM_MODULE_NONE;
    path_bytes = NULL;
    path_length = 0u;
    path_count = 0u;
    path_malformed = 0;
    for (attribute_index = 0u; attribute_index < pending->attribute_count;
            ++attribute_index) {
        const unsigned char *candidate_bytes;
        size_t candidate_length;
        int match;

        candidate_bytes = NULL;
        candidate_length = 0u;
        match = cm_effective_attribute_simple_string(
            &pending->attributes[attribute_index], "path",
            &candidate_bytes, &candidate_length);
        if (match > 0) {
            path_count += 1u;
            path_bytes = candidate_bytes;
            path_length = candidate_length;
        } else if (match < 0) {
            path_malformed = 1;
        }
    }
    if (path_malformed || path_count > 1u) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_SOURCE_IO,
            pending->span.source, pending->span.start, pending->span.end,
            absolute_path,
            cm_graph_intern_c_str(state, "unsupported module path attribute"),
            cm_graph_intern_c_str(state,
                "path must be one simple relative string literal"), 0u, 0u);
        return CM_MODULE_NONE;
    }
    candidate_mod = NULL;
    if (path_count == 1u) {
        char *source_directory;

        parent_source_unit = cm_get_unit_const(state, parent_unit);
        parent_file = parent_source_unit == NULL ? NULL
            : cm_source_get(state->building_sources,
                parent_source_unit->source);
        if (parent_file == NULL) return CM_MODULE_NONE;
        source_directory = cm_path_directory(parent_file->path);
        candidate_file = cm_path_join_bytes(source_directory, path_bytes,
            path_length, "");
        cm_free(source_directory);
    } else {
        candidate_file = cm_path_join_bytes((const char *)directory->bytes,
            name->bytes, name->len, ".rs");
    }
    if (path_count == 0u) {
        char *child_directory;

        child_directory = cm_path_join_bytes((const char *)directory->bytes,
            name->bytes, name->len, "");
        candidate_mod = cm_path_join_bytes(child_directory,
            (const unsigned char *)"mod", 3u, ".rs");
        cm_free(child_directory);
    }
    has_file = cm_path_is_regular_file(candidate_file, &file_stat);
    has_mod = candidate_mod != NULL
        && cm_path_is_regular_file(candidate_mod, &mod_stat);
    if (has_file && has_mod) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_AMBIGUOUS_MODULE_FILE,
            pending->span.source,
            pending->span.start, pending->span.end, absolute_path,
            cm_graph_intern_c_str(state, candidate_file),
            candidate_mod == NULL ? CM_RESOLVE_STRING_NONE
                : cm_graph_intern_c_str(state, candidate_mod), 0u, 0u);
        cm_free(candidate_mod);
        cm_free(candidate_file);
        return CM_MODULE_NONE;
    }
    if (!has_file && !has_mod) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_MISSING_MODULE_FILE,
            pending->span.source,
            pending->span.start, pending->span.end, absolute_path,
            cm_graph_intern_c_str(state, candidate_file),
            candidate_mod == NULL ? CM_RESOLVE_STRING_NONE
                : cm_graph_intern_c_str(state, candidate_mod), 0u, 0u);
        cm_free(candidate_mod);
        cm_free(candidate_file);
        return CM_MODULE_NONE;
    }
    selected = has_file ? candidate_file : candidate_mod;
    selected_stat = has_file ? &file_stat : &mod_stat;
    selected_directory = NULL;
    /* A `#[path = "../../backtrace/src/lib.rs"] mod backtrace_rs;` file
     * owns its own directory for nested modules (std's vendored
     * backtrace declares `mod backtrace;` beside its lib.rs), like a
     * mod.rs would. */
    if (path_count == 1u) {
        selected_directory = cm_path_directory(selected);
        module_directory = cm_graph_intern_c_str(state,
            selected_directory);
    }
    if (cm_active_unit_matches(state, selected, selected_stat)) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_MODULE_CYCLE,
            pending->span.source,
            pending->span.start, pending->span.end, absolute_path,
            cm_graph_intern_c_str(state, selected),
            CM_RESOLVE_STRING_NONE, 0u, 0u);
        cm_free(candidate_mod);
        cm_free(candidate_file);
        cm_free(selected_directory);
        return CM_MODULE_NONE;
    }
    source = cm_find_loaded_source(state->building_sources, selected);
    if (source == 0u) {
        source_status = cm_source_load_file(state->building_sources, selected,
            &source);
        if (source_status != CM_SOURCE_OK) {
            cm_graph_add_error(state, CM_RESOLVE_ERROR_SOURCE_IO,
                pending->span.source,
                pending->span.start, pending->span.end, absolute_path,
                cm_graph_intern_c_str(state, selected),
                cm_graph_intern_c_str(state,
                    cm_source_status_name(source_status)), 0u, 0u);
            cm_free(candidate_mod);
            cm_free(candidate_file);
            cm_free(selected_directory);
            return CM_MODULE_NONE;
        }
    }
    unit_id = cm_get_or_add_unit(state, source);
    unit = cm_get_unit(state, unit_id);
    if (unit == NULL || !unit->parse_ok) {
        cm_free(candidate_mod);
        cm_free(candidate_file);
        cm_free(selected_directory);
        return CM_MODULE_NONE;
    }
    if (!unit->plan_prepared
        && !cm_prepare_unit_plan(state, unit_id, pending->external_scope,
            pending->external_scope_count)) {
        cm_free(candidate_mod);
        cm_free(candidate_file);
        cm_free(selected_directory);
        return CM_MODULE_NONE;
    }
    unit = cm_get_unit(state, unit_id);
    if (unit == NULL || !unit->plan_ok) {
        cm_free(candidate_mod);
        cm_free(candidate_file);
        cm_free(selected_directory);
        return CM_MODULE_NONE;
    }
    if (!unit->plan.crate_is_active) {
        *out_inactive = 1;
        cm_free(candidate_mod);
        cm_free(candidate_file);
        cm_free(selected_directory);
        return CM_MODULE_NONE;
    }
    unit->active = 1;
    root_items = (const CmAstItemId *)unit->ast.root_items.data;
    root_item_count = cm_count_u32(unit->ast.root_items.len);
    memset(&declaration, 0, sizeof(declaration));
    parent_source_unit = cm_get_unit_const(state, parent_unit);
    if (parent_source_unit == NULL) {
        unit->active = 0;
        cm_free(candidate_mod);
        cm_free(candidate_file);
        cm_free(selected_directory);
        return CM_MODULE_NONE;
    }
    declaration = pending->declaration;
    result = cm_build_module(state, parent, unit_id, pending->name,
        absolute_path, module_directory, declaration, 0, root_items,
        root_item_count, unit->plan.roots, unit->plan.root_count,
        unit->plan.crate_attributes, unit->plan.crate_attribute_count,
        0, (CmSpan){ 0u, 0u, 0u });
    unit = cm_get_unit(state, unit_id);
    if (unit != NULL) unit->active = 0;
    cm_free(candidate_mod);
    cm_free(candidate_file);
    cm_free(selected_directory);
    return result;
}

static CmModuleId cm_build_module(CmModuleGraphState *state,
    CmModuleId parent, CmResolveUnitId unit_id, CmResolveStringId name,
    CmResolveStringId absolute_path, CmResolveStringId module_directory,
    CmResolveItemRef declaration, int is_inline,
    const CmAstItemId *items, uint32_t item_count,
    const CmItemMacroPlanNode *effective_nodes,
    size_t effective_node_count,
    const CmEffectiveAttribute *inner_attributes,
    size_t inner_attribute_count, int inner_attributes_generated,
    CmSpan inner_attribute_span)
{
    const CmResolveUnit *unit;
    const CmSourceFile *source_file;
    CmResolveModuleNode node;
    CmModuleId module_id;
    CmVec type_entries;
    CmVec value_entries;
    CmVec macro_entries;
    CmVec imports;
    CmVec pending_modules;
    CmVec children;
    CmVec active_items;
    CmVec effective_items;
    CmResolveEffectiveItemId next_effective_item_id;
    size_t index;

    unit = cm_get_unit_const(state, unit_id);
    if (unit == NULL) return CM_MODULE_NONE;
    if (effective_node_count != 0u && effective_nodes == NULL)
        return CM_MODULE_NONE;
    if (inner_attribute_count != 0u && inner_attributes == NULL)
        return CM_MODULE_NONE;
    source_file = cm_source_get(state->building_sources,
        is_inline ? declaration.source : unit->source);
    if (source_file == NULL) return CM_MODULE_NONE;
    if (cm_module_path_exists(state, absolute_path)) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_DUPLICATE_MODULE_PATH,
            unit->source, 0u, 0u, absolute_path, CM_RESOLVE_STRING_NONE,
            CM_RESOLVE_STRING_NONE, 0u, 0u);
        return CM_MODULE_NONE;
    }
    memset(&node, 0, sizeof(node));
    node.info.id = (CmModuleId)(state->modules.len + 1u);
    node.info.parent = parent;
    node.info.declaration = declaration;
    node.info.source = is_inline ? declaration.source : unit->source;
    node.info.name = name;
    node.info.absolute_path = absolute_path;
    node.info.source_path = cm_graph_intern_c_str(state, source_file->path);
    node.info.is_inline = is_inline;
    node.module_directory = module_directory;
    node.unit = unit_id;
    node.items = items;
    node.item_count = item_count;
    node.inner_attributes = cm_record_effective_inner_attributes(state,
        unit, declaration, inner_attributes, inner_attribute_count,
        inner_attributes_generated, inner_attribute_span);
    node.info.inner_attribute_count = cm_count_u32(inner_attribute_count);
    (void)cm_vec_push(&state->modules, &node);
    module_id = node.info.id;

    cm_vec_init(&type_entries, sizeof(CmResolveNamespaceEntry));
    cm_vec_init(&value_entries, sizeof(CmResolveNamespaceEntry));
    cm_vec_init(&macro_entries, sizeof(CmResolveNamespaceEntry));
    cm_vec_init(&imports, sizeof(CmResolveImport));
    cm_vec_init(&pending_modules, sizeof(CmPendingModule));
    cm_vec_init(&children, sizeof(CmModuleId));
    cm_vec_init(&active_items, sizeof(CmResolveItemRef));
    cm_vec_init(&effective_items, sizeof(CmResolveEffectiveItemRecord));
    next_effective_item_id = 1u;

    if (!cm_record_macro_declarations(state, unit, module_id,
            is_inline ? declaration.item : CM_AST_ITEM_NONE,
            &macro_entries)) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
            unit->source, 0u, 0u, absolute_path,
            cm_graph_intern_c_str(state,
                "invalid cfg-active macro declaration plan"),
            CM_RESOLVE_STRING_NONE, 0u, 0u);
    }

    for (index = 0u; index < effective_node_count; ++index) {
        const CmResolveEffectiveItemRecord *effective;
        const CmItemMacroPlanNode *plan_node;
        CmResolveEffectiveItemRecord effective_record;
        CmAstItemId item_id;
        const CmAstItem *item;

        plan_node = &effective_nodes[index];
        if (!cm_record_effective_plan_item(state, unit, plan_node,
                &next_effective_item_id, &effective_record)) {
            cm_graph_add_error(state, CM_RESOLVE_ERROR_ITEM_MACRO,
                unit->source, plan_node->span.start, plan_node->span.end,
                absolute_path,
                cm_graph_intern_c_str(state,
                    "invalid recursive effective item plan"),
                CM_RESOLVE_STRING_NONE, 0u, 0u);
            break;
        }
        (void)cm_vec_push(&effective_items, &effective_record);
        effective = (const CmResolveEffectiveItemRecord *)cm_vec_at_const(
            &effective_items, effective_items.len - 1u);
        if (effective == NULL) continue;
        item_id = effective->item.declaration.item;
        unit = cm_get_unit_const(state, unit_id);
        if (unit == NULL) break;
        item = cm_ast_get_item(&unit->ast, item_id);
        if (item == NULL) continue;
        {
            CmResolveItemRef active_item;

            active_item = effective->item.declaration;
            (void)cm_vec_push(&active_items, &active_item);
        }
        cm_record_item_declarations(state,
            effective->item.declaration.source, &unit->ast, item_id,
            plan_node->attributes, plan_node->attribute_count,
            &type_entries, &value_entries, &macro_entries);
        if (item->kind == CM_AST_ITEM_USE) {
            CmResolveImport import_directive;

            memset(&import_directive, 0, sizeof(import_directive));
            import_directive.declaration.source =
                effective->item.declaration.source;
            import_directive.declaration.item = item_id;
            import_directive.tree = cm_graph_copy_ast_string(state,
                &unit->ast, item->data.use_item.tree);
            import_directive.visibility = item->visibility.kind;
            (void)cm_vec_push(&imports, &import_directive);
        } else if (item->kind == CM_AST_ITEM_MODULE) {
            CmPendingModule pending;

            memset(&pending, 0, sizeof(pending));
            pending.name = cm_graph_copy_ast_string(state, &unit->ast,
                item->name);
            pending.declaration = effective->item.declaration;
            pending.span = effective->item.span;
            pending.is_inline = item->data.module_item.is_inline;
            pending.items = item->data.module_item.items;
            pending.item_count = item->data.module_item.item_count;
            pending.effective_items = plan_node->children;
            pending.effective_item_count = plan_node->child_count;
            pending.attributes = plan_node->attributes;
            pending.attribute_count = plan_node->attribute_count;
            pending.inner_attributes = plan_node->inner_attributes;
            pending.inner_attribute_count =
                plan_node->inner_attribute_count;
            pending.inner_attributes_generated = plan_node->is_generated;
            pending.external_scope = plan_node->external_scope;
            pending.external_scope_count = plan_node->external_scope_count;
            (void)cm_vec_push(&pending_modules, &pending);
        } else if (item->kind == CM_AST_ITEM_EXTERN_BLOCK) {
            cm_record_extern_block(state,
                effective->item.declaration.source, &unit->ast,
                plan_node, &type_entries, &value_entries, &macro_entries);
        }
    }

    for (index = 0u; index < pending_modules.len; ++index) {
        const CmPendingModule *pending;
        const CmResolveUnit *pending_unit;
        const CmAstItem *pending_item;
        CmResolveStringId child_path;
        const CmInternedString *parent_directory;
        const CmInternedString *child_name;
        char *child_directory_text;
        CmResolveStringId child_directory;
        CmModuleId child;
        int inactive;

        pending = (const CmPendingModule *)cm_vec_at_const(&pending_modules,
            index);
        if (pending == NULL) continue;
        pending_unit = cm_get_unit_const(state, unit_id);
        pending_item = pending_unit == NULL ? NULL : cm_ast_get_item(
            &pending_unit->ast, pending->declaration.item);
        if (!pending->is_inline && state->defer_non_macro_use_modules
            && (pending_item == NULL || !cm_item_has_macro_use_attribute(
                &pending_unit->ast, pending_item))) {
            continue;
        }
        child_path = cm_join_module_path(state, absolute_path, pending->name);
        parent_directory = cm_graph_string(state, module_directory);
        child_name = cm_graph_string(state, pending->name);
        if (parent_directory == NULL || child_name == NULL) continue;
        child_directory_text = cm_path_join_bytes(
            (const char *)parent_directory->bytes, child_name->bytes,
            child_name->len, "");
        child_directory = cm_graph_intern_c_str(state, child_directory_text);
        cm_free(child_directory_text);
        if (cm_module_path_exists(state, child_path)) {
            cm_graph_add_error(state, CM_RESOLVE_ERROR_DUPLICATE_MODULE_PATH,
                pending->span.source,
                pending->span.start, pending->span.end,
                child_path, CM_RESOLVE_STRING_NONE, CM_RESOLVE_STRING_NONE,
                0u, 0u);
            continue;
        }
        if (pending->is_inline) {
            /* External siblings can grow state->units and invalidate `unit`. */
            child = cm_build_module(state, module_id, unit_id, pending->name,
                child_path, child_directory, pending->declaration, 1,
                pending->items, pending->item_count,
                pending->effective_items, pending->effective_item_count,
                pending->inner_attributes, pending->inner_attribute_count,
                pending->inner_attributes_generated, pending->span);
        } else {
            inactive = 0;
            child = cm_build_external_module(state, module_id, unit_id,
                pending, child_path, child_directory, &inactive);
            if (inactive) {
                CmResolveItemRef inactive_declaration;
                size_t active_removed;
                size_t effective_removed;
                size_t type_removed;
                size_t unexpected_removed;

                inactive_declaration = pending->declaration;
                active_removed = cm_remove_active_declaration(&active_items,
                    inactive_declaration);
                effective_removed = cm_remove_effective_declaration(
                    &effective_items, inactive_declaration);
                type_removed = cm_remove_namespace_declaration(&type_entries,
                    inactive_declaration);
                unexpected_removed = cm_remove_namespace_declaration(
                    &value_entries, inactive_declaration);
                unexpected_removed += cm_remove_namespace_declaration(
                    &macro_entries, inactive_declaration);
                unexpected_removed += cm_remove_import_declaration(&imports,
                    inactive_declaration);
                if (active_removed != 1u || effective_removed != 1u
                    || type_removed != 1u || unexpected_removed != 0u) {
                    cm_graph_add_error(state, CM_RESOLVE_ERROR_CFG_EXPANSION,
                        pending->span.source, pending->span.start,
                        pending->span.end, child_path,
                        cm_graph_intern_c_str(state,
                            "inactive external module declaration"),
                        cm_graph_intern_c_str(state,
                            "parent semantic vectors are inconsistent"),
                        0u, 0u);
                }
            }
        }
        if (child != CM_MODULE_NONE) (void)cm_vec_push(&children, &child);
    }

    node = *cm_get_module_node(state, module_id);
    node.type_entries = (CmResolveNamespaceEntry *)cm_graph_copy_array(state,
        &type_entries);
    node.value_entries = (CmResolveNamespaceEntry *)cm_graph_copy_array(state,
        &value_entries);
    node.macro_entries = (CmResolveNamespaceEntry *)cm_graph_copy_array(state,
        &macro_entries);
    node.active_items = (CmResolveItemRef *)cm_graph_copy_array(state,
        &active_items);
    node.effective_items = (CmResolveEffectiveItemRecord *)
        cm_graph_copy_array(state, &effective_items);
    node.imports = (CmResolveImport *)cm_graph_copy_array(state, &imports);
    node.children = (CmModuleId *)cm_graph_copy_array(state, &children);
    node.info.type_count = cm_count_u32(type_entries.len);
    node.info.value_count = cm_count_u32(value_entries.len);
    node.info.macro_count = cm_count_u32(macro_entries.len);
    node.info.import_count = cm_count_u32(imports.len);
    node.info.active_item_count = cm_count_u32(active_items.len);
    node.info.effective_item_count = cm_count_u32(effective_items.len);
    node.info.child_count = cm_count_u32(children.len);
    *cm_get_module_node(state, module_id) = node;

    cm_vec_destroy(&children);
    cm_vec_destroy(&effective_items);
    cm_vec_destroy(&active_items);
    cm_vec_destroy(&pending_modules);
    cm_vec_destroy(&imports);
    cm_vec_destroy(&macro_entries);
    cm_vec_destroy(&value_entries);
    cm_vec_destroy(&type_entries);
    return module_id;
}

CmModuleGraphResult cm_module_graph_build(CmModuleGraph *graph,
    CmSourceSet *sources, CmSourceId root,
    const CmModuleGraphOptions *options)
{
    CmModuleGraphResult result;
    CmModuleGraphState *state;
    const CmSourceFile *root_file;
    CmResolveUnitId root_unit_id;
    CmResolveUnit *root_unit;
    char *normalized_root_path;
    char *root_directory_text;
    CmResolveStringId root_directory;
    CmResolveStringId root_name;
    CmResolveStringId root_path;
    CmModuleGraphRevision next_revision;
    uint64_t lifetime_id;
    size_t staging_round;
    int revision_exhausted;

    memset(&result, 0, sizeof(result));
    if (graph == NULL || graph->state == NULL) return result;
    state = cm_graph_state(graph);
    /* Reserve UINT64_MAX for the first exhausted, failed build. */
    revision_exhausted = state->revision_exhausted
        || state->revision >= UINT64_MAX - 1u;
    next_revision = revision_exhausted ? UINT64_MAX : state->revision + 1u;
    lifetime_id = state->lifetime_id;
    cm_graph_state_destroy(state);
    cm_graph_state_init(state);
    state->owner_graph = graph;
    state->lifetime_id = lifetime_id;
    state->revision = next_revision;
    state->revision_exhausted = revision_exhausted;
    result.revision = next_revision;
    if (revision_exhausted) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_REVISION_EXHAUSTED, 0u,
            0u, 0u, CM_RESOLVE_STRING_NONE, CM_RESOLVE_STRING_NONE,
            CM_RESOLVE_STRING_NONE, 0u, 0u);
        result.error_count = state->errors.len;
        return result;
    }
    if (sources == NULL || cm_source_get(sources, root) == NULL
        || options == NULL || options->cfg == NULL
        || (options->dependency_macro_count != 0u
            && options->dependency_macros == NULL)
        || (options->include_expansion != CM_INCLUDE_EXPANSION_DISABLED
            && options->include_expansion
                != CM_INCLUDE_EXPANSION_AUTHENTICATED
            && options->include_expansion
                != CM_INCLUDE_EXPANSION_SOURCE_FIXTURE)) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_INVALID_ARGUMENT, 0u,
            0u, 0u, CM_RESOLVE_STRING_NONE, CM_RESOLVE_STRING_NONE,
            CM_RESOLVE_STRING_NONE, 0u, 0u);
        result.error_count = state->errors.len;
        return result;
    }
    state->building_sources = sources;
    state->options = *options;
    if (!cm_register_dependency_artifacts(state, graph, options)) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_INVALID_ARGUMENT, 0u,
            0u, 0u, CM_RESOLVE_STRING_NONE,
            cm_graph_intern_c_str(state,
                "invalid dependency macro artifact set"),
            CM_RESOLVE_STRING_NONE, 0u, 0u);
        result.error_count = state->errors.len;
        cm_graph_clear_borrowed_build_inputs(state);
        return result;
    }
    root_file = cm_source_get(sources, root);
    root_unit_id = cm_get_or_add_unit(state, root);
    root_unit = cm_get_unit(state, root_unit_id);
    if (root_unit == NULL || !root_unit->parse_ok
        || !cm_prepare_unit_plan(state, root_unit_id, NULL, 0u)) {
        result.error_count = state->errors.len;
        cm_graph_clear_borrowed_build_inputs(state);
        return result;
    }
    root_unit = cm_get_unit(state, root_unit_id);
    if (root_unit == NULL || !root_unit->plan_ok) {
        result.error_count = state->errors.len;
        cm_graph_clear_borrowed_build_inputs(state);
        return result;
    }
    /* Include loading can grow and relocate CmSourceSet.files. */
    root_file = cm_source_get(sources, root);
    if (root_file == NULL) {
        cm_graph_add_error(state, CM_RESOLVE_ERROR_SOURCE_IO, root,
            0u, 0u, CM_RESOLVE_STRING_NONE, CM_RESOLVE_STRING_NONE,
            CM_RESOLVE_STRING_NONE, 0u, 0u);
        result.error_count = state->errors.len;
        cm_graph_clear_borrowed_build_inputs(state);
        return result;
    }
    normalized_root_path = cm_path_normalize(root_file->path);
    root_directory_text = cm_path_directory(normalized_root_path);
    root_directory = cm_graph_intern_c_str(state, root_directory_text);
    root_name = cm_graph_intern_c_str(state, "crate");
    root_path = root_name;
    root_unit->active = 1;
    state->defer_non_macro_use_modules =
        root_unit->plan.pending_invocation_count != 0u;
    {
        CmResolveItemRef root_declaration;

        memset(&root_declaration, 0, sizeof(root_declaration));
        result.root = cm_build_module(state, CM_MODULE_NONE, root_unit_id,
            root_name, root_path, root_directory, root_declaration, 0,
            (const CmAstItemId *)root_unit->ast.root_items.data,
            cm_count_u32(root_unit->ast.root_items.len),
            root_unit->plan.roots, root_unit->plan.root_count,
            root_unit->plan.crate_attributes,
            root_unit->plan.crate_attribute_count, 0,
            (CmSpan){ 0u, 0u, 0u });
    }
    if (state->errors.len == 0u && result.root != CM_MODULE_NONE)
        (void)cm_apply_macro_uses(state);
    if (state->errors.len == 0u && result.root != CM_MODULE_NONE)
        (void)cm_apply_macro_exports(state, result.root);
    staging_round = 0u;
    while (state->errors.len == 0u && result.root != CM_MODULE_NONE
            && cm_graph_has_pending_invocations(state)) {
        int root_had_pending;
        int spliced_include;

        if (staging_round >= CM_STAGING_MAX_ROUNDS) {
            cm_graph_add_error(state, CM_RESOLVE_ERROR_INCLUDE_LIMIT,
                root, 0u, 0u, root_path,
                cm_graph_intern_c_str(state,
                    "macro/include staging round limit exceeded"),
                CM_RESOLVE_STRING_NONE, 0u, 0u);
            break;
        }
        staging_round += 1u;
        root_unit = cm_get_unit(state, root_unit_id);
        root_had_pending = root_unit != NULL
            && root_unit->plan.pending_invocation_count != 0u;
        spliced_include = 0;
        if (cm_replan_staged_macro_invocations(state, &spliced_include)) {
            CmResolveItemRef root_declaration;

            cm_graph_reset_derived_resolution(state);
            if (spliced_include) {
                cm_graph_reset_unit_plans(state);
                if (!cm_prepare_unit_plan(state, root_unit_id, NULL, 0u)) {
                    result.root = CM_MODULE_NONE;
                    break;
                }
            }
            root_unit = cm_get_unit(state, root_unit_id);
            state->defer_non_macro_use_modules = spliced_include
                && root_unit != NULL
                && root_unit->plan.pending_invocation_count != 0u;
            root_directory = cm_graph_intern_c_str(state,
                root_directory_text);
            root_name = cm_graph_intern_c_str(state, "crate");
            root_path = root_name;
            if (root_unit != NULL) root_unit->active = 1;
            memset(&root_declaration, 0, sizeof(root_declaration));
            result.root = root_unit == NULL ? CM_MODULE_NONE
                : cm_build_module(state, CM_MODULE_NONE, root_unit_id,
                    root_name, root_path, root_directory, root_declaration,
                    0, (const CmAstItemId *)root_unit->ast.root_items.data,
                    cm_count_u32(root_unit->ast.root_items.len),
                    root_unit->plan.roots, root_unit->plan.root_count,
                    root_unit->plan.crate_attributes,
                    root_unit->plan.crate_attribute_count, 0,
                    (CmSpan){ 0u, 0u, 0u });
            if (state->errors.len == 0u
                && result.root != CM_MODULE_NONE)
                (void)cm_apply_macro_uses(state);
            if (state->errors.len == 0u
                && result.root != CM_MODULE_NONE)
                (void)cm_apply_macro_exports(state, result.root);
        } else if (root_had_pending) {
            cm_vec_clear(&state->macro_declarations);
            cm_vec_clear(&state->modules);
            result.root = CM_MODULE_NONE;
            break;
        } else {
            break;
        }
    }
    root_unit = cm_get_unit(state, root_unit_id);
    if (root_unit != NULL) root_unit->active = 0;
    result.error_count = state->errors.len;
    cm_graph_clear_borrowed_build_inputs(state);
    cm_free(root_directory_text);
    cm_free(normalized_root_path);
    return result;
}

size_t cm_module_graph_module_count(const CmModuleGraph *graph)
{
    const CmModuleGraphState *state;

    state = cm_graph_state_const(graph);
    return state == NULL ? 0u : state->modules.len;
}

size_t cm_module_graph_error_count(const CmModuleGraph *graph)
{
    const CmModuleGraphState *state;

    state = cm_graph_state_const(graph);
    return state == NULL ? 0u : state->errors.len;
}

uint64_t cm_module_graph_lifetime_id(const CmModuleGraph *graph)
{
    const CmModuleGraphState *state;

    state = cm_graph_state_const(graph);
    return state == NULL ? UINT64_C(0) : state->lifetime_id;
}

CmModuleGraphRevision cm_module_graph_revision(const CmModuleGraph *graph)
{
    const CmModuleGraphState *state;

    state = cm_graph_state_const(graph);
    return state == NULL ? CM_MODULE_GRAPH_REVISION_NONE : state->revision;
}

int cm_module_graph_get_module_at(const CmModuleGraph *graph, size_t index,
    CmResolveModuleInfo *out_module)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    if (out_module != NULL) memset(out_module, 0, sizeof(*out_module));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_module == NULL) return 0;
    node = (const CmResolveModuleNode *)cm_vec_at_const(&state->modules,
        index);
    if (node == NULL) return 0;
    *out_module = node->info;
    return 1;
}

int cm_module_graph_get_root(const CmModuleGraph *graph,
    CmModuleId *out_root)
{
    const CmModuleGraphState *state;
    size_t index;
    CmModuleId root;

    if (out_root != NULL) *out_root = CM_MODULE_NONE;
    state = cm_graph_state_const(graph);
    if (state == NULL || out_root == NULL) return 0;
    root = CM_MODULE_NONE;
    for (index = 0u; index < state->modules.len; ++index) {
        const CmResolveModuleNode *node;

        node = (const CmResolveModuleNode *)cm_vec_at_const(
            &state->modules, index);
        if (node == NULL || node->info.parent != CM_MODULE_NONE) continue;
        if (root != CM_MODULE_NONE) return 0;
        root = node->info.id;
    }
    if (root == CM_MODULE_NONE) return 0;
    *out_root = root;
    return 1;
}

int cm_module_graph_get_module(const CmModuleGraph *graph, CmModuleId id,
    CmResolveModuleInfo *out_module)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    state = cm_graph_state_const(graph);
    if (state == NULL || out_module == NULL) return 0;
    node = cm_get_module_node_const(state, id);
    if (node == NULL) return 0;
    *out_module = node->info;
    return 1;
}

int cm_module_graph_get_child(const CmModuleGraph *graph, CmModuleId module,
    uint32_t index, CmModuleId *out_child)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    state = cm_graph_state_const(graph);
    if (state == NULL || out_child == NULL) return 0;
    node = cm_get_module_node_const(state, module);
    if (node == NULL || index >= node->info.child_count) return 0;
    *out_child = node->children[index];
    return 1;
}

int cm_module_graph_get_namespace_entry(const CmModuleGraph *graph,
    CmModuleId module, CmResolveNamespace namespace_kind, uint32_t index,
    CmResolveNamespaceEntry *out_entry)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;
    const CmResolveNamespaceEntry *entries;
    uint32_t count;

    if (out_entry != NULL) memset(out_entry, 0, sizeof(*out_entry));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_entry == NULL) return 0;
    if (state->errors.len != 0u) return 0;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return 0;
    if (namespace_kind == CM_RESOLVE_NAMESPACE_TYPE) {
        entries = node->type_entries;
        count = node->info.type_count;
    } else if (namespace_kind == CM_RESOLVE_NAMESPACE_VALUE) {
        entries = node->value_entries;
        count = node->info.value_count;
    } else if (namespace_kind == CM_RESOLVE_NAMESPACE_MACRO) {
        entries = node->macro_entries;
        count = node->info.macro_count;
    } else {
        return 0;
    }
    if (index >= count) return 0;
    *out_entry = entries[index];
    return 1;
}

int cm_module_graph_get_macro_scope_entry(const CmModuleGraph *graph,
    CmModuleId module, uint32_t index, CmResolveMacroScopeEntry *out_entry)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    if (out_entry != NULL) memset(out_entry, 0, sizeof(*out_entry));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_entry == NULL || state->errors.len != 0u)
        return 0;
    node = cm_get_module_node_const(state, module);
    if (node == NULL || index >= node->info.macro_scope_count) return 0;
    *out_entry = node->macro_scope_entries[index];
    return 1;
}

int cm_module_graph_get_import(const CmModuleGraph *graph, CmModuleId module,
    uint32_t index, CmResolveImport *out_import)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    if (out_import != NULL) memset(out_import, 0, sizeof(*out_import));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_import == NULL) return 0;
    if (state->errors.len != 0u) return 0;
    node = cm_get_module_node_const(state, module);
    if (node == NULL || index >= node->info.import_count) return 0;
    *out_import = node->imports[index];
    return 1;
}

int cm_module_graph_get_error(const CmModuleGraph *graph, uint32_t index,
    CmResolveError *out_error)
{
    const CmModuleGraphState *state;
    const CmResolveError *error;

    state = cm_graph_state_const(graph);
    if (state == NULL || out_error == NULL || index >= state->errors.len)
        return 0;
    error = (const CmResolveError *)cm_vec_at_const(&state->errors, index);
    if (error == NULL) return 0;
    *out_error = *error;
    return 1;
}

CmAst *cm_module_graph_borrow_ast_mut(CmModuleGraph *graph, CmModuleId module)
{
    CmModuleGraphState *state;
    CmResolveModuleNode *node;
    CmResolveUnit *unit;

    state = cm_graph_state(graph);
    if (state == NULL) return NULL;
    node = cm_get_module_node(state, module);
    if (node == NULL) return NULL;
    unit = cm_get_unit(state, node->unit);
    if (unit == NULL) return NULL;
    return &unit->ast;
}

int cm_module_graph_borrow_ast(const CmModuleGraph *graph, CmModuleId module,
    const CmAst **out_ast)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;
    const CmResolveUnit *unit;

    if (out_ast != NULL) *out_ast = NULL;
    state = cm_graph_state_const(graph);
    if (state == NULL || out_ast == NULL) return 0;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return 0;
    unit = cm_get_unit_const(state, node->unit);
    if (unit == NULL) return 0;
    *out_ast = &unit->ast;
    return 1;
}

int cm_module_graph_borrow_item_ast(const CmModuleGraph *graph,
    CmModuleId module, CmResolveItemRef declaration,
    const CmAst **out_ast)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;
    const CmResolveUnit *unit;

    if (out_ast != NULL) *out_ast = NULL;
    state = cm_graph_state_const(graph);
    if (state == NULL || out_ast == NULL || declaration.source == 0u
        || declaration.item == CM_AST_ITEM_NONE) {
        return 0;
    }
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return 0;
    unit = cm_get_unit_const(state, node->unit);
    if (unit == NULL
        || cm_unit_item_source(unit, declaration.item) != declaration.source
        || cm_ast_get_item(&unit->ast, declaration.item) == NULL) {
        return 0;
    }
    *out_ast = &unit->ast;
    return 1;
}

int cm_module_graph_borrow_items(const CmModuleGraph *graph,
    CmModuleId module, const CmAstItemId **out_items, uint32_t *out_count)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    if (out_items != NULL) *out_items = NULL;
    if (out_count != NULL) *out_count = 0u;
    state = cm_graph_state_const(graph);
    if (state == NULL || out_items == NULL || out_count == NULL) return 0;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return 0;
    *out_items = node->items;
    *out_count = node->item_count;
    return 1;
}

int cm_module_graph_borrow_active_items(const CmModuleGraph *graph,
    CmModuleId module, const CmResolveItemRef **out_items,
    uint32_t *out_count)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    if (out_items != NULL) *out_items = NULL;
    if (out_count != NULL) *out_count = 0u;
    state = cm_graph_state_const(graph);
    if (state == NULL || out_items == NULL || out_count == NULL) return 0;
    if (state->errors.len != 0u) return 0;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return 0;
    *out_items = node->active_items;
    *out_count = node->info.active_item_count;
    return 1;
}

static const CmResolveEffectiveItemRecord *cm_find_effective_item_record(
    const CmResolveEffectiveItemRecord *items, uint32_t count,
    CmResolveEffectiveItemId id)
{
    uint32_t index;

    if (items == NULL || id == CM_RESOLVE_EFFECTIVE_ITEM_NONE) return NULL;
    for (index = 0u; index < count; ++index) {
        const CmResolveEffectiveItemRecord *found;

        if (items[index].item.id == id) return &items[index];
        found = cm_find_effective_item_record(items[index].children,
            items[index].item.child_count, id);
        if (found != NULL) return found;
    }
    return NULL;
}

CmResolveViewStatus cm_module_graph_get_effective_item(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, uint32_t index, CmResolveEffectiveItem *out_item)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    if (out_item != NULL) memset(out_item, 0, sizeof(*out_item));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_item == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return CM_RESOLVE_VIEW_INVALID_MODULE;
    if (index >= node->info.effective_item_count)
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    *out_item = node->effective_items[index].item;
    return CM_RESOLVE_VIEW_OK;
}

CmResolveViewStatus cm_module_graph_get_effective_child(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveEffectiveItemId parent,
    uint32_t child_index, CmResolveEffectiveItem *out_item)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;
    const CmResolveEffectiveItemRecord *parent_record;

    if (out_item != NULL) memset(out_item, 0, sizeof(*out_item));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_item == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE
        || parent == CM_RESOLVE_EFFECTIVE_ITEM_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return CM_RESOLVE_VIEW_INVALID_MODULE;
    parent_record = cm_find_effective_item_record(node->effective_items,
        node->info.effective_item_count, parent);
    if (parent_record == NULL
        || child_index >= parent_record->item.child_count) {
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    }
    *out_item = parent_record->children[child_index].item;
    return CM_RESOLVE_VIEW_OK;
}

CmResolveViewStatus cm_module_graph_get_effective_variant(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveEffectiveItemId enumeration,
    uint32_t variant_index, CmResolveEffectiveVariant *out_variant)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;
    const CmResolveEffectiveItemRecord *enumeration_record;

    if (out_variant != NULL) memset(out_variant, 0, sizeof(*out_variant));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_variant == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE
        || enumeration == CM_RESOLVE_EFFECTIVE_ITEM_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return CM_RESOLVE_VIEW_INVALID_MODULE;
    enumeration_record = cm_find_effective_item_record(
        node->effective_items, node->info.effective_item_count, enumeration);
    if (enumeration_record == NULL
        || enumeration_record->item.item_kind != CM_AST_ITEM_ENUM
        || variant_index >= enumeration_record->item.variant_count) {
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    }
    *out_variant = enumeration_record->variants[variant_index].variant;
    return CM_RESOLVE_VIEW_OK;
}

CmResolveViewStatus cm_module_graph_get_effective_variant_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveEffectiveItemId enumeration,
    uint32_t variant_index, uint32_t attribute_index,
    CmResolveEffectiveAttribute *out_attribute)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;
    const CmResolveEffectiveItemRecord *enumeration_record;
    const CmResolveEffectiveVariantRecord *variant;

    if (out_attribute != NULL)
        memset(out_attribute, 0, sizeof(*out_attribute));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_attribute == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE
        || enumeration == CM_RESOLVE_EFFECTIVE_ITEM_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return CM_RESOLVE_VIEW_INVALID_MODULE;
    enumeration_record = cm_find_effective_item_record(
        node->effective_items, node->info.effective_item_count, enumeration);
    if (enumeration_record == NULL
        || enumeration_record->item.item_kind != CM_AST_ITEM_ENUM
        || variant_index >= enumeration_record->item.variant_count) {
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    }
    variant = &enumeration_record->variants[variant_index];
    if (attribute_index >= variant->variant.attribute_count)
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    *out_attribute = variant->attributes[attribute_index];
    return CM_RESOLVE_VIEW_OK;
}

CmResolveViewStatus cm_module_graph_get_effective_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, uint32_t item_index, uint32_t attribute_index,
    CmResolveEffectiveAttribute *out_attribute)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;
    CmResolveEffectiveItem root_item;
    CmResolveViewStatus status;

    if (out_attribute != NULL)
        memset(out_attribute, 0, sizeof(*out_attribute));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_attribute == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return CM_RESOLVE_VIEW_INVALID_MODULE;
    if (item_index >= node->info.effective_item_count)
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    root_item = node->effective_items[item_index].item;
    status = cm_module_graph_get_effective_item_attribute(graph, revision,
        module, root_item.id, attribute_index, out_attribute);
    return status;
}

CmResolveViewStatus cm_module_graph_get_effective_item_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveEffectiveItemId item,
    uint32_t attribute_index, CmResolveEffectiveAttribute *out_attribute)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;
    const CmResolveEffectiveItemRecord *record;

    if (out_attribute != NULL)
        memset(out_attribute, 0, sizeof(*out_attribute));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_attribute == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE
        || item == CM_RESOLVE_EFFECTIVE_ITEM_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return CM_RESOLVE_VIEW_INVALID_MODULE;
    record = cm_find_effective_item_record(node->effective_items,
        node->info.effective_item_count, item);
    if (record == NULL || attribute_index >= record->item.attribute_count)
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    *out_attribute = record->attributes[attribute_index];
    return CM_RESOLVE_VIEW_OK;
}

CmResolveViewStatus cm_module_graph_get_effective_inner_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, uint32_t attribute_index,
    CmResolveEffectiveAttribute *out_attribute)
{
    const CmModuleGraphState *state;
    const CmResolveModuleNode *node;

    if (out_attribute != NULL)
        memset(out_attribute, 0, sizeof(*out_attribute));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_attribute == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    node = cm_get_module_node_const(state, module);
    if (node == NULL) return CM_RESOLVE_VIEW_INVALID_MODULE;
    if (attribute_index >= node->info.inner_attribute_count)
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    *out_attribute = node->inner_attributes[attribute_index];
    return CM_RESOLVE_VIEW_OK;
}

CmResolveViewStatus cm_module_graph_get_macro_declaration(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmResolveItemRef declaration, CmResolveMacroDeclaration *out_declaration)
{
    const CmModuleGraphState *state;
    const CmResolveMacroDeclarationRecord *record;

    if (out_declaration != NULL)
        memset(out_declaration, 0, sizeof(*out_declaration));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_declaration == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE
        || declaration.source == 0u
        || declaration.item == CM_AST_ITEM_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    record = cm_graph_find_macro_declaration_record(state, declaration);
    if (record == NULL) return CM_RESOLVE_VIEW_NOT_FOUND;
    *out_declaration = record->declaration;
    return CM_RESOLVE_VIEW_OK;
}

CmResolveViewStatus cm_module_graph_get_macro_declaration_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmResolveItemRef declaration, uint32_t attribute_index,
    CmResolveEffectiveAttribute *out_attribute)
{
    const CmModuleGraphState *state;
    const CmResolveMacroDeclarationRecord *record;

    if (out_attribute != NULL)
        memset(out_attribute, 0, sizeof(*out_attribute));
    state = cm_graph_state_const(graph);
    if (state == NULL || out_attribute == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE
        || declaration.source == 0u
        || declaration.item == CM_AST_ITEM_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    record = cm_graph_find_macro_declaration_record(state, declaration);
    if (record == NULL) return CM_RESOLVE_VIEW_NOT_FOUND;
    if (attribute_index >= record->declaration.attribute_count)
        return CM_RESOLVE_VIEW_OUT_OF_RANGE;
    *out_attribute = record->attributes[attribute_index];
    return CM_RESOLVE_VIEW_OK;
}

CmResolveViewStatus
cm_module_graph_validate_dependency_macro_definition(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmResolveDependencyItemRef reference)
{
    const CmModuleGraphState *state;
    size_t index;

    state = cm_graph_state_const(graph);
    if (state == NULL || revision == CM_MODULE_GRAPH_REVISION_NONE
        || reference.consumer_graph == NULL
        || reference.consumer_revision == CM_MODULE_GRAPH_REVISION_NONE
        || reference.certificate
            == CM_RESOLVE_DEPENDENCY_CERTIFICATE_NONE
        || reference.dependency == CM_RESOLVE_DEPENDENCY_NONE
        || reference.dependency_revision == CM_MODULE_GRAPH_REVISION_NONE
        || reference.declaration.source == 0u
        || reference.declaration.item == CM_AST_ITEM_NONE) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    if (reference.consumer_graph != graph
        || reference.consumer_revision != revision) {
        return CM_RESOLVE_VIEW_NOT_FOUND;
    }
    for (index = 0u; index < state->external_macros.len; ++index) {
        const CmResolveExternalMacro *external;

        external = (const CmResolveExternalMacro *)cm_vec_at_const(
            &state->external_macros, index);
        if (external != NULL
            && external->published
            && external->certificate == reference.certificate
            && external->dependency == reference.dependency
            && external->revision == reference.dependency_revision
            && external->declaration.source == reference.declaration.source
            && external->declaration.item == reference.declaration.item) {
            return CM_RESOLVE_VIEW_OK;
        }
    }
    return CM_RESOLVE_VIEW_NOT_FOUND;
}

CmResolveViewStatus cm_module_graph_validate_dependency_macro_import(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveItemRef import_declaration,
    const unsigned char *local_name, size_t local_name_length)
{
    const CmModuleGraphState *state;
    size_t index;

    state = cm_graph_state_const(graph);
    if (state == NULL || revision == CM_MODULE_GRAPH_REVISION_NONE
        || module == CM_MODULE_NONE || import_declaration.source == 0u
        || import_declaration.item == CM_AST_ITEM_NONE
        || local_name == NULL || local_name_length == 0u) {
        return CM_RESOLVE_VIEW_INVALID_ARGUMENT;
    }
    if (revision != state->revision) return CM_RESOLVE_VIEW_STALE_REVISION;
    if (state->errors.len != 0u) return CM_RESOLVE_VIEW_FAILED_BUILD;
    if (cm_get_module_node_const(state, module) == NULL)
        return CM_RESOLVE_VIEW_INVALID_MODULE;
    for (index = 0u; index < state->external_macro_imports.len; ++index) {
        const CmResolveExternalMacroImport *import_record;
        const CmResolveExternalMacro *external;
        CmResolveUnitId unit_id;
        const CmResolveUnit *unit;
        const CmAstItem *invocation;
        const CmAstPath *path;
        const CmAstPathSegment *segment;
        const CmInternedString *name;
        size_t macro_index;

        import_record = (const CmResolveExternalMacroImport *)
            cm_vec_at_const(&state->external_macro_imports, index);
        if (import_record == NULL || import_record->module != module
            || !cm_resolve_item_ref_equal(
                import_record->import_declaration, import_declaration)) {
            continue;
        }
        external = NULL;
        for (macro_index = 0u; macro_index < state->external_macros.len;
                ++macro_index) {
            const CmResolveExternalMacro *candidate;

            candidate = (const CmResolveExternalMacro *)cm_vec_at_const(
                &state->external_macros, macro_index);
            if (candidate != NULL && candidate->published
                && candidate->owner == import_record->owner
                && candidate->declaration.item
                    == import_record->definition) {
                external = candidate;
                break;
            }
        }
        if (external == NULL) continue;
        unit_id = cm_find_unit(state, import_record->invocation.source);
        unit = cm_get_unit_const(state, unit_id);
        invocation = unit == NULL ? NULL : cm_ast_get_item(&unit->ast,
            import_record->invocation.item);
        path = invocation == NULL || invocation->kind != CM_AST_ITEM_MACRO
                || invocation->data.macro_item.form
                    != CM_AST_MACRO_INVOCATION
            ? NULL : cm_ast_get_path(&unit->ast,
                invocation->data.macro_item.path);
        segment = path == NULL || path->segment_count != 1u
            ? NULL : &path->segments[0];
        name = segment == NULL ? NULL
            : cm_ast_get_string(&unit->ast, segment->name);
        if (name != NULL && name->len == local_name_length
            && memcmp(name->bytes, local_name, local_name_length) == 0) {
            return CM_RESOLVE_VIEW_OK;
        }
    }
    return CM_RESOLVE_VIEW_NOT_FOUND;
}

size_t cm_module_graph_string_length(const CmModuleGraph *graph,
    CmResolveStringId id)
{
    const CmModuleGraphState *state;
    const CmInternedString *string;

    state = cm_graph_state_const(graph);
    if (state == NULL) return 0u;
    string = cm_graph_string(state, id);
    return string == NULL ? 0u : string->len;
}

int cm_module_graph_copy_string(const CmModuleGraph *graph,
    CmResolveStringId id, char *buffer, size_t buffer_size)
{
    const CmModuleGraphState *state;
    const CmInternedString *string;

    state = cm_graph_state_const(graph);
    if (state == NULL || buffer == NULL) return 0;
    string = cm_graph_string(state, id);
    if (string == NULL || buffer_size <= string->len) return 0;
    memcpy(buffer, string->bytes, string->len);
    buffer[string->len] = 0;
    return 1;
}

const char *cm_resolve_error_kind_name(CmResolveErrorKind kind)
{
    switch (kind) {
    case CM_RESOLVE_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case CM_RESOLVE_ERROR_PARSE: return "parse error";
    case CM_RESOLVE_ERROR_SOURCE_IO: return "source I/O error";
    case CM_RESOLVE_ERROR_MISSING_MODULE_FILE: return "missing module file";
    case CM_RESOLVE_ERROR_AMBIGUOUS_MODULE_FILE:
        return "ambiguous module file";
    case CM_RESOLVE_ERROR_DUPLICATE_MODULE_PATH:
        return "duplicate module path";
    case CM_RESOLVE_ERROR_MODULE_CYCLE: return "module cycle";
    case CM_RESOLVE_ERROR_CFG_UNKNOWN: return "unknown cfg status";
    case CM_RESOLVE_ERROR_REVISION_EXHAUSTED:
        return "module graph revision exhausted";
    case CM_RESOLVE_ERROR_CFG_EXPANSION: return "cfg expansion failed";
    case CM_RESOLVE_ERROR_ITEM_MACRO: return "item macro planning failed";
    case CM_RESOLVE_ERROR_UNSUPPORTED_GENERATED_ITEM:
        return "unsupported generated item";
    case CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED:
        return "unsupported include";
    case CM_RESOLVE_ERROR_INCLUDE_CYCLE: return "include cycle";
    case CM_RESOLVE_ERROR_INCLUDE_LIMIT: return "include limit exceeded";
    }
    return "unknown resolver error";
}

const char *cm_resolve_view_status_name(CmResolveViewStatus status)
{
    switch (status) {
    case CM_RESOLVE_VIEW_OK: return "ok";
    case CM_RESOLVE_VIEW_INVALID_ARGUMENT: return "invalid argument";
    case CM_RESOLVE_VIEW_STALE_REVISION: return "stale revision";
    case CM_RESOLVE_VIEW_FAILED_BUILD: return "failed graph build";
    case CM_RESOLVE_VIEW_INVALID_MODULE: return "invalid module";
    case CM_RESOLVE_VIEW_OUT_OF_RANGE: return "out of range";
    case CM_RESOLVE_VIEW_NOT_FOUND: return "not found";
    }
    return "unknown view status";
}
