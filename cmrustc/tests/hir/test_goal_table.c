#include "cm/hir/goal_table.h"
#include "../../src/hir/trait_solver_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirDefId owner_trait;
    CmHirDefId bound_trait;
    CmHirDefId lifetime_trait;
    CmHirDefId const_trait;
    CmHirGenericParamId lifetime_parameter;
    CmHirGenericParamId const_parameter;
    CmHirTypeId bool_hir;
    CmHirTypeId u8_hir;
} TestFixture;

typedef struct TestRuntime {
    CmParamEnv environment;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmTraitGoalTable table;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId u8_type;
} TestRuntime;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static void init_item(CmHirItem *item, CmHirItemKind kind,
    CmHirDefId definition, CmHirModuleId module, CmInternId name)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = module;
    item->parent_definition = cm_hir_def_id_none();
    item->name = name;
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = test_span(1u, 20u);
}

static CmHirTypeId add_scalar(CmHirContext *hir, CmHirTypeKind kind,
    CmHirIntType integer_kind)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(2u, 3u);
    type.data.integer_type.kind = integer_kind;
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirDefId add_trait_in_crate(TestFixture *fixture,
    CmHirCrateId crate_id, CmHirModuleId module, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, module,
        cm_hir_intern(&fixture->hir, name));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_trait(TestFixture *fixture, const char *name)
{
    return add_trait_in_crate(fixture, fixture->crate_id, fixture->root,
        name);
}

static CmHirDefId add_lifetime_trait(TestFixture *fixture)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_LIFETIME;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&fixture->hir, "a");
    parameter.span = test_span(2u, 3u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &fixture->lifetime_parameter) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, "Lifetime"));
    item.generic_parameter_start = fixture->lifetime_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_type_trait(TestFixture *fixture, const char *name)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&fixture->hir, "T");
    parameter.span = test_span(2u, 3u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_const_trait(TestFixture *fixture)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&fixture->hir, "N");
    parameter.span = test_span(2u, 3u);
    parameter.declared_type = fixture->u8_hir;
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &fixture->const_parameter) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, "Const"));
    item.generic_parameter_start = fixture->const_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void add_recursive_candidate(TestFixture *fixture)
{
    CmHirDefId definition;
    CmHirTraitPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = fixture->bool_hir;
    predicate.trait_type.definition = fixture->bound_trait;
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    predicate.span = test_span(4u, 8u);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.impl_item.self_type = fixture->bool_hir;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = fixture->bound_trait;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static CmHirDefId add_bool_impl(TestFixture *fixture,
    CmHirDefId trait_definition)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = fixture->bool_hir;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_bool_impl_with_type_argument(TestFixture *fixture,
    CmHirDefId trait_definition, CmHirTypeId argument_type)
{
    CmHirDefId definition;
    CmHirGenericArg argument;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = argument_type;
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = fixture->bool_hir;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.trait_type.arguments = &argument;
    item.data.impl_item.trait_type.argument_count = 1u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_trait_associated(TestFixture *fixture,
    CmHirDefId trait_definition, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition(&fixture->hir,
        fixture->crate_id, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.parent_definition = trait_definition;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_impl_associated(TestFixture *fixture,
    CmHirDefId impl_definition, CmHirDefId trait_associated_definition,
    CmHirTypeId target)
{
    CmHirDefId definition;
    const CmHirItem *trait_associated;
    CmHirItem item;
    CmHirItemId item_id;
    size_t item_index;

    assert(cm_hir_reserve_item_definition(&fixture->hir,
        fixture->crate_id, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    trait_associated = NULL;
    for (item_index = 0u; item_index < fixture->hir.items.len;
         ++item_index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(
            &fixture->hir.items, item_index);
        if (candidate != NULL && cm_hir_def_id_equal(candidate->definition,
                trait_associated_definition)) {
            trait_associated = candidate;
            break;
        }
    }
    assert(trait_associated != NULL);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        trait_associated->name);
    item.parent_definition = impl_definition;
    item.data.type_alias_item.target = target;
    item.data.type_alias_item.trait_item_definition =
        trait_associated_definition;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirTypeId add_projection_type(TestFixture *fixture,
    CmHirDefId trait_definition, CmHirDefId associated_definition,
    CmHirTypeId self_type)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = test_span(2u, 3u);
    type.data.projection_type.self_type = self_type;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_hir_add_type(&fixture->hir, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirTypeId add_projection_type_with_type_argument(
    TestFixture *fixture, CmHirDefId trait_definition,
    CmHirDefId associated_definition, CmHirTypeId self_type,
    CmHirTypeId argument_type)
{
    CmHirGenericArg argument;
    CmHirType type;
    CmHirTypeId type_id;

    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = argument_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = test_span(2u, 3u);
    type.data.projection_type.self_type = self_type;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.trait_type.arguments = &argument;
    type.data.projection_type.trait_type.argument_count = 1u;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_hir_add_type(&fixture->hir, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirDefId add_generic_predicate_impl(TestFixture *fixture,
    CmHirDefId trait_definition, CmHirDefId predicate_trait,
    uint32_t parameter_count, uint32_t self_parameter,
    uint32_t predicate_parameter)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_start;
    CmHirType parameter_type;
    CmHirTypeId parameter_types[2];
    CmHirTraitPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;
    uint32_t index;

    assert(parameter_count != 0u && parameter_count <= 2u);
    assert(self_parameter < parameter_count);
    assert(predicate_parameter < parameter_count);
    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    for (index = 0u; index < parameter_count; ++index) {
        CmHirGenericParamId parameter_id;

        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = CM_HIR_GENERIC_TYPE;
        parameter.owner = definition;
        parameter.index = index;
        parameter.name = cm_hir_intern(&fixture->hir,
            index == 0u ? "T" : "U");
        parameter.span = test_span(2u, 3u);
        assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
            &parameter_id) == CM_HIR_OK);
        if (index == 0u) parameter_start = parameter_id;
        memset(&parameter_type, 0, sizeof(parameter_type));
        parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
        parameter_type.span = test_span(2u, 3u);
        parameter_type.data.parameter_type.parameter = parameter_id;
        assert(cm_hir_add_type(&fixture->hir, &parameter_type,
            &parameter_types[index]) == CM_HIR_OK);
    }
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = parameter_types[predicate_parameter];
    predicate.trait_type.definition = predicate_trait;
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    predicate.span = test_span(4u, 8u);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.generic_parameter_start = parameter_start;
    item.generic_parameter_count = parameter_count;
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.impl_item.self_type = parameter_types[self_parameter];
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_owner_with_bool_fact(TestFixture *fixture,
    CmHirDefId fact_trait)
{
    CmHirDefId definition;
    CmHirTraitPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = fixture->bool_hir;
    predicate.trait_type.definition = fact_trait;
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    predicate.span = test_span(4u, 8u);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, "PredicateOwner"));
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_owner_with_bool_equality(TestFixture *fixture,
    CmHirDefId fact_trait, CmHirDefId associated_definition,
    CmHirTypeId equality_value, const char *name)
{
    CmHirDefId definition;
    CmHirAssociatedTypeEquality equality;
    CmHirTraitPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&equality, 0, sizeof(equality));
    equality.associated_type = associated_definition;
    equality.value = equality_value;
    equality.span = test_span(6u, 8u);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = fixture->bool_hir;
    predicate.trait_type.definition = fact_trait;
    predicate.equalities = &equality;
    predicate.equality_count = 1u;
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    predicate.span = test_span(4u, 10u);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_owner_with_equalities(TestFixture *fixture,
    CmHirDefId fact_trait, CmHirDefId associated_definition,
    const CmHirTypeId *subjects, const CmHirTypeId *values,
    uint32_t equality_count, int block_first, const char *name)
{
    CmHirDefId definition;
    CmHirAssociatedTypeEquality equalities[2];
    CmHirTraitPredicate predicates[2];
    CmInternId bound_lifetime;
    CmHirItem item;
    CmHirItemId item_id;
    uint32_t index;

    assert(equality_count != 0u && equality_count <= 2u);
    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(equalities, 0, sizeof(equalities));
    memset(predicates, 0, sizeof(predicates));
    bound_lifetime = cm_hir_intern(&fixture->hir, "blocked");
    for (index = 0u; index < equality_count; ++index) {
        equalities[index].associated_type = associated_definition;
        equalities[index].value = values[index];
        equalities[index].span = test_span(6u + index, 8u + index);
        predicates[index].subject = subjects[index];
        predicates[index].trait_type.definition = fact_trait;
        predicates[index].equalities = &equalities[index];
        predicates[index].equality_count = 1u;
        predicates[index].modifier = CM_HIR_PREDICATE_REQUIRED;
        predicates[index].span = test_span(4u + index, 10u + index);
    }
    if (block_first) {
        predicates[0].binder.lifetimes = &bound_lifetime;
        predicates[0].binder.lifetime_count = 1u;
        predicates[0].binder.span = predicates[0].span;
    }
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.predicates = predicates;
    item.predicate_count = equality_count;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void fixture_init(TestFixture *fixture, int recursive_candidate)
{
    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "goal_table"), CM_HIR_EDITION_2024,
        test_span(0u, 30u), &fixture->crate_id, &fixture->root) == CM_HIR_OK);
    fixture->bool_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_BOOL_KIND, CM_HIR_INT_U8);
    fixture->u8_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND, CM_HIR_INT_U8);
    fixture->owner_trait = add_trait(fixture, "Owner");
    fixture->bound_trait = add_trait(fixture, "Bound");
    fixture->lifetime_trait = add_lifetime_trait(fixture);
    fixture->const_trait = add_const_trait(fixture);
    if (recursive_candidate) add_recursive_candidate(fixture);
}

static void fixture_destroy(TestFixture *fixture)
{
    cm_hir_context_destroy(&fixture->hir);
}

static void runtime_init(TestRuntime *runtime, TestFixture *fixture,
    CmTraitGoalTableLimits limits)
{
    memset(runtime, 0, sizeof(*runtime));
    assert(cm_param_env_init(&runtime->environment, &fixture->hir,
        fixture->owner_trait) == CM_PARAM_ENV_READY);
    assert(cm_trait_impl_index_init(&runtime->index, &fixture->hir,
        fixture->crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&runtime->typeck, &fixture->hir);
    assert(cm_typeck_import_hir_type(&runtime->typeck, fixture->bool_hir,
        &runtime->bool_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&runtime->typeck, fixture->u8_hir,
        &runtime->u8_type) == CM_TYPECK_OK);
    cm_typeck_instantiation_init(&runtime->typeck, &runtime->exact);
    runtime->exact.parameter_owner = fixture->owner_trait;
    runtime->exact.self_owner = fixture->owner_trait;
    runtime->exact.self_type = runtime->bool_type;
    runtime->substitution.exact = &runtime->exact;
    assert(cm_trait_goal_table_init(&runtime->table, &runtime->index,
        &runtime->environment, limits) == CM_TRAIT_SOLVER_PROVEN);
}

static void runtime_destroy(TestRuntime *runtime)
{
    cm_trait_goal_table_destroy(&runtime->table);
    cm_typeck_context_destroy(&runtime->typeck);
    cm_trait_impl_index_destroy(&runtime->index);
    cm_param_env_destroy(&runtime->environment);
}

static CmTraitGoal implemented_goal(CmHirDefId owner,
    CmHirDefId trait_definition, CmTypeckTypeId self_type)
{
    CmTraitGoal goal;

    memset(&goal, 0, sizeof(goal));
    goal.kind = CM_TRAIT_GOAL_IMPLEMENTED;
    goal.data.implemented.owner = owner;
    goal.data.implemented.self_type = self_type;
    goal.data.implemented.trait_type.definition = trait_definition;
    return goal;
}

static CmTraitGoal projection_goal(CmHirDefId owner,
    CmTypeckTypeId projection_type, CmTypeckTypeId expected_type)
{
    CmTraitGoal goal;

    memset(&goal, 0, sizeof(goal));
    goal.kind = CM_TRAIT_GOAL_PROJECTION_EQUALITY;
    goal.data.projection_equality.owner = owner;
    goal.data.projection_equality.projection_type = projection_type;
    goal.data.projection_equality.expected_type = expected_type;
    return goal;
}

static void assert_cached_nonproven_clean(CmTraitSelectionResult result)
{
    assert(result.kind != CM_TRAIT_SOLVER_PROVEN);
    assert(result.proof_origin == CM_TRAIT_PROOF_NONE);
    assert(result.param_env_fact_index == CM_TRAIT_PROOF_FACT_NONE);
    assert(result.param_env_equality_index == CM_TRAIT_PROOF_EQUALITY_NONE);
    assert(cm_hir_def_id_is_none(result.impl_definition));
    assert(result.impl_item == CM_HIR_ITEM_NONE);
    assert(cm_hir_def_id_is_none(result.impl_associated_definition));
}

static void assert_unbound(CmTypeckContext *typeck, CmTypeckTypeId variable)
{
    CmTypeckTypeId resolved;

    assert(cm_typeck_resolve(typeck, variable, &resolved) == CM_TYPECK_OK);
    assert(resolved == variable);
}

static void test_structural_keys_and_nonproven_rollback(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmTypeckTypeId second_bool;
    CmTypeckTypeId first_variable;
    CmTypeckTypeId second_variable;
    size_t type_count;
    size_t hit_count;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, fixture.bool_hir,
        &second_bool) == CM_TYPECK_OK);

    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        runtime.bool_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 1u);

    goal.data.implemented.self_type = second_bool;
    hit_count = cm_trait_goal_table_cache_hit_count(&runtime.table);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert_cached_nonproven_clean(result);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 1u);
    assert(cm_trait_goal_table_cache_hit_count(&runtime.table)
        == hit_count + 1u);

    runtime.exact.self_type = runtime.u8_type;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 2u);
    runtime.exact.self_type = runtime.bool_type;

    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(10u, 11u), &first_variable) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(12u, 13u), &second_variable) == CM_TYPECK_OK);
    goal.data.implemented.self_type = first_variable;
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    assert_unbound(&runtime.typeck, first_variable);
    assert_unbound(&runtime.typeck, second_variable);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 3u);
    goal.data.implemented.self_type = second_variable;
    hit_count = cm_trait_goal_table_cache_hit_count(&runtime.table);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert_cached_nonproven_clean(result);
    assert_unbound(&runtime.typeck, first_variable);
    assert_unbound(&runtime.typeck, second_variable);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 3u);
    assert(cm_trait_goal_table_cache_hit_count(&runtime.table)
        == hit_count + 1u);

    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        runtime.bool_type);
    goal.binder.universe = 1u;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 4u);

    /* A matching environment fact is replayed and never cached yet. */
    goal = implemented_goal(fixture.owner_trait, fixture.owner_trait,
        runtime.bool_type);
    type_count = cm_trait_goal_table_entry_count(&runtime.table);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == type_count);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == type_count);

    /* A mismatching environment probe appends no persistent scratch terms. */
    goal.data.implemented.self_type = runtime.u8_type;
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_binder_and_region_canonicalization(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTypeckGenericArg argument;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    size_t entries;
    size_t hits;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, fixture.lifetime_trait,
        runtime.bool_type);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    argument.data.lifetime.kind = CM_HIR_REGION_LATE_BOUND;
    argument.data.lifetime.data.binder_index = 0u;
    goal.data.implemented.trait_type.arguments = &argument;
    goal.data.implemented.trait_type.argument_count = 1u;
    goal.binder.lifetime_count = 1u;
    goal.binder.universe = 7u;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    entries = cm_trait_goal_table_entry_count(&runtime.table);
    assert(entries == 1u);
    hits = cm_trait_goal_table_cache_hit_count(&runtime.table);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == entries);
    assert(cm_trait_goal_table_cache_hit_count(&runtime.table) == hits + 1u);

    goal.binder.debruijn_depth = 1u;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == entries + 1u);
    goal.binder.debruijn_depth = 0u;
    goal.binder.universe = 8u;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == entries + 2u);

    goal.binder.lifetime_count = 0u;
    goal.binder.universe = 0u;
    argument.data.lifetime.kind = CM_HIR_REGION_INFER;
    argument.data.lifetime.data.inference_variable = 41u;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    entries = cm_trait_goal_table_entry_count(&runtime.table);
    argument.data.lifetime.data.inference_variable = 99u;
    hits = cm_trait_goal_table_cache_hit_count(&runtime.table);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == entries);
    assert(cm_trait_goal_table_cache_hit_count(&runtime.table) == hits + 1u);

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_uncacheable_const_inference(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTypeckGenericArg argument;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_CONST;
    argument.data.constant.kind = CM_HIR_CONST_INFER;
    argument.data.constant.type = runtime.u8_type;
    goal = implemented_goal(fixture.owner_trait, fixture.const_trait,
        runtime.bool_type);
    goal.data.implemented.trait_type.arguments = &argument;
    goal.data.implemented.trait_type.argument_count = 1u;
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 0u);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 0u);
    assert(cm_trait_goal_table_cache_hit_count(&runtime.table) == 0u);

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static CmTypeckTypeId add_shared_tuple_chain(CmTypeckContext *typeck,
    CmTypeckTypeId leaf, size_t depth)
{
    CmTypeckType type;
    CmTypeckTypeId elements[2];
    CmTypeckTypeId current;
    size_t index;

    current = leaf;
    for (index = 0u; index < depth; ++index) {
        memset(&type, 0, sizeof(type));
        type.kind = CM_TYPECK_TYPE_TUPLE;
        type.span = test_span(20u, 21u);
        elements[0] = current;
        elements[1] = current;
        type.data.tuple_type.elements = elements;
        type.data.tuple_type.element_count = 2u;
        assert(cm_typeck_add_type(typeck, &type, &current) == CM_TYPECK_OK);
    }
    return current;
}

static CmTypeckTypeId add_duplicated_tuple_tree(CmTypeckContext *typeck,
    CmHirTypeId leaf_hir, size_t depth)
{
    CmTypeckType type;
    CmTypeckTypeId elements[2];
    CmTypeckTypeId result;

    if (depth == 0u) {
        assert(cm_typeck_import_hir_type(typeck, leaf_hir, &result)
            == CM_TYPECK_OK);
        return result;
    }
    elements[0] = add_duplicated_tuple_tree(typeck, leaf_hir, depth - 1u);
    elements[1] = add_duplicated_tuple_tree(typeck, leaf_hir, depth - 1u);
    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_TUPLE;
    type.span = test_span(20u, 21u);
    type.data.tuple_type.elements = elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_typeck_add_type(typeck, &type, &result) == CM_TYPECK_OK);
    return result;
}

static CmTypeckTypeId add_pair(CmTypeckContext *typeck,
    CmTypeckTypeId left, CmTypeckTypeId right)
{
    CmTypeckType type;
    CmTypeckTypeId elements[2];
    CmTypeckTypeId result;

    memset(&type, 0, sizeof(type));
    elements[0] = left;
    elements[1] = right;
    type.kind = CM_TYPECK_TYPE_TUPLE;
    type.span = test_span(20u, 21u);
    type.data.tuple_type.elements = elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_typeck_add_type(typeck, &type, &result) == CM_TYPECK_OK);
    return result;
}

static void test_dag_shape_and_inference_aliasing(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmTypeckTypeId shared;
    CmTypeckTypeId duplicated;
    CmTypeckTypeId first;
    CmTypeckTypeId second;
    CmTypeckTypeId third;
    CmTypeckTypeId fourth;
    CmTypeckTypeId repeated;
    CmTypeckTypeId repeated_again;
    CmTypeckTypeId distinct;
    size_t entries;
    size_t hits;

    memset(&limits, 0, sizeof(limits));
    limits.max_goal_depth = 64u;
    limits.max_canonical_nodes = 64u;
    limits.max_table_entries = 16u;
    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    shared = add_shared_tuple_chain(&runtime.typeck, runtime.bool_type, 4u);
    duplicated = add_duplicated_tuple_tree(&runtime.typeck,
        fixture.bool_hir, 4u);
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        shared);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    entries = cm_trait_goal_table_entry_count(&runtime.table);
    hits = cm_trait_goal_table_cache_hit_count(&runtime.table);
    goal.data.implemented.self_type = duplicated;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == entries);
    assert(cm_trait_goal_table_cache_hit_count(&runtime.table) == hits + 1u);

    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(30u, 31u), &first) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(32u, 33u), &second) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(34u, 35u), &third) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(36u, 37u), &fourth) == CM_TYPECK_OK);
    repeated = add_pair(&runtime.typeck, first, first);
    distinct = add_pair(&runtime.typeck, second, third);
    repeated_again = add_pair(&runtime.typeck, fourth, fourth);
    goal.data.implemented.self_type = repeated;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    entries = cm_trait_goal_table_entry_count(&runtime.table);
    goal.data.implemented.self_type = distinct;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == entries + 1u);
    goal.data.implemented.self_type = repeated_again;
    hits = cm_trait_goal_table_cache_hit_count(&runtime.table);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == entries + 1u);
    assert(cm_trait_goal_table_cache_hit_count(&runtime.table) == hits + 1u);
    assert_unbound(&runtime.typeck, first);
    assert_unbound(&runtime.typeck, second);
    assert_unbound(&runtime.typeck, third);
    assert_unbound(&runtime.typeck, fourth);

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_exact_depth_boundaries(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTable table;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmTypeckTypeId height_two;
    CmTypeckTypeId height_eight;
    CmTypeckTypeId height_nine;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    limits.max_goal_depth = 64u;
    limits.max_canonical_nodes = 64u;
    limits.max_table_entries = 16u;
    runtime_init(&runtime, &fixture, limits);
    height_two = add_shared_tuple_chain(&runtime.typeck, runtime.bool_type,
        1u);
    memset(&table, 0, sizeof(table));
    limits.max_goal_depth = 1u;
    assert(cm_trait_goal_table_init(&table, &runtime.index,
        &runtime.environment, limits) == CM_TRAIT_SOLVER_PROVEN);
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        runtime.bool_type);
    result = cm_trait_goal_table_solve(&table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    goal.data.implemented.self_type = height_two;
    result = cm_trait_goal_table_solve(&table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);
    cm_trait_goal_table_destroy(&table);

    height_eight = add_shared_tuple_chain(&runtime.typeck,
        runtime.bool_type, 7u);
    height_nine = add_shared_tuple_chain(&runtime.typeck,
        runtime.bool_type, 8u);
    memset(&table, 0, sizeof(table));
    limits.max_goal_depth = 8u;
    assert(cm_trait_goal_table_init(&table, &runtime.index,
        &runtime.environment, limits) == CM_TRAIT_SOLVER_PROVEN);
    goal.data.implemented.self_type = height_eight;
    result = cm_trait_goal_table_solve(&table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    goal.data.implemented.self_type = height_nine;
    result = cm_trait_goal_table_solve(&table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);
    cm_trait_goal_table_destroy(&table);

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_bounded_dag_cycle_and_capacity(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoalTable shallow_table;
    CmTraitGoalTable small_table;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmTypeckTypeId root;
    CmTypeckTypeId cycle;
    CmTypeckTypeId malformed;
    CmTypeckTypeId cycle_elements[1];
    CmTypeckType cycle_type;
    CmTypeckType *stored_cycle;
    CmTypeckTypeId *saved_elements;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));
    limits.max_goal_depth = 64u;
    limits.max_canonical_nodes = 64u;
    limits.max_table_entries = 1u;
    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    root = add_shared_tuple_chain(&runtime.typeck, runtime.bool_type, 20u);
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait, root);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 1u);

    goal.data.implemented.self_type = runtime.u8_type;
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 1u);
    goal.data.implemented.self_type = root;
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 1u);

    memset(&shallow_table, 0, sizeof(shallow_table));
    limits.max_goal_depth = 8u;
    limits.max_canonical_nodes = 64u;
    limits.max_table_entries = 8u;
    assert(cm_trait_goal_table_init(&shallow_table, &runtime.index,
        &runtime.environment, limits) == CM_TRAIT_SOLVER_PROVEN);
    goal.data.implemented.self_type = root;
    result = cm_trait_goal_table_solve(&shallow_table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);
    assert(cm_trait_goal_table_entry_count(&shallow_table) == 0u);
    cm_trait_goal_table_destroy(&shallow_table);

    memset(&small_table, 0, sizeof(small_table));
    limits.max_goal_depth = 64u;
    limits.max_canonical_nodes = 8u;
    assert(cm_trait_goal_table_init(&small_table, &runtime.index,
        &runtime.environment, limits) == CM_TRAIT_SOLVER_PROVEN);
    result = cm_trait_goal_table_solve(&small_table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);
    assert(cm_trait_goal_table_entry_count(&small_table) == 0u);
    cm_trait_goal_table_destroy(&small_table);

    memset(&cycle_type, 0, sizeof(cycle_type));
    cycle_type.kind = CM_TYPECK_TYPE_TUPLE;
    cycle_type.span = test_span(22u, 23u);
    cycle_elements[0] = runtime.bool_type;
    cycle_type.data.tuple_type.elements = cycle_elements;
    cycle_type.data.tuple_type.element_count = 1u;
    assert(cm_typeck_add_type(&runtime.typeck, &cycle_type, &malformed)
        == CM_TYPECK_OK);
    stored_cycle = (CmTypeckType *)cm_typeck_get_type(&runtime.typeck,
        malformed);
    assert(stored_cycle != NULL);
    saved_elements = stored_cycle->data.tuple_type.elements;
    stored_cycle->data.tuple_type.elements = NULL;
    goal.data.implemented.self_type = malformed;
    type_count = cm_trait_goal_table_entry_count(&runtime.table);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == type_count);
    stored_cycle->data.tuple_type.elements = saved_elements;

    memset(&cycle_type, 0, sizeof(cycle_type));
    cycle_type.kind = CM_TYPECK_TYPE_TUPLE;
    cycle_type.span = test_span(22u, 23u);
    cycle_elements[0] = runtime.bool_type;
    cycle_type.data.tuple_type.elements = cycle_elements;
    cycle_type.data.tuple_type.element_count = 1u;
    assert(cm_typeck_add_type(&runtime.typeck, &cycle_type, &cycle)
        == CM_TYPECK_OK);
    stored_cycle = (CmTypeckType *)cm_typeck_get_type(&runtime.typeck,
        cycle);
    assert(stored_cycle != NULL);
    stored_cycle->data.tuple_type.elements[0] = cycle;
    goal.data.implemented.self_type = cycle;
    type_count = cm_typeck_type_count(&runtime.typeck);
    {
        size_t entries_before;

        entries_before = cm_trait_goal_table_entry_count(&runtime.table);
        result = cm_trait_goal_table_solve(&runtime.table,
            &runtime.typeck, &runtime.substitution, &goal);
        assert(result.kind == CM_TRAIT_SOLVER_INVALID);
        assert(cm_typeck_type_count(&runtime.typeck) == type_count);
        assert(cm_trait_goal_table_entry_count(&runtime.table)
            == entries_before);
    }

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_recursive_candidate_and_staleness(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirTypeId extra_type;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 1);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        runtime.bool_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.blocking_match_count == 1u);
    assert(result.kind != CM_TRAIT_SOLVER_PROVEN);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 0u);

    extra_type = add_scalar(&fixture.hir, CM_HIR_TYPE_INTEGER_KIND,
        CM_HIR_INT_U16);
    assert(extra_type != CM_HIR_TYPE_NONE);
    assert(!cm_trait_goal_table_is_current(&runtime.table));
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 0u);

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_same_length_semantic_staleness(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmHirAttribute attribute;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    memset(&attribute, 0, sizeof(attribute));
    attribute.metadata = cm_hir_intern(&fixture.hir, "goal-table");
    attribute.span = test_span(1u, 2u);
    attribute.source_attribute = 1u;
    assert(cm_trait_goal_table_is_current(&runtime.table));
    assert(cm_hir_set_crate_inner_attributes(&fixture.hir,
        fixture.crate_id, NULL, 0u) == CM_HIR_OK);
    assert(cm_trait_goal_table_is_current(&runtime.table));
    assert(cm_hir_set_crate_inner_attributes(&fixture.hir,
        fixture.crate_id, &attribute, 1u) == CM_HIR_OK);
    assert(!cm_trait_goal_table_is_current(&runtime.table));
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_generic_predicate_proof_and_cycle_base(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmTypeckNamedType query;
    CmHirDefId outer_trait;
    CmHirDefId cycle_a;
    CmHirDefId cycle_b;
    CmHirDefId generic_impl;
    size_t entry_index;
    size_t type_count;
    int saw_generic_impl;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "Outer");
    generic_impl = add_generic_predicate_impl(&fixture, outer_trait,
        fixture.bound_trait, 1u, 0u, 0u);
    (void)add_bool_impl(&fixture, fixture.bound_trait);
    runtime_init(&runtime, &fixture, limits);
    saw_generic_impl = 0;
    for (entry_index = 0u;
         entry_index < cm_trait_impl_index_entry_count(&runtime.index);
         ++entry_index) {
        const CmTraitImplIndexEntry *entry;

        entry = cm_trait_impl_index_entry(&runtime.index, entry_index);
        assert(entry != NULL);
        if (cm_hir_def_id_equal(entry->impl_definition, generic_impl)) {
            assert(entry->unsupported_flags
                == CM_TRAIT_IMPL_UNSUPPORTED_NONE);
            saw_generic_impl = 1;
        }
    }
    assert(saw_generic_impl);
    memset(&query, 0, sizeof(query));
    query.definition = outer_trait;
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_solver_select(&runtime.index, &runtime.typeck,
        runtime.bool_type, &query);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.blocking_match_count == 1u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    goal = implemented_goal(fixture.owner_trait, outer_trait,
        runtime.bool_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_hir_def_id_equal(result.impl_definition, generic_impl)
        && result.supported_match_count == 1u
        && result.blocking_match_count == 0u);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 0);
    cycle_a = add_trait(&fixture, "CycleA");
    cycle_b = add_trait(&fixture, "CycleB");
    generic_impl = add_generic_predicate_impl(&fixture, cycle_a,
        cycle_b, 1u, 0u, 0u);
    (void)add_generic_predicate_impl(&fixture, cycle_b, cycle_a,
        1u, 0u, 0u);
    fixture.owner_trait = add_owner_with_bool_fact(&fixture, cycle_b);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, cycle_a,
        runtime.bool_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_hir_def_id_equal(result.impl_definition, generic_impl)
        && result.blocking_match_count == 0u);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 0u);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_generic_predicate_cycle_ambiguity_and_rollback(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirDefId cycle_a;
    CmHirDefId cycle_b;
    size_t hits;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    cycle_a = add_trait(&fixture, "CycleA");
    cycle_b = add_trait(&fixture, "CycleB");
    (void)add_generic_predicate_impl(&fixture, cycle_a, cycle_b,
        1u, 0u, 0u);
    (void)add_generic_predicate_impl(&fixture, cycle_b, cycle_a,
        1u, 0u, 0u);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, cycle_a,
        runtime.bool_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    hits = cm_trait_goal_table_cache_hit_count(&runtime.table);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 0u);
    assert(cm_trait_goal_table_cache_hit_count(&runtime.table) == hits);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && cm_typeck_type_count(&runtime.typeck) == type_count
        && cm_trait_goal_table_entry_count(&runtime.table) == 0u);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_predicate_nonproof_lattice_and_unconstrained(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirDefId outer_trait;
    CmHirDefId missing_trait;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "Outer");
    missing_trait = add_trait(&fixture, "Missing");
    (void)add_generic_predicate_impl(&fixture, outer_trait,
        missing_trait, 1u, 0u, 0u);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, outer_trait,
        runtime.bool_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u
        && cm_hir_def_id_is_none(result.impl_definition)
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "Outer");
    (void)add_generic_predicate_impl(&fixture, outer_trait,
        fixture.bound_trait, 1u, 0u, 0u);
    (void)add_bool_impl(&fixture, fixture.bound_trait);
    (void)add_bool_impl(&fixture, fixture.bound_trait);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, outer_trait,
        runtime.bool_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "Outer");
    (void)add_generic_predicate_impl(&fixture, outer_trait,
        fixture.bound_trait, 2u, 0u, 1u);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, outer_trait,
        runtime.bool_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.supported_match_count == 0u
        && result.blocking_match_count == 1u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_proven_and_nonproof_candidate_order(void)
{
    int reverse;

    for (reverse = 0; reverse <= 1; ++reverse) {
        TestFixture fixture;
        TestRuntime runtime;
        CmTraitGoalTableLimits limits;
        CmTraitGoal goal;
        CmTraitSelectionResult result;
        CmHirDefId outer_trait;
        CmHirDefId missing_trait;
        size_t type_count;

        memset(&limits, 0, sizeof(limits));
        fixture_init(&fixture, 0);
        outer_trait = add_trait(&fixture, "Outer");
        missing_trait = add_trait(&fixture, "Missing");
        if (reverse) {
            (void)add_bool_impl(&fixture, outer_trait);
            (void)add_generic_predicate_impl(&fixture, outer_trait,
                missing_trait, 1u, 0u, 0u);
        } else {
            (void)add_generic_predicate_impl(&fixture, outer_trait,
                missing_trait, 1u, 0u, 0u);
            (void)add_bool_impl(&fixture, outer_trait);
        }
        runtime_init(&runtime, &fixture, limits);
        goal = implemented_goal(fixture.owner_trait, outer_trait,
            runtime.bool_type);
        type_count = cm_typeck_type_count(&runtime.typeck);
        result = cm_trait_goal_table_solve(&runtime.table,
            &runtime.typeck, &runtime.substitution, &goal);
        assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
            && result.supported_match_count == 1u
            && result.blocking_match_count == 1u
            && cm_hir_def_id_is_none(result.impl_definition)
            && result.impl_item == CM_HIR_ITEM_NONE
            && cm_typeck_type_count(&runtime.typeck) == type_count);
        runtime_destroy(&runtime);
        fixture_destroy(&fixture);
    }
}

static void test_projection_equality_commit_and_rollback(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated_definition;
    CmHirTypeId projection_hir;
    CmTypeckTypeId projection_type;
    CmTypeckTypeId expected_variable;
    CmTypeckTypeId resolved;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    trait_definition = add_trait(&fixture, "Project");
    associated_definition = add_trait_associated(&fixture,
        trait_definition, "Assoc");
    projection_hir = add_projection_type(&fixture, trait_definition,
        associated_definition, fixture.bool_hir);
    impl_definition = add_bool_impl(&fixture, trait_definition);
    impl_associated_definition = add_impl_associated(&fixture,
        impl_definition, associated_definition, fixture.u8_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &expected_variable) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type,
        expected_variable);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_hir_def_id_equal(result.impl_definition, impl_definition));
    assert(cm_hir_def_id_equal(result.impl_associated_definition,
        impl_associated_definition));
    assert(cm_typeck_resolve(&runtime.typeck, expected_variable, &resolved)
        == CM_TYPECK_OK);
    assert(cm_typeck_get_type(&runtime.typeck, resolved) != NULL
        && cm_typeck_get_type(&runtime.typeck, resolved)->kind
            == CM_TYPECK_TYPE_INTEGER
        && cm_typeck_get_type(&runtime.typeck, resolved)->data.integer_type
            == CM_HIR_INT_U8);

    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.u8_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_hir_def_id_equal(result.impl_associated_definition,
        impl_associated_definition));

    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.bool_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_solver_solve_projection_equality(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        &goal.data.projection_equality, NULL);
    assert(result.kind == CM_TRAIT_SOLVER_NO_SOLUTION
        && result.supported_match_count == 1u
        && result.blocking_match_count == 0u
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_NO_SOLUTION);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 1u);

    goal = projection_goal(fixture.owner_trait, projection_type,
        projection_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_solver_solve_projection_equality(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        &goal.data.projection_equality, NULL);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);

    goal = implemented_goal(fixture.owner_trait, trait_definition,
        projection_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    assert_cached_nonproven_clean(result);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 2u);

    (void)add_scalar(&fixture.hir, CM_HIR_TYPE_CHAR_KIND, CM_HIR_INT_U8);
    assert(!cm_trait_goal_table_is_current(&runtime.table));
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    assert_cached_nonproven_clean(result);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static size_t find_equality_fact(const CmParamEnv *environment,
    CmHirDefId trait_definition, CmHirDefId associated_definition)
{
    size_t fact_index;

    for (fact_index = 0u;
         fact_index < cm_param_env_fact_count(environment); ++fact_index) {
        const CmParamEnvFact *fact;

        fact = cm_param_env_fact(environment, fact_index);
        if (fact != NULL
            && fact->kind == CM_PARAM_ENV_FACT_IMPLEMENTED
            && cm_hir_def_id_equal(fact->data.implemented.trait_type
                    .definition, trait_definition)
            && fact->data.implemented.equality_count == 1u
            && fact->data.implemented.equalities != NULL
            && cm_hir_def_id_equal(fact->data.implemented.equalities[0]
                    .associated_type, associated_definition)) {
            return fact_index;
        }
    }
    return (size_t)-1;
}

static void test_projection_parameter_environment_precedence(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirDefId environment_trait;
    CmHirDefId environment_associated;
    CmHirDefId projection_trait;
    CmHirDefId projection_associated;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated_definition;
    CmHirTypeId projection_hir;
    CmTypeckTypeId projection_type;
    CmTypeckTypeId expected;
    CmTypeckTypeId resolved;
    size_t equality_fact_index;
    size_t type_count;

    /* An unrelated equality must not suppress ordinary impl fallback. */
    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    environment_trait = add_trait(&fixture, "EnvironmentProject");
    environment_associated = add_trait_associated(&fixture,
        environment_trait, "EnvironmentAssoc");
    projection_trait = add_trait(&fixture, "FallbackProject");
    projection_associated = add_trait_associated(&fixture,
        projection_trait, "FallbackAssoc");
    fixture.owner_trait = add_owner_with_bool_equality(&fixture,
        environment_trait, environment_associated, fixture.u8_hir,
        "FallbackOwner");
    projection_hir = add_projection_type(&fixture, projection_trait,
        projection_associated, fixture.bool_hir);
    impl_definition = add_bool_impl(&fixture, projection_trait);
    impl_associated_definition = add_impl_associated(&fixture,
        impl_definition, projection_associated, fixture.u8_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.u8_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_IMPL
        && result.param_env_fact_index == CM_TRAIT_PROOF_FACT_NONE
        && result.param_env_equality_index
            == CM_TRAIT_PROOF_EQUALITY_NONE
        && cm_hir_def_id_equal(result.impl_definition, impl_definition)
        && cm_hir_def_id_equal(result.impl_associated_definition,
            impl_associated_definition));
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* A matching environment equality is authoritative over an impl. */
    fixture_init(&fixture, 0);
    projection_trait = add_trait(&fixture, "PrecedenceProject");
    projection_associated = add_trait_associated(&fixture,
        projection_trait, "PrecedenceAssoc");
    fixture.owner_trait = add_owner_with_bool_equality(&fixture,
        projection_trait, projection_associated, fixture.u8_hir,
        "PrecedenceOwner");
    projection_hir = add_projection_type(&fixture, projection_trait,
        projection_associated, fixture.bool_hir);
    impl_definition = add_bool_impl(&fixture, projection_trait);
    (void)add_impl_associated(&fixture, impl_definition,
        projection_associated, fixture.bool_hir);
    runtime_init(&runtime, &fixture, limits);
    equality_fact_index = find_equality_fact(&runtime.environment,
        projection_trait, projection_associated);
    assert(equality_fact_index != (size_t)-1);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &expected) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type, expected);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_PARAM_ENV
        && result.param_env_fact_index == equality_fact_index
        && result.param_env_equality_index == 0u
        && cm_hir_def_id_is_none(result.impl_definition)
        && cm_hir_def_id_is_none(result.impl_associated_definition));
    assert(cm_typeck_resolve(&runtime.typeck, expected, &resolved)
        == CM_TYPECK_OK);
    assert(cm_typeck_get_type(&runtime.typeck, resolved) != NULL
        && cm_typeck_get_type(&runtime.typeck, resolved)->kind
            == CM_TYPECK_TYPE_INTEGER
        && cm_typeck_get_type(&runtime.typeck, resolved)->data.integer_type
            == CM_HIR_INT_U8);

    /* A relevant equality that conflicts with the expected type must not
     * fall through to an impl whose associated value happens to match. */
    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.bool_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_NO_SOLUTION
        && result.supported_match_count == 0u
        && result.blocking_match_count == 0u
        && result.typeck_status == CM_TYPECK_OK);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_projection_parameter_environment_candidates(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirDefId projection_trait;
    CmHirDefId projection_associated;
    CmHirDefId impl_definition;
    CmHirTypeId subjects[2];
    CmHirTypeId values[2];
    CmHirTypeId projection_hir;
    CmTypeckType inference_projection;
    CmTypeckTypeId inference_self;
    CmTypeckTypeId projection_type;
    CmTypeckTypeId expected;
    CmTypeckTypeId resolved;
    size_t first_fact_index;
    size_t fact_index;
    size_t matching_fact_index;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));

    /* A concrete environment equality cannot select while projection Self is
     * still an inference variable. Probing must not bind that variable. */
    fixture_init(&fixture, 0);
    projection_trait = add_trait(&fixture, "DeferredSelfProject");
    projection_associated = add_trait_associated(&fixture,
        projection_trait, "DeferredSelfAssoc");
    subjects[0] = fixture.bool_hir;
    values[0] = fixture.u8_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture,
        projection_trait, projection_associated, subjects, values, 1u, 0,
        "DeferredSelfOwner");
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &inference_self) == CM_TYPECK_OK);
    memset(&inference_projection, 0, sizeof(inference_projection));
    inference_projection.kind = CM_TYPECK_TYPE_PROJECTION;
    inference_projection.span = test_span(21u, 24u);
    inference_projection.data.projection_type.self_type = inference_self;
    inference_projection.data.projection_type.trait_type.definition =
        projection_trait;
    inference_projection.data.projection_type.associated_type.definition =
        projection_associated;
    assert(cm_typeck_add_type(&runtime.typeck, &inference_projection,
        &projection_type) == CM_TYPECK_OK);
    assert_unbound(&runtime.typeck, inference_self);
    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.u8_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
    assert_cached_nonproven_clean(result);
    assert_unbound(&runtime.typeck, inference_self);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* Two applicable LHS equalities are ambiguous independent of RHS. */
    fixture_init(&fixture, 0);
    projection_trait = add_trait(&fixture, "DuplicateProject");
    projection_associated = add_trait_associated(&fixture,
        projection_trait, "DuplicateAssoc");
    subjects[0] = fixture.bool_hir;
    subjects[1] = fixture.bool_hir;
    values[0] = fixture.u8_hir;
    values[1] = fixture.bool_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture,
        projection_trait, projection_associated, subjects, values, 2u, 0,
        "DuplicateOwner");
    projection_hir = add_projection_type(&fixture, projection_trait,
        projection_associated, fixture.bool_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &expected) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type, expected);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 2u);
    assert_cached_nonproven_clean(result);
    assert_unbound(&runtime.typeck, expected);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);

    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.u8_type);
    result = cm_trait_solver_solve_projection_equality(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        &goal.data.projection_equality, NULL);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.supported_match_count == 1u);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* Identical applicable equalities are idempotent. The first fact is the
     * deterministic evidence source, while every probe remains isolated. */
    fixture_init(&fixture, 0);
    projection_trait = add_trait(&fixture, "IdempotentProject");
    projection_associated = add_trait_associated(&fixture,
        projection_trait, "IdempotentAssoc");
    subjects[0] = fixture.bool_hir;
    subjects[1] = fixture.bool_hir;
    values[0] = fixture.u8_hir;
    values[1] = fixture.u8_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture,
        projection_trait, projection_associated, subjects, values, 2u, 0,
        "IdempotentOwner");
    projection_hir = add_projection_type(&fixture, projection_trait,
        projection_associated, fixture.bool_hir);
    runtime_init(&runtime, &fixture, limits);
    first_fact_index = find_equality_fact(&runtime.environment,
        projection_trait, projection_associated);
    assert(first_fact_index != (size_t)-1);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &expected) == CM_TYPECK_OK);
    assert_unbound(&runtime.typeck, expected);
    goal = projection_goal(fixture.owner_trait, projection_type, expected);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_PARAM_ENV
        && result.param_env_fact_index == first_fact_index
        && result.param_env_equality_index == 0u
        && result.supported_match_count == 2u
        && cm_hir_def_id_is_none(result.impl_definition)
        && cm_hir_def_id_is_none(result.impl_associated_definition));
    assert(cm_typeck_resolve(&runtime.typeck, expected, &resolved)
        == CM_TYPECK_OK);
    assert(cm_typeck_get_type(&runtime.typeck, resolved) != NULL
        && cm_typeck_get_type(&runtime.typeck, resolved)->kind
            == CM_TYPECK_TYPE_INTEGER
        && cm_typeck_get_type(&runtime.typeck, resolved)->data.integer_type
            == CM_HIR_INT_U8);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* A failed LHS probe must roll back before replaying a later winner. */
    fixture_init(&fixture, 0);
    projection_trait = add_trait(&fixture, "ProbeProject");
    projection_associated = add_trait_associated(&fixture,
        projection_trait, "ProbeAssoc");
    subjects[0] = fixture.u8_hir;
    subjects[1] = fixture.bool_hir;
    values[0] = fixture.bool_hir;
    values[1] = fixture.u8_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture,
        projection_trait, projection_associated, subjects, values, 2u, 0,
        "ProbeOwner");
    projection_hir = add_projection_type(&fixture, projection_trait,
        projection_associated, fixture.bool_hir);
    runtime_init(&runtime, &fixture, limits);
    first_fact_index = find_equality_fact(&runtime.environment,
        projection_trait, projection_associated);
    matching_fact_index = (size_t)-1;
    for (fact_index = first_fact_index + 1u;
         fact_index < cm_param_env_fact_count(&runtime.environment);
         ++fact_index) {
        const CmParamEnvFact *fact;

        fact = cm_param_env_fact(&runtime.environment, fact_index);
        if (fact != NULL
            && fact->kind == CM_PARAM_ENV_FACT_IMPLEMENTED
            && cm_hir_def_id_equal(fact->data.implemented.trait_type
                    .definition, projection_trait)
            && fact->data.implemented.equality_count == 1u
            && fact->data.implemented.equalities != NULL
            && cm_hir_def_id_equal(fact->data.implemented.equalities[0]
                    .associated_type, projection_associated)) {
            matching_fact_index = fact_index;
            break;
        }
    }
    assert(first_fact_index != (size_t)-1
        && matching_fact_index != (size_t)-1);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &expected) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type, expected);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_PARAM_ENV
        && result.param_env_fact_index == matching_fact_index
        && result.param_env_equality_index == 0u
        && result.supported_match_count == 1u
        && cm_hir_def_id_is_none(result.impl_definition)
        && cm_hir_def_id_is_none(result.impl_associated_definition));
    assert(cm_typeck_resolve(&runtime.typeck, expected, &resolved)
        == CM_TYPECK_OK);
    assert(cm_typeck_get_type(&runtime.typeck, resolved) != NULL
        && cm_typeck_get_type(&runtime.typeck, resolved)->kind
            == CM_TYPECK_TYPE_INTEGER
        && cm_typeck_get_type(&runtime.typeck, resolved)->data.integer_type
            == CM_HIR_INT_U8);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* A blocked equality with a concrete nonmatching LHS is irrelevant and
     * must neither suppress nor contaminate the valid bool impl. */
    fixture_init(&fixture, 0);
    projection_trait = add_trait(&fixture,
        "NonmatchingBlockedEqualityProject");
    projection_associated = add_trait_associated(&fixture,
        projection_trait, "NonmatchingBlockedEqualityAssoc");
    subjects[0] = fixture.u8_hir;
    values[0] = fixture.bool_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture,
        projection_trait, projection_associated, subjects, values, 1u, 1,
        "NonmatchingBlockedEqualityOwner");
    projection_hir = add_projection_type(&fixture, projection_trait,
        projection_associated, fixture.bool_hir);
    impl_definition = add_bool_impl(&fixture, projection_trait);
    (void)add_impl_associated(&fixture, impl_definition,
        projection_associated, fixture.u8_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &expected) == CM_TYPECK_OK);
    assert_unbound(&runtime.typeck, expected);
    goal = projection_goal(fixture.owner_trait, projection_type, expected);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_IMPL
        && result.param_env_fact_index == CM_TRAIT_PROOF_FACT_NONE
        && result.param_env_equality_index
            == CM_TRAIT_PROOF_EQUALITY_NONE
        && result.blocking_match_count == 0u
        && cm_hir_def_id_equal(result.impl_definition, impl_definition)
        && !cm_hir_def_id_is_none(result.impl_associated_definition));
    assert(cm_typeck_resolve(&runtime.typeck, expected, &resolved)
        == CM_TYPECK_OK);
    assert(cm_typeck_get_type(&runtime.typeck, resolved) != NULL
        && cm_typeck_get_type(&runtime.typeck, resolved)->kind
            == CM_TYPECK_TYPE_INTEGER
        && cm_typeck_get_type(&runtime.typeck, resolved)->data.integer_type
            == CM_HIR_INT_U8);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* A relevant blocked equality suppresses an otherwise usable impl. */
    fixture_init(&fixture, 0);
    projection_trait = add_trait(&fixture, "BlockedEqualityProject");
    projection_associated = add_trait_associated(&fixture,
        projection_trait, "BlockedEqualityAssoc");
    subjects[0] = fixture.bool_hir;
    values[0] = fixture.u8_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture,
        projection_trait, projection_associated, subjects, values, 1u, 1,
        "BlockedEqualityOwner");
    projection_hir = add_projection_type(&fixture, projection_trait,
        projection_associated, fixture.bool_hir);
    impl_definition = add_bool_impl(&fixture, projection_trait);
    (void)add_impl_associated(&fixture, impl_definition,
        projection_associated, fixture.u8_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.u8_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.blocking_match_count == 1u);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_projection_equality_ambiguity_order(void)
{
    int reverse;

    for (reverse = 0; reverse <= 1; ++reverse) {
        TestFixture fixture;
        TestRuntime runtime;
        CmTraitGoalTableLimits limits;
        CmTraitGoal goal;
        CmTraitSelectionResult result;
        CmHirDefId trait_definition;
        CmHirDefId associated_definition;
        CmHirDefId first_impl;
        CmHirDefId second_impl;
        CmHirTypeId projection_hir;
        CmTypeckTypeId projection_type;
        CmTypeckTypeId expected;
        size_t type_count;

        memset(&limits, 0, sizeof(limits));
        fixture_init(&fixture, 0);
        trait_definition = add_trait(&fixture, "AmbiguousProject");
        associated_definition = add_trait_associated(&fixture,
            trait_definition, "Assoc");
        projection_hir = add_projection_type(&fixture, trait_definition,
            associated_definition, fixture.bool_hir);
        first_impl = add_bool_impl(&fixture, trait_definition);
        (void)add_impl_associated(&fixture, first_impl,
            associated_definition,
            reverse ? fixture.bool_hir : fixture.u8_hir);
        second_impl = add_bool_impl(&fixture, trait_definition);
        (void)add_impl_associated(&fixture, second_impl,
            associated_definition,
            reverse ? fixture.u8_hir : fixture.bool_hir);
        runtime_init(&runtime, &fixture, limits);
        assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
            &projection_type) == CM_TYPECK_OK);
        assert(cm_typeck_new_variable(&runtime.typeck,
            CM_HIR_INFER_GENERAL, test_span(21u, 22u), &expected)
            == CM_TYPECK_OK);
        goal = projection_goal(fixture.owner_trait, projection_type,
            expected);
        type_count = cm_typeck_type_count(&runtime.typeck);
        result = cm_trait_goal_table_solve(&runtime.table,
            &runtime.typeck, &runtime.substitution, &goal);
        assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS);
        assert(result.supported_match_count == 2u);
        assert_cached_nonproven_clean(result);
        assert_unbound(&runtime.typeck, expected);
        assert(cm_typeck_type_count(&runtime.typeck) == type_count);

        goal = projection_goal(fixture.owner_trait, projection_type,
            runtime.u8_type);
        result = cm_trait_goal_table_solve(&runtime.table,
            &runtime.typeck, &runtime.substitution, &goal);
        assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS);
        assert(result.supported_match_count == 2u);
        assert_cached_nonproven_clean(result);
        runtime_destroy(&runtime);
        fixture_destroy(&fixture);
    }
}

static void test_projection_equality_defaults_and_foreign_metadata(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirCrateId foreign_crate;
    CmHirModuleId foreign_root;
    CmHirDefId default_trait;
    CmHirDefId default_associated;
    CmHirDefId missing_foreign_trait;
    CmHirDefId missing_foreign_associated;
    CmHirDefId present_foreign_trait;
    CmHirDefId missing_present_trait_associated;
    CmHirItem foreign_item;
    CmHirItemId foreign_item_id;
    CmHirTypeId default_projection_hir;
    CmHirTypeId missing_trait_projection_hir;
    CmHirTypeId missing_associated_projection_hir;
    CmTypeckTypeId projection_type;
    size_t item_index;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    default_trait = add_trait(&fixture, "DefaultProject");
    default_associated = add_trait_associated(&fixture, default_trait,
        "DefaultAssoc");
    default_projection_hir = add_projection_type(&fixture, default_trait,
        default_associated, fixture.bool_hir);
    /* The current HIR builder rejects defaults before the solver sees them. */
    for (item_index = 0u; item_index < fixture.hir.items.len;
         ++item_index) {
        CmHirItem *item;

        item = (CmHirItem *)cm_vec_at(&fixture.hir.items, item_index);
        if (item != NULL && cm_hir_def_id_equal(item->definition,
                default_associated)) {
            item->data.type_alias_item.target = fixture.u8_hir;
            break;
        }
    }
    assert(item_index != fixture.hir.items.len);

    assert(cm_hir_create_crate(&fixture.hir,
        cm_hir_intern(&fixture.hir, "foreign_projection_metadata"),
        CM_HIR_EDITION_2024, test_span(0u, 30u), &foreign_crate,
        &foreign_root) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir, foreign_crate,
        CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &missing_foreign_trait) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir, foreign_crate,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(1u, 20u),
        &missing_foreign_associated) == CM_HIR_OK);
    init_item(&foreign_item, CM_HIR_ITEM_TYPE_ALIAS,
        missing_foreign_associated, foreign_root,
        cm_hir_intern(&fixture.hir, "MissingForeignAssoc"));
    foreign_item.parent_definition = missing_foreign_trait;
    foreign_item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    foreign_item.data.type_alias_item.trait_item_definition =
        cm_hir_def_id_none();
    assert(cm_hir_prebind_trait_associated_type_declaration(&fixture.hir,
        &foreign_item, &foreign_item_id) == CM_HIR_OK);
    missing_trait_projection_hir = add_projection_type(&fixture,
        missing_foreign_trait, missing_foreign_associated,
        fixture.bool_hir);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir, foreign_crate,
        CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &present_foreign_trait) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir, foreign_crate,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(1u, 20u),
        &missing_present_trait_associated) == CM_HIR_OK);
    init_item(&foreign_item, CM_HIR_ITEM_TYPE_ALIAS,
        missing_present_trait_associated, foreign_root,
        cm_hir_intern(&fixture.hir, "MissingPresentTraitAssoc"));
    foreign_item.parent_definition = present_foreign_trait;
    foreign_item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    foreign_item.data.type_alias_item.trait_item_definition =
        cm_hir_def_id_none();
    assert(cm_hir_prebind_trait_associated_type_declaration(&fixture.hir,
        &foreign_item, &foreign_item_id) == CM_HIR_OK);
    missing_associated_projection_hir = add_projection_type(&fixture,
        present_foreign_trait, missing_present_trait_associated,
        fixture.bool_hir);
    init_item(&foreign_item, CM_HIR_ITEM_TRAIT, present_foreign_trait,
        foreign_root, cm_hir_intern(&fixture.hir,
            "PresentForeignProject"));
    foreign_item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &foreign_item, &foreign_item_id)
        == CM_HIR_OK);

    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck,
        default_projection_hir, &projection_type) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.u8_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    assert_cached_nonproven_clean(result);

    assert(cm_typeck_import_hir_type(&runtime.typeck,
        missing_trait_projection_hir, &projection_type) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.u8_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert_cached_nonproven_clean(result);

    assert(cm_typeck_import_hir_type(&runtime.typeck,
        missing_associated_projection_hir, &projection_type)
        == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type,
        runtime.u8_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert_cached_nonproven_clean(result);

    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_projection_trait_argument_reallocation(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated_definition;
    CmHirTypeId projection_hir;
    CmTypeckTypeId projection_type;
    CmTypeckTypeId expected;
    CmTypeckTypeId resolved;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    trait_definition = add_type_trait(&fixture, "ArgumentProject");
    associated_definition = add_trait_associated(&fixture,
        trait_definition, "ArgumentAssoc");
    projection_hir = add_projection_type_with_type_argument(&fixture,
        trait_definition, associated_definition, fixture.bool_hir,
        fixture.u8_hir);
    impl_definition = add_bool_impl_with_type_argument(&fixture,
        trait_definition, fixture.u8_hir);
    impl_associated_definition = add_impl_associated(&fixture,
        impl_definition, associated_definition, fixture.u8_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    /* Four live roots fill the initial small type vector; probing grows it. */
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &expected) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type, expected);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_hir_def_id_equal(result.impl_definition, impl_definition));
    assert(cm_hir_def_id_equal(result.impl_associated_definition,
        impl_associated_definition));
    assert(cm_typeck_resolve(&runtime.typeck, expected, &resolved)
        == CM_TYPECK_OK);
    assert(cm_typeck_get_type(&runtime.typeck, resolved) != NULL
        && cm_typeck_get_type(&runtime.typeck, resolved)->kind
            == CM_TYPECK_TYPE_INTEGER
        && cm_typeck_get_type(&runtime.typeck, resolved)->data.integer_type
            == CM_HIR_INT_U8);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_projection_equality_recursion_and_blockers(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirDefId inner_trait;
    CmHirDefId inner_associated;
    CmHirDefId inner_impl;
    CmHirDefId outer_trait;
    CmHirDefId outer_associated;
    CmHirDefId outer_impl;
    CmHirDefId missing_trait;
    CmHirTypeId inner_projection;
    CmHirTypeId outer_projection;
    CmHirTypeId tuple_target;
    CmHirTypeId tuple_elements[1];
    CmHirType tuple;
    CmTypeckTypeId outer_type;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    inner_trait = add_trait(&fixture, "InnerProject");
    inner_associated = add_trait_associated(&fixture, inner_trait,
        "InnerAssoc");
    inner_projection = add_projection_type(&fixture, inner_trait,
        inner_associated, fixture.bool_hir);
    inner_impl = add_bool_impl(&fixture, inner_trait);
    (void)add_impl_associated(&fixture, inner_impl, inner_associated,
        fixture.u8_hir);
    outer_trait = add_trait(&fixture, "OuterProject");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "OuterAssoc");
    outer_projection = add_projection_type(&fixture, outer_trait,
        outer_associated, fixture.bool_hir);
    outer_impl = add_bool_impl(&fixture, outer_trait);
    (void)add_impl_associated(&fixture, outer_impl, outer_associated,
        inner_projection);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, outer_projection,
        &outer_type) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, outer_type,
        runtime.u8_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_solver_solve_projection_equality(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        &goal.data.projection_equality, NULL);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.blocking_match_count == 1u);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN);
    assert(!cm_hir_def_id_is_none(result.impl_associated_definition));
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "CycleProject");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "CycleAssoc");
    outer_projection = add_projection_type(&fixture, outer_trait,
        outer_associated, fixture.bool_hir);
    outer_impl = add_bool_impl(&fixture, outer_trait);
    (void)add_impl_associated(&fixture, outer_impl, outer_associated,
        outer_projection);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, outer_projection,
        &outer_type) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, outer_type,
        runtime.u8_type);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS);
    assert_cached_nonproven_clean(result);
    assert(cm_trait_goal_table_entry_count(&runtime.table) == 0u);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 0);
    inner_trait = add_trait(&fixture, "NestedInner");
    inner_associated = add_trait_associated(&fixture, inner_trait,
        "NestedInnerAssoc");
    inner_projection = add_projection_type(&fixture, inner_trait,
        inner_associated, fixture.bool_hir);
    memset(&tuple, 0, sizeof(tuple));
    tuple.kind = CM_HIR_TYPE_TUPLE_KIND;
    tuple.span = test_span(2u, 3u);
    tuple_elements[0] = inner_projection;
    tuple.data.tuple_type.elements = tuple_elements;
    tuple.data.tuple_type.element_count = 1u;
    assert(cm_hir_add_type(&fixture.hir, &tuple, &tuple_target)
        == CM_HIR_OK);
    outer_trait = add_trait(&fixture, "NestedOuter");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "NestedOuterAssoc");
    outer_projection = add_projection_type(&fixture, outer_trait,
        outer_associated, fixture.bool_hir);
    outer_impl = add_bool_impl(&fixture, outer_trait);
    (void)add_impl_associated(&fixture, outer_impl, outer_associated,
        tuple_target);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, outer_projection,
        &outer_type) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, outer_type,
        runtime.u8_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.blocking_match_count == 1u);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "PredicateProject");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "PredicateAssoc");
    missing_trait = add_trait(&fixture, "PredicateMissing");
    outer_projection = add_projection_type(&fixture, outer_trait,
        outer_associated, fixture.bool_hir);
    outer_impl = add_generic_predicate_impl(&fixture, outer_trait,
        missing_trait, 1u, 0u, 0u);
    (void)add_impl_associated(&fixture, outer_impl, outer_associated,
        fixture.u8_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, outer_projection,
        &outer_type) == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, outer_type,
        runtime.u8_type);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert_cached_nonproven_clean(result);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void test_destroyed_typeck_rejected(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        runtime.bool_type);
    cm_typeck_context_destroy(&runtime.typeck);
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    cm_trait_goal_table_destroy(&runtime.table);
    cm_trait_impl_index_destroy(&runtime.index);
    cm_param_env_destroy(&runtime.environment);
    fixture_destroy(&fixture);
}

static void test_destroyed_index_and_environment_rejected(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmTraitGoal goal;
    CmTraitSelectionResult result;

    memset(&limits, 0, sizeof(limits));
    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        runtime.bool_type);
    cm_trait_impl_index_destroy(&runtime.index);
    assert(!cm_trait_goal_table_is_current(&runtime.table));
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    fixture_init(&fixture, 0);
    runtime_init(&runtime, &fixture, limits);
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        runtime.bool_type);
    cm_param_env_destroy(&runtime.environment);
    assert(!cm_trait_goal_table_is_current(&runtime.table));
    result = cm_trait_goal_table_solve(&runtime.table, &runtime.typeck,
        &runtime.substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

static void assert_projection_target_nonproven_clean(
    CmProjectionTargetResult result)
{
    assert_cached_nonproven_clean(result.selection);
    assert(result.target == CM_TYPECK_TYPE_NONE);
}

static void test_projection_target_selection(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmTraitGoalTableLimits limits;
    CmProjectionTargetGoal goal;
    CmProjectionTargetResult target;
    CmHirDefId inner_trait;
    CmHirDefId inner_associated;
    CmHirDefId outer_trait;
    CmHirDefId outer_associated;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated;
    CmHirTypeId values[2];
    CmHirTypeId projection_hir;
    CmTypeckTypeId projection_type;
    CmTypeckTypeId resolved;
    const CmTypeckType *resolved_type;
    size_t type_count;

    memset(&limits, 0, sizeof(limits));

    /* A projection-valued environment target is returned raw and remains
     * authoritative over a conflicting impl fallback. */
    fixture_init(&fixture, 0);
    inner_trait = add_trait(&fixture, "TargetInner");
    inner_associated = add_trait_associated(&fixture, inner_trait,
        "TargetInnerAssoc");
    outer_trait = add_trait(&fixture, "TargetOuter");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "TargetOuterAssoc");
    values[0] = add_projection_type(&fixture, inner_trait,
        inner_associated, fixture.bool_hir);
    fixture.owner_trait = add_owner_with_equalities(&fixture, outer_trait,
        outer_associated, &fixture.bool_hir, values, 1u, 0,
        "TargetOwner");
    impl_definition = add_bool_impl(&fixture, outer_trait);
    (void)add_impl_associated(&fixture, impl_definition, outer_associated,
        fixture.bool_hir);
    projection_hir = add_projection_type(&fixture, outer_trait,
        outer_associated, fixture.bool_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    memset(&goal, 0, sizeof(goal));
    goal.owner = fixture.owner_trait;
    goal.projection_type = projection_type;
    target = cm_trait_solver_select_projection_target(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        &goal, NULL);
    assert(target.selection.kind == CM_TRAIT_SOLVER_PROVEN
        && target.selection.proof_origin == CM_TRAIT_PROOF_PARAM_ENV
        && target.selection.param_env_fact_index
            != CM_TRAIT_PROOF_FACT_NONE
        && target.selection.param_env_equality_index == 0u
        && cm_hir_def_id_is_none(target.selection.impl_definition)
        && target.target != CM_TYPECK_TYPE_NONE);
    assert(cm_typeck_resolve(&runtime.typeck, target.target, &resolved)
        == CM_TYPECK_OK);
    resolved_type = cm_typeck_get_type(&runtime.typeck, resolved);
    assert(resolved_type != NULL
        && resolved_type->kind == CM_TYPECK_TYPE_PROJECTION
        && cm_hir_def_id_equal(resolved_type->data.projection_type
                .trait_type.definition, inner_trait)
        && cm_hir_def_id_equal(resolved_type->data.projection_type
                .associated_type.definition, inner_associated));
    /* Equality mode keeps this projection-valued bound blocked even though
     * raw target mode materializes it for the structural normalizer. */
    {
        CmTraitGoal equality;
        CmTraitSelectionResult equality_result;

        equality = projection_goal(fixture.owner_trait, projection_type,
            runtime.u8_type);
        type_count = cm_typeck_type_count(&runtime.typeck);
        equality_result = cm_trait_solver_solve_projection_equality(
            &runtime.index, &runtime.environment, &runtime.typeck,
            &runtime.substitution, &equality.data.projection_equality,
            NULL);
        assert(equality_result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
            && equality_result.supported_match_count == 0u
            && equality_result.blocking_match_count == 1u);
        assert_cached_nonproven_clean(equality_result);
        assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    }
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* Unique impl replay returns a live target and exact associated proof. */
    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "ImplTarget");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "ImplTargetAssoc");
    impl_definition = add_bool_impl(&fixture, outer_trait);
    impl_associated = add_impl_associated(&fixture, impl_definition,
        outer_associated, fixture.u8_hir);
    projection_hir = add_projection_type(&fixture, outer_trait,
        outer_associated, fixture.bool_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    memset(&goal, 0, sizeof(goal));
    goal.owner = fixture.owner_trait;
    goal.projection_type = projection_type;
    target = cm_trait_solver_select_projection_target(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        &goal, NULL);
    assert(target.selection.kind == CM_TRAIT_SOLVER_PROVEN
        && target.selection.proof_origin == CM_TRAIT_PROOF_IMPL
        && cm_hir_def_id_equal(target.selection.impl_definition,
            impl_definition)
        && cm_hir_def_id_equal(
            target.selection.impl_associated_definition, impl_associated)
        && target.target != CM_TYPECK_TYPE_NONE);
    assert(cm_typeck_resolve(&runtime.typeck, target.target, &resolved)
        == CM_TYPECK_OK);
    resolved_type = cm_typeck_get_type(&runtime.typeck, resolved);
    assert(resolved_type != NULL
        && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
        && resolved_type->data.integer_type == CM_HIR_INT_U8);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* Conflicting applicable bounds never leak a target or probe terms. */
    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "ConflictTarget");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "ConflictTargetAssoc");
    values[0] = fixture.u8_hir;
    values[1] = fixture.bool_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture, outer_trait,
        outer_associated, (CmHirTypeId[]){fixture.bool_hir,
            fixture.bool_hir}, values, 2u, 0, "ConflictTargetOwner");
    projection_hir = add_projection_type(&fixture, outer_trait,
        outer_associated, fixture.bool_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    memset(&goal, 0, sizeof(goal));
    goal.owner = fixture.owner_trait;
    goal.projection_type = projection_type;
    type_count = cm_typeck_type_count(&runtime.typeck);
    target = cm_trait_solver_select_projection_target(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        &goal, NULL);
    assert(target.selection.kind == CM_TRAIT_SOLVER_AMBIGUOUS);
    assert_projection_target_nonproven_clean(target);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* A relevant HRTB equality blocks impl fallback transactionally. */
    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "BlockedTarget");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "BlockedTargetAssoc");
    values[0] = fixture.u8_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture, outer_trait,
        outer_associated, &fixture.bool_hir, values, 1u, 1,
        "BlockedTargetOwner");
    impl_definition = add_bool_impl(&fixture, outer_trait);
    (void)add_impl_associated(&fixture, impl_definition, outer_associated,
        fixture.u8_hir);
    projection_hir = add_projection_type(&fixture, outer_trait,
        outer_associated, fixture.bool_hir);
    runtime_init(&runtime, &fixture, limits);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    memset(&goal, 0, sizeof(goal));
    goal.owner = fixture.owner_trait;
    goal.projection_type = projection_type;
    type_count = cm_typeck_type_count(&runtime.typeck);
    target = cm_trait_solver_select_projection_target(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        &goal, NULL);
    assert(target.selection.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && target.selection.blocking_match_count == 1u);
    assert_projection_target_nonproven_clean(target);
    assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);

    /* Inference in projection Self defers without binding. A wrong owner is
     * rejected through the same scrubbed, mutation-free boundary. */
    fixture_init(&fixture, 0);
    outer_trait = add_trait(&fixture, "InferenceTarget");
    outer_associated = add_trait_associated(&fixture, outer_trait,
        "InferenceTargetAssoc");
    values[0] = fixture.u8_hir;
    fixture.owner_trait = add_owner_with_equalities(&fixture, outer_trait,
        outer_associated, &fixture.bool_hir, values, 1u, 0,
        "InferenceTargetOwner");
    runtime_init(&runtime, &fixture, limits);
    {
        CmTypeckType inference_projection;
        CmTypeckTypeId inference_self;

        assert(cm_typeck_new_variable(&runtime.typeck,
            CM_HIR_INFER_GENERAL, test_span(21u, 22u), &inference_self)
            == CM_TYPECK_OK);
        memset(&inference_projection, 0, sizeof(inference_projection));
        inference_projection.kind = CM_TYPECK_TYPE_PROJECTION;
        inference_projection.span = test_span(21u, 24u);
        inference_projection.data.projection_type.self_type =
            inference_self;
        inference_projection.data.projection_type.trait_type.definition =
            outer_trait;
        inference_projection.data.projection_type.associated_type.definition =
            outer_associated;
        assert(cm_typeck_add_type(&runtime.typeck, &inference_projection,
            &projection_type) == CM_TYPECK_OK);
        memset(&goal, 0, sizeof(goal));
        goal.owner = fixture.owner_trait;
        goal.projection_type = projection_type;
        type_count = cm_typeck_type_count(&runtime.typeck);
        target = cm_trait_solver_select_projection_target(&runtime.index,
            &runtime.environment, &runtime.typeck, &runtime.substitution,
            &goal, NULL);
        assert(target.selection.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE);
        assert_projection_target_nonproven_clean(target);
        assert_unbound(&runtime.typeck, inference_self);
        assert(cm_typeck_type_count(&runtime.typeck) == type_count);
        goal.owner = outer_trait;
        target = cm_trait_solver_select_projection_target(&runtime.index,
            &runtime.environment, &runtime.typeck, &runtime.substitution,
            &goal, NULL);
        assert(target.selection.kind == CM_TRAIT_SOLVER_INVALID);
        assert_projection_target_nonproven_clean(target);
        assert_unbound(&runtime.typeck, inference_self);
        assert(cm_typeck_type_count(&runtime.typeck) == type_count);
    }
    runtime_destroy(&runtime);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_structural_keys_and_nonproven_rollback();
    test_binder_and_region_canonicalization();
    test_uncacheable_const_inference();
    test_dag_shape_and_inference_aliasing();
    test_exact_depth_boundaries();
    test_bounded_dag_cycle_and_capacity();
    test_recursive_candidate_and_staleness();
    test_same_length_semantic_staleness();
    test_generic_predicate_proof_and_cycle_base();
    test_generic_predicate_cycle_ambiguity_and_rollback();
    test_predicate_nonproof_lattice_and_unconstrained();
    test_proven_and_nonproof_candidate_order();
    test_projection_equality_commit_and_rollback();
    test_projection_parameter_environment_precedence();
    test_projection_parameter_environment_candidates();
    test_projection_equality_ambiguity_order();
    test_projection_equality_defaults_and_foreign_metadata();
    test_projection_trait_argument_reallocation();
    test_projection_equality_recursion_and_blockers();
    test_projection_target_selection();
    test_destroyed_typeck_rejected();
    test_destroyed_index_and_environment_rejected();
    puts("hir canonical goal table tests passed");
    return 0;
}
