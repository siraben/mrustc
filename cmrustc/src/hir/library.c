#include "cm/hir/library.h"

#include "library_internal.h"

#include "cm/alloc.h"

#include <string.h>

typedef struct CmHirLibraryArtifactState {
    CmHirLibraryOwnedData owned;
    const CmHirContext *context;
    CmHirCrateId crate_id;
    CmHirDefId root_definition;
    char *extern_name;
    size_t public_type_entry_count;
    size_t public_value_entry_count;
} CmHirLibraryArtifactState;

static CmHirLibraryArtifactState *cm_hir_library_state(
    CmHirLibraryArtifact *artifact)
{
    return artifact == NULL ? NULL
        : (CmHirLibraryArtifactState *)artifact->state;
}

static const CmHirLibraryArtifactState *cm_hir_library_state_const(
    const CmHirLibraryArtifact *artifact)
{
    return artifact == NULL ? NULL
        : (const CmHirLibraryArtifactState *)artifact->state;
}

const CmHirLibraryOwnedData *cm_hir_library_artifact_owned_data_const(
    const CmHirLibraryArtifact *artifact)
{
    const CmHirLibraryArtifactState *state;

    state = cm_hir_library_state_const(artifact);
    return state == NULL || state->context == NULL
        ? NULL : &state->owned;
}

static void cm_hir_library_state_clear(CmHirLibraryArtifactState *state)
{
    if (state == NULL) return;
    cm_hir_library_owned_data_destroy(&state->owned);
    cm_free(state->extern_name);
    cm_hir_library_owned_data_init(&state->owned);
    state->context = NULL;
    state->crate_id = CM_HIR_CRATE_NONE;
    state->root_definition = cm_hir_def_id_none();
    state->extern_name = NULL;
    state->public_type_entry_count = 0u;
    state->public_value_entry_count = 0u;
}

void cm_hir_library_artifact_init(CmHirLibraryArtifact *artifact)
{
    CmHirLibraryArtifactState *state;

    if (artifact == NULL) return;
    state = (CmHirLibraryArtifactState *)cm_alloc_zeroed(1u,
        sizeof(*state));
    cm_hir_library_owned_data_init(&state->owned);
    artifact->state = state;
}

void cm_hir_library_artifact_destroy(CmHirLibraryArtifact *artifact)
{
    CmHirLibraryArtifactState *state;

    state = cm_hir_library_state(artifact);
    if (state == NULL) return;
    cm_hir_library_state_clear(state);
    cm_hir_library_owned_data_destroy(&state->owned);
    cm_free(state);
    artifact->state = NULL;
}

static int cm_hir_library_identifier_valid(const char *identifier)
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

static char *cm_hir_library_copy_c_str(const char *text)
{
    size_t length;
    char *copy;

    length = strlen(text);
    copy = (char *)cm_alloc(length + 1u);
    memcpy(copy, text, length + 1u);
    return copy;
}

void cm_hir_library_owned_data_init(CmHirLibraryOwnedData *data)
{
    if (data == NULL) return;
    cm_interner_init(&data->names, 4096u);
    cm_vec_init(&data->modules, sizeof(CmHirLibraryOwnedModule));
    cm_vec_init(&data->values, sizeof(CmHirLibraryOwnedValue));
}

void cm_hir_library_owned_data_destroy(CmHirLibraryOwnedData *data)
{
    size_t index;

    if (data == NULL) return;
    for (index = 0u; index < data->modules.len; ++index) {
        CmHirLibraryOwnedModule *module;

        module = (CmHirLibraryOwnedModule *)cm_vec_at(&data->modules,
            index);
        if (module != NULL) cm_vec_destroy(&module->entries);
    }
    for (index = 0u; index < data->values.len; ++index) {
        CmHirLibraryOwnedValue *value;

        value = (CmHirLibraryOwnedValue *)cm_vec_at(&data->values, index);
        if (value != NULL) cm_free(value->parameter_types);
    }
    cm_vec_destroy(&data->values);
    cm_vec_destroy(&data->modules);
    cm_interner_destroy(&data->names);
    memset(data, 0, sizeof(*data));
}

CmHirLibraryStatus cm_hir_library_owned_data_add_module(
    CmHirLibraryOwnedData *data, CmHirDefId definition,
    size_t *out_module_index)
{
    CmHirLibraryOwnedModule module;
    size_t index;

    if (out_module_index != NULL) *out_module_index = SIZE_MAX;
    if (data == NULL || out_module_index == NULL
        || data->modules.elem_size != sizeof(CmHirLibraryOwnedModule)
        || cm_hir_def_id_is_none(definition)) {
        return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    }
    for (index = 0u; index < data->modules.len; ++index) {
        const CmHirLibraryOwnedModule *existing;

        existing = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
            &data->modules, index);
        if (existing != NULL
            && cm_hir_def_id_equal(existing->definition, definition)) {
            return CM_HIR_LIBRARY_INVALID_HIR;
        }
    }
    memset(&module, 0, sizeof(module));
    module.definition = definition;
    cm_vec_init(&module.entries, sizeof(CmHirLibraryOwnedEntry));
    (void)cm_vec_push(&data->modules, &module);
    *out_module_index = data->modules.len - 1u;
    return CM_HIR_LIBRARY_OK;
}

static int cm_hir_library_binding_shape_valid(
    const CmHirLibraryBinding *binding)
{
    if (binding == NULL
        || (unsigned int)binding->kind
            > (unsigned int)CM_HIR_LIBRARY_BINDING_VALUE) return 0;
    if (binding->kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
        return cm_hir_def_id_is_none(binding->definition)
            && binding->primitive_kind != CM_HIR_PRIMITIVE_NONE
            && (unsigned int)binding->primitive_kind
                <= (unsigned int)CM_HIR_PRIMITIVE_F128
            && binding->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    }
    if (cm_hir_def_id_is_none(binding->definition)
        || binding->primitive_kind != CM_HIR_PRIMITIVE_NONE) return 0;
    if (binding->kind == CM_HIR_LIBRARY_BINDING_VALUE) {
        return binding->type_kind == CM_HIR_TYPE_ERROR_KIND
            && binding->value_kind >= CM_HIR_LIBRARY_VALUE_FUNCTION
            && binding->value_kind <= CM_HIR_LIBRARY_VALUE_STATIC;
    }
    if (binding->value_kind != CM_HIR_LIBRARY_VALUE_NONE) return 0;
    if (binding->kind == CM_HIR_LIBRARY_BINDING_TYPE) {
        return binding->type_kind == CM_HIR_TYPE_ADT_KIND
            || binding->type_kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
            || binding->type_kind == CM_HIR_TYPE_FOREIGN_KIND;
    }
    return binding->type_kind == CM_HIR_TYPE_ERROR_KIND;
}

CmHirLibraryStatus cm_hir_library_owned_data_add_entry(
    CmHirLibraryOwnedData *data, size_t module_index,
    const unsigned char *name, size_t name_length,
    const CmHirLibraryBinding *binding)
{
    CmHirLibraryOwnedModule *module;
    CmHirLibraryOwnedEntry entry;
    CmInternId copied_name;
    size_t index;

    if (data == NULL || data->modules.elem_size
            != sizeof(CmHirLibraryOwnedModule)
        || name == NULL || name_length == 0u
        || !cm_hir_library_binding_shape_valid(binding)) {
        return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    }
    module = (CmHirLibraryOwnedModule *)cm_vec_at(&data->modules,
        module_index);
    if (module == NULL
        || module->entries.elem_size != sizeof(CmHirLibraryOwnedEntry)) {
        return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    }
    copied_name = cm_interner_intern(&data->names, name, name_length);
    if (copied_name == CM_INTERN_ID_NONE)
        return CM_HIR_LIBRARY_INVALID_HIR;
    for (index = 0u; index < module->entries.len; ++index) {
        const CmHirLibraryOwnedEntry *existing;

        existing = (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
            &module->entries, index);
        if (existing != NULL && existing->name == copied_name
            && cm_hir_def_id_equal(existing->target, binding->definition)
            && existing->kind == binding->kind
            && existing->type_kind == binding->type_kind
            && existing->primitive_kind == binding->primitive_kind
            && existing->value_kind == binding->value_kind) {
            return CM_HIR_LIBRARY_OK;
        }
    }
    memset(&entry, 0, sizeof(entry));
    entry.name = copied_name;
    entry.target = binding->definition;
    entry.kind = binding->kind;
    entry.type_kind = binding->type_kind;
    entry.primitive_kind = binding->primitive_kind;
    entry.value_kind = binding->value_kind;
    (void)cm_vec_push(&module->entries, &entry);
    return CM_HIR_LIBRARY_OK;
}

static int cm_hir_library_value_equal(const CmHirLibraryValue *left,
    const CmHirLibraryValue *right)
{
    uint32_t index;

    if (left == NULL || right == NULL
        || !cm_hir_def_id_equal(left->definition, right->definition)
        || left->kind != right->kind) return 0;
    if (left->kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
        if (left->data.function.parameter_count
                != right->data.function.parameter_count
            || left->data.function.return_type
                != right->data.function.return_type
            || left->data.function.abi != right->data.function.abi
            || left->data.function.safety != right->data.function.safety
            || left->data.function.is_const != right->data.function.is_const
            || left->data.function.is_async != right->data.function.is_async
            || left->data.function.is_variadic
                != right->data.function.is_variadic) return 0;
        for (index = 0u; index < left->data.function.parameter_count;
                ++index) {
            if (left->data.function.parameter_types[index]
                    != right->data.function.parameter_types[index]) return 0;
        }
        return 1;
    }
    return left->data.value.type == right->data.value.type
        && left->data.value.mutability == right->data.value.mutability;
}

CmHirLibraryStatus cm_hir_library_owned_data_add_value(
    CmHirLibraryOwnedData *data, const CmHirLibraryValue *value)
{
    CmHirLibraryOwnedValue copy;
    size_t index;

    if (data == NULL || value == NULL
        || data->values.elem_size != sizeof(CmHirLibraryOwnedValue)
        || cm_hir_def_id_is_none(value->definition)
        || value->kind < CM_HIR_LIBRARY_VALUE_FUNCTION
        || value->kind > CM_HIR_LIBRARY_VALUE_STATIC) {
        return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    }
    if (value->kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
        if (value->data.function.return_type == CM_HIR_TYPE_NONE
            || (value->data.function.parameter_count != 0u
                && value->data.function.parameter_types == NULL)) {
            return CM_HIR_LIBRARY_INVALID_ARGUMENT;
        }
    } else if (value->data.value.type == CM_HIR_TYPE_NONE
        || (unsigned int)value->data.value.mutability
            > (unsigned int)CM_HIR_MUTABLE
        || (value->kind == CM_HIR_LIBRARY_VALUE_CONST
            && value->data.value.mutability != CM_HIR_IMMUTABLE)) {
        return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    }
    for (index = 0u; index < data->values.len; ++index) {
        const CmHirLibraryOwnedValue *existing;

        existing = (const CmHirLibraryOwnedValue *)cm_vec_at_const(
            &data->values, index);
        if (existing == NULL
            || !cm_hir_def_id_equal(existing->declaration.definition,
                value->definition)) continue;
        return cm_hir_library_value_equal(&existing->declaration, value)
            ? CM_HIR_LIBRARY_OK : CM_HIR_LIBRARY_INVALID_HIR;
    }
    memset(&copy, 0, sizeof(copy));
    copy.declaration = *value;
    if (value->kind == CM_HIR_LIBRARY_VALUE_FUNCTION
        && value->data.function.parameter_count != 0u) {
        size_t byte_count;

        byte_count = (size_t)value->data.function.parameter_count
            * sizeof(CmHirTypeId);
        copy.parameter_types = (CmHirTypeId *)cm_alloc(byte_count);
        memcpy(copy.parameter_types, value->data.function.parameter_types,
            byte_count);
        copy.declaration.data.function.parameter_types =
            copy.parameter_types;
    }
    (void)cm_vec_push(&data->values, &copy);
    return CM_HIR_LIBRARY_OK;
}

static CmHirLibraryOwnedModule *cm_hir_library_find_graph_module(
    CmHirLibraryArtifactState *state, CmModuleId graph_module)
{
    size_t index;

    for (index = 0u; index < state->owned.modules.len; ++index) {
        CmHirLibraryOwnedModule *module;

        module = (CmHirLibraryOwnedModule *)cm_vec_at(
            &state->owned.modules, index);
        if (module != NULL && module->capture_graph_module == graph_module)
            return module;
    }
    return NULL;
}

static const CmHirLibraryOwnedModule *cm_hir_library_find_definition_module(
    const CmHirLibraryArtifactState *state, CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < state->owned.modules.len; ++index) {
        const CmHirLibraryOwnedModule *module;

        module = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
            &state->owned.modules, index);
        if (module != NULL
            && cm_hir_def_id_equal(module->definition, definition)) {
            return module;
        }
    }
    return NULL;
}

static const CmHirLibraryOwnedValue *cm_hir_library_find_value(
    const CmHirLibraryArtifactState *state, CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < state->owned.values.len; ++index) {
        const CmHirLibraryOwnedValue *value;

        value = (const CmHirLibraryOwnedValue *)cm_vec_at_const(
            &state->owned.values, index);
        if (value != NULL && cm_hir_def_id_equal(
                value->declaration.definition, definition)) return value;
    }
    return NULL;
}

static CmHirLibraryValueKind cm_hir_library_value_kind(
    CmHirItemKind item_kind)
{
    switch (item_kind) {
    case CM_HIR_ITEM_FUNCTION: return CM_HIR_LIBRARY_VALUE_FUNCTION;
    case CM_HIR_ITEM_CONST: return CM_HIR_LIBRARY_VALUE_CONST;
    case CM_HIR_ITEM_STATIC: return CM_HIR_LIBRARY_VALUE_STATIC;
    default: return CM_HIR_LIBRARY_VALUE_NONE;
    }
}

static int cm_hir_library_item_binding_kind(CmHirItemKind item_kind,
    CmHirLibraryBindingKind *out_binding_kind,
    CmHirTypeKind *out_type_kind,
    CmHirLibraryValueKind *out_value_kind)
{
    *out_value_kind = CM_HIR_LIBRARY_VALUE_NONE;
    switch (item_kind) {
    case CM_HIR_ITEM_STRUCT:
    case CM_HIR_ITEM_UNION:
    case CM_HIR_ITEM_ENUM:
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TYPE;
        *out_type_kind = CM_HIR_TYPE_ADT_KIND;
        return 1;
    case CM_HIR_ITEM_TYPE_ALIAS:
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TYPE;
        *out_type_kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
        return 1;
    case CM_HIR_ITEM_EXTERN_TYPE:
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TYPE;
        *out_type_kind = CM_HIR_TYPE_FOREIGN_KIND;
        return 1;
    case CM_HIR_ITEM_TRAIT:
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TRAIT;
        *out_type_kind = CM_HIR_TYPE_ERROR_KIND;
        return 1;
    case CM_HIR_ITEM_FUNCTION:
    case CM_HIR_ITEM_CONST:
    case CM_HIR_ITEM_STATIC:
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_VALUE;
        *out_type_kind = CM_HIR_TYPE_ERROR_KIND;
        *out_value_kind = cm_hir_library_value_kind(item_kind);
        return 1;
    default:
        return 0;
    }
}

static int cm_hir_library_ast_item_binding_kind(CmAstItemKind item_kind,
    CmHirItemKind *out_item_kind,
    CmHirLibraryBindingKind *out_binding_kind,
    CmHirTypeKind *out_type_kind,
    CmHirLibraryValueKind *out_value_kind)
{
    *out_value_kind = CM_HIR_LIBRARY_VALUE_NONE;
    switch (item_kind) {
    case CM_AST_ITEM_FUNCTION:
        *out_item_kind = CM_HIR_ITEM_FUNCTION;
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_VALUE;
        *out_type_kind = CM_HIR_TYPE_ERROR_KIND;
        *out_value_kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
        return 1;
    case CM_AST_ITEM_STRUCT:
        *out_item_kind = CM_HIR_ITEM_STRUCT;
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TYPE;
        *out_type_kind = CM_HIR_TYPE_ADT_KIND;
        return 1;
    case CM_AST_ITEM_UNION:
        *out_item_kind = CM_HIR_ITEM_UNION;
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TYPE;
        *out_type_kind = CM_HIR_TYPE_ADT_KIND;
        return 1;
    case CM_AST_ITEM_ENUM:
        *out_item_kind = CM_HIR_ITEM_ENUM;
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TYPE;
        *out_type_kind = CM_HIR_TYPE_ADT_KIND;
        return 1;
    case CM_AST_ITEM_TYPE_ALIAS:
        *out_item_kind = CM_HIR_ITEM_TYPE_ALIAS;
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TYPE;
        *out_type_kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
        return 1;
    case CM_AST_ITEM_TRAIT:
        *out_item_kind = CM_HIR_ITEM_TRAIT;
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_TRAIT;
        *out_type_kind = CM_HIR_TYPE_ERROR_KIND;
        return 1;
    case CM_AST_ITEM_CONST:
        *out_item_kind = CM_HIR_ITEM_CONST;
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_VALUE;
        *out_type_kind = CM_HIR_TYPE_ERROR_KIND;
        *out_value_kind = CM_HIR_LIBRARY_VALUE_CONST;
        return 1;
    case CM_AST_ITEM_STATIC:
        *out_item_kind = CM_HIR_ITEM_STATIC;
        *out_binding_kind = CM_HIR_LIBRARY_BINDING_VALUE;
        *out_type_kind = CM_HIR_TYPE_ERROR_KIND;
        *out_value_kind = CM_HIR_LIBRARY_VALUE_STATIC;
        return 1;
    default:
        return 0;
    }
}

static int cm_hir_library_names_equal(const CmHirLibraryArtifactState *state,
    CmInternId artifact_name, const CmHirContext *context,
    CmInternId hir_name)
{
    const CmInternedString *left;
    const CmInternedString *right;

    left = cm_interner_get(&state->owned.names, artifact_name);
    right = cm_interner_get(&context->strings, hir_name);
    return left != NULL && right != NULL && left->len == right->len
        && memcmp(left->bytes, right->bytes, left->len) == 0;
}

static CmInternId cm_hir_library_copy_graph_name(
    CmHirLibraryArtifactState *state, const CmModuleGraph *graph,
    CmResolveStringId name)
{
    size_t length;
    char *buffer;
    CmInternId copied;

    length = cm_module_graph_string_length(graph, name);
    if (length == 0u || length == SIZE_MAX) return CM_INTERN_ID_NONE;
    buffer = (char *)cm_alloc(length + 1u);
    if (!cm_module_graph_copy_string(graph, name, buffer, length + 1u)) {
        cm_free(buffer);
        return CM_INTERN_ID_NONE;
    }
    copied = cm_interner_intern(&state->owned.names, buffer, length);
    cm_free(buffer);
    return copied;
}

static CmInternId cm_hir_library_copy_hir_name(
    CmHirLibraryArtifactState *state, const CmHirContext *context,
    CmInternId name)
{
    const CmInternedString *text;

    text = cm_interner_get(&context->strings, name);
    if (text == NULL || text->len == 0u) return CM_INTERN_ID_NONE;
    return cm_interner_intern(&state->owned.names, text->bytes, text->len);
}

static int cm_hir_library_add_entry(CmHirLibraryArtifactState *state,
    CmHirLibraryOwnedModule *module, CmInternId name, CmHirDefId target,
    CmHirLibraryBindingKind kind, CmHirTypeKind type_kind,
    CmHirPrimitiveKind primitive_kind, CmHirLibraryValueKind value_kind)
{
    size_t index;
    CmHirLibraryOwnedEntry entry;

    if (module == NULL || name == CM_INTERN_ID_NONE
        || (kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE
            ? (!cm_hir_def_id_is_none(target)
                || primitive_kind == CM_HIR_PRIMITIVE_NONE
                || (unsigned int)primitive_kind
                    > (unsigned int)CM_HIR_PRIMITIVE_F128
                || value_kind != CM_HIR_LIBRARY_VALUE_NONE)
            : (cm_hir_def_id_is_none(target)
                || primitive_kind != CM_HIR_PRIMITIVE_NONE))
        || (kind == CM_HIR_LIBRARY_BINDING_VALUE
            ? (value_kind < CM_HIR_LIBRARY_VALUE_FUNCTION
                || value_kind > CM_HIR_LIBRARY_VALUE_STATIC)
            : value_kind != CM_HIR_LIBRARY_VALUE_NONE)) return 0;
    for (index = 0u; index < module->entries.len; ++index) {
        const CmHirLibraryOwnedEntry *existing;

        existing = (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
            &module->entries, index);
        if (existing != NULL && existing->name == name
            && cm_hir_def_id_equal(existing->target, target)
            && existing->kind == kind
            && (kind != CM_HIR_LIBRARY_BINDING_TYPE
                || existing->type_kind == type_kind)
            && (kind != CM_HIR_LIBRARY_BINDING_PRIMITIVE
                || existing->primitive_kind == primitive_kind)
            && (kind != CM_HIR_LIBRARY_BINDING_VALUE
                || existing->value_kind == value_kind)) return 1;
    }
    memset(&entry, 0, sizeof(entry));
    entry.name = name;
    entry.target = target;
    entry.kind = kind;
    entry.type_kind = type_kind;
    entry.primitive_kind = primitive_kind;
    entry.value_kind = value_kind;
    (void)cm_vec_push(&module->entries, &entry);
    if (kind == CM_HIR_LIBRARY_BINDING_VALUE)
        state->public_value_entry_count += 1u;
    else if (kind != CM_HIR_LIBRARY_BINDING_MODULE)
        state->public_type_entry_count += 1u;
    return 1;
}

static int cm_hir_library_add_value_from_item(
    CmHirLibraryArtifactState *state, const CmHirItem *item)
{
    CmHirLibraryValue value;
    CmHirTypeId *parameter_types;
    uint32_t index;
    CmHirLibraryStatus status;

    if (state == NULL || item == NULL
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->generic_parameter_count != 0u
        || item->predicate_scope_count != 0u
        || item->predicate_count != 0u
        || item->outlives_predicate_count != 0u) return 0;
    memset(&value, 0, sizeof(value));
    value.definition = item->definition;
    value.kind = cm_hir_library_value_kind(item->kind);
    if (value.kind == CM_HIR_LIBRARY_VALUE_NONE) return 0;
    parameter_types = NULL;
    if (value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
        const CmHirFunctionSignature *signature;

        signature = &item->data.function_item.signature;
        if (signature->receiver != CM_HIR_RECEIVER_NONE
            || (signature->parameter_count != 0u
                && signature->parameters == NULL)) return 0;
        if (signature->parameter_count != 0u) {
            parameter_types = (CmHirTypeId *)cm_alloc(
                (size_t)signature->parameter_count * sizeof(CmHirTypeId));
            for (index = 0u; index < signature->parameter_count; ++index)
                parameter_types[index] = signature->parameters[index].type;
        }
        value.data.function.parameter_types = parameter_types;
        value.data.function.parameter_count = signature->parameter_count;
        value.data.function.return_type = signature->return_type;
        value.data.function.abi = signature->abi;
        value.data.function.safety = signature->safety;
        value.data.function.is_const = signature->is_const;
        value.data.function.is_async = signature->is_async;
        value.data.function.is_variadic = signature->is_variadic;
    } else {
        value.data.value.type = item->data.value_item.type;
        value.data.value.mutability = item->data.value_item.mutability;
    }
    status = cm_hir_library_owned_data_add_value(&state->owned, &value);
    cm_free(parameter_types);
    return status == CM_HIR_LIBRARY_OK;
}

static int cm_hir_library_value_type_valid(const CmHirContext *context,
    CmHirTypeId type)
{
    return type != CM_HIR_TYPE_NONE && cm_hir_get_type(context, type) != NULL;
}

static int cm_hir_library_value_shape_equal(const CmHirLibraryValue *value,
    const CmHirItem *item)
{
    uint32_t index;

    if (value == NULL || item == NULL
        || value->kind != cm_hir_library_value_kind(item->kind)
        || !cm_hir_def_id_equal(value->definition, item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->generic_parameter_count != 0u
        || item->predicate_scope_count != 0u
        || item->predicate_count != 0u
        || item->outlives_predicate_count != 0u) return 0;
    if (value->kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
        const CmHirFunctionSignature *signature;

        signature = &item->data.function_item.signature;
        if (signature->receiver != CM_HIR_RECEIVER_NONE
            || value->data.function.parameter_count
                != signature->parameter_count
            || value->data.function.return_type != signature->return_type
            || value->data.function.abi != signature->abi
            || value->data.function.safety != signature->safety
            || value->data.function.is_const != signature->is_const
            || value->data.function.is_async != signature->is_async
            || value->data.function.is_variadic != signature->is_variadic) {
            return 0;
        }
        for (index = 0u; index < signature->parameter_count; ++index) {
            if (value->data.function.parameter_types[index]
                != signature->parameters[index].type) return 0;
        }
        return 1;
    }
    return value->data.value.type == item->data.value_item.type
        && value->data.value.mutability == item->data.value_item.mutability;
}

static int cm_hir_library_owned_value_valid(
    const CmHirLibraryArtifactState *state,
    const CmHirLibraryOwnedValue *owned_value)
{
    const CmHirLibraryValue *value;
    const CmHirDefinition *definition;
    CmHirItemKind expected_kind;
    uint32_t index;

    if (state == NULL || owned_value == NULL) return 0;
    value = &owned_value->declaration;
    if (value->definition.crate_id != state->crate_id) return 0;
    expected_kind = CM_HIR_ITEM_FUNCTION;
    switch (value->kind) {
    case CM_HIR_LIBRARY_VALUE_FUNCTION:
        expected_kind = CM_HIR_ITEM_FUNCTION;
        if (!cm_hir_library_value_type_valid(state->context,
                value->data.function.return_type)
            || (value->data.function.parameter_count != 0u
                && (owned_value->parameter_types == NULL
                    || value->data.function.parameter_types
                        != owned_value->parameter_types))
            || cm_interner_get(&state->context->strings,
                value->data.function.abi) == NULL
            || (unsigned int)value->data.function.safety
                > (unsigned int)CM_HIR_UNSAFE
            || (value->data.function.is_const != 0
                && value->data.function.is_const != 1)
            || (value->data.function.is_async != 0
                && value->data.function.is_async != 1)
            || (value->data.function.is_variadic != 0
                && value->data.function.is_variadic != 1)) return 0;
        for (index = 0u; index < value->data.function.parameter_count;
                ++index) {
            if (!cm_hir_library_value_type_valid(state->context,
                    value->data.function.parameter_types[index])) return 0;
        }
        break;
    case CM_HIR_LIBRARY_VALUE_CONST:
        expected_kind = CM_HIR_ITEM_CONST;
        if (value->data.value.mutability != CM_HIR_IMMUTABLE
            || !cm_hir_library_value_type_valid(state->context,
                value->data.value.type)) return 0;
        break;
    case CM_HIR_LIBRARY_VALUE_STATIC:
        expected_kind = CM_HIR_ITEM_STATIC;
        if ((unsigned int)value->data.value.mutability
                > (unsigned int)CM_HIR_MUTABLE
            || !cm_hir_library_value_type_valid(state->context,
                value->data.value.type)) return 0;
        break;
    case CM_HIR_LIBRARY_VALUE_NONE:
        return 0;
    }
    definition = cm_hir_lookup_definition(state->context,
        value->definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || !definition->has_reserved_item_kind
        || definition->reserved_item_kind != expected_kind) return 0;
    if (definition->state == CM_HIR_DEFINITION_RESERVED) return 1;
    if (definition->state == CM_HIR_DEFINITION_BOUND) {
        const CmHirItem *item;

        item = cm_hir_get_item(state->context, definition->entity.item_id);
        return cm_hir_library_value_shape_equal(value, item);
    }
    return 0;
}

static int cm_hir_library_definition_valid(
    const CmHirLibraryArtifactState *state, CmHirDefId definition,
    CmHirLibraryBindingKind kind, CmHirTypeKind type_kind)
{
    const CmHirDefinition *resolved;

    if (state->context == NULL || definition.crate_id != state->crate_id)
        return 0;
    resolved = cm_hir_lookup_definition(state->context, definition);
    if (resolved == NULL || resolved->state != CM_HIR_DEFINITION_BOUND)
        return 0;
    if (kind == CM_HIR_LIBRARY_BINDING_MODULE) {
        return resolved->kind == CM_HIR_DEFINITION_MODULE
            && cm_hir_get_module(state->context,
                resolved->entity.module_id) != NULL;
    }
    if (resolved->kind == CM_HIR_DEFINITION_ITEM) {
        const CmHirItem *item;
        CmHirLibraryBindingKind actual_binding_kind;
        CmHirTypeKind actual_kind;
        CmHirLibraryValueKind actual_value_kind;

        item = cm_hir_get_item(state->context, resolved->entity.item_id);
        return item != NULL
            && cm_hir_library_item_binding_kind(item->kind,
                &actual_binding_kind, &actual_kind, &actual_value_kind)
            && actual_binding_kind == kind
            && (kind != CM_HIR_LIBRARY_BINDING_TYPE
                || actual_kind == type_kind);
    }
    return 0;
}

static int cm_hir_library_owned_module_present(
    const CmHirLibraryArtifactState *state, CmHirDefId definition)
{
    return cm_hir_library_find_definition_module(state, definition) != NULL;
}

static int cm_hir_library_entry_is_value_namespace(
    const CmHirLibraryOwnedEntry *entry)
{
    return entry != NULL
        && entry->kind == CM_HIR_LIBRARY_BINDING_VALUE;
}

static int cm_hir_library_owned_data_validate(
    const CmHirLibraryArtifactState *candidate,
    CmHirDefId root_definition, size_t *out_public_type_entry_count,
    size_t *out_public_value_entry_count)
{
    size_t module_index;
    size_t root_count;
    size_t public_type_entry_count;
    size_t public_value_entry_count;
    size_t value_index;

    if (out_public_type_entry_count != NULL)
        *out_public_type_entry_count = 0u;
    if (out_public_value_entry_count != NULL)
        *out_public_value_entry_count = 0u;
    if (candidate == NULL || candidate->context == NULL
        || candidate->crate_id == CM_HIR_CRATE_NONE
        || candidate->owned.modules.elem_size
            != sizeof(CmHirLibraryOwnedModule)
        || candidate->owned.values.elem_size
            != sizeof(CmHirLibraryOwnedValue)
        || candidate->owned.modules.len == 0u
        || out_public_type_entry_count == NULL
        || out_public_value_entry_count == NULL) return 0;
    root_count = 0u;
    public_type_entry_count = 0u;
    public_value_entry_count = 0u;
    for (value_index = 0u; value_index < candidate->owned.values.len;
            ++value_index) {
        const CmHirLibraryOwnedValue *value;

        value = (const CmHirLibraryOwnedValue *)cm_vec_at_const(
            &candidate->owned.values, value_index);
        if (!cm_hir_library_owned_value_valid(candidate, value)) return 0;
    }
    for (module_index = 0u;
            module_index < candidate->owned.modules.len; ++module_index) {
        const CmHirLibraryOwnedModule *module;
        const CmHirDefinition *definition;
        size_t entry_index;

        module = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
            &candidate->owned.modules, module_index);
        if (module == NULL
            || module->entries.elem_size != sizeof(CmHirLibraryOwnedEntry)
            || !cm_hir_library_definition_valid(candidate,
                module->definition, CM_HIR_LIBRARY_BINDING_MODULE,
                CM_HIR_TYPE_ERROR_KIND)) return 0;
        definition = cm_hir_lookup_definition(candidate->context,
            module->definition);
        if (definition == NULL) return 0;
        if (module->capture_hir_module != CM_HIR_MODULE_NONE
            && module->capture_hir_module != definition->entity.module_id) {
            return 0;
        }
        if (cm_hir_def_id_equal(module->definition, root_definition))
            root_count += 1u;
        for (entry_index = 0u; entry_index < module->entries.len;
                ++entry_index) {
            const CmHirLibraryOwnedEntry *entry;
            const CmInternedString *name;
            CmHirLibraryBinding binding;
            size_t prior_index;

            entry = (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
                &module->entries, entry_index);
            name = entry == NULL ? NULL
                : cm_interner_get(&candidate->owned.names, entry->name);
            if (entry == NULL || name == NULL || name->len == 0u) return 0;
            memset(&binding, 0, sizeof(binding));
            binding.kind = entry->kind;
            binding.definition = entry->target;
            binding.type_kind = entry->type_kind;
            binding.primitive_kind = entry->primitive_kind;
            binding.value_kind = entry->value_kind;
            if (!cm_hir_library_binding_shape_valid(&binding)) return 0;
            if (entry->kind == CM_HIR_LIBRARY_BINDING_MODULE) {
                if (!cm_hir_library_owned_module_present(candidate,
                        entry->target)) return 0;
            } else if (entry->kind == CM_HIR_LIBRARY_BINDING_VALUE) {
                const CmHirLibraryOwnedValue *value;

                value = cm_hir_library_find_value(candidate, entry->target);
                if (value == NULL
                    || value->declaration.kind != entry->value_kind) {
                    return 0;
                }
            } else if (entry->kind != CM_HIR_LIBRARY_BINDING_PRIMITIVE
                && !cm_hir_library_definition_valid(candidate, entry->target,
                    entry->kind, entry->type_kind)) {
                return 0;
            }
            for (prior_index = 0u; prior_index < entry_index;
                    ++prior_index) {
                const CmHirLibraryOwnedEntry *prior;

                prior = (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
                    &module->entries, prior_index);
                if (prior != NULL && prior->name == entry->name
                    && cm_hir_library_entry_is_value_namespace(prior)
                        == cm_hir_library_entry_is_value_namespace(entry)
                    && (!cm_hir_def_id_equal(prior->target, entry->target)
                        || prior->kind != entry->kind
                        || prior->type_kind != entry->type_kind
                        || prior->primitive_kind != entry->primitive_kind
                        || prior->value_kind
                            != entry->value_kind)) return 0;
            }
            if (entry->kind == CM_HIR_LIBRARY_BINDING_VALUE)
                public_value_entry_count += 1u;
            else if (entry->kind != CM_HIR_LIBRARY_BINDING_MODULE)
                public_type_entry_count += 1u;
        }
    }
    if (root_count != 1u) return 0;
    for (value_index = 0u; value_index < candidate->owned.values.len;
            ++value_index) {
        const CmHirLibraryOwnedValue *value;
        int referenced;

        value = (const CmHirLibraryOwnedValue *)cm_vec_at_const(
            &candidate->owned.values, value_index);
        referenced = 0;
        for (module_index = 0u;
                module_index < candidate->owned.modules.len; ++module_index) {
            const CmHirLibraryOwnedModule *module;
            size_t entry_index;

            module = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
                &candidate->owned.modules, module_index);
            if (module == NULL || value == NULL) return 0;
            for (entry_index = 0u; entry_index < module->entries.len;
                    ++entry_index) {
                const CmHirLibraryOwnedEntry *entry;

                entry = (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
                    &module->entries, entry_index);
                if (entry != NULL
                    && entry->kind == CM_HIR_LIBRARY_BINDING_VALUE
                    && cm_hir_def_id_equal(entry->target,
                        value->declaration.definition)) referenced = 1;
            }
        }
        if (!referenced) return 0;
    }
    *out_public_type_entry_count = public_type_entry_count;
    *out_public_value_entry_count = public_value_entry_count;
    return 1;
}

CmHirLibraryArtifactResult cm_hir_library_artifact_restore_owned(
    CmHirLibraryArtifact *artifact, const CmHirContext *context,
    CmHirCrateId crate_id, CmHirDefId root_definition,
    const char *extern_name, CmHirLibraryOwnedData *data)
{
    CmHirLibraryArtifactResult result;
    CmHirLibraryArtifactState *state;
    CmHirLibraryArtifactState candidate;
    const CmHirCrate *crate_value;
    const CmHirModule *root_module;
    CmHirLibraryOwnedData previous_owned;
    char *new_extern_name;
    char *previous_extern_name;
    size_t public_type_entry_count;
    size_t public_value_entry_count;

    memset(&result, 0, sizeof(result));
    result.status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
    state = cm_hir_library_state(artifact);
    if (state == NULL || context == NULL || data == NULL
        || crate_id == CM_HIR_CRATE_NONE
        || root_definition.crate_id != crate_id
        || !cm_hir_library_identifier_valid(extern_name)) return result;
    crate_value = cm_hir_get_crate(context, crate_id);
    root_module = crate_value == NULL ? NULL
        : cm_hir_get_module(context, crate_value->root_module);
    if (root_module == NULL || root_module->crate_id != crate_id
        || !cm_hir_def_id_equal(root_module->definition,
            root_definition)) {
        result.status = CM_HIR_LIBRARY_INVALID_HIR;
        return result;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.owned = *data;
    candidate.context = context;
    candidate.crate_id = crate_id;
    candidate.root_definition = root_definition;
    if (!cm_hir_library_owned_data_validate(&candidate, root_definition,
            &public_type_entry_count, &public_value_entry_count)) {
        result.status = CM_HIR_LIBRARY_INVALID_HIR;
        return result;
    }

    /* No validation or allocation may fail after this point. */
    new_extern_name = cm_hir_library_copy_c_str(extern_name);
    previous_owned = state->owned;
    previous_extern_name = state->extern_name;
    state->owned = *data;
    state->context = context;
    state->crate_id = crate_id;
    state->root_definition = root_definition;
    state->extern_name = new_extern_name;
    state->public_type_entry_count = public_type_entry_count;
    state->public_value_entry_count = public_value_entry_count;
    memset(data, 0, sizeof(*data));
    cm_hir_library_owned_data_init(data);
    cm_hir_library_owned_data_destroy(&previous_owned);
    cm_free(previous_extern_name);

    result.status = CM_HIR_LIBRARY_OK;
    result.module_count = state->owned.modules.len;
    result.public_type_entry_count = public_type_entry_count;
    result.public_value_entry_count = public_value_entry_count;
    return result;
}

static int cm_hir_library_add_direct_entry(
    CmHirLibraryArtifactState *state, const CmModuleGraph *graph,
    const CmHirModuleMap *modules, CmHirLibraryOwnedModule *artifact_module,
    const CmResolveModuleInfo *graph_information,
    const CmResolveNamespaceEntry *entry)
{
    CmInternId name;

    name = cm_hir_library_copy_graph_name(state, graph, entry->name);
    if (name == CM_INTERN_ID_NONE) return 0;
    if (entry->item_kind == CM_AST_ITEM_MODULE) {
        uint32_t child_index;
        CmModuleId matched_child;
        uint32_t matches;
        CmHirModuleId child_hir;
        const CmHirModule *child_module;

        matched_child = CM_MODULE_NONE;
        matches = 0u;
        for (child_index = 0u; child_index < graph_information->child_count;
                ++child_index) {
            CmModuleId child;
            CmResolveModuleInfo child_information;

            if (!cm_module_graph_get_child(graph, graph_information->id,
                    child_index, &child)
                || !cm_module_graph_get_module(graph, child,
                    &child_information)) return 0;
            if (child_information.declaration.source
                    == entry->declaration.source
                && child_information.declaration.item
                    == entry->declaration.item) {
                matched_child = child;
                matches += 1u;
            }
        }
        if (matches != 1u
            || cm_hir_module_map_lookup_hir(modules, graph,
                cm_module_graph_revision(graph), matched_child,
                state->context, &child_hir) != CM_HIR_MODULE_MAP_OK
            || (child_module = cm_hir_get_module(state->context,
                child_hir)) == NULL
            || child_module->crate_id != state->crate_id) return 0;
        return cm_hir_library_add_entry(state, artifact_module, name,
            child_module->definition, CM_HIR_LIBRARY_BINDING_MODULE,
            CM_HIR_TYPE_ERROR_KIND, CM_HIR_PRIMITIVE_NONE,
            CM_HIR_LIBRARY_VALUE_NONE);
    }
    {
        CmHirItemKind hir_item_kind;
        CmHirLibraryBindingKind binding_kind;
        CmHirTypeKind type_kind;
        CmHirLibraryValueKind value_kind;
        const CmHirItem *matched_item;
        size_t item_index;
        size_t matches;

        if (!cm_hir_library_ast_item_binding_kind(entry->item_kind,
                &hir_item_kind, &binding_kind, &type_kind,
                &value_kind)) return 1;
        matched_item = NULL;
        matches = 0u;
        for (item_index = 0u; item_index < state->context->items.len;
                ++item_index) {
            const CmHirItem *item;

            item = (const CmHirItem *)cm_vec_at_const(
                &state->context->items, item_index);
            if (item != NULL && item->kind == hir_item_kind
                && item->owner_module == artifact_module->capture_hir_module
                && cm_hir_def_id_is_none(item->parent_definition)
                && item->definition.crate_id == state->crate_id
                && item->visibility.kind == CM_HIR_VIS_PUBLIC
                && cm_hir_library_names_equal(state, name,
                    state->context, item->name)) {
                matched_item = item;
                matches += 1u;
            }
        }
        if (matches != 1u || matched_item == NULL
            || !cm_hir_library_definition_valid(state,
                matched_item->definition, binding_kind, type_kind)) return 0;
        if (binding_kind == CM_HIR_LIBRARY_BINDING_VALUE
            && !cm_hir_library_add_value_from_item(state, matched_item)) {
            return 0;
        }
        return cm_hir_library_add_entry(state, artifact_module, name,
            matched_item->definition, binding_kind, type_kind,
            CM_HIR_PRIMITIVE_NONE, value_kind);
    }
}

static int cm_hir_library_add_public_imports(
    CmHirLibraryArtifactState *state,
    CmHirLibraryOwnedModule *artifact_module, int include_values)
{
    const CmHirModule *module;
    uint32_t import_index;

    module = cm_hir_get_module(state->context,
        artifact_module->capture_hir_module);
    if (module == NULL) return 0;
    for (import_index = 0u; import_index < module->import_count;
            ++import_index) {
        const CmHirImport *import;
        uint32_t binding_index;

        import = &module->imports[import_index];
        if (import->kind != CM_HIR_IMPORT_USE
            || import->visibility.kind != CM_HIR_VIS_PUBLIC) continue;
        for (binding_index = 0u; binding_index < import->binding_count;
                ++binding_index) {
            const CmHirImportBinding *binding;
            const CmHirDefinition *definition;
            CmInternId name;

            binding = &import->bindings[binding_index];
            if ((binding->namespace_kind != CM_HIR_NAMESPACE_TYPE
                    && (!include_values
                        || binding->namespace_kind
                            != CM_HIR_NAMESPACE_VALUE))
                || binding->is_anonymous) continue;
            name = cm_hir_library_copy_hir_name(state, state->context,
                binding->name);
            if (binding->primitive_kind != CM_HIR_PRIMITIVE_NONE) {
                if (name == CM_INTERN_ID_NONE
                    || !cm_hir_library_add_entry(state, artifact_module,
                        name, cm_hir_def_id_none(),
                        CM_HIR_LIBRARY_BINDING_PRIMITIVE,
                        CM_HIR_TYPE_ERROR_KIND,
                        binding->primitive_kind,
                        CM_HIR_LIBRARY_VALUE_NONE)) return 0;
                continue;
            }
            if (binding->target.crate_id != state->crate_id) continue;
            definition = cm_hir_lookup_definition(state->context,
                binding->target);
            if (definition == NULL
                || definition->state != CM_HIR_DEFINITION_BOUND
                || name == CM_INTERN_ID_NONE) return 0;
            if (definition->kind == CM_HIR_DEFINITION_MODULE) {
                if (cm_hir_library_find_definition_module(state,
                        binding->target) == NULL
                    || !cm_hir_library_definition_valid(state,
                        binding->target, CM_HIR_LIBRARY_BINDING_MODULE,
                        CM_HIR_TYPE_ERROR_KIND)
                    || !cm_hir_library_add_entry(state, artifact_module,
                        name, binding->target,
                        CM_HIR_LIBRARY_BINDING_MODULE,
                        CM_HIR_TYPE_ERROR_KIND,
                        CM_HIR_PRIMITIVE_NONE,
                        CM_HIR_LIBRARY_VALUE_NONE)) return 0;
            } else if (definition->kind == CM_HIR_DEFINITION_ITEM) {
                const CmHirItem *item;
                CmHirLibraryBindingKind binding_kind;
                CmHirTypeKind type_kind;
                CmHirLibraryValueKind value_kind;

                item = cm_hir_get_item(state->context,
                    definition->entity.item_id);
                if (item == NULL) return 0;
                if (cm_hir_library_item_binding_kind(item->kind,
                        &binding_kind, &type_kind, &value_kind)
                    && (binding_kind != CM_HIR_LIBRARY_BINDING_VALUE
                        || include_values)
                    && (!cm_hir_library_definition_valid(state,
                            binding->target, binding_kind, type_kind)
                        || (binding_kind == CM_HIR_LIBRARY_BINDING_VALUE
                            && !cm_hir_library_add_value_from_item(state,
                                item))
                        || !cm_hir_library_add_entry(state, artifact_module,
                            name, binding->target, binding_kind,
                            type_kind, CM_HIR_PRIMITIVE_NONE,
                            value_kind))) return 0;
            }
        }
    }
    return 1;
}

static CmHirLibraryArtifactResult cm_hir_library_artifact_build_internal(
    CmHirLibraryArtifact *artifact, const CmHirContext *context,
    CmHirCrateId crate_id, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirModuleMap *modules,
    const char *extern_name, int include_values)
{
    CmHirLibraryArtifactResult result;
    CmHirLibraryArtifactState candidate;
    const CmHirCrate *crate_value;
    const CmHirModule *root_module;
    CmModuleId graph_root;
    size_t module_index;

    memset(&result, 0, sizeof(result));
    result.status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
    if (cm_hir_library_state(artifact) == NULL || context == NULL
        || crate_id == CM_HIR_CRATE_NONE || graph == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE || modules == NULL
        || !cm_hir_library_identifier_valid(extern_name)) return result;
    if (cm_module_graph_revision(graph) != revision) {
        result.status = CM_HIR_LIBRARY_STALE_REVISION;
        return result;
    }
    if (cm_module_graph_error_count(graph) != 0u
        || !cm_module_graph_get_root(graph, &graph_root)) {
        result.status = CM_HIR_LIBRARY_FAILED_GRAPH;
        return result;
    }
    crate_value = cm_hir_get_crate(context, crate_id);
    root_module = crate_value == NULL ? NULL
        : cm_hir_get_module(context, crate_value->root_module);
    if (root_module == NULL || root_module->crate_id != crate_id
        || cm_hir_module_map_count(modules)
            != cm_module_graph_module_count(graph)) {
        result.status = CM_HIR_LIBRARY_INVALID_HIR;
        return result;
    }
    memset(&candidate, 0, sizeof(candidate));
    cm_hir_library_owned_data_init(&candidate.owned);
    candidate.context = context;
    candidate.crate_id = crate_id;
    candidate.root_definition = root_module->definition;
    for (module_index = 0u;
            module_index < cm_module_graph_module_count(graph);
            ++module_index) {
        CmResolveModuleInfo information;
        CmHirModuleId hir_module_id;
        const CmHirModule *hir_module;
        CmHirLibraryOwnedModule artifact_module;

        if (!cm_module_graph_get_module_at(graph, module_index,
                &information)
            || cm_hir_module_map_lookup_hir(modules, graph, revision,
                information.id, context,
                &hir_module_id) != CM_HIR_MODULE_MAP_OK
            || (hir_module = cm_hir_get_module(context,
                hir_module_id)) == NULL
            || hir_module->crate_id != crate_id
            || !cm_hir_library_definition_valid(&candidate,
                hir_module->definition, CM_HIR_LIBRARY_BINDING_MODULE,
                CM_HIR_TYPE_ERROR_KIND)) {
            result.status = CM_HIR_LIBRARY_INVALID_HIR;
            goto fail_candidate;
        }
        memset(&artifact_module, 0, sizeof(artifact_module));
        artifact_module.capture_graph_module = information.id;
        artifact_module.capture_hir_module = hir_module_id;
        artifact_module.definition = hir_module->definition;
        cm_vec_init(&artifact_module.entries,
            sizeof(CmHirLibraryOwnedEntry));
        (void)cm_vec_push(&candidate.owned.modules, &artifact_module);
    }
    {
        CmHirLibraryOwnedModule *mapped_root;

        mapped_root = cm_hir_library_find_graph_module(&candidate,
            graph_root);
        if (mapped_root == NULL
            || !cm_hir_def_id_equal(mapped_root->definition,
                candidate.root_definition)) {
            result.status = CM_HIR_LIBRARY_INVALID_HIR;
            goto fail_candidate;
        }
    }
    for (module_index = 0u; module_index < candidate.owned.modules.len;
            ++module_index) {
        CmHirLibraryOwnedModule *artifact_module;
        CmResolveModuleInfo information;
        uint32_t entry_index;

        artifact_module = (CmHirLibraryOwnedModule *)cm_vec_at(
            &candidate.owned.modules, module_index);
        if (artifact_module == NULL
            || !cm_module_graph_get_module(graph,
                artifact_module->capture_graph_module, &information)) {
            result.status = CM_HIR_LIBRARY_INVALID_HIR;
            goto fail_candidate;
        }
        for (entry_index = 0u; entry_index < information.type_count;
                ++entry_index) {
            CmResolveNamespaceEntry entry;

            if (!cm_module_graph_get_namespace_entry(graph,
                    information.id, CM_RESOLVE_NAMESPACE_TYPE,
                    entry_index, &entry)) {
                result.status = CM_HIR_LIBRARY_FAILED_GRAPH;
                goto fail_candidate;
            }
            if (entry.visibility != CM_AST_VIS_PUBLIC) continue;
            if (!cm_hir_library_add_direct_entry(&candidate, graph,
                    modules, artifact_module, &information, &entry)) {
                result.status = CM_HIR_LIBRARY_INVALID_HIR;
                goto fail_candidate;
            }
        }
        if (include_values) {
            for (entry_index = 0u; entry_index < information.value_count;
                    ++entry_index) {
                CmResolveNamespaceEntry entry;

                if (!cm_module_graph_get_namespace_entry(graph,
                        information.id, CM_RESOLVE_NAMESPACE_VALUE,
                        entry_index, &entry)) {
                    result.status = CM_HIR_LIBRARY_FAILED_GRAPH;
                    goto fail_candidate;
                }
                if (entry.visibility != CM_AST_VIS_PUBLIC) continue;
                if (entry.item_kind != CM_AST_ITEM_FUNCTION
                    && entry.item_kind != CM_AST_ITEM_CONST
                    && entry.item_kind != CM_AST_ITEM_STATIC) continue;
                if (!cm_hir_library_add_direct_entry(&candidate, graph,
                        modules, artifact_module, &information, &entry)) {
                    result.status = CM_HIR_LIBRARY_INVALID_HIR;
                    goto fail_candidate;
                }
            }
        }
        if (!cm_hir_library_add_public_imports(&candidate,
                artifact_module, include_values)) {
            result.status = CM_HIR_LIBRARY_INVALID_HIR;
            goto fail_candidate;
        }
    }
    result = cm_hir_library_artifact_restore_owned(artifact, context,
        crate_id, root_module->definition, extern_name, &candidate.owned);
    cm_hir_library_owned_data_destroy(&candidate.owned);
    return result;

fail_candidate:
    cm_hir_library_owned_data_destroy(&candidate.owned);
    return result;
}

CmHirLibraryArtifactResult cm_hir_library_artifact_build(
    CmHirLibraryArtifact *artifact, const CmHirContext *context,
    CmHirCrateId crate_id, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirModuleMap *modules,
    const char *extern_name)
{
    return cm_hir_library_artifact_build_internal(artifact, context,
        crate_id, graph, revision, modules, extern_name, 0);
}

CmHirLibraryArtifactResult cm_hir_library_declaration_artifact_build(
    CmHirLibraryArtifact *artifact, const CmHirContext *context,
    CmHirCrateId crate_id, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirModuleMap *modules,
    const char *extern_name)
{
    return cm_hir_library_artifact_build_internal(artifact, context,
        crate_id, graph, revision, modules, extern_name, 1);
}

int cm_hir_library_artifact_identity(const CmHirLibraryArtifact *artifact,
    CmHirLibraryArtifactIdentity *out_identity)
{
    const CmHirLibraryArtifactState *state;

    if (out_identity != NULL) memset(out_identity, 0, sizeof(*out_identity));
    state = cm_hir_library_state_const(artifact);
    if (state == NULL || out_identity == NULL || state->context == NULL
        || state->crate_id == CM_HIR_CRATE_NONE
        || cm_hir_def_id_is_none(state->root_definition)
        || state->extern_name == NULL
        || !cm_hir_library_definition_valid(state,
            state->root_definition, CM_HIR_LIBRARY_BINDING_MODULE,
            CM_HIR_TYPE_ERROR_KIND)) return 0;
    out_identity->context = state->context;
    out_identity->crate_id = state->crate_id;
    out_identity->root_definition = state->root_definition;
    out_identity->extern_name = state->extern_name;
    return 1;
}

static int cm_hir_library_entry_name_is(
    const CmHirLibraryArtifactState *state,
    const CmHirLibraryOwnedEntry *entry,
    const CmHirLibraryPathSegment *segment)
{
    const CmInternedString *name;

    name = entry == NULL ? NULL
        : cm_interner_get(&state->owned.names, entry->name);
    return name != NULL && segment != NULL && segment->bytes != NULL
        && segment->length != 0u && name->len == segment->length
        && memcmp(name->bytes, segment->bytes, name->len) == 0;
}

static int cm_hir_library_segment_is_c_str(
    const CmHirLibraryPathSegment *segment, const char *text)
{
    size_t length;

    if (segment == NULL || segment->bytes == NULL || text == NULL) return 0;
    length = strlen(text);
    return segment->length == length
        && memcmp(segment->bytes, text, length) == 0;
}

static CmHirLibraryStatus cm_hir_library_lookup_from_module(
    const CmHirLibraryArtifactState *state, CmHirDefId current_module,
    const CmHirLibraryPathSegment *segments, size_t segment_count,
    int value_namespace, CmHirLibraryBinding *out_binding)
{
    size_t segment_index;

    if (out_binding != NULL) memset(out_binding, 0, sizeof(*out_binding));
    if (state == NULL || out_binding == NULL || segments == NULL
        || segment_count == 0u || state->context == NULL
        || cm_hir_def_id_is_none(current_module)) {
        return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    }
    for (segment_index = 0u; segment_index < segment_count;
            ++segment_index) {
        const CmHirLibraryOwnedModule *module;
        const CmHirLibraryOwnedEntry *selected;
        size_t entry_index;
        int saw_module;
        int saw_nonmodule;

        module = cm_hir_library_find_definition_module(state,
            current_module);
        if (module == NULL) return CM_HIR_LIBRARY_INVALID_HIR;
        selected = NULL;
        saw_module = 0;
        saw_nonmodule = 0;
        for (entry_index = 0u; entry_index < module->entries.len;
                ++entry_index) {
            const CmHirLibraryOwnedEntry *entry;

            entry = (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
                &module->entries, entry_index);
            if (!cm_hir_library_entry_name_is(state, entry,
                    &segments[segment_index])) continue;
            if (segment_index + 1u < segment_count) {
                if (entry->kind != CM_HIR_LIBRARY_BINDING_MODULE) continue;
            } else if (cm_hir_library_entry_is_value_namespace(entry)
                    != value_namespace) {
                continue;
            }
            if (entry->kind == CM_HIR_LIBRARY_BINDING_MODULE)
                saw_module = 1;
            else
                saw_nonmodule = 1;
            if (selected == NULL) {
                selected = entry;
            } else if (!cm_hir_def_id_equal(selected->target, entry->target)
                || selected->kind != entry->kind
                || (entry->kind == CM_HIR_LIBRARY_BINDING_TYPE
                    && selected->type_kind != entry->type_kind)
                || (entry->kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE
                    && selected->primitive_kind
                        != entry->primitive_kind)
                || (entry->kind == CM_HIR_LIBRARY_BINDING_VALUE
                    && selected->value_kind != entry->value_kind)) {
                return CM_HIR_LIBRARY_AMBIGUOUS;
            }
        }
        if (selected == NULL) return CM_HIR_LIBRARY_NOT_FOUND;
        if (saw_module && saw_nonmodule) return CM_HIR_LIBRARY_AMBIGUOUS;
        if (segment_index + 1u < segment_count) {
            if (selected->kind != CM_HIR_LIBRARY_BINDING_MODULE)
                return CM_HIR_LIBRARY_WRONG_NAMESPACE;
            if (!cm_hir_library_definition_valid(state, selected->target,
                    CM_HIR_LIBRARY_BINDING_MODULE,
                    CM_HIR_TYPE_ERROR_KIND)) {
                return CM_HIR_LIBRARY_INVALID_HIR;
            }
            current_module = selected->target;
        } else {
            if (selected->kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
                if (!cm_hir_def_id_is_none(selected->target)
                    || selected->primitive_kind == CM_HIR_PRIMITIVE_NONE
                    || (unsigned int)selected->primitive_kind
                        > (unsigned int)CM_HIR_PRIMITIVE_F128) {
                    return CM_HIR_LIBRARY_INVALID_HIR;
                }
            } else if (selected->kind == CM_HIR_LIBRARY_BINDING_VALUE) {
                const CmHirLibraryOwnedValue *value;

                value = cm_hir_library_find_value(state, selected->target);
                if (value == NULL
                    || value->declaration.kind != selected->value_kind
                    || !cm_hir_library_owned_value_valid(state, value)) {
                    return CM_HIR_LIBRARY_INVALID_HIR;
                }
            } else if (!cm_hir_library_definition_valid(state,
                    selected->target, selected->kind,
                    selected->type_kind)) {
                return CM_HIR_LIBRARY_INVALID_HIR;
            }
            out_binding->kind = selected->kind;
            out_binding->definition = selected->target;
            out_binding->type_kind = selected->type_kind;
            out_binding->primitive_kind = selected->primitive_kind;
            out_binding->value_kind = selected->value_kind;
            return CM_HIR_LIBRARY_OK;
        }
    }
    return CM_HIR_LIBRARY_NOT_FOUND;
}

CmHirLibraryStatus cm_hir_library_artifact_lookup_binding(
    const CmHirLibraryArtifact *artifact,
    const CmHirLibraryPathSegment *segments, size_t segment_count,
    CmHirLibraryBinding *out_binding)
{
    const CmHirLibraryArtifactState *state;

    if (out_binding != NULL) memset(out_binding, 0, sizeof(*out_binding));
    state = cm_hir_library_state_const(artifact);
    if (state == NULL || out_binding == NULL || segments == NULL
        || segment_count < 2u || state->context == NULL
        || state->extern_name == NULL) return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    if (!cm_hir_library_segment_is_c_str(&segments[0],
            state->extern_name)) return CM_HIR_LIBRARY_NOT_FOUND;
    return cm_hir_library_lookup_from_module(state, state->root_definition,
        &segments[1], segment_count - 1u, 0, out_binding);
}

CmHirLibraryStatus cm_hir_library_artifact_lookup_type(
    const CmHirLibraryArtifact *artifact,
    const CmHirLibraryPathSegment *segments, size_t segment_count,
    CmHirLibraryType *out_type)
{
    CmHirLibraryBinding binding;
    CmHirLibraryStatus status;

    if (out_type != NULL) memset(out_type, 0, sizeof(*out_type));
    if (out_type == NULL) return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    memset(&binding, 0, sizeof(binding));
    status = cm_hir_library_artifact_lookup_binding(artifact, segments,
        segment_count, &binding);
    if (status != CM_HIR_LIBRARY_OK) return status;
    if (binding.kind != CM_HIR_LIBRARY_BINDING_TYPE
        && binding.kind != CM_HIR_LIBRARY_BINDING_PRIMITIVE)
        return CM_HIR_LIBRARY_WRONG_NAMESPACE;
    out_type->definition = binding.definition;
    out_type->kind = binding.type_kind;
    out_type->primitive_kind = binding.primitive_kind;
    return CM_HIR_LIBRARY_OK;
}

CmHirLibraryStatus cm_hir_library_artifact_lookup_value(
    const CmHirLibraryArtifact *artifact,
    const CmHirLibraryPathSegment *segments, size_t segment_count,
    CmHirLibraryValue *out_value)
{
    const CmHirLibraryArtifactState *state;
    CmHirLibraryBinding binding;
    CmHirLibraryStatus status;
    const CmHirLibraryOwnedValue *value;

    if (out_value != NULL) memset(out_value, 0, sizeof(*out_value));
    state = cm_hir_library_state_const(artifact);
    if (state == NULL || out_value == NULL || segments == NULL
        || segment_count < 2u || state->context == NULL
        || state->extern_name == NULL) return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    if (!cm_hir_library_segment_is_c_str(&segments[0],
            state->extern_name)) return CM_HIR_LIBRARY_NOT_FOUND;
    memset(&binding, 0, sizeof(binding));
    status = cm_hir_library_lookup_from_module(state,
        state->root_definition, &segments[1], segment_count - 1u, 1,
        &binding);
    if (status != CM_HIR_LIBRARY_OK) return status;
    if (binding.kind != CM_HIR_LIBRARY_BINDING_VALUE)
        return CM_HIR_LIBRARY_WRONG_NAMESPACE;
    value = cm_hir_library_find_value(state, binding.definition);
    if (value == NULL || value->declaration.kind != binding.value_kind)
        return CM_HIR_LIBRARY_INVALID_HIR;
    *out_value = value->declaration;
    return CM_HIR_LIBRARY_OK;
}

static int cm_hir_library_segment_identifier_valid(
    const CmHirLibraryPathSegment *segment)
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

static int cm_hir_library_import_name_is(const CmImportResolver *imports,
    CmResolveStringId name, const CmHirLibraryPathSegment *expected)
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

CmHirLibraryStatus cm_hir_library_artifact_resolve_import(
    const CmHirLibraryArtifact *artifact, const CmImportResolver *imports,
    const CmModuleGraph *consumer,
    CmModuleGraphRevision consumer_revision, CmModuleId consumer_module,
    const CmHirLibraryPathSegment *local_name,
    CmHirLibraryImport *out_import)
{
    const CmHirLibraryArtifactState *state;
    CmResolvePathSegmentView local_segment;
    CmResolvedBinding local_binding;
    CmImportLookupStatus local_lookup;
    CmHirLibraryBinding candidate;
    CmResolveItemRef candidate_import;
    CmHirLibraryStatus candidate_status;
    size_t contender_count;
    size_t leaf_index;
    int unsupported_glob;

    if (out_import != NULL) memset(out_import, 0, sizeof(*out_import));
    state = cm_hir_library_state_const(artifact);
    if (state == NULL || imports == NULL || consumer == NULL
        || out_import == NULL
        || consumer_revision == CM_MODULE_GRAPH_REVISION_NONE
        || consumer_module == CM_MODULE_NONE
        || !cm_hir_library_segment_identifier_valid(local_name)) {
        return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    }
    if (state->context == NULL || state->extern_name == NULL
        || cm_module_graph_revision(consumer) != consumer_revision
        || cm_module_graph_error_count(consumer) != 0u
        || cm_import_resolver_revision(imports) != consumer_revision
        || !cm_import_resolver_matches_graph(imports, consumer)) {
        return CM_HIR_LIBRARY_STALE_REVISION;
    }
    local_segment.bytes = local_name->bytes;
    local_segment.length = local_name->length;
    memset(&local_binding, 0, sizeof(local_binding));
    local_lookup = cm_import_resolve_path_checked(imports, consumer,
        consumer_revision, consumer_module, 0, &local_segment, 1u,
        CM_RESOLVE_NAMESPACE_TYPE, &local_binding);
    if (local_lookup == CM_IMPORT_LOOKUP_OK
        || local_lookup == CM_IMPORT_LOOKUP_AMBIGUOUS
        || local_lookup == CM_IMPORT_LOOKUP_CYCLE) {
        return CM_HIR_LIBRARY_AMBIGUOUS;
    }
    if (local_lookup != CM_IMPORT_LOOKUP_NOT_FOUND)
        return CM_HIR_LIBRARY_STALE_REVISION;
    memset(&candidate, 0, sizeof(candidate));
    memset(&candidate_import, 0, sizeof(candidate_import));
    candidate_status = CM_HIR_LIBRARY_NOT_FOUND;
    contender_count = 0u;
    unsupported_glob = 0;
    for (leaf_index = 0u; leaf_index < cm_import_leaf_count(imports);
            ++leaf_index) {
        CmImportLeafView leaf;
        CmResolvePathSegmentView first;
        CmHirLibraryPathSegment first_library;

        if (leaf_index > (size_t)UINT32_MAX
            || !cm_import_get_leaf(imports, (uint32_t)leaf_index, &leaf)
            || leaf.module != consumer_module || leaf.is_resolved) continue;
        if (leaf.is_glob) {
            if (leaf.segment_count != 0u
                && cm_import_get_leaf_segment(imports,
                    (uint32_t)leaf_index, 0u, &first)) {
                first_library.bytes = first.bytes;
                first_library.length = first.length;
                if (cm_hir_library_segment_is_c_str(&first_library,
                        state->extern_name)) unsupported_glob = 1;
            }
            continue;
        }
        if (leaf.is_anonymous || leaf.segment_count < 2u
            || !cm_hir_library_import_name_is(imports, leaf.import_name,
                local_name)) continue;
        contender_count += 1u;
        if (contender_count != 1u
            || !cm_import_get_leaf_segment(imports,
                (uint32_t)leaf_index, 0u, &first)) continue;
        first_library.bytes = first.bytes;
        first_library.length = first.length;
        if (!cm_hir_library_segment_is_c_str(&first_library,
                state->extern_name)) continue;
        {
            CmHirLibraryPathSegment *path;
            size_t segment_index;
            int path_ok;

            path = (CmHirLibraryPathSegment *)cm_alloc_zeroed(
                leaf.segment_count, sizeof(*path));
            path_ok = 1;
            for (segment_index = 0u; segment_index < leaf.segment_count;
                    ++segment_index) {
                CmResolvePathSegmentView segment;

                if (segment_index > (size_t)UINT32_MAX
                    || !cm_import_get_leaf_segment(imports,
                        (uint32_t)leaf_index, (uint32_t)segment_index,
                        &segment)) {
                    path_ok = 0;
                    break;
                }
                path[segment_index].bytes = segment.bytes;
                path[segment_index].length = segment.length;
            }
            candidate_status = path_ok
                ? cm_hir_library_artifact_lookup_binding(artifact, path,
                    leaf.segment_count, &candidate)
                : CM_HIR_LIBRARY_INVALID_ARGUMENT;
            cm_free(path);
            candidate_import = leaf.declaration;
        }
    }
    if (unsupported_glob) {
        candidate_status = CM_HIR_LIBRARY_UNSUPPORTED_IMPORT;
    } else if (contender_count > 1u) {
        candidate_status = CM_HIR_LIBRARY_AMBIGUOUS;
    } else if (contender_count == 0u) {
        candidate_status = CM_HIR_LIBRARY_NOT_FOUND;
    }
    if (candidate_status == CM_HIR_LIBRARY_OK) {
        out_import->consumer_module = consumer_module;
        out_import->import_declaration = candidate_import;
        out_import->binding = candidate;
    }
    return candidate_status;
}

CmHirLibraryStatus cm_hir_library_artifact_resolve_imported_type(
    const CmHirLibraryArtifact *artifact, const CmImportResolver *imports,
    const CmModuleGraph *consumer,
    CmModuleGraphRevision consumer_revision, CmModuleId consumer_module,
    const CmHirLibraryPathSegment *local_module_name,
    const CmHirLibraryPathSegment *suffix, size_t suffix_count,
    CmHirLibraryType *out_type)
{
    CmHirLibraryBinding binding;
    CmHirLibraryStatus status;

    if (out_type != NULL) memset(out_type, 0, sizeof(*out_type));
    if (out_type == NULL || suffix == NULL
        || suffix_count == 0u) return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    memset(&binding, 0, sizeof(binding));
    status = cm_hir_library_artifact_resolve_imported_binding(artifact,
        imports, consumer, consumer_revision, consumer_module,
        local_module_name, suffix, suffix_count, &binding);
    if (status != CM_HIR_LIBRARY_OK) return status;
    if (binding.kind != CM_HIR_LIBRARY_BINDING_TYPE
        && binding.kind != CM_HIR_LIBRARY_BINDING_PRIMITIVE)
        return CM_HIR_LIBRARY_WRONG_NAMESPACE;
    out_type->definition = binding.definition;
    out_type->kind = binding.type_kind;
    out_type->primitive_kind = binding.primitive_kind;
    return CM_HIR_LIBRARY_OK;
}

CmHirLibraryStatus cm_hir_library_artifact_resolve_imported_binding(
    const CmHirLibraryArtifact *artifact, const CmImportResolver *imports,
    const CmModuleGraph *consumer,
    CmModuleGraphRevision consumer_revision, CmModuleId consumer_module,
    const CmHirLibraryPathSegment *local_module_name,
    const CmHirLibraryPathSegment *suffix, size_t suffix_count,
    CmHirLibraryBinding *out_binding)
{
    const CmHirLibraryArtifactState *state;
    CmHirLibraryImport imported;
    CmHirLibraryStatus status;

    if (out_binding != NULL) memset(out_binding, 0, sizeof(*out_binding));
    state = cm_hir_library_state_const(artifact);
    if (state == NULL || out_binding == NULL || suffix == NULL
        || suffix_count == 0u) return CM_HIR_LIBRARY_INVALID_ARGUMENT;
    memset(&imported, 0, sizeof(imported));
    status = cm_hir_library_artifact_resolve_import(artifact, imports,
        consumer, consumer_revision, consumer_module, local_module_name,
        &imported);
    if (status != CM_HIR_LIBRARY_OK) return status;
    if (imported.binding.kind != CM_HIR_LIBRARY_BINDING_MODULE)
        return CM_HIR_LIBRARY_WRONG_NAMESPACE;
    return cm_hir_library_lookup_from_module(state,
        imported.binding.definition, suffix, suffix_count, 0, out_binding);
}

const char *cm_hir_library_status_name(CmHirLibraryStatus status)
{
    switch (status) {
    case CM_HIR_LIBRARY_OK:
        return "ok";
    case CM_HIR_LIBRARY_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_HIR_LIBRARY_FAILED_GRAPH:
        return "failed graph";
    case CM_HIR_LIBRARY_STALE_REVISION:
        return "stale revision";
    case CM_HIR_LIBRARY_INVALID_HIR:
        return "invalid HIR";
    case CM_HIR_LIBRARY_NOT_FOUND:
        return "not found";
    case CM_HIR_LIBRARY_WRONG_NAMESPACE:
        return "wrong namespace";
    case CM_HIR_LIBRARY_UNSUPPORTED_IMPORT:
        return "unsupported import";
    case CM_HIR_LIBRARY_AMBIGUOUS:
        return "ambiguous";
    }
    return "unknown HIR library status";
}
