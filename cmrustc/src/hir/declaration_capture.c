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
    int source_is_generated;
    int is_import;
} CmDeclCaptureNamespace;

typedef struct CmDeclCaptureItem {
    const CmHirItem *item;
    CmHirItemId id;
    uint32_t owner_module;
    uint32_t source_ordinal;
    uint32_t local;
} CmDeclCaptureItem;

typedef struct CmDeclCaptureState {
    const CmHirDeclarationCaptureInput *input;
    const CmHirContext *hir;
    const CmHirCrate *crate_value;
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
                    == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR);
        if (entry_name != NULL && entry_value == value_namespace
            && cm_decl_bytes_equal(entry_name->bytes, entry_name->len,
                name, name_length)) {
            out->kind = entry->kind;
            out->definition = entry->target;
            out->type_kind = entry->type_kind;
            out->primitive_kind = entry->primitive_kind;
            out->value_kind = entry->value_kind;
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

static int cm_decl_namespace_target_shape(const CmResolvedBinding *binding,
    const CmHirLibraryBinding *target)
{
    if (target->kind == CM_HIR_LIBRARY_BINDING_MODULE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && binding->item_kind == CM_AST_ITEM_MODULE;
    if (target->kind == CM_HIR_LIBRARY_BINDING_TRAIT)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && binding->item_kind == CM_AST_ITEM_TRAIT;
    if (target->kind == CM_HIR_LIBRARY_BINDING_VALUE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
            && binding->item_kind == CM_AST_ITEM_FUNCTION
            && target->value_kind == CM_HIR_LIBRARY_VALUE_FUNCTION;
    if (target->kind == CM_HIR_LIBRARY_BINDING_TYPE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && (binding->item_kind == CM_AST_ITEM_STRUCT
                || binding->item_kind == CM_AST_ITEM_TYPE_ALIAS)
            && target->type_kind == (binding->item_kind == CM_AST_ITEM_STRUCT
                ? CM_HIR_TYPE_ADT_KIND : CM_HIR_TYPE_ALIAS_APPLICATION_KIND)
            && target->primitive_kind == CM_HIR_PRIMITIVE_NONE
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    if (target->kind == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
            && binding->item_kind == CM_AST_ITEM_STRUCT
            && target->type_kind == CM_HIR_TYPE_ADT_KIND
            && target->primitive_kind == CM_HIR_PRIMITIVE_NONE
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    return 0;
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
    CmAstItemKind expected_kind)
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
            && entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE
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

enum {
    CM_DECL_ATTR_STABLE = 1u << 0,
    CM_DECL_ATTR_UNSTABLE = 1u << 1,
    CM_DECL_ATTR_DEPRECATED = 1u << 2,
    CM_DECL_ATTR_DERIVE = 1u << 3,
    CM_DECL_ATTR_NON_EXHAUSTIVE = 1u << 4,
    CM_DECL_ATTR_ALLOW = 1u << 5
};

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
        if (module == NULL || import == NULL
            || cm_module_graph_get_effective_item(state->input->graph,
                state->input->revision, module->graph.id,
                entry->export_ordinal, &effective) != CM_RESOLVE_VIEW_OK
            || effective.is_generated
            || effective.item_kind != CM_AST_ITEM_USE
            || !cm_decl_item_ref_equal(effective.declaration,
                entry->introduced_by)
            || effective.attribute_count != entry->source_attribute_count
            || effective.attribute_count != import->attribute_count
            || ((import->attribute_count == 0u)
                != (import->attributes == NULL))) {
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED);
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
                    && kind != CM_DECL_ATTR_DEPRECATED
                    && kind != CM_DECL_ATTR_ALLOW)
                || (seen & kind) != 0u) {
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                    CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED);
            }
            for (duplicate_index = 0u;
                    duplicate_index < attribute_index; ++duplicate_index) {
                if (import->attributes[duplicate_index].span.source
                        == hir_attribute->span.source
                    && import->attributes[duplicate_index].source_attribute
                        == hir_attribute->source_attribute) {
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                        CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED);
                }
            }
            seen |= kind;
        }
        if (first) {
            if ((size_t)effective.attribute_count > SIZE_MAX
                    - state->projected_semantic_attribute_count) {
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
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

static int cm_decl_value_shape(const CmDeclCaptureState *state,
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
                    || !cm_decl_item_attribute_provenance(state, value.item,
                        CM_AST_ITEM_STRUCT)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_struct_source(state, value.item, non_exhaustive,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
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
                        CM_AST_ITEM_TYPE_ALIAS)) {
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
            if (cm_decl_item_already(state->values, state->value_count,
                    value.item->definition)) continue;
            if (!cm_decl_item_source(state, value.item->definition,
                    CM_AST_ITEM_FUNCTION, &value.owner_module,
                    &value.source_ordinal)) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                    entry, value.item, value.id);
                return 0;
            }
            if (!cm_decl_value_shape(state, value.item)) {
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

static int cm_decl_mark_type(CmDeclCaptureState *state, CmHirTypeId type_id,
    CmHirDeclarationCaptureResult *result)
{
    const CmHirType *type = cm_hir_get_type(state->hir, type_id);
    uint8_t primitive = cm_decl_primitive(type);
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
        wire->kind = capture->item->kind == CM_HIR_ITEM_STRUCT
            ? CM_HIR_DECL_ITEM_STRUCT : CM_HIR_DECL_ITEM_TYPE_ALIAS;
        wire->owner_module = capture->owner_module;
        wire->source_ordinal = capture->source_ordinal;
        wire->visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
        wire->visibility.restriction_module = 0u;
        if (!cm_decl_copy_intern(&wire->name,
                cm_decl_item_name(state, capture->item))) return 0;
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
    CM_DECL_FILL_OWNER(state->values, state->value_count, metadata->values,
        CM_HIR_DECL_GENERIC_VALUE);
#undef CM_DECL_FILL_OWNER
    return cursor == generic_count;
}

static int cm_decl_fill_types_values_predicates(CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata,
    CmHirDeclarationCaptureResult *result)
{
    size_t index;
    size_t predicate_count = 0u;
    size_t type_count = 0u;
    size_t cursor;
    state->generic_types = (unsigned char *)cm_alloc_zeroed(
        metadata->generic_count, sizeof(*state->generic_types));
    state->named_item_types = (unsigned char *)cm_alloc_zeroed(
        metadata->item_count, sizeof(*state->named_item_types));
    for (index = 0u; index < state->item_count; ++index) {
        const CmHirItem *item = state->items[index].item;
        if (item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && !cm_decl_mark_named_adt(state,
                item->data.type_alias_item.target, result)) return 0;
    }
    for (index = 0u; index < state->value_count; ++index) {
        const CmHirItem *item = state->values[index].item;
        const CmHirFunctionSignature *signature =
            &item->data.function_item.signature;
        uint32_t child;
        predicate_count += item->predicate_count;
        for (child = 0u; child < signature->parameter_count; ++child)
            if (!cm_decl_mark_type(state, signature->parameters[child].type,
                    result)) return 0;
        if (!cm_decl_mark_type(state, signature->return_type, result))
            return 0;
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
    for (index = 0u; index < state->item_count; ++index) {
        const CmHirItem *item = state->items[index].item;
        if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
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
        const CmHirFunctionSignature *signature =
            &item->data.function_item.signature;
        CmHirDeclarationValue *value = &metadata->values[index];
        uint32_t child;
        value->predicate_start = (uint32_t)(cursor + 1u);
        value->predicate_count = item->predicate_count;
        value->parameter_count = signature->parameter_count;
        value->parameter_types = signature->parameter_count == 0u ? NULL
            : (uint32_t *)cm_alloc_zeroed(signature->parameter_count,
                sizeof(*value->parameter_types));
        for (child = 0u; child < signature->parameter_count; ++child)
            value->parameter_types[child] = cm_decl_type_local(state,
                metadata, signature->parameters[child].type);
        value->return_type = cm_decl_type_local(state, metadata,
            signature->return_type);
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
