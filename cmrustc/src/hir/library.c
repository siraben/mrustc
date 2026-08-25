#include "cm/hir/library.h"

#include "library_internal.h"

#include "cm/alloc.h"

#include <stdlib.h>
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

static void cm_hir_library_owned_value_destroy(
    CmHirLibraryOwnedValue *value)
{
    uint32_t index;

    if (value == NULL) return;
    if (value->storage_kind != CM_HIR_LIBRARY_VALUE_FUNCTION) {
        memset(value, 0, sizeof(*value));
        return;
    }
    for (index = 0u; index < value->predicate_scope_count; ++index) {
        cm_free(value->predicate_scope_lifetimes[index]);
    }
    for (index = 0u; index < value->predicate_count; ++index) {
        cm_free(value->predicate_arguments[index]);
        cm_free(value->predicate_equalities[index]);
        cm_free(value->predicate_lifetimes[index]);
    }
    for (index = 0u; index < value->nominal_reference_count; ++index)
        cm_free(value->nominal_reference_generic_kinds[index]);
    cm_free(value->associated_availability);
    cm_free(value->nominal_reference_generic_kinds);
    cm_free(value->nominal_reference_names);
    cm_free(value->nominal_references);
    cm_free(value->outlives_predicates);
    cm_free(value->predicate_lifetimes);
    cm_free(value->predicate_equalities);
    cm_free(value->predicate_arguments);
    cm_free(value->predicates);
    cm_free(value->predicate_scope_lifetimes);
    cm_free(value->predicate_scopes);
    cm_free(value->parameter_types);
    memset(value, 0, sizeof(*value));
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
        cm_hir_library_owned_value_destroy(value);
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

static int cm_hir_library_span_equal(CmSpan left, CmSpan right)
{
    return left.source == right.source && left.start == right.start
        && left.end == right.end;
}

static int cm_hir_library_region_equal(const CmHirRegion *left,
    const CmHirRegion *right)
{
    if (left->kind != right->kind) return 0;
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
        return left->data.error_reason == right->data.error_reason;
    }
    return 0;
}

static int cm_hir_library_const_arg_equal(const CmHirConstArg *left,
    const CmHirConstArg *right)
{
    if (left->kind != right->kind || left->type != right->type) return 0;
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
        return left->data.error_reason == right->data.error_reason;
    }
    return 0;
}

static int cm_hir_library_generic_arg_equal(const CmHirGenericArg *left,
    const CmHirGenericArg *right)
{
    if (left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_hir_library_region_equal(&left->data.lifetime,
            &right->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return left->data.type == right->data.type;
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_hir_library_const_arg_equal(&left->data.constant,
            &right->data.constant);
    }
    return 0;
}

static int cm_hir_library_binder_equal(const CmHirLifetimeBinder *left,
    const CmHirLifetimeBinder *right)
{
    uint32_t index;

    if (left->lifetime_count != right->lifetime_count
        || !cm_hir_library_span_equal(left->span, right->span)) return 0;
    for (index = 0u; index < left->lifetime_count; ++index) {
        if (left->lifetimes[index] != right->lifetimes[index]) return 0;
    }
    return 1;
}

static int cm_hir_library_named_type_equal(const CmHirNamedType *left,
    const CmHirNamedType *right)
{
    uint32_t index;

    if (!cm_hir_def_id_equal(left->definition, right->definition)
        || left->argument_count != right->argument_count) return 0;
    for (index = 0u; index < left->argument_count; ++index) {
        if (!cm_hir_library_generic_arg_equal(&left->arguments[index],
                &right->arguments[index])) return 0;
    }
    return 1;
}

static int cm_hir_library_trait_predicate_equal(
    const CmHirTraitPredicate *left, const CmHirTraitPredicate *right)
{
    uint32_t index;

    if (left->subject != right->subject
        || !cm_hir_library_named_type_equal(&left->trait_type,
            &right->trait_type)
        || left->equality_count != right->equality_count
        || left->scope != right->scope
        || !cm_hir_library_binder_equal(&left->binder, &right->binder)
        || !cm_hir_library_span_equal(left->span, right->span)
        || left->modifier != right->modifier) return 0;
    for (index = 0u; index < left->equality_count; ++index) {
        if (!cm_hir_def_id_equal(left->equalities[index].associated_type,
                right->equalities[index].associated_type)
            || left->equalities[index].value
                != right->equalities[index].value
            || !cm_hir_library_span_equal(left->equalities[index].span,
                right->equalities[index].span)) return 0;
    }
    return 1;
}

static int cm_hir_library_outlives_predicate_equal(
    const CmHirOutlivesPredicate *left,
    const CmHirOutlivesPredicate *right)
{
    if (left->subject_kind != right->subject_kind
        || left->scope != right->scope
        || !cm_hir_library_region_equal(&left->bound, &right->bound)
        || !cm_hir_library_span_equal(left->span, right->span)) return 0;
    if (left->subject_kind == CM_HIR_OUTLIVES_TYPE)
        return left->subject.type == right->subject.type;
    if (left->subject_kind == CM_HIR_OUTLIVES_LIFETIME)
        return cm_hir_library_region_equal(&left->subject.lifetime,
            &right->subject.lifetime);
    return 0;
}

static int cm_hir_library_predicate_scope_equal(
    const CmHirPredicateScope *left, const CmHirPredicateScope *right)
{
    if (left->subject_kind != right->subject_kind
        || !cm_hir_library_binder_equal(&left->binder, &right->binder)
        || left->trait_predicate_count != right->trait_predicate_count
        || left->outlives_predicate_count != right->outlives_predicate_count
        || !cm_hir_library_span_equal(left->span, right->span)) return 0;
    if (left->subject_kind == CM_HIR_OUTLIVES_TYPE)
        return left->subject.type == right->subject.type;
    if (left->subject_kind == CM_HIR_OUTLIVES_LIFETIME)
        return cm_hir_library_region_equal(&left->subject.lifetime,
            &right->subject.lifetime);
    return 0;
}

static int cm_hir_library_nominal_reference_equal(
    const CmHirLibraryNominalReference *left,
    const CmHirLibraryNominalReference *right)
{
    uint32_t index;

    if (!cm_hir_def_id_equal(left->definition, right->definition)
        || !cm_hir_def_id_equal(left->owner_module, right->owner_module)
        || left->name.length != right->name.length
        || memcmp(left->name.bytes, right->name.bytes,
            left->name.length) != 0
        || left->use != right->use || left->kind != right->kind
        || !cm_hir_def_id_equal(left->declaring_trait,
            right->declaring_trait)
        || left->generic_parameter_count
            != right->generic_parameter_count) return 0;
    for (index = 0u; index < left->generic_parameter_count; ++index) {
        if (left->generic_parameter_kinds[index]
            != right->generic_parameter_kinds[index]) return 0;
    }
    return 1;
}

static int cm_hir_library_availability_equal(
    const CmHirLibraryAssociatedAvailability *left,
    const CmHirLibraryAssociatedAvailability *right)
{
    return cm_hir_def_id_equal(left->direct_trait, right->direct_trait)
        && cm_hir_def_id_equal(left->associated_type,
            right->associated_type);
}

static int cm_hir_library_function_predicates_equal(
    const CmHirLibraryFunctionSignature *left,
    const CmHirLibraryFunctionSignature *right)
{
    uint32_t index;

    if (left->predicate_scope_count != right->predicate_scope_count
        || left->predicate_count != right->predicate_count
        || left->outlives_predicate_count
            != right->outlives_predicate_count
        || left->nominal_reference_count != right->nominal_reference_count
        || left->associated_availability_count
            != right->associated_availability_count) return 0;
    for (index = 0u; index < left->predicate_scope_count; ++index) {
        if (!cm_hir_library_predicate_scope_equal(
                &left->predicate_scopes[index],
                &right->predicate_scopes[index])) return 0;
    }
    for (index = 0u; index < left->predicate_count; ++index) {
        if (!cm_hir_library_trait_predicate_equal(&left->predicates[index],
                &right->predicates[index])) return 0;
    }
    for (index = 0u; index < left->outlives_predicate_count; ++index) {
        if (!cm_hir_library_outlives_predicate_equal(
                &left->outlives_predicates[index],
                &right->outlives_predicates[index])) return 0;
    }
    for (index = 0u; index < left->nominal_reference_count; ++index) {
        if (!cm_hir_library_nominal_reference_equal(
                &left->nominal_references[index],
                &right->nominal_references[index])) return 0;
    }
    for (index = 0u; index < left->associated_availability_count; ++index) {
        if (!cm_hir_library_availability_equal(
                &left->associated_availability[index],
                &right->associated_availability[index])) return 0;
    }
    return 1;
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
            || left->data.function.generic_parameter_start
                != right->data.function.generic_parameter_start
            || left->data.function.generic_parameter_count
                != right->data.function.generic_parameter_count
            || !cm_hir_library_function_predicates_equal(
                &left->data.function, &right->data.function)
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

static int cm_hir_library_array_shape_valid(const void *array,
    uint32_t count, size_t element_size)
{
    size_t byte_count;

    return (count == 0u) == (array == NULL)
        && element_size != 0u
        && cm_size_mul((size_t)count, element_size, &byte_count);
}

static int cm_hir_library_binder_shape_valid(
    const CmHirLifetimeBinder *binder)
{
    return binder != NULL && cm_hir_library_array_shape_valid(
        binder->lifetimes, binder->lifetime_count, sizeof(CmInternId));
}

static int cm_hir_library_function_array_shapes_valid(
    const CmHirLibraryFunctionSignature *function)
{
    uint32_t index;

    if (function == NULL
        || !cm_hir_library_array_shape_valid(function->parameter_types,
            function->parameter_count, sizeof(CmHirTypeId))
        || !cm_hir_library_array_shape_valid(function->predicate_scopes,
            function->predicate_scope_count, sizeof(CmHirPredicateScope))
        || !cm_hir_library_array_shape_valid(function->predicates,
            function->predicate_count, sizeof(CmHirTraitPredicate))
        || !cm_hir_library_array_shape_valid(function->outlives_predicates,
            function->outlives_predicate_count,
            sizeof(CmHirOutlivesPredicate))
        || !cm_hir_library_array_shape_valid(function->nominal_references,
            function->nominal_reference_count,
            sizeof(CmHirLibraryNominalReference))
        || !cm_hir_library_array_shape_valid(
            function->associated_availability,
            function->associated_availability_count,
            sizeof(CmHirLibraryAssociatedAvailability))) return 0;
    for (index = 0u; index < function->predicate_scope_count; ++index) {
        if (!cm_hir_library_binder_shape_valid(
                &function->predicate_scopes[index].binder)) return 0;
    }
    for (index = 0u; index < function->predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;

        predicate = &function->predicates[index];
        if (!cm_hir_library_array_shape_valid(
                predicate->trait_type.arguments,
                predicate->trait_type.argument_count,
                sizeof(CmHirGenericArg))
            || !cm_hir_library_array_shape_valid(predicate->equalities,
                predicate->equality_count,
                sizeof(CmHirAssociatedTypeEquality))
            || !cm_hir_library_binder_shape_valid(&predicate->binder)) {
            return 0;
        }
    }
    for (index = 0u; index < function->nominal_reference_count; ++index) {
        if (function->nominal_references[index].name.bytes == NULL
            || function->nominal_references[index].name.length == 0u
            || !cm_hir_library_array_shape_valid(
                function->nominal_references[index].generic_parameter_kinds,
                function->nominal_references[index].generic_parameter_count,
                sizeof(CmHirGenericParamKind))) return 0;
    }
    return 1;
}

static void *cm_hir_library_copy_array(const void *source, uint32_t count,
    size_t element_size)
{
    void *copy;
    size_t byte_count;

    if (count == 0u) return NULL;
    byte_count = (size_t)count * element_size;
    copy = cm_alloc(byte_count);
    memcpy(copy, source, byte_count);
    return copy;
}

static int cm_hir_library_owned_value_copy(CmHirLibraryOwnedValue *copy,
    CmInterner *names, const CmHirLibraryValue *value)
{
    const CmHirLibraryFunctionSignature *source;
    CmHirLibraryFunctionSignature *target;
    uint32_t index;

    memset(copy, 0, sizeof(*copy));
    copy->declaration = *value;
    copy->storage_kind = value->kind;
    if (value->kind != CM_HIR_LIBRARY_VALUE_FUNCTION) return 1;
    source = &value->data.function;
    target = &copy->declaration.data.function;
    copy->parameter_count = source->parameter_count;
    copy->predicate_scope_count = source->predicate_scope_count;
    copy->predicate_count = source->predicate_count;
    copy->outlives_predicate_count = source->outlives_predicate_count;
    copy->nominal_reference_count = source->nominal_reference_count;
    copy->associated_availability_count =
        source->associated_availability_count;
    target->parameter_types = NULL;
    target->predicate_scopes = NULL;
    target->predicates = NULL;
    target->outlives_predicates = NULL;
    target->nominal_references = NULL;
    target->associated_availability = NULL;
    copy->parameter_types = (CmHirTypeId *)cm_hir_library_copy_array(
        source->parameter_types, source->parameter_count,
        sizeof(CmHirTypeId));
    target->parameter_types = copy->parameter_types;
    copy->predicate_scopes = (CmHirPredicateScope *)cm_hir_library_copy_array(
        source->predicate_scopes, source->predicate_scope_count,
        sizeof(CmHirPredicateScope));
    copy->predicate_scope_lifetimes = source->predicate_scope_count == 0u
        ? NULL : (CmInternId **)cm_alloc_zeroed(
            source->predicate_scope_count, sizeof(CmInternId *));
    target->predicate_scopes = copy->predicate_scopes;
    for (index = 0u; index < source->predicate_scope_count; ++index) {
        copy->predicate_scopes[index].binder.lifetimes = NULL;
        copy->predicate_scope_lifetimes[index] =
            (CmInternId *)cm_hir_library_copy_array(
                source->predicate_scopes[index].binder.lifetimes,
                source->predicate_scopes[index].binder.lifetime_count,
                sizeof(CmInternId));
        copy->predicate_scopes[index].binder.lifetimes =
            copy->predicate_scope_lifetimes[index];
    }
    copy->predicates = (CmHirTraitPredicate *)cm_hir_library_copy_array(
        source->predicates, source->predicate_count,
        sizeof(CmHirTraitPredicate));
    copy->predicate_arguments = source->predicate_count == 0u
        ? NULL : (CmHirGenericArg **)cm_alloc_zeroed(
            source->predicate_count, sizeof(CmHirGenericArg *));
    copy->predicate_equalities =
        source->predicate_count == 0u ? NULL
        : (CmHirAssociatedTypeEquality **)cm_alloc_zeroed(
            source->predicate_count, sizeof(CmHirAssociatedTypeEquality *));
    copy->predicate_lifetimes = source->predicate_count == 0u
        ? NULL : (CmInternId **)cm_alloc_zeroed(
            source->predicate_count, sizeof(CmInternId *));
    target->predicates = copy->predicates;
    for (index = 0u; index < source->predicate_count; ++index) {
        copy->predicates[index].trait_type.arguments = NULL;
        copy->predicates[index].equalities = NULL;
        copy->predicates[index].binder.lifetimes = NULL;
        copy->predicate_arguments[index] =
            (CmHirGenericArg *)cm_hir_library_copy_array(
                source->predicates[index].trait_type.arguments,
                source->predicates[index].trait_type.argument_count,
                sizeof(CmHirGenericArg));
        copy->predicates[index].trait_type.arguments =
            copy->predicate_arguments[index];
        copy->predicate_equalities[index] =
            (CmHirAssociatedTypeEquality *)cm_hir_library_copy_array(
                source->predicates[index].equalities,
                source->predicates[index].equality_count,
                sizeof(CmHirAssociatedTypeEquality));
        copy->predicates[index].equalities = copy->predicate_equalities[index];
        copy->predicate_lifetimes[index] =
            (CmInternId *)cm_hir_library_copy_array(
                source->predicates[index].binder.lifetimes,
                source->predicates[index].binder.lifetime_count,
                sizeof(CmInternId));
        copy->predicates[index].binder.lifetimes =
            copy->predicate_lifetimes[index];
    }
    copy->outlives_predicates =
        (CmHirOutlivesPredicate *)cm_hir_library_copy_array(
            source->outlives_predicates, source->outlives_predicate_count,
            sizeof(CmHirOutlivesPredicate));
    target->outlives_predicates = copy->outlives_predicates;
    copy->nominal_references =
        (CmHirLibraryNominalReference *)cm_hir_library_copy_array(
            source->nominal_references, source->nominal_reference_count,
            sizeof(CmHirLibraryNominalReference));
    copy->nominal_reference_generic_kinds =
        source->nominal_reference_count == 0u ? NULL
        : (CmHirGenericParamKind **)cm_alloc_zeroed(
            source->nominal_reference_count, sizeof(CmHirGenericParamKind *));
    copy->nominal_reference_names = source->nominal_reference_count == 0u
        ? NULL : (CmInternId *)cm_alloc(
            (size_t)source->nominal_reference_count * sizeof(CmInternId));
    target->nominal_references = copy->nominal_references;
    for (index = 0u; index < source->nominal_reference_count; ++index) {
        const CmInternedString *owned_name;

        copy->nominal_reference_names[index] = cm_interner_intern(names,
            source->nominal_references[index].name.bytes,
            source->nominal_references[index].name.length);
        owned_name = cm_interner_get(names,
            copy->nominal_reference_names[index]);
        if (owned_name == NULL) return 0;
        copy->nominal_references[index].name.bytes = owned_name->bytes;
        copy->nominal_references[index].name.length = owned_name->len;
        copy->nominal_references[index].generic_parameter_kinds = NULL;
        copy->nominal_reference_generic_kinds[index] =
            (CmHirGenericParamKind *)cm_hir_library_copy_array(
                source->nominal_references[index].generic_parameter_kinds,
                source->nominal_references[index].generic_parameter_count,
                sizeof(CmHirGenericParamKind));
        copy->nominal_references[index].generic_parameter_kinds =
            copy->nominal_reference_generic_kinds[index];
    }
    copy->associated_availability =
        (CmHirLibraryAssociatedAvailability *)cm_hir_library_copy_array(
            source->associated_availability,
            source->associated_availability_count,
            sizeof(CmHirLibraryAssociatedAvailability));
    target->associated_availability = copy->associated_availability;
    return 1;
}

CmHirLibraryStatus cm_hir_library_owned_data_add_value(
    CmHirLibraryOwnedData *data, const CmHirLibraryValue *value)
{
    CmHirLibraryOwnedValue copy;
    CmInternerMark names_mark;
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
            || !cm_hir_library_function_array_shapes_valid(
                &value->data.function)
            || (value->data.function.generic_parameter_count == 0u
                ? value->data.function.generic_parameter_start
                    != CM_HIR_GENERIC_PARAM_NONE
                : value->data.function.generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE)) {
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
    names_mark = cm_interner_mark(&data->names);
    if (!cm_hir_library_owned_value_copy(&copy, &data->names, value)) {
        cm_hir_library_owned_value_destroy(&copy);
        cm_interner_rewind(&data->names, names_mark);
        return CM_HIR_LIBRARY_INVALID_HIR;
    }
    cm_interner_discard_mark(&data->names, names_mark);
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
    case CM_HIR_ITEM_TRAIT_ALIAS:
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

static int cm_hir_library_definition_compare(CmHirDefId left,
    CmHirDefId right)
{
    if (left.crate_id != right.crate_id)
        return left.crate_id < right.crate_id ? -1 : 1;
    if (left.index != right.index) return left.index < right.index ? -1 : 1;
    return 0;
}

static int cm_hir_library_nominal_reference_compare(const void *left_value,
    const void *right_value)
{
    const CmHirLibraryNominalReference *left;
    const CmHirLibraryNominalReference *right;
    int order;

    left = (const CmHirLibraryNominalReference *)left_value;
    right = (const CmHirLibraryNominalReference *)right_value;
    order = cm_hir_library_definition_compare(left->definition,
        right->definition);
    if (order != 0) return order;
    if (left->kind != right->kind) return left->kind < right->kind ? -1 : 1;
    return cm_hir_library_definition_compare(left->declaring_trait,
        right->declaring_trait);
}

static int cm_hir_library_availability_compare(const void *left_value,
    const void *right_value)
{
    const CmHirLibraryAssociatedAvailability *left;
    const CmHirLibraryAssociatedAvailability *right;
    int order;

    left = (const CmHirLibraryAssociatedAvailability *)left_value;
    right = (const CmHirLibraryAssociatedAvailability *)right_value;
    order = cm_hir_library_definition_compare(left->direct_trait,
        right->direct_trait);
    return order != 0 ? order : cm_hir_library_definition_compare(
        left->associated_type, right->associated_type);
}

static int cm_hir_library_intern_id_compare(const void *left_value,
    const void *right_value)
{
    CmInternId left;
    CmInternId right;

    left = *(const CmInternId *)left_value;
    right = *(const CmInternId *)right_value;
    return left < right ? -1 : (left > right ? 1 : 0);
}

static int cm_hir_library_def_id_qsort_compare(const void *left_value,
    const void *right_value)
{
    return cm_hir_library_definition_compare(
        *(const CmHirDefId *)left_value,
        *(const CmHirDefId *)right_value);
}

static void cm_hir_library_nominal_reference_vec_destroy(CmVec *references)
{
    size_t index;

    if (references == NULL) return;
    for (index = 0u; index < references->len; ++index) {
        CmHirLibraryNominalReference *reference;

        reference = (CmHirLibraryNominalReference *)cm_vec_at(references,
            index);
        if (reference != NULL)
            cm_free((void *)reference->generic_parameter_kinds);
    }
    cm_vec_destroy(references);
}

static int cm_hir_library_reference_schema_from_item(
    const CmHirContext *context, const CmHirItem *item,
    CmHirGenericParamKind **out_kinds)
{
    CmHirGenericParamKind *kinds;
    uint32_t index;

    if (out_kinds == NULL || context == NULL || item == NULL) return 0;
    *out_kinds = NULL;
    if (item->generic_parameter_count == 0u)
        return item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE;
    if (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) return 0;
    kinds = (CmHirGenericParamKind *)cm_alloc(
        (size_t)item->generic_parameter_count
            * sizeof(CmHirGenericParamKind));
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        CmHirGenericParamId parameter_id;
        const CmHirGenericParam *parameter;

        parameter_id = item->generic_parameter_start + index;
        if (parameter_id < item->generic_parameter_start) {
            cm_free(kinds);
            return 0;
        }
        parameter = cm_hir_get_generic_param(context, parameter_id);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner, item->definition)) {
            cm_free(kinds);
            return 0;
        }
        kinds[index] = parameter->kind;
    }
    *out_kinds = kinds;
    return 1;
}

static const CmHirItem *cm_hir_library_bound_item(
    const CmHirContext *context, CmHirDefId definition)
{
    const CmHirDefinition *resolved;

    resolved = cm_hir_lookup_definition(context, definition);
    if (resolved == NULL || resolved->state != CM_HIR_DEFINITION_BOUND
        || resolved->kind != CM_HIR_DEFINITION_ITEM) return NULL;
    return cm_hir_get_item(context, resolved->entity.item_id);
}

static const CmHirItem *cm_hir_library_bound_item_with_id(
    const CmHirContext *context, CmHirDefId definition,
    CmHirItemId *out_item_id)
{
    const CmHirDefinition *resolved;

    if (out_item_id == NULL) return NULL;
    *out_item_id = CM_HIR_ITEM_NONE;
    resolved = cm_hir_lookup_definition(context, definition);
    if (resolved == NULL || resolved->state != CM_HIR_DEFINITION_BOUND
        || resolved->kind != CM_HIR_DEFINITION_ITEM
        || resolved->entity.item_id == CM_HIR_ITEM_NONE
        || (size_t)resolved->entity.item_id > context->items.len) return NULL;
    *out_item_id = resolved->entity.item_id;
    return cm_hir_get_item(context, resolved->entity.item_id);
}

static int cm_hir_library_add_nominal_reference(const CmHirContext *context,
    const CmInterner *names, CmInterner *capture_names, CmVec *references,
    uint32_t *reference_locals,
    CmHirDefId definition,
    CmHirLibraryNominalReferenceKind kind, CmHirDefId declaring_trait)
{
    const CmHirItem *item;
    CmHirItemId item_id;
    const CmHirModule *module;
    const CmInternedString *owned_name;
    const CmInternedString *source_name;
    CmHirGenericParamKind *kinds;
    CmInternId name;
    CmHirLibraryNominalReference reference;

    item = cm_hir_library_bound_item_with_id(context, definition, &item_id);
    module = item == NULL ? NULL
        : cm_hir_get_module(context, item->owner_module);
    source_name = item == NULL ? NULL
        : cm_interner_get(&context->strings, item->name);
    if (item == NULL
        || module == NULL || source_name == NULL || source_name->len == 0u) {
        return 0;
    }
    if (kind == CM_HIR_LIBRARY_NOMINAL_TRAIT) {
        if (item->kind != CM_HIR_ITEM_TRAIT
            || !cm_hir_def_id_is_none(declaring_trait)) return 0;
    } else if (kind == CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS) {
        if (item->kind != CM_HIR_ITEM_TRAIT_ALIAS
            || !cm_hir_def_id_is_none(declaring_trait)) return 0;
    } else if (kind == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) {
        if (item->kind != CM_HIR_ITEM_TYPE_ALIAS
                || item->data.type_alias_item.target != CM_HIR_TYPE_NONE
                || cm_hir_def_id_is_none(declaring_trait)
                || !cm_hir_def_id_equal(item->parent_definition,
                    declaring_trait)) return 0;
    } else {
        return 0;
    }
    name = capture_names == NULL
        ? cm_interner_lookup(names, source_name->bytes, source_name->len)
        : cm_interner_intern(capture_names, source_name->bytes,
            source_name->len);
    if (name == CM_INTERN_ID_NONE) return 0;
    owned_name = cm_interner_get(capture_names == NULL ? names : capture_names,
        name);
    if (owned_name == NULL) return 0;
    kinds = NULL;
    if (!cm_hir_library_reference_schema_from_item(context, item, &kinds))
        return 0;
    memset(&reference, 0, sizeof(reference));
    reference.definition = definition;
    reference.owner_module = module->definition;
    reference.name.bytes = owned_name->bytes;
    reference.name.length = owned_name->len;
    reference.use = CM_HIR_LIBRARY_REFERENCE_ONLY;
    reference.kind = kind;
    reference.declaring_trait = declaring_trait;
    reference.generic_parameter_kinds = kinds;
    reference.generic_parameter_count = item->generic_parameter_count;
    if (reference_locals[item_id - 1u] != 0u) {
        const CmHirLibraryNominalReference *existing;

        existing = (const CmHirLibraryNominalReference *)cm_vec_at_const(
            references, (size_t)(reference_locals[item_id - 1u] - 1u));
        if (!cm_hir_library_nominal_reference_equal(existing, &reference)) {
            cm_free(kinds);
            return 0;
        }
        cm_free(kinds);
        return 1;
    }
    (void)cm_vec_push(references, &reference);
    reference_locals[item_id - 1u] = (uint32_t)references->len;
    return 1;
}

static int cm_hir_library_collect_nominal_bound_closure(
    const CmHirContext *context, const CmInterner *names,
    CmInterner *capture_names, CmHirDefId bound_definition,
    CmVec *references, uint32_t *reference_locals,
    uint32_t *visit_generations, unsigned char *visit_states,
    uint32_t generation, size_t depth)
{
    const CmHirItem *bound_item;
    CmHirItemId item_id;
    CmHirLibraryNominalReferenceKind reference_kind;
    uint32_t index;

    /* Bound nominal closure is attacker-reachable during capture.  Keep its
     * recursion under the same node budget as metadata type traversal. */
    if (depth >= (size_t)CM_META_MAX_TYPE_NESTING) return 0;
    bound_item = cm_hir_library_bound_item_with_id(context, bound_definition,
        &item_id);
    if (bound_item == NULL) return 0;
    if (visit_generations[item_id - 1u] == generation) {
        return visit_states[item_id - 1u] == UINT8_C(2);
    }
    visit_generations[item_id - 1u] = generation;
    visit_states[item_id - 1u] = UINT8_C(1);
    if (bound_item->kind == CM_HIR_ITEM_TRAIT) {
        reference_kind = CM_HIR_LIBRARY_NOMINAL_TRAIT;
    } else if (bound_item->kind == CM_HIR_ITEM_TRAIT_ALIAS) {
        reference_kind = CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS;
    } else {
        return 0;
    }
    if (!cm_hir_library_add_nominal_reference(context, names, capture_names,
            references, reference_locals, bound_definition, reference_kind,
            cm_hir_def_id_none())) return 0;
    if (bound_item->kind == CM_HIR_ITEM_TRAIT) {
        for (index = 0u;
                index < bound_item->data.trait_item.supertrait_count;
                ++index) {
            if (!cm_hir_library_collect_nominal_bound_closure(context, names,
                    capture_names,
                    bound_item->data.trait_item.supertraits[index]
                        .trait_type.definition,
                    references, reference_locals, visit_generations,
                    visit_states, generation, depth + 1u)) return 0;
        }
    } else {
        for (index = 0u;
                index < bound_item->data.trait_alias_item.bound_count;
                ++index) {
            const CmHirTraitAliasBound *bound;

            bound = &bound_item->data.trait_alias_item.bounds[index];
            if (bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME) continue;
            /*
             * Alias-bound equalities belong to the opaque alias definition,
             * not to the consuming function predicate.  This slice records
             * only the transitive nominal identity closure.
             */
            if (bound->kind != CM_HIR_TRAIT_ALIAS_BOUND_TRAIT
                || !cm_hir_library_collect_nominal_bound_closure(context,
                    names, capture_names,
                    bound->data.trait_bound.trait_type.definition,
                    references, reference_locals, visit_generations,
                    visit_states, generation, depth + 1u)) return 0;
        }
    }
    visit_states[item_id - 1u] = UINT8_C(2);
    return 1;
}

static int cm_hir_library_add_availability(CmVec *availability,
    CmHirDefId direct_trait, CmHirDefId associated_type)
{
    CmHirLibraryAssociatedAvailability value;
    value.direct_trait = direct_trait;
    value.associated_type = associated_type;
    (void)cm_vec_push(availability, &value);
    return 1;
}

static int cm_hir_library_collect_nominal_references(
    const CmHirContext *context, const CmInterner *names,
    CmInterner *capture_names, const CmHirItem *item, CmVec *references,
    CmVec *availability)
{
    uint32_t *reference_locals;
    uint32_t *visit_generations;
    unsigned char *visit_states;
    uint32_t generation;
    uint32_t predicate_index;
    int valid;

    cm_vec_init(references, sizeof(CmHirLibraryNominalReference));
    cm_vec_init(availability, sizeof(CmHirLibraryAssociatedAvailability));
    if (context == NULL || item == NULL) return 0;
    reference_locals = (uint32_t *)cm_alloc_zeroed(context->items.len,
        sizeof(uint32_t));
    visit_generations = (uint32_t *)cm_alloc_zeroed(context->items.len,
        sizeof(uint32_t));
    visit_states = (unsigned char *)cm_alloc_zeroed(context->items.len,
        sizeof(unsigned char));
    generation = 0u;
    valid = 1;
    for (predicate_index = 0u; valid
            && predicate_index < item->predicate_count;
            ++predicate_index) {
        const CmHirTraitPredicate *predicate;
        uint32_t equality_index;

        predicate = &item->predicates[predicate_index];
        generation += 1u;
        if (generation == 0u) {
            memset(visit_generations, 0,
                context->items.len * sizeof(uint32_t));
            generation = 1u;
        }
        if (!cm_hir_library_collect_nominal_bound_closure(context, names,
                capture_names,
                predicate->trait_type.definition, references,
                reference_locals, visit_generations, visit_states,
                generation, 0u)) {
            valid = 0;
            break;
        }
        for (equality_index = 0u; equality_index < predicate->equality_count;
                ++equality_index) {
            const CmHirAssociatedTypeEquality *equality;
            const CmHirItem *associated;
            const CmHirItem *parent;
            CmHirItemId parent_id;

            equality = &predicate->equalities[equality_index];
            associated = cm_hir_library_bound_item(context,
                equality->associated_type);
            parent = associated == NULL
                ? NULL : cm_hir_library_bound_item_with_id(context,
                    associated->parent_definition, &parent_id);
            if (associated == NULL
                || associated->kind != CM_HIR_ITEM_TYPE_ALIAS
                || associated->data.type_alias_item.target != CM_HIR_TYPE_NONE
                || cm_hir_def_id_is_none(associated->parent_definition)
                || parent == NULL || parent->kind != CM_HIR_ITEM_TRAIT
                || visit_generations[parent_id - 1u] != generation
                || visit_states[parent_id - 1u] != UINT8_C(2)
                || !cm_hir_library_add_nominal_reference(context, names,
                    capture_names, references, reference_locals,
                    equality->associated_type,
                    CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE,
                    associated->parent_definition)
                || !cm_hir_library_add_availability(availability,
                    predicate->trait_type.definition,
                    equality->associated_type)) {
                valid = 0;
                break;
            }
        }
    }
    cm_free(visit_states);
    cm_free(visit_generations);
    cm_free(reference_locals);
    if (!valid) return 0;
    if (references->len > 1u) qsort(references->data, references->len,
        sizeof(CmHirLibraryNominalReference),
        cm_hir_library_nominal_reference_compare);
    if (availability->len > 1u) qsort(availability->data, availability->len,
        sizeof(CmHirLibraryAssociatedAvailability),
        cm_hir_library_availability_compare);
    if (availability->len > 1u) {
        size_t read_index;
        size_t write_index;

        write_index = 1u;
        for (read_index = 1u; read_index < availability->len; ++read_index) {
            CmHirLibraryAssociatedAvailability *prior;
            CmHirLibraryAssociatedAvailability *current;

            prior = (CmHirLibraryAssociatedAvailability *)cm_vec_at(
                availability, write_index - 1u);
            current = (CmHirLibraryAssociatedAvailability *)cm_vec_at(
                availability, read_index);
            if (prior == NULL || current == NULL) return 0;
            if (cm_hir_library_availability_compare(prior, current) == 0)
                continue;
            if (write_index != read_index) {
                CmHirLibraryAssociatedAvailability *target;

                target = (CmHirLibraryAssociatedAvailability *)cm_vec_at(
                    availability, write_index);
                if (target == NULL) return 0;
                *target = *current;
            }
            write_index += 1u;
        }
        availability->len = write_index;
    }
    return 1;
}

static int cm_hir_library_add_value_from_item(
    CmHirLibraryArtifactState *state, const CmHirItem *item)
{
    CmHirLibraryValue value;
    CmHirTypeId *parameter_types;
    CmVec nominal_references;
    CmVec associated_availability;
    uint32_t index;
    CmHirLibraryStatus status;

    if (state == NULL || item == NULL
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || (item->kind == CM_HIR_ITEM_FUNCTION
            && item->data.function_item.has_default_body)
        || ((item->kind == CM_HIR_ITEM_CONST
                || item->kind == CM_HIR_ITEM_STATIC)
            && item->data.value_item.has_default_body)
        || (item->kind != CM_HIR_ITEM_FUNCTION
            && (item->generic_parameter_count != 0u
                || item->predicate_scope_count != 0u
                || item->predicate_count != 0u
                || item->outlives_predicate_count != 0u))) return 0;
    memset(&value, 0, sizeof(value));
    value.definition = item->definition;
    value.kind = cm_hir_library_value_kind(item->kind);
    if (value.kind == CM_HIR_LIBRARY_VALUE_NONE) return 0;
    parameter_types = NULL;
    memset(&nominal_references, 0, sizeof(nominal_references));
    memset(&associated_availability, 0, sizeof(associated_availability));
    if (value.kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
        const CmHirFunctionSignature *signature;

        signature = &item->data.function_item.signature;
        if (signature->receiver != CM_HIR_RECEIVER_NONE
            || (signature->parameter_count != 0u
                && signature->parameters == NULL)) return 0;
        if (!cm_hir_library_collect_nominal_references(state->context,
                &state->owned.names, &state->owned.names, item,
                &nominal_references, &associated_availability)) {
            cm_hir_library_nominal_reference_vec_destroy(&nominal_references);
            cm_vec_destroy(&associated_availability);
            return 0;
        }
        if (nominal_references.len > (size_t)UINT32_MAX
            || associated_availability.len > (size_t)UINT32_MAX) {
            cm_hir_library_nominal_reference_vec_destroy(&nominal_references);
            cm_vec_destroy(&associated_availability);
            return 0;
        }
        if (signature->parameter_count != 0u) {
            parameter_types = (CmHirTypeId *)cm_alloc(
                (size_t)signature->parameter_count * sizeof(CmHirTypeId));
            for (index = 0u; index < signature->parameter_count; ++index)
                parameter_types[index] = signature->parameters[index].type;
        }
        value.data.function.parameter_types = parameter_types;
        value.data.function.parameter_count = signature->parameter_count;
        value.data.function.return_type = signature->return_type;
        value.data.function.generic_parameter_start =
            item->generic_parameter_start;
        value.data.function.generic_parameter_count =
            item->generic_parameter_count;
        value.data.function.predicate_scopes = item->predicate_scopes;
        value.data.function.predicate_scope_count =
            item->predicate_scope_count;
        value.data.function.predicates = item->predicates;
        value.data.function.predicate_count = item->predicate_count;
        value.data.function.outlives_predicates = item->outlives_predicates;
        value.data.function.outlives_predicate_count =
            item->outlives_predicate_count;
        value.data.function.nominal_references =
            (const CmHirLibraryNominalReference *)nominal_references.data;
        value.data.function.nominal_reference_count =
            (uint32_t)nominal_references.len;
        value.data.function.associated_availability =
            (const CmHirLibraryAssociatedAvailability *)
                associated_availability.data;
        value.data.function.associated_availability_count =
            (uint32_t)associated_availability.len;
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
    cm_hir_library_nominal_reference_vec_destroy(&nominal_references);
    cm_vec_destroy(&associated_availability);
    cm_free(parameter_types);
    return status == CM_HIR_LIBRARY_OK;
}

static int cm_hir_library_value_type_valid(const CmHirContext *context,
    CmHirTypeId type)
{
    return type != CM_HIR_TYPE_NONE && cm_hir_get_type(context, type) != NULL;
}

static int cm_hir_library_owned_function_storage_valid(
    const CmInterner *names, const CmHirLibraryOwnedValue *owned_value)
{
    const CmHirLibraryFunctionSignature *function;
    uint32_t index;

    function = &owned_value->declaration.data.function;
    if (owned_value->storage_kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || function->parameter_count != owned_value->parameter_count
        || function->predicate_scope_count
            != owned_value->predicate_scope_count
        || function->predicate_count != owned_value->predicate_count
        || function->outlives_predicate_count
            != owned_value->outlives_predicate_count
        || function->nominal_reference_count
            != owned_value->nominal_reference_count
        || function->associated_availability_count
            != owned_value->associated_availability_count
        || !cm_hir_library_function_array_shapes_valid(function)
        || function->parameter_types != owned_value->parameter_types
        || function->predicate_scopes != owned_value->predicate_scopes
        || function->predicates != owned_value->predicates
        || function->outlives_predicates
            != owned_value->outlives_predicates
        || function->nominal_references != owned_value->nominal_references
        || function->associated_availability
            != owned_value->associated_availability
        || !cm_hir_library_array_shape_valid(
            owned_value->predicate_scope_lifetimes,
            function->predicate_scope_count, sizeof(CmInternId *))
        || !cm_hir_library_array_shape_valid(
            owned_value->predicate_arguments, function->predicate_count,
            sizeof(CmHirGenericArg *))
        || !cm_hir_library_array_shape_valid(
            owned_value->predicate_equalities, function->predicate_count,
            sizeof(CmHirAssociatedTypeEquality *))
        || !cm_hir_library_array_shape_valid(
            owned_value->predicate_lifetimes, function->predicate_count,
            sizeof(CmInternId *))
        || !cm_hir_library_array_shape_valid(
            owned_value->nominal_reference_generic_kinds,
            function->nominal_reference_count,
            sizeof(CmHirGenericParamKind *))
        || !cm_hir_library_array_shape_valid(
            owned_value->nominal_reference_names,
            function->nominal_reference_count, sizeof(CmInternId))) return 0;
    for (index = 0u; index < function->predicate_scope_count; ++index) {
        if (function->predicate_scopes[index].binder.lifetimes
                != owned_value->predicate_scope_lifetimes[index]) return 0;
    }
    for (index = 0u; index < function->predicate_count; ++index) {
        if (function->predicates[index].trait_type.arguments
                != owned_value->predicate_arguments[index]
            || function->predicates[index].equalities
                != owned_value->predicate_equalities[index]
            || function->predicates[index].binder.lifetimes
                != owned_value->predicate_lifetimes[index]) return 0;
    }
    for (index = 0u; index < function->nominal_reference_count; ++index) {
        const CmInternedString *owned_name;

        owned_name = cm_interner_get(names,
            owned_value->nominal_reference_names[index]);
        if (owned_name == NULL
            || function->nominal_references[index].name.bytes
                != owned_name->bytes
            || function->nominal_references[index].name.length
                != owned_name->len
            || function->nominal_references[index].generic_parameter_kinds
                != owned_value->nominal_reference_generic_kinds[index]) {
            return 0;
        }
    }
    return 1;
}

static const CmHirLibraryNominalReference *
cm_hir_library_find_nominal_reference(
    const CmHirLibraryFunctionSignature *function, CmHirDefId definition,
    CmHirLibraryNominalReferenceKind kind)
{
    uint32_t low;
    uint32_t high;

    low = 0u;
    high = function->nominal_reference_count;
    while (low < high) {
        uint32_t middle;
        const CmHirLibraryNominalReference *reference;
        int order;

        middle = low + (high - low) / 2u;
        reference = &function->nominal_references[middle];
        order = cm_hir_library_definition_compare(reference->definition,
            definition);
        if (reference->kind == kind
            && order == 0) return reference;
        if (order > 0 || (order == 0 && reference->kind > kind)) high = middle;
        else low = middle + 1u;
    }
    return NULL;
}

static const CmHirLibraryNominalReference *
cm_hir_library_find_trait_like_nominal_reference(
    const CmHirLibraryFunctionSignature *function, CmHirDefId definition)
{
    const CmHirLibraryNominalReference *reference;

    reference = cm_hir_library_find_nominal_reference(function, definition,
        CM_HIR_LIBRARY_NOMINAL_TRAIT);
    return reference != NULL ? reference
        : cm_hir_library_find_nominal_reference(function, definition,
            CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS);
}

static int cm_hir_library_reference_schema_valid(
    const CmHirContext *context,
    const CmHirLibraryNominalReference *reference)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    uint32_t schema_index;

    definition = cm_hir_lookup_definition(context, reference->definition);
    item = definition == NULL || definition->state != CM_HIR_DEFINITION_BOUND
        || definition->kind != CM_HIR_DEFINITION_ITEM ? NULL
        : cm_hir_get_item(context, definition->entity.item_id);
    if (item == NULL
        || item->generic_parameter_count
            != reference->generic_parameter_count) return 0;
    for (schema_index = 0u;
            schema_index < reference->generic_parameter_count;
            ++schema_index) {
        CmHirGenericParamId parameter_id;
        const CmHirGenericParam *parameter;

        parameter_id = item->generic_parameter_start + schema_index;
        if (parameter_id < item->generic_parameter_start) return 0;
        parameter = cm_hir_get_generic_param(context, parameter_id);
        if (parameter == NULL
            || !cm_hir_def_id_equal(parameter->owner, reference->definition)
            || parameter->index != schema_index
            || parameter->kind
                != reference->generic_parameter_kinds[schema_index]) return 0;
    }
    return 1;
}

static int cm_hir_library_nominal_reference_valid(
    const CmHirContext *context,
    const CmHirLibraryFunctionSignature *function,
    const CmHirLibraryNominalReference *reference)
{
    const CmHirDefinition *definition;
    const CmHirDefinition *owner_definition;
    const CmHirModule *owner_module;
    const CmHirItem *item;
    const CmInternedString *item_name;
    CmHirItemKind expected_kind;

    if (reference->use != CM_HIR_LIBRARY_REFERENCE_ONLY
        || (unsigned int)reference->kind
            > (unsigned int)CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS
        || cm_hir_def_id_is_none(reference->definition)
        || cm_hir_def_id_is_none(reference->owner_module)
        || (reference->generic_parameter_count != 0u
            && reference->generic_parameter_kinds == NULL)) {
        return 0;
    }
    {
        uint32_t schema_index;

        for (schema_index = 0u;
                schema_index < reference->generic_parameter_count;
                ++schema_index) {
            if ((unsigned int)reference->generic_parameter_kinds[schema_index]
                    > (unsigned int)CM_HIR_GENERIC_CONST) return 0;
        }
    }
    owner_definition = cm_hir_lookup_definition(context,
        reference->owner_module);
    owner_module = owner_definition == NULL
        || owner_definition->state != CM_HIR_DEFINITION_BOUND
        || owner_definition->kind != CM_HIR_DEFINITION_MODULE
        ? NULL : cm_hir_get_module(context,
            owner_definition->entity.module_id);
    if (reference->name.bytes == NULL || reference->name.length == 0u
        || owner_module == NULL
        || !cm_hir_def_id_equal(owner_module->definition,
            reference->owner_module)) return 0;
    if (reference->kind == CM_HIR_LIBRARY_NOMINAL_TRAIT) {
        expected_kind = CM_HIR_ITEM_TRAIT;
    } else if (reference->kind == CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS) {
        expected_kind = CM_HIR_ITEM_TRAIT_ALIAS;
    } else {
        expected_kind = CM_HIR_ITEM_TYPE_ALIAS;
    }
    if (reference->kind == CM_HIR_LIBRARY_NOMINAL_TRAIT
        || reference->kind == CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS) {
        if (!cm_hir_def_id_is_none(reference->declaring_trait)) return 0;
    } else if (cm_hir_def_id_is_none(reference->declaring_trait)
        || cm_hir_library_find_nominal_reference(function,
            reference->declaring_trait, CM_HIR_LIBRARY_NOMINAL_TRAIT)
            == NULL) return 0;
    definition = cm_hir_lookup_definition(context, reference->definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || !definition->has_reserved_item_kind
        || definition->reserved_item_kind != expected_kind) return 0;
    if (definition->state == CM_HIR_DEFINITION_RESERVED) return 1;
    if (definition->state != CM_HIR_DEFINITION_BOUND) return 0;
    if (!cm_hir_library_reference_schema_valid(context, reference)) return 0;
    item = cm_hir_get_item(context, definition->entity.item_id);
    if (item == NULL || item->kind != expected_kind
        || !cm_hir_def_id_equal(item->definition,
            reference->definition)) return 0;
    item_name = cm_interner_get(&context->strings, item->name);
    owner_module = cm_hir_get_module(context, item->owner_module);
    if (item_name == NULL || owner_module == NULL
        || !cm_hir_def_id_equal(owner_module->definition,
            reference->owner_module)
        || reference->name.length != item_name->len
        || memcmp(reference->name.bytes, item_name->bytes,
            reference->name.length) != 0) return 0;
    if (reference->kind == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) {
        return item->data.type_alias_item.target == CM_HIR_TYPE_NONE
            && cm_hir_def_id_equal(item->parent_definition,
                reference->declaring_trait);
    }
    return cm_hir_def_id_is_none(item->parent_definition);
}

static int cm_hir_library_availability_find(
    const CmHirLibraryFunctionSignature *function, CmHirDefId direct_trait,
    CmHirDefId associated_type, uint32_t *out_index)
{
    uint32_t low;
    uint32_t high;

    low = 0u;
    high = function->associated_availability_count;
    while (low < high) {
        uint32_t middle;
        const CmHirLibraryAssociatedAvailability *availability;
        int order;

        middle = low + (high - low) / 2u;
        availability = &function->associated_availability[middle];
        order = cm_hir_library_definition_compare(
            availability->direct_trait, direct_trait);
        if (order == 0) order = cm_hir_library_definition_compare(
            availability->associated_type, associated_type);
        if (order == 0) {
            if (out_index != NULL) *out_index = middle;
            return 1;
        }
        if (order > 0) high = middle;
        else low = middle + 1u;
    }
    return 0;
}

static int cm_hir_library_function_references_structurally_valid(
    const CmHirContext *context,
    const CmHirLibraryFunctionSignature *function)
{
    uint32_t index;
    unsigned char *availability_witnessed;

    for (index = 0u; index < function->nominal_reference_count; ++index) {
        if (!cm_hir_library_nominal_reference_valid(context, function,
                &function->nominal_references[index])
            || (index != 0u && cm_hir_library_nominal_reference_compare(
                &function->nominal_references[index - 1u],
                &function->nominal_references[index]) >= 0)) return 0;
    }
    for (index = 0u; index < function->associated_availability_count;
            ++index) {
        const CmHirLibraryAssociatedAvailability *availability;
        availability = &function->associated_availability[index];
        if (cm_hir_library_find_nominal_reference(function,
                availability->direct_trait,
                CM_HIR_LIBRARY_NOMINAL_TRAIT) == NULL
            || cm_hir_library_find_nominal_reference(function,
                availability->associated_type,
                CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) == NULL
            || (index != 0u && cm_hir_library_availability_compare(
                &function->associated_availability[index - 1u],
                availability) >= 0)) return 0;
    }
    availability_witnessed = (unsigned char *)cm_alloc_zeroed(
        function->associated_availability_count, sizeof(unsigned char));
    for (index = 0u; index < function->predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;
        uint32_t equality_index;

        predicate = &function->predicates[index];
        if (cm_hir_library_find_trait_like_nominal_reference(function,
                predicate->trait_type.definition) == NULL) {
            cm_free(availability_witnessed);
            return 0;
        }
        for (equality_index = 0u; equality_index < predicate->equality_count;
                ++equality_index) {
            uint32_t availability_index;

            if (cm_hir_library_find_nominal_reference(function,
                    predicate->equalities[equality_index].associated_type,
                    CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE) == NULL
                || !cm_hir_library_availability_find(function,
                    predicate->trait_type.definition,
                    predicate->equalities[equality_index].associated_type,
                    &availability_index)) {
                cm_free(availability_witnessed);
                return 0;
            }
            availability_witnessed[availability_index] = UINT8_C(1);
        }
    }
    for (index = 0u; index < function->associated_availability_count;
            ++index) {
        if (availability_witnessed[index] == 0u) {
            cm_free(availability_witnessed);
            return 0;
        }
    }
    cm_free(availability_witnessed);
    return 1;
}

static int cm_hir_library_function_references_match_item(
    const CmHirContext *context, const CmInterner *names,
    const CmHirLibraryFunctionSignature *function, const CmHirItem *item)
{
    CmVec references;
    CmVec availability;
    CmHirLibraryFunctionSignature expected;
    int result;

    memset(&references, 0, sizeof(references));
    memset(&availability, 0, sizeof(availability));
    if (!cm_hir_library_collect_nominal_references(context, names, NULL, item,
            &references, &availability)
        || references.len > (size_t)UINT32_MAX
        || availability.len > (size_t)UINT32_MAX) {
        cm_hir_library_nominal_reference_vec_destroy(&references);
        cm_vec_destroy(&availability);
        return 0;
    }
    expected = *function;
    expected.nominal_references =
        (const CmHirLibraryNominalReference *)references.data;
    expected.nominal_reference_count = (uint32_t)references.len;
    expected.associated_availability =
        (const CmHirLibraryAssociatedAvailability *)availability.data;
    expected.associated_availability_count = (uint32_t)availability.len;
    result = cm_hir_library_function_predicates_equal(function, &expected);
    cm_hir_library_nominal_reference_vec_destroy(&references);
    cm_vec_destroy(&availability);
    return result;
}

static int cm_hir_library_type_binder_requirement_cached(
    const CmHirContext *context, CmHirTypeId id, size_t depth,
    uint32_t *requirements, unsigned char *states,
    uint32_t *out_requirement)
{
    const CmHirType *type;

    (void)depth;
    (void)requirements;
    (void)states;
    if (context == NULL || out_requirement == NULL) return 0;
    type = cm_hir_get_type(context, id);
    if (type == NULL) return 0;
    *out_requirement = type->late_bound_requirement;
    return 1;
}

static int cm_hir_library_type_parameters_owned_cached(
    const CmHirContext *context, CmHirTypeId id, CmHirDefId owner,
    CmHirGenericParamId start, uint32_t count, size_t depth,
    int predicate_root, unsigned char *seen)
{
    const CmHirType *type;
    uint32_t index;

    if (id == CM_HIR_TYPE_NONE
        || depth >= (size_t)CM_META_MAX_TYPE_NESTING) return 0;
    if ((size_t)id > context->types.len) return 0;
    if (seen[id - 1u] != 0u) return 1;
    seen[id - 1u] = UINT8_C(1);
    type = cm_hir_get_type(context, id);
    if (type == NULL) return 0;
#define CM_LIBRARY_OWNED_TYPE(child_id) \
    cm_hir_library_type_parameters_owned_cached(context, (child_id), owner, \
        start, count, depth + 1u, predicate_root, seen)
#define CM_LIBRARY_GENERIC_OWNED(parameter_id, expected_kind) do { \
        const CmHirGenericParam *owned_parameter; \
        CmHirGenericParamId owned_id; \
        owned_id = (parameter_id); \
        owned_parameter = cm_hir_get_generic_param(context, owned_id); \
        if (owned_parameter == NULL \
            || owned_parameter->kind != (expected_kind) \
            || !cm_hir_def_id_equal(owned_parameter->owner, owner) \
            || count == 0u || owned_id < start \
            || owned_id - start >= count \
            || owned_parameter->index != owned_id - start) return 0; \
    } while (0)
    if (type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        const CmHirGenericParam *parameter;
        CmHirGenericParamId parameter_id;

        parameter_id = type->data.parameter_type.parameter;
        parameter = cm_hir_get_generic_param(context, parameter_id);
        return parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
            && cm_hir_def_id_equal(parameter->owner, owner)
            && count != 0u && parameter_id >= start
            && parameter_id - start < count
            && parameter->index == parameter_id - start;
    }
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        if (predicate_root && type->data.reference_type.region.kind
                == CM_HIR_REGION_EARLY_BOUND) return 0;
        if (type->data.reference_type.region.kind
                == CM_HIR_REGION_EARLY_BOUND) {
            CM_LIBRARY_GENERIC_OWNED(
                type->data.reference_type.region.data.parameter,
                CM_HIR_GENERIC_LIFETIME);
        }
        return CM_LIBRARY_OWNED_TYPE(type->data.reference_type.pointee);
    }
    if (type->kind == CM_HIR_TYPE_RAW_POINTER_KIND)
        return CM_LIBRARY_OWNED_TYPE(type->data.raw_pointer_type.pointee);
    if (type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        for (index = 0u; index < type->data.tuple_type.element_count; ++index)
            if (!CM_LIBRARY_OWNED_TYPE(type->data.tuple_type.elements[index]))
                return 0;
    } else if (type->kind == CM_HIR_TYPE_ARRAY_KIND) {
        if (predicate_root && type->data.array_type.length.kind
                == CM_HIR_CONST_PARAMETER) return 0;
        if (type->data.array_type.length.kind == CM_HIR_CONST_PARAMETER) {
            CM_LIBRARY_GENERIC_OWNED(
                type->data.array_type.length.data.parameter,
                CM_HIR_GENERIC_CONST);
        }
        if (!CM_LIBRARY_OWNED_TYPE(type->data.array_type.element)
            || !CM_LIBRARY_OWNED_TYPE(type->data.array_type.length.type))
            return 0;
    } else if (type->kind == CM_HIR_TYPE_SLICE_KIND) {
        if (!CM_LIBRARY_OWNED_TYPE(type->data.slice_type.element)) return 0;
    } else if (type->kind == CM_HIR_TYPE_FN_POINTER_KIND) {
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!CM_LIBRARY_OWNED_TYPE(
                    type->data.fn_pointer_type.parameters[index])) return 0;
        }
        if (!CM_LIBRARY_OWNED_TYPE(
                type->data.fn_pointer_type.return_type)) return 0;
    } else if (type->kind == CM_HIR_TYPE_ADT_KIND
            || type->kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
            || type->kind == CM_HIR_TYPE_FOREIGN_KIND) {
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            const CmHirGenericArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (predicate_root
                && ((argument->kind == CM_HIR_GENERIC_ARG_LIFETIME
                        && argument->data.lifetime.kind
                            == CM_HIR_REGION_EARLY_BOUND)
                    || (argument->kind == CM_HIR_GENERIC_ARG_CONST
                    && argument->data.constant.kind
                            == CM_HIR_CONST_PARAMETER))) return 0;
            if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME
                && argument->data.lifetime.kind == CM_HIR_REGION_EARLY_BOUND) {
                CM_LIBRARY_GENERIC_OWNED(argument->data.lifetime.data.parameter,
                    CM_HIR_GENERIC_LIFETIME);
            }
            if (argument->kind == CM_HIR_GENERIC_ARG_CONST
                && argument->data.constant.kind == CM_HIR_CONST_PARAMETER) {
                CM_LIBRARY_GENERIC_OWNED(argument->data.constant.data.parameter,
                    CM_HIR_GENERIC_CONST);
            }
            if (argument->kind == CM_HIR_GENERIC_ARG_TYPE
                && !CM_LIBRARY_OWNED_TYPE(argument->data.type)) return 0;
            if (argument->kind == CM_HIR_GENERIC_ARG_CONST
                && !CM_LIBRARY_OWNED_TYPE(argument->data.constant.type))
                return 0;
        }
    }
#undef CM_LIBRARY_OWNED_TYPE
#undef CM_LIBRARY_GENERIC_OWNED
    return 1;
}

static int cm_hir_library_predicate_modifier_valid(
    CmHirTraitPredicateModifier modifier)
{
    return modifier == CM_HIR_PREDICATE_REQUIRED
        || modifier == CM_HIR_PREDICATE_CONST_IF_CONST
        || modifier == CM_HIR_PREDICATE_CONST;
}

static int cm_hir_library_reserved_function_predicates_valid_cached(
    const CmHirContext *context,
    CmHirDefId function_definition,
    const CmHirLibraryFunctionSignature *function,
    uint32_t *requirements, unsigned char *requirement_states,
    unsigned char *signature_seen, unsigned char *predicate_seen)
{
    uint32_t index;

    for (index = 0u; index < function->parameter_count; ++index) {
        uint32_t requirement;

        if (!cm_hir_library_type_binder_requirement_cached(context,
                function->parameter_types[index], 0u, requirements,
                requirement_states, &requirement)
            || requirement != 0u
            || !cm_hir_library_type_parameters_owned_cached(context,
                function->parameter_types[index], function_definition,
                function->generic_parameter_start,
                function->generic_parameter_count, 0u, 0, signature_seen))
            return 0;
    }
    {
        uint32_t requirement;

        if (!cm_hir_library_type_binder_requirement_cached(context,
                function->return_type, 0u, requirements,
                requirement_states, &requirement)
            || requirement != 0u
            || !cm_hir_library_type_parameters_owned_cached(context,
            function->return_type, function_definition,
            function->generic_parameter_start,
            function->generic_parameter_count, 0u, 0, signature_seen))
            return 0;
    }

    if (function->predicate_scope_count != 0u) return 0;
    for (index = 0u; index < function->predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;
        const CmHirLibraryNominalReference *direct;
        const CmHirType *subject_type;
        uint32_t child;
        uint32_t requirement;

        predicate = &function->predicates[index];
        subject_type = cm_hir_get_type(context, predicate->subject);
        direct = cm_hir_library_find_nominal_reference(function,
            predicate->trait_type.definition,
            CM_HIR_LIBRARY_NOMINAL_TRAIT);
        if (direct == NULL)
            direct = cm_hir_library_find_nominal_reference(function,
                predicate->trait_type.definition,
                CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS);
        if (direct == NULL || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
            || !cm_hir_library_predicate_modifier_valid(predicate->modifier)
            || (direct->kind == CM_HIR_LIBRARY_NOMINAL_TRAIT_ALIAS
                && predicate->equality_count != 0u)
            || subject_type == NULL
            || subject_type->kind != CM_HIR_TYPE_PARAMETER_KIND
            || predicate->trait_type.argument_count
                != direct->generic_parameter_count
            || !cm_hir_library_type_binder_requirement_cached(context,
                predicate->subject, 0u, requirements, requirement_states,
                &requirement)
            || requirement != 0u
            || !cm_hir_library_type_parameters_owned_cached(context,
                predicate->subject, function_definition,
                function->generic_parameter_start,
                function->generic_parameter_count, 0u, 1, predicate_seen))
            return 0;
        for (child = 0u; child < direct->generic_parameter_count; ++child) {
            if (direct->generic_parameter_kinds[child]
                    != CM_HIR_GENERIC_TYPE) return 0;
        }
        for (child = 0u; child < predicate->binder.lifetime_count; ++child) {
            const CmInternedString *name;

            name = cm_interner_get(&context->strings,
                predicate->binder.lifetimes[child]);
            if (name == NULL || name->len == 0u
                || predicate->binder.span.source == 0u
                || predicate->binder.span.start
                    >= predicate->binder.span.end
                || predicate->binder.span.source != predicate->span.source
                || predicate->binder.span.start < predicate->span.start
                || predicate->binder.span.end > predicate->span.end) return 0;
        }
        if (predicate->binder.lifetime_count > 1u) {
            CmInternId *sorted_lifetimes;

            sorted_lifetimes = (CmInternId *)cm_alloc(
                (size_t)predicate->binder.lifetime_count
                    * sizeof(CmInternId));
            memcpy(sorted_lifetimes, predicate->binder.lifetimes,
                (size_t)predicate->binder.lifetime_count
                    * sizeof(CmInternId));
            qsort(sorted_lifetimes, predicate->binder.lifetime_count,
                sizeof(CmInternId), cm_hir_library_intern_id_compare);
            for (child = 1u; child < predicate->binder.lifetime_count;
                    ++child) {
                if (sorted_lifetimes[child - 1u]
                        == sorted_lifetimes[child]) {
                    cm_free(sorted_lifetimes);
                    return 0;
                }
            }
            cm_free(sorted_lifetimes);
        }
        if (predicate->binder.lifetime_count == 0u
            && (predicate->binder.span.source != 0u
                || predicate->binder.span.start != 0u
                || predicate->binder.span.end != 0u)) return 0;
        for (child = 0u; child < predicate->trait_type.argument_count;
                ++child) {
            if (predicate->trait_type.arguments[child].kind
                    != CM_HIR_GENERIC_ARG_TYPE
                || !cm_hir_library_type_binder_requirement_cached(context,
                    predicate->trait_type.arguments[child].data.type, 0u,
                    requirements, requirement_states, &requirement)
                || requirement > predicate->binder.lifetime_count
                || !cm_hir_library_type_parameters_owned_cached(context,
                    predicate->trait_type.arguments[child].data.type,
                    function_definition, function->generic_parameter_start,
                    function->generic_parameter_count, 0u, 1,
                    predicate_seen)) return 0;
        }
        for (child = 0u; child < predicate->equality_count; ++child) {
            const CmHirLibraryNominalReference *associated;

            associated = cm_hir_library_find_nominal_reference(function,
                predicate->equalities[child].associated_type,
                CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE);
            if (associated == NULL
                || associated->generic_parameter_count != 0u
                || !cm_hir_library_type_binder_requirement_cached(context,
                    predicate->equalities[child].value, 0u, requirements,
                    requirement_states, &requirement)
                || requirement > predicate->binder.lifetime_count
                || !cm_hir_library_type_parameters_owned_cached(context,
                    predicate->equalities[child].value,
                    function_definition, function->generic_parameter_start,
                    function->generic_parameter_count, 0u, 1,
                    predicate_seen)) return 0;
        }
        if (predicate->equality_count > 1u) {
            CmHirDefId *sorted_equalities;

            sorted_equalities = (CmHirDefId *)cm_alloc(
                (size_t)predicate->equality_count * sizeof(CmHirDefId));
            for (child = 0u; child < predicate->equality_count; ++child)
                sorted_equalities[child] =
                    predicate->equalities[child].associated_type;
            qsort(sorted_equalities, predicate->equality_count,
                sizeof(CmHirDefId), cm_hir_library_def_id_qsort_compare);
            for (child = 1u; child < predicate->equality_count; ++child) {
                if (cm_hir_def_id_equal(sorted_equalities[child - 1u],
                        sorted_equalities[child])) {
                    cm_free(sorted_equalities);
                    return 0;
                }
            }
            cm_free(sorted_equalities);
        }
    }
    for (index = 0u; index < function->outlives_predicate_count; ++index) {
        const CmHirOutlivesPredicate *outlives;
        const CmHirType *subject_type;
        uint32_t requirement;

        outlives = &function->outlives_predicates[index];
        subject_type = outlives->subject_kind == CM_HIR_OUTLIVES_TYPE
            ? cm_hir_get_type(context, outlives->subject.type) : NULL;
        if (outlives->scope != CM_HIR_PREDICATE_SCOPE_NONE
            || outlives->subject_kind != CM_HIR_OUTLIVES_TYPE
            || subject_type == NULL
            || subject_type->kind != CM_HIR_TYPE_PARAMETER_KIND
            || outlives->bound.kind != CM_HIR_REGION_STATIC
            || !cm_hir_library_type_binder_requirement_cached(context,
                outlives->subject.type, 0u, requirements,
                requirement_states, &requirement)
            || requirement != 0u
            || !cm_hir_library_type_parameters_owned_cached(context,
                outlives->subject.type, function_definition,
                function->generic_parameter_start,
                function->generic_parameter_count, 0u, 1, predicate_seen))
            return 0;
    }
    return 1;
}

static int cm_hir_library_reserved_function_predicates_valid(
    const CmHirContext *context, CmHirDefId function_definition,
    const CmHirLibraryFunctionSignature *function)
{
    uint32_t *requirements;
    unsigned char *requirement_states;
    unsigned char *signature_seen;
    unsigned char *predicate_seen;
    int valid;

    requirements = NULL;
    requirement_states = NULL;
    signature_seen = (unsigned char *)cm_alloc_zeroed(context->types.len,
        sizeof(unsigned char));
    predicate_seen = (unsigned char *)cm_alloc_zeroed(context->types.len,
        sizeof(unsigned char));
    valid = cm_hir_library_reserved_function_predicates_valid_cached(context,
        function_definition, function, requirements, requirement_states,
        signature_seen, predicate_seen);
    cm_free(predicate_seen);
    cm_free(signature_seen);
    return valid;
}

static int cm_hir_library_value_shape_equal(const CmHirLibraryValue *value,
    const CmHirContext *context, const CmInterner *names,
    const CmHirItem *item)
{
    uint32_t index;

    if (value == NULL || item == NULL
        || value->kind != cm_hir_library_value_kind(item->kind)
        || !cm_hir_def_id_equal(value->definition, item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || (item->kind == CM_HIR_ITEM_FUNCTION
            && item->data.function_item.has_default_body)
        || ((item->kind == CM_HIR_ITEM_CONST
                || item->kind == CM_HIR_ITEM_STATIC)
            && item->data.value_item.has_default_body)
        || (item->kind != CM_HIR_ITEM_FUNCTION
            && (item->generic_parameter_count != 0u
                || item->predicate_scope_count != 0u
                || item->predicate_count != 0u
                || item->outlives_predicate_count != 0u))) return 0;
    if (value->kind == CM_HIR_LIBRARY_VALUE_FUNCTION) {
        const CmHirFunctionSignature *signature;
        CmHirLibraryFunctionSignature item_function;

        signature = &item->data.function_item.signature;
        if (signature->receiver != CM_HIR_RECEIVER_NONE
            || value->data.function.parameter_count
                != signature->parameter_count
            || value->data.function.return_type != signature->return_type
            || value->data.function.generic_parameter_start
                != item->generic_parameter_start
            || value->data.function.generic_parameter_count
                != item->generic_parameter_count
            || value->data.function.abi != signature->abi
            || value->data.function.safety != signature->safety
            || value->data.function.is_const != signature->is_const
            || value->data.function.is_async != signature->is_async
            || value->data.function.is_variadic != signature->is_variadic) {
            return 0;
        }
        item_function = value->data.function;
        item_function.predicate_scopes = item->predicate_scopes;
        item_function.predicate_scope_count = item->predicate_scope_count;
        item_function.predicates = item->predicates;
        item_function.predicate_count = item->predicate_count;
        item_function.outlives_predicates = item->outlives_predicates;
        item_function.outlives_predicate_count =
            item->outlives_predicate_count;
        if (!cm_hir_library_function_predicates_equal(
                &value->data.function, &item_function)
            || !cm_hir_library_function_references_match_item(context, names,
                &value->data.function, item)) return 0;
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
            || !cm_hir_library_owned_function_storage_valid(
                &state->owned.names, owned_value)
            || !cm_hir_library_function_references_structurally_valid(
                state->context, &value->data.function)
            || (value->data.function.generic_parameter_count == 0u
                ? value->data.function.generic_parameter_start
                    != CM_HIR_GENERIC_PARAM_NONE
                : value->data.function.generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE)
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
        for (index = 0u;
                index < value->data.function.generic_parameter_count;
                ++index) {
            CmHirGenericParamId parameter_id;
            const CmHirGenericParam *parameter;

            parameter_id = value->data.function.generic_parameter_start
                + index;
            if (parameter_id
                    < value->data.function.generic_parameter_start) return 0;
            parameter = cm_hir_get_generic_param(state->context,
                parameter_id);
            if (parameter == NULL
                || !cm_hir_def_id_equal(parameter->owner,
                    value->definition)
                || parameter->index != index) return 0;
        }
        break;
    case CM_HIR_LIBRARY_VALUE_CONST:
        expected_kind = CM_HIR_ITEM_CONST;
        if (owned_value->storage_kind != CM_HIR_LIBRARY_VALUE_CONST
            || owned_value->parameter_count != 0u
            || owned_value->predicate_scope_count != 0u
            || owned_value->predicate_count != 0u
            || owned_value->outlives_predicate_count != 0u
            || owned_value->nominal_reference_count != 0u
            || owned_value->associated_availability_count != 0u
            || owned_value->parameter_types != NULL
            || owned_value->predicate_scopes != NULL
            || owned_value->predicate_scope_lifetimes != NULL
            || owned_value->predicates != NULL
            || owned_value->predicate_arguments != NULL
            || owned_value->predicate_equalities != NULL
            || owned_value->predicate_lifetimes != NULL
            || owned_value->outlives_predicates != NULL
            || owned_value->nominal_references != NULL
            || owned_value->nominal_reference_names != NULL
            || owned_value->nominal_reference_generic_kinds != NULL
            || owned_value->associated_availability != NULL
            || value->data.value.mutability != CM_HIR_IMMUTABLE
            || !cm_hir_library_value_type_valid(state->context,
                value->data.value.type)) return 0;
        break;
    case CM_HIR_LIBRARY_VALUE_STATIC:
        expected_kind = CM_HIR_ITEM_STATIC;
        if (owned_value->storage_kind != CM_HIR_LIBRARY_VALUE_STATIC
            || owned_value->parameter_count != 0u
            || owned_value->predicate_scope_count != 0u
            || owned_value->predicate_count != 0u
            || owned_value->outlives_predicate_count != 0u
            || owned_value->nominal_reference_count != 0u
            || owned_value->associated_availability_count != 0u
            || owned_value->parameter_types != NULL
            || owned_value->predicate_scopes != NULL
            || owned_value->predicate_scope_lifetimes != NULL
            || owned_value->predicates != NULL
            || owned_value->predicate_arguments != NULL
            || owned_value->predicate_equalities != NULL
            || owned_value->predicate_lifetimes != NULL
            || owned_value->outlives_predicates != NULL
            || owned_value->nominal_references != NULL
            || owned_value->nominal_reference_names != NULL
            || owned_value->nominal_reference_generic_kinds != NULL
            || owned_value->associated_availability != NULL
            || (unsigned int)value->data.value.mutability
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
    if (definition->state == CM_HIR_DEFINITION_RESERVED) {
        return value->kind != CM_HIR_LIBRARY_VALUE_FUNCTION
            || cm_hir_library_reserved_function_predicates_valid(
                state->context, value->definition, &value->data.function);
    }
    if (definition->state == CM_HIR_DEFINITION_BOUND) {
        const CmHirItem *item;

        item = cm_hir_get_item(state->context, definition->entity.item_id);
        return cm_hir_library_value_shape_equal(value, state->context,
            &state->owned.names, item);
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

        CmHirItemKind scan_kind;

        if (!cm_hir_library_ast_item_binding_kind(entry->item_kind,
                &hir_item_kind, &binding_kind, &type_kind,
                &value_kind)) return 1;
        matched_item = NULL;
        matches = 0u;
        scan_kind = hir_item_kind;
        for (item_index = 0u; item_index < state->context->items.len;
                ++item_index) {
            const CmHirItem *item;

            item = (const CmHirItem *)cm_vec_at_const(
                &state->context->items, item_index);
            if (item != NULL && item->kind == scan_kind
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
        if (matches == 0u && hir_item_kind == CM_HIR_ITEM_TRAIT) {
            /*
             * `trait Alias = Bound;` lowers under its own item kind while
             * the module graph records it as a trait declaration.
             */
            scan_kind = CM_HIR_ITEM_TRAIT_ALIAS;
            for (item_index = 0u; item_index < state->context->items.len;
                    ++item_index) {
                const CmHirItem *item;

                item = (const CmHirItem *)cm_vec_at_const(
                    &state->context->items, item_index);
                if (item != NULL && item->kind == scan_kind
                    && item->owner_module
                        == artifact_module->capture_hir_module
                    && cm_hir_def_id_is_none(item->parent_definition)
                    && item->definition.crate_id == state->crate_id
                    && item->visibility.kind == CM_HIR_VIS_PUBLIC
                    && cm_hir_library_names_equal(state, name,
                        state->context, item->name)) {
                    matched_item = item;
                    matches += 1u;
                }
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
