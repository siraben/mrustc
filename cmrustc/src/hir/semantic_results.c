#include "semantic_results_internal.h"

#include "cm/alloc.h"
#include "cm/hir/admission.h"

#include <stdint.h>
#include <string.h>

#define CM_RESULTS_TYPE_DEPTH ((size_t)128u)

typedef struct CmResultsBuffer {
    unsigned char *data;
    size_t len;
    size_t cap;
    int sizing;
} CmResultsBuffer;

typedef struct CmSemanticBodyRecord {
    int present;
    CmHirDefId owner;
    uint32_t expression_count;
    size_t signature_return_offset;
    size_t signature_return_size;
    size_t signature_parameter_start;
    uint32_t signature_parameter_count;
} CmSemanticBodyRecord;

typedef struct CmSemanticTypeRecord {
    size_t type_offset;
    size_t type_size;
} CmSemanticTypeRecord;

typedef struct CmSemanticExpressionRecord {
    int present;
    CmHirBodyId body;
    size_t type_offset;
    size_t type_size;
    int has_direct_callable;
    CmHirDefId direct_callable;
    size_t call_return_offset;
    size_t call_return_size;
    size_t call_parameter_start;
    uint32_t call_parameter_count;
    int has_primitive_operator;
    CmHirBinaryOperator primitive_operator;
} CmSemanticExpressionRecord;

typedef struct CmSemanticInstanceRecord {
    CmHirCanonicalInstance identity;
    CmSemanticBodyRecord body;
    CmSemanticExpressionRecord *expressions;
    size_t expression_count;
    unsigned char *type_bytes;
    size_t type_bytes_len;
    CmSemanticTypeRecord *signature_parameters;
    size_t signature_parameter_count;
} CmSemanticInstanceRecord;

struct CmSemanticResults {
    const CmHirContext *hir;
    CmHirCrateId local_crate;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    CmSemanticBodyRecord *bodies;
    size_t body_count;
    CmSemanticExpressionRecord *expressions;
    size_t expression_count;
    unsigned char *type_bytes;
    size_t type_bytes_len;
    CmSemanticTypeRecord *signature_parameters;
    size_t signature_parameter_count;
    CmSemanticTypeRecord *call_parameters;
    size_t call_parameter_count;
    size_t admitted_body_count;
    CmSemanticInstanceRecord *instances;
    size_t instance_count;
    int sealed;
};

typedef struct CmSemanticResultsBodyStageState {
    const CmHirContext *hir;
    const CmSemanticSession *producer_session;
    const CmTypeckContext *producer_typeck;
    CmTraitImplUniverse universe;
    CmHirCrateId local_crate;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    CmHirBodyId body;
    CmHirDefId owner;
    CmSemanticExpressionRecord *expressions;
    size_t expression_count;
    unsigned char *type_bytes;
    size_t type_bytes_len;
    CmSemanticTypeRecord signature_return;
    CmSemanticTypeRecord *signature_parameters;
    size_t signature_parameter_count;
    CmSemanticTypeRecord *call_parameters;
    size_t call_parameter_count;
    uint32_t body_expression_count;
} CmSemanticResultsBodyStageState;

static CmSemanticResultsStatus cm_results_validate(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission);

static const CmSemanticInstanceRecord *cm_results_find_instance(
    const CmSemanticResults *results,
    const CmHirCanonicalInstance *identity)
{
    size_t index;

    if (results == NULL || identity == NULL) return NULL;
    for (index = 0u; index < results->instance_count; ++index) {
        int equal;

        if (cm_hir_canonical_instance_equal(
                &results->instances[index].identity, identity, &equal)
                == CM_HIR_INSTANCE_OK && equal) {
            return &results->instances[index];
        }
    }
    return NULL;
}

static const CmHirItem *cm_results_item(const CmHirContext *hir,
    CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL
            && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static CmSemanticResultsStatus cm_results_write(CmResultsBuffer *buffer,
    const void *bytes, size_t length)
{
    size_t new_length;

    if (buffer == NULL || (length != 0u && bytes == NULL)
        || !cm_size_add(buffer->len, length, &new_length)) {
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    if (!buffer->sizing) {
        if (new_length > buffer->cap) return CM_SEMANTIC_RESULTS_OVERFLOW;
        if (length != 0u) memcpy(buffer->data + buffer->len, bytes, length);
    }
    buffer->len = new_length;
    return CM_SEMANTIC_RESULTS_OK;
}

static CmSemanticResultsStatus cm_results_u8(CmResultsBuffer *buffer,
    unsigned int value)
{
    unsigned char byte;

    byte = (unsigned char)(value & 0xffu);
    return cm_results_write(buffer, &byte, 1u);
}

static CmSemanticResultsStatus cm_results_u32(CmResultsBuffer *buffer,
    uint32_t value)
{
    unsigned char bytes[4];
    unsigned int index;

    for (index = 0u; index < 4u; ++index) {
        bytes[index] = (unsigned char)((value >> (8u * index)) & 0xffu);
    }
    return cm_results_write(buffer, bytes, sizeof(bytes));
}

static CmSemanticResultsStatus cm_results_u64(CmResultsBuffer *buffer,
    uint64_t value)
{
    unsigned char bytes[8];
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        bytes[index] = (unsigned char)((value >> (8u * index))
            & UINT64_C(0xff));
    }
    return cm_results_write(buffer, bytes, sizeof(bytes));
}

static CmSemanticResultsStatus cm_results_def(CmResultsBuffer *buffer,
    CmHirDefId definition)
{
    CmSemanticResultsStatus status;

    status = cm_results_u32(buffer, definition.crate_id);
    return status == CM_SEMANTIC_RESULTS_OK
        ? cm_results_u32(buffer, definition.index) : status;
}

static CmSemanticResultsStatus cm_results_interned(
    CmResultsBuffer *buffer, const CmHirContext *hir, CmInternId id)
{
    const CmInternedString *string;
    CmSemanticResultsStatus status;

    string = hir == NULL ? NULL : cm_interner_get(&hir->strings, id);
    if (string == NULL || string->len > (size_t)UINT32_MAX) {
        return string == NULL ? CM_SEMANTIC_RESULTS_INVALID_HIR
            : CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    status = cm_results_u32(buffer, (uint32_t)string->len);
    return status == CM_SEMANTIC_RESULTS_OK
        ? cm_results_write(buffer, string->bytes, string->len) : status;
}

static CmSemanticResultsStatus cm_results_typeck_type(
    CmResultsBuffer *buffer, const CmHirContext *hir,
    const CmTypeckContext *typeck, CmTypeckTypeId type_id, size_t depth);

static CmSemanticResultsStatus cm_results_typeck_region(
    CmResultsBuffer *buffer, const CmHirRegion *region)
{
    if (region == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
    if (region->kind == CM_HIR_REGION_STATIC
        || region->kind == CM_HIR_REGION_ERASED) {
        return cm_results_u8(buffer, (unsigned int)region->kind);
    }
    if (region->kind == CM_HIR_REGION_INFER) {
        return CM_SEMANTIC_RESULTS_DEFERRED_INFERENCE;
    }
    return CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE;
}

static CmSemanticResultsStatus cm_results_typeck_const(
    CmResultsBuffer *buffer, const CmHirContext *hir,
    const CmTypeckContext *typeck, const CmTypeckConst *constant,
    size_t depth)
{
    const CmHirGenericParam *parameter;
    CmSemanticResultsStatus status;

    if (constant == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
    if (constant->kind == CM_HIR_CONST_INFER) {
        return CM_SEMANTIC_RESULTS_DEFERRED_INFERENCE;
    }
    if (constant->kind != CM_HIR_CONST_VALUE
            && constant->kind != CM_HIR_CONST_PARAMETER) {
        return CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE;
    }
    status = cm_results_u8(buffer, (unsigned int)constant->kind);
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_typeck_type(buffer, hir, typeck,
            constant->type, depth + 1u);
    }
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (constant->kind == CM_HIR_CONST_VALUE) {
        status = cm_results_u64(buffer, constant->data.value.low_bits);
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_u64(buffer, constant->data.value.high_bits) : status;
    }
    parameter = cm_hir_get_generic_param(hir, constant->data.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_CONST
        || cm_hir_def_id_is_none(parameter->owner)) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    status = cm_results_def(buffer, parameter->owner);
    return status == CM_SEMANTIC_RESULTS_OK
        ? cm_results_u32(buffer, parameter->index) : status;
}

static CmSemanticResultsStatus cm_results_typeck_argument(
    CmResultsBuffer *buffer, const CmHirContext *hir,
    const CmTypeckContext *typeck, const CmTypeckGenericArg *argument,
    size_t depth)
{
    CmSemanticResultsStatus status;

    if (argument == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
    status = cm_results_u8(buffer, (unsigned int)argument->kind);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_results_typeck_region(buffer, &argument->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_results_typeck_type(buffer, hir, typeck,
            argument->data.type, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_results_typeck_const(buffer, hir, typeck,
            &argument->data.constant, depth + 1u);
    }
    return CM_SEMANTIC_RESULTS_INVALID_HIR;
}

static CmSemanticResultsStatus cm_results_typeck_named(
    CmResultsBuffer *buffer, const CmHirContext *hir,
    const CmTypeckContext *typeck, const CmTypeckNamedType *named,
    size_t depth)
{
    CmSemanticResultsStatus status;
    uint32_t index;

    if (named == NULL || cm_hir_def_id_is_none(named->definition)
        || (named->argument_count == 0u) != (named->arguments == NULL)
        || cm_hir_lookup_definition(hir, named->definition) == NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    status = cm_results_def(buffer, named->definition);
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_u32(buffer, named->argument_count);
    }
    for (index = 0u; status == CM_SEMANTIC_RESULTS_OK
            && index < named->argument_count; ++index) {
        status = cm_results_typeck_argument(buffer, hir, typeck,
            &named->arguments[index], depth + 1u);
    }
    return status;
}

static CmSemanticResultsStatus cm_results_typeck_type(
    CmResultsBuffer *buffer, const CmHirContext *hir,
    const CmTypeckContext *typeck, CmTypeckTypeId type_id, size_t depth)
{
    const CmTypeckType *type;
    const CmHirGenericParam *parameter;
    CmTypeckTypeId resolved;
    CmSemanticResultsStatus status;
    unsigned int tag;
    uint32_t index;

    if (depth >= CM_RESULTS_TYPE_DEPTH) return CM_SEMANTIC_RESULTS_OVERFLOW;
    if (cm_typeck_resolve(typeck, type_id, &resolved) != CM_TYPECK_OK) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    type = cm_typeck_get_type(typeck, resolved);
    if (type == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
    switch (type->kind) {
    case CM_TYPECK_TYPE_NEVER: tag = CM_HIR_TYPE_NEVER_KIND; break;
    case CM_TYPECK_TYPE_UNIT: tag = CM_HIR_TYPE_UNIT_KIND; break;
    case CM_TYPECK_TYPE_BOOL: tag = CM_HIR_TYPE_BOOL_KIND; break;
    case CM_TYPECK_TYPE_CHAR: tag = CM_HIR_TYPE_CHAR_KIND; break;
    case CM_TYPECK_TYPE_STR: tag = CM_HIR_TYPE_STR_KIND; break;
    case CM_TYPECK_TYPE_INTEGER: tag = CM_HIR_TYPE_INTEGER_KIND; break;
    case CM_TYPECK_TYPE_FLOAT: tag = CM_HIR_TYPE_FLOAT_KIND; break;
    case CM_TYPECK_TYPE_REFERENCE: tag = CM_HIR_TYPE_REFERENCE_KIND; break;
    case CM_TYPECK_TYPE_RAW_POINTER: tag = CM_HIR_TYPE_RAW_POINTER_KIND; break;
    case CM_TYPECK_TYPE_TUPLE: tag = CM_HIR_TYPE_TUPLE_KIND; break;
    case CM_TYPECK_TYPE_ARRAY: tag = CM_HIR_TYPE_ARRAY_KIND; break;
    case CM_TYPECK_TYPE_SLICE: tag = CM_HIR_TYPE_SLICE_KIND; break;
    case CM_TYPECK_TYPE_FN_POINTER: tag = CM_HIR_TYPE_FN_POINTER_KIND; break;
    case CM_TYPECK_TYPE_ADT: tag = CM_HIR_TYPE_ADT_KIND; break;
    case CM_TYPECK_TYPE_PARAMETER: tag = CM_HIR_TYPE_PARAMETER_KIND; break;
    case CM_TYPECK_TYPE_VARIABLE:
        return CM_SEMANTIC_RESULTS_DEFERRED_INFERENCE;
    case CM_TYPECK_TYPE_PROJECTION:
        return CM_SEMANTIC_RESULTS_PENDING_PROJECTION;
    default:
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    status = cm_results_u8(buffer, tag);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    switch (type->kind) {
    case CM_TYPECK_TYPE_NEVER:
    case CM_TYPECK_TYPE_UNIT:
    case CM_TYPECK_TYPE_BOOL:
    case CM_TYPECK_TYPE_CHAR:
    case CM_TYPECK_TYPE_STR:
        return CM_SEMANTIC_RESULTS_OK;
    case CM_TYPECK_TYPE_INTEGER:
        return cm_results_u8(buffer, (unsigned int)type->data.integer_type);
    case CM_TYPECK_TYPE_FLOAT:
        return cm_results_u8(buffer, (unsigned int)type->data.float_type);
    case CM_TYPECK_TYPE_REFERENCE:
        status = cm_results_typeck_region(buffer,
            &type->data.reference_type.region);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_u8(buffer,
                (unsigned int)type->data.reference_type.mutability);
        }
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_typeck_type(buffer, hir, typeck,
                type->data.reference_type.pointee, depth + 1u) : status;
    case CM_TYPECK_TYPE_RAW_POINTER:
        status = cm_results_u8(buffer,
            (unsigned int)type->data.raw_pointer_type.mutability);
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_typeck_type(buffer, hir, typeck,
                type->data.raw_pointer_type.pointee, depth + 1u) : status;
    case CM_TYPECK_TYPE_TUPLE:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        status = cm_results_u32(buffer, type->data.tuple_type.element_count);
        for (index = 0u; status == CM_SEMANTIC_RESULTS_OK
                && index < type->data.tuple_type.element_count; ++index) {
            status = cm_results_typeck_type(buffer, hir, typeck,
                type->data.tuple_type.elements[index], depth + 1u);
        }
        return status;
    case CM_TYPECK_TYPE_ARRAY:
        status = cm_results_typeck_type(buffer, hir, typeck,
            type->data.array_type.element, depth + 1u);
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_typeck_const(buffer, hir, typeck,
                &type->data.array_type.length, depth + 1u) : status;
    case CM_TYPECK_TYPE_SLICE:
        return cm_results_typeck_type(buffer, hir, typeck,
            type->data.slice_type.element, depth + 1u);
    case CM_TYPECK_TYPE_FN_POINTER:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        status = cm_results_u32(buffer,
            type->data.fn_pointer_type.parameter_count);
        for (index = 0u; status == CM_SEMANTIC_RESULTS_OK
                && index < type->data.fn_pointer_type.parameter_count;
             ++index) {
            status = cm_results_typeck_type(buffer, hir, typeck,
                type->data.fn_pointer_type.parameters[index], depth + 1u);
        }
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_typeck_type(buffer, hir, typeck,
                type->data.fn_pointer_type.return_type, depth + 1u);
        }
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_interned(buffer, hir,
                type->data.fn_pointer_type.abi);
        }
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_u8(buffer,
                (unsigned int)type->data.fn_pointer_type.safety);
        }
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_u8(buffer,
                type->data.fn_pointer_type.is_variadic ? 1u : 0u) : status;
    case CM_TYPECK_TYPE_ADT:
        return cm_results_typeck_named(buffer, hir, typeck,
            &type->data.named_type, depth + 1u);
    case CM_TYPECK_TYPE_PARAMETER:
        parameter = cm_hir_get_generic_param(hir,
            type->data.parameter_type.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || cm_hir_def_id_is_none(parameter->owner)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        status = cm_results_def(buffer, parameter->owner);
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_u32(buffer, parameter->index) : status;
    case CM_TYPECK_TYPE_VARIABLE:
        return CM_SEMANTIC_RESULTS_DEFERRED_INFERENCE;
    case CM_TYPECK_TYPE_PROJECTION:
        return CM_SEMANTIC_RESULTS_PENDING_PROJECTION;
    }
    return CM_SEMANTIC_RESULTS_INVALID_HIR;
}

static CmSemanticResultsStatus cm_results_collect_expression(
    CmSemanticResults *results, const CmHirContext *hir,
    CmHirBodyId body_id, CmHirExprId expression_id, unsigned char *seen,
    CmResultsBuffer *types, uint32_t *body_expression_count, size_t depth,
    int publish, const CmTypeckContext *typeck,
    const CmTypeckTypeId *expression_terms, size_t expression_term_count)
{
    const CmHirExpr *expression;
    CmSemanticExpressionRecord *record;
    CmSemanticResultsStatus status;
    uint32_t index;

    if (depth >= results->expression_count
        || expression_id == CM_HIR_EXPR_NONE
        || (size_t)expression_id > results->expression_count) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    expression = cm_hir_get_expr(hir, expression_id);
    if (expression == NULL || expression->owner_body != body_id) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    if (seen[(size_t)expression_id - 1u] == 1u) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    if (seen[(size_t)expression_id - 1u] == 2u) return CM_SEMANTIC_RESULTS_OK;
    seen[(size_t)expression_id - 1u] = 1u;
    switch (expression->kind) {
    case CM_HIR_EXPR_BLOCK:
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            if (expression->data.block.statements[index].kind
                != CM_HIR_STATEMENT_LET) {
                return CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.block.statements[index].data.let_statement
                    .initializer,
                seen, types, body_expression_count, depth + 1u, publish,
                typeck, expression_terms, expression_term_count);
            if (status != CM_SEMANTIC_RESULTS_OK) return status;
        }
        status = cm_results_collect_expression(results, hir, body_id,
            expression->data.block.tail_expression, seen, types,
            body_expression_count, depth + 1u, publish, typeck,
            expression_terms, expression_term_count);
        if (status != CM_SEMANTIC_RESULTS_OK) return status;
        break;
    case CM_HIR_EXPR_CALL:
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.call.arguments[index], seen, types,
                body_expression_count, depth + 1u, publish, typeck,
                expression_terms, expression_term_count);
            if (status != CM_SEMANTIC_RESULTS_OK) return status;
        }
        break;
    case CM_HIR_EXPR_BINARY:
        status = cm_results_collect_expression(results, hir, body_id,
            expression->data.binary.left, seen, types,
            body_expression_count, depth + 1u, publish, typeck,
            expression_terms, expression_term_count);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.binary.right, seen, types,
                body_expression_count, depth + 1u, publish, typeck,
                expression_terms, expression_term_count);
        }
        if (status != CM_SEMANTIC_RESULTS_OK) return status;
        break;
    case CM_HIR_EXPR_AGGREGATE:
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.aggregate.fields[index].value, seen, types,
                body_expression_count, depth + 1u, publish, typeck,
                expression_terms, expression_term_count);
            if (status != CM_SEMANTIC_RESULTS_OK) return status;
        }
        break;
    case CM_HIR_EXPR_FIELD:
        status = cm_results_collect_expression(results, hir, body_id,
            expression->data.field.base, seen, types,
            body_expression_count, depth + 1u, publish, typeck,
            expression_terms, expression_term_count);
        if (status != CM_SEMANTIC_RESULTS_OK) return status;
        break;
    case CM_HIR_EXPR_IF:
        status = cm_results_collect_expression(results, hir, body_id,
            expression->data.if_expr.condition, seen, types,
            body_expression_count, depth + 1u, publish, typeck,
            expression_terms, expression_term_count);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.if_expr.then_expression, seen, types,
                body_expression_count, depth + 1u, publish, typeck,
                expression_terms, expression_term_count);
        }
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.if_expr.else_expression, seen, types,
                body_expression_count, depth + 1u, publish, typeck,
                expression_terms, expression_term_count);
        }
        if (status != CM_SEMANTIC_RESULTS_OK) return status;
        break;
    case CM_HIR_EXPR_INTEGER:
    case CM_HIR_EXPR_LOCAL:
        break;
    default:
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    record = &results->expressions[(size_t)expression_id - 1u];
    if (record->present) return CM_SEMANTIC_RESULTS_INVALID_HIR;
    if (publish) record->type_offset = types->len;
    status = typeck == NULL || expression_terms == NULL
            || expression_term_count != results->expression_count
            || expression_terms[(size_t)expression_id - 1u]
                == CM_TYPECK_TYPE_NONE
        ? CM_SEMANTIC_RESULTS_INVALID_HIR
        : cm_results_typeck_type(types, hir, typeck,
            expression_terms[(size_t)expression_id - 1u], 0u);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (publish) {
        record->type_size = types->len - record->type_offset;
        record->present = 1;
        record->body = body_id;
        if (expression->kind == CM_HIR_EXPR_BINARY) {
            record->has_primitive_operator = 1;
            record->primitive_operator = expression->data.binary.operator_kind;
        }
    }
    if (*body_expression_count == UINT32_MAX) {
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    *body_expression_count += 1u;
    seen[(size_t)expression_id - 1u] = 2u;
    return CM_SEMANTIC_RESULTS_OK;
}

static CmSemanticResultsStatus cm_results_collect_type_record(
    CmResultsBuffer *buffer, const CmHirContext *hir,
    const CmTypeckContext *typeck, CmTypeckTypeId type, int publish,
    CmSemanticTypeRecord *record)
{
    CmSemanticResultsStatus status;
    size_t start;

    if (buffer == NULL || hir == NULL || typeck == NULL
        || type == CM_TYPECK_TYPE_NONE || (publish && record == NULL)) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    start = buffer->len;
    status = cm_results_typeck_type(buffer, hir, typeck, type, 0u);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (publish) {
        record->type_offset = start;
        record->type_size = buffer->len - start;
    }
    return CM_SEMANTIC_RESULTS_OK;
}

static CmSemanticResultsStatus cm_results_validate_membership(
    const CmSemanticResults *results, const CmHirContext *hir)
{
    size_t expression_index;

    for (expression_index = 0u;
            expression_index < results->expression_count;
            ++expression_index) {
        const CmHirExpr *expression;
        const CmSemanticExpressionRecord *record;
        int owner_is_admitted;

        expression = cm_hir_get_expr(hir,
            (CmHirExprId)(expression_index + 1u));
        record = &results->expressions[expression_index];
        if (expression == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
        owner_is_admitted = expression->owner_body != CM_HIR_BODY_NONE
            && (size_t)expression->owner_body <= results->body_count
            && results->bodies[(size_t)expression->owner_body - 1u].present;
        if ((owner_is_admitted && (!record->present
                || record->body != expression->owner_body))
            || (record->present && (!owner_is_admitted
                || record->body != expression->owner_body))) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        if (record->present
            && ((expression->kind == CM_HIR_EXPR_CALL)
                != record->has_direct_callable)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
    }
    return CM_SEMANTIC_RESULTS_OK;
}

static int cm_results_type_bytes_equal(const CmSemanticResults *results,
    size_t left_offset, size_t left_size, size_t right_offset,
    size_t right_size)
{
    if (results == NULL || left_size != right_size
        || left_offset > results->type_bytes_len
        || left_size > results->type_bytes_len - left_offset
        || right_offset > results->type_bytes_len
        || right_size > results->type_bytes_len - right_offset) return 0;
    return left_size == 0u || memcmp(results->type_bytes + left_offset,
        results->type_bytes + right_offset, left_size) == 0;
}

CmSemanticResultsStatus cm_semantic_results_begin(
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmSemanticResults **out_results)
{
    CmSemanticResults *results;
    size_t body_bytes;
    size_t expression_bytes;

    if (hir == NULL || local_crate == CM_HIR_CRATE_NONE
        || out_results == NULL || *out_results != NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!cm_size_mul(hir->bodies.len, sizeof(CmSemanticBodyRecord),
            &body_bytes)
        || !cm_size_mul(hir->expressions.len,
            sizeof(CmSemanticExpressionRecord), &expression_bytes)) {
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    results = (CmSemanticResults *)cm_alloc_zeroed(1u, sizeof(*results));
    results->body_count = hir->bodies.len;
    results->expression_count = hir->expressions.len;
    results->bodies = body_bytes == 0u ? NULL
        : (CmSemanticBodyRecord *)cm_alloc_zeroed(1u, body_bytes);
    results->expressions = expression_bytes == 0u ? NULL
        : (CmSemanticExpressionRecord *)cm_alloc_zeroed(1u,
            expression_bytes);
    results->hir = hir;
    results->local_crate = local_crate;
    results->storage_lifetime_id = hir->storage.lifetime_id;
    results->semantic_generation = hir->semantic_generation;
    results->rewind_generation = hir->rewind_generation;
    *out_results = results;
    return CM_SEMANTIC_RESULTS_OK;
}

void cm_semantic_results_body_stage_init(CmSemanticResultsBodyStage *stage)
{
    if (stage != NULL) stage->state = NULL;
}

void cm_semantic_results_body_stage_destroy(
    CmSemanticResultsBodyStage *stage)
{
    CmSemanticResultsBodyStageState *state;

    if (stage == NULL) return;
    state = (CmSemanticResultsBodyStageState *)stage->state;
    if (state != NULL) {
        cm_free(state->type_bytes);
        cm_free(state->call_parameters);
        cm_free(state->signature_parameters);
        cm_free(state->expressions);
        memset(state, 0, sizeof(*state));
        cm_free(state);
    }
    stage->state = NULL;
}

static CmSemanticBodyWritebackStatus cm_results_writeback_status(
    CmSemanticResultsStatus status)
{
    if (status == CM_SEMANTIC_RESULTS_OK) {
        return CM_SEMANTIC_BODY_WRITEBACK_OK;
    }
    if (status == CM_SEMANTIC_RESULTS_OVERFLOW) {
        return CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW;
    }
    if (status == CM_SEMANTIC_RESULTS_DEFERRED_INFERENCE) {
        return CM_SEMANTIC_BODY_WRITEBACK_DEFERRED_INFERENCE;
    }
    if (status == CM_SEMANTIC_RESULTS_PENDING_PROJECTION) {
        return CM_SEMANTIC_BODY_WRITEBACK_PENDING_PROJECTION;
    }
    if (status == CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE) {
        return CM_SEMANTIC_BODY_WRITEBACK_UNSUPPORTED;
    }
    return CM_SEMANTIC_BODY_WRITEBACK_INVALID;
}

CmSemanticBodyWritebackStatus cm_semantic_results_stage_checked_body(
    void *context, CmSemanticSession *session, CmHirBodyId body_id,
    const CmSemanticCheckedBodyFacts *facts)
{
    CmSemanticResultsBodyStage *stage;
    CmSemanticResultsBodyStageState *state;
    const CmHirContext *hir;
    const CmHirBody *body;
    const CmHirItem *owner_item;
    CmTypeckContext *typeck;
    CmResultsBuffer sizing;
    CmResultsBuffer output;
    unsigned char *seen;
    CmSemanticResults body_results;
    CmSemanticResultsStatus status;
    size_t expression_bytes;
    size_t expression_index;
    size_t call_index;
    size_t parameter_index;
    size_t call_parameter_count;
    size_t signature_parameter_bytes;
    size_t call_parameter_bytes;
    uint32_t expression_count;

    stage = (CmSemanticResultsBodyStage *)context;
    if (stage == NULL || stage->state != NULL || session == NULL
        || body_id == CM_HIR_BODY_NONE || facts == NULL
        || facts->expression_terms == NULL
        || facts->signature_return_type == CM_TYPECK_TYPE_NONE
        || (facts->signature_parameter_count != 0u
            && facts->signature_parameter_types == NULL)
        || (facts->call_count != 0u && facts->calls == NULL)
        || !cm_semantic_session_is_current(session)
        || cm_semantic_session_universe(session)
            != CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE) {
        return CM_SEMANTIC_BODY_WRITEBACK_INVALID;
    }
    hir = cm_semantic_session_hir(session);
    typeck = cm_semantic_session_typeck(session);
    if (hir == NULL || typeck == NULL
        || facts->expression_term_count != hir->expressions.len) {
        return CM_SEMANTIC_BODY_WRITEBACK_INVALID;
    }
    body = cm_hir_get_body(hir, body_id);
    owner_item = body == NULL ? NULL
        : cm_results_item(hir, body->owner);
    if (body == NULL || body->state != CM_HIR_BODY_TYPED
        || owner_item == NULL || owner_item->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_def_id_equal(owner_item->definition, body->owner)
        || owner_item->data.function_item.body != body_id
        || owner_item->data.function_item.signature.parameter_count
            != facts->signature_parameter_count
        || body->root_expression == CM_HIR_EXPR_NONE
        || body->owner.crate_id != cm_semantic_session_local_crate(session)
        || !cm_hir_def_id_equal(body->owner,
            cm_semantic_session_exact_owner(session))) {
        return CM_SEMANTIC_BODY_WRITEBACK_INVALID;
    }
    if (!cm_size_mul(facts->expression_term_count,
            sizeof(CmSemanticExpressionRecord), &expression_bytes)) {
        return CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW;
    }
    call_parameter_count = 0u;
    for (call_index = 0u; call_index < facts->call_count; ++call_index) {
        if (facts->calls[call_index].parameter_count != 0u
                && facts->calls[call_index].parameter_types == NULL) {
            return CM_SEMANTIC_BODY_WRITEBACK_INVALID;
        }
        if (!cm_size_add(call_parameter_count,
                facts->calls[call_index].parameter_count,
                &call_parameter_count)) {
            return CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW;
        }
    }
    if (!cm_size_mul(facts->signature_parameter_count,
            sizeof(CmSemanticTypeRecord), &signature_parameter_bytes)
        || !cm_size_mul(call_parameter_count,
            sizeof(CmSemanticTypeRecord), &call_parameter_bytes)) {
        return CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW;
    }
    state = (CmSemanticResultsBodyStageState *)cm_alloc_zeroed(1u,
        sizeof(*state));
    state->expression_count = facts->expression_term_count;
    state->expressions = expression_bytes == 0u ? NULL
        : (CmSemanticExpressionRecord *)cm_alloc_zeroed(1u,
            expression_bytes);
    state->signature_parameter_count = facts->signature_parameter_count;
    state->signature_parameters = signature_parameter_bytes == 0u ? NULL
        : (CmSemanticTypeRecord *)cm_alloc_zeroed(1u,
            signature_parameter_bytes);
    state->call_parameter_count = call_parameter_count;
    state->call_parameters = call_parameter_bytes == 0u ? NULL
        : (CmSemanticTypeRecord *)cm_alloc_zeroed(1u,
            call_parameter_bytes);
    seen = facts->expression_term_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(facts->expression_term_count, 1u);
    memset(&body_results, 0, sizeof(body_results));
    body_results.expression_count = facts->expression_term_count;
    body_results.expressions = state->expressions;
    memset(&sizing, 0, sizeof(sizing));
    sizing.sizing = 1;
    expression_count = 0u;
    status = cm_results_collect_expression(&body_results, hir, body_id,
        body->root_expression, seen, &sizing, &expression_count, 0u, 0,
        typeck, facts->expression_terms, facts->expression_term_count);
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_collect_type_record(&sizing, hir, typeck,
            facts->signature_return_type, 0, NULL);
    }
    for (parameter_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && parameter_index < facts->signature_parameter_count;
         ++parameter_index) {
        status = cm_results_collect_type_record(&sizing, hir, typeck,
            facts->signature_parameter_types[parameter_index], 0, NULL);
    }
    for (call_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK && call_index < facts->call_count;
         ++call_index) {
        status = cm_results_collect_type_record(&sizing, hir, typeck,
            facts->calls[call_index].return_type, 0, NULL);
        for (parameter_index = 0u;
             status == CM_SEMANTIC_RESULTS_OK
                && parameter_index < facts->calls[call_index].parameter_count;
             ++parameter_index) {
            status = cm_results_collect_type_record(&sizing, hir, typeck,
                facts->calls[call_index].parameter_types[parameter_index],
                0, NULL);
        }
    }
    if (status != CM_SEMANTIC_RESULTS_OK) {
        cm_free(seen);
        cm_free(state->call_parameters);
        cm_free(state->signature_parameters);
        cm_free(state->expressions);
        cm_free(state);
        return cm_results_writeback_status(status);
    }
    for (expression_index = 0u;
            expression_index < facts->expression_term_count;
            ++expression_index) {
        const CmHirExpr *expression;
        int owned;

        expression = cm_hir_get_expr(hir,
            (CmHirExprId)(expression_index + 1u));
        owned = expression != NULL && expression->owner_body == body_id;
        if (expression == NULL
            || (owned && (seen[expression_index] != 2u
                || facts->expression_terms[expression_index]
                    == CM_TYPECK_TYPE_NONE))
            || (!owned && facts->expression_terms[expression_index]
                != CM_TYPECK_TYPE_NONE)) {
            cm_free(seen);
            cm_free(state->call_parameters);
            cm_free(state->signature_parameters);
            cm_free(state->expressions);
            cm_free(state);
            return CM_SEMANTIC_BODY_WRITEBACK_INVALID;
        }
    }
    state->type_bytes = sizing.len == 0u ? NULL
        : (unsigned char *)cm_alloc(sizing.len);
    if (seen != NULL) memset(seen, 0, facts->expression_term_count);
    memset(&output, 0, sizeof(output));
    output.data = state->type_bytes;
    output.cap = sizing.len;
    expression_count = 0u;
    status = cm_results_collect_expression(&body_results, hir, body_id,
        body->root_expression, seen, &output, &expression_count, 0u, 1,
        typeck, facts->expression_terms, facts->expression_term_count);
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_collect_type_record(&output, hir, typeck,
            facts->signature_return_type, 1, &state->signature_return);
    }
    for (parameter_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && parameter_index < facts->signature_parameter_count;
         ++parameter_index) {
        status = cm_results_collect_type_record(&output, hir, typeck,
            facts->signature_parameter_types[parameter_index], 1,
            &state->signature_parameters[parameter_index]);
    }
    call_parameter_count = 0u;
    for (call_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK && call_index < facts->call_count;
         ++call_index) {
        const CmSemanticCheckedCallFacts *call;
        const CmHirExpr *expression;
        CmSemanticExpressionRecord *record;
        CmSemanticTypeRecord return_record;

        call = &facts->calls[call_index];
        expression = cm_hir_get_expr(hir, call->expression);
        if (expression == NULL || expression->owner_body != body_id
            || expression->kind != CM_HIR_EXPR_CALL
            || !cm_hir_def_id_equal(expression->data.call.callee,
                call->callee)
            || expression->data.call.argument_count
                != call->parameter_count) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record = &state->expressions[(size_t)call->expression - 1u];
        if (!record->present || record->has_direct_callable) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record->has_direct_callable = 1;
        record->direct_callable = call->callee;
        record->call_parameter_start = call_parameter_count;
        record->call_parameter_count = call->parameter_count;
        memset(&return_record, 0, sizeof(return_record));
        status = cm_results_collect_type_record(&output, hir, typeck,
            call->return_type, 1, &return_record);
        record->call_return_offset = return_record.type_offset;
        record->call_return_size = return_record.type_size;
        for (parameter_index = 0u;
             status == CM_SEMANTIC_RESULTS_OK
                && parameter_index < call->parameter_count;
             ++parameter_index) {
            status = cm_results_collect_type_record(&output, hir, typeck,
                call->parameter_types[parameter_index], 1,
                &state->call_parameters[call_parameter_count]);
            call_parameter_count += 1u;
        }
    }
    cm_free(seen);
    if (status != CM_SEMANTIC_RESULTS_OK || output.len != sizing.len) {
        cm_free(state->type_bytes);
        cm_free(state->call_parameters);
        cm_free(state->signature_parameters);
        cm_free(state->expressions);
        cm_free(state);
        return cm_results_writeback_status(status == CM_SEMANTIC_RESULTS_OK
            ? CM_SEMANTIC_RESULTS_INVALID_HIR : status);
    }
    state->hir = hir;
    state->producer_session = session;
    state->producer_typeck = typeck;
    state->universe = cm_semantic_session_universe(session);
    state->local_crate = cm_semantic_session_local_crate(session);
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->body = body_id;
    state->owner = body->owner;
    state->type_bytes_len = sizing.len;
    state->body_expression_count = expression_count;
    stage->state = state;
    return CM_SEMANTIC_BODY_WRITEBACK_OK;
}

CmSemanticResultsStatus cm_semantic_results_commit_checked_body(
    CmSemanticResults *results, CmSemanticSession *session,
    const CmSemanticBodyResult *check, CmSemanticResultsBodyStage *stage)
{
    CmSemanticResultsBodyStageState *state;
    const CmHirContext *hir;
    const CmHirBody *body;
    CmSemanticBodyRecord *record;
    unsigned char *combined_type_bytes;
    CmSemanticTypeRecord *combined_signature_parameters;
    CmSemanticTypeRecord *combined_call_parameters;
    size_t new_type_bytes_len;
    size_t new_signature_parameter_count;
    size_t new_call_parameter_count;
    size_t signature_parameter_bytes;
    size_t call_parameter_bytes;
    size_t expression_index;
    size_t parameter_index;

    state = stage == NULL ? NULL
        : (CmSemanticResultsBodyStageState *)stage->state;
    if (results == NULL || session == NULL || results->sealed
        || check == NULL || check->status != CM_SEMANTIC_BODY_OK
        || state == NULL || !cm_semantic_session_is_current(session)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    hir = cm_semantic_session_hir(session);
    if (hir == NULL || session != state->producer_session
        || cm_semantic_session_typeck(session) != state->producer_typeck
        || cm_semantic_session_universe(session) != state->universe
        || hir != results->hir || hir != state->hir
        || state->body != check->body
        || state->expression_count != results->expression_count
        || state->local_crate != results->local_crate
        || cm_semantic_session_local_crate(session) != results->local_crate
        || state->storage_lifetime_id != results->storage_lifetime_id
        || state->semantic_generation != results->semantic_generation
        || state->rewind_generation != results->rewind_generation
        || hir->storage.lifetime_id != results->storage_lifetime_id
        || hir->semantic_generation != results->semantic_generation
        || hir->rewind_generation != results->rewind_generation
        || state->body == CM_HIR_BODY_NONE
        || (size_t)state->body > results->body_count) {
        return CM_SEMANTIC_RESULTS_STALE;
    }
    body = cm_hir_get_body(hir, state->body);
    record = &results->bodies[(size_t)state->body - 1u];
    if (body == NULL || body->state != CM_HIR_BODY_TYPED
        || !cm_hir_def_id_equal(body->owner, state->owner)
        || !cm_hir_def_id_equal(body->owner,
            cm_semantic_session_exact_owner(session))
        || record->present) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    for (expression_index = 0u;
            expression_index < results->expression_count;
            ++expression_index) {
        if (state->expressions[expression_index].present
            && results->expressions[expression_index].present) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
    }
    if (!cm_size_add(results->type_bytes_len, state->type_bytes_len,
            &new_type_bytes_len)
        || !cm_size_add(results->signature_parameter_count,
            state->signature_parameter_count,
            &new_signature_parameter_count)
        || !cm_size_add(results->call_parameter_count,
            state->call_parameter_count, &new_call_parameter_count)
        || !cm_size_mul(new_signature_parameter_count,
            sizeof(*combined_signature_parameters),
            &signature_parameter_bytes)
        || !cm_size_mul(new_call_parameter_count,
            sizeof(*combined_call_parameters), &call_parameter_bytes)) {
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    combined_type_bytes = new_type_bytes_len == 0u ? NULL
        : (unsigned char *)cm_alloc(new_type_bytes_len);
    combined_signature_parameters = signature_parameter_bytes == 0u ? NULL
        : (CmSemanticTypeRecord *)cm_alloc(signature_parameter_bytes);
    combined_call_parameters = call_parameter_bytes == 0u ? NULL
        : (CmSemanticTypeRecord *)cm_alloc(call_parameter_bytes);
    if (results->type_bytes_len != 0u) {
        memcpy(combined_type_bytes, results->type_bytes,
            results->type_bytes_len);
    }
    if (state->type_bytes_len != 0u) {
        memcpy(combined_type_bytes + results->type_bytes_len,
            state->type_bytes, state->type_bytes_len);
    }
    if (results->signature_parameter_count != 0u) {
        memcpy(combined_signature_parameters, results->signature_parameters,
            results->signature_parameter_count
                * sizeof(*combined_signature_parameters));
    }
    for (parameter_index = 0u;
         parameter_index < state->signature_parameter_count;
         ++parameter_index) {
        combined_signature_parameters[
            results->signature_parameter_count + parameter_index] =
            state->signature_parameters[parameter_index];
        combined_signature_parameters[
            results->signature_parameter_count + parameter_index]
                .type_offset += results->type_bytes_len;
    }
    if (results->call_parameter_count != 0u) {
        memcpy(combined_call_parameters, results->call_parameters,
            results->call_parameter_count
                * sizeof(*combined_call_parameters));
    }
    for (parameter_index = 0u;
         parameter_index < state->call_parameter_count;
         ++parameter_index) {
        combined_call_parameters[
            results->call_parameter_count + parameter_index] =
            state->call_parameters[parameter_index];
        combined_call_parameters[
            results->call_parameter_count + parameter_index]
                .type_offset += results->type_bytes_len;
    }
    for (expression_index = 0u;
            expression_index < results->expression_count;
            ++expression_index) {
        if (!state->expressions[expression_index].present) continue;
        state->expressions[expression_index].type_offset +=
            results->type_bytes_len;
        if (state->expressions[expression_index].has_direct_callable) {
            state->expressions[expression_index].call_return_offset +=
                results->type_bytes_len;
            state->expressions[expression_index].call_parameter_start +=
                results->call_parameter_count;
        }
        results->expressions[expression_index] =
            state->expressions[expression_index];
    }
    cm_free(results->type_bytes);
    cm_free(results->signature_parameters);
    cm_free(results->call_parameters);
    results->type_bytes = combined_type_bytes;
    results->type_bytes_len = new_type_bytes_len;
    results->signature_parameters = combined_signature_parameters;
    results->signature_parameter_count = new_signature_parameter_count;
    results->call_parameters = combined_call_parameters;
    results->call_parameter_count = new_call_parameter_count;
    record->present = 1;
    record->owner = body->owner;
    record->expression_count = state->body_expression_count;
    record->signature_return_offset = state->signature_return.type_offset
        + (new_type_bytes_len - state->type_bytes_len);
    record->signature_return_size = state->signature_return.type_size;
    record->signature_parameter_start = new_signature_parameter_count
        - state->signature_parameter_count;
    record->signature_parameter_count =
        (uint32_t)state->signature_parameter_count;
    results->admitted_body_count += 1u;
    cm_semantic_results_body_stage_destroy(stage);
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_commit_checked_instance(
    CmSemanticResults *results, CmSemanticSession *session,
    const CmHirCanonicalInstance *instance,
    const CmSemanticBodyResult *check, CmSemanticResultsBodyStage *stage)
{
    CmSemanticResultsBodyStageState *state;
    CmSemanticInstanceRecord *combined;
    CmSemanticInstanceRecord *record;
    size_t bytes;
    size_t expression_index;

    state = stage == NULL ? NULL
        : (CmSemanticResultsBodyStageState *)stage->state;
    if (results == NULL || results->sealed || session == NULL
        || instance == NULL || check == NULL
        || check->status != CM_SEMANTIC_BODY_OK || state == NULL
        || !cm_semantic_session_is_current(session)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_semantic_session_hir(session) != results->hir
        || state->hir != results->hir || state->body != instance->body
        || check->body != instance->body
        || !cm_hir_def_id_equal(state->owner, instance->definition)
        || !cm_hir_def_id_equal(cm_semantic_session_exact_owner(session),
            instance->definition)
        || cm_results_find_instance(results, instance) != NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    for (expression_index = 0u;
         expression_index < state->expression_count; ++expression_index) {
        if (state->expressions[expression_index].present
            && state->expressions[expression_index].has_direct_callable) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
    }
    if (!cm_size_add(results->instance_count, 1u, &bytes)
        || !cm_size_mul(bytes, sizeof(*combined), &bytes)) {
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    combined = (CmSemanticInstanceRecord *)cm_alloc_zeroed(1u, bytes);
    if (results->instance_count != 0u) {
        memcpy(combined, results->instances,
            results->instance_count * sizeof(*combined));
    }
    record = &combined[results->instance_count];
    cm_hir_canonical_instance_init(&record->identity);
    if (cm_hir_canonical_instance_clone(&record->identity, instance)
            != CM_HIR_INSTANCE_OK) {
        cm_free(combined);
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    record->body.present = 1;
    record->body.owner = state->owner;
    record->body.expression_count = state->body_expression_count;
    record->body.signature_return_offset =
        state->signature_return.type_offset;
    record->body.signature_return_size = state->signature_return.type_size;
    record->body.signature_parameter_count =
        (uint32_t)state->signature_parameter_count;
    record->expressions = state->expressions;
    record->expression_count = state->expression_count;
    record->type_bytes = state->type_bytes;
    record->type_bytes_len = state->type_bytes_len;
    record->signature_parameters = state->signature_parameters;
    record->signature_parameter_count = state->signature_parameter_count;
    state->expressions = NULL;
    state->type_bytes = NULL;
    state->signature_parameters = NULL;
    cm_free(results->instances);
    results->instances = combined;
    results->instance_count += 1u;
    cm_semantic_results_body_stage_destroy(stage);
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_seal(CmSemanticResults *results)
{
    const CmHirContext *hir;
    CmSemanticResultsStatus status;
    size_t item_index;

    if (results == NULL || results->sealed) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    hir = results->hir;
    if (hir == NULL || hir->storage.lifetime_id != results->storage_lifetime_id
        || hir->semantic_generation != results->semantic_generation
        || hir->rewind_generation != results->rewind_generation) {
        return CM_SEMANTIC_RESULTS_STALE;
    }
    for (item_index = 0u; item_index < hir->items.len; ++item_index) {
        const CmHirItem *item;
        const CmHirBody *body;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, item_index);
        if (item == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
        if (item->definition.crate_id != results->local_crate
            || item->kind != CM_HIR_ITEM_FUNCTION
            || item->data.function_item.body == CM_HIR_BODY_NONE) continue;
        if ((size_t)item->data.function_item.body > results->body_count
            || !results->bodies[(size_t)item->data.function_item.body - 1u]
                .present) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        body = cm_hir_get_body(hir, item->data.function_item.body);
        if (body == NULL || !cm_hir_def_id_equal(body->owner,
                item->definition)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
    }
    status = cm_results_validate_membership(results, hir);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    results->sealed = 1;
    return CM_SEMANTIC_RESULTS_OK;

}

CmSemanticResultsStatus cm_semantic_results_seal_reachable(
    CmSemanticResults *results, const CmHirBodyId *bodies,
    size_t body_count)
{
    const CmHirContext *hir;
    unsigned char *selected;
    size_t index;
    CmSemanticResultsStatus status;

    if (results == NULL || results->sealed || bodies == NULL
        || body_count == 0u || body_count != results->admitted_body_count) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    hir = results->hir;
    if (hir == NULL || hir->storage.lifetime_id != results->storage_lifetime_id
        || hir->semantic_generation != results->semantic_generation
        || hir->rewind_generation != results->rewind_generation) {
        return CM_SEMANTIC_RESULTS_STALE;
    }
    selected = results->body_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(results->body_count, 1u);
    for (index = 0u; index < body_count; ++index) {
        CmHirBodyId body;

        body = bodies[index];
        if (body == CM_HIR_BODY_NONE || (size_t)body > results->body_count
            || selected[(size_t)body - 1u] != 0u
            || !results->bodies[(size_t)body - 1u].present) {
            cm_free(selected);
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        selected[(size_t)body - 1u] = 1u;
    }
    for (index = 0u; index < results->body_count; ++index) {
        if ((selected[index] != 0u) != results->bodies[index].present) {
            cm_free(selected);
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
    }
    status = cm_results_validate_membership(results, hir);
    if (status != CM_SEMANTIC_RESULTS_OK) {
        cm_free(selected);
        return status;
    }
    for (index = 0u; index < results->expression_count; ++index) {
        const CmSemanticExpressionRecord *expression;
        const CmHirDefinition *definition;
        const CmHirItem *callee;
        const CmSemanticBodyRecord *callee_record;
        CmHirBodyId callee_body;
        uint32_t parameter;

        expression = &results->expressions[index];
        if (!expression->present || !expression->has_direct_callable) {
            continue;
        }
        definition = cm_hir_lookup_definition(hir,
            expression->direct_callable);
        callee = definition == NULL
                || definition->kind != CM_HIR_DEFINITION_ITEM
                || definition->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
        callee_body = callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION
            ? CM_HIR_BODY_NONE : callee->data.function_item.body;
        if (callee_body == CM_HIR_BODY_NONE
            || (size_t)callee_body > results->body_count
            || selected[(size_t)callee_body - 1u] == 0u
            || callee->definition.crate_id != results->local_crate
            || !cm_hir_def_id_equal(callee->definition,
                expression->direct_callable)) {
            cm_free(selected);
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        callee_record = &results->bodies[(size_t)callee_body - 1u];
        if (!callee_record->present
            || !cm_hir_def_id_equal(callee_record->owner,
                expression->direct_callable)
            || expression->type_size == 0u
            || expression->call_return_size == 0u
            || expression->call_parameter_count
                != callee_record->signature_parameter_count
            || !cm_results_type_bytes_equal(results,
                expression->type_offset, expression->type_size,
                expression->call_return_offset,
                expression->call_return_size)
            || !cm_results_type_bytes_equal(results,
                expression->call_return_offset,
                expression->call_return_size,
                callee_record->signature_return_offset,
                callee_record->signature_return_size)) {
            cm_free(selected);
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        for (parameter = 0u;
             parameter < expression->call_parameter_count; ++parameter) {
            const CmSemanticTypeRecord *call_type;
            const CmSemanticTypeRecord *signature_type;
            size_t call_index;
            size_t signature_index;

            if (!cm_size_add(expression->call_parameter_start,
                    (size_t)parameter, &call_index)
                || call_index >= results->call_parameter_count
                || !cm_size_add(callee_record->signature_parameter_start,
                    (size_t)parameter, &signature_index)
                || signature_index >= results->signature_parameter_count) {
                cm_free(selected);
                return CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
            call_type = &results->call_parameters[call_index];
            signature_type = &results->signature_parameters[signature_index];
            if (call_type->type_size == 0u
                || signature_type->type_size == 0u
                || !cm_results_type_bytes_equal(results,
                    call_type->type_offset, call_type->type_size,
                    signature_type->type_offset,
                    signature_type->type_size)) {
                cm_free(selected);
                return CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
        }
    }
    cm_free(selected);
    results->sealed = 1;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_seal_leaf_instances(
    CmSemanticResults *results, size_t instance_count)
{
    const CmHirContext *hir;

    if (results == NULL || results->sealed || instance_count == 0u
        || results->admitted_body_count != 0u
        || results->instance_count != instance_count) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    hir = results->hir;
    if (hir == NULL || hir->storage.lifetime_id != results->storage_lifetime_id
        || hir->semantic_generation != results->semantic_generation
        || hir->rewind_generation != results->rewind_generation) {
        return CM_SEMANTIC_RESULTS_STALE;
    }
    results->sealed = 1;
    return CM_SEMANTIC_RESULTS_OK;
}

void cm_semantic_results_destroy(CmSemanticResults *results)
{
    size_t index;

    if (results == NULL) return;
    for (index = 0u; index < results->instance_count; ++index) {
        CmSemanticInstanceRecord *record;

        record = &results->instances[index];
        cm_free(record->signature_parameters);
        cm_free(record->type_bytes);
        cm_free(record->expressions);
        cm_hir_canonical_instance_destroy(&record->identity);
    }
    cm_free(results->instances);
    cm_free(results->call_parameters);
    cm_free(results->signature_parameters);
    cm_free(results->type_bytes);
    cm_free(results->expressions);
    cm_free(results->bodies);
    memset(results, 0, sizeof(*results));
    cm_free(results);
}

static CmSemanticResultsStatus cm_results_query_instance(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *spec,
    const CmSemanticInstanceRecord **out_record)
{
    CmHirCanonicalInstance identity;
    const CmSemanticInstanceRecord *record;
    CmSemanticResultsStatus status;

    if (spec == NULL || out_record == NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    cm_hir_canonical_instance_init(&identity);
    if (cm_hir_canonical_instance_encode(results->hir,
            results->local_crate, spec, &identity) != CM_HIR_INSTANCE_OK) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    record = cm_results_find_instance(results, &identity);
    cm_hir_canonical_instance_destroy(&identity);
    if (record == NULL) return CM_SEMANTIC_RESULTS_NOT_FOUND;
    *out_record = record;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_instance_body(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *spec, CmSemanticBodyView *out_view)
{
    const CmSemanticInstanceRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    status = cm_results_query_instance(results, admission, spec, &record);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    out_view->body = record->identity.body;
    out_view->owner = record->identity.definition;
    out_view->expression_count = record->body.expression_count;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_instance_expression(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *spec, CmHirExprId expression,
    CmSemanticExpressionView *out_view)
{
    const CmSemanticInstanceRecord *instance;
    const CmSemanticExpressionRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->direct_callable = cm_hir_def_id_none();
    status = cm_results_query_instance(results, admission, spec, &instance);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (expression == CM_HIR_EXPR_NONE
        || (size_t)expression > instance->expression_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    record = &instance->expressions[(size_t)expression - 1u];
    if (!record->present || record->body != instance->identity.body) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    out_view->expression = expression;
    out_view->body = record->body;
    out_view->unadjusted_type.bytes = instance->type_bytes
        + record->type_offset;
    out_view->unadjusted_type.size = record->type_size;
    out_view->adjusted_type = out_view->unadjusted_type;
    out_view->has_primitive_operator = record->has_primitive_operator;
    out_view->primitive_operator = record->primitive_operator;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_instance_signature(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *spec,
    CmSemanticFunctionSignatureView *out_view)
{
    const CmSemanticInstanceRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->definition = cm_hir_def_id_none();
    status = cm_results_query_instance(results, admission, spec, &record);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    out_view->definition = record->identity.definition;
    out_view->body = record->identity.body;
    out_view->parameter_count = record->body.signature_parameter_count;
    out_view->return_type.bytes = record->type_bytes
        + record->body.signature_return_offset;
    out_view->return_type.size = record->body.signature_return_size;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_instance_signature_parameter(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *spec, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    const CmSemanticInstanceRecord *record;
    const CmSemanticTypeRecord *type;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    status = cm_results_query_instance(results, admission, spec, &record);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (parameter >= record->body.signature_parameter_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    type = &record->signature_parameters[
        record->body.signature_parameter_start + parameter];
    out_view->bytes = record->type_bytes + type->type_offset;
    out_view->size = type->type_size;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_signature(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, CmHirBodyId body,
    CmSemanticFunctionSignatureView *out_view)
{
    const CmSemanticBodyRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->definition = cm_hir_def_id_none();
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (body == CM_HIR_BODY_NONE || (size_t)body > results->body_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    record = &results->bodies[(size_t)body - 1u];
    if (!record->present) return CM_SEMANTIC_RESULTS_NOT_FOUND;
    out_view->definition = record->owner;
    out_view->body = body;
    out_view->parameter_count = record->signature_parameter_count;
    out_view->return_type.bytes = results->type_bytes
        + record->signature_return_offset;
    out_view->return_type.size = record->signature_return_size;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_signature_parameter(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, CmHirBodyId body,
    uint32_t parameter, CmSemanticTypeView *out_view)
{
    const CmSemanticBodyRecord *record;
    const CmSemanticTypeRecord *type;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (body == CM_HIR_BODY_NONE || (size_t)body > results->body_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    record = &results->bodies[(size_t)body - 1u];
    if (!record->present || parameter >= record->signature_parameter_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    type = &results->signature_parameters[
        record->signature_parameter_start + parameter];
    out_view->bytes = results->type_bytes + type->type_offset;
    out_view->size = type->type_size;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_direct_call(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression, CmSemanticDirectCallView *out_view)
{
    const CmSemanticExpressionRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->callee = cm_hir_def_id_none();
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (expression == CM_HIR_EXPR_NONE
        || (size_t)expression > results->expression_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    record = &results->expressions[(size_t)expression - 1u];
    if (!record->present || record->body != body
        || !record->has_direct_callable) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    out_view->body = body;
    out_view->expression = expression;
    out_view->callee = record->direct_callable;
    out_view->parameter_count = record->call_parameter_count;
    out_view->return_type.bytes = results->type_bytes
        + record->call_return_offset;
    out_view->return_type.size = record->call_return_size;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_direct_call_parameter(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    const CmSemanticExpressionRecord *record;
    const CmSemanticTypeRecord *type;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (expression == CM_HIR_EXPR_NONE
        || (size_t)expression > results->expression_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    record = &results->expressions[(size_t)expression - 1u];
    if (!record->present || record->body != body
        || !record->has_direct_callable
        || parameter >= record->call_parameter_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    type = &results->call_parameters[
        record->call_parameter_start + parameter];
    out_view->bytes = results->type_bytes + type->type_offset;
    out_view->size = type->type_size;
    return CM_SEMANTIC_RESULTS_OK;
}

static CmSemanticResultsStatus cm_results_validate(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission)
{
    const CmHirContext *hir;

    if (results == NULL || admission == NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (!results->sealed) return CM_SEMANTIC_RESULTS_STALE;
    if (!cm_semantic_admission_is_current(admission)) {
        return CM_SEMANTIC_RESULTS_STALE;
    }
    if (cm_semantic_admission_results(admission) != results) {
        return CM_SEMANTIC_RESULTS_FOREIGN;
    }
    hir = cm_semantic_admission_hir(admission);
    if (hir != results->hir
        || cm_semantic_admission_crate(admission) != results->local_crate) {
        return CM_SEMANTIC_RESULTS_FOREIGN;
    }
    if (hir->storage.lifetime_id != results->storage_lifetime_id
        || hir->semantic_generation != results->semantic_generation
        || hir->rewind_generation != results->rewind_generation) {
        return CM_SEMANTIC_RESULTS_STALE;
    }
    return CM_SEMANTIC_RESULTS_OK;
}

int cm_semantic_results_is_current(const CmSemanticResults *results,
    const CmSemanticAdmission *admission)
{
    return cm_results_validate(results, admission)
        == CM_SEMANTIC_RESULTS_OK;
}

const CmHirContext *cm_semantic_results_hir(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission)
{
    return cm_semantic_results_is_current(results, admission)
        ? results->hir : NULL;
}

CmHirCrateId cm_semantic_results_crate(const CmSemanticResults *results,
    const CmSemanticAdmission *admission)
{
    return cm_semantic_results_is_current(results, admission)
        ? results->local_crate : CM_HIR_CRATE_NONE;
}

uint64_t cm_semantic_results_generation(const CmSemanticResults *results,
    const CmSemanticAdmission *admission)
{
    return cm_semantic_results_is_current(results, admission)
        ? results->semantic_generation : UINT64_C(0);
}

size_t cm_semantic_results_body_count(const CmSemanticResults *results,
    const CmSemanticAdmission *admission)
{
    return cm_semantic_results_is_current(results, admission)
        ? results->admitted_body_count : 0u;
}

CmSemanticResultsStatus cm_semantic_results_body_at(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, size_t index,
    CmSemanticBodyView *out_view)
{
    CmSemanticResultsStatus status;
    size_t body_index;
    size_t present_index;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (index >= results->admitted_body_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    present_index = 0u;
    for (body_index = 0u; body_index < results->body_count; ++body_index) {
        if (!results->bodies[body_index].present) continue;
        if (present_index == index) {
            return cm_semantic_results_body(results, admission,
                (CmHirBodyId)(body_index + 1u), out_view);
        }
        present_index += 1u;
    }
    return CM_SEMANTIC_RESULTS_NOT_FOUND;
}

CmSemanticResultsStatus cm_semantic_results_body(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, CmHirBodyId body,
    CmSemanticBodyView *out_view)
{
    const CmSemanticBodyRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (body == CM_HIR_BODY_NONE || (size_t)body > results->body_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    record = &results->bodies[(size_t)body - 1u];
    if (!record->present) return CM_SEMANTIC_RESULTS_NOT_FOUND;
    out_view->body = body;
    out_view->owner = record->owner;
    out_view->expression_count = record->expression_count;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_expression(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, CmHirBodyId body,
    CmHirExprId expression,
    CmSemanticExpressionView *out_view)
{
    const CmSemanticExpressionRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->direct_callable = cm_hir_def_id_none();
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (expression == CM_HIR_EXPR_NONE
        || (size_t)expression > results->expression_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    record = &results->expressions[(size_t)expression - 1u];
    if (!record->present || record->body != body) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    out_view->expression = expression;
    out_view->body = record->body;
    out_view->unadjusted_type.bytes = results->type_bytes
        + record->type_offset;
    out_view->unadjusted_type.size = record->type_size;
    out_view->adjusted_type = out_view->unadjusted_type;
    out_view->adjustment_count = 0u;
    out_view->has_direct_callable = record->has_direct_callable;
    out_view->direct_callable = record->direct_callable;
    out_view->has_primitive_operator = record->has_primitive_operator;
    out_view->primitive_operator = record->primitive_operator;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_type_view_equal(
    const CmSemanticTypeView *left, const CmSemanticTypeView *right,
    int *out_equal)
{
    if (left == NULL || right == NULL || out_equal == NULL
        || (left->size != 0u && left->bytes == NULL)
        || (right->size != 0u && right->bytes == NULL)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    *out_equal = left->size == right->size
        && (left->size == 0u
            || memcmp(left->bytes, right->bytes, left->size) == 0);
    return CM_SEMANTIC_RESULTS_OK;
}

static int cm_results_hir_type_is_monomorphic(const CmHirContext *hir,
    CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    if (hir == NULL || depth >= CM_RESULTS_TYPE_DEPTH) return 0;
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        return 1;
    case CM_HIR_TYPE_ADT_KIND:
        if (cm_hir_def_id_is_none(type->data.named_type.definition)
            || (type->data.named_type.argument_count == 0u)
                != (type->data.named_type.arguments == NULL)) return 0;
        for (index = 0u; index < type->data.named_type.argument_count;
             ++index) {
            const CmHirGenericArg *argument;

            argument = &type->data.named_type.arguments[index];
            if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
                if (!cm_results_hir_type_is_monomorphic(hir,
                        argument->data.type, depth + 1u)) return 0;
            } else if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
                if (argument->data.lifetime.kind != CM_HIR_REGION_STATIC
                    && argument->data.lifetime.kind
                        != CM_HIR_REGION_ERASED) return 0;
            } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
                if (argument->data.constant.kind != CM_HIR_CONST_VALUE
                    || !cm_results_hir_type_is_monomorphic(hir,
                        argument->data.constant.type, depth + 1u)) return 0;
            } else {
                return 0;
            }
        }
        return 1;
    default:
        return 0;
    }
}

CmSemanticResultsStatus cm_semantic_type_view_matches_monomorphic_hir(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission,
    const CmSemanticTypeView *view, CmHirTypeId type, int *out_equal)
{
    CmTypeckContext typeck;
    CmTypeckTypeId imported;
    CmResultsBuffer sizing;
    CmResultsBuffer output;
    unsigned char *bytes;
    CmSemanticResultsStatus status;
    uintptr_t view_start;
    uintptr_t bytes_start;
    uintptr_t bytes_end;

    if (out_equal == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    *out_equal = 0;
    status = cm_results_validate(results, admission);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (view == NULL || view->bytes == NULL || view->size == 0u
        || !cm_results_hir_type_is_monomorphic(results->hir, type, 0u)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    view_start = (uintptr_t)view->bytes;
    bytes_start = (uintptr_t)results->type_bytes;
    bytes_end = bytes_start + results->type_bytes_len;
    if (view_start < bytes_start || view_start > bytes_end
        || view->size > (size_t)(bytes_end - view_start)) {
        return CM_SEMANTIC_RESULTS_FOREIGN;
    }
    memset(&typeck, 0, sizeof(typeck));
    cm_typeck_context_init(&typeck, results->hir);
    cm_typeck_context_track_hir_semantic_generation(&typeck);
    if (cm_typeck_import_hir_type(&typeck, type, &imported)
            != CM_TYPECK_OK) {
        cm_typeck_context_destroy(&typeck);
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    memset(&sizing, 0, sizeof(sizing));
    sizing.sizing = 1;
    status = cm_results_typeck_type(&sizing, results->hir, &typeck,
        imported, 0u);
    bytes = status != CM_SEMANTIC_RESULTS_OK || sizing.len == 0u ? NULL
        : (unsigned char *)cm_alloc(sizing.len);
    if (status == CM_SEMANTIC_RESULTS_OK) {
        memset(&output, 0, sizeof(output));
        output.data = bytes;
        output.cap = sizing.len;
        status = cm_results_typeck_type(&output, results->hir, &typeck,
            imported, 0u);
        if (status == CM_SEMANTIC_RESULTS_OK && output.len != sizing.len) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
    }
    if (status == CM_SEMANTIC_RESULTS_OK) {
        *out_equal = view->size == sizing.len
            && memcmp(view->bytes, bytes, sizing.len) == 0;
    }
    cm_free(bytes);
    cm_typeck_context_destroy(&typeck);
    return status;
}

const char *cm_semantic_results_status_name(CmSemanticResultsStatus status)
{
    switch (status) {
    case CM_SEMANTIC_RESULTS_OK: return "ok";
    case CM_SEMANTIC_RESULTS_INVALID_ARGUMENT: return "invalid-argument";
    case CM_SEMANTIC_RESULTS_STALE: return "stale";
    case CM_SEMANTIC_RESULTS_FOREIGN: return "foreign";
    case CM_SEMANTIC_RESULTS_NOT_FOUND: return "not-found";
    case CM_SEMANTIC_RESULTS_INVALID_HIR: return "invalid-hir";
    case CM_SEMANTIC_RESULTS_DEFERRED_INFERENCE:
        return "deferred-inference";
    case CM_SEMANTIC_RESULTS_PENDING_PROJECTION:
        return "pending-projection";
    case CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE: return "unsupported-type";
    case CM_SEMANTIC_RESULTS_OVERFLOW: return "overflow";
    }
    return "unknown";
}
