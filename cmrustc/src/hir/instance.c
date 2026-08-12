#include "cm/hir/instance.h"

#include "cm/alloc.h"
#include "cm/hir/semantic_results.h"

#include <string.h>

#define CM_INSTANCE_FORMAT_VERSION ((unsigned int)1u)
#define CM_INSTANCE_TYPE_DEPTH ((size_t)128u)

typedef struct CmInstanceBuffer {
    unsigned char *data;
    size_t len;
    size_t cap;
    int sizing;
} CmInstanceBuffer;

typedef struct CmInstanceSubstitution {
    CmHirDefId owner;
    const CmHirGenericArg *arguments;
    uint32_t argument_count;
} CmInstanceSubstitution;

typedef struct CmHirInstanceKeyState {
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    size_t encoded_size;
    unsigned char encoded[1];
} CmHirInstanceKeyState;

static CmHirInstanceStatus cm_instance_write(CmInstanceBuffer *buffer,
    const void *bytes, size_t count)
{
    size_t new_length;

    if (buffer == NULL || (count != 0u && bytes == NULL)
        || !cm_size_add(buffer->len, count, &new_length)) {
        return CM_HIR_INSTANCE_OVERFLOW;
    }
    if (!buffer->sizing) {
        if (new_length > buffer->cap) return CM_HIR_INSTANCE_OVERFLOW;
        if (count != 0u) memcpy(buffer->data + buffer->len, bytes, count);
    }
    buffer->len = new_length;
    return CM_HIR_INSTANCE_OK;
}

static CmHirInstanceStatus cm_instance_u8(CmInstanceBuffer *buffer,
    unsigned int value)
{
    unsigned char byte;

    byte = (unsigned char)(value & 0xffu);
    return cm_instance_write(buffer, &byte, 1u);
}

static CmHirInstanceStatus cm_instance_u32(CmInstanceBuffer *buffer,
    uint32_t value)
{
    unsigned char bytes[4];
    unsigned int index;

    for (index = 0u; index < 4u; ++index) {
        bytes[index] = (unsigned char)((value >> (index * 8u)) & 0xffu);
    }
    return cm_instance_write(buffer, bytes, sizeof(bytes));
}

static CmHirInstanceStatus cm_instance_u64(CmInstanceBuffer *buffer,
    uint64_t value)
{
    unsigned char bytes[8];
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        bytes[index] = (unsigned char)((value >> (index * 8u))
            & UINT64_C(0xff));
    }
    return cm_instance_write(buffer, bytes, sizeof(bytes));
}

static CmHirInstanceStatus cm_instance_def(CmInstanceBuffer *buffer,
    CmHirDefId definition)
{
    CmHirInstanceStatus status;

    status = cm_instance_u32(buffer, definition.crate_id);
    return status == CM_HIR_INSTANCE_OK
        ? cm_instance_u32(buffer, definition.index) : status;
}

static const CmHirItem *cm_instance_item(const CmHirContext *hir,
    CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = hir == NULL ? NULL
        : cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL
            && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static CmHirInstanceStatus cm_instance_encode_type(
    CmInstanceBuffer *buffer, const CmHirContext *hir, CmHirTypeId type_id,
    const CmInstanceSubstitution *substitution, size_t depth);

static CmHirInstanceStatus cm_instance_encode_region(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    const CmHirRegion *region,
    const CmInstanceSubstitution *substitution, size_t depth)
{
    const CmHirGenericParam *parameter;
    const CmHirGenericArg *argument;

    (void)depth;
    if (region == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    if (region->kind == CM_HIR_REGION_STATIC
        || region->kind == CM_HIR_REGION_ERASED) {
        return cm_instance_u8(buffer, (unsigned int)region->kind);
    }
    if (region->kind == CM_HIR_REGION_EARLY_BOUND
        && substitution != NULL) {
        parameter = cm_hir_get_generic_param(hir,
            region->data.parameter);
        if (parameter != NULL
            && parameter->kind == CM_HIR_GENERIC_LIFETIME
            && cm_hir_def_id_equal(parameter->owner, substitution->owner)
            && parameter->index < substitution->argument_count) {
            argument = &substitution->arguments[parameter->index];
            if (argument->kind != CM_HIR_GENERIC_ARG_LIFETIME) {
                return CM_HIR_INSTANCE_INVALID_RELATION;
            }
            return cm_instance_encode_region(buffer, hir,
                &argument->data.lifetime, NULL, depth + 1u);
        }
    }
    return CM_HIR_INSTANCE_UNSUPPORTED_REGION;
}

static CmHirInstanceStatus cm_instance_encode_const(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    const CmHirConstArg *constant,
    const CmInstanceSubstitution *substitution, size_t depth)
{
    const CmHirGenericParam *parameter;
    const CmHirGenericArg *argument;
    CmHirInstanceStatus status;

    if (constant == NULL || depth >= CM_INSTANCE_TYPE_DEPTH) {
        return depth >= CM_INSTANCE_TYPE_DEPTH
            ? CM_HIR_INSTANCE_OVERFLOW : CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    if (constant->kind == CM_HIR_CONST_PARAMETER
        && substitution != NULL) {
        parameter = cm_hir_get_generic_param(hir,
            constant->data.parameter);
        if (parameter != NULL && parameter->kind == CM_HIR_GENERIC_CONST
            && cm_hir_def_id_equal(parameter->owner, substitution->owner)
            && parameter->index < substitution->argument_count) {
            argument = &substitution->arguments[parameter->index];
            if (argument->kind != CM_HIR_GENERIC_ARG_CONST) {
                return CM_HIR_INSTANCE_INVALID_RELATION;
            }
            return cm_instance_encode_const(buffer, hir,
                &argument->data.constant, NULL, depth + 1u);
        }
    }
    if (constant->kind != CM_HIR_CONST_VALUE) {
        return CM_HIR_INSTANCE_UNSUPPORTED_CONST;
    }
    status = cm_instance_u8(buffer, (unsigned int)CM_HIR_CONST_VALUE);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_encode_type(buffer, hir, constant->type,
            substitution, depth + 1u);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_u64(buffer, constant->data.value.low_bits);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_u64(buffer, constant->data.value.high_bits);
    }
    return status;
}

static CmHirInstanceStatus cm_instance_encode_argument(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    const CmHirGenericArg *argument,
    const CmInstanceSubstitution *substitution, size_t depth)
{
    CmHirInstanceStatus status;

    if (argument == NULL || depth >= CM_INSTANCE_TYPE_DEPTH) {
        return depth >= CM_INSTANCE_TYPE_DEPTH
            ? CM_HIR_INSTANCE_OVERFLOW : CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    status = cm_instance_u8(buffer, (unsigned int)argument->kind);
    if (status != CM_HIR_INSTANCE_OK) return status;
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_instance_encode_region(buffer, hir,
            &argument->data.lifetime, substitution, depth + 1u);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_instance_encode_type(buffer, hir, argument->data.type,
            substitution, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_instance_encode_const(buffer, hir,
            &argument->data.constant, substitution, depth + 1u);
    }
    return CM_HIR_INSTANCE_INVALID_ARGUMENT;
}

static CmHirInstanceStatus cm_instance_encode_arguments(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    const CmHirGenericArg *arguments, uint32_t argument_count,
    const CmInstanceSubstitution *substitution, size_t depth)
{
    CmHirInstanceStatus status;
    uint32_t index;

    if ((argument_count == 0u) != (arguments == NULL)) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    status = cm_instance_u32(buffer, argument_count);
    for (index = 0u; status == CM_HIR_INSTANCE_OK
            && index < argument_count; ++index) {
        status = cm_instance_encode_argument(buffer, hir,
            &arguments[index], substitution, depth + 1u);
    }
    return status;
}

static CmHirInstanceStatus cm_instance_encode_named(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    const CmHirNamedType *named,
    const CmInstanceSubstitution *substitution, size_t depth)
{
    const CmHirItem *item;
    CmHirInstanceStatus status;

    if (named == NULL || depth >= CM_INSTANCE_TYPE_DEPTH) {
        return depth >= CM_INSTANCE_TYPE_DEPTH
            ? CM_HIR_INSTANCE_OVERFLOW : CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    item = cm_instance_item(hir, named->definition);
    if (item == NULL) return CM_HIR_INSTANCE_INVALID_ID;
    status = cm_instance_def(buffer, named->definition);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_encode_arguments(buffer, hir,
            named->arguments, named->argument_count, substitution,
            depth + 1u);
    }
    return status;
}

static CmHirInstanceStatus cm_instance_encode_type(
    CmInstanceBuffer *buffer, const CmHirContext *hir, CmHirTypeId type_id,
    const CmInstanceSubstitution *substitution, size_t depth)
{
    const CmHirType *type;
    const CmHirGenericParam *parameter;
    const CmHirGenericArg *argument;
    const CmHirItem *adt;
    CmHirInstanceStatus status;
    uint32_t index;

    if (depth >= CM_INSTANCE_TYPE_DEPTH) return CM_HIR_INSTANCE_OVERFLOW;
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return CM_HIR_INSTANCE_INVALID_ID;
    if (type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && substitution != NULL) {
        parameter = cm_hir_get_generic_param(hir,
            type->data.parameter_type.parameter);
        if (parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
            && cm_hir_def_id_equal(parameter->owner, substitution->owner)
            && parameter->index < substitution->argument_count) {
            argument = &substitution->arguments[parameter->index];
            if (argument->kind != CM_HIR_GENERIC_ARG_TYPE) {
                return CM_HIR_INSTANCE_INVALID_RELATION;
            }
            return cm_instance_encode_type(buffer, hir,
                argument->data.type, NULL, depth + 1u);
        }
    }
    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
        return CM_HIR_INSTANCE_UNSUPPORTED_TYPE;
    case CM_HIR_TYPE_PARAMETER_KIND:
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return CM_HIR_INSTANCE_UNSUPPORTED_TYPE;
    default:
        break;
    }
    status = cm_instance_u8(buffer, (unsigned int)type->kind);
    if (status != CM_HIR_INSTANCE_OK) return status;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return CM_HIR_INSTANCE_OK;
    case CM_HIR_TYPE_INTEGER_KIND:
        return cm_instance_u8(buffer,
            (unsigned int)type->data.integer_type.kind);
    case CM_HIR_TYPE_FLOAT_KIND:
        return cm_instance_u8(buffer,
            (unsigned int)type->data.float_type.kind);
    case CM_HIR_TYPE_REFERENCE_KIND:
        status = cm_instance_encode_region(buffer, hir,
            &type->data.reference_type.region, substitution, depth + 1u);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_u8(buffer,
                (unsigned int)type->data.reference_type.mutability);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_encode_type(buffer, hir,
                type->data.reference_type.pointee, substitution,
                depth + 1u);
        }
        return status;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        status = cm_instance_u8(buffer,
            (unsigned int)type->data.raw_pointer_type.mutability);
        return status == CM_HIR_INSTANCE_OK
            ? cm_instance_encode_type(buffer, hir,
                type->data.raw_pointer_type.pointee, substitution,
                depth + 1u) : status;
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        status = cm_instance_u32(buffer,
            type->data.tuple_type.element_count);
        for (index = 0u; status == CM_HIR_INSTANCE_OK
                && index < type->data.tuple_type.element_count; ++index) {
            status = cm_instance_encode_type(buffer, hir,
                type->data.tuple_type.elements[index], substitution,
                depth + 1u);
        }
        return status;
    case CM_HIR_TYPE_ARRAY_KIND:
        status = cm_instance_encode_type(buffer, hir,
            type->data.array_type.element, substitution, depth + 1u);
        return status == CM_HIR_INSTANCE_OK
            ? cm_instance_encode_const(buffer, hir,
                &type->data.array_type.length, substitution, depth + 1u)
            : status;
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_instance_encode_type(buffer, hir,
            type->data.slice_type.element, substitution, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        status = cm_instance_u32(buffer,
            type->data.fn_pointer_type.parameter_count);
        for (index = 0u; status == CM_HIR_INSTANCE_OK
                && index < type->data.fn_pointer_type.parameter_count;
             ++index) {
            status = cm_instance_encode_type(buffer, hir,
                type->data.fn_pointer_type.parameters[index], substitution,
                depth + 1u);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_encode_type(buffer, hir,
                type->data.fn_pointer_type.return_type, substitution,
                depth + 1u);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_u32(buffer,
                type->data.fn_pointer_type.abi);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_u8(buffer,
                (unsigned int)type->data.fn_pointer_type.safety);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_u8(buffer,
                type->data.fn_pointer_type.is_variadic ? 1u : 0u);
        }
        return status;
    case CM_HIR_TYPE_ADT_KIND:
        adt = cm_instance_item(hir, type->data.named_type.definition);
        if (adt == NULL || (adt->kind != CM_HIR_ITEM_STRUCT
                && adt->kind != CM_HIR_ITEM_UNION
                && adt->kind != CM_HIR_ITEM_ENUM)
            || adt->generic_parameter_count
                != type->data.named_type.argument_count) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        return cm_instance_encode_named(buffer, hir,
            &type->data.named_type, substitution, depth + 1u);
    default:
        return CM_HIR_INSTANCE_UNSUPPORTED_TYPE;
    }
}

static CmHirInstanceStatus cm_instance_validate_arguments(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    const CmHirItem *owner, const CmHirGenericArg *arguments,
    uint32_t argument_count, unsigned int section)
{
    const CmHirGenericParam *parameter;
    CmHirGenericArgKind expected;
    CmHirInstanceStatus status;
    uint32_t index;

    if (owner == NULL || owner->generic_parameter_count != argument_count
        || (argument_count == 0u) != (arguments == NULL)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    status = cm_instance_u8(buffer, section);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_u32(buffer, argument_count);
    }
    for (index = 0u; status == CM_HIR_INSTANCE_OK
            && index < argument_count; ++index) {
        parameter = cm_hir_get_generic_param(hir,
            owner->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner, owner->definition)) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        expected = parameter->kind == CM_HIR_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_ARG_LIFETIME
            : parameter->kind == CM_HIR_GENERIC_TYPE
                ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (arguments[index].kind != expected) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        status = cm_instance_encode_argument(buffer, hir,
            &arguments[index], NULL, 0u);
    }
    return status;
}

static CmHirInstanceStatus cm_instance_encode_spec(CmInstanceBuffer *buffer,
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirInstanceSpec *spec)
{
    const CmHirItem *selected;
    const CmHirItem *declared;
    const CmHirItem *enclosing;
    const CmHirItem *trait_item;
    CmInstanceSubstitution impl_substitution;
    CmInstanceBuffer expected;
    CmInstanceBuffer actual;
    CmHirInstanceStatus status;

    selected = cm_instance_item(hir, spec->selected_callable);
    if (selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION
        || selected->definition.crate_id != local_crate) {
        return selected == NULL ? CM_HIR_INSTANCE_INVALID_ID
            : CM_HIR_INSTANCE_FOREIGN_ADMISSION;
    }
    status = cm_instance_u8(buffer, CM_INSTANCE_FORMAT_VERSION);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, spec->selected_callable);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, spec->declared_trait_callable);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, spec->enclosing_impl);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, spec->implemented_trait);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, spec->self_owner);
    }
    if (status != CM_HIR_INSTANCE_OK) return status;

    if (cm_hir_def_id_is_none(selected->parent_definition)) {
        if (!cm_hir_def_id_is_none(spec->declared_trait_callable)
            || !cm_hir_def_id_is_none(spec->enclosing_impl)
            || !cm_hir_def_id_is_none(spec->implemented_trait)
            || !cm_hir_def_id_is_none(spec->self_owner)
            || spec->self_type != CM_HIR_TYPE_NONE
            || spec->method_argument_count != 0u
            || spec->method_arguments != NULL
            || spec->enclosing_impl_argument_count != 0u
            || spec->enclosing_impl_arguments != NULL
            || spec->implemented_trait_argument_count != 0u
            || spec->implemented_trait_arguments != NULL) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        return cm_instance_validate_arguments(buffer, hir, selected,
            spec->item_arguments, spec->item_argument_count, 1u);
    }

    if (spec->item_argument_count != 0u || spec->item_arguments != NULL) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    enclosing = cm_instance_item(hir, spec->enclosing_impl);
    declared = cm_instance_item(hir, spec->declared_trait_callable);
    trait_item = cm_instance_item(hir, spec->implemented_trait);
    if (enclosing == NULL || enclosing->kind != CM_HIR_ITEM_IMPL
        || !cm_hir_def_id_equal(selected->parent_definition,
            enclosing->definition)
        || !enclosing->data.impl_item.has_trait
        || enclosing->data.impl_item.is_negative
        || declared == NULL || declared->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_def_id_equal(
            selected->data.function_item.trait_item_definition,
            declared->definition)
        || trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || !cm_hir_def_id_equal(declared->parent_definition,
            trait_item->definition)
        || !cm_hir_def_id_equal(
            enclosing->data.impl_item.trait_type.definition,
            trait_item->definition)
        || !cm_hir_def_id_equal(spec->self_owner, enclosing->definition)
        || spec->self_type == CM_HIR_TYPE_NONE) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    status = cm_instance_validate_arguments(buffer, hir, selected,
        spec->method_arguments, spec->method_argument_count, 2u);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_validate_arguments(buffer, hir, enclosing,
            spec->enclosing_impl_arguments,
            spec->enclosing_impl_argument_count, 3u);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_validate_arguments(buffer, hir, trait_item,
            spec->implemented_trait_arguments,
            spec->implemented_trait_argument_count, 4u);
    }
    if (status != CM_HIR_INSTANCE_OK) return status;
    memset(&impl_substitution, 0, sizeof(impl_substitution));
    impl_substitution.owner = enclosing->definition;
    impl_substitution.arguments = spec->enclosing_impl_arguments;
    impl_substitution.argument_count = spec->enclosing_impl_argument_count;

    if (buffer->sizing) {
        unsigned char *expected_bytes;
        unsigned char *actual_bytes;
        size_t workspace_size;

        expected = *buffer;
        expected.sizing = 1;
        expected.len = 0u;
        actual = expected;
        status = cm_instance_encode_type(&expected, hir,
            enclosing->data.impl_item.self_type, &impl_substitution, 0u);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_encode_type(&actual, hir, spec->self_type,
                NULL, 0u);
        }
        if (status != CM_HIR_INSTANCE_OK || expected.len != actual.len) {
            return status == CM_HIR_INSTANCE_OK
                ? CM_HIR_INSTANCE_INVALID_RELATION : status;
        }
        if (!cm_size_add(expected.len, actual.len, &workspace_size)) {
            return CM_HIR_INSTANCE_OVERFLOW;
        }
        expected_bytes = (unsigned char *)cm_alloc(workspace_size);
        actual_bytes = expected_bytes + expected.len;
        expected.data = expected_bytes;
        expected.cap = expected.len;
        expected.len = 0u;
        expected.sizing = 0;
        actual.data = actual_bytes;
        actual.cap = actual.len;
        actual.len = 0u;
        actual.sizing = 0;
        status = cm_instance_encode_type(&expected, hir,
            enclosing->data.impl_item.self_type, &impl_substitution, 0u);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_encode_type(&actual, hir, spec->self_type,
                NULL, 0u);
        }
        if (status == CM_HIR_INSTANCE_OK
            && (expected.len != actual.len
                || memcmp(expected.data, actual.data, expected.len) != 0)) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
        }
        cm_free(expected_bytes);
        if (status != CM_HIR_INSTANCE_OK) return status;

        expected = *buffer;
        expected.sizing = 1;
        expected.len = 0u;
        actual = expected;
        status = cm_instance_encode_arguments(&expected, hir,
            enclosing->data.impl_item.trait_type.arguments,
            enclosing->data.impl_item.trait_type.argument_count,
            &impl_substitution, 0u);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_encode_arguments(&actual, hir,
                spec->implemented_trait_arguments,
                spec->implemented_trait_argument_count, NULL, 0u);
        }
        if (status != CM_HIR_INSTANCE_OK || expected.len != actual.len) {
            return status == CM_HIR_INSTANCE_OK
                ? CM_HIR_INSTANCE_INVALID_RELATION : status;
        }
        if (!cm_size_add(expected.len, actual.len, &workspace_size)) {
            return CM_HIR_INSTANCE_OVERFLOW;
        }
        expected_bytes = (unsigned char *)cm_alloc(workspace_size);
        actual_bytes = expected_bytes + expected.len;
        expected.data = expected_bytes;
        expected.cap = expected.len;
        expected.len = 0u;
        expected.sizing = 0;
        actual.data = actual_bytes;
        actual.cap = actual.len;
        actual.len = 0u;
        actual.sizing = 0;
        status = cm_instance_encode_arguments(&expected, hir,
            enclosing->data.impl_item.trait_type.arguments,
            enclosing->data.impl_item.trait_type.argument_count,
            &impl_substitution, 0u);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_encode_arguments(&actual, hir,
                spec->implemented_trait_arguments,
                spec->implemented_trait_argument_count, NULL, 0u);
        }
        if (status == CM_HIR_INSTANCE_OK
            && (expected.len != actual.len
                || memcmp(expected.data, actual.data, expected.len) != 0)) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
        }
        cm_free(expected_bytes);
        if (status != CM_HIR_INSTANCE_OK) return status;
    }
    status = cm_instance_u8(buffer, 5u);
    return status == CM_HIR_INSTANCE_OK
        ? cm_instance_encode_type(buffer, hir, spec->self_type, NULL, 0u)
        : status;
}

void cm_hir_instance_spec_init(CmHirInstanceSpec *spec)
{
    if (spec == NULL) return;
    memset(spec, 0, sizeof(*spec));
    spec->selected_callable = cm_hir_def_id_none();
    spec->declared_trait_callable = cm_hir_def_id_none();
    spec->enclosing_impl = cm_hir_def_id_none();
    spec->implemented_trait = cm_hir_def_id_none();
    spec->self_owner = cm_hir_def_id_none();
}

CmHirInstanceStatus cm_hir_instance_key_init(CmHirInstanceKey *key,
    const CmSemanticAdmission *admission, const CmHirInstanceSpec *spec)
{
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    CmInstanceBuffer sizing;
    CmInstanceBuffer output;
    CmHirInstanceKeyState *state;
    CmHirInstanceStatus status;
    const CmHirItem *selected;
    const CmSemanticResults *semantic_results;
    CmSemanticBodyView semantic_body;
    size_t allocation_size;

    if (key == NULL || key->state != NULL || admission == NULL
        || spec == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    if (!cm_semantic_admission_is_current(admission)) {
        return CM_HIR_INSTANCE_STALE_ADMISSION;
    }
    hir = cm_semantic_admission_hir(admission);
    local_crate = cm_semantic_admission_crate(admission);
    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE) {
        return CM_HIR_INSTANCE_STALE_ADMISSION;
    }
    selected = cm_instance_item(hir, spec->selected_callable);
    semantic_results = cm_semantic_admission_results(admission);
    if (selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION
        || selected->data.function_item.body == CM_HIR_BODY_NONE
        || semantic_results == NULL
        || cm_semantic_results_body(semantic_results, admission,
            selected->data.function_item.body, &semantic_body)
                != CM_SEMANTIC_RESULTS_OK
        || !cm_hir_def_id_equal(semantic_body.owner,
            spec->selected_callable)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    memset(&sizing, 0, sizeof(sizing));
    sizing.sizing = 1;
    status = cm_instance_encode_spec(&sizing, hir, local_crate, spec);
    if (status != CM_HIR_INSTANCE_OK) return status;
    if (!cm_size_add(sizeof(*state) - 1u, sizing.len,
            &allocation_size)) return CM_HIR_INSTANCE_OVERFLOW;
    state = (CmHirInstanceKeyState *)cm_alloc(allocation_size);
    memset(state, 0, sizeof(*state) - 1u);
    memset(&output, 0, sizeof(output));
    output.data = state->encoded;
    output.cap = sizing.len;
    status = cm_instance_encode_spec(&output, hir, local_crate, spec);
    if (status != CM_HIR_INSTANCE_OK || output.len != sizing.len) {
        cm_free(state);
        return status == CM_HIR_INSTANCE_OK
            ? CM_HIR_INSTANCE_INVALID_RELATION : status;
    }
    state->hir = hir;
    state->local_crate = local_crate;
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->encoded_size = sizing.len;
    key->state = state;
    return CM_HIR_INSTANCE_OK;
}

CmHirInstanceStatus cm_hir_instance_key_validate(
    const CmHirInstanceKey *key, const CmSemanticAdmission *admission)
{
    const CmHirInstanceKeyState *state;
    const CmHirContext *hir;

    if (key == NULL || key->state == NULL || admission == NULL) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    state = (const CmHirInstanceKeyState *)key->state;
    if (!cm_semantic_admission_is_current(admission)) {
        return CM_HIR_INSTANCE_STALE_ADMISSION;
    }
    hir = cm_semantic_admission_hir(admission);
    if (hir != state->hir
        || cm_semantic_admission_crate(admission) != state->local_crate) {
        return CM_HIR_INSTANCE_FOREIGN_ADMISSION;
    }
    if (hir->storage.lifetime_id != state->storage_lifetime_id
        || hir->semantic_generation != state->semantic_generation
        || hir->rewind_generation != state->rewind_generation
        || cm_semantic_admission_generation(admission)
            != state->semantic_generation) {
        return CM_HIR_INSTANCE_STALE_ADMISSION;
    }
    return CM_HIR_INSTANCE_OK;
}

CmHirInstanceStatus cm_hir_instance_key_clone(CmHirInstanceKey *out_key,
    const CmSemanticAdmission *admission,
    const CmHirInstanceKey *source_key)
{
    const CmHirInstanceKeyState *source;
    CmHirInstanceKeyState *copy;
    CmHirInstanceStatus status;
    size_t allocation_size;

    if (out_key == NULL || out_key->state != NULL) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    status = cm_hir_instance_key_validate(source_key, admission);
    if (status != CM_HIR_INSTANCE_OK) return status;
    source = (const CmHirInstanceKeyState *)source_key->state;
    if (!cm_size_add(sizeof(*source) - 1u, source->encoded_size,
            &allocation_size)) return CM_HIR_INSTANCE_OVERFLOW;
    copy = (CmHirInstanceKeyState *)cm_alloc(allocation_size);
    memcpy(copy, source, allocation_size);
    out_key->state = copy;
    return CM_HIR_INSTANCE_OK;
}

void cm_hir_instance_key_destroy(CmHirInstanceKey *key)
{
    if (key == NULL) return;
    cm_free(key->state);
    key->state = NULL;
}

static CmHirInstanceStatus cm_instance_pair_validate(
    const CmSemanticAdmission *admission, const CmHirInstanceKey *left,
    const CmHirInstanceKey *right)
{
    CmHirInstanceStatus status;

    status = cm_hir_instance_key_validate(left, admission);
    return status == CM_HIR_INSTANCE_OK
        ? cm_hir_instance_key_validate(right, admission) : status;
}

CmHirInstanceStatus cm_hir_instance_key_equal(
    const CmSemanticAdmission *admission, const CmHirInstanceKey *left,
    const CmHirInstanceKey *right, int *out_equal)
{
    const CmHirInstanceKeyState *left_state;
    const CmHirInstanceKeyState *right_state;
    CmHirInstanceStatus status;

    if (out_equal == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    *out_equal = 0;
    status = cm_instance_pair_validate(admission, left, right);
    if (status != CM_HIR_INSTANCE_OK) return status;
    left_state = (const CmHirInstanceKeyState *)left->state;
    right_state = (const CmHirInstanceKeyState *)right->state;
    *out_equal = left_state->encoded_size == right_state->encoded_size
        && memcmp(left_state->encoded, right_state->encoded,
            left_state->encoded_size) == 0;
    return CM_HIR_INSTANCE_OK;
}

CmHirInstanceStatus cm_hir_instance_key_compare(
    const CmSemanticAdmission *admission, const CmHirInstanceKey *left,
    const CmHirInstanceKey *right, int *out_order)
{
    const CmHirInstanceKeyState *left_state;
    const CmHirInstanceKeyState *right_state;
    CmHirInstanceStatus status;
    size_t shared;
    int comparison;

    if (out_order == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    *out_order = 0;
    status = cm_instance_pair_validate(admission, left, right);
    if (status != CM_HIR_INSTANCE_OK) return status;
    left_state = (const CmHirInstanceKeyState *)left->state;
    right_state = (const CmHirInstanceKeyState *)right->state;
    shared = left_state->encoded_size < right_state->encoded_size
        ? left_state->encoded_size : right_state->encoded_size;
    comparison = memcmp(left_state->encoded, right_state->encoded, shared);
    if (comparison == 0 && left_state->encoded_size != right_state->encoded_size) {
        comparison = left_state->encoded_size < right_state->encoded_size
            ? -1 : 1;
    }
    *out_order = comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
    return CM_HIR_INSTANCE_OK;
}

CmHirInstanceStatus cm_hir_instance_key_dump(FILE *stream,
    const CmSemanticAdmission *admission, const CmHirInstanceKey *key)
{
    static const char hex[] = "0123456789abcdef";
    const CmHirInstanceKeyState *state;
    CmHirInstanceStatus status;
    size_t index;

    if (stream == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    status = cm_hir_instance_key_validate(key, admission);
    if (status != CM_HIR_INSTANCE_OK) return status;
    state = (const CmHirInstanceKeyState *)key->state;
    fputs("hir-instance-v1 crate=", stream);
    fprintf(stream, "%u generation=%llu key=",
        (unsigned int)state->local_crate,
        (unsigned long long)state->semantic_generation);
    for (index = 0u; index < state->encoded_size; ++index) {
        fputc(hex[state->encoded[index] >> 4u], stream);
        fputc(hex[state->encoded[index] & 0x0fu], stream);
    }
    fputc('\n', stream);
    return ferror(stream) ? CM_HIR_INSTANCE_INVALID_ARGUMENT
        : CM_HIR_INSTANCE_OK;
}

const char *cm_hir_instance_status_name(CmHirInstanceStatus status)
{
    switch (status) {
    case CM_HIR_INSTANCE_OK: return "ok";
    case CM_HIR_INSTANCE_INVALID_ARGUMENT: return "invalid-argument";
    case CM_HIR_INSTANCE_STALE_ADMISSION: return "stale-admission";
    case CM_HIR_INSTANCE_FOREIGN_ADMISSION: return "foreign-admission";
    case CM_HIR_INSTANCE_INVALID_ID: return "invalid-id";
    case CM_HIR_INSTANCE_INVALID_RELATION: return "invalid-relation";
    case CM_HIR_INSTANCE_UNSUPPORTED_TYPE: return "unsupported-type";
    case CM_HIR_INSTANCE_UNSUPPORTED_REGION: return "unsupported-region";
    case CM_HIR_INSTANCE_UNSUPPORTED_CONST: return "unsupported-const";
    case CM_HIR_INSTANCE_OVERFLOW: return "overflow";
    }
    return "unknown";
}
