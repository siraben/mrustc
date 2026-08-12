#include "cm/hir/param_env.h"

#include "cm/alloc.h"
#include "cm/vec.h"

#include <string.h>

#define CM_PARAM_ENV_MAX_RECURSION 256u

enum {
    CM_PARAM_DEP_EXACT = 1u << 0,
    CM_PARAM_DEP_ENCLOSING = 1u << 1,
    CM_PARAM_DEP_FOREIGN = 1u << 2,
    CM_PARAM_DEP_SELF_EXACT = 1u << 3,
    CM_PARAM_DEP_SELF_ENCLOSING = 1u << 4,
    CM_PARAM_DEP_SELF_FOREIGN = 1u << 5,
    CM_PARAM_DEP_LATE_BOUND = 1u << 6,
    CM_PARAM_DEP_PROJECTION = 1u << 7,
    CM_PARAM_DEP_OVERFLOW = 1u << 8
};

typedef struct CmParamEnvState {
    const CmHirContext *hir;
    CmHirDefId exact_owner;
    CmHirDefId enclosing_owner;
    CmHirDefId semantic_owner;
    CmVec facts;
    CmVec pending;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    size_t crate_count;
    size_t module_count;
    size_t item_count;
    size_t body_count;
    size_t expression_count;
    size_t type_count;
    size_t generic_parameter_count;
    size_t definition_count;
    size_t prebound_associated_type_count;
} CmParamEnvState;

typedef struct CmParamDependencyScan {
    const CmHirContext *hir;
    CmHirDefId exact_owner;
    CmHirDefId enclosing_owner;
} CmParamDependencyScan;

static CmParamEnvState *cm_param_env_state(CmParamEnv *environment)
{
    return environment == NULL ? NULL : (CmParamEnvState *)environment->state;
}

static const CmParamEnvState *cm_param_env_state_const(
    const CmParamEnv *environment)
{
    return environment == NULL ? NULL
        : (const CmParamEnvState *)environment->state;
}

static int cm_param_env_state_current(const CmParamEnvState *state)
{
    const CmHirContext *hir;

    if (state == NULL || state->hir == NULL) return 0;
    hir = state->hir;
    return hir->storage.lifetime_id == state->storage_lifetime_id
        && hir->semantic_generation == state->semantic_generation
        && hir->rewind_generation == state->rewind_generation
        && hir->crates.len == state->crate_count
        && hir->modules.len == state->module_count
        && hir->items.len == state->item_count
        && hir->bodies.len == state->body_count
        && hir->expressions.len == state->expression_count
        && hir->types.len == state->type_count
        && hir->generic_parameters.len == state->generic_parameter_count
        && hir->definitions.len == state->definition_count
        && hir->prebound_associated_types.len
            == state->prebound_associated_type_count;
}

static const CmHirItem *cm_param_env_find_item(const CmHirContext *hir,
    CmHirDefId definition)
{
    const CmHirDefinition *entry;

    entry = cm_hir_lookup_definition(hir, definition);
    if (entry == NULL || entry->kind != CM_HIR_DEFINITION_ITEM
        || entry->state != CM_HIR_DEFINITION_BOUND
        || entry->entity.item_id == CM_HIR_ITEM_NONE) return NULL;
    return cm_hir_get_item(hir, entry->entity.item_id);
}

static unsigned int cm_param_dependency_owner(
    const CmParamDependencyScan *scan, CmHirDefId owner, int is_self)
{
    if (cm_hir_def_id_equal(owner, scan->exact_owner)) {
        return is_self ? CM_PARAM_DEP_SELF_EXACT : CM_PARAM_DEP_EXACT;
    }
    if (!cm_hir_def_id_is_none(scan->enclosing_owner)
        && cm_hir_def_id_equal(owner, scan->enclosing_owner)) {
        return is_self ? CM_PARAM_DEP_SELF_ENCLOSING
            : CM_PARAM_DEP_ENCLOSING;
    }
    return is_self ? CM_PARAM_DEP_SELF_FOREIGN : CM_PARAM_DEP_FOREIGN;
}

static unsigned int cm_param_dependency_type(
    const CmParamDependencyScan *scan, CmHirTypeId type_id, size_t depth);

static unsigned int cm_param_dependency_region(
    const CmParamDependencyScan *scan, const CmHirRegion *region)
{
    const CmHirGenericParam *parameter;

    if (region == NULL) return CM_PARAM_DEP_FOREIGN;
    if (region->kind == CM_HIR_REGION_LATE_BOUND) {
        return CM_PARAM_DEP_LATE_BOUND;
    }
    if (region->kind != CM_HIR_REGION_EARLY_BOUND) return 0u;
    parameter = cm_hir_get_generic_param(scan->hir,
        region->data.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_LIFETIME) {
        return CM_PARAM_DEP_FOREIGN;
    }
    return cm_param_dependency_owner(scan, parameter->owner, 0);
}

static unsigned int cm_param_dependency_const(
    const CmParamDependencyScan *scan, const CmHirConstArg *constant,
    size_t depth)
{
    const CmHirGenericParam *parameter;
    unsigned int flags;

    if (constant == NULL) return CM_PARAM_DEP_FOREIGN;
    flags = cm_param_dependency_type(scan, constant->type, depth + 1u);
    if (constant->kind != CM_HIR_CONST_PARAMETER) return flags;
    parameter = cm_hir_get_generic_param(scan->hir,
        constant->data.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_CONST) {
        return flags | CM_PARAM_DEP_FOREIGN;
    }
    return flags | cm_param_dependency_owner(scan, parameter->owner, 0);
}

static unsigned int cm_param_dependency_named(
    const CmParamDependencyScan *scan, const CmHirNamedType *named,
    size_t depth)
{
    unsigned int flags;
    uint32_t index;

    if (named == NULL || depth >= CM_PARAM_ENV_MAX_RECURSION) {
        return CM_PARAM_DEP_OVERFLOW;
    }
    flags = 0u;
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericArg *argument;

        argument = &named->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            flags |= cm_param_dependency_region(scan,
                &argument->data.lifetime);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            flags |= cm_param_dependency_type(scan, argument->data.type,
                depth + 1u);
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            flags |= cm_param_dependency_const(scan,
                &argument->data.constant, depth + 1u);
        } else {
            flags |= CM_PARAM_DEP_FOREIGN;
        }
    }
    return flags;
}

static unsigned int cm_param_dependency_type(
    const CmParamDependencyScan *scan, CmHirTypeId type_id, size_t depth)
{
    const CmHirGenericParam *parameter;
    const CmHirType *type;
    unsigned int flags;
    uint32_t index;

    if (depth >= CM_PARAM_ENV_MAX_RECURSION) return CM_PARAM_DEP_OVERFLOW;
    type = cm_hir_get_type(scan->hir, type_id);
    if (type == NULL) return CM_PARAM_DEP_FOREIGN;
    flags = 0u;
    switch (type->kind) {
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_param_dependency_region(scan,
                &type->data.reference_type.region)
            | cm_param_dependency_type(scan,
                type->data.reference_type.pointee, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_param_dependency_type(scan,
            type->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            flags |= cm_param_dependency_type(scan,
                type->data.tuple_type.elements[index], depth + 1u);
        }
        return flags;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_param_dependency_type(scan,
                type->data.array_type.element, depth + 1u)
            | cm_param_dependency_const(scan,
                &type->data.array_type.length, depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_param_dependency_type(scan,
            type->data.slice_type.element, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            flags |= cm_param_dependency_type(scan,
                type->data.fn_pointer_type.parameters[index], depth + 1u);
        }
        return flags | cm_param_dependency_type(scan,
            type->data.fn_pointer_type.return_type, depth + 1u);
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_param_dependency_named(scan, &type->data.named_type,
            depth + 1u);
    case CM_HIR_TYPE_SELF_KIND:
        return cm_param_dependency_owner(scan,
            type->data.self_type.owner, 1);
    case CM_HIR_TYPE_PARAMETER_KIND:
        parameter = cm_hir_get_generic_param(scan->hir,
            type->data.parameter_type.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE) {
            return CM_PARAM_DEP_FOREIGN;
        }
        return cm_param_dependency_owner(scan, parameter->owner, 0);
    case CM_HIR_TYPE_PROJECTION_KIND:
        return CM_PARAM_DEP_PROJECTION
            | cm_param_dependency_type(scan,
                type->data.projection_type.self_type, depth + 1u)
            | cm_param_dependency_named(scan,
                &type->data.projection_type.trait_type, depth + 1u)
            | cm_param_dependency_named(scan,
                &type->data.projection_type.associated_type, depth + 1u);
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        return cm_param_dependency_named(scan,
                &type->data.dyn_trait_type.principal_trait, depth + 1u)
            | cm_param_dependency_region(scan,
                &type->data.dyn_trait_type.region);
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        return 0u;
    }
    return CM_PARAM_DEP_FOREIGN;
}

static void cm_param_env_add_pending(CmParamEnvState *state,
    CmParamEnvPendingKind kind, CmHirDefId owner, size_t fact_index,
    CmSpan span)
{
    CmParamEnvPendingGoal pending;

    pending.kind = kind;
    pending.source_owner = owner;
    pending.fact_index = fact_index;
    pending.span = span;
    (void)cm_vec_push(&state->pending, &pending);
}

static unsigned int cm_param_env_fact_dependencies(CmParamEnvState *state,
    const CmHirTraitPredicate *predicate, CmHirDefId source_owner,
    size_t fact_index, unsigned int *out_dependencies)
{
    CmParamDependencyScan scan;
    unsigned int dependencies;
    unsigned int blockers;
    uint32_t equality_index;

    scan.hir = state->hir;
    scan.exact_owner = state->exact_owner;
    scan.enclosing_owner = state->enclosing_owner;
    dependencies = cm_param_dependency_type(&scan, predicate->subject, 0u)
        | cm_param_dependency_named(&scan, &predicate->trait_type, 0u);
    for (equality_index = 0u;
         equality_index < predicate->equality_count; ++equality_index) {
        dependencies |= cm_param_dependency_type(&scan,
            predicate->equalities[equality_index].value, 0u);
    }
    blockers = CM_PARAM_ENV_BLOCK_NONE;
    *out_dependencies = dependencies;
    if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
        || predicate->binder.lifetime_count != 0u
        || (dependencies & CM_PARAM_DEP_LATE_BOUND) != 0u) {
        blockers |= CM_PARAM_ENV_BLOCK_HIGHER_RANKED;
        cm_param_env_add_pending(state, CM_PARAM_ENV_PENDING_HIGHER_RANKED,
            source_owner, fact_index, predicate->span);
    }
    if (predicate->modifier != CM_HIR_PREDICATE_REQUIRED) {
        blockers |= CM_PARAM_ENV_BLOCK_MODIFIER;
        cm_param_env_add_pending(state,
            CM_PARAM_ENV_PENDING_UNSUPPORTED_MODIFIER,
            source_owner, fact_index, predicate->span);
    }
    if ((dependencies & CM_PARAM_DEP_PROJECTION) != 0u) {
        blockers |= CM_PARAM_ENV_BLOCK_PROJECTION;
        cm_param_env_add_pending(state,
            CM_PARAM_ENV_PENDING_PROJECTION_NORMALIZATION,
            source_owner, fact_index, predicate->span);
    }
    if ((dependencies & CM_PARAM_DEP_OVERFLOW) != 0u) {
        blockers |= CM_PARAM_ENV_BLOCK_OVERFLOW;
    }
    if ((dependencies & CM_PARAM_DEP_EXACT) != 0u
        && (dependencies & CM_PARAM_DEP_ENCLOSING) != 0u) {
        blockers |= CM_PARAM_ENV_BLOCK_MIXED_OWNER;
        cm_param_env_add_pending(state,
            CM_PARAM_ENV_PENDING_MIXED_OWNER_SUBSTITUTION,
            source_owner, fact_index, predicate->span);
    }
    if ((dependencies & (CM_PARAM_DEP_FOREIGN
            | CM_PARAM_DEP_SELF_FOREIGN)) != 0u) {
        blockers |= CM_PARAM_ENV_BLOCK_FOREIGN_OWNER;
        cm_param_env_add_pending(state,
            CM_PARAM_ENV_PENDING_FOREIGN_OWNER_SUBSTITUTION,
            source_owner, fact_index, predicate->span);
    }
    if (predicate->equality_count != 0u) {
        cm_param_env_add_pending(state,
            CM_PARAM_ENV_PENDING_PROJECTION_EQUALITY,
            source_owner, fact_index, predicate->span);
    }
    return blockers;
}

static void cm_param_env_add_predicates(CmParamEnvState *state,
    const CmHirItem *item, int enclosing)
{
    uint32_t index;

    for (index = 0u; index < item->predicate_count; ++index) {
        const CmHirTraitPredicate *predicate;
        CmParamEnvFact fact;
        size_t fact_index;
        unsigned int dependencies;

        predicate = &item->predicates[index];
        memset(&fact, 0, sizeof(fact));
        fact.kind = CM_PARAM_ENV_FACT_IMPLEMENTED;
        fact.provenance = enclosing
            ? CM_PARAM_ENV_PROVENANCE_ENCLOSING_PREDICATE
            : CM_PARAM_ENV_PROVENANCE_EXACT_PREDICATE;
        fact.source_owner = item->definition;
        fact.parameter_owner = item->definition;
        fact.self_owner = state->semantic_owner;
        fact.span = predicate->span;
        fact.data.implemented.subject = predicate->subject;
        fact.data.implemented.trait_type = predicate->trait_type;
        fact.data.implemented.equalities = predicate->equalities;
        fact.data.implemented.equality_count = predicate->equality_count;
        fact.data.implemented.scope = predicate->scope;
        fact.data.implemented.binder = predicate->binder;
        fact.data.implemented.modifier = predicate->modifier;
        fact_index = state->facts.len;
        fact.blocker_flags = cm_param_env_fact_dependencies(state,
            predicate, item->definition, fact_index, &dependencies);
        if ((dependencies & CM_PARAM_DEP_EXACT) == 0u
            && (dependencies & CM_PARAM_DEP_ENCLOSING) != 0u) {
            fact.parameter_owner = state->enclosing_owner;
        }
        (void)cm_vec_push(&state->facts, &fact);
        if (enclosing && item->kind == CM_HIR_ITEM_IMPL) {
            cm_param_env_add_pending(state,
                CM_PARAM_ENV_PENDING_RECURSIVE_IMPL_PREDICATE,
                item->definition, fact_index, predicate->span);
        }
    }
}

static void cm_param_env_add_outlives(CmParamEnvState *state,
    const CmHirItem *item, int enclosing)
{
    uint32_t index;

    for (index = 0u; index < item->outlives_predicate_count; ++index) {
        CmParamEnvFact fact;
        size_t fact_index;

        memset(&fact, 0, sizeof(fact));
        fact.kind = CM_PARAM_ENV_FACT_OUTLIVES;
        fact.provenance = enclosing
            ? CM_PARAM_ENV_PROVENANCE_ENCLOSING_OUTLIVES
            : CM_PARAM_ENV_PROVENANCE_EXACT_OUTLIVES;
        fact.source_owner = item->definition;
        fact.parameter_owner = item->definition;
        fact.self_owner = state->semantic_owner;
        fact.span = item->outlives_predicates[index].span;
        fact.data.outlives = item->outlives_predicates[index];
        fact_index = state->facts.len;
        (void)cm_vec_push(&state->facts, &fact);
        cm_param_env_add_pending(state, CM_PARAM_ENV_PENDING_OUTLIVES,
            item->definition, fact_index, fact.span);
    }
}

static void cm_param_env_add_trait_facts(CmParamEnvState *state,
    const CmHirItem *trait_item)
{
    CmParamEnvFact fact;
    uint32_t index;

    memset(&fact, 0, sizeof(fact));
    fact.kind = CM_PARAM_ENV_FACT_IMPLEMENTED;
    fact.provenance = CM_PARAM_ENV_PROVENANCE_TRAIT_SELF;
    fact.source_owner = trait_item->definition;
    fact.parameter_owner = trait_item->definition;
    fact.self_owner = trait_item->definition;
    fact.span = trait_item->span;
    fact.data.implemented.subject = CM_HIR_TYPE_NONE;
    fact.data.implemented.trait_type.definition = trait_item->definition;
    fact.data.implemented.modifier = CM_HIR_PREDICATE_REQUIRED;
    (void)cm_vec_push(&state->facts, &fact);
    for (index = 0u; index < trait_item->data.trait_item.supertrait_count;
         ++index) {
        const CmHirSupertrait *supertrait;
        CmParamDependencyScan scan;
        unsigned int dependencies;
        uint32_t equality_index;
        size_t fact_index;

        supertrait = &trait_item->data.trait_item.supertraits[index];
        memset(&fact, 0, sizeof(fact));
        fact.kind = CM_PARAM_ENV_FACT_IMPLEMENTED;
        fact.provenance = CM_PARAM_ENV_PROVENANCE_SUPERTRAIT;
        fact.source_owner = trait_item->definition;
        fact.parameter_owner = trait_item->definition;
        fact.self_owner = trait_item->definition;
        fact.span = supertrait->span;
        fact.data.implemented.subject = CM_HIR_TYPE_NONE;
        fact.data.implemented.trait_type = supertrait->trait_type;
        fact.data.implemented.equalities = supertrait->equalities;
        fact.data.implemented.equality_count = supertrait->equality_count;
        fact.data.implemented.modifier = supertrait->modifier
                == CM_HIR_SUPERTRAIT_REQUIRED
            ? CM_HIR_PREDICATE_REQUIRED : CM_HIR_PREDICATE_CONST_IF_CONST;
        fact_index = state->facts.len;
        scan.hir = state->hir;
        scan.exact_owner = state->exact_owner;
        scan.enclosing_owner = state->enclosing_owner;
        dependencies = cm_param_dependency_named(&scan,
            &supertrait->trait_type, 0u);
        for (equality_index = 0u;
             equality_index < supertrait->equality_count;
             ++equality_index) {
            dependencies |= cm_param_dependency_type(&scan,
                supertrait->equalities[equality_index].value, 0u);
        }
        if (supertrait->modifier != CM_HIR_SUPERTRAIT_REQUIRED) {
            fact.blocker_flags |= CM_PARAM_ENV_BLOCK_MODIFIER;
            cm_param_env_add_pending(state,
                CM_PARAM_ENV_PENDING_UNSUPPORTED_MODIFIER,
                trait_item->definition, fact_index, supertrait->span);
        }
        if ((dependencies & CM_PARAM_DEP_LATE_BOUND) != 0u) {
            fact.blocker_flags |= CM_PARAM_ENV_BLOCK_HIGHER_RANKED;
            cm_param_env_add_pending(state,
                CM_PARAM_ENV_PENDING_HIGHER_RANKED,
                trait_item->definition, fact_index, supertrait->span);
        }
        if ((dependencies & CM_PARAM_DEP_PROJECTION) != 0u) {
            fact.blocker_flags |= CM_PARAM_ENV_BLOCK_PROJECTION;
            cm_param_env_add_pending(state,
                CM_PARAM_ENV_PENDING_PROJECTION_NORMALIZATION,
                trait_item->definition, fact_index, supertrait->span);
        }
        if ((dependencies & CM_PARAM_DEP_OVERFLOW) != 0u) {
            fact.blocker_flags |= CM_PARAM_ENV_BLOCK_OVERFLOW;
        }
        if ((dependencies & CM_PARAM_DEP_EXACT) != 0u
            && (dependencies & CM_PARAM_DEP_ENCLOSING) != 0u) {
            fact.blocker_flags |= CM_PARAM_ENV_BLOCK_MIXED_OWNER;
            cm_param_env_add_pending(state,
                CM_PARAM_ENV_PENDING_MIXED_OWNER_SUBSTITUTION,
                trait_item->definition, fact_index, supertrait->span);
        }
        if ((dependencies & (CM_PARAM_DEP_FOREIGN
                | CM_PARAM_DEP_SELF_FOREIGN)) != 0u) {
            fact.blocker_flags |= CM_PARAM_ENV_BLOCK_FOREIGN_OWNER;
            cm_param_env_add_pending(state,
                CM_PARAM_ENV_PENDING_FOREIGN_OWNER_SUBSTITUTION,
                trait_item->definition, fact_index, supertrait->span);
        }
        if (supertrait->equality_count != 0u) {
            cm_param_env_add_pending(state,
                CM_PARAM_ENV_PENDING_PROJECTION_EQUALITY,
                trait_item->definition, fact_index, supertrait->span);
        }
        (void)cm_vec_push(&state->facts, &fact);
    }
}

static void cm_param_env_add_impl_header(CmParamEnvState *state,
    const CmHirItem *impl_item)
{
    CmParamEnvFact fact;

    if (!impl_item->data.impl_item.has_trait
        || impl_item->data.impl_item.is_negative) return;
    memset(&fact, 0, sizeof(fact));
    fact.kind = CM_PARAM_ENV_FACT_IMPLEMENTED;
    fact.provenance = CM_PARAM_ENV_PROVENANCE_POSITIVE_IMPL_HEADER;
    fact.source_owner = impl_item->definition;
    fact.parameter_owner = impl_item->definition;
    fact.self_owner = impl_item->definition;
    fact.span = impl_item->span;
    fact.data.implemented.subject = impl_item->data.impl_item.self_type;
    fact.data.implemented.trait_type = impl_item->data.impl_item.trait_type;
    fact.data.implemented.modifier = CM_HIR_PREDICATE_REQUIRED;
    (void)cm_vec_push(&state->facts, &fact);
}

CmParamEnvStatus cm_param_env_init(CmParamEnv *environment,
    const CmHirContext *hir, CmHirDefId exact_owner)
{
    const CmHirItem *exact_item;
    const CmHirItem *semantic_item;
    CmParamEnvState *state;

    if (environment == NULL || hir == NULL
        || environment->state != NULL) return CM_PARAM_ENV_INVALID;
    exact_item = cm_param_env_find_item(hir, exact_owner);
    if (exact_item == NULL) return CM_PARAM_ENV_INVALID;
    state = (CmParamEnvState *)cm_alloc_zeroed(1u,
        sizeof(CmParamEnvState));
    state->hir = hir;
    state->exact_owner = exact_owner;
    state->enclosing_owner = cm_hir_def_id_none();
    if (!cm_hir_def_id_is_none(exact_item->parent_definition)) {
        semantic_item = cm_param_env_find_item(hir,
            exact_item->parent_definition);
        if (semantic_item == NULL
            || (semantic_item->kind != CM_HIR_ITEM_TRAIT
                && semantic_item->kind != CM_HIR_ITEM_IMPL)) {
            cm_free(state);
            return CM_PARAM_ENV_INVALID;
        }
        state->enclosing_owner = semantic_item->definition;
    } else if (exact_item->kind == CM_HIR_ITEM_TRAIT
            || exact_item->kind == CM_HIR_ITEM_IMPL) {
        semantic_item = exact_item;
    } else {
        semantic_item = NULL;
    }
    state->semantic_owner = semantic_item == NULL
        ? cm_hir_def_id_none() : semantic_item->definition;
    cm_vec_init(&state->facts, sizeof(CmParamEnvFact));
    cm_vec_init(&state->pending, sizeof(CmParamEnvPendingGoal));
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->crate_count = hir->crates.len;
    state->module_count = hir->modules.len;
    state->item_count = hir->items.len;
    state->body_count = hir->bodies.len;
    state->expression_count = hir->expressions.len;
    state->type_count = hir->types.len;
    state->generic_parameter_count = hir->generic_parameters.len;
    state->definition_count = hir->definitions.len;
    state->prebound_associated_type_count =
        hir->prebound_associated_types.len;
    environment->state = state;

    cm_param_env_add_predicates(state, exact_item, 0);
    cm_param_env_add_outlives(state, exact_item, 0);
    if (semantic_item != NULL && semantic_item != exact_item) {
        cm_param_env_add_predicates(state, semantic_item, 1);
        cm_param_env_add_outlives(state, semantic_item, 1);
    }
    if (semantic_item != NULL && semantic_item->kind == CM_HIR_ITEM_TRAIT) {
        cm_param_env_add_trait_facts(state, semantic_item);
    } else if (semantic_item != NULL
            && semantic_item->kind == CM_HIR_ITEM_IMPL) {
        cm_param_env_add_impl_header(state, semantic_item);
    }
    return CM_PARAM_ENV_READY;
}

void cm_param_env_destroy(CmParamEnv *environment)
{
    CmParamEnvState *state;

    state = cm_param_env_state(environment);
    if (state == NULL) return;
    cm_vec_destroy(&state->pending);
    cm_vec_destroy(&state->facts);
    cm_free(state);
    environment->state = NULL;
}

int cm_param_env_is_current(const CmParamEnv *environment)
{
    return cm_param_env_state_current(cm_param_env_state_const(environment));
}

const CmHirContext *cm_param_env_hir(const CmParamEnv *environment)
{
    const CmParamEnvState *state;

    state = cm_param_env_state_const(environment);
    return !cm_param_env_state_current(state) ? NULL : state->hir;
}

CmHirDefId cm_param_env_exact_owner(const CmParamEnv *environment)
{
    const CmParamEnvState *state;

    state = cm_param_env_state_const(environment);
    return !cm_param_env_state_current(state)
        ? cm_hir_def_id_none() : state->exact_owner;
}

CmHirDefId cm_param_env_enclosing_owner(const CmParamEnv *environment)
{
    const CmParamEnvState *state;

    state = cm_param_env_state_const(environment);
    return !cm_param_env_state_current(state)
        ? cm_hir_def_id_none() : state->enclosing_owner;
}

size_t cm_param_env_fact_count(const CmParamEnv *environment)
{
    const CmParamEnvState *state;

    state = cm_param_env_state_const(environment);
    return !cm_param_env_state_current(state) ? 0u : state->facts.len;
}

const CmParamEnvFact *cm_param_env_fact(const CmParamEnv *environment,
    size_t fact_index)
{
    const CmParamEnvState *state;

    state = cm_param_env_state_const(environment);
    if (!cm_param_env_state_current(state)) return NULL;
    return (const CmParamEnvFact *)cm_vec_at_const(&state->facts,
        fact_index);
}

size_t cm_param_env_pending_count(const CmParamEnv *environment)
{
    const CmParamEnvState *state;

    state = cm_param_env_state_const(environment);
    return !cm_param_env_state_current(state) ? 0u : state->pending.len;
}

const CmParamEnvPendingGoal *cm_param_env_pending(
    const CmParamEnv *environment, size_t pending_index)
{
    const CmParamEnvState *state;

    state = cm_param_env_state_const(environment);
    if (!cm_param_env_state_current(state)) return NULL;
    return (const CmParamEnvPendingGoal *)cm_vec_at_const(&state->pending,
        pending_index);
}

static const CmTypeckInstantiation *cm_param_env_choose_instantiation(
    const CmParamEnvState *state, const CmParamEnvFact *fact,
    const CmParamEnvSubstitution *substitution)
{
    if (substitution == NULL) return NULL;
    if (!cm_hir_def_id_is_none(state->enclosing_owner)
        && cm_hir_def_id_equal(fact->parameter_owner,
            state->enclosing_owner)) return substitution->enclosing;
    return substitution->exact;
}

CmParamEnvStatus cm_param_env_instantiate_implemented(
    const CmParamEnv *environment, size_t fact_index,
    CmTypeckContext *typeck, const CmParamEnvSubstitution *substitution,
    CmTypeckTypeId *out_subject, CmTypeckNamedType *out_trait,
    CmTypeckStatus *out_typeck_status)
{
    const CmTypeckInstantiation *instantiation;
    const CmParamEnvState *state;
    const CmParamEnvFact *fact;
    CmTypeckSnapshot snapshot;
    CmTypeckStatus status;

    if (out_subject != NULL) *out_subject = CM_TYPECK_TYPE_NONE;
    if (out_trait != NULL) memset(out_trait, 0, sizeof(*out_trait));
    if (out_typeck_status != NULL) {
        *out_typeck_status = CM_TYPECK_INVALID_ARGUMENT;
    }
    state = cm_param_env_state_const(environment);
    fact = cm_param_env_fact(environment, fact_index);
    if (!cm_param_env_state_current(state)) return CM_PARAM_ENV_STALE;
    if (fact == NULL || fact->kind != CM_PARAM_ENV_FACT_IMPLEMENTED
        || typeck == NULL || cm_typeck_hir_context(typeck) != state->hir
        || out_subject == NULL || out_trait == NULL
        || fact->blocker_flags != CM_PARAM_ENV_BLOCK_NONE) {
        if (fact != NULL && (fact->blocker_flags
                & CM_PARAM_ENV_BLOCK_OVERFLOW) != 0u) {
            return CM_PARAM_ENV_OVERFLOW;
        }
        return fact != NULL && fact->blocker_flags != CM_PARAM_ENV_BLOCK_NONE
            ? CM_PARAM_ENV_UNSUPPORTED : CM_PARAM_ENV_INVALID;
    }
    instantiation = cm_param_env_choose_instantiation(state, fact,
        substitution);
    if (!cm_typeck_instantiation_is_valid(typeck, instantiation)
        || !cm_hir_def_id_equal(instantiation->parameter_owner,
            fact->parameter_owner)
        || (!cm_hir_def_id_is_none(fact->self_owner)
            && (instantiation->self_type == CM_TYPECK_TYPE_NONE
                || !cm_hir_def_id_equal(instantiation->self_owner,
                    fact->self_owner)))) return CM_PARAM_ENV_INVALID;
    status = cm_typeck_snapshot(typeck, &snapshot);
    if (status != CM_TYPECK_OK) return CM_PARAM_ENV_TYPECK_FAILURE;
    if (fact->data.implemented.subject == CM_HIR_TYPE_NONE) {
        if (instantiation->self_type == CM_TYPECK_TYPE_NONE
            || !cm_hir_def_id_equal(instantiation->self_owner,
                fact->self_owner)) {
            status = CM_TYPECK_INVALID_ARGUMENT;
        } else {
            *out_subject = instantiation->self_type;
        }
    } else {
        status = cm_typeck_instantiate_hir_type(typeck,
            fact->data.implemented.subject, instantiation, out_subject);
    }
    if (status == CM_TYPECK_OK
        && fact->provenance == CM_PARAM_ENV_PROVENANCE_TRAIT_SELF) {
        out_trait->definition = fact->data.implemented.trait_type.definition;
        out_trait->arguments = (CmTypeckGenericArg *)instantiation->arguments;
        out_trait->argument_count = instantiation->argument_count;
    } else if (status == CM_TYPECK_OK) {
        status = cm_typeck_instantiate_hir_named(typeck,
            &fact->data.implemented.trait_type, instantiation, out_trait);
    }
    if (out_typeck_status != NULL) *out_typeck_status = status;
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_commit(typeck, &snapshot);
    } else {
        (void)cm_typeck_rollback(typeck, &snapshot);
        *out_subject = CM_TYPECK_TYPE_NONE;
        memset(out_trait, 0, sizeof(*out_trait));
    }
    if (status == CM_TYPECK_OK) return CM_PARAM_ENV_READY;
    if (status == CM_TYPECK_OVERFLOW) return CM_PARAM_ENV_OVERFLOW;
    if (status == CM_TYPECK_UNSUPPORTED_HIR_TYPE
        || status == CM_TYPECK_UNSUPPORTED_CONSTANT) {
        return CM_PARAM_ENV_UNSUPPORTED;
    }
    return CM_PARAM_ENV_TYPECK_FAILURE;
}

const char *cm_param_env_status_name(CmParamEnvStatus status)
{
    switch (status) {
    case CM_PARAM_ENV_READY: return "ready";
    case CM_PARAM_ENV_INVALID: return "invalid";
    case CM_PARAM_ENV_STALE: return "stale";
    case CM_PARAM_ENV_UNSUPPORTED: return "unsupported";
    case CM_PARAM_ENV_OVERFLOW: return "overflow";
    case CM_PARAM_ENV_TYPECK_FAILURE: return "typeck failure";
    }
    return "unknown";
}

const char *cm_param_env_pending_name(CmParamEnvPendingKind kind)
{
    switch (kind) {
    case CM_PARAM_ENV_PENDING_HIGHER_RANKED: return "higher-ranked";
    case CM_PARAM_ENV_PENDING_OUTLIVES: return "outlives";
    case CM_PARAM_ENV_PENDING_PROJECTION_EQUALITY:
        return "projection equality";
    case CM_PARAM_ENV_PENDING_PROJECTION_NORMALIZATION:
        return "projection normalization";
    case CM_PARAM_ENV_PENDING_RECURSIVE_IMPL_PREDICATE:
        return "recursive impl predicate";
    case CM_PARAM_ENV_PENDING_UNSUPPORTED_MODIFIER:
        return "unsupported modifier";
    case CM_PARAM_ENV_PENDING_MIXED_OWNER_SUBSTITUTION:
        return "mixed-owner substitution";
    case CM_PARAM_ENV_PENDING_FOREIGN_OWNER_SUBSTITUTION:
        return "foreign-owner substitution";
    }
    return "unknown";
}
