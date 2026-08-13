#include "cm/hir/goal_table.h"

#include "trait_solver_internal.h"

#include "cm/alloc.h"
#include "cm/vec.h"

#include <string.h>

#define CM_GOAL_TABLE_DEFAULT_DEPTH 256u
#define CM_GOAL_TABLE_DEFAULT_NODES 4096u
#define CM_GOAL_TABLE_DEFAULT_ENTRIES 1024u
#define CM_GOAL_KEY_VERSION 3u

typedef enum CmGoalCanonicalStatus {
    CM_GOAL_CANONICAL_OK = 0,
    CM_GOAL_CANONICAL_UNCACHEABLE,
    CM_GOAL_CANONICAL_OVERFLOW,
    CM_GOAL_CANONICAL_INVALID
} CmGoalCanonicalStatus;

typedef enum CmGoalTableEntryState {
    CM_GOAL_ENTRY_VACANT = 0,
    CM_GOAL_ENTRY_EVALUATING,
    CM_GOAL_ENTRY_COMPLETE
} CmGoalTableEntryState;

typedef struct CmGoalCanonicalNode {
    size_t word_start;
    size_t word_count;
    size_t height;
} CmGoalCanonicalNode;

typedef struct CmGoalTypeFrame {
    CmTypeckTypeId type;
    size_t next_child;
    size_t child_count;
} CmGoalTypeFrame;

typedef struct CmGoalRegionVariable {
    uint32_t source;
    uint32_t canonical;
} CmGoalRegionVariable;

typedef struct CmGoalCanonicalContext {
    const CmTypeckContext *typeck;
    const CmHirContext *hir;
    CmTraitGoalBinder binder;
    size_t max_depth;
    size_t max_nodes;
    size_t type_count;
    size_t reachable_count;
    uint32_t next_type_variable;
    uint32_t next_region_variable;
    unsigned char *visit_state;
    size_t *canonical_node;
    size_t *height;
    CmVec nodes;
    CmVec node_words;
    CmVec record;
    CmVec frames;
    CmVec region_variables;
} CmGoalCanonicalContext;

typedef struct CmGoalTableEntry {
    CmGoalTableEntryState state;
    int cycle_tainted;
    CmVec key;
    CmTraitSelectionResult result;
} CmGoalTableEntry;

typedef struct CmTraitGoalTableState {
    const CmTraitImplIndex *index;
    const CmParamEnv *environment;
    const CmHirContext *hir;
    CmTraitImplUniverse universe;
    CmHirCrateId local_crate;
    uint64_t hir_storage_lifetime_id;
    uint64_t hir_semantic_generation;
    uint64_t hir_rewind_generation;
    size_t hir_crate_count;
    size_t hir_module_count;
    size_t hir_item_count;
    size_t hir_body_count;
    size_t hir_expression_count;
    size_t hir_type_count;
    size_t hir_generic_parameter_count;
    size_t hir_definition_count;
    size_t hir_prebound_associated_type_count;
    CmHirDefId exact_owner;
    CmHirDefId enclosing_owner;
    size_t fact_count;
    size_t pending_count;
    CmTraitGoalTableLimits limits;
    CmVec entries;
    size_t cache_hit_count;
} CmTraitGoalTableState;

static CmTraitSelectionResult cm_goal_result(CmTraitSolverResultKind kind)
{
    CmTraitSelectionResult result;

    memset(&result, 0, sizeof(result));
    result.kind = kind;
    result.param_env_fact_index = CM_TRAIT_PROOF_FACT_NONE;
    result.param_env_equality_index = CM_TRAIT_PROOF_EQUALITY_NONE;
    result.impl_definition = cm_hir_def_id_none();
    result.impl_item = CM_HIR_ITEM_NONE;
    result.impl_associated_definition = cm_hir_def_id_none();
    result.typeck_status = CM_TYPECK_OK;
    return result;
}

static CmTraitGoalTableState *cm_goal_table_state(CmTraitGoalTable *table)
{
    return table == NULL ? NULL : (CmTraitGoalTableState *)table->state;
}

static const CmTraitGoalTableState *cm_goal_table_state_const(
    const CmTraitGoalTable *table)
{
    return table == NULL ? NULL
        : (const CmTraitGoalTableState *)table->state;
}

static int cm_goal_table_state_is_current(
    const CmTraitGoalTableState *state)
{
    const CmHirContext *hir;

    if (state == NULL || state->hir == NULL
        || !cm_trait_impl_index_is_current(state->index)
        || cm_trait_impl_index_hir(state->index) != state->hir
        || cm_trait_impl_index_universe(state->index) != state->universe
        || cm_trait_impl_index_local_crate(state->index)
            != state->local_crate
        || !cm_param_env_is_current(state->environment)
        || cm_param_env_hir(state->environment) != state->hir
        || !cm_hir_def_id_equal(cm_param_env_exact_owner(
                state->environment), state->exact_owner)
        || !cm_hir_def_id_equal(cm_param_env_enclosing_owner(
                state->environment), state->enclosing_owner)
        || cm_param_env_fact_count(state->environment) != state->fact_count
        || cm_param_env_pending_count(state->environment)
            != state->pending_count) {
        return 0;
    }
    hir = state->hir;
    return hir->storage.lifetime_id == state->hir_storage_lifetime_id
        && hir->semantic_generation == state->hir_semantic_generation
        && hir->rewind_generation == state->hir_rewind_generation
        && hir->crates.len == state->hir_crate_count
        && hir->modules.len == state->hir_module_count
        && hir->items.len == state->hir_item_count
        && hir->bodies.len == state->hir_body_count
        && hir->expressions.len == state->hir_expression_count
        && hir->types.len == state->hir_type_count
        && hir->generic_parameters.len
            == state->hir_generic_parameter_count
        && hir->definitions.len == state->hir_definition_count
        && hir->prebound_associated_types.len
            == state->hir_prebound_associated_type_count;
}

static void cm_goal_push_word(CmVec *words, uint64_t word)
{
    (void)cm_vec_push(words, &word);
}

static void cm_goal_push_def(CmVec *words, CmHirDefId definition)
{
    cm_goal_push_word(words, (uint64_t)definition.crate_id);
    cm_goal_push_word(words, (uint64_t)definition.index);
}

static CmGoalCanonicalStatus cm_goal_region_variable(
    CmGoalCanonicalContext *canonical, uint32_t source,
    uint32_t *out_variable)
{
    size_t index;
    CmGoalRegionVariable variable;

    for (index = 0u; index < canonical->region_variables.len; ++index) {
        const CmGoalRegionVariable *existing;

        existing = (const CmGoalRegionVariable *)cm_vec_at_const(
            &canonical->region_variables, index);
        if (existing->source == source) {
            *out_variable = existing->canonical;
            return CM_GOAL_CANONICAL_OK;
        }
    }
    if (canonical->next_region_variable == UINT32_MAX) {
        return CM_GOAL_CANONICAL_OVERFLOW;
    }
    variable.source = source;
    variable.canonical = canonical->next_region_variable++;
    (void)cm_vec_push(&canonical->region_variables, &variable);
    *out_variable = variable.canonical;
    return CM_GOAL_CANONICAL_OK;
}

static CmGoalCanonicalStatus cm_goal_append_region(
    CmGoalCanonicalContext *canonical, CmVec *words,
    const CmHirRegion *region)
{
    const CmHirGenericParam *parameter;
    uint32_t variable;

    if (region == NULL) return CM_GOAL_CANONICAL_INVALID;
    cm_goal_push_word(words, (uint64_t)region->kind);
    switch (region->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return CM_GOAL_CANONICAL_OK;
    case CM_HIR_REGION_EARLY_BOUND:
        parameter = cm_hir_get_generic_param(canonical->hir,
            region->data.parameter);
        if (parameter == NULL
            || parameter->kind != CM_HIR_GENERIC_LIFETIME) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        cm_goal_push_word(words, (uint64_t)region->data.parameter);
        return CM_GOAL_CANONICAL_OK;
    case CM_HIR_REGION_LATE_BOUND:
        if (region->data.binder_index >= canonical->binder.lifetime_count) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        cm_goal_push_word(words,
            (uint64_t)canonical->binder.debruijn_depth);
        cm_goal_push_word(words, (uint64_t)region->data.binder_index);
        return CM_GOAL_CANONICAL_OK;
    case CM_HIR_REGION_INFER:
        if (cm_goal_region_variable(canonical,
                region->data.inference_variable, &variable)
                != CM_GOAL_CANONICAL_OK) {
            return CM_GOAL_CANONICAL_OVERFLOW;
        }
        cm_goal_push_word(words, (uint64_t)variable);
        return CM_GOAL_CANONICAL_OK;
    case CM_HIR_REGION_ERROR:
        return CM_GOAL_CANONICAL_UNCACHEABLE;
    }
    return CM_GOAL_CANONICAL_INVALID;
}

static CmGoalCanonicalStatus cm_goal_type_child_count(
    const CmTypeckType *type, size_t *out_count)
{
    size_t count;
    uint32_t index;

    if (type == NULL || out_count == NULL) return CM_GOAL_CANONICAL_INVALID;
    count = 0u;
    switch (type->kind) {
    case CM_TYPECK_TYPE_REFERENCE:
    case CM_TYPECK_TYPE_RAW_POINTER:
    case CM_TYPECK_TYPE_SLICE:
        count = 1u;
        break;
    case CM_TYPECK_TYPE_TUPLE:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        count = type->data.tuple_type.element_count;
        break;
    case CM_TYPECK_TYPE_ARRAY:
        count = 2u;
        break;
    case CM_TYPECK_TYPE_FN_POINTER:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        count = (size_t)type->data.fn_pointer_type.parameter_count + 1u;
        break;
    case CM_TYPECK_TYPE_ADT:
        if ((type->data.named_type.argument_count == 0u)
                != (type->data.named_type.arguments == NULL)) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        for (index = 0u; index < type->data.named_type.argument_count;
             ++index) {
            CmHirGenericArgKind kind;

            kind = type->data.named_type.arguments[index].kind;
            if (kind == CM_HIR_GENERIC_ARG_TYPE
                || kind == CM_HIR_GENERIC_ARG_CONST) {
                count += 1u;
            } else if (kind != CM_HIR_GENERIC_ARG_LIFETIME) {
                return CM_GOAL_CANONICAL_INVALID;
            }
        }
        break;
    case CM_TYPECK_TYPE_PROJECTION:
        if ((type->data.projection_type.trait_type.argument_count == 0u)
                != (type->data.projection_type.trait_type.arguments == NULL)
            || (type->data.projection_type.associated_type.argument_count
                    == 0u)
                != (type->data.projection_type.associated_type.arguments
                    == NULL)) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        count = 1u;
        for (index = 0u;
             index < type->data.projection_type.trait_type.argument_count;
             ++index) {
            CmHirGenericArgKind kind;

            kind = type->data.projection_type.trait_type.arguments[index]
                .kind;
            if (kind == CM_HIR_GENERIC_ARG_TYPE
                || kind == CM_HIR_GENERIC_ARG_CONST) count += 1u;
            else if (kind != CM_HIR_GENERIC_ARG_LIFETIME) {
                return CM_GOAL_CANONICAL_INVALID;
            }
        }
        for (index = 0u;
             index < type->data.projection_type.associated_type
                .argument_count; ++index) {
            CmHirGenericArgKind kind;

            kind = type->data.projection_type.associated_type
                .arguments[index].kind;
            if (kind == CM_HIR_GENERIC_ARG_TYPE
                || kind == CM_HIR_GENERIC_ARG_CONST) count += 1u;
            else if (kind != CM_HIR_GENERIC_ARG_LIFETIME) {
                return CM_GOAL_CANONICAL_INVALID;
            }
        }
        break;
    case CM_TYPECK_TYPE_VARIABLE:
    case CM_TYPECK_TYPE_NEVER:
    case CM_TYPECK_TYPE_UNIT:
    case CM_TYPECK_TYPE_BOOL:
    case CM_TYPECK_TYPE_CHAR:
    case CM_TYPECK_TYPE_STR:
    case CM_TYPECK_TYPE_INTEGER:
    case CM_TYPECK_TYPE_FLOAT:
    case CM_TYPECK_TYPE_PARAMETER:
        break;
    default:
        return CM_GOAL_CANONICAL_INVALID;
    }
    *out_count = count;
    return CM_GOAL_CANONICAL_OK;
}

static CmTypeckTypeId cm_goal_named_child(const CmTypeckNamedType *named,
    size_t requested, size_t *cursor)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count; ++index) {
        const CmTypeckGenericArg *argument;

        argument = &named->arguments[index];
        if (argument->kind != CM_HIR_GENERIC_ARG_TYPE
            && argument->kind != CM_HIR_GENERIC_ARG_CONST) continue;
        if (*cursor == requested) {
            return argument->kind == CM_HIR_GENERIC_ARG_TYPE
                ? argument->data.type : argument->data.constant.type;
        }
        *cursor += 1u;
    }
    return CM_TYPECK_TYPE_NONE;
}

static CmGoalCanonicalStatus cm_goal_type_child_at(
    const CmTypeckType *type, size_t child_index,
    CmTypeckTypeId *out_child)
{
    size_t cursor;

    if (type == NULL || out_child == NULL) return CM_GOAL_CANONICAL_INVALID;
    *out_child = CM_TYPECK_TYPE_NONE;
    switch (type->kind) {
    case CM_TYPECK_TYPE_REFERENCE:
        *out_child = type->data.reference_type.pointee;
        break;
    case CM_TYPECK_TYPE_RAW_POINTER:
        *out_child = type->data.raw_pointer_type.pointee;
        break;
    case CM_TYPECK_TYPE_TUPLE:
        if (child_index < type->data.tuple_type.element_count) {
            *out_child = type->data.tuple_type.elements[child_index];
        }
        break;
    case CM_TYPECK_TYPE_ARRAY:
        *out_child = child_index == 0u ? type->data.array_type.element
            : child_index == 1u ? type->data.array_type.length.type
            : CM_TYPECK_TYPE_NONE;
        break;
    case CM_TYPECK_TYPE_SLICE:
        *out_child = type->data.slice_type.element;
        break;
    case CM_TYPECK_TYPE_FN_POINTER:
        if (child_index < type->data.fn_pointer_type.parameter_count) {
            *out_child = type->data.fn_pointer_type
                .parameters[child_index];
        } else if (child_index
                == type->data.fn_pointer_type.parameter_count) {
            *out_child = type->data.fn_pointer_type.return_type;
        }
        break;
    case CM_TYPECK_TYPE_ADT:
        cursor = 0u;
        *out_child = cm_goal_named_child(&type->data.named_type,
            child_index, &cursor);
        break;
    case CM_TYPECK_TYPE_PROJECTION:
        if (child_index == 0u) {
            *out_child = type->data.projection_type.self_type;
            break;
        }
        cursor = 1u;
        *out_child = cm_goal_named_child(
            &type->data.projection_type.trait_type, child_index,
            &cursor);
        if (*out_child == CM_TYPECK_TYPE_NONE) {
            *out_child = cm_goal_named_child(
                &type->data.projection_type.associated_type,
                child_index, &cursor);
        }
        break;
    default:
        break;
    }
    return *out_child == CM_TYPECK_TYPE_NONE
        ? CM_GOAL_CANONICAL_INVALID : CM_GOAL_CANONICAL_OK;
}

static CmGoalCanonicalStatus cm_goal_resolve_type(
    CmGoalCanonicalContext *canonical, CmTypeckTypeId type,
    CmTypeckTypeId *out_resolved)
{
    CmTypeckStatus status;

    status = cm_typeck_resolve(canonical->typeck, type, out_resolved);
    if (status == CM_TYPECK_OVERFLOW) return CM_GOAL_CANONICAL_OVERFLOW;
    if (status != CM_TYPECK_OK || *out_resolved == CM_TYPECK_TYPE_NONE
        || (size_t)*out_resolved > canonical->type_count
        || cm_typeck_get_type(canonical->typeck, *out_resolved) == NULL) {
        return CM_GOAL_CANONICAL_INVALID;
    }
    return CM_GOAL_CANONICAL_OK;
}

static CmGoalCanonicalStatus cm_goal_canonical_node_for_type(
    CmGoalCanonicalContext *canonical, CmTypeckTypeId type,
    size_t *out_node);

static CmGoalCanonicalStatus cm_goal_append_const(
    CmGoalCanonicalContext *canonical, CmVec *words,
    const CmTypeckConst *constant)
{
    const CmHirGenericParam *parameter;
    size_t type_node;
    CmGoalCanonicalStatus status;

    if (constant == NULL) return CM_GOAL_CANONICAL_INVALID;
    status = cm_goal_canonical_node_for_type(canonical, constant->type,
        &type_node);
    if (status != CM_GOAL_CANONICAL_OK) return status;
    cm_goal_push_word(words, (uint64_t)constant->kind);
    cm_goal_push_word(words, (uint64_t)type_node);
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE:
        cm_goal_push_word(words, constant->data.value.low_bits);
        cm_goal_push_word(words, constant->data.value.high_bits);
        return CM_GOAL_CANONICAL_OK;
    case CM_HIR_CONST_PARAMETER:
        parameter = cm_hir_get_generic_param(canonical->hir,
            constant->data.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_CONST) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        cm_goal_push_word(words, (uint64_t)constant->data.parameter);
        return CM_GOAL_CANONICAL_OK;
    case CM_HIR_CONST_INFER:
    case CM_HIR_CONST_UNEVALUATED:
    case CM_HIR_CONST_ERROR:
        /* CmTypeckConst does not retain their source identity yet. */
        return CM_GOAL_CANONICAL_UNCACHEABLE;
    }
    return CM_GOAL_CANONICAL_INVALID;
}

static CmGoalCanonicalStatus cm_goal_append_named(
    CmGoalCanonicalContext *canonical, CmVec *words,
    const CmTypeckNamedType *named)
{
    CmGoalCanonicalStatus status;
    uint32_t index;

    if (named == NULL
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return CM_GOAL_CANONICAL_INVALID;
    }
    cm_goal_push_def(words, named->definition);
    cm_goal_push_word(words, (uint64_t)named->argument_count);
    for (index = 0u; index < named->argument_count; ++index) {
        const CmTypeckGenericArg *argument;

        argument = &named->arguments[index];
        cm_goal_push_word(words, (uint64_t)argument->kind);
        if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            status = cm_goal_append_region(canonical, words,
                &argument->data.lifetime);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            size_t node;

            status = cm_goal_canonical_node_for_type(canonical,
                argument->data.type, &node);
            if (status == CM_GOAL_CANONICAL_OK) {
                cm_goal_push_word(words, (uint64_t)node);
            }
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            status = cm_goal_append_const(canonical, words,
                &argument->data.constant);
        } else {
            status = CM_GOAL_CANONICAL_INVALID;
        }
        if (status != CM_GOAL_CANONICAL_OK) return status;
    }
    return CM_GOAL_CANONICAL_OK;
}

static size_t cm_goal_child_node(CmGoalCanonicalContext *canonical,
    CmTypeckTypeId child)
{
    CmTypeckTypeId resolved;

    if (cm_goal_resolve_type(canonical, child, &resolved)
            != CM_GOAL_CANONICAL_OK) return 0u;
    return canonical->canonical_node[(size_t)resolved - 1u];
}

static CmGoalCanonicalStatus cm_goal_append_type_record(
    CmGoalCanonicalContext *canonical, CmTypeckTypeId resolved,
    size_t *out_height)
{
    const CmHirGenericParam *parameter;
    const CmTypeckType *type;
    CmGoalCanonicalStatus status;
    size_t child_count;
    size_t maximum_height;
    size_t child_index;
    uint32_t index;

    type = cm_typeck_get_type(canonical->typeck, resolved);
    if (type == NULL) return CM_GOAL_CANONICAL_INVALID;
    cm_vec_clear(&canonical->record);
    cm_goal_push_word(&canonical->record, (uint64_t)type->kind);
    status = cm_goal_type_child_count(type, &child_count);
    if (status != CM_GOAL_CANONICAL_OK) return status;
    maximum_height = 0u;
    for (child_index = 0u; child_index < child_count; ++child_index) {
        CmTypeckTypeId child;
        CmTypeckTypeId child_resolved;

        status = cm_goal_type_child_at(type, child_index, &child);
        if (status != CM_GOAL_CANONICAL_OK) return status;
        status = cm_goal_resolve_type(canonical, child, &child_resolved);
        if (status != CM_GOAL_CANONICAL_OK) return status;
        if (canonical->canonical_node[(size_t)child_resolved - 1u] == 0u) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        if (canonical->height[(size_t)child_resolved - 1u]
                > maximum_height) {
            maximum_height = canonical->height[
                (size_t)child_resolved - 1u];
        }
    }
    if (maximum_height >= canonical->max_depth) {
        return CM_GOAL_CANONICAL_OVERFLOW;
    }
    *out_height = maximum_height + 1u;
    switch (type->kind) {
    case CM_TYPECK_TYPE_VARIABLE:
        if (canonical->next_type_variable == UINT32_MAX) {
            return CM_GOAL_CANONICAL_OVERFLOW;
        }
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.variable.class_kind);
        cm_goal_push_word(&canonical->record,
            (uint64_t)canonical->next_type_variable++);
        break;
    case CM_TYPECK_TYPE_NEVER:
    case CM_TYPECK_TYPE_UNIT:
    case CM_TYPECK_TYPE_BOOL:
    case CM_TYPECK_TYPE_CHAR:
    case CM_TYPECK_TYPE_STR:
        break;
    case CM_TYPECK_TYPE_INTEGER:
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.integer_type);
        break;
    case CM_TYPECK_TYPE_FLOAT:
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.float_type);
        break;
    case CM_TYPECK_TYPE_REFERENCE:
        status = cm_goal_append_region(canonical, &canonical->record,
            &type->data.reference_type.region);
        if (status != CM_GOAL_CANONICAL_OK) return status;
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.reference_type.mutability);
        cm_goal_push_word(&canonical->record,
            (uint64_t)cm_goal_child_node(canonical,
                type->data.reference_type.pointee));
        break;
    case CM_TYPECK_TYPE_RAW_POINTER:
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.raw_pointer_type.mutability);
        cm_goal_push_word(&canonical->record,
            (uint64_t)cm_goal_child_node(canonical,
                type->data.raw_pointer_type.pointee));
        break;
    case CM_TYPECK_TYPE_TUPLE:
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.tuple_type.element_count);
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            cm_goal_push_word(&canonical->record,
                (uint64_t)cm_goal_child_node(canonical,
                    type->data.tuple_type.elements[index]));
        }
        break;
    case CM_TYPECK_TYPE_ARRAY:
        cm_goal_push_word(&canonical->record,
            (uint64_t)cm_goal_child_node(canonical,
                type->data.array_type.element));
        status = cm_goal_append_const(canonical, &canonical->record,
            &type->data.array_type.length);
        if (status != CM_GOAL_CANONICAL_OK) return status;
        break;
    case CM_TYPECK_TYPE_SLICE:
        cm_goal_push_word(&canonical->record,
            (uint64_t)cm_goal_child_node(canonical,
                type->data.slice_type.element));
        break;
    case CM_TYPECK_TYPE_FN_POINTER:
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.fn_pointer_type.parameter_count);
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            cm_goal_push_word(&canonical->record,
                (uint64_t)cm_goal_child_node(canonical,
                    type->data.fn_pointer_type.parameters[index]));
        }
        cm_goal_push_word(&canonical->record,
            (uint64_t)cm_goal_child_node(canonical,
                type->data.fn_pointer_type.return_type));
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.fn_pointer_type.abi);
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.fn_pointer_type.safety);
        cm_goal_push_word(&canonical->record,
            (uint64_t)(type->data.fn_pointer_type.is_variadic != 0));
        break;
    case CM_TYPECK_TYPE_ADT:
        status = cm_goal_append_named(canonical, &canonical->record,
            &type->data.named_type);
        if (status != CM_GOAL_CANONICAL_OK) return status;
        break;
    case CM_TYPECK_TYPE_PARAMETER:
        parameter = cm_hir_get_generic_param(canonical->hir,
            type->data.parameter_type.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE) {
            return CM_GOAL_CANONICAL_INVALID;
        }
        cm_goal_push_word(&canonical->record,
            (uint64_t)type->data.parameter_type.parameter);
        break;
    case CM_TYPECK_TYPE_PROJECTION:
        cm_goal_push_word(&canonical->record,
            (uint64_t)cm_goal_child_node(canonical,
                type->data.projection_type.self_type));
        status = cm_goal_append_named(canonical, &canonical->record,
            &type->data.projection_type.trait_type);
        if (status == CM_GOAL_CANONICAL_OK) {
            status = cm_goal_append_named(canonical, &canonical->record,
                &type->data.projection_type.associated_type);
        }
        if (status != CM_GOAL_CANONICAL_OK) return status;
        break;
    default:
        return CM_GOAL_CANONICAL_INVALID;
    }
    return CM_GOAL_CANONICAL_OK;
}

static size_t cm_goal_intern_record(CmGoalCanonicalContext *canonical,
    size_t height)
{
    size_t node_index;
    CmGoalCanonicalNode node;

    for (node_index = 0u; node_index < canonical->nodes.len;
         ++node_index) {
        const CmGoalCanonicalNode *existing;

        existing = (const CmGoalCanonicalNode *)cm_vec_at_const(
            &canonical->nodes, node_index);
        if (existing->word_count == canonical->record.len
            && memcmp(canonical->node_words.data
                    + existing->word_start * sizeof(uint64_t),
                canonical->record.data,
                existing->word_count * sizeof(uint64_t)) == 0) {
            return node_index + 1u;
        }
    }
    node.word_start = canonical->node_words.len;
    node.word_count = canonical->record.len;
    node.height = height;
    cm_vec_append(&canonical->node_words, canonical->record.data,
        canonical->record.len);
    (void)cm_vec_push(&canonical->nodes, &node);
    return canonical->nodes.len;
}

static CmGoalCanonicalStatus cm_goal_canonical_node_for_type(
    CmGoalCanonicalContext *canonical, CmTypeckTypeId type,
    size_t *out_node)
{
    CmGoalCanonicalStatus status;
    CmTypeckTypeId resolved;
    CmGoalTypeFrame frame;

    status = cm_goal_resolve_type(canonical, type, &resolved);
    if (status != CM_GOAL_CANONICAL_OK) return status;
    if (canonical->visit_state[(size_t)resolved - 1u] == 2u) {
        *out_node = canonical->canonical_node[(size_t)resolved - 1u];
        return CM_GOAL_CANONICAL_OK;
    }
    if (canonical->visit_state[(size_t)resolved - 1u] == 1u) {
        return CM_GOAL_CANONICAL_INVALID;
    }
    if (canonical->reachable_count >= canonical->max_nodes) {
        return CM_GOAL_CANONICAL_OVERFLOW;
    }
    status = cm_goal_type_child_count(
        cm_typeck_get_type(canonical->typeck, resolved),
        &frame.child_count);
    if (status != CM_GOAL_CANONICAL_OK) return status;
    frame.type = resolved;
    frame.next_child = 0u;
    canonical->visit_state[(size_t)resolved - 1u] = 1u;
    canonical->reachable_count += 1u;
    (void)cm_vec_push(&canonical->frames, &frame);
    while (canonical->frames.len != 0u) {
        CmGoalTypeFrame *top;

        top = (CmGoalTypeFrame *)cm_vec_at(&canonical->frames,
            canonical->frames.len - 1u);
        if (top->next_child < top->child_count) {
            CmTypeckTypeId child;
            CmTypeckTypeId child_resolved;

            status = cm_goal_type_child_at(
                cm_typeck_get_type(canonical->typeck, top->type),
                top->next_child++, &child);
            if (status != CM_GOAL_CANONICAL_OK) return status;
            status = cm_goal_resolve_type(canonical, child,
                &child_resolved);
            if (status != CM_GOAL_CANONICAL_OK) return status;
            if (canonical->visit_state[(size_t)child_resolved - 1u]
                    == 1u) {
                return CM_GOAL_CANONICAL_INVALID;
            }
            if (canonical->visit_state[(size_t)child_resolved - 1u]
                    == 2u) continue;
            if (canonical->frames.len >= canonical->max_depth
                || canonical->reachable_count >= canonical->max_nodes) {
                return CM_GOAL_CANONICAL_OVERFLOW;
            }
            status = cm_goal_type_child_count(
                cm_typeck_get_type(canonical->typeck, child_resolved),
                &frame.child_count);
            if (status != CM_GOAL_CANONICAL_OK) return status;
            frame.type = child_resolved;
            frame.next_child = 0u;
            canonical->visit_state[(size_t)child_resolved - 1u] = 1u;
            canonical->reachable_count += 1u;
            (void)cm_vec_push(&canonical->frames, &frame);
            continue;
        }
        {
            size_t height;
            size_t node;
            CmTypeckTypeId completed;

            completed = top->type;
            status = cm_goal_append_type_record(canonical, completed,
                &height);
            if (status != CM_GOAL_CANONICAL_OK) return status;
            node = cm_goal_intern_record(canonical, height);
            canonical->canonical_node[(size_t)completed - 1u] = node;
            canonical->height[(size_t)completed - 1u] = height;
            canonical->visit_state[(size_t)completed - 1u] = 2u;
            canonical->frames.len -= 1u;
        }
    }
    *out_node = canonical->canonical_node[(size_t)resolved - 1u];
    return *out_node == 0u ? CM_GOAL_CANONICAL_INVALID
        : CM_GOAL_CANONICAL_OK;
}

static CmGoalCanonicalStatus cm_goal_visit_named_types(
    CmGoalCanonicalContext *canonical, const CmTypeckNamedType *named)
{
    uint32_t index;

    if (named == NULL
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return CM_GOAL_CANONICAL_INVALID;
    }
    for (index = 0u; index < named->argument_count; ++index) {
        const CmTypeckGenericArg *argument;
        CmGoalCanonicalStatus status;
        size_t ignored_node;

        argument = &named->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            status = cm_goal_canonical_node_for_type(canonical,
                argument->data.type, &ignored_node);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            status = cm_goal_canonical_node_for_type(canonical,
                argument->data.constant.type, &ignored_node);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            status = CM_GOAL_CANONICAL_OK;
        } else {
            status = CM_GOAL_CANONICAL_INVALID;
        }
        if (status != CM_GOAL_CANONICAL_OK) return status;
    }
    return CM_GOAL_CANONICAL_OK;
}

static CmGoalCanonicalStatus cm_goal_visit_instantiation_types(
    CmGoalCanonicalContext *canonical,
    const CmTypeckInstantiation *instantiation)
{
    uint32_t index;
    CmGoalCanonicalStatus status;
    size_t ignored_node;

    for (index = 0u; index < instantiation->argument_count; ++index) {
        const CmTypeckGenericArg *argument;

        argument = &instantiation->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            status = cm_goal_canonical_node_for_type(canonical,
                argument->data.type, &ignored_node);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            status = cm_goal_canonical_node_for_type(canonical,
                argument->data.constant.type, &ignored_node);
        } else {
            status = argument->kind == CM_HIR_GENERIC_ARG_LIFETIME
                ? CM_GOAL_CANONICAL_OK : CM_GOAL_CANONICAL_INVALID;
        }
        if (status != CM_GOAL_CANONICAL_OK) return status;
    }
    if (instantiation->self_type != CM_TYPECK_TYPE_NONE) {
        return cm_goal_canonical_node_for_type(canonical,
            instantiation->self_type, &ignored_node);
    }
    return CM_GOAL_CANONICAL_OK;
}

static CmGoalCanonicalStatus cm_goal_append_instantiation(
    CmGoalCanonicalContext *canonical, CmVec *words,
    const CmTypeckInstantiation *instantiation)
{
    CmGoalCanonicalStatus status;
    uint32_t index;

    cm_goal_push_def(words, instantiation->parameter_owner);
    cm_goal_push_word(words, (uint64_t)instantiation->argument_count);
    for (index = 0u; index < instantiation->argument_count; ++index) {
        const CmTypeckGenericArg *argument;

        argument = &instantiation->arguments[index];
        cm_goal_push_word(words, (uint64_t)argument->kind);
        if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            status = cm_goal_append_region(canonical, words,
                &argument->data.lifetime);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            size_t node;

            status = cm_goal_canonical_node_for_type(canonical,
                argument->data.type, &node);
            if (status == CM_GOAL_CANONICAL_OK) {
                cm_goal_push_word(words, (uint64_t)node);
            }
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            status = cm_goal_append_const(canonical, words,
                &argument->data.constant);
        } else {
            status = CM_GOAL_CANONICAL_INVALID;
        }
        if (status != CM_GOAL_CANONICAL_OK) return status;
    }
    cm_goal_push_def(words, instantiation->self_owner);
    cm_goal_push_word(words,
        instantiation->self_type == CM_TYPECK_TYPE_NONE ? 0u : 1u);
    if (instantiation->self_type != CM_TYPECK_TYPE_NONE) {
        size_t node;

        status = cm_goal_canonical_node_for_type(canonical,
            instantiation->self_type, &node);
        if (status != CM_GOAL_CANONICAL_OK) return status;
        cm_goal_push_word(words, (uint64_t)node);
    }
    return CM_GOAL_CANONICAL_OK;
}

static void cm_goal_canonical_context_destroy(
    CmGoalCanonicalContext *canonical)
{
    cm_vec_destroy(&canonical->region_variables);
    cm_vec_destroy(&canonical->frames);
    cm_vec_destroy(&canonical->record);
    cm_vec_destroy(&canonical->node_words);
    cm_vec_destroy(&canonical->nodes);
    cm_free(canonical->height);
    cm_free(canonical->canonical_node);
    cm_free(canonical->visit_state);
    memset(canonical, 0, sizeof(*canonical));
}

static CmGoalCanonicalStatus cm_goal_make_key(
    const CmTraitGoalTableState *table, const CmTypeckContext *typeck,
    const CmParamEnvSubstitution *substitution, const CmTraitGoal *goal,
    CmVec *out_key)
{
    CmGoalCanonicalContext canonical;
    const CmImplementedTraitGoal *implemented;
    const CmProjectionEqualityGoal *projection;
    CmGoalCanonicalStatus status;
    CmHirDefId owner;
    CmTypeckTypeId first_type;
    CmTypeckTypeId second_type;
    const CmTypeckNamedType *named;
    size_t first_node;
    size_t second_node;
    size_t node_index;

    if (table == NULL || typeck == NULL || substitution == NULL
        || substitution->exact == NULL || goal == NULL
        || cm_typeck_hir_context(typeck) != table->hir) {
        return CM_GOAL_CANONICAL_INVALID;
    }
    implemented = NULL;
    projection = NULL;
    named = NULL;
    second_type = CM_TYPECK_TYPE_NONE;
    second_node = 0u;
    if (goal->kind == CM_TRAIT_GOAL_IMPLEMENTED) {
        implemented = &goal->data.implemented;
        owner = implemented->owner;
        first_type = implemented->self_type;
        named = &implemented->trait_type;
    } else if (goal->kind == CM_TRAIT_GOAL_PROJECTION_EQUALITY) {
        projection = &goal->data.projection_equality;
        owner = projection->owner;
        first_type = projection->projection_type;
        second_type = projection->expected_type;
    } else {
        return CM_GOAL_CANONICAL_INVALID;
    }
    if (!cm_hir_def_id_equal(owner, table->exact_owner)
        || !cm_typeck_instantiation_is_valid(typeck,
            substitution->exact)
        || !cm_hir_def_id_equal(substitution->exact->parameter_owner,
            owner)) {
        return CM_GOAL_CANONICAL_INVALID;
    }
    if (!cm_hir_def_id_is_none(table->enclosing_owner)) {
        if (substitution->enclosing == NULL
            || !cm_typeck_instantiation_is_valid(typeck,
                substitution->enclosing)
            || !cm_hir_def_id_equal(
                substitution->enclosing->parameter_owner,
                table->enclosing_owner)) {
            return CM_GOAL_CANONICAL_INVALID;
        }
    } else if (substitution->enclosing != NULL) {
        return CM_GOAL_CANONICAL_INVALID;
    }
    memset(&canonical, 0, sizeof(canonical));
    canonical.typeck = typeck;
    canonical.hir = table->hir;
    canonical.binder = goal->binder;
    canonical.max_depth = table->limits.max_goal_depth;
    canonical.max_nodes = table->limits.max_canonical_nodes;
    canonical.type_count = cm_typeck_type_count(typeck);
    if (canonical.type_count == 0u) return CM_GOAL_CANONICAL_INVALID;
    canonical.visit_state = (unsigned char *)cm_alloc_zeroed(
        canonical.type_count, sizeof(unsigned char));
    canonical.canonical_node = (size_t *)cm_alloc_zeroed(
        canonical.type_count, sizeof(size_t));
    canonical.height = (size_t *)cm_alloc_zeroed(canonical.type_count,
        sizeof(size_t));
    cm_vec_init(&canonical.nodes, sizeof(CmGoalCanonicalNode));
    cm_vec_init(&canonical.node_words, sizeof(uint64_t));
    cm_vec_init(&canonical.record, sizeof(uint64_t));
    cm_vec_init(&canonical.frames, sizeof(CmGoalTypeFrame));
    cm_vec_init(&canonical.region_variables,
        sizeof(CmGoalRegionVariable));

    status = cm_goal_canonical_node_for_type(&canonical,
        first_type, &first_node);
    if (status == CM_GOAL_CANONICAL_OK && named != NULL) {
        status = cm_goal_visit_named_types(&canonical,
            named);
    } else if (status == CM_GOAL_CANONICAL_OK) {
        status = cm_goal_canonical_node_for_type(&canonical,
            second_type, &second_node);
    }
    if (status == CM_GOAL_CANONICAL_OK) {
        status = cm_goal_visit_instantiation_types(&canonical,
            substitution->exact);
    }
    if (status == CM_GOAL_CANONICAL_OK
        && substitution->enclosing != NULL) {
        status = cm_goal_visit_instantiation_types(&canonical,
            substitution->enclosing);
    }
    if (status != CM_GOAL_CANONICAL_OK) {
        cm_goal_canonical_context_destroy(&canonical);
        return status;
    }

    cm_vec_clear(out_key);
    cm_goal_push_word(out_key, CM_GOAL_KEY_VERSION);
    cm_goal_push_word(out_key, table->hir_storage_lifetime_id);
    cm_goal_push_word(out_key, table->hir_semantic_generation);
    cm_goal_push_word(out_key, table->hir_rewind_generation);
    cm_goal_push_word(out_key, (uint64_t)table->hir_crate_count);
    cm_goal_push_word(out_key, (uint64_t)table->hir_module_count);
    cm_goal_push_word(out_key, (uint64_t)table->hir_item_count);
    cm_goal_push_word(out_key, (uint64_t)table->hir_type_count);
    cm_goal_push_word(out_key,
        (uint64_t)table->hir_generic_parameter_count);
    cm_goal_push_word(out_key, (uint64_t)table->hir_definition_count);
    cm_goal_push_word(out_key, (uint64_t)table->universe);
    cm_goal_push_word(out_key, (uint64_t)table->local_crate);
    cm_goal_push_def(out_key, table->exact_owner);
    cm_goal_push_def(out_key, table->enclosing_owner);
    cm_goal_push_word(out_key, (uint64_t)table->fact_count);
    cm_goal_push_word(out_key, (uint64_t)table->pending_count);
    cm_goal_push_word(out_key, (uint64_t)goal->kind);
    cm_goal_push_word(out_key, (uint64_t)goal->binder.universe);
    cm_goal_push_word(out_key, (uint64_t)goal->binder.debruijn_depth);
    cm_goal_push_word(out_key, (uint64_t)goal->binder.lifetime_count);
    cm_goal_push_def(out_key, owner);
    cm_goal_push_word(out_key, (uint64_t)first_node);
    if (named != NULL) {
        status = cm_goal_append_named(&canonical, out_key, named);
    } else {
        cm_goal_push_word(out_key, (uint64_t)second_node);
        status = CM_GOAL_CANONICAL_OK;
    }
    if (status == CM_GOAL_CANONICAL_OK) {
        status = cm_goal_append_instantiation(&canonical, out_key,
            substitution->exact);
    }
    cm_goal_push_word(out_key,
        substitution->enclosing == NULL ? 0u : 1u);
    if (status == CM_GOAL_CANONICAL_OK
        && substitution->enclosing != NULL) {
        status = cm_goal_append_instantiation(&canonical, out_key,
            substitution->enclosing);
    }
    if (status == CM_GOAL_CANONICAL_OK) {
        cm_goal_push_word(out_key,
            (uint64_t)canonical.next_type_variable);
        cm_goal_push_word(out_key,
            (uint64_t)canonical.next_region_variable);
        cm_goal_push_word(out_key, (uint64_t)canonical.nodes.len);
        for (node_index = 0u; node_index < canonical.nodes.len;
             ++node_index) {
            const CmGoalCanonicalNode *node;

            node = (const CmGoalCanonicalNode *)cm_vec_at_const(
                &canonical.nodes, node_index);
            cm_goal_push_word(out_key, (uint64_t)node->word_count);
            cm_vec_append(out_key,
                (const uint64_t *)canonical.node_words.data
                    + node->word_start,
                node->word_count);
        }
    }
    cm_goal_canonical_context_destroy(&canonical);
    if (status != CM_GOAL_CANONICAL_OK) cm_vec_clear(out_key);
    return status;
}

static int cm_goal_key_equal(const CmVec *left, const CmVec *right)
{
    return left->len == right->len
        && (left->len == 0u || memcmp(left->data, right->data,
            left->len * sizeof(uint64_t)) == 0);
}

static size_t cm_goal_find_entry_index(CmTraitGoalTableState *state,
    const CmVec *key)
{
    size_t index;

    for (index = 0u; index < state->entries.len; ++index) {
        CmGoalTableEntry *entry;

        entry = (CmGoalTableEntry *)cm_vec_at(&state->entries, index);
        if (entry->state != CM_GOAL_ENTRY_VACANT
            && cm_goal_key_equal(&entry->key, key)) return index;
    }
    return (size_t)-1;
}

static CmGoalTableEntry *cm_goal_entry(CmTraitGoalTableState *state,
    size_t index)
{
    return index == (size_t)-1 ? NULL
        : (CmGoalTableEntry *)cm_vec_at(&state->entries, index);
}

static size_t cm_goal_active_entry_count(
    const CmTraitGoalTableState *state)
{
    size_t count;
    size_t index;

    count = 0u;
    for (index = 0u; index < state->entries.len; ++index) {
        const CmGoalTableEntry *entry;

        entry = (const CmGoalTableEntry *)cm_vec_at_const(&state->entries,
            index);
        if (entry->state != CM_GOAL_ENTRY_VACANT) count += 1u;
    }
    return count;
}

static size_t cm_goal_add_evaluating_entry(
    CmTraitGoalTableState *state, CmVec *key)
{
    size_t index;
    CmGoalTableEntry *entry;
    CmGoalTableEntry new_entry;

    for (index = 0u; index < state->entries.len; ++index) {
        entry = (CmGoalTableEntry *)cm_vec_at(&state->entries, index);
        if (entry->state == CM_GOAL_ENTRY_VACANT) {
            entry->state = CM_GOAL_ENTRY_EVALUATING;
            entry->cycle_tainted = 0;
            entry->key = *key;
            key->data = NULL;
            key->len = key->cap = 0u;
            return index;
        }
    }
    memset(&new_entry, 0, sizeof(new_entry));
    new_entry.state = CM_GOAL_ENTRY_EVALUATING;
    new_entry.key = *key;
    key->data = NULL;
    key->len = key->cap = 0u;
    (void)cm_vec_push(&state->entries, &new_entry);
    return state->entries.len - 1u;
}

static void cm_goal_taint_active_entries(CmTraitGoalTableState *state)
{
    size_t index;

    for (index = 0u; index < state->entries.len; ++index) {
        CmGoalTableEntry *entry;

        entry = (CmGoalTableEntry *)cm_vec_at(&state->entries, index);
        if (entry->state == CM_GOAL_ENTRY_EVALUATING) {
            entry->cycle_tainted = 1;
        }
    }
}

static void cm_goal_vacate_entry(CmGoalTableEntry *entry)
{
    cm_vec_destroy(&entry->key);
    memset(entry, 0, sizeof(*entry));
}

static int cm_goal_result_cacheable(CmTraitSolverResultKind kind)
{
    return kind == CM_TRAIT_SOLVER_NEGATIVE
        || kind == CM_TRAIT_SOLVER_NO_SOLUTION
        || kind == CM_TRAIT_SOLVER_AMBIGUOUS
        || kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE
        || kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        || kind == CM_TRAIT_SOLVER_UNSUPPORTED;
}

CmTraitSolverResultKind cm_trait_goal_table_init(CmTraitGoalTable *table,
    const CmTraitImplIndex *index, const CmParamEnv *environment,
    CmTraitGoalTableLimits limits)
{
    CmTraitGoalTableState *state;
    const CmHirContext *hir;

    if (table == NULL || table->state != NULL
        || !cm_trait_impl_index_is_current(index)
        || !cm_param_env_is_current(environment)) {
        return CM_TRAIT_SOLVER_INVALID;
    }
    hir = cm_trait_impl_index_hir(index);
    if (hir == NULL || cm_param_env_hir(environment) != hir) {
        return CM_TRAIT_SOLVER_INVALID;
    }
    if (limits.max_goal_depth == 0u) {
        limits.max_goal_depth = CM_GOAL_TABLE_DEFAULT_DEPTH;
    }
    if (limits.max_canonical_nodes == 0u) {
        limits.max_canonical_nodes = CM_GOAL_TABLE_DEFAULT_NODES;
    }
    if (limits.max_table_entries == 0u) {
        limits.max_table_entries = CM_GOAL_TABLE_DEFAULT_ENTRIES;
    }
    state = (CmTraitGoalTableState *)cm_alloc_zeroed(1u,
        sizeof(CmTraitGoalTableState));
    state->index = index;
    state->environment = environment;
    state->hir = hir;
    state->universe = cm_trait_impl_index_universe(index);
    state->local_crate = cm_trait_impl_index_local_crate(index);
    state->hir_storage_lifetime_id = hir->storage.lifetime_id;
    state->hir_semantic_generation = hir->semantic_generation;
    state->hir_rewind_generation = hir->rewind_generation;
    state->hir_crate_count = hir->crates.len;
    state->hir_module_count = hir->modules.len;
    state->hir_item_count = hir->items.len;
    state->hir_body_count = hir->bodies.len;
    state->hir_expression_count = hir->expressions.len;
    state->hir_type_count = hir->types.len;
    state->hir_generic_parameter_count = hir->generic_parameters.len;
    state->hir_definition_count = hir->definitions.len;
    state->hir_prebound_associated_type_count =
        hir->prebound_associated_types.len;
    state->exact_owner = cm_param_env_exact_owner(environment);
    state->enclosing_owner = cm_param_env_enclosing_owner(environment);
    state->fact_count = cm_param_env_fact_count(environment);
    state->pending_count = cm_param_env_pending_count(environment);
    state->limits = limits;
    cm_vec_init(&state->entries, sizeof(CmGoalTableEntry));
    table->state = state;
    return CM_TRAIT_SOLVER_PROVEN;
}

void cm_trait_goal_table_destroy(CmTraitGoalTable *table)
{
    CmTraitGoalTableState *state;
    size_t index;

    state = cm_goal_table_state(table);
    if (state == NULL) return;
    for (index = 0u; index < state->entries.len; ++index) {
        CmGoalTableEntry *entry;

        entry = (CmGoalTableEntry *)cm_vec_at(&state->entries, index);
        if (entry->key.elem_size != 0u) cm_vec_destroy(&entry->key);
    }
    cm_vec_destroy(&state->entries);
    memset(state, 0, sizeof(*state));
    cm_free(state);
    table->state = NULL;
}

int cm_trait_goal_table_is_current(const CmTraitGoalTable *table)
{
    return cm_goal_table_state_is_current(cm_goal_table_state_const(table));
}

size_t cm_trait_goal_table_entry_count(const CmTraitGoalTable *table)
{
    const CmTraitGoalTableState *state;

    state = cm_goal_table_state_const(table);
    return !cm_goal_table_state_is_current(state) ? 0u
        : cm_goal_active_entry_count(state);
}

size_t cm_trait_goal_table_cache_hit_count(const CmTraitGoalTable *table)
{
    const CmTraitGoalTableState *state;

    state = cm_goal_table_state_const(table);
    return !cm_goal_table_state_is_current(state) ? 0u
        : state->cache_hit_count;
}

typedef struct CmGoalRecursiveEvaluatorContext {
    CmTraitGoalTable *table;
    const CmParamEnvSubstitution *substitution;
} CmGoalRecursiveEvaluatorContext;

static CmTraitSelectionResult cm_goal_evaluate_implemented(void *context,
    CmTypeckContext *typeck, const CmImplementedTraitGoal *implemented)
{
    CmGoalRecursiveEvaluatorContext *recursive;
    CmTraitGoal goal;

    recursive = (CmGoalRecursiveEvaluatorContext *)context;
    if (recursive == NULL || recursive->table == NULL
        || recursive->substitution == NULL || implemented == NULL) {
        return cm_goal_result(CM_TRAIT_SOLVER_INVALID);
    }
    memset(&goal, 0, sizeof(goal));
    goal.kind = CM_TRAIT_GOAL_IMPLEMENTED;
    goal.data.implemented = *implemented;
    return cm_trait_goal_table_solve(recursive->table, typeck,
        recursive->substitution, &goal);
}

static CmTraitSelectionResult cm_goal_evaluate_projection(void *context,
    CmTypeckContext *typeck, const CmProjectionEqualityGoal *projection)
{
    CmGoalRecursiveEvaluatorContext *recursive;
    CmTraitGoal goal;

    recursive = (CmGoalRecursiveEvaluatorContext *)context;
    if (recursive == NULL || recursive->table == NULL
        || recursive->substitution == NULL || projection == NULL) {
        return cm_goal_result(CM_TRAIT_SOLVER_INVALID);
    }
    memset(&goal, 0, sizeof(goal));
    goal.kind = CM_TRAIT_GOAL_PROJECTION_EQUALITY;
    goal.data.projection_equality = *projection;
    return cm_trait_goal_table_solve(recursive->table, typeck,
        recursive->substitution, &goal);
}

CmTraitSelectionResult cm_trait_goal_table_solve_with_impl_witness(
    CmTraitGoalTable *table,
    CmTypeckContext *typeck,
    const CmParamEnvSubstitution *substitution,
    const CmTraitGoal *goal, CmTraitImplSelectionWitness *witness)
{
    CmTraitGoalTableState *state;
    CmGoalCanonicalStatus canonical_status;
    CmGoalTableEntry *entry;
    CmTraitSelectionResult result;
    CmGoalRecursiveEvaluatorContext recursive;
    CmTraitGoalEvaluator evaluator;
    CmTypeckSnapshot snapshot;
    CmTypeckStatus typeck_status;
    CmVec key;
    size_t entry_index;
    int may_cache;

    result = cm_goal_result(CM_TRAIT_SOLVER_INVALID);
    if (witness != NULL) {
        cm_trait_impl_selection_witness_clear(witness);
        if (goal == NULL || goal->kind != CM_TRAIT_GOAL_IMPLEMENTED) {
            return result;
        }
    }
    state = cm_goal_table_state(table);
    if (!cm_goal_table_state_is_current(state)) return result;
    cm_vec_init(&key, sizeof(uint64_t));
    canonical_status = cm_goal_make_key(state, typeck, substitution, goal,
        &key);
    if (canonical_status == CM_GOAL_CANONICAL_OVERFLOW) {
        cm_vec_destroy(&key);
        return cm_goal_result(CM_TRAIT_SOLVER_OVERFLOW);
    }
    if (canonical_status == CM_GOAL_CANONICAL_INVALID) {
        cm_vec_destroy(&key);
        return result;
    }
    may_cache = canonical_status == CM_GOAL_CANONICAL_OK;
    entry_index = may_cache
        ? cm_goal_find_entry_index(state, &key) : (size_t)-1;
    entry = cm_goal_entry(state, entry_index);
    if (entry != NULL) {
        cm_vec_destroy(&key);
        if (entry->state == CM_GOAL_ENTRY_EVALUATING) {
            cm_goal_taint_active_entries(state);
            return cm_goal_result(CM_TRAIT_SOLVER_AMBIGUOUS);
        }
        state->cache_hit_count += 1u;
        return entry->result;
    }
    if (may_cache
        && cm_goal_active_entry_count(state)
            >= state->limits.max_table_entries) {
        cm_vec_destroy(&key);
        return cm_goal_result(CM_TRAIT_SOLVER_OVERFLOW);
    }
    entry_index = may_cache
        ? cm_goal_add_evaluating_entry(state, &key) : (size_t)-1;
    cm_vec_destroy(&key);

    typeck_status = cm_typeck_snapshot(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        entry = cm_goal_entry(state, entry_index);
        if (entry != NULL) cm_goal_vacate_entry(entry);
        result = cm_goal_result(CM_TRAIT_SOLVER_TYPECK_FAILURE);
        result.typeck_status = typeck_status;
        return result;
    }
    if (goal->kind == CM_TRAIT_GOAL_IMPLEMENTED) {
        recursive.table = table;
        recursive.substitution = substitution;
        memset(&evaluator, 0, sizeof(evaluator));
        evaluator.context = &recursive;
        evaluator.evaluate = cm_goal_evaluate_implemented;
        evaluator.evaluate_projection = cm_goal_evaluate_projection;
        result =
            cm_trait_solver_solve_implemented_with_evaluator_and_witness(
            state->index, state->environment, typeck, substitution,
            &goal->data.implemented, &evaluator, witness);
    } else if (goal->kind == CM_TRAIT_GOAL_PROJECTION_EQUALITY) {
        recursive.table = table;
        recursive.substitution = substitution;
        memset(&evaluator, 0, sizeof(evaluator));
        evaluator.context = &recursive;
        evaluator.evaluate = cm_goal_evaluate_implemented;
        evaluator.evaluate_projection = cm_goal_evaluate_projection;
        result = cm_trait_solver_solve_projection_equality(state->index,
            state->environment, typeck, substitution,
            &goal->data.projection_equality, &evaluator);
    } else {
        result = cm_goal_result(CM_TRAIT_SOLVER_INVALID);
    }
    if (result.kind == CM_TRAIT_SOLVER_PROVEN) {
        typeck_status = cm_typeck_commit(typeck, &snapshot);
        entry = cm_goal_entry(state, entry_index);
        if (entry != NULL) cm_goal_vacate_entry(entry);
        if (typeck_status != CM_TYPECK_OK) {
            if (witness != NULL) {
                cm_trait_impl_selection_witness_clear(witness);
            }
            result = cm_goal_result(CM_TRAIT_SOLVER_TYPECK_FAILURE);
            result.typeck_status = typeck_status;
        }
        return result;
    }
    if (witness != NULL) cm_trait_impl_selection_witness_clear(witness);
    typeck_status = cm_typeck_rollback(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        entry = cm_goal_entry(state, entry_index);
        if (entry != NULL) cm_goal_vacate_entry(entry);
        result = cm_goal_result(CM_TRAIT_SOLVER_TYPECK_FAILURE);
        result.typeck_status = typeck_status;
        return result;
    }
    entry = cm_goal_entry(state, entry_index);
    if (entry != NULL && !entry->cycle_tainted
        && cm_goal_result_cacheable(result.kind)) {
        /* A non-proof never retains a candidate identity for later replay. */
        result.impl_definition = cm_hir_def_id_none();
        result.impl_item = CM_HIR_ITEM_NONE;
        result.impl_associated_definition = cm_hir_def_id_none();
        result.proof_origin = CM_TRAIT_PROOF_NONE;
        result.param_env_fact_index = CM_TRAIT_PROOF_FACT_NONE;
        result.param_env_equality_index = CM_TRAIT_PROOF_EQUALITY_NONE;
        entry->result = result;
        entry->state = CM_GOAL_ENTRY_COMPLETE;
    } else if (entry != NULL) {
        cm_goal_vacate_entry(entry);
    }
    return result;
}

CmTraitSelectionResult cm_trait_goal_table_solve(CmTraitGoalTable *table,
    CmTypeckContext *typeck,
    const CmParamEnvSubstitution *substitution,
    const CmTraitGoal *goal)
{
    return cm_trait_goal_table_solve_with_impl_witness(table, typeck,
        substitution, goal, NULL);
}
