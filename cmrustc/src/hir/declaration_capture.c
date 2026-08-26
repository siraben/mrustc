#include "cm/hir/declaration_capture.h"

#include "cm/alloc.h"
#include "library_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmDeclCaptureModule {
    CmResolveModuleInfo graph;
    CmHirModuleId hir_id;
    const CmHirModule *hir;
    unsigned char *path;
    size_t path_length;
    uint32_t local;
} CmDeclCaptureModule;

typedef struct CmDeclCaptureNamespace {
    uint32_t owner_module;
    uint8_t namespace_kind;
    unsigned char *name;
    size_t name_length;
    CmHirLibraryBinding target;
    CmResolveItemRef declaration;
    CmResolveItemRef introduced_by;
    CmAstItemKind item_kind;
    uint32_t export_ordinal;
    uint32_t source_attribute_count;
    CmSpan introduction_span;
    int source_is_generated;
    int is_import;
} CmDeclCaptureNamespace;

typedef struct CmDeclCaptureItem {
    const CmHirItem *item;
    CmHirItemId id;
    uint32_t owner_module;
    uint32_t source_ordinal;
    uint32_t local;
    uint8_t aggregate_repr;
    uint16_t aggregate_flags;
    const unsigned char *lang_item;
    size_t lang_item_length;
} CmDeclCaptureItem;

typedef struct CmDeclCaptureState {
    const CmHirDeclarationCaptureInput *input;
    const CmHirContext *hir;
    const CmHirCrate *crate_value;
    const CmHirLibraryOwnedData *owned;
    CmDeclCaptureModule *modules;
    size_t module_count;
    CmDeclCaptureNamespace *namespace_values;
    size_t namespace_count;
    size_t namespace_capacity;
    CmDeclCaptureItem *traits;
    size_t trait_count;
    CmDeclCaptureItem *items;
    size_t item_count;
    CmDeclCaptureItem *values;
    size_t value_count;
    size_t projected_semantic_attribute_count;
    uint32_t *generic_locals;
    unsigned char primitive_types[CM_HIR_DECL_PRIMITIVE_F64 + 1u];
    unsigned char *generic_types;
    unsigned char *named_item_types;
    unsigned char *application_types;
    unsigned char *compound_types;
} CmDeclCaptureState;

static CmHirDeclarationCaptureResult cm_decl_capture_result(
    CmHirDeclarationCaptureStatus status)
{
    CmHirDeclarationCaptureResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.metadata_status = CM_HIR_DECL_METADATA_OK;
    result.library_status = CM_HIR_LIBRARY_OK;
    return result;
}

static int cm_decl_capture_fail(CmHirDeclarationCaptureResult *result,
    CmHirDeclarationCaptureStage stage,
    CmHirDeclarationCaptureReason reason)
{
    if (result->failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE) {
        result->failure_stage = stage;
        result->failure_reason = reason;
    }
    return 0;
}

static void cm_decl_capture_binding_failure(
    CmHirDeclarationCaptureResult *result,
    CmHirDeclarationCaptureReason reason, const CmResolvedBinding *binding,
    const CmHirLibraryBinding *target, CmResolveItemRef source_item,
    const CmResolveEffectiveItem *effective)
{
    if (result->failure_reason != CM_HIR_DECL_CAPTURE_REASON_NONE) return;
    result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE;
    result->failure_reason = reason;
    result->has_rejected_binding = 1;
    result->rejected_ast_item_kind = binding->item_kind;
    result->rejected_namespace_kind = binding->namespace_kind;
    result->rejected_source_item = source_item;
    if (target != NULL) {
        result->has_rejected_target = 1;
        result->rejected_binding_kind = target->kind;
        result->rejected_definition = target->definition;
    }
    if (effective != NULL) {
        result->has_rejected_span = 1;
        result->rejected_span = effective->span;
    }
}

static void cm_decl_capture_item_failure(
    CmHirDeclarationCaptureResult *result,
    CmHirDeclarationCaptureReason reason, const CmDeclCaptureNamespace *entry,
    const CmHirItem *item, CmHirItemId item_id)
{
    if (result->failure_reason != CM_HIR_DECL_CAPTURE_REASON_NONE) return;
    result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_ITEMS;
    result->failure_reason = reason;
    result->rejected_item = item_id;
    if (entry != NULL) {
        result->has_rejected_binding = 1;
        result->has_rejected_target = 1;
        result->rejected_binding_kind = entry->target.kind;
        result->rejected_ast_item_kind = entry->item_kind;
        result->rejected_namespace_kind = entry->namespace_kind
                == CM_HIR_DECL_NAMESPACE_TYPE
            ? CM_RESOLVE_NAMESPACE_TYPE : CM_RESOLVE_NAMESPACE_VALUE;
        result->rejected_definition = entry->target.definition;
        result->rejected_source_item = entry->introduced_by;
    }
    if (item != NULL) {
        result->has_rejected_span = 1;
        result->rejected_span = item->span;
    }
}

static int cm_decl_capture_reexport_failure(
    CmHirDeclarationCaptureResult *result,
    CmHirDeclarationCaptureReason reason,
    const CmDeclCaptureNamespace *entry, CmSpan span)
{
    if (result->failure_reason != CM_HIR_DECL_CAPTURE_REASON_NONE) return 0;
    result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE;
    result->failure_reason = reason;
    if (entry != NULL) {
        result->has_rejected_binding = 1;
        result->has_rejected_target = 1;
        result->rejected_binding_kind = entry->target.kind;
        result->rejected_ast_item_kind = entry->item_kind;
        result->rejected_namespace_kind = entry->namespace_kind
                == CM_HIR_DECL_NAMESPACE_TYPE
            ? CM_RESOLVE_NAMESPACE_TYPE : CM_RESOLVE_NAMESPACE_VALUE;
        result->rejected_definition = entry->target.definition;
        result->rejected_source_item = entry->introduced_by;
    }
    if (span.source == 0u && entry != NULL)
        span = entry->introduction_span;
    if (span.source != 0u) {
        result->has_rejected_span = 1;
        result->rejected_span = span;
    }
    return 0;
}

static int cm_decl_bytes_equal(const unsigned char *left, size_t left_length,
    const unsigned char *right, size_t right_length)
{
    return left_length == right_length
        && (left_length == 0u || memcmp(left, right, left_length) == 0);
}

static int cm_decl_bytes_compare(const unsigned char *left,
    size_t left_length, const unsigned char *right, size_t right_length)
{
    size_t common;
    int order;
    common = left_length < right_length ? left_length : right_length;
    order = common == 0u ? 0 : memcmp(left, right, common);
    if (order != 0) return order;
    return left_length < right_length ? -1 : left_length > right_length;
}

static int cm_decl_copy_bytes(CmHirDeclarationString *out,
    const void *bytes, size_t length)
{
    out->data = length == 0u ? NULL : (unsigned char *)cm_alloc(length);
    out->length = length;
    if (length != 0u) memcpy(out->data, bytes, length);
    return 1;
}

static int cm_decl_copy_intern(CmHirDeclarationString *out,
    const CmInternedString *value)
{
    return value != NULL && value->len != 0u
        && cm_decl_copy_bytes(out, value->bytes, value->len);
}

static int cm_decl_copy_graph_string(const CmModuleGraph *graph,
    CmResolveStringId id, unsigned char **out, size_t *out_length)
{
    size_t length;
    unsigned char *bytes;
    *out = NULL;
    *out_length = 0u;
    length = cm_module_graph_string_length(graph, id);
    if (length == 0u || length == SIZE_MAX) return 0;
    bytes = (unsigned char *)cm_alloc(length + 1u);
    if (!cm_module_graph_copy_string(graph, id, (char *)bytes, length + 1u)) {
        cm_free(bytes);
        return 0;
    }
    *out = bytes;
    *out_length = length;
    return 1;
}

static int cm_decl_copy_import_string(const CmImportResolver *imports,
    CmResolveStringId id, unsigned char **out, size_t *out_length)
{
    size_t length;
    unsigned char *bytes;
    *out = NULL;
    *out_length = 0u;
    length = cm_import_string_length(imports, id);
    if (length == 0u || length == SIZE_MAX) return 0;
    bytes = (unsigned char *)cm_alloc(length + 1u);
    if (!cm_import_copy_string(imports, id, (char *)bytes, length + 1u)) {
        cm_free(bytes);
        return 0;
    }
    *out = bytes;
    *out_length = length;
    return 1;
}

static int cm_decl_module_compare(const void *left_value,
    const void *right_value)
{
    const CmDeclCaptureModule *left =
        (const CmDeclCaptureModule *)left_value;
    const CmDeclCaptureModule *right =
        (const CmDeclCaptureModule *)right_value;
    return cm_decl_bytes_compare(left->path, left->path_length,
        right->path, right->path_length);
}

static int cm_decl_namespace_compare(const void *left_value,
    const void *right_value)
{
    const CmDeclCaptureNamespace *left =
        (const CmDeclCaptureNamespace *)left_value;
    const CmDeclCaptureNamespace *right =
        (const CmDeclCaptureNamespace *)right_value;
    int order;
    if (left->owner_module != right->owner_module)
        return left->owner_module < right->owner_module ? -1 : 1;
    if (left->namespace_kind != right->namespace_kind)
        return left->namespace_kind < right->namespace_kind ? -1 : 1;
    order = cm_decl_bytes_compare(left->name, left->name_length,
        right->name, right->name_length);
    if (order != 0) return order;
    return left->export_ordinal < right->export_ordinal ? -1
        : left->export_ordinal > right->export_ordinal;
}

static const CmInternedString *cm_decl_item_name(
    const CmDeclCaptureState *state, const CmHirItem *item)
{
    return item == NULL ? NULL
        : cm_interner_get(&state->hir->strings, item->name);
}

static int cm_decl_item_compare(const void *left_value,
    const void *right_value, const CmDeclCaptureState *state)
{
    const CmDeclCaptureItem *left = (const CmDeclCaptureItem *)left_value;
    const CmDeclCaptureItem *right = (const CmDeclCaptureItem *)right_value;
    const CmInternedString *left_name = cm_decl_item_name(state, left->item);
    const CmInternedString *right_name = cm_decl_item_name(state, right->item);
    int order;
    if (left->owner_module != right->owner_module)
        return left->owner_module < right->owner_module ? -1 : 1;
    order = cm_decl_bytes_compare(left_name->bytes, left_name->len,
        right_name->bytes, right_name->len);
    if (order != 0) return order;
    return left->source_ordinal < right->source_ordinal ? -1
        : left->source_ordinal > right->source_ordinal;
}

static void cm_decl_sort_items(CmDeclCaptureItem *items, size_t count,
    const CmDeclCaptureState *state)
{
    size_t index;
    for (index = 1u; index < count; ++index) {
        CmDeclCaptureItem value = items[index];
        size_t cursor = index;
        while (cursor != 0u
            && cm_decl_item_compare(&items[cursor - 1u], &value,
                state) > 0) {
            items[cursor] = items[cursor - 1u];
            cursor -= 1u;
        }
        items[cursor] = value;
    }
}

static CmDeclCaptureModule *cm_decl_module_by_graph(CmDeclCaptureState *state,
    CmModuleId id)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        if (state->modules[index].graph.id == id) return &state->modules[index];
    return NULL;
}

static CmDeclCaptureModule *cm_decl_module_by_definition(
    CmDeclCaptureState *state, CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        if (cm_hir_def_id_equal(state->modules[index].hir->definition,
                definition)) return &state->modules[index];
    return NULL;
}

static CmDeclCaptureModule *cm_decl_module_by_local(
    CmDeclCaptureState *state, uint32_t local)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        if (state->modules[index].local == local) return &state->modules[index];
    return NULL;
}

static int cm_decl_project_module_attributes(CmDeclCaptureState *state,
    const CmHirAttribute *attributes, uint32_t attribute_count,
    CmHirDeclarationCaptureResult *result)
{
    uint32_t index;
    if ((attribute_count == 0u) != (attributes == NULL))
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_MODULES,
            CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID);
    if ((size_t)attribute_count > SIZE_MAX
            - state->projected_semantic_attribute_count)
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_MODULES,
            CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
    for (index = 0u; index < attribute_count; ++index) {
        const CmHirAttribute *attribute = &attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        uint32_t prior;
        if (metadata == NULL || metadata->len == 0u
            || attribute->source_attribute == 0u
            || attribute->span.source == 0u
            || attribute->span.start > attribute->span.end) {
            if (result->failure_reason
                    == CM_HIR_DECL_CAPTURE_REASON_NONE) {
                result->has_rejected_span = attribute->span.source != 0u;
                result->rejected_span = attribute->span;
            }
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_MODULES,
                CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID);
        }
        for (prior = 0u; prior < index; ++prior) {
            if (attributes[prior].span.source == attribute->span.source
                && attributes[prior].source_attribute
                    == attribute->source_attribute) {
                result->has_rejected_span = 1;
                result->rejected_span = attribute->span;
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_MODULES,
                    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID);
            }
        }
    }
    state->projected_semantic_attribute_count += attribute_count;
    return 1;
}

static int cm_decl_collect_modules(CmDeclCaptureState *state,
    CmHirDeclarationCaptureResult *result)
{
    const CmModuleGraph *graph = state->input->graph;
    size_t index;
    if (!cm_decl_project_module_attributes(state,
            state->crate_value->inner_attributes,
            state->crate_value->inner_attribute_count, result)) return 0;
    state->module_count = cm_module_graph_module_count(graph);
    if (state->module_count == 0u
        || state->module_count > CM_HIR_DECL_METADATA_MAX_MODULES
        || cm_hir_module_map_count(state->input->modules)
            != state->module_count) return 0;
    state->modules = (CmDeclCaptureModule *)cm_alloc_zeroed(
        state->module_count, sizeof(*state->modules));
    for (index = 0u; index < state->module_count; ++index) {
        CmDeclCaptureModule *module = &state->modules[index];
        if (!cm_module_graph_get_module_at(graph, index, &module->graph)
            || cm_hir_module_map_lookup_hir(state->input->modules, graph,
                state->input->revision, module->graph.id, state->hir,
                &module->hir_id) != CM_HIR_MODULE_MAP_OK
            || (module->hir = cm_hir_get_module(state->hir,
                module->hir_id)) == NULL
            || module->hir->crate_id != state->input->crate_id
            || !cm_decl_copy_graph_string(graph, module->graph.absolute_path,
                &module->path, &module->path_length)) return 0;
        if (!cm_decl_project_module_attributes(state,
                module->hir->outer_attributes,
                module->hir->outer_attribute_count, result)
            || !cm_decl_project_module_attributes(state,
                module->hir->inner_attributes,
                module->hir->inner_attribute_count, result)) return 0;
    }
    qsort(state->modules, state->module_count, sizeof(*state->modules),
        cm_decl_module_compare);
    for (index = 0u; index < state->module_count; ++index) {
        CmDeclCaptureModule *module = &state->modules[index];
        CmDeclCaptureModule *parent;
        module->local = (uint32_t)(index + 1u);
        if (module->graph.parent == CM_MODULE_NONE) {
            if (module->hir_id != state->crate_value->root_module) return 0;
        } else {
            parent = cm_decl_module_by_graph(state, module->graph.parent);
            if (parent == NULL || parent->local == 0u
                || parent->local >= module->local
                || module->hir->parent != parent->hir_id) return 0;
        }
    }
    return 1;
}

static const CmHirLibraryOwnedModule *cm_decl_owned_module(
    const CmHirLibraryOwnedData *owned, CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < owned->modules.len; ++index) {
        const CmHirLibraryOwnedModule *module =
            (const CmHirLibraryOwnedModule *)cm_vec_at_const(
                &owned->modules, index);
        if (module != NULL && cm_hir_def_id_equal(module->definition,
                definition)) return module;
    }
    return NULL;
}

static const CmHirLibraryOwnedValue *cm_decl_owned_value(
    const CmHirLibraryOwnedData *owned, CmHirDefId definition)
{
    size_t index;
    if (owned == NULL) return NULL;
    for (index = 0u; index < owned->values.len; ++index) {
        const CmHirLibraryOwnedValue *value =
            (const CmHirLibraryOwnedValue *)cm_vec_at_const(
                &owned->values, index);
        if (value != NULL && cm_hir_def_id_equal(
                value->declaration.definition, definition)) return value;
    }
    return NULL;
}

static int cm_decl_library_binding(const CmHirLibraryOwnedData *owned,
    const CmHirLibraryOwnedModule *module, int value_namespace,
    const unsigned char *name, size_t name_length,
    CmHirLibraryBinding *out)
{
    size_t index;
    size_t matches = 0u;
    memset(out, 0, sizeof(*out));
    for (index = 0u; index < module->entries.len; ++index) {
        const CmHirLibraryOwnedEntry *entry =
            (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
                &module->entries, index);
        const CmInternedString *entry_name = entry == NULL ? NULL
            : cm_interner_get(&owned->names, entry->name);
        int entry_value = entry != NULL
            && (entry->kind == CM_HIR_LIBRARY_BINDING_VALUE
                || entry->kind
                    == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
                || (entry->kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
                    && entry->enum_variant_namespace
                        == CM_HIR_LIBRARY_ENUM_VARIANT_VALUE));
        if (entry_name != NULL && entry_value == value_namespace
            && cm_decl_bytes_equal(entry_name->bytes, entry_name->len,
                name, name_length)) {
            out->kind = entry->kind;
            out->definition = entry->target;
            out->type_kind = entry->type_kind;
            out->primitive_kind = entry->primitive_kind;
            out->value_kind = entry->value_kind;
            out->enum_definition = entry->enum_definition;
            out->enum_variant_index = entry->enum_variant_index;
            out->enum_variant_form = entry->enum_variant_form;
            out->enum_variant_namespace = entry->enum_variant_namespace;
            matches += 1u;
        }
    }
    return matches == 1u;
}

static int cm_decl_item_ref_equal(CmResolveItemRef left,
    CmResolveItemRef right)
{
    return left.source == right.source && left.item == right.item;
}

static int cm_decl_effective_ordinal(const CmDeclCaptureState *state,
    CmModuleId module, CmResolveItemRef declaration, uint32_t *out_ordinal,
    CmResolveEffectiveItem *out_effective)
{
    CmResolveModuleInfo information;
    uint32_t index;
    size_t matches = 0u;
    if (!cm_module_graph_get_module(state->input->graph, module,
            &information)) return 0;
    for (index = 0u; index < information.effective_item_count; ++index) {
        CmResolveEffectiveItem effective;
        if (cm_module_graph_get_effective_item(state->input->graph,
                state->input->revision, module, index, &effective)
                != CM_RESOLVE_VIEW_OK) return 0;
        if (cm_decl_item_ref_equal(effective.declaration, declaration)) {
            if (out_effective != NULL) *out_effective = effective;
            *out_ordinal = index;
            matches += 1u;
        }
    }
    return matches == 1u;
}

static uint8_t cm_decl_resolver_primitive(
    CmResolvePrimitiveKind primitive)
{
    switch (primitive) {
    case CM_RESOLVE_PRIMITIVE_BOOL: return CM_HIR_DECL_PRIMITIVE_BOOL;
    case CM_RESOLVE_PRIMITIVE_CHAR: return CM_HIR_DECL_PRIMITIVE_CHAR;
    case CM_RESOLVE_PRIMITIVE_STR: return CM_HIR_DECL_PRIMITIVE_STR;
    case CM_RESOLVE_PRIMITIVE_I8: return CM_HIR_DECL_PRIMITIVE_I8;
    case CM_RESOLVE_PRIMITIVE_I16: return CM_HIR_DECL_PRIMITIVE_I16;
    case CM_RESOLVE_PRIMITIVE_I32: return CM_HIR_DECL_PRIMITIVE_I32;
    case CM_RESOLVE_PRIMITIVE_I64: return CM_HIR_DECL_PRIMITIVE_I64;
    case CM_RESOLVE_PRIMITIVE_I128: return CM_HIR_DECL_PRIMITIVE_I128;
    case CM_RESOLVE_PRIMITIVE_ISIZE: return CM_HIR_DECL_PRIMITIVE_ISIZE;
    case CM_RESOLVE_PRIMITIVE_U8: return CM_HIR_DECL_PRIMITIVE_U8;
    case CM_RESOLVE_PRIMITIVE_U16: return CM_HIR_DECL_PRIMITIVE_U16;
    case CM_RESOLVE_PRIMITIVE_U32: return CM_HIR_DECL_PRIMITIVE_U32;
    case CM_RESOLVE_PRIMITIVE_U64: return CM_HIR_DECL_PRIMITIVE_U64;
    case CM_RESOLVE_PRIMITIVE_U128: return CM_HIR_DECL_PRIMITIVE_U128;
    case CM_RESOLVE_PRIMITIVE_USIZE: return CM_HIR_DECL_PRIMITIVE_USIZE;
    case CM_RESOLVE_PRIMITIVE_F32: return CM_HIR_DECL_PRIMITIVE_F32;
    case CM_RESOLVE_PRIMITIVE_F64: return CM_HIR_DECL_PRIMITIVE_F64;
    default: return 0u;
    }
}

static uint8_t cm_decl_library_primitive(CmHirPrimitiveKind primitive)
{
    switch (primitive) {
    case CM_HIR_PRIMITIVE_BOOL: return CM_HIR_DECL_PRIMITIVE_BOOL;
    case CM_HIR_PRIMITIVE_CHAR: return CM_HIR_DECL_PRIMITIVE_CHAR;
    case CM_HIR_PRIMITIVE_STR: return CM_HIR_DECL_PRIMITIVE_STR;
    case CM_HIR_PRIMITIVE_I8: return CM_HIR_DECL_PRIMITIVE_I8;
    case CM_HIR_PRIMITIVE_I16: return CM_HIR_DECL_PRIMITIVE_I16;
    case CM_HIR_PRIMITIVE_I32: return CM_HIR_DECL_PRIMITIVE_I32;
    case CM_HIR_PRIMITIVE_I64: return CM_HIR_DECL_PRIMITIVE_I64;
    case CM_HIR_PRIMITIVE_I128: return CM_HIR_DECL_PRIMITIVE_I128;
    case CM_HIR_PRIMITIVE_ISIZE: return CM_HIR_DECL_PRIMITIVE_ISIZE;
    case CM_HIR_PRIMITIVE_U8: return CM_HIR_DECL_PRIMITIVE_U8;
    case CM_HIR_PRIMITIVE_U16: return CM_HIR_DECL_PRIMITIVE_U16;
    case CM_HIR_PRIMITIVE_U32: return CM_HIR_DECL_PRIMITIVE_U32;
    case CM_HIR_PRIMITIVE_U64: return CM_HIR_DECL_PRIMITIVE_U64;
    case CM_HIR_PRIMITIVE_U128: return CM_HIR_DECL_PRIMITIVE_U128;
    case CM_HIR_PRIMITIVE_USIZE: return CM_HIR_DECL_PRIMITIVE_USIZE;
    case CM_HIR_PRIMITIVE_F32: return CM_HIR_DECL_PRIMITIVE_F32;
    case CM_HIR_PRIMITIVE_F64: return CM_HIR_DECL_PRIMITIVE_F64;
    default: return 0u;
    }
}

static int cm_decl_namespace_target_shape(const CmResolvedBinding *binding,
    const CmHirLibraryBinding *target)
{
    if (target->kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
        uint8_t resolver_primitive = cm_decl_resolver_primitive(
            binding->primitive_kind);
        uint8_t library_primitive = cm_decl_library_primitive(
            target->primitive_kind);
        /* A builtin primitive has no AST declaration.  CmAstItemKind's zero
         * value happens to print as FUNCTION, so it is deliberately not an
         * authority for this already-authenticated synthetic binding. */
        return resolver_primitive != 0u
            && resolver_primitive == library_primitive
            && binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && binding->target_module == CM_MODULE_NONE
            && binding->declaration.source == 0u
            && binding->declaration.item == CM_AST_ITEM_NONE
            && binding->variant.enumeration.source == 0u
            && binding->variant.enumeration.item == CM_AST_ITEM_NONE
            && binding->variant.index == 0u
            && cm_hir_def_id_is_none(target->definition)
            && target->type_kind == CM_HIR_TYPE_ERROR_KIND
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    }
    if (target->kind == CM_HIR_LIBRARY_BINDING_MODULE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && binding->item_kind == CM_AST_ITEM_MODULE;
    if (target->kind == CM_HIR_LIBRARY_BINDING_TRAIT)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && binding->item_kind == CM_AST_ITEM_TRAIT;
    if (target->kind == CM_HIR_LIBRARY_BINDING_VALUE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
            && ((binding->item_kind == CM_AST_ITEM_FUNCTION
                    && target->value_kind == CM_HIR_LIBRARY_VALUE_FUNCTION)
                || (binding->item_kind == CM_AST_ITEM_CONST
                    && target->value_kind == CM_HIR_LIBRARY_VALUE_CONST)
                || (binding->item_kind == CM_AST_ITEM_STATIC
                    && target->value_kind == CM_HIR_LIBRARY_VALUE_STATIC));
    if (target->kind == CM_HIR_LIBRARY_BINDING_TYPE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && (binding->item_kind == CM_AST_ITEM_STRUCT
                || binding->item_kind == CM_AST_ITEM_UNION
                || binding->item_kind == CM_AST_ITEM_ENUM
                || binding->item_kind == CM_AST_ITEM_TYPE_ALIAS)
            && target->type_kind == (binding->item_kind == CM_AST_ITEM_STRUCT
                    || binding->item_kind == CM_AST_ITEM_UNION
                    || binding->item_kind == CM_AST_ITEM_ENUM
                ? CM_HIR_TYPE_ADT_KIND : CM_HIR_TYPE_ALIAS_APPLICATION_KIND)
            && target->primitive_kind == CM_HIR_PRIMITIVE_NONE
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    if (target->kind == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
            && binding->item_kind == CM_AST_ITEM_STRUCT
            && target->type_kind == CM_HIR_TYPE_ADT_KIND
            && target->primitive_kind == CM_HIR_PRIMITIVE_NONE
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    if (target->kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT)
        return binding->item_kind == CM_AST_ITEM_ENUM
            && target->type_kind == CM_HIR_TYPE_ADT_KIND
            && target->primitive_kind == CM_HIR_PRIMITIVE_NONE
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE
            && (target->enum_variant_form == CM_HIR_AGGREGATE_UNIT
                || target->enum_variant_form == CM_HIR_AGGREGATE_TUPLE)
            && ((binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
                    && target->enum_variant_namespace
                        == CM_HIR_LIBRARY_ENUM_VARIANT_TYPE)
                || (binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
                    && target->enum_variant_namespace
                        == CM_HIR_LIBRARY_ENUM_VARIANT_VALUE));
    return 0;
}

static int cm_decl_primitive_reexport_provenance(
    const CmDeclCaptureState *state, const CmDeclCaptureNamespace *entry);

static const CmHirItem *cm_decl_enum_variant_parent(
    const CmDeclCaptureState *state, const CmHirLibraryBinding *target,
    CmHirItemId *out_item_id)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirVariant *variant;
    if (target == NULL
        || target->kind != CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        || cm_hir_def_id_is_none(target->definition)
        || cm_hir_def_id_is_none(target->enum_definition)) return NULL;
    definition = cm_hir_lookup_definition(state->hir, target->definition);
    if (definition == NULL || definition->state != CM_HIR_DEFINITION_BOUND
        || definition->kind != CM_HIR_DEFINITION_ENUM_VARIANT
        || definition->id.crate_id != state->input->crate_id
        || definition->entity.enum_variant.variant_index
            != target->enum_variant_index) return NULL;
    *out_item_id = definition->entity.enum_variant.enum_item_id;
    item = cm_hir_get_item(state->hir, *out_item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_ENUM
        || !cm_hir_def_id_equal(item->definition, target->enum_definition)
        || item->definition.crate_id != state->input->crate_id
        || target->enum_variant_index >= item->data.enum_item.variant_count
        || item->data.enum_item.variants == NULL) return NULL;
    variant = &item->data.enum_item.variants[target->enum_variant_index];
    return cm_hir_def_id_equal(variant->definition, target->definition)
            && variant->form == target->enum_variant_form
            && (variant->form == CM_HIR_AGGREGATE_UNIT
                || variant->form == CM_HIR_AGGREGATE_TUPLE)
        ? item : NULL;
}

static int cm_decl_enum_variant_mate(const CmDeclCaptureState *state,
    size_t entry_index)
{
    const CmDeclCaptureNamespace *entry =
        &state->namespace_values[entry_index];
    size_t index;
    size_t matches = 0u;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *mate = &state->namespace_values[index];
        if (mate->target.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
            && mate->owner_module == entry->owner_module
            && mate->namespace_kind != entry->namespace_kind
            && mate->export_ordinal == entry->export_ordinal
            && mate->source_attribute_count == entry->source_attribute_count
            && mate->source_is_generated == entry->source_is_generated
            && mate->is_import == entry->is_import
            && mate->item_kind == entry->item_kind
            && mate->introduction_span.source
                == entry->introduction_span.source
            && mate->introduction_span.start
                == entry->introduction_span.start
            && mate->introduction_span.end == entry->introduction_span.end
            && cm_hir_def_id_equal(mate->target.definition,
                entry->target.definition)
            && cm_hir_def_id_equal(mate->target.enum_definition,
                entry->target.enum_definition)
            && mate->target.enum_variant_index
                == entry->target.enum_variant_index
            && mate->target.enum_variant_form
                == entry->target.enum_variant_form
            && mate->target.enum_variant_namespace
                != entry->target.enum_variant_namespace
            && cm_decl_item_ref_equal(mate->declaration,
                entry->declaration)
            && cm_decl_item_ref_equal(mate->introduced_by,
                entry->introduced_by)
            && cm_decl_bytes_equal(mate->name, mate->name_length,
                entry->name, entry->name_length)) matches += 1u;
    }
    return matches == 1u;
}

static int cm_decl_collect_namespace(CmDeclCaptureState *state,
    const CmHirLibraryOwnedData *owned,
    CmHirDeclarationCaptureResult *result)
{
    size_t module_index;
    size_t capacity = 0u;
    size_t owned_entry_count = 0u;
    for (module_index = 0u; module_index < state->module_count;
            ++module_index) {
        const CmDeclCaptureModule *module = &state->modules[module_index];
        const CmHirLibraryOwnedModule *owned_module =
            cm_decl_owned_module(owned, module->hir->definition);
        int namespace_index;
        if (owned_module == NULL)
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_MODULE_MISSING);
        if (owned_module->entries.len > SIZE_MAX - owned_entry_count)
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT);
        owned_entry_count += owned_module->entries.len;
        for (namespace_index = 0; namespace_index < 2; ++namespace_index) {
            CmResolveNamespace namespace_kind =
                (CmResolveNamespace)namespace_index;
            size_t binding_count = cm_import_binding_count(
                state->input->imports, module->graph.id, namespace_kind);
            uint32_t binding_index;
            if (binding_count > (size_t)UINT32_MAX)
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                    CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT);
            for (binding_index = 0u; (size_t)binding_index < binding_count;
                    ++binding_index) {
                CmResolvedBinding binding;
                if (!cm_import_get_binding(state->input->imports,
                        module->graph.id, namespace_kind, binding_index,
                        &binding))
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_LOOKUP_FAILED);
                if (!binding.is_public) continue;
                if (capacity == CM_HIR_DECL_METADATA_MAX_NAMESPACE_ENTRIES)
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                        CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT);
                capacity += 1u;
            }
        }
    }
    state->namespace_values = capacity == 0u ? NULL
        : (CmDeclCaptureNamespace *)cm_alloc_zeroed(capacity,
            sizeof(*state->namespace_values));
    state->namespace_capacity = capacity;
    for (module_index = 0u; module_index < state->module_count;
            ++module_index) {
        const CmDeclCaptureModule *module = &state->modules[module_index];
        const CmHirLibraryOwnedModule *owned_module =
            cm_decl_owned_module(owned, module->hir->definition);
        int namespace_index;
        for (namespace_index = 0; namespace_index < 2; ++namespace_index) {
            CmResolveNamespace namespace_kind =
                (CmResolveNamespace)namespace_index;
            size_t binding_count = cm_import_binding_count(
                state->input->imports, module->graph.id, namespace_kind);
            uint32_t binding_index;
            if (binding_count > (size_t)UINT32_MAX)
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                    CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT);
            for (binding_index = 0u; (size_t)binding_index < binding_count;
                    ++binding_index) {
                CmResolvedBinding binding;
                CmDeclCaptureNamespace *entry;
                CmResolveItemRef introduced;
                CmResolveEffectiveItem effective;
                uint32_t effective_index = 0u;
                int has_effective;
                if (!cm_import_get_binding(state->input->imports,
                        module->graph.id, namespace_kind, binding_index,
                        &binding))
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_LOOKUP_FAILED);
                if (!binding.is_public) continue;
                introduced = binding.is_import ? binding.import_declaration
                    : binding.declaration;
                has_effective = introduced.source != 0u
                    && introduced.item != CM_AST_ITEM_NONE
                    && cm_decl_effective_ordinal(state, module->graph.id,
                        introduced, &effective_index, &effective);
                if (binding.is_ambiguous || binding.is_anonymous
                    || binding.revision != state->input->revision
                    || binding.module != module->graph.id) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_AUTHORITY_INVALID,
                        &binding, NULL, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                if (state->namespace_count >= capacity) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT,
                        &binding, NULL, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                entry = &state->namespace_values[state->namespace_count];
                if (!cm_decl_copy_import_string(state->input->imports,
                        binding.name, &entry->name, &entry->name_length)) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_NAME_INVALID,
                        &binding, NULL, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                if (!cm_decl_library_binding(owned, owned_module,
                        namespace_kind == CM_RESOLVE_NAMESPACE_VALUE,
                        entry->name, entry->name_length, &entry->target)) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_LIBRARY_MISMATCH,
                        &binding, NULL, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                if (!cm_decl_namespace_target_shape(&binding,
                        &entry->target)) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
                        &binding, &entry->target, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                if (introduced.source == 0u
                    || introduced.item == CM_AST_ITEM_NONE
                    || !has_effective) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_INTRODUCTION_INVALID,
                        &binding, &entry->target, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                entry->owner_module = module->local;
                entry->namespace_kind = namespace_kind
                        == CM_RESOLVE_NAMESPACE_TYPE
                    ? CM_HIR_DECL_NAMESPACE_TYPE
                    : CM_HIR_DECL_NAMESPACE_VALUE;
                entry->declaration = binding.declaration;
                entry->introduced_by = introduced;
                entry->item_kind = binding.item_kind;
                entry->export_ordinal = effective_index;
                entry->source_attribute_count = effective.attribute_count;
                entry->introduction_span = effective.span;
                entry->source_is_generated = effective.is_generated;
                entry->is_import = binding.is_import;
                state->namespace_count += 1u;
            }
        }
    }
    /* The owned artifact has one entry for every effective public binding. */
    if (owned_entry_count != state->namespace_count)
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
            CM_HIR_DECL_CAPTURE_REASON_BINDING_CENSUS_MISMATCH);
    qsort(state->namespace_values, state->namespace_count,
        sizeof(*state->namespace_values), cm_decl_namespace_compare);
    for (module_index = 1u; module_index < state->namespace_count;
            ++module_index) {
        CmDeclCaptureNamespace *prior =
            &state->namespace_values[module_index - 1u];
        CmDeclCaptureNamespace *entry =
            &state->namespace_values[module_index];
        if (prior->owner_module == entry->owner_module
            && prior->namespace_kind == entry->namespace_kind
            && cm_decl_bytes_equal(prior->name, prior->name_length,
                entry->name, entry->name_length))
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                CM_HIR_DECL_CAPTURE_REASON_BINDING_DUPLICATE);
    }
    for (module_index = 0u; module_index < state->namespace_count;
            ++module_index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[module_index];
        CmHirItemId ignored_item;
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
            if (!cm_decl_primitive_reexport_provenance(state, entry)) {
                cm_decl_capture_reexport_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
                    entry, entry->introduction_span);
                return 0;
            }
            continue;
        }
        if (entry->target.kind != CM_HIR_LIBRARY_BINDING_ENUM_VARIANT)
            continue;
        if (!entry->is_import || entry->source_is_generated
            || cm_decl_enum_variant_parent(state, &entry->target,
                &ignored_item) == NULL
            || !cm_decl_enum_variant_mate(state, module_index)) {
            cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
                entry, entry->introduction_span);
            return 0;
        }
    }
    return 1;
}

static int cm_decl_item_source(const CmDeclCaptureState *state,
    CmHirDefId definition, CmAstItemKind expected_kind,
    uint32_t *out_module, uint32_t *out_ordinal)
{
    size_t index;
    size_t matches = 0u;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (entry->item_kind == expected_kind
            && cm_hir_def_id_equal(entry->target.definition, definition)) {
            if (entry->source_is_generated
                || entry->source_attribute_count != 0u) return 0;
            if (entry->is_import) continue;
            *out_module = entry->owner_module;
            *out_ordinal = entry->export_ordinal;
            matches += 1u;
        }
    }
    return matches == 1u;
}

static int cm_decl_struct_entry_mate(const CmDeclCaptureState *state,
    size_t entry_index)
{
    const CmDeclCaptureNamespace *entry =
        &state->namespace_values[entry_index];
    CmHirLibraryBindingKind mate_kind = entry->target.kind
            == CM_HIR_LIBRARY_BINDING_TYPE
        ? CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
        : CM_HIR_LIBRARY_BINDING_TYPE;
    size_t index;
    size_t matches = 0u;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *mate = &state->namespace_values[index];
        if (mate->target.kind == mate_kind
            && mate->owner_module == entry->owner_module
            && mate->namespace_kind != entry->namespace_kind
            && mate->export_ordinal == entry->export_ordinal
            && mate->source_attribute_count
                == entry->source_attribute_count
            && mate->source_is_generated == entry->source_is_generated
            && mate->is_import == entry->is_import
            && mate->item_kind == CM_AST_ITEM_STRUCT
            && cm_hir_def_id_equal(mate->target.definition,
                entry->target.definition)
            && cm_decl_item_ref_equal(mate->declaration,
                entry->declaration)
            && cm_decl_item_ref_equal(mate->introduced_by,
                entry->introduced_by)
            && cm_decl_bytes_equal(mate->name, mate->name_length,
                entry->name, entry->name_length)) matches += 1u;
    }
    return matches == 1u;
}

static int cm_decl_struct_source(const CmDeclCaptureState *state,
    const CmHirItem *item, int non_exhaustive, uint32_t *out_module,
    uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *type_source = NULL;
    const CmDeclCaptureNamespace *constructor_source = NULL;
    size_t index;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (entry->item_kind != CM_AST_ITEM_STRUCT
            || (entry->target.kind != CM_HIR_LIBRARY_BINDING_TYPE
                && entry->target.kind
                    != CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR)) return 0;
        if (non_exhaustive) {
            if (entry->target.kind
                    == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
                || cm_decl_struct_entry_mate(state, index)) return 0;
        } else if (!cm_decl_struct_entry_mate(state, index)) return 0;
        if (entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            if (entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
                if (type_source != NULL) return 0;
                type_source = entry;
            } else {
                if (constructor_source != NULL) return 0;
                constructor_source = entry;
            }
        }
    }
    if (type_source == NULL
        || (non_exhaustive && constructor_source != NULL)
        || (!non_exhaustive && (constructor_source == NULL
            || type_source->owner_module != constructor_source->owner_module
            || type_source->export_ordinal
                != constructor_source->export_ordinal
            || !cm_decl_item_ref_equal(type_source->declaration,
                constructor_source->declaration)
            || !cm_decl_item_ref_equal(type_source->introduced_by,
                constructor_source->introduced_by)))) return 0;
    *out_module = type_source->owner_module;
    *out_ordinal = type_source->export_ordinal;
    return 1;
}

static int cm_decl_named_aggregate_source(const CmDeclCaptureState *state,
    const CmHirItem *item, CmAstItemKind expected_kind,
    uint32_t *out_module, uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    size_t index;
    size_t direct_count = 0u;
    if (item_name == NULL || item_name->len == 0u
        || (expected_kind != CM_AST_ITEM_STRUCT
            && expected_kind != CM_AST_ITEM_UNION)) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (entry->item_kind != expected_kind
            || entry->target.kind != CM_HIR_LIBRARY_BINDING_TYPE
            || entry->target.type_kind != CM_HIR_TYPE_ADT_KIND
            || entry->target.primitive_kind != CM_HIR_PRIMITIVE_NONE
            || entry->target.value_kind != CM_HIR_LIBRARY_VALUE_NONE
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
            || entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            source = entry;
            direct_count += 1u;
        }
    }
    if (direct_count != 1u || source == NULL) return 0;
    *out_module = source->owner_module;
    *out_ordinal = source->export_ordinal;
    return 1;
}

static int cm_decl_alias_source(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    size_t index;
    size_t direct_count = 0u;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (entry->item_kind != CM_AST_ITEM_TYPE_ALIAS
            || entry->target.kind != CM_HIR_LIBRARY_BINDING_TYPE
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
            || entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            source = entry;
            direct_count += 1u;
        }
    }
    if (direct_count != 1u || source == NULL) return 0;
    *out_module = source->owner_module;
    *out_ordinal = source->export_ordinal;
    return 1;
}

static int cm_decl_enum_source(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    size_t index;
    size_t direct_count = 0u;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        /* ENUM identities are module TYPE bindings only. Variants are owned
         * by the ITEM and never fabricate a module VALUE mate. */
        if (entry->item_kind != CM_AST_ITEM_ENUM
            || entry->target.kind != CM_HIR_LIBRARY_BINDING_TYPE
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
            || entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            source = entry;
            direct_count += 1u;
        }
    }
    if (direct_count != 1u || source == NULL) return 0;
    *out_module = source->owner_module;
    *out_ordinal = source->export_ordinal;
    return 1;
}

static int cm_decl_free_value_source(const CmDeclCaptureState *state,
    const CmHirItem *item, CmAstItemKind ast_kind,
    CmHirLibraryValueKind library_kind, uint32_t *out_module,
    uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    size_t index;
    size_t direct_count = 0u;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (entry->item_kind != ast_kind
            || entry->target.kind != CM_HIR_LIBRARY_BINDING_VALUE
            || entry->target.value_kind != library_kind
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_VALUE
            || entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            source = entry;
            direct_count += 1u;
        }
    }
    if (direct_count != 1u || source == NULL) return 0;
    *out_module = source->owner_module;
    *out_ordinal = source->export_ordinal;
    return 1;
}

static int cm_decl_const_source(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal)
{
    return cm_decl_free_value_source(state, item, CM_AST_ITEM_CONST,
        CM_HIR_LIBRARY_VALUE_CONST, out_module, out_ordinal);
}

static int cm_decl_static_source(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal)
{
    return cm_decl_free_value_source(state, item, CM_AST_ITEM_STATIC,
        CM_HIR_LIBRARY_VALUE_STATIC, out_module, out_ordinal);
}

static int cm_decl_effective_attribute_matches_hir(
    const CmDeclCaptureState *state,
    const CmResolveEffectiveAttribute *effective,
    const CmHirAttribute *attribute, CmResolveItemRef owner)
{
    unsigned char *metadata = NULL;
    size_t metadata_length = 0u;
    const CmInternedString *hir_metadata = cm_interner_get(
        &state->hir->strings, attribute->metadata);
    int equal;
    if (effective->source != owner.source
        || !cm_decl_item_ref_equal(effective->owner, owner)
        || effective->style != CM_AST_ATTR_OUTER
        || effective->source_attribute != attribute->source_attribute
        || effective->expansion_depth != attribute->expansion_depth
        || effective->span.source != attribute->span.source
        || effective->span.start != attribute->span.start
        || effective->span.end != attribute->span.end
        || hir_metadata == NULL || hir_metadata->len == 0u
        || !cm_decl_copy_graph_string(state->input->graph,
            effective->metadata, &metadata, &metadata_length)) return 0;
    equal = cm_decl_bytes_equal(metadata, metadata_length,
        hir_metadata->bytes, hir_metadata->len);
    cm_free(metadata);
    return equal;
}

static int cm_decl_item_attribute_provenance(
    const CmDeclCaptureState *state, const CmHirItem *item,
    CmAstItemKind expected_kind,
    CmHirLibraryBindingKind expected_binding_kind)
{
    const CmDeclCaptureNamespace *source = NULL;
    CmDeclCaptureModule *module;
    CmResolveEffectiveItem effective;
    uint32_t index;
    size_t namespace_index;
    size_t matches = 0u;
    for (namespace_index = 0u; namespace_index < state->namespace_count;
            ++namespace_index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[namespace_index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (!entry->is_import
            && entry->source_attribute_count != item->attribute_count)
            return 0;
        if (!entry->is_import
            && entry->target.kind == expected_binding_kind
            && entry->item_kind == expected_kind) {
            source = entry;
            matches += 1u;
        }
    }
    if (matches != 1u || source == NULL || source->source_is_generated
        || !cm_decl_item_ref_equal(source->declaration,
            source->introduced_by)) return 0;
    module = cm_decl_module_by_local((CmDeclCaptureState *)state,
        source->owner_module);
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id, source->export_ordinal,
            &effective) != CM_RESOLVE_VIEW_OK
        || effective.is_generated || effective.item_kind != expected_kind
        || !cm_decl_item_ref_equal(effective.declaration,
            source->declaration)
        || effective.attribute_count != item->attribute_count) return 0;
    for (index = 0u; index < effective.attribute_count; ++index) {
        CmResolveEffectiveAttribute graph_attribute;
        if (cm_module_graph_get_effective_item_attribute(
                state->input->graph, state->input->revision,
                module->graph.id, effective.id, index, &graph_attribute)
                != CM_RESOLVE_VIEW_OK
            || !cm_decl_effective_attribute_matches_hir(state,
                &graph_attribute, &item->attributes[index],
                source->declaration)) return 0;
    }
    return 1;
}

enum {
    CM_DECL_ATTR_STABLE = 1u << 0,
    CM_DECL_ATTR_UNSTABLE = 1u << 1,
    CM_DECL_ATTR_DEPRECATED = 1u << 2,
    CM_DECL_ATTR_DERIVE = 1u << 3,
    CM_DECL_ATTR_NON_EXHAUSTIVE = 1u << 4,
    CM_DECL_ATTR_ALLOW = 1u << 5,
    CM_DECL_ATTR_DOC_ALIAS = 1u << 6,
    CM_DECL_ATTR_REPR_U8 = 1u << 7,
    CM_DECL_ATTR_DIAGNOSTIC_ITEM = 1u << 8,
    CM_DECL_ATTR_LANG_ITEM = 1u << 9,
    CM_DECL_ATTR_REPR_TRANSPARENT = 1u << 10,
    CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT = 1u << 11,
    CM_DECL_ATTR_DOC_HIDDEN = 1u << 12,
    CM_DECL_ATTR_DOC_NO_INLINE = 1u << 13,
    CM_DECL_ATTR_DOC_SEARCH_UNBOX = 1u << 14,
    CM_DECL_ATTR_MUST_USE = 1u << 15,
    CM_DECL_ATTR_RUSTFMT_SKIP = 1u << 16,
    CM_DECL_ATTR_DOC_INLINE = 1u << 17
};

enum {
    CM_DECL_ENUM_PROFILE_U8_EXPLICIT = 0,
    CM_DECL_ENUM_PROFILE_DEFAULT_UNIT = 1,
    CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC = 2
};

static int cm_decl_plain_visibility(const CmHirItem *item);
static unsigned int cm_decl_attribute_kind(
    const CmInternedString *metadata);
static uint8_t cm_decl_primitive(const CmHirType *type);
static int cm_decl_ascii_identifier(const unsigned char *bytes,
    size_t length);
static int cm_decl_ast_ordinary_enum_generics(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstItem *ast_item, const CmHirItem *item);
static int cm_decl_ast_type_matches_hir_field(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type,
    const CmHirItem *owner, size_t depth);
static int cm_decl_field_visibility_matches(CmAstVisibility ast_visibility,
    CmHirVisibility hir_visibility);

static int cm_decl_graph_string_matches_intern(
    const CmDeclCaptureState *state, CmResolveStringId graph_id,
    CmInternId hir_id)
{
    unsigned char *graph_bytes = NULL;
    size_t graph_length = 0u;
    const CmInternedString *hir_value = cm_interner_get(
        &state->hir->strings, hir_id);
    int equal;
    if (hir_value == NULL || hir_value->len == 0u
        || !cm_decl_copy_graph_string(state->input->graph, graph_id,
            &graph_bytes, &graph_length)) return 0;
    equal = cm_decl_bytes_equal(graph_bytes, graph_length,
        hir_value->bytes, hir_value->len);
    cm_free(graph_bytes);
    return equal;
}

static int cm_decl_diagnostic_item_name(const CmInternedString *metadata,
    const unsigned char **out_name, size_t *out_length)
{
    static const unsigned char prefix[] = "rustc_diagnostic_item = \"";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t length;
    if (out_name != NULL) *out_name = NULL;
    if (out_length != NULL) *out_length = 0u;
    if (metadata == NULL || metadata->len <= prefix_length + 1u
        || memcmp(metadata->bytes, prefix, prefix_length) != 0
        || metadata->bytes[metadata->len - 1u] != (unsigned char)'\"')
        return 0;
    length = metadata->len - prefix_length - 1u;
    if (!cm_decl_ascii_identifier(metadata->bytes + prefix_length, length))
        return 0;
    if (out_name != NULL) *out_name = metadata->bytes + prefix_length;
    if (out_length != NULL) *out_length = length;
    return 1;
}

static int cm_decl_lang_item_name(const CmInternedString *metadata,
    const unsigned char **out_name, size_t *out_length)
{
    static const unsigned char prefix[] = "lang = \"";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t length;
    if (out_name != NULL) *out_name = NULL;
    if (out_length != NULL) *out_length = 0u;
    if (metadata == NULL || metadata->len <= prefix_length + 1u
        || memcmp(metadata->bytes, prefix, prefix_length) != 0
        || metadata->bytes[metadata->len - 1u] != (unsigned char)'\"')
        return 0;
    length = metadata->len - prefix_length - 1u;
    if (!cm_decl_ascii_identifier(metadata->bytes + prefix_length, length))
        return 0;
    if (out_name != NULL) *out_name = metadata->bytes + prefix_length;
    if (out_length != NULL) *out_length = length;
    return 1;
}

static int cm_decl_enum_item_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, size_t *out_projected_count,
    int *out_profile, const unsigned char **out_lang,
    size_t *out_lang_length)
{
    uint32_t index;
    unsigned int seen = 0u;
    const unsigned char *lang = NULL;
    size_t lang_length = 0u;
    size_t retained = 0u;
    *out_profile = CM_DECL_ENUM_PROFILE_U8_EXPLICIT;
    *out_lang = NULL;
    *out_lang_length = 0u;
    if ((item->attribute_count == 0u) != (item->attributes == NULL)) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || (kind != CM_DECL_ATTR_STABLE
                && kind != CM_DECL_ATTR_UNSTABLE
                && kind != CM_DECL_ATTR_DEPRECATED
                && kind != CM_DECL_ATTR_DERIVE
                && kind != CM_DECL_ATTR_ALLOW
                && kind != CM_DECL_ATTR_REPR_U8
                && kind != CM_DECL_ATTR_DIAGNOSTIC_ITEM
                && kind != CM_DECL_ATTR_LANG_ITEM
                && kind != CM_DECL_ATTR_DOC_SEARCH_UNBOX
                && kind != CM_DECL_ATTR_MUST_USE)
            || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior) {
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        }
        if (kind == CM_DECL_ATTR_LANG_ITEM
            && !cm_decl_lang_item_name(metadata, &lang, &lang_length))
            return 0;
        seen |= kind;
    }
    if (!cm_decl_item_attribute_provenance(state, item,
            CM_AST_ITEM_ENUM, CM_HIR_LIBRARY_BINDING_TYPE)) return 0;
    if ((seen & CM_DECL_ATTR_STABLE) != 0u
        && (seen & CM_DECL_ATTR_UNSTABLE) != 0u) return 0;
    if (item->generic_parameter_count == 0u
        && seen == CM_DECL_ATTR_DIAGNOSTIC_ITEM) {
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, item->attributes[0].metadata);
        if (!cm_decl_diagnostic_item_name(metadata, NULL, NULL)) return 0;
        /* The diagnostic identity is retained in ITEM, not projected away. */
        *out_projected_count = 0u;
        *out_profile = CM_DECL_ENUM_PROFILE_DEFAULT_UNIT;
    } else if (seen == (CM_DECL_ATTR_DERIVE | CM_DECL_ATTR_UNSTABLE
            | CM_DECL_ATTR_REPR_U8)) {
        /* repr(u8) is normalized into ITEM; derive and unstable are omitted. */
        *out_projected_count = 2u;
    } else if (item->generic_parameter_count != 0u
        && (seen & (CM_DECL_ATTR_DIAGNOSTIC_ITEM
                | CM_DECL_ATTR_DERIVE | CM_DECL_ATTR_DOC_SEARCH_UNBOX))
            == (CM_DECL_ATTR_DIAGNOSTIC_ITEM
                | CM_DECL_ATTR_DERIVE | CM_DECL_ATTR_DOC_SEARCH_UNBOX)
        && ((seen & CM_DECL_ATTR_STABLE) != 0u
            || (seen & CM_DECL_ATTR_UNSTABLE) != 0u)
        && (seen & CM_DECL_ATTR_REPR_U8) == 0u) {
        for (index = 0u; index < item->attribute_count; ++index) {
            const CmInternedString *metadata = cm_interner_get(
                &state->hir->strings, item->attributes[index].metadata);
            if (cm_decl_attribute_kind(metadata)
                    == CM_DECL_ATTR_DIAGNOSTIC_ITEM
                && !cm_decl_diagnostic_item_name(metadata, NULL, NULL))
                return 0;
        }
        retained = 1u;
        if ((seen & CM_DECL_ATTR_LANG_ITEM) != 0u) retained += 1u;
        if ((size_t)item->attribute_count < retained) return 0;
        *out_projected_count = (size_t)item->attribute_count - retained;
        *out_profile = CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC;
        *out_lang = lang;
        *out_lang_length = lang_length;
    } else {
        return 0;
    }
    return 1;
}

static int cm_decl_enum_variant_attribute(
    const CmDeclCaptureState *state, const CmDeclCaptureModule *module,
    const CmResolveEffectiveItem *enumeration,
    const CmResolveEffectiveVariant *effective, uint32_t variant_index,
    const CmAst *ast, const CmAstVariant *ast_variant)
{
    CmResolveEffectiveAttribute attribute;
    const CmAstAttribute *source_attribute;
    const CmInternedString *source_metadata;
    unsigned char *graph_metadata = NULL;
    size_t graph_metadata_length = 0u;
    CmInternedString metadata_view;
    int valid;
    if (effective->attribute_count != 1u
        || cm_module_graph_get_effective_variant_attribute(
            state->input->graph, state->input->revision, module->graph.id,
            enumeration->id, variant_index, 0u, &attribute)
            != CM_RESOLVE_VIEW_OK
        || attribute.source == 0u
        || attribute.source_attribute == 0u
        || attribute.style != CM_AST_ATTR_OUTER
        || attribute.expansion_depth != 0u
        || attribute.span.source != attribute.source
        || attribute.span.source != effective->span.source
        || attribute.span.start > attribute.span.end
        || attribute.span.start < effective->span.start
        || attribute.span.end > effective->span.end
        || !cm_decl_item_ref_equal(attribute.owner,
            enumeration->declaration)
        || !cm_decl_item_ref_equal(attribute.owner_variant.enumeration,
            effective->declaration.enumeration)
        || attribute.owner_variant.index != effective->declaration.index
        || ast == NULL
        || ast_variant == NULL || ast_variant->attribute_count != 1u
        || ast_variant->attributes == NULL
        || ast_variant->attributes[0] != attribute.source_attribute
        || (source_attribute = cm_ast_get_attribute(ast,
            attribute.source_attribute)) == NULL
        || source_attribute->style != CM_AST_ATTR_OUTER
        || source_attribute->span.start > attribute.span.start
        || source_attribute->span.end < attribute.span.end
        || (source_metadata = cm_ast_get_string(ast,
            source_attribute->text)) == NULL
        || source_metadata->len == 0u
        || !cm_decl_copy_graph_string(state->input->graph,
            attribute.metadata, &graph_metadata,
            &graph_metadata_length)) return 0;
    metadata_view.bytes = graph_metadata;
    metadata_view.len = graph_metadata_length;
    valid = graph_metadata_length <= SIZE_MAX - 3u
        && source_metadata->len == graph_metadata_length + 3u
        && source_metadata->bytes[0] == (unsigned char)'#'
        && source_metadata->bytes[1] == (unsigned char)'['
        && source_metadata->bytes[source_metadata->len - 1u]
            == (unsigned char)']'
        && memcmp(source_metadata->bytes + 2u, graph_metadata,
            graph_metadata_length) == 0
        && cm_decl_attribute_kind(&metadata_view) == CM_DECL_ATTR_UNSTABLE;
    cm_free(graph_metadata);
    return valid;
}

static int cm_decl_enum_generic_variant_attributes(
    const CmDeclCaptureState *state, const CmDeclCaptureModule *module,
    const CmResolveEffectiveItem *enumeration,
    const CmResolveEffectiveVariant *effective, uint32_t variant_index,
    const CmAst *ast, const CmAstVariant *ast_variant,
    CmInternId hir_lang_item, CmHirDeclarationString *out_lang)
{
    const CmInternedString *hir_lang = cm_interner_get(&state->hir->strings,
        hir_lang_item);
    unsigned int seen = 0u;
    uint32_t index;
    if (out_lang != NULL) {
        out_lang->data = NULL;
        out_lang->length = 0u;
    }
    if (hir_lang == NULL || hir_lang->len == 0u
        || effective->attribute_count != 2u || ast == NULL
        || ast_variant == NULL || ast_variant->attribute_count != 2u
        || ast_variant->attributes == NULL) return 0;
    for (index = 0u; index < effective->attribute_count; ++index) {
        CmResolveEffectiveAttribute attribute;
        const CmAstAttribute *source_attribute;
        const CmInternedString *source_metadata;
        unsigned char *graph_metadata = NULL;
        size_t graph_metadata_length = 0u;
        CmInternedString metadata_view;
        unsigned int kind;
        const unsigned char *lang = NULL;
        size_t lang_length = 0u;
        int valid;
        if (cm_module_graph_get_effective_variant_attribute(
                state->input->graph, state->input->revision,
                module->graph.id, enumeration->id, variant_index, index,
                &attribute) != CM_RESOLVE_VIEW_OK
            || attribute.source == 0u || attribute.source_attribute == 0u
            || attribute.style != CM_AST_ATTR_OUTER
            || attribute.expansion_depth != 0u
            || attribute.span.source != attribute.source
            || attribute.span.source != effective->span.source
            || attribute.span.start > attribute.span.end
            || attribute.span.start < effective->span.start
            || attribute.span.end > effective->span.end
            || !cm_decl_item_ref_equal(attribute.owner,
                enumeration->declaration)
            || !cm_decl_item_ref_equal(attribute.owner_variant.enumeration,
                effective->declaration.enumeration)
            || attribute.owner_variant.index != effective->declaration.index
            || ast_variant->attributes[index] != attribute.source_attribute
            || (source_attribute = cm_ast_get_attribute(ast,
                attribute.source_attribute)) == NULL
            || source_attribute->style != CM_AST_ATTR_OUTER
            || source_attribute->span.start > attribute.span.start
            || source_attribute->span.end < attribute.span.end
            || (source_metadata = cm_ast_get_string(ast,
                source_attribute->text)) == NULL
            || !cm_decl_copy_graph_string(state->input->graph,
                attribute.metadata, &graph_metadata,
                &graph_metadata_length)) return 0;
        metadata_view.bytes = graph_metadata;
        metadata_view.len = graph_metadata_length;
        kind = cm_decl_attribute_kind(&metadata_view);
        valid = graph_metadata_length <= SIZE_MAX - 3u
            && source_metadata->len == graph_metadata_length + 3u
            && source_metadata->bytes[0] == (unsigned char)'#'
            && source_metadata->bytes[1] == (unsigned char)'['
            && source_metadata->bytes[source_metadata->len - 1u]
                == (unsigned char)']'
            && memcmp(source_metadata->bytes + 2u, graph_metadata,
                graph_metadata_length) == 0
            && (kind == CM_DECL_ATTR_STABLE
                || kind == CM_DECL_ATTR_LANG_ITEM)
            && (seen & kind) == 0u;
        if (valid && kind == CM_DECL_ATTR_LANG_ITEM)
            valid = cm_decl_lang_item_name(&metadata_view, &lang,
                &lang_length);
        if (valid && kind == CM_DECL_ATTR_LANG_ITEM)
            valid = hir_lang->len == lang_length
                && memcmp(hir_lang->bytes, lang, lang_length) == 0;
        if (valid && out_lang != NULL && kind == CM_DECL_ATTR_LANG_ITEM)
            valid = cm_decl_copy_bytes(out_lang, lang, lang_length);
        cm_free(graph_metadata);
        if (!valid) return 0;
        seen |= kind;
    }
    return seen == (CM_DECL_ATTR_STABLE | CM_DECL_ATTR_LANG_ITEM);
}

static int cm_decl_enum_variant_has_no_attributes(
    const CmResolveEffectiveVariant *effective,
    const CmAstVariant *ast_variant)
{
    return effective != NULL && ast_variant != NULL
        && effective->attribute_count == 0u
        && ast_variant->attribute_count == 0u
        && ast_variant->attributes == NULL;
}

static int cm_decl_parse_u8_decimal(const CmInternedString *text,
    uint64_t *out_value)
{
    size_t index;
    uint64_t value = 0u;
    if (text == NULL || text->len == 0u) return 0;
    for (index = 0u; index < text->len; ++index) {
        unsigned int digit;
        if (text->bytes[index] < (unsigned char)'0'
            || text->bytes[index] > (unsigned char)'9') return 0;
        digit = (unsigned int)(text->bytes[index] - (unsigned char)'0');
        if (value > (UINT64_C(255) - digit) / UINT64_C(10)) return 0;
        value = value * UINT64_C(10) + digit;
    }
    *out_value = value;
    return 1;
}

static int cm_decl_parse_u64_decimal(const CmInternedString *text,
    uint64_t *out_value)
{
    size_t index;
    uint64_t value = 0u;
    if (text == NULL || text->len == 0u) return 0;
    for (index = 0u; index < text->len; ++index) {
        unsigned int digit;
        if (text->bytes[index] < (unsigned char)'0'
            || text->bytes[index] > (unsigned char)'9') return 0;
        digit = (unsigned int)(text->bytes[index] - (unsigned char)'0');
        if (value > (UINT64_MAX - digit) / UINT64_C(10)) return 0;
        value = value * UINT64_C(10) + digit;
    }
    *out_value = value;
    return 1;
}

static int cm_decl_enum_shape_and_variants(const CmDeclCaptureState *state,
    const CmHirItem *item, CmHirItemId item_id, uint32_t owner_module,
    uint32_t source_ordinal, int profile,
    size_t *out_projected_count)
{
    CmDeclCaptureModule *module;
    CmResolveEffectiveItem enumeration;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    uint32_t index;
    uint32_t prior_source_ordinal = 0u;
    int saw_tuple = 0;
    if (item->kind != CM_HIR_ITEM_ENUM || !cm_decl_plain_visibility(item)
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC
            ? (item->generic_parameter_count == 0u
                || item->generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE)
            : (item->generic_parameter_start
                    != CM_HIR_GENERIC_PARAM_NONE
                || item->generic_parameter_count != 0u))
        || item->predicate_scopes != NULL
        || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || item->data.enum_item.variant_count == 0u
        || (size_t)item->data.enum_item.variant_count
            > CM_HIR_DECL_METADATA_MAX_VARIANTS
        || item->data.enum_item.variants == NULL) return 0;
    module = cm_decl_module_by_local((CmDeclCaptureState *)state,
        owner_module);
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id, source_ordinal,
            &enumeration) != CM_RESOLVE_VIEW_OK
        || enumeration.is_generated
        || enumeration.item_kind != CM_AST_ITEM_ENUM
        || enumeration.variant_count != item->data.enum_item.variant_count
        || !cm_module_graph_borrow_item_ast(state->input->graph,
            module->graph.id, enumeration.declaration, &ast)
        || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            enumeration.declaration.item)) == NULL
        || ast_item->kind != CM_AST_ITEM_ENUM
        || ast_item->data.enum_item.variant_count == 0u
        || ast_item->data.enum_item.variants == NULL
        || (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC
            && !cm_decl_ast_ordinary_enum_generics(state, ast, ast_item,
                item)))
        return 0;
    for (index = 0u; index < item->data.enum_item.variant_count; ++index) {
        const CmHirVariant *variant = &item->data.enum_item.variants[index];
        const CmAstVariant *ast_variant;
        const CmInternedString *ast_discriminant;
        const CmInternedString *name = cm_interner_get(&state->hir->strings,
            variant->name);
        const CmHirDefinition *definition = cm_hir_lookup_definition(
            state->hir, variant->definition);
        const CmHirType *discriminant_type = NULL;
        CmResolveEffectiveVariant effective;
        uint64_t source_discriminant;
        uint32_t prior;
        if (cm_module_graph_get_effective_variant(state->input->graph,
                state->input->revision, module->graph.id, enumeration.id,
                index, &effective) != CM_RESOLVE_VIEW_OK
            || effective.declaration.index
                >= ast_item->data.enum_item.variant_count
            || (index != 0u && effective.declaration.index
                <= prior_source_ordinal)) return 0;
        ast_variant = &ast_item->data.enum_item.variants[
            effective.declaration.index];
        ast_discriminant = ast_variant->discriminant == CM_INTERN_ID_NONE
            ? NULL : cm_ast_get_string(ast, ast_variant->discriminant);
        if (name == NULL || name->len == 0u || definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ENUM_VARIANT
            || definition->state != CM_HIR_DEFINITION_BOUND
            || definition->entity.enum_variant.enum_item_id != item_id
            || definition->entity.enum_variant.variant_index != index
            || definition->span.source != variant->span.source
            || definition->span.start != variant->span.start
            || definition->span.end != variant->span.end
            || variant->span.source != item->span.source
            || variant->span.start > variant->span.end
            || variant->span.start < item->span.start
            || variant->span.end > item->span.end
            || effective.is_generated
            || !cm_decl_item_ref_equal(effective.declaration.enumeration,
                enumeration.declaration)
            || effective.span.source != variant->span.source
            || effective.span.start != variant->span.start
            || effective.span.end != variant->span.end
            || !cm_decl_graph_string_matches_intern(state, effective.name,
                variant->name)) return 0;
        if (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC) {
            uint32_t field_index;
            CmAstFieldForm ast_form = variant->form
                    == CM_HIR_AGGREGATE_UNIT
                ? CM_AST_FIELDS_UNIT : CM_AST_FIELDS_TUPLE;
            if ((variant->form != CM_HIR_AGGREGATE_UNIT
                    && variant->form != CM_HIR_AGGREGATE_TUPLE)
                || effective.form != ast_form || ast_variant->form != ast_form
                || effective.field_count != variant->field_count
                || ast_variant->field_count != variant->field_count
                || ((variant->field_count == 0u)
                    != (variant->fields == NULL))
                || ((ast_variant->field_count == 0u)
                    != (ast_variant->fields == NULL))
                || (variant->form == CM_HIR_AGGREGATE_UNIT
                    && variant->field_count != 0u)
                || (variant->form == CM_HIR_AGGREGATE_TUPLE
                    && variant->field_count == 0u)
                || ast_variant->discriminant != CM_INTERN_ID_NONE
                || ast_discriminant != NULL || variant->has_discriminant
                || variant->discriminant.kind != 0
                || variant->discriminant.type != CM_HIR_TYPE_NONE
                || variant->discriminant.data.value.low_bits != 0u
                || variant->discriminant.data.value.high_bits != 0u
                || !cm_decl_enum_generic_variant_attributes(state, module,
                    &enumeration, &effective, index, ast, ast_variant,
                    variant->lang_item, NULL))
                return 0;
            for (field_index = 0u; field_index < variant->field_count;
                    ++field_index) {
                const CmHirField *field = &variant->fields[field_index];
                const CmAstField *ast_field =
                    &ast_variant->fields[field_index];
                const CmAstType *ast_type = cm_ast_get_type(ast,
                    ast_field->type);
                const CmHirType *hir_type = cm_hir_get_type(state->hir,
                    field->type);
                if (field->name != CM_INTERN_ID_NONE
                    || !cm_decl_field_visibility_matches(
                        ast_field->visibility, field->visibility)
                    || field->visibility.kind != CM_HIR_VIS_PRIVATE
                    || hir_type == NULL
                    || hir_type->kind != CM_HIR_TYPE_PARAMETER_KIND
                    || field->span.source != item->span.source
                    || field->span.start != item->span.start
                    || field->span.end != item->span.end
                    || !cm_decl_ast_type_matches_hir_field(state, ast,
                        ast_type, hir_type, item, 0u)) return 0;
            }
            if (variant->form == CM_HIR_AGGREGATE_TUPLE) saw_tuple = 1;
        } else if (profile == CM_DECL_ENUM_PROFILE_DEFAULT_UNIT) {
            if (variant->lang_item != CM_INTERN_ID_NONE
                || variant->form != CM_HIR_AGGREGATE_UNIT
                || variant->fields != NULL || variant->field_count != 0u
                || effective.form != CM_AST_FIELDS_UNIT
                || effective.field_count != 0u
                || ast_variant->form != CM_AST_FIELDS_UNIT
                || ast_variant->fields != NULL
                || ast_variant->field_count != 0u
                || ast_variant->discriminant != CM_INTERN_ID_NONE
                || ast_discriminant != NULL || variant->has_discriminant
                || variant->discriminant.kind != 0
                || variant->discriminant.type != CM_HIR_TYPE_NONE
                || variant->discriminant.data.value.low_bits != 0u
                || variant->discriminant.data.value.high_bits != 0u
                || !cm_decl_enum_variant_has_no_attributes(&effective,
                    ast_variant)) return 0;
        } else {
            if (variant->lang_item != CM_INTERN_ID_NONE
                || variant->form != CM_HIR_AGGREGATE_UNIT
                || variant->fields != NULL || variant->field_count != 0u
                || effective.form != CM_AST_FIELDS_UNIT
                || effective.field_count != 0u
                || ast_variant->form != CM_AST_FIELDS_UNIT
                || ast_variant->fields != NULL
                || ast_variant->field_count != 0u
                || ast_variant->discriminant == CM_INTERN_ID_NONE
                || !cm_decl_parse_u8_decimal(ast_discriminant,
                    &source_discriminant)
                || !variant->has_discriminant
                || variant->discriminant.kind != CM_HIR_CONST_VALUE
                || (discriminant_type = cm_hir_get_type(state->hir,
                    variant->discriminant.type)) == NULL
                || discriminant_type->kind != CM_HIR_TYPE_INTEGER_KIND
                || discriminant_type->data.integer_type.kind
                    != CM_HIR_INT_ISIZE
                || variant->discriminant.data.value.high_bits != 0u
                || variant->discriminant.data.value.low_bits > UINT64_C(255)
                || variant->discriminant.data.value.low_bits
                    != source_discriminant
                || !cm_decl_enum_variant_attribute(state, module,
                    &enumeration, &effective, index, ast, ast_variant))
                return 0;
        }
        for (prior = 0u; prior < index; ++prior) {
            const CmHirVariant *prior_variant =
                &item->data.enum_item.variants[prior];
            if (prior_variant->name == variant->name
                || (profile == CM_DECL_ENUM_PROFILE_U8_EXPLICIT
                    && prior_variant->discriminant.data.value.low_bits
                        == variant->discriminant.data.value.low_bits)) return 0;
        }
        prior_source_ordinal = effective.declaration.index;
    }
    if (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC) {
        uint32_t generic_index;
        if (!saw_tuple) return 0;
        for (generic_index = 0u;
                generic_index < item->generic_parameter_count;
                ++generic_index) {
            CmHirGenericParamId expected = item->generic_parameter_start
                + generic_index;
            int used = 0;
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->data.enum_item.variant_count;
                    ++variant_index) {
                const CmHirVariant *variant =
                    &item->data.enum_item.variants[variant_index];
                uint32_t field_index;
                for (field_index = 0u; field_index < variant->field_count;
                        ++field_index) {
                    const CmHirType *field_type = cm_hir_get_type(state->hir,
                        variant->fields[field_index].type);
                    if (field_type != NULL
                        && field_type->kind == CM_HIR_TYPE_PARAMETER_KIND
                        && field_type->data.parameter_type.parameter
                            == expected) used = 1;
                }
            }
            if (!used) return 0;
        }
    }
    if (profile != CM_DECL_ENUM_PROFILE_DEFAULT_UNIT) {
        if ((size_t)item->data.enum_item.variant_count > SIZE_MAX
                - *out_projected_count) return 0;
        *out_projected_count += item->data.enum_item.variant_count;
    }
    return 1;
}

static int cm_decl_item_already(const CmDeclCaptureItem *items,
    size_t count, CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < count; ++index)
        if (cm_hir_def_id_equal(items[index].item->definition, definition))
            return 1;
    return 0;
}

static const CmHirItem *cm_decl_bound_item(const CmHirContext *hir,
    CmHirDefId definition, CmHirItemId *out_id)
{
    const CmHirDefinition *resolved = cm_hir_lookup_definition(hir,
        definition);
    if (resolved == NULL || resolved->state != CM_HIR_DEFINITION_BOUND
        || resolved->kind != CM_HIR_DEFINITION_ITEM
        || resolved->entity.item_id == CM_HIR_ITEM_NONE) return NULL;
    *out_id = resolved->entity.item_id;
    return cm_hir_get_item(hir, *out_id);
}

static int cm_decl_plain_visibility(const CmHirItem *item)
{
    return item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_is_none(item->visibility.restriction);
}

static int cm_decl_attribute_call_is(const CmInternedString *metadata,
    const char *head)
{
    size_t head_length = strlen(head);
    return metadata != NULL && metadata->len > head_length + 2u
        && memcmp(metadata->bytes, head, head_length) == 0
        && metadata->bytes[head_length] == (unsigned char)'('
        && metadata->bytes[metadata->len - 1u] == (unsigned char)')';
}

static int cm_decl_attribute_bare_is(const CmInternedString *metadata,
    const char *name)
{
    size_t length = strlen(name);
    return metadata != NULL && metadata->len == length
        && memcmp(metadata->bytes, name, length) == 0;
}

static int cm_decl_ascii_identifier(const unsigned char *bytes, size_t length)
{
    size_t index;
    if (bytes == NULL || length == 0u
        || !((bytes[0] >= (unsigned char)'a'
                && bytes[0] <= (unsigned char)'z')
            || (bytes[0] >= (unsigned char)'A'
                && bytes[0] <= (unsigned char)'Z')
            || bytes[0] == (unsigned char)'_')) return 0;
    for (index = 1u; index < length; ++index) {
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

static int cm_decl_attribute_doc_alias_is(const CmInternedString *metadata)
{
    static const unsigned char prefix[] = "doc(alias(\"";
    static const unsigned char suffix[] = "\"))";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t suffix_length = sizeof(suffix) - 1u;
    size_t identifier_length;
    if (metadata == NULL
        || metadata->len <= prefix_length + suffix_length
        || memcmp(metadata->bytes, prefix, prefix_length) != 0
        || memcmp(metadata->bytes + metadata->len - suffix_length,
            suffix, suffix_length) != 0) return 0;
    identifier_length = metadata->len - prefix_length - suffix_length;
    return cm_decl_ascii_identifier(metadata->bytes + prefix_length,
        identifier_length);
}

static int cm_decl_attribute_must_use_is(const CmInternedString *metadata)
{
    static const unsigned char prefix[] = "must_use = \"";
    size_t prefix_length = sizeof(prefix) - 1u;
    return metadata != NULL && metadata->len > prefix_length + 1u
        && memcmp(metadata->bytes, prefix, prefix_length) == 0
        && metadata->bytes[metadata->len - 1u] == (unsigned char)'\"';
}

static unsigned int cm_decl_attribute_kind(const CmInternedString *metadata)
{
    if (cm_decl_attribute_call_is(metadata, "stable"))
        return CM_DECL_ATTR_STABLE;
    if (cm_decl_attribute_call_is(metadata, "unstable"))
        return CM_DECL_ATTR_UNSTABLE;
    if (cm_decl_attribute_call_is(metadata, "deprecated"))
        return CM_DECL_ATTR_DEPRECATED;
    if (cm_decl_attribute_call_is(metadata, "derive"))
        return CM_DECL_ATTR_DERIVE;
    if (cm_decl_attribute_bare_is(metadata, "non_exhaustive"))
        return CM_DECL_ATTR_NON_EXHAUSTIVE;
    if (cm_decl_attribute_call_is(metadata, "allow"))
        return CM_DECL_ATTR_ALLOW;
    if (cm_decl_attribute_doc_alias_is(metadata))
        return CM_DECL_ATTR_DOC_ALIAS;
    if (cm_decl_attribute_bare_is(metadata, "repr(u8)"))
        return CM_DECL_ATTR_REPR_U8;
    if (cm_decl_diagnostic_item_name(metadata, NULL, NULL))
        return CM_DECL_ATTR_DIAGNOSTIC_ITEM;
    if (cm_decl_lang_item_name(metadata, NULL, NULL))
        return CM_DECL_ATTR_LANG_ITEM;
    if (cm_decl_attribute_bare_is(metadata, "repr(transparent)"))
        return CM_DECL_ATTR_REPR_TRANSPARENT;
    if (cm_decl_attribute_bare_is(metadata, "rustc_pub_transparent"))
        return CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT;
    if (cm_decl_attribute_bare_is(metadata, "doc(hidden)"))
        return CM_DECL_ATTR_DOC_HIDDEN;
    if (cm_decl_attribute_bare_is(metadata, "doc(no_inline)"))
        return CM_DECL_ATTR_DOC_NO_INLINE;
    if (cm_decl_attribute_bare_is(metadata, "doc(search_unbox)"))
        return CM_DECL_ATTR_DOC_SEARCH_UNBOX;
    if (cm_decl_attribute_must_use_is(metadata))
        return CM_DECL_ATTR_MUST_USE;
    if (cm_decl_attribute_bare_is(metadata, "rustfmt::skip"))
        return CM_DECL_ATTR_RUSTFMT_SKIP;
    if (cm_decl_attribute_bare_is(metadata, "doc(inline)"))
        return CM_DECL_ATTR_DOC_INLINE;
    return 0u;
}

static int cm_decl_project_item_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, unsigned int allowed,
    size_t *out_projected_count, int *out_non_exhaustive)
{
    uint32_t index;
    unsigned int seen = 0u;
    *out_projected_count = 0u;
    *out_non_exhaustive = 0;
    if ((item->attribute_count == 0u) != (item->attributes == NULL)) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || kind == 0u || (allowed & kind) == 0u
            || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior) {
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        }
        seen |= kind;
        *out_projected_count += 1u;
    }
    if ((seen & CM_DECL_ATTR_STABLE) != 0u
        && (seen & CM_DECL_ATTR_UNSTABLE) != 0u) return 0;
    *out_non_exhaustive = (seen & CM_DECL_ATTR_NON_EXHAUSTIVE) != 0u;
    return 1;
}

static int cm_decl_static_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, size_t *out_projected_count)
{
    int non_exhaustive = 0;
    const CmInternedString *metadata;
    if (item->attribute_count != 1u || item->attributes == NULL
        || (metadata = cm_interner_get(&state->hir->strings,
            item->attributes[0].metadata)) == NULL
        || cm_decl_attribute_kind(metadata) != CM_DECL_ATTR_DOC_HIDDEN
        || !cm_decl_project_item_attributes(state, item,
            CM_DECL_ATTR_DOC_HIDDEN, out_projected_count, &non_exhaustive)
        || non_exhaustive || *out_projected_count != 1u) return 0;
    return 1;
}

static int cm_decl_aggregate_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, uint8_t *out_repr, uint16_t *out_flags,
    const unsigned char **out_lang, size_t *out_lang_length,
    size_t *out_projected_count)
{
    uint32_t index;
    unsigned int seen = 0u;
    unsigned int expected;
    const unsigned char *lang = NULL;
    size_t lang_length = 0u;
    if ((item->attribute_count == 0u) != (item->attributes == NULL)) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || (kind != CM_DECL_ATTR_STABLE
                && kind != CM_DECL_ATTR_UNSTABLE
                && kind != CM_DECL_ATTR_DERIVE
                && kind != CM_DECL_ATTR_LANG_ITEM
                && kind != CM_DECL_ATTR_REPR_TRANSPARENT
                && kind != CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT)
            || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior) {
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        }
        if (kind == CM_DECL_ATTR_LANG_ITEM
            && !cm_decl_lang_item_name(metadata, &lang, &lang_length)) return 0;
        seen |= kind;
    }
    if ((seen & CM_DECL_ATTR_STABLE) != 0u
        && (seen & CM_DECL_ATTR_UNSTABLE) != 0u) return 0;
    if (item->kind == CM_HIR_ITEM_STRUCT
        && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && item->generic_parameter_count == 0u) {
        expected = CM_DECL_ATTR_UNSTABLE | CM_DECL_ATTR_DERIVE
            | CM_DECL_ATTR_LANG_ITEM;
        *out_repr = CM_HIR_DECL_AGGREGATE_REPR_RUST;
        *out_flags = CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
    } else if ((item->kind == CM_HIR_ITEM_STRUCT
            || item->kind == CM_HIR_ITEM_UNION)
        && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && item->generic_parameter_count == 1u) {
        expected = CM_DECL_ATTR_DERIVE | CM_DECL_ATTR_LANG_ITEM
            | CM_DECL_ATTR_REPR_TRANSPARENT
            | CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT;
        if ((seen & CM_DECL_ATTR_STABLE) != 0u)
            expected |= CM_DECL_ATTR_STABLE;
        else if ((seen & CM_DECL_ATTR_UNSTABLE) != 0u)
            expected |= CM_DECL_ATTR_UNSTABLE;
        else return 0;
        *out_repr = CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT;
        *out_flags = CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
            | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    } else return 0;
    if (seen != expected || lang == NULL || lang_length == 0u) return 0;
    *out_lang = lang;
    *out_lang_length = lang_length;
    /* Stability and derive are the only omitted semantic attributes here. */
    *out_projected_count = 2u;
    return 1;
}

static const CmHirImport *cm_decl_reexport_import(
    const CmDeclCaptureModule *module, CmResolveItemRef declaration)
{
    const CmHirImport *match = NULL;
    uint32_t index;
    if (module == NULL || module->hir == NULL) return NULL;
    for (index = 0u; index < module->hir->import_count; ++index) {
        const CmHirImport *candidate = &module->hir->imports[index];
        if (candidate->span.source == declaration.source
            && candidate->source_item == declaration.item) {
            if (match != NULL) return NULL;
            match = candidate;
        }
    }
    return match;
}

static int cm_decl_primitive_reexport_provenance(
    const CmDeclCaptureState *state, const CmDeclCaptureNamespace *entry)
{
    CmDeclCaptureModule *module;
    CmResolveEffectiveItem effective;
    const CmHirImport *import;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    const CmInternedString *ast_tree;
    const CmInternedString *hir_tree;
    uint8_t primitive;
    size_t leaf_count;
    size_t declaration_count;
    size_t matched_leaf_count = 0u;
    size_t matched_binding_count = 0u;
    size_t index;
    int matched_entry = 0;
    if (entry == NULL
        || entry->target.kind != CM_HIR_LIBRARY_BINDING_PRIMITIVE
        || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
        || !entry->is_import || entry->source_is_generated
        || entry->declaration.source != 0u
        || entry->declaration.item != CM_AST_ITEM_NONE
        || !cm_hir_def_id_is_none(entry->target.definition)
        || entry->target.type_kind != CM_HIR_TYPE_ERROR_KIND
        || entry->target.value_kind != CM_HIR_LIBRARY_VALUE_NONE
        || (primitive = cm_decl_library_primitive(
            entry->target.primitive_kind)) == 0u)
        return 0;
    module = cm_decl_module_by_local((CmDeclCaptureState *)state,
        entry->owner_module);
    import = cm_decl_reexport_import(module, entry->introduced_by);
    if (module == NULL || import == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id, entry->export_ordinal,
            &effective) != CM_RESOLVE_VIEW_OK
        || effective.is_generated || effective.item_kind != CM_AST_ITEM_USE
        || !cm_decl_item_ref_equal(effective.declaration,
            entry->introduced_by)
        || effective.span.source != entry->introduction_span.source
        || effective.span.start != entry->introduction_span.start
        || effective.span.end != entry->introduction_span.end
        || !cm_module_graph_borrow_item_ast(state->input->graph,
            module->graph.id, entry->introduced_by, &ast)
        || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            entry->introduced_by.item)) == NULL
        || ast_item->kind != CM_AST_ITEM_USE
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->span.start != effective.span.start
        || ast_item->span.end != effective.span.end
        || ast_item->generic_parameters != NULL
        || ast_item->generic_parameter_count != 0u
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicates != NULL
        || ast_item->where_predicate_count != 0u
        || (ast_tree = cm_ast_get_string(ast,
            ast_item->data.use_item.tree)) == NULL
        || ast_tree->len == 0u
        || import->kind != CM_HIR_IMPORT_USE
        || import->visibility.kind != CM_HIR_VIS_PUBLIC
        || !cm_hir_def_id_is_none(import->visibility.restriction)
        || import->span.source != effective.span.source
        || import->span.start != effective.span.start
        || import->span.end != effective.span.end
        || import->source_item != entry->introduced_by.item
        || (hir_tree = cm_interner_get(&state->hir->strings,
            import->tree)) == NULL
        || !cm_decl_bytes_equal(hir_tree->bytes, hir_tree->len,
            ast_tree->bytes, ast_tree->len))
        return 0;
    declaration_count = cm_import_declaration_binding_count(
        state->input->imports, module->graph.id, entry->introduced_by);
    if (declaration_count == 0u || declaration_count > (size_t)UINT32_MAX
        || declaration_count != import->binding_count
        || import->bindings == NULL) return 0;
    leaf_count = cm_import_leaf_count(state->input->imports);
    if (leaf_count > (size_t)UINT32_MAX) return 0;
    for (index = 0u; index < leaf_count; ++index) {
        CmImportLeafView leaf;
        CmResolvedBinding resolver_binding;
        const CmHirImportBinding *hir_binding;
        const CmInternedString *hir_name;
        unsigned char *resolver_name = NULL;
        size_t resolver_name_length = 0u;
        unsigned char *leaf_name = NULL;
        size_t leaf_name_length = 0u;
        uint8_t resolver_primitive = 0u;
        uint32_t segment_index;
        int valid;
        if (!cm_import_get_leaf(state->input->imports, (uint32_t)index,
                &leaf)) return 0;
        if (leaf.module != module->graph.id
            || !cm_decl_item_ref_equal(leaf.declaration,
                entry->introduced_by)) continue;
        valid = matched_binding_count < declaration_count
            && leaf.revision == state->input->revision
            && leaf.segment_count != 0u && leaf.binding_count == 1u
            && !leaf.is_glob && !leaf.is_anonymous && leaf.is_public
            && leaf.is_crate_visible && leaf.is_resolved
            && !leaf.saw_ambiguous
            && cm_import_get_declaration_binding(state->input->imports,
                module->graph.id, entry->introduced_by,
                (uint32_t)matched_binding_count, &resolver_binding)
            && resolver_binding.revision == state->input->revision
            && resolver_binding.module == module->graph.id
            && resolver_binding.namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && (resolver_primitive = cm_decl_resolver_primitive(
                resolver_binding.primitive_kind)) != 0u
            && resolver_binding.declaration.source == 0u
            && resolver_binding.declaration.item == CM_AST_ITEM_NONE
            && resolver_binding.variant.enumeration.source == 0u
            && resolver_binding.variant.enumeration.item == CM_AST_ITEM_NONE
            && resolver_binding.variant.index == 0u
            && resolver_binding.target_module == CM_MODULE_NONE
            && cm_decl_item_ref_equal(resolver_binding.import_declaration,
                entry->introduced_by)
            && resolver_binding.is_public
            && resolver_binding.is_crate_visible
            && resolver_binding.is_import && resolver_binding.is_reexport
            && !resolver_binding.is_ambiguous
            && !resolver_binding.is_anonymous
            && cm_decl_copy_import_string(state->input->imports,
                resolver_binding.name, &resolver_name,
                &resolver_name_length)
            && cm_decl_copy_import_string(state->input->imports,
                leaf.import_name, &leaf_name, &leaf_name_length)
            && cm_decl_bytes_equal(resolver_name, resolver_name_length,
                leaf_name, leaf_name_length);
        hir_binding = valid ? &import->bindings[matched_binding_count] : NULL;
        hir_name = hir_binding == NULL ? NULL : cm_interner_get(
            &state->hir->strings, hir_binding->name);
        valid = valid && hir_binding->namespace_kind == CM_HIR_NAMESPACE_TYPE
            && cm_decl_library_primitive(hir_binding->primitive_kind)
                == resolver_primitive
            && cm_hir_def_id_is_none(hir_binding->target)
            && !hir_binding->is_anonymous && hir_binding->is_public
            && hir_binding->is_crate_visible && hir_name != NULL
            && cm_decl_bytes_equal(hir_name->bytes, hir_name->len,
                resolver_name, resolver_name_length);
        for (segment_index = 0u; valid
                && segment_index < leaf.segment_count; ++segment_index) {
            CmResolvePathSegmentView segment;
            valid = cm_import_get_leaf_segment(state->input->imports,
                    (uint32_t)index, segment_index, &segment)
                && segment.bytes != NULL && segment.length != 0u;
        }
        if (valid && resolver_primitive == primitive
            && cm_decl_bytes_equal(resolver_name, resolver_name_length,
                entry->name, entry->name_length)) matched_entry = 1;
        if (valid) matched_binding_count += 1u;
        cm_free(leaf_name);
        cm_free(resolver_name);
        if (!valid) return 0;
        matched_leaf_count += 1u;
    }
    return matched_leaf_count != 0u
        && matched_leaf_count == declaration_count
        && matched_binding_count == declaration_count && matched_entry;
}

/*
 * `doc(no_inline)` is admitted only for a source-authenticated public use
 * tree.  A declaration may be a group of named leaves or one glob leaf; a
 * mixed/multiple-glob tree is outside this bounded projection.  The resolver
 * snapshot is the authority for the parsed leaf shape and its complete
 * binding census.
 */
static int cm_decl_reexport_no_inline_provenance(
    const CmDeclCaptureState *state, const CmDeclCaptureModule *module,
    CmResolveItemRef declaration)
{
    size_t leaf_count = cm_import_leaf_count(state->input->imports);
    size_t declaration_count;
    size_t matched = 0u;
    size_t glob_count = 0u;
    size_t binding_total = 0u;
    size_t index;
    if (leaf_count > (size_t)UINT32_MAX) return 0;
    for (index = 0u; index < leaf_count; ++index) {
        CmImportLeafView leaf;
        if (!cm_import_get_leaf(state->input->imports, (uint32_t)index,
                &leaf)) return 0;
        if (leaf.module != module->graph.id
            || !cm_decl_item_ref_equal(leaf.declaration, declaration))
            continue;
        if (leaf.revision != state->input->revision || !leaf.is_public
            || !leaf.is_crate_visible || !leaf.is_resolved
            || leaf.saw_ambiguous || leaf.is_anonymous
            || leaf.binding_count == 0u
            || leaf.binding_count > SIZE_MAX - binding_total) return 0;
        binding_total += leaf.binding_count;
        matched += 1u;
        if (leaf.is_glob) glob_count += 1u;
    }
    declaration_count = cm_import_declaration_binding_count(
        state->input->imports, module->graph.id, declaration);
    if (matched == 0u || binding_total != declaration_count
        || (glob_count != 0u && (glob_count != 1u || matched != 1u)))
        return 0;
    for (index = 0u; index < declaration_count; ++index) {
        CmResolvedBinding binding;
        if (index > (size_t)UINT32_MAX
            || !cm_import_get_declaration_binding(state->input->imports,
                module->graph.id, declaration, (uint32_t)index, &binding)
            || binding.revision != state->input->revision
            || binding.module != module->graph.id
            || !cm_decl_item_ref_equal(binding.import_declaration,
                declaration)
            || !binding.is_import || !binding.is_public
            || !binding.is_reexport || binding.is_ambiguous
            || binding.is_anonymous) return 0;
    }
    return 1;
}

static int cm_decl_reexport_attributes(CmDeclCaptureState *state,
    CmHirDeclarationCaptureResult *result)
{
    size_t entry_index;
    for (entry_index = 0u; entry_index < state->namespace_count;
            ++entry_index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[entry_index];
        CmDeclCaptureModule *module;
        CmResolveEffectiveItem effective;
        const CmHirImport *import;
        unsigned int seen = 0u;
        uint32_t attribute_index;
        size_t prior;
        int first = 1;
        CmResolveViewStatus effective_status;
        if (!entry->is_import) continue;
        for (prior = 0u; prior < entry_index; ++prior) {
            if (state->namespace_values[prior].is_import
                && cm_decl_item_ref_equal(
                    state->namespace_values[prior].introduced_by,
                    entry->introduced_by)) {
                first = 0;
                break;
            }
        }
        module = cm_decl_module_by_local(state, entry->owner_module);
        import = cm_decl_reexport_import(module, entry->introduced_by);
        if (module == NULL || import == NULL) {
            CmSpan span = import == NULL
                ? (CmSpan){ 0u, 0u, 0u } : import->span;
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, span);
        }
        effective_status = cm_module_graph_get_effective_item(
            state->input->graph, state->input->revision, module->graph.id,
            entry->export_ordinal, &effective);
        if (effective_status != CM_RESOLVE_VIEW_OK) {
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, import->span);
        }
        if (effective.is_generated) {
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, effective.span);
        }
        if (effective.item_kind != CM_AST_ITEM_USE
            || !cm_decl_item_ref_equal(effective.declaration,
                entry->introduced_by)
            || effective.attribute_count != entry->source_attribute_count
            || effective.attribute_count != import->attribute_count
            || ((import->attribute_count == 0u)
                != (import->attributes == NULL))) {
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, import->span);
        }
        for (attribute_index = 0u;
                attribute_index < effective.attribute_count;
                ++attribute_index) {
            CmResolveEffectiveAttribute graph_attribute;
            const CmHirAttribute *hir_attribute =
                &import->attributes[attribute_index];
            const CmInternedString *metadata = cm_interner_get(
                &state->hir->strings, hir_attribute->metadata);
            unsigned int kind = cm_decl_attribute_kind(metadata);
            uint32_t duplicate_index;
            if (cm_module_graph_get_effective_item_attribute(
                    state->input->graph, state->input->revision,
                    module->graph.id, effective.id, attribute_index,
                    &graph_attribute) != CM_RESOLVE_VIEW_OK
                || !cm_decl_effective_attribute_matches_hir(state,
                    &graph_attribute, hir_attribute, entry->introduced_by)
                || hir_attribute->expansion_depth != 0u
                || (kind == CM_DECL_ATTR_STABLE
                    && (seen & CM_DECL_ATTR_UNSTABLE) != 0u)
                || (kind == CM_DECL_ATTR_UNSTABLE
                    && (seen & CM_DECL_ATTR_STABLE) != 0u)
                || (kind != CM_DECL_ATTR_STABLE
                    && kind != CM_DECL_ATTR_UNSTABLE
                    && kind != CM_DECL_ATTR_DEPRECATED
                    && kind != CM_DECL_ATTR_ALLOW
                    && kind != CM_DECL_ATTR_DOC_ALIAS
                    && kind != CM_DECL_ATTR_DOC_NO_INLINE
                    && kind != CM_DECL_ATTR_RUSTFMT_SKIP
                    && kind != CM_DECL_ATTR_DOC_INLINE)
                || (seen & kind) != 0u) {
                return cm_decl_capture_reexport_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                    entry, hir_attribute->span);
            }
            for (duplicate_index = 0u;
                    duplicate_index < attribute_index; ++duplicate_index) {
                if (import->attributes[duplicate_index].span.source
                        == hir_attribute->span.source
                    && import->attributes[duplicate_index].source_attribute
                        == hir_attribute->source_attribute) {
                    return cm_decl_capture_reexport_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, hir_attribute->span);
                }
            }
            seen |= kind;
        }
        if ((seen & CM_DECL_ATTR_DOC_NO_INLINE) != 0u
            && !cm_decl_reexport_no_inline_provenance(state, module,
                entry->introduced_by)) {
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, import->span);
        }
        if (first) {
            if ((size_t)effective.attribute_count > SIZE_MAX
                    - state->projected_semantic_attribute_count) {
                return cm_decl_capture_reexport_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT,
                    entry, import->span);
            }
            state->projected_semantic_attribute_count +=
                effective.attribute_count;
        }
    }
    return 1;
}

static int cm_decl_unit_struct_shape(const CmHirItem *item)
{
    return item->kind == CM_HIR_ITEM_STRUCT
        && cm_decl_plain_visibility(item)
        && cm_hir_def_id_is_none(item->parent_definition)
        && !item->is_specializable
        && item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        && item->generic_parameter_count == 0u
        && item->predicate_scopes == NULL
        && item->predicate_scope_count == 0u
        && item->predicates == NULL
        && item->predicate_count == 0u
        && item->outlives_predicates == NULL
        && item->outlives_predicate_count == 0u
        && item->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT
        && item->data.aggregate_item.fields == NULL
        && item->data.aggregate_item.field_count == 0u;
}

static int cm_decl_type_alias_shape(const CmDeclCaptureState *state,
    const CmHirItem *item)
{
    const CmHirType *target;
    const CmHirItem *target_item;
    CmHirItemId target_id;
    if (item->kind != CM_HIR_ITEM_TYPE_ALIAS
        || !cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count != 0u
        || item->predicate_scopes != NULL || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || !cm_hir_def_id_is_none(
            item->data.type_alias_item.trait_item_definition)
        || item->data.type_alias_item.bounds != NULL
        || item->data.type_alias_item.bound_count != 0u
        || (target = cm_hir_get_type(state->hir,
            item->data.type_alias_item.target)) == NULL
        || target->kind != CM_HIR_TYPE_ADT_KIND
        || target->data.named_type.argument_count != 0u
        || target->data.named_type.arguments != NULL
        || target->data.named_type.definition.crate_id
            != state->input->crate_id) return 0;
    target_item = cm_decl_bound_item(state->hir,
        target->data.named_type.definition, &target_id);
    return target_item != NULL && cm_decl_unit_struct_shape(target_item);
}

static int cm_decl_generics_shape(const CmDeclCaptureState *state,
    const CmHirItem *item)
{
    uint32_t index;
    if (item->generic_parameter_count == 0u)
        return item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE;
    if (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) return 0;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        CmHirGenericParamId id = item->generic_parameter_start + index;
        const CmHirGenericParam *generic;
        const CmInternedString *name;
        if (id < item->generic_parameter_start
            || (generic = cm_hir_get_generic_param(state->hir, id)) == NULL
            || generic->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(generic->owner, item->definition)
            || generic->index != index || generic->has_default
            || generic->declared_type != CM_HIR_TYPE_NONE
            || (name = cm_interner_get(&state->hir->strings,
                generic->name)) == NULL || name->len == 0u) return 0;
    }
    return 1;
}

static int cm_decl_trait_shape(const CmDeclCaptureState *state,
    const CmHirItem *item)
{
    size_t index;
    if (item->kind != CM_HIR_ITEM_TRAIT || !cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable || item->attribute_count != 0u
        || item->predicate_scope_count != 0u || item->predicate_count != 0u
        || item->outlives_predicate_count != 0u
        || item->data.trait_item.safety != CM_HIR_SAFE
        || item->data.trait_item.is_auto || item->data.trait_item.is_const
        || item->data.trait_item.supertrait_count != 0u
        || !cm_decl_generics_shape(state, item)) return 0;
    for (index = 0u; index < state->hir->items.len; ++index) {
        const CmHirItem *child = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, index);
        if (child != NULL && cm_hir_def_id_equal(child->parent_definition,
                item->definition)) return 0;
    }
    return 1;
}

static int cm_decl_string_is(const CmHirContext *hir, CmInternId id,
    const char *text)
{
    const CmInternedString *value = cm_interner_get(&hir->strings, id);
    size_t length = strlen(text);
    return value != NULL && value->len == length
        && memcmp(value->bytes, text, length) == 0;
}

static int cm_decl_function_shape(const CmDeclCaptureState *state,
    const CmHirItem *item)
{
    const CmHirFunctionSignature *signature;
    if (item->kind != CM_HIR_ITEM_FUNCTION
        || !cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable || item->attribute_count != 0u
        || item->predicate_scope_count != 0u
        || item->outlives_predicate_count != 0u
        || item->generic_parameter_count == 0u || item->predicate_count == 0u
        || item->data.function_item.has_default_body
        || !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || !cm_decl_generics_shape(state, item)) return 0;
    signature = &item->data.function_item.signature;
    return signature->receiver == CM_HIR_RECEIVER_NONE
        && signature->safety == CM_HIR_SAFE && !signature->is_const
        && !signature->is_async && !signature->is_variadic
        && cm_decl_string_is(state->hir, signature->abi, "Rust")
        && (signature->parameter_count == 0u
            ? signature->parameters == NULL : signature->parameters != NULL);
}

static int cm_decl_ast_path_is(const CmAst *ast, CmAstPathId id,
    const char *expected)
{
    const CmAstPath *path = cm_ast_get_path(ast, id);
    const CmInternedString *name;
    size_t length = strlen(expected);
    if (path == NULL || path->absolute || path->segment_count != 1u
        || path->segments == NULL || path->segments[0].argument_count != 0u
        || path->segments[0].arguments != NULL) return 0;
    name = cm_ast_get_string(ast, path->segments[0].name);
    return name != NULL && name->len == length
        && memcmp(name->bytes, expected, length) == 0;
}

static const char *cm_decl_primitive_name(uint8_t primitive)
{
    switch (primitive) {
    case CM_HIR_DECL_PRIMITIVE_BOOL: return "bool";
    case CM_HIR_DECL_PRIMITIVE_CHAR: return "char";
    case CM_HIR_DECL_PRIMITIVE_STR: return "str";
    case CM_HIR_DECL_PRIMITIVE_I8: return "i8";
    case CM_HIR_DECL_PRIMITIVE_I16: return "i16";
    case CM_HIR_DECL_PRIMITIVE_I32: return "i32";
    case CM_HIR_DECL_PRIMITIVE_I64: return "i64";
    case CM_HIR_DECL_PRIMITIVE_I128: return "i128";
    case CM_HIR_DECL_PRIMITIVE_ISIZE: return "isize";
    case CM_HIR_DECL_PRIMITIVE_U8: return "u8";
    case CM_HIR_DECL_PRIMITIVE_U16: return "u16";
    case CM_HIR_DECL_PRIMITIVE_U32: return "u32";
    case CM_HIR_DECL_PRIMITIVE_U64: return "u64";
    case CM_HIR_DECL_PRIMITIVE_U128: return "u128";
    case CM_HIR_DECL_PRIMITIVE_USIZE: return "usize";
    case CM_HIR_DECL_PRIMITIVE_F32: return "f32";
    case CM_HIR_DECL_PRIMITIVE_F64: return "f64";
    default: return NULL;
    }
}

static int cm_decl_ast_type_matches_hir_primitive(const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type)
{
    uint8_t primitive = cm_decl_primitive(hir_type);
    const char *name;
    if (primitive == CM_HIR_DECL_PRIMITIVE_UNIT) {
        return ast_type != NULL && ast_type->kind == CM_AST_TYPE_TUPLE
            && ast_type->tuple_provenance == CM_AST_TUPLE_SOURCE
            && ast_type->elements == NULL && ast_type->element_count == 0u;
    }
    name = cm_decl_primitive_name(primitive);
    return name != NULL && ast_type != NULL
        && ast_type->kind == CM_AST_TYPE_PATH
        && cm_decl_ast_path_is(ast, ast_type->path, name);
}

static int cm_decl_ast_type_matches_hir_static(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type, size_t depth)
{
    uint32_t index;
    if (depth > CM_META_MAX_TYPE_NESTING || ast_type == NULL
        || hir_type == NULL || ast_type->span.start != hir_type->span.start
        || ast_type->span.end != hir_type->span.end) return 0;
    if (cm_decl_primitive(hir_type) != 0u)
        return cm_decl_ast_type_matches_hir_primitive(ast, ast_type, hir_type);
    if (hir_type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        if (ast_type->kind != CM_AST_TYPE_TUPLE
            || ast_type->tuple_provenance != CM_AST_TUPLE_SOURCE
            || hir_type->data.tuple_type.element_count == 0u
            || ast_type->element_count
                != hir_type->data.tuple_type.element_count
            || ast_type->elements == NULL
            || hir_type->data.tuple_type.elements == NULL) return 0;
        for (index = 0u; index < ast_type->element_count; ++index) {
            const CmAstType *ast_child = cm_ast_get_type(ast,
                ast_type->elements[index]);
            const CmHirType *hir_child = cm_hir_get_type(state->hir,
                hir_type->data.tuple_type.elements[index]);
            if (!cm_decl_ast_type_matches_hir_static(state, ast, ast_child,
                    hir_child, depth + 1u)) return 0;
        }
        return 1;
    }
    if (hir_type->kind == CM_HIR_TYPE_ARRAY_KIND) {
        const CmAstType *ast_child;
        const CmHirType *hir_child;
        const CmHirType *length_type;
        const CmInternedString *length_text;
        uint64_t length;
        if (ast_type->kind != CM_AST_TYPE_ARRAY
            || ast_type->child == CM_AST_TYPE_NONE
            || hir_type->data.array_type.element == CM_HIR_TYPE_NONE
            || (ast_child = cm_ast_get_type(ast, ast_type->child)) == NULL
            || (hir_child = cm_hir_get_type(state->hir,
                hir_type->data.array_type.element)) == NULL
            || (length_type = cm_hir_get_type(state->hir,
                hir_type->data.array_type.length.type)) == NULL
            || cm_decl_primitive(length_type) != CM_HIR_DECL_PRIMITIVE_USIZE
            || hir_type->data.array_type.length.kind != CM_HIR_CONST_VALUE
            || hir_type->data.array_type.length.data.value.high_bits != 0u
            || (length_text = cm_ast_get_string(ast, ast_type->text)) == NULL
            || !cm_decl_parse_u64_decimal(length_text, &length)
            || length
                != hir_type->data.array_type.length.data.value.low_bits)
            return 0;
        return cm_decl_ast_type_matches_hir_static(state, ast, ast_child,
            hir_child, depth + 1u);
    }
    return 0;
}

static int cm_decl_ast_name_matches_hir(const CmAst *ast, CmInternId ast_id,
    const CmHirContext *hir, CmInternId hir_id)
{
    const CmInternedString *ast_name = cm_ast_get_string(ast, ast_id);
    const CmInternedString *hir_name = cm_interner_get(&hir->strings, hir_id);
    return ast_name != NULL && hir_name != NULL
        && cm_decl_bytes_equal(ast_name->bytes, ast_name->len,
            hir_name->bytes, hir_name->len);
}

static int cm_decl_ast_generic_shape(const CmDeclCaptureState *state,
    const CmAst *ast, const CmAstItem *ast_item, const CmHirItem *item)
{
    const CmAstGenericParam *ast_generic;
    const CmHirGenericParam *generic;
    const CmAstGenericParamBound *bound;
    const CmAstType *bound_type;
    if (ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicates != NULL
        || ast_item->where_predicate_count != 0u
        || ast_item->generic_parameter_count != item->generic_parameter_count
        || ((ast_item->generic_parameter_count == 0u)
            != (ast_item->generic_parameters == NULL))) return 0;
    if (item->generic_parameter_count == 0u)
        return item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE;
    if (item->generic_parameter_count != 1u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE)
        return 0;
    ast_generic = &ast_item->generic_parameters[0];
    generic = cm_hir_get_generic_param(state->hir,
        item->generic_parameter_start);
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
        || !cm_hir_def_id_equal(generic->owner, item->definition)
        || generic->index != 0u || generic->declared_type != CM_HIR_TYPE_NONE
        || generic->has_default
        || generic->span.source != item->span.source
        || generic->span.start != item->span.start
        || generic->span.end != item->span.end
        || ast_generic->kind != CM_AST_PARAM_TYPE
        || ast_generic->attributes != NULL
        || ast_generic->attribute_count != 0u
        || !cm_decl_ast_name_matches_hir(ast, ast_generic->name,
            state->hir, generic->name)
        || ast_generic->declared_type != CM_AST_TYPE_NONE
        || ast_generic->default_type != CM_AST_TYPE_NONE
        || ast_generic->default_const != CM_INTERN_ID_NONE
        || ast_generic->default_const_expr != CM_AST_EXPR_NONE) return 0;
    if (!generic->is_relaxed_sized)
        return ast_generic->constraint == CM_INTERN_ID_NONE
            && ast_generic->bounds == NULL && ast_generic->bound_count == 0u;
    if (ast_generic->constraint == CM_INTERN_ID_NONE
        || ast_generic->bounds == NULL || ast_generic->bound_count != 1u)
        return 0;
    bound = &ast_generic->bounds[0];
    bound_type = cm_ast_get_type(ast, bound->trait_type);
    return bound->kind == CM_AST_GENERIC_BOUND_TRAIT
        && bound->modifier == CM_AST_GENERIC_BOUND_RELAXED
        && bound->lifetime == CM_INTERN_ID_NONE
        && bound->span.start <= bound->span.end
        && bound->trait_type != CM_AST_TYPE_NONE
        && bound_type != NULL && bound_type->kind == CM_AST_TYPE_PATH
        && cm_decl_ast_path_is(ast, bound_type->path, "Sized");
}

static int cm_decl_ast_ordinary_enum_generics(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstItem *ast_item, const CmHirItem *item)
{
    uint32_t index;
    if (item->generic_parameter_count == 0u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicates != NULL
        || ast_item->where_predicate_count != 0u
        || ast_item->generic_parameter_count != item->generic_parameter_count
        || ast_item->generic_parameters == NULL
        || !cm_decl_generics_shape(state, item)) return 0;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmAstGenericParam *ast_generic =
            &ast_item->generic_parameters[index];
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, item->generic_parameter_start + index);
        if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(generic->owner, item->definition)
            || generic->index != index || generic->is_relaxed_sized
            || generic->declared_type != CM_HIR_TYPE_NONE
            || generic->has_default
            || generic->span.source != item->span.source
            || generic->span.start != item->span.start
            || generic->span.end != item->span.end
            || ast_generic->kind != CM_AST_PARAM_TYPE
            || ast_generic->attributes != NULL
            || ast_generic->attribute_count != 0u
            || !cm_decl_ast_name_matches_hir(ast, ast_generic->name,
                state->hir, generic->name)
            || ast_generic->constraint != CM_INTERN_ID_NONE
            || ast_generic->bounds != NULL || ast_generic->bound_count != 0u
            || ast_generic->declared_type != CM_AST_TYPE_NONE
            || ast_generic->default_type != CM_AST_TYPE_NONE
            || ast_generic->default_const != CM_INTERN_ID_NONE
            || ast_generic->default_const_expr != CM_AST_EXPR_NONE) return 0;
    }
    return 1;
}

static int cm_decl_field_visibility_matches(CmAstVisibility ast_visibility,
    CmHirVisibility hir_visibility)
{
    if (ast_visibility.kind == CM_AST_VIS_INHERITED)
        return ast_visibility.restriction == CM_AST_PATH_NONE
            && hir_visibility.kind == CM_HIR_VIS_PRIVATE
            && cm_hir_def_id_is_none(hir_visibility.restriction);
    if (ast_visibility.kind == CM_AST_VIS_PUBLIC)
        return ast_visibility.restriction == CM_AST_PATH_NONE
            && hir_visibility.kind == CM_HIR_VIS_PUBLIC
            && cm_hir_def_id_is_none(hir_visibility.restriction);
    return 0;
}

static int cm_decl_ast_type_matches_hir_field(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type,
    const CmHirItem *owner, size_t depth)
{
    const CmAstPath *path;
    const CmAstPathSegment *segment;
    const CmHirItem *target;
    CmHirItemId target_id;
    uint32_t index;
    if (depth > 16u || ast_type == NULL || hir_type == NULL
        || hir_type->span.source != owner->span.source
        || hir_type->span.start != ast_type->span.start
        || hir_type->span.end != ast_type->span.end) return 0;
    if (cm_decl_primitive(hir_type) != 0u)
        return cm_decl_ast_type_matches_hir_primitive(ast, ast_type, hir_type);
    if (hir_type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, hir_type->data.parameter_type.parameter);
        path = ast_type->kind == CM_AST_TYPE_PATH
            ? cm_ast_get_path(ast, ast_type->path) : NULL;
        return generic != NULL
            && cm_hir_def_id_equal(generic->owner, owner->definition)
            && path != NULL && !path->absolute && path->segment_count == 1u
            && path->segments != NULL
            && path->segments[0].argument_count == 0u
            && path->segments[0].arguments == NULL
            && cm_decl_ast_name_matches_hir(ast, path->segments[0].name,
                state->hir, generic->name);
    }
    if (hir_type->kind != CM_HIR_TYPE_ADT_KIND
        || ast_type->kind != CM_AST_TYPE_PATH
        || (hir_type->data.named_type.argument_count == 0u)
            != (hir_type->data.named_type.arguments == NULL)
        || (target = cm_decl_bound_item(state->hir,
            hir_type->data.named_type.definition, &target_id)) == NULL
        || target->definition.crate_id != state->input->crate_id
        || (target->kind != CM_HIR_ITEM_STRUCT
            && target->kind != CM_HIR_ITEM_UNION)
        || target->generic_parameter_count
            != hir_type->data.named_type.argument_count) return 0;
    path = cm_ast_get_path(ast, ast_type->path);
    if (path == NULL || path->segment_count == 0u || path->segments == NULL)
        return 0;
    for (index = 0u; index + 1u < path->segment_count; ++index) {
        if (path->segments[index].argument_count != 0u
            || path->segments[index].arguments != NULL) return 0;
    }
    segment = &path->segments[path->segment_count - 1u];
    if (!cm_decl_ast_name_matches_hir(ast, segment->name, state->hir,
            target->name)
        || segment->argument_count != hir_type->data.named_type.argument_count
        || ((segment->argument_count == 0u)
            != (segment->arguments == NULL))) return 0;
    for (index = 0u; index < segment->argument_count; ++index) {
        const CmAstGenericArg *ast_argument = &segment->arguments[index];
        const CmHirGenericArg *hir_argument =
            &hir_type->data.named_type.arguments[index];
        const CmAstType *ast_child;
        const CmHirType *hir_child;
        if (ast_argument->kind != CM_AST_GENERIC_TYPE
            || ast_argument->name != CM_INTERN_ID_NONE
            || ast_argument->name_arguments != NULL
            || ast_argument->name_argument_count != 0u
            || ast_argument->type == CM_AST_TYPE_NONE
            || ast_argument->bounds != NULL || ast_argument->bound_count != 0u
            || hir_argument->kind != CM_HIR_GENERIC_ARG_TYPE
            || (ast_child = cm_ast_get_type(ast, ast_argument->type)) == NULL
            || (hir_child = cm_hir_get_type(state->hir,
                hir_argument->data.type)) == NULL
            || !cm_decl_ast_type_matches_hir_field(state, ast, ast_child,
                hir_child, owner, depth + 1u)) return 0;
    }
    return 1;
}

static int cm_decl_aggregate_shape_and_source(CmDeclCaptureState *state,
    CmDeclCaptureItem *capture, size_t *out_projected_count)
{
    const CmHirItem *item = capture->item;
    const CmDeclCaptureModule *module;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    CmResolveEffectiveItem effective;
    CmAstItemKind ast_kind = item->kind == CM_HIR_ITEM_UNION
        ? CM_AST_ITEM_UNION : CM_AST_ITEM_STRUCT;
    uint32_t index;
    size_t projected_count;
    if ((item->kind != CM_HIR_ITEM_STRUCT && item->kind != CM_HIR_ITEM_UNION)
        || !cm_decl_plain_visibility(item)
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->predicate_scopes != NULL || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || item->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
        || item->data.aggregate_item.field_count == 0u
        || (size_t)item->data.aggregate_item.field_count
            > CM_HIR_DECL_METADATA_MAX_FIELDS
        || item->data.aggregate_item.fields == NULL
        || !cm_decl_generics_shape(state, item)
        || !cm_decl_named_aggregate_source(state, item, ast_kind,
            &capture->owner_module, &capture->source_ordinal)
        || !cm_decl_aggregate_attributes(state, item,
            &capture->aggregate_repr, &capture->aggregate_flags,
            &capture->lang_item, &capture->lang_item_length,
            &projected_count)
        || !cm_decl_item_attribute_provenance(state, item, ast_kind,
            CM_HIR_LIBRARY_BINDING_TYPE)) return 0;
    *out_projected_count = projected_count;
    module = cm_decl_module_by_local(state, capture->owner_module);
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id,
            capture->source_ordinal, &effective) != CM_RESOLVE_VIEW_OK
        || effective.is_generated || effective.item_kind != ast_kind
        || effective.visibility != CM_AST_VIS_PUBLIC
        || effective.span.source != item->span.source
        || effective.span.start != item->span.start
        || effective.span.end != item->span.end
        || !cm_module_graph_borrow_item_ast(state->input->graph,
            module->graph.id, effective.declaration, &ast)
        || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            effective.declaration.item)) == NULL
        || ast_item->kind != ast_kind
        || ast_item->span.start != item->span.start
        || ast_item->span.end != item->span.end
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->is_default
        || ast_item->data.aggregate_item.form != CM_AST_FIELDS_NAMED
        || ast_item->data.aggregate_item.field_count
            != item->data.aggregate_item.field_count
        || ast_item->data.aggregate_item.fields == NULL
        || !cm_decl_ast_generic_shape(state, ast, ast_item, item)) return 0;
    if (capture->aggregate_repr == CM_HIR_DECL_AGGREGATE_REPR_RUST) {
        if (item->kind != CM_HIR_ITEM_STRUCT
            || item->generic_parameter_count != 0u
            || item->data.aggregate_item.field_count != 4u) return 0;
    } else if (item->kind == CM_HIR_ITEM_STRUCT) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, item->generic_parameter_start);
        const CmHirType *field_type = cm_hir_get_type(state->hir,
            item->data.aggregate_item.fields[0].type);
        if (item->generic_parameter_count != 1u || generic == NULL
            || !generic->is_relaxed_sized
            || item->data.aggregate_item.field_count != 1u
            || item->data.aggregate_item.fields[0].visibility.kind
                != CM_HIR_VIS_PRIVATE
            || !cm_hir_def_id_is_none(item->data.aggregate_item.fields[0]
                .visibility.restriction)
            || field_type == NULL
            || field_type->kind != CM_HIR_TYPE_PARAMETER_KIND
            || field_type->data.parameter_type.parameter
                != item->generic_parameter_start) return 0;
    } else {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, item->generic_parameter_start);
        const CmHirType *unit_type = cm_hir_get_type(state->hir,
            item->data.aggregate_item.fields[0].type);
        const CmHirType *application = cm_hir_get_type(state->hir,
            item->data.aggregate_item.fields[1].type);
        const CmHirItem *target;
        CmHirItemId target_id;
        uint8_t target_repr = 0u;
        uint16_t target_flags = 0u;
        const unsigned char *target_lang = NULL;
        size_t target_lang_length = 0u;
        size_t ignored_projection = 0u;
        if (item->generic_parameter_count != 1u || generic == NULL
            || generic->is_relaxed_sized
            || item->data.aggregate_item.field_count != 2u
            || item->data.aggregate_item.fields[0].visibility.kind
                != CM_HIR_VIS_PRIVATE
            || item->data.aggregate_item.fields[1].visibility.kind
                != CM_HIR_VIS_PRIVATE
            || !cm_hir_def_id_is_none(item->data.aggregate_item.fields[0]
                .visibility.restriction)
            || !cm_hir_def_id_is_none(item->data.aggregate_item.fields[1]
                .visibility.restriction)
            || cm_decl_primitive(unit_type) != CM_HIR_DECL_PRIMITIVE_UNIT
            || application == NULL || application->kind != CM_HIR_TYPE_ADT_KIND
            || application->data.named_type.argument_count != 1u
            || application->data.named_type.arguments == NULL
            || application->data.named_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
            || cm_hir_get_type(state->hir,
                application->data.named_type.arguments[0].data.type) == NULL
            || cm_hir_get_type(state->hir,
                application->data.named_type.arguments[0].data.type)->kind
                != CM_HIR_TYPE_PARAMETER_KIND
            || cm_hir_get_type(state->hir,
                application->data.named_type.arguments[0].data.type)->data
                    .parameter_type.parameter != item->generic_parameter_start
            || (target = cm_decl_bound_item(state->hir,
                application->data.named_type.definition, &target_id)) == NULL
            || target->kind != CM_HIR_ITEM_STRUCT
            || target->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
            || target->generic_parameter_count != 1u
            || !cm_decl_aggregate_attributes(state, target, &target_repr,
                &target_flags, &target_lang, &target_lang_length,
                &ignored_projection)
            || target_repr != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
            || (target_flags & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) == 0u
            || !cm_decl_bytes_equal(target_lang, target_lang_length,
                (const unsigned char *)"manually_drop", 13u)) return 0;
    }
    for (index = 0u; index < item->data.aggregate_item.field_count; ++index) {
        const CmHirField *field = &item->data.aggregate_item.fields[index];
        const CmAstField *ast_field =
            &ast_item->data.aggregate_item.fields[index];
        const CmHirType *hir_type = cm_hir_get_type(state->hir, field->type);
        const CmAstType *ast_type = cm_ast_get_type(ast, ast_field->type);
        if (!cm_decl_ast_name_matches_hir(ast, ast_field->name,
                state->hir, field->name)
            || !cm_decl_field_visibility_matches(ast_field->visibility,
                field->visibility)
            || field->span.source != item->span.source
            || field->span.start != item->span.start
            || field->span.end != item->span.end
            || !cm_decl_ast_type_matches_hir_field(state, ast, ast_type,
                hir_type, item, 0u)) return 0;
        if (capture->aggregate_repr == CM_HIR_DECL_AGGREGATE_REPR_RUST
            && (field->visibility.kind != CM_HIR_VIS_PUBLIC
                || cm_decl_primitive(hir_type)
                    != CM_HIR_DECL_PRIMITIVE_BOOL)) return 0;
    }
    return 1;
}

static int cm_decl_free_value_shape(const CmDeclCaptureState *state,
    const CmHirItem *item, CmHirItemKind hir_kind,
    CmAstItemKind ast_kind, CmHirLibraryValueKind library_kind,
    uint32_t owner_module, uint32_t source_ordinal)
{
    const CmDeclCaptureModule *module;
    const CmHirLibraryOwnedValue *owned;
    const CmHirType *type;
    const CmHirBody *body;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    const CmAstType *ast_type;
    const CmAstExpr *initializer;
    CmResolveEffectiveItem effective;
    if (item->kind != hir_kind
        || !cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count != 0u
        || item->predicate_scopes != NULL
        || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || (hir_kind == CM_HIR_ITEM_CONST
            && item->data.value_item.mutability != CM_HIR_IMMUTABLE)
        || item->data.value_item.has_default_body != 0
        || !cm_hir_def_id_is_none(
            item->data.value_item.trait_item_definition)
        || item->data.value_item.body == CM_HIR_BODY_NONE
        || (type = cm_hir_get_type(state->hir,
            item->data.value_item.type)) == NULL
        || (hir_kind == CM_HIR_ITEM_CONST && cm_decl_primitive(type) == 0u)
        || type->span.source != item->span.source
        || type->span.start > type->span.end
        || type->span.start < item->span.start
        || type->span.end > item->span.end) return 0;
    owned = cm_decl_owned_value(state->owned, item->definition);
    if (owned == NULL
        || owned->storage_kind != library_kind
        || owned->declaration.kind != library_kind
        || !cm_hir_def_id_equal(owned->declaration.definition,
            item->definition)
        || owned->declaration.data.value.type != item->data.value_item.type
        || owned->declaration.data.value.mutability
            != item->data.value_item.mutability
        || owned->parameter_types != NULL || owned->parameter_count != 0u
        || owned->predicate_scopes != NULL
        || owned->predicate_scope_lifetimes != NULL
        || owned->predicate_scope_count != 0u
        || owned->predicates != NULL
        || owned->predicate_arguments != NULL
        || owned->predicate_equalities != NULL
        || owned->predicate_lifetimes != NULL
        || owned->predicate_count != 0u
        || owned->outlives_predicates != NULL
        || owned->outlives_predicate_count != 0u
        || owned->nominal_references != NULL
        || owned->nominal_reference_names != NULL
        || owned->nominal_reference_generic_kinds != NULL
        || owned->nominal_reference_count != 0u
        || owned->associated_availability != NULL
        || owned->associated_availability_count != 0u) return 0;
    module = cm_decl_module_by_local((CmDeclCaptureState *)state,
        owner_module);
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id, source_ordinal,
            &effective) != CM_RESOLVE_VIEW_OK
        || effective.is_generated || effective.item_kind != ast_kind
        || !cm_hir_def_id_equal(item->definition,
            owned->declaration.definition)
        || !cm_module_graph_borrow_ast(state->input->graph,
            module->graph.id, &ast) || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            effective.declaration.item)) == NULL
        || ast_item->kind != ast_kind
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->is_default
        || ast_item->generic_parameters != NULL
        || ast_item->generic_parameter_count != 0u
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicates != NULL
        || ast_item->where_predicate_count != 0u
        || ast_item->data.value_item.type == CM_AST_TYPE_NONE
        || !ast_item->data.value_item.has_value
        || ast_item->data.value_item.initializer == CM_AST_EXPR_NONE
        || ast_item->data.value_item.is_mutable
            != (item->data.value_item.mutability == CM_HIR_MUTABLE)
        || ast_item->data.value_item.bounds != NULL
        || ast_item->data.value_item.bound_count != 0u
        || ast_item->data.value_item.post_value_where_clause
            != CM_INTERN_ID_NONE
        || ast_item->data.value_item.post_value_where_predicates != NULL
        || ast_item->data.value_item.post_value_where_predicate_count != 0u
        || ast_item->span.start != item->span.start
        || ast_item->span.end != item->span.end
        || (ast_type = cm_ast_get_type(ast,
            ast_item->data.value_item.type)) == NULL
        || (hir_kind == CM_HIR_ITEM_CONST
            ? !cm_decl_ast_type_matches_hir_primitive(ast, ast_type, type)
            : !cm_decl_ast_type_matches_hir_static(state, ast, ast_type, type,
                0u))
        || ast_type->span.start != type->span.start
        || ast_type->span.end != type->span.end
        || (initializer = cm_ast_get_expr(ast,
            ast_item->data.value_item.initializer)) == NULL
        || ((initializer->attribute_count == 0u)
            != (initializer->attributes == NULL))
        || initializer->attribute_count != 0u
        || initializer->span.start > initializer->span.end
        || initializer->span.start < ast_item->span.start
        || initializer->span.end > ast_item->span.end) return 0;
    body = cm_hir_get_body(state->hir, item->data.value_item.body);
    return body != NULL
        && cm_hir_def_id_equal(body->owner, item->definition)
        && body->origin.kind == CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        && cm_hir_def_id_equal(body->origin.definition, item->definition)
        && cm_hir_def_id_equal(body->origin.enclosing_definition,
            item->definition)
        && cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition, item->definition)
        && body->state == CM_HIR_BODY_UNLOWERED
        && body->expected_type == item->data.value_item.type
        && body->locals == NULL && body->local_count == 0u
        && body->parameter_count == 0u
        && body->source == effective.declaration.source
        && body->source == item->span.source
        && body->source_expression_id
            == ast_item->data.value_item.initializer
        && body->root_expression == CM_HIR_EXPR_NONE
        && body->error_reason == CM_INTERN_ID_NONE
        && body->span.source == item->span.source
        && body->span.start == item->span.start
        && body->span.end == item->span.end;
}

static int cm_decl_const_shape(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t owner_module, uint32_t source_ordinal)
{
    return cm_decl_free_value_shape(state, item, CM_HIR_ITEM_CONST,
        CM_AST_ITEM_CONST, CM_HIR_LIBRARY_VALUE_CONST, owner_module,
        source_ordinal);
}

static int cm_decl_static_shape(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t owner_module, uint32_t source_ordinal)
{
    return cm_decl_free_value_shape(state, item, CM_HIR_ITEM_STATIC,
        CM_AST_ITEM_STATIC, CM_HIR_LIBRARY_VALUE_STATIC, owner_module,
        source_ordinal);
}

static int cm_decl_collect_items(CmDeclCaptureState *state,
    CmHirDeclarationCaptureResult *result)
{
    size_t index;
    if (!cm_decl_reexport_attributes(state, result)) return 0;
    state->traits = (CmDeclCaptureItem *)cm_alloc_zeroed(
        state->namespace_count, sizeof(*state->traits));
    state->items = (CmDeclCaptureItem *)cm_alloc_zeroed(
        state->namespace_count, sizeof(*state->items));
    state->values = (CmDeclCaptureItem *)cm_alloc_zeroed(
        state->namespace_count, sizeof(*state->values));
    for (index = 0u; index < state->namespace_count; ++index) {
        CmDeclCaptureNamespace *entry = &state->namespace_values[index];
        CmDeclCaptureItem value;
        memset(&value, 0, sizeof(value));
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_MODULE) {
            if (cm_decl_module_by_definition(state,
                    entry->target.definition) == NULL) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                    entry, NULL, CM_HIR_ITEM_NONE);
                return 0;
            }
            continue;
        }
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE)
            continue;
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT) {
            CmHirItemId enum_item_id = CM_HIR_ITEM_NONE;
            if (cm_decl_enum_variant_parent(state, &entry->target,
                    &enum_item_id) == NULL) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_DEFINITION_UNBOUND,
                    entry, NULL, enum_item_id);
                return 0;
            }
            continue;
        }
        value.item = cm_decl_bound_item(state->hir,
            entry->target.definition, &value.id);
        if (value.item == NULL
            || value.item->definition.crate_id != state->input->crate_id) {
            cm_decl_capture_item_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_ITEM_DEFINITION_UNBOUND,
                entry, value.item, value.id);
            return 0;
        }
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_TRAIT) {
            if (cm_decl_item_already(state->traits, state->trait_count,
                    value.item->definition)) continue;
            if (!cm_decl_item_source(state, value.item->definition,
                    CM_AST_ITEM_TRAIT, &value.owner_module,
                    &value.source_ordinal)) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                    entry, value.item, value.id);
                return 0;
            }
            if (!cm_decl_trait_shape(state, value.item)) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            state->traits[state->trait_count++] = value;
        } else if (entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE
            || entry->target.kind
                == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR) {
            size_t projected_count;
            int non_exhaustive;
            if (cm_decl_item_already(state->items, state->item_count,
                    value.item->definition)) continue;
            if (state->item_count == CM_HIR_DECL_METADATA_MAX_ITEMS) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            if (value.item->kind == CM_HIR_ITEM_STRUCT) {
                if (value.item->data.aggregate_item.form
                        == CM_HIR_AGGREGATE_UNIT) {
                    if (!cm_decl_unit_struct_shape(value.item)) {
                        cm_decl_capture_item_failure(result,
                            CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                            entry, value.item, value.id);
                        return 0;
                    }
                    if (!cm_decl_project_item_attributes(state, value.item,
                            CM_DECL_ATTR_STABLE | CM_DECL_ATTR_UNSTABLE
                                | CM_DECL_ATTR_DEPRECATED | CM_DECL_ATTR_DERIVE
                                | CM_DECL_ATTR_NON_EXHAUSTIVE,
                            &projected_count, &non_exhaustive)
                        || !cm_decl_item_attribute_provenance(state,
                            value.item, CM_AST_ITEM_STRUCT,
                            CM_HIR_LIBRARY_BINDING_TYPE)) {
                        cm_decl_capture_item_failure(result,
                            CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                            entry, value.item, value.id);
                        return 0;
                    }
                    if (!cm_decl_struct_source(state, value.item,
                            non_exhaustive, &value.owner_module,
                            &value.source_ordinal)) {
                        cm_decl_capture_item_failure(result,
                            CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                            entry, value.item, value.id);
                        return 0;
                    }
                } else if (!cm_decl_aggregate_shape_and_source(state, &value,
                        &projected_count)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
            } else if (value.item->kind == CM_HIR_ITEM_UNION
                && entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
                if (!cm_decl_aggregate_shape_and_source(state, &value,
                        &projected_count)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
            } else if (value.item->kind == CM_HIR_ITEM_TYPE_ALIAS
                && entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
                if (!cm_decl_type_alias_shape(state, value.item)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_project_item_attributes(state, value.item,
                        CM_DECL_ATTR_STABLE | CM_DECL_ATTR_UNSTABLE
                            | CM_DECL_ATTR_DEPRECATED,
                        &projected_count, &non_exhaustive)
                    || non_exhaustive
                    || !cm_decl_item_attribute_provenance(state, value.item,
                        CM_AST_ITEM_TYPE_ALIAS,
                        CM_HIR_LIBRARY_BINDING_TYPE)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_alias_source(state, value.item,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
            } else if (value.item->kind == CM_HIR_ITEM_ENUM
                && entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
                int enum_profile;
                const unsigned char *enum_lang = NULL;
                size_t enum_lang_length = 0u;
                if (!cm_decl_enum_source(state, value.item,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_enum_item_attributes(state, value.item,
                        &projected_count, &enum_profile, &enum_lang,
                        &enum_lang_length)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_enum_shape_and_variants(state, value.item,
                        value.id, value.owner_module, value.source_ordinal,
                        enum_profile, &projected_count)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                value.lang_item = enum_lang;
                value.lang_item_length = enum_lang_length;
            } else {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            if (projected_count > SIZE_MAX
                    - state->projected_semantic_attribute_count)
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
                    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
            state->projected_semantic_attribute_count += projected_count;
            state->items[state->item_count++] = value;
        } else if (entry->target.kind == CM_HIR_LIBRARY_BINDING_VALUE) {
            size_t projected_count = 0u;
            int non_exhaustive = 0;
            if (cm_decl_item_already(state->values, state->value_count,
                    value.item->definition)) continue;
            if (state->value_count == CM_HIR_DECL_METADATA_MAX_VALUES) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            if (value.item->kind == CM_HIR_ITEM_FUNCTION) {
                if (!cm_decl_item_source(state, value.item->definition,
                        CM_AST_ITEM_FUNCTION, &value.owner_module,
                        &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_function_shape(state, value.item)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
            } else if (value.item->kind == CM_HIR_ITEM_CONST) {
                if (!cm_decl_const_source(state, value.item,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_project_item_attributes(state, value.item,
                        CM_DECL_ATTR_STABLE | CM_DECL_ATTR_UNSTABLE
                            | CM_DECL_ATTR_DEPRECATED,
                        &projected_count, &non_exhaustive)
                    || non_exhaustive
                    || !cm_decl_item_attribute_provenance(state, value.item,
                        CM_AST_ITEM_CONST,
                        CM_HIR_LIBRARY_BINDING_VALUE)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_const_shape(state, value.item,
                        value.owner_module, value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (projected_count > SIZE_MAX
                        - state->projected_semantic_attribute_count)
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
                        CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
                state->projected_semantic_attribute_count += projected_count;
            } else if (value.item->kind == CM_HIR_ITEM_STATIC) {
                if (!cm_decl_static_source(state, value.item,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_static_attributes(state, value.item,
                        &projected_count)
                    || !cm_decl_item_attribute_provenance(state, value.item,
                        CM_AST_ITEM_STATIC,
                        CM_HIR_LIBRARY_BINDING_VALUE)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_static_shape(state, value.item,
                        value.owner_module, value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (projected_count > SIZE_MAX
                        - state->projected_semantic_attribute_count)
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
                        CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
                state->projected_semantic_attribute_count += projected_count;
            } else {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            state->values[state->value_count++] = value;
        } else {
            cm_decl_capture_item_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
                entry, value.item, value.id);
            return 0;
        }
    }
    cm_decl_sort_items(state->traits, state->trait_count, state);
    cm_decl_sort_items(state->items, state->item_count, state);
    cm_decl_sort_items(state->values, state->value_count, state);
    for (index = 0u; index < state->trait_count; ++index)
        state->traits[index].local = (uint32_t)(index + 1u);
    for (index = 0u; index < state->value_count; ++index)
        state->values[index].local = (uint32_t)(index + 1u);
    for (index = 0u; index < state->item_count; ++index)
        state->items[index].local = (uint32_t)(index + 1u);
    if (state->trait_count == 0u || state->value_count == 0u)
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
            CM_HIR_DECL_CAPTURE_REASON_REQUIRED_ITEMS_MISSING);
    return 1;
}

static uint32_t cm_decl_trait_local(const CmDeclCaptureState *state,
    CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < state->trait_count; ++index)
        if (cm_hir_def_id_equal(state->traits[index].item->definition,
                definition)) return state->traits[index].local;
    return 0u;
}

static uint32_t cm_decl_value_local(const CmDeclCaptureState *state,
    CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < state->value_count; ++index)
        if (cm_hir_def_id_equal(state->values[index].item->definition,
                definition)) return state->values[index].local;
    return 0u;
}

static uint32_t cm_decl_item_local(const CmDeclCaptureState *state,
    CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < state->item_count; ++index)
        if (cm_hir_def_id_equal(state->items[index].item->definition,
                definition)) return state->items[index].local;
    return 0u;
}

static uint32_t cm_decl_enum_variant_local(
    const CmDeclCaptureState *state, const CmHirLibraryBinding *target)
{
    size_t item_index;
    uint32_t local = 0u;
    if (target == NULL
        || target->kind != CM_HIR_LIBRARY_BINDING_ENUM_VARIANT) return 0u;
    for (item_index = 0u; item_index < state->item_count; ++item_index) {
        const CmHirItem *item = state->items[item_index].item;
        uint32_t variant_index;
        if (item->kind != CM_HIR_ITEM_ENUM) continue;
        for (variant_index = 0u;
                variant_index < item->data.enum_item.variant_count;
                ++variant_index) {
            const CmHirVariant *variant =
                &item->data.enum_item.variants[variant_index];
            if (local == UINT32_MAX) return 0u;
            local += 1u;
            if (cm_hir_def_id_equal(item->definition,
                    target->enum_definition)
                && variant_index == target->enum_variant_index
                && cm_hir_def_id_equal(variant->definition,
                    target->definition)
                && variant->form == target->enum_variant_form)
                return local;
        }
    }
    return 0u;
}

static uint8_t cm_decl_primitive(const CmHirType *type)
{
    if (type == NULL) return 0u;
    switch (type->kind) {
    case CM_HIR_TYPE_UNIT_KIND: return CM_HIR_DECL_PRIMITIVE_UNIT;
    case CM_HIR_TYPE_BOOL_KIND: return CM_HIR_DECL_PRIMITIVE_BOOL;
    case CM_HIR_TYPE_CHAR_KIND: return CM_HIR_DECL_PRIMITIVE_CHAR;
    case CM_HIR_TYPE_STR_KIND: return CM_HIR_DECL_PRIMITIVE_STR;
    case CM_HIR_TYPE_INTEGER_KIND:
        return (uint8_t)(CM_HIR_DECL_PRIMITIVE_I8
            + (unsigned int)type->data.integer_type.kind);
    case CM_HIR_TYPE_FLOAT_KIND:
        if (type->data.float_type.kind == CM_HIR_FLOAT_F32)
            return CM_HIR_DECL_PRIMITIVE_F32;
        if (type->data.float_type.kind == CM_HIR_FLOAT_F64)
            return CM_HIR_DECL_PRIMITIVE_F64;
        return 0u;
    default: return 0u;
    }
}

static int cm_decl_mark_type_depth(CmDeclCaptureState *state,
    CmHirTypeId type_id, CmHirDeclarationCaptureResult *result, size_t depth)
{
    const CmHirType *type = cm_hir_get_type(state->hir, type_id);
    uint8_t primitive = cm_decl_primitive(type);
    uint32_t child;
    uint32_t item_local;
    if (depth > CM_META_MAX_TYPE_NESTING) type = NULL;
    if (primitive != 0u) {
        state->primitive_types[primitive] = 1u;
        return 1;
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        CmHirGenericParamId parameter = type->data.parameter_type.parameter;
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, parameter);
        if (generic != NULL && generic->kind == CM_HIR_GENERIC_TYPE
            && parameter != CM_HIR_GENERIC_PARAM_NONE
            && (size_t)parameter <= state->hir->generic_parameters.len
            && state->generic_locals[parameter - 1u] != 0u) {
            state->generic_types[state->generic_locals[parameter - 1u] - 1u]
                = 1u;
            return 1;
        }
    }
    if (type != NULL && type_id != CM_HIR_TYPE_NONE
        && (size_t)type_id <= state->hir->types.len
        && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count != 0u
        && type->data.tuple_type.elements != NULL) {
        for (child = 0u; child < type->data.tuple_type.element_count; ++child)
            if (!cm_decl_mark_type_depth(state,
                    type->data.tuple_type.elements[child], result,
                    depth + 1u)) return 0;
        state->compound_types[type_id - 1u] = 1u;
        return 1;
    }
    if (type != NULL && type_id != CM_HIR_TYPE_NONE
        && (size_t)type_id <= state->hir->types.len
        && type->kind == CM_HIR_TYPE_ARRAY_KIND
        && type->data.array_type.element != CM_HIR_TYPE_NONE
        && type->data.array_type.length.kind == CM_HIR_CONST_VALUE
        && type->data.array_type.length.data.value.high_bits == 0u
        && cm_decl_primitive(cm_hir_get_type(state->hir,
            type->data.array_type.length.type))
            == CM_HIR_DECL_PRIMITIVE_USIZE
        && cm_decl_mark_type_depth(state, type->data.array_type.element,
            result, depth + 1u)
        && cm_decl_mark_type_depth(state,
            type->data.array_type.length.type, result, depth + 1u)) {
        state->compound_types[type_id - 1u] = 1u;
        return 1;
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count != 0u
        && type->data.named_type.arguments != NULL
        && type_id != CM_HIR_TYPE_NONE
        && (size_t)type_id <= state->hir->types.len
        && (item_local = cm_decl_item_local(state,
            type->data.named_type.definition)) != 0u
        && (state->items[item_local - 1u].item->kind == CM_HIR_ITEM_STRUCT
            || state->items[item_local - 1u].item->kind == CM_HIR_ITEM_UNION
            || state->items[item_local - 1u].item->kind == CM_HIR_ITEM_ENUM)
        && state->items[item_local - 1u].item->generic_parameter_count
            == type->data.named_type.argument_count) {
        for (child = 0u; child < type->data.named_type.argument_count;
                ++child) {
            if (type->data.named_type.arguments[child].kind
                    != CM_HIR_GENERIC_ARG_TYPE
                || !cm_decl_mark_type_depth(state,
                    type->data.named_type.arguments[child].data.type,
                    result, depth + 1u)) return 0;
        }
        state->application_types[type_id - 1u] = 1u;
        return 1;
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL
        && (item_local = cm_decl_item_local(state,
            type->data.named_type.definition)) != 0u
        && state->items[item_local - 1u].item->generic_parameter_count == 0u
        && (state->items[item_local - 1u].item->kind == CM_HIR_ITEM_STRUCT
            || state->items[item_local - 1u].item->kind == CM_HIR_ITEM_UNION
            || state->items[item_local - 1u].item->kind == CM_HIR_ITEM_ENUM)) {
        state->named_item_types[item_local - 1u] = 1u;
        return 1;
    }
    if (result->failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE) {
        result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA;
        result->failure_reason = CM_HIR_DECL_CAPTURE_REASON_TYPE_UNSUPPORTED;
        result->rejected_type = type_id;
        if (type != NULL) {
            result->has_rejected_span = 1;
            result->rejected_span = type->span;
        }
    }
    return 0;
}

static int cm_decl_mark_type(CmDeclCaptureState *state, CmHirTypeId type_id,
    CmHirDeclarationCaptureResult *result)
{
    return cm_decl_mark_type_depth(state, type_id, result, 0u);
}

static int cm_decl_mark_named_adt(CmDeclCaptureState *state,
    CmHirTypeId type_id, CmHirDeclarationCaptureResult *result)
{
    const CmHirType *type = cm_hir_get_type(state->hir, type_id);
    uint32_t item_local;
    if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL
        && (item_local = cm_decl_item_local(state,
            type->data.named_type.definition)) != 0u
        && state->items[item_local - 1u].item->kind == CM_HIR_ITEM_STRUCT) {
        state->named_item_types[item_local - 1u] = 1u;
        return 1;
    }
    if (result->failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE) {
        result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA;
        result->failure_reason = CM_HIR_DECL_CAPTURE_REASON_TYPE_UNSUPPORTED;
        result->rejected_type = type_id;
        if (type != NULL) {
            result->has_rejected_span = 1;
            result->rejected_span = type->span;
        }
    }
    return 0;
}

static uint32_t cm_decl_type_local(const CmDeclCaptureState *state,
    const CmHirDeclarationMetadata *metadata, CmHirTypeId type_id)
{
    const CmHirType *type = cm_hir_get_type(state->hir, type_id);
    uint8_t primitive = cm_decl_primitive(type);
    size_t index;
    if (primitive != 0u) {
        for (index = 0u; index < metadata->type_count; ++index)
            if (metadata->types[index].kind == CM_HIR_DECL_TYPE_PRIMITIVE
                && metadata->types[index].primitive == primitive)
                return (uint32_t)(index + 1u);
        return 0u;
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter != CM_HIR_GENERIC_PARAM_NONE) {
        uint32_t generic = state->generic_locals[
            type->data.parameter_type.parameter - 1u];
        for (index = 0u; index < metadata->type_count; ++index)
            if (metadata->types[index].kind == CM_HIR_DECL_TYPE_GENERIC
                && metadata->types[index].generic_local == generic)
                return (uint32_t)(index + 1u);
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL) {
        uint32_t item_local = cm_decl_item_local(state,
            type->data.named_type.definition);
        for (index = 0u; index < metadata->type_count; ++index)
            if (metadata->types[index].kind == CM_HIR_DECL_TYPE_NAMED_ADT
                && metadata->types[index].item_local == item_local)
                return (uint32_t)(index + 1u);
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count != 0u
        && type->data.named_type.arguments != NULL) {
        uint32_t item_local = cm_decl_item_local(state,
            type->data.named_type.definition);
        for (index = 0u; index < metadata->type_count; ++index) {
            const CmHirDeclarationType *wire = &metadata->types[index];
            uint32_t child;
            if (wire->kind != CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION
                || wire->item_local != item_local
                || wire->argument_count
                    != type->data.named_type.argument_count) continue;
            for (child = 0u; child < wire->argument_count; ++child) {
                if (type->data.named_type.arguments[child].kind
                        != CM_HIR_GENERIC_ARG_TYPE
                    || wire->argument_types[child] != cm_decl_type_local(
                        state, metadata,
                        type->data.named_type.arguments[child].data.type))
                    break;
            }
            if (child == wire->argument_count) return (uint32_t)(index + 1u);
        }
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count != 0u
        && type->data.tuple_type.elements != NULL) {
        for (index = 0u; index < metadata->type_count; ++index) {
            const CmHirDeclarationType *wire = &metadata->types[index];
            uint32_t child;
            if (wire->kind != CM_HIR_DECL_TYPE_TUPLE
                || wire->element_count
                    != type->data.tuple_type.element_count
                || wire->element_types == NULL) continue;
            for (child = 0u; child < wire->element_count; ++child)
                if (wire->element_types[child] != cm_decl_type_local(state,
                        metadata, type->data.tuple_type.elements[child])) break;
            if (child == wire->element_count) return (uint32_t)(index + 1u);
        }
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_ARRAY_KIND
        && type->data.array_type.length.kind == CM_HIR_CONST_VALUE) {
        uint32_t child_local = cm_decl_type_local(state, metadata,
            type->data.array_type.element);
        uint32_t length_local = cm_decl_type_local(state, metadata,
            type->data.array_type.length.type);
        for (index = 0u; index < metadata->type_count; ++index) {
            const CmHirDeclarationType *wire = &metadata->types[index];
            if (wire->kind == CM_HIR_DECL_TYPE_ARRAY
                && wire->child_type == child_local
                && wire->array_length_type == length_local
                && wire->array_length_low_bits
                    == type->data.array_type.length.data.value.low_bits
                && wire->array_length_high_bits
                    == type->data.array_type.length.data.value.high_bits)
                return (uint32_t)(index + 1u);
        }
    }
    return 0u;
}

static int cm_decl_fill_identity(const CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata)
{
    const CmInternedString *crate_name = cm_interner_get(&state->hir->strings,
        state->crate_value->name);
    const CmHirArtifactConfig *config = state->input->configuration;
    size_t index;
    uint8_t edition;
    switch (state->crate_value->edition) {
    case CM_HIR_EDITION_2015: edition = CM_HIR_DECL_EDITION_2015; break;
    case CM_HIR_EDITION_2018: edition = CM_HIR_DECL_EDITION_2018; break;
    case CM_HIR_EDITION_2021: edition = CM_HIR_DECL_EDITION_2021; break;
    case CM_HIR_EDITION_2024: edition = CM_HIR_DECL_EDITION_2024; break;
    default: return 0;
    }
    if (config->edition != (uint32_t)(edition == CM_HIR_DECL_EDITION_2015
            ? 2015u : edition == CM_HIR_DECL_EDITION_2018 ? 2018u
            : edition == CM_HIR_DECL_EDITION_2021 ? 2021u : 2024u)
        || !cm_decl_copy_intern(&metadata->crate_name, crate_name)
        || !cm_decl_copy_bytes(&metadata->crate_disambiguator,
            state->input->crate_disambiguator.data,
            state->input->crate_disambiguator.length)
        || !cm_decl_copy_bytes(&metadata->target_triple,
            state->input->target_triple.data,
            state->input->target_triple.length)
        || !cm_decl_copy_bytes(&metadata->data_layout,
            state->input->data_layout.data,
            state->input->data_layout.length)) return 0;
    metadata->edition = edition;
    if (config->panic_strategy.length == 5u
        && memcmp(config->panic_strategy.data, "abort", 5u) == 0)
        metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    else if (config->panic_strategy.length == 6u
        && memcmp(config->panic_strategy.data, "unwind", 6u) == 0)
        metadata->panic_strategy = CM_HIR_DECL_PANIC_UNWIND;
    else return 0;
    metadata->cfg_count = config->cfg_count;
    metadata->cfgs = config->cfg_count == 0u ? NULL
        : (CmHirDeclarationString *)cm_alloc_zeroed(config->cfg_count,
            sizeof(*metadata->cfgs));
    for (index = 0u; index < config->cfg_count; ++index)
        cm_decl_copy_bytes(&metadata->cfgs[index], config->cfgs[index].data,
            config->cfgs[index].length);
    return 1;
}

static int cm_decl_fill_modules(const CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata)
{
    size_t index;
    metadata->module_count = state->module_count;
    metadata->modules = (CmHirDeclarationModule *)cm_alloc_zeroed(
        metadata->module_count, sizeof(*metadata->modules));
    for (index = 0u; index < state->module_count; ++index) {
        const CmDeclCaptureModule *source = &state->modules[index];
        const CmInternedString *name = source->graph.parent == CM_MODULE_NONE
            ? cm_interner_get(&state->hir->strings, state->crate_value->name)
            : cm_interner_get(&state->hir->strings, source->hir->name);
        CmDeclCaptureModule *parent = source->graph.parent == CM_MODULE_NONE
            ? NULL : cm_decl_module_by_graph((CmDeclCaptureState *)state,
                source->graph.parent);
        if (!cm_decl_copy_intern(&metadata->modules[index].name, name))
            return 0;
        metadata->modules[index].parent_module = parent == NULL ? 0u
            : parent->local;
        if (parent == NULL) metadata->root_module = source->local;
    }
    return metadata->root_module != 0u;
}

static int cm_decl_enum_variant_source_ordinal(
    const CmDeclCaptureState *state, const CmDeclCaptureItem *capture,
    uint32_t variant_index, uint32_t *out_ordinal)
{
    CmDeclCaptureModule *module = cm_decl_module_by_local(
        (CmDeclCaptureState *)state, capture->owner_module);
    CmResolveEffectiveItem enumeration;
    CmResolveEffectiveVariant variant;
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id,
            capture->source_ordinal, &enumeration) != CM_RESOLVE_VIEW_OK
        || enumeration.item_kind != CM_AST_ITEM_ENUM
        || cm_module_graph_get_effective_variant(state->input->graph,
            state->input->revision, module->graph.id, enumeration.id,
            variant_index, &variant) != CM_RESOLVE_VIEW_OK) return 0;
    *out_ordinal = variant.declaration.index;
    return 1;
}

static int cm_decl_copy_enum_variant_lang(
    const CmDeclCaptureState *state, const CmDeclCaptureItem *capture,
    uint32_t variant_index, CmHirDeclarationString *out_lang)
{
    CmDeclCaptureModule *module = cm_decl_module_by_local(
        (CmDeclCaptureState *)state, capture->owner_module);
    CmResolveEffectiveItem enumeration;
    CmResolveEffectiveVariant effective;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    const CmAstVariant *ast_variant;
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id,
            capture->source_ordinal, &enumeration) != CM_RESOLVE_VIEW_OK
        || enumeration.item_kind != CM_AST_ITEM_ENUM
        || cm_module_graph_get_effective_variant(state->input->graph,
            state->input->revision, module->graph.id, enumeration.id,
            variant_index, &effective) != CM_RESOLVE_VIEW_OK
        || !cm_module_graph_borrow_item_ast(state->input->graph,
            module->graph.id, enumeration.declaration, &ast)
        || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            enumeration.declaration.item)) == NULL
        || ast_item->kind != CM_AST_ITEM_ENUM
        || effective.declaration.index
            >= ast_item->data.enum_item.variant_count
        || ast_item->data.enum_item.variants == NULL) return 0;
    ast_variant = &ast_item->data.enum_item.variants[
        effective.declaration.index];
    return cm_decl_enum_generic_variant_attributes(state, module,
        &enumeration, &effective, variant_index, ast, ast_variant,
        capture->item->data.enum_item.variants[variant_index].lang_item,
        out_lang);
}

static int cm_decl_fill_items_and_generics(CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t generic_count = 0u;
    size_t cursor = 0u;
    metadata->trait_count = state->trait_count;
    metadata->traits = (CmHirDeclarationTrait *)cm_alloc_zeroed(
        state->trait_count, sizeof(*metadata->traits));
    metadata->item_count = state->item_count;
    metadata->items = state->item_count == 0u ? NULL
        : (CmHirDeclarationItem *)cm_alloc_zeroed(state->item_count,
            sizeof(*metadata->items));
    metadata->value_count = state->value_count;
    metadata->values = (CmHirDeclarationValue *)cm_alloc_zeroed(
        state->value_count, sizeof(*metadata->values));
    for (index = 0u; index < state->trait_count; ++index)
        generic_count += state->traits[index].item->generic_parameter_count;
    for (index = 0u; index < state->item_count; ++index)
        generic_count += state->items[index].item->generic_parameter_count;
    for (index = 0u; index < state->value_count; ++index)
        generic_count += state->values[index].item->generic_parameter_count;
    if (generic_count > CM_HIR_DECL_METADATA_MAX_RECORDS) return 0;
    metadata->generic_count = generic_count;
    metadata->generics = (CmHirDeclarationGeneric *)cm_alloc_zeroed(
        generic_count, sizeof(*metadata->generics));
    state->generic_locals = (uint32_t *)cm_alloc_zeroed(
        state->hir->generic_parameters.len, sizeof(*state->generic_locals));
    for (index = 0u; index < state->item_count; ++index) {
        const CmDeclCaptureItem *capture = &state->items[index];
        CmHirDeclarationItem *wire = &metadata->items[index];
        uint32_t variant_index;
        if (capture->item->kind == CM_HIR_ITEM_STRUCT)
            wire->kind = CM_HIR_DECL_ITEM_STRUCT;
        else if (capture->item->kind == CM_HIR_ITEM_UNION)
            wire->kind = CM_HIR_DECL_ITEM_UNION;
        else if (capture->item->kind == CM_HIR_ITEM_ENUM)
            wire->kind = CM_HIR_DECL_ITEM_ENUM;
        else wire->kind = CM_HIR_DECL_ITEM_TYPE_ALIAS;
        wire->owner_module = capture->owner_module;
        wire->source_ordinal = capture->source_ordinal;
        wire->visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
        wire->visibility.restriction_module = 0u;
        if (capture->item->kind == CM_HIR_ITEM_STRUCT
                || capture->item->kind == CM_HIR_ITEM_UNION) {
            uint32_t field_index;
            wire->aggregate_form = capture->item->data.aggregate_item.form
                    == CM_HIR_AGGREGATE_UNIT
                ? CM_HIR_DECL_AGGREGATE_UNIT
                : CM_HIR_DECL_AGGREGATE_NAMED;
            wire->aggregate_repr = capture->item->data.aggregate_item.form
                    == CM_HIR_AGGREGATE_UNIT
                ? CM_HIR_DECL_AGGREGATE_REPR_RUST
                : capture->aggregate_repr;
            wire->aggregate_flags = capture->aggregate_flags;
            wire->field_count =
                capture->item->data.aggregate_item.field_count;
            wire->fields = wire->field_count == 0u ? NULL
                : (CmHirDeclarationField *)cm_alloc_zeroed(
                    wire->field_count, sizeof(*wire->fields));
            if ((wire->aggregate_flags
                    & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u
                && !cm_decl_copy_bytes(&wire->lang_item, capture->lang_item,
                    capture->lang_item_length)) return 0;
            for (field_index = 0u; field_index < wire->field_count;
                    ++field_index) {
                const CmHirField *field =
                    &capture->item->data.aggregate_item.fields[field_index];
                CmHirDeclarationField *wire_field =
                    &wire->fields[field_index];
                if (!cm_decl_copy_intern(&wire_field->name,
                        cm_interner_get(&state->hir->strings, field->name)))
                    return 0;
                wire_field->visibility.kind = field->visibility.kind
                        == CM_HIR_VIS_PUBLIC
                    ? CM_HIR_DECL_VISIBILITY_PUBLIC
                    : CM_HIR_DECL_VISIBILITY_PRIVATE;
                wire_field->visibility.restriction_module = 0u;
                wire_field->source_ordinal = field_index;
            }
        } else if (capture->item->kind == CM_HIR_ITEM_ENUM) {
            const unsigned char *diagnostic_name = NULL;
            size_t diagnostic_name_length = 0u;
            int generic_default =
                capture->item->generic_parameter_count != 0u;
            int rust_default = generic_default;
            uint32_t attribute_index;
            for (attribute_index = 0u;
                    attribute_index < capture->item->attribute_count;
                    ++attribute_index) {
                const CmInternedString *attribute_metadata = cm_interner_get(
                    &state->hir->strings,
                    capture->item->attributes[attribute_index].metadata);
                const unsigned char *candidate_name = NULL;
                size_t candidate_length = 0u;
                if (cm_decl_diagnostic_item_name(attribute_metadata,
                        &candidate_name, &candidate_length)) {
                    diagnostic_name = candidate_name;
                    diagnostic_name_length = candidate_length;
                    rust_default = 1;
                }
            }
            wire->enum_repr_primitive = rust_default
                ? CM_HIR_DECL_ENUM_REPR_RUST
                : CM_HIR_DECL_PRIMITIVE_U8;
            if (rust_default
                && !cm_decl_copy_bytes(&wire->diagnostic_item,
                    diagnostic_name, diagnostic_name_length)) return 0;
            if (capture->lang_item_length != 0u) {
                wire->enum_flags = CM_HIR_DECL_ENUM_HAS_LANG_ITEM;
                if (!cm_decl_copy_bytes(&wire->enum_lang_item,
                        capture->lang_item, capture->lang_item_length))
                    return 0;
            }
            wire->variant_count =
                capture->item->data.enum_item.variant_count;
            wire->variants = (CmHirDeclarationVariant *)cm_alloc_zeroed(
                wire->variant_count, sizeof(*wire->variants));
            for (variant_index = 0u; variant_index < wire->variant_count;
                    ++variant_index) {
                const CmHirVariant *source =
                    &capture->item->data.enum_item.variants[variant_index];
                CmHirDeclarationVariant *variant =
                    &wire->variants[variant_index];
                variant->kind = source->form == CM_HIR_AGGREGATE_TUPLE
                    ? CM_HIR_DECL_VARIANT_TUPLE
                    : CM_HIR_DECL_VARIANT_UNIT;
                if (!cm_decl_enum_variant_source_ordinal(state, capture,
                        variant_index, &variant->source_ordinal)) return 0;
                if (rust_default) {
                    variant->discriminant_primitive =
                        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
                } else {
                    variant->discriminant_primitive =
                        CM_HIR_DECL_PRIMITIVE_ISIZE;
                    variant->discriminant_low =
                        source->discriminant.data.value.low_bits;
                    variant->discriminant_high =
                        source->discriminant.data.value.high_bits;
                }
                if (!cm_decl_copy_intern(&variant->name,
                        cm_interner_get(&state->hir->strings,
                            source->name))) return 0;
                if (generic_default) {
                    uint32_t field_index;
                    variant->flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
                    if (!cm_decl_copy_enum_variant_lang(state, capture,
                            variant_index, &variant->lang_item)) return 0;
                    variant->field_count = source->field_count;
                    variant->fields = variant->field_count == 0u ? NULL
                        : (CmHirDeclarationVariantField *)cm_alloc_zeroed(
                            variant->field_count,
                            sizeof(*variant->fields));
                    for (field_index = 0u;
                            field_index < variant->field_count;
                            ++field_index)
                        variant->fields[field_index].source_ordinal =
                            field_index;
                }
            }
        }
    }
#define CM_DECL_FILL_OWNER(items_, count_, wire_, owner_tag_) do { \
    for (index = 0u; index < (count_); ++index) { \
        const CmDeclCaptureItem *capture_ = &(items_)[index]; \
        const CmHirItem *item_ = capture_->item; \
        const CmInternedString *item_name_ = cm_decl_item_name(state, item_); \
        uint32_t child_; \
        (wire_)[index].owner_module = capture_->owner_module; \
        (wire_)[index].source_ordinal = capture_->source_ordinal; \
        cm_decl_copy_intern(&(wire_)[index].name, item_name_); \
        (wire_)[index].generic_start = item_->generic_parameter_count == 0u \
            ? 0u : (uint32_t)(cursor + 1u); \
        (wire_)[index].generic_count = item_->generic_parameter_count; \
        for (child_ = 0u; child_ < item_->generic_parameter_count; ++child_) { \
            CmHirGenericParamId id_ = item_->generic_parameter_start + child_; \
            const CmHirGenericParam *generic_ = cm_hir_get_generic_param( \
                state->hir, id_); \
            const CmInternedString *name_ = cm_interner_get( \
                &state->hir->strings, generic_->name); \
            metadata->generics[cursor].owner_kind = (owner_tag_); \
            metadata->generics[cursor].owner_local = (uint32_t)(index + 1u); \
            metadata->generics[cursor].index = child_; \
            metadata->generics[cursor].kind = CM_HIR_DECL_GENERIC_TYPE; \
            metadata->generics[cursor].is_relaxed_sized = \
                (uint8_t)generic_->is_relaxed_sized; \
            cm_decl_copy_intern(&metadata->generics[cursor].name, name_); \
            state->generic_locals[id_ - 1u] = (uint32_t)(cursor + 1u); \
            cursor += 1u; \
        } \
    } \
} while (0)
    CM_DECL_FILL_OWNER(state->traits, state->trait_count, metadata->traits,
        CM_HIR_DECL_GENERIC_NOMINAL);
    CM_DECL_FILL_OWNER(state->items, state->item_count, metadata->items,
        CM_HIR_DECL_GENERIC_ITEM);
    CM_DECL_FILL_OWNER(state->values, state->value_count, metadata->values,
        CM_HIR_DECL_GENERIC_VALUE);
#undef CM_DECL_FILL_OWNER
    return cursor == generic_count;
}

static int cm_decl_compound_candidate(const CmDeclCaptureState *state,
    const CmHirDeclarationMetadata *metadata, const uint32_t *depths,
    CmHirTypeId id, uint8_t *out_kind, uint32_t *out_depth)
{
    const CmHirType *type = cm_hir_get_type(state->hir, id);
    uint32_t depth = 1u;
    uint32_t child;
    if (type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_ADT_KIND) {
        if (type->data.named_type.argument_count == 0u
            || type->data.named_type.arguments == NULL
            || cm_decl_item_local(state,
                type->data.named_type.definition) == 0u) return 0;
        *out_kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
        for (child = 0u; child < type->data.named_type.argument_count;
                ++child) {
            uint32_t local;
            if (type->data.named_type.arguments[child].kind
                    != CM_HIR_GENERIC_ARG_TYPE
                || (local = cm_decl_type_local(state, metadata,
                    type->data.named_type.arguments[child].data.type)) == 0u)
                return 0;
            if (depths[local - 1u] >= depth)
                depth = depths[local - 1u] + 1u;
        }
    } else if (type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        if (type->data.tuple_type.element_count == 0u
            || type->data.tuple_type.elements == NULL) return 0;
        *out_kind = CM_HIR_DECL_TYPE_TUPLE;
        for (child = 0u; child < type->data.tuple_type.element_count;
                ++child) {
            uint32_t local = cm_decl_type_local(state, metadata,
                type->data.tuple_type.elements[child]);
            if (local == 0u) return 0;
            if (depths[local - 1u] >= depth)
                depth = depths[local - 1u] + 1u;
        }
    } else if (type->kind == CM_HIR_TYPE_ARRAY_KIND) {
        uint32_t element = cm_decl_type_local(state, metadata,
            type->data.array_type.element);
        uint32_t length = cm_decl_type_local(state, metadata,
            type->data.array_type.length.type);
        if (element == 0u || length == 0u
            || type->data.array_type.length.kind != CM_HIR_CONST_VALUE)
            return 0;
        *out_kind = CM_HIR_DECL_TYPE_ARRAY;
        if (depths[element - 1u] >= depth)
            depth = depths[element - 1u] + 1u;
        if (depths[length - 1u] >= depth)
            depth = depths[length - 1u] + 1u;
    } else return 0;
    *out_depth = depth;
    return 1;
}

static int cm_decl_compound_before(const CmDeclCaptureState *state,
    const CmHirDeclarationMetadata *metadata, CmHirTypeId left_id,
    uint8_t left_kind, uint32_t left_depth, CmHirTypeId right_id,
    uint8_t right_kind, uint32_t right_depth)
{
    const CmHirType *left = cm_hir_get_type(state->hir, left_id);
    const CmHirType *right = cm_hir_get_type(state->hir, right_id);
    uint32_t index;
    if (left_depth != right_depth) return left_depth < right_depth;
    if (left_kind != right_kind) return left_kind < right_kind;
    if (left_kind == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
        uint32_t left_item = cm_decl_item_local(state,
            left->data.named_type.definition);
        uint32_t right_item = cm_decl_item_local(state,
            right->data.named_type.definition);
        if (left_item != right_item) return left_item < right_item;
        if (left->data.named_type.argument_count
                != right->data.named_type.argument_count)
            return left->data.named_type.argument_count
                < right->data.named_type.argument_count;
        for (index = 0u; index < left->data.named_type.argument_count;
                ++index) {
            uint32_t left_local = cm_decl_type_local(state, metadata,
                left->data.named_type.arguments[index].data.type);
            uint32_t right_local = cm_decl_type_local(state, metadata,
                right->data.named_type.arguments[index].data.type);
            if (left_local != right_local) return left_local < right_local;
        }
    } else if (left_kind == CM_HIR_DECL_TYPE_TUPLE) {
        if (left->data.tuple_type.element_count
                != right->data.tuple_type.element_count)
            return left->data.tuple_type.element_count
                < right->data.tuple_type.element_count;
        for (index = 0u; index < left->data.tuple_type.element_count; ++index) {
            uint32_t left_local = cm_decl_type_local(state, metadata,
                left->data.tuple_type.elements[index]);
            uint32_t right_local = cm_decl_type_local(state, metadata,
                right->data.tuple_type.elements[index]);
            if (left_local != right_local) return left_local < right_local;
        }
    } else {
        uint32_t left_child = cm_decl_type_local(state, metadata,
            left->data.array_type.element);
        uint32_t right_child = cm_decl_type_local(state, metadata,
            right->data.array_type.element);
        uint32_t left_length = cm_decl_type_local(state, metadata,
            left->data.array_type.length.type);
        uint32_t right_length = cm_decl_type_local(state, metadata,
            right->data.array_type.length.type);
        if (left_child != right_child) return left_child < right_child;
        if (left_length != right_length) return left_length < right_length;
        if (left->data.array_type.length.data.value.low_bits
                != right->data.array_type.length.data.value.low_bits)
            return left->data.array_type.length.data.value.low_bits
                < right->data.array_type.length.data.value.low_bits;
        if (left->data.array_type.length.data.value.high_bits
                != right->data.array_type.length.data.value.high_bits)
            return left->data.array_type.length.data.value.high_bits
                < right->data.array_type.length.data.value.high_bits;
    }
    return left_id < right_id;
}

static int cm_decl_fill_compound_types(CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata, size_t *cursor,
    size_t pending)
{
    uint32_t *depths = (uint32_t *)cm_alloc_zeroed(metadata->type_count,
        sizeof(*depths));
    while (pending != 0u) {
        CmHirTypeId selected = CM_HIR_TYPE_NONE;
        uint8_t selected_kind = 0u;
        uint32_t selected_depth = 0u;
        size_t index;
        for (index = 0u; index < state->hir->types.len; ++index) {
            CmHirTypeId candidate_id;
            uint8_t kind;
            uint32_t depth;
            int active = state->application_types[index] == 1u
                || state->compound_types[index] == 1u;
            if (!active) continue;
            candidate_id = (CmHirTypeId)(index + 1u);
            if (cm_decl_type_local(state, metadata, candidate_id) != 0u) {
                state->application_types[index] = state->application_types[index]
                    == 0u ? 0u : 2u;
                state->compound_types[index] = state->compound_types[index]
                    == 0u ? 0u : 2u;
                pending -= 1u;
                continue;
            }
            if (!cm_decl_compound_candidate(state, metadata, depths,
                    candidate_id, &kind, &depth)) continue;
            if (selected == CM_HIR_TYPE_NONE
                || cm_decl_compound_before(state, metadata, candidate_id,
                    kind, depth, selected, selected_kind, selected_depth)) {
                selected = candidate_id;
                selected_kind = kind;
                selected_depth = depth;
            }
        }
        if (pending == 0u) break;
        if (selected == CM_HIR_TYPE_NONE || *cursor >= metadata->type_count) {
            cm_free(depths);
            return 0;
        }
        {
            const CmHirType *source = cm_hir_get_type(state->hir, selected);
            CmHirDeclarationType *wire = &metadata->types[*cursor];
            uint32_t child;
            wire->kind = selected_kind;
            if (selected_kind == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
                wire->item_local = cm_decl_item_local(state,
                    source->data.named_type.definition);
                wire->argument_count = source->data.named_type.argument_count;
                wire->argument_types = (uint32_t *)cm_alloc_zeroed(
                    wire->argument_count, sizeof(*wire->argument_types));
                for (child = 0u; child < wire->argument_count; ++child)
                    wire->argument_types[child] = cm_decl_type_local(state,
                        metadata,
                        source->data.named_type.arguments[child].data.type);
            } else if (selected_kind == CM_HIR_DECL_TYPE_TUPLE) {
                wire->element_count = source->data.tuple_type.element_count;
                wire->element_types = (uint32_t *)cm_alloc_zeroed(
                    wire->element_count, sizeof(*wire->element_types));
                for (child = 0u; child < wire->element_count; ++child)
                    wire->element_types[child] = cm_decl_type_local(state,
                        metadata, source->data.tuple_type.elements[child]);
            } else {
                wire->child_type = cm_decl_type_local(state, metadata,
                    source->data.array_type.element);
                wire->array_length_type = cm_decl_type_local(state, metadata,
                    source->data.array_type.length.type);
                wire->array_length_low_bits =
                    source->data.array_type.length.data.value.low_bits;
                wire->array_length_high_bits =
                    source->data.array_type.length.data.value.high_bits;
            }
            depths[*cursor] = selected_depth;
            *cursor += 1u;
            state->application_types[selected - 1u] =
                state->application_types[selected - 1u] == 0u ? 0u : 2u;
            state->compound_types[selected - 1u] =
                state->compound_types[selected - 1u] == 0u ? 0u : 2u;
            pending -= 1u;
        }
    }
    cm_free(depths);
    return 1;
}

static int cm_decl_fill_types_values_predicates(CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata,
    CmHirDeclarationCaptureResult *result)
{
    size_t index;
    size_t predicate_count = 0u;
    size_t type_count = 0u;
    size_t application_count = 0u;
    size_t compound_count = 0u;
    size_t cursor;
    state->generic_types = (unsigned char *)cm_alloc_zeroed(
        metadata->generic_count, sizeof(*state->generic_types));
    state->named_item_types = (unsigned char *)cm_alloc_zeroed(
        metadata->item_count, sizeof(*state->named_item_types));
    state->application_types = (unsigned char *)cm_alloc_zeroed(
        state->hir->types.len, sizeof(*state->application_types));
    state->compound_types = (unsigned char *)cm_alloc_zeroed(
        state->hir->types.len, sizeof(*state->compound_types));
    for (index = 0u; index < state->item_count; ++index) {
        const CmHirItem *item = state->items[index].item;
        uint32_t child;
        if ((item->kind == CM_HIR_ITEM_STRUCT
                || item->kind == CM_HIR_ITEM_UNION)
            && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED) {
            for (child = 0u; child < item->data.aggregate_item.field_count;
                    ++child) {
                if (!cm_decl_mark_type(state,
                        item->data.aggregate_item.fields[child].type, result))
                    return 0;
            }
        } else if (item->kind == CM_HIR_ITEM_ENUM) {
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->data.enum_item.variant_count;
                    ++variant_index) {
                const CmHirVariant *variant =
                    &item->data.enum_item.variants[variant_index];
                for (child = 0u; child < variant->field_count; ++child)
                    if (!cm_decl_mark_type(state, variant->fields[child].type,
                            result)) return 0;
            }
        } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && !cm_decl_mark_named_adt(state,
                item->data.type_alias_item.target, result)) return 0;
    }
    for (index = 0u; index < state->value_count; ++index) {
        const CmHirItem *item = state->values[index].item;
        uint32_t child;
        if (item->kind == CM_HIR_ITEM_CONST
            || item->kind == CM_HIR_ITEM_STATIC) {
            if (!cm_decl_mark_type(state, item->data.value_item.type, result))
                return 0;
            continue;
        }
        if (item->kind != CM_HIR_ITEM_FUNCTION) return 0;
        {
            const CmHirFunctionSignature *signature =
                &item->data.function_item.signature;
            predicate_count += item->predicate_count;
            for (child = 0u; child < signature->parameter_count; ++child)
                if (!cm_decl_mark_type(state,
                        signature->parameters[child].type, result)) return 0;
            if (!cm_decl_mark_type(state, signature->return_type, result))
                return 0;
        }
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            uint32_t argument;
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->binder.lifetime_count != 0u
                || predicate->binder.lifetimes != NULL
                || predicate->equality_count != 0u
                || predicate->equalities != NULL
                || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
                || cm_decl_trait_local(state,
                    predicate->trait_type.definition) == 0u
                || !cm_decl_mark_type(state, predicate->subject, result))
                return 0;
            for (argument = 0u;
                    argument < predicate->trait_type.argument_count;
                    ++argument) {
                if (predicate->trait_type.arguments[argument].kind
                        != CM_HIR_GENERIC_ARG_TYPE
                    || !cm_decl_mark_type(state,
                        predicate->trait_type.arguments[argument].data.type,
                        result)) return 0;
            }
        }
    }
    if (predicate_count > CM_HIR_DECL_METADATA_MAX_RECORDS) return 0;
    for (index = CM_HIR_DECL_PRIMITIVE_UNIT;
            index <= CM_HIR_DECL_PRIMITIVE_F64; ++index)
        if (state->primitive_types[index]) type_count += 1u;
    for (index = 0u; index < metadata->generic_count; ++index)
        if (state->generic_types[index]) type_count += 1u;
    for (index = 0u; index < metadata->item_count; ++index)
        if (state->named_item_types[index]) type_count += 1u;
    for (index = 0u; index < state->hir->types.len; ++index)
        if (state->application_types[index]) {
            application_count += 1u;
            type_count += 1u;
        }
    for (index = 0u; index < state->hir->types.len; ++index)
        if (state->compound_types[index]) {
            compound_count += 1u;
            type_count += 1u;
        }
    if (type_count > CM_HIR_DECL_METADATA_MAX_TYPES) return 0;
    metadata->type_count = type_count;
    metadata->types = (CmHirDeclarationType *)cm_alloc_zeroed(type_count,
        sizeof(*metadata->types));
    cursor = 0u;
    for (index = CM_HIR_DECL_PRIMITIVE_UNIT;
            index <= CM_HIR_DECL_PRIMITIVE_F64; ++index) {
        if (state->primitive_types[index]) {
            metadata->types[cursor].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
            metadata->types[cursor].primitive = (uint8_t)index;
            cursor += 1u;
        }
    }
    for (index = 0u; index < metadata->generic_count; ++index) {
        if (state->generic_types[index]) {
            metadata->types[cursor].kind = CM_HIR_DECL_TYPE_GENERIC;
            metadata->types[cursor].generic_local = (uint32_t)(index + 1u);
            cursor += 1u;
        }
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        if (state->named_item_types[index]) {
            metadata->types[cursor].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
            metadata->types[cursor].item_local = (uint32_t)(index + 1u);
            cursor += 1u;
        }
    }
    if (!cm_decl_fill_compound_types(state, metadata, &cursor,
            application_count + compound_count)) return 0;
    /* The provisional count is by live HIR ID. Structural duplicates consume
     * pending IDs but intentionally share the already emitted canonical local. */
    if (cursor > type_count) return 0;
    metadata->type_count = cursor;
    for (index = 0u; index < state->item_count; ++index) {
        const CmHirItem *item = state->items[index].item;
        uint32_t child;
        if ((item->kind == CM_HIR_ITEM_STRUCT
                || item->kind == CM_HIR_ITEM_UNION)
            && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED) {
            for (child = 0u; child < item->data.aggregate_item.field_count;
                    ++child) {
                metadata->items[index].fields[child].type_local =
                    cm_decl_type_local(state, metadata,
                        item->data.aggregate_item.fields[child].type);
                if (metadata->items[index].fields[child].type_local == 0u)
                    return 0;
            }
        } else if (item->kind == CM_HIR_ITEM_ENUM) {
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->data.enum_item.variant_count;
                    ++variant_index) {
                const CmHirVariant *variant =
                    &item->data.enum_item.variants[variant_index];
                CmHirDeclarationVariant *wire_variant =
                    &metadata->items[index].variants[variant_index];
                for (child = 0u; child < variant->field_count; ++child) {
                    wire_variant->fields[child].type_local =
                        cm_decl_type_local(state, metadata,
                            variant->fields[child].type);
                    if (wire_variant->fields[child].type_local == 0u)
                        return 0;
                }
            }
        } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            metadata->items[index].alias_target_type = cm_decl_type_local(
                state, metadata, item->data.type_alias_item.target);
            if (metadata->items[index].alias_target_type == 0u) return 0;
        }
    }
    metadata->predicate_count = predicate_count;
    metadata->predicates = (CmHirDeclarationPredicate *)cm_alloc_zeroed(
        predicate_count, sizeof(*metadata->predicates));
    cursor = 0u;
    for (index = 0u; index < state->value_count; ++index) {
        const CmHirItem *item = state->values[index].item;
        CmHirDeclarationValue *value = &metadata->values[index];
        uint32_t child;
        if (item->kind == CM_HIR_ITEM_CONST
            || item->kind == CM_HIR_ITEM_STATIC) {
            value->kind = item->kind == CM_HIR_ITEM_CONST
                ? CM_HIR_DECL_VALUE_CONST : CM_HIR_DECL_VALUE_STATIC;
            value->declared_type = cm_decl_type_local(state, metadata,
                item->data.value_item.type);
            value->mutability = item->data.value_item.mutability
                == CM_HIR_MUTABLE ? CM_HIR_DECL_MUTABLE
                : CM_HIR_DECL_IMMUTABLE;
            value->has_body = UINT8_C(1);
            if (value->declared_type == 0u) return 0;
            continue;
        }
        if (item->kind != CM_HIR_ITEM_FUNCTION) return 0;
        value->kind = CM_HIR_DECL_VALUE_FUNCTION;
        value->predicate_start = (uint32_t)(cursor + 1u);
        value->predicate_count = item->predicate_count;
        value->parameter_count = item->data.function_item.signature
            .parameter_count;
        value->parameter_types = value->parameter_count == 0u ? NULL
            : (uint32_t *)cm_alloc_zeroed(value->parameter_count,
                sizeof(*value->parameter_types));
        for (child = 0u; child < value->parameter_count; ++child)
            value->parameter_types[child] = cm_decl_type_local(state,
                metadata,
                item->data.function_item.signature.parameters[child].type);
        value->return_type = cm_decl_type_local(state, metadata,
            item->data.function_item.signature.return_type);
        value->has_body = item->data.function_item.body == CM_HIR_BODY_NONE
            ? UINT8_C(0) : UINT8_C(1);
        if (value->return_type == 0u) return 0;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            CmHirDeclarationPredicate *wire = &metadata->predicates[cursor];
            uint32_t argument;
            wire->owner_value = (uint32_t)(index + 1u);
            wire->ordinal = child;
            wire->subject_type = cm_decl_type_local(state, metadata,
                predicate->subject);
            wire->trait_local = cm_decl_trait_local(state,
                predicate->trait_type.definition);
            wire->argument_count = predicate->trait_type.argument_count;
            wire->argument_types = wire->argument_count == 0u ? NULL
                : (uint32_t *)cm_alloc_zeroed(wire->argument_count,
                    sizeof(*wire->argument_types));
            for (argument = 0u; argument < wire->argument_count; ++argument)
                wire->argument_types[argument] = cm_decl_type_local(state,
                    metadata,
                    predicate->trait_type.arguments[argument].data.type);
            if (wire->subject_type == 0u || wire->trait_local == 0u)
                return 0;
            cursor += 1u;
        }
    }
    return cursor == predicate_count;
}

static int cm_decl_fill_namespace(const CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata)
{
    size_t index;
    metadata->namespace_count = state->namespace_count;
    metadata->namespace_entries = (CmHirDeclarationNamespaceEntry *)
        cm_alloc_zeroed(state->namespace_count,
            sizeof(*metadata->namespace_entries));
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *source =
            &state->namespace_values[index];
        CmHirDeclarationNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        entry->owner_module = source->owner_module;
        entry->namespace_kind = source->namespace_kind;
        cm_decl_copy_bytes(&entry->name, source->name, source->name_length);
        entry->export_ordinal = source->export_ordinal;
        if (source->target.kind == CM_HIR_LIBRARY_BINDING_MODULE) {
            CmDeclCaptureModule *module = cm_decl_module_by_definition(
                (CmDeclCaptureState *)state, source->target.definition);
            if (module == NULL) return 0;
            entry->target_kind = CM_HIR_DECL_TARGET_MODULE;
            entry->target_local = module->local;
        } else if (source->target.kind == CM_HIR_LIBRARY_BINDING_TRAIT) {
            entry->target_kind = CM_HIR_DECL_TARGET_NOMINAL;
            entry->target_local = cm_decl_trait_local(state,
                source->target.definition);
        } else if (source->target.kind == CM_HIR_LIBRARY_BINDING_VALUE) {
            entry->target_kind = CM_HIR_DECL_TARGET_VALUE;
            entry->target_local = cm_decl_value_local(state,
                source->target.definition);
        } else if (source->target.kind
                == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
            entry->target_kind = CM_HIR_DECL_TARGET_PRIMITIVE;
            entry->target_local = cm_decl_library_primitive(
                source->target.primitive_kind);
        } else if (source->target.kind
                == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT) {
            entry->target_kind = CM_HIR_DECL_TARGET_ENUM_VARIANT;
            entry->target_local = cm_decl_enum_variant_local(state,
                &source->target);
        } else if (source->target.kind == CM_HIR_LIBRARY_BINDING_TYPE
            || source->target.kind
                == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR) {
            entry->target_kind = CM_HIR_DECL_TARGET_ITEM;
            entry->target_local = cm_decl_item_local(state,
                source->target.definition);
        } else return 0;
        if (entry->target_local == 0u) return 0;
    }
    return 1;
}

static void cm_decl_state_destroy(CmDeclCaptureState *state)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        cm_free(state->modules[index].path);
    for (index = 0u; index < state->namespace_capacity; ++index)
        cm_free(state->namespace_values[index].name);
    cm_free(state->modules);
    cm_free(state->namespace_values);
    cm_free(state->traits);
    cm_free(state->items);
    cm_free(state->values);
    cm_free(state->generic_locals);
    cm_free(state->generic_types);
    cm_free(state->named_item_types);
    cm_free(state->application_types);
    cm_free(state->compound_types);
}

CmHirDeclarationCaptureResult cm_hir_declaration_metadata_capture(
    const CmHirDeclarationCaptureInput *input,
    CmHirDeclarationMetadata *output)
{
    CmHirDeclarationCaptureResult result = cm_decl_capture_result(
        CM_HIR_DECL_CAPTURE_INVALID_ARGUMENT);
    CmDeclCaptureState state;
    CmHirLibraryArtifact library;
    CmHirLibraryArtifactResult library_result;
    const CmHirLibraryOwnedData *owned;
    CmHirDeclarationMetadata candidate;
    CmHirDeclarationMetadata old;
    uint64_t graph_lifetime;
    uint64_t resolver_lifetime;
    uint64_t resolver_generation;
    uint64_t storage_lifetime;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    result.failure_stage = CM_HIR_DECL_CAPTURE_STAGE_INPUT;
    result.failure_reason = CM_HIR_DECL_CAPTURE_REASON_INVALID_ARGUMENT;
    if (input == NULL || output == NULL || input->hir == NULL
        || input->graph == NULL || input->imports == NULL
        || input->modules == NULL || input->configuration == NULL
        || input->crate_id == CM_HIR_CRATE_NONE
        || input->revision == CM_MODULE_GRAPH_REVISION_NONE
        || input->crate_disambiguator.data == NULL
        || input->crate_disambiguator.length == 0u
        || input->target_triple.data == NULL
        || input->target_triple.length == 0u
        || input->data_layout.data == NULL
        || input->data_layout.length == 0u) return result;
    if (cm_module_graph_revision(input->graph) != input->revision
        || cm_module_graph_error_count(input->graph) != 0u
        || cm_import_resolver_revision(input->imports) != input->revision
        || cm_import_error_count(input->imports) != 0u
        || !cm_import_resolver_matches_graph(input->imports, input->graph)) {
        result.status = CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY;
        result.failure_stage = CM_HIR_DECL_CAPTURE_STAGE_AUTHORITY;
        result.failure_reason = CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_MISMATCH;
        return result;
    }
    result.failure_stage = CM_HIR_DECL_CAPTURE_STAGE_NONE;
    result.failure_reason = CM_HIR_DECL_CAPTURE_REASON_NONE;
    memset(&state, 0, sizeof(state));
    state.input = input;
    state.hir = input->hir;
    state.crate_value = cm_hir_get_crate(input->hir, input->crate_id);
    if (state.crate_value == NULL) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_INPUT,
            CM_HIR_DECL_CAPTURE_REASON_CRATE_NOT_FOUND);
        return result;
    }
    graph_lifetime = cm_module_graph_lifetime_id(input->graph);
    resolver_lifetime = cm_import_resolver_lifetime_id(input->imports);
    resolver_generation = cm_import_resolver_generation(input->imports);
    storage_lifetime = input->hir->storage.lifetime_id;
    semantic_generation = input->hir->semantic_generation;
    rewind_generation = input->hir->rewind_generation;
    cm_hir_library_artifact_init(&library);
    library_result = cm_hir_library_declaration_artifact_build(&library,
        input->hir, input->crate_id, input->graph, input->revision,
        input->modules, "capture");
    if (library_result.status != CM_HIR_LIBRARY_OK) {
        result.status = CM_HIR_DECL_CAPTURE_LIBRARY_FAILURE;
        result.library_status = library_result.status;
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_LIBRARY,
            CM_HIR_DECL_CAPTURE_REASON_LIBRARY_REJECTED);
        cm_hir_library_artifact_destroy(&library);
        return result;
    }
    owned = cm_hir_library_artifact_owned_data_const(&library);
    cm_hir_declaration_metadata_init(&candidate);
    candidate.owns_storage = 1;
    result.status = CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR;
    if (owned == NULL) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_LIBRARY,
            CM_HIR_DECL_CAPTURE_REASON_OWNED_DATA_MISSING);
        goto done;
    }
    state.owned = owned;
    if (!cm_decl_collect_modules(&state, &result)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_MODULES,
            CM_HIR_DECL_CAPTURE_REASON_MODULE_CENSUS_INVALID);
        goto done;
    }
    if (!cm_decl_collect_namespace(&state, owned, &result)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
            CM_HIR_DECL_CAPTURE_REASON_BINDING_CENSUS_MISMATCH);
        goto done;
    }
    if (!cm_decl_collect_items(&state, &result)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
            CM_HIR_DECL_CAPTURE_REASON_ITEM_METADATA_INVALID);
        goto done;
    }
    if (!cm_decl_fill_identity(&state, &candidate)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_IDENTITY,
            CM_HIR_DECL_CAPTURE_REASON_IDENTITY_UNSUPPORTED);
        goto done;
    }
    if (!cm_decl_fill_modules(&state, &candidate)) {
        cm_decl_capture_fail(&result,
            CM_HIR_DECL_CAPTURE_STAGE_MODULE_METADATA,
            CM_HIR_DECL_CAPTURE_REASON_MODULE_METADATA_INVALID);
        goto done;
    }
    if (!cm_decl_fill_items_and_generics(&state, &candidate)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_ITEM_METADATA,
            CM_HIR_DECL_CAPTURE_REASON_ITEM_METADATA_INVALID);
        goto done;
    }
    if (!cm_decl_fill_types_values_predicates(&state, &candidate, &result)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA,
            CM_HIR_DECL_CAPTURE_REASON_TYPE_METADATA_INVALID);
        goto done;
    }
    if (!cm_decl_fill_namespace(&state, &candidate)) {
        cm_decl_capture_fail(&result,
            CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE_METADATA,
            CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_TARGET_UNMAPPED);
        goto done;
    }
    result.metadata_status = cm_hir_declaration_metadata_validate(&candidate);
    if (result.metadata_status != CM_HIR_DECL_METADATA_OK) {
        result.status = CM_HIR_DECL_CAPTURE_METADATA_FAILURE;
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_VALIDATE,
            CM_HIR_DECL_CAPTURE_REASON_METADATA_INVALID);
        goto done;
    }
    if (cm_module_graph_lifetime_id(input->graph) != graph_lifetime
        || cm_module_graph_revision(input->graph) != input->revision
        || cm_import_resolver_lifetime_id(input->imports) != resolver_lifetime
        || cm_import_resolver_generation(input->imports)
            != resolver_generation
        || !cm_import_resolver_matches_graph(input->imports, input->graph)
        || input->hir->storage.lifetime_id != storage_lifetime
        || input->hir->semantic_generation != semantic_generation
        || input->hir->rewind_generation != rewind_generation) {
        result.status = CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY;
        cm_decl_capture_fail(&result,
            CM_HIR_DECL_CAPTURE_STAGE_FINAL_AUTHORITY,
            CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_CHANGED);
        goto done;
    }
    old = *output;
    *output = candidate;
    cm_hir_declaration_metadata_init(&candidate);
    cm_hir_declaration_metadata_destroy(&old);
    result.status = CM_HIR_DECL_CAPTURE_OK;
    result.failure_stage = CM_HIR_DECL_CAPTURE_STAGE_NONE;
    result.failure_reason = CM_HIR_DECL_CAPTURE_REASON_NONE;
    result.module_count = state.module_count;
    result.trait_count = state.trait_count;
    result.item_count = state.item_count;
    result.value_count = state.value_count;
    result.predicate_count = output->predicate_count;
    result.namespace_count = state.namespace_count;
    result.projected_semantic_attribute_count =
        state.projected_semantic_attribute_count;
    result.semantic_attributes = state.projected_semantic_attribute_count == 0u
        ? CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE
        : CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION;
done:
    cm_hir_declaration_metadata_destroy(&candidate);
    cm_decl_state_destroy(&state);
    cm_hir_library_artifact_destroy(&library);
    return result;
}

const char *cm_hir_declaration_capture_status_name(
    CmHirDeclarationCaptureStatus status)
{
    switch (status) {
    case CM_HIR_DECL_CAPTURE_OK: return "ok";
    case CM_HIR_DECL_CAPTURE_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY: return "invalid authority";
    case CM_HIR_DECL_CAPTURE_LIBRARY_FAILURE: return "library failure";
    case CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR: return "unsupported HIR";
    case CM_HIR_DECL_CAPTURE_METADATA_FAILURE: return "metadata failure";
    }
    return "unknown";
}

const char *cm_hir_declaration_capture_stage_name(
    CmHirDeclarationCaptureStage stage)
{
    switch (stage) {
    case CM_HIR_DECL_CAPTURE_STAGE_NONE: return "none";
    case CM_HIR_DECL_CAPTURE_STAGE_INPUT: return "input";
    case CM_HIR_DECL_CAPTURE_STAGE_AUTHORITY: return "authority";
    case CM_HIR_DECL_CAPTURE_STAGE_LIBRARY: return "library";
    case CM_HIR_DECL_CAPTURE_STAGE_MODULES: return "modules";
    case CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE: return "namespace";
    case CM_HIR_DECL_CAPTURE_STAGE_ITEMS: return "items";
    case CM_HIR_DECL_CAPTURE_STAGE_IDENTITY: return "identity";
    case CM_HIR_DECL_CAPTURE_STAGE_MODULE_METADATA: return "module-metadata";
    case CM_HIR_DECL_CAPTURE_STAGE_ITEM_METADATA: return "item-metadata";
    case CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA: return "type-metadata";
    case CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE_METADATA:
        return "namespace-metadata";
    case CM_HIR_DECL_CAPTURE_STAGE_VALIDATE: return "validate";
    case CM_HIR_DECL_CAPTURE_STAGE_FINAL_AUTHORITY:
        return "final-authority";
    }
    return "unknown";
}

const char *cm_hir_declaration_capture_reason_name(
    CmHirDeclarationCaptureReason reason)
{
    switch (reason) {
    case CM_HIR_DECL_CAPTURE_REASON_NONE: return "none";
    case CM_HIR_DECL_CAPTURE_REASON_INVALID_ARGUMENT:
        return "invalid-argument";
    case CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_MISMATCH:
        return "authority-mismatch";
    case CM_HIR_DECL_CAPTURE_REASON_CRATE_NOT_FOUND:
        return "crate-not-found";
    case CM_HIR_DECL_CAPTURE_REASON_LIBRARY_REJECTED:
        return "library-rejected";
    case CM_HIR_DECL_CAPTURE_REASON_OWNED_DATA_MISSING:
        return "owned-data-missing";
    case CM_HIR_DECL_CAPTURE_REASON_MODULE_CENSUS_INVALID:
        return "module-census-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID:
        return "semantic-attribute-provenance-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT:
        return "semantic-attribute-projection-limit";
    case CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_MODULE_MISSING:
        return "namespace-module-missing";
    case CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT:
        return "namespace-limit";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_LOOKUP_FAILED:
        return "binding-lookup-failed";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_AUTHORITY_INVALID:
        return "binding-authority-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_NAME_INVALID:
        return "binding-name-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_LIBRARY_MISMATCH:
        return "binding-library-mismatch";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED:
        return "binding-shape-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_INTRODUCTION_INVALID:
        return "binding-introduction-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_CENSUS_MISMATCH:
        return "binding-census-mismatch";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_DUPLICATE:
        return "binding-duplicate";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_DEFINITION_UNBOUND:
        return "item-definition-unbound";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID:
        return "item-source-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED:
        return "trait-shape-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED:
        return "item-shape-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED:
        return "item-attribute-projection-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED:
        return "reexport-attribute-projection-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED:
        return "value-shape-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_REQUIRED_ITEMS_MISSING:
        return "required-items-missing";
    case CM_HIR_DECL_CAPTURE_REASON_IDENTITY_UNSUPPORTED:
        return "identity-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_MODULE_METADATA_INVALID:
        return "module-metadata-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_METADATA_INVALID:
        return "item-metadata-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_TYPE_UNSUPPORTED:
        return "type-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_PREDICATE_UNSUPPORTED:
        return "predicate-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_TYPE_METADATA_INVALID:
        return "type-metadata-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_TARGET_UNMAPPED:
        return "namespace-target-unmapped";
    case CM_HIR_DECL_CAPTURE_REASON_METADATA_INVALID:
        return "metadata-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_CHANGED:
        return "authority-changed";
    }
    return "unknown";
}
