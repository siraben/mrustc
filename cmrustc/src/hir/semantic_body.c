#include "cm/hir/semantic_body.h"

#include "cm/hir/body.h"

#include "cm/alloc.h"

#include <string.h>

#define CM_SEMANTIC_BODY_TYPE_DEPTH ((size_t)128u)

typedef enum CmSemanticTypeScan {
    CM_SEMANTIC_TYPE_OK = 0,
    CM_SEMANTIC_TYPE_PROJECTION,
    CM_SEMANTIC_TYPE_INFERENCE,
    CM_SEMANTIC_TYPE_UNSUPPORTED,
    CM_SEMANTIC_TYPE_OVERFLOW,
    CM_SEMANTIC_TYPE_INVALID
} CmSemanticTypeScan;

static CmSemanticBodyResult cm_semantic_body_result(
    CmSemanticBodyStatus status, CmHirBodyId body)
{
    CmSemanticBodyResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.body = body;
    result.expression = CM_HIR_EXPR_NONE;
    result.callee = cm_hir_def_id_none();
    result.predicate_index = CM_SEMANTIC_BODY_PREDICATE_NONE;
    result.solver_kind = CM_TRAIT_SOLVER_INVALID;
    result.typeck_status = CM_TYPECK_OK;
    return result;
}

static const CmHirItem *cm_semantic_body_item(const CmHirContext *hir,
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

static CmSemanticTypeScan cm_semantic_scan_type(const CmHirContext *hir,
    CmHirTypeId type_id, size_t depth);

static CmSemanticTypeScan cm_semantic_scan_merge(CmSemanticTypeScan left,
    CmSemanticTypeScan right)
{
    return right > left ? right : left;
}

static CmSemanticTypeScan cm_semantic_scan_region(
    const CmHirRegion *region)
{
    if (region == NULL) return CM_SEMANTIC_TYPE_INVALID;
    switch (region->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return CM_SEMANTIC_TYPE_OK;
    case CM_HIR_REGION_INFER:
        return CM_SEMANTIC_TYPE_INFERENCE;
    case CM_HIR_REGION_EARLY_BOUND:
    case CM_HIR_REGION_LATE_BOUND:
        return CM_SEMANTIC_TYPE_UNSUPPORTED;
    case CM_HIR_REGION_ERROR:
        return CM_SEMANTIC_TYPE_INVALID;
    }
    return CM_SEMANTIC_TYPE_INVALID;
}

static CmSemanticTypeScan cm_semantic_scan_const(const CmHirContext *hir,
    const CmHirConstArg *constant, size_t depth)
{
    CmSemanticTypeScan result;

    if (constant == NULL) return CM_SEMANTIC_TYPE_INVALID;
    result = cm_semantic_scan_type(hir, constant->type, depth + 1u);
    if (result != CM_SEMANTIC_TYPE_OK) return result;
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE: return CM_SEMANTIC_TYPE_OK;
    case CM_HIR_CONST_INFER: return CM_SEMANTIC_TYPE_INFERENCE;
    case CM_HIR_CONST_PARAMETER:
    case CM_HIR_CONST_UNEVALUATED:
        return CM_SEMANTIC_TYPE_UNSUPPORTED;
    case CM_HIR_CONST_ERROR: return CM_SEMANTIC_TYPE_INVALID;
    }
    return CM_SEMANTIC_TYPE_INVALID;
}

static CmSemanticTypeScan cm_semantic_scan_named(const CmHirContext *hir,
    const CmHirNamedType *named, size_t depth)
{
    CmSemanticTypeScan result;
    uint32_t index;

    if (named == NULL || cm_hir_def_id_is_none(named->definition)
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return CM_SEMANTIC_TYPE_INVALID;
    }
    result = CM_SEMANTIC_TYPE_OK;
    for (index = 0u; index < named->argument_count; ++index) {
        CmSemanticTypeScan child;

        if (named->arguments[index].kind == CM_HIR_GENERIC_ARG_TYPE) {
            child = cm_semantic_scan_type(hir,
                named->arguments[index].data.type, depth + 1u);
        } else if (named->arguments[index].kind
                == CM_HIR_GENERIC_ARG_LIFETIME) {
            child = cm_semantic_scan_region(
                &named->arguments[index].data.lifetime);
        } else if (named->arguments[index].kind
                == CM_HIR_GENERIC_ARG_CONST) {
            child = cm_semantic_scan_const(hir,
                &named->arguments[index].data.constant, depth + 1u);
        } else {
            child = CM_SEMANTIC_TYPE_INVALID;
        }
        result = cm_semantic_scan_merge(result, child);
    }
    return result;
}

static CmSemanticTypeScan cm_semantic_scan_type(const CmHirContext *hir,
    CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;
    CmSemanticTypeScan result;
    uint32_t index;

    if (depth >= CM_SEMANTIC_BODY_TYPE_DEPTH) {
        return CM_SEMANTIC_TYPE_OVERFLOW;
    }
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return CM_SEMANTIC_TYPE_INVALID;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        return CM_SEMANTIC_TYPE_OK;
    case CM_HIR_TYPE_INFER_KIND:
        return CM_SEMANTIC_TYPE_INFERENCE;
    case CM_HIR_TYPE_PROJECTION_KIND:
        return CM_SEMANTIC_TYPE_PROJECTION;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_semantic_scan_merge(
            cm_semantic_scan_region(&type->data.reference_type.region),
            cm_semantic_scan_type(hir, type->data.reference_type.pointee,
                depth + 1u));
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_semantic_scan_type(hir,
            type->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) {
            return CM_SEMANTIC_TYPE_INVALID;
        }
        result = CM_SEMANTIC_TYPE_OK;
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            result = cm_semantic_scan_merge(result,
                cm_semantic_scan_type(hir,
                    type->data.tuple_type.elements[index], depth + 1u));
        }
        return result;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_semantic_scan_merge(
            cm_semantic_scan_type(hir, type->data.array_type.element,
                depth + 1u),
            cm_semantic_scan_const(hir, &type->data.array_type.length,
                depth + 1u));
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_semantic_scan_type(hir, type->data.slice_type.element,
            depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)) {
            return CM_SEMANTIC_TYPE_INVALID;
        }
        result = cm_semantic_scan_type(hir,
            type->data.fn_pointer_type.return_type, depth + 1u);
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            result = cm_semantic_scan_merge(result,
                cm_semantic_scan_type(hir,
                    type->data.fn_pointer_type.parameters[index],
                    depth + 1u));
        }
        return result;
    case CM_HIR_TYPE_ADT_KIND:
        return cm_semantic_scan_named(hir, &type->data.named_type,
            depth + 1u);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return cm_hir_get_generic_param(hir,
                type->data.parameter_type.parameter) == NULL
            ? CM_SEMANTIC_TYPE_INVALID : CM_SEMANTIC_TYPE_OK;
    case CM_HIR_TYPE_SELF_KIND:
        return CM_SEMANTIC_TYPE_UNSUPPORTED;
    case CM_HIR_TYPE_ERROR_KIND:
        return CM_SEMANTIC_TYPE_INVALID;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return CM_SEMANTIC_TYPE_UNSUPPORTED;
    }
    return CM_SEMANTIC_TYPE_INVALID;
}

static CmSemanticBodyStatus cm_semantic_scan_status(CmSemanticTypeScan scan)
{
    switch (scan) {
    case CM_SEMANTIC_TYPE_OK: return CM_SEMANTIC_BODY_OK;
    case CM_SEMANTIC_TYPE_PROJECTION:
        return CM_SEMANTIC_BODY_PENDING_PROJECTION;
    case CM_SEMANTIC_TYPE_INFERENCE:
        return CM_SEMANTIC_BODY_DEFERRED_INFERENCE;
    case CM_SEMANTIC_TYPE_UNSUPPORTED:
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    case CM_SEMANTIC_TYPE_OVERFLOW: return CM_SEMANTIC_BODY_OVERFLOW;
    case CM_SEMANTIC_TYPE_INVALID: return CM_SEMANTIC_BODY_INVALID;
    }
    return CM_SEMANTIC_BODY_INVALID;
}

static CmSemanticBodyStatus cm_semantic_typeck_status(
    CmTypeckStatus status)
{
    switch (status) {
    case CM_TYPECK_OK: return CM_SEMANTIC_BODY_OK;
    case CM_TYPECK_OVERFLOW: return CM_SEMANTIC_BODY_OVERFLOW;
    case CM_TYPECK_UNRESOLVED: return CM_SEMANTIC_BODY_DEFERRED_INFERENCE;
    case CM_TYPECK_UNSUPPORTED_HIR_TYPE:
    case CM_TYPECK_UNSUPPORTED_CONSTANT:
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    case CM_TYPECK_INVALID_ARGUMENT:
    case CM_TYPECK_INVALID_ID:
        return CM_SEMANTIC_BODY_INVALID;
    case CM_TYPECK_INVALID_SNAPSHOT:
    case CM_TYPECK_KIND_CONFLICT:
    case CM_TYPECK_TYPE_MISMATCH:
    case CM_TYPECK_OCCURS_CHECK:
    case CM_TYPECK_HIR_FAILURE:
        return CM_SEMANTIC_BODY_TYPECK_FAILURE;
    }
    return CM_SEMANTIC_BODY_TYPECK_FAILURE;
}

static CmSemanticBodyStatus cm_semantic_solver_status(
    CmTraitSolverResultKind kind)
{
    switch (kind) {
    case CM_TRAIT_SOLVER_PROVEN: return CM_SEMANTIC_BODY_OK;
    case CM_TRAIT_SOLVER_NEGATIVE: return CM_SEMANTIC_BODY_NEGATIVE;
    case CM_TRAIT_SOLVER_NO_SOLUTION: return CM_SEMANTIC_BODY_NO_SOLUTION;
    case CM_TRAIT_SOLVER_AMBIGUOUS: return CM_SEMANTIC_BODY_AMBIGUOUS;
    case CM_TRAIT_SOLVER_DEFERRED_INFERENCE:
        return CM_SEMANTIC_BODY_DEFERRED_INFERENCE;
    case CM_TRAIT_SOLVER_DEFERRED_METADATA:
        return CM_SEMANTIC_BODY_DEFERRED_METADATA;
    case CM_TRAIT_SOLVER_UNSUPPORTED: return CM_SEMANTIC_BODY_UNSUPPORTED;
    case CM_TRAIT_SOLVER_OVERFLOW: return CM_SEMANTIC_BODY_OVERFLOW;
    case CM_TRAIT_SOLVER_INVALID: return CM_SEMANTIC_BODY_INVALID;
    case CM_TRAIT_SOLVER_TYPECK_FAILURE:
        return CM_SEMANTIC_BODY_TYPECK_FAILURE;
    }
    return CM_SEMANTIC_BODY_INVALID;
}

static int cm_semantic_type_only_owner(const CmHirContext *hir,
    const CmHirItem *item, uint32_t count)
{
    uint32_t index;

    if (item == NULL || item->generic_parameter_count != count
        || (count == 0u) != (item->generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE)) return 0;
    if (count == 0u) return item->generic_parameter_start
        == CM_HIR_GENERIC_PARAM_NONE;
    if (count - 1u > UINT32_MAX - item->generic_parameter_start) return 0;
    for (index = 0u; index < count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(hir,
            item->generic_parameter_start + index);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                item->definition)) return 0;
    }
    return 1;
}

static CmSemanticBodyResult cm_semantic_body_fail_snapshot_impl(
    CmSemanticBodyResult result, CmTypeckContext *typeck,
    CmTypeckSnapshot *snapshot, CmHirExprId *call_expressions,
    CmTypeckGenericArg *owner_arguments,
    CmTypeckGenericArg *callee_arguments)
{
    CmTypeckStatus rollback;

    rollback = cm_typeck_rollback(typeck, snapshot);
    if (rollback != CM_TYPECK_OK) {
        result.status = CM_SEMANTIC_BODY_TYPECK_FAILURE;
        result.typeck_status = rollback;
    }
    cm_free(call_expressions);
    cm_free(owner_arguments);
    cm_free(callee_arguments);
    return result;
}

#define cm_semantic_body_fail_snapshot(result, typeck, snapshot, ...) \
    cm_semantic_body_fail_snapshot_impl((result), (typeck), (snapshot), \
        call_expressions, owner_arguments, callee_arguments)

static CmSemanticBodyStatus cm_semantic_body_allocate_arguments(
    uint32_t count, CmTypeckGenericArg **out_arguments)
{
    size_t bytes;

    if (out_arguments == NULL) return CM_SEMANTIC_BODY_INVALID;
    *out_arguments = NULL;
    if (count == 0u) return CM_SEMANTIC_BODY_OK;
    if (!cm_size_mul((size_t)count, sizeof(**out_arguments), &bytes)) {
        return CM_SEMANTIC_BODY_OVERFLOW;
    }
    *out_arguments = (CmTypeckGenericArg *)cm_alloc_zeroed(1u, bytes);
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_check_call_signature(
    CmTypeckContext *typeck, const CmHirContext *hir,
    const CmHirExpr *expression, const CmHirItem *callee,
    const CmTypeckInstantiation *owner_instantiation,
    const CmTypeckInstantiation *callee_instantiation,
    CmTypeckStatus *out_typeck_status)
{
    const CmHirFunctionSignature *signature;
    CmSemanticTypeScan scan;
    CmTypeckTypeId actual_type;
    CmTypeckTypeId declared_type;
    CmTypeckStatus status;
    uint32_t index;

    if (out_typeck_status == NULL) return CM_SEMANTIC_BODY_INVALID;
    *out_typeck_status = CM_TYPECK_OK;
    signature = callee == NULL ? NULL : &callee->data.function_item.signature;
    if (expression == NULL || signature == NULL
        || signature->parameter_count != expression->data.call.argument_count
        || (signature->parameter_count == 0u)
            != (signature->parameters == NULL)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    scan = cm_semantic_scan_merge(
        cm_semantic_scan_type(hir, expression->type, 0u),
        cm_semantic_scan_type(hir, signature->return_type, 0u));
    if (scan != CM_SEMANTIC_TYPE_OK) return cm_semantic_scan_status(scan);
    status = cm_typeck_instantiate_hir_type(typeck, expression->type,
        owner_instantiation, &actual_type);
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_instantiate_hir_type(typeck,
            signature->return_type, callee_instantiation, &declared_type);
    }
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_unify(typeck, actual_type, declared_type);
    }
    if (status != CM_TYPECK_OK) {
        *out_typeck_status = status;
        return cm_semantic_typeck_status(status);
    }
    for (index = 0u; index < signature->parameter_count; ++index) {
        const CmHirExpr *argument;

        argument = cm_hir_get_expr(hir,
            expression->data.call.arguments[index]);
        if (argument == NULL
            || argument->owner_body != expression->owner_body) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        scan = cm_semantic_scan_merge(
            cm_semantic_scan_type(hir, argument->type, 0u),
            cm_semantic_scan_type(hir,
                signature->parameters[index].type, 0u));
        if (scan != CM_SEMANTIC_TYPE_OK) {
            return cm_semantic_scan_status(scan);
        }
        status = cm_typeck_instantiate_hir_type(typeck, argument->type,
            owner_instantiation, &actual_type);
        if (status == CM_TYPECK_OK) {
            status = cm_typeck_instantiate_hir_type(typeck,
                signature->parameters[index].type,
                callee_instantiation, &declared_type);
        }
        if (status == CM_TYPECK_OK) {
            status = cm_typeck_unify(typeck, actual_type, declared_type);
        }
        if (status != CM_TYPECK_OK) {
            *out_typeck_status = status;
            return cm_semantic_typeck_status(status);
        }
    }
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_walk(
    const CmHirContext *hir, CmHirBodyId body, CmHirExprId id,
    unsigned char *seen, CmHirExprId *calls, size_t *count)
{
    const CmHirExpr *e;
    uint32_t i;
    if (id == CM_HIR_EXPR_NONE || (size_t)id > hir->expressions.len)
        return CM_SEMANTIC_BODY_INVALID;
    e = cm_hir_get_expr(hir, id);
    if (e == NULL || e->owner_body != body || cm_hir_get_type(hir, e->type) == NULL)
        return CM_SEMANTIC_BODY_INVALID;
    if (seen[(size_t)id - 1u] == 1u) return CM_SEMANTIC_BODY_INVALID;
    if (seen[(size_t)id - 1u] == 2u) return CM_SEMANTIC_BODY_OK;
    seen[(size_t)id - 1u] = 1u;
    switch (e->kind) {
    case CM_HIR_EXPR_INTEGER: case CM_HIR_EXPR_LOCAL: break;
    case CM_HIR_EXPR_BLOCK:
        if (e->data.block.tail_expression == CM_HIR_EXPR_NONE
            || (e->data.block.statement_count != 0u
                && e->data.block.statements == NULL)) return CM_SEMANTIC_BODY_INVALID;
        for (i = 0u; i < e->data.block.statement_count; ++i) {
            if (e->data.block.statements[i].kind != CM_HIR_STATEMENT_LET
                || cm_semantic_body_walk(hir, body,
                    e->data.block.statements[i].data.let_statement.initializer,
                    seen, calls, count) != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        }
        if (cm_semantic_body_walk(hir, body, e->data.block.tail_expression,
                seen, calls, count) != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_CALL:
        if (e->data.call.argument_count != 0u && e->data.call.arguments == NULL)
            return CM_SEMANTIC_BODY_INVALID;
        for (i = 0u; i < e->data.call.argument_count; ++i)
            if (cm_semantic_body_walk(hir, body, e->data.call.arguments[i],
                    seen, calls, count) != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        if (*count >= hir->expressions.len) return CM_SEMANTIC_BODY_OVERFLOW;
        calls[(*count)++] = id;
        break;
    case CM_HIR_EXPR_BINARY:
        if (cm_semantic_body_walk(hir, body, e->data.binary.left, seen, calls, count)
                != CM_SEMANTIC_BODY_OK
            || cm_semantic_body_walk(hir, body, e->data.binary.right, seen, calls, count)
                != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_AGGREGATE:
        if (e->data.aggregate.field_count != 0u && e->data.aggregate.fields == NULL)
            return CM_SEMANTIC_BODY_INVALID;
        for (i = 0u; i < e->data.aggregate.field_count; ++i)
            if (cm_semantic_body_walk(hir, body, e->data.aggregate.fields[i].value,
                    seen, calls, count) != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_FIELD:
        if (cm_semantic_body_walk(hir, body, e->data.field.base, seen, calls, count)
                != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_IF:
        if (cm_semantic_body_walk(hir, body, e->data.if_expr.condition, seen, calls, count)
                != CM_SEMANTIC_BODY_OK
            || cm_semantic_body_walk(hir, body, e->data.if_expr.then_expression, seen, calls, count)
                != CM_SEMANTIC_BODY_OK
            || cm_semantic_body_walk(hir, body, e->data.if_expr.else_expression, seen, calls, count)
                != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    default: return CM_SEMANTIC_BODY_INVALID;
    }
    seen[(size_t)id - 1u] = 2u;
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_collect_calls(
    const CmHirContext *hir, CmHirBodyId body, CmHirExprId root,
    CmHirExprId **out_calls, size_t *out_count)
{
    unsigned char *seen;
    CmHirExprId *calls;
    CmSemanticBodyStatus status;
    if (hir == NULL || out_calls == NULL || out_count == NULL
        || root == CM_HIR_EXPR_NONE || hir->expressions.len == 0u)
        return CM_SEMANTIC_BODY_INVALID;
    seen = (unsigned char *)cm_alloc_zeroed(hir->expressions.len, sizeof(unsigned char));
    calls = (CmHirExprId *)cm_alloc_zeroed(hir->expressions.len, sizeof(CmHirExprId));
    *out_count = 0u;
    status = cm_semantic_body_walk(hir, body, root, seen, calls, out_count);
    cm_free(seen);
    if (status != CM_SEMANTIC_BODY_OK) { cm_free(calls); return status; }
    *out_calls = calls;
    return status;
}

static CmSemanticBodyResult cm_semantic_body_check_calls_mode(
    CmSemanticSession *session, CmHirBodyId body_id,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count, int definition_mode)
{
    CmSemanticBodyResult result;
    const CmHirContext *hir;
    const CmHirBody *body;
    const CmHirItem *owner_item;
    const CmHirItem *enclosing_item;
    CmHirDefId owner;
    CmHirBodyFunctionOwnerKind owner_kind;
    CmTypeckContext *typeck;
    CmTypeckSnapshot snapshot;
    CmTypeckGenericArg *owner_arguments;
    CmTypeckGenericArg *callee_arguments;
    CmTypeckInstantiation owner_instantiation;
    CmTypeckInstantiation enclosing_instantiation;
    CmParamEnvSubstitution environment_substitution;
    CmHirExprId *call_expressions;
    size_t call_expression_count;
    size_t call_index;
    CmTypeckStatus typeck_status;
    uint32_t owner_argument_index;

    (void)definition_mode;

    result = cm_semantic_body_result(CM_SEMANTIC_BODY_INVALID, body_id);
    if (session == NULL || !cm_semantic_session_is_current(session)) {
        result.status = session != NULL && session->state != NULL
            ? CM_SEMANTIC_BODY_STALE : CM_SEMANTIC_BODY_INVALID;
        return result;
    }
    hir = cm_semantic_session_hir(session);
    body = cm_hir_get_body(hir, body_id);
    owner = cm_semantic_session_exact_owner(session);
    owner_item = body == NULL ? NULL
        : cm_semantic_body_item(hir, body->owner);
    owner_kind = cm_hir_body_function_owner_kind(hir, owner_item);
    enclosing_item = owner_kind
            == CM_HIR_BODY_FUNCTION_OWNER_CONCRETE_TRAIT_IMPL_METHOD
        ? cm_semantic_body_item(hir, owner_item->parent_definition) : NULL;
    if (hir == NULL || body == NULL || owner_item == NULL
        || owner_item->kind != CM_HIR_ITEM_FUNCTION
        || owner_kind == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED
        || body->state != CM_HIR_BODY_TYPED
        || body->root_expression == CM_HIR_EXPR_NONE
        || owner_item->data.function_item.body != body_id
        || !cm_hir_def_id_equal(body->owner, owner)
        || (owner_kind == CM_HIR_BODY_FUNCTION_OWNER_FREE
            ? !cm_hir_def_id_is_none(
                cm_semantic_session_enclosing_owner(session))
            : enclosing_item == NULL
                || enclosing_item->kind != CM_HIR_ITEM_IMPL
                || !cm_hir_def_id_equal(owner_item->parent_definition,
                    cm_semantic_session_enclosing_owner(session)))
        || (owner_type_substitution_count == 0u)
            != (owner_type_substitutions == NULL)
        || (definition_mode
            ? (owner_type_substitution_count != 0u
                || !cm_semantic_type_only_owner(hir, owner_item,
                    owner_item->generic_parameter_count))
            : !cm_semantic_type_only_owner(hir, owner_item,
                owner_type_substitution_count))) {
        return result;
    }
    typeck = cm_semantic_session_typeck(session);
    if (typeck == NULL) {
        result.status = CM_SEMANTIC_BODY_STALE;
        return result;
    }
    owner_arguments = NULL;
    callee_arguments = NULL;
    result.status = cm_semantic_body_collect_calls(hir, body_id,
        body->root_expression, &call_expressions, &call_expression_count);
    if (result.status != CM_SEMANTIC_BODY_OK) return result;
    memset(&snapshot, 0, sizeof(snapshot));
    typeck_status = cm_typeck_snapshot(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        result.status = cm_semantic_typeck_status(typeck_status);
        result.typeck_status = typeck_status;
        cm_free(call_expressions);
        return result;
    }

    memset(&owner_instantiation, 0, sizeof(owner_instantiation));
    memset(&enclosing_instantiation, 0, sizeof(enclosing_instantiation));
    owner_instantiation.parameter_owner = owner;
    if (owner_kind
            == CM_HIR_BODY_FUNCTION_OWNER_CONCRETE_TRAIT_IMPL_METHOD) {
        CmSemanticTypeScan scan;

        scan = cm_semantic_scan_merge(
            cm_semantic_scan_type(hir,
                enclosing_item->data.impl_item.self_type, 0u),
            cm_semantic_scan_named(hir,
                &enclosing_item->data.impl_item.trait_type, 0u));
        result.status = cm_semantic_scan_status(scan);
        if (result.status != CM_SEMANTIC_BODY_OK) {
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        typeck_status = cm_typeck_import_hir_type(typeck,
            enclosing_item->data.impl_item.self_type,
            &owner_instantiation.self_type);
        if (typeck_status != CM_TYPECK_OK) {
            result.status = cm_semantic_typeck_status(typeck_status);
            result.typeck_status = typeck_status;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        owner_instantiation.self_owner = enclosing_item->definition;
        enclosing_instantiation.parameter_owner =
            enclosing_item->definition;
        enclosing_instantiation.self_owner = enclosing_item->definition;
        enclosing_instantiation.self_type = owner_instantiation.self_type;
    }
    result.status = cm_semantic_body_allocate_arguments(
        owner_item->generic_parameter_count, &owner_arguments);
    if (result.status != CM_SEMANTIC_BODY_OK) {
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    if (owner_item->generic_parameter_count != 0u) {
        for (owner_argument_index = 0u;
             owner_argument_index < owner_item->generic_parameter_count;
             ++owner_argument_index) {
            const CmHirGenericParam *parameter;

            parameter = cm_hir_get_generic_param(hir,
                owner_item->generic_parameter_start + owner_argument_index);
            if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
                || parameter->index != owner_argument_index
                || !cm_hir_def_id_equal(parameter->owner, owner)) {
                result.status = CM_SEMANTIC_BODY_INVALID;
                return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
                    call_expressions);
            }
            owner_arguments[owner_argument_index].kind =
                CM_HIR_GENERIC_ARG_TYPE;
            if (!definition_mode) {
                CmSemanticTypeScan scan;

                scan = cm_semantic_scan_type(hir,
                    owner_type_substitutions[owner_argument_index], 0u);
                result.status = cm_semantic_scan_status(scan);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                typeck_status = cm_typeck_import_hir_type(typeck,
                    owner_type_substitutions[owner_argument_index],
                    &owner_arguments[owner_argument_index].data.type);
            } else {
                CmTypeckType rigid_type;

                memset(&rigid_type, 0, sizeof(rigid_type));
                rigid_type.kind = CM_TYPECK_TYPE_PARAMETER;
                rigid_type.span = parameter->span;
                rigid_type.data.parameter_type.parameter =
                    owner_item->generic_parameter_start
                        + owner_argument_index;
                typeck_status = cm_typeck_add_type(typeck, &rigid_type,
                    &owner_arguments[owner_argument_index].data.type);
            }
            if (typeck_status != CM_TYPECK_OK) {
                result.status = cm_semantic_typeck_status(typeck_status);
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
        }
        owner_instantiation.arguments = owner_arguments;
        owner_instantiation.argument_count =
            owner_item->generic_parameter_count;
    }
    if (!cm_typeck_instantiation_is_valid(typeck, &owner_instantiation)) {
        result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    if (owner_kind
            == CM_HIR_BODY_FUNCTION_OWNER_CONCRETE_TRAIT_IMPL_METHOD
        && !cm_typeck_instantiation_is_valid(typeck,
            &enclosing_instantiation)) {
        result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    memset(&environment_substitution, 0,
        sizeof(environment_substitution));
    environment_substitution.exact = &owner_instantiation;
    if (owner_kind
            == CM_HIR_BODY_FUNCTION_OWNER_CONCRETE_TRAIT_IMPL_METHOD) {
        environment_substitution.enclosing = &enclosing_instantiation;
    }

    for (call_index = 0u; call_index < call_expression_count; ++call_index) {
        const CmHirExpr *expression;
        CmHirExprId expression_id;
        const CmHirItem *callee;
        CmTypeckInstantiation callee_instantiation;
        uint32_t predicate_index;
        uint32_t callee_argument_index;

        expression_id = call_expressions[call_index];
        expression = cm_hir_get_expr(hir, expression_id);
        if (expression == NULL || expression->owner_body != body_id
            || expression->kind != CM_HIR_EXPR_CALL) {
            result.status = CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
                call_expressions);
        }
        result.expression = expression_id;
        result.callee = expression->data.call.callee;
        callee = cm_semantic_body_item(hir, expression->data.call.callee);
        if (callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION) {
            result.status = CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
                call_expressions);
        }
        if (!cm_hir_def_id_is_none(callee->parent_definition)
            || (expression->data.call.type_substitution_count != 0u
                && expression->data.call.type_substitutions == NULL)
            || (expression->data.call.argument_count != 0u
                && expression->data.call.arguments == NULL)
            || !cm_semantic_type_only_owner(hir, callee,
                expression->data.call.type_substitution_count)) {
            result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if ((callee->predicate_scope_count == 0u)
                != (callee->predicate_scopes == NULL)
            || (callee->predicate_count == 0u)
                != (callee->predicates == NULL)
            || (callee->outlives_predicate_count == 0u)
                != (callee->outlives_predicates == NULL)) {
            result.status = CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if (callee->predicate_scope_count != 0u) {
            result.status = CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if (callee->outlives_predicate_count != 0u) {
            result.status = CM_SEMANTIC_BODY_PENDING_OUTLIVES;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        cm_free(callee_arguments);
        callee_arguments = NULL;
        memset(&callee_instantiation, 0, sizeof(callee_instantiation));
        callee_instantiation.parameter_owner = callee->definition;
        result.status = cm_semantic_body_allocate_arguments(
            callee->generic_parameter_count, &callee_arguments);
        if (result.status != CM_SEMANTIC_BODY_OK) {
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if (expression->data.call.type_substitution_count != 0u) {
            for (callee_argument_index = 0u;
                 callee_argument_index
                    < expression->data.call.type_substitution_count;
                 ++callee_argument_index) {
                CmSemanticTypeScan scan;

                scan = cm_semantic_scan_type(hir,
                    expression->data.call.type_substitutions[
                        callee_argument_index], 0u);
                result.status = cm_semantic_scan_status(scan);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                callee_arguments[callee_argument_index].kind =
                    CM_HIR_GENERIC_ARG_TYPE;
                typeck_status = cm_typeck_instantiate_hir_type(typeck,
                    expression->data.call.type_substitutions[
                        callee_argument_index], &owner_instantiation,
                    &callee_arguments[callee_argument_index].data.type);
                if (typeck_status != CM_TYPECK_OK) {
                    result.status = cm_semantic_typeck_status(typeck_status);
                    result.typeck_status = typeck_status;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
            }
            callee_instantiation.arguments = callee_arguments;
            callee_instantiation.argument_count =
                expression->data.call.type_substitution_count;
        }
        if (!cm_typeck_instantiation_is_valid(typeck,
                &callee_instantiation)) {
            result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        result.status = cm_semantic_body_check_call_signature(typeck, hir,
            expression, callee, &owner_instantiation,
            &callee_instantiation, &typeck_status);
        if (result.status != CM_SEMANTIC_BODY_OK) {
            result.typeck_status = typeck_status;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }

        for (predicate_index = 0u;
             predicate_index < callee->predicate_count;
             ++predicate_index) {
            const CmHirTraitPredicate *predicate;
            CmSemanticTypeScan scan;
            CmTraitGoal goal;
            CmTraitSelectionResult selection;
            CmTypeckTypeId implemented_self;
            CmTypeckNamedType implemented_trait;
            uint32_t equality_index;

            predicate = &callee->predicates[predicate_index];
            result.predicate_index = predicate_index;
            if ((predicate->binder.lifetime_count == 0u)
                    != (predicate->binder.lifetimes == NULL)) {
                result.status = CM_SEMANTIC_BODY_INVALID;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->binder.lifetime_count != 0u) {
                result.status = CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            if (predicate->modifier != CM_HIR_PREDICATE_REQUIRED) {
                result.status = CM_SEMANTIC_BODY_PENDING_MODIFIER;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            if ((predicate->equality_count == 0u)
                    != (predicate->equalities == NULL)) {
                result.status = CM_SEMANTIC_BODY_INVALID;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            scan = cm_semantic_scan_merge(
                cm_semantic_scan_type(hir, predicate->subject, 0u),
                cm_semantic_scan_named(hir, &predicate->trait_type, 0u));
            result.status = cm_semantic_scan_status(scan);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            memset(&goal, 0, sizeof(goal));
            goal.kind = CM_TRAIT_GOAL_IMPLEMENTED;
            goal.data.implemented.owner = owner;
            typeck_status = cm_typeck_instantiate_hir_type(typeck,
                predicate->subject, &callee_instantiation,
                &goal.data.implemented.self_type);
            if (typeck_status == CM_TYPECK_OK) {
                typeck_status = cm_typeck_instantiate_hir_named(typeck,
                    &predicate->trait_type, &callee_instantiation,
                    &goal.data.implemented.trait_type);
            }
            if (typeck_status != CM_TYPECK_OK) {
                result.status = cm_semantic_typeck_status(typeck_status);
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            selection = cm_semantic_session_solve_goal(session,
                typeck, &environment_substitution, &goal);
            result.solver_kind = selection.kind;
            result.typeck_status = selection.typeck_status;
            result.status = cm_semantic_solver_status(selection.kind);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            implemented_self = goal.data.implemented.self_type;
            implemented_trait = goal.data.implemented.trait_type;
            for (equality_index = 0u;
                 equality_index < predicate->equality_count;
                 ++equality_index) {
                const CmHirAssociatedTypeEquality *equality;
                CmTypeckType projection;
                CmTypeckTypeId projection_type;
                CmTypeckTypeId expected_type;

                equality = &predicate->equalities[equality_index];
                scan = cm_semantic_scan_type(hir, equality->value, 0u);
                if (scan == CM_SEMANTIC_TYPE_PROJECTION) {
                    result.status = CM_SEMANTIC_BODY_UNSUPPORTED;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                result.status = cm_semantic_scan_status(scan);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                typeck_status = cm_typeck_instantiate_hir_type(typeck,
                    equality->value, &callee_instantiation,
                    &expected_type);
                if (typeck_status != CM_TYPECK_OK) {
                    result.status = cm_semantic_typeck_status(typeck_status);
                    result.typeck_status = typeck_status;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                memset(&projection, 0, sizeof(projection));
                projection.kind = CM_TYPECK_TYPE_PROJECTION;
                projection.span = equality->span;
                projection.data.projection_type.self_type =
                    implemented_self;
                projection.data.projection_type.trait_type =
                    implemented_trait;
                projection.data.projection_type.associated_type.definition =
                    equality->associated_type;
                typeck_status = cm_typeck_add_type(typeck, &projection,
                    &projection_type);
                if (typeck_status != CM_TYPECK_OK) {
                    result.status = cm_semantic_typeck_status(typeck_status);
                    result.typeck_status = typeck_status;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                memset(&goal, 0, sizeof(goal));
                goal.kind = CM_TRAIT_GOAL_PROJECTION_EQUALITY;
                goal.data.projection_equality.owner = owner;
                goal.data.projection_equality.projection_type =
                    projection_type;
                goal.data.projection_equality.expected_type = expected_type;
                selection = cm_semantic_session_solve_goal(session, typeck,
                    &environment_substitution, &goal);
                result.solver_kind = selection.kind;
                result.typeck_status = selection.typeck_status;
                result.status = cm_semantic_solver_status(selection.kind);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                if (cm_hir_def_id_is_none(
                        selection.impl_associated_definition)) {
                    result.status = CM_SEMANTIC_BODY_INVALID;
                    result.solver_kind = CM_TRAIT_SOLVER_INVALID;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
            }
        }
    }
    typeck_status = cm_typeck_commit(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        result.status = CM_SEMANTIC_BODY_TYPECK_FAILURE;
        result.typeck_status = typeck_status;
        (void)cm_typeck_rollback(typeck, &snapshot);
        cm_free(call_expressions);
        cm_free(owner_arguments);
        cm_free(callee_arguments);
        return result;
    }
    cm_free(call_expressions);
    cm_free(owner_arguments);
    cm_free(callee_arguments);
    result = cm_semantic_body_result(CM_SEMANTIC_BODY_OK, body_id);
    result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    return result;
}

CmSemanticBodyResult cm_semantic_body_check_calls(
    CmSemanticSession *session, CmHirBodyId body,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count)
{
    return cm_semantic_body_check_calls_mode(session, body,
        owner_type_substitutions, owner_type_substitution_count, 0);
}

CmSemanticBodyResult cm_semantic_body_check_definition(
    CmSemanticSession *session, CmHirBodyId body)
{
    return cm_semantic_body_check_calls_mode(session, body, NULL, 0u, 1);
}

const char *cm_semantic_body_status_name(CmSemanticBodyStatus status)
{
    switch (status) {
    case CM_SEMANTIC_BODY_OK: return "ok";
    case CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED:
        return "pending-higher-ranked";
    case CM_SEMANTIC_BODY_PENDING_OUTLIVES: return "pending-outlives";
    case CM_SEMANTIC_BODY_PENDING_PROJECTION: return "pending-projection";
    case CM_SEMANTIC_BODY_PENDING_MODIFIER: return "pending-modifier";
    case CM_SEMANTIC_BODY_PENDING_SUBSTITUTION:
        return "pending-substitution";
    case CM_SEMANTIC_BODY_DEFERRED_INFERENCE: return "deferred-inference";
    case CM_SEMANTIC_BODY_DEFERRED_METADATA: return "deferred-metadata";
    case CM_SEMANTIC_BODY_AMBIGUOUS: return "ambiguous";
    case CM_SEMANTIC_BODY_NO_SOLUTION: return "no-solution";
    case CM_SEMANTIC_BODY_NEGATIVE: return "negative";
    case CM_SEMANTIC_BODY_UNSUPPORTED: return "unsupported";
    case CM_SEMANTIC_BODY_OVERFLOW: return "overflow";
    case CM_SEMANTIC_BODY_TYPECK_FAILURE: return "typeck-failure";
    case CM_SEMANTIC_BODY_STALE: return "stale";
    case CM_SEMANTIC_BODY_INVALID: return "invalid";
    }
    return "unknown";
}
