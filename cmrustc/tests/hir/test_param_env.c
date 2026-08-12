#include "cm/hir/param_env.h"
#include "cm/hir/trait_solver.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirDefId bound_trait;
    CmHirDefId bound_associated_type;
    CmHirDefId super_trait;
    CmHirDefId generic_trait;
    CmHirDefId outer_trait;
    CmHirDefId associated_type;
    CmHirGenericParamId generic_parameter;
    CmHirGenericParamId outer_parameter;
    CmHirGenericParamId associated_parameter;
    CmHirTypeId u8_hir;
    CmHirTypeId bool_hir;
} TestFixture;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static void init_item(CmHirItem *item, CmHirItemKind kind,
    CmHirDefId definition, CmHirModuleId module, const char *name,
    CmHirContext *hir)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = module;
    item->parent_definition = cm_hir_def_id_none();
    item->name = name == NULL ? CM_INTERN_ID_NONE : cm_hir_intern(hir, name);
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

static CmHirDefId add_empty_trait(TestFixture *fixture, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root, name,
        &fixture->hir);
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_associated_declaration(TestFixture *fixture,
    CmHirDefId trait_definition, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TYPE_ALIAS, test_span(2u, 6u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        name, &fixture->hir);
    item.span = test_span(2u, 6u);
    item.parent_definition = trait_definition;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirTypeId add_parameter_type(TestFixture *fixture,
    CmHirGenericParamId parameter)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(4u, 5u);
    type.data.parameter_type.parameter = parameter;
    assert(cm_hir_add_type(&fixture->hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirDefId add_generic_trait(TestFixture *fixture)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirTraitPredicate predicates[2];
    CmHirAssociatedTypeEquality equality;
    CmHirOutlivesPredicate outlives;
    CmHirSupertrait supertrait;
    CmHirTypeId parameter_type;
    CmHirTypeId reference_type;
    CmHirType type;
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
        &fixture->generic_parameter) == CM_HIR_OK);
    parameter_type = add_parameter_type(fixture,
        fixture->generic_parameter);

    memset(predicates, 0, sizeof(predicates));
    predicates[0].subject = parameter_type;
    predicates[0].trait_type.definition = fixture->bound_trait;
    memset(&equality, 0, sizeof(equality));
    equality.associated_type = fixture->bound_associated_type;
    equality.value = parameter_type;
    equality.span = test_span(5u, 7u);
    predicates[0].equalities = &equality;
    predicates[0].equality_count = 1u;
    predicates[0].span = test_span(4u, 8u);
    predicates[0].modifier = CM_HIR_PREDICATE_REQUIRED;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(4u, 8u);
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type.data.reference_type.pointee = parameter_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&fixture->hir, &type, &reference_type)
        == CM_HIR_OK);
    predicates[1].subject = reference_type;
    predicates[1].trait_type.definition = fixture->bound_trait;
    predicates[1].span = test_span(4u, 8u);
    predicates[1].modifier = CM_HIR_PREDICATE_REQUIRED;
    memset(&outlives, 0, sizeof(outlives));
    outlives.subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives.subject.type = parameter_type;
    outlives.bound.kind = CM_HIR_REGION_STATIC;
    outlives.span = test_span(9u, 12u);
    memset(&supertrait, 0, sizeof(supertrait));
    supertrait.trait_type.definition = fixture->super_trait;
    supertrait.span = test_span(13u, 16u);
    supertrait.modifier = CM_HIR_SUPERTRAIT_REQUIRED;

    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        "Generic", &fixture->hir);
    item.generic_parameter_start = fixture->generic_parameter;
    item.generic_parameter_count = 1u;
    item.predicates = predicates;
    item.predicate_count = 2u;
    item.outlives_predicates = &outlives;
    item.outlives_predicate_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = &supertrait;
    item.data.trait_item.supertrait_count = 1u;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_outer_trait(TestFixture *fixture)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&fixture->hir, "P");
    parameter.span = test_span(2u, 3u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &fixture->outer_parameter) == CM_HIR_OK);
    (void)add_parameter_type(fixture, fixture->outer_parameter);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        "Outer", &fixture->hir);
    item.generic_parameter_start = fixture->outer_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_mixed_associated_type(TestFixture *fixture)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirTypeId outer_type;
    CmHirTypeId associated_type;
    CmHirTypeId tuple_type;
    CmHirTypeId tuple_elements[2];
    CmHirType type;
    CmHirTraitPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TYPE_ALIAS, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&fixture->hir, "Q");
    parameter.span = test_span(2u, 3u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &fixture->associated_parameter) == CM_HIR_OK);
    outer_type = add_parameter_type(fixture, fixture->outer_parameter);
    associated_type = add_parameter_type(fixture,
        fixture->associated_parameter);
    tuple_elements[0] = outer_type;
    tuple_elements[1] = associated_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(4u, 8u);
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(&fixture->hir, &type, &tuple_type) == CM_HIR_OK);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = tuple_type;
    predicate.trait_type.definition = fixture->bound_trait;
    predicate.span = test_span(4u, 10u);
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        "Assoc", &fixture->hir);
    item.parent_definition = fixture->outer_trait;
    item.generic_parameter_start = fixture->associated_parameter;
    item.generic_parameter_count = 1u;
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void fixture_init(TestFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "param_env"), CM_HIR_EDITION_2024,
        test_span(0u, 30u), &fixture->crate_id, &fixture->root) == CM_HIR_OK);
    fixture->u8_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND, CM_HIR_INT_U8);
    fixture->bool_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_BOOL_KIND, CM_HIR_INT_U8);
    fixture->bound_trait = add_empty_trait(fixture, "Bound");
    fixture->bound_associated_type = add_associated_declaration(fixture,
        fixture->bound_trait, "Item");
    fixture->super_trait = add_empty_trait(fixture, "Super");
    fixture->generic_trait = add_generic_trait(fixture);
    fixture->outer_trait = add_outer_trait(fixture);
    fixture->associated_type = add_mixed_associated_type(fixture);
}

static CmTypeckNamedType trait_query(CmHirDefId definition)
{
    CmTypeckNamedType query;

    memset(&query, 0, sizeof(query));
    query.definition = definition;
    return query;
}

static void test_environment_facts_and_solver(void)
{
    TestFixture fixture;
    CmParamEnv environment;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckGenericArg generic_argument;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmImplementedTraitGoal goal;
    CmTraitSelectionResult result;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId reference_type;
    CmTypeckType reference;
    const CmParamEnvFact *fact;
    size_t fact_index;
    int saw_predicate;
    int saw_outlives;
    int saw_self;
    int saw_supertrait;
    int saw_equality_pending;
    int saw_outlives_pending;

    fixture_init(&fixture);
    memset(&environment, 0, sizeof(environment));
    assert(cm_param_env_init(&environment, &fixture.hir,
        fixture.generic_trait) == CM_PARAM_ENV_READY);
    assert(cm_param_env_fact_count(&environment) == 5u);
    assert(cm_param_env_pending_count(&environment) == 2u);
    saw_equality_pending = 0;
    saw_outlives_pending = 0;
    for (fact_index = 0u;
         fact_index < cm_param_env_pending_count(&environment);
         ++fact_index) {
        if (cm_param_env_pending(&environment, fact_index)->kind
                == CM_PARAM_ENV_PENDING_PROJECTION_EQUALITY) {
            saw_equality_pending = 1;
        } else if (cm_param_env_pending(&environment, fact_index)->kind
                == CM_PARAM_ENV_PENDING_OUTLIVES) {
            saw_outlives_pending = 1;
        }
    }
    assert(saw_equality_pending && saw_outlives_pending);
    saw_predicate = saw_outlives = saw_self = saw_supertrait = 0;
    for (fact_index = 0u;
         fact_index < cm_param_env_fact_count(&environment); ++fact_index) {
        fact = cm_param_env_fact(&environment, fact_index);
        assert(fact != NULL
            && cm_hir_def_id_equal(fact->source_owner,
                fixture.generic_trait));
        if (fact->provenance == CM_PARAM_ENV_PROVENANCE_EXACT_PREDICATE) {
            if (fact->data.implemented.equality_count != 0u) {
                assert(fact->data.implemented.equality_count == 1u
                    && cm_hir_def_id_equal(fact->data.implemented
                            .equalities[0].associated_type,
                        fixture.bound_associated_type));
            }
            saw_predicate = 1;
        } else if (fact->provenance
                == CM_PARAM_ENV_PROVENANCE_EXACT_OUTLIVES) {
            saw_outlives = 1;
        } else if (fact->provenance
                == CM_PARAM_ENV_PROVENANCE_TRAIT_SELF) {
            saw_self = 1;
        } else if (fact->provenance
                == CM_PARAM_ENV_PROVENANCE_SUPERTRAIT) {
            saw_supertrait = 1;
        }
    }
    assert(saw_predicate && saw_outlives && saw_self && saw_supertrait);

    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);
    memset(&generic_argument, 0, sizeof(generic_argument));
    generic_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    generic_argument.data.type = u8_type;
    memset(&exact, 0, sizeof(exact));
    exact.parameter_owner = fixture.generic_trait;
    exact.arguments = &generic_argument;
    exact.argument_count = 1u;
    exact.self_owner = fixture.generic_trait;
    exact.self_type = bool_type;
    substitution.exact = &exact;
    substitution.enclosing = NULL;

    memset(&goal, 0, sizeof(goal));
    goal.owner = fixture.generic_trait;
    goal.self_type = u8_type;
    goal.trait_type = trait_query(fixture.bound_trait);
    fact_index = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_hir_def_id_is_none(result.impl_definition)
        && cm_typeck_type_count(&typeck) == fact_index);

    memset(&reference, 0, sizeof(reference));
    reference.kind = CM_TYPECK_TYPE_REFERENCE;
    reference.span = test_span(4u, 8u);
    reference.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    reference.data.reference_type.pointee = u8_type;
    reference.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_typeck_add_type(&typeck, &reference, &reference_type)
        == CM_TYPECK_OK);
    goal.self_type = reference_type;
    fact_index = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && cm_typeck_type_count(&typeck) == fact_index + 1u);

    goal.self_type = bool_type;
    goal.trait_type = trait_query(fixture.super_trait);
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN);

    goal.trait_type = trait_query(fixture.generic_trait);
    goal.trait_type.arguments = &generic_argument;
    goal.trait_type.argument_count = 1u;
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN);

    goal.owner = fixture.outer_trait;
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    cm_param_env_destroy(&environment);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_mixed_owner_and_staleness(void)
{
    TestFixture fixture;
    CmParamEnv environment;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckGenericArg exact_argument;
    CmTypeckGenericArg enclosing_argument;
    CmTypeckInstantiation exact;
    CmTypeckInstantiation enclosing;
    CmParamEnvSubstitution substitution;
    CmImplementedTraitGoal goal;
    CmTraitSelectionResult result;
    CmTypeckType tuple;
    CmTypeckTypeId tuple_elements[2];
    CmTypeckTypeId tuple_type;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId bool_type;
    CmHirType appended;
    CmHirTypeId appended_id;
    size_t type_count;

    fixture_init(&fixture);
    memset(&environment, 0, sizeof(environment));
    assert(cm_param_env_init(&environment, &fixture.hir,
        fixture.associated_type) == CM_PARAM_ENV_READY);
    assert(cm_param_env_fact_count(&environment) == 2u);
    assert(cm_param_env_fact(&environment, 0u)->blocker_flags
        == CM_PARAM_ENV_BLOCK_MIXED_OWNER);
    assert(cm_param_env_pending_count(&environment) == 1u);
    assert(cm_param_env_pending(&environment, 0u)->kind
        == CM_PARAM_ENV_PENDING_MIXED_OWNER_SUBSTITUTION);

    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);
    memset(&exact_argument, 0, sizeof(exact_argument));
    exact_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    exact_argument.data.type = u8_type;
    enclosing_argument = exact_argument;
    memset(&exact, 0, sizeof(exact));
    exact.parameter_owner = fixture.associated_type;
    exact.arguments = &exact_argument;
    exact.argument_count = 1u;
    exact.self_owner = fixture.outer_trait;
    exact.self_type = bool_type;
    memset(&enclosing, 0, sizeof(enclosing));
    enclosing.parameter_owner = fixture.outer_trait;
    enclosing.arguments = &enclosing_argument;
    enclosing.argument_count = 1u;
    enclosing.self_owner = fixture.outer_trait;
    enclosing.self_type = bool_type;
    substitution.exact = &exact;
    substitution.enclosing = &enclosing;
    tuple_elements[0] = u8_type;
    tuple_elements[1] = u8_type;
    memset(&tuple, 0, sizeof(tuple));
    tuple.kind = CM_TYPECK_TYPE_TUPLE;
    tuple.span = test_span(4u, 8u);
    tuple.data.tuple_type.elements = tuple_elements;
    tuple.data.tuple_type.element_count = 2u;
    assert(cm_typeck_add_type(&typeck, &tuple, &tuple_type) == CM_TYPECK_OK);
    memset(&goal, 0, sizeof(goal));
    goal.owner = fixture.associated_type;
    goal.self_type = tuple_type;
    goal.trait_type = trait_query(fixture.bound_trait);
    type_count = cm_typeck_type_count(&typeck);
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_UNSUPPORTED
        && result.blocking_match_count == 1u
        && cm_typeck_type_count(&typeck) == type_count);

    memset(&appended, 0, sizeof(appended));
    appended.kind = CM_HIR_TYPE_UNIT_KIND;
    appended.span = test_span(2u, 3u);
    assert(cm_hir_add_type(&fixture.hir, &appended, &appended_id)
        == CM_HIR_OK);
    assert(!cm_param_env_is_current(&environment)
        && cm_param_env_fact_count(&environment) == 0u);
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);

    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    cm_param_env_destroy(&environment);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_dependency_overflow_is_not_projection(void)
{
    TestFixture fixture;
    CmHirDefId deep_trait;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirTypeId subject;
    CmHirType type;
    CmHirTraitPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;
    CmParamEnv environment;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckTypeId u8_type;
    CmTypeckTypeId bool_type;
    CmTypeckGenericArg argument;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmImplementedTraitGoal goal;
    CmTraitSelectionResult result;
    uint32_t depth;

    fixture_init(&fixture);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &deep_trait) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = deep_trait;
    parameter.name = cm_hir_intern(&fixture.hir, "D");
    parameter.span = test_span(2u, 3u);
    assert(cm_hir_add_generic_param(&fixture.hir, &parameter, &parameter_id)
        == CM_HIR_OK);
    subject = add_parameter_type(&fixture, parameter_id);
    for (depth = 0u; depth < 256u; ++depth) {
        memset(&type, 0, sizeof(type));
        type.kind = CM_HIR_TYPE_REFERENCE_KIND;
        type.span = test_span(4u, 8u);
        type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
        type.data.reference_type.pointee = subject;
        type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
        assert(cm_hir_add_type(&fixture.hir, &type, &subject) == CM_HIR_OK);
    }
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = subject;
    predicate.trait_type.definition = fixture.bound_trait;
    predicate.span = test_span(4u, 10u);
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    init_item(&item, CM_HIR_ITEM_TRAIT, deep_trait, fixture.root, "Deep",
        &fixture.hir);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    memset(&environment, 0, sizeof(environment));
    assert(cm_param_env_init(&environment, &fixture.hir, deep_trait)
        == CM_PARAM_ENV_READY);
    assert(cm_param_env_fact(&environment, 0u)->blocker_flags
        == CM_PARAM_ENV_BLOCK_OVERFLOW);
    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &fixture.hir,
        fixture.crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&typeck, fixture.u8_hir, &u8_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u8_type;
    memset(&exact, 0, sizeof(exact));
    exact.parameter_owner = deep_trait;
    exact.arguments = &argument;
    exact.argument_count = 1u;
    exact.self_owner = deep_trait;
    exact.self_type = bool_type;
    substitution.exact = &exact;
    substitution.enclosing = NULL;
    memset(&goal, 0, sizeof(goal));
    goal.owner = deep_trait;
    goal.self_type = u8_type;
    goal.trait_type = trait_query(fixture.bound_trait);
    result = cm_trait_solver_solve_implemented(&index, &environment,
        &typeck, &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW);

    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    cm_param_env_destroy(&environment);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_rewind_generation_stales_environment(void)
{
    TestFixture fixture;
    CmHirContextMark mark;
    CmParamEnv environment;
    size_t fact_count;

    fixture_init(&fixture);
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    memset(&environment, 0, sizeof(environment));
    assert(cm_param_env_init(&environment, &fixture.hir,
        fixture.generic_trait) == CM_PARAM_ENV_READY);
    fact_count = cm_param_env_fact_count(&environment);
    assert(fact_count != 0u);
    assert(cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK);
    assert(!cm_param_env_is_current(&environment)
        && cm_param_env_fact_count(&environment) == 0u);
    cm_param_env_destroy(&environment);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_semantic_generation_stales_environment(void)
{
    TestFixture fixture;
    CmParamEnv environment;
    CmHirAttribute attribute;

    fixture_init(&fixture);
    memset(&environment, 0, sizeof(environment));
    assert(cm_param_env_init(&environment, &fixture.hir,
        fixture.generic_trait) == CM_PARAM_ENV_READY);
    memset(&attribute, 0, sizeof(attribute));
    attribute.metadata = cm_hir_intern(&fixture.hir, "environment");
    attribute.span = test_span(1u, 2u);
    attribute.source_attribute = 1u;
    assert(cm_param_env_is_current(&environment));
    assert(cm_hir_set_crate_inner_attributes(&fixture.hir,
        fixture.crate_id, NULL, 0u) == CM_HIR_OK);
    assert(cm_param_env_is_current(&environment));
    assert(cm_hir_set_crate_inner_attributes(&fixture.hir,
        fixture.crate_id, &attribute, 1u) == CM_HIR_OK);
    assert(!cm_param_env_is_current(&environment));
    assert(cm_param_env_fact_count(&environment) == 0u);
    cm_param_env_destroy(&environment);
    cm_hir_context_destroy(&fixture.hir);
}

int main(void)
{
    test_environment_facts_and_solver();
    test_mixed_owner_and_staleness();
    test_dependency_overflow_is_not_projection();
    test_rewind_generation_stales_environment();
    test_semantic_generation_stales_environment();
    assert(strcmp(cm_param_env_status_name(CM_PARAM_ENV_READY), "ready")
        == 0);
    assert(strcmp(cm_param_env_pending_name(CM_PARAM_ENV_PENDING_OUTLIVES),
        "outlives") == 0);
    puts("hir parameter environment tests passed");
    return 0;
}
