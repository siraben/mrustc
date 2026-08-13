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

typedef struct CmSemanticAdjustmentRecord {
    CmSemanticAdjustmentKind kind;
    CmSemanticTypeRecord source_type;
    CmSemanticTypeRecord target_type;
    int has_selected_trait;
    CmHirDefId selected_trait;
    CmHirDefId selected_method;
    CmHirDefId selected_impl;
} CmSemanticAdjustmentRecord;

typedef struct CmSemanticExpressionRecord {
    int present;
    CmHirBodyId body;
    size_t type_offset;
    size_t type_size;
    size_t adjusted_type_offset;
    size_t adjusted_type_size;
    size_t adjustment_start;
    uint32_t adjustment_count;
    int has_direct_callable;
    CmHirDefId direct_callable;
    size_t call_return_offset;
    size_t call_return_size;
    size_t call_parameter_start;
    uint32_t call_parameter_count;
    size_t canonical_callee_index;
    int has_primitive_operator;
    CmHirBinaryOperator primitive_operator;
    CmHirExprId primitive_left_expression;
    CmHirExprId primitive_right_expression;
    CmSemanticTypeRecord primitive_left_type;
    CmSemanticTypeRecord primitive_right_type;
    CmSemanticTypeRecord primitive_result_type;
    int has_field_selection;
    CmHirExprId field_base_expression;
    CmHirDefId field_aggregate_definition;
    uint32_t field_index;
    CmSemanticTypeRecord field_base_type;
    CmSemanticTypeRecord field_type;
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
    CmSemanticTypeRecord *call_parameters;
    size_t call_parameter_count;
    CmSemanticAdjustmentRecord *adjustments;
    size_t adjustment_count;
    CmHirCanonicalInstance *callees;
    size_t callee_count;
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
    CmSemanticAdjustmentRecord *adjustments;
    size_t adjustment_count;
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
    CmSemanticAdjustmentRecord *adjustments;
    size_t adjustment_count;
    size_t call_count;
    uint32_t body_expression_count;
} CmSemanticResultsBodyStageState;

static CmSemanticResultsStatus cm_results_validate(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission);
static int cm_results_instance_type_equal(
    const CmSemanticInstanceRecord *left, size_t left_offset,
    size_t left_size, const CmSemanticInstanceRecord *right,
    size_t right_offset, size_t right_size);
static int cm_results_type_bytes_equal(const CmSemanticResults *results,
    size_t left_offset, size_t left_size, size_t right_offset,
    size_t right_size);

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
        record->adjusted_type_offset = record->type_offset;
        record->adjusted_type_size = record->type_size;
        record->present = 1;
        record->body = body_id;
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

static int cm_results_type_records_equal(const unsigned char *bytes,
    size_t bytes_len, const CmSemanticTypeRecord *left,
    const CmSemanticTypeRecord *right)
{
    if (bytes == NULL || left == NULL || right == NULL
        || left->type_size == 0u || left->type_size != right->type_size
        || left->type_offset > bytes_len
        || left->type_size > bytes_len - left->type_offset
        || right->type_offset > bytes_len
        || right->type_size > bytes_len - right->type_offset) {
        return 0;
    }
    return memcmp(bytes + left->type_offset, bytes + right->type_offset,
        left->type_size) == 0;
}

static int cm_results_adjustment_kind_valid(CmSemanticAdjustmentKind kind)
{
    return kind >= CM_SEMANTIC_ADJUSTMENT_DEREFERENCE_BUILTIN
        && kind <= CM_SEMANTIC_ADJUSTMENT_NEVER_TO_ANY;
}

static CmSemanticResultsStatus cm_results_validate_membership(
    const CmSemanticResults *results, const CmHirContext *hir)
{
    size_t expression_index;
    size_t seen_adjustments;

    if (results == NULL || hir == NULL
        || (results->expression_count != 0u && results->expressions == NULL)
        || (results->type_bytes_len != 0u && results->type_bytes == NULL)
        || (results->adjustment_count != 0u
            && results->adjustments == NULL)) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    seen_adjustments = 0u;

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
        if (record->present
            && (((expression->kind == CM_HIR_EXPR_BINARY)
                    != record->has_primitive_operator)
                || ((expression->kind == CM_HIR_EXPR_FIELD)
                    != record->has_field_selection))) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        if (record->present) {
            size_t adjustment_index;

            if ((record->type_size == 0u)
                || (record->adjusted_type_size == 0u)
                || record->type_offset > results->type_bytes_len
                || record->type_size
                    > results->type_bytes_len - record->type_offset
                || record->adjusted_type_offset > results->type_bytes_len
                || record->adjusted_type_size > results->type_bytes_len
                    - record->adjusted_type_offset
                || !cm_size_add(seen_adjustments,
                    record->adjustment_count, &adjustment_index)
                || adjustment_index > results->adjustment_count
                || (record->adjustment_count != 0u
                    && (record->adjustment_start
                            > results->adjustment_count
                        || record->adjustment_count
                            > results->adjustment_count
                                - record->adjustment_start))) {
                return CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
            if (record->adjustment_count != 0u) {
                size_t prior_expression;

                for (prior_expression = 0u;
                     prior_expression < expression_index;
                     ++prior_expression) {
                    const CmSemanticExpressionRecord *prior;
                    size_t record_end;
                    size_t prior_end;

                    prior = &results->expressions[prior_expression];
                    if (!prior->present || prior->adjustment_count == 0u) {
                        continue;
                    }
                    record_end = record->adjustment_start
                        + record->adjustment_count;
                    prior_end = prior->adjustment_start
                        + prior->adjustment_count;
                    if (record->adjustment_start < prior_end
                        && prior->adjustment_start < record_end) {
                        return CM_SEMANTIC_RESULTS_INVALID_HIR;
                    }
                }
            }
            if (record->adjustment_count == 0u) {
                if (!cm_results_type_bytes_equal(results,
                        record->type_offset, record->type_size,
                        record->adjusted_type_offset,
                        record->adjusted_type_size)) {
                    return CM_SEMANTIC_RESULTS_INVALID_HIR;
                }
            } else {
                size_t local;

                for (local = 0u; local < record->adjustment_count; ++local) {
                    const CmSemanticAdjustmentRecord *adjustment;

                    adjustment = &results->adjustments[
                        record->adjustment_start + local];
                    if (!cm_results_adjustment_kind_valid(adjustment->kind)
                        || (adjustment->kind
                                == CM_SEMANTIC_ADJUSTMENT_DEREFERENCE_TRAIT)
                            != adjustment->has_selected_trait
                        || (adjustment->has_selected_trait
                            && (cm_hir_def_id_is_none(
                                    adjustment->selected_trait)
                                || cm_hir_def_id_is_none(
                                    adjustment->selected_method)
                                || cm_hir_def_id_is_none(
                                    adjustment->selected_impl)))
                        || (!adjustment->has_selected_trait
                            && (!cm_hir_def_id_is_none(
                                    adjustment->selected_trait)
                                || !cm_hir_def_id_is_none(
                                    adjustment->selected_method)
                                || !cm_hir_def_id_is_none(
                                    adjustment->selected_impl)))) {
                        return CM_SEMANTIC_RESULTS_INVALID_HIR;
                    }
                    if (local == 0u) {
                        if (!cm_results_type_bytes_equal(results,
                                record->type_offset, record->type_size,
                                adjustment->source_type.type_offset,
                                adjustment->source_type.type_size)) {
                            return CM_SEMANTIC_RESULTS_INVALID_HIR;
                        }
                    } else {
                        const CmSemanticAdjustmentRecord *previous;

                        previous = adjustment - 1;
                        if (!cm_results_type_bytes_equal(results,
                                previous->target_type.type_offset,
                                previous->target_type.type_size,
                                adjustment->source_type.type_offset,
                                adjustment->source_type.type_size)) {
                            return CM_SEMANTIC_RESULTS_INVALID_HIR;
                        }
                    }
                    if (local + 1u == record->adjustment_count
                        && !cm_results_type_bytes_equal(results,
                            adjustment->target_type.type_offset,
                            adjustment->target_type.type_size,
                            record->adjusted_type_offset,
                            record->adjusted_type_size)) {
                        return CM_SEMANTIC_RESULTS_INVALID_HIR;
                    }
                }
            }
            seen_adjustments = adjustment_index;
        }
    }
    if (seen_adjustments != results->adjustment_count) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    for (expression_index = 0u;
         expression_index < results->adjustment_count;
         ++expression_index) {
        size_t owner_count;
        size_t record_index;

        owner_count = 0u;
        for (record_index = 0u; record_index < results->expression_count;
             ++record_index) {
            const CmSemanticExpressionRecord *record;

            record = &results->expressions[record_index];
            if (record->present && record->adjustment_count != 0u
                && expression_index >= record->adjustment_start
                && expression_index - record->adjustment_start
                    < record->adjustment_count) {
                owner_count += 1u;
            }
        }
        if (owner_count != 1u) return CM_SEMANTIC_RESULTS_INVALID_HIR;
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

static int cm_results_instance_adjustments_valid(
    const CmSemanticInstanceRecord *instance)
{
    size_t expression_index;
    size_t counted;

    if (instance == NULL
        || (instance->expression_count != 0u && instance->expressions == NULL)
        || (instance->type_bytes_len != 0u && instance->type_bytes == NULL)
        || (instance->adjustment_count != 0u
            && instance->adjustments == NULL)) return 0;
    counted = 0u;
    for (expression_index = 0u;
         expression_index < instance->expression_count; ++expression_index) {
        const CmSemanticExpressionRecord *record;
        size_t local;

        record = &instance->expressions[expression_index];
        if (!record->present) continue;
        if (record->type_size == 0u
            || record->type_offset > instance->type_bytes_len
            || record->type_size > instance->type_bytes_len
                - record->type_offset
            || record->adjusted_type_size == 0u
            || record->adjusted_type_offset > instance->type_bytes_len
            || record->adjusted_type_size > instance->type_bytes_len
                - record->adjusted_type_offset
            || record->adjustment_start > instance->adjustment_count
            || record->adjustment_count > instance->adjustment_count
                - record->adjustment_start) {
            return 0;
        }
        if (record->adjustment_count == 0u) {
            if (!cm_results_instance_type_equal(instance,
                    record->type_offset, record->type_size, instance,
                    record->adjusted_type_offset,
                    record->adjusted_type_size)) return 0;
            continue;
        }
        {
            size_t prior_expression;

            for (prior_expression = 0u;
                 prior_expression < expression_index; ++prior_expression) {
                const CmSemanticExpressionRecord *prior;
                size_t record_end;
                size_t prior_end;

                prior = &instance->expressions[prior_expression];
                if (!prior->present || prior->adjustment_count == 0u) {
                    continue;
                }
                record_end = record->adjustment_start
                    + record->adjustment_count;
                prior_end = prior->adjustment_start
                    + prior->adjustment_count;
                if (record->adjustment_start < prior_end
                    && prior->adjustment_start < record_end) return 0;
            }
        }
        if (!cm_size_add(counted, record->adjustment_count, &counted)) {
            return 0;
        }
        for (local = 0u; local < record->adjustment_count; ++local) {
            const CmSemanticAdjustmentRecord *adjustment;
            const CmSemanticTypeRecord *expected_source;

            adjustment = &instance->adjustments[
                record->adjustment_start + local];
            expected_source = local == 0u
                ? &(CmSemanticTypeRecord){ record->type_offset,
                    record->type_size }
                : &instance->adjustments[
                    record->adjustment_start + local - 1u].target_type;
            if (!cm_results_adjustment_kind_valid(adjustment->kind)
                || ((adjustment->kind
                        == CM_SEMANTIC_ADJUSTMENT_DEREFERENCE_TRAIT)
                    != adjustment->has_selected_trait)
                || (adjustment->has_selected_trait
                    && (cm_hir_def_id_is_none(adjustment->selected_trait)
                        || cm_hir_def_id_is_none(adjustment->selected_method)
                        || cm_hir_def_id_is_none(adjustment->selected_impl)))
                || (!adjustment->has_selected_trait
                    && (!cm_hir_def_id_is_none(adjustment->selected_trait)
                        || !cm_hir_def_id_is_none(
                            adjustment->selected_method)
                        || !cm_hir_def_id_is_none(
                            adjustment->selected_impl)))
                || !cm_results_instance_type_equal(instance,
                    expected_source->type_offset, expected_source->type_size,
                    instance, adjustment->source_type.type_offset,
                    adjustment->source_type.type_size)) {
                return 0;
            }
        }
        if (!cm_results_instance_type_equal(instance,
                instance->adjustments[record->adjustment_start
                    + record->adjustment_count - 1u].target_type.type_offset,
                instance->adjustments[record->adjustment_start
                    + record->adjustment_count - 1u].target_type.type_size,
                instance, record->adjusted_type_offset,
                record->adjusted_type_size)) {
            return 0;
        }
    }
    return counted == instance->adjustment_count;
}

static int cm_results_instance_recipes_valid(
    const CmSemanticInstanceRecord *instance, const CmHirContext *hir)
{
    size_t expression_index;

    if (instance == NULL || hir == NULL
        || (instance->expression_count != 0u && instance->expressions == NULL)
        || (instance->type_bytes_len != 0u && instance->type_bytes == NULL)) {
        return 0;
    }
    for (expression_index = 0u;
         expression_index < instance->expression_count; ++expression_index) {
        const CmSemanticExpressionRecord *record;
        const CmHirExpr *expression;

        record = &instance->expressions[expression_index];
        if (!record->present) continue;
        expression = cm_hir_get_expr(hir,
            (CmHirExprId)(expression_index + 1u));
        if (expression == NULL || expression->owner_body != record->body
            || ((expression->kind == CM_HIR_EXPR_BINARY)
                != record->has_primitive_operator)
            || ((expression->kind == CM_HIR_EXPR_FIELD)
                != record->has_field_selection)) return 0;
        if (record->has_primitive_operator) {
            const CmSemanticExpressionRecord *left;
            const CmSemanticExpressionRecord *right;

            if (record->primitive_left_expression == CM_HIR_EXPR_NONE
                || record->primitive_right_expression == CM_HIR_EXPR_NONE
                || (size_t)record->primitive_left_expression
                    > instance->expression_count
                || (size_t)record->primitive_right_expression
                    > instance->expression_count
                || expression->data.binary.operator_kind
                    != record->primitive_operator
                || expression->data.binary.left
                    != record->primitive_left_expression
                || expression->data.binary.right
                    != record->primitive_right_expression) return 0;
            left = &instance->expressions[
                (size_t)record->primitive_left_expression - 1u];
            right = &instance->expressions[
                (size_t)record->primitive_right_expression - 1u];
            if (!left->present || !right->present
                || !cm_results_instance_type_equal(instance,
                    record->primitive_left_type.type_offset,
                    record->primitive_left_type.type_size, instance,
                    left->adjusted_type_offset, left->adjusted_type_size)
                || !cm_results_instance_type_equal(instance,
                    record->primitive_right_type.type_offset,
                    record->primitive_right_type.type_size, instance,
                    right->adjusted_type_offset, right->adjusted_type_size)
                || !cm_results_instance_type_equal(instance,
                    record->primitive_result_type.type_offset,
                    record->primitive_result_type.type_size, instance,
                    record->adjusted_type_offset,
                    record->adjusted_type_size)) return 0;
        }
        if (record->has_field_selection) {
            const CmSemanticExpressionRecord *base;

            if (record->field_base_expression == CM_HIR_EXPR_NONE
                || (size_t)record->field_base_expression
                    > instance->expression_count
                || expression->data.field.base
                    != record->field_base_expression
                || !cm_hir_def_id_equal(expression->data.field.definition,
                    record->field_aggregate_definition)
                || expression->data.field.field_index != record->field_index) {
                return 0;
            }
            base = &instance->expressions[
                (size_t)record->field_base_expression - 1u];
            if (!base->present
                || !cm_results_instance_type_equal(instance,
                    record->field_base_type.type_offset,
                    record->field_base_type.type_size, instance,
                    base->adjusted_type_offset, base->adjusted_type_size)
                || !cm_results_instance_type_equal(instance,
                    record->field_type.type_offset,
                    record->field_type.type_size, instance,
                    record->adjusted_type_offset,
                    record->adjusted_type_size)) return 0;
        }
    }
    return 1;
}

static int cm_results_body_recipes_valid(const CmSemanticResults *results,
    const CmHirContext *hir)
{
    CmSemanticInstanceRecord view;

    if (results == NULL) return 0;
    memset(&view, 0, sizeof(view));
    view.expressions = results->expressions;
    view.expression_count = results->expression_count;
    view.type_bytes = results->type_bytes;
    view.type_bytes_len = results->type_bytes_len;
    return cm_results_instance_recipes_valid(&view, hir);
}

static int cm_results_instance_membership_valid(
    const CmSemanticInstanceRecord *instance, const CmHirContext *hir)
{
    const CmHirBody *body;
    size_t expression_index;
    uint32_t present_count;

    if (instance == NULL || hir == NULL || !instance->body.present
        || instance->identity.body == CM_HIR_BODY_NONE
        || !cm_hir_def_id_equal(instance->identity.definition,
            instance->body.owner)
        || instance->expression_count != hir->expressions.len
        || (instance->expression_count != 0u
            && instance->expressions == NULL)) return 0;
    body = cm_hir_get_body(hir, instance->identity.body);
    if (body == NULL || body->state != CM_HIR_BODY_TYPED
        || !cm_hir_def_id_equal(body->owner, instance->identity.definition)) {
        return 0;
    }
    present_count = 0u;
    for (expression_index = 0u;
         expression_index < instance->expression_count; ++expression_index) {
        const CmHirExpr *expression;
        const CmSemanticExpressionRecord *record;
        int owned;

        expression = cm_hir_get_expr(hir,
            (CmHirExprId)(expression_index + 1u));
        record = &instance->expressions[expression_index];
        if (expression == NULL) return 0;
        owned = expression->owner_body == instance->identity.body;
        if (owned != record->present
            || (record->present
                && record->body != instance->identity.body)) return 0;
        if (record->present) {
            if (present_count == UINT32_MAX) return 0;
            present_count += 1u;
        }
    }
    return present_count == instance->body.expression_count;
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
        cm_free(state->adjustments);
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
    size_t adjustment_bytes;
    uint32_t expression_count;

    stage = (CmSemanticResultsBodyStage *)context;
    if (stage == NULL || stage->state != NULL || session == NULL
        || body_id == CM_HIR_BODY_NONE || facts == NULL
        || facts->expression_terms == NULL
        || facts->signature_return_type == CM_TYPECK_TYPE_NONE
        || (facts->signature_parameter_count != 0u
            && facts->signature_parameter_types == NULL)
        || (facts->call_count != 0u && facts->calls == NULL)
        || (facts->adjustment_count != 0u && facts->adjustments == NULL)
        || (facts->primitive_binary_count != 0u
            && facts->primitive_binaries == NULL)
        || (facts->field_selection_count != 0u
            && facts->field_selections == NULL)
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
            sizeof(CmSemanticTypeRecord), &call_parameter_bytes)
        || !cm_size_mul(facts->adjustment_count,
            sizeof(CmSemanticAdjustmentRecord), &adjustment_bytes)) {
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
    state->adjustment_count = facts->adjustment_count;
    state->adjustments = adjustment_bytes == 0u ? NULL
        : (CmSemanticAdjustmentRecord *)cm_alloc_zeroed(1u,
            adjustment_bytes);
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
    for (expression_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && expression_index < facts->adjustment_count;
         ++expression_index) {
        const CmSemanticCheckedAdjustmentFacts *adjustment;

        adjustment = &facts->adjustments[expression_index];
        status = cm_results_collect_type_record(&sizing, hir, typeck,
            adjustment->source_type, 0, NULL);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_type_record(&sizing, hir, typeck,
                adjustment->target_type, 0, NULL);
        }
    }
    for (expression_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && expression_index < facts->primitive_binary_count;
         ++expression_index) {
        const CmSemanticCheckedPrimitiveBinaryFacts *binary;

        binary = &facts->primitive_binaries[expression_index];
        status = cm_results_collect_type_record(&sizing, hir, typeck,
            binary->left_type, 0, NULL);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_type_record(&sizing, hir, typeck,
                binary->right_type, 0, NULL);
        }
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_type_record(&sizing, hir, typeck,
                binary->result_type, 0, NULL);
        }
    }
    for (expression_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && expression_index < facts->field_selection_count;
         ++expression_index) {
        const CmSemanticCheckedFieldSelectionFacts *field;

        field = &facts->field_selections[expression_index];
        status = cm_results_collect_type_record(&sizing, hir, typeck,
            field->base_type, 0, NULL);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_type_record(&sizing, hir, typeck,
                field->field_type, 0, NULL);
        }
    }
    if (status != CM_SEMANTIC_RESULTS_OK) {
        cm_free(seen);
        cm_free(state->adjustments);
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
            cm_free(state->adjustments);
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
        record->canonical_callee_index = (size_t)-1;
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
    for (expression_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && expression_index < facts->adjustment_count;
         ++expression_index) {
        const CmSemanticCheckedAdjustmentFacts *fact;
        CmSemanticAdjustmentRecord *record;
        CmSemanticExpressionRecord *expression_record;
        size_t previous;

        fact = &facts->adjustments[expression_index];
        if (fact->expression == CM_HIR_EXPR_NONE
            || (size_t)fact->expression > state->expression_count
            || !cm_results_adjustment_kind_valid(fact->kind)
            || (fact->kind == CM_SEMANTIC_ADJUSTMENT_DEREFERENCE_TRAIT)
                != fact->has_selected_trait
            || (fact->has_selected_trait
                && (cm_hir_def_id_is_none(fact->selected_trait)
                    || cm_hir_def_id_is_none(fact->selected_method)
                    || cm_hir_def_id_is_none(fact->selected_impl)))) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        expression_record = &state->expressions[
            (size_t)fact->expression - 1u];
        if (!expression_record->present
            || expression_record->body != body_id
            || expression_record->adjustment_count == UINT32_MAX) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        previous = expression_record->adjustment_count;
        if (previous == 0u) {
            expression_record->adjustment_start = expression_index;
        } else if (expression_record->adjustment_start
                != expression_index - previous) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record = &state->adjustments[expression_index];
        record->kind = fact->kind;
        record->has_selected_trait = fact->has_selected_trait;
        record->selected_trait = fact->selected_trait;
        record->selected_method = fact->selected_method;
        record->selected_impl = fact->selected_impl;
        status = cm_results_collect_type_record(&output, hir, typeck,
            fact->source_type, 1, &record->source_type);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_type_record(&output, hir, typeck,
                fact->target_type, 1, &record->target_type);
        }
        if (status != CM_SEMANTIC_RESULTS_OK) break;
        if ((previous == 0u
                && !cm_results_type_records_equal(state->type_bytes,
                    sizing.len, &(CmSemanticTypeRecord){
                        expression_record->type_offset,
                        expression_record->type_size },
                    &record->source_type))
            || (previous != 0u
                && !cm_results_type_records_equal(state->type_bytes,
                    sizing.len,
                    &state->adjustments[expression_index - 1u].target_type,
                    &record->source_type))) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        expression_record->adjusted_type_offset =
            record->target_type.type_offset;
        expression_record->adjusted_type_size =
            record->target_type.type_size;
        expression_record->adjustment_count += 1u;
    }
    for (expression_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && expression_index < facts->primitive_binary_count;
         ++expression_index) {
        const CmSemanticCheckedPrimitiveBinaryFacts *fact;
        const CmHirExpr *expression;
        CmSemanticExpressionRecord *record;

        fact = &facts->primitive_binaries[expression_index];
        expression = cm_hir_get_expr(hir, fact->expression);
        if (expression == NULL || expression->owner_body != body_id
            || expression->kind != CM_HIR_EXPR_BINARY
            || expression->data.binary.operator_kind != fact->operator_kind
            || expression->data.binary.left != fact->left_expression
            || expression->data.binary.right != fact->right_expression) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record = &state->expressions[(size_t)fact->expression - 1u];
        if (!record->present || record->has_primitive_operator) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record->has_primitive_operator = 1;
        record->primitive_operator = fact->operator_kind;
        record->primitive_left_expression = fact->left_expression;
        record->primitive_right_expression = fact->right_expression;
        status = cm_results_collect_type_record(&output, hir, typeck,
            fact->left_type, 1, &record->primitive_left_type);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_type_record(&output, hir, typeck,
                fact->right_type, 1, &record->primitive_right_type);
        }
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_type_record(&output, hir, typeck,
                fact->result_type, 1, &record->primitive_result_type);
        }
    }
    for (expression_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && expression_index < facts->field_selection_count;
         ++expression_index) {
        const CmSemanticCheckedFieldSelectionFacts *fact;
        const CmHirExpr *expression;
        CmSemanticExpressionRecord *record;

        fact = &facts->field_selections[expression_index];
        expression = cm_hir_get_expr(hir, fact->expression);
        if (expression == NULL || expression->owner_body != body_id
            || expression->kind != CM_HIR_EXPR_FIELD
            || expression->data.field.base != fact->base_expression
            || !cm_hir_def_id_equal(expression->data.field.definition,
                fact->aggregate_definition)
            || expression->data.field.field_index != fact->field_index) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record = &state->expressions[(size_t)fact->expression - 1u];
        if (!record->present || record->has_field_selection) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record->has_field_selection = 1;
        record->field_base_expression = fact->base_expression;
        record->field_aggregate_definition = fact->aggregate_definition;
        record->field_index = fact->field_index;
        status = cm_results_collect_type_record(&output, hir, typeck,
            fact->base_type, 1, &record->field_base_type);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_type_record(&output, hir, typeck,
                fact->field_type, 1, &record->field_type);
        }
    }
    for (expression_index = 0u;
         status == CM_SEMANTIC_RESULTS_OK
            && expression_index < state->expression_count;
         ++expression_index) {
        const CmHirExpr *expression;
        const CmSemanticExpressionRecord *record;

        expression = cm_hir_get_expr(hir,
            (CmHirExprId)(expression_index + 1u));
        record = &state->expressions[expression_index];
        if (!record->present) continue;
        if (((expression->kind == CM_HIR_EXPR_BINARY)
                != record->has_primitive_operator)
            || ((expression->kind == CM_HIR_EXPR_FIELD)
                != record->has_field_selection)) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            continue;
        }
        if (record->has_primitive_operator) {
            const CmSemanticExpressionRecord *left;
            const CmSemanticExpressionRecord *right;

            left = &state->expressions[
                (size_t)record->primitive_left_expression - 1u];
            right = &state->expressions[
                (size_t)record->primitive_right_expression - 1u];
            if (!left->present || !right->present
                || !cm_results_type_records_equal(state->type_bytes,
                    sizing.len, &record->primitive_left_type,
                    &(CmSemanticTypeRecord){ left->adjusted_type_offset,
                        left->adjusted_type_size })
                || !cm_results_type_records_equal(state->type_bytes,
                    sizing.len, &record->primitive_right_type,
                    &(CmSemanticTypeRecord){ right->adjusted_type_offset,
                        right->adjusted_type_size })
                || !cm_results_type_records_equal(state->type_bytes,
                    sizing.len, &record->primitive_result_type,
                    &(CmSemanticTypeRecord){ record->adjusted_type_offset,
                        record->adjusted_type_size })) {
                status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
        }
        if (record->has_field_selection) {
            const CmSemanticExpressionRecord *base;

            base = &state->expressions[
                (size_t)record->field_base_expression - 1u];
            if (!base->present
                || !cm_results_type_records_equal(state->type_bytes,
                    sizing.len, &record->field_base_type,
                    &(CmSemanticTypeRecord){ base->adjusted_type_offset,
                        base->adjusted_type_size })
                || !cm_results_type_records_equal(state->type_bytes,
                    sizing.len, &record->field_type,
                    &(CmSemanticTypeRecord){ record->adjusted_type_offset,
                        record->adjusted_type_size })) {
                status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
        }
    }
    cm_free(seen);
    if (status != CM_SEMANTIC_RESULTS_OK || output.len != sizing.len) {
        cm_free(state->type_bytes);
        cm_free(state->adjustments);
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
    state->call_count = facts->call_count;
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
    CmSemanticAdjustmentRecord *combined_adjustments;
    size_t new_type_bytes_len;
    size_t new_signature_parameter_count;
    size_t new_call_parameter_count;
    size_t new_adjustment_count;
    size_t signature_parameter_bytes;
    size_t call_parameter_bytes;
    size_t adjustment_bytes;
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
        || !cm_size_add(results->adjustment_count,
            state->adjustment_count, &new_adjustment_count)
        || !cm_size_mul(new_signature_parameter_count,
            sizeof(*combined_signature_parameters),
            &signature_parameter_bytes)
        || !cm_size_mul(new_call_parameter_count,
            sizeof(*combined_call_parameters), &call_parameter_bytes)
        || !cm_size_mul(new_adjustment_count,
            sizeof(*combined_adjustments), &adjustment_bytes)) {
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    combined_type_bytes = new_type_bytes_len == 0u ? NULL
        : (unsigned char *)cm_alloc(new_type_bytes_len);
    combined_signature_parameters = signature_parameter_bytes == 0u ? NULL
        : (CmSemanticTypeRecord *)cm_alloc(signature_parameter_bytes);
    combined_call_parameters = call_parameter_bytes == 0u ? NULL
        : (CmSemanticTypeRecord *)cm_alloc(call_parameter_bytes);
    combined_adjustments = adjustment_bytes == 0u ? NULL
        : (CmSemanticAdjustmentRecord *)cm_alloc(adjustment_bytes);
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
    if (results->adjustment_count != 0u) {
        memcpy(combined_adjustments, results->adjustments,
            results->adjustment_count * sizeof(*combined_adjustments));
    }
    for (parameter_index = 0u;
         parameter_index < state->adjustment_count; ++parameter_index) {
        combined_adjustments[results->adjustment_count + parameter_index] =
            state->adjustments[parameter_index];
        combined_adjustments[results->adjustment_count + parameter_index]
            .source_type.type_offset += results->type_bytes_len;
        combined_adjustments[results->adjustment_count + parameter_index]
            .target_type.type_offset += results->type_bytes_len;
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
        state->expressions[expression_index].adjusted_type_offset +=
            results->type_bytes_len;
        if (state->expressions[expression_index].adjustment_count != 0u) {
            state->expressions[expression_index].adjustment_start +=
                results->adjustment_count;
        }
        if (state->expressions[expression_index].has_direct_callable) {
            state->expressions[expression_index].call_return_offset +=
                results->type_bytes_len;
            state->expressions[expression_index].call_parameter_start +=
                results->call_parameter_count;
        }
        if (state->expressions[expression_index].has_primitive_operator) {
            state->expressions[expression_index].primitive_left_type
                .type_offset += results->type_bytes_len;
            state->expressions[expression_index].primitive_right_type
                .type_offset += results->type_bytes_len;
            state->expressions[expression_index].primitive_result_type
                .type_offset += results->type_bytes_len;
        }
        if (state->expressions[expression_index].has_field_selection) {
            state->expressions[expression_index].field_base_type.type_offset +=
                results->type_bytes_len;
            state->expressions[expression_index].field_type.type_offset +=
                results->type_bytes_len;
        }
        results->expressions[expression_index] =
            state->expressions[expression_index];
    }
    cm_free(results->type_bytes);
    cm_free(results->signature_parameters);
    cm_free(results->call_parameters);
    cm_free(results->adjustments);
    results->type_bytes = combined_type_bytes;
    results->type_bytes_len = new_type_bytes_len;
    results->signature_parameters = combined_signature_parameters;
    results->signature_parameter_count = new_signature_parameter_count;
    results->call_parameters = combined_call_parameters;
    results->call_parameter_count = new_call_parameter_count;
    results->adjustments = combined_adjustments;
    results->adjustment_count = new_adjustment_count;
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
    const CmSemanticBodyResult *check, CmSemanticResultsBodyStage *stage,
    const CmSemanticCanonicalCallInput *calls, size_t call_count)
{
    CmSemanticResultsBodyStageState *state;
    CmSemanticInstanceRecord *combined;
    CmSemanticInstanceRecord *record;
    const CmHirContext *hir;
    size_t *expression_indices;
    size_t bytes;
    size_t call_index;

    state = stage == NULL ? NULL
        : (CmSemanticResultsBodyStageState *)stage->state;
    if (results == NULL || results->sealed || session == NULL
        || instance == NULL || check == NULL
        || check->status != CM_SEMANTIC_BODY_OK || state == NULL
        || (call_count == 0u) != (calls == NULL)
        || call_count != state->call_count
        || !cm_semantic_session_is_current(session)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    hir = cm_semantic_session_hir(session);
    if (hir == NULL || session != state->producer_session
        || cm_semantic_session_typeck(session) != state->producer_typeck
        || cm_semantic_session_universe(session) != state->universe
        || hir != results->hir || hir != state->hir
        || state->local_crate != results->local_crate
        || cm_semantic_session_local_crate(session) != results->local_crate
        || state->storage_lifetime_id != results->storage_lifetime_id
        || state->semantic_generation != results->semantic_generation
        || state->rewind_generation != results->rewind_generation
        || hir->storage.lifetime_id != results->storage_lifetime_id
        || hir->semantic_generation != results->semantic_generation
        || hir->rewind_generation != results->rewind_generation) {
        return CM_SEMANTIC_RESULTS_STALE;
    }
    if (state->body != instance->body
        || check->body != instance->body
        || state->expression_count != results->expression_count
        || !cm_hir_def_id_equal(state->owner, instance->definition)
        || !cm_hir_def_id_equal(cm_semantic_session_exact_owner(session),
            instance->definition)
        || cm_results_find_instance(results, instance) != NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    if (!cm_size_mul(call_count, sizeof(*expression_indices), &bytes)) {
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    expression_indices = call_count == 0u ? NULL
        : (size_t *)cm_alloc(bytes);
    for (call_index = 0u; call_index < call_count; ++call_index) {
        CmSemanticExpressionRecord *expression;
        size_t previous_call;

        if (calls[call_index].callee == NULL
            || calls[call_index].expression == CM_HIR_EXPR_NONE
            || (size_t)calls[call_index].expression
                > state->expression_count) {
            cm_free(expression_indices);
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        expression_indices[call_index] =
            (size_t)calls[call_index].expression - 1u;
        expression = &state->expressions[expression_indices[call_index]];
        if (!expression->present || !expression->has_direct_callable
            || !cm_hir_def_id_equal(expression->direct_callable,
                calls[call_index].callee->definition)) {
            cm_free(expression_indices);
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        for (previous_call = 0u; previous_call < call_index;
             ++previous_call) {
            if (calls[previous_call].expression
                    == calls[call_index].expression) {
                cm_free(expression_indices);
                return CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
        }
    }
    if (!cm_size_add(results->instance_count, 1u, &bytes)
        || !cm_size_mul(bytes, sizeof(*combined), &bytes)) {
        cm_free(expression_indices);
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
        cm_free(expression_indices);
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
    record->call_parameters = state->call_parameters;
    record->call_parameter_count = state->call_parameter_count;
    record->adjustments = state->adjustments;
    record->adjustment_count = state->adjustment_count;
    record->callees = call_count == 0u ? NULL
        : (CmHirCanonicalInstance *)cm_alloc_zeroed(call_count,
            sizeof(*record->callees));
    record->callee_count = call_count;
    for (call_index = 0u; call_index < call_count; ++call_index) {
        cm_hir_canonical_instance_init(&record->callees[call_index]);
        if (cm_hir_canonical_instance_clone(&record->callees[call_index],
                calls[call_index].callee) != CM_HIR_INSTANCE_OK) {
            size_t initialized_call;

            for (initialized_call = 0u; initialized_call <= call_index;
                 ++initialized_call) {
                cm_hir_canonical_instance_destroy(
                    &record->callees[initialized_call]);
            }
            cm_free(record->callees);
            cm_hir_canonical_instance_destroy(&record->identity);
            cm_free(expression_indices);
            cm_free(combined);
            return CM_SEMANTIC_RESULTS_OVERFLOW;
        }
    }
    for (call_index = 0u; call_index < call_count; ++call_index) {
        state->expressions[expression_indices[call_index]]
            .canonical_callee_index = call_index;
    }
    cm_free(expression_indices);
    state->expressions = NULL;
    state->type_bytes = NULL;
    state->signature_parameters = NULL;
    state->call_parameters = NULL;
    state->adjustments = NULL;
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
    if (!cm_results_body_recipes_valid(results, hir)) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
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
    if (!cm_results_body_recipes_valid(results, hir)) {
        cm_free(selected);
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
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
                expression->adjusted_type_offset,
                expression->adjusted_type_size,
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
    for (instance_count = 0u; instance_count < results->instance_count;
         ++instance_count) {
        const CmSemanticInstanceRecord *instance;
        size_t expression_index;

        instance = &results->instances[instance_count];
        if (!cm_results_instance_membership_valid(instance, hir)
            || instance->callee_count != 0u
            || !cm_results_instance_adjustments_valid(instance)
            || !cm_results_instance_recipes_valid(
                instance, hir)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        for (expression_index = 0u;
             expression_index < instance->expression_count;
             ++expression_index) {
            if (instance->expressions[expression_index].present
                && instance->expressions[expression_index]
                    .has_direct_callable) {
                return CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
        }
    }
    results->sealed = 1;
    return CM_SEMANTIC_RESULTS_OK;
}

static int cm_results_instance_type_equal(
    const CmSemanticInstanceRecord *left, size_t left_offset,
    size_t left_size, const CmSemanticInstanceRecord *right,
    size_t right_offset, size_t right_size)
{
    return left != NULL && right != NULL && left_size != 0u
        && left_size == right_size
        && left_offset <= left->type_bytes_len
        && left_size <= left->type_bytes_len - left_offset
        && right_offset <= right->type_bytes_len
        && right_size <= right->type_bytes_len - right_offset
        && (left_size == 0u || memcmp(left->type_bytes + left_offset,
            right->type_bytes + right_offset, left_size) == 0);
}

CmSemanticResultsStatus cm_semantic_results_seal_instance_closure(
    CmSemanticResults *results, size_t instance_count)
{
    const CmHirContext *hir;
    size_t instance_index;

    if (results == NULL || results->sealed || instance_count == 0u
        || results->admitted_body_count != 0u
        || results->instance_count != instance_count) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    hir = results->hir;
    if (hir == NULL || hir->storage.lifetime_id
            != results->storage_lifetime_id
        || hir->semantic_generation != results->semantic_generation
        || hir->rewind_generation != results->rewind_generation) {
        return CM_SEMANTIC_RESULTS_STALE;
    }
    for (instance_index = 0u; instance_index < results->instance_count;
         ++instance_index) {
        const CmSemanticInstanceRecord *caller;
        size_t expression_index;
        size_t seen_calls;

        caller = &results->instances[instance_index];
        if (!cm_results_instance_membership_valid(caller, hir)
            || !cm_results_instance_adjustments_valid(caller)
            || !cm_results_instance_recipes_valid(caller, hir)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        seen_calls = 0u;
        for (expression_index = 0u;
             expression_index < caller->expression_count;
             ++expression_index) {
            const CmSemanticExpressionRecord *expression;
            const CmSemanticInstanceRecord *callee;
            uint32_t parameter;
            size_t previous_expression;

            expression = &caller->expressions[expression_index];
            if (!expression->present || !expression->has_direct_callable) {
                continue;
            }
            if (expression->canonical_callee_index >= caller->callee_count) {
                return CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
            for (previous_expression = 0u;
                 previous_expression < expression_index;
                 ++previous_expression) {
                const CmSemanticExpressionRecord *previous;

                previous = &caller->expressions[previous_expression];
                if (previous->present && previous->has_direct_callable
                    && previous->canonical_callee_index
                        == expression->canonical_callee_index) {
                    return CM_SEMANTIC_RESULTS_INVALID_HIR;
                }
            }
            callee = cm_results_find_instance(results,
                &caller->callees[expression->canonical_callee_index]);
            if (callee == NULL
                || !cm_hir_def_id_equal(expression->direct_callable,
                    callee->identity.definition)
                || expression->call_parameter_count
                    != callee->body.signature_parameter_count
                || !cm_results_instance_type_equal(caller,
                    expression->adjusted_type_offset,
                    expression->adjusted_type_size,
                    caller, expression->call_return_offset,
                    expression->call_return_size)
                || !cm_results_instance_type_equal(caller,
                    expression->call_return_offset,
                    expression->call_return_size, callee,
                    callee->body.signature_return_offset,
                    callee->body.signature_return_size)) {
                return CM_SEMANTIC_RESULTS_INVALID_HIR;
            }
            for (parameter = 0u;
                 parameter < expression->call_parameter_count;
                 ++parameter) {
                const CmSemanticTypeRecord *call_type;
                const CmSemanticTypeRecord *signature_type;
                size_t call_parameter;
                size_t signature_parameter;

                if (!cm_size_add(expression->call_parameter_start,
                        (size_t)parameter, &call_parameter)
                    || !cm_size_add(
                        callee->body.signature_parameter_start,
                        (size_t)parameter, &signature_parameter)
                    || call_parameter >= caller->call_parameter_count
                    || signature_parameter
                        >= callee->signature_parameter_count) {
                    return CM_SEMANTIC_RESULTS_INVALID_HIR;
                }
                call_type = &caller->call_parameters[call_parameter];
                signature_type =
                    &callee->signature_parameters[signature_parameter];
                if (!cm_results_instance_type_equal(caller,
                        call_type->type_offset, call_type->type_size,
                        callee, signature_type->type_offset,
                        signature_type->type_size)) {
                    return CM_SEMANTIC_RESULTS_INVALID_HIR;
                }
            }
            ++seen_calls;
        }
        if (seen_calls != caller->callee_count) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
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
        {
            size_t call_index;

            for (call_index = 0u; call_index < record->callee_count;
                 ++call_index) {
                cm_hir_canonical_instance_destroy(
                    &record->callees[call_index]);
            }
        }
        cm_free(record->callees);
        cm_free(record->call_parameters);
        cm_free(record->adjustments);
        cm_free(record->signature_parameters);
        cm_free(record->type_bytes);
        cm_free(record->expressions);
        cm_hir_canonical_instance_destroy(&record->identity);
    }
    cm_free(results->instances);
    cm_free(results->call_parameters);
    cm_free(results->adjustments);
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
    out_view->adjusted_type.bytes = instance->type_bytes
        + record->adjusted_type_offset;
    out_view->adjusted_type.size = record->adjusted_type_size;
    out_view->adjustment_count = record->adjustment_count;
    out_view->has_direct_callable = record->has_direct_callable;
    out_view->direct_callable = record->direct_callable;
    out_view->has_primitive_operator = record->has_primitive_operator;
    out_view->primitive_operator = record->primitive_operator;
    return CM_SEMANTIC_RESULTS_OK;
}

static CmSemanticResultsStatus cm_results_adjustment_view(
    const unsigned char *type_bytes, size_t type_bytes_len,
    const CmSemanticAdjustmentRecord *adjustments, size_t adjustment_count,
    const CmSemanticExpressionRecord *expression_record,
    CmHirExprId expression, uint32_t adjustment,
    CmSemanticAdjustmentView *out_view)
{
    const CmSemanticAdjustmentRecord *record;
    size_t index;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->selected_trait = cm_hir_def_id_none();
    out_view->selected_method = cm_hir_def_id_none();
    out_view->selected_impl = cm_hir_def_id_none();
    if (type_bytes == NULL || expression_record == NULL
        || adjustment >= expression_record->adjustment_count
        || !cm_size_add(expression_record->adjustment_start,
            (size_t)adjustment, &index)
        || index >= adjustment_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    record = &adjustments[index];
    if (record->source_type.type_offset > type_bytes_len
        || record->source_type.type_size
            > type_bytes_len - record->source_type.type_offset
        || record->target_type.type_offset > type_bytes_len
        || record->target_type.type_size
            > type_bytes_len - record->target_type.type_offset) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    out_view->body = expression_record->body;
    out_view->expression = expression;
    out_view->index = adjustment;
    out_view->kind = record->kind;
    out_view->source_type.bytes = type_bytes
        + record->source_type.type_offset;
    out_view->source_type.size = record->source_type.type_size;
    out_view->target_type.bytes = type_bytes
        + record->target_type.type_offset;
    out_view->target_type.size = record->target_type.type_size;
    out_view->has_selected_trait = record->has_selected_trait;
    out_view->selected_trait = record->selected_trait;
    out_view->selected_method = record->selected_method;
    out_view->selected_impl = record->selected_impl;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_instance_expression_adjustment(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *spec, CmHirExprId expression,
    uint32_t adjustment, CmSemanticAdjustmentView *out_view)
{
    const CmSemanticInstanceRecord *instance;
    const CmSemanticExpressionRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->selected_trait = cm_hir_def_id_none();
    out_view->selected_method = cm_hir_def_id_none();
    out_view->selected_impl = cm_hir_def_id_none();
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
    return cm_results_adjustment_view(instance->type_bytes,
        instance->type_bytes_len, instance->adjustments,
        instance->adjustment_count, record, expression, adjustment, out_view);
}

static CmSemanticResultsStatus cm_results_primitive_binary_view(
    const unsigned char *type_bytes, size_t type_bytes_len,
    const CmSemanticExpressionRecord *record, CmHirExprId expression,
    CmSemanticPrimitiveBinaryView *out_view)
{
    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    if (type_bytes == NULL || record == NULL || !record->present
        || !record->has_primitive_operator) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    if (record->primitive_left_type.type_offset > type_bytes_len
        || record->primitive_left_type.type_size > type_bytes_len
            - record->primitive_left_type.type_offset
        || record->primitive_right_type.type_offset > type_bytes_len
        || record->primitive_right_type.type_size > type_bytes_len
            - record->primitive_right_type.type_offset
        || record->primitive_result_type.type_offset > type_bytes_len
        || record->primitive_result_type.type_size > type_bytes_len
            - record->primitive_result_type.type_offset) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    out_view->body = record->body;
    out_view->expression = expression;
    out_view->operator_kind = record->primitive_operator;
    out_view->left_expression = record->primitive_left_expression;
    out_view->right_expression = record->primitive_right_expression;
    out_view->left_type.bytes = type_bytes
        + record->primitive_left_type.type_offset;
    out_view->left_type.size = record->primitive_left_type.type_size;
    out_view->right_type.bytes = type_bytes
        + record->primitive_right_type.type_offset;
    out_view->right_type.size = record->primitive_right_type.type_size;
    out_view->result_type.bytes = type_bytes
        + record->primitive_result_type.type_offset;
    out_view->result_type.size = record->primitive_result_type.type_size;
    return CM_SEMANTIC_RESULTS_OK;
}

static CmSemanticResultsStatus cm_results_field_selection_view(
    const unsigned char *type_bytes, size_t type_bytes_len,
    const CmSemanticExpressionRecord *record, CmHirExprId expression,
    CmSemanticFieldSelectionView *out_view)
{
    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->aggregate_definition = cm_hir_def_id_none();
    if (type_bytes == NULL || record == NULL || !record->present
        || !record->has_field_selection) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    if (record->field_base_type.type_offset > type_bytes_len
        || record->field_base_type.type_size > type_bytes_len
            - record->field_base_type.type_offset
        || record->field_type.type_offset > type_bytes_len
        || record->field_type.type_size > type_bytes_len
            - record->field_type.type_offset) {
        return CM_SEMANTIC_RESULTS_INVALID_HIR;
    }
    out_view->body = record->body;
    out_view->expression = expression;
    out_view->base_expression = record->field_base_expression;
    out_view->aggregate_definition = record->field_aggregate_definition;
    out_view->field_index = record->field_index;
    out_view->base_type.bytes = type_bytes
        + record->field_base_type.type_offset;
    out_view->base_type.size = record->field_base_type.type_size;
    out_view->field_type.bytes = type_bytes + record->field_type.type_offset;
    out_view->field_type.size = record->field_type.type_size;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_instance_primitive_binary(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *spec, CmHirExprId expression,
    CmSemanticPrimitiveBinaryView *out_view)
{
    const CmSemanticInstanceRecord *instance;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    status = cm_results_query_instance(results, admission, spec, &instance);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (expression == CM_HIR_EXPR_NONE
        || (size_t)expression > instance->expression_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    return cm_results_primitive_binary_view(instance->type_bytes,
        instance->type_bytes_len,
        &instance->expressions[(size_t)expression - 1u], expression,
        out_view);
}

CmSemanticResultsStatus cm_semantic_results_instance_field_selection(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *spec, CmHirExprId expression,
    CmSemanticFieldSelectionView *out_view)
{
    const CmSemanticInstanceRecord *instance;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->aggregate_definition = cm_hir_def_id_none();
    status = cm_results_query_instance(results, admission, spec, &instance);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (expression == CM_HIR_EXPR_NONE
        || (size_t)expression > instance->expression_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    return cm_results_field_selection_view(instance->type_bytes,
        instance->type_bytes_len,
        &instance->expressions[(size_t)expression - 1u], expression,
        out_view);
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

static CmSemanticResultsStatus cm_results_query_instance_call(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *caller, CmHirExprId expression,
    const CmHirInstanceSpec *expected_callee,
    const CmSemanticInstanceRecord **out_caller,
    const CmSemanticExpressionRecord **out_expression)
{
    const CmSemanticInstanceRecord *caller_record;
    const CmSemanticExpressionRecord *expression_record;
    CmHirCanonicalInstance callee_identity;
    CmSemanticResultsStatus status;
    int equal;

    if (expected_callee == NULL || out_caller == NULL
        || out_expression == NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    status = cm_results_query_instance(results, admission, caller,
        &caller_record);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (expression == CM_HIR_EXPR_NONE
        || (size_t)expression > caller_record->expression_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    expression_record = &caller_record->expressions[(size_t)expression - 1u];
    if (!expression_record->present
        || expression_record->body != caller_record->identity.body
        || !expression_record->has_direct_callable
        || expression_record->canonical_callee_index
            >= caller_record->callee_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    cm_hir_canonical_instance_init(&callee_identity);
    if (cm_hir_canonical_instance_encode(results->hir,
            results->local_crate, expected_callee, &callee_identity)
            != CM_HIR_INSTANCE_OK) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    equal = 0;
    if (cm_hir_canonical_instance_equal(&callee_identity,
            &caller_record->callees[
                expression_record->canonical_callee_index], &equal)
            != CM_HIR_INSTANCE_OK || !equal) {
        cm_hir_canonical_instance_destroy(&callee_identity);
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    cm_hir_canonical_instance_destroy(&callee_identity);
    *out_caller = caller_record;
    *out_expression = expression_record;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_instance_direct_call(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *caller, CmHirExprId expression,
    const CmHirInstanceSpec *expected_callee,
    CmSemanticDirectCallView *out_view)
{
    const CmSemanticInstanceRecord *caller_record;
    const CmSemanticExpressionRecord *expression_record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->callee = cm_hir_def_id_none();
    status = cm_results_query_instance_call(results, admission, caller,
        expression, expected_callee, &caller_record, &expression_record);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    out_view->body = caller_record->identity.body;
    out_view->expression = expression;
    out_view->callee = expression_record->direct_callable;
    out_view->parameter_count = expression_record->call_parameter_count;
    out_view->return_type.bytes = caller_record->type_bytes
        + expression_record->call_return_offset;
    out_view->return_type.size = expression_record->call_return_size;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus
cm_semantic_results_instance_direct_call_parameter(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    const CmHirInstanceSpec *caller, CmHirExprId expression,
    const CmHirInstanceSpec *expected_callee, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    const CmSemanticInstanceRecord *caller_record;
    const CmSemanticExpressionRecord *expression_record;
    const CmSemanticTypeRecord *type;
    CmSemanticResultsStatus status;
    size_t parameter_index;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    status = cm_results_query_instance_call(results, admission, caller,
        expression, expected_callee, &caller_record, &expression_record);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    if (parameter >= expression_record->call_parameter_count
        || !cm_size_add(expression_record->call_parameter_start,
            (size_t)parameter, &parameter_index)
        || parameter_index >= caller_record->call_parameter_count) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    type = &caller_record->call_parameters[parameter_index];
    out_view->bytes = caller_record->type_bytes + type->type_offset;
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
    out_view->adjusted_type.bytes = results->type_bytes
        + record->adjusted_type_offset;
    out_view->adjusted_type.size = record->adjusted_type_size;
    out_view->adjustment_count = record->adjustment_count;
    out_view->has_direct_callable = record->has_direct_callable;
    out_view->direct_callable = record->direct_callable;
    out_view->has_primitive_operator = record->has_primitive_operator;
    out_view->primitive_operator = record->primitive_operator;
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_expression_adjustment(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    CmHirBodyId body, CmHirExprId expression, uint32_t adjustment,
    CmSemanticAdjustmentView *out_view)
{
    const CmSemanticExpressionRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->selected_trait = cm_hir_def_id_none();
    out_view->selected_method = cm_hir_def_id_none();
    out_view->selected_impl = cm_hir_def_id_none();
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
    return cm_results_adjustment_view(results->type_bytes,
        results->type_bytes_len, results->adjustments,
        results->adjustment_count, record, expression, adjustment, out_view);
}

CmSemanticResultsStatus cm_semantic_results_primitive_binary(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    CmHirBodyId body, CmHirExprId expression,
    CmSemanticPrimitiveBinaryView *out_view)
{
    const CmSemanticExpressionRecord *record;
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
    if (!record->present || record->body != body) {
        return CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    return cm_results_primitive_binary_view(results->type_bytes,
        results->type_bytes_len, record, expression, out_view);
}

CmSemanticResultsStatus cm_semantic_results_field_selection(
    const CmSemanticResults *results, const CmSemanticAdmission *admission,
    CmHirBodyId body, CmHirExprId expression,
    CmSemanticFieldSelectionView *out_view)
{
    const CmSemanticExpressionRecord *record;
    CmSemanticResultsStatus status;

    if (out_view == NULL) return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->aggregate_definition = cm_hir_def_id_none();
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
    return cm_results_field_selection_view(results->type_bytes,
        results->type_bytes_len, record, expression, out_view);
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
    size_t instance_index;
    int owned_view;

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
    owned_view = results->type_bytes_len <= UINTPTR_MAX - bytes_start;
    bytes_end = owned_view ? bytes_start + results->type_bytes_len : 0u;
    owned_view = owned_view && view_start >= bytes_start
        && view_start <= bytes_end
        && view->size <= (size_t)(bytes_end - view_start);
    for (instance_index = 0u;
         !owned_view && instance_index < results->instance_count;
         ++instance_index) {
        const CmSemanticInstanceRecord *record;

        record = &results->instances[instance_index];
        bytes_start = (uintptr_t)record->type_bytes;
        owned_view = record->type_bytes_len <= UINTPTR_MAX - bytes_start;
        bytes_end = owned_view ? bytes_start + record->type_bytes_len : 0u;
        owned_view = owned_view && view_start >= bytes_start
            && view_start <= bytes_end
            && view->size <= (size_t)(bytes_end - view_start);
    }
    if (!owned_view) {
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
