#include "cm/hir/semantic_body.h"

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
    const CmHirGenericParam *parameter;

    if (item == NULL || item->generic_parameter_count != count
        || count > 1u) return 0;
    if (count == 0u) return item->generic_parameter_start
        == CM_HIR_GENERIC_PARAM_NONE;
    parameter = cm_hir_get_generic_param(hir,
        item->generic_parameter_start);
    return parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && cm_hir_def_id_equal(parameter->owner, item->definition);
}

static CmSemanticBodyResult cm_semantic_body_fail_snapshot(
    CmSemanticBodyResult result, CmTypeckContext *typeck,
    CmTypeckSnapshot *snapshot)
{
    CmTypeckStatus rollback;

    rollback = cm_typeck_rollback(typeck, snapshot);
    if (rollback != CM_TYPECK_OK) {
        result.status = CM_SEMANTIC_BODY_TYPECK_FAILURE;
        result.typeck_status = rollback;
    }
    return result;
}

CmSemanticBodyResult cm_semantic_body_check_calls(
    CmSemanticSession *session, CmHirBodyId body_id,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count)
{
    CmSemanticBodyResult result;
    const CmHirContext *hir;
    const CmHirBody *body;
    const CmHirItem *owner_item;
    CmHirDefId owner;
    CmTypeckContext *typeck;
    CmTypeckSnapshot snapshot;
    CmTypeckGenericArg owner_arguments[1];
    CmTypeckInstantiation owner_instantiation;
    CmParamEnvSubstitution environment_substitution;
    size_t expression_index;
    CmTypeckStatus typeck_status;

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
    if (hir == NULL || body == NULL || owner_item == NULL
        || owner_item->kind != CM_HIR_ITEM_FUNCTION
        || body->state != CM_HIR_BODY_TYPED
        || body->root_expression == CM_HIR_EXPR_NONE
        || owner_item->data.function_item.body != body_id
        || !cm_hir_def_id_equal(body->owner, owner)
        || !cm_hir_def_id_is_none(owner_item->parent_definition)
        || !cm_hir_def_id_is_none(
            cm_semantic_session_enclosing_owner(session))
        || (owner_type_substitution_count == 0u)
            != (owner_type_substitutions == NULL)
        || !cm_semantic_type_only_owner(hir, owner_item,
            owner_type_substitution_count)) {
        return result;
    }
    typeck = cm_semantic_session_typeck(session);
    if (typeck == NULL) {
        result.status = CM_SEMANTIC_BODY_STALE;
        return result;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    typeck_status = cm_typeck_snapshot(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        result.status = cm_semantic_typeck_status(typeck_status);
        result.typeck_status = typeck_status;
        return result;
    }

    memset(owner_arguments, 0, sizeof(owner_arguments));
    memset(&owner_instantiation, 0, sizeof(owner_instantiation));
    owner_instantiation.parameter_owner = owner;
    if (owner_type_substitution_count != 0u) {
        CmSemanticTypeScan scan;

        scan = cm_semantic_scan_type(hir, owner_type_substitutions[0], 0u);
        result.status = cm_semantic_scan_status(scan);
        if (result.status != CM_SEMANTIC_BODY_OK) {
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot);
        }
        owner_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
        typeck_status = cm_typeck_import_hir_type(typeck,
            owner_type_substitutions[0], &owner_arguments[0].data.type);
        if (typeck_status != CM_TYPECK_OK) {
            result.status = cm_semantic_typeck_status(typeck_status);
            result.typeck_status = typeck_status;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot);
        }
        owner_instantiation.arguments = owner_arguments;
        owner_instantiation.argument_count = 1u;
    }
    if (!cm_typeck_instantiation_is_valid(typeck, &owner_instantiation)) {
        result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot);
    }
    memset(&environment_substitution, 0,
        sizeof(environment_substitution));
    environment_substitution.exact = &owner_instantiation;

    for (expression_index = 0u; expression_index < hir->expressions.len;
         ++expression_index) {
        const CmHirExpr *expression;
        CmHirExprId expression_id;
        const CmHirItem *callee;
        CmTypeckGenericArg callee_arguments[1];
        CmTypeckInstantiation callee_instantiation;
        uint32_t predicate_index;

        expression = (const CmHirExpr *)cm_vec_at_const(&hir->expressions,
            expression_index);
        expression_id = (CmHirExprId)(expression_index + 1u);
        if (expression == NULL || expression->owner_body != body_id
            || expression->kind != CM_HIR_EXPR_CALL) continue;
        result.expression = expression_id;
        result.callee = expression->data.call.callee;
        callee = cm_semantic_body_item(hir, expression->data.call.callee);
        if (callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION) {
            result.status = CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot);
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
                &snapshot);
        }
        if ((callee->predicate_scope_count == 0u)
                != (callee->predicate_scopes == NULL)
            || (callee->predicate_count == 0u)
                != (callee->predicates == NULL)
            || (callee->outlives_predicate_count == 0u)
                != (callee->outlives_predicates == NULL)) {
            result.status = CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot);
        }
        if (callee->predicate_scope_count != 0u) {
            result.status = CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot);
        }
        if (callee->outlives_predicate_count != 0u) {
            result.status = CM_SEMANTIC_BODY_PENDING_OUTLIVES;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot);
        }
        memset(callee_arguments, 0, sizeof(callee_arguments));
        memset(&callee_instantiation, 0, sizeof(callee_instantiation));
        callee_instantiation.parameter_owner = callee->definition;
        if (expression->data.call.type_substitution_count != 0u) {
            CmSemanticTypeScan scan;

            scan = cm_semantic_scan_type(hir,
                expression->data.call.type_substitutions[0], 0u);
            result.status = cm_semantic_scan_status(scan);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
            }
            callee_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
            typeck_status = cm_typeck_instantiate_hir_type(typeck,
                expression->data.call.type_substitutions[0],
                &owner_instantiation, &callee_arguments[0].data.type);
            if (typeck_status != CM_TYPECK_OK) {
                result.status = cm_semantic_typeck_status(typeck_status);
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
            }
            callee_instantiation.arguments = callee_arguments;
            callee_instantiation.argument_count = 1u;
        }
        if (!cm_typeck_instantiation_is_valid(typeck,
                &callee_instantiation)) {
            result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot);
        }

        for (predicate_index = 0u;
             predicate_index < callee->predicate_count;
             ++predicate_index) {
            const CmHirTraitPredicate *predicate;
            CmSemanticTypeScan scan;
            CmTraitGoal goal;
            CmTraitSelectionResult selection;

            predicate = &callee->predicates[predicate_index];
            result.predicate_index = predicate_index;
            if ((predicate->binder.lifetime_count == 0u)
                    != (predicate->binder.lifetimes == NULL)) {
                result.status = CM_SEMANTIC_BODY_INVALID;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
            }
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->binder.lifetime_count != 0u) {
                result.status = CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
            }
            if (predicate->modifier != CM_HIR_PREDICATE_REQUIRED) {
                result.status = CM_SEMANTIC_BODY_PENDING_MODIFIER;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
            }
            if ((predicate->equality_count == 0u)
                    != (predicate->equalities == NULL)) {
                result.status = CM_SEMANTIC_BODY_INVALID;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
            }
            if (predicate->equality_count != 0u) {
                result.status = CM_SEMANTIC_BODY_PENDING_PROJECTION;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
            }
            scan = cm_semantic_scan_merge(
                cm_semantic_scan_type(hir, predicate->subject, 0u),
                cm_semantic_scan_named(hir, &predicate->trait_type, 0u));
            result.status = cm_semantic_scan_status(scan);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
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
                    &snapshot);
            }
            selection = cm_semantic_session_solve_implemented(session,
                typeck, &environment_substitution, &goal);
            result.solver_kind = selection.kind;
            result.typeck_status = selection.typeck_status;
            result.status = cm_semantic_solver_status(selection.kind);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot);
            }
        }
    }
    typeck_status = cm_typeck_commit(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        result.status = CM_SEMANTIC_BODY_TYPECK_FAILURE;
        result.typeck_status = typeck_status;
        (void)cm_typeck_rollback(typeck, &snapshot);
        return result;
    }
    result = cm_semantic_body_result(CM_SEMANTIC_BODY_OK, body_id);
    result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    return result;
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
