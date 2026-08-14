#include "cm/hir/lower.h"

#include "cm/hir/type_alias.h"

#include "cm/syntax/parser.h"

#include "cm/alloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The bootstrap headers implement vsnprintf without declaring it here. */
extern int vsnprintf(char *buffer, size_t size, const char *format,
    va_list arguments);

#define CM_LOWER_APIT_MAX_DEPTH ((size_t)1024u)

typedef enum CmLowerParentKind {
    CM_LOWER_PARENT_NONE = 0,
    CM_LOWER_PARENT_TRAIT,
    CM_LOWER_PARENT_IMPL
} CmLowerParentKind;

typedef enum CmLowerPhase {
    CM_LOWER_PHASE_TRAIT_HEADER = 0,
    CM_LOWER_PHASE_TRAIT_ASSOCIATED_TYPES,
    CM_LOWER_PHASE_TRAIT_ASSOCIATED_CONSTS,
    CM_LOWER_PHASE_TRAIT_METHODS,
    CM_LOWER_PHASE_IMPL_HEADER,
    CM_LOWER_PHASE_OTHER_ROOT,
    CM_LOWER_PHASE_IMPL_ASSOCIATED_TYPES,
    CM_LOWER_PHASE_IMPL_ASSOCIATED_CONSTS,
    CM_LOWER_PHASE_IMPL_METHODS,
    CM_LOWER_PHASE_COUNT
} CmLowerPhase;

typedef struct CmLowerItemRecord {
    const CmAst *ast;
    CmSourceId source;
    CmModuleId graph_module;
    CmAstItemId ast_id;
    CmHirModuleId owner_module;
    CmHirModuleId nested_module;
    CmHirDefId definition;
    CmHirDefId parent_definition;
    CmLowerParentKind parent_kind;
    CmAstItemKind kind;
    CmInternId hir_name;
    CmResolveItemProvenance provenance;
    CmSpan effective_span;
    CmResolveEffectiveItemId graph_effective_item;
    uint32_t effective_attribute_count;
    CmHirGenericParamId generic_parameter_start;
    uint32_t generic_parameter_count;
    int is_generated;
    int is_foreign;
    CmInternId inherited_abi;
    const CmExpandedItem *expanded_item;
} CmLowerItemRecord;

typedef struct CmLowerTraitTarget {
    CmHirDefId definition;
    CmHirGenericParamId generic_parameter_start;
    uint32_t generic_parameter_count;
    const CmHirItem *item;
    const CmLowerItemRecord *local_record;
} CmLowerTraitTarget;

typedef struct CmLowerAssociatedTarget {
    CmHirDefId definition;
    /* Trait that directly declares this associated item.  This may differ
     * from the trait through which it was found when resolving shorthand
     * projections such as `P::Target` across supertraits. */
    CmHirDefId trait_definition;
    uint32_t generic_parameter_count;
    const CmHirItem *item;
    const CmLowerItemRecord *local_record;
} CmLowerAssociatedTarget;

typedef struct CmLowerGenericRecord {
    CmHirDefId owner;
    CmInternId ast_name;
    CmHirGenericParamId hir_id;
    CmHirGenericParamKind kind;
} CmLowerGenericRecord;

typedef struct CmLowerApitRecord {
    const CmAst *ast;
    CmHirDefId owner;
    CmAstTypeId ast_type;
    CmHirGenericParamId hir_id;
} CmLowerApitRecord;

typedef struct CmLowerVariantRecord {
    CmHirDefId enumeration;
    CmHirDefId definition;
    uint32_t source_index;
} CmLowerVariantRecord;

typedef struct CmLowerMacroRecord {
    CmResolveItemRef declaration;
    CmHirDefId definition;
} CmLowerMacroRecord;

typedef struct CmLowerState {
    CmHirContext *hir;
    const CmModuleGraph *graph;
    const CmImportResolver *imports;
    const CmHirModuleMap *module_map;
    const CmAst *ast;
    CmSourceId source;
    CmModuleId graph_module;
    CmModuleGraphRevision graph_revision;
    const CmHirLowerOptions *options;
    CmHirLowerResult result;
    CmVec item_records;
    CmVec variant_records;
    CmVec macro_records;
    CmVec generic_records;
    CmVec apit_records;
    CmVec expanded_source_ids;
    CmVec expected_module_bindings;
    const CmHirItem *active_item;
    const CmAstLifetimeBinder *active_lifetime_binder;
    uint32_t next_type_inference;
    uint32_t next_region_inference;
    CmSpan generated_span;
    int use_generated_span;
    int authenticated_external_import_errors_only;
    const CmHirItem *active_predicate_item;
    int failed;
} CmLowerState;

static const CmHirItem *cm_lower_bound_item(const CmLowerState *state,
    CmHirDefId definition_id);
static void cm_lower_find_associated_type(
    const CmLowerState *state, CmHirDefId trait_definition,
    CmInternId ast_name, CmLowerAssociatedTarget *out_target,
    uint32_t *out_matches);
static void cm_lower_find_inherited_associated_type(
    const CmLowerState *state, CmHirDefId trait_definition,
    CmInternId ast_name, CmLowerAssociatedTarget *out_target,
    uint32_t *out_matches);
static int cm_lower_ast_path_storage_valid(const CmAstPath *path);
static int cm_lower_attribute_has_head(const CmLowerState *state,
    CmAstAttributeId attribute_id, const char *expected);
static int cm_lower_record_self_type_matches(
    const CmLowerState *state, const CmLowerItemRecord *impl_record,
    const CmLowerItemRecord *type_record);
static const CmLowerItemRecord *cm_lower_associated_const_length(
    const CmLowerState *state, CmHirDefId owner,
    const CmInternedString *text);
static int cm_lower_validate_impl_trait_type(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId ast_type_id,
    const CmAstType *ast_type, int *out_relaxed_sized);
static int cm_lower_lifetime_binder_is_valid(
    const CmAstLifetimeBinder *binder, CmAstSpan bound_span,
    const CmAstType *trait_type);
static int cm_lower_item_trait_predicates(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item);
static int cm_lower_one_trait_predicate(CmLowerState *state,
    CmAstItemId ast_item_id, const CmLowerItemRecord *record,
    CmHirTypeId subject, CmAstTypeId trait_ast_type, CmSpan predicate_span,
    const CmAstLifetimeBinder *ast_binder,
    CmHirTraitPredicateModifier modifier,
    CmHirTraitPredicate *out_predicate);
static int cm_lower_trait_identity_arguments(CmLowerState *state,
    CmAstItemId ast_item_id, const CmHirItem *trait_item, CmSpan span,
    CmHirGenericArg **out_arguments, uint32_t *out_count);
static int cm_lower_trait_positional_arguments(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstPathSegment *segment,
    const CmLowerTraitTarget *trait_target, CmHirModuleId module,
    CmHirDefId owner, CmHirTypeId default_self, int allow_bindings,
    int allow_constraints, int allow_synthesized_default_self, CmSpan span,
    CmHirGenericArg **out_arguments, uint32_t *out_count);
static int cm_lower_predicate_equalities(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId trait_ast_type,
    const CmLowerTraitTarget *trait_target, CmHirModuleId module,
    CmHirDefId owner, int allow_constraints,
    CmHirAssociatedTypeEquality **out_equalities, uint32_t *out_count);
static int cm_lower_find_instantiated_supertrait(CmLowerState *state,
    const CmHirNamedType *root, CmHirDefId target_definition,
    CmHirTypeId self_type, CmSpan span, CmHirGenericArg **out_arguments,
    uint32_t *out_argument_count, uint32_t *out_matches);
static CmHirTypeId cm_lower_dyn_trait_type(CmLowerState *state,
    CmAstTypeId ast_type_id, const CmAstType *ast_type,
    CmHirModuleId module, CmHirDefId owner);
static CmInternId cm_lower_copy_graph_attribute_metadata(
    CmLowerState *state, const CmModuleGraph *graph, CmResolveStringId id,
    CmSpan span, CmAstItemId declaration);

typedef enum CmLowerLookupResult {
    CM_LOWER_LOOKUP_NOT_FOUND = 0,
    CM_LOWER_LOOKUP_DEFINITION,
    CM_LOWER_LOOKUP_MODULE,
    CM_LOWER_LOOKUP_ALIAS,
    CM_LOWER_LOOKUP_TRAIT,
    CM_LOWER_LOOKUP_WRONG_NAMESPACE,
    CM_LOWER_LOOKUP_RESOLVER_ERROR,
    CM_LOWER_LOOKUP_STALE_GRAPH
} CmLowerLookupResult;

static CmLowerLookupResult cm_lower_lookup_trait_target(
    CmLowerState *state, const CmAstPath *path, CmHirModuleId module,
    CmLowerTraitTarget *out_target);

static CmSpan cm_lower_span(const CmLowerState *state, CmAstSpan ast_span)
{
    CmSpan span;

    if (state->use_generated_span) {
        return state->generated_span;
    }
    span.source = state->source;
    span.start = ast_span.start;
    span.end = ast_span.end;
    return span;
}

static int cm_lower_resolve_library_import(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmImportResolver *imports, const CmHirLowerOptions *options,
    CmModuleId module, CmResolveItemRef import_declaration,
    const unsigned char *name, size_t name_length,
    CmHirLibraryBinding *out_binding)
{
    CmHirLibraryPathSegment local_name;
    size_t index;
    size_t matches;
    int failed;

    if (out_binding != NULL) memset(out_binding, 0, sizeof(*out_binding));
    if (graph == NULL || imports == NULL || options == NULL
        || module == CM_MODULE_NONE
        || ((import_declaration.source == 0u)
            != (import_declaration.item == CM_AST_ITEM_NONE))
        || name == NULL
        || name_length == 0u) return 0;
    local_name.bytes = name;
    local_name.length = name_length;
    matches = 0u;
    failed = 0;
    for (index = 0u; index < options->dependency_library_count; ++index) {
        CmHirLibraryImport imported;
        CmHirLibraryStatus status;

        memset(&imported, 0, sizeof(imported));
        status = cm_hir_library_artifact_resolve_import(
            options->dependency_libraries[index], imports, graph, revision,
            module, &local_name, &imported);
        if (status == CM_HIR_LIBRARY_OK) {
            if (imported.consumer_module != module
                || (import_declaration.source != 0u
                    && (imported.import_declaration.source
                            != import_declaration.source
                        || imported.import_declaration.item
                            != import_declaration.item))) {
                failed = 1;
                continue;
            }
            if (out_binding != NULL) *out_binding = imported.binding;
            matches += 1u;
        } else if (status != CM_HIR_LIBRARY_NOT_FOUND) {
            failed = 1;
        }
    }
    if (failed || matches != 1u) {
        if (out_binding != NULL)
            memset(out_binding, 0, sizeof(*out_binding));
        return 0;
    }
    return 1;
}

static int cm_lower_import_errors_are_authenticated(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmImportResolver *imports, const CmHirLowerOptions *options)
{
    size_t error_count;
    size_t index;

    error_count = cm_import_error_count(imports);
    if (error_count == 0u) return 0;
    if (error_count > (size_t)UINT32_MAX) return 0;
    for (index = 0u; index < error_count; ++index) {
        CmImportError error;
        size_t name_length;
        unsigned char *name;
        CmResolveViewStatus status;

        memset(&error, 0, sizeof(error));
        if (!cm_import_get_error(imports, (uint32_t)index, &error)
            || error.kind != CM_IMPORT_ERROR_UNRESOLVED
            || error.module == CM_MODULE_NONE
            || error.import_declaration.source == 0u
            || error.import_declaration.item == CM_AST_ITEM_NONE
            || error.name == CM_RESOLVE_STRING_NONE) return 0;
        name_length = cm_import_string_length(imports, error.name);
        if (name_length == 0u || name_length == SIZE_MAX) return 0;
        name = (unsigned char *)cm_alloc(name_length + 1u);
        if (!cm_import_copy_string(imports, error.name, (char *)name,
                name_length + 1u)) {
            cm_free(name);
            return 0;
        }
        status = cm_module_graph_validate_dependency_macro_import(graph,
            revision, error.module, error.import_declaration, name,
            name_length);
        if (status != CM_RESOLVE_VIEW_OK
            && !cm_lower_resolve_library_import(graph, revision, imports,
                options, error.module, error.import_declaration, name,
                name_length, NULL)) {
            cm_free(name);
            return 0;
        }
        cm_free(name);
    }
    return 1;
}

static int cm_lower_graph_snapshot_matches(const CmLowerState *state)
{
    if (state->graph == NULL) return 1;
    return state->imports != NULL
        && cm_module_graph_revision(state->graph) == state->graph_revision
        && cm_module_graph_error_count(state->graph) == 0u
        && cm_import_resolver_revision(state->imports)
            == state->graph_revision
        && cm_import_resolver_matches_graph(state->imports, state->graph)
        && (cm_import_error_count(state->imports) == 0u
            || state->authenticated_external_import_errors_only);
}

static int cm_lower_module_map_matches_expected(const CmLowerState *state)
{
    size_t index;

    if (state->graph == NULL || state->module_map == NULL
        || cm_hir_module_map_count(state->module_map)
            != state->expected_module_bindings.len) {
        return 0;
    }
    for (index = 0u; index < state->expected_module_bindings.len; ++index) {
        const CmHirModuleMapEntry *expected;
        CmHirModuleId hir_module;
        CmModuleId module;

        expected = (const CmHirModuleMapEntry *)cm_vec_at_const(
            &state->expected_module_bindings, index);
        if (expected == NULL
            || cm_hir_module_map_lookup_hir(state->module_map,
                state->graph, state->graph_revision, expected->module,
                state->hir, &hir_module) != CM_HIR_MODULE_MAP_OK
            || hir_module != expected->hir_module
            || cm_hir_module_map_lookup_module(state->module_map,
                state->graph, state->graph_revision, state->hir,
                expected->hir_module, &module) != CM_HIR_MODULE_MAP_OK
            || module != expected->module) {
            return 0;
        }
    }
    return 1;
}

static void cm_lower_fail(CmLowerState *state, CmHirLowerErrorKind kind,
    CmSpan span, CmAstItemId item, CmAstTypeId type, CmAstPathId path,
    CmHirStatus hir_status, const char *format, ...)
{
    va_list arguments;

    if (state->failed) {
        return;
    }
    state->failed = 1;
    state->result.error_count = 1u;
    state->result.first_error.kind = kind;
    state->result.first_error.span = span;
    state->result.first_error.item = item;
    state->result.first_error.type = type;
    state->result.first_error.path = path;
    state->result.first_error.hir_status = hir_status;
    va_start(arguments, format);
    (void)vsnprintf(state->result.first_error.message,
        sizeof(state->result.first_error.message), format, arguments);
    va_end(arguments);
}

static void cm_lower_fail_hir(CmLowerState *state, CmSpan span,
    CmAstItemId item, CmHirStatus status, const char *operation)
{
    cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span, item,
        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, status, "%s: %s", operation,
        cm_hir_status_name(status));
}

static const CmInternedString *cm_lower_ast_string(const CmLowerState *state,
    CmInternId id)
{
    return cm_ast_get_string(state->ast, id);
}

static int cm_lower_string_is(const CmLowerState *state, CmInternId id,
    const char *text)
{
    const CmInternedString *string;
    size_t length;

    string = cm_lower_ast_string(state, id);
    if (string == NULL) {
        return 0;
    }
    length = strlen(text);
    return string->len == length && memcmp(string->bytes, text, length) == 0;
}

static int cm_lower_strings_equal(const CmLowerState *state, CmInternId left,
    CmInternId right)
{
    const CmInternedString *left_string;
    const CmInternedString *right_string;

    left_string = cm_lower_ast_string(state, left);
    right_string = cm_lower_ast_string(state, right);
    return left_string != NULL && right_string != NULL
        && left_string->len == right_string->len
        && memcmp(left_string->bytes, right_string->bytes,
            left_string->len) == 0;
}

static int cm_lower_hir_name_matches_ast(const CmLowerState *state,
    CmInternId hir_name, CmInternId ast_name)
{
    const CmInternedString *hir_string;
    const CmInternedString *ast_string;

    hir_string = cm_interner_get(&state->hir->strings, hir_name);
    ast_string = cm_lower_ast_string(state, ast_name);
    return hir_string != NULL && ast_string != NULL
        && hir_string->len == ast_string->len
        && memcmp(hir_string->bytes, ast_string->bytes,
            hir_string->len) == 0;
}

static CmInternId cm_lower_copy_string(CmLowerState *state, CmInternId id,
    CmSpan span, CmAstItemId item)
{
    const CmInternedString *string;
    CmInternId result;

    string = cm_lower_ast_string(state, id);
    if (string == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, item,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "AST contains an invalid interned string ID");
        return CM_INTERN_ID_NONE;
    }
    result = cm_interner_intern(&state->hir->strings, string->bytes,
        string->len);
    return result;
}

static const CmLowerGenericRecord *cm_lower_find_generic(
    const CmLowerState *state, CmHirDefId owner, CmInternId ast_name)
{
    size_t index;

    for (index = 0u; index < state->generic_records.len; ++index) {
        const CmLowerGenericRecord *record;

        record = (const CmLowerGenericRecord *)cm_vec_at_const(
            &state->generic_records, index);
        if (cm_hir_def_id_equal(record->owner, owner)
            && cm_lower_strings_equal(state, record->ast_name, ast_name)) {
            return record;
        }
    }
    return NULL;
}

static const CmLowerApitRecord *cm_lower_find_apit(
    const CmLowerState *state, const CmAst *ast, CmHirDefId owner,
    CmAstTypeId ast_type)
{
    size_t index;

    for (index = 0u; index < state->apit_records.len; ++index) {
        const CmLowerApitRecord *record;

        record = (const CmLowerApitRecord *)cm_vec_at_const(
            &state->apit_records, index);
        if (record != NULL && record->ast == ast
            && cm_hir_def_id_equal(record->owner, owner)
            && record->ast_type == ast_type) {
            return record;
        }
    }
    return NULL;
}

static const CmLowerItemRecord *cm_lower_find_record_by_definition(
    const CmLowerState *state, CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < state->item_records.len; ++index) {
        const CmLowerItemRecord *record;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if (record != NULL
            && cm_hir_def_id_equal(record->definition, definition)) {
            return record;
        }
    }
    return NULL;
}

static int cm_lower_parent_is_inherent_impl(
    const CmLowerState *state, CmLowerParentKind parent_kind,
    CmHirDefId parent_definition)
{
    const CmLowerItemRecord *record;
    const CmAstItem *item;

    if (parent_kind != CM_LOWER_PARENT_IMPL) return 0;
    record = cm_lower_find_record_by_definition(state, parent_definition);
    item = record == NULL ? NULL
        : cm_ast_get_item(record->ast, record->ast_id);
    return item != NULL && item->kind == CM_AST_ITEM_IMPL
        && item->data.impl_item.trait_type == CM_AST_TYPE_NONE;
}

static int cm_lower_parent_supports_generic_method(
    const CmLowerState *state, CmLowerParentKind parent_kind,
    CmHirDefId parent_definition)
{
    const CmLowerItemRecord *record;
    const CmAstItem *item;

    if (parent_kind == CM_LOWER_PARENT_TRAIT) return 1;
    if (parent_kind != CM_LOWER_PARENT_IMPL) return 0;
    record = cm_lower_find_record_by_definition(state, parent_definition);
    item = record == NULL ? NULL
        : cm_ast_get_item(record->ast, record->ast_id);
    return item != NULL && item->kind == CM_AST_ITEM_IMPL
        && !item->data.impl_item.is_negative;
}

static int cm_lower_parent_supports_generic_associated_type(
    const CmLowerState *state, CmLowerParentKind parent_kind,
    CmHirDefId parent_definition)
{
    const CmLowerItemRecord *record;
    const CmAstItem *item;

    if (parent_kind == CM_LOWER_PARENT_TRAIT) return 1;
    if (parent_kind != CM_LOWER_PARENT_IMPL) return 0;
    record = cm_lower_find_record_by_definition(state, parent_definition);
    item = record == NULL ? NULL
        : cm_ast_get_item(record->ast, record->ast_id);
    return item != NULL && item->kind == CM_AST_ITEM_IMPL
        && !item->data.impl_item.is_negative
        && item->data.impl_item.trait_type != CM_AST_TYPE_NONE;
}

static const CmLowerGenericRecord *cm_lower_find_generic_in_scope(
    const CmLowerState *state, CmHirDefId owner, CmInternId ast_name)
{
    const CmLowerGenericRecord *generic;
    const CmLowerItemRecord *record;

    generic = cm_lower_find_generic(state, owner, ast_name);
    if (generic != NULL) return generic;
    record = cm_lower_find_record_by_definition(state, owner);
    if (record == NULL
        || (record->kind != CM_AST_ITEM_FUNCTION
            && record->kind != CM_AST_ITEM_TYPE_ALIAS)
        || record->parent_kind == CM_LOWER_PARENT_NONE) {
        return NULL;
    }
    return cm_lower_find_generic(state, record->parent_definition,
        ast_name);
}

static int cm_lower_type_path_starts_with_parameter(
    const CmLowerState *state, const CmAstPath *path, CmHirDefId owner)
{
    const CmLowerGenericRecord *generic;

    if (path == NULL || path->absolute || path->segment_count == 0u
        || path->segments == NULL) {
        return 0;
    }
    generic = cm_lower_find_generic_in_scope(state, owner,
        path->segments[0].name);
    return generic != NULL && generic->kind == CM_HIR_GENERIC_TYPE;
}

static int cm_lower_ast_type_is_plain_sized_path(
    const CmLowerState *state, CmAstTypeId type_id)
{
    const CmAstType *type;
    const CmAstPath *path;

    type = cm_ast_get_type(state->ast, type_id);
    path = type == NULL || type->kind != CM_AST_TYPE_PATH ? NULL
        : cm_ast_get_path(state->ast, type->path);
    return path != NULL && !path->absolute && path->segment_count == 1u
        && path->segments != NULL
        && path->segments[0].argument_count == 0u
        && path->segments[0].arguments == NULL
        && cm_lower_string_is(state, path->segments[0].name, "Sized");
}

static int cm_lower_where_bound_relaxes_named_parameter(
    const CmLowerState *state, const CmAstWherePredicate *predicate,
    const CmAstWhereBound *bound, CmInternId parameter_name)
{
    const CmAstType *subject;
    const CmAstPath *path;

    if (predicate == NULL || bound == NULL
        || predicate->kind != CM_AST_WHERE_PREDICATE_TYPE
        || predicate->binder.lifetime_count != 0u
        || bound->kind != CM_AST_WHERE_BOUND_TRAIT
        || bound->modifier != CM_AST_WHERE_BOUND_RELAXED
        || bound->binder.lifetime_count != 0u
        || !cm_lower_ast_type_is_plain_sized_path(state,
            bound->trait_type)) {
        return 0;
    }
    subject = cm_ast_get_type(state->ast, predicate->subject);
    path = subject == NULL || subject->kind != CM_AST_TYPE_PATH ? NULL
        : cm_ast_get_path(state->ast, subject->path);
    return path != NULL && !path->absolute && path->segment_count == 1u
        && path->segments != NULL
        && path->segments[0].argument_count == 0u
        && path->segments[0].arguments == NULL
        && path->segments[0].name == parameter_name;
}

static int cm_lower_where_bound_relaxes_generic_parameter(
    const CmLowerState *state, const CmAstItem *item,
    const CmAstWherePredicate *predicate, const CmAstWhereBound *bound)
{
    uint32_t index;

    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmAstGenericParam *parameter;

        parameter = &item->generic_parameters[index];
        if (parameter->kind == CM_AST_PARAM_TYPE
            && cm_lower_where_bound_relaxes_named_parameter(state,
                predicate, bound, parameter->name)) {
            return 1;
        }
    }
    return 0;
}

static int cm_lower_item_kind_supported(CmAstItemKind kind)
{
    return kind == CM_AST_ITEM_FUNCTION || kind == CM_AST_ITEM_STRUCT
        || kind == CM_AST_ITEM_UNION
        || kind == CM_AST_ITEM_ENUM || kind == CM_AST_ITEM_TYPE_ALIAS
        || kind == CM_AST_ITEM_CONST || kind == CM_AST_ITEM_STATIC
        || kind == CM_AST_ITEM_MODULE || kind == CM_AST_ITEM_TRAIT
        || kind == CM_AST_ITEM_IMPL;
}

static int cm_lower_anonymous_const(const CmAst *ast,
    const CmAstItem *item)
{
    const CmInternedString *name;

    if (ast == NULL || item == NULL || item->kind != CM_AST_ITEM_CONST) {
        return 0;
    }
    name = cm_ast_get_string(ast, item->name);
    return name != NULL && name->len == 1u
        && name->bytes[0] == (unsigned char)'_';
}

static int cm_lower_item_has_where_clause(const CmAstItem *item)
{
    return item->where_clause != CM_INTERN_ID_NONE
        || (item->kind == CM_AST_ITEM_TYPE_ALIAS
            && item->data.value_item.post_value_where_clause
                != CM_INTERN_ID_NONE);
}

static int cm_lower_hir_item_kind(const CmAstItem *item,
    CmHirItemKind *out_kind)
{
    if (item == NULL || out_kind == NULL) return 0;
    switch (item->kind) {
    case CM_AST_ITEM_FUNCTION:
        *out_kind = CM_HIR_ITEM_FUNCTION;
        return 1;
    case CM_AST_ITEM_STRUCT:
        *out_kind = CM_HIR_ITEM_STRUCT;
        return 1;
    case CM_AST_ITEM_UNION:
        *out_kind = CM_HIR_ITEM_UNION;
        return 1;
    case CM_AST_ITEM_ENUM:
        *out_kind = CM_HIR_ITEM_ENUM;
        return 1;
    case CM_AST_ITEM_TYPE_ALIAS:
        *out_kind = CM_HIR_ITEM_TYPE_ALIAS;
        return 1;
    case CM_AST_ITEM_CONST:
        *out_kind = CM_HIR_ITEM_CONST;
        return 1;
    case CM_AST_ITEM_STATIC:
        *out_kind = CM_HIR_ITEM_STATIC;
        return 1;
    case CM_AST_ITEM_MODULE:
        *out_kind = CM_HIR_ITEM_MODULE;
        return 1;
    case CM_AST_ITEM_TRAIT:
        *out_kind = item->data.trait_item.is_alias
            ? CM_HIR_ITEM_TRAIT_ALIAS : CM_HIR_ITEM_TRAIT;
        return 1;
    case CM_AST_ITEM_IMPL:
        *out_kind = CM_HIR_ITEM_IMPL;
        return 1;
    default:
        break;
    }
    return 0;
}

static void cm_lower_source_children(const CmAstItem *item,
    const CmAstItemId **out_items, size_t *out_count,
    CmExpandedChildKind *out_kind)
{
    *out_items = NULL;
    *out_count = 0u;
    *out_kind = CM_EXPANDED_CHILD_NONE;
    switch (item->kind) {
    case CM_AST_ITEM_MODULE:
        *out_items = item->data.module_item.items;
        *out_count = (size_t)item->data.module_item.item_count;
        *out_kind = CM_EXPANDED_CHILD_MODULE;
        break;
    case CM_AST_ITEM_EXTERN_BLOCK:
        *out_items = item->data.extern_block_item.items;
        *out_count = (size_t)item->data.extern_block_item.item_count;
        *out_kind = CM_EXPANDED_CHILD_EXTERN_BLOCK;
        break;
    case CM_AST_ITEM_TRAIT:
        *out_items = item->data.trait_item.items;
        *out_count = (size_t)item->data.trait_item.item_count;
        *out_kind = CM_EXPANDED_CHILD_TRAIT;
        break;
    case CM_AST_ITEM_IMPL:
        *out_items = item->data.impl_item.items;
        *out_count = (size_t)item->data.impl_item.item_count;
        *out_kind = CM_EXPANDED_CHILD_IMPL;
        break;
    default:
        break;
    }
}

static int cm_lower_validate_item_where_predicates(
    CmLowerState *state, CmAstItemId item_id, const CmAstItem *item)
{
    CmSpan item_span;
    uint32_t predicate_index;

    item_span = cm_lower_span(state, item->span);
    if ((item->where_clause == CM_INTERN_ID_NONE)
            != (item->where_predicate_count == 0u)
        || (item->where_predicate_count != 0u
            && item->where_predicates == NULL)
        || (item->where_predicate_count == 0u
            && item->where_predicates != NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
            item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "where-clause provenance and predicate storage disagree");
        return 0;
    }
    for (predicate_index = 0u;
         predicate_index < item->where_predicate_count;
         ++predicate_index) {
        const CmAstWherePredicate *predicate;
        uint32_t bound_index;

        predicate = &item->where_predicates[predicate_index];
        if (predicate->span.start > predicate->span.end
            || predicate->bound_count == 0u
            || predicate->bounds == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                item_id, predicate->subject, CM_AST_PATH_NONE, CM_HIR_OK,
                "where predicate span or bound storage is invalid");
            return 0;
        }
        if (!cm_lower_lifetime_binder_is_valid(&predicate->binder,
                predicate->span,
                cm_ast_get_type(state->ast, predicate->subject))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, predicate->span), item_id,
                predicate->subject, CM_AST_PATH_NONE, CM_HIR_OK,
                "where-predicate lifetime binder storage is invalid");
            return 0;
        }
        if ((unsigned int)predicate->kind
                > (unsigned int)CM_AST_WHERE_PREDICATE_LIFETIME
            || (predicate->kind == CM_AST_WHERE_PREDICATE_TYPE
                && (predicate->subject == CM_AST_TYPE_NONE
                    || predicate->subject_lifetime != CM_INTERN_ID_NONE))
            || (predicate->kind == CM_AST_WHERE_PREDICATE_LIFETIME
                && (predicate->subject != CM_AST_TYPE_NONE
                    || predicate->subject_lifetime == CM_INTERN_ID_NONE
                    || cm_ast_get_string(state->ast,
                        predicate->subject_lifetime) == NULL))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                item_id, predicate->subject, CM_AST_PATH_NONE, CM_HIR_OK,
                "where predicate kind or subject is invalid");
            return 0;
        }
        for (bound_index = 0u; bound_index < predicate->bound_count;
             ++bound_index) {
            const CmAstWhereBound *bound;

            bound = &predicate->bounds[bound_index];
            if (predicate->binder.lifetime_count != 0u
                && bound->binder.lifetime_count != 0u) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, bound->span), item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "nested higher-ranked where binders require binder "
                    "depth in HIR");
                return 0;
            }
            if ((unsigned int)bound->modifier
                > (unsigned int)CM_AST_WHERE_BOUND_CONST) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, bound->span), item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "where-bound modifier is invalid");
                return 0;
            }
            if (bound->kind == CM_AST_WHERE_BOUND_LIFETIME) {
                if (!cm_lower_lifetime_binder_is_valid(&bound->binder,
                        bound->span, NULL)
                    || bound->binder.lifetime_count != 0u
                    || bound->modifier != CM_AST_WHERE_BOUND_REQUIRED
                    || bound->trait_type != CM_AST_TYPE_NONE
                    || bound->lifetime == CM_INTERN_ID_NONE
                    || cm_ast_get_string(state->ast, bound->lifetime)
                        == NULL) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                        cm_lower_span(state, bound->span), item_id,
                        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                        "lifetime where bound is malformed");
                    return 0;
                }
                continue;
            }
            if (bound->kind != CM_AST_WHERE_BOUND_TRAIT
                || bound->lifetime != CM_INTERN_ID_NONE
                || predicate->kind
                    == CM_AST_WHERE_PREDICATE_LIFETIME) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, bound->span), item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "where bound kind is invalid");
                return 0;
            }
            if (!cm_lower_lifetime_binder_is_valid(&bound->binder,
                    bound->span,
                    cm_ast_get_type(state->ast, bound->trait_type))) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, bound->span), item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "where-bound lifetime binder storage is invalid");
                return 0;
            }
        }
    }
    return 1;
}

static int cm_lower_preflight_source_lifetime_where_predicates(
    CmLowerState *state, const CmAstItemId *items, size_t item_count)
{
    size_t index;

    if (item_count != 0u && items == NULL) return 1;
    for (index = 0u; index < item_count; ++index) {
        const CmAstItem *item;
        const CmAstItemId *children;
        size_t child_count;
        CmExpandedChildKind child_kind;

        item = cm_ast_get_item(state->ast, items[index]);
        if (item == NULL) return 1;
        if (!cm_lower_validate_item_where_predicates(state,
                items[index], item)) {
            return 0;
        }
        cm_lower_source_children(item, &children, &child_count,
            &child_kind);
        (void)child_kind;
        if (!cm_lower_preflight_source_lifetime_where_predicates(state,
                children, child_count)) {
            return 0;
        }
    }
    return 1;
}

static int cm_lower_preflight_expanded_lifetime_where_predicates(
    CmLowerState *state, const CmExpandedItem *items, size_t item_count)
{
    size_t index;

    for (index = 0u; index < item_count; ++index) {
        const CmExpandedItem *expanded_item;
        const CmAstItem *item;

        expanded_item = &items[index];
        item = cm_ast_get_item(state->ast, expanded_item->source_id);
        if (item == NULL) return 1;
        if (!cm_lower_validate_item_where_predicates(state,
                expanded_item->source_id, item)
            || !cm_lower_preflight_expanded_lifetime_where_predicates(state,
                expanded_item->children, expanded_item->child_count)) {
            return 0;
        }
    }
    return 1;
}

static int cm_lower_attribute_id_in_list(CmAstAttributeId id,
    const CmAstAttributeId *attributes, size_t attribute_count)
{
    size_t index;

    for (index = 0u; index < attribute_count; ++index) {
        if (attributes[index] == id) {
            return 1;
        }
    }
    return 0;
}

static int cm_lower_validate_effective_attributes(CmLowerState *state,
    const CmEffectiveAttribute *attributes, size_t attribute_count,
    const CmAstAttributeId *source_attributes, size_t source_attribute_count,
    CmSpan span, CmAstItemId item)
{
    size_t index;

    if (attribute_count != 0u && attributes == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, item,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "effective attribute count has no attribute storage");
        return 0;
    }
    if (source_attribute_count != 0u && source_attributes == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, item,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "source attribute count has no attribute ID storage");
        return 0;
    }
    for (index = 0u; index < attribute_count; ++index) {
        const CmEffectiveAttribute *attribute;
        const CmAstAttribute *source_attribute;

        attribute = &attributes[index];
        source_attribute = cm_ast_get_attribute(state->ast,
            attribute->source_id);
        if (!cm_lower_attribute_id_in_list(attribute->source_id,
                source_attributes, source_attribute_count)
            || source_attribute == NULL
            || attribute->style != source_attribute->style
            || attribute->span.start > attribute->span.end
            || (attribute->meta_length != 0u && attribute->meta == NULL)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, item,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "effective attribute has invalid source provenance");
            return 0;
        }
    }
    return 1;
}

static int cm_lower_expanded_id_seen(CmLowerState *state, CmAstItemId id)
{
    size_t index;

    for (index = 0u; index < state->expanded_source_ids.len; ++index) {
        const CmAstItemId *seen;

        seen = (const CmAstItemId *)cm_vec_at_const(
            &state->expanded_source_ids, index);
        if (*seen == id) {
            return 1;
        }
    }
    (void)cm_vec_push(&state->expanded_source_ids, &id);
    return 0;
}

static int cm_lower_validate_expanded_items(CmLowerState *state,
    const CmExpandedItem *items, size_t item_count,
    const CmAstItemId *allowed_items, size_t allowed_count)
{
    size_t index;
    size_t allowed_position;

    if (item_count > allowed_count) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), CM_AST_ITEM_NONE,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "expanded child count exceeds its source parent count");
        return 0;
    }
    if (item_count != 0u && items == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), CM_AST_ITEM_NONE,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "expanded item count has no item storage");
        return 0;
    }
    if (allowed_count != 0u && allowed_items == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), CM_AST_ITEM_NONE,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "source child count has no item ID storage");
        return 0;
    }
    allowed_position = 0u;
    for (index = 0u; index < item_count && !state->failed; ++index) {
        const CmExpandedItem *expanded_item;
        const CmAstItem *source_item;
        const CmAstItemId *source_children;
        size_t source_child_count;
        const CmAstAttributeId *source_inner_attributes;
        size_t source_inner_attribute_count;
        CmExpandedChildKind source_child_kind;
        CmSpan span;

        expanded_item = &items[index];
        while (allowed_position < allowed_count
            && allowed_items[allowed_position] != expanded_item->source_id) {
            allowed_position += 1u;
        }
        if (allowed_position == allowed_count
            || cm_lower_expanded_id_seen(state,
                expanded_item->source_id)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, expanded_item->span),
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "expanded item is duplicated, reordered, or outside its "
                "source parent");
            return 0;
        }
        allowed_position += 1u;
        source_item = cm_ast_get_item(state->ast,
            expanded_item->source_id);
        if (source_item == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, expanded_item->span),
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "expanded item contains an invalid source ID");
            return 0;
        }
        span = cm_lower_span(state, source_item->span);
        if (expanded_item->span.start != source_item->span.start
            || expanded_item->span.end != source_item->span.end) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "expanded item span does not match its source item");
            return 0;
        }
        cm_lower_source_children(source_item, &source_children,
            &source_child_count, &source_child_kind);
        source_inner_attributes = NULL;
        source_inner_attribute_count = 0u;
        if (source_item->kind == CM_AST_ITEM_MODULE
            && source_item->data.module_item.is_inline) {
            source_inner_attributes =
                source_item->data.module_item.inner_attributes;
            source_inner_attribute_count = (size_t)
                source_item->data.module_item.inner_attribute_count;
        }
        if (expanded_item->child_kind != source_child_kind
            || (expanded_item->child_count != 0u
                && expanded_item->children == NULL)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "expanded child kind or child storage mismatches source");
            return 0;
        }
        if (!cm_lower_validate_effective_attributes(state,
                expanded_item->attributes, expanded_item->attribute_count,
                source_item->attributes,
                (size_t)source_item->attribute_count, span,
                expanded_item->source_id)
            || !cm_lower_validate_effective_attributes(state,
                expanded_item->inner_attributes,
                expanded_item->inner_attribute_count,
                source_inner_attributes, source_inner_attribute_count,
                span, expanded_item->source_id)
            || !cm_lower_validate_expanded_items(state,
                expanded_item->children, expanded_item->child_count,
                source_children, source_child_count)) {
            return 0;
        }
    }
    return !state->failed;
}

static unsigned int cm_lower_item_namespace_mask(CmAstItemKind kind)
{
    switch (kind) {
    case CM_AST_ITEM_STRUCT:
    case CM_AST_ITEM_UNION:
    case CM_AST_ITEM_ENUM:
        return 3u;
    case CM_AST_ITEM_MODULE:
    case CM_AST_ITEM_TYPE_ALIAS:
    case CM_AST_ITEM_TRAIT:
        return 1u;
    case CM_AST_ITEM_FUNCTION:
    case CM_AST_ITEM_CONST:
    case CM_AST_ITEM_STATIC:
        return 2u;
    default:
        return 0u;
    }
}

static int cm_lower_name_exists(const CmLowerState *state,
    CmHirModuleId module, CmInternId ast_name, CmAstItemKind kind)
{
    size_t index;

    if (kind == CM_AST_ITEM_CONST) {
        const CmInternedString *name;

        name = cm_ast_get_string(state->ast, ast_name);
        if (name != NULL && name->len == 1u
            && name->bytes[0] == (unsigned char)'_') {
            return 0;
        }
    }

    for (index = 0u; index < state->item_records.len; ++index) {
        const CmLowerItemRecord *record;
        const CmAstItem *record_item;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        record_item = record == NULL ? NULL
            : cm_ast_get_item(record->ast, record->ast_id);
        if ((cm_lower_item_namespace_mask(record->kind)
                & cm_lower_item_namespace_mask(kind)) != 0u
            && record->owner_module == module
            && cm_hir_def_id_is_none(record->parent_definition)
            && !cm_lower_anonymous_const(record->ast, record_item)
            && cm_lower_hir_name_matches_ast(state, record->hir_name,
                ast_name)) {
            return 1;
        }
    }
    return 0;
}

static int cm_lower_associated_name_exists(const CmLowerState *state,
    CmHirDefId parent_definition, CmInternId ast_name,
    CmAstItemKind kind)
{
    size_t index;

    for (index = 0u; index < state->item_records.len; ++index) {
        const CmLowerItemRecord *record;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if ((cm_lower_item_namespace_mask(record->kind)
                & cm_lower_item_namespace_mask(kind)) != 0u
            && cm_hir_def_id_equal(record->parent_definition,
                parent_definition)
            && cm_lower_hir_name_matches_ast(state, record->hir_name,
                ast_name)) {
            return 1;
        }
    }
    return 0;
}

static int cm_lower_reserve_enum_variant_definitions(CmLowerState *state,
    const CmAstItem *item, const CmLowerItemRecord *record)
{
    const CmAstEnum *enumeration;
    uint32_t index;

    if (item->kind != CM_AST_ITEM_ENUM) return 1;
    enumeration = &item->data.enum_item;
    if ((enumeration->variant_count == 0u)
            != (enumeration->variants == NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            record->is_generated ? record->effective_span
                : (CmSpan){ record->source, item->span.start, item->span.end },
            record->ast_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "enum variant count has inconsistent storage");
        return 0;
    }
    for (index = 0u; index < enumeration->variant_count; ++index) {
        const CmAstVariant *variant;
        CmLowerVariantRecord variant_record;
        CmSpan span;
        CmHirStatus status;

        variant = &enumeration->variants[index];
        span = record->is_generated ? record->effective_span
            : (CmSpan){ record->source, variant->span.start,
                variant->span.end };
        memset(&variant_record, 0, sizeof(variant_record));
        variant_record.enumeration = record->definition;
        variant_record.source_index = index;
        status = cm_hir_reserve_enum_variant_definition(state->hir,
            state->result.crate_id, span, &variant_record.definition);
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state, span, record->ast_id, status,
                "cannot reserve enum variant definition");
            return 0;
        }
        (void)cm_vec_push(&state->variant_records, &variant_record);
    }
    return 1;
}

static const CmLowerVariantRecord *cm_lower_find_variant_record(
    const CmLowerState *state, CmHirDefId enumeration, uint32_t source_index,
    uint32_t *out_matches)
{
    const CmLowerVariantRecord *result;
    size_t index;
    uint32_t matches;

    result = NULL;
    matches = 0u;
    for (index = 0u; index < state->variant_records.len; ++index) {
        const CmLowerVariantRecord *record;

        record = (const CmLowerVariantRecord *)cm_vec_at_const(
            &state->variant_records, index);
        if (record != NULL
            && cm_hir_def_id_equal(record->enumeration, enumeration)
            && record->source_index == source_index) {
            result = record;
            matches += 1u;
        }
    }
    if (out_matches != NULL) *out_matches = matches;
    return result;
}

static int cm_lower_reserve_items(CmLowerState *state,
    CmHirModuleId owner_module, CmHirDefId parent_definition,
    CmLowerParentKind parent_kind, const CmAstItemId *items,
    uint32_t item_count)
{
    uint32_t index;

    for (index = 0u; index < item_count && !state->failed; ++index) {
        const CmAstItem *item;
        CmLowerItemRecord record;
        CmHirStatus status;
        CmSpan span;

        item = cm_ast_get_item(state->ast, items[index]);
        if (item == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, (CmAstSpan){ 0u, 0u }), items[index],
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "item list contains an invalid AST item ID");
            break;
        }
        span = cm_lower_span(state, item->span);
        if ((parent_kind == CM_LOWER_PARENT_NONE)
            != cm_hir_def_id_is_none(parent_definition)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "item reservation has an inconsistent parent role");
            break;
        }
        if (item->kind == CM_AST_ITEM_IMPL
            && item->visibility.kind != CM_AST_VIS_INHERITED) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "impl blocks cannot have explicit visibility");
            break;
        }
        if (parent_kind != CM_LOWER_PARENT_NONE
            && item->kind != CM_AST_ITEM_TYPE_ALIAS
            && item->kind != CM_AST_ITEM_FUNCTION) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "%s children other than associated types and methods are "
                "not supported",
                parent_kind == CM_LOWER_PARENT_TRAIT ? "trait" : "impl");
            break;
        }
        if (!cm_lower_item_kind_supported(item->kind)) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "item kind %u has no declaration-preserving HIR lowering",
                (unsigned int)item->kind);
            break;
        }
        if ((item->kind == CM_AST_ITEM_IMPL
                && item->name != CM_INTERN_ID_NONE)
            || (item->kind != CM_AST_ITEM_IMPL
                && (item->name == CM_INTERN_ID_NONE
                    || cm_lower_ast_string(state, item->name) == NULL))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, items[index],
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "supported item has an invalid name");
            break;
        }
        if (parent_kind != CM_LOWER_PARENT_NONE
            && (item->generic_parameter_count != 0u
                || cm_lower_item_has_where_clause(item))
            && !((item->kind == CM_AST_ITEM_FUNCTION
                    && cm_lower_parent_supports_generic_method(state,
                        parent_kind, parent_definition))
                || (item->kind == CM_AST_ITEM_TYPE_ALIAS
                    && cm_lower_parent_supports_generic_associated_type(
                        state, parent_kind, parent_definition)))) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                item->generic_parameter_count != 0u
                    ? "generic associated items are supported only on "
                      "methods and positive-trait associated types"
                    : "where predicates are currently supported only on "
                      "methods and positive-trait associated types");
            break;
        }
        if (parent_kind != CM_LOWER_PARENT_NONE
            && item->kind == CM_AST_ITEM_TYPE_ALIAS
            && (item->visibility.kind != CM_AST_VIS_INHERITED
                || item->attribute_count != 0u
                || (parent_kind == CM_LOWER_PARENT_TRAIT
                    && (item->data.value_item.has_value
                        || item->data.value_item.type != CM_AST_TYPE_NONE))
                || (parent_kind == CM_LOWER_PARENT_IMPL
                    && (!item->data.value_item.has_value
                        || item->data.value_item.type == CM_AST_TYPE_NONE
                        || item->data.value_item.bound_count != 0u)))) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                parent_kind == CM_LOWER_PARENT_TRAIT
                    ? (item->data.value_item.has_value
                            || item->data.value_item.type
                                != CM_AST_TYPE_NONE
                        ? "associated type defaults are not supported"
                        : "associated types must be targetless, non-generic "
                          "declarations in this HIR slice")
                    : "impl associated types must be bare, target-bearing, "
                      "non-generic definitions in this HIR slice");
            break;
        }
        if (parent_kind != CM_LOWER_PARENT_NONE
            && item->kind == CM_AST_ITEM_FUNCTION
            && (item->visibility.kind != CM_AST_VIS_INHERITED
                || item->attribute_count != 0u
                || item->data.function_item.is_const
                || (item->data.function_item.is_async
                    && parent_kind != CM_LOWER_PARENT_TRAIT)
                || (item->data.function_item.abi != CM_INTERN_ID_NONE
                    && !cm_lower_string_is(state,
                        item->data.function_item.abi, "rust-call"))
                || (parent_kind == CM_LOWER_PARENT_IMPL
                    && item->data.function_item.body
                        == CM_AST_EXPR_NONE))) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                parent_kind == CM_LOWER_PARENT_TRAIT
                    ? "trait methods must be attribute-free ordinary "
                      "Rust or rust-call ABI declarations in raw lowering"
                    : "impl methods must be attribute-free "
                      "ordinary Rust or rust-call ABI definitions in raw "
                      "lowering");
            break;
        }
        if ((parent_kind == CM_LOWER_PARENT_NONE
                && !cm_lower_anonymous_const(state->ast, item)
                && cm_lower_name_exists(state, owner_module, item->name,
                    item->kind))
            || (parent_kind != CM_LOWER_PARENT_NONE
                && cm_lower_associated_name_exists(state, parent_definition,
                    item->name, item->kind))) {
            cm_lower_fail(state, CM_HIR_LOWER_DUPLICATE_NAME, span,
                items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                parent_kind == CM_LOWER_PARENT_IMPL
                    ? "duplicate associated definition in one impl"
                    : "duplicate declaration name in one module or trait");
            break;
        }
        memset(&record, 0, sizeof(record));
        record.ast = state->ast;
        record.source = state->source;
        record.graph_module = state->graph_module;
        record.ast_id = items[index];
        record.owner_module = owner_module;
        record.parent_definition = parent_definition;
        record.parent_kind = parent_kind;
        record.kind = item->kind;
        record.hir_name = item->kind == CM_AST_ITEM_IMPL
            ? CM_INTERN_ID_NONE
            : cm_lower_copy_string(state, item->name, span, items[index]);
        if (state->failed) {
            break;
        }
        if (item->kind == CM_AST_ITEM_MODULE) {
            CmInternId name;

            if (!item->data.module_item.is_inline) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "external module declarations require a module graph");
                break;
            }
            name = cm_lower_copy_string(state, item->name, span,
                items[index]);
            if (state->failed) {
                break;
            }
            status = cm_hir_add_module(state->hir, state->result.crate_id,
                owner_module, name, span, &record.nested_module);
            if (status != CM_HIR_OK) {
                cm_lower_fail_hir(state, span, items[index], status,
                    "cannot create HIR module");
                break;
            }
        }
        {
            CmHirItemKind hir_item_kind;

            if (!cm_lower_hir_item_kind(item, &hir_item_kind)) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK, "cannot map reserved AST item kind");
                break;
            }
            status = cm_hir_reserve_item_definition_as(state->hir,
                state->result.crate_id, hir_item_kind, span,
                &record.definition);
        }
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state, span, items[index], status,
                "cannot reserve item definition");
            break;
        }
        if (!cm_lower_reserve_enum_variant_definitions(state, item,
                &record)) {
            break;
        }
        (void)cm_vec_push(&state->item_records, &record);
        if (item->kind == CM_AST_ITEM_MODULE) {
            if (item->data.module_item.item_count != 0u
                && item->data.module_item.items == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                    items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "inline module item count has no item storage");
                break;
            }
            if (!cm_lower_reserve_items(state, record.nested_module,
                    cm_hir_def_id_none(), CM_LOWER_PARENT_NONE,
                    item->data.module_item.items,
                    item->data.module_item.item_count)) {
                break;
            }
        } else if (item->kind == CM_AST_ITEM_TRAIT) {
            if (item->data.trait_item.item_count != 0u
                && item->data.trait_item.items == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                    items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK, "trait item count has no item storage");
                break;
            }
            if (!cm_lower_reserve_items(state, owner_module,
                    record.definition, CM_LOWER_PARENT_TRAIT,
                    item->data.trait_item.items,
                    item->data.trait_item.item_count)) {
                break;
            }
        } else if (item->kind == CM_AST_ITEM_IMPL) {
            if (item->data.impl_item.item_count != 0u
                && item->data.impl_item.items == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                    items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK, "impl item count has no item storage");
                break;
            }
            if (!cm_lower_reserve_items(state, owner_module,
                    record.definition, CM_LOWER_PARENT_IMPL,
                    item->data.impl_item.items,
                    item->data.impl_item.item_count)) {
                break;
            }
        }
    }
    return !state->failed;
}

static int cm_lower_reserve_expanded_items(CmLowerState *state,
    CmHirModuleId owner_module, CmHirDefId parent_definition,
    CmLowerParentKind parent_kind, const CmExpandedItem *items,
    size_t item_count)
{
    size_t index;

    for (index = 0u; index < item_count && !state->failed; ++index) {
        const CmExpandedItem *expanded_item;
        const CmAstItem *item;
        CmLowerItemRecord record;
        CmHirStatus status;
        CmSpan span;

        expanded_item = &items[index];
        item = cm_ast_get_item(state->ast, expanded_item->source_id);
        if (item == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, expanded_item->span),
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "validated expanded item lost its source item");
            break;
        }
        span = cm_lower_span(state, item->span);
        if ((parent_kind == CM_LOWER_PARENT_NONE)
            != cm_hir_def_id_is_none(parent_definition)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "expanded item reservation has an inconsistent parent role");
            break;
        }
        if (parent_kind != CM_LOWER_PARENT_NONE
            && item->kind != CM_AST_ITEM_TYPE_ALIAS
            && item->kind != CM_AST_ITEM_FUNCTION) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "%s children other than associated types and methods are "
                "not supported",
                parent_kind == CM_LOWER_PARENT_TRAIT ? "trait" : "impl");
            break;
        }
        if (!cm_lower_item_kind_supported(item->kind)) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "item kind %u has no declaration-preserving HIR lowering",
                (unsigned int)item->kind);
            break;
        }
        if ((item->kind == CM_AST_ITEM_IMPL
                && item->name != CM_INTERN_ID_NONE)
            || (item->kind != CM_AST_ITEM_IMPL
                && (item->name == CM_INTERN_ID_NONE
                    || cm_lower_ast_string(state, item->name) == NULL))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "supported expanded item has an invalid name");
            break;
        }
        if (item->kind == CM_AST_ITEM_IMPL
            && item->visibility.kind != CM_AST_VIS_INHERITED) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "impl blocks cannot have explicit visibility");
            break;
        }
        if (parent_kind != CM_LOWER_PARENT_NONE
            && (item->generic_parameter_count != 0u
                || cm_lower_item_has_where_clause(item))
            && !((item->kind == CM_AST_ITEM_FUNCTION
                    && cm_lower_parent_supports_generic_method(state,
                        parent_kind, parent_definition))
                || (item->kind == CM_AST_ITEM_TYPE_ALIAS
                    && cm_lower_parent_supports_generic_associated_type(
                        state, parent_kind, parent_definition)))) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                item->generic_parameter_count != 0u
                    ? "generic associated items are supported only on "
                      "methods and positive-trait associated types"
                    : "where predicates are currently supported only on "
                      "methods and positive-trait associated types");
            break;
        }
        if (parent_kind != CM_LOWER_PARENT_NONE
            && item->kind == CM_AST_ITEM_TYPE_ALIAS
            && (item->visibility.kind != CM_AST_VIS_INHERITED
                || expanded_item->attribute_count != 0u
                || expanded_item->inner_attribute_count != 0u
                || (parent_kind == CM_LOWER_PARENT_TRAIT
                    && (item->data.value_item.has_value
                        || item->data.value_item.type != CM_AST_TYPE_NONE))
                || (parent_kind == CM_LOWER_PARENT_IMPL
                    && (!item->data.value_item.has_value
                        || item->data.value_item.type == CM_AST_TYPE_NONE
                        || item->data.value_item.bound_count != 0u)))) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                parent_kind == CM_LOWER_PARENT_TRAIT
                    ? (item->data.value_item.has_value
                            || item->data.value_item.type
                                != CM_AST_TYPE_NONE
                        ? "associated type defaults are not supported"
                        : "associated types must be targetless, non-generic "
                          "declarations in this HIR slice")
                    : "impl associated types must be bare, target-bearing, "
                      "non-generic definitions in this HIR slice");
            break;
        }
        if (parent_kind != CM_LOWER_PARENT_NONE
            && item->kind == CM_AST_ITEM_FUNCTION
            && (item->visibility.kind != CM_AST_VIS_INHERITED
                || expanded_item->inner_attribute_count != 0u
                || item->data.function_item.is_const
                || (item->data.function_item.is_async
                    && parent_kind != CM_LOWER_PARENT_TRAIT)
                || (item->data.function_item.abi != CM_INTERN_ID_NONE
                    && !cm_lower_string_is(state,
                        item->data.function_item.abi, "rust-call"))
                || (parent_kind == CM_LOWER_PARENT_IMPL
                    && item->data.function_item.body
                        == CM_AST_EXPR_NONE))) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                parent_kind == CM_LOWER_PARENT_TRAIT
                    ? "trait methods must use Rust or rust-call ABI "
                      "declarations"
                    : "impl methods must use Rust or rust-call ABI "
                      "definitions");
            break;
        }
        if ((parent_kind == CM_LOWER_PARENT_NONE
                && cm_lower_name_exists(state, owner_module, item->name,
                    item->kind))
            || (parent_kind != CM_LOWER_PARENT_NONE
                && cm_lower_associated_name_exists(state, parent_definition,
                    item->name, item->kind))) {
            cm_lower_fail(state, CM_HIR_LOWER_DUPLICATE_NAME, span,
                expanded_item->source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                parent_kind == CM_LOWER_PARENT_IMPL
                    ? "duplicate associated definition in one impl"
                    : "duplicate active declaration name in one module or "
                      "trait");
            break;
        }
        memset(&record, 0, sizeof(record));
        record.ast = state->ast;
        record.source = state->source;
        record.graph_module = state->graph_module;
        record.ast_id = expanded_item->source_id;
        record.owner_module = owner_module;
        record.parent_definition = parent_definition;
        record.parent_kind = parent_kind;
        record.kind = item->kind;
        record.hir_name = item->kind == CM_AST_ITEM_IMPL
            ? CM_INTERN_ID_NONE
            : cm_lower_copy_string(state, item->name, span,
                expanded_item->source_id);
        if (state->failed) {
            break;
        }
        record.expanded_item = expanded_item;
        if (item->kind == CM_AST_ITEM_MODULE) {
            CmInternId name;

            if (!item->data.module_item.is_inline) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    expanded_item->source_id, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "external module declarations require a module graph");
                break;
            }
            name = cm_lower_copy_string(state, item->name, span,
                expanded_item->source_id);
            if (state->failed) {
                break;
            }
            status = cm_hir_add_module(state->hir, state->result.crate_id,
                owner_module, name, span, &record.nested_module);
            if (status != CM_HIR_OK) {
                cm_lower_fail_hir(state, span, expanded_item->source_id,
                    status, "cannot create active HIR module");
                break;
            }
        }
        {
            CmHirItemKind hir_item_kind;

            if (!cm_lower_hir_item_kind(item, &hir_item_kind)) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    expanded_item->source_id, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "cannot map active reserved AST item kind");
                break;
            }
            status = cm_hir_reserve_item_definition_as(state->hir,
                state->result.crate_id, hir_item_kind, span,
                &record.definition);
        }
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state, span, expanded_item->source_id, status,
                "cannot reserve active item definition");
            break;
        }
        if (!cm_lower_reserve_enum_variant_definitions(state, item,
                &record)) {
            break;
        }
        (void)cm_vec_push(&state->item_records, &record);
        if (item->kind == CM_AST_ITEM_MODULE) {
            if (!cm_lower_reserve_expanded_items(state,
                    record.nested_module, cm_hir_def_id_none(),
                    CM_LOWER_PARENT_NONE,
                    expanded_item->children, expanded_item->child_count)) {
                break;
            }
        } else if (item->kind == CM_AST_ITEM_TRAIT) {
            if (!cm_lower_reserve_expanded_items(state, owner_module,
                    record.definition, CM_LOWER_PARENT_TRAIT,
                    expanded_item->children,
                    expanded_item->child_count)) {
                break;
            }
        } else if (item->kind == CM_AST_ITEM_IMPL) {
            if (!cm_lower_reserve_expanded_items(state, owner_module,
                    record.definition, CM_LOWER_PARENT_IMPL,
                    expanded_item->children,
                    expanded_item->child_count)) {
                break;
            }
        }
    }
    return !state->failed;
}

static const CmLowerItemRecord *cm_lower_find_name_in_module(
    const CmLowerState *state, CmHirModuleId module, CmInternId ast_name)
{
    size_t index;

    for (index = 0u; index < state->item_records.len; ++index) {
        const CmLowerItemRecord *record;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if ((cm_lower_item_namespace_mask(record->kind) & 1u) != 0u
            && record->owner_module == module
            && cm_hir_def_id_is_none(record->parent_definition)
            && cm_lower_hir_name_matches_ast(state, record->hir_name,
                ast_name)) {
            return record;
        }
    }
    return NULL;
}

static CmHirModuleId cm_lower_parent_module(const CmLowerState *state,
    CmHirModuleId module)
{
    const CmHirModule *module_value;

    module_value = cm_hir_get_module(state->hir, module);
    return module_value == NULL ? CM_HIR_MODULE_NONE : module_value->parent;
}

static CmLowerLookupResult cm_lower_lookup_local_path(
    const CmLowerState *state, const CmAstPath *path,
    CmHirModuleId current_module, CmHirLowerPathUse use,
    const CmLowerItemRecord **out_record, CmHirDefId *out_module_definition)
{
    CmHirModuleId module;
    uint32_t segment_index;
    const CmLowerItemRecord *record;

    *out_record = NULL;
    *out_module_definition = cm_hir_def_id_none();
    if (path->segment_count == 0u) {
        return CM_LOWER_LOOKUP_NOT_FOUND;
    }
    module = path->absolute ? state->result.root_module : current_module;
    segment_index = 0u;
    if (cm_lower_string_is(state, path->segments[0].name, "crate")) {
        module = state->result.root_module;
        segment_index = 1u;
    } else if (cm_lower_string_is(state, path->segments[0].name, "self")) {
        module = current_module;
        segment_index = 1u;
    } else {
        while (segment_index < path->segment_count
            && cm_lower_string_is(state,
                path->segments[segment_index].name, "super")) {
            module = cm_lower_parent_module(state, module);
            if (module == CM_HIR_MODULE_NONE) {
                return CM_LOWER_LOOKUP_NOT_FOUND;
            }
            ++segment_index;
        }
    }
    if (segment_index == path->segment_count) {
        if (use == CM_HIR_LOWER_PATH_VISIBILITY) {
            const CmHirModule *module_value;

            module_value = cm_hir_get_module(state->hir, module);
            if (module_value == NULL) {
                return CM_LOWER_LOOKUP_NOT_FOUND;
            }
            *out_module_definition = module_value->definition;
            return CM_LOWER_LOOKUP_MODULE;
        }
        return CM_LOWER_LOOKUP_WRONG_NAMESPACE;
    }
    record = cm_lower_find_name_in_module(state, module,
        path->segments[segment_index].name);
    if (record == NULL) {
        return CM_LOWER_LOOKUP_NOT_FOUND;
    }
    ++segment_index;
    while (segment_index < path->segment_count) {
        if (record->kind != CM_AST_ITEM_MODULE
            || record->nested_module == CM_HIR_MODULE_NONE) {
            return CM_LOWER_LOOKUP_WRONG_NAMESPACE;
        }
        record = cm_lower_find_name_in_module(state, record->nested_module,
            path->segments[segment_index].name);
        if (record == NULL) {
            return CM_LOWER_LOOKUP_NOT_FOUND;
        }
        ++segment_index;
    }
    if (use == CM_HIR_LOWER_PATH_VISIBILITY) {
        if (record->kind != CM_AST_ITEM_MODULE) {
            return CM_LOWER_LOOKUP_WRONG_NAMESPACE;
        }
        {
            const CmHirModule *module_value;

            module_value = cm_hir_get_module(state->hir,
                record->nested_module);
            if (module_value == NULL) {
                return CM_LOWER_LOOKUP_NOT_FOUND;
            }
            *out_module_definition = module_value->definition;
        }
        return CM_LOWER_LOOKUP_MODULE;
    }
    if (record->kind == CM_AST_ITEM_STRUCT
        || record->kind == CM_AST_ITEM_UNION
        || record->kind == CM_AST_ITEM_ENUM) {
        *out_record = record;
        return CM_LOWER_LOOKUP_DEFINITION;
    }
    if (record->kind == CM_AST_ITEM_TYPE_ALIAS) {
        *out_record = record;
        return CM_LOWER_LOOKUP_ALIAS;
    }
    if (record->kind == CM_AST_ITEM_TRAIT) {
        *out_record = record;
        return CM_LOWER_LOOKUP_TRAIT;
    }
    return CM_LOWER_LOOKUP_WRONG_NAMESPACE;
}

static const CmLowerItemRecord *cm_lower_find_graph_declaration(
    const CmLowerState *state, CmResolveItemRef declaration,
    uint32_t *out_match_count)
{
    const CmLowerItemRecord *result;
    size_t index;
    uint32_t matches;

    result = NULL;
    matches = 0u;
    for (index = 0u; index < state->item_records.len; ++index) {
        const CmLowerItemRecord *record;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if (record != NULL && record->source == declaration.source
            && record->ast_id == declaration.item) {
            result = record;
            matches += 1u;
        }
    }
    if (out_match_count != NULL) *out_match_count = matches;
    return result;
}

static int cm_lower_path_is_module_keyword_only(const CmLowerState *state,
    const CmAstPath *path)
{
    uint32_t index;

    if (path == NULL || path->segment_count == 0u) return 0;
    for (index = 0u; index < path->segment_count; ++index) {
        if (!cm_lower_string_is(state, path->segments[index].name, "crate")
            && !cm_lower_string_is(state, path->segments[index].name, "self")
            && !cm_lower_string_is(state, path->segments[index].name,
                "super")) {
            return 0;
        }
    }
    return 1;
}

static CmLowerLookupResult cm_lower_lookup_graph_path(
    const CmLowerState *state, const CmAstPath *path,
    CmHirLowerPathUse use, const CmLowerItemRecord **out_record,
    CmHirDefId *out_module_definition)
{
    CmResolvePathSegmentView *segments;
    CmResolvedBinding binding;
    CmImportLookupStatus status;
    const CmLowerItemRecord *record;
    uint32_t index;
    uint32_t matches;

    *out_record = NULL;
    *out_module_definition = cm_hir_def_id_none();
    if (state->graph == NULL || state->imports == NULL
        || state->module_map == NULL || state->graph_module == CM_MODULE_NONE
        || path == NULL || path->segment_count == 0u
        || path->segments == NULL) {
        return CM_LOWER_LOOKUP_RESOLVER_ERROR;
    }
    segments = (CmResolvePathSegmentView *)cm_alloc_zeroed(
        (size_t)path->segment_count, sizeof(*segments));
    for (index = 0u; index < path->segment_count; ++index) {
        const CmInternedString *name;

        name = cm_ast_get_string(state->ast, path->segments[index].name);
        if (name == NULL || name->len == 0u) {
            cm_free(segments);
            return CM_LOWER_LOOKUP_RESOLVER_ERROR;
        }
        segments[index].bytes = name->bytes;
        segments[index].length = name->len;
    }
    status = cm_import_resolve_path_checked(state->imports, state->graph,
        state->graph_revision, state->graph_module, path->absolute, segments,
        (size_t)path->segment_count, CM_RESOLVE_NAMESPACE_TYPE, &binding);
    cm_free(segments);
    if (status == CM_IMPORT_LOOKUP_NOT_FOUND)
        return CM_LOWER_LOOKUP_NOT_FOUND;
    if (status == CM_IMPORT_LOOKUP_STALE_REVISION
        || status == CM_IMPORT_LOOKUP_FAILED_BUILD) {
        return CM_LOWER_LOOKUP_STALE_GRAPH;
    }
    if (status != CM_IMPORT_LOOKUP_OK)
        return CM_LOWER_LOOKUP_RESOLVER_ERROR;
    if (binding.revision != state->graph_revision
        || binding.namespace_kind != CM_RESOLVE_NAMESPACE_TYPE
        || binding.is_ambiguous
        || (binding.is_import
            && (binding.import_declaration.source == 0u
                || binding.import_declaration.item == CM_AST_ITEM_NONE))
        || (!binding.is_import
            && (binding.import_declaration.source != 0u
                || binding.import_declaration.item != CM_AST_ITEM_NONE))) {
        return CM_LOWER_LOOKUP_RESOLVER_ERROR;
    }
    if (binding.item_kind == CM_AST_ITEM_MODULE) {
        CmHirModuleId hir_module;
        const CmHirModule *module;
        CmHirModuleMapStatus map_status;

        if (binding.target_module == CM_MODULE_NONE) {
            return CM_LOWER_LOOKUP_RESOLVER_ERROR;
        }
        map_status = cm_hir_module_map_lookup_hir(state->module_map,
            state->graph, state->graph_revision, binding.target_module,
            state->hir, &hir_module);
        module = map_status == CM_HIR_MODULE_MAP_OK
            ? cm_hir_get_module(state->hir, hir_module) : NULL;
        if (module == NULL) return CM_LOWER_LOOKUP_RESOLVER_ERROR;
        if (binding.declaration.source != 0u
            || binding.declaration.item != CM_AST_ITEM_NONE) {
            record = cm_lower_find_graph_declaration(state,
                binding.declaration, &matches);
            if (record == NULL || matches != 1u
                || record->kind != CM_AST_ITEM_MODULE
                || record->nested_module != hir_module) {
                return CM_LOWER_LOOKUP_RESOLVER_ERROR;
            }
        }
        if (use != CM_HIR_LOWER_PATH_VISIBILITY)
            return CM_LOWER_LOOKUP_WRONG_NAMESPACE;
        *out_module_definition = module->definition;
        return CM_LOWER_LOOKUP_MODULE;
    }
    if (binding.target_module != CM_MODULE_NONE
        || binding.declaration.source == 0u
        || binding.declaration.item == CM_AST_ITEM_NONE) {
        return CM_LOWER_LOOKUP_RESOLVER_ERROR;
    }
    record = cm_lower_find_graph_declaration(state, binding.declaration,
        &matches);
    if (record == NULL || matches != 1u || record->kind != binding.item_kind) {
        return CM_LOWER_LOOKUP_RESOLVER_ERROR;
    }
    if (use == CM_HIR_LOWER_PATH_VISIBILITY)
        return CM_LOWER_LOOKUP_WRONG_NAMESPACE;
    if (record->kind == CM_AST_ITEM_STRUCT
        || record->kind == CM_AST_ITEM_UNION
        || record->kind == CM_AST_ITEM_ENUM) {
        *out_record = record;
        return CM_LOWER_LOOKUP_DEFINITION;
    }
    if (record->kind == CM_AST_ITEM_TYPE_ALIAS) {
        *out_record = record;
        return CM_LOWER_LOOKUP_ALIAS;
    }
    if (record->kind == CM_AST_ITEM_TRAIT) {
        *out_record = record;
        return CM_LOWER_LOOKUP_TRAIT;
    }
    return CM_LOWER_LOOKUP_WRONG_NAMESPACE;
}

static CmLowerLookupResult cm_lower_lookup_path(const CmLowerState *state,
    const CmAstPath *path, CmHirModuleId current_module,
    CmHirLowerPathUse use, const CmLowerItemRecord **out_record,
    CmHirDefId *out_module_definition)
{
    if (state->graph == NULL) {
        return cm_lower_lookup_local_path(state, path, current_module, use,
            out_record, out_module_definition);
    }
    {
        CmModuleId mapped_module;

        if (state->module_map == NULL
            || cm_hir_module_map_lookup_module(state->module_map,
                state->graph, state->graph_revision, state->hir,
                current_module, &mapped_module) != CM_HIR_MODULE_MAP_OK
            || mapped_module != state->graph_module) {
            *out_record = NULL;
            *out_module_definition = cm_hir_def_id_none();
            return CM_LOWER_LOOKUP_RESOLVER_ERROR;
        }
    }
    if (use == CM_HIR_LOWER_PATH_VISIBILITY
        && cm_lower_path_is_module_keyword_only(state, path)) {
        return cm_lower_lookup_local_path(state, path, current_module, use,
            out_record, out_module_definition);
    }
    return cm_lower_lookup_graph_path(state, path, use, out_record,
        out_module_definition);
}

static int cm_lower_simple_identifier(const CmInternedString *text,
    CmResolvePathSegmentView *out_segment)
{
    size_t start;
    size_t end;
    size_t index;

    if (text == NULL || out_segment == NULL) return 0;
    start = 0u;
    end = text->len;
    while (start < end && (text->bytes[start] == (unsigned char)' '
            || text->bytes[start] == (unsigned char)'\t'
            || text->bytes[start] == (unsigned char)'\r'
            || text->bytes[start] == (unsigned char)'\n')) {
        start += 1u;
    }
    while (end > start && (text->bytes[end - 1u] == (unsigned char)' '
            || text->bytes[end - 1u] == (unsigned char)'\t'
            || text->bytes[end - 1u] == (unsigned char)'\r'
            || text->bytes[end - 1u] == (unsigned char)'\n')) {
        end -= 1u;
    }
    if (start == end
        || !((text->bytes[start] >= (unsigned char)'A'
                && text->bytes[start] <= (unsigned char)'Z')
            || (text->bytes[start] >= (unsigned char)'a'
                && text->bytes[start] <= (unsigned char)'z')
            || text->bytes[start] == (unsigned char)'_')) {
        return 0;
    }
    for (index = start + 1u; index < end; ++index) {
        if (!((text->bytes[index] >= (unsigned char)'A'
                    && text->bytes[index] <= (unsigned char)'Z')
                || (text->bytes[index] >= (unsigned char)'a'
                    && text->bytes[index] <= (unsigned char)'z')
                || (text->bytes[index] >= (unsigned char)'0'
                    && text->bytes[index] <= (unsigned char)'9')
                || text->bytes[index] == (unsigned char)'_')) {
            return 0;
        }
    }
    out_segment->bytes = text->bytes + start;
    out_segment->length = end - start;
    return 1;
}

static const CmLowerItemRecord *cm_lower_named_const_length(
    const CmLowerState *state, CmHirModuleId module,
    const CmInternedString *text)
{
    CmResolvePathSegmentView segment;
    const CmLowerItemRecord *record;
    size_t index;

    memset(&segment, 0, sizeof(segment));
    if (!cm_lower_simple_identifier(text, &segment)) return NULL;
    if (state->graph == NULL) {
        for (index = 0u; index < state->item_records.len; ++index) {
            const CmInternedString *name;

            record = (const CmLowerItemRecord *)cm_vec_at_const(
                &state->item_records, index);
            name = record == NULL ? NULL
                : cm_interner_get(&state->hir->strings, record->hir_name);
            if (record != NULL && record->owner_module == module
                && cm_hir_def_id_is_none(record->parent_definition)
                && record->kind == CM_AST_ITEM_CONST && name != NULL
                && name->len == segment.length
                && memcmp(name->bytes, segment.bytes,
                    segment.length) == 0) {
                return record;
            }
        }
        return NULL;
    }
    {
        CmResolvedBinding binding;
        CmImportLookupStatus status;
        uint32_t matches;

        if (state->imports == NULL || state->module_map == NULL
            || state->graph_module == CM_MODULE_NONE) {
            return NULL;
        }
        memset(&binding, 0, sizeof(binding));
        status = cm_import_resolve_path_checked(state->imports,
            state->graph, state->graph_revision, state->graph_module, 0,
            &segment, 1u, CM_RESOLVE_NAMESPACE_VALUE, &binding);
        if (status != CM_IMPORT_LOOKUP_OK
            || binding.revision != state->graph_revision
            || binding.namespace_kind != CM_RESOLVE_NAMESPACE_VALUE
            || binding.item_kind != CM_AST_ITEM_CONST
            || binding.target_module != CM_MODULE_NONE
            || binding.primitive_kind != CM_RESOLVE_PRIMITIVE_NONE
            || binding.is_ambiguous || binding.is_anonymous
            || binding.declaration.source == 0u
            || binding.declaration.item == CM_AST_ITEM_NONE) {
            return NULL;
        }
        record = cm_lower_find_graph_declaration(state,
            binding.declaration, &matches);
        return record != NULL && matches == 1u
            && record->kind == CM_AST_ITEM_CONST ? record : NULL;
    }
}

static CmHirTypeId cm_lower_type(CmLowerState *state, CmAstTypeId ast_type_id,
    CmHirModuleId module, CmHirDefId owner);

static int cm_lower_parse_u64(const CmInternedString *text,
    uint64_t *out_value)
{
    size_t index;
    uint64_t value;
    int saw_digit;

    if (text == NULL) {
        return 0;
    }
    value = 0u;
    saw_digit = 0;
    for (index = 0u; index < text->len; ++index) {
        unsigned int digit;
        unsigned char byte;

        byte = text->bytes[index];
        if (byte == (unsigned char)'_') {
            continue;
        }
        if (byte < (unsigned char)'0' || byte > (unsigned char)'9') {
            return 0;
        }
        digit = (unsigned int)(byte - (unsigned char)'0');
        if (value > (UINT64_MAX - (uint64_t)digit) / 10u) {
            return 0;
        }
        value = value * 10u + (uint64_t)digit;
        saw_digit = 1;
    }
    if (!saw_digit) {
        return 0;
    }
    *out_value = value;
    return 1;
}

static int cm_lower_parse_negative_u64(const CmInternedString *text,
    uint64_t *out_magnitude)
{
    size_t index;
    uint64_t value;

    if (text == NULL || text->len < 2u
        || text->bytes[0] != (unsigned char)'-') return 0;
    value = 0u;
    for (index = 1u; index < text->len; ++index) {
        unsigned int digit;
        unsigned char byte;

        byte = text->bytes[index];
        if (byte == (unsigned char)'_') continue;
        if (byte < (unsigned char)'0' || byte > (unsigned char)'9') {
            return 0;
        }
        digit = (unsigned int)(byte - (unsigned char)'0');
        if (value > (UINT64_MAX - (uint64_t)digit) / 10u) return 0;
        value = value * 10u + (uint64_t)digit;
    }
    *out_magnitude = value;
    return 1;
}

/*
 * Macro-expanded array lengths can retain a small constant expression rather
 * than a single decimal token.  The core `array_impl_default!` recursion, for
 * example, produces nested forms such as `((32 - 1) - 1)`.  Keep this parser
 * deliberately narrow: only unsigned decimal literals, parentheses, checked
 * addition/subtraction, and checked left shifts are admitted.  Names, suffixes,
 * and all other operators continue to fail closed in the array-lowering path
 * below.
 */
typedef struct CmLowerArrayLengthParser {
    const unsigned char *bytes;
    size_t length;
    size_t position;
} CmLowerArrayLengthParser;

static void cm_lower_array_length_skip_space(
    CmLowerArrayLengthParser *parser)
{
    while (parser->position < parser->length) {
        unsigned char byte;

        byte = parser->bytes[parser->position];
        if (byte != (unsigned char)' '
            && byte != (unsigned char)'\t'
            && byte != (unsigned char)'\r'
            && byte != (unsigned char)'\n') break;
        parser->position += 1u;
    }
}

static int cm_lower_array_length_parse_expression(
    CmLowerArrayLengthParser *parser, uint64_t *out_value);

static int cm_lower_array_length_parse_atom(
    CmLowerArrayLengthParser *parser, uint64_t *out_value)
{
    uint64_t value;
    int saw_digit;

    cm_lower_array_length_skip_space(parser);
    if (parser->position >= parser->length) return 0;
    if (parser->bytes[parser->position] == (unsigned char)'(') {
        parser->position += 1u;
        if (!cm_lower_array_length_parse_expression(parser, &value)) {
            return 0;
        }
        cm_lower_array_length_skip_space(parser);
        if (parser->position >= parser->length
            || parser->bytes[parser->position] != (unsigned char)')') {
            return 0;
        }
        parser->position += 1u;
        *out_value = value;
        return 1;
    }
    value = 0u;
    saw_digit = 0;
    while (parser->position < parser->length) {
        unsigned int digit;
        unsigned char byte;

        byte = parser->bytes[parser->position];
        if (byte == (unsigned char)'_') {
            parser->position += 1u;
            continue;
        }
        if (byte < (unsigned char)'0' || byte > (unsigned char)'9') break;
        digit = (unsigned int)(byte - (unsigned char)'0');
        if (value > (UINT64_MAX - (uint64_t)digit) / 10u) return 0;
        value = value * 10u + (uint64_t)digit;
        saw_digit = 1;
        parser->position += 1u;
    }
    if (!saw_digit) return 0;
    *out_value = value;
    return 1;
}

static int cm_lower_array_length_parse_expression(
    CmLowerArrayLengthParser *parser, uint64_t *out_value)
{
    uint64_t value;

    if (!cm_lower_array_length_parse_atom(parser, &value)) return 0;
    for (;;) {
        uint64_t right;
        unsigned char operator;

        cm_lower_array_length_skip_space(parser);
        if (parser->position >= parser->length
            || parser->bytes[parser->position] == (unsigned char)')') {
            break;
        }
        operator = parser->bytes[parser->position];
        if (operator == (unsigned char)'<'
            && parser->position + 1u < parser->length
            && parser->bytes[parser->position + 1u] == (unsigned char)'<') {
            uint64_t shift;

            parser->position += 2u;
            if (!cm_lower_array_length_parse_atom(parser, &shift)
                || shift >= 64u || value > (UINT64_MAX >> shift)) {
                return 0;
            }
            value <<= shift;
            continue;
        }
        if (operator != (unsigned char)'+'
            && operator != (unsigned char)'-') return 0;
        parser->position += 1u;
        if (!cm_lower_array_length_parse_atom(parser, &right)) return 0;
        if (operator == (unsigned char)'+') {
            if (value > UINT64_MAX - right) return 0;
            value += right;
        } else {
            if (value < right) return 0;
            value -= right;
        }
    }
    *out_value = value;
    return 1;
}

static int cm_lower_parse_array_length_expression(
    const CmInternedString *text, uint64_t *out_value)
{
    CmLowerArrayLengthParser parser;

    if (text == NULL || out_value == NULL) return 0;
    parser.bytes = text->bytes;
    parser.length = text->len;
    parser.position = 0u;
    if (!cm_lower_array_length_parse_expression(&parser, out_value)) {
        return 0;
    }
    cm_lower_array_length_skip_space(&parser);
    return parser.position == parser.length;
}

static int cm_lower_interned_string_is(const CmInternedString *string,
    const char *text)
{
    size_t length;

    if (string == NULL || text == NULL) return 0;
    length = strlen(text);
    return string->len == length
        && memcmp(string->bytes, text, length) == 0;
}

/*
 * Array length text is captured before HIR lowering, but the expression
 * parser remains the authority for grouping and operator precedence.  Parse
 * into a private AST so failed evaluation cannot append recovery nodes to the
 * source AST, then admit only checked arithmetic over nonnegative decimal
 * literals.  Subtraction and left shift retain the earlier bootstrap surface;
 * division is needed by portable-simd's `($lanes + 7) / 8` expansion.  The
 * u64 bound is a conservative bootstrap shortcut until the configured target
 * usize width is carried into HIR lowering.
 */
static int cm_lower_eval_array_length_ast(const CmAst *ast,
    CmAstExprId expression_id, size_t depth, uint64_t *out_value)
{
    const CmAstExpr *expression;
    const CmInternedString *operator_name;
    uint64_t left;
    uint64_t right;
    uint64_t value;

    if (ast == NULL || out_value == NULL
        || depth >= CM_LOWER_APIT_MAX_DEPTH) return 0;
    expression = cm_ast_get_expr(ast, expression_id);
    if (expression == NULL) return 0;
    if (expression->kind == CM_AST_EXPR_LITERAL) {
        return cm_lower_parse_u64(cm_ast_get_string(ast,
            expression->data.literal.text), out_value);
    }
    if (expression->kind != CM_AST_EXPR_BINARY) return 0;
    operator_name = cm_ast_get_string(ast,
        expression->data.binary.operator_name);
    if (!cm_lower_interned_string_is(operator_name, "+")
        && !cm_lower_interned_string_is(operator_name, "-")
        && !cm_lower_interned_string_is(operator_name, "<<")
        && !cm_lower_interned_string_is(operator_name, "/")) return 0;
    if (!cm_lower_eval_array_length_ast(ast,
            expression->data.binary.left, depth + 1u, &left)
        || !cm_lower_eval_array_length_ast(ast,
            expression->data.binary.right, depth + 1u, &right)) return 0;
    if (cm_lower_interned_string_is(operator_name, "+")) {
        if (left > UINT64_MAX - right) return 0;
        value = left + right;
    } else if (cm_lower_interned_string_is(operator_name, "-")) {
        if (left < right) return 0;
        value = left - right;
    } else if (cm_lower_interned_string_is(operator_name, "<<")) {
        if (right >= 64u || left > (UINT64_MAX >> right)) return 0;
        value = left << right;
    } else {
        if (right == 0u) return 0;
        value = left / right;
    }
    *out_value = value;
    return 1;
}

static enum cm_edition cm_lower_syntax_edition(CmHirEdition edition)
{
    switch (edition) {
    case CM_HIR_EDITION_2015: return CM_EDITION_2015;
    case CM_HIR_EDITION_2018: return CM_EDITION_2018;
    case CM_HIR_EDITION_2021: return CM_EDITION_2021;
    case CM_HIR_EDITION_2024: return CM_EDITION_2024;
    default: return CM_EDITION_2024;
    }
}

static int cm_lower_eval_array_length_expression(
    const CmInternedString *text, CmHirEdition edition,
    uint64_t *out_value)
{
    CmAst expression_ast;
    CmExpressionFragment fragment;
    uint64_t value;
    int evaluated;

    if (text == NULL || out_value == NULL) return 0;
    cm_ast_init(&expression_ast);
    fragment = cm_parse_expression_fragment(&expression_ast,
        (const char *)text->bytes, text->len,
        cm_lower_syntax_edition(edition));
    evaluated = fragment.parse.error_count == 0u
        && cm_lower_eval_array_length_ast(&expression_ast,
            fragment.expression, 0u, &value);
    cm_ast_destroy(&expression_ast);
    if (!evaluated) return 0;
    *out_value = value;
    return 1;
}

/* Rust 1.90's TypeId storage is expressed in pointer-sized words.  HIR
 * lowering does not yet receive the configured target width, so admit only
 * this exact bootstrap expression and use the bootstrap compiler's pointer
 * width.  General size_of evaluation remains closed until target properties
 * are carried into this pass. */
static int cm_lower_parse_pointer_storage_length(
    const CmInternedString *text, uint64_t *out_value)
{
    static const unsigned char expected[] =
        "16/size_of::<*const()>()";
    size_t expected_position;
    size_t position;

    if (text == NULL || out_value == NULL) return 0;
    expected_position = 0u;
    for (position = 0u; position < text->len; ++position) {
        unsigned char byte;

        byte = text->bytes[position];
        if (byte == (unsigned char)' '
            || byte == (unsigned char)'\t'
            || byte == (unsigned char)'\r'
            || byte == (unsigned char)'\n') {
            continue;
        }
        if (expected_position + 1u >= sizeof(expected)
            || byte != expected[expected_position]) {
            return 0;
        }
        expected_position += 1u;
    }
    if (expected_position + 1u != sizeof(expected)) return 0;
    *out_value = 16u / (uint64_t)sizeof(void *);
    return 1;
}

static CmHirTypeId cm_lower_add_type(CmLowerState *state,
    const CmHirType *type, CmAstTypeId ast_type_id)
{
    CmHirTypeId id;
    CmHirStatus status;

    status = cm_hir_add_type(state->hir, type, &id);
    if (status != CM_HIR_OK) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, type->span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, status,
            "cannot add HIR type: %s", cm_hir_status_name(status));
        return CM_HIR_TYPE_NONE;
    }
    return id;
}

static int cm_lower_primitive(const CmLowerState *state, CmInternId name,
    CmHirType *type)
{
    static const char *const integer_names[] = {
        "i8", "i16", "i32", "i64", "i128", "isize",
        "u8", "u16", "u32", "u64", "u128", "usize"
    };
    static const char *const float_names[] = { "f16", "f32", "f64", "f128" };
    size_t index;

    if (cm_lower_string_is(state, name, "bool")) {
        type->kind = CM_HIR_TYPE_BOOL_KIND;
        return 1;
    }
    if (cm_lower_string_is(state, name, "char")) {
        type->kind = CM_HIR_TYPE_CHAR_KIND;
        return 1;
    }
    if (cm_lower_string_is(state, name, "str")) {
        type->kind = CM_HIR_TYPE_STR_KIND;
        return 1;
    }
    for (index = 0u; index < CM_ARRAY_LEN(integer_names); ++index) {
        if (cm_lower_string_is(state, name, integer_names[index])) {
            type->kind = CM_HIR_TYPE_INTEGER_KIND;
            type->data.integer_type.kind = (CmHirIntType)index;
            return 1;
        }
    }
    for (index = 0u; index < CM_ARRAY_LEN(float_names); ++index) {
        if (cm_lower_string_is(state, name, float_names[index])) {
            type->kind = CM_HIR_TYPE_FLOAT_KIND;
            type->data.float_type.kind = (CmHirFloatType)index;
            return 1;
        }
    }
    return 0;
}

static int cm_lower_resolved_primitive(CmResolvePrimitiveKind source,
    CmHirPrimitiveKind *out_kind, CmHirType *out_type)
{
    CmHirPrimitiveKind kind;
    CmHirType type;

    memset(&type, 0, sizeof(type));
    switch (source) {
    case CM_RESOLVE_PRIMITIVE_BOOL:
        kind = CM_HIR_PRIMITIVE_BOOL;
        type.kind = CM_HIR_TYPE_BOOL_KIND;
        break;
    case CM_RESOLVE_PRIMITIVE_CHAR:
        kind = CM_HIR_PRIMITIVE_CHAR;
        type.kind = CM_HIR_TYPE_CHAR_KIND;
        break;
    case CM_RESOLVE_PRIMITIVE_STR:
        kind = CM_HIR_PRIMITIVE_STR;
        type.kind = CM_HIR_TYPE_STR_KIND;
        break;
    case CM_RESOLVE_PRIMITIVE_I8:
    case CM_RESOLVE_PRIMITIVE_I16:
    case CM_RESOLVE_PRIMITIVE_I32:
    case CM_RESOLVE_PRIMITIVE_I64:
    case CM_RESOLVE_PRIMITIVE_I128:
    case CM_RESOLVE_PRIMITIVE_ISIZE:
        kind = (CmHirPrimitiveKind)((unsigned int)CM_HIR_PRIMITIVE_I8
            + (unsigned int)source
            - (unsigned int)CM_RESOLVE_PRIMITIVE_I8);
        type.kind = CM_HIR_TYPE_INTEGER_KIND;
        type.data.integer_type.kind = (CmHirIntType)(
            (unsigned int)source
            - (unsigned int)CM_RESOLVE_PRIMITIVE_I8);
        break;
    case CM_RESOLVE_PRIMITIVE_U8:
    case CM_RESOLVE_PRIMITIVE_U16:
    case CM_RESOLVE_PRIMITIVE_U32:
    case CM_RESOLVE_PRIMITIVE_U64:
    case CM_RESOLVE_PRIMITIVE_U128:
    case CM_RESOLVE_PRIMITIVE_USIZE:
        kind = (CmHirPrimitiveKind)((unsigned int)CM_HIR_PRIMITIVE_U8
            + (unsigned int)source
            - (unsigned int)CM_RESOLVE_PRIMITIVE_U8);
        type.kind = CM_HIR_TYPE_INTEGER_KIND;
        type.data.integer_type.kind = (CmHirIntType)(
            (unsigned int)CM_HIR_INT_U8
            + (unsigned int)source
            - (unsigned int)CM_RESOLVE_PRIMITIVE_U8);
        break;
    case CM_RESOLVE_PRIMITIVE_F16:
    case CM_RESOLVE_PRIMITIVE_F32:
    case CM_RESOLVE_PRIMITIVE_F64:
    case CM_RESOLVE_PRIMITIVE_F128:
        kind = (CmHirPrimitiveKind)((unsigned int)CM_HIR_PRIMITIVE_F16
            + (unsigned int)source
            - (unsigned int)CM_RESOLVE_PRIMITIVE_F16);
        type.kind = CM_HIR_TYPE_FLOAT_KIND;
        type.data.float_type.kind = (CmHirFloatType)(
            (unsigned int)source
            - (unsigned int)CM_RESOLVE_PRIMITIVE_F16);
        break;
    case CM_RESOLVE_PRIMITIVE_NONE:
    default:
        return 0;
    }
    if (out_kind != NULL) *out_kind = kind;
    if (out_type != NULL) *out_type = type;
    return 1;
}

static int cm_lower_hir_primitive(CmHirPrimitiveKind source,
    CmHirType *out_type)
{
    CmResolvePrimitiveKind resolved;

    switch (source) {
    case CM_HIR_PRIMITIVE_BOOL:
        resolved = CM_RESOLVE_PRIMITIVE_BOOL;
        break;
    case CM_HIR_PRIMITIVE_CHAR:
        resolved = CM_RESOLVE_PRIMITIVE_CHAR;
        break;
    case CM_HIR_PRIMITIVE_STR:
        resolved = CM_RESOLVE_PRIMITIVE_STR;
        break;
    case CM_HIR_PRIMITIVE_I8:
    case CM_HIR_PRIMITIVE_I16:
    case CM_HIR_PRIMITIVE_I32:
    case CM_HIR_PRIMITIVE_I64:
    case CM_HIR_PRIMITIVE_I128:
    case CM_HIR_PRIMITIVE_ISIZE:
        resolved = (CmResolvePrimitiveKind)(
            (unsigned int)CM_RESOLVE_PRIMITIVE_I8
            + (unsigned int)source
            - (unsigned int)CM_HIR_PRIMITIVE_I8);
        break;
    case CM_HIR_PRIMITIVE_U8:
    case CM_HIR_PRIMITIVE_U16:
    case CM_HIR_PRIMITIVE_U32:
    case CM_HIR_PRIMITIVE_U64:
    case CM_HIR_PRIMITIVE_U128:
    case CM_HIR_PRIMITIVE_USIZE:
        resolved = (CmResolvePrimitiveKind)(
            (unsigned int)CM_RESOLVE_PRIMITIVE_U8
            + (unsigned int)source
            - (unsigned int)CM_HIR_PRIMITIVE_U8);
        break;
    case CM_HIR_PRIMITIVE_F16:
    case CM_HIR_PRIMITIVE_F32:
    case CM_HIR_PRIMITIVE_F64:
    case CM_HIR_PRIMITIVE_F128:
        resolved = (CmResolvePrimitiveKind)(
            (unsigned int)CM_RESOLVE_PRIMITIVE_F16
            + (unsigned int)source
            - (unsigned int)CM_HIR_PRIMITIVE_F16);
        break;
    case CM_HIR_PRIMITIVE_NONE:
    default:
        return 0;
    }
    return cm_lower_resolved_primitive(resolved, NULL, out_type);
}

static int cm_lower_lifetime(CmLowerState *state, CmInternId lifetime,
    CmHirDefId owner, CmSpan span, CmHirRegion *out_region)
{
    const CmLowerGenericRecord *generic;
    uint32_t binder_index;

    memset(out_region, 0, sizeof(*out_region));
    if (lifetime == CM_INTERN_ID_NONE
        || cm_lower_string_is(state, lifetime, "'_")) {
        out_region->kind = CM_HIR_REGION_INFER;
        out_region->data.inference_variable = state->next_region_inference;
        state->next_region_inference += 1u;
        return 1;
    }
    if (cm_lower_string_is(state, lifetime, "'static")) {
        out_region->kind = CM_HIR_REGION_STATIC;
        return 1;
    }
    if (state->active_lifetime_binder != NULL) {
        for (binder_index = 0u;
             binder_index < state->active_lifetime_binder->lifetime_count;
             ++binder_index) {
            if (cm_lower_strings_equal(state, lifetime,
                    state->active_lifetime_binder->lifetimes[
                        binder_index])) {
                out_region->kind = CM_HIR_REGION_LATE_BOUND;
                out_region->data.binder_index = binder_index;
                return 1;
            }
        }
    }
    generic = cm_lower_find_generic_in_scope(state, owner, lifetime);
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_LIFETIME) {
        cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "reference uses an undeclared lifetime");
        return 0;
    }
    out_region->kind = CM_HIR_REGION_EARLY_BOUND;
    out_region->data.parameter = generic->hir_id;
    return 1;
}

static int cm_lower_validate_generic_constraint(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstPathId ast_path_id,
    const CmAstGenericArg *argument)
{
    uint32_t index;

    if ((unsigned int)argument->kind
            > (unsigned int)CM_AST_GENERIC_CONSTRAINT) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, argument->span), ast_item_id,
            argument->type, ast_path_id, CM_HIR_OK,
            "generic argument has an invalid kind");
        return 0;
    }
    if (argument->kind == CM_AST_GENERIC_CONSTRAINT
        && (argument->span.start > argument->span.end
            || argument->name == CM_INTERN_ID_NONE
            || cm_lower_ast_string(state, argument->name) == NULL
            || argument->text == CM_INTERN_ID_NONE
            || cm_lower_ast_string(state, argument->text) == NULL
            || argument->type != CM_AST_TYPE_NONE
            || argument->bound_count == 0u
            || argument->bounds == NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, argument->span), ast_item_id,
            argument->type, ast_path_id, CM_HIR_OK,
            "associated-type constraint is structurally invalid");
        return 0;
    }
    if ((argument->name_argument_count != 0u
            && argument->name_arguments == NULL)
        || (argument->name_argument_count == 0u
            && argument->name_arguments != NULL)
        || (argument->name_argument_count != 0u
            && argument->kind != CM_AST_GENERIC_BINDING
            && argument->kind != CM_AST_GENERIC_CONSTRAINT)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, argument->span), ast_item_id,
            argument->type, ast_path_id, CM_HIR_OK,
            "generic associated-type name argument storage is invalid");
        return 0;
    }
    for (index = 0u; index < argument->name_argument_count; ++index) {
        const CmAstGenericArg *name_argument;
        int valid;

        name_argument = &argument->name_arguments[index];
        valid = name_argument->span.start <= name_argument->span.end
            && name_argument->span.start >= argument->span.start
            && name_argument->span.end <= argument->span.end
            && name_argument->name == CM_INTERN_ID_NONE
            && name_argument->name_argument_count == 0u
            && name_argument->name_arguments == NULL
            && name_argument->bound_count == 0u
            && name_argument->bounds == NULL;
        if (valid && name_argument->kind == CM_AST_GENERIC_TYPE) {
            valid = name_argument->type != CM_AST_TYPE_NONE
                && cm_ast_get_type(state->ast, name_argument->type) != NULL
                && name_argument->text == CM_INTERN_ID_NONE;
        } else if (valid
            && (name_argument->kind == CM_AST_GENERIC_LIFETIME
                || name_argument->kind == CM_AST_GENERIC_CONST)) {
            valid = name_argument->type == CM_AST_TYPE_NONE
                && name_argument->text != CM_INTERN_ID_NONE
                && cm_lower_ast_string(state, name_argument->text) != NULL;
        } else {
            valid = 0;
        }
        if (!valid) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, name_argument->span), ast_item_id,
                name_argument->type, ast_path_id, CM_HIR_OK,
                "generic associated-type name argument is malformed");
            return 0;
        }
    }
    if (argument->kind != CM_AST_GENERIC_CONSTRAINT) {
        if (argument->bound_count != 0u || argument->bounds != NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, argument->span), ast_item_id,
                argument->type, ast_path_id, CM_HIR_OK,
                "non-constraint generic argument has constraint bounds");
            return 0;
        }
        if (argument->kind == CM_AST_GENERIC_BINDING
            && argument->name_argument_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, argument->span), ast_item_id,
                argument->type, ast_path_id, CM_HIR_OK,
                "generic associated-type equality names are not supported "
                "in HIR");
            return 0;
        }
        return 1;
    }
    for (index = 0u; index < argument->bound_count; ++index) {
        const CmAstGenericParamBound *bound;
        const CmAstPath *path;
        const CmAstType *type;

        bound = &argument->bounds[index];
        if (bound->span.start > bound->span.end
            || bound->span.start < argument->span.start
            || bound->span.end > argument->span.end) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, bound->span), ast_item_id,
                bound->trait_type, ast_path_id, CM_HIR_OK,
                "associated-type constraint bound span is invalid");
            return 0;
        }
        if (bound->modifier != CM_AST_GENERIC_BOUND_REQUIRED
            && bound->modifier != CM_AST_GENERIC_BOUND_RELAXED
            && bound->modifier
                != CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, bound->span), ast_item_id,
                bound->trait_type, ast_path_id, CM_HIR_OK,
                "associated-type constraint bound has invalid modifier");
            return 0;
        }
        if (bound->kind == CM_AST_GENERIC_BOUND_LIFETIME) {
            if (bound->modifier != CM_AST_GENERIC_BOUND_REQUIRED
                || bound->trait_type != CM_AST_TYPE_NONE
                || bound->lifetime == CM_INTERN_ID_NONE
                || cm_lower_ast_string(state, bound->lifetime) == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type, ast_path_id, CM_HIR_OK,
                    "associated-type lifetime constraint is malformed");
                return 0;
            }
            continue;
        }
        type = cm_ast_get_type(state->ast, bound->trait_type);
        path = type == NULL || type->kind != CM_AST_TYPE_PATH ? NULL
            : cm_ast_get_path(state->ast, type->path);
        if (bound->kind != CM_AST_GENERIC_BOUND_TRAIT
            || bound->trait_type == CM_AST_TYPE_NONE
            || bound->lifetime != CM_INTERN_ID_NONE
            || type == NULL || type->kind != CM_AST_TYPE_PATH
            || !cm_lower_ast_path_storage_valid(path)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, bound->span), ast_item_id,
                bound->trait_type, ast_path_id, CM_HIR_OK,
                "associated-type trait constraint is malformed");
            return 0;
        }
    }
    return 1;
}

static int cm_lower_generic_arguments(CmLowerState *state,
    const CmAstPathSegment *segment, CmHirModuleId module, CmHirDefId owner,
    CmSpan span, CmHirGenericArg **out_arguments, uint32_t *out_count)
{
    CmHirGenericArg *arguments;
    uint32_t index;

    *out_arguments = NULL;
    *out_count = 0u;
    if (segment->argument_count == 0u) {
        return 1;
    }
    if (segment->arguments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "generic argument count has no argument storage");
        return 0;
    }
    arguments = (CmHirGenericArg *)cm_alloc_zeroed(
        (size_t)segment->argument_count, sizeof(CmHirGenericArg));
    for (index = 0u; index < segment->argument_count && !state->failed;
         ++index) {
        const CmAstGenericArg *ast_argument;

        ast_argument = &segment->arguments[index];
        if (!cm_lower_validate_generic_constraint(state, CM_AST_ITEM_NONE,
                CM_AST_PATH_NONE, ast_argument)) {
            break;
        }
        if (ast_argument->kind == CM_AST_GENERIC_TYPE) {
            arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
            arguments[index].data.type = cm_lower_type(state,
                ast_argument->type, module, owner);
        } else if (ast_argument->kind == CM_AST_GENERIC_LIFETIME) {
            arguments[index].kind = CM_HIR_GENERIC_ARG_LIFETIME;
            (void)cm_lower_lifetime(state, ast_argument->text, owner, span,
                &arguments[index].data.lifetime);
        } else {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "const and associated-type path arguments require typed "
                "generic syntax");
        }
    }
    if (state->failed) {
        cm_free(arguments);
        return 0;
    }
    *out_arguments = arguments;
    *out_count = segment->argument_count;
    return 1;
}

static int cm_lower_const_path_argument(CmLowerState *state,
    const CmAstGenericArg *ast_argument, CmHirDefId owner,
    const CmHirGenericParam *parameter, CmSpan span,
    CmHirGenericArg *out_argument)
{
    const CmAstType *ast_type;
    const CmAstPath *path;
    const CmLowerGenericRecord *generic;
    const CmHirGenericParam *source_parameter;
    const CmHirType *source_type;
    const CmHirType *target_type;
    int compatible;

    ast_type = cm_ast_get_type(state->ast, ast_argument->type);
    path = ast_type == NULL || ast_type->kind != CM_AST_TYPE_PATH ? NULL
        : cm_ast_get_path(state->ast, ast_type->path);
    if (path == NULL || path->absolute || path->segment_count != 1u
        || path->segments == NULL
        || path->segments[0].argument_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            CM_AST_ITEM_NONE, ast_argument->type, CM_AST_PATH_NONE,
            CM_HIR_OK,
            "const generic argument is not a plain authenticated const "
            "parameter path");
        return 0;
    }
    generic = cm_lower_find_generic_in_scope(state, owner,
        path->segments[0].name);
    source_parameter = generic == NULL ? NULL
        : cm_hir_get_generic_param(state->hir, generic->hir_id);
    source_type = source_parameter == NULL ? NULL
        : cm_hir_get_type(state->hir, source_parameter->declared_type);
    target_type = cm_hir_get_type(state->hir, parameter->declared_type);
    compatible = source_type != NULL && target_type != NULL
        && source_type->kind == target_type->kind
        && ((source_type->kind == CM_HIR_TYPE_INTEGER_KIND
                && source_type->data.integer_type.kind
                    == target_type->data.integer_type.kind)
            || (source_type->kind == CM_HIR_TYPE_BOOL_KIND
                || source_type->kind == CM_HIR_TYPE_CHAR_KIND));
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_CONST
        || source_parameter == NULL
        || source_parameter->kind != CM_HIR_GENERIC_CONST
        || !compatible) {
        cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
            CM_AST_ITEM_NONE, ast_argument->type, ast_type->path, CM_HIR_OK,
            "const generic argument does not name a type-compatible const "
            "parameter");
        return 0;
    }
    memset(out_argument, 0, sizeof(*out_argument));
    out_argument->kind = CM_HIR_GENERIC_ARG_CONST;
    out_argument->data.constant.kind = CM_HIR_CONST_PARAMETER;
    out_argument->data.constant.type = parameter->declared_type;
    out_argument->data.constant.data.parameter = generic->hir_id;
    return 1;
}

static int cm_lower_alias_signature(const CmLowerState *state,
    CmHirDefId definition, const CmLowerItemRecord *record,
    CmHirGenericParamId *out_start, uint32_t *out_count)
{
    const CmHirDefinition *hir_definition;
    const CmHirItem *item;

    *out_start = CM_HIR_GENERIC_PARAM_NONE;
    *out_count = 0u;
    if (record != NULL && cm_hir_def_id_equal(record->definition,
            definition)) {
        *out_start = record->generic_parameter_start;
        *out_count = record->generic_parameter_count;
        return 1;
    }
    hir_definition = cm_hir_lookup_definition(state->hir, definition);
    if (hir_definition == NULL
        || hir_definition->kind != CM_HIR_DEFINITION_ITEM
        || hir_definition->state != CM_HIR_DEFINITION_BOUND) {
        return 0;
    }
    item = cm_hir_get_item(state->hir, hir_definition->entity.item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_TYPE_ALIAS) {
        return 0;
    }
    *out_start = item->generic_parameter_start;
    *out_count = item->generic_parameter_count;
    return 1;
}

static int cm_lower_alias_arguments(CmLowerState *state,
    const CmAstPathSegment *segment, CmHirModuleId module, CmHirDefId owner,
    CmHirDefId alias_definition, const CmLowerItemRecord *record, CmSpan span,
    CmHirGenericArg **out_arguments, uint32_t *out_count)
{
    CmHirGenericArg *arguments;
    CmHirGenericParamId parameter_start;
    uint32_t parameter_count;
    uint32_t lifetime_count;
    uint32_t explicit_lifetime_count;
    uint32_t inferred_lifetime_count;
    uint32_t argument_count;
    uint32_t parameter_index;
    uint32_t explicit_index;

    *out_arguments = NULL;
    *out_count = 0u;
    if (!cm_lower_alias_signature(state, alias_definition, record,
            &parameter_start, &parameter_count)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_ALIAS, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "type alias has no bound generic signature");
        return 0;
    }
    lifetime_count = 0u;
    for (parameter_index = 0u; parameter_index < parameter_count;
         ++parameter_index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(state->hir,
            parameter_start + parameter_index);
        if (parameter == NULL
            || !cm_hir_def_id_equal(parameter->owner, alias_definition)
            || parameter->index != parameter_index) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_ALIAS, span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK, "type alias has an invalid generic signature");
            return 0;
        }
        if (parameter->kind == CM_HIR_GENERIC_LIFETIME) {
            if (parameter_index != lifetime_count) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_ALIAS, span,
                    CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "type alias lifetime parameters are not leading");
                return 0;
            }
            lifetime_count += 1u;
        }
    }
    if ((segment->argument_count == 0u) != (segment->arguments == NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "type alias generic argument count and storage disagree");
        return 0;
    }
    explicit_lifetime_count = 0u;
    for (explicit_index = 0u; explicit_index < segment->argument_count;
         ++explicit_index) {
        if (segment->arguments[explicit_index].kind
            == CM_AST_GENERIC_LIFETIME) {
            explicit_lifetime_count += 1u;
        }
    }
    inferred_lifetime_count = explicit_lifetime_count == 0u
        ? lifetime_count : 0u;
    if (segment->argument_count
            > UINT32_MAX - inferred_lifetime_count) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_ALIAS, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_ID_EXHAUSTED,
            "type alias generic argument count overflow");
        return 0;
    }
    argument_count = inferred_lifetime_count + segment->argument_count;
    if (argument_count > parameter_count) {
        cm_lower_fail(state, CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "type alias application supplies too many generic arguments");
        return 0;
    }
    arguments = argument_count == 0u ? NULL
        : (CmHirGenericArg *)cm_alloc_zeroed(
            (size_t)argument_count, sizeof(*arguments));
    for (parameter_index = 0u;
         parameter_index < inferred_lifetime_count;
         ++parameter_index) {
        arguments[parameter_index].kind = CM_HIR_GENERIC_ARG_LIFETIME;
        arguments[parameter_index].data.lifetime.kind = CM_HIR_REGION_INFER;
        arguments[parameter_index].data.lifetime.data.inference_variable =
            state->next_region_inference;
        state->next_region_inference += 1u;
    }
    for (explicit_index = 0u;
         explicit_index < segment->argument_count && !state->failed;
         ++explicit_index) {
        const CmAstGenericArg *ast_argument;
        const CmHirGenericParam *parameter;
        CmHirGenericArg *argument;

        ast_argument = &segment->arguments[explicit_index];
        parameter_index = inferred_lifetime_count + explicit_index;
        parameter = cm_hir_get_generic_param(state->hir,
            parameter_start + parameter_index);
        argument = &arguments[parameter_index];
        if (!cm_lower_validate_generic_constraint(state, CM_AST_ITEM_NONE,
                CM_AST_PATH_NONE, ast_argument)) {
            break;
        }
        if (parameter == NULL
            || !cm_hir_def_id_equal(parameter->owner, alias_definition)
            || parameter->index != parameter_index) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_ALIAS, span,
                CM_AST_ITEM_NONE, ast_argument->type, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "type alias has an invalid authenticated generic signature");
            break;
        }
        if (parameter->kind == CM_HIR_GENERIC_TYPE
            && ast_argument->kind == CM_AST_GENERIC_TYPE) {
            argument->kind = CM_HIR_GENERIC_ARG_TYPE;
            argument->data.type = cm_lower_type(state, ast_argument->type,
                module, owner);
        } else if (parameter->kind == CM_HIR_GENERIC_LIFETIME
            && ast_argument->kind == CM_AST_GENERIC_LIFETIME) {
            argument->kind = CM_HIR_GENERIC_ARG_LIFETIME;
            (void)cm_lower_lifetime(state, ast_argument->text, owner,
                cm_lower_span(state, ast_argument->span),
                &argument->data.lifetime);
        } else if (parameter->kind == CM_HIR_GENERIC_CONST
            && ast_argument->kind == CM_AST_GENERIC_TYPE) {
            (void)cm_lower_const_path_argument(state, ast_argument, owner,
                parameter, cm_lower_span(state, ast_argument->span),
                argument);
        } else if (parameter->kind == CM_HIR_GENERIC_CONST
            && ast_argument->kind == CM_AST_GENERIC_CONST) {
            const CmHirType *declared_type;
            uint64_t value;

            declared_type = cm_hir_get_type(state->hir,
                parameter->declared_type);
            if (!cm_lower_parse_u64(cm_lower_ast_string(state,
                    ast_argument->text), &value)
                || declared_type == NULL
                || declared_type->kind != CM_HIR_TYPE_INTEGER_KIND) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, ast_argument->span),
                    CM_AST_ITEM_NONE, ast_argument->type,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "const type-alias argument is not a supported integer "
                    "literal");
                break;
            }
            argument->kind = CM_HIR_GENERIC_ARG_CONST;
            argument->data.constant.kind = CM_HIR_CONST_VALUE;
            argument->data.constant.type = parameter->declared_type;
            argument->data.constant.data.value.low_bits = value;
            argument->data.constant.data.value.high_bits = 0u;
        } else {
            cm_lower_fail(state, CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH,
                cm_lower_span(state, ast_argument->span),
                CM_AST_ITEM_NONE, ast_argument->type, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "type alias generic argument kind differs from its "
                "parameter");
            break;
        }
    }
    if (state->failed) {
        cm_free(arguments);
        return 0;
    }
    *out_arguments = arguments;
    *out_count = argument_count;
    return 1;
}

static int cm_lower_adt_signature(const CmLowerState *state,
    CmHirDefId definition, const CmLowerItemRecord *record,
    CmHirGenericParamId *out_start, uint32_t *out_count)
{
    const CmHirItem *item;

    *out_start = CM_HIR_GENERIC_PARAM_NONE;
    *out_count = 0u;
    if (record != NULL && cm_hir_def_id_equal(record->definition,
            definition)) {
        if (record->kind != CM_AST_ITEM_STRUCT
            && record->kind != CM_AST_ITEM_UNION
            && record->kind != CM_AST_ITEM_ENUM) {
            return 0;
        }
        *out_start = record->generic_parameter_start;
        *out_count = record->generic_parameter_count;
        return 1;
    }
    item = cm_lower_bound_item(state, definition);
    if (item == NULL || (item->kind != CM_HIR_ITEM_STRUCT
            && item->kind != CM_HIR_ITEM_UNION
            && item->kind != CM_HIR_ITEM_ENUM)) {
        return 0;
    }
    *out_start = item->generic_parameter_start;
    *out_count = item->generic_parameter_count;
    return 1;
}

static int cm_lower_adt_default_argument(CmLowerState *state,
    CmHirDefId definition, CmHirGenericParamId parameter_start,
    uint32_t parameter_index, const CmHirGenericArg *prior_arguments,
    CmSpan span, CmHirGenericArg *out_argument)
{
    const CmHirGenericParam *parameter;
    const CmHirType *default_type;

    parameter = cm_hir_get_generic_param(state->hir,
        parameter_start + parameter_index);
    if (parameter == NULL
        || !cm_hir_def_id_equal(parameter->owner, definition)
        || parameter->index != parameter_index
        || parameter->kind != CM_HIR_GENERIC_TYPE
        || !parameter->has_default
        || parameter->default_argument.kind != CM_HIR_GENERIC_ARG_TYPE) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "ADT type application omits a required generic argument");
        return 0;
    }
    default_type = cm_hir_get_type(state->hir,
        parameter->default_argument.data.type);
    if (default_type == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVALID_ID,
            "ADT generic default references an invalid HIR type");
        return 0;
    }
    memset(out_argument, 0, sizeof(*out_argument));
    out_argument->kind = CM_HIR_GENERIC_ARG_TYPE;
    if (default_type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        const CmHirGenericParam *source;

        source = cm_hir_get_generic_param(state->hir,
            default_type->data.parameter_type.parameter);
        if (source != NULL
            && cm_hir_def_id_equal(source->owner, definition)
            && source->kind == CM_HIR_GENERIC_TYPE
            && source->index < parameter_index
            && prior_arguments[source->index].kind
                == CM_HIR_GENERIC_ARG_TYPE) {
            *out_argument = prior_arguments[source->index];
            return 1;
        }
    }
    /* A common ADT default pattern is a zero-argument function pointer whose
     * return type is the immediately preceding type parameter, e.g.
     * `LazyCell<T, F = fn() -> T>`.  Preserve the authenticated function
     * pointer shape while substituting that prior application argument. */
    if (default_type->kind == CM_HIR_TYPE_FN_POINTER_KIND
        && default_type->data.fn_pointer_type.parameter_count == 0u
        && default_type->data.fn_pointer_type.parameters == NULL
        && prior_arguments != NULL) {
        const CmHirType *return_type;
        const CmHirGenericParam *source;

        return_type = cm_hir_get_type(state->hir,
            default_type->data.fn_pointer_type.return_type);
        source = return_type == NULL
            || return_type->kind != CM_HIR_TYPE_PARAMETER_KIND
            ? NULL : cm_hir_get_generic_param(state->hir,
                return_type->data.parameter_type.parameter);
        if (source != NULL
            && cm_hir_def_id_equal(source->owner, definition)
            && source->kind == CM_HIR_GENERIC_TYPE
            && source->index < parameter_index
            && prior_arguments[source->index].kind
                == CM_HIR_GENERIC_ARG_TYPE) {
            CmHirType replacement;

            replacement = *default_type;
            replacement.data.fn_pointer_type.return_type =
                prior_arguments[source->index].data.type;
            out_argument->data.type = cm_lower_add_type(state, &replacement,
                CM_AST_TYPE_NONE);
            if (out_argument->data.type == CM_HIR_TYPE_NONE) return 0;
            return 1;
        }
    }
    switch (default_type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        out_argument->data.type = parameter->default_argument.data.type;
        return 1;
    default:
        break;
    }
    cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
        CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
        "ADT generic default requires unsupported structural substitution");
    return 0;
}

static int cm_lower_adt_arguments(CmLowerState *state,
    const CmAstPathSegment *segment, CmHirModuleId module, CmHirDefId owner,
    CmHirDefId definition, const CmLowerItemRecord *record, CmSpan span,
    CmHirGenericArg **out_arguments, uint32_t *out_count)
{
    CmHirGenericArg *explicit_arguments;
    CmHirGenericArg *arguments;
    CmHirGenericParamId parameter_start;
    uint32_t parameter_count;
    uint32_t explicit_count;
    uint32_t index;

    *out_arguments = NULL;
    *out_count = 0u;
    if (!cm_lower_adt_signature(state, definition, record,
            &parameter_start, &parameter_count)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVALID_ID, "ADT has no bound generic signature");
        return 0;
    }
    explicit_arguments = segment->argument_count == 0u ? NULL
        : (CmHirGenericArg *)cm_alloc_zeroed(
            (size_t)segment->argument_count, sizeof(*explicit_arguments));
    explicit_count = segment->argument_count;
    if ((segment->argument_count == 0u) != (segment->arguments == NULL)) {
        cm_free(explicit_arguments);
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "ADT generic argument count and storage disagree");
        return 0;
    }
    if (explicit_count > parameter_count) {
        cm_free(explicit_arguments);
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "ADT type application supplies too many generic arguments");
        return 0;
    }
    arguments = parameter_count == 0u ? NULL
        : (CmHirGenericArg *)cm_alloc_zeroed(
            (size_t)parameter_count, sizeof(*arguments));
    for (index = 0u; index < explicit_count; ++index) {
        const CmAstGenericArg *ast_argument;
        const CmHirGenericParam *parameter;

        ast_argument = &segment->arguments[index];
        if (!cm_lower_validate_generic_constraint(state, CM_AST_ITEM_NONE,
                CM_AST_PATH_NONE, ast_argument)) {
            cm_free(arguments);
            cm_free(explicit_arguments);
            return 0;
        }
        if (ast_argument->kind == CM_AST_GENERIC_BINDING
            || ast_argument->kind == CM_AST_GENERIC_CONSTRAINT) {
            cm_free(arguments);
            cm_free(explicit_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                CM_AST_ITEM_NONE, ast_argument->type, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "associated-type path arguments are not valid ADT "
                "positional arguments");
            return 0;
        }
        parameter = cm_hir_get_generic_param(state->hir,
            parameter_start + index);
        if (parameter == NULL
            || !cm_hir_def_id_equal(parameter->owner, definition)
            || parameter->index != index) {
            cm_free(arguments);
            cm_free(explicit_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "ADT has an invalid authenticated generic signature");
            return 0;
        }
        if (parameter->kind == CM_HIR_GENERIC_TYPE
            && ast_argument->kind == CM_AST_GENERIC_TYPE) {
            explicit_arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
            explicit_arguments[index].data.type = cm_lower_type(state,
                ast_argument->type, module, owner);
        } else if (parameter->kind == CM_HIR_GENERIC_LIFETIME
            && ast_argument->kind == CM_AST_GENERIC_LIFETIME) {
            explicit_arguments[index].kind = CM_HIR_GENERIC_ARG_LIFETIME;
            (void)cm_lower_lifetime(state, ast_argument->text, owner, span,
                &explicit_arguments[index].data.lifetime);
        } else if (parameter->kind == CM_HIR_GENERIC_CONST
            && ast_argument->kind == CM_AST_GENERIC_TYPE) {
            if (!cm_lower_const_path_argument(state, ast_argument, owner,
                    parameter, span, &explicit_arguments[index])) {
                cm_free(arguments);
                cm_free(explicit_arguments);
                return 0;
            }
        } else if (parameter->kind == CM_HIR_GENERIC_CONST
            && ast_argument->kind == CM_AST_GENERIC_CONST) {
            uint64_t value;
            const CmHirType *declared_type;

            declared_type = cm_hir_get_type(state->hir,
                parameter->declared_type);
            if (!cm_lower_parse_u64(cm_lower_ast_string(state,
                    ast_argument->text), &value)
                || declared_type == NULL
                || declared_type->kind != CM_HIR_TYPE_INTEGER_KIND) {
                cm_free(arguments);
                cm_free(explicit_arguments);
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    span, CM_AST_ITEM_NONE, ast_argument->type,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "const generic literal is not a supported integer "
                    "argument");
                return 0;
            }
            explicit_arguments[index].kind = CM_HIR_GENERIC_ARG_CONST;
            explicit_arguments[index].data.constant.kind =
                CM_HIR_CONST_VALUE;
            explicit_arguments[index].data.constant.type =
                parameter->declared_type;
            explicit_arguments[index].data.constant.data.value.low_bits =
                value;
            explicit_arguments[index].data.constant.data.value.high_bits = 0u;
        } else {
            cm_free(arguments);
            cm_free(explicit_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                CM_AST_ITEM_NONE, ast_argument->type, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "ADT generic argument kind differs from its parameter");
            return 0;
        }
        if (state->failed) {
            cm_free(arguments);
            cm_free(explicit_arguments);
            return 0;
        }
        arguments[index] = explicit_arguments[index];
    }
    cm_free(explicit_arguments);
    for (index = explicit_count; index < parameter_count && !state->failed;
         ++index) {
        if (!cm_lower_adt_default_argument(state, definition,
                parameter_start, index, arguments, span,
                &arguments[index])) {
            break;
        }
    }
    if (state->failed) {
        cm_free(arguments);
        return 0;
    }
    *out_arguments = arguments;
    *out_count = parameter_count;
    return 1;
}

static int cm_lower_external_definition_matches(
    const CmLowerState *state, const CmHirLowerResolution *resolution)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(state->hir,
        resolution->definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND) {
        return 0;
    }
    item = cm_hir_get_item(state->hir, definition->entity.item_id);
    if (item == NULL) {
        return 0;
    }
    switch (resolution->named_type_kind) {
    case CM_HIR_TYPE_ADT_KIND:
        return item->kind == CM_HIR_ITEM_STRUCT
            || item->kind == CM_HIR_ITEM_UNION
            || item->kind == CM_HIR_ITEM_ENUM;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
        return item->kind == CM_HIR_ITEM_FUNCTION;
    case CM_HIR_TYPE_FOREIGN_KIND:
        return item->kind == CM_HIR_ITEM_EXTERN_TYPE;
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
        return item->kind == CM_HIR_ITEM_TYPE_ALIAS;
    case CM_HIR_TYPE_OPAQUE_KIND:
        /* Opaque definitions have no dedicated item kind in this HIR yet. */
        return 1;
    default:
        return 0;
    }
}

static CmLowerLookupResult cm_lower_resolve_library_binding(
    const CmLowerState *state, const CmAstPath *path,
    CmHirLibraryBinding *out_binding)
{
    CmHirLibraryPathSegment *segments;
    uint32_t segment_index;
    size_t artifact_index;
    size_t match_count;
    int failed;

    memset(out_binding, 0, sizeof(*out_binding));
    if (state == NULL || path == NULL || path->segments == NULL
        || path->segment_count == 0u
        || state->options->dependency_library_count == 0u) {
        return CM_LOWER_LOOKUP_NOT_FOUND;
    }
    segments = (CmHirLibraryPathSegment *)cm_alloc_zeroed(
        (size_t)path->segment_count, sizeof(*segments));
    for (segment_index = 0u; segment_index < path->segment_count;
            ++segment_index) {
        const CmInternedString *name;

        name = cm_ast_get_string(state->ast,
            path->segments[segment_index].name);
        if (name == NULL || name->len == 0u) {
            cm_free(segments);
            return CM_LOWER_LOOKUP_RESOLVER_ERROR;
        }
        segments[segment_index].bytes = name->bytes;
        segments[segment_index].length = name->len;
    }
    if (path->segment_count == 1u) {
        int resolved;

        resolved = !path->absolute && state->graph != NULL
            && state->imports != NULL
            && state->graph_module != CM_MODULE_NONE
            && cm_lower_resolve_library_import(state->graph,
                state->graph_revision, state->imports, state->options,
                state->graph_module, (CmResolveItemRef){ 0u,
                    CM_AST_ITEM_NONE }, segments[0].bytes,
                segments[0].length, out_binding);
        cm_free(segments);
        return resolved ? CM_LOWER_LOOKUP_DEFINITION
                        : CM_LOWER_LOOKUP_NOT_FOUND;
    }
    match_count = 0u;
    failed = 0;
    for (artifact_index = 0u;
            artifact_index < state->options->dependency_library_count;
            ++artifact_index) {
        CmHirLibraryBinding binding;
        CmHirLibraryStatus status;

        memset(&binding, 0, sizeof(binding));
        status = cm_hir_library_artifact_lookup_binding(
            state->options->dependency_libraries[artifact_index], segments,
            (size_t)path->segment_count, &binding);
        if (status == CM_HIR_LIBRARY_OK) {
            *out_binding = binding;
            match_count += 1u;
        } else if (status != CM_HIR_LIBRARY_NOT_FOUND) {
            failed = 1;
        }
        if (!path->absolute && state->graph != NULL
            && state->imports != NULL
            && state->graph_module != CM_MODULE_NONE) {
            memset(&binding, 0, sizeof(binding));
            status = cm_hir_library_artifact_resolve_imported_binding(
                state->options->dependency_libraries[artifact_index],
                state->imports, state->graph, state->graph_revision,
                state->graph_module, &segments[0], &segments[1],
                (size_t)path->segment_count - 1u, &binding);
            if (status == CM_HIR_LIBRARY_OK) {
                *out_binding = binding;
                match_count += 1u;
            } else if (status != CM_HIR_LIBRARY_NOT_FOUND) {
                failed = 1;
            }
        }
    }
    cm_free(segments);
    if (failed || match_count > 1u) {
        memset(out_binding, 0, sizeof(*out_binding));
        return CM_LOWER_LOOKUP_RESOLVER_ERROR;
    }
    return match_count == 1u ? CM_LOWER_LOOKUP_DEFINITION
                             : CM_LOWER_LOOKUP_NOT_FOUND;
}

static CmLowerLookupResult cm_lower_lookup_trait_target(
    CmLowerState *state, const CmAstPath *path, CmHirModuleId module,
    CmLowerTraitTarget *out_target)
{
    const CmLowerItemRecord *record;
    const CmHirItem *item;
    CmHirLibraryBinding binding;
    CmHirDefId module_definition;
    CmLowerLookupResult lookup;

    memset(out_target, 0, sizeof(*out_target));
    out_target->definition = cm_hir_def_id_none();
    lookup = cm_lower_lookup_path(state, path, module,
        CM_HIR_LOWER_PATH_TYPE, &record, &module_definition);
    (void)module_definition;
    if (lookup == CM_LOWER_LOOKUP_TRAIT && record != NULL) {
        item = cm_lower_bound_item(state, record->definition);
        if (item != NULL && item->kind != CM_HIR_ITEM_TRAIT)
            return CM_LOWER_LOOKUP_RESOLVER_ERROR;
        out_target->definition = record->definition;
        out_target->generic_parameter_start =
            record->generic_parameter_start;
        out_target->generic_parameter_count =
            record->generic_parameter_count;
        out_target->item = item;
        out_target->local_record = record;
        return CM_LOWER_LOOKUP_TRAIT;
    }
    if (lookup != CM_LOWER_LOOKUP_NOT_FOUND) return lookup;
    lookup = cm_lower_resolve_library_binding(state, path, &binding);
    if (lookup != CM_LOWER_LOOKUP_DEFINITION) return lookup;
    if (binding.kind != CM_HIR_LIBRARY_BINDING_TRAIT)
        return CM_LOWER_LOOKUP_WRONG_NAMESPACE;
    item = cm_lower_bound_item(state, binding.definition);
    if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT)
        return CM_LOWER_LOOKUP_RESOLVER_ERROR;
    out_target->definition = item->definition;
    out_target->generic_parameter_start = item->generic_parameter_start;
    out_target->generic_parameter_count = item->generic_parameter_count;
    out_target->item = item;
    out_target->local_record = NULL;
    return CM_LOWER_LOOKUP_TRAIT;
}

static CmHirLowerResolution cm_lower_resolve_library_type(
    const CmLowerState *state, const CmAstPath *path)
{
    CmHirLowerResolution resolution;
    CmHirLibraryPathSegment *segments;
    uint32_t segment_index;
    size_t artifact_index;
    size_t match_count;
    int failed;

    memset(&resolution, 0, sizeof(resolution));
    resolution.kind = CM_HIR_LOWER_UNRESOLVED;
    if (state == NULL || path == NULL || path->segments == NULL
        || path->segment_count == 0u
        || state->options->dependency_library_count == 0u) {
        return resolution;
    }
    segments = (CmHirLibraryPathSegment *)cm_alloc_zeroed(
        (size_t)path->segment_count, sizeof(*segments));
    for (segment_index = 0u; segment_index < path->segment_count;
            ++segment_index) {
        const CmInternedString *name;

        name = cm_ast_get_string(state->ast,
            path->segments[segment_index].name);
        if (name == NULL || name->len == 0u) {
            cm_free(segments);
            resolution.kind = CM_HIR_LOWER_RESOLVER_ERROR;
            return resolution;
        }
        segments[segment_index].bytes = name->bytes;
        segments[segment_index].length = name->len;
    }
    match_count = 0u;
    failed = 0;
    if (path->segment_count == 1u) {
        CmHirLibraryBinding imported_binding;

        if (path->absolute || state->graph == NULL || state->imports == NULL
            || state->graph_module == CM_MODULE_NONE) {
            cm_free(segments);
            return resolution;
        }
        memset(&imported_binding, 0, sizeof(imported_binding));
        if (cm_lower_resolve_library_import(state->graph,
                state->graph_revision, state->imports, state->options,
                state->graph_module, (CmResolveItemRef){ 0u,
                    CM_AST_ITEM_NONE }, segments[0].bytes,
                segments[0].length, &imported_binding)
            && (imported_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
                || imported_binding.kind
                    == CM_HIR_LIBRARY_BINDING_PRIMITIVE)) {
            if (imported_binding.kind
                    == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
                resolution.kind = CM_HIR_LOWER_PRIMITIVE;
                resolution.primitive_kind =
                    imported_binding.primitive_kind;
            } else {
                resolution.kind = CM_HIR_LOWER_DEFINITION;
                resolution.definition = imported_binding.definition;
                resolution.named_type_kind = imported_binding.type_kind;
            }
        }
        cm_free(segments);
        return resolution;
    }
    for (artifact_index = 0u;
            artifact_index < state->options->dependency_library_count;
            ++artifact_index) {
        CmHirLibraryType resolved_type;
        CmHirLibraryStatus status;

        memset(&resolved_type, 0, sizeof(resolved_type));
        status = cm_hir_library_artifact_lookup_type(
            state->options->dependency_libraries[artifact_index], segments,
            (size_t)path->segment_count, &resolved_type);
        if (status == CM_HIR_LIBRARY_OK) {
            if (resolved_type.primitive_kind != CM_HIR_PRIMITIVE_NONE) {
                resolution.kind = CM_HIR_LOWER_PRIMITIVE;
                resolution.primitive_kind = resolved_type.primitive_kind;
            } else {
                resolution.kind = CM_HIR_LOWER_DEFINITION;
                resolution.definition = resolved_type.definition;
                resolution.named_type_kind = resolved_type.kind;
            }
            match_count += 1u;
        } else if (status != CM_HIR_LIBRARY_NOT_FOUND) {
            failed = 1;
        }
        if (!path->absolute && state->graph != NULL
            && state->imports != NULL
            && state->graph_module != CM_MODULE_NONE) {
            memset(&resolved_type, 0, sizeof(resolved_type));
            status = cm_hir_library_artifact_resolve_imported_type(
                state->options->dependency_libraries[artifact_index],
                state->imports, state->graph, state->graph_revision,
                state->graph_module, &segments[0], &segments[1],
                (size_t)path->segment_count - 1u, &resolved_type);
            if (status == CM_HIR_LIBRARY_OK) {
                if (resolved_type.primitive_kind
                        != CM_HIR_PRIMITIVE_NONE) {
                    resolution.kind = CM_HIR_LOWER_PRIMITIVE;
                    resolution.primitive_kind =
                        resolved_type.primitive_kind;
                } else {
                    resolution.kind = CM_HIR_LOWER_DEFINITION;
                    resolution.definition = resolved_type.definition;
                    resolution.named_type_kind = resolved_type.kind;
                }
                match_count += 1u;
            } else if (status != CM_HIR_LIBRARY_NOT_FOUND) {
                failed = 1;
            }
        }
    }
    cm_free(segments);
    if (failed || match_count > 1u) {
        memset(&resolution, 0, sizeof(resolution));
        resolution.kind = CM_HIR_LOWER_RESOLVER_ERROR;
    }
    return resolution;
}

static int cm_lower_self_context(const CmLowerState *state,
    CmHirDefId owner, CmHirDefId *out_self_owner,
    CmHirDefId *out_trait_definition)
{
    const CmLowerItemRecord *record;
    const CmHirItem *parent;

    *out_self_owner = cm_hir_def_id_none();
    *out_trait_definition = cm_hir_def_id_none();
    record = cm_lower_find_record_by_definition(state, owner);
    if (record == NULL) return 0;
    if ((record->kind == CM_AST_ITEM_FUNCTION
            || record->kind == CM_AST_ITEM_TYPE_ALIAS)
        && record->parent_kind != CM_LOWER_PARENT_NONE) {
        record = cm_lower_find_record_by_definition(state,
            record->parent_definition);
    }
    if (record == NULL || (record->kind != CM_AST_ITEM_TRAIT
            && record->kind != CM_AST_ITEM_IMPL)) {
        return 0;
    }
    parent = cm_lower_bound_item(state, record->definition);
    if (parent == NULL && record->kind == CM_AST_ITEM_TRAIT) {
        *out_self_owner = record->definition;
        *out_trait_definition = record->definition;
        return 1;
    }
    if (parent == NULL && record->kind == CM_AST_ITEM_IMPL
        && state->active_predicate_item != NULL
        && cm_hir_def_id_equal(state->active_predicate_item->definition,
            record->definition)
        && state->active_predicate_item->owner_module
            == record->owner_module
        && state->active_predicate_item->generic_parameter_start
            == record->generic_parameter_start
        && state->active_predicate_item->generic_parameter_count
            == record->generic_parameter_count) {
        const CmHirDefinition *definition;

        definition = cm_hir_lookup_definition(state->hir,
            record->definition);
        if (definition != NULL
            && definition->kind == CM_HIR_DEFINITION_ITEM
            && definition->state == CM_HIR_DEFINITION_RESERVED
            && definition->has_reserved_item_kind
            && definition->reserved_item_kind == CM_HIR_ITEM_IMPL) {
            /*
             * Impl generic predicates are lowered before the temporary impl
             * header can be bound.  The reserved DefId plus the active
             * predicate item is the authenticated owner for a bare Self;
             * the implemented-trait identity is intentionally unavailable at
             * this point, so Self::Assoc remains fail-closed below.
             */
            *out_self_owner = record->definition;
            return 1;
        }
    }
    if (parent == NULL && record->kind == CM_AST_ITEM_IMPL
        && state->active_item != NULL
        && state->active_item->kind == CM_HIR_ITEM_IMPL
        && cm_hir_def_id_equal(state->active_item->definition,
            record->definition)
        && state->active_item->owner_module == record->owner_module
        && state->active_item->generic_parameter_start
            == record->generic_parameter_start
        && state->active_item->generic_parameter_count
            == record->generic_parameter_count) {
        /* The impl header is lowered before its HIR item is bound.  During
         * that window the active item is the authenticated owner for a bare
         * `Self` in an implemented trait's generic arguments (for example,
         * `impl PartialEq<&Self> for CStr`). */
        *out_self_owner = record->definition;
        if (state->active_item->data.impl_item.has_trait) {
            *out_trait_definition = state->active_item
                ->data.impl_item.trait_type.definition;
        }
        return 1;
    }
    /*
     * Impl headers are lowered before the temporary impl item is bound into
     * the HIR context.  During that window the reserved record is
     * authenticated, and state->active_item identifies the same owner,
     * but cm_lower_bound_item() quite correctly returns NULL.  Permit a bare
     * Self in the impl's self/trait header; associated projections still
     * fail closed below because the implemented-trait identity is not bound
     * until cm_lower_impl_item() finishes the header.
     */
    if (parent == NULL && record->kind == CM_AST_ITEM_IMPL
        && state->active_item != NULL
        && cm_hir_def_id_equal(state->active_item->definition, owner)) {
        *out_self_owner = owner;
        return 1;
    }
    if (parent == NULL) return 0;
    *out_self_owner = record->definition;
    if (parent->kind == CM_HIR_ITEM_TRAIT) {
        *out_trait_definition = parent->definition;
        return 1;
    }
    if (parent->kind != CM_HIR_ITEM_IMPL
        || parent->data.impl_item.is_negative != 0) {
        return 0;
    }
    if (!parent->data.impl_item.has_trait) return 1;
    *out_trait_definition = parent->data.impl_item.trait_type.definition;
    return 1;
}

static CmHirTypeId cm_lower_self_type(CmLowerState *state,
    CmAstTypeId ast_type_id, CmSpan span, CmHirDefId self_owner)
{
    CmHirType type;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = span;
    type.data.self_type.owner = self_owner;
    return cm_lower_add_type(state, &type, ast_type_id);
}

static CmHirTypeId cm_lower_self_path_type(CmLowerState *state,
    CmAstTypeId ast_type_id, const CmAstPath *path, CmSpan span,
    CmHirModuleId module, CmHirDefId owner)
{
    CmHirDefId self_owner;
    CmHirDefId trait_definition;
    CmHirDefId associated_trait_definition;
    CmHirTypeId self_type;
    const CmHirItem *defining_trait;
    const CmHirItem *self_trait;
    const CmHirItem *associated_item;
    CmHirGenericArg *owned_trait_arguments;
    CmHirGenericArg *owned_associated_arguments;
    CmHirGenericParamId associated_parameter_start;
    uint32_t associated_parameter_count;
    CmLowerAssociatedTarget associated;
    CmHirType projection;
    CmHirTypeId result;
    uint32_t matches;
    uint32_t immediate_matches;
    uint32_t index;

    if (path->absolute || path->segment_count == 0u
        || !cm_lower_string_is(state, path->segments[0].name, "Self")) {
        return CM_HIR_TYPE_NONE;
    }
    if (!cm_lower_self_context(state, owner, &self_owner,
            &trait_definition)) {
        cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "Self is used outside a trait or impl item");
        return CM_HIR_TYPE_NONE;
    }
    /*
     * In an impl header, Self in the implemented trait's generic
     * arguments denotes the concrete impl target (e.g. impl Trait<&Self>
     * for CStr), not the eventual CM_HIR_TYPE_SELF_KIND placeholder used
     * by methods.  The temporary impl item has already lowered its
     * self_type before cm_lower_trait_reference() processes those arguments.
     */
    if (path->segment_count == 1u
        && state->active_item != NULL
        && state->active_item->kind == CM_HIR_ITEM_IMPL
        && cm_hir_def_id_equal(state->active_item->definition, owner)
        && state->active_item->data.impl_item.self_type
            != CM_HIR_TYPE_NONE) {
        return state->active_item->data.impl_item.self_type;
    }
    if (path->segment_count > 2u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "Self paths beyond one associated type are not supported");
        return CM_HIR_TYPE_NONE;
    }
    if (path->segments[0].argument_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "generic arguments on Self itself are not supported");
        return CM_HIR_TYPE_NONE;
    }
    if (path->segment_count == 2u
        && cm_hir_def_id_is_none(trait_definition)) {
        cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "Self associated-type projection requires an authenticated "
            "implemented trait");
        return CM_HIR_TYPE_NONE;
    }
    self_type = cm_lower_self_type(state, ast_type_id, span, self_owner);
    if (state->failed || path->segment_count == 1u) return self_type;
    cm_lower_find_inherited_associated_type(state, trait_definition,
        path->segments[1].name, &associated, &matches);
    if (matches != 1u) {
        cm_lower_fail(state,
            matches == 0u ? CM_HIR_LOWER_UNRESOLVED_PATH
                          : CM_HIR_LOWER_INVALID_AST,
            span, CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
            CM_HIR_OK,
            matches == 0u
                ? "trait has no associated type named by Self projection"
                : "Self associated-type identity is ambiguous");
        return CM_HIR_TYPE_NONE;
    }
    associated_trait_definition = associated.item != NULL
        ? associated.item->parent_definition
        : associated.local_record != NULL
            ? associated.local_record->parent_definition
            : cm_hir_def_id_none();
    if (cm_hir_def_id_is_none(associated_trait_definition)) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "Self associated-type projection lost its defining trait");
        return CM_HIR_TYPE_NONE;
    }
    defining_trait = cm_lower_bound_item(state,
        associated_trait_definition);
    self_trait = cm_lower_bound_item(state, trait_definition);
    associated_item = associated.item != NULL ? associated.item
        : cm_lower_bound_item(state, associated.definition);
    if (defining_trait == NULL
        || defining_trait->kind != CM_HIR_ITEM_TRAIT
        || self_trait == NULL || self_trait->kind != CM_HIR_ITEM_TRAIT
        || (associated_item == NULL && associated.local_record == NULL)
        || (associated_item != NULL
            && (associated_item->kind != CM_HIR_ITEM_TYPE_ALIAS
                || associated_item->data.type_alias_item.target
                    != CM_HIR_TYPE_NONE
                || !cm_hir_def_id_equal(
                    associated_item->parent_definition,
                    associated_trait_definition)))
        || (associated_item == NULL
            && (associated.local_record->kind != CM_AST_ITEM_TYPE_ALIAS
                || !cm_hir_def_id_equal(
                    associated.local_record->parent_definition,
                    associated_trait_definition)))) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "Self associated-type projection has no authenticated bound "
            "declaration");
        return CM_HIR_TYPE_NONE;
    }
    associated_parameter_start = associated_item != NULL
        ? associated_item->generic_parameter_start
        : associated.local_record->generic_parameter_start;
    associated_parameter_count = associated_item != NULL
        ? associated_item->generic_parameter_count
        : associated.local_record->generic_parameter_count;
    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_HIR_TYPE_PROJECTION_KIND;
    projection.span = span;
    projection.data.projection_type.self_type = self_type;
    projection.data.projection_type.trait_type.definition =
        associated_trait_definition;
    projection.data.projection_type.associated_type.definition =
        associated.definition;
    owned_trait_arguments = NULL;
    owned_associated_arguments = NULL;
    if (defining_trait->generic_parameter_count != 0u
        && cm_hir_def_id_equal(associated_trait_definition,
            trait_definition)) {
        if (!cm_lower_trait_identity_arguments(state, CM_AST_ITEM_NONE,
                defining_trait, span, &owned_trait_arguments,
                &projection.data.projection_type.trait_type
                    .argument_count)) {
            return CM_HIR_TYPE_NONE;
        }
        projection.data.projection_type.trait_type.arguments =
            owned_trait_arguments;
    } else if (defining_trait->generic_parameter_count != 0u) {
        immediate_matches = 0u;
        for (index = 0u;
             index < self_trait->data.trait_item.supertrait_count;
             ++index) {
            const CmHirNamedType *supertrait;

            supertrait = &self_trait->data.trait_item.supertraits[index]
                .trait_type;
            if (!cm_hir_def_id_equal(supertrait->definition,
                    associated_trait_definition)) {
                continue;
            }
            projection.data.projection_type.trait_type = *supertrait;
            immediate_matches += 1u;
        }
        if (immediate_matches == 0u) {
            for (index = 0u;
                 index < self_trait->data.trait_item.supertrait_count;
                 ++index) {
                CmHirGenericArg *transitive_arguments;
                uint32_t transitive_argument_count;
                uint32_t transitive_matches;

                transitive_arguments = NULL;
                transitive_argument_count = 0u;
                transitive_matches = 0u;
                if (!cm_lower_find_instantiated_supertrait(state,
                        &self_trait->data.trait_item.supertraits[index]
                            .trait_type,
                        associated_trait_definition, self_type, span,
                        &transitive_arguments, &transitive_argument_count,
                        &transitive_matches)) {
                    cm_free(owned_trait_arguments);
                    return CM_HIR_TYPE_NONE;
                }
                if (transitive_matches != 0u) {
                    if (immediate_matches == 0u) {
                        owned_trait_arguments = transitive_arguments;
                        projection.data.projection_type.trait_type.arguments =
                            owned_trait_arguments;
                        projection.data.projection_type.trait_type
                            .argument_count = transitive_argument_count;
                    } else {
                        cm_free(transitive_arguments);
                    }
                    if (immediate_matches
                            > UINT32_MAX - transitive_matches) {
                        immediate_matches = UINT32_MAX;
                    } else {
                        immediate_matches += transitive_matches;
                    }
                }
            }
        }
        if (immediate_matches != 1u
            || projection.data.projection_type.trait_type.argument_count
                != defining_trait->generic_parameter_count) {
            cm_free(owned_trait_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "Self associated-type projection has an ambiguous or "
                "invalid generic supertrait path");
            return CM_HIR_TYPE_NONE;
        }
    }
    if (!cm_lower_generic_arguments(state, &path->segments[1], module,
            owner, span, &owned_associated_arguments,
            &projection.data.projection_type.associated_type
                .argument_count)) {
        cm_free(owned_trait_arguments);
        return CM_HIR_TYPE_NONE;
    }
    if (associated_item == NULL
        && projection.data.projection_type.associated_type.argument_count
            != 0u) {
        cm_free(owned_associated_arguments);
        cm_free(owned_trait_arguments);
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "generic Self associated-type projection was not bound before "
            "use");
        return CM_HIR_TYPE_NONE;
    }
    if (associated_parameter_count != 0u
        && (associated_parameter_start == CM_HIR_GENERIC_PARAM_NONE
            || associated_parameter_start
                > UINT32_MAX - (associated_parameter_count - 1u))) {
        cm_free(owned_associated_arguments);
        cm_free(owned_trait_arguments);
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "Self associated-type projection generic signature overflows");
        return CM_HIR_TYPE_NONE;
    }
    if (projection.data.projection_type.associated_type.argument_count
            != associated_parameter_count) {
        cm_free(owned_associated_arguments);
        cm_free(owned_trait_arguments);
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "Self associated-type projection has the wrong generic "
            "argument count");
        return CM_HIR_TYPE_NONE;
    }
    for (index = 0u; index < associated_parameter_count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected_kind;

        parameter = cm_hir_get_generic_param(state->hir,
            associated_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                associated.definition)) {
            cm_free(owned_associated_arguments);
            cm_free(owned_trait_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "Self associated-type projection has an invalid generic "
                "signature");
            return CM_HIR_TYPE_NONE;
        }
        expected_kind = parameter->kind == CM_HIR_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_ARG_LIFETIME
            : parameter->kind == CM_HIR_GENERIC_TYPE
                ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (owned_associated_arguments[index].kind != expected_kind) {
            cm_free(owned_associated_arguments);
            cm_free(owned_trait_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "Self associated-type projection generic argument kind "
                "does not match its parameter");
            return CM_HIR_TYPE_NONE;
        }
    }
    projection.data.projection_type.associated_type.arguments =
        owned_associated_arguments;
    result = cm_lower_add_type(state, &projection, ast_type_id);
    cm_free(owned_associated_arguments);
    cm_free(owned_trait_arguments);
    return result;
}

static int cm_lower_projection_region_equal(const CmHirRegion *left,
    const CmHirRegion *right)
{
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return 0;
    }
    switch (left->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
        return left->data.parameter == right->data.parameter;
    case CM_HIR_REGION_LATE_BOUND:
        return left->data.binder_index == right->data.binder_index;
    case CM_HIR_REGION_INFER:
        return left->data.inference_variable
            == right->data.inference_variable;
    case CM_HIR_REGION_ERROR:
        return 0;
    }
    return 0;
}

static int cm_lower_projection_type_equal(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id, size_t depth);

static int cm_lower_projection_const_equal(const CmHirContext *hir,
    const CmHirConstArg *left, const CmHirConstArg *right, size_t depth)
{
    if (left == NULL || right == NULL || left->kind != right->kind
        || !cm_lower_projection_type_equal(hir, left->type, right->type,
            depth + 1u)) {
        return 0;
    }
    switch (left->kind) {
    case CM_HIR_CONST_VALUE:
        return left->data.value.low_bits == right->data.value.low_bits
            && left->data.value.high_bits == right->data.value.high_bits;
    case CM_HIR_CONST_PARAMETER:
        return left->data.parameter == right->data.parameter;
    case CM_HIR_CONST_UNEVALUATED:
        return cm_hir_def_id_equal(left->data.definition,
            right->data.definition);
    case CM_HIR_CONST_INFER:
        return left->data.inference_variable
            == right->data.inference_variable;
    case CM_HIR_CONST_ERROR:
        return 0;
    }
    return 0;
}

static int cm_lower_projection_generic_arg_equal(const CmHirContext *hir,
    const CmHirGenericArg *left, const CmHirGenericArg *right, size_t depth)
{
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return 0;
    }
    switch (left->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_lower_projection_region_equal(&left->data.lifetime,
            &right->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_lower_projection_type_equal(hir, left->data.type,
            right->data.type, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_lower_projection_const_equal(hir, &left->data.constant,
            &right->data.constant, depth + 1u);
    }
    return 0;
}

static int cm_lower_projection_named_type_equal(const CmHirContext *hir,
    const CmHirNamedType *left, const CmHirNamedType *right, size_t depth)
{
    uint32_t index;

    if (left == NULL || right == NULL
        || !cm_hir_def_id_equal(left->definition, right->definition)
        || left->argument_count != right->argument_count
        || (left->argument_count != 0u && left->arguments == NULL)
        || (right->argument_count != 0u && right->arguments == NULL)) {
        return 0;
    }
    for (index = 0u; index < left->argument_count; ++index) {
        if (!cm_lower_projection_generic_arg_equal(hir,
                &left->arguments[index], &right->arguments[index],
                depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

/*
 * Supertrait substitution can allocate a fresh HIR type for each path.
 * Candidate identity must therefore compare the resulting type trees, not
 * their arena IDs.  Unsupported or incomplete shapes deliberately compare
 * unequal so ambiguity is preserved rather than guessed away.
 */
static int cm_lower_projection_type_equal(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id, size_t depth)
{
    const CmHirType *left;
    const CmHirType *right;
    uint32_t index;

    if (hir == NULL || depth > hir->types.len) return 0;
    left = cm_hir_get_type(hir, left_id);
    right = cm_hir_get_type(hir, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return 0;
    }
    switch (left->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        return left->data.integer_type.kind == right->data.integer_type.kind;
    case CM_HIR_TYPE_FLOAT_KIND:
        return left->data.float_type.kind == right->data.float_type.kind;
    case CM_HIR_TYPE_SELF_KIND:
        return cm_hir_def_id_equal(left->data.self_type.owner,
            right->data.self_type.owner);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return left->data.parameter_type.parameter
            == right->data.parameter_type.parameter;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return left->data.reference_type.mutability
                == right->data.reference_type.mutability
            && cm_lower_projection_region_equal(
                &left->data.reference_type.region,
                &right->data.reference_type.region)
            && cm_lower_projection_type_equal(hir,
                left->data.reference_type.pointee,
                right->data.reference_type.pointee, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return left->data.raw_pointer_type.mutability
                == right->data.raw_pointer_type.mutability
            && cm_lower_projection_type_equal(hir,
                left->data.raw_pointer_type.pointee,
                right->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if (left->data.tuple_type.element_count
                != right->data.tuple_type.element_count
            || (left->data.tuple_type.element_count != 0u
                && (left->data.tuple_type.elements == NULL
                    || right->data.tuple_type.elements == NULL))) {
            return 0;
        }
        for (index = 0u; index < left->data.tuple_type.element_count;
             ++index) {
            if (!cm_lower_projection_type_equal(hir,
                    left->data.tuple_type.elements[index],
                    right->data.tuple_type.elements[index], depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_lower_projection_type_equal(hir,
                left->data.array_type.element,
                right->data.array_type.element, depth + 1u)
            && cm_lower_projection_const_equal(hir,
                &left->data.array_type.length,
                &right->data.array_type.length, depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_lower_projection_type_equal(hir,
            left->data.slice_type.element,
            right->data.slice_type.element, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (left->data.fn_pointer_type.parameter_count
                != right->data.fn_pointer_type.parameter_count
            || left->data.fn_pointer_type.abi
                != right->data.fn_pointer_type.abi
            || left->data.fn_pointer_type.safety
                != right->data.fn_pointer_type.safety
            || left->data.fn_pointer_type.is_variadic
                != right->data.fn_pointer_type.is_variadic
            || (left->data.fn_pointer_type.parameter_count != 0u
                && (left->data.fn_pointer_type.parameters == NULL
                    || right->data.fn_pointer_type.parameters == NULL))) {
            return 0;
        }
        for (index = 0u;
             index < left->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_lower_projection_type_equal(hir,
                    left->data.fn_pointer_type.parameters[index],
                    right->data.fn_pointer_type.parameters[index],
                    depth + 1u)) {
                return 0;
            }
        }
        return cm_lower_projection_type_equal(hir,
            left->data.fn_pointer_type.return_type,
            right->data.fn_pointer_type.return_type, depth + 1u);
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_lower_projection_named_type_equal(hir,
            &left->data.named_type, &right->data.named_type, depth + 1u);
    case CM_HIR_TYPE_CLOSURE_KIND:
        return left->data.closure_type.closure
            == right->data.closure_type.closure;
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_lower_projection_type_equal(hir,
                left->data.projection_type.self_type,
                right->data.projection_type.self_type, depth + 1u)
            && cm_lower_projection_named_type_equal(hir,
                &left->data.projection_type.trait_type,
                &right->data.projection_type.trait_type, depth + 1u)
            && cm_lower_projection_named_type_equal(hir,
                &left->data.projection_type.associated_type,
                &right->data.projection_type.associated_type, depth + 1u);
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        return 0;
    }
    return 0;
}

static int cm_lower_parameter_projection_defining_trait(
    CmLowerState *state, const CmHirTraitPredicate *predicate,
    const CmLowerAssociatedTarget *associated, CmHirNamedType *out_trait,
    CmHirGenericArg **out_owned_arguments, uint32_t *out_matches)
{
    CmHirDefId defining_trait;

    memset(out_trait, 0, sizeof(*out_trait));
    *out_owned_arguments = NULL;
    *out_matches = 0u;
    defining_trait = associated->trait_definition;
    if (cm_hir_def_id_is_none(defining_trait)) return 1;
    if (cm_hir_def_id_equal(defining_trait,
            predicate->trait_type.definition)) {
        *out_trait = predicate->trait_type;
        *out_matches = 1u;
        return 1;
    }
    if (!cm_lower_find_instantiated_supertrait(state,
            &predicate->trait_type, defining_trait, predicate->subject,
            predicate->span, out_owned_arguments,
            &out_trait->argument_count, out_matches)) {
        return 0;
    }
    out_trait->definition = defining_trait;
    out_trait->arguments = *out_owned_arguments;
    return 1;
}

static int cm_lower_parameter_projection_candidates_equal(
    CmLowerState *state, const CmHirTraitPredicate *left_predicate,
    const CmLowerAssociatedTarget *left_associated,
    const CmHirTraitPredicate *right_predicate,
    const CmLowerAssociatedTarget *right_associated)
{
    CmHirNamedType left_trait;
    CmHirNamedType right_trait;
    CmHirGenericArg *left_owned;
    CmHirGenericArg *right_owned;
    uint32_t left_matches;
    uint32_t right_matches;
    int equal;

    if (!cm_hir_def_id_equal(left_associated->definition,
            right_associated->definition)) {
        return 0;
    }
    if (!cm_lower_parameter_projection_defining_trait(state,
            left_predicate, left_associated, &left_trait, &left_owned,
            &left_matches)) {
        return 0;
    }
    if (!cm_lower_parameter_projection_defining_trait(state,
            right_predicate, right_associated, &right_trait, &right_owned,
            &right_matches)) {
        cm_free(left_owned);
        return 0;
    }
    equal = left_matches == 1u && right_matches == 1u
        && cm_lower_projection_named_type_equal(state->hir,
            &left_trait, &right_trait, 0u);
    cm_free(right_owned);
    cm_free(left_owned);
    return equal;
}

static void cm_lower_find_parameter_projection_in_item(
    CmLowerState *state, const CmHirItem *item,
    CmHirGenericParamId parameter, CmInternId associated_name,
    const CmHirTraitPredicate **out_predicate,
    CmLowerAssociatedTarget *out_associated, uint32_t *in_out_matches)
{
    uint32_t index;

    if (item == NULL) return;
    for (index = 0u; index < item->predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;
        const CmHirType *subject;
        CmLowerAssociatedTarget associated;
        uint32_t associated_matches;

        predicate = &item->predicates[index];
        subject = cm_hir_get_type(state->hir, predicate->subject);
        if (subject == NULL
            || subject->kind != CM_HIR_TYPE_PARAMETER_KIND
            || subject->data.parameter_type.parameter != parameter) {
            continue;
        }
        cm_lower_find_inherited_associated_type(state,
            predicate->trait_type.definition, associated_name,
            &associated, &associated_matches);
        if (associated_matches == 0u) continue;
        if (associated_matches != 1u) {
            *in_out_matches = 2u;
            return;
        }
        if (*in_out_matches == 0u || *out_predicate == NULL) {
            *in_out_matches = 1u;
            *out_predicate = predicate;
            *out_associated = associated;
        } else if (*in_out_matches != 1u
            || !cm_lower_parameter_projection_candidates_equal(state,
                *out_predicate, out_associated, predicate, &associated)) {
            *in_out_matches = 2u;
            return;
        }
        if (state->failed) return;
    }
}

/*
 * Associated constraints can mention a later generic parameter before that
 * parameter's own bound has been materialized in the temporary HIR item.
 * Keep shorthand projection lookup order-independent by consulting the
 * authenticated AST declaration as a bounded fallback.  This is deliberately
 * limited to direct type-parameter and where-clause trait bounds; once HIR
 * predicates exist, the ordinary path above remains authoritative.
 */
static int cm_lower_find_parameter_projection_in_ast(
    CmLowerState *state, const CmLowerItemRecord *record,
    const CmLowerGenericRecord *generic, CmHirTypeId subject,
    CmInternId associated_name,
    CmHirTraitPredicate *out_predicate)
{
    const CmAstItem *ast_item;
    const CmAstGenericParamBound *generic_bound;
    const CmAstWherePredicate *matching_where_predicate;
    const CmAstWhereBound *where_bound;
    uint32_t candidates;
    uint32_t parameter_index;
    uint32_t bound_index;
    uint32_t where_index;
    int subject_matches;

    if (record == NULL || generic == NULL || out_predicate == NULL
        || record->ast == NULL) return 0;
    ast_item = cm_ast_get_item(record->ast, record->ast_id);
    if (ast_item == NULL) return 0;
    generic_bound = NULL;
    matching_where_predicate = NULL;
    where_bound = NULL;
    candidates = 0u;
    for (parameter_index = 0u;
         parameter_index < ast_item->generic_parameter_count;
         ++parameter_index) {
        const CmAstGenericParam *parameter;

        parameter = &ast_item->generic_parameters[parameter_index];
        if (parameter->kind != CM_AST_PARAM_TYPE
            || !cm_lower_strings_equal(state, parameter->name,
                generic->ast_name)) continue;
        for (bound_index = 0u; bound_index < parameter->bound_count;
             ++bound_index) {
            const CmAstGenericParamBound *bound;
            const CmAstType *trait_ast;
            const CmAstPath *trait_path;
            CmLowerTraitTarget trait_target;
            CmLowerAssociatedTarget associated;
            uint32_t associated_matches;

            bound = &parameter->bounds[bound_index];
            if (bound->kind != CM_AST_GENERIC_BOUND_TRAIT
                || bound->trait_type == CM_AST_TYPE_NONE) continue;
            trait_ast = cm_ast_get_type(record->ast, bound->trait_type);
            trait_path = trait_ast == NULL
                    || trait_ast->kind != CM_AST_TYPE_PATH
                ? NULL : cm_ast_get_path(record->ast, trait_ast->path);
            memset(&trait_target, 0, sizeof(trait_target));
            if (trait_path == NULL
                || cm_lower_lookup_trait_target(state, trait_path,
                    record->owner_module, &trait_target)
                    != CM_LOWER_LOOKUP_TRAIT) continue;
            cm_lower_find_inherited_associated_type(state,
                trait_target.definition, associated_name, &associated,
                &associated_matches);
            if (associated_matches == 0u) continue;
            if (candidates == 0u) generic_bound = bound;
            candidates += 1u;
        }
    }
    for (where_index = 0u;
         where_index < ast_item->where_predicate_count;
         ++where_index) {
        const CmAstType *subject_type;
        const CmAstPath *subject_path;

        const CmAstWherePredicate *where_predicate;

        where_predicate = &ast_item->where_predicates[where_index];
        if (where_predicate->kind != CM_AST_WHERE_PREDICATE_TYPE) {
            continue;
        }
        subject_type = cm_ast_get_type(record->ast,
            where_predicate->subject);
        subject_path = subject_type == NULL
                || subject_type->kind != CM_AST_TYPE_PATH
            ? NULL : cm_ast_get_path(record->ast, subject_type->path);
        subject_matches = subject_path != NULL
            && !subject_path->absolute
            && subject_path->segment_count == 1u
            && subject_path->segments != NULL
            && subject_path->segments[0].argument_count == 0u
            && cm_lower_strings_equal(state,
                subject_path->segments[0].name, generic->ast_name);
        if (!subject_matches) continue;
        for (bound_index = 0u;
             bound_index < where_predicate->bound_count; ++bound_index) {
            const CmAstWhereBound *bound;
            const CmAstType *trait_ast;
            const CmAstPath *trait_path;
            CmLowerTraitTarget trait_target;
            CmLowerAssociatedTarget associated;
            uint32_t associated_matches;

            bound = &where_predicate->bounds[bound_index];
            if (bound->kind != CM_AST_WHERE_BOUND_TRAIT
                || bound->trait_type == CM_AST_TYPE_NONE) continue;
            trait_ast = cm_ast_get_type(record->ast, bound->trait_type);
            trait_path = trait_ast == NULL
                    || trait_ast->kind != CM_AST_TYPE_PATH
                ? NULL : cm_ast_get_path(record->ast, trait_ast->path);
            memset(&trait_target, 0, sizeof(trait_target));
            if (trait_path == NULL
                || cm_lower_lookup_trait_target(state, trait_path,
                    record->owner_module, &trait_target)
                    != CM_LOWER_LOOKUP_TRAIT) continue;
            cm_lower_find_inherited_associated_type(state,
                trait_target.definition, associated_name, &associated,
                &associated_matches);
            if (associated_matches == 0u) continue;
            if (candidates == 0u) {
                where_bound = bound;
                matching_where_predicate = where_predicate;
            }
            candidates += 1u;
        }
    }
    if (candidates != 1u) return candidates == 0u ? 0 : -1;
    if (generic_bound != NULL) {
        return cm_lower_one_trait_predicate(state, record->ast_id, record,
            subject, generic_bound->trait_type,
            cm_lower_span(state, generic_bound->span), NULL,
            generic_bound->modifier == CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST
                ? CM_HIR_PREDICATE_CONST_IF_CONST
                : CM_HIR_PREDICATE_REQUIRED, out_predicate);
    }
    if (where_bound != NULL && matching_where_predicate != NULL) {
        return cm_lower_one_trait_predicate(state, record->ast_id, record,
            subject, where_bound->trait_type,
            cm_lower_span(state, where_bound->span),
            &matching_where_predicate->binder,
            where_bound->modifier == CM_AST_WHERE_BOUND_CONDITIONALLY_CONST
                ? CM_HIR_PREDICATE_CONST_IF_CONST
                : where_bound->modifier == CM_AST_WHERE_BOUND_CONST
                    ? CM_HIR_PREDICATE_CONST
                    : CM_HIR_PREDICATE_REQUIRED, out_predicate);
    }
    return 0;
}

static CmHirTypeId cm_lower_parameter_projection_type(CmLowerState *state,
    CmAstTypeId ast_type_id, const CmAstPath *path, CmSpan span,
    CmHirModuleId module, CmHirDefId owner,
    const CmLowerGenericRecord *generic)
{
    const CmHirTraitPredicate *predicate;
    CmLowerAssociatedTarget associated;
    const CmLowerItemRecord *owner_record;
    const CmHirItem *parent_item;
    CmHirType parameter_type;
    CmHirType projection;
    CmHirTypeId self_type;
    CmHirNamedType projection_trait;
    CmHirGenericArg *owned_trait_arguments;
    CmHirGenericArg *owned_associated_arguments;
    CmHirGenericParamId associated_parameter_start;
    uint32_t associated_parameter_count;
    CmHirDefId defining_trait;
    const CmHirItem *associated_item;
    CmHirTraitPredicate synthesized_predicate;
    int synthesized;
    uint32_t index;
    uint32_t projection_argument_count;
    uint32_t projection_matches;
    uint32_t matches;

    predicate = NULL;
    memset(&associated, 0, sizeof(associated));
    associated.definition = cm_hir_def_id_none();
    memset(&synthesized_predicate, 0, sizeof(synthesized_predicate));
    synthesized = 0;
    matches = 0u;
    if (state->active_item != NULL
        && cm_hir_def_id_equal(state->active_item->definition, owner)) {
        cm_lower_find_parameter_projection_in_item(state,
            state->active_item, generic->hir_id, path->segments[1].name,
            &predicate, &associated, &matches);
    }
    owner_record = cm_lower_find_record_by_definition(state, owner);
    parent_item = NULL;
    if (owner_record != NULL
        && (owner_record->parent_kind == CM_LOWER_PARENT_TRAIT
            || owner_record->parent_kind == CM_LOWER_PARENT_IMPL)) {
        parent_item = cm_lower_bound_item(state,
            owner_record->parent_definition);
        cm_lower_find_parameter_projection_in_item(state, parent_item,
            generic->hir_id, path->segments[1].name, &predicate,
            &associated, &matches);
    }
    if (matches == 0u && predicate == NULL) {
        int ast_matches;

        ast_matches = cm_lower_find_parameter_projection_in_ast(state,
            owner_record, generic, CM_HIR_TYPE_NONE,
            path->segments[1].name,
            &synthesized_predicate);
        if (ast_matches < 0) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "shorthand associated type is ambiguous across "
                "declaration predicates");
            return CM_HIR_TYPE_NONE;
        }
        if (ast_matches > 0) {
            predicate = &synthesized_predicate;
            cm_lower_find_inherited_associated_type(state,
                predicate->trait_type.definition, path->segments[1].name,
                &associated, &matches);
            synthesized = 1;
        }
    }
    if (matches == 0u || predicate == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "type parameter shadows this path prefix and has no unique "
            "predicate for shorthand associated type resolution");
        return CM_HIR_TYPE_NONE;
    }
    if (matches != 1u) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "shorthand associated type is ambiguous across predicates or "
            "supertraits");
        return CM_HIR_TYPE_NONE;
    }
    memset(&parameter_type, 0, sizeof(parameter_type));
    parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type.span = span;
    parameter_type.data.parameter_type.parameter = generic->hir_id;
    self_type = cm_lower_add_type(state, &parameter_type, ast_type_id);
    if (state->failed) {
        if (synthesized) {
            cm_free(synthesized_predicate.trait_type.arguments);
            cm_free(synthesized_predicate.equalities);
            cm_free(synthesized_predicate.binder.lifetimes);
        }
        return CM_HIR_TYPE_NONE;
    }
    associated_item = associated.item != NULL ? associated.item
        : cm_lower_bound_item(state, associated.definition);
    defining_trait = associated_item != NULL
        ? associated_item->parent_definition
        : associated.local_record != NULL
            ? associated.local_record->parent_definition
            : cm_hir_def_id_none();
    if (cm_hir_def_id_is_none(defining_trait)) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "shorthand associated type lost its defining trait");
        return CM_HIR_TYPE_NONE;
    }
    associated_parameter_start = associated_item != NULL
        ? associated_item->generic_parameter_start
        : associated.local_record != NULL
            ? associated.local_record->generic_parameter_start
            : CM_HIR_GENERIC_PARAM_NONE;
    associated_parameter_count = associated_item != NULL
        ? associated_item->generic_parameter_count
        : associated.local_record != NULL
            ? associated.local_record->generic_parameter_count : 0u;
    memset(&projection_trait, 0, sizeof(projection_trait));
    owned_trait_arguments = NULL;
    projection_argument_count = 0u;
    if (cm_hir_def_id_equal(defining_trait,
            predicate->trait_type.definition)) {
        projection_trait = predicate->trait_type;
    } else {
        projection_matches = 0u;
        if (!cm_lower_find_instantiated_supertrait(state,
                &predicate->trait_type, defining_trait, self_type, span,
                &owned_trait_arguments, &projection_argument_count,
                &projection_matches)) {
            cm_free(owned_trait_arguments);
            return CM_HIR_TYPE_NONE;
        }
        if (projection_matches != 1u) {
            cm_free(owned_trait_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "shorthand associated type has no unique defining "
                "supertrait");
            return CM_HIR_TYPE_NONE;
        }
        projection_trait.definition = defining_trait;
        projection_trait.arguments = owned_trait_arguments;
        projection_trait.argument_count = projection_argument_count;
    }
    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_HIR_TYPE_PROJECTION_KIND;
    projection.span = span;
    projection.data.projection_type.self_type = self_type;
    projection.data.projection_type.trait_type = projection_trait;
    projection.data.projection_type.associated_type.definition =
        associated.definition;
    owned_associated_arguments = NULL;
    if (!cm_lower_generic_arguments(state, &path->segments[1], module,
            owner, span, &owned_associated_arguments,
            &projection.data.projection_type.associated_type
                .argument_count)) {
        cm_free(owned_trait_arguments);
        if (synthesized) {
            cm_free(synthesized_predicate.trait_type.arguments);
            cm_free(synthesized_predicate.equalities);
            cm_free(synthesized_predicate.binder.lifetimes);
        }
        return CM_HIR_TYPE_NONE;
    }
    if (associated_parameter_count != 0u
        && (associated_parameter_start == CM_HIR_GENERIC_PARAM_NONE
            || associated_parameter_start
                > UINT32_MAX - (associated_parameter_count - 1u))) {
        cm_free(owned_associated_arguments);
        cm_free(owned_trait_arguments);
        if (synthesized) {
            cm_free(synthesized_predicate.trait_type.arguments);
            cm_free(synthesized_predicate.equalities);
            cm_free(synthesized_predicate.binder.lifetimes);
        }
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "shorthand associated-type projection generic signature "
            "overflows");
        return CM_HIR_TYPE_NONE;
    }
    if (projection.data.projection_type.associated_type.argument_count
            != associated_parameter_count) {
        cm_free(owned_associated_arguments);
        cm_free(owned_trait_arguments);
        if (synthesized) {
            cm_free(synthesized_predicate.trait_type.arguments);
            cm_free(synthesized_predicate.equalities);
            cm_free(synthesized_predicate.binder.lifetimes);
        }
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "shorthand associated-type projection has the wrong generic "
            "argument count");
        return CM_HIR_TYPE_NONE;
    }
    for (index = 0u; index < associated_parameter_count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected_kind;

        parameter = cm_hir_get_generic_param(state->hir,
            associated_parameter_start + index);
        expected_kind = parameter != NULL
                && parameter->kind == CM_HIR_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_ARG_LIFETIME
            : parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
                ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                associated.definition)
            || owned_associated_arguments[index].kind != expected_kind) {
            cm_free(owned_associated_arguments);
            cm_free(owned_trait_arguments);
            if (synthesized) {
                cm_free(synthesized_predicate.trait_type.arguments);
                cm_free(synthesized_predicate.equalities);
                cm_free(synthesized_predicate.binder.lifetimes);
            }
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "shorthand associated-type projection generic argument "
                "kind does not match its parameter");
            return CM_HIR_TYPE_NONE;
        }
    }
    projection.data.projection_type.associated_type.arguments =
        owned_associated_arguments;
    self_type = cm_lower_add_type(state, &projection, ast_type_id);
    cm_free(owned_associated_arguments);
    cm_free(owned_trait_arguments);
    if (synthesized) {
        cm_free(synthesized_predicate.trait_type.arguments);
        cm_free(synthesized_predicate.equalities);
        cm_free(synthesized_predicate.binder.lifetimes);
    }
    return self_type;
}

static CmResolvePrimitiveKind cm_lower_graph_primitive_path(
    const CmLowerState *state, const CmAstPath *path)
{
    CmResolvePathSegmentView *segments;
    CmResolvedBinding binding;
    CmImportLookupStatus status;
    CmResolvePrimitiveKind result;
    uint32_t index;

    if (state->graph == NULL || state->imports == NULL
        || state->graph_module == CM_MODULE_NONE || path == NULL
        || path->segment_count == 0u || path->segments == NULL) {
        return CM_RESOLVE_PRIMITIVE_NONE;
    }
    segments = (CmResolvePathSegmentView *)cm_alloc_zeroed(
        (size_t)path->segment_count, sizeof(*segments));
    for (index = 0u; index < path->segment_count; ++index) {
        const CmInternedString *name;

        name = cm_ast_get_string(state->ast, path->segments[index].name);
        if (name == NULL || name->len == 0u) {
            cm_free(segments);
            return CM_RESOLVE_PRIMITIVE_NONE;
        }
        segments[index].bytes = name->bytes;
        segments[index].length = name->len;
    }
    memset(&binding, 0, sizeof(binding));
    status = cm_import_resolve_path_checked(state->imports, state->graph,
        state->graph_revision, state->graph_module, path->absolute, segments,
        (size_t)path->segment_count, CM_RESOLVE_NAMESPACE_TYPE, &binding);
    cm_free(segments);
    result = status == CM_IMPORT_LOOKUP_OK
            && binding.revision == state->graph_revision
            && binding.namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && !binding.is_ambiguous
            && binding.primitive_kind != CM_RESOLVE_PRIMITIVE_NONE
            && binding.target_module == CM_MODULE_NONE
            && binding.declaration.source == 0u
            && binding.declaration.item == CM_AST_ITEM_NONE
        ? binding.primitive_kind : CM_RESOLVE_PRIMITIVE_NONE;
    return result;
}

/* A lexical declaration or generic parameter wins over a builtin spelling. */
static int cm_lower_primitive_name_is_shadowed(
    const CmLowerState *state, const CmAstPath *path,
    CmHirModuleId current_module)
{
    const CmLowerItemRecord *record;

    if (path == NULL || path->absolute || path->segment_count == 0u
        || path->segments == NULL) {
        return 0;
    }
    if (state->graph == NULL) {
        record = cm_lower_find_name_in_module(state, current_module,
            path->segments[0].name);
        return record != NULL && record->kind != CM_AST_ITEM_MODULE;
    }
    {
        CmResolvePathSegmentView segment;
        CmResolvedBinding binding;
        const CmInternedString *name;
        CmImportLookupStatus status;

        name = cm_ast_get_string(state->ast, path->segments[0].name);
        if (name == NULL || name->len == 0u) return 0;
        segment.bytes = name->bytes;
        segment.length = name->len;
        memset(&binding, 0, sizeof(binding));
        status = cm_import_resolve_path_checked(state->imports,
            state->graph, state->graph_revision, state->graph_module, 0,
            &segment, 1u, CM_RESOLVE_NAMESPACE_TYPE, &binding);
        if (status == CM_IMPORT_LOOKUP_NOT_FOUND) return 0;
        if (status != CM_IMPORT_LOOKUP_OK
            || binding.revision != state->graph_revision
            || binding.namespace_kind != CM_RESOLVE_NAMESPACE_TYPE
            || binding.is_ambiguous) {
            return 1;
        }
        if (binding.target_module != CM_MODULE_NONE
            && binding.item_kind == CM_AST_ITEM_MODULE) {
            return 0;
        }
        return !(binding.primitive_kind != CM_RESOLVE_PRIMITIVE_NONE
            && binding.target_module == CM_MODULE_NONE
            && binding.declaration.source == 0u
            && binding.declaration.item == CM_AST_ITEM_NONE);
    }
}

static CmHirTypeId cm_lower_path_type(CmLowerState *state,
    CmAstTypeId ast_type_id, const CmAstType *ast_type,
    CmHirModuleId module, CmHirDefId owner)
{
    const CmAstPath *path;
    const CmLowerGenericRecord *generic;
    const CmLowerItemRecord *record;
    CmHirDefId module_definition;
    CmLowerLookupResult lookup;
    CmHirLowerResolution resolution;
    CmHirType type;
    CmHirGenericArg *arguments;
    CmResolvePrimitiveKind primitive_kind;
    int primitive_prefix;
    uint32_t argument_count;
    CmSpan span;
    uint32_t index;

    span = cm_lower_span(state, ast_type->span);
    path = cm_ast_get_path(state->ast, ast_type->path);
    if (path == NULL || path->segment_count == 0u
        || path->segments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "path type has an invalid AST path");
        return CM_HIR_TYPE_NONE;
    }
    for (index = 0u; index + 1u < path->segment_count; ++index) {
        if (path->segments[index].argument_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
                CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
                "generic arguments on an intermediate path segment are not "
                "yet represented");
            return CM_HIR_TYPE_NONE;
        }
    }
    memset(&type, 0, sizeof(type));
    type.span = span;
    primitive_prefix = 0;
    if (!path->absolute
        && cm_lower_string_is(state, path->segments[0].name, "Self")) {
        return cm_lower_self_path_type(state, ast_type_id, path, span,
            module, owner);
    }
    if (!path->absolute && path->segment_count <= 2u
        && path->segments[0].argument_count == 0u) {
        generic = cm_lower_find_generic_in_scope(state, owner,
            path->segments[0].name);
        if (generic != NULL) {
            if (generic->kind != CM_HIR_GENERIC_TYPE) {
                cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
                    CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
                    "non-type generic parameter used as a type");
                return CM_HIR_TYPE_NONE;
            }
            if (path->segment_count == 2u) {
                return cm_lower_parameter_projection_type(state,
                    ast_type_id, path, span, module, owner, generic);
            }
            type.kind = CM_HIR_TYPE_PARAMETER_KIND;
            type.data.parameter_type.parameter = generic->hir_id;
            return cm_lower_add_type(state, &type, ast_type_id);
        }
        if (cm_lower_primitive(state, path->segments[0].name, &type)) {
            primitive_prefix = 1;
            if (path->segment_count == 1u
                && !cm_lower_primitive_name_is_shadowed(state, path,
                    module)) {
                return cm_lower_add_type(state, &type, ast_type_id);
            }
        }
    }
    if (cm_lower_type_path_starts_with_parameter(state, path, owner)) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "type parameter shadows this path prefix; shorthand associated "
            "type projections are not supported");
        return CM_HIR_TYPE_NONE;
    }
    primitive_kind = cm_lower_graph_primitive_path(state, path);
    if (primitive_kind != CM_RESOLVE_PRIMITIVE_NONE) {
        if (path->segments[path->segment_count - 1u].argument_count != 0u
            || !cm_lower_resolved_primitive(primitive_kind, NULL, &type)) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
                "primitive type import has generic arguments");
            return CM_HIR_TYPE_NONE;
        }
        type.span = span;
        return cm_lower_add_type(state, &type, ast_type_id);
    }
    lookup = cm_lower_lookup_path(state, path, module,
        CM_HIR_LOWER_PATH_TYPE, &record, &module_definition);
    (void)module_definition;
    if (lookup == CM_LOWER_LOOKUP_STALE_GRAPH) {
        cm_lower_fail(state, CM_HIR_LOWER_STALE_GRAPH, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "graph or import revision changed during type-path lookup");
        return CM_HIR_TYPE_NONE;
    }
    if (lookup == CM_LOWER_LOOKUP_RESOLVER_ERROR) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "local-crate type-path resolution failed");
        return CM_HIR_TYPE_NONE;
    }
    if (lookup == CM_LOWER_LOOKUP_WRONG_NAMESPACE
        || lookup == CM_LOWER_LOOKUP_MODULE
        || lookup == CM_LOWER_LOOKUP_TRAIT) {
        cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "resolved path does not name a type definition");
        return CM_HIR_TYPE_NONE;
    }
    if (lookup == CM_LOWER_LOOKUP_NOT_FOUND && primitive_prefix) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "primitive-qualified path has no authenticated module type; "
            "primitive associated paths are not supported");
        return CM_HIR_TYPE_NONE;
    }
    memset(&resolution, 0, sizeof(resolution));
    if (lookup == CM_LOWER_LOOKUP_DEFINITION) {
        resolution.kind = CM_HIR_LOWER_DEFINITION;
        resolution.definition = record->definition;
        resolution.named_type_kind = CM_HIR_TYPE_ADT_KIND;
    } else if (lookup == CM_LOWER_LOOKUP_ALIAS) {
        resolution.kind = CM_HIR_LOWER_DEFINITION;
        resolution.definition = record->definition;
        resolution.named_type_kind = record->is_foreign
            ? CM_HIR_TYPE_FOREIGN_KIND
            : CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    } else {
        resolution = cm_lower_resolve_library_type(state, path);
    }
    if (lookup == CM_LOWER_LOOKUP_NOT_FOUND
        && resolution.kind == CM_HIR_LOWER_UNRESOLVED
        && state->options->resolve_path != NULL) {
        resolution = state->options->resolve_path(
            state->options->resolve_context, state->ast, ast_type->path,
            module, CM_HIR_LOWER_PATH_TYPE);
    }
    if (!cm_lower_graph_snapshot_matches(state)) {
        cm_lower_fail(state, CM_HIR_LOWER_STALE_GRAPH, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "graph or import resolver changed in the path callback");
        return CM_HIR_TYPE_NONE;
    }
    if (resolution.kind == CM_HIR_LOWER_RESOLVER_ERROR) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "external path resolver failed");
        return CM_HIR_TYPE_NONE;
    }
    if (resolution.kind == CM_HIR_LOWER_UNRESOLVED) {
        cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "type path is unresolved");
        return CM_HIR_TYPE_NONE;
    }
    if (resolution.kind == CM_HIR_LOWER_PRIMITIVE) {
        if (path->segments[path->segment_count - 1u].argument_count != 0u
            || !cm_lower_hir_primitive(resolution.primitive_kind, &type)) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
                "resolver returned an invalid primitive type");
            return CM_HIR_TYPE_NONE;
        }
        type.span = span;
        return cm_lower_add_type(state, &type, ast_type_id);
    }
    if (resolution.kind == CM_HIR_LOWER_EXISTING_TYPE) {
        if (path->segments[path->segment_count - 1u].argument_count != 0u
            || cm_hir_get_type(state->hir, resolution.existing_type) == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
                "resolver returned an invalid existing type");
            return CM_HIR_TYPE_NONE;
        }
        return resolution.existing_type;
    }
    if (resolution.kind != CM_HIR_LOWER_DEFINITION
        || (resolution.named_type_kind != CM_HIR_TYPE_ADT_KIND
            && resolution.named_type_kind != CM_HIR_TYPE_FN_DEFINITION_KIND
            && resolution.named_type_kind
                != CM_HIR_TYPE_ALIAS_APPLICATION_KIND
            && resolution.named_type_kind != CM_HIR_TYPE_OPAQUE_KIND
            && resolution.named_type_kind != CM_HIR_TYPE_FOREIGN_KIND)) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "resolver returned an invalid named type classification");
        return CM_HIR_TYPE_NONE;
    }
    if (lookup == CM_LOWER_LOOKUP_NOT_FOUND
        && !cm_lower_external_definition_matches(state, &resolution)) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "resolver returned an absent, unbound, or mismatched definition");
        return CM_HIR_TYPE_NONE;
    }
    arguments = NULL;
    argument_count = 0u;
    if (resolution.named_type_kind == CM_HIR_TYPE_FOREIGN_KIND
        && path->segments[path->segment_count - 1u].argument_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->path, CM_HIR_OK,
            "foreign types do not accept generic arguments");
        return CM_HIR_TYPE_NONE;
    }
    if (resolution.named_type_kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND) {
        if (!cm_lower_alias_arguments(state,
                &path->segments[path->segment_count - 1u], module, owner,
                resolution.definition, record, span, &arguments,
                &argument_count)) {
            return CM_HIR_TYPE_NONE;
        }
    } else if (resolution.named_type_kind == CM_HIR_TYPE_ADT_KIND) {
        if (!cm_lower_adt_arguments(state,
                &path->segments[path->segment_count - 1u], module, owner,
                resolution.definition, record, span, &arguments,
                &argument_count)) {
            return CM_HIR_TYPE_NONE;
        }
    } else if (!cm_lower_generic_arguments(state,
                   &path->segments[path->segment_count - 1u], module, owner,
                   span, &arguments, &argument_count)) {
        return CM_HIR_TYPE_NONE;
    }
    type.kind = resolution.named_type_kind;
    type.data.named_type.definition = resolution.definition;
    type.data.named_type.arguments = arguments;
    type.data.named_type.argument_count = argument_count;
    {
        CmHirTypeId result;

        result = cm_lower_add_type(state, &type, ast_type_id);
        cm_free(arguments);
        return result;
    }
}

static void cm_lower_find_associated_type(
    const CmLowerState *state, CmHirDefId trait_definition,
    CmInternId ast_name, CmLowerAssociatedTarget *out_target,
    uint32_t *out_matches)
{
    size_t index;
    uint32_t matches;

    memset(out_target, 0, sizeof(*out_target));
    out_target->definition = cm_hir_def_id_none();
    matches = 0u;
    for (index = 0u; index < state->item_records.len; ++index) {
        const CmLowerItemRecord *record;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if (record != NULL && record->kind == CM_AST_ITEM_TYPE_ALIAS
            && record->parent_kind == CM_LOWER_PARENT_TRAIT
            && cm_hir_def_id_equal(record->parent_definition,
                trait_definition)
            && cm_lower_hir_name_matches_ast(state, record->hir_name,
                ast_name)) {
            out_target->definition = record->definition;
            out_target->trait_definition = trait_definition;
            out_target->generic_parameter_count =
                record->generic_parameter_count;
            out_target->item = cm_lower_bound_item(state,
                record->definition);
            out_target->local_record = record;
            matches += 1u;
        }
    }
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            index);
        if (item != NULL && item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && cm_hir_def_id_equal(item->parent_definition,
                trait_definition)
            && cm_lower_find_record_by_definition(state,
                item->definition) == NULL
            && cm_lower_hir_name_matches_ast(state, item->name,
                ast_name)) {
            out_target->definition = item->definition;
            out_target->trait_definition = trait_definition;
            out_target->generic_parameter_count =
                item->generic_parameter_count;
            out_target->item = item;
            out_target->local_record = NULL;
            matches += 1u;
        }
    }
    *out_matches = matches;
}

static int cm_lower_definition_seen(const CmVec *definitions,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < definitions->len; ++index) {
        const CmHirDefId *old_definition;

        old_definition = (const CmHirDefId *)cm_vec_at_const(definitions,
            index);
        if (old_definition != NULL
            && cm_hir_def_id_equal(*old_definition, definition)) {
            return 1;
        }
    }
    return 0;
}

static void cm_lower_find_inherited_associated_type(
    const CmLowerState *state, CmHirDefId trait_definition,
    CmInternId ast_name, CmLowerAssociatedTarget *out_target,
    uint32_t *out_matches)
{
    CmVec pending;
    CmVec seen;
    CmHirDefId current;
    uint32_t matches;

    memset(out_target, 0, sizeof(*out_target));
    out_target->definition = cm_hir_def_id_none();
    matches = 0u;
    cm_vec_init(&pending, sizeof(CmHirDefId));
    cm_vec_init(&seen, sizeof(CmHirDefId));
    (void)cm_vec_push(&pending, &trait_definition);
    while (cm_vec_pop(&pending, &current)) {
        CmLowerAssociatedTarget direct;
        const CmHirItem *trait_item;
        uint32_t direct_matches;
        uint32_t index;

        if (cm_lower_definition_seen(&seen, current)) continue;
        (void)cm_vec_push(&seen, &current);
        cm_lower_find_associated_type(state, current, ast_name, &direct,
            &direct_matches);
        if (direct_matches != 0u) {
            *out_target = direct;
            if (matches > UINT32_MAX - direct_matches) {
                matches = UINT32_MAX;
                break;
            }
            matches += direct_matches;
        }
        trait_item = cm_lower_bound_item(state, current);
        if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT) {
            continue;
        }
        for (index = 0u;
             index < trait_item->data.trait_item.supertrait_count; ++index) {
            CmHirDefId next;

            next = trait_item->data.trait_item.supertraits[index]
                .trait_type.definition;
            if (!cm_lower_definition_seen(&seen, next)) {
                (void)cm_vec_push(&pending, &next);
            }
        }
    }
    cm_vec_destroy(&seen);
    cm_vec_destroy(&pending);
    *out_matches = matches;
}

static CmHirTypeId cm_lower_projection_type(CmLowerState *state,
    CmAstTypeId ast_type_id, const CmAstType *ast_type,
    CmHirModuleId module, CmHirDefId owner)
{
    const CmAstPath *trait_path;
    CmLowerTraitTarget trait_target;
    CmLowerAssociatedTarget associated_record;
    CmLowerLookupResult lookup;
    CmHirType type;
    CmHirGenericArg *trait_arguments;
    CmHirGenericArg *associated_arguments;
    CmHirGenericParamId associated_parameter_start;
    const CmHirItem *associated_item;
    CmSpan span;
    uint32_t associated_argument_count;
    uint32_t index;
    uint32_t matches;
    uint32_t trait_argument_count;

    span = cm_lower_span(state, ast_type->span);
    trait_path = cm_ast_get_path(state->ast,
        ast_type->projection.trait_path);
    if (ast_type->projection.self_type == CM_AST_TYPE_NONE
        || trait_path == NULL || trait_path->segment_count == 0u
        || trait_path->segments == NULL
        || ast_type->projection.associated.name == CM_INTERN_ID_NONE
        || cm_lower_ast_string(state,
            ast_type->projection.associated.name) == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->projection.trait_path,
            CM_HIR_OK, "explicit projection has invalid structural parts");
        return CM_HIR_TYPE_NONE;
    }
    for (index = 0u; index + 1u < trait_path->segment_count; ++index) {
        if (trait_path->segments[index].argument_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                CM_AST_ITEM_NONE, ast_type_id,
                ast_type->projection.trait_path, CM_HIR_OK,
                "generic trait arguments in projections are not supported "
                "until trait substitution is structural");
            return CM_HIR_TYPE_NONE;
        }
    }
    if (cm_lower_type_path_starts_with_parameter(state, trait_path, owner)) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            CM_AST_ITEM_NONE, ast_type_id,
            ast_type->projection.trait_path, CM_HIR_OK,
            "type parameter shadows the projection trait path");
        return CM_HIR_TYPE_NONE;
    }
    lookup = cm_lower_lookup_trait_target(state, trait_path, module,
        &trait_target);
    if (lookup == CM_LOWER_LOOKUP_STALE_GRAPH) {
        cm_lower_fail(state, CM_HIR_LOWER_STALE_GRAPH, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->projection.trait_path,
            CM_HIR_OK,
            "graph or import revision changed during trait-path lookup");
        return CM_HIR_TYPE_NONE;
    }
    if (lookup == CM_LOWER_LOOKUP_RESOLVER_ERROR) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->projection.trait_path,
            CM_HIR_OK, "local-crate trait-path resolution failed");
        return CM_HIR_TYPE_NONE;
    }
    if (lookup == CM_LOWER_LOOKUP_NOT_FOUND) {
        cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->projection.trait_path,
            CM_HIR_OK, "projection trait path is unresolved");
        return CM_HIR_TYPE_NONE;
    }
    if (lookup != CM_LOWER_LOOKUP_TRAIT) {
        cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->projection.trait_path,
            CM_HIR_OK, "projection qualifier does not name a trait");
        return CM_HIR_TYPE_NONE;
    }
    cm_lower_find_associated_type(state,
        trait_target.definition, ast_type->projection.associated.name,
        &associated_record, &matches);
    if (matches == 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->projection.trait_path,
            CM_HIR_OK, "trait has no associated type with this name");
        return CM_HIR_TYPE_NONE;
    }
    if (matches != 1u) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, ast_type_id, ast_type->projection.trait_path,
            CM_HIR_OK, "trait associated-type identity is ambiguous");
        return CM_HIR_TYPE_NONE;
    }
    associated_item = associated_record.item != NULL
        ? associated_record.item
        : cm_lower_bound_item(state, associated_record.definition);
    associated_parameter_start = associated_item != NULL
        ? associated_item->generic_parameter_start
        : associated_record.local_record != NULL
            ? associated_record.local_record->generic_parameter_start
            : CM_HIR_GENERIC_PARAM_NONE;
    associated_arguments = NULL;
    associated_argument_count = 0u;
    if (!cm_lower_generic_arguments(state,
            &ast_type->projection.associated, module, owner, span,
            &associated_arguments, &associated_argument_count)) {
        return CM_HIR_TYPE_NONE;
    }
    if (associated_record.generic_parameter_count != 0u
        && (associated_parameter_start == CM_HIR_GENERIC_PARAM_NONE
            || associated_parameter_start > UINT32_MAX
                - (associated_record.generic_parameter_count - 1u))) {
        cm_free(associated_arguments);
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, ast_type_id,
            ast_type->projection.trait_path,
            CM_HIR_INVARIANT_VIOLATION,
            "explicit associated-type projection generic signature "
            "overflows");
        return CM_HIR_TYPE_NONE;
    }
    if (associated_argument_count
            != associated_record.generic_parameter_count) {
        cm_free(associated_arguments);
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            CM_AST_ITEM_NONE, ast_type_id,
            ast_type->projection.trait_path, CM_HIR_OK,
            "explicit associated-type projection has the wrong generic "
            "argument count");
        return CM_HIR_TYPE_NONE;
    }
    for (index = 0u; index < associated_argument_count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected_kind;

        parameter = cm_hir_get_generic_param(state->hir,
            associated_parameter_start + index);
        expected_kind = parameter != NULL
                && parameter->kind == CM_HIR_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_ARG_LIFETIME
            : parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
                ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                associated_record.definition)
            || associated_arguments[index].kind != expected_kind) {
            cm_free(associated_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                CM_AST_ITEM_NONE, ast_type_id,
                ast_type->projection.trait_path, CM_HIR_OK,
                "explicit associated-type projection generic argument "
                "kind does not match its parameter");
            return CM_HIR_TYPE_NONE;
        }
    }
    trait_arguments = NULL;
    trait_argument_count = 0u;
    if (!cm_lower_trait_positional_arguments(state, CM_AST_ITEM_NONE,
            &trait_path->segments[trait_path->segment_count - 1u],
            &trait_target, module, owner, CM_HIR_TYPE_NONE, 0, 0, 0, span,
            &trait_arguments, &trait_argument_count)) {
        cm_free(associated_arguments);
        return CM_HIR_TYPE_NONE;
    }
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = span;
    type.data.projection_type.self_type = cm_lower_type(state,
        ast_type->projection.self_type, module, owner);
    if (state->failed) {
        cm_free(associated_arguments);
        cm_free(trait_arguments);
        return CM_HIR_TYPE_NONE;
    }
    type.data.projection_type.trait_type.definition =
        trait_target.definition;
    type.data.projection_type.trait_type.arguments = trait_arguments;
    type.data.projection_type.trait_type.argument_count = trait_argument_count;
    type.data.projection_type.associated_type.definition =
        associated_record.definition;
    type.data.projection_type.associated_type.arguments =
        associated_arguments;
    type.data.projection_type.associated_type.argument_count =
        associated_argument_count;
    if (state->failed) {
        cm_free(associated_arguments);
        cm_free(trait_arguments);
        return CM_HIR_TYPE_NONE;
    }
    {
        CmHirTypeId result;

        result = cm_lower_add_type(state, &type, ast_type_id);
        cm_free(associated_arguments);
        cm_free(trait_arguments);
        return result;
    }
}

static int cm_lower_lifetime_binder_is_valid(
    const CmAstLifetimeBinder *binder, CmAstSpan bound_span,
    const CmAstType *trait_type)
{
    uint32_t index;
    uint32_t prior;

    if (binder->lifetime_count == 0u) {
        return binder->lifetimes == NULL
            && binder->span.start == 0u && binder->span.end == 0u;
    }
    if (binder->lifetimes == NULL || trait_type == NULL
        || binder->span.start >= binder->span.end
        || binder->span.start < bound_span.start
        || binder->span.end > bound_span.end
        || binder->span.end > trait_type->span.start) {
        return 0;
    }
    for (index = 0u; index < binder->lifetime_count; ++index) {
        if (binder->lifetimes[index] == CM_INTERN_ID_NONE) {
            return 0;
        }
        for (prior = 0u; prior < index; ++prior) {
            if (binder->lifetimes[prior] == binder->lifetimes[index]) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_lower_validate_impl_trait_type(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId ast_type_id,
    const CmAstType *ast_type, int *out_relaxed_sized)
{
    CmSpan span;
    uint32_t index;
    int relaxed_sized;
    int saw_trait;

    if (out_relaxed_sized != NULL) *out_relaxed_sized = 0;
    span = ast_type == NULL
        ? cm_lower_span(state, (CmAstSpan){ 0u, 0u })
        : cm_lower_span(state, ast_type->span);
    if (ast_type == NULL || ast_type->kind != CM_AST_TYPE_IMPL_TRAIT
        || ast_type->bound_count == 0u || ast_type->bounds == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "opaque impl trait type has no bound storage");
        return 0;
    }
    relaxed_sized = 0;
    saw_trait = 0;
    for (index = 0u; index < ast_type->bound_count; ++index) {
        const CmAstTypeBound *bound;
        const CmAstType *bound_type;
        const CmAstPath *bound_path;
        int has_lifetime;

        bound = &ast_type->bounds[index];
        has_lifetime = bound->lifetime != CM_INTERN_ID_NONE;
        bound_type = cm_ast_get_type(state->ast, bound->trait_type);
        bound_path = bound_type == NULL
            || bound_type->kind != CM_AST_TYPE_PATH ? NULL
            : cm_ast_get_path(state->ast, bound_type->path);
        if ((has_lifetime && bound->trait_type != CM_AST_TYPE_NONE)
            || (has_lifetime
                && (bound->binder.lifetime_count != 0u
                    || bound->binder.lifetimes != NULL
                    || bound->binder.span.start != 0u
                    || bound->binder.span.end != 0u))
            || (bound->modifier != CM_AST_TYPE_BOUND_REQUIRED
                && bound->modifier != CM_AST_TYPE_BOUND_RELAXED
                && bound->modifier
                    != CM_AST_TYPE_BOUND_CONDITIONALLY_CONST)
            || (has_lifetime
                && (bound->modifier != CM_AST_TYPE_BOUND_REQUIRED
                    || cm_lower_ast_string(state, bound->lifetime)
                        == NULL))
            || (!has_lifetime
                && (bound_type == NULL
                    || bound_type->kind != CM_AST_TYPE_PATH
                    || !cm_lower_ast_path_storage_valid(bound_path)
                    || !cm_lower_lifetime_binder_is_valid(&bound->binder,
                        bound->span, bound_type)))
            || bound->span.start >= bound->span.end
            || bound->span.start < ast_type->span.start
            || bound->span.end > ast_type->span.end
            || (index != 0u
                && ast_type->bounds[index - 1u].span.end
                    >= bound->span.start)
            || (bound->modifier == CM_AST_TYPE_BOUND_RELAXED
                && (has_lifetime
                    || !cm_lower_ast_type_is_plain_sized_path(state,
                        bound->trait_type)))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "opaque impl trait bound storage is invalid");
            return 0;
        }
        if (bound->modifier == CM_AST_TYPE_BOUND_RELAXED) {
            if (relaxed_sized) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                    ast_item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                    "opaque impl trait has duplicate relaxed Sized bounds");
                return 0;
            }
            relaxed_sized = 1;
        }
        if (!has_lifetime) saw_trait = 1;
    }
    if (!saw_trait) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "opaque impl trait must include at least one trait bound");
        return 0;
    }
    if (out_relaxed_sized != NULL) *out_relaxed_sized = relaxed_sized;
    return 1;
}

static int cm_lower_record_identity_arguments(CmLowerState *state,
    const CmLowerItemRecord *record, CmSpan span,
    CmHirGenericArg **out_arguments, uint32_t *out_count)
{
    CmHirGenericArg *arguments;
    uint32_t index;

    *out_arguments = NULL;
    *out_count = 0u;
    if (record->generic_parameter_count == 0u) return 1;
    if (record->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            record->ast_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "opaque type owner lost its generic parameter range");
        return 0;
    }
    arguments = (CmHirGenericArg *)cm_alloc_zeroed(
        (size_t)record->generic_parameter_count, sizeof(*arguments));
    for (index = 0u; index < record->generic_parameter_count
            && !state->failed;
         ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(state->hir,
            record->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                record->definition)) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                record->ast_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "opaque type owner has an invalid generic signature");
            break;
        }
        if (parameter->kind == CM_HIR_GENERIC_LIFETIME) {
            arguments[index].kind = CM_HIR_GENERIC_ARG_LIFETIME;
            arguments[index].data.lifetime.kind =
                CM_HIR_REGION_EARLY_BOUND;
            arguments[index].data.lifetime.data.parameter =
                record->generic_parameter_start + index;
        } else if (parameter->kind == CM_HIR_GENERIC_TYPE) {
            CmHirType parameter_type;

            memset(&parameter_type, 0, sizeof(parameter_type));
            parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
            parameter_type.span = span;
            parameter_type.data.parameter_type.parameter =
                record->generic_parameter_start + index;
            arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
            arguments[index].data.type = cm_lower_add_type(state,
                &parameter_type, CM_AST_TYPE_NONE);
        } else if (parameter->kind == CM_HIR_GENERIC_CONST) {
            arguments[index].kind = CM_HIR_GENERIC_ARG_CONST;
            arguments[index].data.constant.kind = CM_HIR_CONST_PARAMETER;
            arguments[index].data.constant.type = parameter->declared_type;
            arguments[index].data.constant.data.parameter =
                record->generic_parameter_start + index;
        } else {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                record->ast_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "opaque type owner has an invalid generic parameter kind");
        }
    }
    if (state->failed) {
        cm_free(arguments);
        return 0;
    }
    *out_arguments = arguments;
    *out_count = record->generic_parameter_count;
    return 1;
}

static CmHirTypeId cm_lower_type(CmLowerState *state, CmAstTypeId ast_type_id,
    CmHirModuleId module, CmHirDefId owner)
{
    const CmAstType *ast_type;
    CmHirType type;
    CmHirTypeId *elements;
    CmSpan span;
    uint32_t index;

    if (state->failed) {
        return CM_HIR_TYPE_NONE;
    }
    ast_type = cm_ast_get_type(state->ast, ast_type_id);
    if (ast_type == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), CM_AST_ITEM_NONE,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "declaration refers to an invalid AST type ID");
        return CM_HIR_TYPE_NONE;
    }
    span = cm_lower_span(state, ast_type->span);
    if (ast_type->kind != CM_AST_TYPE_TUPLE
        && ast_type->tuple_provenance != CM_AST_TUPLE_SOURCE) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "non-tuple type has tuple syntax provenance");
        return CM_HIR_TYPE_NONE;
    }
    memset(&type, 0, sizeof(type));
    type.span = span;
    switch (ast_type->kind) {
    case CM_AST_TYPE_INFER:
        type.kind = CM_HIR_TYPE_INFER_KIND;
        type.data.infer_type.kind = CM_HIR_INFER_GENERAL;
        type.data.infer_type.variable = state->next_type_inference;
        state->next_type_inference += 1u;
        return cm_lower_add_type(state, &type, ast_type_id);
    case CM_AST_TYPE_NEVER:
        type.kind = CM_HIR_TYPE_NEVER_KIND;
        return cm_lower_add_type(state, &type, ast_type_id);
    case CM_AST_TYPE_PATH:
        return cm_lower_path_type(state, ast_type_id, ast_type, module,
            owner);
    case CM_AST_TYPE_PROJECTION:
        return cm_lower_projection_type(state, ast_type_id, ast_type, module,
            owner);
    case CM_AST_TYPE_REFERENCE:
        type.kind = CM_HIR_TYPE_REFERENCE_KIND;
        type.data.reference_type.pointee = cm_lower_type(state,
            ast_type->child, module, owner);
        type.data.reference_type.mutability = ast_type->is_mutable
            ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;
        if (!cm_lower_lifetime(state, ast_type->lifetime, owner, span,
                &type.data.reference_type.region)) {
            return CM_HIR_TYPE_NONE;
        }
        return state->failed ? CM_HIR_TYPE_NONE
            : cm_lower_add_type(state, &type, ast_type_id);
    case CM_AST_TYPE_POINTER:
        type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
        type.data.raw_pointer_type.pointee = cm_lower_type(state,
            ast_type->child, module, owner);
        type.data.raw_pointer_type.mutability = ast_type->is_mutable
            ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;
        return state->failed ? CM_HIR_TYPE_NONE
            : cm_lower_add_type(state, &type, ast_type_id);
    case CM_AST_TYPE_TUPLE:
        if (ast_type->tuple_provenance != CM_AST_TUPLE_SOURCE
            && ast_type->tuple_provenance
                != CM_AST_TUPLE_CALLABLE_INPUTS) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "tuple type has invalid syntax provenance");
            return CM_HIR_TYPE_NONE;
        }
        if (ast_type->element_count != 0u && ast_type->elements == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "tuple type count has no element storage");
            return CM_HIR_TYPE_NONE;
        }
        if (ast_type->element_count == 0u) {
            type.kind = CM_HIR_TYPE_UNIT_KIND;
            return cm_lower_add_type(state, &type, ast_type_id);
        }
        elements = (CmHirTypeId *)cm_alloc_zeroed(
            (size_t)ast_type->element_count, sizeof(CmHirTypeId));
        for (index = 0u; index < ast_type->element_count && !state->failed;
             ++index) {
            elements[index] = cm_lower_type(state, ast_type->elements[index],
                module, owner);
        }
        type.kind = CM_HIR_TYPE_TUPLE_KIND;
        type.data.tuple_type.elements = elements;
        type.data.tuple_type.element_count = ast_type->element_count;
        if (state->failed) {
            cm_free(elements);
            return CM_HIR_TYPE_NONE;
        }
        {
            CmHirTypeId result;

            result = cm_lower_add_type(state, &type, ast_type_id);
            cm_free(elements);
            return result;
        }
    case CM_AST_TYPE_SLICE:
        type.kind = CM_HIR_TYPE_SLICE_KIND;
        type.data.slice_type.element = cm_lower_type(state, ast_type->child,
            module, owner);
        return state->failed ? CM_HIR_TYPE_NONE
            : cm_lower_add_type(state, &type, ast_type_id);
    case CM_AST_TYPE_ARRAY:
    {
        CmHirType usize_type;
        const CmInternedString *length_text;
        const CmLowerItemRecord *length_record;
        const CmLowerGenericRecord *length_generic;
        const CmHirGenericParam *length_parameter;
        const CmHirType *length_parameter_type;
        uint64_t length_value;
        int has_literal_length;

        length_text = cm_lower_ast_string(state, ast_type->text);
        length_record = NULL;
        length_generic = NULL;
        length_parameter = NULL;
        length_parameter_type = NULL;
        has_literal_length = cm_lower_parse_u64(length_text, &length_value);
        if (!has_literal_length) {
            has_literal_length = cm_lower_eval_array_length_expression(
                length_text, state->options->edition, &length_value);
        }
        if (!has_literal_length) {
            has_literal_length = cm_lower_parse_pointer_storage_length(
                length_text, &length_value);
        }
        if (!has_literal_length) {
            length_generic = cm_lower_find_generic_in_scope(state, owner,
                ast_type->text);
            if (length_generic == NULL) {
                length_record = cm_lower_named_const_length(state, module,
                    length_text);
                if (length_record == NULL) {
                    length_record = cm_lower_associated_const_length(state,
                        owner, length_text);
                }
            }
        }
        if (length_generic != NULL) {
            length_parameter = cm_hir_get_generic_param(state->hir,
                length_generic->hir_id);
            length_parameter_type = length_parameter == NULL ? NULL
                : cm_hir_get_type(state->hir,
                    length_parameter->declared_type);
            if (length_generic->kind != CM_HIR_GENERIC_CONST
                || length_parameter == NULL
                || length_parameter_type == NULL
                || length_parameter_type->kind
                    != CM_HIR_TYPE_INTEGER_KIND
                || length_parameter_type->data.integer_type.kind
                    != CM_HIR_INT_USIZE) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
                    CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "array length generic must be a typed usize const "
                    "parameter");
                return CM_HIR_TYPE_NONE;
            }
        } else if (!has_literal_length && length_record == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "array length is neither a plain, in-range decimal or "
                "constant expression, a typed const parameter, nor a "
                "resolved simple const name");
            return CM_HIR_TYPE_NONE;
        }
        memset(&usize_type, 0, sizeof(usize_type));
        usize_type.kind = CM_HIR_TYPE_INTEGER_KIND;
        usize_type.span = span;
        usize_type.data.integer_type.kind = CM_HIR_INT_USIZE;
        type.kind = CM_HIR_TYPE_ARRAY_KIND;
        type.data.array_type.element = cm_lower_type(state, ast_type->child,
            module, owner);
        type.data.array_type.length.type = length_parameter == NULL
            ? cm_lower_add_type(state, &usize_type, ast_type_id)
            : length_parameter->declared_type;
        if (length_parameter != NULL) {
            type.data.array_type.length.kind = CM_HIR_CONST_PARAMETER;
            type.data.array_type.length.data.parameter =
                length_generic->hir_id;
        } else if (has_literal_length) {
            type.data.array_type.length.kind = CM_HIR_CONST_VALUE;
            type.data.array_type.length.data.value.low_bits = length_value;
            type.data.array_type.length.data.value.high_bits = 0u;
        } else {
            type.data.array_type.length.kind = CM_HIR_CONST_UNEVALUATED;
            type.data.array_type.length.data.definition =
                length_record->definition;
        }
        return state->failed ? CM_HIR_TYPE_NONE
            : cm_lower_add_type(state, &type, ast_type_id);
    }
    case CM_AST_TYPE_FUNCTION:
        if (ast_type->element_count != 0u && ast_type->elements == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "function type count has no parameter storage");
            return CM_HIR_TYPE_NONE;
        }
        elements = NULL;
        if (ast_type->element_count != 0u) {
            elements = (CmHirTypeId *)cm_alloc_zeroed(
                (size_t)ast_type->element_count, sizeof(CmHirTypeId));
        }
        for (index = 0u; index < ast_type->element_count && !state->failed;
             ++index) {
            elements[index] = cm_lower_type(state, ast_type->elements[index],
                module, owner);
        }
        type.kind = CM_HIR_TYPE_FN_POINTER_KIND;
        type.data.fn_pointer_type.parameters = elements;
        type.data.fn_pointer_type.parameter_count = ast_type->element_count;
        type.data.fn_pointer_type.return_type = cm_lower_type(state,
            ast_type->child, module, owner);
        type.data.fn_pointer_type.abi = cm_hir_intern(state->hir, "Rust");
        type.data.fn_pointer_type.safety = ast_type->is_unsafe
            ? CM_HIR_UNSAFE : CM_HIR_SAFE;
        if (state->failed) {
            cm_free(elements);
            return CM_HIR_TYPE_NONE;
        }
        {
            CmHirTypeId result;

            result = cm_lower_add_type(state, &type, ast_type_id);
            cm_free(elements);
            return result;
        }
    case CM_AST_TYPE_IMPL_TRAIT:
    {
        const CmLowerApitRecord *apit;
        const CmLowerItemRecord *owner_record;
        const CmHirGenericParam *parameter;
        CmHirGenericArg *arguments;
        CmHirTypeId result;
        uint32_t argument_count;

        if (!cm_lower_validate_impl_trait_type(state, CM_AST_ITEM_NONE,
                ast_type_id, ast_type, NULL)) {
            return CM_HIR_TYPE_NONE;
        }
        apit = cm_lower_find_apit(state, state->ast, owner, ast_type_id);
        parameter = apit == NULL ? NULL
            : cm_hir_get_generic_param(state->hir, apit->hir_id);
        if (apit != NULL && parameter != NULL
            && parameter->kind == CM_HIR_GENERIC_TYPE
            && cm_hir_def_id_equal(parameter->owner, owner)) {
            type.kind = CM_HIR_TYPE_PARAMETER_KIND;
            type.data.parameter_type.parameter = apit->hir_id;
            return cm_lower_add_type(state, &type, ast_type_id);
        }
        if (apit != NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
                CM_HIR_INVALID_ID,
                "argument impl trait lost its synthetic generic parameter");
            return CM_HIR_TYPE_NONE;
        }
        owner_record = cm_lower_find_record_by_definition(state, owner);
        if (owner_record == NULL
            || owner_record->kind != CM_AST_ITEM_FUNCTION) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
                CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "opaque impl trait is supported only as a function return "
                "type");
            return CM_HIR_TYPE_NONE;
        }
        arguments = NULL;
        argument_count = 0u;
        if (!cm_lower_record_identity_arguments(state, owner_record, span,
                &arguments, &argument_count)) {
            return CM_HIR_TYPE_NONE;
        }
        /* The owner definition gives one stable opaque identity per function.
         * Bounds are structurally validated above; a later dedicated opaque
         * declaration layer will retain them for semantic selection. */
        type.kind = CM_HIR_TYPE_OPAQUE_KIND;
        type.data.named_type.definition = owner;
        type.data.named_type.arguments = arguments;
        type.data.named_type.argument_count = argument_count;
        result = cm_lower_add_type(state, &type, ast_type_id);
        cm_free(arguments);
        return result;
    }
    case CM_AST_TYPE_DYN_TRAIT:
        return cm_lower_dyn_trait_type(state, ast_type_id, ast_type, module,
            owner);
    case CM_AST_TYPE_MACRO:
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "type-position macros must be expanded before HIR lowering");
        return CM_HIR_TYPE_NONE;
    case CM_AST_TYPE_OTHER:
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "unclassified AST type cannot be lowered without guessing");
        return CM_HIR_TYPE_NONE;
    }
    cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
        CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
        "unknown AST type kind");
    return CM_HIR_TYPE_NONE;
}

static int cm_lower_predeclare_apit_type(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId ast_type_id,
    CmLowerItemRecord *record, uint32_t explicit_count, size_t depth);

static int cm_lower_predeclare_apit_arguments(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstGenericArg *arguments,
    uint32_t argument_count, CmLowerItemRecord *record,
    uint32_t explicit_count, size_t depth)
{
    uint32_t index;

    if (depth > CM_LOWER_APIT_MAX_DEPTH) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "argument impl trait path graph contains a cycle");
        return 0;
    }
    if ((argument_count != 0u && arguments == NULL)
        || (argument_count == 0u && arguments != NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "argument impl trait traversal found invalid path storage");
        return 0;
    }
    for (index = 0u; index < argument_count && !state->failed; ++index) {
        const CmAstGenericArg *argument;

        argument = &arguments[index];
        if (!cm_lower_predeclare_apit_arguments(state, ast_item_id,
                argument->name_arguments, argument->name_argument_count,
                record, explicit_count, depth + 1u)) {
            return 0;
        }
        if (argument->type != CM_AST_TYPE_NONE
            && !cm_lower_predeclare_apit_type(state, ast_item_id,
                argument->type, record, explicit_count, depth + 1u)) {
            return 0;
        }
    }
    return !state->failed;
}

static int cm_lower_predeclare_apit_path(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstPathId ast_path_id,
    CmLowerItemRecord *record, uint32_t explicit_count, size_t depth)
{
    const CmAstPath *path;
    uint32_t index;

    if (depth > CM_LOWER_APIT_MAX_DEPTH) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), ast_item_id,
            CM_AST_TYPE_NONE, ast_path_id, CM_HIR_INVALID_ID,
            "argument impl trait path graph contains a cycle");
        return 0;
    }
    path = cm_ast_get_path(state->ast, ast_path_id);
    if (!cm_lower_ast_path_storage_valid(path)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), ast_item_id,
            CM_AST_TYPE_NONE, ast_path_id, CM_HIR_INVALID_ID,
            "argument impl trait traversal found invalid path storage");
        return 0;
    }
    for (index = 0u; index < path->segment_count && !state->failed;
         ++index) {
        if (!cm_lower_predeclare_apit_arguments(state, ast_item_id,
                path->segments[index].arguments,
                path->segments[index].argument_count, record,
                explicit_count, depth + 1u)) {
            return 0;
        }
    }
    return !state->failed;
}

static int cm_lower_predeclare_one_apit(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId ast_type_id,
    const CmAstType *ast_type, CmLowerItemRecord *record,
    uint32_t explicit_count)
{
    CmHirGenericParam parameter;
    CmLowerApitRecord apit_record;
    CmHirStatus status;
    char name[32];
    unsigned long ordinal;
    int name_length;
    int relaxed_sized;

    if (cm_lower_find_apit(state, state->ast, record->definition,
            ast_type_id) != NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, ast_type->span), ast_item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "argument impl trait AST type is reused");
        return 0;
    }
    if (record->is_foreign) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            cm_lower_span(state, ast_type->span), ast_item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "foreign functions cannot have argument impl trait generics");
        return 0;
    }
    if (!cm_lower_validate_impl_trait_type(state, ast_item_id,
            ast_type_id, ast_type, &relaxed_sized)) {
        return 0;
    }
    if (record->generic_parameter_count < explicit_count
        || record->generic_parameter_count == UINT32_MAX) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, ast_type->span), ast_item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
            "argument impl trait generic index overflow");
        return 0;
    }
    ordinal = (unsigned long)(record->generic_parameter_count
        - explicit_count);
    name_length = snprintf(name, sizeof(name), "$APIT%lu", ordinal);
    if (name_length < 0 || (size_t)name_length >= sizeof(name)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, ast_type->span), ast_item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
            "argument impl trait synthetic name overflow");
        return 0;
    }
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = record->definition;
    parameter.index = record->generic_parameter_count;
    parameter.name = cm_hir_intern(state->hir, name);
    parameter.span = cm_lower_span(state, ast_type->span);
    parameter.is_relaxed_sized = relaxed_sized;
    status = cm_hir_add_generic_param(state->hir, &parameter,
        &apit_record.hir_id);
    if (status != CM_HIR_OK) {
        cm_lower_fail_hir(state, parameter.span, ast_item_id, status,
            "cannot add argument impl trait generic parameter");
        return 0;
    }
    apit_record.ast = state->ast;
    apit_record.owner = record->definition;
    apit_record.ast_type = ast_type_id;
    if (record->generic_parameter_count == 0u) {
        record->generic_parameter_start = apit_record.hir_id;
    }
    record->generic_parameter_count += 1u;
    (void)cm_vec_push(&state->apit_records, &apit_record);
    return 1;
}

static int cm_lower_predeclare_apit_type(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId ast_type_id,
    CmLowerItemRecord *record, uint32_t explicit_count, size_t depth)
{
    const CmAstType *ast_type;
    uint32_t index;

    if (depth > CM_LOWER_APIT_MAX_DEPTH) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), ast_item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "argument impl trait type graph contains a cycle");
        return 0;
    }
    ast_type = cm_ast_get_type(state->ast, ast_type_id);
    if (ast_type == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), ast_item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "function parameter refers to an invalid AST type ID");
        return 0;
    }
    switch (ast_type->kind) {
    case CM_AST_TYPE_PATH:
        return cm_lower_predeclare_apit_path(state, ast_item_id,
            ast_type->path, record, explicit_count, depth + 1u);
    case CM_AST_TYPE_PROJECTION:
        if (!cm_lower_predeclare_apit_type(state, ast_item_id,
                ast_type->projection.self_type, record, explicit_count,
                depth + 1u)
            || !cm_lower_predeclare_apit_path(state, ast_item_id,
                ast_type->projection.trait_path, record, explicit_count,
                depth + 1u)
            || !cm_lower_predeclare_apit_arguments(state, ast_item_id,
                ast_type->projection.associated.arguments,
                ast_type->projection.associated.argument_count, record,
                explicit_count, depth + 1u)) {
            return 0;
        }
        return 1;
    case CM_AST_TYPE_REFERENCE:
    case CM_AST_TYPE_POINTER:
    case CM_AST_TYPE_SLICE:
    case CM_AST_TYPE_ARRAY:
        return cm_lower_predeclare_apit_type(state, ast_item_id,
            ast_type->child, record, explicit_count, depth + 1u);
    case CM_AST_TYPE_TUPLE:
    case CM_AST_TYPE_FUNCTION:
        if ((ast_type->element_count != 0u && ast_type->elements == NULL)
            || (ast_type->element_count == 0u
                && ast_type->elements != NULL)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, ast_type->span), ast_item_id,
                ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                "argument impl trait traversal found invalid type storage");
            return 0;
        }
        for (index = 0u; index < ast_type->element_count
                && !state->failed; ++index) {
            if (!cm_lower_predeclare_apit_type(state, ast_item_id,
                    ast_type->elements[index], record, explicit_count,
                    depth + 1u)) {
                return 0;
            }
        }
        if (ast_type->kind == CM_AST_TYPE_FUNCTION) {
            return cm_lower_predeclare_apit_type(state, ast_item_id,
                ast_type->child, record, explicit_count, depth + 1u);
        }
        return !state->failed;
    case CM_AST_TYPE_IMPL_TRAIT:
        return cm_lower_predeclare_one_apit(state, ast_item_id,
            ast_type_id, ast_type, record, explicit_count);
    case CM_AST_TYPE_INFER:
    case CM_AST_TYPE_NEVER:
    case CM_AST_TYPE_DYN_TRAIT:
    case CM_AST_TYPE_OTHER:
    case CM_AST_TYPE_MACRO:
        return 1;
    }
    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
        cm_lower_span(state, ast_type->span), ast_item_id, ast_type_id,
        CM_AST_PATH_NONE, CM_HIR_OK,
        "argument impl trait traversal found an unknown type kind");
    return 0;
}

static int cm_lower_predeclare_generic_parameters(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    CmLowerItemRecord *record)
{
    uint32_t index;
    CmSpan span;
    int saw_non_lifetime;
    int saw_default;

    span = cm_lower_span(state, ast_item->span);
    if (ast_item->generic_parameter_count != 0u
        && ast_item->generic_parameters == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "generic parameter count has no parameter storage");
        return 0;
    }
    if (ast_item->kind == CM_AST_ITEM_FUNCTION
        && ((ast_item->data.function_item.parameter_count != 0u
                && ast_item->data.function_item.parameters == NULL)
            || (ast_item->data.function_item.parameter_count == 0u
                && ast_item->data.function_item.parameters != NULL))) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "function parameter count and storage disagree");
        return 0;
    }
    if (ast_item->where_clause != CM_INTERN_ID_NONE
        && cm_ast_get_string(state->ast, ast_item->where_clause) == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "where-clause provenance string is invalid");
        return 0;
    }
    if ((ast_item->where_predicate_count != 0u
            && ast_item->where_predicates == NULL)
        || (ast_item->where_predicate_count == 0u
            && ast_item->where_predicates != NULL)
        || (ast_item->where_clause != CM_INTERN_ID_NONE
            && ast_item->where_predicate_count == 0u)
        || (ast_item->where_clause == CM_INTERN_ID_NONE
            && ast_item->where_predicate_count != 0u)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "where-predicate text and structural storage disagree");
        return 0;
    }
    if ((ast_item->kind == CM_AST_ITEM_TYPE_ALIAS
            || ast_item->kind == CM_AST_ITEM_CONST
            || ast_item->kind == CM_AST_ITEM_STATIC)
        && ast_item->data.value_item.post_value_where_clause
            != CM_INTERN_ID_NONE
        && cm_ast_get_string(state->ast,
            ast_item->data.value_item.post_value_where_clause) == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "post-value where-clause provenance string is invalid");
        return 0;
    }
    if ((ast_item->kind == CM_AST_ITEM_TYPE_ALIAS
            || ast_item->kind == CM_AST_ITEM_CONST
            || ast_item->kind == CM_AST_ITEM_STATIC)
        && ((ast_item->data.value_item.post_value_where_predicate_count != 0u
            && ast_item->data.value_item.post_value_where_predicates == NULL)
        || (ast_item->data.value_item.post_value_where_predicate_count == 0u
            && ast_item->data.value_item.post_value_where_predicates != NULL)
        || (ast_item->data.value_item.post_value_where_clause
                != CM_INTERN_ID_NONE
            && ast_item->data.value_item.post_value_where_predicate_count
                == 0u)
        || (ast_item->data.value_item.post_value_where_clause
                == CM_INTERN_ID_NONE
            && ast_item->data.value_item.post_value_where_predicate_count
                != 0u))) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "post-value where-predicate text and structural storage "
            "disagree");
        return 0;
    }
    if ((ast_item->kind == CM_AST_ITEM_CONST
            || ast_item->kind == CM_AST_ITEM_STATIC)
        && ast_item->data.value_item.post_value_where_clause
            != CM_INTERN_ID_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "post-value where clause belongs to a non-type item");
        return 0;
    }
    saw_non_lifetime = 0;
    saw_default = 0;
    for (index = 0u; index < ast_item->generic_parameter_count; ++index) {
        const CmAstGenericParam *ast_parameter;
        CmHirGenericParam parameter;
        CmLowerGenericRecord generic_record;
        CmHirStatus status;
        uint32_t bound_index;
        uint32_t where_index;
        int is_relaxed_sized;

        ast_parameter = &ast_item->generic_parameters[index];
        is_relaxed_sized = 0;
        if ((ast_parameter->attribute_count != 0u
                && ast_parameter->attributes == NULL)
            || (ast_parameter->attribute_count == 0u
                && ast_parameter->attributes != NULL)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "generic parameter attribute count and storage disagree");
            return 0;
        }
        if (ast_parameter->attribute_count != 0u) {
            const CmAstAttribute *attribute;
            uint32_t attribute_index;

            for (attribute_index = 0u;
                 attribute_index < ast_parameter->attribute_count;
                 ++attribute_index) {
                attribute = cm_ast_get_attribute(state->ast,
                    ast_parameter->attributes[attribute_index]);
                if (attribute == NULL
                    || attribute->style != CM_AST_ATTR_OUTER) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                        ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                        CM_HIR_OK,
                        "generic parameter attribute is malformed");
                    return 0;
                }
            }
            attribute = cm_ast_get_attribute(state->ast,
                ast_parameter->attributes[0]);
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, attribute->span), ast_item_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "generic parameter attributes are outside the supported "
                "HIR slice");
            return 0;
        }
        if ((ast_parameter->bound_count != 0u
                && ast_parameter->bounds == NULL)
            || (ast_parameter->bound_count == 0u
                && ast_parameter->bounds != NULL)
            || (ast_parameter->kind == CM_AST_PARAM_CONST
                && (ast_parameter->bound_count != 0u
                    || ast_parameter->constraint == CM_INTERN_ID_NONE
                    || ast_parameter->declared_type == CM_AST_TYPE_NONE))
            || (ast_parameter->kind != CM_AST_PARAM_CONST
                && ast_parameter->declared_type != CM_AST_TYPE_NONE)
            || (ast_parameter->kind != CM_AST_PARAM_CONST
                && (ast_parameter->default_const != CM_INTERN_ID_NONE
                    || ast_parameter->default_const_expr
                        != CM_AST_EXPR_NONE))
            || ((ast_parameter->default_const == CM_INTERN_ID_NONE)
                != (ast_parameter->default_const_expr
                    == CM_AST_EXPR_NONE))
            || (ast_parameter->kind != CM_AST_PARAM_CONST
                && ((ast_parameter->constraint == CM_INTERN_ID_NONE)
                    != (ast_parameter->bound_count == 0u)))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "generic parameter constraint provenance and bound storage "
                "disagree");
            return 0;
        }
        if (ast_parameter->default_const != CM_INTERN_ID_NONE
            && (cm_ast_get_string(state->ast,
                    ast_parameter->default_const) == NULL
                || cm_ast_get_expr(state->ast,
                    ast_parameter->default_const_expr) == NULL)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVALID_ID,
                "const generic default provenance is invalid");
            return 0;
        }
        for (bound_index = 0u;
             bound_index < ast_parameter->bound_count; ++bound_index) {
            const CmAstGenericParamBound *bound;

            bound = &ast_parameter->bounds[bound_index];
            if ((unsigned int)bound->kind
                    > (unsigned int)CM_AST_GENERIC_BOUND_LIFETIME
                || (unsigned int)bound->modifier
                    > (unsigned int)
                        CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST
                || bound->span.start > bound->span.end) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                    ast_item_id, bound->trait_type, CM_AST_PATH_NONE,
                    CM_HIR_OK, "generic parameter bound is malformed");
                return 0;
            }
            if (bound->kind == CM_AST_GENERIC_BOUND_LIFETIME) {
                if (bound->modifier != CM_AST_GENERIC_BOUND_REQUIRED
                    || bound->trait_type != CM_AST_TYPE_NONE
                    || bound->lifetime == CM_INTERN_ID_NONE
                    || cm_ast_get_string(state->ast, bound->lifetime)
                        == NULL) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                        cm_lower_span(state, bound->span), ast_item_id,
                        bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                        "lifetime generic parameter bound is malformed");
                    return 0;
                }
                continue;
            }
            if (bound->trait_type == CM_AST_TYPE_NONE
                || bound->lifetime != CM_INTERN_ID_NONE) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "trait generic parameter bound is malformed");
                return 0;
            }
            if (bound->modifier == CM_AST_GENERIC_BOUND_RELAXED) {
                if (!cm_lower_ast_type_is_plain_sized_path(state,
                        bound->trait_type)) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                        cm_lower_span(state, bound->span), ast_item_id,
                        bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                        "relaxed generic parameter bound must be plain "
                        "?Sized");
                    return 0;
                }
                is_relaxed_sized = 1;
            }
        }
        for (where_index = 0u;
             where_index < ast_item->where_predicate_count;
             ++where_index) {
            const CmAstWherePredicate *where_predicate;

            where_predicate = &ast_item->where_predicates[where_index];
            for (bound_index = 0u;
                 bound_index < where_predicate->bound_count;
                 ++bound_index) {
                if (cm_lower_where_bound_relaxes_named_parameter(state,
                        where_predicate,
                        &where_predicate->bounds[bound_index],
                        ast_parameter->name)) {
                    is_relaxed_sized = 1;
                }
            }
        }
        if (ast_parameter->kind == CM_AST_PARAM_LIFETIME) {
            if (saw_non_lifetime) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "lifetime parameters must precede type parameters");
                return 0;
            }
        } else {
            saw_non_lifetime = 1;
        }
        if (cm_lower_find_generic(state, record->definition,
                ast_parameter->name) != NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_DUPLICATE_NAME, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "duplicate generic parameter name");
            return 0;
        }
        if (ast_parameter->default_type != CM_AST_TYPE_NONE
            && ast_parameter->kind != CM_AST_PARAM_TYPE) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, ast_parameter->default_type, CM_AST_PATH_NONE,
                CM_HIR_OK, "only type parameters can have type defaults");
            return 0;
        }
        if (ast_parameter->default_type != CM_AST_TYPE_NONE
            && ast_item->kind != CM_AST_ITEM_TYPE_ALIAS
            && ast_item->kind != CM_AST_ITEM_TRAIT
            && ast_item->kind != CM_AST_ITEM_STRUCT
            && ast_item->kind != CM_AST_ITEM_UNION
            && ast_item->kind != CM_AST_ITEM_ENUM) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                ast_item_id, ast_parameter->default_type,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "generic type defaults are currently supported only on "
                "type aliases, traits, and ADTs");
            return 0;
        }
        if (ast_parameter->default_const_expr != CM_AST_EXPR_NONE
            && ast_item->kind == CM_AST_ITEM_FUNCTION) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "const generic defaults are not supported on functions");
            return 0;
        }
        if (ast_parameter->kind != CM_AST_PARAM_LIFETIME) {
            if (ast_parameter->default_type != CM_AST_TYPE_NONE
                || ast_parameter->default_const_expr
                    != CM_AST_EXPR_NONE) {
                saw_default = 1;
            } else if (saw_default) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "a required generic parameter follows a defaulted one");
                return 0;
            }
        }
        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = ast_parameter->kind == CM_AST_PARAM_TYPE
            ? CM_HIR_GENERIC_TYPE
            : (ast_parameter->kind == CM_AST_PARAM_LIFETIME
                ? CM_HIR_GENERIC_LIFETIME : CM_HIR_GENERIC_CONST);
        parameter.owner = record->definition;
        parameter.index = index;
        parameter.name = cm_lower_copy_string(state, ast_parameter->name,
            span, ast_item_id);
        parameter.span = span;
        parameter.is_relaxed_sized = is_relaxed_sized;
        if (parameter.kind == CM_HIR_GENERIC_CONST) {
            parameter.declared_type = cm_lower_type(state,
                ast_parameter->declared_type, record->owner_module,
                record->definition);
        }
        if (state->failed) {
            return 0;
        }
        status = cm_hir_add_generic_param(state->hir, &parameter,
            &generic_record.hir_id);
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state, span, ast_item_id, status,
                "cannot add HIR generic parameter");
            return 0;
        }
        generic_record.owner = record->definition;
        generic_record.ast_name = ast_parameter->name;
        generic_record.kind = parameter.kind;
        if (index == 0u) {
            record->generic_parameter_start = generic_record.hir_id;
        }
        record->generic_parameter_count += 1u;
        (void)cm_vec_push(&state->generic_records, &generic_record);
    }
    if (ast_item->kind == CM_AST_ITEM_FUNCTION) {
        const CmAstFunction *function;

        function = &ast_item->data.function_item;
        for (index = 0u; index < function->parameter_count
                && !state->failed; ++index) {
            if (!function->parameters[index].is_self
                && function->parameters[index].type != CM_AST_TYPE_NONE
                && !cm_lower_predeclare_apit_type(state, ast_item_id,
                    function->parameters[index].type, record,
                    ast_item->generic_parameter_count, 0u)) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_lower_predeclare_all_generics(CmLowerState *state)
{
    size_t index;

    for (index = 0u; index < state->item_records.len && !state->failed;
         ++index) {
        CmLowerItemRecord *record;
        const CmAstItem *ast_item;

        record = (CmLowerItemRecord *)cm_vec_at(&state->item_records, index);
        if (record == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ 0u, 0u, 0u }, CM_AST_ITEM_NONE,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "generic predeclaration lost an item record");
            return 0;
        }
        state->ast = record->ast;
        state->source = record->source;
        state->graph_module = record->graph_module;
        state->generated_span = record->effective_span;
        state->use_generated_span = record->is_generated;
        ast_item = cm_ast_get_item(state->ast, record->ast_id);
        if (ast_item == NULL
            || !cm_lower_predeclare_generic_parameters(state,
                record->ast_id, ast_item, record)) {
            return 0;
        }
    }
    return !state->failed;
}

static int cm_lower_validate_default_type(CmLowerState *state,
    CmHirTypeId type_id, CmHirDefId owner, uint32_t parameter_index,
    CmSpan span, CmAstItemId item_id, CmAstTypeId ast_type_id, size_t depth);

/* Keep malformed or adversarial generic defaults off the C call stack. */
#define CM_LOWER_GENERIC_DEFAULT_MAX_DEPTH 256u

/*
 * The public HIR setter repeats this scope check as an API invariant.  Lowering
 * walks the type first so source programs retain precise unsupported-feature
 * diagnostics instead of exposing a generic model failure.
 */

static int cm_lower_validate_default_parameter(CmLowerState *state,
    CmHirGenericParamId parameter_id, CmHirDefId owner,
    uint32_t parameter_index, CmSpan span, CmAstItemId item_id,
    CmAstTypeId ast_type_id)
{
    const CmHirGenericParam *parameter;

    parameter = cm_hir_get_generic_param(state->hir, parameter_id);
    if (parameter == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "generic type default references an invalid parameter");
        return 0;
    }
    if (cm_hir_def_id_equal(parameter->owner, owner)
        && parameter->index >= parameter_index) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "generic type default references itself or a later parameter");
        return 0;
    }
    return 1;
}

static int cm_lower_validate_default_region(CmLowerState *state,
    const CmHirRegion *region, CmHirDefId owner, uint32_t parameter_index,
    CmSpan span, CmAstItemId item_id, CmAstTypeId ast_type_id)
{
    if (region->kind == CM_HIR_REGION_STATIC) return 1;
    if (region->kind == CM_HIR_REGION_EARLY_BOUND) {
        return cm_lower_validate_default_parameter(state,
            region->data.parameter, owner, parameter_index, span, item_id,
            ast_type_id);
    }
    cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span, item_id,
        ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
        "generic type default contains an unresolved, erased, or "
        "unauthenticated bound lifetime");
    return 0;
}

static int cm_lower_validate_default_named(CmLowerState *state,
    const CmHirNamedType *named, CmHirDefId owner,
    uint32_t parameter_index, CmSpan span, CmAstItemId item_id,
    CmAstTypeId ast_type_id, size_t depth)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericArg *argument;

        argument = &named->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (!cm_lower_validate_default_region(state,
                    &argument->data.lifetime, owner, parameter_index, span,
                    item_id, ast_type_id)) {
                return 0;
            }
        } else if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (!cm_lower_validate_default_type(state,
                    argument->data.type, owner, parameter_index, span,
                    item_id, ast_type_id, depth + 1u)) {
                return 0;
            }
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST
            && !cm_lower_validate_default_type(state,
                argument->data.constant.type, owner, parameter_index, span,
                item_id, ast_type_id, depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_lower_validate_default_type(CmLowerState *state,
    CmHirTypeId type_id, CmHirDefId owner, uint32_t parameter_index,
    CmSpan span, CmAstItemId item_id, CmAstTypeId ast_type_id, size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    if (depth > CM_LOWER_GENERIC_DEFAULT_MAX_DEPTH) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "generic type default exceeds the structural depth limit");
        return 0;
    }
    if (depth > state->hir->types.len) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_INVARIANT_VIOLATION,
            "generic type default has cyclic type structure");
        return 0;
    }
    type = cm_hir_get_type(state->hir, type_id);
    if (type == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, item_id,
            ast_type_id, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "generic type default references an invalid type");
        return 0;
    }
    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "generic type default contains an unresolved or unnameable "
            "HIR type");
        return 0;
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        return 1;
    case CM_HIR_TYPE_SELF_KIND:
    {
        const CmLowerItemRecord *record;

        record = cm_lower_find_record_by_definition(state, owner);
        if (record != NULL && record->kind == CM_AST_ITEM_TRAIT
            && cm_hir_def_id_equal(type->data.self_type.owner, owner)) {
            return 1;
        }
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "generic type defaults may use Self only on the enclosing "
            "trait");
        return 0;
    }
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_lower_validate_default_region(state,
                &type->data.reference_type.region, owner, parameter_index,
                span, item_id, ast_type_id)
            && cm_lower_validate_default_type(state,
                type->data.reference_type.pointee, owner, parameter_index,
                span, item_id, ast_type_id, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_lower_validate_default_type(state,
            type->data.raw_pointer_type.pointee, owner, parameter_index,
            span, item_id, ast_type_id, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (!cm_lower_validate_default_type(state,
                    type->data.tuple_type.elements[index], owner,
                    parameter_index, span, item_id, ast_type_id,
                    depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_lower_validate_default_type(state,
                type->data.array_type.element, owner, parameter_index, span,
                item_id, ast_type_id, depth + 1u)
            && cm_lower_validate_default_type(state,
                type->data.array_type.length.type, owner, parameter_index,
                span, item_id, ast_type_id, depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_lower_validate_default_type(state,
            type->data.slice_type.element, owner, parameter_index, span,
            item_id, ast_type_id, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_lower_validate_default_type(state,
                    type->data.fn_pointer_type.parameters[index], owner,
                    parameter_index, span, item_id, ast_type_id,
                    depth + 1u)) {
                return 0;
            }
        }
        return cm_lower_validate_default_type(state,
            type->data.fn_pointer_type.return_type, owner, parameter_index,
            span, item_id, ast_type_id, depth + 1u);
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
        return cm_lower_validate_default_named(state,
            &type->data.named_type, owner, parameter_index, span, item_id,
            ast_type_id, depth);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return cm_lower_validate_default_parameter(state,
            type->data.parameter_type.parameter, owner, parameter_index,
            span, item_id, ast_type_id);
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_lower_validate_default_type(state,
                type->data.projection_type.self_type, owner,
                parameter_index, span, item_id, ast_type_id, depth + 1u)
            && cm_lower_validate_default_named(state,
                &type->data.projection_type.trait_type, owner,
                parameter_index, span, item_id, ast_type_id, depth)
            && cm_lower_validate_default_named(state,
                &type->data.projection_type.associated_type, owner,
                parameter_index, span, item_id, ast_type_id, depth);
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        if ((type->data.dyn_trait_type.has_principal
                && !cm_lower_validate_default_named(state,
                    &type->data.dyn_trait_type.principal_trait, owner,
                    parameter_index, span, item_id, ast_type_id, depth))
            || !cm_lower_validate_default_region(state,
                &type->data.dyn_trait_type.region, owner, parameter_index,
                span, item_id, ast_type_id)) return 0;
        for (index = 0u;
             index < type->data.dyn_trait_type.auto_trait_count; ++index) {
            if (!cm_lower_validate_default_named(state,
                    &type->data.dyn_trait_type.auto_traits[index], owner,
                    parameter_index, span, item_id, ast_type_id, depth)) {
                return 0;
            }
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.equality_count; ++index) {
            if (!cm_lower_validate_default_type(state,
                    type->data.dyn_trait_type.equalities[index].value,
                    owner, parameter_index, span, item_id, ast_type_id,
                    depth + 1u)) {
                return 0;
            }
        }
        return 1;
    }
    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, item_id,
        ast_type_id, CM_AST_PATH_NONE, CM_HIR_INVALID_ARGUMENT,
        "generic type default has an unknown HIR type kind");
    return 0;
}

static int cm_lower_record_self_type_matches(
    const CmLowerState *state, const CmLowerItemRecord *impl_record,
    const CmLowerItemRecord *type_record)
{
    const CmAstItem *impl_item;
    const CmAstType *self_type;
    const CmAstPath *self_path;
    const CmInternedString *self_name;
    const CmInternedString *type_name;

    if (impl_record == NULL || type_record == NULL
        || impl_record->kind != CM_AST_ITEM_IMPL
        || impl_record->owner_module != type_record->owner_module) {
        return 0;
    }
    impl_item = cm_ast_get_item(impl_record->ast, impl_record->ast_id);
    self_type = impl_item == NULL ? NULL
        : cm_ast_get_type(impl_record->ast,
            impl_item->data.impl_item.self_type);
    self_path = self_type == NULL || self_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(impl_record->ast, self_type->path);
    if (impl_item == NULL
        || impl_item->data.impl_item.trait_type != CM_AST_TYPE_NONE
        || self_path == NULL || self_path->absolute
        || self_path->segment_count != 1u || self_path->segments == NULL
        || self_path->segments[0].argument_count != 0u) {
        return 0;
    }
    self_name = cm_ast_get_string(impl_record->ast,
        self_path->segments[0].name);
    type_name = cm_interner_get(&state->hir->strings,
        type_record->hir_name);
    return self_name != NULL && type_name != NULL
        && self_name->len == type_name->len
        && memcmp(self_name->bytes, type_name->bytes,
            self_name->len) == 0;
}

/* Resolve the `Self::NAME` form used by an array length while lowering an
 * item owned by an inherent impl (or by the impl's target type).  Array
 * lengths are intentionally represented as unevaluated const definitions;
 * this lookup therefore only authenticates the associated-const identity and
 * does not attempt to evaluate its initializer. */
static const CmLowerItemRecord *cm_lower_associated_const_length(
    const CmLowerState *state, CmHirDefId owner,
    const CmInternedString *text)
{
    const CmLowerItemRecord *owner_record;
    const CmLowerItemRecord *type_record;
    const CmLowerItemRecord *direct_impl;
    const CmLowerItemRecord *result;
    size_t index;
    size_t position;
    size_t name_start;
    size_t name_end;
    uint32_t matches;

    if (state == NULL || text == NULL || text->bytes == NULL) return NULL;
    /* The parser stores the array bound as captured source text, so tolerate
     * whitespace around both path segments and the `::` separator. */
    position = 0u;
    while (position < text->len
        && (text->bytes[position] == ' ' || text->bytes[position] == '\t'
            || text->bytes[position] == '\r'
            || text->bytes[position] == '\n')) ++position;
    if (text->len - position < 4u
        || memcmp(text->bytes + position, "Self", 4u) != 0) return NULL;
    position += 4u;
    if (position < text->len
        && ((text->bytes[position] >= 'A'
                && text->bytes[position] <= 'Z')
            || (text->bytes[position] >= 'a'
                && text->bytes[position] <= 'z')
            || (text->bytes[position] >= '0'
                && text->bytes[position] <= '9')
            || text->bytes[position] == '_')) return NULL;
    while (position < text->len
        && (text->bytes[position] == ' ' || text->bytes[position] == '\t'
            || text->bytes[position] == '\r'
            || text->bytes[position] == '\n')) ++position;
    if (position + 2u > text->len || text->bytes[position] != ':'
        || text->bytes[position + 1u] != ':') return NULL;
    position += 2u;
    while (position < text->len
        && (text->bytes[position] == ' ' || text->bytes[position] == '\t'
            || text->bytes[position] == '\r'
            || text->bytes[position] == '\n')) ++position;
    if (position >= text->len
        || !((text->bytes[position] >= 'A'
                && text->bytes[position] <= 'Z')
            || (text->bytes[position] >= 'a'
                && text->bytes[position] <= 'z')
            || text->bytes[position] == '_')) return NULL;
    name_start = position++;
    while (position < text->len
        && ((text->bytes[position] >= 'A'
                && text->bytes[position] <= 'Z')
            || (text->bytes[position] >= 'a'
                && text->bytes[position] <= 'z')
            || (text->bytes[position] >= '0'
                && text->bytes[position] <= '9')
            || text->bytes[position] == '_')) ++position;
    name_end = position;
    while (position < text->len
        && (text->bytes[position] == ' ' || text->bytes[position] == '\t'
            || text->bytes[position] == '\r'
            || text->bytes[position] == '\n')) ++position;
    if (position != text->len || name_end == name_start) return NULL;

    owner_record = cm_lower_find_record_by_definition(state, owner);
    if (owner_record == NULL) return NULL;
    type_record = owner_record;
    direct_impl = NULL;
    if (owner_record->kind == CM_AST_ITEM_IMPL) {
        direct_impl = owner_record;
        type_record = NULL;
    } else if (owner_record->parent_kind == CM_LOWER_PARENT_IMPL) {
        direct_impl = cm_lower_find_record_by_definition(state,
            owner_record->parent_definition);
        type_record = NULL;
    }

    result = NULL;
    matches = 0u;
    for (index = 0u; index < state->item_records.len; ++index) {
        const CmLowerItemRecord *impl_record;
        size_t associated_index;

        impl_record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if (impl_record == NULL || impl_record->kind != CM_AST_ITEM_IMPL
            || (direct_impl != NULL
                && !cm_hir_def_id_equal(impl_record->definition,
                    direct_impl->definition))
            || (direct_impl == NULL
                && !cm_lower_record_self_type_matches(state, impl_record,
                    type_record))) continue;
        for (associated_index = 0u;
             associated_index < state->item_records.len;
             ++associated_index) {
            const CmLowerItemRecord *associated;
            const CmInternedString *associated_name;

            associated = (const CmLowerItemRecord *)cm_vec_at_const(
                &state->item_records, associated_index);
            associated_name = associated == NULL ? NULL
                : cm_interner_get(&state->hir->strings,
                    associated->hir_name);
            if (associated != NULL && associated->kind == CM_AST_ITEM_CONST
                && cm_hir_def_id_equal(associated->parent_definition,
                    impl_record->definition)
                && associated_name != NULL
                && associated_name->len == name_end - name_start
                && memcmp(associated_name->bytes,
                    text->bytes + name_start, name_end - name_start) == 0) {
                result = associated;
                matches += 1u;
            }
        }
    }
    return matches == 1u ? result : NULL;
}

static const CmLowerItemRecord *cm_lower_const_default_definition(
    CmLowerState *state, const CmLowerItemRecord *owner_record,
    const CmAstPath *path, CmSpan span, CmAstItemId ast_item_id)
{
    const CmLowerItemRecord *type_record;
    const CmLowerItemRecord *result;
    size_t index;
    uint32_t matches;

    type_record = NULL;
    result = NULL;
    matches = 0u;
    if (path->absolute || path->segment_count == 0u
        || path->segment_count > 2u || path->segments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "const generic default path form is unsupported");
        return NULL;
    }
    for (index = 0u; index < path->segment_count; ++index) {
        if (path->segments[index].argument_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "const generic default path has generic arguments");
            return NULL;
        }
    }
    if (path->segment_count == 1u) {
        for (index = 0u; index < state->item_records.len; ++index) {
            const CmLowerItemRecord *record;

            record = (const CmLowerItemRecord *)cm_vec_at_const(
                &state->item_records, index);
            if (record != NULL && record->kind == CM_AST_ITEM_CONST
                && record->owner_module == owner_record->owner_module
                && cm_hir_def_id_is_none(record->parent_definition)
                && cm_lower_hir_name_matches_ast(state, record->hir_name,
                    path->segments[0].name)) {
                result = record;
                matches += 1u;
            }
        }
    } else {
        for (index = 0u; index < state->item_records.len; ++index) {
            const CmLowerItemRecord *record;

            record = (const CmLowerItemRecord *)cm_vec_at_const(
                &state->item_records, index);
            if (record != NULL
                && (record->kind == CM_AST_ITEM_STRUCT
                    || record->kind == CM_AST_ITEM_UNION
                    || record->kind == CM_AST_ITEM_ENUM)
                && record->owner_module == owner_record->owner_module
                && cm_hir_def_id_is_none(record->parent_definition)
                && cm_lower_hir_name_matches_ast(state, record->hir_name,
                    path->segments[0].name)) {
                if (type_record != NULL) {
                    type_record = NULL;
                    break;
                }
                type_record = record;
            }
        }
        if (type_record != NULL) {
            for (index = 0u; index < state->item_records.len; ++index) {
                const CmLowerItemRecord *impl_record;
                size_t associated_index;

                impl_record = (const CmLowerItemRecord *)cm_vec_at_const(
                    &state->item_records, index);
                if (!cm_lower_record_self_type_matches(state, impl_record,
                        type_record)) {
                    continue;
                }
                for (associated_index = 0u;
                     associated_index < state->item_records.len;
                     ++associated_index) {
                    const CmLowerItemRecord *associated;

                    associated = (const CmLowerItemRecord *)cm_vec_at_const(
                        &state->item_records, associated_index);
                    if (associated != NULL
                        && associated->kind == CM_AST_ITEM_CONST
                        && cm_hir_def_id_equal(
                            associated->parent_definition,
                            impl_record->definition)
                        && cm_lower_hir_name_matches_ast(state,
                            associated->hir_name,
                            path->segments[1].name)) {
                        result = associated;
                        matches += 1u;
                    }
                }
            }
        }
    }
    if (matches == 1u) return result;
    cm_lower_fail(state,
        matches == 0u ? CM_HIR_LOWER_UNRESOLVED_PATH
                      : CM_HIR_LOWER_INVALID_AST,
        span, ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
        matches == 0u
            ? "const generic default path is unresolved"
            : "const generic default path is ambiguous");
    return NULL;
}

static int cm_lower_const_generic_default(CmLowerState *state,
    const CmLowerItemRecord *record, const CmAstGenericParam *ast_parameter,
    uint32_t parameter_index, CmHirTypeId declared_type,
    CmHirConstArg *out_constant)
{
    const CmAstExpr *expression;
    const CmAstPath *path;
    const CmLowerGenericRecord *generic;
    const CmLowerItemRecord *definition;
    CmSpan span;

    span = record->effective_span;
    expression = cm_ast_get_expr(state->ast,
        ast_parameter->default_const_expr);
    if (expression == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            record->ast_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVALID_ID,
            "const generic default expression is invalid");
        return 0;
    }
    if (expression->kind == CM_AST_EXPR_BLOCK) {
        if (expression->attribute_count != 0u
            || expression->data.block.inner_attribute_count != 0u
            || expression->data.block.statement_count != 0u
            || expression->data.block.tail == CM_AST_EXPR_NONE
            || expression->data.block.is_const
            || expression->data.block.is_unsafe) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, expression->span), record->ast_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "const generic default block is not a plain path wrapper");
            return 0;
        }
        expression = cm_ast_get_expr(state->ast,
            expression->data.block.tail);
    }
    if (expression == NULL || expression->kind != CM_AST_EXPR_PATH
        || expression->attribute_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            expression == NULL ? span
                : cm_lower_span(state, expression->span), record->ast_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "const generic default expression must be a plain const path");
        return 0;
    }
    path = cm_ast_get_path(state->ast, expression->data.path.path);
    if (!cm_lower_ast_path_storage_valid(path)
        || path->segment_count == 0u || path->segments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, expression->span), record->ast_id,
            CM_AST_TYPE_NONE, expression->data.path.path,
            CM_HIR_INVALID_ID,
            "const generic default path storage is invalid");
        return 0;
    }
    memset(out_constant, 0, sizeof(*out_constant));
    out_constant->type = declared_type;
    generic = path->segment_count == 1u && !path->absolute
        ? cm_lower_find_generic(state, record->definition,
            path->segments[0].name) : NULL;
    if (generic != NULL) {
        const CmHirGenericParam *source_parameter;
        const CmHirType *source_type;
        const CmHirType *target_type;
        int types_match;

        if (generic->kind != CM_HIR_GENERIC_CONST
            || !cm_lower_validate_default_parameter(state,
                generic->hir_id, record->definition, parameter_index,
                cm_lower_span(state, expression->span), record->ast_id,
                CM_AST_TYPE_NONE)) {
            if (!state->failed) {
                cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE,
                    cm_lower_span(state, expression->span), record->ast_id,
                    CM_AST_TYPE_NONE, expression->data.path.path,
                    CM_HIR_OK,
                    "const generic default names a non-const parameter");
            }
            return 0;
        }
        source_parameter = cm_hir_get_generic_param(state->hir,
            generic->hir_id);
        source_type = source_parameter == NULL ? NULL
            : cm_hir_get_type(state->hir, source_parameter->declared_type);
        target_type = cm_hir_get_type(state->hir, declared_type);
        types_match = source_parameter != NULL
            && (source_parameter->declared_type == declared_type
                || (source_type != NULL && target_type != NULL
                    && source_type->kind == target_type->kind
                    && (source_type->kind == CM_HIR_TYPE_BOOL_KIND
                        || source_type->kind == CM_HIR_TYPE_CHAR_KIND
                        || (source_type->kind == CM_HIR_TYPE_INTEGER_KIND
                            && source_type->data.integer_type.kind
                                == target_type->data.integer_type.kind))));
        if (!types_match) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, expression->span), record->ast_id,
                CM_AST_TYPE_NONE, expression->data.path.path, CM_HIR_OK,
                "const generic default parameter type differs from its "
                "declared type");
            return 0;
        }
        out_constant->kind = CM_HIR_CONST_PARAMETER;
        out_constant->data.parameter = generic->hir_id;
        return 1;
    }
    definition = cm_lower_const_default_definition(state, record, path,
        cm_lower_span(state, expression->span), record->ast_id);
    if (definition == NULL) return 0;
    out_constant->kind = CM_HIR_CONST_UNEVALUATED;
    out_constant->data.definition = definition->definition;
    return 1;
}

/* Publish defaults in dependency order.  Trait method declarations need to
 * see defaults on ordinary ADTs (e.g. ControlFlow<B, C = ()>), but aliases and
 * const defaults may depend on later item/projection binding. */
enum {
    CM_LOWER_DEFAULTS_REMAINING = 0,
    CM_LOWER_DEFAULTS_TRAIT = 1,
    CM_LOWER_DEFAULTS_ADT_TYPES = 2
};

static int cm_lower_predeclare_all_generic_defaults(CmLowerState *state,
    int defaults_kind)
{
    size_t record_index;

    for (record_index = 0u;
         record_index < state->item_records.len && !state->failed;
         ++record_index) {
        CmLowerItemRecord *record;
        const CmAstItem *ast_item;
        uint32_t parameter_index;

        record = (CmLowerItemRecord *)cm_vec_at(&state->item_records,
            record_index);
        if (record == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ 0u, 0u, 0u }, CM_AST_ITEM_NONE,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "generic-default predeclaration lost an item record");
            return 0;
        }
        state->ast = record->ast;
        state->source = record->source;
        state->graph_module = record->graph_module;
        state->generated_span = record->effective_span;
        state->use_generated_span = record->is_generated;
        ast_item = cm_ast_get_item(state->ast, record->ast_id);
        if (ast_item == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                record->effective_span, record->ast_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "generic-default predeclaration lost its AST item");
            return 0;
        }
        if (defaults_kind == CM_LOWER_DEFAULTS_TRAIT
                && ast_item->kind != CM_AST_ITEM_TRAIT) {
            continue;
        }
        if (defaults_kind == CM_LOWER_DEFAULTS_ADT_TYPES
                && (ast_item->kind != CM_AST_ITEM_STRUCT
                    && ast_item->kind != CM_AST_ITEM_UNION
                    && ast_item->kind != CM_AST_ITEM_ENUM)) {
            continue;
        }
        if (defaults_kind == CM_LOWER_DEFAULTS_REMAINING
                && ast_item->kind == CM_AST_ITEM_TRAIT) {
            continue;
        }
        for (parameter_index = 0u;
             parameter_index < ast_item->generic_parameter_count;
             ++parameter_index) {
            const CmAstGenericParam *ast_parameter;
            const CmHirGenericParam *hir_parameter;
            CmHirGenericArg argument;
            CmHirGenericParamId parameter_id;
            CmHirStatus status;

            ast_parameter = &ast_item->generic_parameters[parameter_index];
            if (defaults_kind == CM_LOWER_DEFAULTS_ADT_TYPES
                    && ast_parameter->default_type == CM_AST_TYPE_NONE) {
                continue;
            }
            if (defaults_kind == CM_LOWER_DEFAULTS_REMAINING
                    && (ast_parameter->default_type != CM_AST_TYPE_NONE
                        && (ast_item->kind == CM_AST_ITEM_STRUCT
                            || ast_item->kind == CM_AST_ITEM_UNION
                            || ast_item->kind == CM_AST_ITEM_ENUM))) {
                continue;
            }
            if (ast_parameter->default_type == CM_AST_TYPE_NONE
                && ast_parameter->default_const_expr
                    == CM_AST_EXPR_NONE) {
                continue;
            }
            if (record->generic_parameter_start ==
                    CM_HIR_GENERIC_PARAM_NONE
                || parameter_index > UINT32_MAX
                    - record->generic_parameter_start) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    record->effective_span, record->ast_id,
                    ast_parameter->default_type, CM_AST_PATH_NONE,
                    CM_HIR_OK, "generic-default parameter ID overflow");
                return 0;
            }
            parameter_id = record->generic_parameter_start
                + parameter_index;
            hir_parameter = cm_hir_get_generic_param(state->hir,
                parameter_id);
            if (hir_parameter == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    record->effective_span, record->ast_id,
                    ast_parameter->default_type, CM_AST_PATH_NONE,
                    CM_HIR_INVALID_ID,
                    "generic-default parameter is missing from HIR");
                return 0;
            }
            memset(&argument, 0, sizeof(argument));
            if (ast_parameter->default_type != CM_AST_TYPE_NONE) {
                argument.kind = CM_HIR_GENERIC_ARG_TYPE;
                argument.data.type = cm_lower_type(state,
                    ast_parameter->default_type, record->owner_module,
                    record->definition);
                if (state->failed) return 0;
                if (!cm_lower_validate_default_type(state,
                        argument.data.type, record->definition,
                        parameter_index, record->effective_span,
                        record->ast_id, ast_parameter->default_type, 0u)) {
                    return 0;
                }
            } else {
                argument.kind = CM_HIR_GENERIC_ARG_CONST;
                if (hir_parameter->kind != CM_HIR_GENERIC_CONST
                    || !cm_lower_const_generic_default(state, record,
                        ast_parameter, parameter_index,
                        hir_parameter->declared_type,
                        &argument.data.constant)) {
                    if (!state->failed) {
                        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                            record->effective_span, record->ast_id,
                            CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                            CM_HIR_INVALID_ARGUMENT,
                            "const default belongs to a non-const generic");
                    }
                    return 0;
                }
            }
            status = cm_hir_set_generic_param_default(state->hir,
                parameter_id, &argument);
            if (status != CM_HIR_OK) {
                cm_lower_fail_hir(state, record->effective_span,
                    record->ast_id, status,
                    "cannot assign HIR generic parameter default");
                return 0;
            }
        }
    }
    return !state->failed;
}

static int cm_lower_visibility(CmLowerState *state,
    CmAstVisibility ast_visibility, CmHirModuleId module, CmSpan span,
    CmAstItemId ast_item_id, CmHirVisibility *out_visibility)
{
    const CmHirModule *module_value;

    memset(out_visibility, 0, sizeof(*out_visibility));
    out_visibility->restriction = cm_hir_def_id_none();
    switch (ast_visibility.kind) {
    case CM_AST_VIS_INHERITED:
        out_visibility->kind = CM_HIR_VIS_PRIVATE;
        return 1;
    case CM_AST_VIS_PUBLIC:
        out_visibility->kind = CM_HIR_VIS_PUBLIC;
        return 1;
    case CM_AST_VIS_CRATE:
        out_visibility->kind = CM_HIR_VIS_CRATE;
        return 1;
    case CM_AST_VIS_SELF:
        module_value = cm_hir_get_module(state->hir, module);
        if (module_value == NULL) {
            break;
        }
        out_visibility->kind = CM_HIR_VIS_RESTRICTED;
        out_visibility->restriction = module_value->definition;
        return 1;
    case CM_AST_VIS_SUPER:
        module = cm_lower_parent_module(state, module);
        if (module == CM_HIR_MODULE_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "pub(super) appears in the crate root");
            return 0;
        }
        module_value = cm_hir_get_module(state->hir, module);
        if (module_value == NULL) {
            break;
        }
        out_visibility->kind = CM_HIR_VIS_RESTRICTED;
        out_visibility->restriction = module_value->definition;
        return 1;
    case CM_AST_VIS_RESTRICTED:
    {
        const CmAstPath *path;
        const CmLowerItemRecord *record;
        CmHirDefId definition;
        CmLowerLookupResult lookup;
        CmHirLowerResolution resolution;

        path = cm_ast_get_path(state->ast, ast_visibility.restriction);
        if (path == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
                CM_AST_TYPE_NONE, ast_visibility.restriction, CM_HIR_OK,
                "restricted visibility contains an invalid path");
            return 0;
        }
        lookup = cm_lower_lookup_path(state, path, module,
            CM_HIR_LOWER_PATH_VISIBILITY, &record, &definition);
        (void)record;
        if (lookup == CM_LOWER_LOOKUP_STALE_GRAPH) {
            cm_lower_fail(state, CM_HIR_LOWER_STALE_GRAPH, span,
                ast_item_id, CM_AST_TYPE_NONE, ast_visibility.restriction,
                CM_HIR_OK,
                "graph or import revision changed during visibility lookup");
            return 0;
        }
        if (lookup == CM_LOWER_LOOKUP_RESOLVER_ERROR) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                ast_item_id, CM_AST_TYPE_NONE, ast_visibility.restriction,
                CM_HIR_OK, "local-crate visibility resolution failed");
            return 0;
        }
        if (lookup == CM_LOWER_LOOKUP_MODULE) {
            out_visibility->kind = CM_HIR_VIS_RESTRICTED;
            out_visibility->restriction = definition;
            return 1;
        }
        if (lookup == CM_LOWER_LOOKUP_WRONG_NAMESPACE
            || lookup == CM_LOWER_LOOKUP_DEFINITION
            || lookup == CM_LOWER_LOOKUP_ALIAS) {
            cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
                ast_item_id, CM_AST_TYPE_NONE, ast_visibility.restriction,
                CM_HIR_OK, "visibility path does not name a module");
            return 0;
        }
        memset(&resolution, 0, sizeof(resolution));
        if (state->options->resolve_path != NULL) {
            resolution = state->options->resolve_path(
                state->options->resolve_context, state->ast,
                ast_visibility.restriction, module,
                CM_HIR_LOWER_PATH_VISIBILITY);
        }
        if (!cm_lower_graph_snapshot_matches(state)) {
            cm_lower_fail(state, CM_HIR_LOWER_STALE_GRAPH, span,
                ast_item_id, CM_AST_TYPE_NONE, ast_visibility.restriction,
                CM_HIR_OK,
                "graph or import resolver changed in the path callback");
            return 0;
        }
        if (resolution.kind == CM_HIR_LOWER_DEFINITION
            && !cm_hir_def_id_is_none(resolution.definition)) {
            const CmHirDefinition *resolved_definition;

            resolved_definition = cm_hir_lookup_definition(state->hir,
                resolution.definition);
            if (resolved_definition == NULL
                || resolved_definition->kind != CM_HIR_DEFINITION_MODULE) {
                cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                    ast_item_id, CM_AST_TYPE_NONE,
                    ast_visibility.restriction, CM_HIR_OK,
                    "resolver returned a visibility definition that is not "
                    "a loaded module");
                return 0;
            }
            out_visibility->kind = CM_HIR_VIS_RESTRICTED;
            out_visibility->restriction = resolution.definition;
            return 1;
        }
        cm_lower_fail(state,
            resolution.kind == CM_HIR_LOWER_RESOLVER_ERROR
                ? CM_HIR_LOWER_RESOLVER_FAILURE
                : CM_HIR_LOWER_UNRESOLVED_PATH,
            span, ast_item_id, CM_AST_TYPE_NONE,
            ast_visibility.restriction, CM_HIR_OK,
            "restricted visibility path is unresolved");
        return 0;
    }
    }
    cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span, ast_item_id,
        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
        "cannot obtain visibility module definition");
    return 0;
}

static CmHirAggregateForm cm_lower_aggregate_form(CmAstFieldForm form)
{
    switch (form) {
    case CM_AST_FIELDS_UNIT:
        return CM_HIR_AGGREGATE_UNIT;
    case CM_AST_FIELDS_TUPLE:
        return CM_HIR_AGGREGATE_TUPLE;
    case CM_AST_FIELDS_NAMED:
        return CM_HIR_AGGREGATE_NAMED;
    }
    return CM_HIR_AGGREGATE_UNIT;
}

static CmHirField *cm_lower_fields(CmLowerState *state,
    const CmAstField *ast_fields, uint32_t field_count, CmAstFieldForm form,
    CmHirModuleId module, CmHirDefId owner, CmSpan span,
    CmAstItemId ast_item_id)
{
    CmHirField *fields;
    uint32_t index;

    if (field_count == 0u) {
        return NULL;
    }
    if (ast_fields == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "field count is nonzero but field storage is absent");
        return NULL;
    }
    fields = (CmHirField *)cm_alloc_zeroed((size_t)field_count,
        sizeof(CmHirField));
    for (index = 0u; index < field_count && !state->failed; ++index) {
        if (form == CM_AST_FIELDS_NAMED) {
            fields[index].name = cm_lower_copy_string(state,
                ast_fields[index].name, span, ast_item_id);
        }
        fields[index].type = cm_lower_type(state, ast_fields[index].type,
            module, owner);
        fields[index].span = span;
        (void)cm_lower_visibility(state, ast_fields[index].visibility,
            module, span, ast_item_id, &fields[index].visibility);
    }
    if (state->failed) {
        cm_free(fields);
        return NULL;
    }
    return fields;
}

static int cm_lower_pattern_binding(CmLowerState *state,
    CmAstPatternId pattern_id, CmSpan item_span, CmAstItemId ast_item_id,
    CmInternId *out_name, CmHirMutability *out_mutability, CmSpan *out_span,
    CmHirBindingKind *out_binding_kind,
    CmHirParameterBindingMode *out_binding_mode)
{
    const CmAstPattern *pattern;

    pattern = cm_ast_get_pattern(state->ast, pattern_id);
    if (pattern == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "function parameter has an invalid pattern ID");
        return 0;
    }
    if (pattern->kind == CM_AST_PATTERN_WILDCARD) {
        *out_name = CM_INTERN_ID_NONE;
        *out_mutability = CM_HIR_IMMUTABLE;
        *out_span = cm_lower_span(state, pattern->span);
        *out_binding_kind = CM_HIR_BINDING_DISCARD;
        *out_binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
        return 1;
    }
    if (pattern->kind != CM_AST_PATTERN_BINDING
        || pattern->data.binding.subpattern != CM_AST_PATTERN_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
            cm_lower_span(state, pattern->span), ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "function parameter patterns require typed pattern HIR");
        return 0;
    }
    *out_name = cm_lower_copy_string(state, pattern->data.binding.name,
        cm_lower_span(state, pattern->span), ast_item_id);
    *out_mutability = !pattern->data.binding.is_ref
            && pattern->data.binding.is_mutable
        ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;
    *out_span = cm_lower_span(state, pattern->span);
    *out_binding_kind = CM_HIR_BINDING_NAMED;
    *out_binding_mode = pattern->data.binding.is_ref
        ? pattern->data.binding.is_mutable
            ? CM_HIR_PARAMETER_BINDING_REF_MUTABLE
            : CM_HIR_PARAMETER_BINDING_REF_SHARED
        : CM_HIR_PARAMETER_BINDING_MOVE;
    return !state->failed;
}

static int cm_lower_tuple_parameter_pattern(CmLowerState *state,
    CmAstPatternId pattern_id, CmSpan item_span, CmAstItemId ast_item_id,
    uint32_t parameter_index, CmHirFunctionParameter *out_parameter,
    CmHirLocal *out_locals)
{
    const CmAstPattern *pattern;
    const CmHirType *tuple_type;
    uint32_t binding_index;

    pattern = cm_ast_get_pattern(state->ast, pattern_id);
    tuple_type = out_parameter == NULL ? NULL
        : cm_hir_get_type(state->hir, out_parameter->type);
    if (pattern == NULL || out_parameter == NULL || out_locals == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "tuple parameter pattern has invalid storage");
        return 0;
    }
    if (pattern->kind != CM_AST_PATTERN_TUPLE
        || pattern->data.list.has_rest
        || pattern->data.list.pattern_count
            != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
        || pattern->data.list.patterns == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
            cm_lower_span(state, pattern->span), ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "function tuple parameter requires exactly two bindings");
        return 0;
    }
    if (tuple_type == NULL || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
        || tuple_type->data.tuple_type.element_count
            != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
        || tuple_type->data.tuple_type.elements == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE,
            cm_lower_span(state, pattern->span), ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "function tuple parameter requires a two-element tuple type");
        return 0;
    }
    out_parameter->name = CM_INTERN_ID_NONE;
    out_parameter->span = cm_lower_span(state, pattern->span);
    out_parameter->binding_kind = CM_HIR_BINDING_TUPLE_PATTERN;
    out_parameter->binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    for (binding_index = 0u;
         binding_index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
         ++binding_index) {
        const CmAstPattern *binding;
        CmSpan binding_span;
        CmInternId binding_name;

        binding = cm_ast_get_pattern(state->ast,
            pattern->data.list.patterns[binding_index]);
        if (binding == NULL || binding->kind != CM_AST_PATTERN_BINDING
            || binding->data.binding.subpattern != CM_AST_PATTERN_NONE
            || binding->data.binding.is_ref
            || binding->data.binding.is_mutable) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
                binding == NULL ? out_parameter->span
                    : cm_lower_span(state, binding->span),
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "function tuple parameter supports only immutable move "
                "bindings");
            return 0;
        }
        binding_span = cm_lower_span(state, binding->span);
        binding_name = cm_lower_copy_string(state,
            binding->data.binding.name, binding_span, ast_item_id);
        if (state->failed) return 0;
        out_parameter->tuple_bindings[binding_index].name = binding_name;
        out_parameter->tuple_bindings[binding_index].span = binding_span;
        out_locals[binding_index].name = binding_name;
        out_locals[binding_index].type =
            tuple_type->data.tuple_type.elements[binding_index];
        out_locals[binding_index].mutability = CM_HIR_IMMUTABLE;
        out_locals[binding_index].span = binding_span;
        out_locals[binding_index].parameter_index = parameter_index;
        out_locals[binding_index].parameter_binding_index = binding_index;
    }
    if (out_parameter->tuple_bindings[0].name
            == out_parameter->tuple_bindings[1].name) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
            out_parameter->span, ast_item_id, CM_AST_TYPE_NONE,
            CM_AST_PATH_NONE, CM_HIR_OK,
            "function tuple parameter bindings must have distinct names");
        return 0;
    }
    return 1;
}

static CmHirTypeId cm_lower_parameter_binding_type(CmLowerState *state,
    CmHirTypeId parameter_type, CmHirParameterBindingMode binding_mode,
    CmSpan span)
{
    CmHirType reference;

    if (binding_mode == CM_HIR_PARAMETER_BINDING_MOVE) {
        return parameter_type;
    }
    memset(&reference, 0, sizeof(reference));
    reference.kind = CM_HIR_TYPE_REFERENCE_KIND;
    reference.span = span;
    reference.data.reference_type.region.kind = CM_HIR_REGION_INFER;
    reference.data.reference_type.region.data.inference_variable =
        state->next_region_inference;
    state->next_region_inference += 1u;
    reference.data.reference_type.pointee = parameter_type;
    reference.data.reference_type.mutability =
        binding_mode == CM_HIR_PARAMETER_BINDING_REF_MUTABLE
            ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;
    return cm_lower_add_type(state, &reference, CM_AST_TYPE_NONE);
}

static int cm_lower_receiver_pattern(CmLowerState *state,
    CmAstPatternId pattern_id, CmSpan item_span, CmAstItemId ast_item_id,
    int has_explicit_type, CmHirReceiverKind *out_receiver,
    CmInternId *out_name, CmHirMutability *out_mutability,
    CmSpan *out_span)
{
    const CmAstPattern *pattern;
    const CmAstPattern *binding;

    pattern = cm_ast_get_pattern(state->ast, pattern_id);
    if (pattern == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "receiver has an invalid pattern ID");
        return 0;
    }
    binding = pattern;
    if (pattern->kind == CM_AST_PATTERN_REFERENCE) {
        if (has_explicit_type) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
                cm_lower_span(state, pattern->span), ast_item_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "typed receivers require a plain self binding");
            return 0;
        }
        binding = cm_ast_get_pattern(state->ast,
            pattern->data.reference.pattern);
        *out_receiver = pattern->data.reference.is_mutable
            ? CM_HIR_RECEIVER_REF_MUTABLE
            : CM_HIR_RECEIVER_REF_SHARED;
    } else {
        *out_receiver = has_explicit_type ? CM_HIR_RECEIVER_CUSTOM
                                         : CM_HIR_RECEIVER_VALUE;
    }
    if (binding == NULL || binding->kind != CM_AST_PATTERN_BINDING
        || binding->data.binding.subpattern != CM_AST_PATTERN_NONE
        || binding->data.binding.is_ref
        || !cm_lower_string_is(state, binding->data.binding.name, "self")) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
            cm_lower_span(state, pattern->span), ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "receiver pattern must be self, mut self, &self, or &mut self");
        return 0;
    }
    *out_name = cm_lower_copy_string(state,
        binding->data.binding.name, cm_lower_span(state, binding->span),
        ast_item_id);
    *out_mutability = binding->data.binding.is_mutable
        ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;
    *out_span = cm_lower_span(state, pattern->span);
    return !state->failed;
}

static CmHirTypeId cm_lower_receiver_type(CmLowerState *state,
    CmHirReceiverKind receiver, CmHirDefId owner, CmSpan span,
    CmAstItemId ast_item_id, CmInternId lifetime)
{
    CmHirDefId self_owner;
    CmHirDefId trait_definition;
    CmHirTypeId self_type;
    CmHirType reference;

    (void)ast_item_id;
    if (!cm_lower_self_context(state, owner, &self_owner,
            &trait_definition)) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "implicit receiver is outside a trait or impl method");
        return CM_HIR_TYPE_NONE;
    }
    (void)trait_definition;
    self_type = cm_lower_self_type(state, CM_AST_TYPE_NONE, span,
        self_owner);
    if (state->failed || receiver == CM_HIR_RECEIVER_VALUE) {
        return self_type;
    }
    memset(&reference, 0, sizeof(reference));
    reference.kind = CM_HIR_TYPE_REFERENCE_KIND;
    reference.span = span;
    if (!cm_lower_lifetime(state, lifetime, owner, span,
            &reference.data.reference_type.region)) {
        return CM_HIR_TYPE_NONE;
    }
    reference.data.reference_type.pointee = self_type;
    reference.data.reference_type.mutability =
        receiver == CM_HIR_RECEIVER_REF_MUTABLE
            ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;
    return cm_lower_add_type(state, &reference, CM_AST_TYPE_NONE);
}

static const CmHirItem *cm_lower_find_associated_method(
    const CmLowerState *state, CmHirDefId trait_definition,
    CmInternId ast_name, uint32_t *out_matches)
{
    const CmHirItem *result;
    size_t index;
    uint32_t matches;

    result = NULL;
    matches = 0u;
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            index);
        if (item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
            && cm_hir_def_id_equal(item->parent_definition,
                trait_definition)
            && cm_lower_hir_name_matches_ast(state, item->name,
                ast_name)) {
            result = item;
            matches += 1u;
        }
    }
    *out_matches = matches;
    return result;
}

static const CmHirItem *cm_lower_find_associated_const(
    const CmLowerState *state, CmHirDefId trait_definition,
    CmInternId ast_name, uint32_t *out_matches)
{
    const CmHirItem *result;
    size_t index;
    uint32_t matches;

    result = NULL;
    matches = 0u;
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            index);
        if (item != NULL && item->kind == CM_HIR_ITEM_CONST
            && item->data.value_item.body == CM_HIR_BODY_NONE
            && cm_hir_def_id_equal(item->parent_definition,
                trait_definition)
            && cm_lower_hir_name_matches_ast(state, item->name,
                ast_name)) {
            result = item;
            matches += 1u;
        }
    }
    *out_matches = matches;
    return result;
}

static CmHirBodyId cm_lower_body(CmLowerState *state, CmHirDefId owner,
    CmHirTypeId expected_type, CmAstExprId source_expression,
    const CmHirLocal *locals, uint32_t local_count, uint32_t parameter_count,
    CmSpan span, CmAstItemId ast_item_id)
{
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirStatus status;

    if (source_expression == CM_AST_EXPR_NONE) {
        return CM_HIR_BODY_NONE;
    }
    if (cm_ast_get_expr(state->ast, source_expression) == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "body refers to an invalid AST expression ID");
        return CM_HIR_BODY_NONE;
    }
    memset(&body, 0, sizeof(body));
    body.owner = owner;
    body.origin = cm_hir_body_origin_item_source(owner);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = expected_type;
    body.locals = (CmHirLocal *)locals;
    body.local_count = local_count;
    body.parameter_count = parameter_count;
    body.source = state->source;
    body.source_expression_id = source_expression;
    body.span = span;
    status = cm_hir_add_body(state->hir, &body, &body_id);
    if (status != CM_HIR_OK) {
        cm_lower_fail_hir(state, span, ast_item_id, status,
            "cannot add unlowered HIR body");
        return CM_HIR_BODY_NONE;
    }
    return body_id;
}

static CmHirTypeId cm_lower_unit_type(CmLowerState *state, CmSpan span,
    CmAstItemId ast_item_id)
{
    CmHirType type;
    CmHirTypeId id;
    CmHirStatus status;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_UNIT_KIND;
    type.span = span;
    status = cm_hir_add_type(state->hir, &type, &id);
    if (status != CM_HIR_OK) {
        cm_lower_fail_hir(state, span, ast_item_id, status,
            "cannot add unit return type");
        return CM_HIR_TYPE_NONE;
    }
    return id;
}

static CmHirAttribute *cm_lower_item_attributes(CmLowerState *state,
    const CmLowerItemRecord *record, CmSpan item_span,
    CmAstItemId ast_item_id, uint32_t *out_count)
{
    const CmExpandedItem *expanded_item;
    CmHirAttribute *attributes;
    size_t index;

    *out_count = 0u;
    expanded_item = record->expanded_item;
    if (record->graph_effective_item != CM_RESOLVE_EFFECTIVE_ITEM_NONE) {
        attributes = record->effective_attribute_count == 0u ? NULL
            : (CmHirAttribute *)cm_alloc_zeroed(
                (size_t)record->effective_attribute_count,
                sizeof(CmHirAttribute));
        for (index = 0u; index < record->effective_attribute_count;
             ++index) {
            CmResolveEffectiveAttribute effective;
            CmResolveViewStatus status;

            status = cm_module_graph_get_effective_item_attribute(
                state->graph, state->graph_revision, record->graph_module,
                record->graph_effective_item, (uint32_t)index, &effective);
            if (status != CM_RESOLVE_VIEW_OK) {
                cm_free(attributes);
                cm_lower_fail(state,
                    status == CM_RESOLVE_VIEW_STALE_REVISION
                        ? CM_HIR_LOWER_STALE_GRAPH
                        : CM_HIR_LOWER_INVALID_AST,
                    item_span, ast_item_id, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "cannot access graph-owned effective item attribute: %s",
                    cm_resolve_view_status_name(status));
                return NULL;
            }
            if (effective.style != CM_AST_ATTR_OUTER
                || effective.owner.source != record->source
                || effective.owner.item != ast_item_id
                || effective.span.start > effective.span.end) {
                cm_free(attributes);
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "graph-owned effective item attribute is malformed");
                return NULL;
            }
            attributes[index].metadata =
                cm_lower_copy_graph_attribute_metadata(state, state->graph,
                    effective.metadata, effective.span, ast_item_id);
            attributes[index].span = effective.span;
            attributes[index].source_attribute =
                effective.source_attribute;
            attributes[index].expansion_depth =
                effective.expansion_depth;
            if (state->failed) {
                cm_free(attributes);
                return NULL;
            }
        }
        *out_count = record->effective_attribute_count;
        return attributes;
    }
    if (expanded_item == NULL || expanded_item->attribute_count == 0u) {
        return NULL;
    }
    if (expanded_item->attribute_count > (size_t)UINT32_MAX
        || expanded_item->attributes == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            expanded_item->attribute_count > (size_t)UINT32_MAX
                ? CM_HIR_ID_EXHAUSTED : CM_HIR_OK,
            "effective item attributes have invalid storage or count");
        return NULL;
    }
    attributes = (CmHirAttribute *)cm_alloc_zeroed(
        expanded_item->attribute_count, sizeof(CmHirAttribute));
    for (index = 0u; index < expanded_item->attribute_count; ++index) {
        const CmEffectiveAttribute *effective;

        effective = &expanded_item->attributes[index];
        if (effective->style != CM_AST_ATTR_OUTER
            || (effective->meta_length != 0u
                && effective->meta == NULL)) {
            cm_free(attributes);
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK, "effective item attribute is malformed");
            return NULL;
        }
        attributes[index].metadata = cm_interner_intern(
            &state->hir->strings, effective->meta,
            effective->meta_length);
        attributes[index].span = cm_lower_span(state, effective->span);
        attributes[index].source_attribute = effective->source_id;
        attributes[index].expansion_depth =
            effective->expansion_depth;
    }
    *out_count = (uint32_t)expanded_item->attribute_count;
    return attributes;
}

static int cm_lower_item_header(CmLowerState *state, CmAstItemId ast_item_id,
    const CmAstItem *ast_item, const CmLowerItemRecord *record,
    CmHirItem *out_item)
{
    const CmHirItem *implemented_trait;
    const CmHirItem *parent_impl;
    CmSpan span;

    span = cm_lower_span(state, ast_item->span);
    if (ast_item->is_default != 0 && ast_item->is_default != 1) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "item has an invalid default specialization flag");
        return 0;
    }
    if (ast_item->is_default) {
        parent_impl = record->parent_kind == CM_LOWER_PARENT_IMPL
            ? cm_lower_bound_item(state, record->parent_definition) : NULL;
        implemented_trait = parent_impl != NULL
                && parent_impl->kind == CM_HIR_ITEM_IMPL
                && parent_impl->data.impl_item.has_trait
            ? cm_lower_bound_item(state,
                parent_impl->data.impl_item.trait_type.definition) : NULL;
        if (record->is_foreign
            || record->parent_kind != CM_LOWER_PARENT_IMPL
            || (ast_item->kind != CM_AST_ITEM_FUNCTION
                && ast_item->kind != CM_AST_ITEM_TYPE_ALIAS
                && ast_item->kind != CM_AST_ITEM_CONST)
            || parent_impl == NULL
            || parent_impl->kind != CM_HIR_ITEM_IMPL
            || !parent_impl->data.impl_item.has_trait
            || parent_impl->data.impl_item.is_negative
            || implemented_trait == NULL
            || implemented_trait->kind != CM_HIR_ITEM_TRAIT
            || implemented_trait->data.trait_item.is_auto) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "default specialization is supported only on fn, type, "
                "and const definitions directly inside a positive "
                "ordinary trait impl");
            return 0;
        }
    }
    if (record->expanded_item != NULL
        && record->expanded_item->inner_attribute_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "effective module inner attributes require graph HIR lowering");
        return 0;
    }
    if (record->expanded_item == NULL
        && record->graph_module == CM_MODULE_NONE
        && ast_item->attribute_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "active non-cfg attributes require structural HIR attributes");
        return 0;
    }
    if (record->expanded_item == NULL
        && record->graph_module == CM_MODULE_NONE
        && ast_item->kind == CM_AST_ITEM_MODULE
        && ast_item->data.module_item.inner_attribute_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "active module inner attributes require graph HIR lowering");
        return 0;
    }
    memset(out_item, 0, sizeof(*out_item));
    out_item->definition = record->definition;
    out_item->owner_module = record->owner_module;
    out_item->parent_definition = record->parent_definition;
    out_item->is_specializable = ast_item->is_default;
    out_item->name = record->hir_name;
    out_item->span = span;
    out_item->attributes = cm_lower_item_attributes(state,
        record, span, ast_item_id,
        &out_item->attribute_count);
    if (state->failed) return 0;
    out_item->generic_parameter_start = record->generic_parameter_start;
    out_item->generic_parameter_count = record->generic_parameter_count;
    if (!cm_lower_visibility(state, ast_item->visibility,
            record->owner_module, span, ast_item_id,
            &out_item->visibility)) {
        return 0;
    }
    if (!cm_lower_item_trait_predicates(state, ast_item_id, ast_item,
            record, out_item)) {
        return 0;
    }
    return !state->failed;
}

static int cm_lower_function_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    const CmAstFunction *function;
    CmHirFunctionParameter *parameters;
    CmHirLocal *locals;
    CmHirTypeId return_type;
    CmHirReceiverKind receiver;
    CmHirDefId trait_item_definition;
    CmSpan span;
    uint32_t index;
    uint32_t local_count;

    function = &ast_item->data.function_item;
    span = cm_lower_span(state, ast_item->span);
    receiver = CM_HIR_RECEIVER_NONE;
    trait_item_definition = cm_hir_def_id_none();
    if (function->is_safe && function->is_unsafe) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "function cannot be both explicitly safe and unsafe");
        return 0;
    }
    if (record->parent_kind == CM_LOWER_PARENT_IMPL
        && function->body == CM_AST_EXPR_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "impl method must have a body");
        return 0;
    }
    parameters = NULL;
    locals = NULL;
    local_count = 0u;
    if (function->parameter_count != 0u) {
        if (function->parameters == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "function parameter count has no parameter storage");
            return 0;
        }
        if (function->parameter_count > UINT32_MAX / 2u) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "function parameter binding count exceeds HIR capacity");
            return 0;
        }
        parameters = (CmHirFunctionParameter *)cm_alloc_zeroed(
            (size_t)function->parameter_count,
            sizeof(CmHirFunctionParameter));
        locals = (CmHirLocal *)cm_alloc_zeroed(
            (size_t)function->parameter_count * 2u, sizeof(CmHirLocal));
    }
    for (index = 0u; index < function->parameter_count && !state->failed;
         ++index) {
        CmInternId name;
        CmHirMutability mutability;
        CmSpan parameter_span;
        CmHirBindingKind binding_kind;
        CmHirParameterBindingMode binding_mode;

        binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;

        if (function->parameters[index].is_self) {
            int has_explicit_type;

            has_explicit_type = function->parameters[index].type
                != CM_AST_TYPE_NONE;
            if (index != 0u || receiver != CM_HIR_RECEIVER_NONE
                || record->parent_kind == CM_LOWER_PARENT_NONE) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "a receiver must be the first parameter of a trait or "
                    "impl method");
                break;
            }
            if (!cm_lower_receiver_pattern(state,
                    function->parameters[index].pattern, span, ast_item_id,
                    has_explicit_type, &receiver, &name, &mutability,
                    &parameter_span)) {
                break;
            }
            if (function->parameters[index].receiver_lifetime
                    != CM_INTERN_ID_NONE
                && receiver != CM_HIR_RECEIVER_REF_SHARED
                && receiver != CM_HIR_RECEIVER_REF_MUTABLE) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    parameter_span, ast_item_id, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "receiver lifetime requires a reference receiver");
                break;
            }
            binding_kind = CM_HIR_BINDING_NAMED;
            parameters[index].type = has_explicit_type
                ? cm_lower_type(state, function->parameters[index].type,
                    record->owner_module, record->definition)
                : cm_lower_receiver_type(state, receiver,
                    record->definition, parameter_span, ast_item_id,
                    function->parameters[index].receiver_lifetime);
            if (!state->failed && receiver == CM_HIR_RECEIVER_CUSTOM
                && !cm_hir_custom_receiver_type_valid(state->hir,
                    parameters[index].type, record->parent_definition)) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE,
                    parameter_span, ast_item_id,
                    function->parameters[index].type, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "custom receiver type must follow a reference or "
                    "single-argument nominal wrapper to the enclosing Self");
                break;
            }
        } else {
            const CmAstPattern *ast_pattern;

            if (function->parameters[index].receiver_lifetime
                != CM_INTERN_ID_NONE) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    span, ast_item_id, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "non-receiver parameter stores a receiver lifetime");
                break;
            }
            if (function->parameters[index].type == CM_AST_TYPE_NONE) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "untyped non-receiver parameters require typed pattern "
                    "HIR");
                break;
            }
            parameters[index].type = cm_lower_type(state,
                function->parameters[index].type, record->owner_module,
                record->definition);
            if (state->failed) break;
            ast_pattern = cm_ast_get_pattern(state->ast,
                function->parameters[index].pattern);
            if (ast_pattern != NULL
                && ast_pattern->kind == CM_AST_PATTERN_TUPLE) {
                if (record->parent_kind != CM_LOWER_PARENT_NONE
                    || record->is_foreign
                    || function->body == CM_AST_EXPR_NONE) {
                    cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
                        cm_lower_span(state, ast_pattern->span), ast_item_id,
                        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                        "tuple parameter patterns require bodyful free "
                        "functions");
                    break;
                }
                if (!cm_lower_tuple_parameter_pattern(state,
                        function->parameters[index].pattern, span,
                        ast_item_id, index, &parameters[index],
                        &locals[local_count])) {
                    break;
                }
                local_count += CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
                continue;
            }
            if (!cm_lower_pattern_binding(state,
                    function->parameters[index].pattern, span, ast_item_id,
                    &name, &mutability, &parameter_span, &binding_kind,
                    &binding_mode)) {
                break;
            }
        }
        parameters[index].name = name;
        parameters[index].span = parameter_span;
        parameters[index].binding_kind = binding_kind;
        parameters[index].binding_mode = binding_mode;
        if (binding_kind == CM_HIR_BINDING_NAMED) {
            locals[local_count].name = name;
            locals[local_count].type = cm_lower_parameter_binding_type(state,
                parameters[index].type, binding_mode, parameter_span);
            if (state->failed) break;
            locals[local_count].mutability = mutability;
            locals[local_count].span = parameter_span;
            locals[local_count].parameter_index = index;
            locals[local_count].parameter_binding_index = 0u;
            local_count += 1u;
        }
    }
    if (state->failed) {
        cm_free(locals);
        cm_free(parameters);
        return 0;
    }
    if (function->return_type == CM_AST_TYPE_NONE) {
        return_type = cm_lower_unit_type(state, span, ast_item_id);
    } else {
        return_type = cm_lower_type(state, function->return_type,
            record->owner_module, record->definition);
    }
    if (state->failed) {
        cm_free(locals);
        cm_free(parameters);
        return 0;
    }
    if (record->parent_kind == CM_LOWER_PARENT_IMPL) {
        const CmHirItem *parent_impl;
        const CmHirItem *trait_method;
        const CmLowerItemRecord *trait_method_record;
        uint32_t matches;

        parent_impl = cm_lower_bound_item(state, record->parent_definition);
        if (parent_impl == NULL || parent_impl->kind != CM_HIR_ITEM_IMPL
            || parent_impl->data.impl_item.is_negative != 0) {
            cm_free(locals);
            cm_free(parameters);
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "impl method has no bound positive impl parent");
            return 0;
        }
        if (parent_impl->data.impl_item.has_trait) {
            trait_method = cm_lower_find_associated_method(state,
                parent_impl->data.impl_item.trait_type.definition,
                ast_item->name, &matches);
            if (matches != 1u || trait_method == NULL) {
                cm_free(locals);
                cm_free(parameters);
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    matches == 0u
                        ? "impl method has no matching trait method declaration"
                        : "impl method identity is ambiguous or unbound");
                return 0;
            }
            if (trait_method->generic_parameter_count
                    != record->generic_parameter_count) {
                cm_free(locals);
                cm_free(parameters);
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "impl method generic arity differs from trait method");
                return 0;
            }
            trait_method_record = cm_lower_find_record_by_definition(state,
                trait_method->definition);
            if (trait_method_record != NULL) {
                const CmAstItem *trait_ast_method;

                trait_ast_method = cm_ast_get_item(trait_method_record->ast,
                    trait_method_record->ast_id);
                if (trait_ast_method == NULL
                    || trait_ast_method->kind != CM_AST_ITEM_FUNCTION) {
                    cm_free(locals);
                    cm_free(parameters);
                    cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                        ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                        CM_HIR_INVARIANT_VIOLATION,
                        "trait method declaration lost its AST provenance");
                    return 0;
                }
                /* APIT is universal, but the impl must still spell APIT. */
                if (trait_ast_method->generic_parameter_count
                        != ast_item->generic_parameter_count) {
                    cm_free(locals);
                    cm_free(parameters);
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
                        ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                        CM_HIR_OK,
                        "impl method explicit generic arity differs from "
                        "trait method");
                    return 0;
                }
            }
            trait_item_definition = trait_method->definition;
        }
    }
    hir_item->kind = CM_HIR_ITEM_FUNCTION;
    hir_item->data.function_item.signature.parameters = parameters;
    hir_item->data.function_item.signature.parameter_count =
        function->parameter_count;
    hir_item->data.function_item.signature.receiver = receiver;
    hir_item->data.function_item.signature.return_type = return_type;
    hir_item->data.function_item.signature.abi =
        record->is_foreign
            ? cm_lower_copy_string(state, record->inherited_abi, span,
                ast_item_id)
            : (function->abi == CM_INTERN_ID_NONE
                ? cm_hir_intern(state->hir, "Rust")
                : cm_lower_copy_string(state, function->abi, span,
                    ast_item_id));
    hir_item->data.function_item.signature.safety = function->is_unsafe
            || (record->is_foreign && !function->is_safe)
        ? CM_HIR_UNSAFE : CM_HIR_SAFE;
    hir_item->data.function_item.signature.is_const = function->is_const;
    hir_item->data.function_item.signature.is_async = function->is_async;
    hir_item->data.function_item.trait_item_definition =
        trait_item_definition;
    hir_item->data.function_item.body = cm_lower_body(state,
        record->definition, return_type, function->body, locals,
        local_count, function->parameter_count, span,
        ast_item_id);
    cm_free(locals);
    if (state->failed) {
        cm_free(parameters);
        return 0;
    }
    return 1;
}

static int cm_lower_aggregate_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    const CmAstAggregate *aggregate;
    CmSpan span;

    aggregate = &ast_item->data.aggregate_item;
    span = cm_lower_span(state, ast_item->span);
    if (aggregate->form > CM_AST_FIELDS_NAMED) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "%s has an invalid field form",
            ast_item->kind == CM_AST_ITEM_UNION ? "union" : "struct");
        return 0;
    }
    if (ast_item->kind == CM_AST_ITEM_UNION
        && aggregate->form != CM_AST_FIELDS_NAMED) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "union must have named fields");
        return 0;
    }
    hir_item->kind = ast_item->kind == CM_AST_ITEM_UNION
        ? CM_HIR_ITEM_UNION : CM_HIR_ITEM_STRUCT;
    hir_item->data.aggregate_item.form = cm_lower_aggregate_form(
        aggregate->form);
    hir_item->data.aggregate_item.fields = cm_lower_fields(state,
        aggregate->fields, aggregate->field_count, aggregate->form,
        record->owner_module, record->definition, span, ast_item_id);
    hir_item->data.aggregate_item.field_count = aggregate->field_count;
    return !state->failed;
}

static int cm_lower_enum_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    const CmAstEnum *enumeration;
    CmHirVariant *variants;
    CmSpan span;
    uint32_t index;

    enumeration = &ast_item->data.enum_item;
    span = cm_lower_span(state, ast_item->span);
    if (enumeration->variant_count != 0u && enumeration->variants == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "enum variant count has no variant storage");
        return 0;
    }
    variants = NULL;
    if (enumeration->variant_count != 0u) {
        variants = (CmHirVariant *)cm_alloc_zeroed(
            (size_t)enumeration->variant_count, sizeof(CmHirVariant));
    }
    for (index = 0u; index < enumeration->variant_count && !state->failed;
         ++index) {
        const CmAstVariant *ast_variant;
        const CmLowerVariantRecord *variant_record;
        CmSpan variant_span;
        uint32_t variant_matches;
        uint32_t attribute_index;
        int has_cfg_attribute;

        ast_variant = &enumeration->variants[index];
        variant_span = cm_lower_span(state, ast_variant->span);
        has_cfg_attribute = 0;
        variant_record = cm_lower_find_variant_record(state,
            record->definition, index, &variant_matches);
        if (variant_record == NULL || variant_matches != 1u) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, variant_span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "enum variant has no unique reserved HIR identity");
            break;
        }
        if (ast_variant->span.start > ast_variant->span.end
            || ast_variant->span.start < ast_item->span.start
            || ast_variant->span.end > ast_item->span.end
            || ((ast_variant->attribute_count == 0u)
                != (ast_variant->attributes == NULL))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "enum variant attribute storage or span is invalid");
            break;
        }
        for (attribute_index = 0u;
                attribute_index < ast_variant->attribute_count;
                ++attribute_index) {
            const CmAstAttribute *attribute;

            attribute = cm_ast_get_attribute(state->ast,
                ast_variant->attributes[attribute_index]);
            if (attribute == NULL || attribute->style != CM_AST_ATTR_OUTER
                || attribute->span.start > attribute->span.end
                || attribute->span.start < ast_variant->span.start
                || attribute->span.end > ast_variant->span.end) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, variant_span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK, "enum variant attribute is invalid");
                break;
            }
            if (cm_lower_attribute_has_head(state,
                    ast_variant->attributes[attribute_index], "cfg")
                || cm_lower_attribute_has_head(state,
                    ast_variant->attributes[attribute_index], "cfg_attr")) {
                has_cfg_attribute = 1;
            }
        }
        if (state->failed) break;
        /*
         * Graph-backed crates have already passed item/variant attributes
         * through cfg expansion.  Stable metadata attributes such as
         * #[stable] remain attached to core enum variants but have no HIR
         * semantics here, so discard them after structural validation.  A
         * source-only lowering still rejects cfg-bearing variant attributes,
         * because accepting those without effective expansion could retain a
         * disabled variant; inert metadata such as #[stable] is harmless.
         */
        if (ast_variant->attribute_count != 0u
            && state->graph == NULL
            && has_cfg_attribute) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, variant_span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "enum variant attributes require effective cfg lowering");
            break;
        }
        if (ast_variant->form > CM_AST_FIELDS_NAMED) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, variant_span,
                ast_item_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "enum variant has an invalid field form");
            break;
        }
        if (ast_variant->discriminant != CM_INTERN_ID_NONE) {
            const CmInternedString *discriminant_text;
            CmHirType discriminant_type;
            CmHirTypeId discriminant_type_id;
            uint64_t discriminant_value;
            uint64_t negative_magnitude;
            int negative_discriminant;

            discriminant_text = cm_lower_ast_string(state,
                ast_variant->discriminant);
            negative_discriminant = 0;
            if (!cm_lower_parse_u64(discriminant_text,
                    &discriminant_value)
                && !cm_lower_parse_array_length_expression(discriminant_text,
                    &discriminant_value)) {
                if (!cm_lower_parse_negative_u64(discriminant_text,
                        &negative_magnitude)
                    || negative_magnitude > (UINT64_C(1) << 63)) {
                    cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
                        variant_span, ast_item_id, CM_AST_TYPE_NONE,
                        CM_AST_PATH_NONE, CM_HIR_OK,
                        "enum discriminant must be an in-range integer "
                        "literal");
                    break;
                }
                discriminant_value = 0u - negative_magnitude;
                negative_discriminant = 1;
            }
            memset(&discriminant_type, 0, sizeof(discriminant_type));
            discriminant_type.kind = CM_HIR_TYPE_INTEGER_KIND;
            discriminant_type.span = variant_span;
            discriminant_type.data.integer_type.kind = CM_HIR_INT_ISIZE;
            discriminant_type_id = cm_lower_add_type(state,
                &discriminant_type, CM_AST_TYPE_NONE);
            if (discriminant_type_id == CM_HIR_TYPE_NONE) break;
            variants[index].has_discriminant = 1;
            variants[index].discriminant.kind = CM_HIR_CONST_VALUE;
            variants[index].discriminant.type = discriminant_type_id;
            variants[index].discriminant.data.value.low_bits =
                discriminant_value;
            variants[index].discriminant.data.value.high_bits =
                negative_discriminant ? UINT64_MAX : 0u;
        }
        variants[index].definition = variant_record->definition;
        variants[index].name = cm_lower_copy_string(state,
            ast_variant->name, span, ast_item_id);
        variants[index].form = cm_lower_aggregate_form(ast_variant->form);
        variants[index].fields = cm_lower_fields(state, ast_variant->fields,
            ast_variant->field_count, ast_variant->form,
            record->owner_module, record->definition, span, ast_item_id);
        variants[index].field_count = ast_variant->field_count;
        variants[index].span = variant_span;
    }
    if (state->failed) {
        if (variants != NULL) {
            for (index = 0u; index < enumeration->variant_count; ++index) {
                cm_free(variants[index].fields);
            }
        }
        cm_free(variants);
        return 0;
    }
    hir_item->kind = CM_HIR_ITEM_ENUM;
    hir_item->data.enum_item.variants = variants;
    hir_item->data.enum_item.variant_count = enumeration->variant_count;
    return 1;
}

static void cm_lower_free_enum_temporary(CmHirItem *item)
{
    uint32_t index;

    if (item->kind != CM_HIR_ITEM_ENUM) {
        return;
    }
    for (index = 0u; index < item->data.enum_item.variant_count; ++index) {
        cm_free(item->data.enum_item.variants[index].fields);
    }
    cm_free(item->data.enum_item.variants);
}

static const CmHirItem *cm_lower_bound_item(const CmLowerState *state,
    CmHirDefId definition)
{
    const CmHirDefinition *stored;

    stored = cm_hir_lookup_definition(state->hir, definition);
    if (stored == NULL || stored->kind != CM_HIR_DEFINITION_ITEM
        || stored->state != CM_HIR_DEFINITION_BOUND) {
        return NULL;
    }
    return cm_hir_get_item(state->hir, stored->entity.item_id);
}

typedef struct CmLowerTraitDefaultSubstitution {
    CmLowerState *state;
    CmAstItemId ast_item_id;
    CmHirDefId source_owner;
    CmHirDefId replacement_owner;
    uint32_t parameter_index;
    const CmHirGenericArg *prior_arguments;
    CmHirTypeId default_self;
    int allow_synthesized_default_self;
    CmSpan use_span;
    size_t source_type_count;
    unsigned char *marks;
    CmHirTypeId *results;
} CmLowerTraitDefaultSubstitution;

static int cm_lower_trait_default_substitute_type(
    CmLowerTraitDefaultSubstitution *substitution, CmHirTypeId source_id,
    size_t depth, CmHirTypeId *out_id);

static int cm_lower_trait_default_parameter_argument(
    CmLowerTraitDefaultSubstitution *substitution,
    CmHirGenericParamId parameter_id, CmHirGenericArgKind expected_kind,
    const CmHirGenericArg **out_argument)
{
    const CmHirGenericParam *parameter;
    const CmHirGenericArg *argument;

    *out_argument = NULL;
    parameter = cm_hir_get_generic_param(substitution->state->hir,
        parameter_id);
    if (parameter == NULL
        || !cm_hir_def_id_equal(parameter->owner,
            substitution->source_owner)
        || parameter->index >= substitution->parameter_index) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "trait generic default has a foreign, forward, or cyclic "
            "parameter reference");
        return 0;
    }
    argument = &substitution->prior_arguments[parameter->index];
    if (argument->kind != expected_kind) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "trait generic default substitution changed an argument kind");
        return 0;
    }
    *out_argument = argument;
    return 1;
}

static int cm_lower_trait_default_substitute_region(
    CmLowerTraitDefaultSubstitution *substitution,
    const CmHirRegion *source, CmHirRegion *out_region, int *out_changed)
{
    const CmHirGenericArg *argument;

    *out_region = *source;
    *out_changed = 0;
    if (source->kind == CM_HIR_REGION_STATIC) return 1;
    if (source->kind != CM_HIR_REGION_EARLY_BOUND) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ARGUMENT,
            "trait generic default contains an unresolved, erased, or "
            "unauthenticated bound lifetime");
        return 0;
    }
    if (!cm_lower_trait_default_parameter_argument(substitution,
            source->data.parameter, CM_HIR_GENERIC_ARG_LIFETIME,
            &argument)) {
        return 0;
    }
    *out_region = argument->data.lifetime;
    *out_changed = 1;
    return 1;
}

static int cm_lower_trait_default_substitute_const(
    CmLowerTraitDefaultSubstitution *substitution,
    const CmHirConstArg *source, size_t depth, CmHirConstArg *out_constant,
    int *out_changed)
{
    const CmHirGenericArg *argument;
    CmHirTypeId substituted_type;

    *out_constant = *source;
    *out_changed = 0;
    if (source->kind == CM_HIR_CONST_INFER
        || source->kind == CM_HIR_CONST_ERROR) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ARGUMENT,
            "trait generic default contains an unresolved const argument");
        return 0;
    }
    if (source->kind == CM_HIR_CONST_PARAMETER) {
        if (!cm_lower_trait_default_parameter_argument(substitution,
                source->data.parameter, CM_HIR_GENERIC_ARG_CONST,
                &argument)) {
            return 0;
        }
        *out_constant = argument->data.constant;
        *out_changed = 1;
        return 1;
    }
    if (!cm_lower_trait_default_substitute_type(substitution, source->type,
            depth + 1u, &substituted_type)) {
        return 0;
    }
    out_constant->type = substituted_type;
    *out_changed = substituted_type != source->type;
    return 1;
}

static int cm_lower_trait_default_substitute_argument(
    CmLowerTraitDefaultSubstitution *substitution,
    const CmHirGenericArg *source, size_t depth, CmHirGenericArg *out_argument,
    int *out_changed)
{
    int changed;

    *out_argument = *source;
    *out_changed = 0;
    switch (source->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_lower_trait_default_substitute_region(substitution,
            &source->data.lifetime, &out_argument->data.lifetime,
            out_changed);
    case CM_HIR_GENERIC_ARG_TYPE:
        if (!cm_lower_trait_default_substitute_type(substitution,
                source->data.type, depth + 1u,
                &out_argument->data.type)) {
            return 0;
        }
        *out_changed = out_argument->data.type != source->data.type;
        return 1;
    case CM_HIR_GENERIC_ARG_CONST:
        changed = 0;
        if (!cm_lower_trait_default_substitute_const(substitution,
                &source->data.constant, depth,
                &out_argument->data.constant, &changed)) {
            return 0;
        }
        *out_changed = changed;
        return 1;
    }
    cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
        substitution->use_span, substitution->ast_item_id,
        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
        "trait generic default contains an invalid argument kind");
    return 0;
}

static int cm_lower_trait_default_substitute_named(
    CmLowerTraitDefaultSubstitution *substitution,
    const CmHirNamedType *source, size_t depth, CmHirNamedType *out_named,
    int *out_changed)
{
    CmHirGenericArg *arguments;
    uint32_t index;

    memset(out_named, 0, sizeof(*out_named));
    out_named->definition = source->definition;
    out_named->argument_count = source->argument_count;
    *out_changed = 0;
    if ((source->argument_count == 0u) != (source->arguments == NULL)) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "trait generic default has invalid named argument storage");
        return 0;
    }
    arguments = source->argument_count == 0u ? NULL
        : (CmHirGenericArg *)cm_alloc_zeroed(
            (size_t)source->argument_count, sizeof(*arguments));
    for (index = 0u; index < source->argument_count; ++index) {
        int changed;

        if (!cm_lower_trait_default_substitute_argument(substitution,
                &source->arguments[index], depth, &arguments[index],
                &changed)) {
            cm_free(arguments);
            return 0;
        }
        if (changed) *out_changed = 1;
    }
    out_named->arguments = arguments;
    return 1;
}

static void cm_lower_trait_default_free_type_temporary(CmHirType *type)
{
    switch (type->kind) {
    case CM_HIR_TYPE_TUPLE_KIND:
        cm_free(type->data.tuple_type.elements);
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        cm_free(type->data.fn_pointer_type.parameters);
        break;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        cm_free(type->data.named_type.arguments);
        break;
    case CM_HIR_TYPE_CLOSURE_KIND:
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
        cm_free(type->data.projection_type.trait_type.arguments);
        cm_free(type->data.projection_type.associated_type.arguments);
        break;
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    {
        uint32_t marker_index;

        for (marker_index = 0u;
             marker_index < type->data.dyn_trait_type.auto_trait_count;
             ++marker_index) {
            cm_free(type->data.dyn_trait_type
                .auto_traits[marker_index].arguments);
        }
        cm_free(type->data.dyn_trait_type.auto_traits);
        cm_free(type->data.dyn_trait_type.principal_trait.arguments);
        cm_free(type->data.dyn_trait_type.equalities);
        break;
    }
    default:
        break;
    }
}

static int cm_lower_trait_default_substitute_type(
    CmLowerTraitDefaultSubstitution *substitution, CmHirTypeId source_id,
    size_t depth, CmHirTypeId *out_id)
{
    const CmHirType *stored;
    CmHirType source;
    CmHirType copy;
    size_t cache_index;
    uint32_t index;
    int changed;

    *out_id = CM_HIR_TYPE_NONE;
    if (depth > CM_LOWER_GENERIC_DEFAULT_MAX_DEPTH) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ARGUMENT,
            "trait generic default substitution exceeds the structural "
            "depth limit");
        return 0;
    }
    if (source_id == CM_HIR_TYPE_NONE
        || (size_t)source_id > substitution->source_type_count
        || depth > substitution->source_type_count) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "trait generic default substitution exceeded its source DAG "
            "depth");
        return 0;
    }
    cache_index = (size_t)source_id;
    if (substitution->marks[cache_index] == 2u) {
        *out_id = substitution->results[cache_index];
        return 1;
    }
    if (substitution->marks[cache_index] == 1u) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "trait generic default type graph contains a cycle");
        return 0;
    }
    stored = cm_hir_get_type(substitution->state->hir, source_id);
    if (stored == NULL) {
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
            "trait generic default references an invalid HIR type");
        return 0;
    }
    source = *stored;
    substitution->marks[cache_index] = 1u;
    if (source.kind == CM_HIR_TYPE_SELF_KIND) {
        if (!cm_hir_def_id_equal(source.data.self_type.owner,
                substitution->source_owner)) {
            cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
                substitution->use_span, substitution->ast_item_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
                "trait generic default contains foreign Self");
            return 0;
        }
        if (substitution->default_self == CM_HIR_TYPE_NONE) {
            if (!substitution->allow_synthesized_default_self) {
                cm_lower_fail(substitution->state,
                    CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    substitution->use_span, substitution->ast_item_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "associated-type bound generic default containing "
                    "Self requires an authenticated associated-type "
                    "subject");
                return 0;
            }
            substitution->default_self = cm_lower_self_type(
                substitution->state, CM_AST_TYPE_NONE,
                substitution->use_span, substitution->replacement_owner);
        }
        if (substitution->default_self == CM_HIR_TYPE_NONE) return 0;
        substitution->results[cache_index] = substitution->default_self;
        substitution->marks[cache_index] = 2u;
        *out_id = substitution->default_self;
        return 1;
    }
    if (source.kind == CM_HIR_TYPE_PARAMETER_KIND) {
        const CmHirGenericArg *argument;

        if (!cm_lower_trait_default_parameter_argument(substitution,
                source.data.parameter_type.parameter,
                CM_HIR_GENERIC_ARG_TYPE, &argument)) {
            return 0;
        }
        substitution->results[cache_index] = argument->data.type;
        substitution->marks[cache_index] = 2u;
        *out_id = argument->data.type;
        return 1;
    }
    copy = source;
    changed = 0;
    switch (source.kind) {
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
        cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
            substitution->use_span, substitution->ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ARGUMENT,
            "trait generic default contains an unresolved or unnameable "
            "HIR type");
        return 0;
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
    {
        int region_changed;

        if (!cm_lower_trait_default_substitute_type(substitution,
                source.data.reference_type.pointee, depth + 1u,
                &copy.data.reference_type.pointee)
            || !cm_lower_trait_default_substitute_region(substitution,
                &source.data.reference_type.region,
                &copy.data.reference_type.region, &region_changed)) {
            return 0;
        }
        changed = region_changed
            || copy.data.reference_type.pointee
                != source.data.reference_type.pointee;
        break;
    }
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        if (!cm_lower_trait_default_substitute_type(substitution,
                source.data.raw_pointer_type.pointee, depth + 1u,
                &copy.data.raw_pointer_type.pointee)) return 0;
        changed = copy.data.raw_pointer_type.pointee
            != source.data.raw_pointer_type.pointee;
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((source.data.tuple_type.element_count == 0u)
                != (source.data.tuple_type.elements == NULL)) {
            cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
                substitution->use_span, substitution->ast_item_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
                "trait generic default has invalid tuple element storage");
            return 0;
        }
        copy.data.tuple_type.elements = source.data.tuple_type.element_count
                == 0u ? NULL : (CmHirTypeId *)cm_alloc_zeroed(
                    (size_t)source.data.tuple_type.element_count,
                    sizeof(CmHirTypeId));
        for (index = 0u; index < source.data.tuple_type.element_count;
             ++index) {
            if (!cm_lower_trait_default_substitute_type(substitution,
                    source.data.tuple_type.elements[index], depth + 1u,
                    &copy.data.tuple_type.elements[index])) {
                cm_lower_trait_default_free_type_temporary(&copy);
                return 0;
            }
            if (copy.data.tuple_type.elements[index]
                    != source.data.tuple_type.elements[index]) changed = 1;
        }
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
    {
        int constant_changed;

        if (!cm_lower_trait_default_substitute_type(substitution,
                source.data.array_type.element, depth + 1u,
                &copy.data.array_type.element)
            || !cm_lower_trait_default_substitute_const(substitution,
                &source.data.array_type.length, depth,
                &copy.data.array_type.length, &constant_changed)) return 0;
        changed = constant_changed || copy.data.array_type.element
            != source.data.array_type.element;
        break;
    }
    case CM_HIR_TYPE_SLICE_KIND:
        if (!cm_lower_trait_default_substitute_type(substitution,
                source.data.slice_type.element, depth + 1u,
                &copy.data.slice_type.element)) return 0;
        changed = copy.data.slice_type.element
            != source.data.slice_type.element;
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((source.data.fn_pointer_type.parameter_count == 0u)
                != (source.data.fn_pointer_type.parameters == NULL)) {
            cm_lower_fail(substitution->state, CM_HIR_LOWER_INVALID_TRAIT,
                substitution->use_span, substitution->ast_item_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
                "trait generic default has invalid function parameter "
                "storage");
            return 0;
        }
        copy.data.fn_pointer_type.parameters =
            source.data.fn_pointer_type.parameter_count == 0u ? NULL
                : (CmHirTypeId *)cm_alloc_zeroed(
                    (size_t)source.data.fn_pointer_type.parameter_count,
                    sizeof(CmHirTypeId));
        for (index = 0u; index < source.data.fn_pointer_type.parameter_count;
             ++index) {
            if (!cm_lower_trait_default_substitute_type(substitution,
                    source.data.fn_pointer_type.parameters[index],
                    depth + 1u,
                    &copy.data.fn_pointer_type.parameters[index])) {
                cm_lower_trait_default_free_type_temporary(&copy);
                return 0;
            }
            if (copy.data.fn_pointer_type.parameters[index]
                    != source.data.fn_pointer_type.parameters[index]) {
                changed = 1;
            }
        }
        if (!cm_lower_trait_default_substitute_type(substitution,
                source.data.fn_pointer_type.return_type, depth + 1u,
                &copy.data.fn_pointer_type.return_type)) {
            cm_lower_trait_default_free_type_temporary(&copy);
            return 0;
        }
        if (copy.data.fn_pointer_type.return_type
                != source.data.fn_pointer_type.return_type) changed = 1;
        break;
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        if (!cm_lower_trait_default_substitute_named(substitution,
                &source.data.named_type, depth,
                &copy.data.named_type, &changed)) return 0;
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
    {
        int trait_changed;
        int associated_changed;

        memset(&copy.data.projection_type.trait_type, 0,
            sizeof(copy.data.projection_type.trait_type));
        memset(&copy.data.projection_type.associated_type, 0,
            sizeof(copy.data.projection_type.associated_type));
        if (!cm_lower_trait_default_substitute_type(substitution,
                source.data.projection_type.self_type, depth + 1u,
                &copy.data.projection_type.self_type)
            || !cm_lower_trait_default_substitute_named(substitution,
                &source.data.projection_type.trait_type, depth,
                &copy.data.projection_type.trait_type, &trait_changed)
            || !cm_lower_trait_default_substitute_named(substitution,
                &source.data.projection_type.associated_type, depth,
                &copy.data.projection_type.associated_type,
                &associated_changed)) {
            cm_lower_trait_default_free_type_temporary(&copy);
            return 0;
        }
        changed = trait_changed || associated_changed
            || copy.data.projection_type.self_type
                != source.data.projection_type.self_type;
        break;
    }
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    {
        CmHirAssociatedTypeEquality *equalities;
        CmHirNamedType *markers;
        int principal_changed;
        int region_changed;

        memset(&copy.data.dyn_trait_type.principal_trait, 0,
            sizeof(copy.data.dyn_trait_type.principal_trait));
        copy.data.dyn_trait_type.equalities = NULL;
        copy.data.dyn_trait_type.auto_traits = NULL;
        principal_changed = 0;
        if ((source.data.dyn_trait_type.has_principal
                && !cm_lower_trait_default_substitute_named(substitution,
                    &source.data.dyn_trait_type.principal_trait, depth,
                    &copy.data.dyn_trait_type.principal_trait,
                    &principal_changed))
            || !cm_lower_trait_default_substitute_region(substitution,
                &source.data.dyn_trait_type.region,
                &copy.data.dyn_trait_type.region, &region_changed)) {
            cm_lower_trait_default_free_type_temporary(&copy);
            return 0;
        }
        markers = source.data.dyn_trait_type.auto_trait_count == 0u ? NULL
            : (CmHirNamedType *)cm_alloc_zeroed(
                source.data.dyn_trait_type.auto_trait_count,
                sizeof(*markers));
        copy.data.dyn_trait_type.auto_traits = markers;
        changed = principal_changed || region_changed;
        for (index = 0u;
             index < source.data.dyn_trait_type.auto_trait_count; ++index) {
            int marker_changed;

            if (!cm_lower_trait_default_substitute_named(substitution,
                    &source.data.dyn_trait_type.auto_traits[index], depth,
                    &markers[index], &marker_changed)) {
                cm_lower_trait_default_free_type_temporary(&copy);
                return 0;
            }
            if (marker_changed) changed = 1;
        }
        equalities = source.data.dyn_trait_type.equality_count == 0u
            ? NULL : (CmHirAssociatedTypeEquality *)cm_alloc_zeroed(
                source.data.dyn_trait_type.equality_count,
                sizeof(*equalities));
        copy.data.dyn_trait_type.equalities = equalities;
        for (index = 0u;
             index < source.data.dyn_trait_type.equality_count; ++index) {
            equalities[index] =
                source.data.dyn_trait_type.equalities[index];
            if (!cm_lower_trait_default_substitute_type(substitution,
                    source.data.dyn_trait_type.equalities[index].value,
                    depth + 1u, &equalities[index].value)) {
                cm_lower_trait_default_free_type_temporary(&copy);
                return 0;
            }
            if (equalities[index].value
                    != source.data.dyn_trait_type.equalities[index].value) {
                changed = 1;
            }
        }
        break;
    }
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_PARAMETER_KIND:
        return 0;
    }
    if (!changed) {
        cm_lower_trait_default_free_type_temporary(&copy);
        substitution->results[cache_index] = source_id;
        substitution->marks[cache_index] = 2u;
        *out_id = source_id;
        return 1;
    }
    *out_id = cm_lower_add_type(substitution->state, &copy,
        CM_AST_TYPE_NONE);
    cm_lower_trait_default_free_type_temporary(&copy);
    if (*out_id == CM_HIR_TYPE_NONE) return 0;
    substitution->results[cache_index] = *out_id;
    substitution->marks[cache_index] = 2u;
    return 1;
}

static int cm_lower_trait_default_argument(CmLowerState *state,
    CmAstItemId ast_item_id, const CmLowerTraitTarget *trait_target,
    uint32_t parameter_index, CmHirTypeId default_self,
    CmHirDefId replacement_owner, int allow_synthesized_default_self,
    const CmHirGenericArg *prior_arguments, CmSpan span,
    CmHirGenericArg *out_argument)
{
    const CmHirGenericParam *parameter;
    const CmHirType *default_type;
    CmLowerTraitDefaultSubstitution substitution;

    parameter = cm_hir_get_generic_param(state->hir,
        trait_target->generic_parameter_start + parameter_index);
    if (parameter == NULL
        || !cm_hir_def_id_equal(parameter->owner,
            trait_target->definition)
        || parameter->index != parameter_index
        || parameter->kind != CM_HIR_GENERIC_TYPE
        || !parameter->has_default
        || parameter->default_argument.kind != CM_HIR_GENERIC_ARG_TYPE) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait reference omits a required type argument");
        return 0;
    }
    default_type = cm_hir_get_type(state->hir,
        parameter->default_argument.data.type);
    if (default_type == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVALID_ID,
            "trait generic default references an invalid HIR type");
        return 0;
    }
    (void)default_type;
    memset(&substitution, 0, sizeof(substitution));
    substitution.state = state;
    substitution.ast_item_id = ast_item_id;
    substitution.source_owner = trait_target->definition;
    substitution.replacement_owner = replacement_owner;
    substitution.parameter_index = parameter_index;
    substitution.prior_arguments = prior_arguments;
    substitution.default_self = default_self;
    substitution.allow_synthesized_default_self =
        allow_synthesized_default_self;
    substitution.use_span = span;
    substitution.source_type_count = state->hir->types.len;
    substitution.marks = (unsigned char *)cm_alloc_zeroed(
        substitution.source_type_count + 1u, sizeof(unsigned char));
    substitution.results = (CmHirTypeId *)cm_alloc_zeroed(
        substitution.source_type_count + 1u, sizeof(CmHirTypeId));
    memset(out_argument, 0, sizeof(*out_argument));
    out_argument->kind = CM_HIR_GENERIC_ARG_TYPE;
    if (!cm_lower_trait_default_substitute_type(&substitution,
            parameter->default_argument.data.type, 0u,
            &out_argument->data.type)) {
        cm_free(substitution.results);
        cm_free(substitution.marks);
        return 0;
    }
    cm_free(substitution.results);
    cm_free(substitution.marks);
    return 1;
}

static int cm_lower_find_instantiated_supertrait_inner(CmLowerState *state,
    const CmHirNamedType *root, CmHirDefId target_definition,
    CmHirTypeId self_type, CmSpan span, size_t depth,
    CmHirGenericArg **out_arguments, uint32_t *out_argument_count,
    uint32_t *out_matches)
{
    const CmHirItem *root_trait;
    uint32_t index;

    if (depth > state->hir->items.len) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "transitive supertrait substitution exceeded the trait graph "
            "depth");
        return 0;
    }
    root_trait = cm_lower_bound_item(state, root->definition);
    if (root_trait == NULL || root_trait->kind != CM_HIR_ITEM_TRAIT
        || root->argument_count != root_trait->generic_parameter_count
        || (root->argument_count == 0u) != (root->arguments == NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "transitive supertrait substitution has an invalid root trait "
            "reference");
        return 0;
    }
    if (root->argument_count != 0u
        && (root_trait->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
            || root_trait->generic_parameter_start
                > UINT32_MAX - (root->argument_count - 1u))) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "transitive supertrait substitution has an invalid generic "
            "signature range");
        return 0;
    }
    for (index = 0u; index < root->argument_count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected_kind;

        parameter = cm_hir_get_generic_param(state->hir,
            root_trait->generic_parameter_start + index);
        expected_kind = parameter != NULL
                && parameter->kind == CM_HIR_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_ARG_LIFETIME
            : parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
                ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                root_trait->definition)
            || root->arguments[index].kind != expected_kind) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "transitive supertrait substitution argument does not "
                "match its authenticated parameter");
            return 0;
        }
    }
    if (cm_hir_def_id_equal(root->definition, target_definition)) {
        CmHirGenericArg *arguments;

        arguments = root->argument_count == 0u ? NULL
            : (CmHirGenericArg *)cm_alloc_zeroed(
                (size_t)root->argument_count, sizeof(*arguments));
        if (root->argument_count != 0u) {
            memcpy(arguments, root->arguments,
                (size_t)root->argument_count * sizeof(*arguments));
        }
        *out_arguments = arguments;
        *out_argument_count = root->argument_count;
        *out_matches = 1u;
        return 1;
    }
    for (index = 0u;
         index < root_trait->data.trait_item.supertrait_count; ++index) {
        CmLowerTraitDefaultSubstitution substitution;
        CmHirNamedType instantiated;
        CmHirGenericArg *child_arguments;
        uint32_t child_argument_count;
        uint32_t child_matches;
        int changed;

        memset(&substitution, 0, sizeof(substitution));
        substitution.state = state;
        substitution.ast_item_id = CM_AST_ITEM_NONE;
        substitution.source_owner = root_trait->definition;
        substitution.replacement_owner = root_trait->definition;
        substitution.parameter_index = root->argument_count;
        substitution.prior_arguments = root->arguments;
        substitution.default_self = self_type;
        substitution.use_span = span;
        substitution.source_type_count = state->hir->types.len;
        substitution.marks = (unsigned char *)cm_alloc_zeroed(
            substitution.source_type_count + 1u, sizeof(unsigned char));
        substitution.results = (CmHirTypeId *)cm_alloc_zeroed(
            substitution.source_type_count + 1u, sizeof(CmHirTypeId));
        memset(&instantiated, 0, sizeof(instantiated));
        changed = 0;
        if (!cm_lower_trait_default_substitute_named(&substitution,
                &root_trait->data.trait_item.supertraits[index].trait_type,
                0u, &instantiated, &changed)) {
            cm_free(substitution.results);
            cm_free(substitution.marks);
            cm_free(*out_arguments);
            *out_arguments = NULL;
            *out_argument_count = 0u;
            *out_matches = 0u;
            return 0;
        }
        cm_free(substitution.results);
        cm_free(substitution.marks);
        child_arguments = NULL;
        child_argument_count = 0u;
        child_matches = 0u;
        if (!cm_lower_find_instantiated_supertrait_inner(state,
                &instantiated, target_definition, self_type, span,
                depth + 1u, &child_arguments, &child_argument_count,
                &child_matches)) {
            cm_free(instantiated.arguments);
            cm_free(*out_arguments);
            *out_arguments = NULL;
            *out_argument_count = 0u;
            *out_matches = 0u;
            return 0;
        }
        cm_free(instantiated.arguments);
        if (child_matches == 0u) continue;
        if (*out_matches == 0u) {
            *out_arguments = child_arguments;
            *out_argument_count = child_argument_count;
            *out_matches = child_matches > 1u ? 2u : 1u;
        } else if (*out_matches > 1u || child_matches > 1u) {
            cm_free(child_arguments);
            *out_matches = 2u;
        } else if (*out_argument_count == child_argument_count
            && cm_lower_projection_named_type_equal(state->hir,
                &(CmHirNamedType){ target_definition, *out_arguments,
                    *out_argument_count },
                &(CmHirNamedType){ target_definition, child_arguments,
                    child_argument_count }, 0u)) {
            cm_free(child_arguments);
        } else {
            cm_free(child_arguments);
            *out_matches = 2u;
        }
    }
    return 1;
}

static int cm_lower_find_instantiated_supertrait(CmLowerState *state,
    const CmHirNamedType *root, CmHirDefId target_definition,
    CmHirTypeId self_type, CmSpan span, CmHirGenericArg **out_arguments,
    uint32_t *out_argument_count, uint32_t *out_matches)
{
    *out_arguments = NULL;
    *out_argument_count = 0u;
    *out_matches = 0u;
    return cm_lower_find_instantiated_supertrait_inner(state, root,
        target_definition, self_type, span, 0u, out_arguments,
        out_argument_count, out_matches);
}

static int cm_lower_trait_positional_arguments(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstPathSegment *segment,
    const CmLowerTraitTarget *trait_target, CmHirModuleId module,
    CmHirDefId owner, CmHirTypeId default_self, int allow_bindings,
    int allow_constraints, int allow_synthesized_default_self, CmSpan span,
    CmHirGenericArg **out_arguments, uint32_t *out_count)
{
    CmHirGenericArg *arguments;
    uint32_t explicit_count;
    uint32_t index;
    int saw_binding;

    *out_arguments = NULL;
    *out_count = 0u;
    if ((segment->argument_count != 0u && segment->arguments == NULL)
        || (segment->argument_count == 0u && segment->arguments != NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait reference argument count and storage disagree");
        return 0;
    }
    explicit_count = 0u;
    saw_binding = 0;
    for (index = 0u; index < segment->argument_count; ++index) {
        const CmAstGenericArg *ast_argument;

        ast_argument = &segment->arguments[index];
        if (!cm_lower_validate_generic_constraint(state, ast_item_id,
                CM_AST_PATH_NONE, ast_argument)) {
            return 0;
        }
        if (ast_argument->kind == CM_AST_GENERIC_CONSTRAINT) {
            if (!allow_constraints) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, ast_argument->span), ast_item_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "associated-type constraints are not supported in this "
                    "HIR position");
                return 0;
            }
            saw_binding = 1;
            continue;
        }
        if (ast_argument->kind == CM_AST_GENERIC_BINDING) {
            if (!allow_bindings) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, ast_argument->span), ast_item_id,
                    ast_argument->type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "associated equality is not allowed in this trait "
                    "reference");
                return 0;
            }
            saw_binding = 1;
            continue;
        }
        if (saw_binding) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, ast_argument->span), ast_item_id,
                ast_argument->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "positional trait arguments must precede associated "
                "equalities or constraints");
            return 0;
        }
        if (ast_argument->kind != CM_AST_GENERIC_TYPE
            && ast_argument->kind != CM_AST_GENERIC_LIFETIME
            && ast_argument->kind != CM_AST_GENERIC_CONST) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, ast_argument->span), ast_item_id,
                ast_argument->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "const trait arguments require typed const-expression "
                "lowering");
            return 0;
        }
        explicit_count += 1u;
    }
    if (explicit_count > trait_target->generic_parameter_count) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait reference supplies too many positional arguments");
        return 0;
    }
    arguments = trait_target->generic_parameter_count == 0u ? NULL
        : (CmHirGenericArg *)cm_alloc_zeroed(
            (size_t)trait_target->generic_parameter_count,
            sizeof(CmHirGenericArg));
    explicit_count = 0u;
    for (index = 0u; index < segment->argument_count && !state->failed;
         ++index) {
        const CmAstGenericArg *ast_argument;
        const CmHirGenericParam *parameter;

        ast_argument = &segment->arguments[index];
        if (ast_argument->kind == CM_AST_GENERIC_BINDING
            || ast_argument->kind == CM_AST_GENERIC_CONSTRAINT) continue;
        parameter = cm_hir_get_generic_param(state->hir,
            trait_target->generic_parameter_start + explicit_count);
        if (parameter == NULL
            || !cm_hir_def_id_equal(parameter->owner,
                trait_target->definition)
            || parameter->index != explicit_count) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                cm_lower_span(state, ast_argument->span), ast_item_id,
                ast_argument->type, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "trait has an invalid generic parameter signature");
            break;
        }
        if (parameter->kind == CM_HIR_GENERIC_TYPE
            && ast_argument->kind == CM_AST_GENERIC_TYPE) {
            arguments[explicit_count].kind = CM_HIR_GENERIC_ARG_TYPE;
            arguments[explicit_count].data.type = cm_lower_type(state,
                ast_argument->type, module, owner);
        } else if (parameter->kind == CM_HIR_GENERIC_CONST
            && ast_argument->kind == CM_AST_GENERIC_TYPE) {
            if (!cm_lower_const_path_argument(state, ast_argument, owner,
                    parameter, cm_lower_span(state, ast_argument->span),
                    &arguments[explicit_count])) {
                break;
            }
        } else if (parameter->kind == CM_HIR_GENERIC_CONST
            && ast_argument->kind == CM_AST_GENERIC_CONST) {
            uint64_t value;
            const CmHirType *declared_type;

            declared_type = cm_hir_get_type(state->hir,
                parameter->declared_type);
            if (!cm_lower_parse_u64(cm_lower_ast_string(state,
                    ast_argument->text), &value)
                || declared_type == NULL
                || declared_type->kind != CM_HIR_TYPE_INTEGER_KIND) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, ast_argument->span), ast_item_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "const generic literal is not a supported integer "
                    "argument");
                break;
            }
            arguments[explicit_count].kind = CM_HIR_GENERIC_ARG_CONST;
            arguments[explicit_count].data.constant.kind =
                CM_HIR_CONST_VALUE;
            arguments[explicit_count].data.constant.type =
                parameter->declared_type;
            arguments[explicit_count].data.constant.data.value.low_bits =
                value;
            arguments[explicit_count].data.constant.data.value.high_bits = 0u;
        } else if (parameter->kind == CM_HIR_GENERIC_LIFETIME
            && ast_argument->kind == CM_AST_GENERIC_LIFETIME) {
            const CmHirGenericParam *lifetime_parameter;
            int lifetime_valid;

            arguments[explicit_count].kind = CM_HIR_GENERIC_ARG_LIFETIME;
            (void)cm_lower_lifetime(state, ast_argument->text, owner,
                cm_lower_span(state, ast_argument->span),
                &arguments[explicit_count].data.lifetime);
            lifetime_parameter = state->failed
                    || arguments[explicit_count].data.lifetime.kind
                        != CM_HIR_REGION_EARLY_BOUND
                ? NULL : cm_hir_get_generic_param(state->hir,
                    arguments[explicit_count].data.lifetime.data.parameter);
            lifetime_valid = !state->failed
                && (arguments[explicit_count].data.lifetime.kind
                        == CM_HIR_REGION_STATIC
                    || (arguments[explicit_count].data.lifetime.kind
                        == CM_HIR_REGION_EARLY_BOUND
                        && lifetime_parameter != NULL
                        && lifetime_parameter->kind
                            == CM_HIR_GENERIC_LIFETIME)
                    || (arguments[explicit_count].data.lifetime.kind
                        == CM_HIR_REGION_LATE_BOUND
                        && state->active_lifetime_binder != NULL
                        && arguments[explicit_count].data.lifetime
                                .data.binder_index
                            < state->active_lifetime_binder
                                ->lifetime_count));
            if (!lifetime_valid) {
                if (!state->failed) {
                    cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                        cm_lower_span(state, ast_argument->span),
                        ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                        CM_HIR_OK,
                        "explicit trait lifetime argument must be 'static "
                        "or an authenticated early- or late-bound "
                        "lifetime");
                }
                break;
            }
        } else {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, ast_argument->span), ast_item_id,
                ast_argument->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait generic argument kind differs from its parameter");
            break;
        }
        explicit_count += 1u;
    }
    for (index = explicit_count;
         index < trait_target->generic_parameter_count && !state->failed;
         ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(state->hir,
            trait_target->generic_parameter_start + index);
        if (parameter != NULL
            && parameter->kind != CM_HIR_GENERIC_TYPE) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait reference omits a required lifetime or const "
                "argument");
            break;
        }
        if (!cm_lower_trait_default_argument(state, ast_item_id,
                trait_target, index, default_self, owner,
                allow_synthesized_default_self, arguments, span,
                &arguments[index])) {
            break;
        }
    }
    if (state->failed) {
        cm_free(arguments);
        return 0;
    }
    *out_arguments = arguments;
    *out_count = trait_target->generic_parameter_count;
    return 1;
}

static int cm_lower_trait_reference(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId ast_type_id,
    CmHirModuleId module, CmHirDefId owner, CmHirTypeId default_self,
    CmHirNamedType *out_trait, CmLowerTraitTarget *out_target,
    int is_supertrait, int allow_bindings, int allow_constraints,
    int allow_trait_alias)
{
    const CmAstType *ast_type;
    const CmAstPath *path;
    const CmLowerItemRecord *record;
    const CmAstItem *target_ast_item;
    const CmHirItem *item;
    CmHirLibraryBinding external_binding;
    CmHirDefId module_definition;
    CmLowerLookupResult lookup;
    CmSpan span;
    uint32_t index;

    memset(out_trait, 0, sizeof(*out_trait));
    memset(out_target, 0, sizeof(*out_target));
    out_target->definition = cm_hir_def_id_none();
    ast_type = cm_ast_get_type(state->ast, ast_type_id);
    span = ast_type == NULL
        ? cm_lower_span(state, (CmAstSpan){ 0u, 0u })
        : cm_lower_span(state, ast_type->span);
    if (ast_type == NULL || ast_type->kind != CM_AST_TYPE_PATH
        || (path = cm_ast_get_path(state->ast, ast_type->path)) == NULL
        || path->segment_count == 0u || path->segments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            ast_item_id, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            is_supertrait
                ? "supertrait must be a non-generic trait path"
                : "implemented trait must be a non-generic trait path");
        return 0;
    }
    if (!cm_lower_ast_path_storage_valid(path)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, ast_type_id, ast_type->path, CM_HIR_OK,
            "trait path storage is invalid");
        return 0;
    }
    for (index = 0u; index + 1u < path->segment_count; ++index) {
        if (path->segments[index].argument_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                ast_item_id, ast_type_id, ast_type->path, CM_HIR_OK,
                is_supertrait
                    ? "generic supertrait arguments require structural "
                      "trait-bound substitution"
                    : "generic trait arguments in impl headers are deferred "
                      "until impl substitution is structural");
            return 0;
        }
    }
    lookup = cm_lower_lookup_path(state, path, module,
        CM_HIR_LOWER_PATH_TYPE, &record, &module_definition);
    (void)module_definition;
    if (lookup == CM_LOWER_LOOKUP_STALE_GRAPH) {
        cm_lower_fail(state, CM_HIR_LOWER_STALE_GRAPH, span, ast_item_id,
            ast_type_id, ast_type->path, CM_HIR_OK,
            is_supertrait
                ? "graph or import revision changed during supertrait lookup"
                : "graph or import revision changed during impl trait lookup");
        return 0;
    }
    if (lookup == CM_LOWER_LOOKUP_RESOLVER_ERROR) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            ast_item_id, ast_type_id, ast_type->path, CM_HIR_OK,
            is_supertrait
                ? "local-crate supertrait resolution failed"
                : "local-crate impl trait resolution failed");
        return 0;
    }
    if (lookup == CM_LOWER_LOOKUP_NOT_FOUND) {
        lookup = cm_lower_resolve_library_binding(state, path,
            &external_binding);
        if (lookup == CM_LOWER_LOOKUP_RESOLVER_ERROR) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                ast_item_id, ast_type_id, ast_type->path, CM_HIR_OK,
                "dependency trait-path resolution failed");
            return 0;
        }
        if (lookup == CM_LOWER_LOOKUP_NOT_FOUND) {
            cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
                ast_item_id, ast_type_id, ast_type->path, CM_HIR_OK,
                is_supertrait
                    ? "supertrait path is unresolved"
                    : "implemented trait path is unresolved");
            return 0;
        }
        if (external_binding.kind != CM_HIR_LIBRARY_BINDING_TRAIT) {
            cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
                ast_item_id, ast_type_id, ast_type->path, CM_HIR_OK,
                "dependency trait path names a non-trait binding");
            return 0;
        }
        item = cm_lower_bound_item(state, external_binding.definition);
        if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                ast_item_id, ast_type_id, ast_type->path,
                CM_HIR_INVARIANT_VIOLATION,
                "authenticated dependency trait has an invalid HIR target");
            return 0;
        }
        out_target->definition = item->definition;
        out_target->generic_parameter_start =
            item->generic_parameter_start;
        out_target->generic_parameter_count = item->generic_parameter_count;
        out_target->item = item;
        out_target->local_record = NULL;
    } else if (lookup == CM_LOWER_LOOKUP_TRAIT && record != NULL) {
        item = cm_lower_bound_item(state, record->definition);
        target_ast_item = cm_ast_get_item(record->ast, record->ast_id);
        if (target_ast_item == NULL
            || target_ast_item->kind != CM_AST_ITEM_TRAIT) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                ast_item_id, ast_type_id, ast_type->path,
                CM_HIR_INVARIANT_VIOLATION,
                "local trait record has an invalid AST target");
            return 0;
        }
        if (target_ast_item->data.trait_item.is_alias
            && !allow_trait_alias) {
            cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
                ast_item_id, ast_type_id, ast_type->path, CM_HIR_OK,
                is_supertrait
                    ? "ordinary trait bound cannot name a trait alias"
                    : "implemented trait cannot name a trait alias");
            return 0;
        }
        if (item != NULL && item->kind != CM_HIR_ITEM_TRAIT
            && !(allow_trait_alias
                && item->kind == CM_HIR_ITEM_TRAIT_ALIAS)) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                ast_item_id, ast_type_id, ast_type->path,
                CM_HIR_INVARIANT_VIOLATION,
                "local trait record is bound to a non-trait HIR item");
            return 0;
        }
        out_target->definition = record->definition;
        out_target->generic_parameter_start =
            record->generic_parameter_start;
        out_target->generic_parameter_count =
            record->generic_parameter_count;
        out_target->item = item;
        out_target->local_record = record;
    } else {
        cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
            ast_item_id, ast_type_id, ast_type->path, CM_HIR_OK,
            is_supertrait
                ? "supertrait path does not name a trait"
                : "impl trait path does not name a trait");
        return 0;
    }
    out_trait->definition = out_target->definition;
    if (!cm_lower_trait_positional_arguments(state, ast_item_id,
            &path->segments[path->segment_count - 1u], out_target, module,
            owner, default_self, allow_bindings, allow_constraints,
            is_supertrait, span, &out_trait->arguments,
            &out_trait->argument_count)) {
        return 0;
    }
    return 1;
}

static CmHirTypeId cm_lower_dyn_trait_type(CmLowerState *state,
    CmAstTypeId ast_type_id, const CmAstType *ast_type,
    CmHirModuleId module, CmHirDefId owner)
{
    const CmAstTypeBound *lifetime_bound;
    CmHirNamedType principal_trait;
    CmHirAssociatedTypeEquality *principal_equalities;
    CmHirNamedType *auto_traits;
    CmHirType type;
    CmSpan span;
    uint32_t auto_trait_count;
    uint32_t principal_equality_count;
    uint32_t index;
    int has_principal;

    span = cm_lower_span(state, ast_type->span);
    if (ast_type->bound_count == 0u || ast_type->bounds == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "dynamic trait type has no bound storage");
        return CM_HIR_TYPE_NONE;
    }
    lifetime_bound = NULL;
    has_principal = 0;
    auto_trait_count = 0u;
    principal_equalities = NULL;
    principal_equality_count = 0u;
    auto_traits = (CmHirNamedType *)cm_alloc_zeroed(ast_type->bound_count,
        sizeof(*auto_traits));
    memset(&principal_trait, 0, sizeof(principal_trait));
    for (index = 0u; index < ast_type->bound_count; ++index) {
        const CmAstTypeBound *bound;
        const CmAstType *bound_type;
        const CmAstPath *bound_path;
        int has_lifetime;

        bound = &ast_type->bounds[index];
        has_lifetime = bound->lifetime != CM_INTERN_ID_NONE;
        bound_type = cm_ast_get_type(state->ast, bound->trait_type);
        bound_path = bound_type == NULL
                || bound_type->kind != CM_AST_TYPE_PATH
            ? NULL : cm_ast_get_path(state->ast, bound_type->path);
        if ((has_lifetime && bound->trait_type != CM_AST_TYPE_NONE)
            || (has_lifetime
                && (bound->binder.lifetime_count != 0u
                    || bound->binder.lifetimes != NULL
                    || bound->binder.span.start != 0u
                    || bound->binder.span.end != 0u))
            || (bound->modifier != CM_AST_TYPE_BOUND_REQUIRED
                && bound->modifier != CM_AST_TYPE_BOUND_RELAXED
                && bound->modifier
                    != CM_AST_TYPE_BOUND_CONDITIONALLY_CONST)
            || (has_lifetime
                && (bound->modifier != CM_AST_TYPE_BOUND_REQUIRED
                    || cm_lower_ast_string(state, bound->lifetime)
                        == NULL))
            || (!has_lifetime
                && (bound_type == NULL
                    || bound_type->kind != CM_AST_TYPE_PATH
                    || !cm_lower_ast_path_storage_valid(bound_path)
                    || !cm_lower_lifetime_binder_is_valid(&bound->binder,
                        bound->span, bound_type)))
            || bound->span.start >= bound->span.end
            || bound->span.start < ast_type->span.start
            || bound->span.end > ast_type->span.end
            || (index != 0u
                && ast_type->bounds[index - 1u].span.end
                    >= bound->span.start)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                CM_AST_ITEM_NONE, ast_type_id,
                bound_type == NULL ? CM_AST_PATH_NONE : bound_type->path,
                CM_HIR_OK, "dynamic trait bound storage is invalid");
            goto fail;
        }
        if (bound->modifier != CM_AST_TYPE_BOUND_REQUIRED) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE,
                cm_lower_span(state, bound->span), CM_AST_ITEM_NONE,
                ast_type_id,
                bound_type == NULL ? CM_AST_PATH_NONE : bound_type->path,
                CM_HIR_OK,
                "dynamic trait bounds with relaxed or const modifiers are "
                "not representable in HIR");
            goto fail;
        }
        if (has_lifetime) {
            if (lifetime_bound != NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
                    CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "dynamic trait HIR requires exactly one explicit "
                    "lifetime bound");
                goto fail;
            }
            lifetime_bound = bound;
        } else {
            CmLowerTraitTarget trait_target;
            CmHirNamedType trait;
            CmHirAssociatedTypeEquality *equalities;
            const CmAstItem *target_ast_item;
            uint32_t equality_count;
            int is_auto;

            if (bound->binder.lifetime_count != 0u) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE,
                    cm_lower_span(state, bound->span), CM_AST_ITEM_NONE,
                    ast_type_id, bound_type->path, CM_HIR_OK,
                    "higher-ranked dynamic trait bounds are not "
                    "representable in HIR");
                goto fail;
            }
            memset(&trait, 0, sizeof(trait));
            memset(&trait_target, 0, sizeof(trait_target));
            if (!cm_lower_trait_reference(state, CM_AST_ITEM_NONE,
                    bound->trait_type, module, owner, CM_HIR_TYPE_NONE,
                    &trait, &trait_target, 1, 1, 0, 0)) {
                goto fail;
            }
            target_ast_item = trait_target.local_record == NULL ? NULL
                : cm_ast_get_item(trait_target.local_record->ast,
                    trait_target.local_record->ast_id);
            if (trait_target.item != NULL) {
                if (trait_target.item->kind != CM_HIR_ITEM_TRAIT) {
                    cm_free(trait.arguments);
                    cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE,
                        cm_lower_span(state, bound->span),
                        CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE,
                        CM_HIR_OK, "dynamic trait bound must name an "
                        "authenticated trait");
                    goto fail;
                }
                is_auto = trait_target.item->data.trait_item.is_auto;
            } else if (target_ast_item != NULL
                && target_ast_item->kind == CM_AST_ITEM_TRAIT
                && !target_ast_item->data.trait_item.is_alias) {
                is_auto = target_ast_item->data.trait_item.is_auto;
            } else {
                cm_free(trait.arguments);
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE,
                    cm_lower_span(state, bound->span), CM_AST_ITEM_NONE,
                    ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
                    "dynamic trait bound must name an authenticated trait");
                goto fail;
            }
            if (!is_auto && has_principal) {
                cm_free(trait.arguments);
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
                    CM_AST_ITEM_NONE, ast_type_id, bound_type->path,
                    CM_HIR_OK,
                    "dynamic trait HIR cannot represent multiple ordinary "
                    "principal traits");
                goto fail;
            }
            equalities = NULL;
            equality_count = 0u;
            if (!cm_lower_predicate_equalities(state, CM_AST_ITEM_NONE,
                    bound->trait_type, &trait_target, module, owner, 0,
                    &equalities, &equality_count)) {
                cm_free(trait.arguments);
                goto fail;
            }
            if (is_auto && equality_count != 0u) {
                cm_free(equalities);
                cm_free(trait.arguments);
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, bound->span), CM_AST_ITEM_NONE,
                    ast_type_id, bound_type->path, CM_HIR_OK,
                    "dynamic auto-trait markers cannot have associated "
                    "equalities");
                goto fail;
            }
            if (!is_auto) {
                uint32_t equality_index;

                for (equality_index = 0u;
                     equality_index < equality_count; ++equality_index) {
                    const CmHirItem *associated_item;
                    const CmLowerItemRecord *associated_record;
                    CmHirDefId parent_definition;
                    uint32_t generic_parameter_count;

                    associated_item = cm_lower_bound_item(state,
                        equalities[equality_index].associated_type);
                    associated_record =
                        cm_lower_find_record_by_definition(state,
                            equalities[equality_index].associated_type);
                    if (associated_item != NULL) {
                        parent_definition =
                            associated_item->parent_definition;
                        generic_parameter_count =
                            associated_item->generic_parameter_count;
                    } else if (associated_record != NULL) {
                        parent_definition =
                            associated_record->parent_definition;
                        generic_parameter_count =
                            associated_record->generic_parameter_count;
                    } else {
                        cm_free(equalities);
                        cm_free(trait.arguments);
                        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                            cm_lower_span(state, bound->span),
                            CM_AST_ITEM_NONE, ast_type_id, bound_type->path,
                            CM_HIR_INVARIANT_VIOLATION,
                            "dynamic associated equality lost its "
                            "authenticated declaration");
                        goto fail;
                    }
                    if (!cm_hir_def_id_equal(parent_definition,
                            trait.definition)
                        || generic_parameter_count != 0u) {
                        cm_free(equalities);
                        cm_free(trait.arguments);
                        cm_lower_fail(state,
                            CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                            cm_lower_span(state, bound->span),
                            CM_AST_ITEM_NONE, ast_type_id, bound_type->path,
                            CM_HIR_OK,
                            "dynamic associated equalities are limited to "
                            "nongeneric types declared directly by the "
                            "principal trait");
                        goto fail;
                    }
                }
            }
            if (is_auto) {
                uint32_t marker_index;

                for (marker_index = 0u; marker_index < auto_trait_count;
                     ++marker_index) {
                    if (cm_hir_def_id_equal(
                            auto_traits[marker_index].definition,
                            trait.definition)) {
                        cm_free(trait.arguments);
                        cm_lower_fail(state,
                            CM_HIR_LOWER_UNSUPPORTED_TYPE,
                            cm_lower_span(state, bound->span),
                            CM_AST_ITEM_NONE, ast_type_id, bound_type->path,
                            CM_HIR_OK, "dynamic trait HIR rejects duplicate "
                            "auto-trait markers");
                        goto fail;
                    }
                }
                auto_traits[auto_trait_count++] = trait;
            } else {
                principal_trait = trait;
                principal_equalities = equalities;
                principal_equality_count = equality_count;
                has_principal = 1;
            }
        }
    }
    if (!has_principal && auto_trait_count == 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            CM_AST_ITEM_NONE, ast_type_id, CM_AST_PATH_NONE, CM_HIR_OK,
            "dynamic trait HIR requires a principal or auto-trait marker");
        goto fail;
    }
    for (index = 1u; index < auto_trait_count; ++index) {
        CmHirNamedType marker;
        uint32_t insertion;

        marker = auto_traits[index];
        insertion = index;
        while (insertion != 0u
            && (auto_traits[insertion - 1u].definition.crate_id
                    > marker.definition.crate_id
                || (auto_traits[insertion - 1u].definition.crate_id
                        == marker.definition.crate_id
                    && auto_traits[insertion - 1u].definition.index
                        > marker.definition.index))) {
            auto_traits[insertion] = auto_traits[insertion - 1u];
            insertion -= 1u;
        }
        auto_traits[insertion] = marker;
    }
    for (index = 1u; index < principal_equality_count; ++index) {
        CmHirAssociatedTypeEquality equality;
        uint32_t insertion;

        equality = principal_equalities[index];
        insertion = index;
        while (insertion != 0u
            && (principal_equalities[insertion - 1u]
                    .associated_type.crate_id
                    > equality.associated_type.crate_id
                || (principal_equalities[insertion - 1u]
                        .associated_type.crate_id
                        == equality.associated_type.crate_id
                    && principal_equalities[insertion - 1u]
                        .associated_type.index
                        > equality.associated_type.index))) {
            principal_equalities[insertion] =
                principal_equalities[insertion - 1u];
            insertion -= 1u;
        }
        principal_equalities[insertion] = equality;
    }
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_DYN_TRAIT_KIND;
    type.span = span;
    type.data.dyn_trait_type.principal_trait = principal_trait;
    type.data.dyn_trait_type.has_principal = has_principal;
    type.data.dyn_trait_type.equalities = principal_equalities;
    type.data.dyn_trait_type.equality_count = principal_equality_count;
    type.data.dyn_trait_type.auto_traits = auto_trait_count == 0u
        ? NULL : auto_traits;
    type.data.dyn_trait_type.auto_trait_count = auto_trait_count;
    if (lifetime_bound == NULL) {
        type.data.dyn_trait_type.region.kind = CM_HIR_REGION_INFER;
        type.data.dyn_trait_type.region.data.inference_variable =
            state->next_region_inference;
        state->next_region_inference += 1u;
    } else {
        if (!cm_lower_lifetime(state, lifetime_bound->lifetime, owner,
                cm_lower_span(state, lifetime_bound->span),
                &type.data.dyn_trait_type.region)) {
            goto fail;
        }
    }
    {
        CmHirTypeId result;

        result = cm_lower_add_type(state, &type, ast_type_id);
        cm_free(principal_equalities);
        cm_free(principal_trait.arguments);
        for (index = 0u; index < auto_trait_count; ++index) {
            cm_free(auto_traits[index].arguments);
        }
        cm_free(auto_traits);
        return result;
    }

fail:
    cm_free(principal_equalities);
    cm_free(principal_trait.arguments);
    for (index = 0u; index < auto_trait_count; ++index) {
        cm_free(auto_traits[index].arguments);
    }
    cm_free(auto_traits);
    return CM_HIR_TYPE_NONE;
}

static int cm_lower_ast_path_storage_valid(const CmAstPath *path)
{
    uint32_t index;

    if (path == NULL
        || (path->segment_count != 0u && path->segments == NULL)
        || (path->segment_count == 0u && path->segments != NULL)) {
        return 0;
    }
    for (index = 0u; index < path->segment_count; ++index) {
        if ((path->segments[index].argument_count != 0u
                && path->segments[index].arguments == NULL)
            || (path->segments[index].argument_count == 0u
                && path->segments[index].arguments != NULL)) {
            return 0;
        }
    }
    return 1;
}

static int cm_lower_attribute_has_head(const CmLowerState *state,
    CmAstAttributeId attribute_id, const char *expected)
{
    const CmAstAttribute *attribute;
    const CmInternedString *text;
    size_t expected_length;
    size_t index;

    attribute = cm_ast_get_attribute(state->ast, attribute_id);
    text = attribute == NULL ? NULL
        : cm_ast_get_string(state->ast, attribute->text);
    if (text == NULL || expected == NULL) return 0;
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
                || text->bytes[index] == '\n')) {
            ++index;
        }
        if (index >= text->len || text->bytes[index] != '[') return 0;
        ++index;
        while (index < text->len
            && (text->bytes[index] == ' ' || text->bytes[index] == '\t'
                || text->bytes[index] == '\r'
                || text->bytes[index] == '\n')) {
            ++index;
        }
    }
    expected_length = strlen(expected);
    return text->len - index >= expected_length
        && memcmp(text->bytes + index, expected, expected_length) == 0
        && (text->len - index == expected_length
            || text->bytes[index + expected_length] == ' '
            || text->bytes[index + expected_length] == '\t'
            || text->bytes[index + expected_length] == '\r'
            || text->bytes[index + expected_length] == '\n'
            || text->bytes[index + expected_length] == '('
            || text->bytes[index + expected_length] == ']');
}

static int cm_lower_predicate_equalities(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId trait_ast_type,
    const CmLowerTraitTarget *trait_target, CmHirModuleId module,
    CmHirDefId owner, int allow_constraints,
    CmHirAssociatedTypeEquality **out_equalities, uint32_t *out_count)
{
    const CmAstType *ast_type;
    const CmAstPath *path;
    const CmAstPathSegment *segment;
    CmHirAssociatedTypeEquality *equalities;
    uint32_t equality_count;
    uint32_t index;

    *out_equalities = NULL;
    *out_count = 0u;
    ast_type = cm_ast_get_type(state->ast, trait_ast_type);
    path = ast_type == NULL ? NULL
        : cm_ast_get_path(state->ast, ast_type->path);
    if (ast_type == NULL || ast_type->kind != CM_AST_TYPE_PATH
        || !cm_lower_ast_path_storage_valid(path)
        || path->segment_count == 0u || path->segments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            ast_type == NULL
                ? cm_lower_span(state, (CmAstSpan){ 0u, 0u })
                : cm_lower_span(state, ast_type->span), ast_item_id,
            trait_ast_type,
            ast_type == NULL ? CM_AST_PATH_NONE : ast_type->path,
            CM_HIR_OK, "predicate trait path storage is invalid");
        return 0;
    }
    segment = &path->segments[path->segment_count - 1u];
    equality_count = 0u;
    for (index = 0u; index < segment->argument_count; ++index) {
        if (!cm_lower_validate_generic_constraint(state, ast_item_id,
                ast_type->path, &segment->arguments[index])) {
            return 0;
        }
        if (segment->arguments[index].kind
            == CM_AST_GENERIC_CONSTRAINT) {
            if (allow_constraints) continue;
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, segment->arguments[index].span),
                ast_item_id, CM_AST_TYPE_NONE, ast_type->path, CM_HIR_OK,
                "associated-type constraints cannot be lowered as "
                "predicate equalities");
            return 0;
        }
        if (segment->arguments[index].kind == CM_AST_GENERIC_BINDING) {
            equality_count += 1u;
        }
    }
    equalities = equality_count == 0u ? NULL
        : (CmHirAssociatedTypeEquality *)cm_alloc_zeroed(
            (size_t)equality_count, sizeof(*equalities));
    equality_count = 0u;
    for (index = 0u; index < segment->argument_count && !state->failed;
         ++index) {
        const CmAstGenericArg *argument;
        CmLowerAssociatedTarget associated;
        uint32_t matches;
        uint32_t prior;

        argument = &segment->arguments[index];
        if (argument->kind != CM_AST_GENERIC_BINDING) continue;
        if (argument->span.start > argument->span.end
            || argument->name == CM_INTERN_ID_NONE
            || cm_lower_ast_string(state, argument->name) == NULL
            || argument->type == CM_AST_TYPE_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, argument->span), ast_item_id,
                argument->type, ast_type->path, CM_HIR_OK,
                "predicate associated equality is structurally invalid");
            break;
        }
        cm_lower_find_inherited_associated_type(state,
            trait_target->definition, argument->name, &associated,
            &matches);
        if (matches == 0u) {
            int wrong_namespace;

            wrong_namespace = cm_lower_associated_name_exists(state,
                trait_target->definition, argument->name,
                CM_AST_ITEM_FUNCTION);
            cm_lower_fail(state,
                wrong_namespace ? CM_HIR_LOWER_WRONG_NAMESPACE
                    : CM_HIR_LOWER_UNRESOLVED_PATH,
                cm_lower_span(state, argument->span), ast_item_id,
                argument->type, ast_type->path, CM_HIR_OK,
                wrong_namespace
                    ? "predicate associated equality name resolves in the "
                      "value namespace instead of naming a type"
                    : "predicate trait has no associated type named by "
                      "equality");
            break;
        }
        if (matches != 1u) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                cm_lower_span(state, argument->span), ast_item_id,
                argument->type, ast_type->path, CM_HIR_OK,
                "predicate associated type identity is ambiguous through "
                "the supertrait graph");
            break;
        }
        if (associated.generic_parameter_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, argument->span), ast_item_id,
                argument->type, ast_type->path, CM_HIR_OK,
                "GAT equality bindings are not supported in predicates");
            break;
        }
        for (prior = 0u; prior < equality_count; ++prior) {
            if (cm_hir_def_id_equal(equalities[prior].associated_type,
                    associated.definition)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                    cm_lower_span(state, argument->span), ast_item_id,
                    argument->type, ast_type->path, CM_HIR_OK,
                    "duplicate predicate associated type equality");
                break;
            }
        }
        if (state->failed) break;
        equalities[equality_count].associated_type = associated.definition;
        equalities[equality_count].value = cm_lower_type(state,
            argument->type, module, owner);
        equalities[equality_count].span = cm_lower_span(state,
            argument->span);
        equality_count += 1u;
    }
    if (state->failed) {
        cm_free(equalities);
        return 0;
    }
    *out_equalities = equalities;
    *out_count = equality_count;
    return 1;
}

static int cm_lower_associated_constraint_count(CmLowerState *state,
    CmAstItemId ast_item_id, CmAstTypeId trait_ast_type,
    uint32_t *out_count)
{
    const CmAstType *ast_type;
    const CmAstPath *path;
    const CmAstPathSegment *segment;
    uint32_t count;
    uint32_t index;

    *out_count = 0u;
    ast_type = cm_ast_get_type(state->ast, trait_ast_type);
    path = ast_type == NULL || ast_type->kind != CM_AST_TYPE_PATH ? NULL
        : cm_ast_get_path(state->ast, ast_type->path);
    if (path == NULL || !cm_lower_ast_path_storage_valid(path)
        || path->segment_count == 0u || path->segments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            ast_type == NULL
                ? cm_lower_span(state, (CmAstSpan){ 0u, 0u })
                : cm_lower_span(state, ast_type->span),
            ast_item_id, trait_ast_type,
            ast_type == NULL ? CM_AST_PATH_NONE : ast_type->path,
            CM_HIR_OK, "predicate trait path storage is invalid");
        return 0;
    }
    segment = &path->segments[path->segment_count - 1u];
    count = 0u;
    for (index = 0u; index < segment->argument_count; ++index) {
        const CmAstGenericArg *argument;
        CmInternId associated_name;
        uint32_t bound_index;
        uint32_t prior;

        argument = &segment->arguments[index];
        if (!cm_lower_validate_generic_constraint(state, ast_item_id,
                ast_type->path, argument)) {
            return 0;
        }
        if (argument->kind != CM_AST_GENERIC_CONSTRAINT) continue;
        associated_name = argument->name;
        for (prior = 0u; prior < index; ++prior) {
            const CmAstGenericArg *prior_argument;

            prior_argument = &segment->arguments[prior];
            if ((prior_argument->kind == CM_AST_GENERIC_BINDING
                    || prior_argument->kind
                        == CM_AST_GENERIC_CONSTRAINT)
                && cm_lower_strings_equal(state, prior_argument->name,
                    associated_name)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                    cm_lower_span(state, argument->span), ast_item_id,
                    trait_ast_type, ast_type->path, CM_HIR_OK,
                    "duplicate predicate associated-type binding or "
                    "constraint");
                return 0;
            }
        }
        for (bound_index = 0u; bound_index < argument->bound_count;
             ++bound_index) {
            const CmAstGenericParamBound *bound;
            const CmAstType *bound_type;
            const CmAstPath *bound_path;
            const CmAstPathSegment *bound_segment;
            uint32_t nested_index;

            bound = &argument->bounds[bound_index];
            if (bound->kind != CM_AST_GENERIC_BOUND_TRAIT) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, bound->span), ast_item_id,
                    trait_ast_type, ast_type->path, CM_HIR_OK,
                    "associated-type lifetime constraints are not "
                    "supported in HIR predicates");
                return 0;
            }
            if (bound->modifier != CM_AST_GENERIC_BOUND_REQUIRED) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type, ast_type->path, CM_HIR_OK,
                    "associated-type constraint modifiers are not "
                    "supported in HIR predicates");
                return 0;
            }
            bound_type = cm_ast_get_type(state->ast, bound->trait_type);
            bound_path = bound_type == NULL
                    || bound_type->kind != CM_AST_TYPE_PATH
                ? NULL : cm_ast_get_path(state->ast, bound_type->path);
            bound_segment = bound_path == NULL
                    || !cm_lower_ast_path_storage_valid(bound_path)
                    || bound_path->segment_count == 0u
                ? NULL
                : &bound_path->segments[bound_path->segment_count - 1u];
            if (bound_segment == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type,
                    bound_type == NULL ? CM_AST_PATH_NONE
                        : bound_type->path,
                    CM_HIR_OK,
                    "associated-type constraint trait path is malformed");
                return 0;
            }
            for (nested_index = 0u;
                 nested_index < bound_segment->argument_count;
                 ++nested_index) {
                const CmAstGenericArg *nested;

                nested = &bound_segment->arguments[nested_index];
                if (!cm_lower_validate_generic_constraint(state,
                        ast_item_id, bound_type->path, nested)) {
                    return 0;
                }
                if (nested->kind == CM_AST_GENERIC_CONSTRAINT) {
                    cm_lower_fail(state,
                        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                        cm_lower_span(state, nested->span), ast_item_id,
                        bound->trait_type, bound_type->path, CM_HIR_OK,
                        "nested associated-type constraints require "
                        "recursive predicate expansion");
                    return 0;
                }
            }
            if (count == UINT32_MAX) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, argument->span), ast_item_id,
                    trait_ast_type, ast_type->path,
                    CM_HIR_ID_EXHAUSTED,
                    "associated-type constraint predicate count "
                    "overflow");
                return 0;
            }
            count += 1u;
        }
    }
    *out_count = count;
    return 1;
}

static int cm_lower_copy_lifetime_binder(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstLifetimeBinder *ast_binder,
    CmHirLifetimeBinder *out_binder)
{
    uint32_t binder_index;

    memset(out_binder, 0, sizeof(*out_binder));
    if (ast_binder == NULL || ast_binder->lifetime_count == 0u) return 1;
    out_binder->lifetimes = (CmInternId *)cm_alloc_zeroed(
        ast_binder->lifetime_count, sizeof(CmInternId));
    out_binder->lifetime_count = ast_binder->lifetime_count;
    out_binder->span = cm_lower_span(state, ast_binder->span);
    for (binder_index = 0u;
         binder_index < ast_binder->lifetime_count; ++binder_index) {
        out_binder->lifetimes[binder_index] = cm_lower_copy_string(state,
            ast_binder->lifetimes[binder_index], out_binder->span,
            ast_item_id);
        if (state->failed) {
            cm_free(out_binder->lifetimes);
            memset(out_binder, 0, sizeof(*out_binder));
            return 0;
        }
    }
    return 1;
}

static int cm_lower_one_trait_predicate(CmLowerState *state,
    CmAstItemId ast_item_id, const CmLowerItemRecord *record,
    CmHirTypeId subject, CmAstTypeId trait_ast_type, CmSpan predicate_span,
    const CmAstLifetimeBinder *ast_binder,
    CmHirTraitPredicateModifier modifier,
    CmHirTraitPredicate *out_predicate)
{
    CmLowerTraitTarget trait_target;
    const CmAstLifetimeBinder *previous_binder;

    memset(out_predicate, 0, sizeof(*out_predicate));
    out_predicate->scope = CM_HIR_PREDICATE_SCOPE_NONE;
    out_predicate->subject = subject;
    out_predicate->span = predicate_span;
    out_predicate->modifier = modifier;
    if (!cm_lower_copy_lifetime_binder(state, ast_item_id, ast_binder,
            &out_predicate->binder)) return 0;
    previous_binder = state->active_lifetime_binder;
    if (ast_binder != NULL && ast_binder->lifetime_count != 0u) {
        state->active_lifetime_binder = ast_binder;
    }
    if (!cm_lower_trait_reference(state, ast_item_id, trait_ast_type,
            record->owner_module, record->definition, subject,
            &out_predicate->trait_type, &trait_target, 1, 1, 1, 1)
        || !cm_lower_predicate_equalities(state, ast_item_id,
            trait_ast_type, &trait_target, record->owner_module,
            record->definition, 1, &out_predicate->equalities,
            &out_predicate->equality_count)) {
        state->active_lifetime_binder = previous_binder;
        cm_free(out_predicate->trait_type.arguments);
        cm_free(out_predicate->equalities);
        cm_free(out_predicate->binder.lifetimes);
        memset(out_predicate, 0, sizeof(*out_predicate));
        return 0;
    }
    state->active_lifetime_binder = previous_binder;
    return 1;
}

static int cm_lower_associated_constraint_predicates(
    CmLowerState *state, CmAstItemId ast_item_id,
    const CmLowerItemRecord *record, CmAstTypeId trait_ast_type,
    const CmHirTraitPredicate *outer_predicate,
    CmHirTraitPredicate *predicates, uint32_t *in_out_predicate_count)
{
    const CmAstType *ast_type;
    const CmAstPath *path;
    const CmAstPathSegment *segment;
    uint32_t expected_count;
    uint32_t index;

    if (!cm_lower_associated_constraint_count(state, ast_item_id,
            trait_ast_type, &expected_count)) {
        return 0;
    }
    if (expected_count == 0u) return 1;
    ast_type = cm_ast_get_type(state->ast, trait_ast_type);
    path = cm_ast_get_path(state->ast, ast_type->path);
    segment = &path->segments[path->segment_count - 1u];
    for (index = 0u; index < segment->argument_count && !state->failed;
         ++index) {
        const CmAstGenericArg *argument;
        CmLowerAssociatedTarget associated;
        const CmHirItem *associated_item;
        CmHirDefId defining_trait;
        CmHirGenericArg *associated_arguments;
        CmAstPathSegment associated_segment;
        CmHirGenericParamId associated_parameter_start;
        uint32_t associated_argument_count;
        uint32_t matches;
        uint32_t bound_index;

        argument = &segment->arguments[index];
        if (argument->kind != CM_AST_GENERIC_CONSTRAINT) continue;
        cm_lower_find_inherited_associated_type(state,
            outer_predicate->trait_type.definition, argument->name,
            &associated, &matches);
        if (matches == 0u) {
            int wrong_namespace;

            wrong_namespace = cm_lower_associated_name_exists(state,
                outer_predicate->trait_type.definition, argument->name,
                CM_AST_ITEM_FUNCTION);
            cm_lower_fail(state,
                wrong_namespace ? CM_HIR_LOWER_WRONG_NAMESPACE
                    : CM_HIR_LOWER_UNRESOLVED_PATH,
                cm_lower_span(state, argument->span), ast_item_id,
                trait_ast_type, ast_type->path, CM_HIR_OK,
                wrong_namespace
                    ? "associated-type constraint name resolves in the "
                      "value namespace instead of naming a type"
                    : "predicate trait has no associated type named by "
                      "constraint");
            break;
        }
        if (matches != 1u) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                cm_lower_span(state, argument->span), ast_item_id,
                trait_ast_type, ast_type->path, CM_HIR_OK,
                "predicate associated-type constraint is ambiguous "
                "through the supertrait graph");
            break;
        }
        associated_item = associated.item != NULL ? associated.item
            : cm_lower_bound_item(state, associated.definition);
        defining_trait = associated_item != NULL
            ? associated_item->parent_definition
            : associated.local_record != NULL
                ? associated.local_record->parent_definition
                : cm_hir_def_id_none();
        if (cm_hir_def_id_is_none(defining_trait)) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                cm_lower_span(state, argument->span), ast_item_id,
                trait_ast_type, ast_type->path,
                CM_HIR_INVARIANT_VIOLATION,
                "associated-type constraint lost its defining trait");
            break;
        }
        associated_parameter_start = associated_item != NULL
            ? associated_item->generic_parameter_start
            : associated.local_record->generic_parameter_start;
        memset(&associated_segment, 0, sizeof(associated_segment));
        associated_segment.arguments = argument->name_arguments;
        associated_segment.argument_count = argument->name_argument_count;
        associated_arguments = NULL;
        associated_argument_count = 0u;
        if (!cm_lower_generic_arguments(state, &associated_segment,
                record->owner_module, record->definition,
                cm_lower_span(state, argument->span), &associated_arguments,
                &associated_argument_count)) {
            break;
        }
        if (associated_argument_count != associated.generic_parameter_count) {
            cm_free(associated_arguments);
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, argument->span), ast_item_id,
                trait_ast_type, ast_type->path, CM_HIR_OK,
                "associated-type constraint supplies the wrong number of "
                "GAT arguments");
            break;
        }
        for (bound_index = 0u;
             bound_index < associated_argument_count; ++bound_index) {
            const CmHirGenericParam *parameter;
            CmHirGenericArgKind expected_kind;

            parameter = cm_hir_get_generic_param(state->hir,
                associated_parameter_start + bound_index);
            expected_kind = parameter != NULL
                    && parameter->kind == CM_HIR_GENERIC_LIFETIME
                ? CM_HIR_GENERIC_ARG_LIFETIME
                : parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
                    ? CM_HIR_GENERIC_ARG_TYPE
                    : CM_HIR_GENERIC_ARG_CONST;
            if (parameter == NULL
                || parameter->index != bound_index
                || !cm_hir_def_id_equal(parameter->owner,
                    associated.definition)
                || associated_arguments[bound_index].kind != expected_kind) {
                cm_free(associated_arguments);
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, argument->span), ast_item_id,
                    trait_ast_type, ast_type->path, CM_HIR_OK,
                    "associated-type constraint GAT argument kind does not "
                    "match its parameter");
                break;
            }
        }
        if (state->failed) break;
        for (bound_index = 0u; bound_index < argument->bound_count
                && !state->failed;
             ++bound_index) {
            const CmAstGenericParamBound *bound;
            CmHirNamedType projection_trait;
            CmHirGenericArg *owned_arguments;
            CmHirType projection;
            CmHirTypeId projection_id;

            bound = &argument->bounds[bound_index];
            memset(&projection_trait, 0, sizeof(projection_trait));
            owned_arguments = NULL;
            if (cm_hir_def_id_equal(defining_trait,
                    outer_predicate->trait_type.definition)) {
                projection_trait = outer_predicate->trait_type;
            } else {
                uint32_t argument_count;
                uint32_t path_matches;

                argument_count = 0u;
                path_matches = 0u;
                if (!cm_lower_find_instantiated_supertrait(state,
                        &outer_predicate->trait_type, defining_trait,
                        outer_predicate->subject,
                        cm_lower_span(state, argument->span),
                        &owned_arguments, &argument_count,
                        &path_matches)) {
                    break;
                }
                if (path_matches != 1u) {
                    cm_free(owned_arguments);
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                        cm_lower_span(state, argument->span), ast_item_id,
                        trait_ast_type, ast_type->path, CM_HIR_OK,
                        "associated-type constraint has no unique "
                        "instantiated defining supertrait");
                    break;
                }
                projection_trait.definition = defining_trait;
                projection_trait.arguments = owned_arguments;
                projection_trait.argument_count = argument_count;
            }
            memset(&projection, 0, sizeof(projection));
            projection.kind = CM_HIR_TYPE_PROJECTION_KIND;
            projection.span = cm_lower_span(state, argument->span);
            projection.data.projection_type.self_type =
                outer_predicate->subject;
            projection.data.projection_type.trait_type = projection_trait;
            projection.data.projection_type.associated_type.definition =
                associated.definition;
            projection.data.projection_type.associated_type.arguments =
                associated_arguments;
            projection.data.projection_type.associated_type.argument_count =
                associated_argument_count;
            projection_id = cm_lower_add_type(state, &projection,
                trait_ast_type);
            cm_free(owned_arguments);
            if (state->failed) break;
            if (!cm_lower_one_trait_predicate(state, ast_item_id, record,
                    projection_id, bound->trait_type,
                    cm_lower_span(state, bound->span), NULL,
                    CM_HIR_PREDICATE_REQUIRED,
                    &predicates[*in_out_predicate_count])) {
                break;
            }
            *in_out_predicate_count += 1u;
        }
        cm_free(associated_arguments);
    }
    return !state->failed;
}

static int cm_lower_item_trait_predicates(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    CmHirTraitPredicate *predicates;
    CmHirOutlivesPredicate *outlives_predicates;
    CmHirPredicateScope *predicate_scopes;
    uint32_t predicate_count;
    uint32_t outlives_predicate_count;
    uint32_t predicate_scope_count;
    uint32_t total_count;
    uint32_t outlives_total_count;
    int has_inline_predicate;
    uint32_t parameter_index;
    uint32_t where_index;
    size_t apit_index;
    CmSpan item_span;
    const CmHirItem *previous_active_item;
    const CmHirItem *previous_active_predicate_item;

    item_span = cm_lower_span(state, ast_item->span);
    if (!cm_lower_validate_item_where_predicates(state, ast_item_id,
            ast_item)) {
        return 0;
    }
    total_count = 0u;
    outlives_total_count = 0u;
    predicate_scope_count = 0u;
    has_inline_predicate = 0;
    for (parameter_index = 0u;
         parameter_index < ast_item->generic_parameter_count;
         ++parameter_index) {
        const CmAstGenericParam *parameter;
        uint32_t bound_index;

        parameter = &ast_item->generic_parameters[parameter_index];
        for (bound_index = 0u; bound_index < parameter->bound_count;
             ++bound_index) {
            uint32_t nested_count;

            if (parameter->bounds[bound_index].modifier
                    == CM_AST_GENERIC_BOUND_RELAXED) {
                continue;
            }
            if (parameter->bounds[bound_index].kind
                    == CM_AST_GENERIC_BOUND_LIFETIME) {
                if (outlives_total_count == UINT32_MAX) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                        item_span, ast_item_id, CM_AST_TYPE_NONE,
                        CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
                        "outlives predicate count overflow");
                    return 0;
                }
                outlives_total_count += 1u;
                has_inline_predicate = 1;
                continue;
            }
            if (total_count == UINT32_MAX) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    item_span, ast_item_id, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
                    "trait predicate count overflow");
                return 0;
            }
            total_count += 1u;
            nested_count = 0u;
            if (parameter->bounds[bound_index].trait_type
                    != CM_AST_TYPE_NONE
                && !cm_lower_associated_constraint_count(state, ast_item_id,
                    parameter->bounds[bound_index].trait_type,
                    &nested_count)) {
                return 0;
            }
            if (total_count > UINT32_MAX - nested_count) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    item_span, ast_item_id,
                    parameter->bounds[bound_index].trait_type,
                    CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
                    "trait predicate count overflow");
                return 0;
            }
            total_count += nested_count;
            has_inline_predicate = 1;
        }
    }
    for (apit_index = 0u; apit_index < state->apit_records.len;
         ++apit_index) {
        const CmLowerApitRecord *apit;
        const CmAstType *apit_type;
        uint32_t bound_index;

        apit = (const CmLowerApitRecord *)cm_vec_at_const(
            &state->apit_records, apit_index);
        if (apit == NULL || apit->ast != state->ast
            || !cm_hir_def_id_equal(apit->owner, record->definition)) {
            continue;
        }
        apit_type = cm_ast_get_type(state->ast, apit->ast_type);
        if (!cm_lower_validate_impl_trait_type(state, ast_item_id,
                apit->ast_type, apit_type, NULL)) {
            return 0;
        }
        for (bound_index = 0u; bound_index < apit_type->bound_count;
             ++bound_index) {
            const CmAstTypeBound *bound;
            uint32_t *count;
            uint32_t nested_count;

            bound = &apit_type->bounds[bound_index];
            if (bound->modifier == CM_AST_TYPE_BOUND_RELAXED) continue;
            count = bound->lifetime == CM_INTERN_ID_NONE
                ? &total_count : &outlives_total_count;
            if (*count == UINT32_MAX) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    item_span, ast_item_id, apit->ast_type,
                    CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
                    "argument impl trait predicate count overflow");
                return 0;
            }
            *count += 1u;
            nested_count = 0u;
            if (bound->lifetime == CM_INTERN_ID_NONE
                && bound->trait_type != CM_AST_TYPE_NONE
                && !cm_lower_associated_constraint_count(state,
                    ast_item_id, bound->trait_type, &nested_count)) {
                return 0;
            }
            if (nested_count != 0u
                && bound->binder.lifetime_count != 0u) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "higher-ranked associated-type constraints require "
                    "binder depth in HIR");
                return 0;
            }
            if (*count > UINT32_MAX - nested_count) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    item_span, ast_item_id, bound->trait_type,
                    CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
                    "item predicate count overflow");
                return 0;
            }
            *count += nested_count;
            has_inline_predicate = 1;
        }
    }
    for (where_index = 0u;
         where_index < ast_item->where_predicate_count; ++where_index) {
        const CmAstWherePredicate *ast_predicate;
        uint32_t bound_index;

        ast_predicate = &ast_item->where_predicates[where_index];

        if (ast_predicate->binder.lifetime_count != 0u) {
            predicate_scope_count += 1u;
        }

        for (bound_index = 0u;
             bound_index < ast_predicate->bound_count; ++bound_index) {
            const CmAstWhereBound *bound;
            uint32_t *count;
            uint32_t nested_count;

            if (cm_lower_where_bound_relaxes_generic_parameter(state,
                    ast_item, ast_predicate,
                    &ast_predicate->bounds[bound_index])) {
                continue;
            }

            bound = &ast_predicate->bounds[bound_index];
            count = bound->kind
                    == CM_AST_WHERE_BOUND_LIFETIME
                ? &outlives_total_count : &total_count;
            if (*count == UINT32_MAX) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                    ast_item_id, ast_predicate->subject, CM_AST_PATH_NONE,
                    CM_HIR_ID_EXHAUSTED, "item predicate count overflow");
                return 0;
            }
            *count += 1u;
            nested_count = 0u;
            if (bound->kind == CM_AST_WHERE_BOUND_TRAIT
                && bound->trait_type != CM_AST_TYPE_NONE
                && !cm_lower_associated_constraint_count(state,
                    ast_item_id, bound->trait_type, &nested_count)) {
                return 0;
            }
            if (nested_count != 0u
                && (ast_predicate->binder.lifetime_count != 0u
                    || bound->binder.lifetime_count != 0u)) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "higher-ranked associated-type constraints require "
                    "binder depth in HIR");
                return 0;
            }
            if (*count > UINT32_MAX - nested_count) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    item_span, ast_item_id, bound->trait_type,
                    CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
                    "item predicate count overflow");
                return 0;
            }
            *count += nested_count;
        }
    }
    if (total_count == 0u && outlives_total_count == 0u) return 1;
    if (!(((ast_item->kind == CM_AST_ITEM_TRAIT
                || ast_item->kind == CM_AST_ITEM_IMPL)
            && record->parent_kind == CM_LOWER_PARENT_NONE)
        || ((ast_item->kind == CM_AST_ITEM_STRUCT
                || ast_item->kind == CM_AST_ITEM_UNION
                || ast_item->kind == CM_AST_ITEM_ENUM))
        || (ast_item->kind == CM_AST_ITEM_FUNCTION
            && (record->parent_kind == CM_LOWER_PARENT_NONE
                || record->parent_kind == CM_LOWER_PARENT_TRAIT
                || record->parent_kind == CM_LOWER_PARENT_IMPL))
        || (ast_item->kind == CM_AST_ITEM_TYPE_ALIAS
            && (record->parent_kind == CM_LOWER_PARENT_NONE
                || record->parent_kind == CM_LOWER_PARENT_TRAIT
                || record->parent_kind == CM_LOWER_PARENT_IMPL)))) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, item_span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            has_inline_predicate
                ? "generic bounds and const declarations are not supported "
                  "on this item kind"
                : "where predicates are supported only on traits, impls, "
                  "free functions, methods, and trait-associated types in "
                  "this HIR slice");
        return 0;
    }
    predicates = total_count == 0u ? NULL
        : (CmHirTraitPredicate *)cm_alloc_zeroed(
            (size_t)total_count, sizeof(*predicates));
    outlives_predicates = outlives_total_count == 0u ? NULL
        : (CmHirOutlivesPredicate *)cm_alloc_zeroed(
            (size_t)outlives_total_count, sizeof(*outlives_predicates));
    predicate_scopes = predicate_scope_count == 0u ? NULL
        : (CmHirPredicateScope *)cm_alloc_zeroed(
            (size_t)predicate_scope_count, sizeof(*predicate_scopes));
    predicate_count = 0u;
    outlives_predicate_count = 0u;
    predicate_scope_count = 0u;
    previous_active_item = state->active_item;
    previous_active_predicate_item = state->active_predicate_item;
    hir_item->predicates = predicates;
    hir_item->predicate_count = 0u;
    hir_item->outlives_predicates = outlives_predicates;
    hir_item->outlives_predicate_count = 0u;
    hir_item->predicate_scopes = predicate_scopes;
    hir_item->predicate_scope_count = 0u;
    state->active_item = hir_item;
    state->active_predicate_item = hir_item;
    for (parameter_index = 0u;
         parameter_index < ast_item->generic_parameter_count
            && !state->failed;
         ++parameter_index) {
        const CmAstGenericParam *parameter;
        const CmLowerGenericRecord *generic;
        CmHirType subject_type;
        CmHirTypeId subject;
        CmHirRegion subject_lifetime;
        uint32_t bound_index;
        int has_predicate_bound;

        parameter = &ast_item->generic_parameters[parameter_index];
        has_predicate_bound = 0;
        for (bound_index = 0u; bound_index < parameter->bound_count;
             ++bound_index) {
            if (parameter->bounds[bound_index].modifier
                    != CM_AST_GENERIC_BOUND_RELAXED
                && (parameter->bounds[bound_index].kind
                        == CM_AST_GENERIC_BOUND_TRAIT
                    || parameter->bounds[bound_index].kind
                        == CM_AST_GENERIC_BOUND_LIFETIME)) {
                has_predicate_bound = 1;
                break;
            }
        }
        if (!has_predicate_bound) continue;
        generic = cm_lower_find_generic(state, record->definition,
            parameter->name);
        if (generic == NULL
            || (generic->kind != CM_HIR_GENERIC_TYPE
                && generic->kind != CM_HIR_GENERIC_LIFETIME)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVALID_ID,
                "inline bound lost its generic parameter");
            break;
        }
        subject = CM_HIR_TYPE_NONE;
        memset(&subject_type, 0, sizeof(subject_type));
        memset(&subject_lifetime, 0, sizeof(subject_lifetime));
        if (generic->kind == CM_HIR_GENERIC_TYPE) {
            subject_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
            subject_type.span = item_span;
            subject_type.data.parameter_type.parameter = generic->hir_id;
            subject = cm_lower_add_type(state, &subject_type,
                CM_AST_TYPE_NONE);
        } else {
            subject_lifetime.kind = CM_HIR_REGION_EARLY_BOUND;
            subject_lifetime.data.parameter = generic->hir_id;
        }
        for (bound_index = 0u;
             bound_index < parameter->bound_count && !state->failed;
             ++bound_index) {
            const CmAstGenericParamBound *bound;

            bound = &parameter->bounds[bound_index];
            if (bound->modifier == CM_AST_GENERIC_BOUND_RELAXED) {
                continue;
            }
            if (bound->kind == CM_AST_GENERIC_BOUND_LIFETIME) {
                CmHirOutlivesPredicate *predicate;

                predicate = &outlives_predicates[
                    outlives_predicate_count];
                predicate->subject_kind = generic->kind
                        == CM_HIR_GENERIC_TYPE
                    ? CM_HIR_OUTLIVES_TYPE
                    : CM_HIR_OUTLIVES_LIFETIME;
                if (predicate->subject_kind == CM_HIR_OUTLIVES_TYPE) {
                    predicate->subject.type = subject;
                } else {
                    predicate->subject.lifetime = subject_lifetime;
                }
                predicate->span = cm_lower_span(state, bound->span);
                if (!cm_lower_lifetime(state, bound->lifetime,
                        record->definition, predicate->span,
                        &predicate->bound)) {
                    break;
                }
                outlives_predicate_count += 1u;
                hir_item->outlives_predicate_count =
                    outlives_predicate_count;
                continue;
            }
            if (generic->kind != CM_HIR_GENERIC_TYPE) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "lifetime generic parameter has a trait bound");
                break;
            }
            if (bound->span.start > bound->span.end
                || bound->trait_type == CM_AST_TYPE_NONE) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                    ast_item_id, bound->trait_type, CM_AST_PATH_NONE,
                    CM_HIR_OK, "inline trait bound is malformed");
                break;
            }
            if (!cm_lower_one_trait_predicate(state, ast_item_id, record,
                    subject, bound->trait_type,
                    cm_lower_span(state, bound->span),
                    NULL,
                    bound->modifier
                            == CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST
                        ? CM_HIR_PREDICATE_CONST_IF_CONST
                        : CM_HIR_PREDICATE_REQUIRED,
                    &predicates[predicate_count])) {
                break;
            }
            predicate_count += 1u;
            hir_item->predicate_count = predicate_count;
            if (!cm_lower_associated_constraint_predicates(state,
                    ast_item_id, record, bound->trait_type,
                    &predicates[predicate_count - 1u], predicates,
                    &predicate_count)) {
                break;
            }
            hir_item->predicate_count = predicate_count;
        }
    }
    for (apit_index = 0u;
         apit_index < state->apit_records.len && !state->failed;
         ++apit_index) {
        const CmLowerApitRecord *apit;
        const CmAstType *apit_type;
        const CmHirGenericParam *generic;
        CmHirType subject_type;
        CmHirTypeId subject;
        uint32_t bound_index;

        apit = (const CmLowerApitRecord *)cm_vec_at_const(
            &state->apit_records, apit_index);
        if (apit == NULL || apit->ast != state->ast
            || !cm_hir_def_id_equal(apit->owner, record->definition)) {
            continue;
        }
        apit_type = cm_ast_get_type(state->ast, apit->ast_type);
        generic = cm_hir_get_generic_param(state->hir, apit->hir_id);
        if (apit_type == NULL || generic == NULL
            || generic->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(generic->owner, record->definition)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                ast_item_id, apit->ast_type, CM_AST_PATH_NONE,
                CM_HIR_INVALID_ID,
                "argument impl trait lost its authenticated generic");
            break;
        }
        memset(&subject_type, 0, sizeof(subject_type));
        subject_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
        subject_type.span = cm_lower_span(state, apit_type->span);
        subject_type.data.parameter_type.parameter = apit->hir_id;
        subject = cm_lower_add_type(state, &subject_type,
            apit->ast_type);
        for (bound_index = 0u;
             bound_index < apit_type->bound_count && !state->failed;
             ++bound_index) {
            const CmAstTypeBound *bound;

            bound = &apit_type->bounds[bound_index];
            if (bound->modifier == CM_AST_TYPE_BOUND_RELAXED) continue;
            if (bound->lifetime != CM_INTERN_ID_NONE) {
                CmHirOutlivesPredicate *predicate;

                predicate = &outlives_predicates[
                    outlives_predicate_count];
                predicate->subject_kind = CM_HIR_OUTLIVES_TYPE;
                predicate->subject.type = subject;
                predicate->span = cm_lower_span(state, bound->span);
                if (!cm_lower_lifetime(state, bound->lifetime,
                        record->definition, predicate->span,
                        &predicate->bound)) {
                    break;
                }
                outlives_predicate_count += 1u;
                hir_item->outlives_predicate_count =
                    outlives_predicate_count;
                continue;
            }
            if (!cm_lower_one_trait_predicate(state, ast_item_id, record,
                    subject, bound->trait_type,
                    cm_lower_span(state, bound->span), &bound->binder,
                    bound->modifier
                            == CM_AST_TYPE_BOUND_CONDITIONALLY_CONST
                        ? CM_HIR_PREDICATE_CONST_IF_CONST
                        : CM_HIR_PREDICATE_REQUIRED,
                    &predicates[predicate_count])) {
                break;
            }
            predicate_count += 1u;
            hir_item->predicate_count = predicate_count;
            if (!cm_lower_associated_constraint_predicates(state,
                    ast_item_id, record, bound->trait_type,
                    &predicates[predicate_count - 1u], predicates,
                    &predicate_count)) {
                break;
            }
            hir_item->predicate_count = predicate_count;
        }
    }
    for (where_index = 0u;
         where_index < ast_item->where_predicate_count && !state->failed;
         ++where_index) {
        const CmAstWherePredicate *ast_predicate;
        CmHirTypeId subject;
        CmHirRegion subject_lifetime;
        CmHirPredicateScope *predicate_scope;
        CmHirPredicateScopeId predicate_scope_id;
        const CmAstLifetimeBinder *previous_lifetime_binder;
        uint32_t bound_index;

        ast_predicate = &ast_item->where_predicates[where_index];
        predicate_scope = NULL;
        predicate_scope_id = CM_HIR_PREDICATE_SCOPE_NONE;
        if (ast_predicate->span.start > ast_predicate->span.end
            || ast_predicate->bound_count == 0u
            || ast_predicate->bounds == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, item_span,
                ast_item_id, ast_predicate->subject, CM_AST_PATH_NONE,
                CM_HIR_OK, "where predicate span or bound storage is "
                "invalid");
            break;
        }
        subject = CM_HIR_TYPE_NONE;
        memset(&subject_lifetime, 0, sizeof(subject_lifetime));
        previous_lifetime_binder = state->active_lifetime_binder;
        if (ast_predicate->binder.lifetime_count != 0u) {
            predicate_scope_id = predicate_scope_count + 1u;
            predicate_scope = &predicate_scopes[predicate_scope_count];
            predicate_scope->span = cm_lower_span(state,
                ast_predicate->span);
            if (!cm_lower_copy_lifetime_binder(state, ast_item_id,
                    &ast_predicate->binder, &predicate_scope->binder)) {
                break;
            }
            for (bound_index = 0u;
                 bound_index < ast_predicate->bound_count; ++bound_index) {
                if (ast_predicate->bounds[bound_index].kind
                        == CM_AST_WHERE_BOUND_LIFETIME) {
                    predicate_scope->outlives_predicate_count += 1u;
                } else {
                    predicate_scope->trait_predicate_count += 1u;
                }
            }
            predicate_scope_count += 1u;
            hir_item->predicate_scope_count = predicate_scope_count;
        }
        if (ast_predicate->kind == CM_AST_WHERE_PREDICATE_TYPE) {
            const CmAstType *ast_subject;
            const CmAstPath *subject_path;

            ast_subject = cm_ast_get_type(state->ast,
                ast_predicate->subject);
            subject_path = ast_subject == NULL
                || ast_subject->kind != CM_AST_TYPE_PATH ? NULL
                : cm_ast_get_path(state->ast, ast_subject->path);
            if (subject_path != NULL
                && !cm_lower_ast_path_storage_valid(subject_path)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, ast_predicate->span), ast_item_id,
                    ast_predicate->subject, ast_subject->path, CM_HIR_OK,
                    "where-predicate subject path storage is invalid");
                break;
            }
            if (predicate_scope != NULL) {
                state->active_lifetime_binder = &ast_predicate->binder;
            }
            subject = cm_lower_type(state, ast_predicate->subject,
                record->owner_module, record->definition);
            state->active_lifetime_binder = previous_lifetime_binder;
            if (state->failed) break;
            if (predicate_scope != NULL) {
                predicate_scope->subject_kind = CM_HIR_OUTLIVES_TYPE;
                predicate_scope->subject.type = subject;
            }
        } else {
            if (predicate_scope != NULL) {
                state->active_lifetime_binder = &ast_predicate->binder;
            }
            if (!cm_lower_lifetime(state,
                    ast_predicate->subject_lifetime, record->definition,
                    cm_lower_span(state, ast_predicate->span),
                    &subject_lifetime)) {
                state->active_lifetime_binder = previous_lifetime_binder;
                break;
            }
            state->active_lifetime_binder = previous_lifetime_binder;
            if (predicate_scope != NULL) {
                predicate_scope->subject_kind = CM_HIR_OUTLIVES_LIFETIME;
                predicate_scope->subject.lifetime = subject_lifetime;
            }
        }
        for (bound_index = 0u;
             bound_index < ast_predicate->bound_count && !state->failed;
             ++bound_index) {
            const CmAstWhereBound *bound;

            bound = &ast_predicate->bounds[bound_index];
            if (cm_lower_where_bound_relaxes_generic_parameter(state,
                    ast_item, ast_predicate, bound)) {
                continue;
            }
            if (bound->kind == CM_AST_WHERE_BOUND_LIFETIME) {
                CmHirOutlivesPredicate *predicate;

                predicate =
                    &outlives_predicates[outlives_predicate_count];
                predicate->subject_kind = ast_predicate->kind
                        == CM_AST_WHERE_PREDICATE_TYPE
                    ? CM_HIR_OUTLIVES_TYPE
                    : CM_HIR_OUTLIVES_LIFETIME;
                if (predicate->subject_kind == CM_HIR_OUTLIVES_TYPE) {
                    predicate->subject.type = subject;
                } else {
                    predicate->subject.lifetime = subject_lifetime;
                }
                predicate->span = cm_lower_span(state,
                    ast_predicate->span);
                predicate->scope = predicate_scope_id;
                if (predicate_scope != NULL) {
                    state->active_lifetime_binder =
                        &ast_predicate->binder;
                }
                if (!cm_lower_lifetime(state, bound->lifetime,
                        record->definition, predicate->span,
                        &predicate->bound)) {
                    state->active_lifetime_binder =
                        previous_lifetime_binder;
                    break;
                }
                state->active_lifetime_binder = previous_lifetime_binder;
                outlives_predicate_count += 1u;
                hir_item->outlives_predicate_count =
                    outlives_predicate_count;
                continue;
            }
            if (bound->span.start > bound->span.end
                || bound->modifier == CM_AST_WHERE_BOUND_RELAXED
                || bound->trait_type == CM_AST_TYPE_NONE) {
                cm_lower_fail(state,
                    bound->span.start > bound->span.end
                            || bound->trait_type == CM_AST_TYPE_NONE
                        ? CM_HIR_LOWER_INVALID_AST
                        : CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    bound->span.start > bound->span.end
                        ? "where-bound span is invalid"
                        : bound->trait_type == CM_AST_TYPE_NONE
                            ? "where-bound trait type is missing"
                            : "relaxed trait bounds are not supported in "
                              "where predicates");
                break;
            }
            if (predicate_scope != NULL) {
                state->active_lifetime_binder = &ast_predicate->binder;
            }
            if (!cm_lower_one_trait_predicate(state, ast_item_id, record,
                    subject, bound->trait_type,
                    cm_lower_span(state, ast_predicate->span),
                    &bound->binder,
                    bound->modifier
                            == CM_AST_WHERE_BOUND_CONDITIONALLY_CONST
                        ? CM_HIR_PREDICATE_CONST_IF_CONST
                        : bound->modifier == CM_AST_WHERE_BOUND_CONST
                            ? CM_HIR_PREDICATE_CONST
                            : CM_HIR_PREDICATE_REQUIRED,
                    &predicates[predicate_count])) {
                state->active_lifetime_binder = previous_lifetime_binder;
                break;
            }
            state->active_lifetime_binder = previous_lifetime_binder;
            predicates[predicate_count].scope = predicate_scope_id;
            predicate_count += 1u;
            hir_item->predicate_count = predicate_count;
            if (!cm_lower_associated_constraint_predicates(state,
                    ast_item_id, record, bound->trait_type,
                    &predicates[predicate_count - 1u], predicates,
                    &predicate_count)) {
                state->active_lifetime_binder = previous_lifetime_binder;
                break;
            }
            hir_item->predicate_count = predicate_count;
        }
    }
    state->active_item = previous_active_item;
    state->active_predicate_item = previous_active_predicate_item;
    if (state->failed || predicate_count != total_count
        || outlives_predicate_count != outlives_total_count) {
        uint32_t index;

        for (index = 0u; index < predicate_count; ++index) {
            cm_free(predicates[index].trait_type.arguments);
            cm_free(predicates[index].equalities);
            cm_free(predicates[index].binder.lifetimes);
        }
        cm_free(predicates);
        hir_item->predicates = NULL;
        hir_item->predicate_count = 0u;
        cm_free(outlives_predicates);
        hir_item->outlives_predicates = NULL;
        hir_item->outlives_predicate_count = 0u;
        for (index = 0u; index < predicate_scope_count; ++index) {
            cm_free(predicate_scopes[index].binder.lifetimes);
        }
        cm_free(predicate_scopes);
        hir_item->predicate_scopes = NULL;
        hir_item->predicate_scope_count = 0u;
        return 0;
    }
    return 1;
}

static CmHirTypeId cm_lower_associated_bound_projection_subject(
    CmLowerState *state, CmAstItemId ast_item_id,
    const CmLowerItemRecord *record, CmSpan span);

static int cm_lower_associated_type_bound(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstAssociatedTypeBound *ast_bound,
    const CmLowerItemRecord *record, CmHirTypeId *projection_subject,
    CmHirAssociatedTypeBound *out_bound)
{
    const CmAstType *ast_type;
    const CmAstPath *path;
    const CmAstPathSegment *last_segment;
    CmLowerTraitTarget trait_target;
    CmLowerLookupResult lookup;
    CmSpan span;
    uint32_t equality_count;
    uint32_t index;

    memset(out_bound, 0, sizeof(*out_bound));
    span = cm_lower_span(state, ast_bound->span);
    if ((unsigned int)ast_bound->kind
            > (unsigned int)CM_AST_ASSOC_BOUND_LIFETIME
        || (unsigned int)ast_bound->modifier
            > (unsigned int)CM_AST_ASSOC_BOUND_RELAXED
        || ast_bound->span.start > ast_bound->span.end) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            ast_bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
            "associated-type bound is malformed");
        return 0;
    }
    if (ast_bound->kind == CM_AST_ASSOC_BOUND_LIFETIME) {
        if (ast_bound->modifier != CM_AST_ASSOC_BOUND_REQUIRED
            || ast_bound->trait_type != CM_AST_TYPE_NONE
            || ast_bound->lifetime == CM_INTERN_ID_NONE
            || cm_ast_get_string(state->ast, ast_bound->lifetime) == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, ast_bound->trait_type, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "lifetime associated-type bound is malformed");
            return 0;
        }
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "lifetime associated-type bounds are outside the supported "
            "HIR slice");
        return 0;
    }
    if (ast_bound->trait_type == CM_AST_TYPE_NONE
        || ast_bound->lifetime != CM_INTERN_ID_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            ast_bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait associated-type bound is malformed");
        return 0;
    }
    ast_type = cm_ast_get_type(state->ast, ast_bound->trait_type);
    if (ast_type == NULL || ast_type->kind != CM_AST_TYPE_PATH
        || (path = cm_ast_get_path(state->ast, ast_type->path)) == NULL
        || path->segment_count == 0u || path->segments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
            ast_item_id, ast_bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
            "associated-type bound must be a trait path");
        return 0;
    }
    last_segment = &path->segments[path->segment_count - 1u];
    for (index = 0u; index + 1u < path->segment_count; ++index) {
        if (path->segments[index].argument_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
                ast_item_id, ast_bound->trait_type, ast_type->path,
                CM_HIR_OK,
                "associated-type bound qualifiers cannot have generic "
                "arguments");
            return 0;
        }
    }
    if (last_segment->argument_count != 0u
        && last_segment->arguments == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            ast_bound->trait_type, ast_type->path, CM_HIR_OK,
            "associated-type bound argument count has no storage");
        return 0;
    }
    for (index = 0u; index < last_segment->argument_count; ++index) {
        const CmAstGenericArg *argument;

        argument = &last_segment->arguments[index];
        if (!cm_lower_validate_generic_constraint(state, ast_item_id,
                ast_type->path, argument)) {
            return 0;
        }
        if (argument->kind == CM_AST_GENERIC_CONSTRAINT) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, argument->span), ast_item_id,
                ast_bound->trait_type, ast_type->path, CM_HIR_OK,
                "nested associated-type constraints are not supported "
                "in HIR associated-type bounds");
            return 0;
        }
    }
    lookup = cm_lower_lookup_trait_target(state, path,
        record->owner_module,
        &trait_target);
    if (lookup == CM_LOWER_LOOKUP_STALE_GRAPH) {
        cm_lower_fail(state, CM_HIR_LOWER_STALE_GRAPH, span, ast_item_id,
            ast_bound->trait_type, ast_type->path, CM_HIR_OK,
            "graph or import revision changed during associated-bound "
            "lookup");
        return 0;
    }
    if (lookup == CM_LOWER_LOOKUP_RESOLVER_ERROR) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            ast_item_id, ast_bound->trait_type, ast_type->path, CM_HIR_OK,
            "local-crate associated-bound trait resolution failed");
        return 0;
    }
    if (lookup == CM_LOWER_LOOKUP_NOT_FOUND) {
        cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
            ast_item_id, ast_bound->trait_type, ast_type->path, CM_HIR_OK,
            "associated-type bound trait path is unresolved");
        return 0;
    }
    if (lookup != CM_LOWER_LOOKUP_TRAIT) {
        cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE, span,
            ast_item_id, ast_bound->trait_type, ast_type->path, CM_HIR_OK,
            "associated-type bound path does not name a trait");
        return 0;
    }
    if (ast_bound->modifier == CM_AST_ASSOC_BOUND_REQUIRED) {
        out_bound->modifier = CM_HIR_ASSOC_BOUND_REQUIRED;
    } else if (ast_bound->modifier == CM_AST_ASSOC_BOUND_RELAXED) {
        if (!cm_lower_string_is(state, last_segment->name, "Sized")
            || last_segment->argument_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, span,
                ast_item_id, ast_bound->trait_type, ast_type->path,
                CM_HIR_OK,
                "only ?Sized relaxed associated-type bounds are supported");
            return 0;
        }
        out_bound->modifier = CM_HIR_ASSOC_BOUND_RELAXED;
    } else {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            ast_bound->trait_type, ast_type->path, CM_HIR_OK,
            "associated-type bound has an invalid modifier");
        return 0;
    }
    out_bound->trait_type.definition = trait_target.definition;
    out_bound->span = span;
    if (*projection_subject == CM_HIR_TYPE_NONE) {
        uint32_t positional_count;

        positional_count = 0u;
        for (index = 0u; index < last_segment->argument_count; ++index) {
            if (last_segment->arguments[index].kind
                != CM_AST_GENERIC_BINDING) {
                positional_count += 1u;
            }
        }
        for (index = positional_count;
             index < trait_target.generic_parameter_count; ++index) {
            const CmHirGenericParam *parameter;
            const CmHirType *default_type;

            parameter = cm_hir_get_generic_param(state->hir,
                trait_target.generic_parameter_start + index);
            default_type = parameter == NULL || !parameter->has_default
                    || parameter->default_argument.kind
                        != CM_HIR_GENERIC_ARG_TYPE
                ? NULL : cm_hir_get_type(state->hir,
                    parameter->default_argument.data.type);
            if (default_type != NULL
                && default_type->kind == CM_HIR_TYPE_SELF_KIND
                && cm_hir_def_id_equal(default_type->data.self_type.owner,
                    trait_target.definition)) {
                *projection_subject =
                    cm_lower_associated_bound_projection_subject(state,
                        ast_item_id, record, span);
                if (state->failed) return 0;
                break;
            }
        }
    }
    if (!cm_lower_trait_positional_arguments(state, ast_item_id,
            last_segment, &trait_target, record->owner_module,
            record->definition, *projection_subject, 1, 0, 0, span,
            &out_bound->trait_type.arguments,
            &out_bound->trait_type.argument_count)) {
        return 0;
    }
    equality_count = 0u;
    for (index = 0u; index < last_segment->argument_count; ++index) {
        if (last_segment->arguments[index].kind
            == CM_AST_GENERIC_BINDING) {
            equality_count += 1u;
        }
    }
    if (equality_count != 0u) {
        out_bound->equalities =
            (CmHirAssociatedTypeEquality *)cm_alloc_zeroed(
                (size_t)equality_count,
                sizeof(CmHirAssociatedTypeEquality));
    }
    equality_count = 0u;
    for (index = 0u; index < last_segment->argument_count
         && !state->failed; ++index) {
        const CmAstGenericArg *argument;
        CmLowerAssociatedTarget associated_record;
        uint32_t matches;
        uint32_t prior;

        argument = &last_segment->arguments[index];
        if (argument->kind != CM_AST_GENERIC_BINDING) continue;
        if (argument->name == CM_INTERN_ID_NONE
            || argument->type == CM_AST_TYPE_NONE
            || cm_lower_ast_string(state, argument->name) == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, argument->span), ast_item_id,
                ast_bound->trait_type, ast_type->path, CM_HIR_OK,
                "associated-type equality is structurally incomplete");
            break;
        }
        cm_lower_find_associated_type(state, trait_target.definition,
            argument->name, &associated_record, &matches);
        if (matches == 0u) {
            int wrong_namespace;

            wrong_namespace = cm_lower_associated_name_exists(state,
                trait_target.definition, argument->name,
                CM_AST_ITEM_FUNCTION);
            cm_lower_fail(state,
                wrong_namespace ? CM_HIR_LOWER_WRONG_NAMESPACE
                    : CM_HIR_LOWER_UNRESOLVED_PATH,
                cm_lower_span(state, argument->span), ast_item_id,
                ast_bound->trait_type, ast_type->path, CM_HIR_OK,
                wrong_namespace
                    ? "associated-type equality name resolves in the value "
                      "namespace instead of naming a type"
                    : "bound trait has no associated type named by equality");
            break;
        }
        if (matches != 1u) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                cm_lower_span(state, argument->span), ast_item_id,
                ast_bound->trait_type, ast_type->path, CM_HIR_OK,
                "bound trait associated-type identity is ambiguous");
            break;
        }
        if (associated_record.generic_parameter_count != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                cm_lower_span(state, argument->span), ast_item_id,
                ast_bound->trait_type, ast_type->path, CM_HIR_OK,
                "GAT equality bindings are not supported in this slice");
            break;
        }
        for (prior = 0u; prior < equality_count; ++prior) {
            if (cm_hir_def_id_equal(out_bound->equalities[prior]
                        .associated_type,
                    associated_record.definition)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                    cm_lower_span(state, argument->span), ast_item_id,
                    ast_bound->trait_type, ast_type->path, CM_HIR_OK,
                    "duplicate associated type equality binding");
                break;
            }
        }
        if (state->failed) break;
        out_bound->equalities[equality_count].associated_type =
            associated_record.definition;
        out_bound->equalities[equality_count].value = cm_lower_type(state,
            argument->type, record->owner_module,
            record->definition);
        out_bound->equalities[equality_count].span = cm_lower_span(state,
            argument->span);
        equality_count += 1u;
    }
    if (state->failed) {
        cm_free(out_bound->trait_type.arguments);
        out_bound->trait_type.arguments = NULL;
        out_bound->trait_type.argument_count = 0u;
        cm_free(out_bound->equalities);
        out_bound->equalities = NULL;
        return 0;
    }
    out_bound->equality_count = equality_count;
    return 1;
}

static int cm_lower_trait_identity_arguments(CmLowerState *state,
    CmAstItemId ast_item_id, const CmHirItem *trait_item, CmSpan span,
    CmHirGenericArg **out_arguments, uint32_t *out_count)
{
    CmHirGenericArg *arguments;
    uint32_t index;

    *out_arguments = NULL;
    *out_count = 0u;
    if (trait_item->generic_parameter_count == 0u) return 1;
    if (trait_item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "associated projection parent trait lost its generic range");
        return 0;
    }
    arguments = (CmHirGenericArg *)cm_alloc_zeroed(
        (size_t)trait_item->generic_parameter_count, sizeof(*arguments));
    for (index = 0u; index < trait_item->generic_parameter_count
            && !state->failed;
         ++index) {
        const CmHirGenericParam *parameter;

        if (index > UINT32_MAX - trait_item->generic_parameter_start) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_ID_EXHAUSTED,
                "associated projection parent generic ID overflow");
            break;
        }
        parameter = cm_hir_get_generic_param(state->hir,
            trait_item->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                trait_item->definition)) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "associated projection parent trait has an invalid "
                "generic signature");
            break;
        }
        if (parameter->kind == CM_HIR_GENERIC_LIFETIME) {
            arguments[index].kind = CM_HIR_GENERIC_ARG_LIFETIME;
            arguments[index].data.lifetime.kind =
                CM_HIR_REGION_EARLY_BOUND;
            arguments[index].data.lifetime.data.parameter =
                trait_item->generic_parameter_start + index;
        } else if (parameter->kind == CM_HIR_GENERIC_TYPE) {
            CmHirType parameter_type;

            memset(&parameter_type, 0, sizeof(parameter_type));
            parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
            parameter_type.span = span;
            parameter_type.data.parameter_type.parameter =
                trait_item->generic_parameter_start + index;
            arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
            arguments[index].data.type = cm_lower_add_type(state,
                &parameter_type, CM_AST_TYPE_NONE);
        } else if (parameter->kind == CM_HIR_GENERIC_CONST) {
            if (parameter->declared_type == CM_HIR_TYPE_NONE
                || cm_hir_get_type(state->hir,
                    parameter->declared_type) == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_INVALID_ID,
                    "associated projection const parameter has no "
                    "declared type");
                break;
            }
            arguments[index].kind = CM_HIR_GENERIC_ARG_CONST;
            arguments[index].data.constant.kind =
                CM_HIR_CONST_PARAMETER;
            arguments[index].data.constant.type =
                parameter->declared_type;
            arguments[index].data.constant.data.parameter =
                trait_item->generic_parameter_start + index;
        } else {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "associated projection parent trait has an invalid "
                "generic kind");
            break;
        }
    }
    if (state->failed) {
        cm_free(arguments);
        return 0;
    }
    *out_arguments = arguments;
    *out_count = trait_item->generic_parameter_count;
    return 1;
}

static CmHirTypeId cm_lower_associated_bound_projection_subject(
    CmLowerState *state, CmAstItemId ast_item_id,
    const CmLowerItemRecord *record, CmSpan span)
{
    const CmHirItem *parent_trait;
    CmHirGenericArg *trait_arguments;
    uint32_t trait_argument_count;
    CmHirType projection;
    CmHirTypeId result;

    if (record->generic_parameter_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "GAT lifetime bounds require structural associated-type "
            "projection arguments");
        return CM_HIR_TYPE_NONE;
    }
    parent_trait = cm_lower_bound_item(state, record->parent_definition);
    if (parent_trait == NULL || parent_trait->kind != CM_HIR_ITEM_TRAIT
        || parent_trait->data.trait_item.is_auto) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "associated bound has no authenticated parent trait");
        return CM_HIR_TYPE_NONE;
    }
    trait_arguments = NULL;
    trait_argument_count = 0u;
    if (!cm_lower_trait_identity_arguments(state, ast_item_id,
            parent_trait, span, &trait_arguments,
            &trait_argument_count)) {
        return CM_HIR_TYPE_NONE;
    }
    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_HIR_TYPE_PROJECTION_KIND;
    projection.span = span;
    projection.data.projection_type.self_type = cm_lower_self_type(state,
        CM_AST_TYPE_NONE, span, parent_trait->definition);
    projection.data.projection_type.trait_type.definition =
        parent_trait->definition;
    projection.data.projection_type.trait_type.arguments = trait_arguments;
    projection.data.projection_type.trait_type.argument_count =
        trait_argument_count;
    projection.data.projection_type.associated_type.definition =
        record->definition;
    result = state->failed ? CM_HIR_TYPE_NONE
        : cm_lower_add_type(state, &projection, CM_AST_TYPE_NONE);
    cm_free(trait_arguments);
    return result;
}

static int cm_lower_associated_type_lifetime_bound(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstAssociatedTypeBound *ast_bound,
    const CmLowerItemRecord *record, CmHirTypeId subject,
    CmHirOutlivesPredicate *out_predicate)
{
    const CmHirGenericParam *parameter;
    CmSpan span;

    memset(out_predicate, 0, sizeof(*out_predicate));
    span = cm_lower_span(state, ast_bound->span);
    if (ast_bound->kind != CM_AST_ASSOC_BOUND_LIFETIME
        || ast_bound->modifier != CM_AST_ASSOC_BOUND_REQUIRED
        || ast_bound->span.start > ast_bound->span.end
        || ast_bound->trait_type != CM_AST_TYPE_NONE
        || ast_bound->lifetime == CM_INTERN_ID_NONE
        || cm_ast_get_string(state->ast, ast_bound->lifetime) == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            ast_item_id, ast_bound->trait_type, CM_AST_PATH_NONE,
            CM_HIR_OK, "lifetime associated-type bound is malformed");
        return 0;
    }
    out_predicate->subject_kind = CM_HIR_OUTLIVES_TYPE;
    out_predicate->subject.type = subject;
    out_predicate->span = span;
    if (!cm_lower_lifetime(state, ast_bound->lifetime,
            record->definition, span, &out_predicate->bound)) {
        return 0;
    }
    if (out_predicate->bound.kind == CM_HIR_REGION_STATIC) return 1;
    if (out_predicate->bound.kind != CM_HIR_REGION_EARLY_BOUND) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            out_predicate->bound.kind == CM_HIR_REGION_LATE_BOUND
                ? "late-bound associated-type lifetime requires an actual "
                  "associated-item binder"
                : "associated-type lifetime bound must be 'static or an "
                  "authenticated enclosing lifetime parameter");
        return 0;
    }
    parameter = cm_hir_get_generic_param(state->hir,
        out_predicate->bound.data.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_LIFETIME
        || !cm_hir_def_id_equal(parameter->owner,
            record->parent_definition)) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "associated-type lifetime bound is not owned by its "
            "enclosing trait");
        return 0;
    }
    return 1;
}

static int cm_lower_supertrait_reaches_definition(
    const CmLowerState *state, CmHirDefId start, CmHirDefId target)
{
    unsigned char *seen;
    CmVec pending;
    CmHirDefId current;
    int reaches;

    seen = (unsigned char *)cm_alloc_zeroed(
        state->hir->items.len == 0u ? 1u : state->hir->items.len,
        sizeof(unsigned char));
    cm_vec_init(&pending, sizeof(CmHirDefId));
    (void)cm_vec_push(&pending, &start);
    reaches = 0;
    while (cm_vec_pop(&pending, &current)) {
        const CmHirDefinition *definition;
        const CmHirItem *item;
        size_t item_index;
        uint32_t index;

        if (cm_hir_def_id_equal(current, target)) {
            reaches = 1;
            break;
        }
        definition = cm_hir_lookup_definition(state->hir, current);
        if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND) {
            continue;
        }
        if (definition->entity.item_id == CM_HIR_ITEM_NONE
            || (size_t)definition->entity.item_id > state->hir->items.len) {
            reaches = 1;
            break;
        }
        item_index = (size_t)definition->entity.item_id - 1u;
        if (seen[item_index]) continue;
        seen[item_index] = 1u;
        item = cm_lower_bound_item(state, current);
        if (item == NULL) continue;
        if (item->kind == CM_HIR_ITEM_TRAIT) {
            for (index = 0u; index < item->data.trait_item.supertrait_count;
                 ++index) {
                (void)cm_vec_push(&pending,
                    &item->data.trait_item.supertraits[index]
                        .trait_type.definition);
            }
        } else if (item->kind == CM_HIR_ITEM_TRAIT_ALIAS) {
            for (index = 0u;
                 index < item->data.trait_alias_item.bound_count; ++index) {
                const CmHirTraitAliasBound *bound;

                bound = &item->data.trait_alias_item.bounds[index];
                if (bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) {
                    (void)cm_vec_push(&pending,
                        &bound->data.trait_bound.trait_type.definition);
                }
            }
        }
    }
    cm_vec_destroy(&pending);
    cm_free(seen);
    return reaches;
}

static int cm_lower_alias_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    const CmHirItem *parent_impl;
    CmLowerAssociatedTarget declaration;
    CmHirAssociatedTypeBound *bounds;
    CmHirOutlivesPredicate *outlives_predicates;
    CmHirTypeId projection_subject;
    CmSpan span;
    uint32_t index;
    uint32_t lifetime_bound_count;
    uint32_t lifetime_index;
    uint32_t matches;
    uint32_t outlives_start;
    uint32_t trait_bound_count;
    uint32_t trait_index;

    span = cm_lower_span(state, ast_item->span);
    if (record->is_foreign) {
        if (record->parent_kind != CM_LOWER_PARENT_NONE
            || record->is_generated
            || !cm_lower_string_is(state, record->inherited_abi, "C")
            || ast_item->visibility.kind != CM_AST_VIS_INHERITED
            || ast_item->generic_parameter_count != 0u
            || cm_lower_item_has_where_clause(ast_item)
            || ast_item->is_default
            || ast_item->data.value_item.has_value
            || ast_item->data.value_item.type != CM_AST_TYPE_NONE
            || ast_item->data.value_item.initializer != CM_AST_EXPR_NONE
            || ast_item->data.value_item.bound_count != 0u
            || ast_item->data.value_item.is_mutable) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "foreign type is not a source-written targetless extern C "
                "declaration");
            return 0;
        }
        hir_item->kind = CM_HIR_ITEM_EXTERN_TYPE;
        return 1;
    }
    hir_item->kind = CM_HIR_ITEM_TYPE_ALIAS;
    if (ast_item->data.value_item.bound_count != 0u
        && ast_item->data.value_item.bounds == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "associated-type bound count has no storage");
        return 0;
    }
    if (record->parent_kind == CM_LOWER_PARENT_TRAIT) {
        if (ast_item->data.value_item.has_value
            || ast_item->data.value_item.type != CM_AST_TYPE_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "associated type defaults are not supported; trait "
                "declarations must be targetless");
            return 0;
        }
        trait_bound_count = 0u;
        lifetime_bound_count = 0u;
        for (index = 0u; index < ast_item->data.value_item.bound_count;
             ++index) {
            const CmAstAssociatedTypeBound *bound;

            bound = &ast_item->data.value_item.bounds[index];
            if ((unsigned int)bound->kind
                    > (unsigned int)CM_AST_ASSOC_BOUND_LIFETIME) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, bound->span), ast_item_id,
                    bound->trait_type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "associated-type bound is malformed");
                return 0;
            }
            if (bound->kind == CM_AST_ASSOC_BOUND_LIFETIME) {
                lifetime_bound_count += 1u;
            } else {
                trait_bound_count += 1u;
            }
        }
        if (lifetime_bound_count
                > UINT32_MAX - hir_item->outlives_predicate_count) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_ID_EXHAUSTED,
                "associated-type outlives predicate count overflow");
            return 0;
        }
        bounds = NULL;
        if (trait_bound_count != 0u) {
            bounds = (CmHirAssociatedTypeBound *)cm_alloc_zeroed(
                (size_t)trait_bound_count,
                sizeof(CmHirAssociatedTypeBound));
        }
        outlives_start = hir_item->outlives_predicate_count;
        outlives_predicates = hir_item->outlives_predicates;
        if (lifetime_bound_count != 0u) {
            outlives_predicates =
                (CmHirOutlivesPredicate *)cm_alloc_zeroed(
                    (size_t)(outlives_start + lifetime_bound_count),
                    sizeof(*outlives_predicates));
            if (outlives_start != 0u) {
                memcpy(outlives_predicates, hir_item->outlives_predicates,
                    (size_t)outlives_start
                        * sizeof(*outlives_predicates));
            }
            cm_free(hir_item->outlives_predicates);
            hir_item->outlives_predicates = outlives_predicates;
        }
        projection_subject = CM_HIR_TYPE_NONE;
        trait_index = 0u;
        lifetime_index = 0u;
        for (index = 0u;
             index < ast_item->data.value_item.bound_count && !state->failed;
             ++index) {
            const CmAstAssociatedTypeBound *ast_bound;
            uint32_t prior;

            ast_bound = &ast_item->data.value_item.bounds[index];
            if (ast_bound->kind == CM_AST_ASSOC_BOUND_LIFETIME) {
                if (projection_subject == CM_HIR_TYPE_NONE) {
                    projection_subject =
                        cm_lower_associated_bound_projection_subject(state,
                            ast_item_id, record, span);
                    if (state->failed) break;
                }
                if (!cm_lower_associated_type_lifetime_bound(state,
                        ast_item_id, ast_bound, record, projection_subject,
                        &outlives_predicates[outlives_start
                            + lifetime_index])) {
                    break;
                }
                lifetime_index += 1u;
                hir_item->outlives_predicate_count =
                    outlives_start + lifetime_index;
                continue;
            }
            if (!cm_lower_associated_type_bound(state, ast_item_id,
                    ast_bound, record, &projection_subject,
                    &bounds[trait_index])) {
                break;
            }
            for (prior = 0u; prior < trait_index; ++prior) {
                if (cm_hir_def_id_equal(bounds[prior].trait_type.definition,
                        bounds[trait_index].trait_type.definition)) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                        bounds[trait_index].span, ast_item_id,
                        ast_bound->trait_type,
                        CM_AST_PATH_NONE, CM_HIR_OK,
                        "duplicate associated type bound targets the same "
                        "trait");
                    break;
                }
            }
            trait_index += 1u;
        }
        if (state->failed) {
            if (bounds != NULL) {
                for (index = 0u; index < trait_bound_count; ++index) {
                    cm_free(bounds[index].trait_type.arguments);
                    cm_free(bounds[index].equalities);
                }
            }
            cm_free(bounds);
            return 0;
        }
        hir_item->data.type_alias_item.target = CM_HIR_TYPE_NONE;
        hir_item->data.type_alias_item.trait_item_definition =
            cm_hir_def_id_none();
        hir_item->data.type_alias_item.bounds = bounds;
        hir_item->data.type_alias_item.bound_count =
            trait_bound_count;
        for (index = 1u; index < hir_item->outlives_predicate_count;
             ++index) {
            CmHirOutlivesPredicate current;
            uint32_t position;

            current = hir_item->outlives_predicates[index];
            position = index;
            while (position != 0u
                && (hir_item->outlives_predicates[position - 1u]
                            .span.source > current.span.source
                    || (hir_item->outlives_predicates[position - 1u]
                                .span.source == current.span.source
                        && hir_item->outlives_predicates[position - 1u]
                                .span.start > current.span.start))) {
                hir_item->outlives_predicates[position] =
                    hir_item->outlives_predicates[position - 1u];
                position -= 1u;
            }
            hir_item->outlives_predicates[position] = current;
        }
        return 1;
    }
    if (ast_item->data.value_item.bound_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            record->parent_kind == CM_LOWER_PARENT_IMPL
                ? "impl associated type definitions cannot declare bounds"
                : "bounds on free type aliases are not supported");
        return 0;
    }
    if (!ast_item->data.value_item.has_value
        || ast_item->data.value_item.type == CM_AST_TYPE_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            record->parent_kind == CM_LOWER_PARENT_IMPL
                ? "impl associated type definition has no target type"
                : "type alias has no structural target type");
        return 0;
    }
    if (record->parent_kind == CM_LOWER_PARENT_IMPL) {
        parent_impl = cm_lower_bound_item(state, record->parent_definition);
        if (parent_impl == NULL || parent_impl->kind != CM_HIR_ITEM_IMPL
            || parent_impl->data.impl_item.has_trait != 1) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "impl associated type has no bound trait impl parent");
            return 0;
        }
        cm_lower_find_associated_type(state,
            parent_impl->data.impl_item.trait_type.definition,
            ast_item->name, &declaration, &matches);
        if (matches == 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_UNRESOLVED_PATH, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait has no associated type with this name");
            return 0;
        }
        if (matches != 1u) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "trait associated-type identity is ambiguous");
            return 0;
        }
        if (declaration.generic_parameter_count
                != ast_item->generic_parameter_count) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "impl associated type generic arity differs from trait "
                "declaration");
            return 0;
        }
        hir_item->data.type_alias_item.trait_item_definition =
            declaration.definition;
    } else {
        hir_item->data.type_alias_item.trait_item_definition =
            cm_hir_def_id_none();
    }
    hir_item->data.type_alias_item.target = cm_lower_type(state,
        ast_item->data.value_item.type, record->owner_module,
        record->definition);
    return !state->failed;
}

static void cm_lower_free_alias_temporary(CmHirItem *item)
{
    uint32_t index;

    if (item->kind != CM_HIR_ITEM_TYPE_ALIAS) return;
    for (index = 0u; index < item->data.type_alias_item.bound_count;
         ++index) {
        cm_free(item->data.type_alias_item.bounds[index].trait_type.arguments);
        cm_free(item->data.type_alias_item.bounds[index].equalities);
    }
    cm_free(item->data.type_alias_item.bounds);
}

static void cm_lower_free_item_predicates(CmHirItem *item)
{
    uint32_t index;

    for (index = 0u; index < item->predicate_count; ++index) {
        cm_free(item->predicates[index].trait_type.arguments);
        cm_free(item->predicates[index].equalities);
        cm_free(item->predicates[index].binder.lifetimes);
    }
    cm_free(item->predicates);
    cm_free(item->outlives_predicates);
    for (index = 0u; index < item->predicate_scope_count; ++index) {
        cm_free(item->predicate_scopes[index].binder.lifetimes);
    }
    cm_free(item->predicate_scopes);
}

static int cm_lower_impl_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    CmLowerTraitTarget trait_target;
    const CmHirItem *trait_item;
    CmHirSafety safety;
    CmSpan span;

    span = cm_lower_span(state, ast_item->span);
    if (ast_item->data.impl_item.is_negative != 0
        && ast_item->data.impl_item.is_negative != 1) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "impl has an invalid polarity flag");
        return 0;
    }
    if (ast_item->data.impl_item.is_const != 0
        && ast_item->data.impl_item.is_const != 1) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "impl has an invalid const flag");
        return 0;
    }
    if (ast_item->visibility.kind != CM_AST_VIS_INHERITED) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "impl blocks cannot have explicit visibility");
        return 0;
    }
    /*
     * Match the mrustc oracle: `impl const` is accepted by the parser and
     * deliberately erased before HIR.  The ordinary inherent/trait,
     * polarity, and safety checks below remain authoritative.
     */
    if (ast_item->data.impl_item.self_type == CM_AST_TYPE_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "impl has no self type");
        return 0;
    }
    if (ast_item->data.impl_item.trait_type == CM_AST_TYPE_NONE) {
        if (ast_item->data.impl_item.is_negative) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "negative impl must name an authenticated auto trait");
            return 0;
        }
        if (state->graph == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "inherent impls require source-backed graph lowering");
            return 0;
        }
        if (ast_item->data.impl_item.is_unsafe) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "inherent impls cannot be unsafe");
            return 0;
        }
        hir_item->kind = CM_HIR_ITEM_IMPL;
        hir_item->data.impl_item.has_trait = 0;
        hir_item->data.impl_item.is_negative = 0;
        hir_item->data.impl_item.safety = CM_HIR_SAFE;
        hir_item->data.impl_item.self_type = cm_lower_type(state,
            ast_item->data.impl_item.self_type, record->owner_module,
            record->definition);
        return !state->failed;
    }
    hir_item->kind = CM_HIR_ITEM_IMPL;
    hir_item->data.impl_item.has_trait = 1;
    hir_item->data.impl_item.is_negative =
        ast_item->data.impl_item.is_negative;
    hir_item->data.impl_item.safety = ast_item->data.impl_item.is_unsafe
        ? CM_HIR_UNSAFE : CM_HIR_SAFE;
    hir_item->data.impl_item.self_type = cm_lower_type(state,
        ast_item->data.impl_item.self_type, record->owner_module,
        record->definition);
    if (state->failed) return 0;
    if (!cm_lower_trait_reference(state, ast_item_id,
            ast_item->data.impl_item.trait_type, record->owner_module,
            record->definition, hir_item->data.impl_item.self_type,
            &hir_item->data.impl_item.trait_type, &trait_target,
            0, 0, 0, 0)) {
        return 0;
    }
    trait_item = trait_target.item;
    if (trait_item == NULL)
        trait_item = cm_lower_bound_item(state, trait_target.definition);
    safety = trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
        ? trait_item->data.trait_item.safety : CM_HIR_SAFE;
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "implemented trait header is not bound before the impl");
        return 0;
    }
    if (ast_item->data.impl_item.is_negative
        && (ast_item->data.impl_item.is_unsafe
            || ast_item->data.impl_item.item_count != 0u
            || ast_item->data.impl_item.items != NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "negative impl must be safe and have no associated items");
        return 0;
    }
    /*
     * Negative impls are an unstable language feature, but the compiler's
     * authenticated HIR admits them for ordinary traits as well as auto
     * traits (for example, core's `impl !Clone for &mut T`).  Keep the
     * structural safety checks above authoritative: the target must resolve
     * to an authenticated trait, the impl must be safe, and it must have no
     * associated items.  Do not infer polarity from the trait's `is_auto`
     * bit; that bit only controls auto-trait solving and dyn-trait markers.
     */
    if (!ast_item->data.impl_item.is_negative
        && safety != hir_item->data.impl_item.safety) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "impl safety does not match trait safety");
        return 0;
    }
    return !state->failed;
}

static void cm_lower_free_impl_temporary(CmHirItem *item)
{
    if (item->kind != CM_HIR_ITEM_IMPL
        || !item->data.impl_item.has_trait) return;
    cm_free(item->data.impl_item.trait_type.arguments);
}

static int cm_lower_trait_alias_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    CmHirTraitAliasBound *bounds;
    uint32_t index;

    bounds = (CmHirTraitAliasBound *)cm_alloc_zeroed(
        (size_t)ast_item->data.trait_item.structured_alias_bound_count,
        sizeof(*bounds));
    for (index = 0u;
         index < ast_item->data.trait_item.structured_alias_bound_count
            && !state->failed;
         ++index) {
        const CmAstSupertrait *ast_bound;
        CmHirTraitAliasBound *bound;

        ast_bound = &ast_item->data.trait_item.structured_alias_bounds[index];
        bound = &bounds[index];
        bound->span = cm_lower_span(state, ast_bound->span);
        if ((unsigned int)ast_bound->kind
                > (unsigned int)CM_AST_SUPERTRAIT_LIFETIME
            || (unsigned int)ast_bound->modifier
                > (unsigned int)CM_AST_SUPERTRAIT_CONDITIONALLY_CONST
            || ast_bound->span.start > ast_bound->span.end) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, bound->span,
                ast_item_id, ast_bound->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait-alias bound is malformed");
            break;
        }
        if (ast_bound->kind == CM_AST_SUPERTRAIT_LIFETIME) {
            const CmLowerGenericRecord *lifetime_generic;
            int is_static;

            bound->kind = CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME;
            if (ast_bound->modifier != CM_AST_SUPERTRAIT_REQUIRED
                || ast_bound->type != CM_AST_TYPE_NONE
                || ast_bound->lifetime == CM_INTERN_ID_NONE
                || cm_ast_get_string(state->ast,
                    ast_bound->lifetime) == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, bound->span,
                    ast_item_id, ast_bound->type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "trait-alias lifetime bound is malformed");
                break;
            }
            is_static = cm_lower_string_is(state, ast_bound->lifetime,
                "'static");
            lifetime_generic = is_static ? NULL : cm_lower_find_generic(
                state, record->definition, ast_bound->lifetime);
            if (!is_static && (lifetime_generic == NULL
                    || lifetime_generic->kind
                        != CM_HIR_GENERIC_LIFETIME)) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    bound->span, ast_item_id, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "trait-alias lifetime bound must be 'static or an "
                    "alias-owned lifetime parameter");
                break;
            }
            if (!cm_lower_lifetime(state, ast_bound->lifetime,
                    record->definition, bound->span,
                    &bound->data.lifetime)) {
                break;
            }
            continue;
        }
        if (ast_bound->type == CM_AST_TYPE_NONE
            || ast_bound->lifetime != CM_INTERN_ID_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, bound->span,
                ast_item_id, ast_bound->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait-alias trait bound is malformed");
            break;
        }
        bound->kind = CM_HIR_TRAIT_ALIAS_BOUND_TRAIT;
        bound->data.trait_bound.span = bound->span;
        if (ast_bound->modifier == CM_AST_SUPERTRAIT_REQUIRED) {
            bound->data.trait_bound.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
        } else if (ast_bound->modifier
                == CM_AST_SUPERTRAIT_CONDITIONALLY_CONST) {
            bound->data.trait_bound.modifier =
                CM_HIR_SUPERTRAIT_CONST_IF_CONST;
        } else {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, bound->span,
                ast_item_id, ast_bound->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait-alias bound has an invalid modifier");
            break;
        }
        {
            CmLowerTraitTarget target_trait;

            if (!cm_lower_trait_reference(state, ast_item_id,
                    ast_bound->type, record->owner_module,
                    record->definition, CM_HIR_TYPE_NONE,
                    &bound->data.trait_bound.trait_type, &target_trait,
                    1, 1, 0, 1)
                || !cm_lower_predicate_equalities(state, ast_item_id,
                    ast_bound->type, &target_trait, record->owner_module,
                    record->definition, 0,
                    &bound->data.trait_bound.equalities,
                    &bound->data.trait_bound.equality_count)) {
                break;
            }
            if (cm_hir_def_id_equal(target_trait.definition,
                    record->definition)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT, bound->span,
                    ast_item_id, ast_bound->type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "trait alias cannot name itself as a direct bound");
                break;
            }
        }
    }
    if (state->failed) {
        for (index = 0u;
             index < ast_item->data.trait_item.structured_alias_bound_count;
             ++index) {
            if (bounds[index].kind == CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) {
                cm_free(bounds[index].data.trait_bound.trait_type.arguments);
                cm_free(bounds[index].data.trait_bound.equalities);
            }
        }
        cm_free(bounds);
        return 0;
    }
    hir_item->kind = CM_HIR_ITEM_TRAIT_ALIAS;
    hir_item->data.trait_alias_item.bounds = bounds;
    hir_item->data.trait_alias_item.bound_count =
        ast_item->data.trait_item.structured_alias_bound_count;
    return 1;
}

static int cm_lower_trait_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    CmHirSupertrait *supertraits;
    CmHirOutlivesPredicate *outlives_predicates;
    CmSpan span;
    uint32_t index;
    uint32_t allocated_supertrait_count;
    uint32_t supertrait_count;
    uint32_t outlives_start;
    uint32_t lifetime_supertrait_count;

    span = cm_lower_span(state, ast_item->span);
    if (ast_item->data.trait_item.is_alias != 0
        && ast_item->data.trait_item.is_alias != 1) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait has an invalid alias flag");
        return 0;
    }
    if (ast_item->data.trait_item.is_auto != 0
        && ast_item->data.trait_item.is_auto != 1) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait has an invalid auto flag");
        return 0;
    }
    if (ast_item->data.trait_item.is_auto
        && ast_item->data.trait_item.is_alias) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait cannot be both auto and an alias");
        return 0;
    }
    if ((ast_item->data.trait_item.alias_bounds == CM_INTERN_ID_NONE)
            != (ast_item->data.trait_item.structured_alias_bound_count == 0u)
        || (ast_item->data.trait_item.structured_alias_bound_count != 0u
            && ast_item->data.trait_item.structured_alias_bounds == NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait alias text and structural bounds disagree");
        return 0;
    }
    if (!ast_item->data.trait_item.is_alias
        && ast_item->data.trait_item.structured_alias_bound_count != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "ordinary trait carries trait-alias bounds");
        return 0;
    }
    if (ast_item->data.trait_item.is_alias) {
        if (ast_item->data.trait_item.is_unsafe
            || ast_item->data.trait_item.item_count != 0u
            || ast_item->data.trait_item.items != NULL
            || ast_item->data.trait_item.supertraits != CM_INTERN_ID_NONE
            || ast_item->data.trait_item.structured_supertrait_count != 0u
            || ast_item->data.trait_item.structured_supertraits != NULL
            || ast_item->data.trait_item.structured_alias_bound_count == 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait alias has an invalid declaration shape");
            return 0;
        }
        return cm_lower_trait_alias_item(state, ast_item_id, ast_item,
            record, hir_item);
    }
    if (ast_item->data.trait_item.is_auto
        && (ast_item->generic_parameter_count != 0u
            || ast_item->data.trait_item.structured_supertrait_count != 0u
            || ast_item->where_predicate_count != 0u
            || ast_item->data.trait_item.item_count != 0u
            || ast_item->data.trait_item.items != NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "auto trait must not have generic parameters, bounds, or items");
        return 0;
    }
    if ((ast_item->data.trait_item.supertraits == CM_INTERN_ID_NONE)
            != (ast_item->data.trait_item.structured_supertrait_count == 0u)
        || (ast_item->data.trait_item.structured_supertrait_count != 0u
            && ast_item->data.trait_item.structured_supertraits == NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait supertrait text and structural bounds disagree");
        return 0;
    }
    hir_item->kind = CM_HIR_ITEM_TRAIT;
    supertrait_count = 0u;
    lifetime_supertrait_count = 0u;
    for (index = 0u;
         index < ast_item->data.trait_item.structured_supertrait_count;
         ++index) {
        if (ast_item->data.trait_item.structured_supertraits[index].kind
                == CM_AST_SUPERTRAIT_LIFETIME) {
            lifetime_supertrait_count += 1u;
        } else {
            supertrait_count += 1u;
        }
    }
    if (lifetime_supertrait_count
            > UINT32_MAX - hir_item->outlives_predicate_count) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
            "trait outlives predicate count overflow");
        return 0;
    }
    allocated_supertrait_count = supertrait_count;
    supertraits = NULL;
    if (supertrait_count != 0u) {
        supertraits = (CmHirSupertrait *)cm_alloc_zeroed(
            (size_t)supertrait_count,
            sizeof(CmHirSupertrait));
    }
    outlives_start = hir_item->outlives_predicate_count;
    outlives_predicates = hir_item->outlives_predicates;
    if (lifetime_supertrait_count != 0u) {
        outlives_predicates = (CmHirOutlivesPredicate *)cm_alloc_zeroed(
            (size_t)(outlives_start + lifetime_supertrait_count),
            sizeof(CmHirOutlivesPredicate));
        if (outlives_start != 0u) {
            memcpy(outlives_predicates, hir_item->outlives_predicates,
                (size_t)outlives_start * sizeof(CmHirOutlivesPredicate));
        }
        cm_free(hir_item->outlives_predicates);
        hir_item->outlives_predicates = outlives_predicates;
    }
    supertrait_count = 0u;
    lifetime_supertrait_count = 0u;
    for (index = 0u;
         index < ast_item->data.trait_item.structured_supertrait_count
            && !state->failed;
         ++index) {
        const CmAstSupertrait *ast_supertrait;
        const CmAstType *ast_supertrait_type;
        CmLowerTraitTarget target_trait;
        CmAstPathId ast_supertrait_path;

        ast_supertrait =
            &ast_item->data.trait_item.structured_supertraits[index];
        if ((unsigned int)ast_supertrait->kind
                > (unsigned int)CM_AST_SUPERTRAIT_LIFETIME
            || (unsigned int)ast_supertrait->modifier
                > (unsigned int)CM_AST_SUPERTRAIT_CONDITIONALLY_CONST
            || ast_supertrait->span.start > ast_supertrait->span.end) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, ast_supertrait->span), ast_item_id,
                ast_supertrait->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "supertrait bound is malformed");
            break;
        }
        if (ast_supertrait->kind == CM_AST_SUPERTRAIT_LIFETIME) {
            CmHirOutlivesPredicate *predicate;
            const CmLowerGenericRecord *lifetime_generic;
            int is_static;

            if (ast_supertrait->modifier != CM_AST_SUPERTRAIT_REQUIRED
                || ast_supertrait->type != CM_AST_TYPE_NONE
                || ast_supertrait->lifetime == CM_INTERN_ID_NONE
                || cm_ast_get_string(state->ast,
                    ast_supertrait->lifetime) == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, ast_supertrait->span), ast_item_id,
                    ast_supertrait->type, CM_AST_PATH_NONE, CM_HIR_OK,
                    "lifetime supertrait bound is malformed");
                break;
            }
            is_static = cm_lower_string_is(state,
                ast_supertrait->lifetime, "'static");
            lifetime_generic = is_static ? NULL : cm_lower_find_generic(
                state, record->definition, ast_supertrait->lifetime);
            if (!is_static && (lifetime_generic == NULL
                    || lifetime_generic->kind
                        != CM_HIR_GENERIC_LIFETIME)) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC,
                    cm_lower_span(state, ast_supertrait->span), ast_item_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "lifetime supertrait must be 'static or a trait-owned "
                    "lifetime parameter");
                break;
            }
            predicate = &outlives_predicates[
                outlives_start + lifetime_supertrait_count];
            predicate->subject_kind = CM_HIR_OUTLIVES_TYPE;
            predicate->span = cm_lower_span(state, ast_supertrait->span);
            predicate->subject.type = cm_lower_self_type(state,
                CM_AST_TYPE_NONE, predicate->span, record->definition);
            if (state->failed
                || !cm_lower_lifetime(state, ast_supertrait->lifetime,
                    record->definition, predicate->span,
                    &predicate->bound)) {
                break;
            }
            lifetime_supertrait_count += 1u;
            hir_item->outlives_predicate_count =
                outlives_start + lifetime_supertrait_count;
            continue;
        }
        if (ast_supertrait->type == CM_AST_TYPE_NONE
            || ast_supertrait->lifetime != CM_INTERN_ID_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, ast_supertrait->span), ast_item_id,
                ast_supertrait->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait supertrait bound is malformed");
            break;
        }
        ast_supertrait_type = cm_ast_get_type(state->ast,
            ast_supertrait->type);
        ast_supertrait_path = ast_supertrait_type == NULL
            ? CM_AST_PATH_NONE : ast_supertrait_type->path;
        if (ast_supertrait->modifier == CM_AST_SUPERTRAIT_REQUIRED) {
            supertraits[supertrait_count].modifier =
                CM_HIR_SUPERTRAIT_REQUIRED;
        } else if (ast_supertrait->modifier
                == CM_AST_SUPERTRAIT_CONDITIONALLY_CONST) {
            supertraits[supertrait_count].modifier =
                CM_HIR_SUPERTRAIT_CONST_IF_CONST;
        } else {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                cm_lower_span(state, ast_supertrait->span), ast_item_id,
                ast_supertrait->type, CM_AST_PATH_NONE, CM_HIR_OK,
                "supertrait has an invalid modifier");
            break;
        }
        supertraits[supertrait_count].span =
            cm_lower_span(state, ast_supertrait->span);
        if (!cm_lower_trait_reference(state, ast_item_id,
                ast_supertrait->type, record->owner_module,
                record->definition, CM_HIR_TYPE_NONE,
                &supertraits[supertrait_count].trait_type, &target_trait,
                1, 1, 0, 1)) {
            break;
        }
        if (!cm_lower_predicate_equalities(state, ast_item_id,
                ast_supertrait->type, &target_trait,
                record->owner_module, record->definition, 0,
                &supertraits[supertrait_count].equalities,
                &supertraits[supertrait_count].equality_count)) {
            break;
        }
        if (cm_hir_def_id_equal(
                supertraits[supertrait_count].trait_type.definition,
                record->definition)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                supertraits[supertrait_count].span, ast_item_id,
                ast_supertrait->type, ast_supertrait_path, CM_HIR_OK,
                "trait cannot name itself as a direct supertrait");
            break;
        }
        if (cm_lower_supertrait_reaches_definition(state,
                supertraits[supertrait_count].trait_type.definition,
                record->definition)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                supertraits[supertrait_count].span, ast_item_id,
                ast_supertrait->type, ast_supertrait_path, CM_HIR_OK,
                "trait supertrait graph contains a cycle");
            break;
        }
        {
            uint32_t prior;

            for (prior = 0u; prior < supertrait_count; ++prior) {
                if (cm_hir_def_id_equal(
                        supertraits[prior].trait_type.definition,
                        supertraits[supertrait_count]
                            .trait_type.definition)) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                        supertraits[supertrait_count].span, ast_item_id,
                        ast_supertrait->type, ast_supertrait_path, CM_HIR_OK,
                        "trait repeats the same direct supertrait");
                    break;
                }
            }
            if (state->failed) break;
        }
        (void)target_trait;
        supertrait_count += 1u;
    }
    if (state->failed) {
        for (index = 0u; index < allocated_supertrait_count; ++index) {
            cm_free(supertraits[index].trait_type.arguments);
            cm_free(supertraits[index].equalities);
        }
        cm_free(supertraits);
        return 0;
    }
    hir_item->data.trait_item.safety = ast_item->data.trait_item.is_unsafe
        ? CM_HIR_UNSAFE : CM_HIR_SAFE;
    hir_item->data.trait_item.is_auto =
        ast_item->data.trait_item.is_auto;
    hir_item->data.trait_item.supertraits = supertraits;
    hir_item->data.trait_item.supertrait_count = supertrait_count;
    return 1;
}

static void cm_lower_free_trait_temporary(CmHirItem *item)
{
    if (item->kind == CM_HIR_ITEM_TRAIT) {
        uint32_t index;

        for (index = 0u;
             index < item->data.trait_item.supertrait_count; ++index) {
            cm_free(item->data.trait_item.supertraits[index]
                .trait_type.arguments);
            cm_free(item->data.trait_item.supertraits[index].equalities);
        }
        cm_free(item->data.trait_item.supertraits);
    } else if (item->kind == CM_HIR_ITEM_TRAIT_ALIAS) {
        uint32_t index;

        for (index = 0u;
             index < item->data.trait_alias_item.bound_count; ++index) {
            CmHirTraitAliasBound *bound;

            bound = &item->data.trait_alias_item.bounds[index];
            if (bound->kind != CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) continue;
            cm_free(bound->data.trait_bound.trait_type.arguments);
            cm_free(bound->data.trait_bound.equalities);
        }
        cm_free(item->data.trait_alias_item.bounds);
    }
}

static int cm_lower_value_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    const CmAstExpr *initializer;
    const CmHirItem *parent_impl;
    const CmHirItem *trait_const;
    CmHirDefId trait_item_definition;
    CmHirTypeId type;
    CmSpan span;
    uint32_t matches;

    span = cm_lower_span(state, ast_item->span);
    trait_item_definition = cm_hir_def_id_none();
    if (record->parent_kind == CM_LOWER_PARENT_TRAIT) {
        if (ast_item->data.value_item.type == CM_AST_TYPE_NONE
            || (ast_item->data.value_item.has_value
                != (ast_item->data.value_item.initializer
                    != CM_AST_EXPR_NONE))
            || ast_item->data.value_item.is_mutable) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "trait associated consts must be immutable, explicitly "
                "typed declarations with a consistent optional default");
            return 0;
        }
        hir_item->kind = CM_HIR_ITEM_CONST;
        hir_item->data.value_item.type = cm_lower_type(state,
            ast_item->data.value_item.type, record->owner_module,
            record->parent_definition);
        hir_item->data.value_item.mutability = CM_HIR_IMMUTABLE;
        hir_item->data.value_item.body = cm_lower_body(state,
            record->definition, hir_item->data.value_item.type,
            ast_item->data.value_item.initializer, NULL, 0u, 0u,
            span, ast_item_id);
        hir_item->data.value_item.trait_item_definition =
            trait_item_definition;
        return !state->failed;
    }
    if (ast_item->data.value_item.type == CM_AST_TYPE_NONE
        || !ast_item->data.value_item.has_value
        || ast_item->data.value_item.initializer == CM_AST_EXPR_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "const/static declaration lacks an explicit type or initializer");
        return 0;
    }
    initializer = cm_ast_get_expr(state->ast,
        ast_item->data.value_item.initializer);
    if (initializer == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "const/static initializer has an invalid expression ID");
        return 0;
    }
    (void)initializer;
    if (record->parent_kind == CM_LOWER_PARENT_IMPL) {
        parent_impl = cm_lower_bound_item(state, record->parent_definition);
        if (parent_impl == NULL || parent_impl->kind != CM_HIR_ITEM_IMPL
            || parent_impl->data.impl_item.is_negative) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
                ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "associated const has no bound positive impl parent");
            return 0;
        }
        if (parent_impl->data.impl_item.has_trait) {
            trait_const = cm_lower_find_associated_const(state,
                parent_impl->data.impl_item.trait_type.definition,
                ast_item->name, &matches);
            if (matches != 1u || trait_const == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, span,
                    ast_item_id, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK, matches == 0u
                        ? "impl const has no matching trait const declaration"
                        : "impl const identity is ambiguous or unbound");
                return 0;
            }
            trait_item_definition = trait_const->definition;
        }
    }
    type = cm_lower_type(state, ast_item->data.value_item.type,
        record->owner_module,
        record->parent_kind == CM_LOWER_PARENT_IMPL
            ? record->parent_definition : record->definition);
    if (state->failed) {
        return 0;
    }
    hir_item->kind = ast_item->kind == CM_AST_ITEM_CONST
        ? CM_HIR_ITEM_CONST : CM_HIR_ITEM_STATIC;
    hir_item->data.value_item.type = type;
    hir_item->data.value_item.mutability =
        ast_item->data.value_item.is_mutable
            ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;
    hir_item->data.value_item.trait_item_definition =
        trait_item_definition;
    hir_item->data.value_item.body = cm_lower_body(state,
        record->definition, type, ast_item->data.value_item.initializer,
        NULL, 0u, 0u, span, ast_item_id);
    return !state->failed;
}

static int cm_lower_module_item(CmLowerState *state,
    CmAstItemId ast_item_id, const CmAstItem *ast_item,
    const CmLowerItemRecord *record, CmHirItem *hir_item)
{
    CmSpan span;

    (void)ast_item;
    span = cm_lower_span(state, ast_item->span);
    if (record->nested_module == CM_HIR_MODULE_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "inline module was not reserved");
        return 0;
    }
    hir_item->kind = CM_HIR_ITEM_MODULE;
    hir_item->data.module_item.module_id = record->nested_module;
    return 1;
}

static int cm_lower_one_record_internal(CmLowerState *state,
    const CmLowerItemRecord *record, int prebind_associated_type)
{
    const CmAstItem *ast_item;
    CmHirItem hir_item;
    CmHirItemId hir_item_id;
    CmHirContextMark record_mark;
    CmHirStatus status;
    int lowered;
    int record_marked;
    CmAstItemId ast_item_id;

    state->ast = record->ast;
    state->source = record->source;
    state->graph_module = record->graph_module;
    state->generated_span = record->effective_span;
    state->use_generated_span = record->is_generated;
    ast_item_id = record->ast_id;
    ast_item = cm_ast_get_item(state->ast, ast_item_id);
    if (ast_item == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            cm_lower_span(state, (CmAstSpan){ 0u, 0u }), ast_item_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "lowering pass has no reserved record for item");
        return 0;
    }
    record_marked = 0;
    if (!prebind_associated_type) {
        status = cm_hir_context_mark(state->hir, &record_mark);
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state, record->effective_span, ast_item_id,
                status, "cannot mark item lowering record transaction");
            return 0;
        }
        record_marked = 1;
    }
    memset(&hir_item, 0, sizeof(hir_item));
    if (!cm_lower_item_header(state, ast_item_id, ast_item, record,
            &hir_item)) {
        cm_free(hir_item.attributes);
        if (record_marked) {
            (void)cm_hir_context_rewind(state->hir, &record_mark);
        }
        return 0;
    }
    if (record->graph_module != CM_MODULE_NONE
        && ast_item->kind == CM_AST_ITEM_MODULE) {
        /*
         * The graph declaration denotes the child module definition created
         * before reservation.  CmHirItem definitions are a disjoint kind, so
         * adding a second module item would invent a duplicate semantic DefId.
        */
        status = cm_hir_set_module_outer_attributes(state->hir,
            record->nested_module, hir_item.attributes,
            hir_item.attribute_count);
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state, record->effective_span,
                ast_item_id, status,
                "cannot store effective module declaration attributes");
            cm_free(hir_item.attributes);
            if (record_marked) {
                (void)cm_hir_context_rewind(state->hir, &record_mark);
            }
            return 0;
        }
        state->result.lowered_item_count += 1u;
        cm_free(hir_item.attributes);
        if (record_marked) {
            (void)cm_hir_context_commit(state->hir, &record_mark);
        }
        return 1;
    }
    state->active_item = &hir_item;
    lowered = 0;
    switch (ast_item->kind) {
    case CM_AST_ITEM_FUNCTION:
        lowered = cm_lower_function_item(state, ast_item_id, ast_item,
            record, &hir_item);
        break;
    case CM_AST_ITEM_STRUCT:
    case CM_AST_ITEM_UNION:
        lowered = cm_lower_aggregate_item(state, ast_item_id, ast_item, record,
            &hir_item);
        break;
    case CM_AST_ITEM_ENUM:
        lowered = cm_lower_enum_item(state, ast_item_id, ast_item, record,
            &hir_item);
        break;
    case CM_AST_ITEM_TYPE_ALIAS:
        lowered = cm_lower_alias_item(state, ast_item_id, ast_item, record,
            &hir_item);
        break;
    case CM_AST_ITEM_CONST:
    case CM_AST_ITEM_STATIC:
        lowered = cm_lower_value_item(state, ast_item_id, ast_item, record,
            &hir_item);
        break;
    case CM_AST_ITEM_MODULE:
        lowered = cm_lower_module_item(state, ast_item_id, ast_item, record,
            &hir_item);
        break;
    case CM_AST_ITEM_TRAIT:
        lowered = cm_lower_trait_item(state, ast_item_id, ast_item, record,
            &hir_item);
        break;
    case CM_AST_ITEM_IMPL:
        lowered = cm_lower_impl_item(state, ast_item_id, ast_item, record,
            &hir_item);
        break;
    default:
        break;
    }
    state->active_item = NULL;
    if (!lowered) {
        cm_lower_free_enum_temporary(&hir_item);
        cm_lower_free_trait_temporary(&hir_item);
        cm_lower_free_alias_temporary(&hir_item);
        cm_lower_free_impl_temporary(&hir_item);
        cm_lower_free_item_predicates(&hir_item);
        cm_free(hir_item.attributes);
        if (hir_item.kind == CM_HIR_ITEM_FUNCTION) {
            cm_free(hir_item.data.function_item.signature.parameters);
        } else if (hir_item.kind == CM_HIR_ITEM_STRUCT
                || hir_item.kind == CM_HIR_ITEM_UNION) {
            cm_free(hir_item.data.aggregate_item.fields);
        }
        if (record_marked) {
            (void)cm_hir_context_rewind(state->hir, &record_mark);
        }
        return 0;
    }
    status = prebind_associated_type
        ? cm_hir_prebind_trait_associated_type_declaration(state->hir,
            &hir_item, &hir_item_id)
        : cm_hir_add_item(state->hir, &hir_item, &hir_item_id);
    if (status != CM_HIR_OK) {
        cm_lower_fail_hir(state, hir_item.span, ast_item_id, status,
            "cannot bind lowered HIR item");
    } else if ((hir_item.kind == CM_HIR_ITEM_STRUCT
            || hir_item.kind == CM_HIR_ITEM_UNION)
        && hir_item.generic_parameter_count == 0u
        && hir_item.data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED) {
        CmHirType self_type;
        CmHirTypeId self_type_id;

        memset(&self_type, 0, sizeof(self_type));
        self_type.kind = CM_HIR_TYPE_ADT_KIND;
        self_type.span = hir_item.span;
        self_type.data.named_type.definition = hir_item.definition;
        status = cm_hir_add_type(state->hir, &self_type, &self_type_id);
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state, hir_item.span, ast_item_id, status,
                "cannot materialize named aggregate self type");
        }
    }
    cm_lower_free_enum_temporary(&hir_item);
    cm_lower_free_trait_temporary(&hir_item);
    cm_lower_free_alias_temporary(&hir_item);
    cm_lower_free_impl_temporary(&hir_item);
    cm_lower_free_item_predicates(&hir_item);
    cm_free(hir_item.attributes);
    if (hir_item.kind == CM_HIR_ITEM_FUNCTION) {
        cm_free(hir_item.data.function_item.signature.parameters);
    } else if (hir_item.kind == CM_HIR_ITEM_STRUCT
        || hir_item.kind == CM_HIR_ITEM_UNION) {
        cm_free(hir_item.data.aggregate_item.fields);
    }
    if (status != CM_HIR_OK) {
        if (record_marked) {
            (void)cm_hir_context_rewind(state->hir, &record_mark);
        }
        return 0;
    }
    if (record_marked) {
        (void)cm_hir_context_commit(state->hir, &record_mark);
    }
    if (!prebind_associated_type) {
        state->result.lowered_item_count += 1u;
    }
    return 1;
}

static int cm_lower_one_record(CmLowerState *state,
    const CmLowerItemRecord *record)
{
    return cm_lower_one_record_internal(state, record, 0);
}

static int cm_lower_prebind_associated_type_record(CmLowerState *state,
    const CmLowerItemRecord *record)
{
    const CmAstItem *ast_item;
    CmHirItem declaration;
    CmHirItemId ignored_item;
    CmHirStatus status;
    CmSpan span;

    state->ast = record->ast;
    state->source = record->source;
    state->graph_module = record->graph_module;
    state->generated_span = record->effective_span;
    state->use_generated_span = record->is_generated;
    ast_item = cm_ast_get_item(record->ast, record->ast_id);
    span = ast_item == NULL ? record->effective_span
        : cm_lower_span(state, ast_item->span);
    if (ast_item == NULL || ast_item->kind != CM_AST_ITEM_TYPE_ALIAS
        || record->parent_kind != CM_LOWER_PARENT_TRAIT
        || record->generic_parameter_count != 0u
        || ast_item->data.value_item.has_value
        || ast_item->data.value_item.type != CM_AST_TYPE_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            span, record->ast_id, CM_AST_TYPE_NONE,
            CM_AST_PATH_NONE, CM_HIR_OK,
            "associated-type prebinding requires a targetless nongeneric "
            "trait declaration");
        return 0;
    }
    memset(&declaration, 0, sizeof(declaration));
    declaration.kind = CM_HIR_ITEM_TYPE_ALIAS;
    declaration.definition = record->definition;
    declaration.owner_module = record->owner_module;
    declaration.parent_definition = record->parent_definition;
    declaration.name = record->hir_name;
    declaration.span = span;
    declaration.visibility.kind = CM_HIR_VIS_PRIVATE;
    declaration.visibility.restriction = cm_hir_def_id_none();
    declaration.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    declaration.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    declaration.data.type_alias_item.trait_item_definition =
        cm_hir_def_id_none();
    status = cm_hir_prebind_trait_associated_type_declaration(state->hir,
        &declaration, &ignored_item);
    if (status != CM_HIR_OK) {
        cm_lower_fail_hir(state, span, record->ast_id,
            status, "cannot prebind associated-type projection identity");
        return 0;
    }
    return 1;
}

static int cm_lower_item_ref_equal(CmResolveItemRef left,
    CmResolveItemRef right)
{
    return left.source == right.source && left.item == right.item;
}

static int cm_lower_dependency_item_ref_empty(
    CmResolveDependencyItemRef reference)
{
    return reference.consumer_graph == NULL
        && reference.consumer_revision == CM_MODULE_GRAPH_REVISION_NONE
        && reference.certificate
            == CM_RESOLVE_DEPENDENCY_CERTIFICATE_NONE
        && reference.dependency == CM_RESOLVE_DEPENDENCY_NONE
        && reference.dependency_revision == CM_MODULE_GRAPH_REVISION_NONE
        && reference.declaration.source == 0u
        && reference.declaration.item == CM_AST_ITEM_NONE;
}

static int cm_lower_dependency_item_ref_structurally_valid(
    CmResolveDependencyItemRef reference)
{
    return reference.consumer_graph != NULL
        && reference.consumer_revision != CM_MODULE_GRAPH_REVISION_NONE
        && reference.certificate
            != CM_RESOLVE_DEPENDENCY_CERTIFICATE_NONE
        && reference.dependency != CM_RESOLVE_DEPENDENCY_NONE
        && reference.dependency_revision != CM_MODULE_GRAPH_REVISION_NONE
        && reference.declaration.source != 0u
        && reference.declaration.item != CM_AST_ITEM_NONE;
}

static int cm_lower_graph_get_effective_item(CmLowerState *state,
    const CmModuleGraph *graph, CmModuleId module, uint32_t index,
    CmResolveEffectiveItem *out_item)
{
    CmResolveViewStatus status;

    status = cm_module_graph_get_effective_item(graph,
        state->graph_revision, module, index, out_item);
    if (status == CM_RESOLVE_VIEW_OK) return 1;
    cm_lower_fail(state,
        status == CM_RESOLVE_VIEW_STALE_REVISION
            ? CM_HIR_LOWER_STALE_GRAPH : CM_HIR_LOWER_INVALID_AST,
        (CmSpan){ 0u, 0u, 0u }, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
        CM_AST_PATH_NONE, CM_HIR_OK,
        "cannot access revision-checked effective graph item: %s",
        cm_resolve_view_status_name(status));
    return 0;
}

static int cm_lower_graph_get_effective_child(CmLowerState *state,
    const CmModuleGraph *graph, CmModuleId module,
    CmResolveEffectiveItemId parent, uint32_t index,
    CmResolveEffectiveItem *out_item)
{
    CmResolveViewStatus status;

    status = cm_module_graph_get_effective_child(graph,
        state->graph_revision, module, parent, index, out_item);
    if (status == CM_RESOLVE_VIEW_OK) return 1;
    cm_lower_fail(state,
        status == CM_RESOLVE_VIEW_STALE_REVISION
            ? CM_HIR_LOWER_STALE_GRAPH : CM_HIR_LOWER_INVALID_AST,
        (CmSpan){ 0u, 0u, 0u }, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
        CM_AST_PATH_NONE, CM_HIR_OK,
        "cannot access revision-checked effective graph child: %s",
        cm_resolve_view_status_name(status));
    return 0;
}

static int cm_lower_ast_attribute_id_in(const CmAstAttributeId *ids,
    size_t count, CmAstAttributeId id)
{
    size_t index;

    if (count != 0u && ids == NULL) return 0;
    for (index = 0u; index < count; ++index) {
        if (ids[index] == id) return 1;
    }
    return 0;
}

static int cm_lower_graph_validate_effective_attributes(
    CmLowerState *state, const CmModuleGraph *graph, CmModuleId module,
    const CmResolveEffectiveItem *item, const CmAstItem *ast_item)
{
    uint32_t index;

    for (index = 0u; index < item->attribute_count; ++index) {
        CmResolveEffectiveAttribute attribute;
        CmResolveViewStatus status;
        const CmAstAttribute *source_attribute;
        size_t metadata_length;

        status = cm_module_graph_get_effective_item_attribute(graph,
            state->graph_revision, module, item->id, index, &attribute);
        if (status != CM_RESOLVE_VIEW_OK) {
            cm_lower_fail(state,
                status == CM_RESOLVE_VIEW_STALE_REVISION
                    ? CM_HIR_LOWER_STALE_GRAPH : CM_HIR_LOWER_INVALID_AST,
                item->span, item->declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "cannot access revision-checked effective graph attribute: "
                "%s", cm_resolve_view_status_name(status));
            return 0;
        }
        source_attribute = cm_ast_get_attribute(state->ast,
            attribute.source_attribute);
        metadata_length = cm_module_graph_string_length(graph,
            attribute.metadata);
        if (attribute.source != item->declaration.source
            || !cm_lower_item_ref_equal(attribute.owner,
                item->declaration)
            || attribute.style != CM_AST_ATTR_OUTER
            || attribute.span.start > attribute.span.end
            || attribute.metadata == CM_RESOLVE_STRING_NONE
            || metadata_length == 0u || metadata_length == SIZE_MAX
            || source_attribute == NULL
            || source_attribute->style != CM_AST_ATTR_OUTER
            || !cm_lower_ast_attribute_id_in(ast_item->attributes,
                (size_t)ast_item->attribute_count,
                attribute.source_attribute)
            || (item->is_generated
                && (attribute.span.source != item->span.source
                    || attribute.span.start != item->span.start
                    || attribute.span.end != item->span.end))
            || (!item->is_generated
                && (attribute.span.source != item->declaration.source
                    || attribute.span.start < source_attribute->span.start
                    || attribute.span.end > source_attribute->span.end))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                attribute.span, item->declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "item attribute has invalid effective provenance");
            return 0;
        }
    }
    return 1;
}

static const CmAst *cm_lower_graph_ast_for_source(
    const CmModuleGraph *graph, CmSourceId source)
{
    size_t module_count;
    size_t index;

    module_count = cm_module_graph_module_count(graph);
    for (index = 0u; index < module_count; ++index) {
        CmResolveModuleInfo information;
        const CmAst *ast;

        if (cm_module_graph_get_module_at(graph, index, &information)
            && information.source == source
            && cm_module_graph_borrow_ast(graph, information.id, &ast)) {
            return ast;
        }
    }
    return NULL;
}

static int cm_lower_graph_extern_block_attributes_supported(
    CmLowerState *state, const CmModuleGraph *graph, CmModuleId module,
    const CmResolveEffectiveItem *effective)
{
    static const char supported[] = "allow(improper_ctypes)";
    uint32_t index;

    for (index = 0u; index < effective->attribute_count; ++index) {
        CmResolveEffectiveAttribute attribute;
        CmResolveViewStatus status;
        size_t length;
        char *metadata;
        int matches;

        status = cm_module_graph_get_effective_item_attribute(graph,
            state->graph_revision, module, effective->id, index,
            &attribute);
        if (status != CM_RESOLVE_VIEW_OK) {
            cm_lower_fail(state,
                status == CM_RESOLVE_VIEW_STALE_REVISION
                    ? CM_HIR_LOWER_STALE_GRAPH : CM_HIR_LOWER_INVALID_AST,
                effective->span, effective->declaration.item,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "cannot access extern-block effective attribute: %s",
                cm_resolve_view_status_name(status));
            return 0;
        }
        length = cm_module_graph_string_length(graph, attribute.metadata);
        if (length == 0u || length == SIZE_MAX) return 0;
        metadata = (char *)cm_alloc(length + 1u);
        matches = cm_module_graph_copy_string(graph, attribute.metadata,
                metadata, length + 1u)
            && length == sizeof(supported) - 1u
            && memcmp(metadata, supported, length) == 0;
        cm_free(metadata);
        if (!matches) return 0;
    }
    return 1;
}

static int cm_lower_graph_validate_effective_node(CmLowerState *state,
    const CmModuleGraph *graph, CmModuleId module,
    const CmResolveEffectiveItem *effective,
    CmLowerParentKind parent_kind, int is_foreign)
{
    const CmAst *declaration_ast;
    const CmAstItem *item;
    CmExpandedChildKind expected_child_kind;
    uint32_t child_index;

    declaration_ast = NULL;
    if (effective->id == CM_RESOLVE_EFFECTIVE_ITEM_NONE
        || effective->declaration.item == CM_AST_ITEM_NONE
        || !cm_module_graph_borrow_item_ast(graph, module,
            effective->declaration, &declaration_ast)
        || declaration_ast == NULL
        || (item = cm_ast_get_item(declaration_ast,
            effective->declaration.item)) == NULL
        || effective->item_kind != item->kind
        || effective->visibility != item->visibility.kind
        || effective->span.start > effective->span.end) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, effective->span,
            effective->declaration.item, CM_AST_TYPE_NONE,
            CM_AST_PATH_NONE, CM_HIR_OK,
            "effective graph item has invalid declaration metadata");
        return 0;
    }
    state->ast = declaration_ast;
    state->source = effective->declaration.source;
    expected_child_kind = CM_EXPANDED_CHILD_NONE;
    if (item->kind == CM_AST_ITEM_EXTERN_BLOCK) {
        expected_child_kind = CM_EXPANDED_CHILD_EXTERN_BLOCK;
    } else if (item->kind == CM_AST_ITEM_TRAIT) {
        expected_child_kind = CM_EXPANDED_CHILD_TRAIT;
    } else if (item->kind == CM_AST_ITEM_IMPL) {
        expected_child_kind = CM_EXPANDED_CHILD_IMPL;
    }
    if (effective->child_kind != expected_child_kind
        || (expected_child_kind == CM_EXPANDED_CHILD_NONE
            && effective->child_count != 0u)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, effective->span,
            effective->declaration.item, CM_AST_TYPE_NONE,
            CM_AST_PATH_NONE, CM_HIR_OK,
            "effective graph item has invalid recursive child metadata");
        return 0;
    }
    if (item->kind == CM_AST_ITEM_EXTERN_BLOCK
        && (parent_kind != CM_LOWER_PARENT_NONE || is_foreign
            || effective->is_generated
            || item->name != CM_INTERN_ID_NONE
            || item->visibility.kind != CM_AST_VIS_INHERITED
            || item->generic_parameter_count != 0u
            || cm_lower_item_has_where_clause(item)
            || item->is_default
            || !item->data.extern_block_item.is_unsafe
            || (!cm_lower_string_is(state,
                    item->data.extern_block_item.abi, "C")
                && !cm_lower_string_is(state,
                    item->data.extern_block_item.abi, "unadjusted"))
            || !cm_lower_graph_extern_block_attributes_supported(state,
                graph, module, effective))) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
            effective->span, effective->declaration.item,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "graph HIR lowering retains only source-written unsafe extern C "
            "or unadjusted blocks with no attributes except "
            "allow(improper_ctypes)");
        return 0;
    }
    if (is_foreign
        && (parent_kind != CM_LOWER_PARENT_NONE
            || effective->is_generated
            || item->generic_parameter_count != 0u
            || cm_lower_item_has_where_clause(item)
            || item->is_default
            || (item->kind == CM_AST_ITEM_FUNCTION
                && ((item->data.function_item.is_safe
                        && item->data.function_item.is_unsafe)
                    || item->data.function_item.is_const
                    || item->data.function_item.is_async
                    || item->data.function_item.abi != CM_INTERN_ID_NONE
                    || item->data.function_item.body != CM_AST_EXPR_NONE))
            || (item->kind == CM_AST_ITEM_TYPE_ALIAS
                && (item->visibility.kind != CM_AST_VIS_INHERITED
                    || item->data.value_item.has_value
                    || item->data.value_item.type != CM_AST_TYPE_NONE
                    || item->data.value_item.initializer != CM_AST_EXPR_NONE
                    || item->data.value_item.bound_count != 0u
                    || item->data.value_item.is_mutable))
            || (item->kind != CM_AST_ITEM_FUNCTION
                && item->kind != CM_AST_ITEM_TYPE_ALIAS))) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
            effective->span, effective->declaration.item,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "extern-block children must be source-written non-generic "
            "foreign types or structurally attributed bodyless functions");
        return 0;
    }
    if (parent_kind != CM_LOWER_PARENT_NONE
        && item->kind != CM_AST_ITEM_TYPE_ALIAS
        && item->kind != CM_AST_ITEM_FUNCTION
        && !(item->kind == CM_AST_ITEM_CONST
            && (parent_kind == CM_LOWER_PARENT_TRAIT
                || parent_kind == CM_LOWER_PARENT_IMPL))) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
            effective->span, effective->declaration.item,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "%s children other than associated types and methods are not "
            "supported",
            parent_kind == CM_LOWER_PARENT_TRAIT ? "trait" : "impl");
        return 0;
    }
    if (effective->is_generated) {
        const CmAst *invocation_ast;
        const CmAstItem *macro_invocation;
        int generated_kind_supported;
        int has_dependency_definition;

        generated_kind_supported = parent_kind == CM_LOWER_PARENT_NONE
            ? (item->kind == CM_AST_ITEM_FUNCTION
                || item->kind == CM_AST_ITEM_STRUCT
                || item->kind == CM_AST_ITEM_UNION
                || item->kind == CM_AST_ITEM_ENUM
                || item->kind == CM_AST_ITEM_TYPE_ALIAS
                || item->kind == CM_AST_ITEM_CONST
                || (item->kind == CM_AST_ITEM_MODULE
                    && item->data.module_item.is_inline)
                || item->kind == CM_AST_ITEM_TRAIT
                || item->kind == CM_AST_ITEM_IMPL)
            : (item->kind == CM_AST_ITEM_TYPE_ALIAS
                || item->kind == CM_AST_ITEM_FUNCTION
                || (parent_kind == CM_LOWER_PARENT_IMPL
                    && item->kind == CM_AST_ITEM_CONST));
        has_dependency_definition = !cm_lower_dependency_item_ref_empty(
            effective->provenance.dependency_macro_definition);
        invocation_ast = cm_lower_graph_ast_for_source(graph,
            effective->provenance.macro_invocation.source);
        macro_invocation = invocation_ast == NULL ? NULL
            : cm_ast_get_item(invocation_ast,
                effective->provenance.macro_invocation.item);
        if (effective->provenance.source_item.source != 0u
            || effective->provenance.source_item.item != CM_AST_ITEM_NONE
            || effective->provenance.macro_invocation.source == 0u
            || effective->provenance.macro_invocation.item
                == CM_AST_ITEM_NONE
            || effective->provenance.expansion_depth == 0u
            || effective->span.start == effective->span.end
            || macro_invocation == NULL
            || macro_invocation->kind != CM_AST_ITEM_MACRO
            || macro_invocation->data.macro_item.form
                != CM_AST_MACRO_INVOCATION
            || macro_invocation->name != CM_INTERN_ID_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                effective->span, effective->declaration.item,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "generated graph item has invalid macro invocation "
                "provenance");
            return 0;
        }
        if (has_dependency_definition) {
            CmResolveViewStatus dependency_status;

            dependency_status =
                cm_module_graph_validate_dependency_macro_definition(
                    graph, state->graph_revision,
                    effective->provenance.dependency_macro_definition);
            if (!cm_lower_dependency_item_ref_structurally_valid(
                    effective->provenance.dependency_macro_definition)
                || dependency_status != CM_RESOLVE_VIEW_OK
                || effective->provenance.macro_definition.source != 0u
                || effective->provenance.macro_definition.item
                    != CM_AST_ITEM_NONE) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    effective->span, effective->declaration.item,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "generated graph item has invalid dependency macro "
                    "provenance: %s",
                    cm_resolve_view_status_name(dependency_status));
                return 0;
            }
        } else {
            const CmAst *definition_ast;
            const CmAstItem *macro_definition;

            definition_ast = cm_lower_graph_ast_for_source(graph,
                effective->provenance.macro_definition.source);
            macro_definition = definition_ast == NULL ? NULL
                : cm_ast_get_item(definition_ast,
                    effective->provenance.macro_definition.item);
            if (effective->provenance.macro_definition.source == 0u
                || effective->provenance.macro_definition.item
                    == CM_AST_ITEM_NONE
                || macro_definition == NULL
                || macro_definition->kind != CM_AST_ITEM_MACRO
                || (macro_definition->data.macro_item.form
                        != CM_AST_MACRO_RULES_DEFINITION
                    && macro_definition->data.macro_item.form
                        != CM_AST_MACRO_DECLARATIVE_DEFINITION)
                || macro_definition->name == CM_INTERN_ID_NONE) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    effective->span, effective->declaration.item,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "generated graph item has invalid local macro "
                    "provenance");
                return 0;
            }
        }
        if (!generated_kind_supported) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
                effective->span, effective->declaration.item,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "generated graph item kind is not supported in this HIR "
                "lowering slice");
            return 0;
        }
    } else if (!cm_lower_item_ref_equal(
            effective->provenance.source_item, effective->declaration)
        || effective->provenance.macro_invocation.source != 0u
        || effective->provenance.macro_invocation.item != CM_AST_ITEM_NONE
        || effective->provenance.macro_definition.source != 0u
        || effective->provenance.macro_definition.item != CM_AST_ITEM_NONE
        || !cm_lower_dependency_item_ref_empty(
            effective->provenance.dependency_macro_definition)
        || effective->provenance.expansion_depth != 0u
        || effective->span.source != effective->declaration.source
        || effective->span.start != item->span.start
        || effective->span.end != item->span.end) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, effective->span,
            effective->declaration.item, CM_AST_TYPE_NONE,
            CM_AST_PATH_NONE, CM_HIR_OK,
            "source graph item has invalid source provenance");
        return 0;
    }
    if (!cm_lower_graph_validate_effective_attributes(state, graph,
            module, effective, item)) {
        return 0;
    }
    {
        CmSpan saved_generated_span;
        int saved_use_generated_span;
        int valid;

        saved_generated_span = state->generated_span;
        saved_use_generated_span = state->use_generated_span;
        state->generated_span = effective->span;
        state->use_generated_span = effective->is_generated;
        valid = cm_lower_validate_item_where_predicates(state,
            effective->declaration.item, item);
        state->generated_span = saved_generated_span;
        state->use_generated_span = saved_use_generated_span;
        if (!valid) return 0;
    }
    for (child_index = 0u; child_index < effective->child_count;
         ++child_index) {
        CmResolveEffectiveItem child;
        CmLowerParentKind child_parent;

        if (!cm_lower_graph_get_effective_child(state, graph, module,
                effective->id, child_index, &child)) {
            return 0;
        }
        child_parent = item->kind == CM_AST_ITEM_TRAIT
            ? CM_LOWER_PARENT_TRAIT
            : (item->kind == CM_AST_ITEM_IMPL
                ? CM_LOWER_PARENT_IMPL : CM_LOWER_PARENT_NONE);
        if (!cm_lower_graph_validate_effective_node(state, graph, module,
                &child, child_parent,
                item->kind == CM_AST_ITEM_EXTERN_BLOCK)) {
            return 0;
        }
    }
    return 1;
}

static int cm_lower_graph_validate_inner_attributes(CmLowerState *state,
    const CmModuleGraph *graph, CmModuleId module,
    const CmResolveModuleInfo *information, const CmAst *ast)
{
    const CmAstAttributeId *source_attributes;
    size_t source_attribute_count;
    CmSpan declaration_span;
    int declaration_is_generated;
    int declaration_kind_is_module;
    uint32_t index;

    source_attributes = NULL;
    source_attribute_count = 0u;
    memset(&declaration_span, 0, sizeof(declaration_span));
    declaration_is_generated = 0;
    declaration_kind_is_module = 1;
    if (information->parent != CM_MODULE_NONE) {
        CmResolveModuleInfo parent_information;
        uint32_t match_count;

        match_count = 0u;
        if (!cm_module_graph_get_module(graph, information->parent,
                &parent_information)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ information->source, 0u, 0u },
                information->declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "module inner-attribute owner has no parent module");
            return 0;
        }
        for (index = 0u; index < parent_information.effective_item_count;
             ++index) {
            CmResolveEffectiveItem effective_item;

            if (!cm_lower_graph_get_effective_item(state, graph,
                    information->parent, index, &effective_item)) {
                return 0;
            }
            if (cm_lower_item_ref_equal(effective_item.declaration,
                    information->declaration)) {
                match_count += 1u;
                declaration_span = effective_item.span;
                declaration_is_generated = effective_item.is_generated;
                if (effective_item.item_kind != CM_AST_ITEM_MODULE) {
                    declaration_kind_is_module = 0;
                }
            }
        }
        if (match_count != 1u || !declaration_kind_is_module) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ information->source, 0u, 0u },
                information->declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "module inner-attribute owner has no unique declaration");
            return 0;
        }
    }
    if (information->parent == CM_MODULE_NONE || !information->is_inline) {
        source_attributes =
            (const CmAstAttributeId *)ast->crate_attributes.data;
        source_attribute_count = ast->crate_attributes.len;
    } else {
        const CmAstItem *declaration;

        if (information->declaration.source != information->source
            || (declaration = cm_ast_get_item(ast,
                information->declaration.item)) == NULL
            || declaration->kind != CM_AST_ITEM_MODULE
            || !declaration->data.module_item.is_inline) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ information->source, 0u, 0u },
                information->declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "inline module has invalid inner-attribute owner");
            return 0;
        }
        source_attributes = declaration->data.module_item.inner_attributes;
        source_attribute_count =
            (size_t)declaration->data.module_item.inner_attribute_count;
    }
    for (index = 0u; index < information->inner_attribute_count; ++index) {
        CmResolveEffectiveAttribute attribute;
        CmResolveViewStatus status;
        const CmAstAttribute *source_attribute;
        size_t metadata_length;

        status = cm_module_graph_get_effective_inner_attribute(graph,
            state->graph_revision, module, index, &attribute);
        if (status != CM_RESOLVE_VIEW_OK) {
            cm_lower_fail(state,
                status == CM_RESOLVE_VIEW_STALE_REVISION
                    ? CM_HIR_LOWER_STALE_GRAPH : CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ information->source, 0u, 0u },
                information->declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "cannot access effective module inner attribute: %s",
                cm_resolve_view_status_name(status));
            return 0;
        }
        source_attribute = cm_ast_get_attribute(ast,
            attribute.source_attribute);
        metadata_length = cm_module_graph_string_length(graph,
            attribute.metadata);
        if (attribute.source != information->source
            || attribute.style != CM_AST_ATTR_INNER
            || attribute.span.source != information->source
            || attribute.span.start > attribute.span.end
            || attribute.metadata == CM_RESOLVE_STRING_NONE
            || metadata_length == 0u || metadata_length == SIZE_MAX
            || source_attribute == NULL
            || source_attribute->style != CM_AST_ATTR_INNER
            || !cm_lower_ast_attribute_id_in(source_attributes,
                source_attribute_count, attribute.source_attribute)
            || (declaration_is_generated
                && (attribute.span.source != declaration_span.source
                    || attribute.span.start != declaration_span.start
                    || attribute.span.end != declaration_span.end))
            || (!declaration_is_generated
                && (attribute.span.start < source_attribute->span.start
                    || attribute.span.end > source_attribute->span.end))
            || (information->parent == CM_MODULE_NONE
                && (attribute.owner.source != 0u
                    || attribute.owner.item != CM_AST_ITEM_NONE))
            || (information->parent != CM_MODULE_NONE
                && !cm_lower_item_ref_equal(attribute.owner,
                    information->declaration))) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                attribute.span, information->declaration.item,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "module inner attribute has invalid effective provenance");
            return 0;
        }
    }
    return 1;
}

static int cm_lower_graph_name_matches_ast(const CmModuleGraph *graph,
    CmResolveStringId graph_name, const CmAst *ast, CmInternId ast_name)
{
    const CmInternedString *name;
    size_t length;
    char *buffer;
    int matches;

    name = cm_ast_get_string(ast, ast_name);
    length = cm_module_graph_string_length(graph, graph_name);
    if (name == NULL || length != name->len || length == SIZE_MAX) {
        return 0;
    }
    buffer = (char *)cm_alloc(length + 1u);
    matches = cm_module_graph_copy_string(graph, graph_name, buffer,
        length + 1u) && memcmp(buffer, name->bytes, length) == 0;
    cm_free(buffer);
    return matches;
}

static int cm_lower_graph_import_for_declaration(CmLowerState *state,
    const CmModuleGraph *graph, CmModuleId module,
    CmResolveItemRef declaration, const CmAstItem *ast_item,
    CmResolveImport *out_import)
{
    CmResolveModuleInfo information;
    CmResolveImport matched;
    uint32_t index;
    uint32_t matches;

    memset(&matched, 0, sizeof(matched));
    matches = 0u;
    if (ast_item == NULL || ast_item->kind != CM_AST_ITEM_USE
        || !cm_module_graph_get_module(graph, module, &information)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            (CmSpan){ declaration.source, 0u, 0u }, declaration.item,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "use declaration has no valid graph import owner");
        return 0;
    }
    for (index = 0u; index < information.import_count; ++index) {
        CmResolveImport candidate;

        if (!cm_module_graph_get_import(graph, module, index, &candidate)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ declaration.source, 0u, 0u }, declaration.item,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                "module import list cannot be read");
            return 0;
        }
        if (cm_lower_item_ref_equal(candidate.declaration, declaration)) {
            matched = candidate;
            matches += 1u;
        }
    }
    if (matches != 1u
        || matched.visibility != ast_item->visibility.kind
        || !cm_lower_graph_name_matches_ast(graph, matched.tree,
            state->ast, ast_item->data.use_item.tree)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            (CmSpan){ declaration.source, ast_item->span.start,
                ast_item->span.end }, declaration.item,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "use declaration does not have one matching graph import");
        return 0;
    }
    if (out_import != NULL) *out_import = matched;
    return 1;
}

static CmModuleId cm_lower_graph_declaration_child(
    const CmModuleGraph *graph, CmModuleId parent,
    CmResolveItemRef declaration, uint32_t *out_match_count)
{
    CmResolveModuleInfo parent_information;
    uint32_t index;
    CmModuleId child;
    uint32_t matches;

    child = CM_MODULE_NONE;
    matches = 0u;
    if (!cm_module_graph_get_module(graph, parent, &parent_information)) {
        if (out_match_count != NULL) {
            *out_match_count = 0u;
        }
        return CM_MODULE_NONE;
    }
    for (index = 0u; index < parent_information.child_count; ++index) {
        CmModuleId candidate;
        CmResolveModuleInfo information;

        if (!cm_module_graph_get_child(graph, parent, index, &candidate)
            || !cm_module_graph_get_module(graph, candidate, &information)
            || !cm_lower_item_ref_equal(information.declaration,
                declaration)) {
            continue;
        }
        child = candidate;
        matches += 1u;
    }
    if (out_match_count != NULL) {
        *out_match_count = matches;
    }
    return child;
}

static int cm_lower_graph_traversal_contains(const CmVec *traversal,
    CmModuleId module)
{
    size_t index;

    for (index = 0u; index < traversal->len; ++index) {
        const CmModuleId *seen;

        seen = (const CmModuleId *)cm_vec_at_const(traversal, index);
        if (seen != NULL && *seen == module) return 1;
    }
    return 0;
}

static int cm_lower_graph_visit(CmLowerState *state,
    const CmModuleGraph *graph, CmModuleId module, CmVec *traversal)
{
    CmResolveModuleInfo information;
    uint32_t index;

    if (module == CM_MODULE_NONE
        || !cm_module_graph_get_module(graph, module, &information)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            (CmSpan){ 0u, 0u, 0u }, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
            CM_AST_PATH_NONE, CM_HIR_OK,
            "module graph child list contains an invalid module ID");
        return 0;
    }
    if (cm_lower_graph_traversal_contains(traversal, module)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
            (CmSpan){ information.source, 0u, 0u }, CM_AST_ITEM_NONE,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "module graph child traversal contains a cycle or duplicate");
        return 0;
    }
    (void)cm_vec_push(traversal, &module);
    for (index = 0u; index < information.child_count && !state->failed;
         ++index) {
        CmModuleId child;
        CmResolveModuleInfo child_information;

        if (!cm_module_graph_get_child(graph, module, index, &child)
            || !cm_module_graph_get_module(graph, child,
                &child_information)
            || child_information.parent != module) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ information.source, 0u, 0u },
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK, "module graph child has the wrong parent");
            return 0;
        }
        if (!cm_lower_graph_visit(state, graph, child, traversal)) {
            return 0;
        }
    }
    return !state->failed;
}

static int cm_lower_graph_ref_seen_before(CmLowerState *state,
    const CmModuleGraph *graph, const CmVec *traversal,
    size_t module_position, CmModuleId current_module,
    uint32_t item_position, CmResolveItemRef reference)
{
    size_t prior_module_position;
    uint32_t prior_item_position;

    for (prior_item_position = 0u; prior_item_position < item_position;
         ++prior_item_position) {
        CmResolveEffectiveItem prior_item;

        if (!cm_lower_graph_get_effective_item(state, graph,
                current_module, prior_item_position, &prior_item)) {
            return 0;
        }
        if (cm_lower_item_ref_equal(prior_item.declaration, reference)) {
            return 1;
        }
    }
    for (prior_module_position = 0u;
         prior_module_position < module_position; ++prior_module_position) {
        CmModuleId prior_module;
        CmResolveModuleInfo prior_information;
        uint32_t prior_item_count;

        prior_module = *(const CmModuleId *)cm_vec_at_const(traversal,
            prior_module_position);
        if (!cm_module_graph_get_module(graph, prior_module,
                &prior_information)) {
            continue;
        }
        prior_item_count = prior_information.effective_item_count;
        for (prior_item_position = 0u;
             prior_item_position < prior_item_count;
             ++prior_item_position) {
            CmResolveEffectiveItem prior_item;

            if (!cm_lower_graph_get_effective_item(state, graph,
                    prior_module, prior_item_position, &prior_item)) {
                return 0;
            }
            if (cm_lower_item_ref_equal(prior_item.declaration,
                    reference)) {
                return 1;
            }
        }
    }
    return 0;
}

static int cm_lower_graph_preflight(CmLowerState *state,
    const CmModuleGraph *graph, const CmHirModuleMap *modules,
    CmModuleId *out_root, CmSpan *out_crate_span, CmVec *traversal)
{
    size_t module_count;
    size_t module_index;
    CmModuleId root;
    int root_span_seen;

    module_count = cm_module_graph_module_count(graph);
    root = CM_MODULE_NONE;
    memset(out_crate_span, 0, sizeof(*out_crate_span));
    if (graph->state == NULL || modules->state == NULL
        || module_count == 0u || module_count > (size_t)UINT32_MAX
        || cm_module_graph_error_count(graph) != 0u
        || cm_hir_module_map_count(modules) != 0u) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_ARGUMENT,
            *out_crate_span, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
            CM_AST_PATH_NONE, CM_HIR_INVALID_ARGUMENT,
            "module graph must be successful and the module map must be "
            "initialized and empty");
        return 0;
    }
    if (!cm_module_graph_get_root(graph, &root)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, *out_crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK, "module graph must contain exactly one root");
        return 0;
    }
    for (module_index = 0u; module_index < module_count; ++module_index) {
        CmResolveModuleInfo information;
        size_t prior;

        if (!cm_module_graph_get_module_at(graph, module_index,
                &information)
            || information.id == CM_MODULE_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                *out_crate_span, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "module graph module storage is not enumerable");
            return 0;
        }
        for (prior = 0u; prior < module_index; ++prior) {
            CmResolveModuleInfo prior_information;

            if (cm_module_graph_get_module_at(graph, prior,
                    &prior_information)
                && prior_information.id == information.id) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    *out_crate_span, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "module graph enumeration contains a duplicate ID");
                return 0;
            }
        }
    }
    if (!cm_lower_graph_visit(state, graph, root, traversal)) {
        return 0;
    }
    if (traversal->len != module_count) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, *out_crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK, "module graph contains an unreachable module");
        return 0;
    }
    for (module_index = 0u; module_index < module_count; ++module_index) {
        CmResolveModuleInfo information;

        (void)cm_module_graph_get_module_at(graph, module_index,
            &information);
        if (!cm_lower_graph_traversal_contains(traversal,
                information.id)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                *out_crate_span, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "module graph contains an unreachable module");
            return 0;
        }
    }
    root_span_seen = 0;
    for (module_index = 0u; module_index < traversal->len
         && !state->failed; ++module_index) {
        CmModuleId module_id;
        CmResolveModuleInfo information;
        const CmAst *ast;
        uint32_t effective_item_count;
        uint32_t item_index;

        module_id = *(const CmModuleId *)cm_vec_at_const(traversal,
            module_index);
        if (!cm_module_graph_get_module(graph, module_id, &information)
            || information.id != module_id || information.source == 0u
            || !cm_module_graph_borrow_ast(graph, module_id, &ast)
            || ast == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                *out_crate_span, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "module graph contains an invalid syntax view");
            break;
        }
        effective_item_count = information.effective_item_count;
        state->ast = ast;
        state->source = information.source;
        state->graph_module = module_id;
        if (information.parent == CM_MODULE_NONE) {
            out_crate_span->source = information.source;
        } else {
            CmResolveModuleInfo parent;
            const CmAst *parent_ast;
            const CmAstItem *declaration;

            if (!cm_module_graph_get_module(graph, information.parent,
                    &parent)
                || information.declaration.item == CM_AST_ITEM_NONE
                || !cm_module_graph_borrow_item_ast(graph,
                    information.parent, information.declaration,
                    &parent_ast)
                || (declaration = cm_ast_get_item(parent_ast,
                    information.declaration.item)) == NULL
                || declaration->kind != CM_AST_ITEM_MODULE
                || !cm_lower_graph_name_matches_ast(graph,
                    information.name, parent_ast, declaration->name)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, (CmAstSpan){ 0u, 0u }),
                    information.declaration.item, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "child module has invalid declaration provenance");
                break;
            }
        }
        if (!cm_lower_graph_validate_inner_attributes(state, graph,
                module_id, &information, ast)) {
            break;
        }
        for (item_index = 0u; item_index < effective_item_count;
             ++item_index) {
            CmResolveEffectiveItem effective;
            CmResolveItemRef reference;
            const CmAst *item_ast;
            const CmAstItem *item;
            CmSpan span;

            if (!cm_lower_graph_get_effective_item(state, graph, module_id,
                    item_index, &effective)) {
                break;
            }
            reference = effective.declaration;
            if (cm_lower_graph_ref_seen_before(state, graph, traversal,
                    module_index, module_id, item_index, reference)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, (CmAstSpan){ 0u, 0u }),
                    reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "cfg-active source-qualified item is duplicated");
                break;
            }
            if (state->failed) break;
            item_ast = NULL;
            state->source = reference.source;
            if (reference.item == CM_AST_ITEM_NONE
                || !cm_module_graph_borrow_item_ast(graph, module_id,
                    reference, &item_ast)
                || item_ast == NULL
                || (item = cm_ast_get_item(item_ast,
                    reference.item)) == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                    cm_lower_span(state, (CmAstSpan){ 0u, 0u }),
                    reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "cfg-active item has invalid source-qualified identity");
                break;
            }
            state->ast = item_ast;
            span = effective.span;
            if (!cm_lower_graph_validate_effective_node(state, graph,
                    module_id, &effective, CM_LOWER_PARENT_NONE, 0)) {
                break;
            }
            state->ast = item_ast;
            state->source = reference.source;
            if (item->kind == CM_AST_ITEM_USE) {
                if (!cm_lower_graph_import_for_declaration(state, graph,
                        module_id, reference, item, NULL)) {
                    break;
                }
                continue;
            }
            if (item->kind == CM_AST_ITEM_EXTERN_CRATE) continue;
            if (item->kind == CM_AST_ITEM_EXTERN_BLOCK) {
                if (module_id == root) {
                    if (!root_span_seen
                        || span.start < out_crate_span->start) {
                        out_crate_span->start = span.start;
                    }
                    if (span.end > out_crate_span->end) {
                        out_crate_span->end = span.end;
                    }
                    root_span_seen = 1;
                }
                continue;
            }
            if (!cm_lower_item_kind_supported(item->kind)) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "active item kind %u has no graph HIR lowering",
                    (unsigned int)item->kind);
                break;
            }
            if (item->kind == CM_AST_ITEM_STATIC
                && (item->data.value_item.is_mutable
                    || item->data.value_item.type == CM_AST_TYPE_NONE
                    || !item->data.value_item.has_value
                    || item->data.value_item.initializer
                        == CM_AST_EXPR_NONE)) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "graph lowering retains only source-written, immutable, "
                    "explicitly typed, initializer-bearing statics");
                break;
            }
            if (item->kind == CM_AST_ITEM_FUNCTION
                && item->data.function_item.body != CM_AST_EXPR_NONE
                && effective.is_generated
                && effective.declaration.source != effective.span.source) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK,
                    "generated top-level function bodies produced from an "
                    "included source require distinct syntax and diagnostic "
                    "source provenance");
                break;
            }
            if ((item->kind == CM_AST_ITEM_IMPL
                    && item->name != CM_INTERN_ID_NONE)
                || (item->kind != CM_AST_ITEM_IMPL
                    && (item->name == CM_INTERN_ID_NONE
                        || cm_ast_get_string(ast, item->name) == NULL))) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                    reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK, "active declaration has an invalid name");
                break;
            }
            if (item->kind == CM_AST_ITEM_IMPL
                && item->visibility.kind != CM_AST_VIS_INHERITED) {
                cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                    reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_OK, "impl blocks cannot have explicit visibility");
                break;
            }
            {
                uint32_t prior;

                for (prior = 0u; prior < item_index; ++prior) {
                    CmResolveEffectiveItem prior_effective;
                    const CmAstItem *prior_item;

                    if (!cm_lower_graph_get_effective_item(state, graph,
                            module_id, prior, &prior_effective)) {
                        break;
                    }
                    prior_item = cm_ast_get_item(ast,
                        prior_effective.declaration.item);
                    if (prior_item != NULL
                        && !cm_lower_anonymous_const(item_ast, item)
                        && !cm_lower_anonymous_const(ast, prior_item)
                        && prior_item->kind != CM_AST_ITEM_USE
                        && (cm_lower_item_namespace_mask(prior_item->kind)
                            & cm_lower_item_namespace_mask(item->kind)) != 0u
                        && cm_lower_strings_equal(state, prior_item->name,
                            item->name)) {
                        cm_lower_fail(state, CM_HIR_LOWER_DUPLICATE_NAME,
                            span, reference.item, CM_AST_TYPE_NONE,
                            CM_AST_PATH_NONE, CM_HIR_OK,
                            "duplicate active declaration name in module");
                        break;
                    }
                }
                if (state->failed) {
                    break;
                }
            }
            if (item->kind == CM_AST_ITEM_MODULE) {
                uint32_t match_count;

                (void)cm_lower_graph_declaration_child(graph, module_id,
                    reference, &match_count);
                if (match_count != 1u) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                        reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                        CM_HIR_OK,
                        "active module declaration does not identify exactly "
                        "one graph child");
                    break;
                }
            }
            if (module_id == root) {
                if (!root_span_seen
                    || span.start < out_crate_span->start) {
                    out_crate_span->start = span.start;
                }
                if (span.end > out_crate_span->end) {
                    out_crate_span->end = span.end;
                }
                root_span_seen = 1;
            }
        }
        if (!state->failed) {
            uint32_t child_index;

            for (child_index = 0u; child_index < information.child_count;
                 ++child_index) {
                CmModuleId child;
                CmResolveModuleInfo child_information;
                uint32_t matches;

                matches = 0u;
                (void)cm_module_graph_get_child(graph, module_id,
                    child_index, &child);
                (void)cm_module_graph_get_module(graph, child,
                    &child_information);
                for (item_index = 0u; item_index < effective_item_count;
                     ++item_index) {
                    CmResolveEffectiveItem effective;

                    if (!cm_lower_graph_get_effective_item(state, graph,
                            module_id, item_index, &effective)) {
                        break;
                    }
                    if (cm_lower_item_ref_equal(effective.declaration,
                            child_information.declaration)) {
                        matches += 1u;
                    }
                }
                if (state->failed) break;
                if (matches != 1u) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                        (CmSpan){ information.source, 0u, 0u },
                        child_information.declaration.item,
                        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                        "graph child is not named by exactly one active "
                        "parent module declaration");
                    break;
                }
            }
        }
    }
    *out_root = root;
    return !state->failed;
}

static CmInternId cm_lower_copy_graph_interned(CmLowerState *state,
    const CmModuleGraph *graph, CmResolveStringId id, CmSpan span,
    CmAstItemId declaration, const char *invalid_message,
    const char *copy_message)
{
    size_t length;
    char *buffer;
    CmInternId result;

    length = cm_module_graph_string_length(graph, id);
    if (id == CM_RESOLVE_STRING_NONE || length == 0u
        || length == SIZE_MAX) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, declaration,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "%s", invalid_message);
        return CM_INTERN_ID_NONE;
    }
    buffer = (char *)cm_alloc(length + 1u);
    if (!cm_module_graph_copy_string(graph, id, buffer, length + 1u)) {
        cm_free(buffer);
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span, declaration,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "%s", copy_message);
        return CM_INTERN_ID_NONE;
    }
    result = cm_hir_intern(state->hir, buffer);
    cm_free(buffer);
    return result;
}

static CmInternId cm_lower_copy_graph_string(CmLowerState *state,
    const CmModuleGraph *graph, CmResolveStringId id, CmSpan span,
    CmAstItemId declaration)
{
    return cm_lower_copy_graph_interned(state, graph, id, span, declaration,
        "module has an invalid graph-owned name",
        "cannot copy graph-owned module name");
}

static CmInternId cm_lower_copy_graph_attribute_metadata(
    CmLowerState *state, const CmModuleGraph *graph, CmResolveStringId id,
    CmSpan span, CmAstItemId declaration)
{
    return cm_lower_copy_graph_interned(state, graph, id, span, declaration,
        "attribute has invalid graph-owned metadata",
        "cannot copy graph-owned attribute metadata");
}

static CmInternId cm_lower_copy_import_string(CmLowerState *state,
    CmResolveStringId id, CmSpan span, CmAstItemId declaration)
{
    size_t length;
    char *buffer;
    CmInternId result;

    length = cm_import_string_length(state->imports, id);
    if (id == CM_RESOLVE_STRING_NONE || length == 0u
        || length == SIZE_MAX) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            declaration, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "import binding has an invalid resolver-owned name");
        return CM_INTERN_ID_NONE;
    }
    buffer = (char *)cm_alloc(length + 1u);
    if (!cm_import_copy_string(state->imports, id, buffer, length + 1u)) {
        cm_free(buffer);
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            declaration, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "cannot copy resolver-owned import binding name");
        return CM_INTERN_ID_NONE;
    }
    result = cm_hir_intern(state->hir, buffer);
    cm_free(buffer);
    return result;
}

static int cm_lower_graph_create_modules(CmLowerState *state,
    const CmModuleGraph *graph, CmHirModuleMap *modules,
    const CmVec *traversal, CmModuleId graph_root, CmSpan crate_span)
{
    size_t index;
    CmInternId crate_name;
    CmHirStatus hir_status;
    CmHirModuleMapEntry expected_binding;
    CmHirModuleMapStatus map_status;

    if (cm_module_graph_revision(graph) != state->graph_revision) {
        cm_lower_fail(state, CM_HIR_LOWER_STALE_GRAPH, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "module graph revision changed before HIR mutation");
        return 0;
    }
    crate_name = cm_hir_intern(state->hir, state->options->crate_name);
    hir_status = cm_hir_create_crate(state->hir, crate_name,
        state->options->edition, crate_span, &state->result.crate_id,
        &state->result.root_module);
    if (hir_status != CM_HIR_OK) {
        cm_lower_fail_hir(state, crate_span, CM_AST_ITEM_NONE, hir_status,
            "cannot create graph HIR crate");
        return 0;
    }
    map_status = cm_hir_module_map_bind(modules, graph,
        state->graph_revision, graph_root, state->hir,
        state->result.root_module);
    if (map_status != CM_HIR_MODULE_MAP_OK) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "cannot bind graph root module: %s",
            cm_hir_module_map_status_name(map_status));
        return 0;
    }
    expected_binding.module = graph_root;
    expected_binding.hir_module = state->result.root_module;
    (void)cm_vec_push(&state->expected_module_bindings, &expected_binding);
    for (index = 0u; index < traversal->len && !state->failed; ++index) {
        CmModuleId graph_module;
        CmResolveModuleInfo information;
        CmHirModuleId parent_hir;
        CmHirModuleId hir_module;
        const CmAst *parent_ast;
        const CmAstItem *declaration;
        CmSpan span;
        CmInternId name;
        CmResolveModuleInfo parent_information;
        uint32_t declaration_matches;
        uint32_t item_index;

        graph_module = *(const CmModuleId *)cm_vec_at_const(traversal,
            index);
        if (graph_module == graph_root) {
            continue;
        }
        (void)cm_module_graph_get_module(graph, graph_module, &information);
        (void)cm_hir_module_map_lookup_hir(modules, graph,
            state->graph_revision, information.parent, state->hir,
            &parent_hir);
        (void)cm_module_graph_borrow_ast(graph, information.parent,
            &parent_ast);
        declaration = cm_ast_get_item(parent_ast,
            information.declaration.item);
        memset(&span, 0, sizeof(span));
        declaration_matches = 0u;
        if (!cm_module_graph_get_module(graph, information.parent,
                &parent_information)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ information.source, 0u, 0u },
                information.declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "mapped module has no graph parent");
            break;
        }
        for (item_index = 0u;
             item_index < parent_information.effective_item_count;
             ++item_index) {
            CmResolveEffectiveItem effective;

            if (!cm_lower_graph_get_effective_item(state, graph,
                    information.parent, item_index, &effective)) {
                break;
            }
            if (cm_lower_item_ref_equal(effective.declaration,
                    information.declaration)) {
                span = effective.span;
                declaration_matches += 1u;
            }
        }
        if (state->failed) break;
        if (declaration == NULL || declaration_matches != 1u) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ information.source, 0u, 0u },
                information.declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "mapped module has no unique effective declaration");
            break;
        }
        name = cm_lower_copy_graph_string(state, graph, information.name,
            span, information.declaration.item);
        if (state->failed) {
            break;
        }
        hir_status = cm_hir_add_module(state->hir, state->result.crate_id,
            parent_hir, name, span, &hir_module);
        if (hir_status != CM_HIR_OK) {
            cm_lower_fail_hir(state, span, information.declaration.item,
                hir_status, "cannot create graph HIR module");
            break;
        }
        map_status = cm_hir_module_map_bind(modules, graph,
            state->graph_revision, graph_module, state->hir, hir_module);
        if (map_status != CM_HIR_MODULE_MAP_OK) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                information.declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_INVARIANT_VIOLATION,
                "cannot bind graph HIR module: %s",
                cm_hir_module_map_status_name(map_status));
            break;
        }
        expected_binding.module = graph_module;
        expected_binding.hir_module = hir_module;
        (void)cm_vec_push(&state->expected_module_bindings,
            &expected_binding);
    }
    if (!state->failed
        && cm_hir_module_map_count(modules) != traversal->len) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "graph-to-HIR module map is incomplete");
    }
    for (index = 0u; index < traversal->len && !state->failed; ++index) {
        CmModuleId graph_module;
        CmModuleId reverse_module;
        CmResolveModuleInfo information;
        CmHirModuleId hir_module_id;
        const CmHirModule *hir_module;

        graph_module = *(const CmModuleId *)cm_vec_at_const(traversal,
            index);
        if (!cm_module_graph_get_module(graph, graph_module, &information)
            || cm_hir_module_map_lookup_hir(modules, graph,
                state->graph_revision, graph_module, state->hir,
                &hir_module_id) != CM_HIR_MODULE_MAP_OK
            || cm_hir_module_map_lookup_module(modules, graph,
                state->graph_revision, state->hir, hir_module_id,
                &reverse_module) != CM_HIR_MODULE_MAP_OK
            || reverse_module != graph_module
            || (hir_module = cm_hir_get_module(state->hir,
                hir_module_id)) == NULL
            || hir_module->crate_id != state->result.crate_id
            || (graph_module == graph_root
                && (hir_module_id != state->result.root_module
                    || hir_module->parent != CM_HIR_MODULE_NONE))) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, crate_span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "graph-to-HIR module map failed ownership validation");
            break;
        }
        if (graph_module != graph_root) {
            CmHirModuleId parent_hir;

            if (cm_hir_module_map_lookup_hir(modules, graph,
                    state->graph_revision, information.parent, state->hir,
                    &parent_hir) != CM_HIR_MODULE_MAP_OK
                || hir_module->parent != parent_hir) {
                cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                    hir_module->span, information.declaration.item,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_INVARIANT_VIOLATION,
                    "graph-to-HIR module map failed hierarchy validation");
                break;
            }
        }
    }
    return !state->failed;
}

static int cm_lower_graph_apply_inner_attributes(CmLowerState *state,
    const CmModuleGraph *graph, const CmHirModuleMap *modules,
    const CmVec *traversal, CmModuleId graph_root)
{
    size_t module_index;

    for (module_index = 0u; module_index < traversal->len
         && !state->failed; ++module_index) {
        CmModuleId graph_module;
        CmResolveModuleInfo information;
        CmHirModuleId hir_module;
        CmHirAttribute *attributes;
        uint32_t attribute_index;
        CmHirStatus status;

        graph_module = *(const CmModuleId *)cm_vec_at_const(traversal,
            module_index);
        if (!cm_module_graph_get_module(graph, graph_module, &information)
            || cm_hir_module_map_lookup_hir(modules, graph,
                state->graph_revision, graph_module, state->hir,
                &hir_module) != CM_HIR_MODULE_MAP_OK) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                (CmSpan){ 0u, 0u, 0u }, CM_AST_ITEM_NONE,
                CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "cannot map module while lowering inner attributes");
            break;
        }
        attributes = information.inner_attribute_count == 0u ? NULL
            : (CmHirAttribute *)cm_alloc_zeroed(
                (size_t)information.inner_attribute_count,
                sizeof(CmHirAttribute));
        for (attribute_index = 0u;
             attribute_index < information.inner_attribute_count;
             ++attribute_index) {
            CmResolveEffectiveAttribute effective;
            CmResolveViewStatus view_status;

            view_status = cm_module_graph_get_effective_inner_attribute(
                graph, state->graph_revision, graph_module,
                attribute_index, &effective);
            if (view_status != CM_RESOLVE_VIEW_OK) {
                cm_lower_fail(state,
                    view_status == CM_RESOLVE_VIEW_STALE_REVISION
                        ? CM_HIR_LOWER_STALE_GRAPH
                        : CM_HIR_LOWER_INVALID_AST,
                    (CmSpan){ information.source, 0u, 0u },
                    information.declaration.item, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "cannot consume effective module inner attribute: %s",
                    cm_resolve_view_status_name(view_status));
                break;
            }
            attributes[attribute_index].metadata =
                cm_lower_copy_graph_attribute_metadata(state, graph,
                    effective.metadata, effective.span,
                    information.declaration.item);
            attributes[attribute_index].span = effective.span;
            attributes[attribute_index].source_attribute =
                effective.source_attribute;
            attributes[attribute_index].expansion_depth =
                effective.expansion_depth;
            if (state->failed) break;
        }
        if (state->failed) {
            cm_free(attributes);
            break;
        }
        if (graph_module == graph_root) {
            status = cm_hir_set_crate_inner_attributes(state->hir,
                state->result.crate_id, attributes,
                information.inner_attribute_count);
        } else {
            status = cm_hir_set_module_inner_attributes(state->hir,
                hir_module, attributes, information.inner_attribute_count);
        }
        cm_free(attributes);
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state,
                (CmSpan){ information.source, 0u, 0u },
                information.declaration.item, status,
                "cannot store effective module inner attributes");
            break;
        }
    }
    return !state->failed;
}

static int cm_lower_graph_reserve_effective_item(CmLowerState *state,
    const CmModuleGraph *graph, const CmHirModuleMap *modules,
    CmModuleId graph_module, const CmAst *ast,
    CmHirModuleId owner_module, CmHirDefId parent_definition,
    CmLowerParentKind parent_kind, int is_foreign,
    CmInternId inherited_abi,
    const CmResolveEffectiveItem *effective)
{
    CmResolveItemRef reference;
    const CmAstItem *item;
    CmLowerItemRecord record;
    CmSpan span;
    uint32_t index;

    state->ast = ast;
    state->source = effective->declaration.source;
    state->graph_module = graph_module;
    reference = effective->declaration;
    item = cm_ast_get_item(ast, reference.item);
    span = effective->span;
    if (item == NULL) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "effective graph item has no AST declaration");
        return 0;
    }
    if ((parent_kind == CM_LOWER_PARENT_NONE)
        != cm_hir_def_id_is_none(parent_definition)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "effective item reservation has an inconsistent parent role");
        return 0;
    }
    if (item->kind == CM_AST_ITEM_EXTERN_BLOCK) {
        if (is_foreign || parent_kind != CM_LOWER_PARENT_NONE
            || inherited_abi != CM_INTERN_ID_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
                reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "extern block reservation has an invalid inherited role");
            return 0;
        }
        for (index = 0u; index < effective->child_count
             && !state->failed; ++index) {
            CmResolveEffectiveItem child;

            if (!cm_lower_graph_get_effective_child(state, graph,
                    graph_module, effective->id, index, &child)) {
                return 0;
            }
            if (!cm_lower_graph_reserve_effective_item(state, graph,
                    modules, graph_module, ast, owner_module,
                    cm_hir_def_id_none(), CM_LOWER_PARENT_NONE, 1,
                    item->data.extern_block_item.abi, &child)) {
                return 0;
            }
        }
        return !state->failed;
    }
    if (is_foreign
        && ((item->kind != CM_AST_ITEM_FUNCTION
                && item->kind != CM_AST_ITEM_TYPE_ALIAS)
            || inherited_abi == CM_INTERN_ID_NONE)) {
        cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK,
            "foreign reservation is not a declaration with inherited ABI");
        return 0;
    }
    if (item->kind == CM_AST_ITEM_IMPL
        && item->visibility.kind != CM_AST_VIS_INHERITED) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "impl blocks cannot have explicit visibility");
        return 0;
    }
    if (parent_kind != CM_LOWER_PARENT_NONE
        && (item->generic_parameter_count != 0u
            || cm_lower_item_has_where_clause(item))
        && !((item->kind == CM_AST_ITEM_FUNCTION
                && cm_lower_parent_supports_generic_method(state,
                    parent_kind, parent_definition))
            || (item->kind == CM_AST_ITEM_TYPE_ALIAS
                && cm_lower_parent_supports_generic_associated_type(state,
                    parent_kind, parent_definition)))) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_GENERIC, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            item->generic_parameter_count != 0u
                ? "generic associated items are supported only on methods "
                  "and positive-trait associated types"
                : "where predicates are currently supported only on "
                  "methods and positive-trait associated types");
        return 0;
    }
    if (parent_kind != CM_LOWER_PARENT_NONE
        && item->kind == CM_AST_ITEM_TYPE_ALIAS
        && (item->visibility.kind != CM_AST_VIS_INHERITED
            || (parent_kind == CM_LOWER_PARENT_TRAIT
                && (item->data.value_item.has_value
                    || item->data.value_item.type != CM_AST_TYPE_NONE))
            || (parent_kind == CM_LOWER_PARENT_IMPL
                && (!item->data.value_item.has_value
                    || item->data.value_item.type == CM_AST_TYPE_NONE
                    || item->data.value_item.bound_count != 0u)))) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            parent_kind == CM_LOWER_PARENT_TRAIT
                ? (item->data.value_item.has_value
                        || item->data.value_item.type != CM_AST_TYPE_NONE
                    ? "associated type defaults are not supported"
                    : "associated types must be targetless non-generic "
                      "declarations in this HIR slice")
                : "impl associated types must be target-bearing non-generic "
                  "definitions in this HIR slice");
        return 0;
    }
    if (parent_kind != CM_LOWER_PARENT_NONE
        && item->kind == CM_AST_ITEM_FUNCTION
        && ((item->visibility.kind != CM_AST_VIS_INHERITED
                && !cm_lower_parent_is_inherent_impl(state, parent_kind,
                    parent_definition))
            || (item->data.function_item.is_const
                && !cm_lower_parent_is_inherent_impl(state, parent_kind,
                    parent_definition))
            || (item->data.function_item.is_async
                && parent_kind != CM_LOWER_PARENT_TRAIT)
            || (item->data.function_item.abi != CM_INTERN_ID_NONE
                && !cm_lower_string_is(state,
                    item->data.function_item.abi, "rust-call"))
            || (parent_kind == CM_LOWER_PARENT_IMPL
                && item->data.function_item.body == CM_AST_EXPR_NONE))) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            parent_kind == CM_LOWER_PARENT_TRAIT
                ? "trait methods must use Rust or rust-call ABI "
                  "declarations"
                : "trait impl methods must be inherited-visibility, "
                  "non-const Rust or rust-call ABI definitions; inherent methods may "
                  "add explicit visibility and const");
        return 0;
    }
    if (parent_kind == CM_LOWER_PARENT_TRAIT
        && item->kind == CM_AST_ITEM_CONST
        && (item->visibility.kind != CM_AST_VIS_INHERITED
            || item->is_default
            || item->data.value_item.type == CM_AST_TYPE_NONE
            || (item->data.value_item.has_value
                != (item->data.value_item.initializer != CM_AST_EXPR_NONE))
            || item->data.value_item.is_mutable)) {
        cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "trait associated consts must be inherited-visibility, "
            "immutable, explicitly typed declarations with a consistent "
            "optional default");
        return 0;
    }
    if (parent_kind == CM_LOWER_PARENT_IMPL
        && item->kind == CM_AST_ITEM_CONST) {
        const CmLowerItemRecord *parent_record;
        const CmAstItem *parent_item;

        parent_record = cm_lower_find_record_by_definition(state,
            parent_definition);
        parent_item = parent_record == NULL ? NULL
            : cm_ast_get_item(parent_record->ast, parent_record->ast_id);
        if (parent_item == NULL || parent_item->kind != CM_AST_ITEM_IMPL
            || item->data.value_item.type == CM_AST_TYPE_NONE
            || !item->data.value_item.has_value
            || item->data.value_item.initializer == CM_AST_EXPR_NONE
            || item->data.value_item.is_mutable) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "associated consts must be immutable, "
                "explicitly typed initializer-bearing definitions");
            return 0;
        }
    }
    if ((parent_kind == CM_LOWER_PARENT_NONE
            && !cm_lower_anonymous_const(ast, item)
            && cm_lower_name_exists(state, owner_module, item->name,
                item->kind))
        || (parent_kind != CM_LOWER_PARENT_NONE
            && cm_lower_associated_name_exists(state, parent_definition,
                item->name, item->kind))) {
        cm_lower_fail(state, CM_HIR_LOWER_DUPLICATE_NAME, span,
            reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            parent_kind == CM_LOWER_PARENT_IMPL
                ? "duplicate associated definition in one impl"
                : "duplicate declaration name in one module or trait");
        return 0;
    }
    memset(&record, 0, sizeof(record));
    record.ast = ast;
    record.source = reference.source;
    record.graph_module = graph_module;
    record.graph_effective_item = effective->id;
    record.effective_attribute_count = effective->attribute_count;
    record.ast_id = reference.item;
    record.owner_module = owner_module;
    record.parent_definition = parent_definition;
    record.parent_kind = parent_kind;
    record.kind = item->kind;
    record.provenance = effective->provenance;
    record.effective_span = effective->span;
    record.is_generated = effective->is_generated;
    record.is_foreign = is_foreign;
    record.inherited_abi = inherited_abi;
    record.hir_name = item->kind == CM_AST_ITEM_IMPL
        ? CM_INTERN_ID_NONE
        : cm_lower_copy_string(state, item->name, span, reference.item);
    if (state->failed) return 0;
    if (item->kind == CM_AST_ITEM_MODULE) {
        CmModuleId child;
        const CmHirModule *child_module;

        child = cm_lower_graph_declaration_child(graph, graph_module,
            reference, NULL);
        (void)cm_hir_module_map_lookup_hir(modules, graph,
            state->graph_revision, child, state->hir,
            &record.nested_module);
        child_module = cm_hir_get_module(state->hir,
            record.nested_module);
        if (child_module == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, span,
                reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVALID_ID,
                "module declaration has no mapped HIR child");
            return 0;
        }
        record.definition = child_module->definition;
    } else {
        CmHirItemKind hir_item_kind;
        CmHirStatus status;

        if (is_foreign && item->kind == CM_AST_ITEM_TYPE_ALIAS) {
            hir_item_kind = CM_HIR_ITEM_EXTERN_TYPE;
        } else if (!cm_lower_hir_item_kind(item, &hir_item_kind)) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM, span,
                reference.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK, "cannot map graph reserved AST item kind");
            return 0;
        }
        status = cm_hir_reserve_item_definition_as(state->hir,
            state->result.crate_id, hir_item_kind, span,
            &record.definition);
        if (status != CM_HIR_OK) {
            cm_lower_fail_hir(state, span, reference.item, status,
                "cannot reserve graph item definition");
            return 0;
        }
    }
    if (!cm_lower_reserve_enum_variant_definitions(state, item, &record)) {
        return 0;
    }
    (void)cm_vec_push(&state->item_records, &record);
    for (index = 0u; index < effective->child_count && !state->failed;
         ++index) {
        CmResolveEffectiveItem child;
        CmLowerParentKind child_parent;

        if (!cm_lower_graph_get_effective_child(state, graph, graph_module,
                effective->id, index, &child)) {
            return 0;
        }
        child_parent = item->kind == CM_AST_ITEM_TRAIT
            ? CM_LOWER_PARENT_TRAIT : CM_LOWER_PARENT_IMPL;
        if (!cm_lower_graph_reserve_effective_item(state, graph, modules,
                graph_module, ast, owner_module, record.definition,
                child_parent, 0, CM_INTERN_ID_NONE, &child)) {
            return 0;
        }
    }
    return !state->failed;
}

static int cm_lower_graph_reserve_items(CmLowerState *state,
    const CmModuleGraph *graph, const CmHirModuleMap *modules,
    CmModuleId graph_module)
{
    CmResolveModuleInfo information;
    CmHirModuleId owner_module;
    const CmAst *ast;
    uint32_t index;

    (void)cm_module_graph_get_module(graph, graph_module, &information);
    (void)cm_hir_module_map_lookup_hir(modules, graph,
        state->graph_revision, graph_module, state->hir, &owner_module);
    (void)cm_module_graph_borrow_ast(graph, graph_module, &ast);
    for (index = 0u; index < information.effective_item_count
         && !state->failed; ++index) {
        CmResolveEffectiveItem effective;

        if (!cm_lower_graph_get_effective_item(state, graph, graph_module,
                index, &effective)) {
            break;
        }
        if (effective.item_kind == CM_AST_ITEM_USE
            || effective.item_kind == CM_AST_ITEM_EXTERN_CRATE) continue;
        if (!cm_lower_graph_reserve_effective_item(state, graph, modules,
                graph_module, ast, owner_module, cm_hir_def_id_none(),
                CM_LOWER_PARENT_NONE, 0, CM_INTERN_ID_NONE, &effective)) {
            break;
        }
    }
    return !state->failed;
}

static int cm_lower_import_namespace(CmLowerState *state,
    CmResolveNamespace source, CmSpan span, CmAstItemId declaration,
    CmHirNamespace *out_namespace)
{
    switch (source) {
    case CM_RESOLVE_NAMESPACE_TYPE:
        *out_namespace = CM_HIR_NAMESPACE_TYPE;
        return 1;
    case CM_RESOLVE_NAMESPACE_VALUE:
        *out_namespace = CM_HIR_NAMESPACE_VALUE;
        return 1;
    case CM_RESOLVE_NAMESPACE_MACRO:
        *out_namespace = CM_HIR_NAMESPACE_MACRO;
        return 1;
    }
    cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
        declaration, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
        "import binding has an invalid namespace");
    return 0;
}

static int cm_lower_import_item_namespace_valid(CmAstItemKind item_kind,
    CmResolveNamespace namespace_kind)
{
    switch (item_kind) {
    case CM_AST_ITEM_FUNCTION:
    case CM_AST_ITEM_CONST:
    case CM_AST_ITEM_STATIC:
        return namespace_kind == CM_RESOLVE_NAMESPACE_VALUE;
    case CM_AST_ITEM_STRUCT:
        return namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            || namespace_kind == CM_RESOLVE_NAMESPACE_VALUE;
    case CM_AST_ITEM_UNION:
        return namespace_kind == CM_RESOLVE_NAMESPACE_TYPE;
    case CM_AST_ITEM_ENUM:
    case CM_AST_ITEM_TYPE_ALIAS:
    case CM_AST_ITEM_MODULE:
    case CM_AST_ITEM_EXTERN_CRATE:
    case CM_AST_ITEM_TRAIT:
        return namespace_kind == CM_RESOLVE_NAMESPACE_TYPE;
    case CM_AST_ITEM_MACRO:
        return namespace_kind == CM_RESOLVE_NAMESPACE_MACRO;
    case CM_AST_ITEM_EXTERN_BLOCK:
    case CM_AST_ITEM_USE:
    case CM_AST_ITEM_IMPL:
        return 0;
    }
    return 0;
}

static const CmLowerMacroRecord *cm_lower_find_macro_record(
    const CmLowerState *state, CmResolveItemRef declaration,
    uint32_t *out_matches)
{
    const CmLowerMacroRecord *result;
    size_t index;
    uint32_t matches;

    result = NULL;
    matches = 0u;
    for (index = 0u; index < state->macro_records.len; ++index) {
        const CmLowerMacroRecord *record;

        record = (const CmLowerMacroRecord *)cm_vec_at_const(
            &state->macro_records, index);
        if (record != NULL
            && cm_lower_item_ref_equal(record->declaration,
                declaration)) {
            result = record;
            matches += 1u;
        }
    }
    if (out_matches != NULL) *out_matches = matches;
    return result;
}

static int cm_lower_macro_import_target(CmLowerState *state,
    const CmResolvedBinding *binding, CmResolveItemRef import_declaration,
    CmSpan import_span, CmHirDefId *out_target)
{
    const CmLowerMacroRecord *existing;
    const CmHirDefinition *definition;
    CmResolveMacroDeclaration declaration;
    CmLowerMacroRecord record;
    CmHirModuleId owner_module;
    CmHirMacroDefinitionForm form;
    CmInternId name;
    CmHirStatus hir_status;
    uint32_t matches;

    if (binding->item_kind != CM_AST_ITEM_MACRO
        || binding->namespace_kind != CM_RESOLVE_NAMESPACE_MACRO
        || binding->target_module != CM_MODULE_NONE
        || binding->declaration.source == 0u
        || binding->declaration.item == CM_AST_ITEM_NONE) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, import_span,
            import_declaration.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK, "imported macro target has invalid provenance");
        return 0;
    }
    existing = cm_lower_find_macro_record(state, binding->declaration,
        &matches);
    if (existing != NULL && matches == 1u) {
        definition = cm_hir_lookup_definition(state->hir,
            existing->definition);
        if (definition == NULL
            || definition->kind != CM_HIR_DEFINITION_MACRO
            || definition->state != CM_HIR_DEFINITION_BOUND) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, import_span,
                import_declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_INVARIANT_VIOLATION,
                "retained macro declaration has an invalid HIR identity");
            return 0;
        }
        *out_target = existing->definition;
        return 1;
    }
    if (matches != 0u
        || cm_module_graph_get_macro_declaration(state->graph,
            state->graph_revision, binding->declaration, &declaration)
            != CM_RESOLVE_VIEW_OK
        || !cm_lower_item_ref_equal(declaration.declaration,
            binding->declaration)
        || declaration.owner_module == CM_MODULE_NONE
        || (declaration.form != CM_AST_MACRO_RULES_DEFINITION
            && declaration.form
                != CM_AST_MACRO_DECLARATIVE_DEFINITION)
        || cm_hir_module_map_lookup_hir(state->module_map, state->graph,
            state->graph_revision, declaration.owner_module, state->hir,
            &owner_module) != CM_HIR_MODULE_MAP_OK) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, import_span,
            import_declaration.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK,
            "imported macro declaration has no canonical graph identity");
        return 0;
    }
    name = cm_lower_copy_graph_interned(state, state->graph,
        declaration.name, declaration.span, binding->declaration.item,
        "macro declaration has an invalid graph-owned name",
        "cannot copy graph-owned macro declaration name");
    if (state->failed) return 0;
    form = declaration.form == CM_AST_MACRO_RULES_DEFINITION
        ? CM_HIR_MACRO_RULES_DEFINITION
        : CM_HIR_MACRO_DECLARATIVE_DEFINITION;
    hir_status = cm_hir_add_macro_definition(state->hir, owner_module,
        name, form, declaration.span, &record.definition);
    if (hir_status != CM_HIR_OK) {
        cm_lower_fail_hir(state, declaration.span,
            binding->declaration.item, hir_status,
            "cannot retain macro definition identity");
        return 0;
    }
    record.declaration = binding->declaration;
    (void)cm_vec_push(&state->macro_records, &record);
    *out_target = record.definition;
    return 1;
}

static int cm_lower_import_target(CmLowerState *state,
    const CmResolvedBinding *binding, CmModuleId owner_graph_module,
    CmResolveItemRef import_declaration, CmSpan span,
    CmHirDefId *out_target)
{
    const CmLowerItemRecord *record;
    const CmAstItem *target_ast_item;
    uint32_t matches;

    *out_target = cm_hir_def_id_none();
    if (binding->revision != state->graph_revision
        || binding->module != owner_graph_module
        || !cm_lower_item_ref_equal(binding->import_declaration,
            import_declaration)
        || !binding->is_import || binding->is_ambiguous
        || (binding->is_anonymous != 0 && binding->is_anonymous != 1)) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            import_declaration.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK,
            "resolver returned an inconsistent declaration binding");
        return 0;
    }
    if (binding->primitive_kind != CM_RESOLVE_PRIMITIVE_NONE) {
        if (binding->namespace_kind != CM_RESOLVE_NAMESPACE_TYPE
            || binding->target_module != CM_MODULE_NONE
            || binding->declaration.source != 0u
            || binding->declaration.item != CM_AST_ITEM_NONE
            || binding->variant.enumeration.source != 0u
            || binding->variant.enumeration.item != CM_AST_ITEM_NONE
            || !cm_lower_resolved_primitive(binding->primitive_kind,
                NULL, NULL)) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                import_declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "builtin primitive import has invalid provenance");
            return 0;
        }
        return 1;
    }
    if (binding->variant.enumeration.source != 0u
        || binding->variant.enumeration.item != CM_AST_ITEM_NONE) {
        const CmAstItem *enumeration;
        const CmAstVariant *variant;
        const CmLowerVariantRecord *variant_record;
        const CmHirDefinition *variant_definition;
        uint32_t variant_matches;

        if (!cm_lower_item_ref_equal(binding->declaration,
                binding->variant.enumeration)
            || binding->item_kind != CM_AST_ITEM_ENUM
            || binding->target_module != CM_MODULE_NONE) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                import_declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "enum variant import has invalid resolver provenance");
            return 0;
        }
        record = cm_lower_find_graph_declaration(state,
            binding->variant.enumeration, &matches);
        enumeration = record == NULL ? NULL
            : cm_ast_get_item(record->ast, record->ast_id);
        variant_record = record == NULL ? NULL
            : cm_lower_find_variant_record(state, record->definition,
                binding->variant.index, &variant_matches);
        if (record == NULL || matches != 1u
            || record->kind != CM_AST_ITEM_ENUM
            || enumeration == NULL
            || enumeration->kind != CM_AST_ITEM_ENUM
            || binding->variant.index
                >= enumeration->data.enum_item.variant_count
            || variant_record == NULL || variant_matches != 1u) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                import_declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "enum variant declaration has no reserved HIR identity");
            return 0;
        }
        variant = &enumeration->data.enum_item.variants[
            binding->variant.index];
        variant_definition = cm_hir_lookup_definition(state->hir,
            variant_record->definition);
        if (variant_definition == NULL
            || variant_definition->kind
                != CM_HIR_DEFINITION_ENUM_VARIANT
            || (binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
                && variant->form == CM_AST_FIELDS_NAMED)
            || (binding->namespace_kind != CM_RESOLVE_NAMESPACE_TYPE
                && binding->namespace_kind != CM_RESOLVE_NAMESPACE_VALUE)
            || (variant_definition->state != CM_HIR_DEFINITION_RESERVED
                && variant_definition->state != CM_HIR_DEFINITION_BOUND)) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                import_declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "enum variant import does not match its canonical HIR "
                "identity");
            return 0;
        }
        *out_target = variant_record->definition;
        return 1;
    }
    if (binding->item_kind == CM_AST_ITEM_MODULE) {
        CmHirModuleId target_module_id;
        const CmHirModule *target_module;

        if (binding->namespace_kind != CM_RESOLVE_NAMESPACE_TYPE
            || binding->target_module == CM_MODULE_NONE
            || cm_hir_module_map_lookup_hir(state->module_map,
                state->graph, state->graph_revision,
                binding->target_module, state->hir,
                &target_module_id) != CM_HIR_MODULE_MAP_OK
            || (target_module = cm_hir_get_module(state->hir,
                target_module_id)) == NULL
            || target_module->crate_id != state->result.crate_id) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                import_declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "imported module target cannot be mapped into HIR");
            return 0;
        }
        if (binding->declaration.source != 0u
            || binding->declaration.item != CM_AST_ITEM_NONE) {
            record = cm_lower_find_graph_declaration(state,
                binding->declaration, &matches);
            if (record == NULL || matches != 1u
                || record->kind != CM_AST_ITEM_MODULE
                || record->nested_module != target_module_id
                || !cm_hir_def_id_equal(record->definition,
                    target_module->definition)) {
                cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                    import_declaration.item, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "imported module declaration does not match its HIR "
                    "target");
                return 0;
            }
        }
        *out_target = target_module->definition;
        return 1;
    }
    if (binding->item_kind == CM_AST_ITEM_MACRO) {
        return cm_lower_macro_import_target(state, binding,
            import_declaration, span, out_target);
    }
    if (binding->target_module != CM_MODULE_NONE
        || binding->declaration.source == 0u
        || binding->declaration.item == CM_AST_ITEM_NONE
        || !cm_lower_import_item_namespace_valid(binding->item_kind,
            binding->namespace_kind)) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            import_declaration.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK, "imported item target has invalid provenance");
        return 0;
    }
    record = cm_lower_find_graph_declaration(state, binding->declaration,
        &matches);
    if (record == NULL || matches != 1u
        || record->kind != binding->item_kind
        || cm_hir_def_id_is_none(record->definition)
        || cm_hir_lookup_definition(state->hir, record->definition)
            == NULL) {
        char binding_name[96];

        if (!cm_import_copy_string(state->imports, binding->name,
                binding_name, sizeof(binding_name))) {
            binding_name[0] = '?';
            binding_name[1] = '\0';
        }
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            import_declaration.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK,
            "imported item declaration cannot be mapped into HIR "
            "(name=%s kind=%u source=%u item=%u matches=%u)",
            binding_name, (unsigned int)binding->item_kind,
            (unsigned int)binding->declaration.source,
            (unsigned int)binding->declaration.item,
            (unsigned int)matches);
        return 0;
    }
    if (binding->item_kind == CM_AST_ITEM_STRUCT
        && binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE) {
        target_ast_item = cm_ast_get_item(record->ast, record->ast_id);
        if (target_ast_item == NULL
            || target_ast_item->data.aggregate_item.form
                == CM_AST_FIELDS_NAMED) {
            cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
                import_declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "named-field struct import has a fictitious value binding");
            return 0;
        }
    }
    *out_target = record->definition;
    return 1;
}

static void cm_lower_free_imports(CmHirImport *imports,
    uint32_t import_count)
{
    uint32_t index;

    for (index = 0u; index < import_count; ++index) {
        cm_free(imports[index].bindings);
        cm_free(imports[index].attributes);
    }
    cm_free(imports);
}

static int cm_lower_library_import_leaf(CmLowerState *state,
    CmModuleId graph_module, CmResolveItemRef declaration,
    const CmImportLeafView *leaf, CmHirLibraryBinding *out_binding)
{
    size_t name_length;
    unsigned char *name;
    int resolved;

    if (out_binding != NULL)
        memset(out_binding, 0, sizeof(*out_binding));
    if (state == NULL || leaf == NULL || state->graph == NULL
        || leaf->revision != state->graph_revision
        || leaf->module != graph_module
        || !cm_lower_item_ref_equal(leaf->declaration, declaration)
        || leaf->is_resolved || leaf->is_glob || leaf->is_anonymous
        || leaf->import_name == CM_RESOLVE_STRING_NONE) return 0;
    name_length = cm_import_string_length(state->imports, leaf->import_name);
    if (name_length == 0u || name_length == SIZE_MAX) return 0;
    name = (unsigned char *)cm_alloc(name_length + 1u);
    if (!cm_import_copy_string(state->imports, leaf->import_name,
            (char *)name, name_length + 1u)) {
        cm_free(name);
        return 0;
    }
    resolved = cm_lower_resolve_library_import(state->graph,
        state->graph_revision, state->imports, state->options, graph_module,
        declaration, name, name_length, out_binding);
    cm_free(name);
    return resolved;
}

static size_t cm_lower_library_import_binding_count(CmLowerState *state,
    CmModuleId graph_module, CmResolveItemRef declaration)
{
    size_t leaf_count;
    size_t leaf_index;
    size_t count;

    leaf_count = cm_import_leaf_count(state->imports);
    count = 0u;
    for (leaf_index = 0u; leaf_index < leaf_count; ++leaf_index) {
        CmImportLeafView leaf;

        if (leaf_index > (size_t)UINT32_MAX
            || !cm_import_get_leaf(state->imports, (uint32_t)leaf_index,
                &leaf)) return SIZE_MAX;
        if (cm_lower_library_import_leaf(state, graph_module, declaration,
                &leaf, NULL)) count += 1u;
    }
    return count;
}

static int cm_lower_store_resolved_import_binding(CmLowerState *state,
    const CmResolvedBinding *binding, CmModuleId graph_module,
    CmResolveItemRef declaration, CmSpan span,
    CmHirImportBinding *hir_binding)
{
    hir_binding->name = cm_lower_copy_import_string(state, binding->name,
        span, declaration.item);
    hir_binding->is_anonymous = binding->is_anonymous;
    if (binding->primitive_kind != CM_RESOLVE_PRIMITIVE_NONE
        && !cm_lower_resolved_primitive(binding->primitive_kind,
            &hir_binding->primitive_kind, NULL)) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            declaration.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK, "resolver returned an invalid primitive identity");
    }
    return !state->failed
        && cm_lower_import_namespace(state, binding->namespace_kind, span,
            declaration.item, &hir_binding->namespace_kind)
        && cm_lower_import_target(state, binding, graph_module, declaration,
            span, &hir_binding->target);
}

static int cm_lower_store_library_import_binding(CmLowerState *state,
    const CmImportLeafView *leaf, CmHirLibraryBinding imported_binding,
    CmSpan span, CmResolveItemRef declaration,
    CmHirImportBinding *hir_binding)
{
    int valid_target;

    valid_target = 0;
    if (imported_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
        CmHirLowerResolution resolution;

        memset(&resolution, 0, sizeof(resolution));
        resolution.kind = CM_HIR_LOWER_DEFINITION;
        resolution.definition = imported_binding.definition;
        resolution.named_type_kind = imported_binding.type_kind;
        valid_target = cm_lower_external_definition_matches(state,
            &resolution);
    } else if (imported_binding.kind == CM_HIR_LIBRARY_BINDING_MODULE) {
        const CmHirDefinition *definition;

        definition = cm_hir_lookup_definition(state->hir,
            imported_binding.definition);
        valid_target = definition != NULL
            && definition->kind == CM_HIR_DEFINITION_MODULE
            && definition->state == CM_HIR_DEFINITION_BOUND
            && cm_hir_get_module(state->hir,
                definition->entity.module_id) != NULL;
    } else if (imported_binding.kind == CM_HIR_LIBRARY_BINDING_TRAIT) {
        const CmHirItem *item;

        item = cm_lower_bound_item(state, imported_binding.definition);
        valid_target = item != NULL && item->kind == CM_HIR_ITEM_TRAIT;
    } else if (imported_binding.kind
            == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
        valid_target = cm_hir_def_id_is_none(imported_binding.definition)
            && cm_lower_hir_primitive(imported_binding.primitive_kind,
                NULL);
    }
    if (!valid_target) {
        cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE, span,
            declaration.item, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "authenticated dependency import has an invalid HIR target");
        return 0;
    }
    hir_binding->name = cm_lower_copy_import_string(state,
        leaf->import_name, span, declaration.item);
    hir_binding->namespace_kind = CM_HIR_NAMESPACE_TYPE;
    hir_binding->target = imported_binding.definition;
    hir_binding->primitive_kind = imported_binding.primitive_kind;
    hir_binding->is_anonymous = 0;
    return !state->failed;
}

static int cm_lower_graph_apply_imports(CmLowerState *state,
    const CmModuleGraph *graph, const CmHirModuleMap *modules,
    const CmVec *traversal)
{
    size_t module_index;

    for (module_index = 0u; module_index < traversal->len
         && !state->failed; ++module_index) {
        CmModuleId graph_module;
        CmResolveModuleInfo information;
        CmHirModuleId hir_module;
        const CmAst *ast;
        CmHirImport *imports;
        uint32_t import_index;
        uint32_t use_import_count;
        uint32_t effective_index;
        size_t structural_import_count;
        CmHirStatus hir_status;

        graph_module = *(const CmModuleId *)cm_vec_at_const(traversal,
            module_index);
        memset(&information, 0, sizeof(information));
        if (!cm_module_graph_get_module(graph, graph_module, &information)
            || !cm_module_graph_borrow_ast(graph, graph_module, &ast)
            || cm_hir_module_map_lookup_hir(modules, graph,
                state->graph_revision, graph_module, state->hir,
                &hir_module) != CM_HIR_MODULE_MAP_OK) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                (CmSpan){ information.source, 0u, 0u },
                information.declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_INVARIANT_VIOLATION,
                "cannot map module while lowering structural imports");
            break;
        }
        structural_import_count = (size_t)information.import_count;
        for (effective_index = 0u;
             effective_index < information.effective_item_count
             && !state->failed; ++effective_index) {
            CmResolveEffectiveItem effective;

            if (!cm_lower_graph_get_effective_item(state, graph,
                    graph_module, effective_index, &effective)) break;
            if (effective.item_kind == CM_AST_ITEM_EXTERN_CRATE
                && structural_import_count < (size_t)UINT32_MAX) {
                structural_import_count += 1u;
            } else if (effective.item_kind == CM_AST_ITEM_EXTERN_CRATE) {
                cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                    effective.span, effective.declaration.item,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_ID_EXHAUSTED,
                    "structural module import count exceeds HIR IDs");
            }
        }
        if (structural_import_count > (size_t)UINT32_MAX) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                (CmSpan){ information.source, 0u, 0u },
                information.declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_ID_EXHAUSTED,
                "structural module import count exceeds HIR IDs");
        }
        imports = structural_import_count == 0u ? NULL
            : (CmHirImport *)cm_alloc_zeroed(
                structural_import_count, sizeof(CmHirImport));
        import_index = 0u;
        use_import_count = 0u;
        for (effective_index = 0u;
             effective_index < information.effective_item_count
             && !state->failed; ++effective_index) {
            CmResolveEffectiveItem effective;
            const CmAstItem *ast_item;
            CmResolveImport graph_import;
            CmHirImport *hir_import;
            CmLowerItemRecord attribute_record;
            size_t binding_count;
            size_t resolved_binding_count;
            size_t library_binding_count;
            size_t binding_index;
            size_t resolved_binding_index;
            size_t leaf_count;
            size_t leaf_index;

            if (!cm_lower_graph_get_effective_item(state, graph,
                    graph_module, effective_index, &effective)) {
                break;
            }
            if (effective.item_kind != CM_AST_ITEM_USE
                && effective.item_kind != CM_AST_ITEM_EXTERN_CRATE) continue;
            state->ast = ast;
            state->source = effective.declaration.source;
            state->graph_module = graph_module;
            if ((size_t)import_index >= structural_import_count
                || (ast_item = cm_ast_get_item(ast,
                    effective.declaration.item)) == NULL
                || ast_item->kind != effective.item_kind) {
                if (!state->failed) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                        effective.span, effective.declaration.item,
                        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                        "effective import has no structural HIR slot");
                }
                break;
            }
            hir_import = &imports[import_index];
            hir_import->kind = effective.item_kind == CM_AST_ITEM_USE
                ? CM_HIR_IMPORT_USE : CM_HIR_IMPORT_EXTERN_CRATE;
            if (hir_import->kind == CM_HIR_IMPORT_USE) {
                if (!cm_lower_graph_import_for_declaration(state, graph,
                        graph_module, effective.declaration, ast_item,
                        &graph_import)) break;
                hir_import->tree = cm_lower_copy_graph_interned(state, graph,
                    graph_import.tree, effective.span,
                    effective.declaration.item,
                    "import has an invalid graph-owned use tree",
                    "cannot copy graph-owned import use tree");
                use_import_count += 1u;
            } else {
                hir_import->tree = cm_lower_copy_string(state,
                    ast_item->name, effective.span,
                    effective.declaration.item);
            }
            hir_import->span = effective.span;
            hir_import->source_item = effective.declaration.item;
            if (state->failed
                || !cm_lower_visibility(state, ast_item->visibility,
                    hir_module, effective.span,
                    effective.declaration.item,
                    &hir_import->visibility)) {
                break;
            }
            memset(&attribute_record, 0, sizeof(attribute_record));
            attribute_record.source = effective.declaration.source;
            attribute_record.graph_module = graph_module;
            attribute_record.graph_effective_item = effective.id;
            attribute_record.effective_attribute_count =
                effective.attribute_count;
            hir_import->attributes = cm_lower_item_attributes(state,
                &attribute_record, effective.span,
                effective.declaration.item,
                &hir_import->attribute_count);
            if (state->failed) break;
            if (hir_import->kind == CM_HIR_IMPORT_EXTERN_CRATE) {
                const CmHirModule *root_module;
                CmHirImportBinding *binding;

                if (!cm_lower_string_is(state, ast_item->name, "self")
                    || ast_item->data.extern_crate_item.alias
                        == CM_INTERN_ID_NONE) {
                    cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_ITEM,
                        effective.span, effective.declaration.item,
                        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                        "graph HIR currently supports only aliased "
                        "extern crate self declarations");
                    break;
                }
                root_module = cm_hir_get_module(state->hir,
                    state->result.root_module);
                if (root_module == NULL) {
                    cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                        effective.span, effective.declaration.item,
                        CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                        CM_HIR_INVARIANT_VIOLATION,
                        "extern crate self has no HIR crate root");
                    break;
                }
                hir_import->binding_count = 1u;
                hir_import->bindings = (CmHirImportBinding *)
                    cm_alloc_zeroed(1u, sizeof(CmHirImportBinding));
                binding = &hir_import->bindings[0];
                binding->name = cm_lower_copy_string(state,
                    ast_item->data.extern_crate_item.alias,
                    effective.span, effective.declaration.item);
                binding->namespace_kind = CM_HIR_NAMESPACE_TYPE;
                binding->target = root_module->definition;
                if (state->failed) break;
                import_index += 1u;
                continue;
            }
            resolved_binding_count = cm_import_declaration_binding_count(
                state->imports, graph_module, effective.declaration);
            library_binding_count = cm_lower_library_import_binding_count(
                state, graph_module, effective.declaration);
            if (library_binding_count == SIZE_MAX
                || resolved_binding_count > (size_t)UINT32_MAX
                || library_binding_count > (size_t)UINT32_MAX
                || resolved_binding_count
                    > (size_t)UINT32_MAX - library_binding_count) {
                cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                    effective.span, effective.declaration.item,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_ID_EXHAUSTED,
                    "structural import binding count exceeds HIR IDs");
                break;
            }
            binding_count = resolved_binding_count + library_binding_count;
            hir_import->binding_count = (uint32_t)binding_count;
            hir_import->bindings = binding_count == 0u ? NULL
                : (CmHirImportBinding *)cm_alloc_zeroed(binding_count,
                    sizeof(CmHirImportBinding));
            binding_index = 0u;
            resolved_binding_index = 0u;
            leaf_count = cm_import_leaf_count(state->imports);
            for (leaf_index = 0u; leaf_index < leaf_count
                    && !state->failed; ++leaf_index) {
                CmImportLeafView leaf;
                size_t leaf_binding_index;

                if (leaf_index > (size_t)UINT32_MAX
                    || !cm_import_get_leaf(state->imports,
                        (uint32_t)leaf_index, &leaf)) {
                    cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE,
                        effective.span, effective.declaration.item,
                        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                        "cannot read resolver-owned import leaf");
                    break;
                }
                if (leaf.module != graph_module
                    || !cm_lower_item_ref_equal(leaf.declaration,
                        effective.declaration)) continue;
                if (leaf.is_resolved) {
                    for (leaf_binding_index = 0u;
                            leaf_binding_index < leaf.binding_count
                            && !state->failed; ++leaf_binding_index) {
                        CmResolvedBinding binding;

                        memset(&binding, 0, sizeof(binding));
                        if (resolved_binding_index
                                >= resolved_binding_count
                            || binding_index >= binding_count
                            || !cm_import_get_declaration_binding(
                                state->imports, graph_module,
                                effective.declaration,
                                (uint32_t)resolved_binding_index,
                                &binding)
                            || !cm_lower_store_resolved_import_binding(state,
                                &binding, graph_module,
                                effective.declaration, effective.span,
                                &hir_import->bindings[binding_index])) {
                            if (!state->failed) {
                                cm_lower_fail(state,
                                    CM_HIR_LOWER_RESOLVER_FAILURE,
                                    effective.span,
                                    effective.declaration.item,
                                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                                    CM_HIR_OK,
                                    "cannot retain resolver-owned import "
                                    "binding");
                            }
                            break;
                        }
                        resolved_binding_index += 1u;
                        binding_index += 1u;
                    }
                } else {
                    CmHirLibraryBinding imported_binding;

                    memset(&imported_binding, 0,
                        sizeof(imported_binding));
                    if (cm_lower_library_import_leaf(state, graph_module,
                            effective.declaration, &leaf,
                            &imported_binding)) {
                        if (binding_index >= binding_count
                            || !cm_lower_store_library_import_binding(state,
                                &leaf, imported_binding, effective.span,
                                effective.declaration,
                                &hir_import->bindings[binding_index])) break;
                        binding_index += 1u;
                    }
                }
            }
            if (!state->failed
                && (binding_index != binding_count
                    || resolved_binding_index != resolved_binding_count)) {
                cm_lower_fail(state, CM_HIR_LOWER_RESOLVER_FAILURE,
                    effective.span, effective.declaration.item,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "import leaf order does not match retained bindings");
            }
            if (state->failed) break;
            import_index += 1u;
        }
        if (!state->failed
            && ((size_t)import_index != structural_import_count
                || use_import_count != information.import_count)) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                (CmSpan){ information.source, 0u, 0u },
                information.declaration.item, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_INVARIANT_VIOLATION,
                "module import count differs from effective use items");
        }
        if (!state->failed) {
            hir_status = cm_hir_set_module_imports(state->hir, hir_module,
                imports, (uint32_t)structural_import_count);
            if (hir_status != CM_HIR_OK) {
                cm_lower_fail_hir(state,
                    (CmSpan){ information.source, 0u, 0u },
                    information.declaration.item, hir_status,
                    "cannot store structural module imports");
            }
        }
        cm_lower_free_imports(imports,
            (uint32_t)structural_import_count);
    }
    return !state->failed;
}

static int cm_lower_record_in_phase(const CmLowerItemRecord *record,
    CmLowerPhase phase)
{
    if (phase == CM_LOWER_PHASE_TRAIT_HEADER) {
        return record->kind == CM_AST_ITEM_TRAIT
            && record->parent_kind == CM_LOWER_PARENT_NONE;
    }
    if (phase == CM_LOWER_PHASE_TRAIT_ASSOCIATED_TYPES) {
        return record->kind == CM_AST_ITEM_TYPE_ALIAS
            && record->parent_kind == CM_LOWER_PARENT_TRAIT;
    }
    if (phase == CM_LOWER_PHASE_TRAIT_ASSOCIATED_CONSTS) {
        return record->kind == CM_AST_ITEM_CONST
            && record->parent_kind == CM_LOWER_PARENT_TRAIT;
    }
    if (phase == CM_LOWER_PHASE_TRAIT_METHODS) {
        return record->kind == CM_AST_ITEM_FUNCTION
            && record->parent_kind == CM_LOWER_PARENT_TRAIT;
    }
    if (phase == CM_LOWER_PHASE_IMPL_HEADER) {
        return record->kind == CM_AST_ITEM_IMPL
            && record->parent_kind == CM_LOWER_PARENT_NONE;
    }
    if (phase == CM_LOWER_PHASE_OTHER_ROOT) {
        return record->parent_kind == CM_LOWER_PARENT_NONE
            && record->kind != CM_AST_ITEM_TRAIT
            && record->kind != CM_AST_ITEM_IMPL;
    }
    if (phase == CM_LOWER_PHASE_IMPL_ASSOCIATED_TYPES) {
        return record->kind == CM_AST_ITEM_TYPE_ALIAS
            && record->parent_kind == CM_LOWER_PARENT_IMPL;
    }
    if (phase == CM_LOWER_PHASE_IMPL_ASSOCIATED_CONSTS) {
        return record->kind == CM_AST_ITEM_CONST
            && record->parent_kind == CM_LOWER_PARENT_IMPL;
    }
    if (phase == CM_LOWER_PHASE_IMPL_METHODS) {
        return record->kind == CM_AST_ITEM_FUNCTION
            && record->parent_kind == CM_LOWER_PARENT_IMPL;
    }
    return 0;
}

static int cm_lower_records_in_phase(CmLowerState *state,
    CmLowerPhase phase)
{
    size_t index;

    for (index = 0u; index < state->item_records.len && !state->failed;
         ++index) {
        const CmLowerItemRecord *record;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if (record == NULL) return 0;
        if (cm_lower_record_in_phase(record, phase)
            && !cm_lower_one_record(state, record)) {
            return 0;
        }
    }
    return !state->failed;
}

static int cm_lower_prebind_projection_declarations(CmLowerState *state)
{
    size_t index;

    for (index = 0u; index < state->item_records.len && !state->failed;
         ++index) {
        const CmLowerItemRecord *record;
        const CmAstItem *ast_item;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if (record == NULL) return 0;
        if (!cm_lower_record_in_phase(record,
                CM_LOWER_PHASE_TRAIT_ASSOCIATED_TYPES)
            || record->generic_parameter_count != 0u) {
            continue;
        }
        ast_item = cm_ast_get_item(record->ast, record->ast_id);
        if (ast_item == NULL) {
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_AST,
                record->effective_span, record->ast_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_INVALID_ID,
                "associated-type prebinding lost its AST declaration");
            return 0;
        }
        if (!cm_lower_prebind_associated_type_record(state, record)) {
            return 0;
        }
    }
    return !state->failed;
}

static const CmLowerItemRecord *cm_lower_record_for_definition(
    const CmLowerState *state, CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < state->item_records.len; ++index) {
        const CmLowerItemRecord *record;

        record = (const CmLowerItemRecord *)cm_vec_at_const(
            &state->item_records, index);
        if (record != NULL
            && cm_hir_def_id_equal(record->definition, definition)) {
            return record;
        }
    }
    return NULL;
}

typedef struct CmLowerSupertraitFrame {
    const CmHirItem *trait_item;
    size_t mark_index;
    uint32_t next_supertrait;
} CmLowerSupertraitFrame;

static int cm_lower_trait_mark_index(CmLowerState *state,
    const CmHirItem *trait_item, size_t *out_index)
{
    const CmHirDefinition *definition;
    const CmLowerItemRecord *record;

    definition = cm_hir_lookup_definition(state->hir,
        trait_item->definition);
    record = cm_lower_record_for_definition(state, trait_item->definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND
        || definition->entity.item_id == CM_HIR_ITEM_NONE
        || (size_t)definition->entity.item_id > state->hir->items.len) {
        cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, trait_item->span,
            record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
            CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_INVARIANT_VIOLATION,
            "bound trait has no stable HIR item identity");
        return 0;
    }
    *out_index = (size_t)definition->entity.item_id - 1u;
    return 1;
}

static int cm_lower_validate_supertraits(CmLowerState *state)
{
    unsigned char *marks;
    CmVec stack;
    size_t index;

    marks = (unsigned char *)cm_alloc_zeroed(
        state->hir->items.len == 0u ? 1u : state->hir->items.len,
        sizeof(unsigned char));
    cm_vec_init(&stack, sizeof(CmLowerSupertraitFrame));
    for (index = 0u; index < state->hir->items.len && !state->failed;
         ++index) {
        const CmHirItem *item;
        CmLowerSupertraitFrame root;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            index);
        if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT
            || item->definition.crate_id != state->result.crate_id) {
            continue;
        }
        memset(&root, 0, sizeof(root));
        root.trait_item = item;
        if (!cm_lower_trait_mark_index(state, item, &root.mark_index)) break;
        if (marks[root.mark_index] == 2u) continue;
        if (marks[root.mark_index] != 0u) {
            cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, item->span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_INVARIANT_VIOLATION,
                "trait traversal mark is inconsistent");
            break;
        }
        marks[root.mark_index] = 1u;
        (void)cm_vec_push(&stack, &root);
        while (stack.len != 0u && !state->failed) {
            CmLowerSupertraitFrame *frame;
            const CmLowerItemRecord *record;
            const CmHirSupertrait *supertrait;
            const CmHirItem *target;
            CmLowerSupertraitFrame child;
            uint32_t edge_index;
            uint32_t prior;

            frame = (CmLowerSupertraitFrame *)cm_vec_at(&stack,
                stack.len - 1u);
            if (frame == NULL) {
                cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, item->span,
                    CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_INVARIANT_VIOLATION,
                    "trait traversal stack is inconsistent");
                break;
            }
            if (frame->next_supertrait
                >= frame->trait_item->data.trait_item.supertrait_count) {
                marks[frame->mark_index] = 2u;
                (void)cm_vec_pop(&stack, NULL);
                continue;
            }
            edge_index = frame->next_supertrait;
            frame->next_supertrait += 1u;
            supertrait = &frame->trait_item->data.trait_item
                .supertraits[edge_index];
            record = cm_lower_record_for_definition(state,
                frame->trait_item->definition);
            target = cm_lower_bound_item(state,
                supertrait->trait_type.definition);
            if (target == NULL || (target->kind != CM_HIR_ITEM_TRAIT
                    && target->kind != CM_HIR_ITEM_TRAIT_ALIAS)) {
                cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE,
                    supertrait->span,
                    record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "supertrait target is not a bound trait");
                break;
            }
            for (prior = 0u; prior < edge_index; ++prior) {
                if (cm_hir_def_id_equal(frame->trait_item->data.trait_item
                            .supertraits[prior].trait_type.definition,
                        supertrait->trait_type.definition)) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                        supertrait->span,
                        record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                        CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                        "trait repeats the same direct supertrait");
                    break;
                }
            }
            if (state->failed) break;
            if (target->kind == CM_HIR_ITEM_TRAIT_ALIAS) continue;
            memset(&child, 0, sizeof(child));
            child.trait_item = target;
            if (!cm_lower_trait_mark_index(state, target,
                    &child.mark_index)) {
                break;
            }
            if (marks[child.mark_index] == 1u) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                    supertrait->span,
                    record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "trait supertrait graph contains a cycle");
                break;
            }
            if (marks[child.mark_index] == 0u) {
                marks[child.mark_index] = 1u;
                (void)cm_vec_push(&stack, &child);
            }
        }
        cm_vec_clear(&stack);
    }
    cm_vec_destroy(&stack);
    cm_free(marks);
    return !state->failed;
}

static int cm_lower_validate_trait_aliases(CmLowerState *state)
{
    size_t item_index;

    for (item_index = 0u; item_index < state->hir->items.len
         && !state->failed; ++item_index) {
        const CmHirItem *item;
        const CmLowerItemRecord *record;
        uint32_t bound_index;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            item_index);
        if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT_ALIAS
            || item->definition.crate_id != state->result.crate_id) {
            continue;
        }
        record = cm_lower_record_for_definition(state, item->definition);
        for (bound_index = 0u;
             bound_index < item->data.trait_alias_item.bound_count
                && !state->failed;
             ++bound_index) {
            const CmHirTraitAliasBound *bound;
            const CmHirItem *target;
            bound = &item->data.trait_alias_item.bounds[bound_index];
            if (bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME) continue;
            if (bound->kind != CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) {
                cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE, bound->span,
                    record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                    CM_HIR_INVARIANT_VIOLATION,
                    "trait alias has an invalid bound kind");
                break;
            }
            target = cm_lower_bound_item(state,
                bound->data.trait_bound.trait_type.definition);
            if (target == NULL || (target->kind != CM_HIR_ITEM_TRAIT
                    && target->kind != CM_HIR_ITEM_TRAIT_ALIAS)) {
                cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE,
                    bound->span,
                    record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "trait-alias bound target is not a bound trait or "
                    "trait alias");
                break;
            }
            if (cm_lower_supertrait_reaches_definition(state,
                    target->definition, item->definition)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                    bound->span,
                    record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                    CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
                    "trait-alias bound graph contains a cycle");
                break;
            }
        }
    }
    return !state->failed;
}

static int cm_lower_validate_associated_type_bounds(CmLowerState *state)
{
    size_t item_index;

    for (item_index = 0u; item_index < state->hir->items.len
         && !state->failed; ++item_index) {
        const CmHirItem *item;
        const CmLowerItemRecord *record;
        const CmAstItem *ast_item;
        uint32_t bound_index;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            item_index);
        if (item == NULL || item->kind != CM_HIR_ITEM_TYPE_ALIAS
            || item->data.type_alias_item.target != CM_HIR_TYPE_NONE
            || item->data.type_alias_item.bound_count == 0u
            || item->definition.crate_id != state->result.crate_id) {
            continue;
        }
        record = cm_lower_record_for_definition(state, item->definition);
        ast_item = record == NULL ? NULL
            : cm_ast_get_item(record->ast, record->ast_id);
        for (bound_index = 0u;
             bound_index < item->data.type_alias_item.bound_count
                && !state->failed;
             ++bound_index) {
            const CmHirAssociatedTypeBound *bound;
            const CmHirItem *target_trait;
            CmAstTypeId ast_type_id;
            CmAstPathId ast_path_id;
            uint32_t argument_index;
            uint32_t equality_index;
            uint32_t prior;

            bound = &item->data.type_alias_item.bounds[bound_index];
            ast_type_id = ast_item != NULL
                    && bound_index < ast_item->data.value_item.bound_count
                ? ast_item->data.value_item.bounds[bound_index].trait_type
                : CM_AST_TYPE_NONE;
            ast_path_id = CM_AST_PATH_NONE;
            if (ast_item != NULL && ast_type_id != CM_AST_TYPE_NONE) {
                const CmAstType *ast_type;

                ast_type = cm_ast_get_type(record->ast, ast_type_id);
                if (ast_type != NULL) ast_path_id = ast_type->path;
            }
            target_trait = cm_lower_bound_item(state,
                bound->trait_type.definition);
            if (target_trait == NULL
                || target_trait->kind != CM_HIR_ITEM_TRAIT) {
                cm_lower_fail(state, CM_HIR_LOWER_WRONG_NAMESPACE,
                    bound->span,
                    record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                    ast_type_id, ast_path_id, CM_HIR_OK,
                    "associated-type bound target is not a bound trait");
                break;
            }
            if (bound->trait_type.argument_count
                    != target_trait->generic_parameter_count
                || (bound->trait_type.argument_count != 0u
                    && bound->trait_type.arguments == NULL)
                || (bound->trait_type.argument_count == 0u
                    && bound->trait_type.arguments != NULL)) {
                cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                    bound->span,
                    record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                    ast_type_id, ast_path_id,
                    CM_HIR_INVARIANT_VIOLATION,
                    "associated-type bound argument signature does not "
                    "match its bound trait");
                break;
            }
            for (argument_index = 0u;
                 argument_index < bound->trait_type.argument_count;
                 ++argument_index) {
                const CmHirGenericArg *argument;
                const CmHirGenericParam *parameter;
                int kind_matches;

                argument = &bound->trait_type.arguments[argument_index];
                parameter = cm_hir_get_generic_param(state->hir,
                    target_trait->generic_parameter_start
                        + argument_index);
                kind_matches = parameter != NULL
                    && ((parameter->kind == CM_HIR_GENERIC_LIFETIME
                            && argument->kind
                                == CM_HIR_GENERIC_ARG_LIFETIME)
                        || (parameter->kind == CM_HIR_GENERIC_TYPE
                            && argument->kind
                                == CM_HIR_GENERIC_ARG_TYPE)
                        || (parameter->kind == CM_HIR_GENERIC_CONST
                            && argument->kind
                                == CM_HIR_GENERIC_ARG_CONST));
                if (parameter == NULL
                    || parameter->index != argument_index
                    || !cm_hir_def_id_equal(parameter->owner,
                        target_trait->definition)
                    || !kind_matches) {
                    cm_lower_fail(state, CM_HIR_LOWER_HIR_FAILURE,
                        bound->span,
                        record == NULL ? CM_AST_ITEM_NONE
                            : record->ast_id,
                        ast_type_id, ast_path_id,
                        CM_HIR_INVARIANT_VIOLATION,
                        "associated-type bound argument kind or owner "
                        "does not match its bound trait parameter");
                    break;
                }
            }
            if (state->failed) break;
            for (prior = 0u; prior < bound_index; ++prior) {
                if (cm_hir_def_id_equal(item->data.type_alias_item
                            .bounds[prior].trait_type.definition,
                        bound->trait_type.definition)) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                        bound->span,
                        record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                        ast_type_id, ast_path_id, CM_HIR_OK,
                        "duplicate associated type bound targets the same "
                        "trait");
                    break;
                }
            }
            if (state->failed) break;
            if (bound->modifier == CM_HIR_ASSOC_BOUND_RELAXED
                && bound->equality_count != 0u) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                    bound->span,
                    record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                    ast_type_id, ast_path_id, CM_HIR_OK,
                    "relaxed associated-type bounds cannot carry equality "
                    "bindings");
                break;
            }
            for (equality_index = 0u;
                 equality_index < bound->equality_count; ++equality_index) {
                const CmHirAssociatedTypeEquality *equality;
                const CmHirItem *associated;

                equality = &bound->equalities[equality_index];
                associated = cm_lower_bound_item(state,
                    equality->associated_type);
                if (associated == NULL
                    || associated->kind != CM_HIR_ITEM_TYPE_ALIAS
                    || associated->data.type_alias_item.target
                        != CM_HIR_TYPE_NONE
                    || !cm_hir_def_id_equal(associated->parent_definition,
                        target_trait->definition)
                    || associated->generic_parameter_count != 0u) {
                    cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                        equality->span,
                        record == NULL ? CM_AST_ITEM_NONE : record->ast_id,
                        ast_type_id, ast_path_id, CM_HIR_OK,
                        "associated-type equality target is not a bound "
                        "nongeneric declaration of its bound trait");
                    break;
                }
                for (prior = 0u; prior < equality_index; ++prior) {
                    if (cm_hir_def_id_equal(
                            bound->equalities[prior].associated_type,
                            equality->associated_type)) {
                        cm_lower_fail(state, CM_HIR_LOWER_INVALID_TRAIT,
                            equality->span,
                            record == NULL ? CM_AST_ITEM_NONE
                                : record->ast_id,
                            ast_type_id, ast_path_id, CM_HIR_OK,
                            "duplicate associated type equality target");
                        break;
                    }
                }
                if (state->failed) break;
            }
        }
    }
    return !state->failed;
}

static int cm_lower_bind_projection_declarations(CmLowerState *state)
{
    return cm_lower_records_in_phase(state, CM_LOWER_PHASE_TRAIT_HEADER)
        && cm_lower_validate_supertraits(state)
        && cm_lower_validate_trait_aliases(state)
        && cm_lower_records_in_phase(state,
            CM_LOWER_PHASE_TRAIT_ASSOCIATED_TYPES)
        && cm_lower_validate_associated_type_bounds(state)
        && cm_lower_records_in_phase(state,
            CM_LOWER_PHASE_TRAIT_ASSOCIATED_CONSTS)
        && cm_lower_records_in_phase(state, CM_LOWER_PHASE_TRAIT_METHODS);
}

static int cm_lower_normalize_type_root(CmLowerState *state,
    CmHirTypeId *root, CmSpan span)
{
    CmHirTypeAliasResult result;
    CmHirLowerErrorKind kind;

    if (*root == CM_HIR_TYPE_NONE) return 1;
    result = cm_hir_normalize_type_aliases(state->hir, *root);
    if (result.status == CM_HIR_TYPE_ALIAS_OK) {
        *root = result.type;
        return 1;
    }
    switch (result.status) {
    case CM_HIR_TYPE_ALIAS_ARGUMENT_COUNT:
    case CM_HIR_TYPE_ALIAS_ARGUMENT_KIND:
        kind = CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH;
        break;
    case CM_HIR_TYPE_ALIAS_CYCLE:
        kind = CM_HIR_LOWER_ALIAS_CYCLE;
        break;
    case CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST:
        kind = CM_HIR_LOWER_UNSUPPORTED_GENERIC;
        break;
    case CM_HIR_TYPE_ALIAS_UNSUPPORTED_DYN_TRAIT:
    case CM_HIR_TYPE_ALIAS_UNSUPPORTED_OPAQUE:
        kind = CM_HIR_LOWER_UNSUPPORTED_TYPE;
        break;
    case CM_HIR_TYPE_ALIAS_RECURSION_LIMIT:
        kind = CM_HIR_LOWER_INVALID_ALIAS;
        break;
    case CM_HIR_TYPE_ALIAS_HIR_FAILURE:
        kind = CM_HIR_LOWER_HIR_FAILURE;
        break;
    case CM_HIR_TYPE_ALIAS_INVALID_ARGUMENT:
    case CM_HIR_TYPE_ALIAS_INVALID_TYPE:
    case CM_HIR_TYPE_ALIAS_INVALID_ALIAS:
    case CM_HIR_TYPE_ALIAS_OK:
        kind = CM_HIR_LOWER_INVALID_ALIAS;
        break;
    default:
        kind = CM_HIR_LOWER_INVALID_ALIAS;
        break;
    }
    cm_lower_fail(state, kind, span, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
        CM_AST_PATH_NONE, result.hir_status,
        "type-alias normalization failed for HIR type %u: %s",
        (unsigned int)result.source_type,
        cm_hir_type_alias_status_name(result.status));
    return 0;
}

static int cm_lower_normalize_named_roots(CmLowerState *state,
    CmHirNamedType *named, CmSpan span)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count && !state->failed;
         ++index) {
        CmHirGenericArg *argument;

        argument = &named->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (!cm_lower_normalize_type_root(state, &argument->data.type,
                    span)) {
                return 0;
            }
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (!cm_lower_normalize_type_root(state,
                    &argument->data.constant.type, span)) {
                return 0;
            }
        }
    }
    return !state->failed;
}

static int cm_lower_normalize_item_roots(CmLowerState *state,
    CmHirItem *item)
{
    uint32_t index;
    uint32_t field_index;

    for (index = 0u; index < item->predicate_scope_count; ++index) {
        CmHirPredicateScope *scope;

        scope = &item->predicate_scopes[index];
        if (scope->subject_kind == CM_HIR_OUTLIVES_TYPE
            && !cm_lower_normalize_type_root(state, &scope->subject.type,
                scope->span)) {
            return 0;
        }
    }
    for (index = 0u; index < item->predicate_count; ++index) {
        CmHirTraitPredicate *predicate;

        predicate = &item->predicates[index];
        if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE) {
            predicate->subject =
                item->predicate_scopes[predicate->scope - 1u].subject.type;
        }
        if (!cm_lower_normalize_type_root(state, &predicate->subject,
                predicate->span)
            || !cm_lower_normalize_named_roots(state,
                &predicate->trait_type, predicate->span)) {
            return 0;
        }
        {
            uint32_t equality_index;

            for (equality_index = 0u;
                 equality_index < predicate->equality_count;
                 ++equality_index) {
                if (!cm_lower_normalize_type_root(state,
                        &predicate->equalities[equality_index].value,
                        predicate->equalities[equality_index].span)) {
                    return 0;
                }
            }
        }
    }
    for (index = 0u; index < item->outlives_predicate_count; ++index) {
        CmHirOutlivesPredicate *predicate;

        predicate = &item->outlives_predicates[index];
        if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE) {
            const CmHirPredicateScope *scope;

            scope = &item->predicate_scopes[predicate->scope - 1u];
            predicate->subject_kind = scope->subject_kind;
            if (scope->subject_kind == CM_HIR_OUTLIVES_TYPE) {
                predicate->subject.type = scope->subject.type;
            } else {
                predicate->subject.lifetime = scope->subject.lifetime;
            }
        }
        if (predicate->subject_kind == CM_HIR_OUTLIVES_TYPE
            && !cm_lower_normalize_type_root(state,
                &predicate->subject.type, predicate->span)) {
            return 0;
        }
    }
    switch (item->kind) {
    case CM_HIR_ITEM_FUNCTION:
        for (index = 0u;
             index < item->data.function_item.signature.parameter_count;
             ++index) {
            if (!cm_lower_normalize_type_root(state,
                    &item->data.function_item.signature.parameters[index].type,
                    item->data.function_item.signature.parameters[index].span)) {
                return 0;
            }
        }
        return cm_lower_normalize_type_root(state,
            &item->data.function_item.signature.return_type, item->span);
    case CM_HIR_ITEM_STRUCT:
    case CM_HIR_ITEM_UNION:
        for (index = 0u; index < item->data.aggregate_item.field_count;
             ++index) {
            if (!cm_lower_normalize_type_root(state,
                    &item->data.aggregate_item.fields[index].type,
                    item->data.aggregate_item.fields[index].span)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_ITEM_ENUM:
        for (index = 0u; index < item->data.enum_item.variant_count;
             ++index) {
            CmHirVariant *variant;

            variant = &item->data.enum_item.variants[index];
            for (field_index = 0u; field_index < variant->field_count;
                 ++field_index) {
                if (!cm_lower_normalize_type_root(state,
                        &variant->fields[field_index].type,
                        variant->fields[field_index].span)) {
                    return 0;
                }
            }
            if (variant->has_discriminant
                && !cm_lower_normalize_type_root(state,
                    &variant->discriminant.type, variant->span)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_ITEM_TYPE_ALIAS:
        if (!cm_lower_normalize_type_root(state,
                &item->data.type_alias_item.target, item->span)) {
            return 0;
        }
        for (index = 0u; index < item->data.type_alias_item.bound_count;
             ++index) {
            CmHirAssociatedTypeBound *bound;
            uint32_t equality_index;

            bound = &item->data.type_alias_item.bounds[index];
            if (!cm_lower_normalize_named_roots(state, &bound->trait_type,
                    bound->span)) {
                return 0;
            }
            for (equality_index = 0u;
                 equality_index < bound->equality_count;
                 ++equality_index) {
                if (!cm_lower_normalize_type_root(state,
                        &bound->equalities[equality_index].value,
                        bound->equalities[equality_index].span)) {
                    return 0;
                }
            }
        }
        return 1;
    case CM_HIR_ITEM_TRAIT_ALIAS:
        for (index = 0u;
             index < item->data.trait_alias_item.bound_count; ++index) {
            CmHirTraitAliasBound *alias_bound;
            CmHirSupertrait *trait_bound;
            uint32_t equality_index;

            alias_bound = &item->data.trait_alias_item.bounds[index];
            if (alias_bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME) {
                continue;
            }
            if (alias_bound->kind != CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_ALIAS,
                    alias_bound->span, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    "trait alias has an invalid bound during normalization");
                return 0;
            }
            trait_bound = &alias_bound->data.trait_bound;
            if (!cm_lower_normalize_named_roots(state,
                    &trait_bound->trait_type, trait_bound->span)) {
                return 0;
            }
            for (equality_index = 0u;
                 equality_index < trait_bound->equality_count;
                 ++equality_index) {
                if (!cm_lower_normalize_type_root(state,
                        &trait_bound->equalities[equality_index].value,
                        trait_bound->equalities[equality_index].span)) {
                    return 0;
                }
            }
        }
        return 1;
    case CM_HIR_ITEM_CONST:
    case CM_HIR_ITEM_STATIC:
        return cm_lower_normalize_type_root(state,
            &item->data.value_item.type, item->span);
    case CM_HIR_ITEM_IMPL:
        if (!cm_lower_normalize_type_root(state,
                &item->data.impl_item.self_type, item->span)) {
            return 0;
        }
        return !item->data.impl_item.has_trait
            || cm_lower_normalize_named_roots(state,
                &item->data.impl_item.trait_type, item->span);
    case CM_HIR_ITEM_MODULE:
    case CM_HIR_ITEM_TRAIT:
    case CM_HIR_ITEM_EXTERN_TYPE:
        return 1;
    }
    cm_lower_fail(state, CM_HIR_LOWER_INVALID_ALIAS, item->span,
        CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
        "unknown HIR item kind during type-alias normalization");
    return 0;
}

static int cm_lower_normalize_crate_aliases(CmLowerState *state)
{
    size_t index;

    for (index = 0u; index < state->hir->items.len && !state->failed;
         ++index) {
        CmHirItem *item;

        item = (CmHirItem *)cm_vec_at(&state->hir->items, index);
        if (item != NULL
            && item->definition.crate_id == state->result.crate_id
            && !cm_lower_normalize_item_roots(state, item)) {
            return 0;
        }
    }
    for (index = 0u; index < state->hir->bodies.len && !state->failed;
         ++index) {
        CmHirBody *body;
        uint32_t local_index;

        body = (CmHirBody *)cm_vec_at(&state->hir->bodies, index);
        if (body == NULL || body->owner.crate_id != state->result.crate_id) {
            continue;
        }
        if (!cm_lower_normalize_type_root(state, &body->expected_type,
                body->span)) {
            return 0;
        }
        for (local_index = 0u; local_index < body->local_count;
             ++local_index) {
            if (!cm_lower_normalize_type_root(state,
                    &body->locals[local_index].type,
                    body->locals[local_index].span)) {
                return 0;
            }
        }
    }
    for (index = 0u; index < state->hir->generic_parameters.len
         && !state->failed; ++index) {
        CmHirGenericParam *parameter;

        parameter = (CmHirGenericParam *)cm_vec_at(
            &state->hir->generic_parameters, index);
        if (parameter == NULL
            || parameter->owner.crate_id != state->result.crate_id) {
            continue;
        }
        if (parameter->declared_type != CM_HIR_TYPE_NONE
            && !cm_lower_normalize_type_root(state,
                &parameter->declared_type, parameter->span)) {
            return 0;
        }
        if (parameter->has_default
            && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE
            && !cm_lower_normalize_type_root(state,
                &parameter->default_argument.data.type, parameter->span)) {
            return 0;
        }
        if (parameter->has_default
            && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_CONST
            && !cm_lower_normalize_type_root(state,
                &parameter->default_argument.data.constant.type,
                parameter->span)) {
            return 0;
        }
    }
    return !state->failed;
}

static int cm_lower_validate_impl_completeness(CmLowerState *state)
{
    size_t impl_index;

    for (impl_index = 0u; impl_index < state->hir->items.len
         && !state->failed; ++impl_index) {
        const CmHirItem *impl_item;
        size_t declaration_index;

        impl_item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            impl_index);
        if (impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
            || impl_item->definition.crate_id != state->result.crate_id
            || !impl_item->data.impl_item.has_trait
            || impl_item->data.impl_item.is_negative) {
            continue;
        }
        for (declaration_index = 0u;
             declaration_index < state->hir->items.len;
             ++declaration_index) {
            const CmHirItem *declaration;
            size_t definition_index;
            uint32_t matches;

            declaration = (const CmHirItem *)cm_vec_at_const(
                &state->hir->items, declaration_index);
            if (declaration == NULL
                || (declaration->kind != CM_HIR_ITEM_TYPE_ALIAS
                    && declaration->kind != CM_HIR_ITEM_FUNCTION
                    && declaration->kind != CM_HIR_ITEM_CONST)
                || !cm_hir_def_id_equal(declaration->parent_definition,
                    impl_item->data.impl_item.trait_type.definition)) {
                continue;
            }
            if (declaration->kind == CM_HIR_ITEM_TYPE_ALIAS
                && declaration->data.type_alias_item.target
                    != CM_HIR_TYPE_NONE) {
                continue;
            }
            matches = 0u;
            for (definition_index = 0u;
                 definition_index < state->hir->items.len;
                 ++definition_index) {
                const CmHirItem *definition;

                definition = (const CmHirItem *)cm_vec_at_const(
                    &state->hir->items, definition_index);
                if (definition == NULL
                    || definition->kind != declaration->kind
                    || !cm_hir_def_id_equal(definition->parent_definition,
                        impl_item->definition)) {
                    continue;
                }
                if ((definition->kind == CM_HIR_ITEM_TYPE_ALIAS
                        && cm_hir_def_id_equal(definition->data
                                .type_alias_item.trait_item_definition,
                            declaration->definition))
                    || (definition->kind == CM_HIR_ITEM_FUNCTION
                        && cm_hir_def_id_equal(definition->data
                                .function_item.trait_item_definition,
                            declaration->definition))
                    || (definition->kind == CM_HIR_ITEM_CONST
                        && cm_hir_def_id_equal(definition->data
                                .value_item.trait_item_definition,
                            declaration->definition))) {
                    matches += 1u;
                }
            }
            if ((declaration->kind == CM_HIR_ITEM_TYPE_ALIAS
                    && matches != 1u)
                || (declaration->kind == CM_HIR_ITEM_CONST
                    && declaration->data.value_item.body == CM_HIR_BODY_NONE
                    && matches != 1u)
                || (declaration->kind == CM_HIR_ITEM_CONST
                    && declaration->data.value_item.body != CM_HIR_BODY_NONE
                    && matches > 1u)
                || (declaration->kind == CM_HIR_ITEM_FUNCTION
                    && declaration->data.function_item.body
                        == CM_HIR_BODY_NONE
                    && matches != 1u)
                || (declaration->kind == CM_HIR_ITEM_FUNCTION
                    && declaration->data.function_item.body
                        != CM_HIR_BODY_NONE
                    && matches > 1u)) {
                cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL,
                    impl_item->span, CM_AST_ITEM_NONE, CM_AST_TYPE_NONE,
                    CM_AST_PATH_NONE, CM_HIR_OK,
                    declaration->kind == CM_HIR_ITEM_TYPE_ALIAS
                        ? (matches == 0u
                            ? "impl is missing a required associated type "
                              "definition"
                            : "impl has duplicate definitions for one "
                              "required associated type")
                        : declaration->kind == CM_HIR_ITEM_CONST
                            ? (matches == 0u
                                ? "impl is missing a required associated "
                                  "const definition"
                                : "impl has duplicate definitions for one "
                                  "required associated const")
                        : (matches == 0u
                            ? "impl is missing a required trait method"
                            : "impl has duplicate overrides for one trait "
                              "method"));
                return 0;
            }
        }
    }
    return !state->failed;
}

typedef enum CmLowerImplSelfClass {
    CM_LOWER_IMPL_SELF_UNSUPPORTED = 0,
    CM_LOWER_IMPL_SELF_MONOMORPHIC,
    CM_LOWER_IMPL_SELF_SINGLE_PARAMETER,
    CM_LOWER_IMPL_SELF_ORDERED_GENERIC_ADT,
    CM_LOWER_IMPL_SELF_ORDERED_GENERIC_RAW_POINTER,
    CM_LOWER_IMPL_SELF_ORDERED_GENERIC_REFERENCE
} CmLowerImplSelfClass;

static int cm_lower_impl_has_supported_members(const CmLowerState *state,
    CmHirDefId impl_definition)
{
    size_t index;

    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *child;

        child = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            index);
        if (child == NULL || !cm_hir_def_id_equal(child->parent_definition,
                impl_definition)) continue;
        if (child->kind != CM_HIR_ITEM_FUNCTION
            && child->kind != CM_HIR_ITEM_TYPE_ALIAS
            && child->kind != CM_HIR_ITEM_CONST) return 0;
    }
    return 1;
}

static CmLowerImplSelfClass cm_lower_impl_self_class(
    const CmLowerState *state, const CmHirItem *impl_item,
    CmHirDefId *out_adt_definition)
{
    const CmHirType *type;
    const CmHirItem *adt_item;
    uint32_t index;

    *out_adt_definition = cm_hir_def_id_none();
    if (impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL) {
        return CM_LOWER_IMPL_SELF_UNSUPPORTED;
    }
    type = cm_hir_get_type(state->hir, impl_item->data.impl_item.self_type);
    if (type == NULL) return CM_LOWER_IMPL_SELF_UNSUPPORTED;
    if (type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && impl_item->generic_parameter_count == 1u
        && impl_item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        && type->data.parameter_type.parameter
            == impl_item->generic_parameter_start) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(state->hir,
            impl_item->generic_parameter_start);
        if (parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
            && !parameter->has_default && parameter->index == 0u
            && cm_hir_def_id_equal(parameter->owner,
                impl_item->definition)
            && cm_lower_impl_has_supported_members(state,
                impl_item->definition)) {
            return CM_LOWER_IMPL_SELF_SINGLE_PARAMETER;
        }
    }
    if (type->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && impl_item->generic_parameter_count == 1u) {
        const CmHirGenericParam *parameter;
        const CmHirType *pointee;

        parameter = cm_hir_get_generic_param(state->hir,
            impl_item->generic_parameter_start);
        pointee = cm_hir_get_type(state->hir,
            type->data.raw_pointer_type.pointee);
        if (parameter != NULL
            && parameter->kind == CM_HIR_GENERIC_TYPE
            && !parameter->has_default
            && pointee != NULL
            && pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
            && pointee->data.parameter_type.parameter
                == impl_item->generic_parameter_start) {
            return CM_LOWER_IMPL_SELF_ORDERED_GENERIC_RAW_POINTER;
        }
    }
    /*
     * Core has blanket Clone polarity entries over shared and mutable
     * references (`impl<T> Clone for &T` and `impl<T> !Clone for &mut T`).
     * Admit the same bounded shape as raw-pointer blankets: exactly one
     * required type parameter used as the reference pointee.  The elided
     * reference region is intentionally not part of this class key; all
     * such references can overlap for coherence purposes, while mutability
     * remains a discriminant in cm_lower_impl_self_equal().
     */
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && impl_item->generic_parameter_count == 1u) {
        const CmHirGenericParam *parameter;
        const CmHirType *pointee;

        parameter = cm_hir_get_generic_param(state->hir,
            impl_item->generic_parameter_start);
        pointee = cm_hir_get_type(state->hir,
            type->data.reference_type.pointee);
        if (parameter != NULL
            && parameter->kind == CM_HIR_GENERIC_TYPE
            && !parameter->has_default
            && pointee != NULL
            && pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
            && pointee->data.parameter_type.parameter
                == impl_item->generic_parameter_start) {
            return CM_LOWER_IMPL_SELF_ORDERED_GENERIC_REFERENCE;
        }
    }
    if (impl_item->generic_parameter_count == 0u) {
        if (type->kind == CM_HIR_TYPE_BOOL_KIND
            || type->kind == CM_HIR_TYPE_CHAR_KIND
            || type->kind == CM_HIR_TYPE_INTEGER_KIND
            || type->kind == CM_HIR_TYPE_FLOAT_KIND) {
            return CM_LOWER_IMPL_SELF_MONOMORPHIC;
        }
        if (type->kind != CM_HIR_TYPE_ADT_KIND
            || type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
            || type->data.named_type.definition.crate_id
                != state->result.crate_id) {
            return CM_LOWER_IMPL_SELF_UNSUPPORTED;
        }
        adt_item = cm_lower_bound_item(state,
            type->data.named_type.definition);
        if (adt_item == NULL
            || (adt_item->kind != CM_HIR_ITEM_STRUCT
                && adt_item->kind != CM_HIR_ITEM_UNION
                && adt_item->kind != CM_HIR_ITEM_ENUM)
            || adt_item->generic_parameter_count != 0u) {
            return CM_LOWER_IMPL_SELF_UNSUPPORTED;
        }
        *out_adt_definition = type->data.named_type.definition;
        return CM_LOWER_IMPL_SELF_MONOMORPHIC;
    }
    if (type->kind != CM_HIR_TYPE_ADT_KIND
        || type->data.named_type.argument_count
            != impl_item->generic_parameter_count
        || type->data.named_type.arguments == NULL
        || type->data.named_type.definition.crate_id
            != state->result.crate_id) {
        return CM_LOWER_IMPL_SELF_UNSUPPORTED;
    }
    adt_item = cm_lower_bound_item(state,
        type->data.named_type.definition);
    if (adt_item == NULL
        || (adt_item->kind != CM_HIR_ITEM_STRUCT
            && adt_item->kind != CM_HIR_ITEM_UNION
            && adt_item->kind != CM_HIR_ITEM_ENUM)
        || adt_item->generic_parameter_count
            != impl_item->generic_parameter_count) {
        return CM_LOWER_IMPL_SELF_UNSUPPORTED;
    }
    for (index = 0u; index < impl_item->generic_parameter_count; ++index) {
        const CmHirGenericParam *impl_parameter;
        const CmHirGenericParam *adt_parameter;
        const CmHirGenericArg *argument;
        const CmHirType *argument_type;

        impl_parameter = cm_hir_get_generic_param(state->hir,
            impl_item->generic_parameter_start + index);
        adt_parameter = cm_hir_get_generic_param(state->hir,
            adt_item->generic_parameter_start + index);
        argument = &type->data.named_type.arguments[index];
        if (impl_parameter == NULL || adt_parameter == NULL
            || impl_parameter->has_default
            || impl_parameter->kind != adt_parameter->kind) {
            return CM_LOWER_IMPL_SELF_UNSUPPORTED;
        }
        if (impl_parameter->kind == CM_HIR_GENERIC_TYPE) {
            argument_type = argument->kind == CM_HIR_GENERIC_ARG_TYPE
                ? cm_hir_get_type(state->hir, argument->data.type) : NULL;
            if (argument_type == NULL
                || argument_type->kind != CM_HIR_TYPE_PARAMETER_KIND
                || argument_type->data.parameter_type.parameter
                    != impl_item->generic_parameter_start + index) {
                return CM_LOWER_IMPL_SELF_UNSUPPORTED;
            }
        } else if (impl_parameter->kind == CM_HIR_GENERIC_CONST) {
            if (argument->kind != CM_HIR_GENERIC_ARG_CONST
                || argument->data.constant.kind != CM_HIR_CONST_PARAMETER
                || argument->data.constant.data.parameter
                    != impl_item->generic_parameter_start + index) {
                return CM_LOWER_IMPL_SELF_UNSUPPORTED;
            }
        } else {
            return CM_LOWER_IMPL_SELF_UNSUPPORTED;
        }
    }
    *out_adt_definition = type->data.named_type.definition;
    return CM_LOWER_IMPL_SELF_ORDERED_GENERIC_ADT;
}

static int cm_lower_impl_self_equal(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id)
{
    const CmHirType *left;
    const CmHirType *right;

    left = cm_hir_get_type(hir, left_id);
    right = cm_hir_get_type(hir, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return 0;
    }
    if (left->kind == CM_HIR_TYPE_BOOL_KIND
        || left->kind == CM_HIR_TYPE_CHAR_KIND) {
        return 1;
    }
    if (left->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return left->data.integer_type.kind == right->data.integer_type.kind;
    }
    if (left->kind == CM_HIR_TYPE_FLOAT_KIND) {
        return left->data.float_type.kind == right->data.float_type.kind;
    }
    if (left->kind == CM_HIR_TYPE_ADT_KIND) {
        return left->data.named_type.argument_count == 0u
            && right->data.named_type.argument_count == 0u
            && cm_hir_def_id_equal(left->data.named_type.definition,
                right->data.named_type.definition);
    }
    if (left->kind == CM_HIR_TYPE_RAW_POINTER_KIND) {
        return left->data.raw_pointer_type.mutability
            == right->data.raw_pointer_type.mutability;
    }
    if (left->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        /* Elided reference regions are inferred independently per impl and
         * therefore are not a stable coherence key.  Mutability is the
         * only discriminant in this bounded generic-reference class. */
        return left->data.reference_type.mutability
            == right->data.reference_type.mutability;
    }
    return 0;
}

/*
 * Decide whether two already-classified self types can overlap before doing
 * the more expensive structural comparison of their implemented-trait
 * arguments.  A mismatch here is definitive for the bounded candidate
 * subset, so it is safe to use this as a cheap pair gate.
 */
static int cm_lower_impl_self_candidates_may_overlap(
    const CmHirContext *hir, CmLowerImplSelfClass left_class,
    CmHirDefId left_adt_definition, CmHirTypeId left_self_type,
    CmLowerImplSelfClass right_class, CmHirDefId right_adt_definition,
    CmHirTypeId right_self_type)
{
    if (left_class == CM_LOWER_IMPL_SELF_SINGLE_PARAMETER
        || right_class == CM_LOWER_IMPL_SELF_SINGLE_PARAMETER) {
        return 1;
    }
    if (left_class == CM_LOWER_IMPL_SELF_MONOMORPHIC
        && right_class == CM_LOWER_IMPL_SELF_MONOMORPHIC) {
        return cm_lower_impl_self_equal(hir, left_self_type,
            right_self_type);
    }
    if (left_class == CM_LOWER_IMPL_SELF_ORDERED_GENERIC_RAW_POINTER
        && right_class
            == CM_LOWER_IMPL_SELF_ORDERED_GENERIC_RAW_POINTER) {
        return cm_lower_impl_self_equal(hir, left_self_type,
            right_self_type);
    }
    if (left_class == CM_LOWER_IMPL_SELF_ORDERED_GENERIC_REFERENCE
        && right_class == CM_LOWER_IMPL_SELF_ORDERED_GENERIC_REFERENCE) {
        return cm_lower_impl_self_equal(hir, left_self_type,
            right_self_type);
    }
    return left_class == CM_LOWER_IMPL_SELF_ORDERED_GENERIC_ADT
        && right_class == CM_LOWER_IMPL_SELF_ORDERED_GENERIC_ADT
        && cm_hir_def_id_equal(left_adt_definition, right_adt_definition);
}

/*
 * Trait arguments in two impl headers use different DefIds for their
 * parameters.  Compare the supported structural forms after normalizing a
 * parameter to its positional index within its owning impl.  This is kept
 * deliberately bounded: the candidate validator only admits a correspond-
 * ing bounded self-type subset, and unsupported argument shapes do not gain
 * an invented identity here.
 */
static int cm_lower_impl_parameter_index(const CmHirContext *hir,
    const CmHirItem *impl_item, CmHirGenericParamId parameter_id,
    uint32_t *out_index)
{
    const CmHirGenericParam *parameter;
    uint32_t offset;

    if (hir == NULL || impl_item == NULL
        || parameter_id == CM_HIR_GENERIC_PARAM_NONE
        || impl_item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || parameter_id < impl_item->generic_parameter_start) {
        return 0;
    }
    offset = parameter_id - impl_item->generic_parameter_start;
    if (offset >= impl_item->generic_parameter_count) return 0;
    parameter = cm_hir_get_generic_param(hir, parameter_id);
    if (parameter == NULL || parameter->index != offset
        || !cm_hir_def_id_equal(parameter->owner, impl_item->definition)) {
        return 0;
    }
    *out_index = offset;
    return 1;
}

static int cm_lower_impl_region_equal(const CmHirContext *hir,
    const CmHirRegion *left, const CmHirItem *left_impl,
    const CmHirRegion *right, const CmHirItem *right_impl)
{
    uint32_t left_index;
    uint32_t right_index;

    if (left == NULL || right == NULL || left->kind != right->kind) {
        return 0;
    }
    switch (left->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
        return cm_lower_impl_parameter_index(hir, left_impl,
                left->data.parameter, &left_index)
            && cm_lower_impl_parameter_index(hir, right_impl,
                right->data.parameter, &right_index)
            && left_index == right_index;
    case CM_HIR_REGION_LATE_BOUND:
        return left->data.binder_index == right->data.binder_index;
    case CM_HIR_REGION_INFER:
        return left->data.inference_variable
            == right->data.inference_variable;
    case CM_HIR_REGION_ERROR:
        return left->data.error_reason == right->data.error_reason;
    }
    return 0;
}

static int cm_lower_impl_type_equal(const CmHirContext *hir,
    CmHirTypeId left_id, const CmHirItem *left_impl, CmHirTypeId right_id,
    const CmHirItem *right_impl, size_t depth);

static int cm_lower_impl_const_equal(const CmHirContext *hir,
    const CmHirConstArg *left, const CmHirItem *left_impl,
    const CmHirConstArg *right, const CmHirItem *right_impl, size_t depth)
{
    uint32_t left_index;
    uint32_t right_index;

    if (left == NULL || right == NULL || left->kind != right->kind
        || !cm_lower_impl_type_equal(hir, left->type, left_impl,
            right->type, right_impl, depth + 1u)) {
        return 0;
    }
    switch (left->kind) {
    case CM_HIR_CONST_VALUE:
        return left->data.value.low_bits == right->data.value.low_bits
            && left->data.value.high_bits == right->data.value.high_bits;
    case CM_HIR_CONST_PARAMETER:
        return cm_lower_impl_parameter_index(hir, left_impl,
                left->data.parameter, &left_index)
            && cm_lower_impl_parameter_index(hir, right_impl,
                right->data.parameter, &right_index)
            && left_index == right_index;
    case CM_HIR_CONST_UNEVALUATED:
        return cm_hir_def_id_equal(left->data.definition,
            right->data.definition);
    case CM_HIR_CONST_INFER:
        return left->data.inference_variable
            == right->data.inference_variable;
    case CM_HIR_CONST_ERROR:
        return left->data.error_reason == right->data.error_reason;
    }
    return 0;
}

static int cm_lower_impl_generic_arg_equal(const CmHirContext *hir,
    const CmHirGenericArg *left, const CmHirItem *left_impl,
    const CmHirGenericArg *right, const CmHirItem *right_impl, size_t depth)
{
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return 0;
    }
    switch (left->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_lower_impl_region_equal(hir, &left->data.lifetime,
            left_impl, &right->data.lifetime, right_impl);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_lower_impl_type_equal(hir, left->data.type, left_impl,
            right->data.type, right_impl, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_lower_impl_const_equal(hir, &left->data.constant,
            left_impl, &right->data.constant, right_impl, depth + 1u);
    }
    return 0;
}

static int cm_lower_impl_named_type_equal(const CmHirContext *hir,
    const CmHirNamedType *left, const CmHirItem *left_impl,
    const CmHirNamedType *right, const CmHirItem *right_impl, size_t depth)
{
    uint32_t index;

    if (left == NULL || right == NULL
        || !cm_hir_def_id_equal(left->definition, right->definition)
        || left->argument_count != right->argument_count
        || (left->argument_count != 0u && left->arguments == NULL)
        || (right->argument_count != 0u && right->arguments == NULL)) {
        return 0;
    }
    for (index = 0u; index < left->argument_count; ++index) {
        if (!cm_lower_impl_generic_arg_equal(hir, &left->arguments[index],
                left_impl, &right->arguments[index], right_impl,
                depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_lower_impl_type_equal(const CmHirContext *hir,
    CmHirTypeId left_id, const CmHirItem *left_impl, CmHirTypeId right_id,
    const CmHirItem *right_impl, size_t depth)
{
    const CmHirType *left;
    const CmHirType *right;
    uint32_t index;
    uint32_t left_index;
    uint32_t right_index;

    if (left_id == right_id && left_id != CM_HIR_TYPE_NONE) return 1;
    if (hir == NULL || depth > hir->types.len) return 0;
    left = cm_hir_get_type(hir, left_id);
    right = cm_hir_get_type(hir, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return 0;
    }
    switch (left->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        return left->data.integer_type.kind == right->data.integer_type.kind;
    case CM_HIR_TYPE_FLOAT_KIND:
        return left->data.float_type.kind == right->data.float_type.kind;
    case CM_HIR_TYPE_SELF_KIND:
        return cm_hir_def_id_equal(left->data.self_type.owner,
            left_impl->definition)
            && cm_hir_def_id_equal(right->data.self_type.owner,
                right_impl->definition);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return cm_lower_impl_parameter_index(hir, left_impl,
                left->data.parameter_type.parameter, &left_index)
            && cm_lower_impl_parameter_index(hir, right_impl,
                right->data.parameter_type.parameter, &right_index)
            && left_index == right_index;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return left->data.reference_type.mutability
                == right->data.reference_type.mutability
            && cm_lower_impl_region_equal(hir,
                &left->data.reference_type.region, left_impl,
                &right->data.reference_type.region, right_impl)
            && cm_lower_impl_type_equal(hir,
                left->data.reference_type.pointee, left_impl,
                right->data.reference_type.pointee, right_impl,
                depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return left->data.raw_pointer_type.mutability
                == right->data.raw_pointer_type.mutability
            && cm_lower_impl_type_equal(hir,
                left->data.raw_pointer_type.pointee, left_impl,
                right->data.raw_pointer_type.pointee, right_impl,
                depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if (left->data.tuple_type.element_count
                != right->data.tuple_type.element_count
            || (left->data.tuple_type.element_count != 0u
                && (left->data.tuple_type.elements == NULL
                    || right->data.tuple_type.elements == NULL))) {
            return 0;
        }
        for (index = 0u; index < left->data.tuple_type.element_count;
             ++index) {
            if (!cm_lower_impl_type_equal(hir,
                    left->data.tuple_type.elements[index], left_impl,
                    right->data.tuple_type.elements[index], right_impl,
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_lower_impl_type_equal(hir,
                left->data.array_type.element, left_impl,
                right->data.array_type.element, right_impl, depth + 1u)
            && cm_lower_impl_const_equal(hir, &left->data.array_type.length,
                left_impl, &right->data.array_type.length, right_impl,
                depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_lower_impl_type_equal(hir,
            left->data.slice_type.element, left_impl,
            right->data.slice_type.element, right_impl, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (left->data.fn_pointer_type.parameter_count
                != right->data.fn_pointer_type.parameter_count
            || left->data.fn_pointer_type.abi != right->data.fn_pointer_type.abi
            || left->data.fn_pointer_type.safety
                != right->data.fn_pointer_type.safety
            || left->data.fn_pointer_type.is_variadic
                != right->data.fn_pointer_type.is_variadic) return 0;
        for (index = 0u;
             index < left->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_lower_impl_type_equal(hir,
                    left->data.fn_pointer_type.parameters[index], left_impl,
                    right->data.fn_pointer_type.parameters[index], right_impl,
                    depth + 1u)) return 0;
        }
        return cm_lower_impl_type_equal(hir,
            left->data.fn_pointer_type.return_type, left_impl,
            right->data.fn_pointer_type.return_type, right_impl,
            depth + 1u);
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_lower_impl_named_type_equal(hir, &left->data.named_type,
            left_impl, &right->data.named_type, right_impl, depth + 1u);
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
        return 0;
    }
    return 0;
}

static int cm_lower_impl_trait_arguments_equal(const CmHirContext *hir,
    const CmHirItem *left_impl, const CmHirItem *right_impl)
{
    const CmHirNamedType *left;
    const CmHirNamedType *right;
    uint32_t index;

    if (hir == NULL || left_impl == NULL || right_impl == NULL) return 0;
    left = &left_impl->data.impl_item.trait_type;
    right = &right_impl->data.impl_item.trait_type;
    if (left->argument_count != right->argument_count
        || (left->argument_count != 0u
            && (left->arguments == NULL || right->arguments == NULL))) {
        return 0;
    }
    for (index = 0u; index < left->argument_count; ++index) {
        if (!cm_lower_impl_generic_arg_equal(hir, &left->arguments[index],
                left_impl, &right->arguments[index], right_impl, 0u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_lower_validate_impl_candidates(CmLowerState *state)
{
    CmLowerImplSelfClass *classes;
    CmHirDefId *adt_definitions;
    size_t index;

    classes = (CmLowerImplSelfClass *)cm_alloc_zeroed(
        state->hir->items.len, sizeof(*classes));
    adt_definitions = (CmHirDefId *)cm_alloc_zeroed(
        state->hir->items.len, sizeof(*adt_definitions));
    /* Classify each local trait impl exactly once.  In particular, this
     * avoids rescanning all HIR children for every prior candidate pair. */
    for (index = 0u; index < state->hir->items.len && !state->failed;
         ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || item->definition.crate_id != state->result.crate_id
            || !item->data.impl_item.has_trait) {
            continue;
        }
        classes[index] = cm_lower_impl_self_class(state, item,
            &adt_definitions[index]);
        if (classes[index] == CM_LOWER_IMPL_SELF_UNSUPPORTED) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, item->span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "impl self type is outside the bounded scalar, single-type "
                "parameter, generic reference/raw-pointer, zero-argument "
                "local ADT, or full ordered generic local ADT subset");
            cm_free(adt_definitions);
            cm_free(classes);
            return 0;
        }
    }
    for (index = 0u; index < state->hir->items.len && !state->failed;
         ++index) {
        const CmHirItem *item;
        CmLowerImplSelfClass item_class;
        CmHirDefId item_adt_definition;
        size_t prior_index;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
            index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || item->definition.crate_id != state->result.crate_id
            || !item->data.impl_item.has_trait) {
            continue;
        }
        item_class = classes[index];
        item_adt_definition = adt_definitions[index];
        for (prior_index = 0u; prior_index < index; ++prior_index) {
            const CmHirItem *prior;
            CmLowerImplSelfClass prior_class;
            CmHirDefId prior_adt_definition;

            prior = (const CmHirItem *)cm_vec_at_const(&state->hir->items,
                prior_index);
            if (prior == NULL || prior->kind != CM_HIR_ITEM_IMPL
                || prior->definition.crate_id != state->result.crate_id
                || !cm_hir_def_id_equal(
                    prior->data.impl_item.trait_type.definition,
                    item->data.impl_item.trait_type.definition)) {
                continue;
            }
            prior_class = classes[prior_index];
            prior_adt_definition = adt_definitions[prior_index];
            if (!cm_lower_impl_self_candidates_may_overlap(state->hir,
                    prior_class, prior_adt_definition,
                    prior->data.impl_item.self_type, item_class,
                    item_adt_definition, item->data.impl_item.self_type)) {
                continue;
            }
            if (!cm_lower_impl_trait_arguments_equal(state->hir, prior,
                    item)) continue;
            cm_lower_fail(state, CM_HIR_LOWER_INVALID_IMPL, item->span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                item_class == CM_LOWER_IMPL_SELF_SINGLE_PARAMETER
                    || prior_class == CM_LOWER_IMPL_SELF_SINGLE_PARAMETER
                    ? "overlapping blanket impl candidates for one trait"
                : item_class == CM_LOWER_IMPL_SELF_ORDERED_GENERIC_ADT
                    ? "overlapping ordered generic impl candidates for one "
                      "trait and local ADT head"
                    : prior->data.impl_item.is_negative
                            != item->data.impl_item.is_negative
                        ? "conflicting positive and negative impl candidates "
                          "for one trait and self type"
                        : item->data.impl_item.is_negative
                            ? "duplicate exact negative impl candidate for "
                              "one trait and self type"
                            : "duplicate exact impl candidate for one trait "
                              "and self type");
            return 0;
        }
    }
    cm_free(adt_definitions);
    cm_free(classes);
    return !state->failed;
}

static int cm_lower_validate_custom_receivers(CmLowerState *state)
{
    size_t index;

    for (index = 0u; index < state->hir->items.len && !state->failed;
         ++index) {
        const CmHirItem *item;
        const CmHirFunctionSignature *signature;

        item = (const CmHirItem *)cm_vec_at_const(&state->hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
            || item->definition.crate_id != state->result.crate_id) {
            continue;
        }
        signature = &item->data.function_item.signature;
        if (signature->receiver != CM_HIR_RECEIVER_CUSTOM) continue;
        if (signature->parameter_count == 0u
            || !cm_hir_custom_receiver_type_valid(state->hir,
                signature->parameters[0].type, item->parent_definition)) {
            cm_lower_fail(state, CM_HIR_LOWER_UNSUPPORTED_TYPE, item->span,
                CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK,
                "custom receiver type is not rooted in the enclosing Self "
                "after type-alias normalization");
            return 0;
        }
    }
    return !state->failed;
}

static int cm_lower_bind_remaining_items(CmLowerState *state)
{
    return cm_lower_records_in_phase(state, CM_LOWER_PHASE_IMPL_HEADER)
        && cm_lower_records_in_phase(state, CM_LOWER_PHASE_OTHER_ROOT)
        && cm_lower_records_in_phase(state,
            CM_LOWER_PHASE_IMPL_ASSOCIATED_TYPES)
        && cm_lower_records_in_phase(state,
            CM_LOWER_PHASE_IMPL_ASSOCIATED_CONSTS)
        && cm_lower_records_in_phase(state, CM_LOWER_PHASE_IMPL_METHODS)
        && cm_lower_validate_impl_completeness(state)
        && cm_lower_normalize_crate_aliases(state)
        && cm_lower_validate_custom_receivers(state)
        && cm_lower_validate_impl_candidates(state);
}

static int cm_lower_library_options_valid(const CmHirContext *context,
    const CmHirLowerOptions *options)
{
    size_t index;

    if (context == NULL || options == NULL) return 0;
    if (options->dependency_library_count != 0u
        && options->dependency_libraries == NULL) return 0;
    for (index = 0u; index < options->dependency_library_count; ++index) {
        CmHirLibraryArtifactIdentity identity;
        size_t prior;

        memset(&identity, 0, sizeof(identity));
        if (options->dependency_libraries[index] == NULL
            || !cm_hir_library_artifact_identity(
                options->dependency_libraries[index], &identity)
            || identity.context != context
            || identity.crate_id == CM_HIR_CRATE_NONE
            || identity.extern_name == NULL
            || identity.extern_name[0] == 0) return 0;
        for (prior = 0u; prior < index; ++prior) {
            CmHirLibraryArtifactIdentity prior_identity;

            memset(&prior_identity, 0, sizeof(prior_identity));
            if (!cm_hir_library_artifact_identity(
                    options->dependency_libraries[prior],
                    &prior_identity)
                || prior_identity.extern_name == NULL
                || strcmp(identity.extern_name,
                    prior_identity.extern_name) == 0) return 0;
        }
    }
    return 1;
}

void cm_hir_lower_options_init(CmHirLowerOptions *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->crate_name = "crate";
    options->edition = CM_HIR_EDITION_2024;
    options->source = 1u;
}

CmHirLowerResult cm_hir_lower_crate(CmHirContext *context, const CmAst *ast,
    const CmHirLowerOptions *options)
{
    CmLowerState state;
    CmSpan crate_span;
    CmInternId crate_name;
    CmHirStatus status;
    const CmAstItemId *root_items;
    uint32_t root_item_count;
    size_t index;

    memset(&state, 0, sizeof(state));
    if (context == NULL || ast == NULL || options == NULL
        || options->crate_name == NULL
        || options->crate_name[0] == '\0'
        || options->edition > CM_HIR_EDITION_2024
        || options->source == 0u
        || !cm_lower_library_options_valid(context, options)) {
        state.result.error_count = 1u;
        state.result.first_error.kind = CM_HIR_LOWER_INVALID_ARGUMENT;
        state.result.first_error.hir_status = CM_HIR_INVALID_ARGUMENT;
        (void)snprintf(state.result.first_error.message,
            sizeof(state.result.first_error.message),
            "invalid AST-to-HIR lowering arguments");
        return state.result;
    }
    state.hir = context;
    state.ast = ast;
    state.source = options->source;
    state.options = options;
    state.next_type_inference = 1u;
    state.next_region_inference = 1u;
    cm_vec_init(&state.item_records, sizeof(CmLowerItemRecord));
    cm_vec_init(&state.variant_records, sizeof(CmLowerVariantRecord));
    cm_vec_init(&state.macro_records, sizeof(CmLowerMacroRecord));
    cm_vec_init(&state.generic_records, sizeof(CmLowerGenericRecord));
    cm_vec_init(&state.apit_records, sizeof(CmLowerApitRecord));
    cm_vec_init(&state.expanded_source_ids, sizeof(CmAstItemId));
    crate_span.source = options->source;
    crate_span.start = 0u;
    crate_span.end = 0u;
    if (ast->root_items.len > (size_t)UINT32_MAX) {
        cm_lower_fail(&state, CM_HIR_LOWER_INVALID_AST, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "root item count exceeds the HIR ID range");
        goto finish;
    }
    root_item_count = (uint32_t)ast->root_items.len;
    root_items = (const CmAstItemId *)ast->root_items.data;
    for (index = 0u; index < ast->root_items.len; ++index) {
        const CmAstItem *item;

        item = cm_ast_get_item(ast, root_items[index]);
        if (item == NULL) {
            cm_lower_fail(&state, CM_HIR_LOWER_INVALID_AST, crate_span,
                root_items[index], CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
                CM_HIR_OK, "crate root contains an invalid item ID");
            goto finish;
        }
        if (index == 0u || item->span.start < crate_span.start) {
            crate_span.start = item->span.start;
        }
        if (item->span.end > crate_span.end) {
            crate_span.end = item->span.end;
        }
    }
    if (ast->crate_attributes.len != 0u) {
        cm_lower_fail(&state, CM_HIR_LOWER_UNSUPPORTED_ITEM, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "active crate attributes require structural HIR attributes");
        goto finish;
    }
    if (!cm_lower_preflight_source_lifetime_where_predicates(&state,
            root_items, (size_t)root_item_count)) {
        goto finish;
    }
    crate_name = cm_hir_intern(context, options->crate_name);
    status = cm_hir_create_crate(context, crate_name, options->edition,
        crate_span, &state.result.crate_id, &state.result.root_module);
    if (status != CM_HIR_OK) {
        cm_lower_fail_hir(&state, crate_span, CM_AST_ITEM_NONE, status,
            "cannot create HIR crate");
        goto finish;
    }
    if (!cm_lower_reserve_items(&state, state.result.root_module,
            cm_hir_def_id_none(), CM_LOWER_PARENT_NONE, root_items,
            root_item_count)) {
        goto finish;
    }
    if (!cm_lower_predeclare_all_generics(&state)) {
        goto finish;
    }
    if (!cm_lower_prebind_projection_declarations(&state)) {
        goto finish;
    }
    if (!cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_ADT_TYPES)) {
        goto finish;
    }
    if (!cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_TRAIT)) {
        goto finish;
    }
    if (!cm_lower_bind_projection_declarations(&state)) {
        goto finish;
    }
    if (!cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_REMAINING)) {
        goto finish;
    }
    (void)cm_lower_bind_remaining_items(&state);

finish:
    cm_vec_destroy(&state.expanded_source_ids);
    cm_vec_destroy(&state.apit_records);
    cm_vec_destroy(&state.generic_records);
    cm_vec_destroy(&state.macro_records);
    cm_vec_destroy(&state.variant_records);
    cm_vec_destroy(&state.item_records);
    return state.result;
}

CmHirLowerResult cm_hir_lower_expanded_crate(CmHirContext *context,
    const CmAst *ast, const CmExpandedAst *expanded,
    const CmHirLowerOptions *options)
{
    CmLowerState state;
    CmSpan crate_span;
    CmInternId crate_name;
    CmHirStatus status;
    const CmAstItemId *source_root_items;
    size_t index;

    memset(&state, 0, sizeof(state));
    if (context == NULL || ast == NULL || expanded == NULL || options == NULL
        || options->crate_name == NULL || options->crate_name[0] == '\0'
        || options->edition > CM_HIR_EDITION_2024
        || options->source == 0u
        || !cm_lower_library_options_valid(context, options)) {
        state.result.error_count = 1u;
        state.result.first_error.kind = CM_HIR_LOWER_INVALID_ARGUMENT;
        state.result.first_error.hir_status = CM_HIR_INVALID_ARGUMENT;
        (void)snprintf(state.result.first_error.message,
            sizeof(state.result.first_error.message),
            "invalid expanded AST-to-HIR lowering arguments");
        return state.result;
    }
    state.hir = context;
    state.ast = ast;
    state.source = options->source;
    state.options = options;
    state.next_type_inference = 1u;
    state.next_region_inference = 1u;
    cm_vec_init(&state.item_records, sizeof(CmLowerItemRecord));
    cm_vec_init(&state.variant_records, sizeof(CmLowerVariantRecord));
    cm_vec_init(&state.macro_records, sizeof(CmLowerMacroRecord));
    cm_vec_init(&state.generic_records, sizeof(CmLowerGenericRecord));
    cm_vec_init(&state.apit_records, sizeof(CmLowerApitRecord));
    cm_vec_init(&state.expanded_source_ids, sizeof(CmAstItemId));
    crate_span.source = options->source;
    crate_span.start = 0u;
    crate_span.end = 0u;
    if (!expanded->crate_is_active) {
        cm_lower_fail(&state, CM_HIR_LOWER_INACTIVE_CRATE, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "cfg-derived crate is inactive");
        goto finish;
    }
    source_root_items = (const CmAstItemId *)ast->root_items.data;
    if (!cm_lower_validate_effective_attributes(&state,
            expanded->crate_attributes, expanded->crate_attribute_count,
            (const CmAstAttributeId *)ast->crate_attributes.data,
            ast->crate_attributes.len, crate_span, CM_AST_ITEM_NONE)
        || !cm_lower_validate_expanded_items(&state, expanded->root_items,
            expanded->root_item_count, source_root_items,
            ast->root_items.len)) {
        goto finish;
    }
    for (index = 0u; index < expanded->root_item_count; ++index) {
        const CmAstItem *item;

        item = cm_ast_get_item(ast, expanded->root_items[index].source_id);
        if (item == NULL) {
            cm_lower_fail(&state, CM_HIR_LOWER_INVALID_AST, crate_span,
                expanded->root_items[index].source_id, CM_AST_TYPE_NONE,
                CM_AST_PATH_NONE, CM_HIR_OK,
                "validated root view lost its source item");
            goto finish;
        }
        if (index == 0u || item->span.start < crate_span.start) {
            crate_span.start = item->span.start;
        }
        if (item->span.end > crate_span.end) {
            crate_span.end = item->span.end;
        }
    }
    if (expanded->crate_attribute_count != 0u) {
        cm_lower_fail(&state, CM_HIR_LOWER_UNSUPPORTED_ITEM, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "effective non-cfg crate attributes require structural HIR "
            "attributes");
        goto finish;
    }
    if (!cm_lower_preflight_expanded_lifetime_where_predicates(&state,
            expanded->root_items, expanded->root_item_count)) {
        goto finish;
    }
    crate_name = cm_hir_intern(context, options->crate_name);
    status = cm_hir_create_crate(context, crate_name, options->edition,
        crate_span, &state.result.crate_id, &state.result.root_module);
    if (status != CM_HIR_OK) {
        cm_lower_fail_hir(&state, crate_span, CM_AST_ITEM_NONE, status,
            "cannot create cfg-active HIR crate");
        goto finish;
    }
    if (!cm_lower_reserve_expanded_items(&state, state.result.root_module,
            cm_hir_def_id_none(), CM_LOWER_PARENT_NONE,
            expanded->root_items,
            expanded->root_item_count)) {
        goto finish;
    }
    if (!cm_lower_predeclare_all_generics(&state)) {
        goto finish;
    }
    if (!cm_lower_prebind_projection_declarations(&state)) {
        goto finish;
    }
    if (!cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_ADT_TYPES)) {
        goto finish;
    }
    if (!cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_TRAIT)) {
        goto finish;
    }
    if (!cm_lower_bind_projection_declarations(&state)) {
        goto finish;
    }
    if (!cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_REMAINING)) {
        goto finish;
    }
    (void)cm_lower_bind_remaining_items(&state);

finish:
    cm_vec_destroy(&state.expanded_source_ids);
    cm_vec_destroy(&state.apit_records);
    cm_vec_destroy(&state.generic_records);
    cm_vec_destroy(&state.macro_records);
    cm_vec_destroy(&state.variant_records);
    cm_vec_destroy(&state.item_records);
    return state.result;
}

CmHirLowerResult cm_hir_lower_module_graph(CmHirContext *context,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmImportResolver *imports, CmHirModuleMap *modules,
    const CmHirLowerOptions *options)
{
    CmLowerState state;
    CmModuleId graph_root;
    CmSpan crate_span;
    size_t index;
    CmVec traversal;
    CmHirContextMark hir_mark;
    int hir_marked;

    memset(&state, 0, sizeof(state));
    memset(&hir_mark, 0, sizeof(hir_mark));
    memset(&crate_span, 0, sizeof(crate_span));
    hir_marked = 0;
    if (context == NULL || graph == NULL || imports == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE || modules == NULL
        || options == NULL || options->crate_name == NULL
        || options->crate_name[0] == '\0'
        || options->edition > CM_HIR_EDITION_2024
        || !cm_lower_library_options_valid(context, options)) {
        state.result.error_count = 1u;
        state.result.first_error.kind = CM_HIR_LOWER_INVALID_ARGUMENT;
        state.result.first_error.hir_status = CM_HIR_INVALID_ARGUMENT;
        (void)snprintf(state.result.first_error.message,
            sizeof(state.result.first_error.message),
            "invalid module-graph-to-HIR lowering arguments");
        return state.result;
    }
    if (cm_module_graph_revision(graph) != revision) {
        state.result.error_count = 1u;
        state.result.first_error.kind = CM_HIR_LOWER_STALE_GRAPH;
        state.result.first_error.hir_status = CM_HIR_OK;
        (void)snprintf(state.result.first_error.message,
            sizeof(state.result.first_error.message),
            "module graph revision is stale");
        return state.result;
    }
    if (cm_module_graph_error_count(graph) != 0u) {
        state.result.error_count = 1u;
        state.result.first_error.kind = CM_HIR_LOWER_RESOLVER_FAILURE;
        state.result.first_error.hir_status = CM_HIR_OK;
        (void)snprintf(state.result.first_error.message,
            sizeof(state.result.first_error.message),
            "module graph build failed before import-aware HIR lowering");
        return state.result;
    }
    if (cm_import_resolver_revision(imports) != revision
        || !cm_import_resolver_matches_graph(imports, graph)) {
        state.result.error_count = 1u;
        state.result.first_error.kind = CM_HIR_LOWER_STALE_GRAPH;
        state.result.first_error.hir_status = CM_HIR_OK;
        (void)snprintf(state.result.first_error.message,
            sizeof(state.result.first_error.message),
            "import resolver revision does not match the module graph");
        return state.result;
    }
    if (cm_import_error_count(imports) != 0u
        && !cm_lower_import_errors_are_authenticated(graph, revision,
            imports, options)) {
        state.result.error_count = 1u;
        state.result.first_error.kind = CM_HIR_LOWER_RESOLVER_FAILURE;
        state.result.first_error.hir_status = CM_HIR_OK;
        (void)snprintf(state.result.first_error.message,
            sizeof(state.result.first_error.message),
            "import resolution failed before HIR mutation");
        return state.result;
    }
    if (cm_hir_module_map_count(modules) != 0u) {
        state.result.error_count = 1u;
        state.result.first_error.kind = CM_HIR_LOWER_INVALID_ARGUMENT;
        state.result.first_error.hir_status = CM_HIR_INVALID_ARGUMENT;
        (void)snprintf(state.result.first_error.message,
            sizeof(state.result.first_error.message),
            "module map must be empty before graph HIR lowering");
        return state.result;
    }
    state.hir = context;
    state.graph = graph;
    state.imports = imports;
    state.module_map = modules;
    state.graph_revision = revision;
    state.options = options;
    state.authenticated_external_import_errors_only =
        cm_import_error_count(imports) != 0u;
    state.next_type_inference = 1u;
    state.next_region_inference = 1u;
    cm_vec_init(&state.item_records, sizeof(CmLowerItemRecord));
    cm_vec_init(&state.variant_records, sizeof(CmLowerVariantRecord));
    cm_vec_init(&state.macro_records, sizeof(CmLowerMacroRecord));
    cm_vec_init(&state.generic_records, sizeof(CmLowerGenericRecord));
    cm_vec_init(&state.apit_records, sizeof(CmLowerApitRecord));
    cm_vec_init(&state.expanded_source_ids, sizeof(CmAstItemId));
    cm_vec_init(&state.expected_module_bindings,
        sizeof(CmHirModuleMapEntry));
    cm_vec_init(&traversal, sizeof(CmModuleId));
    if (cm_hir_context_mark(context, &hir_mark) != CM_HIR_OK) {
        cm_lower_fail_hir(&state, crate_span, CM_AST_ITEM_NONE,
            CM_HIR_INVALID_ARGUMENT, "cannot mark HIR transaction");
        goto finish;
    }
    hir_marked = 1;

    graph_root = CM_MODULE_NONE;
    if (!cm_lower_graph_preflight(&state, graph, modules, &graph_root,
            &crate_span, &traversal)) {
        goto finish;
    }
    if (!cm_lower_graph_create_modules(&state, graph, modules, &traversal,
            graph_root, crate_span)) {
        goto finish;
    }
    if (!cm_lower_graph_apply_inner_attributes(&state, graph, modules,
            &traversal, graph_root)) {
        goto finish;
    }
    for (index = 0u; index < traversal.len && !state.failed; ++index) {
        CmModuleId graph_module;

        graph_module = *(const CmModuleId *)cm_vec_at_const(&traversal,
            index);
        (void)cm_lower_graph_reserve_items(&state, graph, modules,
            graph_module);
    }
    if (!state.failed) {
        (void)cm_lower_graph_apply_imports(&state, graph, modules,
            &traversal);
    }
    if (!state.failed) {
        (void)cm_lower_predeclare_all_generics(&state);
    }
    if (!state.failed) {
        (void)cm_lower_prebind_projection_declarations(&state);
    }
    if (!state.failed) {
        (void)cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_ADT_TYPES);
    }
    if (!state.failed) {
        (void)cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_TRAIT);
    }
    if (!state.failed) {
        (void)cm_lower_bind_projection_declarations(&state);
    }
    if (!state.failed) {
        (void)cm_lower_predeclare_all_generic_defaults(&state,
            CM_LOWER_DEFAULTS_REMAINING);
    }
    if (!state.failed) {
        (void)cm_lower_bind_remaining_items(&state);
    }

finish:
    if (!state.failed && !cm_lower_graph_snapshot_matches(&state)) {
        cm_lower_fail(&state, CM_HIR_LOWER_STALE_GRAPH, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE, CM_HIR_OK,
            "graph or import resolver changed during HIR lowering");
    }
    if (!state.failed && !cm_lower_module_map_matches_expected(&state)) {
        cm_lower_fail(&state, CM_HIR_LOWER_STALE_GRAPH, crate_span,
            CM_AST_ITEM_NONE, CM_AST_TYPE_NONE, CM_AST_PATH_NONE,
            CM_HIR_OK, "module map changed during HIR lowering");
    }
    if (state.failed) {
        if (cm_hir_module_map_count(modules) != 0u) {
            cm_hir_module_map_clear(modules);
        }
        if (hir_marked) {
            (void)cm_hir_context_rewind(context, &hir_mark);
        }
        state.result.crate_id = CM_HIR_CRATE_NONE;
        state.result.root_module = CM_HIR_MODULE_NONE;
        state.result.lowered_item_count = 0u;
    } else if (hir_marked) {
        (void)cm_hir_context_commit(context, &hir_mark);
    }
    cm_vec_destroy(&traversal);
    cm_vec_destroy(&state.expected_module_bindings);
    cm_vec_destroy(&state.expanded_source_ids);
    cm_vec_destroy(&state.apit_records);
    cm_vec_destroy(&state.generic_records);
    cm_vec_destroy(&state.macro_records);
    cm_vec_destroy(&state.variant_records);
    cm_vec_destroy(&state.item_records);
    return state.result;
}

const char *cm_hir_lower_error_kind_name(CmHirLowerErrorKind kind)
{
    switch (kind) {
    case CM_HIR_LOWER_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_HIR_LOWER_STALE_GRAPH:
        return "stale graph";
    case CM_HIR_LOWER_INVALID_AST:
        return "invalid AST";
    case CM_HIR_LOWER_INACTIVE_CRATE:
        return "inactive crate";
    case CM_HIR_LOWER_DUPLICATE_NAME:
        return "duplicate name";
    case CM_HIR_LOWER_UNSUPPORTED_ITEM:
        return "unsupported item";
    case CM_HIR_LOWER_UNSUPPORTED_TYPE:
        return "unsupported type";
    case CM_HIR_LOWER_UNSUPPORTED_GENERIC:
        return "unsupported generic";
    case CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH:
        return "alias argument mismatch";
    case CM_HIR_LOWER_ALIAS_CYCLE:
        return "alias cycle";
    case CM_HIR_LOWER_INVALID_ALIAS:
        return "invalid alias";
    case CM_HIR_LOWER_INVALID_TRAIT:
        return "invalid trait";
    case CM_HIR_LOWER_INVALID_IMPL:
        return "invalid impl";
    case CM_HIR_LOWER_UNRESOLVED_PATH:
        return "unresolved path";
    case CM_HIR_LOWER_WRONG_NAMESPACE:
        return "wrong namespace";
    case CM_HIR_LOWER_RESOLVER_FAILURE:
        return "resolver failure";
    case CM_HIR_LOWER_HIR_FAILURE:
        return "HIR failure";
    }
    return "unknown lowering error";
}
