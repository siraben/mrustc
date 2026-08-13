#include "instance_internal.h"

#include "cm/alloc.h"
#include "cm/hir/semantic_results.h"

#include <string.h>

#define CM_INSTANCE_FORMAT_VERSION ((unsigned int)2u)
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
    const CmHirCanonicalArgumentPart *parts;
    uint32_t argument_count;
} CmInstanceSubstitution;

typedef struct CmInstanceReader {
    const CmHirContext *hir;
    const unsigned char *data;
    size_t len;
    size_t pos;
} CmInstanceReader;

typedef struct CmHirInstanceKeyState {
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    uint64_t admission_capability_id;
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

static CmHirInstanceStatus cm_instance_interned(CmInstanceBuffer *buffer,
    const CmHirContext *hir, CmInternId id)
{
    const CmInternedString *string;
    CmHirInstanceStatus status;

    string = hir == NULL ? NULL : cm_interner_get(&hir->strings, id);
    if (string == NULL || string->len > (size_t)UINT32_MAX) {
        return string == NULL ? CM_HIR_INSTANCE_INVALID_ID
            : CM_HIR_INSTANCE_OVERFLOW;
    }
    status = cm_instance_u32(buffer, (uint32_t)string->len);
    return status == CM_HIR_INSTANCE_OK
        ? cm_instance_write(buffer, string->bytes, string->len) : status;
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

static int cm_instance_trait_callable_relation_valid(
    const CmHirContext *hir, const CmHirItem *selected,
    const CmHirItem *declared, const CmHirItem *enclosing,
    const CmHirItem *trait_item)
{
    const CmHirItem *linked;
    size_t linked_count;
    size_t index;
    int inherited_default;

    if (hir == NULL || selected == NULL || declared == NULL
        || enclosing == NULL || trait_item == NULL
        || selected->kind != CM_HIR_ITEM_FUNCTION
        || declared->kind != CM_HIR_ITEM_FUNCTION
        || enclosing->kind != CM_HIR_ITEM_IMPL
        || trait_item->kind != CM_HIR_ITEM_TRAIT
        || !enclosing->data.impl_item.has_trait
        || enclosing->data.impl_item.is_negative
        || !cm_hir_def_id_equal(declared->parent_definition,
            trait_item->definition)
        || !cm_hir_def_id_is_none(
            declared->data.function_item.trait_item_definition)
        || !cm_hir_def_id_equal(
            enclosing->data.impl_item.trait_type.definition,
            trait_item->definition)) {
        return 0;
    }
    inherited_default = cm_hir_def_id_equal(selected->definition,
        declared->definition);
    if (inherited_default) {
        if (!cm_hir_def_id_equal(selected->parent_definition,
                trait_item->definition)
            || selected->data.function_item.body == CM_HIR_BODY_NONE) {
            return 0;
        }
    } else if (!cm_hir_def_id_equal(selected->parent_definition,
            enclosing->definition)
        || !cm_hir_def_id_equal(
            selected->data.function_item.trait_item_definition,
            declared->definition)) {
        return 0;
    }
    linked = NULL;
    linked_count = 0u;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_FUNCTION
            || !cm_hir_def_id_equal(candidate->parent_definition,
                enclosing->definition)
            || !cm_hir_def_id_equal(candidate->data.function_item
                .trait_item_definition, declared->definition)) {
            continue;
        }
        linked = candidate;
        ++linked_count;
    }
    return inherited_default ? linked_count == 0u
        : linked_count == 1u && linked != NULL
            && cm_hir_def_id_equal(linked->definition,
                selected->definition);
}

static CmHirInstanceStatus cm_instance_encode_type(
    CmInstanceBuffer *buffer, const CmHirContext *hir, CmHirTypeId type_id,
    const CmInstanceSubstitution *substitution, size_t depth);

static CmHirInstanceStatus cm_instance_validate_type_payload(
    CmInstanceReader *reader, size_t depth);

static CmHirInstanceStatus cm_instance_substituted_payload(
    CmInstanceBuffer *buffer, const CmInstanceSubstitution *substitution,
    uint32_t index, CmHirGenericArgKind expected)
{
    const CmHirCanonicalArgumentPart *part;

    if (substitution == NULL || substitution->parts == NULL
        || index >= substitution->argument_count) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    part = &substitution->parts[index];
    if (part->kind != expected || part->bytes == NULL || part->size == 0u) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    return cm_instance_write(buffer, part->bytes, part->size);
}

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
            if (substitution->parts != NULL) {
                return cm_instance_substituted_payload(buffer, substitution,
                    parameter->index, CM_HIR_GENERIC_ARG_LIFETIME);
            }
            argument = substitution->arguments == NULL ? NULL
                : &substitution->arguments[parameter->index];
            if (argument == NULL
                    || argument->kind != CM_HIR_GENERIC_ARG_LIFETIME) {
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
            if (substitution->parts != NULL) {
                return cm_instance_substituted_payload(buffer, substitution,
                    parameter->index, CM_HIR_GENERIC_ARG_CONST);
            }
            argument = substitution->arguments == NULL ? NULL
                : &substitution->arguments[parameter->index];
            if (argument == NULL
                    || argument->kind != CM_HIR_GENERIC_ARG_CONST) {
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
    const CmHirGenericParam *parameter;
    CmHirInstanceStatus status;
    uint32_t index;

    if (named == NULL || depth >= CM_INSTANCE_TYPE_DEPTH) {
        return depth >= CM_INSTANCE_TYPE_DEPTH
            ? CM_HIR_INSTANCE_OVERFLOW : CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    item = cm_instance_item(hir, named->definition);
    if (item == NULL) return CM_HIR_INSTANCE_INVALID_ID;
    if (item->generic_parameter_count != named->argument_count
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    for (index = 0u; index < named->argument_count; ++index) {
        CmHirGenericArgKind expected;

        parameter = cm_hir_get_generic_param(hir,
            item->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner, item->definition)) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        expected = parameter->kind == CM_HIR_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_ARG_LIFETIME
            : parameter->kind == CM_HIR_GENERIC_TYPE
                ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (named->arguments[index].kind != expected) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
    }
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
            if (substitution->parts != NULL) {
                return cm_instance_substituted_payload(buffer, substitution,
                    parameter->index, CM_HIR_GENERIC_ARG_TYPE);
            }
            argument = substitution->arguments == NULL ? NULL
                : &substitution->arguments[parameter->index];
            if (argument == NULL
                    || argument->kind != CM_HIR_GENERIC_ARG_TYPE) {
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
            status = cm_instance_interned(buffer, hir,
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

static CmHirInstanceStatus cm_instance_read(CmInstanceReader *reader,
    void *out, size_t count)
{
    if (reader == NULL || (count != 0u && out == NULL)
        || reader->pos > reader->len || count > reader->len - reader->pos) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    if (count != 0u) memcpy(out, reader->data + reader->pos, count);
    reader->pos += count;
    return CM_HIR_INSTANCE_OK;
}

static CmHirInstanceStatus cm_instance_read_u8(CmInstanceReader *reader,
    unsigned int *out)
{
    unsigned char value;
    CmHirInstanceStatus status;

    if (out == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    status = cm_instance_read(reader, &value, 1u);
    if (status == CM_HIR_INSTANCE_OK) *out = value;
    return status;
}

static CmHirInstanceStatus cm_instance_read_u32(CmInstanceReader *reader,
    uint32_t *out)
{
    unsigned char bytes[4];
    CmHirInstanceStatus status;

    if (out == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    status = cm_instance_read(reader, bytes, sizeof(bytes));
    if (status == CM_HIR_INSTANCE_OK) {
        *out = (uint32_t)bytes[0]
            | ((uint32_t)bytes[1] << 8u)
            | ((uint32_t)bytes[2] << 16u)
            | ((uint32_t)bytes[3] << 24u);
    }
    return status;
}

static CmHirInstanceStatus cm_instance_read_def(CmInstanceReader *reader,
    CmHirDefId *out)
{
    CmHirInstanceStatus status;

    if (out == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    status = cm_instance_read_u32(reader, &out->crate_id);
    return status == CM_HIR_INSTANCE_OK
        ? cm_instance_read_u32(reader, &out->index) : status;
}

static CmHirInstanceStatus cm_instance_validate_region_payload(
    CmInstanceReader *reader)
{
    unsigned int kind;
    CmHirInstanceStatus status;

    status = cm_instance_read_u8(reader, &kind);
    if (status != CM_HIR_INSTANCE_OK) return status;
    return kind == (unsigned int)CM_HIR_REGION_STATIC
            || kind == (unsigned int)CM_HIR_REGION_ERASED
        ? CM_HIR_INSTANCE_OK : CM_HIR_INSTANCE_UNSUPPORTED_REGION;
}

static CmHirInstanceStatus cm_instance_validate_const_payload(
    CmInstanceReader *reader, size_t depth)
{
    unsigned char value[16];
    unsigned int kind;
    CmHirInstanceStatus status;

    if (depth >= CM_INSTANCE_TYPE_DEPTH) return CM_HIR_INSTANCE_OVERFLOW;
    status = cm_instance_read_u8(reader, &kind);
    if (status != CM_HIR_INSTANCE_OK) return status;
    if (kind != (unsigned int)CM_HIR_CONST_VALUE) {
        return CM_HIR_INSTANCE_UNSUPPORTED_CONST;
    }
    status = cm_instance_validate_type_payload(reader, depth + 1u);
    return status == CM_HIR_INSTANCE_OK
        ? cm_instance_read(reader, value, sizeof(value)) : status;
}

static CmHirInstanceStatus cm_instance_validate_argument_payload(
    CmInstanceReader *reader, CmHirGenericArgKind expected, size_t depth)
{
    if (depth >= CM_INSTANCE_TYPE_DEPTH) return CM_HIR_INSTANCE_OVERFLOW;
    switch (expected) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_instance_validate_region_payload(reader);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_instance_validate_type_payload(reader, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_instance_validate_const_payload(reader, depth + 1u);
    }
    return CM_HIR_INSTANCE_INVALID_RELATION;
}

static CmHirInstanceStatus cm_instance_validate_tagged_argument(
    CmInstanceReader *reader, const CmHirItem *owner, uint32_t index,
    size_t depth)
{
    const CmHirGenericParam *parameter;
    CmHirGenericArgKind expected;
    unsigned int kind;
    CmHirInstanceStatus status;

    if (owner == NULL || index >= owner->generic_parameter_count) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    parameter = cm_hir_get_generic_param(reader->hir,
        owner->generic_parameter_start + index);
    if (parameter == NULL || parameter->index != index
        || !cm_hir_def_id_equal(parameter->owner, owner->definition)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    expected = parameter->kind == CM_HIR_GENERIC_LIFETIME
        ? CM_HIR_GENERIC_ARG_LIFETIME
        : parameter->kind == CM_HIR_GENERIC_TYPE
            ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
    status = cm_instance_read_u8(reader, &kind);
    if (status != CM_HIR_INSTANCE_OK) return status;
    return kind == (unsigned int)expected
        ? cm_instance_validate_argument_payload(reader, expected, depth + 1u)
        : CM_HIR_INSTANCE_INVALID_RELATION;
}

static CmHirInstanceStatus cm_instance_validate_type_payload(
    CmInstanceReader *reader, size_t depth)
{
    const CmHirItem *item;
    CmHirDefId definition;
    uint32_t count;
    uint32_t index;
    unsigned int tag;
    unsigned int scalar;
    CmHirInstanceStatus status;

    if (reader == NULL || depth >= CM_INSTANCE_TYPE_DEPTH) {
        return reader == NULL ? CM_HIR_INSTANCE_INVALID_ARGUMENT
            : CM_HIR_INSTANCE_OVERFLOW;
    }
    status = cm_instance_read_u8(reader, &tag);
    if (status != CM_HIR_INSTANCE_OK) return status;
    switch ((CmHirTypeKind)tag) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return CM_HIR_INSTANCE_OK;
    case CM_HIR_TYPE_INTEGER_KIND:
        status = cm_instance_read_u8(reader, &scalar);
        return status != CM_HIR_INSTANCE_OK ? status
            : scalar <= (unsigned int)CM_HIR_INT_USIZE
                ? CM_HIR_INSTANCE_OK : CM_HIR_INSTANCE_INVALID_RELATION;
    case CM_HIR_TYPE_FLOAT_KIND:
        status = cm_instance_read_u8(reader, &scalar);
        return status != CM_HIR_INSTANCE_OK ? status
            : scalar <= (unsigned int)CM_HIR_FLOAT_F128
                ? CM_HIR_INSTANCE_OK : CM_HIR_INSTANCE_INVALID_RELATION;
    case CM_HIR_TYPE_REFERENCE_KIND:
        status = cm_instance_validate_region_payload(reader);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_read_u8(reader, &scalar);
        }
        if (status == CM_HIR_INSTANCE_OK
            && scalar > (unsigned int)CM_HIR_MUTABLE) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
        }
        return status == CM_HIR_INSTANCE_OK
            ? cm_instance_validate_type_payload(reader, depth + 1u) : status;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        status = cm_instance_read_u8(reader, &scalar);
        if (status == CM_HIR_INSTANCE_OK
            && scalar > (unsigned int)CM_HIR_MUTABLE) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
        }
        return status == CM_HIR_INSTANCE_OK
            ? cm_instance_validate_type_payload(reader, depth + 1u) : status;
    case CM_HIR_TYPE_TUPLE_KIND:
        status = cm_instance_read_u32(reader, &count);
        for (index = 0u; status == CM_HIR_INSTANCE_OK && index < count;
             ++index) {
            status = cm_instance_validate_type_payload(reader, depth + 1u);
        }
        return status;
    case CM_HIR_TYPE_ARRAY_KIND:
        status = cm_instance_validate_type_payload(reader, depth + 1u);
        return status == CM_HIR_INSTANCE_OK
            ? cm_instance_validate_const_payload(reader, depth + 1u) : status;
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_instance_validate_type_payload(reader, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        status = cm_instance_read_u32(reader, &count);
        for (index = 0u; status == CM_HIR_INSTANCE_OK && index < count;
             ++index) {
            status = cm_instance_validate_type_payload(reader, depth + 1u);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_validate_type_payload(reader, depth + 1u);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_read_u32(reader, &count);
        }
        if (status == CM_HIR_INSTANCE_OK
            && (reader->pos > reader->len || count > reader->len - reader->pos)) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
        }
        if (status == CM_HIR_INSTANCE_OK) reader->pos += count;
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_read_u8(reader, &scalar);
        }
        if (status == CM_HIR_INSTANCE_OK
            && scalar > (unsigned int)CM_HIR_UNSAFE) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_read_u8(reader, &scalar);
        }
        return status != CM_HIR_INSTANCE_OK ? status
            : scalar <= 1u ? CM_HIR_INSTANCE_OK
                : CM_HIR_INSTANCE_INVALID_RELATION;
    case CM_HIR_TYPE_ADT_KIND:
        status = cm_instance_read_def(reader, &definition);
        item = status == CM_HIR_INSTANCE_OK
            ? cm_instance_item(reader->hir, definition) : NULL;
        if (status != CM_HIR_INSTANCE_OK) return status;
        if (item == NULL || (item->kind != CM_HIR_ITEM_STRUCT
                && item->kind != CM_HIR_ITEM_UNION
                && item->kind != CM_HIR_ITEM_ENUM)) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        status = cm_instance_read_u32(reader, &count);
        if (status != CM_HIR_INSTANCE_OK) return status;
        if (count != item->generic_parameter_count) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        for (index = 0u; status == CM_HIR_INSTANCE_OK && index < count;
             ++index) {
            status = cm_instance_validate_tagged_argument(reader, item,
                index, depth + 1u);
        }
        return status;
    case CM_HIR_TYPE_PARAMETER_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
        return CM_HIR_INSTANCE_UNSUPPORTED_TYPE;
    default:
        return CM_HIR_INSTANCE_UNSUPPORTED_TYPE;
    }
}

static CmHirInstanceStatus cm_instance_validate_payload(
    const CmHirContext *hir, const unsigned char *bytes, size_t size,
    CmHirGenericArgKind kind)
{
    CmInstanceReader reader;
    CmHirInstanceStatus status;

    if (hir == NULL || bytes == NULL || size == 0u) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    memset(&reader, 0, sizeof(reader));
    reader.hir = hir;
    reader.data = bytes;
    reader.len = size;
    status = cm_instance_validate_argument_payload(&reader, kind, 0u);
    return status == CM_HIR_INSTANCE_OK && reader.pos != reader.len
        ? CM_HIR_INSTANCE_INVALID_RELATION : status;
}

static CmHirInstanceStatus cm_instance_encode_part_arguments(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    const CmHirItem *owner, const CmHirCanonicalArgumentPart *arguments,
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
    for (index = 0u; status == CM_HIR_INSTANCE_OK && index < argument_count;
         ++index) {
        parameter = cm_hir_get_generic_param(hir,
            owner->generic_parameter_start + index);
        expected = parameter == NULL ? (CmHirGenericArgKind)-1
            : parameter->kind == CM_HIR_GENERIC_LIFETIME
                ? CM_HIR_GENERIC_ARG_LIFETIME
                : parameter->kind == CM_HIR_GENERIC_TYPE
                    ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner, owner->definition)
            || arguments[index].kind != expected) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        status = cm_instance_validate_payload(hir, arguments[index].bytes,
            arguments[index].size, arguments[index].kind);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_u8(buffer,
                (unsigned int)arguments[index].kind);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_write(buffer, arguments[index].bytes,
                arguments[index].size);
        }
    }
    return status;
}

static CmHirInstanceStatus cm_instance_compare_encoded(
    const CmHirContext *hir, CmHirTypeId type,
    const CmInstanceSubstitution *substitution,
    const unsigned char *actual, size_t actual_size)
{
    CmInstanceBuffer sizing;
    CmInstanceBuffer output;
    unsigned char *bytes;
    CmHirInstanceStatus status;

    if (actual == NULL || actual_size == 0u) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    memset(&sizing, 0, sizeof(sizing));
    sizing.sizing = 1;
    status = cm_instance_encode_type(&sizing, hir, type, substitution, 0u);
    if (status != CM_HIR_INSTANCE_OK || sizing.len != actual_size) {
        return status == CM_HIR_INSTANCE_OK
            ? CM_HIR_INSTANCE_INVALID_RELATION : status;
    }
    bytes = (unsigned char *)cm_alloc(sizing.len);
    memset(&output, 0, sizeof(output));
    output.data = bytes;
    output.cap = sizing.len;
    status = cm_instance_encode_type(&output, hir, type, substitution, 0u);
    if (status == CM_HIR_INSTANCE_OK
        && (output.len != actual_size
            || memcmp(bytes, actual, actual_size) != 0)) {
        status = CM_HIR_INSTANCE_INVALID_RELATION;
    }
    cm_free(bytes);
    return status;
}

CmHirInstanceStatus cm_hir_canonical_type_matches(
    const CmHirContext *hir, CmHirTypeId type,
    const unsigned char *bytes, size_t size)
{
    if (hir == NULL || type == CM_HIR_TYPE_NONE
        || bytes == NULL || size == 0u) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    return cm_instance_compare_encoded(hir, type, NULL, bytes, size);
}

static CmHirInstanceStatus cm_instance_encode_parts_value(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    CmHirCrateId local_crate, const CmHirCanonicalInstanceParts *parts)
{
    const CmHirItem *selected;
    const CmHirItem *declared;
    const CmHirItem *enclosing;
    const CmHirItem *trait_item;
    CmInstanceSubstitution impl_substitution;
    CmInstanceBuffer expected;
    CmInstanceBuffer actual;
    unsigned char *workspace;
    size_t workspace_size;
    CmHirInstanceStatus status;

    if (buffer == NULL || hir == NULL || parts == NULL) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    selected = cm_instance_item(hir, parts->selected_callable);
    if (selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION
        || selected->definition.crate_id != local_crate) {
        return selected == NULL ? CM_HIR_INSTANCE_INVALID_ID
            : CM_HIR_INSTANCE_FOREIGN_ADMISSION;
    }
    if (!cm_hir_def_id_equal(parts->body_definition,
            parts->selected_callable)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    status = cm_instance_u8(buffer, CM_INSTANCE_FORMAT_VERSION);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, parts->selected_callable);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, parts->body_definition);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, parts->declared_trait_callable);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, parts->enclosing_impl);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, parts->implemented_trait);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, parts->self_owner);
    }
    if (status != CM_HIR_INSTANCE_OK) return status;
    if (cm_hir_def_id_is_none(selected->parent_definition)) {
        if (!cm_hir_def_id_is_none(parts->declared_trait_callable)
            || !cm_hir_def_id_is_none(parts->enclosing_impl)
            || !cm_hir_def_id_is_none(parts->implemented_trait)
            || !cm_hir_def_id_is_none(parts->self_owner)
            || parts->self_type != NULL || parts->self_type_size != 0u
            || parts->method_argument_count != 0u
            || parts->method_arguments != NULL
            || parts->enclosing_impl_argument_count != 0u
            || parts->enclosing_impl_arguments != NULL
            || parts->implemented_trait_argument_count != 0u
            || parts->implemented_trait_arguments != NULL) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        return cm_instance_encode_part_arguments(buffer, hir, selected,
            parts->item_arguments, parts->item_argument_count, 1u);
    }
    if (parts->item_argument_count != 0u || parts->item_arguments != NULL) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    enclosing = cm_instance_item(hir, parts->enclosing_impl);
    declared = cm_instance_item(hir, parts->declared_trait_callable);
    trait_item = cm_instance_item(hir, parts->implemented_trait);
    if (!cm_instance_trait_callable_relation_valid(hir, selected, declared,
            enclosing, trait_item)
        || !cm_hir_def_id_equal(parts->self_owner, enclosing->definition)
        || parts->self_type == NULL || parts->self_type_size == 0u) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    status = cm_instance_encode_part_arguments(buffer, hir, selected,
        parts->method_arguments, parts->method_argument_count, 2u);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_encode_part_arguments(buffer, hir, enclosing,
            parts->enclosing_impl_arguments,
            parts->enclosing_impl_argument_count, 3u);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_encode_part_arguments(buffer, hir, trait_item,
            parts->implemented_trait_arguments,
            parts->implemented_trait_argument_count, 4u);
    }
    if (status != CM_HIR_INSTANCE_OK) return status;
    status = cm_instance_validate_payload(hir, parts->self_type,
        parts->self_type_size, CM_HIR_GENERIC_ARG_TYPE);
    if (status != CM_HIR_INSTANCE_OK) return status;
    memset(&impl_substitution, 0, sizeof(impl_substitution));
    impl_substitution.owner = enclosing->definition;
    impl_substitution.parts = parts->enclosing_impl_arguments;
    impl_substitution.argument_count = parts->enclosing_impl_argument_count;
    status = cm_instance_compare_encoded(hir,
        enclosing->data.impl_item.self_type, &impl_substitution,
        parts->self_type, parts->self_type_size);
    if (status != CM_HIR_INSTANCE_OK) return status;

    memset(&expected, 0, sizeof(expected));
    expected.sizing = 1;
    status = cm_instance_encode_arguments(&expected, hir,
        enclosing->data.impl_item.trait_type.arguments,
        enclosing->data.impl_item.trait_type.argument_count,
        &impl_substitution, 0u);
    memset(&actual, 0, sizeof(actual));
    actual.sizing = 1;
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_u32(&actual,
            parts->implemented_trait_argument_count);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        uint32_t index;

        for (index = 0u; status == CM_HIR_INSTANCE_OK
                && index < parts->implemented_trait_argument_count; ++index) {
            status = cm_instance_u8(&actual,
                (unsigned int)parts->implemented_trait_arguments[index].kind);
            if (status == CM_HIR_INSTANCE_OK) {
                status = cm_instance_write(&actual,
                    parts->implemented_trait_arguments[index].bytes,
                    parts->implemented_trait_arguments[index].size);
            }
        }
    }
    if (status != CM_HIR_INSTANCE_OK || expected.len != actual.len) {
        return status == CM_HIR_INSTANCE_OK
            ? CM_HIR_INSTANCE_INVALID_RELATION : status;
    }
    if (!cm_size_add(expected.len, actual.len, &workspace_size)) {
        return CM_HIR_INSTANCE_OVERFLOW;
    }
    workspace = (unsigned char *)cm_alloc(workspace_size);
    expected.data = workspace;
    expected.cap = expected.len;
    expected.len = 0u;
    expected.sizing = 0;
    actual.data = workspace + expected.cap;
    actual.cap = actual.len;
    actual.len = 0u;
    actual.sizing = 0;
    status = cm_instance_encode_arguments(&expected, hir,
        enclosing->data.impl_item.trait_type.arguments,
        enclosing->data.impl_item.trait_type.argument_count,
        &impl_substitution, 0u);
    if (status == CM_HIR_INSTANCE_OK) {
        uint32_t index;

        status = cm_instance_u32(&actual,
            parts->implemented_trait_argument_count);
        for (index = 0u; status == CM_HIR_INSTANCE_OK
                && index < parts->implemented_trait_argument_count; ++index) {
            status = cm_instance_u8(&actual,
                (unsigned int)parts->implemented_trait_arguments[index].kind);
            if (status == CM_HIR_INSTANCE_OK) {
                status = cm_instance_write(&actual,
                    parts->implemented_trait_arguments[index].bytes,
                    parts->implemented_trait_arguments[index].size);
            }
        }
    }
    if (status == CM_HIR_INSTANCE_OK
        && (expected.len != actual.len
            || memcmp(expected.data, actual.data, expected.len) != 0)) {
        status = CM_HIR_INSTANCE_INVALID_RELATION;
    }
    cm_free(workspace);
    if (status != CM_HIR_INSTANCE_OK) return status;
    status = cm_instance_u8(buffer, 5u);
    return status == CM_HIR_INSTANCE_OK
        ? cm_instance_write(buffer, parts->self_type,
            parts->self_type_size) : status;
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

static CmHirInstanceStatus cm_instance_encode_direct_call(
    CmInstanceBuffer *buffer, const CmHirContext *hir,
    CmHirCrateId local_crate, const CmHirInstanceSpec *caller_spec,
    const CmHirCanonicalInstanceParts *caller_parts, const CmHirExpr *call)
{
    const CmHirItem *caller_item;
    const CmHirItem *callee;
    CmInstanceSubstitution caller_substitution;
    CmHirInstanceStatus status;
    uint32_t index;

    if ((caller_spec == NULL) == (caller_parts == NULL)) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    caller_item = cm_instance_item(hir, caller_spec != NULL
        ? caller_spec->selected_callable : caller_parts->selected_callable);
    callee = call == NULL || call->kind != CM_HIR_EXPR_CALL ? NULL
        : cm_instance_item(hir, call->data.call.callee);
    if (caller_item == NULL || caller_item->kind != CM_HIR_ITEM_FUNCTION
        || caller_item->definition.crate_id != local_crate
        || !cm_hir_def_id_is_none(caller_item->parent_definition)
        || callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION
        || callee->definition.crate_id != local_crate
        || !cm_hir_def_id_is_none(callee->parent_definition)
        || call->owner_body != caller_item->data.function_item.body
        || callee->generic_parameter_count
            != call->data.call.type_substitution_count
        || (call->data.call.type_substitution_count != 0u
            && call->data.call.type_substitutions == NULL)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    memset(&caller_substitution, 0, sizeof(caller_substitution));
    caller_substitution.owner = caller_item->definition;
    if (caller_spec != NULL) {
        caller_substitution.arguments = caller_spec->item_arguments;
        caller_substitution.argument_count = caller_spec->item_argument_count;
    } else {
        caller_substitution.parts = caller_parts->item_arguments;
        caller_substitution.argument_count = caller_parts->item_argument_count;
    }
    status = cm_instance_u8(buffer, CM_INSTANCE_FORMAT_VERSION);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, callee->definition);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, callee->definition);
    }
    for (index = 0u; status == CM_HIR_INSTANCE_OK && index < 4u; ++index) {
        status = cm_instance_def(buffer, cm_hir_def_id_none());
    }
    if (status == CM_HIR_INSTANCE_OK) status = cm_instance_u8(buffer, 1u);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_u32(buffer,
            call->data.call.type_substitution_count);
    }
    for (index = 0u; status == CM_HIR_INSTANCE_OK
            && index < call->data.call.type_substitution_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(hir,
            callee->generic_parameter_start + index);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                callee->definition)) {
            return CM_HIR_INSTANCE_INVALID_RELATION;
        }
        status = cm_instance_u8(buffer, CM_HIR_GENERIC_ARG_TYPE);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_encode_type(buffer, hir,
                call->data.call.type_substitutions[index],
                &caller_substitution, 0u);
        }
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
    if (!cm_hir_def_id_equal(spec->body_definition,
            spec->selected_callable)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    status = cm_instance_u8(buffer, CM_INSTANCE_FORMAT_VERSION);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, spec->selected_callable);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_def(buffer, spec->body_definition);
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
    if (!cm_instance_trait_callable_relation_valid(hir, selected, declared,
            enclosing, trait_item)
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

static int cm_canonical_instance_is_empty(
    const CmHirCanonicalInstance *instance)
{
    return instance != NULL
        && cm_hir_def_id_is_none(instance->definition)
        && cm_hir_def_id_is_none(instance->body_definition)
        && instance->body == CM_HIR_BODY_NONE
        && instance->bytes == NULL && instance->size == 0u;
}

static int cm_canonical_instance_is_valid(
    const CmHirCanonicalInstance *instance)
{
    return instance != NULL
        && !cm_hir_def_id_is_none(instance->definition)
        && !cm_hir_def_id_is_none(instance->body_definition)
        && instance->body != CM_HIR_BODY_NONE
        && instance->bytes != NULL && instance->size != 0u;
}

void cm_hir_canonical_instance_init(CmHirCanonicalInstance *instance)
{
    if (instance == NULL) return;
    memset(instance, 0, sizeof(*instance));
    instance->definition = cm_hir_def_id_none();
    instance->body_definition = cm_hir_def_id_none();
}

CmHirInstanceStatus cm_hir_canonical_instance_encode(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirInstanceSpec *spec, CmHirCanonicalInstance *out_instance)
{
    const CmHirItem *selected;
    const CmHirItem *body_item;
    CmHirCanonicalInstance encoded;
    CmInstanceBuffer sizing;
    CmInstanceBuffer output;
    CmHirInstanceStatus status;

    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE || spec == NULL
        || !cm_canonical_instance_is_empty(out_instance)) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    selected = cm_instance_item(hir, spec->selected_callable);
    if (!cm_hir_def_id_equal(spec->body_definition,
            spec->selected_callable)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    body_item = cm_instance_item(hir, spec->body_definition);
    if (selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION) {
        return CM_HIR_INSTANCE_INVALID_ID;
    }
    if (body_item == NULL || body_item->kind != CM_HIR_ITEM_FUNCTION) {
        return CM_HIR_INSTANCE_INVALID_ID;
    }
    if (body_item->data.function_item.body == CM_HIR_BODY_NONE) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    memset(&sizing, 0, sizeof(sizing));
    sizing.sizing = 1;
    status = cm_instance_encode_spec(&sizing, hir, local_crate, spec);
    if (status != CM_HIR_INSTANCE_OK) return status;

    cm_hir_canonical_instance_init(&encoded);
    encoded.bytes = (unsigned char *)cm_alloc(sizing.len);
    memset(&output, 0, sizeof(output));
    output.data = encoded.bytes;
    output.cap = sizing.len;
    status = cm_instance_encode_spec(&output, hir, local_crate, spec);
    if (status != CM_HIR_INSTANCE_OK || output.len != sizing.len) {
        cm_hir_canonical_instance_destroy(&encoded);
        return status == CM_HIR_INSTANCE_OK
            ? CM_HIR_INSTANCE_INVALID_RELATION : status;
    }
    encoded.definition = selected->definition;
    encoded.body_definition = body_item->definition;
    encoded.body = body_item->data.function_item.body;
    encoded.size = output.len;
    *out_instance = encoded;
    return CM_HIR_INSTANCE_OK;
}

CmHirInstanceStatus cm_hir_canonical_instance_encode_parts(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirCanonicalInstanceParts *parts,
    CmHirCanonicalInstance *out_instance)
{
    const CmHirItem *selected;
    const CmHirItem *body_item;
    CmHirCanonicalInstance encoded;
    CmInstanceBuffer sizing;
    CmInstanceBuffer output;
    CmHirInstanceStatus status;

    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE || parts == NULL
        || !cm_canonical_instance_is_empty(out_instance)) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    selected = cm_instance_item(hir, parts->selected_callable);
    if (!cm_hir_def_id_equal(parts->body_definition,
            parts->selected_callable)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    body_item = cm_instance_item(hir, parts->body_definition);
    if (selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION) {
        return CM_HIR_INSTANCE_INVALID_ID;
    }
    if (body_item == NULL || body_item->kind != CM_HIR_ITEM_FUNCTION) {
        return CM_HIR_INSTANCE_INVALID_ID;
    }
    if (body_item->data.function_item.body == CM_HIR_BODY_NONE) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    memset(&sizing, 0, sizeof(sizing));
    sizing.sizing = 1;
    status = cm_instance_encode_parts_value(&sizing, hir, local_crate,
        parts);
    if (status != CM_HIR_INSTANCE_OK) return status;
    cm_hir_canonical_instance_init(&encoded);
    encoded.bytes = (unsigned char *)cm_alloc(sizing.len);
    memset(&output, 0, sizeof(output));
    output.data = encoded.bytes;
    output.cap = sizing.len;
    status = cm_instance_encode_parts_value(&output, hir, local_crate,
        parts);
    if (status != CM_HIR_INSTANCE_OK || output.len != sizing.len) {
        cm_hir_canonical_instance_destroy(&encoded);
        return status == CM_HIR_INSTANCE_OK
            ? CM_HIR_INSTANCE_INVALID_RELATION : status;
    }
    encoded.definition = selected->definition;
    encoded.body_definition = body_item->definition;
    encoded.body = body_item->data.function_item.body;
    encoded.size = output.len;
    *out_instance = encoded;
    return CM_HIR_INSTANCE_OK;
}

static CmHirInstanceStatus cm_canonical_instance_encode_direct_call_value(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirInstanceSpec *caller_spec,
    const CmHirCanonicalInstanceParts *caller_parts, const CmHirExpr *call,
    CmHirCanonicalInstance *out_instance)
{
    const CmHirItem *callee;
    CmHirCanonicalInstance caller_identity;
    CmHirCanonicalInstance encoded;
    CmInstanceBuffer sizing;
    CmInstanceBuffer output;
    CmHirInstanceStatus status;

    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE
        || (caller_spec == NULL) == (caller_parts == NULL)
        || call == NULL || !cm_canonical_instance_is_empty(out_instance)) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    cm_hir_canonical_instance_init(&caller_identity);
    status = caller_spec != NULL
        ? cm_hir_canonical_instance_encode(hir, local_crate, caller_spec,
            &caller_identity)
        : cm_hir_canonical_instance_encode_parts(hir, local_crate,
            caller_parts, &caller_identity);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_hir_canonical_instance_validate(hir, local_crate,
            &caller_identity);
    }
    cm_hir_canonical_instance_destroy(&caller_identity);
    if (status != CM_HIR_INSTANCE_OK) return status;
    callee = call->kind == CM_HIR_EXPR_CALL
        ? cm_instance_item(hir, call->data.call.callee) : NULL;
    if (callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION
        || callee->data.function_item.body == CM_HIR_BODY_NONE) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    memset(&sizing, 0, sizeof(sizing));
    sizing.sizing = 1;
    status = cm_instance_encode_direct_call(&sizing, hir, local_crate,
        caller_spec, caller_parts, call);
    if (status != CM_HIR_INSTANCE_OK) return status;
    cm_hir_canonical_instance_init(&encoded);
    encoded.bytes = (unsigned char *)cm_alloc(sizing.len);
    memset(&output, 0, sizeof(output));
    output.data = encoded.bytes;
    output.cap = sizing.len;
    status = cm_instance_encode_direct_call(&output, hir, local_crate,
        caller_spec, caller_parts, call);
    if (status != CM_HIR_INSTANCE_OK || output.len != sizing.len) {
        cm_hir_canonical_instance_destroy(&encoded);
        return status == CM_HIR_INSTANCE_OK
            ? CM_HIR_INSTANCE_INVALID_RELATION : status;
    }
    encoded.definition = callee->definition;
    encoded.body_definition = callee->definition;
    encoded.body = callee->data.function_item.body;
    encoded.size = output.len;
    *out_instance = encoded;
    return CM_HIR_INSTANCE_OK;
}

CmHirInstanceStatus cm_hir_canonical_instance_encode_direct_call(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirInstanceSpec *caller, const CmHirExpr *call,
    CmHirCanonicalInstance *out_instance)
{
    return cm_canonical_instance_encode_direct_call_value(hir, local_crate,
        caller, NULL, call, out_instance);
}

CmHirInstanceStatus cm_hir_canonical_instance_encode_direct_call_parts(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirCanonicalInstanceParts *caller, const CmHirExpr *call,
    CmHirCanonicalInstance *out_instance)
{
    return cm_canonical_instance_encode_direct_call_value(hir, local_crate,
        NULL, caller, call, out_instance);
}

CmHirInstanceStatus cm_hir_canonical_instance_clone(
    CmHirCanonicalInstance *out_instance,
    const CmHirCanonicalInstance *source)
{
    CmHirCanonicalInstance copy;

    if (!cm_canonical_instance_is_empty(out_instance)
        || !cm_canonical_instance_is_valid(source)) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    cm_hir_canonical_instance_init(&copy);
    copy.bytes = (unsigned char *)cm_alloc(source->size);
    memcpy(copy.bytes, source->bytes, source->size);
    copy.definition = source->definition;
    copy.body_definition = source->body_definition;
    copy.body = source->body;
    copy.size = source->size;
    *out_instance = copy;
    return CM_HIR_INSTANCE_OK;
}

static CmHirInstanceStatus cm_instance_read_part_arguments(
    CmInstanceReader *reader, const CmHirItem *owner, unsigned int section,
    CmHirCanonicalArgumentPart **out_arguments, uint32_t *out_count)
{
    CmHirCanonicalArgumentPart *arguments;
    CmHirInstanceStatus status;
    uint32_t count;
    uint32_t index;
    unsigned int actual_section;

    if (reader == NULL || owner == NULL || out_arguments == NULL
        || out_count == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    *out_arguments = NULL;
    *out_count = 0u;
    status = cm_instance_read_u8(reader, &actual_section);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_read_u32(reader, &count);
    }
    if (status != CM_HIR_INSTANCE_OK) return status;
    if (actual_section != section
        || count != owner->generic_parameter_count) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    arguments = count == 0u ? NULL
        : (CmHirCanonicalArgumentPart *)cm_alloc_zeroed(count,
            sizeof(*arguments));
    for (index = 0u; index < count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected;
        size_t start;
        unsigned int kind;

        parameter = cm_hir_get_generic_param(reader->hir,
            owner->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner, owner->definition)) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
            break;
        }
        expected = parameter->kind == CM_HIR_GENERIC_LIFETIME
            ? CM_HIR_GENERIC_ARG_LIFETIME
            : parameter->kind == CM_HIR_GENERIC_TYPE
                ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        status = cm_instance_read_u8(reader, &kind);
        if (status != CM_HIR_INSTANCE_OK
            || kind != (unsigned int)expected) {
            if (status == CM_HIR_INSTANCE_OK) {
                status = CM_HIR_INSTANCE_INVALID_RELATION;
            }
            break;
        }
        start = reader->pos;
        status = cm_instance_validate_argument_payload(reader, expected, 0u);
        if (status != CM_HIR_INSTANCE_OK) break;
        arguments[index].kind = expected;
        arguments[index].bytes = reader->data + start;
        arguments[index].size = reader->pos - start;
    }
    if (status != CM_HIR_INSTANCE_OK) {
        cm_free(arguments);
        return status;
    }
    *out_arguments = arguments;
    *out_count = count;
    return CM_HIR_INSTANCE_OK;
}

void cm_hir_decoded_canonical_instance_init(
    CmHirDecodedCanonicalInstance *decoded)
{
    if (decoded == NULL) return;
    memset(decoded, 0, sizeof(*decoded));
    decoded->parts.selected_callable = cm_hir_def_id_none();
    decoded->parts.body_definition = cm_hir_def_id_none();
    decoded->parts.declared_trait_callable = cm_hir_def_id_none();
    decoded->parts.enclosing_impl = cm_hir_def_id_none();
    decoded->parts.implemented_trait = cm_hir_def_id_none();
    decoded->parts.self_owner = cm_hir_def_id_none();
}

static int cm_decoded_canonical_instance_is_empty(
    const CmHirDecodedCanonicalInstance *decoded)
{
    return decoded != NULL
        && cm_hir_def_id_is_none(decoded->parts.selected_callable)
        && cm_hir_def_id_is_none(decoded->parts.body_definition)
        && cm_hir_def_id_is_none(decoded->parts.declared_trait_callable)
        && decoded->parts.item_arguments == NULL
        && decoded->parts.item_argument_count == 0u
        && decoded->parts.method_arguments == NULL
        && decoded->parts.method_argument_count == 0u
        && cm_hir_def_id_is_none(decoded->parts.enclosing_impl)
        && decoded->parts.enclosing_impl_arguments == NULL
        && decoded->parts.enclosing_impl_argument_count == 0u
        && cm_hir_def_id_is_none(decoded->parts.implemented_trait)
        && decoded->parts.implemented_trait_arguments == NULL
        && decoded->parts.implemented_trait_argument_count == 0u
        && cm_hir_def_id_is_none(decoded->parts.self_owner)
        && decoded->parts.self_type == NULL
        && decoded->parts.self_type_size == 0u
        && decoded->owned_item_arguments == NULL
        && decoded->owned_method_arguments == NULL
        && decoded->owned_enclosing_impl_arguments == NULL
        && decoded->owned_implemented_trait_arguments == NULL;
}

void cm_hir_decoded_canonical_instance_destroy(
    CmHirDecodedCanonicalInstance *decoded)
{
    if (decoded == NULL) return;
    cm_free(decoded->owned_implemented_trait_arguments);
    cm_free(decoded->owned_enclosing_impl_arguments);
    cm_free(decoded->owned_method_arguments);
    cm_free(decoded->owned_item_arguments);
    cm_hir_decoded_canonical_instance_init(decoded);
}

CmHirInstanceStatus cm_hir_canonical_instance_decode(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirCanonicalInstance *instance,
    CmHirDecodedCanonicalInstance *out_decoded)
{
    CmHirDecodedCanonicalInstance decoded;
    CmHirCanonicalInstance encoded;
    CmInstanceReader reader;
    const CmHirItem *selected;
    const CmHirItem *body_item;
    const CmHirBody *body;
    const CmHirItem *enclosing;
    const CmHirItem *trait_item;
    CmHirInstanceStatus status;
    uint32_t item_count;
    uint32_t method_count;
    uint32_t impl_count;
    uint32_t trait_count;
    unsigned int version;
    unsigned int section;
    size_t self_start;
    int equal;

    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE
        || !cm_canonical_instance_is_valid(instance)
        || !cm_decoded_canonical_instance_is_empty(out_decoded)) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    cm_hir_decoded_canonical_instance_init(&decoded);
    cm_hir_canonical_instance_init(&encoded);
    memset(&reader, 0, sizeof(reader));
    reader.hir = hir;
    reader.data = instance->bytes;
    reader.len = instance->size;
    item_count = 0u;
    method_count = 0u;
    impl_count = 0u;
    trait_count = 0u;
    status = cm_instance_read_u8(&reader, &version);
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_read_def(&reader,
            &decoded.parts.selected_callable);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_read_def(&reader,
            &decoded.parts.body_definition);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_read_def(&reader,
            &decoded.parts.declared_trait_callable);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_read_def(&reader,
            &decoded.parts.enclosing_impl);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_read_def(&reader,
            &decoded.parts.implemented_trait);
    }
    if (status == CM_HIR_INSTANCE_OK) {
        status = cm_instance_read_def(&reader, &decoded.parts.self_owner);
    }
    if (status != CM_HIR_INSTANCE_OK) goto done;
    if (version != CM_INSTANCE_FORMAT_VERSION
        || !cm_hir_def_id_equal(decoded.parts.selected_callable,
            instance->definition)
        || !cm_hir_def_id_equal(decoded.parts.body_definition,
            instance->body_definition)
        || !cm_hir_def_id_equal(decoded.parts.body_definition,
            decoded.parts.selected_callable)
        || instance->definition.crate_id != local_crate) {
        status = CM_HIR_INSTANCE_INVALID_RELATION;
        goto done;
    }
    selected = cm_instance_item(hir, decoded.parts.selected_callable);
    body_item = cm_instance_item(hir, decoded.parts.body_definition);
    body = body_item == NULL || body_item->kind != CM_HIR_ITEM_FUNCTION
            || body_item->data.function_item.body == CM_HIR_BODY_NONE
        ? NULL : cm_hir_get_body(hir, body_item->data.function_item.body);
    if (selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION
        || body_item == NULL || body_item->kind != CM_HIR_ITEM_FUNCTION
        || body_item->data.function_item.body != instance->body
        || body == NULL
        || !cm_hir_def_id_equal(body->owner,
            decoded.parts.body_definition)
        || body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        || !cm_hir_def_id_equal(body->origin.definition,
            decoded.parts.body_definition)
        || !cm_hir_def_id_equal(body->origin.enclosing_definition,
            decoded.parts.body_definition)
        || !cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition,
            decoded.parts.body_definition)) {
        status = CM_HIR_INSTANCE_INVALID_RELATION;
        goto done;
    }
    if (cm_hir_def_id_is_none(selected->parent_definition)) {
        if (!cm_hir_def_id_is_none(
                decoded.parts.declared_trait_callable)
            || !cm_hir_def_id_is_none(decoded.parts.enclosing_impl)
            || !cm_hir_def_id_is_none(decoded.parts.implemented_trait)
            || !cm_hir_def_id_is_none(decoded.parts.self_owner)) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
            goto done;
        }
        status = cm_instance_read_part_arguments(&reader, selected, 1u,
            &decoded.owned_item_arguments, &item_count);
        decoded.parts.item_arguments = decoded.owned_item_arguments;
        decoded.parts.item_argument_count = item_count;
    } else {
        enclosing = cm_instance_item(hir, decoded.parts.enclosing_impl);
        trait_item = cm_instance_item(hir,
            decoded.parts.implemented_trait);
        if (enclosing == NULL || enclosing->kind != CM_HIR_ITEM_IMPL
            || trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
            goto done;
        }
        status = cm_instance_read_part_arguments(&reader, selected, 2u,
            &decoded.owned_method_arguments, &method_count);
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_read_part_arguments(&reader, enclosing, 3u,
                &decoded.owned_enclosing_impl_arguments, &impl_count);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_read_part_arguments(&reader, trait_item, 4u,
                &decoded.owned_implemented_trait_arguments, &trait_count);
        }
        if (status == CM_HIR_INSTANCE_OK) {
            status = cm_instance_read_u8(&reader, &section);
        }
        if (status != CM_HIR_INSTANCE_OK) goto done;
        if (section != 5u) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
            goto done;
        }
        self_start = reader.pos;
        status = cm_instance_validate_type_payload(&reader, 0u);
        if (status != CM_HIR_INSTANCE_OK) goto done;
        decoded.parts.method_arguments = decoded.owned_method_arguments;
        decoded.parts.method_argument_count = method_count;
        decoded.parts.enclosing_impl_arguments =
            decoded.owned_enclosing_impl_arguments;
        decoded.parts.enclosing_impl_argument_count = impl_count;
        decoded.parts.implemented_trait_arguments =
            decoded.owned_implemented_trait_arguments;
        decoded.parts.implemented_trait_argument_count = trait_count;
        decoded.parts.self_type = reader.data + self_start;
        decoded.parts.self_type_size = reader.pos - self_start;
    }
    if (reader.pos != reader.len) {
        status = CM_HIR_INSTANCE_INVALID_RELATION;
        goto done;
    }
    status = cm_hir_canonical_instance_encode_parts(hir, local_crate,
        &decoded.parts, &encoded);
    if (status == CM_HIR_INSTANCE_OK) {
        equal = 0;
        status = cm_hir_canonical_instance_equal(instance, &encoded, &equal);
        if (status == CM_HIR_INSTANCE_OK && !equal) {
            status = CM_HIR_INSTANCE_INVALID_RELATION;
        }
    }
    if (status == CM_HIR_INSTANCE_OK) {
        *out_decoded = decoded;
        cm_hir_decoded_canonical_instance_init(&decoded);
    }

done:
    cm_hir_canonical_instance_destroy(&encoded);
    cm_hir_decoded_canonical_instance_destroy(&decoded);
    return status;
}

CmHirInstanceStatus cm_hir_canonical_instance_validate(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmHirCanonicalInstance *instance)
{
    CmHirDecodedCanonicalInstance decoded;
    CmHirInstanceStatus status;

    cm_hir_decoded_canonical_instance_init(&decoded);
    status = cm_hir_canonical_instance_decode(hir, local_crate, instance,
        &decoded);
    cm_hir_decoded_canonical_instance_destroy(&decoded);
    return status;
}

void cm_hir_canonical_instance_destroy(CmHirCanonicalInstance *instance)
{
    if (instance == NULL) return;
    cm_free(instance->bytes);
    cm_hir_canonical_instance_init(instance);
}

static int cm_instance_compare_u32(uint32_t left, uint32_t right)
{
    return left < right ? -1 : left > right ? 1 : 0;
}

CmHirInstanceStatus cm_hir_canonical_instance_compare(
    const CmHirCanonicalInstance *left,
    const CmHirCanonicalInstance *right, int *out_order)
{
    size_t shared;
    int comparison;

    if (out_order == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    *out_order = 0;
    if (!cm_canonical_instance_is_valid(left)
        || !cm_canonical_instance_is_valid(right)) {
        return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    }
    comparison = cm_instance_compare_u32(left->definition.crate_id,
        right->definition.crate_id);
    if (comparison == 0) {
        comparison = cm_instance_compare_u32(left->definition.index,
            right->definition.index);
    }
    if (comparison == 0) {
        comparison = cm_instance_compare_u32(left->body_definition.crate_id,
            right->body_definition.crate_id);
    }
    if (comparison == 0) {
        comparison = cm_instance_compare_u32(left->body_definition.index,
            right->body_definition.index);
    }
    if (comparison == 0) {
        comparison = cm_instance_compare_u32(left->body, right->body);
    }
    shared = left->size < right->size ? left->size : right->size;
    if (comparison == 0) {
        comparison = memcmp(left->bytes, right->bytes, shared);
    }
    if (comparison == 0 && left->size != right->size) {
        comparison = left->size < right->size ? -1 : 1;
    }
    *out_order = comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
    return CM_HIR_INSTANCE_OK;
}

CmHirInstanceStatus cm_hir_canonical_instance_equal(
    const CmHirCanonicalInstance *left,
    const CmHirCanonicalInstance *right, int *out_equal)
{
    CmHirInstanceStatus status;
    int order;

    if (out_equal == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    *out_equal = 0;
    status = cm_hir_canonical_instance_compare(left, right, &order);
    if (status == CM_HIR_INSTANCE_OK) *out_equal = order == 0;
    return status;
}

void cm_hir_instance_spec_init(CmHirInstanceSpec *spec)
{
    if (spec == NULL) return;
    memset(spec, 0, sizeof(*spec));
    spec->selected_callable = cm_hir_def_id_none();
    spec->body_definition = cm_hir_def_id_none();
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
    CmHirCanonicalInstance canonical;
    CmHirInstanceKeyState *state;
    CmHirInstanceStatus status;
    const CmHirItem *selected;
    const CmSemanticResults *semantic_results;
    CmSemanticBodyView semantic_body;
    CmSemanticResultsStatus semantic_status;
    int relationful;
    uint64_t admission_capability_id;
    size_t allocation_size;

    if (key == NULL || key->state != NULL || admission == NULL
        || spec == NULL) return CM_HIR_INSTANCE_INVALID_ARGUMENT;
    if (!cm_semantic_admission_is_current(admission)) {
        return CM_HIR_INSTANCE_STALE_ADMISSION;
    }
    hir = cm_semantic_admission_hir(admission);
    local_crate = cm_semantic_admission_crate(admission);
    admission_capability_id =
        cm_semantic_admission_capability_id(admission);
    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE
        || admission_capability_id == UINT64_C(0)) {
        return CM_HIR_INSTANCE_STALE_ADMISSION;
    }
    selected = cm_instance_item(hir, spec->selected_callable);
    semantic_results = cm_semantic_admission_results(admission);
    relationful = !cm_hir_def_id_is_none(spec->declared_trait_callable)
        || !cm_hir_def_id_is_none(spec->enclosing_impl)
        || !cm_hir_def_id_is_none(spec->implemented_trait)
        || !cm_hir_def_id_is_none(spec->self_owner)
        || spec->self_type != CM_HIR_TYPE_NONE;
    semantic_status = semantic_results == NULL
        ? CM_SEMANTIC_RESULTS_NOT_FOUND
        : relationful
            ? cm_semantic_results_instance_body(semantic_results,
                admission, spec, &semantic_body)
            : cm_semantic_results_body(semantic_results, admission,
                selected == NULL ? CM_HIR_BODY_NONE
                    : selected->data.function_item.body, &semantic_body);
    if (!relationful && semantic_status == CM_SEMANTIC_RESULTS_NOT_FOUND
        && semantic_results != NULL) {
        semantic_status = cm_semantic_results_instance_body(
            semantic_results, admission, spec, &semantic_body);
    }
    if (selected == NULL || selected->kind != CM_HIR_ITEM_FUNCTION
        || selected->data.function_item.body == CM_HIR_BODY_NONE
        || semantic_results == NULL
        || semantic_status != CM_SEMANTIC_RESULTS_OK
        || !cm_hir_def_id_equal(semantic_body.owner,
            spec->selected_callable)) {
        return CM_HIR_INSTANCE_INVALID_RELATION;
    }
    cm_hir_canonical_instance_init(&canonical);
    status = cm_hir_canonical_instance_encode(hir, local_crate, spec,
        &canonical);
    if (status != CM_HIR_INSTANCE_OK) return status;
    if (!cm_size_add(sizeof(*state) - 1u, canonical.size,
            &allocation_size)) {
        cm_hir_canonical_instance_destroy(&canonical);
        return CM_HIR_INSTANCE_OVERFLOW;
    }
    state = (CmHirInstanceKeyState *)cm_alloc(allocation_size);
    memset(state, 0, sizeof(*state) - 1u);
    memcpy(state->encoded, canonical.bytes, canonical.size);
    state->hir = hir;
    state->local_crate = local_crate;
    state->admission_capability_id = admission_capability_id;
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->encoded_size = canonical.size;
    cm_hir_canonical_instance_destroy(&canonical);
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
        || cm_semantic_admission_crate(admission) != state->local_crate
        || cm_semantic_admission_capability_id(admission)
            != state->admission_capability_id) {
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
    fputs("hir-instance-v2 crate=", stream);
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
