#include "cm/hir/projection_normalizer.h"

#include "trait_solver_internal.h"

#include "cm/alloc.h"
#include "cm/vec.h"

#include <string.h>

typedef struct CmProjectionNormalizeState {
    const CmTraitImplIndex *index;
    const CmParamEnv *environment;
    CmTypeckContext *typeck;
    const CmParamEnvSubstitution *substitution;
    CmHirDefId owner;
    const CmTraitGoalEvaluator *evaluator;
    CmProjectionNormalizeLimits limits;
    size_t visited_nodes;
    size_t projection_steps;
    CmVec active_projections;
    CmVec trace_steps;
} CmProjectionNormalizeState;

typedef struct CmProjectionNormalizeTraceState {
    CmVec steps;
    CmTypeckTypeId input_type;
    CmTypeckTypeId normalized_type;
    const CmTypeckContext *term_owner;
    uint64_t term_lifetime;
    uint64_t term_revision;
} CmProjectionNormalizeTraceState;

typedef struct CmNormalizeTypePair {
    CmTypeckTypeId left;
    CmTypeckTypeId right;
} CmNormalizeTypePair;

typedef struct CmNormalizeCompareState {
    const CmTypeckContext *typeck;
    CmVec pairs;
    size_t max_pairs;
    int valid;
} CmNormalizeCompareState;

static CmProjectionNormalizeTraceState *cm_normalize_trace_state(
    CmProjectionNormalizeTrace *trace)
{
    return trace == NULL ? NULL
        : (CmProjectionNormalizeTraceState *)trace->state;
}

static const CmProjectionNormalizeTraceState *cm_normalize_trace_state_const(
    const CmProjectionNormalizeTrace *trace)
{
    return trace == NULL ? NULL
        : (const CmProjectionNormalizeTraceState *)trace->state;
}

void cm_projection_normalize_trace_init(CmProjectionNormalizeTrace *trace)
{
    CmProjectionNormalizeTraceState *state;

    if (trace == NULL) return;
    state = (CmProjectionNormalizeTraceState *)cm_alloc_zeroed(
        1u, sizeof(*state));
    cm_vec_init(&state->steps, sizeof(CmProjectionNormalizeStep));
    trace->state = state;
}

void cm_projection_normalize_trace_destroy(CmProjectionNormalizeTrace *trace)
{
    CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state(trace);
    if (state == NULL) return;
    cm_vec_destroy(&state->steps);
    cm_free(state);
    trace->state = NULL;
}

void cm_projection_normalize_trace_clear(CmProjectionNormalizeTrace *trace)
{
    CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state(trace);
    if (state != NULL) cm_vec_clear(&state->steps);
    if (state != NULL) state->input_type = CM_TYPECK_TYPE_NONE;
    if (state != NULL) state->normalized_type = CM_TYPECK_TYPE_NONE;
    if (state != NULL) state->term_owner = NULL;
    if (state != NULL) state->term_lifetime = 0u;
    if (state != NULL) state->term_revision = 0u;
}

const CmTypeckContext *cm_projection_normalize_trace_term_owner(
    const CmProjectionNormalizeTrace *trace)
{
    const CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state_const(trace);
    return state == NULL || state->steps.len == 0u
        ? NULL : state->term_owner;
}

uint64_t cm_projection_normalize_trace_term_lifetime(
    const CmProjectionNormalizeTrace *trace)
{
    const CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state_const(trace);
    return state == NULL || state->steps.len == 0u
        ? 0u : state->term_lifetime;
}

uint64_t cm_projection_normalize_trace_term_revision(
    const CmProjectionNormalizeTrace *trace)
{
    const CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state_const(trace);
    return state == NULL || state->steps.len == 0u
        ? 0u : state->term_revision;
}

size_t cm_projection_normalize_trace_count(
    const CmProjectionNormalizeTrace *trace)
{
    const CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state_const(trace);
    return state == NULL ? 0u : state->steps.len;
}

const CmProjectionNormalizeStep *cm_projection_normalize_trace_step(
    const CmProjectionNormalizeTrace *trace, size_t index)
{
    const CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state_const(trace);
    return state == NULL ? NULL
        : (const CmProjectionNormalizeStep *)cm_vec_at_const(
            &state->steps, index);
}

CmTypeckTypeId cm_projection_normalize_trace_input_type(
    const CmProjectionNormalizeTrace *trace)
{
    const CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state_const(trace);
    return state == NULL || state->steps.len == 0u
        ? CM_TYPECK_TYPE_NONE : state->input_type;
}

CmTypeckTypeId cm_projection_normalize_trace_normalized_type(
    const CmProjectionNormalizeTrace *trace)
{
    const CmProjectionNormalizeTraceState *state;

    state = cm_normalize_trace_state_const(trace);
    return state == NULL || state->steps.len == 0u
        ? CM_TYPECK_TYPE_NONE : state->normalized_type;
}

static CmProjectionNormalizeResult cm_normalize_result(
    CmTraitSolverResultKind kind)
{
    CmProjectionNormalizeResult result;

    memset(&result, 0, sizeof(result));
    result.kind = kind;
    result.type = CM_TYPECK_TYPE_NONE;
    result.typeck_status = CM_TYPECK_OK;
    return result;
}

static CmProjectionNormalizeResult cm_normalize_type_inner(
    CmProjectionNormalizeState *state, CmTypeckTypeId input);

static int cm_normalize_region_equal(const CmHirRegion *left,
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

static int cm_normalize_type_equal_inner(CmNormalizeCompareState *compare,
    CmTypeckTypeId left_id, CmTypeckTypeId right_id);

static int cm_normalize_const_equal(CmNormalizeCompareState *compare,
    const CmTypeckConst *left, const CmTypeckConst *right)
{
    if (left->kind != right->kind
        || !cm_normalize_type_equal_inner(compare, left->type,
            right->type)) return 0;
    if (!compare->valid) return 0;
    if (left->kind == CM_HIR_CONST_VALUE) {
        return left->data.value.low_bits == right->data.value.low_bits
            && left->data.value.high_bits == right->data.value.high_bits;
    }
    if (left->kind == CM_HIR_CONST_PARAMETER) {
        return left->data.parameter == right->data.parameter;
    }
    compare->valid = 0;
    return 0;
}

static int cm_normalize_named_equal(CmNormalizeCompareState *compare,
    const CmTypeckNamedType *left, const CmTypeckNamedType *right,
    int *out_equal)
{
    uint32_t index;

    *out_equal = 0;

    if (!cm_hir_def_id_equal(left->definition, right->definition)
        || left->argument_count != right->argument_count
        || ((left->argument_count == 0u) != (left->arguments == NULL))
        || ((right->argument_count == 0u) != (right->arguments == NULL))) {
        return 0;
    }
    for (index = 0u; index < left->argument_count; ++index) {
        const CmTypeckGenericArg *left_argument;
        const CmTypeckGenericArg *right_argument;

        left_argument = &left->arguments[index];
        right_argument = &right->arguments[index];
        if (left_argument->kind != right_argument->kind) return 0;
        if (left_argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (!cm_normalize_region_equal(&left_argument->data.lifetime,
                    &right_argument->data.lifetime)) return 0;
        } else if (left_argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (!cm_normalize_type_equal_inner(compare,
                    left_argument->data.type,
                    right_argument->data.type)) return 1;
        } else if (left_argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (!cm_normalize_const_equal(compare,
                    &left_argument->data.constant,
                    &right_argument->data.constant)) return 1;
        } else {
            compare->valid = 0;
            return 0;
        }
        if (!compare->valid) return 0;
    }
    *out_equal = 1;
    return 1;
}

static int cm_normalize_type_equal_inner(CmNormalizeCompareState *compare,
    CmTypeckTypeId left_id, CmTypeckTypeId right_id)
{
    const CmTypeckType *left;
    const CmTypeckType *right;
    CmNormalizeTypePair pair;
    CmTypeckStatus status;
    size_t pair_index;
    uint32_t index;

    status = cm_typeck_resolve(compare->typeck, left_id, &left_id);
    if (status != CM_TYPECK_OK) {
        compare->valid = 0;
        return 0;
    }
    status = cm_typeck_resolve(compare->typeck, right_id, &right_id);
    if (status != CM_TYPECK_OK) {
        compare->valid = 0;
        return 0;
    }
    if (left_id == right_id) return 1;
    for (pair_index = 0u; pair_index < compare->pairs.len; ++pair_index) {
        const CmNormalizeTypePair *seen;

        seen = (const CmNormalizeTypePair *)cm_vec_at_const(
            &compare->pairs, pair_index);
        if (seen != NULL && seen->left == left_id
            && seen->right == right_id) return 1;
    }
    if (compare->pairs.len >= compare->max_pairs) {
        compare->valid = 0;
        return 0;
    }
    pair.left = left_id;
    pair.right = right_id;
    (void)cm_vec_push(&compare->pairs, &pair);
    left = cm_typeck_get_type(compare->typeck, left_id);
    right = cm_typeck_get_type(compare->typeck, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) {
        return 0;
    }
    switch (left->kind) {
    case CM_TYPECK_TYPE_VARIABLE:
        /* Distinct inference variables are not alpha-equivalent keys. */
        return 0;
    case CM_TYPECK_TYPE_NEVER:
    case CM_TYPECK_TYPE_UNIT:
    case CM_TYPECK_TYPE_BOOL:
    case CM_TYPECK_TYPE_CHAR:
    case CM_TYPECK_TYPE_STR:
        return 1;
    case CM_TYPECK_TYPE_INTEGER:
        return left->data.integer_type == right->data.integer_type;
    case CM_TYPECK_TYPE_FLOAT:
        return left->data.float_type == right->data.float_type;
    case CM_TYPECK_TYPE_REFERENCE:
        return left->data.reference_type.mutability
                == right->data.reference_type.mutability
            && cm_normalize_region_equal(&left->data.reference_type.region,
                &right->data.reference_type.region)
            && cm_normalize_type_equal_inner(compare,
                left->data.reference_type.pointee,
                right->data.reference_type.pointee);
    case CM_TYPECK_TYPE_RAW_POINTER:
        return left->data.raw_pointer_type.mutability
                == right->data.raw_pointer_type.mutability
            && cm_normalize_type_equal_inner(compare,
                left->data.raw_pointer_type.pointee,
                right->data.raw_pointer_type.pointee);
    case CM_TYPECK_TYPE_TUPLE:
        if (left->data.tuple_type.element_count
                != right->data.tuple_type.element_count) return 0;
        for (index = 0u; index < left->data.tuple_type.element_count;
             ++index) {
            if (!cm_normalize_type_equal_inner(compare,
                    left->data.tuple_type.elements[index],
                    right->data.tuple_type.elements[index])) return 0;
        }
        return 1;
    case CM_TYPECK_TYPE_ARRAY:
        return cm_normalize_type_equal_inner(compare,
                left->data.array_type.element,
                right->data.array_type.element)
            && cm_normalize_const_equal(compare,
                &left->data.array_type.length,
                &right->data.array_type.length);
    case CM_TYPECK_TYPE_SLICE:
        return cm_normalize_type_equal_inner(compare,
            left->data.slice_type.element,
            right->data.slice_type.element);
    case CM_TYPECK_TYPE_FN_POINTER:
        if (left->data.fn_pointer_type.parameter_count
                != right->data.fn_pointer_type.parameter_count
            || left->data.fn_pointer_type.abi
                != right->data.fn_pointer_type.abi
            || left->data.fn_pointer_type.safety
                != right->data.fn_pointer_type.safety
            || left->data.fn_pointer_type.is_variadic
                != right->data.fn_pointer_type.is_variadic) return 0;
        for (index = 0u;
             index < left->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_normalize_type_equal_inner(compare,
                    left->data.fn_pointer_type.parameters[index],
                    right->data.fn_pointer_type.parameters[index])) return 0;
        }
        return cm_normalize_type_equal_inner(compare,
            left->data.fn_pointer_type.return_type,
            right->data.fn_pointer_type.return_type);
    case CM_TYPECK_TYPE_ADT:
    {
        int equal;

        return cm_normalize_named_equal(compare, &left->data.named_type,
            &right->data.named_type, &equal) && equal;
    }
    case CM_TYPECK_TYPE_PARAMETER:
        return left->data.parameter_type.parameter
            == right->data.parameter_type.parameter;
    case CM_TYPECK_TYPE_PROJECTION:
    {
        int trait_equal;
        int associated_equal;

        return cm_normalize_type_equal_inner(compare,
                left->data.projection_type.self_type,
                right->data.projection_type.self_type)
            && cm_normalize_named_equal(compare,
                &left->data.projection_type.trait_type,
                &right->data.projection_type.trait_type, &trait_equal)
            && trait_equal
            && cm_normalize_named_equal(compare,
                &left->data.projection_type.associated_type,
                &right->data.projection_type.associated_type,
                &associated_equal)
            && associated_equal;
    }
    }
    compare->valid = 0;
    return 0;
}

static int cm_normalize_type_equal(const CmTypeckContext *typeck,
    CmTypeckTypeId left, CmTypeckTypeId right, size_t max_pairs,
    int *out_valid, size_t *out_pair_count)
{
    CmNormalizeCompareState compare;
    int equal;

    memset(&compare, 0, sizeof(compare));
    compare.typeck = typeck;
    compare.max_pairs = max_pairs;
    compare.valid = 1;
    cm_vec_init(&compare.pairs, sizeof(CmNormalizeTypePair));
    equal = cm_normalize_type_equal_inner(&compare, left, right);
    *out_valid = compare.valid;
    *out_pair_count = compare.pairs.len;
    cm_vec_destroy(&compare.pairs);
    return equal;
}

static CmProjectionNormalizeResult cm_normalize_typeck_failure(
    CmTypeckStatus status)
{
    CmProjectionNormalizeResult result;

    result = cm_normalize_result(status == CM_TYPECK_OVERFLOW
        ? CM_TRAIT_SOLVER_OVERFLOW : CM_TRAIT_SOLVER_TYPECK_FAILURE);
    result.typeck_status = status;
    return result;
}

static CmProjectionNormalizeResult cm_normalize_child(
    CmProjectionNormalizeState *state, CmTypeckTypeId input,
    CmTypeckTypeId *out_type, int *out_changed)
{
    CmProjectionNormalizeResult result;
    CmTypeckTypeId resolved;
    CmTypeckStatus status;

    result = cm_normalize_type_inner(state, input);
    if (result.kind != CM_TRAIT_SOLVER_PROVEN) return result;
    status = cm_typeck_resolve(state->typeck, input, &resolved);
    if (status != CM_TYPECK_OK) return cm_normalize_typeck_failure(status);
    *out_type = result.type;
    if (result.type != resolved) *out_changed = 1;
    return result;
}

static CmProjectionNormalizeResult cm_normalize_named(
    CmProjectionNormalizeState *state, const CmTypeckNamedType *input,
    CmTypeckNamedType *output, int *out_changed)
{
    CmProjectionNormalizeResult result;
    uint32_t index;

    *output = *input;
    if ((input->argument_count == 0u) != (input->arguments == NULL)) {
        return cm_normalize_result(CM_TRAIT_SOLVER_INVALID);
    }
    output->arguments = input->argument_count == 0u ? NULL
        : (CmTypeckGenericArg *)cm_alloc_zeroed(input->argument_count,
            sizeof(CmTypeckGenericArg));
    for (index = 0u; index < input->argument_count; ++index) {
        output->arguments[index] = input->arguments[index];
        if (input->arguments[index].kind == CM_HIR_GENERIC_ARG_TYPE) {
            result = cm_normalize_child(state,
                input->arguments[index].data.type,
                &output->arguments[index].data.type, out_changed);
            if (result.kind != CM_TRAIT_SOLVER_PROVEN) return result;
        } else if (input->arguments[index].kind
                == CM_HIR_GENERIC_ARG_CONST) {
            result = cm_normalize_child(state,
                input->arguments[index].data.constant.type,
                &output->arguments[index].data.constant.type,
                out_changed);
            if (result.kind != CM_TRAIT_SOLVER_PROVEN) return result;
        } else if (input->arguments[index].kind
                != CM_HIR_GENERIC_ARG_LIFETIME) {
            return cm_normalize_result(CM_TRAIT_SOLVER_INVALID);
        }
    }
    return cm_normalize_result(CM_TRAIT_SOLVER_PROVEN);
}

static void cm_normalize_free_named(CmTypeckNamedType *named)
{
    cm_free(named->arguments);
    named->arguments = NULL;
}

static CmProjectionNormalizeResult cm_normalize_projection(
    CmProjectionNormalizeState *state, CmTypeckTypeId resolved,
    const CmTypeckType *source)
{
    CmProjectionNormalizeResult result;
    CmProjectionTargetGoal goal;
    CmProjectionTargetResult target;
    CmTypeckType projection;
    CmTypeckNamedType source_trait;
    CmTypeckNamedType source_associated;
    CmTypeckTypeId source_self;
    CmTypeckTypeId normalized_projection;
    size_t active_index;
    size_t trace_step_index;
    size_t compared_pair_count;
    size_t remaining_nodes;
    int valid;
    int changed;
    int pushed;

    projection = *source;
    source_self = projection.data.projection_type.self_type;
    source_trait = projection.data.projection_type.trait_type;
    source_associated = projection.data.projection_type.associated_type;
    projection.data.projection_type.trait_type.arguments = NULL;
    projection.data.projection_type.associated_type.arguments = NULL;
    changed = 0;
    pushed = 0;
    trace_step_index = (size_t)-1;
    result = cm_normalize_child(state,
        source_self,
        &projection.data.projection_type.self_type, &changed);
    if (result.kind != CM_TRAIT_SOLVER_PROVEN) return result;
    result = cm_normalize_named(state,
        &source_trait,
        &projection.data.projection_type.trait_type, &changed);
    if (result.kind != CM_TRAIT_SOLVER_PROVEN) goto done;
    result = cm_normalize_named(state,
        &source_associated,
        &projection.data.projection_type.associated_type, &changed);
    if (result.kind != CM_TRAIT_SOLVER_PROVEN) goto done;
    normalized_projection = resolved;
    if (changed) {
        result.typeck_status = cm_typeck_add_type(state->typeck,
            &projection, &normalized_projection);
        if (result.typeck_status != CM_TYPECK_OK) {
            result.kind = result.typeck_status == CM_TYPECK_OVERFLOW
                ? CM_TRAIT_SOLVER_OVERFLOW
                : CM_TRAIT_SOLVER_TYPECK_FAILURE;
            goto done;
        }
    }
    for (active_index = 0u;
         active_index < state->active_projections.len; ++active_index) {
        const CmTypeckTypeId *active;

        active = (const CmTypeckTypeId *)cm_vec_at_const(
            &state->active_projections, active_index);
        valid = 1;
        compared_pair_count = 0u;
        remaining_nodes = state->limits.max_nodes - state->visited_nodes;
        if (active != NULL && cm_normalize_type_equal(state->typeck,
                *active, normalized_projection, remaining_nodes,
                &valid, &compared_pair_count)) {
            state->visited_nodes += compared_pair_count;
            result = cm_normalize_result(CM_TRAIT_SOLVER_AMBIGUOUS);
            result.cause = CM_PROJECTION_NORMALIZE_CAUSE_CYCLE;
            goto done;
        }
        state->visited_nodes += compared_pair_count;
        if (!valid) {
            result = cm_normalize_result(CM_TRAIT_SOLVER_OVERFLOW);
            result.cause = CM_PROJECTION_NORMALIZE_CAUSE_NODE_LIMIT;
            result.typeck_status = CM_TYPECK_OVERFLOW;
            goto done;
        }
    }
    if (state->projection_steps >= state->limits.max_projection_steps) {
        result = cm_normalize_result(CM_TRAIT_SOLVER_OVERFLOW);
        result.cause = CM_PROJECTION_NORMALIZE_CAUSE_PROJECTION_LIMIT;
        result.typeck_status = CM_TYPECK_OVERFLOW;
        goto done;
    }
    (void)cm_vec_push(&state->active_projections, &normalized_projection);
    pushed = 1;
    memset(&goal, 0, sizeof(goal));
    goal.owner = state->owner;
    goal.projection_type = normalized_projection;
    goal.expected_type = CM_TYPECK_TYPE_NONE;
    target = cm_trait_solver_select_projection_target(state->index,
        state->environment, state->typeck, state->substitution, &goal,
        state->evaluator);
    if (target.selection.kind != CM_TRAIT_SOLVER_PROVEN) {
        result = cm_normalize_result(target.selection.kind);
        result.typeck_status = target.selection.typeck_status;
        goto done;
    }
    {
        CmProjectionNormalizeStep step;
        int valid_proof;

        valid_proof = 0;
        if (target.selection.proof_origin == CM_TRAIT_PROOF_PARAM_ENV) {
            valid_proof = target.selection.param_env_fact_index
                    != CM_TRAIT_PROOF_FACT_NONE
                && target.selection.param_env_equality_index
                    != CM_TRAIT_PROOF_EQUALITY_NONE
                && cm_hir_def_id_is_none(
                    target.selection.impl_definition)
                && cm_hir_def_id_is_none(
                    target.selection.impl_associated_definition);
        } else if (target.selection.proof_origin == CM_TRAIT_PROOF_IMPL) {
            valid_proof = target.selection.param_env_fact_index
                    == CM_TRAIT_PROOF_FACT_NONE
                && target.selection.param_env_equality_index
                    == CM_TRAIT_PROOF_EQUALITY_NONE
                && !cm_hir_def_id_is_none(
                    target.selection.impl_definition)
                && !cm_hir_def_id_is_none(
                    target.selection.impl_associated_definition);
        }
        if (!valid_proof || target.target == CM_TYPECK_TYPE_NONE
            || cm_typeck_get_type(state->typeck, target.target) == NULL) {
            result = cm_normalize_result(CM_TRAIT_SOLVER_INVALID);
            goto done;
        }
        memset(&step, 0, sizeof(step));
        step.projection = normalized_projection;
        step.target = target.target;
        step.normalized_target = CM_TYPECK_TYPE_NONE;
        step.proof_origin = target.selection.proof_origin;
        step.param_env_fact_index =
            target.selection.param_env_fact_index;
        step.param_env_equality_index =
            target.selection.param_env_equality_index;
        step.impl_definition = target.selection.impl_definition;
        step.impl_associated_definition =
            target.selection.impl_associated_definition;
        trace_step_index = state->trace_steps.len;
        (void)cm_vec_push(&state->trace_steps, &step);
    }
    state->projection_steps += 1u;
    result = cm_normalize_type_inner(state, target.target);
    if (result.kind == CM_TRAIT_SOLVER_PROVEN) {
        CmProjectionNormalizeStep *step;

        step = (CmProjectionNormalizeStep *)cm_vec_at(
            &state->trace_steps, trace_step_index);
        if (step == NULL || result.type == CM_TYPECK_TYPE_NONE
            || cm_typeck_get_type(state->typeck, result.type) == NULL) {
            result = cm_normalize_result(CM_TRAIT_SOLVER_INVALID);
        } else {
            step->normalized_target = result.type;
        }
    }

done:
    if (pushed) cm_vec_resize(&state->active_projections,
        state->active_projections.len - 1u);
    cm_normalize_free_named(&projection.data.projection_type.trait_type);
    cm_normalize_free_named(&projection.data.projection_type.associated_type);
    return result;
}

static CmProjectionNormalizeResult cm_normalize_type_inner(
    CmProjectionNormalizeState *state, CmTypeckTypeId input)
{
    CmProjectionNormalizeResult result;
    const CmTypeckType *source;
    CmTypeckType rebuilt;
    CmTypeckTypeKind source_kind;
    CmTypeckTypeId *source_types;
    CmTypeckGenericArg *source_arguments;
    CmTypeckTypeId first_child;
    CmTypeckTypeId second_child;
    CmTypeckTypeId resolved;
    CmTypeckStatus status;
    uint32_t index;
    int changed;

    if (state->visited_nodes >= state->limits.max_nodes) {
        result = cm_normalize_result(CM_TRAIT_SOLVER_OVERFLOW);
        result.cause = CM_PROJECTION_NORMALIZE_CAUSE_NODE_LIMIT;
        result.typeck_status = CM_TYPECK_OVERFLOW;
        return result;
    }
    state->visited_nodes += 1u;
    status = cm_typeck_resolve(state->typeck, input, &resolved);
    if (status != CM_TYPECK_OK) return cm_normalize_typeck_failure(status);
    source = cm_typeck_get_type(state->typeck, resolved);
    if (source == NULL) {
        return cm_normalize_result(CM_TRAIT_SOLVER_INVALID);
    }
    if (source->kind == CM_TYPECK_TYPE_PROJECTION) {
        return cm_normalize_projection(state, resolved, source);
    }
    source_kind = source->kind;
    source_types = NULL;
    source_arguments = NULL;
    first_child = CM_TYPECK_TYPE_NONE;
    second_child = CM_TYPECK_TYPE_NONE;
    result = cm_normalize_result(CM_TRAIT_SOLVER_PROVEN);
    result.type = resolved;
    changed = 0;
    rebuilt = *source;
    switch (source->kind) {
    case CM_TYPECK_TYPE_VARIABLE:
    case CM_TYPECK_TYPE_NEVER:
    case CM_TYPECK_TYPE_UNIT:
    case CM_TYPECK_TYPE_BOOL:
    case CM_TYPECK_TYPE_CHAR:
    case CM_TYPECK_TYPE_STR:
    case CM_TYPECK_TYPE_INTEGER:
    case CM_TYPECK_TYPE_FLOAT:
    case CM_TYPECK_TYPE_PARAMETER:
        return result;
    case CM_TYPECK_TYPE_REFERENCE:
        first_child = rebuilt.data.reference_type.pointee;
        result = cm_normalize_child(state, first_child,
            &rebuilt.data.reference_type.pointee, &changed);
        break;
    case CM_TYPECK_TYPE_RAW_POINTER:
        first_child = rebuilt.data.raw_pointer_type.pointee;
        result = cm_normalize_child(state, first_child,
            &rebuilt.data.raw_pointer_type.pointee, &changed);
        break;
    case CM_TYPECK_TYPE_TUPLE:
        source_types = rebuilt.data.tuple_type.elements;
        rebuilt.data.tuple_type.elements = rebuilt.data.tuple_type.element_count
            == 0u ? NULL : (CmTypeckTypeId *)cm_alloc_zeroed(
                rebuilt.data.tuple_type.element_count,
                sizeof(CmTypeckTypeId));
        for (index = 0u;
             index < rebuilt.data.tuple_type.element_count; ++index) {
            result = cm_normalize_child(state,
                source_types[index],
                &rebuilt.data.tuple_type.elements[index], &changed);
            if (result.kind != CM_TRAIT_SOLVER_PROVEN) break;
        }
        break;
    case CM_TYPECK_TYPE_ARRAY:
        first_child = rebuilt.data.array_type.element;
        second_child = rebuilt.data.array_type.length.type;
        result = cm_normalize_child(state, first_child,
            &rebuilt.data.array_type.element, &changed);
        if (result.kind == CM_TRAIT_SOLVER_PROVEN) {
            result = cm_normalize_child(state, second_child,
                &rebuilt.data.array_type.length.type, &changed);
        }
        break;
    case CM_TYPECK_TYPE_SLICE:
        first_child = rebuilt.data.slice_type.element;
        result = cm_normalize_child(state, first_child,
            &rebuilt.data.slice_type.element, &changed);
        break;
    case CM_TYPECK_TYPE_FN_POINTER:
        source_types = rebuilt.data.fn_pointer_type.parameters;
        first_child = rebuilt.data.fn_pointer_type.return_type;
        rebuilt.data.fn_pointer_type.parameters =
            rebuilt.data.fn_pointer_type.parameter_count == 0u ? NULL
            : (CmTypeckTypeId *)cm_alloc_zeroed(
                rebuilt.data.fn_pointer_type.parameter_count,
                sizeof(CmTypeckTypeId));
        for (index = 0u;
             index < rebuilt.data.fn_pointer_type.parameter_count; ++index) {
            result = cm_normalize_child(state,
                source_types[index],
                &rebuilt.data.fn_pointer_type.parameters[index], &changed);
            if (result.kind != CM_TRAIT_SOLVER_PROVEN) break;
        }
        if (result.kind == CM_TRAIT_SOLVER_PROVEN) {
            result = cm_normalize_child(state, first_child,
                &rebuilt.data.fn_pointer_type.return_type, &changed);
        }
        break;
    case CM_TYPECK_TYPE_ADT:
        if (!cm_typeck_adt_is_valid(state->typeck,
                &source->data.named_type)) {
            return cm_normalize_result(CM_TRAIT_SOLVER_INVALID);
        }
        source_arguments = rebuilt.data.named_type.arguments;
        rebuilt.data.named_type.arguments = NULL;
        {
            CmTypeckNamedType source_named;

            source_named = rebuilt.data.named_type;
            source_named.arguments = source_arguments;
            result = cm_normalize_named(state, &source_named,
                &rebuilt.data.named_type, &changed);
        }
        break;
    case CM_TYPECK_TYPE_PROJECTION:
        return cm_normalize_result(CM_TRAIT_SOLVER_INVALID);
    }
    if (result.kind == CM_TRAIT_SOLVER_PROVEN && changed) {
        status = cm_typeck_add_type(state->typeck, &rebuilt, &result.type);
        if (status != CM_TYPECK_OK) result = cm_normalize_typeck_failure(status);
    } else if (result.kind == CM_TRAIT_SOLVER_PROVEN) {
        result.type = resolved;
    }
    if (source_kind == CM_TYPECK_TYPE_TUPLE) {
        cm_free(rebuilt.data.tuple_type.elements);
    } else if (source_kind == CM_TYPECK_TYPE_FN_POINTER) {
        cm_free(rebuilt.data.fn_pointer_type.parameters);
    } else if (source_kind == CM_TYPECK_TYPE_ADT) {
        cm_normalize_free_named(&rebuilt.data.named_type);
    }
    return result;
}

static CmProjectionNormalizeResult cm_projection_normalize_type_impl(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    CmHirDefId owner, CmTypeckTypeId type,
    const CmTraitGoalEvaluator *evaluator,
    CmProjectionNormalizeLimits limits,
    CmProjectionNormalizeTrace *trace)
{
    CmProjectionNormalizeState state;
    CmProjectionNormalizeResult result;
    CmProjectionNormalizeTraceState *trace_state;
    CmTypeckSnapshot snapshot;
    CmTypeckStatus status;

    result = cm_normalize_result(CM_TRAIT_SOLVER_INVALID);
    trace_state = cm_normalize_trace_state(trace);
    if (trace != NULL && trace_state == NULL) return result;
    if (trace_state != NULL) {
        cm_vec_clear(&trace_state->steps);
        trace_state->input_type = CM_TYPECK_TYPE_NONE;
        trace_state->normalized_type = CM_TYPECK_TYPE_NONE;
        trace_state->term_owner = NULL;
        trace_state->term_lifetime = 0u;
        trace_state->term_revision = 0u;
    }
    if (limits.max_nodes == 0u || type == CM_TYPECK_TYPE_NONE
        || cm_trait_solver_validate_session(index, environment, typeck,
            substitution, owner) != CM_TRAIT_SOLVER_PROVEN
        || cm_typeck_get_type(typeck, type) == NULL) return result;
    memset(&state, 0, sizeof(state));
    state.index = index;
    state.environment = environment;
    state.typeck = typeck;
    state.substitution = substitution;
    state.owner = owner;
    state.evaluator = evaluator;
    state.limits = limits;
    cm_vec_init(&state.active_projections, sizeof(CmTypeckTypeId));
    cm_vec_init(&state.trace_steps, sizeof(CmProjectionNormalizeStep));
    status = cm_typeck_snapshot(typeck, &snapshot);
    if (status != CM_TYPECK_OK) {
        cm_vec_destroy(&state.active_projections);
        cm_vec_destroy(&state.trace_steps);
        return cm_normalize_typeck_failure(status);
    }
    result = cm_normalize_type_inner(&state, type);
    result.visited_node_count = state.visited_nodes;
    result.projection_step_count = state.projection_steps;
    if (result.kind == CM_TRAIT_SOLVER_PROVEN) {
        status = cm_typeck_commit(typeck, &snapshot);
    } else {
        status = cm_typeck_rollback(typeck, &snapshot);
        result.type = CM_TYPECK_TYPE_NONE;
    }
    if (status != CM_TYPECK_OK) {
        result = cm_normalize_typeck_failure(status);
        result.visited_node_count = state.visited_nodes;
        result.projection_step_count = state.projection_steps;
    }
    if (result.kind == CM_TRAIT_SOLVER_PROVEN
        && trace_state != NULL) {
        cm_vec_append(&trace_state->steps, state.trace_steps.data,
            state.trace_steps.len);
        trace_state->input_type = state.trace_steps.len == 0u
            ? CM_TYPECK_TYPE_NONE : type;
        trace_state->normalized_type = state.trace_steps.len == 0u
            ? CM_TYPECK_TYPE_NONE : result.type;
        trace_state->term_owner = state.trace_steps.len == 0u
            ? NULL : typeck;
        trace_state->term_lifetime = state.trace_steps.len == 0u
            ? 0u : cm_typeck_lifetime_id(typeck);
        trace_state->term_revision = state.trace_steps.len == 0u
            ? 0u : cm_typeck_state_revision(typeck);
    }
    cm_vec_destroy(&state.active_projections);
    cm_vec_destroy(&state.trace_steps);
    return result;
}

CmProjectionNormalizeResult cm_projection_normalize_type(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    CmHirDefId owner, CmTypeckTypeId type,
    const CmTraitGoalEvaluator *evaluator,
    CmProjectionNormalizeLimits limits)
{
    return cm_projection_normalize_type_impl(index, environment, typeck,
        substitution, owner, type, evaluator, limits, NULL);
}

CmProjectionNormalizeResult cm_projection_normalize_type_traced(
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    CmHirDefId owner, CmTypeckTypeId type,
    const CmTraitGoalEvaluator *evaluator,
    CmProjectionNormalizeLimits limits,
    CmProjectionNormalizeTrace *trace)
{
    return cm_projection_normalize_type_impl(index, environment, typeck,
        substitution, owner, type, evaluator, limits, trace);
}
