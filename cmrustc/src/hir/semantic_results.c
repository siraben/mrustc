#include "semantic_results_internal.h"

#include "cm/alloc.h"
#include "cm/hir/admission.h"

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
} CmSemanticBodyRecord;

typedef struct CmSemanticExpressionRecord {
    int present;
    CmHirBodyId body;
    size_t type_offset;
    size_t type_size;
    int has_direct_callable;
    CmHirDefId direct_callable;
    int has_primitive_operator;
    CmHirBinaryOperator primitive_operator;
} CmSemanticExpressionRecord;

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
    size_t admitted_body_count;
};

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

static CmSemanticResultsStatus cm_results_type(CmResultsBuffer *buffer,
    const CmHirContext *hir, CmHirTypeId type_id, size_t depth);

static CmSemanticResultsStatus cm_results_region(CmResultsBuffer *buffer,
    const CmHirRegion *region)
{
    if (region == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
    if (region->kind == CM_HIR_REGION_STATIC
        || region->kind == CM_HIR_REGION_ERASED) {
        return cm_results_u8(buffer, (unsigned int)region->kind);
    }
    return CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE;
}

static CmSemanticResultsStatus cm_results_const(CmResultsBuffer *buffer,
    const CmHirContext *hir, const CmHirConstArg *constant, size_t depth)
{
    CmSemanticResultsStatus status;

    if (constant == NULL || constant->kind != CM_HIR_CONST_VALUE) {
        return CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE;
    }
    status = cm_results_u8(buffer, (unsigned int)constant->kind);
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_type(buffer, hir, constant->type, depth + 1u);
    }
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_u64(buffer, constant->data.value.low_bits);
    }
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_u64(buffer, constant->data.value.high_bits);
    }
    return status;
}

static CmSemanticResultsStatus cm_results_argument(
    CmResultsBuffer *buffer, const CmHirContext *hir,
    const CmHirGenericArg *argument, size_t depth)
{
    CmSemanticResultsStatus status;

    if (argument == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
    status = cm_results_u8(buffer, (unsigned int)argument->kind);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_results_region(buffer, &argument->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_results_type(buffer, hir, argument->data.type, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_results_const(buffer, hir, &argument->data.constant,
            depth + 1u);
    }
    return CM_SEMANTIC_RESULTS_INVALID_HIR;
}

static CmSemanticResultsStatus cm_results_named(CmResultsBuffer *buffer,
    const CmHirContext *hir, const CmHirNamedType *named, size_t depth)
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
        status = cm_results_argument(buffer, hir, &named->arguments[index],
            depth + 1u);
    }
    return status;
}

static CmSemanticResultsStatus cm_results_type(CmResultsBuffer *buffer,
    const CmHirContext *hir, CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;
    const CmHirGenericParam *parameter;
    CmSemanticResultsStatus status;
    uint32_t index;

    if (depth >= CM_RESULTS_TYPE_DEPTH) return CM_SEMANTIC_RESULTS_OVERFLOW;
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return CM_SEMANTIC_RESULTS_INVALID_HIR;
    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE;
    default:
        break;
    }
    status = cm_results_u8(buffer, (unsigned int)type->kind);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return CM_SEMANTIC_RESULTS_OK;
    case CM_HIR_TYPE_INTEGER_KIND:
        return cm_results_u8(buffer,
            (unsigned int)type->data.integer_type.kind);
    case CM_HIR_TYPE_FLOAT_KIND:
        return cm_results_u8(buffer,
            (unsigned int)type->data.float_type.kind);
    case CM_HIR_TYPE_REFERENCE_KIND:
        status = cm_results_region(buffer,
            &type->data.reference_type.region);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_u8(buffer,
                (unsigned int)type->data.reference_type.mutability);
        }
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_type(buffer, hir,
                type->data.reference_type.pointee, depth + 1u) : status;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        status = cm_results_u8(buffer,
            (unsigned int)type->data.raw_pointer_type.mutability);
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_type(buffer, hir,
                type->data.raw_pointer_type.pointee, depth + 1u) : status;
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        status = cm_results_u32(buffer,
            type->data.tuple_type.element_count);
        for (index = 0u; status == CM_SEMANTIC_RESULTS_OK
                && index < type->data.tuple_type.element_count; ++index) {
            status = cm_results_type(buffer, hir,
                type->data.tuple_type.elements[index], depth + 1u);
        }
        return status;
    case CM_HIR_TYPE_ARRAY_KIND:
        status = cm_results_type(buffer, hir,
            type->data.array_type.element, depth + 1u);
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_const(buffer, hir, &type->data.array_type.length,
                depth + 1u) : status;
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_results_type(buffer, hir,
            type->data.slice_type.element, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        status = cm_results_u32(buffer,
            type->data.fn_pointer_type.parameter_count);
        for (index = 0u; status == CM_SEMANTIC_RESULTS_OK
                && index < type->data.fn_pointer_type.parameter_count;
             ++index) {
            status = cm_results_type(buffer, hir,
                type->data.fn_pointer_type.parameters[index], depth + 1u);
        }
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_type(buffer, hir,
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
    case CM_HIR_TYPE_ADT_KIND:
        return cm_results_named(buffer, hir, &type->data.named_type,
            depth + 1u);
    case CM_HIR_TYPE_SELF_KIND:
        if (cm_hir_def_id_is_none(type->data.self_type.owner)
            || cm_hir_lookup_definition(hir,
                type->data.self_type.owner) == NULL) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        return cm_results_def(buffer, type->data.self_type.owner);
    case CM_HIR_TYPE_PARAMETER_KIND:
        parameter = cm_hir_get_generic_param(hir,
            type->data.parameter_type.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || cm_hir_def_id_is_none(parameter->owner)) {
            return CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        status = cm_results_def(buffer, parameter->owner);
        return status == CM_SEMANTIC_RESULTS_OK
            ? cm_results_u32(buffer, parameter->index) : status;
    default:
        return CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE;
    }
}

static CmSemanticResultsStatus cm_results_collect_expression(
    CmSemanticResults *results, const CmHirContext *hir,
    CmHirBodyId body_id, CmHirExprId expression_id, unsigned char *seen,
    CmResultsBuffer *types, uint32_t *body_expression_count, size_t depth)
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
                seen, types, body_expression_count, depth + 1u);
            if (status != CM_SEMANTIC_RESULTS_OK) return status;
        }
        status = cm_results_collect_expression(results, hir, body_id,
            expression->data.block.tail_expression, seen, types,
            body_expression_count, depth + 1u);
        if (status != CM_SEMANTIC_RESULTS_OK) return status;
        break;
    case CM_HIR_EXPR_CALL:
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.call.arguments[index], seen, types,
                body_expression_count, depth + 1u);
            if (status != CM_SEMANTIC_RESULTS_OK) return status;
        }
        break;
    case CM_HIR_EXPR_BINARY:
        status = cm_results_collect_expression(results, hir, body_id,
            expression->data.binary.left, seen, types,
            body_expression_count, depth + 1u);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.binary.right, seen, types,
                body_expression_count, depth + 1u);
        }
        if (status != CM_SEMANTIC_RESULTS_OK) return status;
        break;
    case CM_HIR_EXPR_AGGREGATE:
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.aggregate.fields[index].value, seen, types,
                body_expression_count, depth + 1u);
            if (status != CM_SEMANTIC_RESULTS_OK) return status;
        }
        break;
    case CM_HIR_EXPR_FIELD:
        status = cm_results_collect_expression(results, hir, body_id,
            expression->data.field.base, seen, types,
            body_expression_count, depth + 1u);
        if (status != CM_SEMANTIC_RESULTS_OK) return status;
        break;
    case CM_HIR_EXPR_IF:
        status = cm_results_collect_expression(results, hir, body_id,
            expression->data.if_expr.condition, seen, types,
            body_expression_count, depth + 1u);
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.if_expr.then_expression, seen, types,
                body_expression_count, depth + 1u);
        }
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = cm_results_collect_expression(results, hir, body_id,
                expression->data.if_expr.else_expression, seen, types,
                body_expression_count, depth + 1u);
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
    record->type_offset = types->len;
    status = cm_results_type(types, hir, expression->type, 0u);
    if (status != CM_SEMANTIC_RESULTS_OK) return status;
    record->type_size = types->len - record->type_offset;
    record->present = 1;
    record->body = body_id;
    if (expression->kind == CM_HIR_EXPR_CALL) {
        record->has_direct_callable = 1;
        record->direct_callable = expression->data.call.callee;
    } else if (expression->kind == CM_HIR_EXPR_BINARY) {
        record->has_primitive_operator = 1;
        record->primitive_operator = expression->data.binary.operator_kind;
    }
    if (*body_expression_count == UINT32_MAX) {
        return CM_SEMANTIC_RESULTS_OVERFLOW;
    }
    *body_expression_count += 1u;
    seen[(size_t)expression_id - 1u] = 2u;
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
    }
    return CM_SEMANTIC_RESULTS_OK;
}

CmSemanticResultsStatus cm_semantic_results_create(
    const CmHirContext *hir, CmHirCrateId local_crate,
    CmSemanticResults **out_results)
{
    CmSemanticResults *results;
    CmResultsBuffer sizing;
    CmResultsBuffer output;
    unsigned char *seen;
    CmSemanticResultsStatus status;
    size_t body_bytes;
    size_t expression_bytes;
    size_t body_index;
    size_t item_index;

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
    seen = hir->expressions.len == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(hir->expressions.len, 1u);
    memset(&sizing, 0, sizeof(sizing));
    sizing.sizing = 1;
    status = CM_SEMANTIC_RESULTS_OK;
    for (item_index = 0u; status == CM_SEMANTIC_RESULTS_OK
            && item_index < hir->items.len; ++item_index) {
        const CmHirItem *item;
        const CmHirBody *body;
        CmSemanticBodyRecord *record;
        CmHirBodyId body_id;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, item_index);
        if (item == NULL) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        if (item->definition.crate_id != local_crate
            || item->kind != CM_HIR_ITEM_FUNCTION
            || item->data.function_item.body == CM_HIR_BODY_NONE) continue;
        body_id = item->data.function_item.body;
        body = cm_hir_get_body(hir, body_id);
        if (body == NULL || !cm_hir_def_id_equal(body->owner,
                item->definition)) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        if (body->state != CM_HIR_BODY_TYPED
            || body->root_expression == CM_HIR_EXPR_NONE) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record = &results->bodies[(size_t)body_id - 1u];
        if (record->present) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
            break;
        }
        record->present = 1;
        results->admitted_body_count += 1u;
        record->owner = body->owner;
        status = cm_results_collect_expression(results, hir,
            body_id, body->root_expression, seen,
            &sizing, &record->expression_count, 0u);
    }
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_validate_membership(results, hir);
    }
    if (status != CM_SEMANTIC_RESULTS_OK) goto fail;
    results->type_bytes = sizing.len == 0u ? NULL
        : (unsigned char *)cm_alloc(sizing.len);
    results->type_bytes_len = sizing.len;
    if (expression_bytes != 0u) {
        memset(results->expressions, 0, expression_bytes);
    }
    if (seen != NULL) memset(seen, 0, hir->expressions.len);
    memset(&output, 0, sizeof(output));
    output.data = results->type_bytes;
    output.cap = sizing.len;
    for (body_index = 0u; status == CM_SEMANTIC_RESULTS_OK
            && body_index < hir->bodies.len; ++body_index) {
        const CmHirBody *body;
        CmSemanticBodyRecord *record;

        record = &results->bodies[body_index];
        if (!record->present) continue;
        record->expression_count = 0u;
        body = cm_hir_get_body(hir, (CmHirBodyId)(body_index + 1u));
        status = cm_results_collect_expression(results, hir,
            (CmHirBodyId)(body_index + 1u), body->root_expression, seen,
            &output, &record->expression_count, 0u);
    }
    if (status == CM_SEMANTIC_RESULTS_OK) {
        status = cm_results_validate_membership(results, hir);
    }
    if (status != CM_SEMANTIC_RESULTS_OK || output.len != sizing.len) {
        if (status == CM_SEMANTIC_RESULTS_OK) {
            status = CM_SEMANTIC_RESULTS_INVALID_HIR;
        }
        goto fail;
    }
    cm_free(seen);
    results->hir = hir;
    results->local_crate = local_crate;
    results->storage_lifetime_id = hir->storage.lifetime_id;
    results->semantic_generation = hir->semantic_generation;
    results->rewind_generation = hir->rewind_generation;
    *out_results = results;
    return CM_SEMANTIC_RESULTS_OK;

fail:
    cm_free(seen);
    cm_semantic_results_destroy(results);
    return status;
}

void cm_semantic_results_destroy(CmSemanticResults *results)
{
    if (results == NULL) return;
    cm_free(results->type_bytes);
    cm_free(results->expressions);
    cm_free(results->bodies);
    memset(results, 0, sizeof(*results));
    cm_free(results);
}

static CmSemanticResultsStatus cm_results_validate(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission)
{
    const CmHirContext *hir;

    if (results == NULL || admission == NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
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

const char *cm_semantic_results_status_name(CmSemanticResultsStatus status)
{
    switch (status) {
    case CM_SEMANTIC_RESULTS_OK: return "ok";
    case CM_SEMANTIC_RESULTS_INVALID_ARGUMENT: return "invalid-argument";
    case CM_SEMANTIC_RESULTS_STALE: return "stale";
    case CM_SEMANTIC_RESULTS_FOREIGN: return "foreign";
    case CM_SEMANTIC_RESULTS_NOT_FOUND: return "not-found";
    case CM_SEMANTIC_RESULTS_INVALID_HIR: return "invalid-hir";
    case CM_SEMANTIC_RESULTS_UNSUPPORTED_TYPE: return "unsupported-type";
    case CM_SEMANTIC_RESULTS_OVERFLOW: return "overflow";
    }
    return "unknown";
}
