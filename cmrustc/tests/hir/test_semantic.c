#include "cm/hir/semantic.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirDefId owner_trait;
    CmHirDefId bound_trait;
    CmHirDefId associated_owner;
    CmHirTypeId bool_hir;
    CmHirTypeId u8_hir;
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

static CmHirDefId add_trait(TestFixture *fixture, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_associated_owner(TestFixture *fixture)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TYPE_ALIAS, test_span(3u, 8u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        cm_hir_intern(&fixture->hir, "Associated"));
    item.parent_definition = fixture->owner_trait;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
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
        fixture->crate_id, test_span(3u, 8u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.parent_definition = trait_definition;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void add_bool_impl_associated(TestFixture *fixture,
    CmHirDefId trait_definition, CmHirDefId associated_definition,
    CmHirTypeId target)
{
    CmHirDefId impl_definition;
    CmHirDefId definition;
    const CmHirItem *trait_associated;
    CmHirItem item;
    CmHirItemId item_id;
    size_t item_index;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 20u),
        &impl_definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, impl_definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = fixture->bool_hir;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&fixture->hir,
        fixture->crate_id, test_span(4u, 9u), &definition) == CM_HIR_OK);
    trait_associated = NULL;
    for (item_index = 0u; item_index < fixture->hir.items.len;
         ++item_index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(
            &fixture->hir.items, item_index);
        if (candidate != NULL && cm_hir_def_id_equal(candidate->definition,
                associated_definition)) {
            trait_associated = candidate;
            break;
        }
    }
    assert(trait_associated != NULL);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        trait_associated->name);
    item.parent_definition = impl_definition;
    item.data.type_alias_item.target = target;
    item.data.type_alias_item.trait_item_definition = associated_definition;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
}

static void fixture_init(TestFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "semantic"), CM_HIR_EDITION_2024,
        test_span(0u, 30u), &fixture->crate_id, &fixture->root) == CM_HIR_OK);
    fixture->bool_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_BOOL_KIND, CM_HIR_INT_U8);
    fixture->u8_hir = add_scalar(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND, CM_HIR_INT_U8);
    fixture->owner_trait = add_trait(fixture, "Owner");
    fixture->bound_trait = add_trait(fixture, "Bound");
    fixture->associated_owner = add_associated_owner(fixture);
}

static void fixture_destroy(TestFixture *fixture)
{
    cm_hir_context_destroy(&fixture->hir);
}

static CmSemanticSessionOptions session_options(const TestFixture *fixture,
    CmHirDefId owner)
{
    CmSemanticSessionOptions options;

    cm_semantic_session_options_init(&options);
    options.local_crate = fixture->crate_id;
    options.exact_owner = owner;
    return options;
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

static void assert_invalid_result(CmTraitSelectionResult result)
{
    assert(result.kind == CM_TRAIT_SOLVER_INVALID);
    assert(result.proof_origin == CM_TRAIT_PROOF_NONE);
    assert(result.param_env_fact_index == CM_TRAIT_PROOF_FACT_NONE);
    assert(result.param_env_equality_index
        == CM_TRAIT_PROOF_EQUALITY_NONE);
    assert(cm_hir_def_id_is_none(result.impl_definition));
    assert(result.impl_item == CM_HIR_ITEM_NONE);
    assert(cm_hir_def_id_is_none(result.impl_associated_definition));
}

static void test_open_session_solve_and_accessors(void)
{
    TestFixture fixture;
    CmSemanticSessionOptions options;
    CmSemanticSession session;
    CmTypeckContext *typeck;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmTypeckTypeId bool_type;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmTypeckType projection;
    CmTypeckTypeId projection_type;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.owner_trait);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_semantic_session_is_current(&session));
    assert(cm_semantic_session_hir(&session) == &fixture.hir);
    assert(cm_semantic_session_local_crate(&session) == fixture.crate_id);
    assert(cm_hir_def_id_equal(cm_semantic_session_exact_owner(&session),
        fixture.owner_trait));
    assert(cm_hir_def_id_is_none(
        cm_semantic_session_enclosing_owner(&session)));
    assert(cm_semantic_session_universe(&session)
        == CM_TRAIT_IMPL_UNIVERSE_OPEN);

    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    assert(cm_typeck_import_hir_type(typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);
    cm_typeck_instantiation_init(typeck, &exact);
    exact.parameter_owner = fixture.owner_trait;
    exact.self_owner = fixture.owner_trait;
    exact.self_type = bool_type;
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &exact;

    goal = implemented_goal(fixture.owner_trait, fixture.owner_trait,
        bool_type);
    result = cm_semantic_session_solve_implemented(&session, typeck,
        &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.proof_origin == CM_TRAIT_PROOF_PARAM_ENV
        && result.param_env_fact_index != CM_TRAIT_PROOF_FACT_NONE
        && cm_hir_def_id_is_none(result.impl_definition));

    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        bool_type);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_session_solve_implemented(&session, typeck,
        &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    assert(cm_typeck_type_count(typeck) == type_count);

    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_TYPECK_TYPE_PROJECTION;
    projection.span = test_span(3u, 8u);
    projection.data.projection_type.self_type = bool_type;
    projection.data.projection_type.trait_type.definition =
        fixture.owner_trait;
    projection.data.projection_type.associated_type.definition =
        fixture.associated_owner;
    assert(cm_typeck_add_type(typeck, &projection, &projection_type)
        == CM_TYPECK_OK);
    goal = projection_goal(fixture.owner_trait, projection_type, bool_type);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_session_solve_goal(&session, typeck,
        &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && cm_hir_def_id_is_none(result.impl_definition)
        && result.impl_item == CM_HIR_ITEM_NONE
        && cm_hir_def_id_is_none(result.impl_associated_definition)
        && cm_typeck_type_count(typeck) == type_count);
    result = cm_semantic_session_solve_implemented(&session, typeck,
        &substitution, &goal);
    assert_invalid_result(result);
    assert(cm_typeck_type_count(typeck) == type_count);

    cm_semantic_session_destroy(&session);
    cm_semantic_session_destroy(&session);
    assert(!cm_semantic_session_is_current(&session));
    fixture_destroy(&fixture);
}

static void test_enclosing_owner_and_reinitialization(void)
{
    TestFixture fixture;
    CmSemanticSessionOptions options;
    CmSemanticSession session;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.associated_owner);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_hir_def_id_equal(cm_semantic_session_exact_owner(&session),
        fixture.associated_owner));
    assert(cm_hir_def_id_equal(
        cm_semantic_session_enclosing_owner(&session),
        fixture.owner_trait));
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    cm_semantic_session_destroy(&session);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_atomic_initialization_failures(void)
{
    TestFixture fixture;
    CmSemanticSessionOptions options;
    CmSemanticSession session;
    CmHirDefId reserved_owner;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.owner_trait);
    options.universe = CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);
    cm_semantic_session_destroy(&session);

    options = session_options(&fixture, fixture.owner_trait);
    options.local_crate = CM_HIR_CRATE_NONE;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);

    options = session_options(&fixture, cm_hir_def_id_none());
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(8u, 9u),
        &reserved_owner) == CM_HIR_OK);
    options = session_options(&fixture, reserved_owner);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);

    options = session_options(&fixture, fixture.owner_trait);
    options.goal_limits.max_goal_depth = 0u;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);
    cm_semantic_session_destroy(&session);

    options = session_options(&fixture, fixture.owner_trait);
    options.goal_limits.max_canonical_nodes = (size_t)UINT32_MAX;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);
    fixture_destroy(&fixture);
}

static void test_foreign_and_malformed_data_rejected(void)
{
    TestFixture fixture;
    TestFixture foreign_fixture;
    CmSemanticSessionOptions options;
    CmSemanticSession session;
    CmTypeckContext *typeck;
    CmTypeckContext foreign_typeck;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmTypeckTypeId bool_type;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    size_t type_count;

    fixture_init(&fixture);
    fixture_init(&foreign_fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.owner_trait);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(cm_typeck_import_hir_type(typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);
    cm_typeck_instantiation_init(typeck, &exact);
    exact.parameter_owner = fixture.owner_trait;
    exact.self_owner = fixture.owner_trait;
    exact.self_type = bool_type;
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &exact;
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        bool_type);
    type_count = cm_typeck_type_count(typeck);

    memset(&foreign_typeck, 0, sizeof(foreign_typeck));
    cm_typeck_context_init(&foreign_typeck, &foreign_fixture.hir);
    result = cm_semantic_session_solve_implemented(&session,
        &foreign_typeck, &substitution, &goal);
    assert_invalid_result(result);
    assert(cm_typeck_type_count(typeck) == type_count);

    exact.parameter_owner = fixture.bound_trait;
    result = cm_semantic_session_solve_implemented(&session, typeck,
        &substitution, &goal);
    assert_invalid_result(result);
    exact.parameter_owner = fixture.owner_trait;
    goal.data.implemented.trait_type.definition = cm_hir_def_id_none();
    result = cm_semantic_session_solve_implemented(&session, typeck,
        &substitution, &goal);
    assert_invalid_result(result);
    assert(cm_typeck_type_count(typeck) == type_count);

    cm_typeck_context_destroy(&foreign_typeck);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&foreign_fixture);
    fixture_destroy(&fixture);
}

static void test_append_and_rewind_staleness(void)
{
    TestFixture fixture;
    CmSemanticSessionOptions options;
    CmSemanticSession session;
    CmTypeckContext *typeck;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmTypeckTypeId bool_type;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirContextMark mark;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.owner_trait);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(cm_typeck_import_hir_type(typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);
    cm_typeck_instantiation_init(typeck, &exact);
    exact.parameter_owner = fixture.owner_trait;
    exact.self_owner = fixture.owner_trait;
    exact.self_type = bool_type;
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &exact;
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        bool_type);
    type_count = cm_typeck_type_count(typeck);
    (void)add_scalar(&fixture.hir, CM_HIR_TYPE_INTEGER_KIND, CM_HIR_INT_U16);
    assert(!cm_semantic_session_is_current(&session));
    assert(cm_semantic_session_typeck(&session) == NULL);
    result = cm_semantic_session_solve_implemented(&session, typeck,
        &substitution, &goal);
    assert_invalid_result(result);
    assert(type_count != 0u && cm_typeck_type_count(typeck) == 0u);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    options = session_options(&fixture, fixture.owner_trait);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK);
    assert(!cm_semantic_session_is_current(&session));
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_complete_session_authentication(void)
{
    TestFixture fixture;
    TestFixture foreign;
    CmHirCrateFinalization finalization;
    CmSemanticSessionOptions options;
    CmSemanticSession session;
    CmTypeckContext *typeck;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmTypeckTypeId bool_type;
    CmTraitGoal goal;
    CmTraitSelectionResult result;
    CmHirAttribute attribute;
    CmHirCrateId second_crate;
    CmHirModuleId second_root;

    fixture_init(&fixture);
    fixture_init(&foreign);
    assert(cm_hir_create_crate(&fixture.hir,
        cm_hir_intern(&fixture.hir, "second"), CM_HIR_EDITION_2024,
        test_span(0u, 30u), &second_crate, &second_root) == CM_HIR_OK);
    assert(second_crate != fixture.crate_id
        && second_root != CM_HIR_MODULE_NONE);
    memset(&finalization, 0, sizeof(finalization));
    memset(&session, 0, sizeof(session));
    assert(cm_hir_crate_finalization_init(&finalization, &fixture.hir,
        fixture.crate_id) == CM_HIR_OK);

    options = session_options(&fixture, fixture.owner_trait);
    options.universe =
        CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    assert(cm_semantic_session_universe(&session)
        == CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    assert(cm_typeck_import_hir_type(typeck, fixture.bool_hir, &bool_type)
        == CM_TYPECK_OK);
    cm_typeck_instantiation_init(typeck, &exact);
    exact.parameter_owner = fixture.owner_trait;
    exact.self_owner = fixture.owner_trait;
    exact.self_type = bool_type;
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &exact;
    goal = implemented_goal(fixture.owner_trait, fixture.bound_trait,
        bool_type);
    result = cm_semantic_session_solve_implemented(&session, typeck,
        &substitution, &goal);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    cm_semantic_session_destroy(&session);

    options = session_options(&fixture, fixture.owner_trait);
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);

    options = session_options(&foreign, foreign.owner_trait);
    options.universe =
        CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &foreign.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);

    options = session_options(&fixture, fixture.owner_trait);
    options.universe =
        CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    options.local_crate = second_crate;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);

    memset(&attribute, 0, sizeof(attribute));
    attribute.metadata = cm_hir_intern(&fixture.hir, "mutated");
    attribute.span = test_span(1u, 2u);
    attribute.source_attribute = 1u;
    assert(cm_hir_set_crate_inner_attributes(&fixture.hir,
        fixture.crate_id, &attribute, 1u) == CM_HIR_OK);
    options = session_options(&fixture, fixture.owner_trait);
    options.universe =
        CM_TRAIT_IMPL_UNIVERSE_SINGLE_LOCAL_CRATE_COMPLETE;
    options.finalization = &finalization;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_INVALID);
    assert(session.state == NULL);

    cm_semantic_session_destroy(&session);
    cm_hir_crate_finalization_destroy(&finalization);
    fixture_destroy(&foreign);
    fixture_destroy(&fixture);
}

static void test_session_projection_normalization(void)
{
    TestFixture fixture;
    CmHirDefId associated;
    CmSemanticSessionOptions options;
    CmSemanticSession session;
    CmTypeckContext *typeck;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmTypeckType projection;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId projection_type;
    CmProjectionNormalizeResult result;
    CmProjectionNormalizeTrace trace;
    const CmProjectionNormalizeStep *step;
    const CmTypeckType *normalized;
    size_t type_count;

    fixture_init(&fixture);
    associated = add_trait_associated(&fixture, fixture.bound_trait,
        "Output");
    add_bool_impl_associated(&fixture, fixture.bound_trait, associated,
        fixture.u8_hir);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.owner_trait);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL
        && cm_typeck_import_hir_type(typeck, fixture.bool_hir, &bool_type)
            == CM_TYPECK_OK);
    cm_typeck_instantiation_init(typeck, &exact);
    exact.parameter_owner = fixture.owner_trait;
    exact.self_owner = fixture.owner_trait;
    exact.self_type = bool_type;
    memset(&substitution, 0, sizeof(substitution));
    substitution.exact = &exact;
    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_TYPECK_TYPE_PROJECTION;
    projection.span = test_span(8u, 12u);
    projection.data.projection_type.self_type = bool_type;
    projection.data.projection_type.trait_type.definition =
        fixture.bound_trait;
    projection.data.projection_type.associated_type.definition = associated;
    assert(cm_typeck_add_type(typeck, &projection, &projection_type)
        == CM_TYPECK_OK);
    cm_projection_normalize_trace_init(&trace);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_session_normalize_type_traced(&session, typeck,
        &substitution, projection_type,
        (CmProjectionNormalizeLimits){64u, 0u}, &trace);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW
        && result.cause == CM_PROJECTION_NORMALIZE_CAUSE_PROJECTION_LIMIT
        && result.type == CM_TYPECK_TYPE_NONE
        && cm_projection_normalize_trace_count(&trace) == 0u
        && cm_typeck_type_count(typeck) == type_count);
    result = cm_semantic_session_normalize_type_traced(&session, typeck,
        &substitution, projection_type,
        (CmProjectionNormalizeLimits){64u, 1u}, &trace);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.projection_step_count == 1u
        && cm_projection_normalize_trace_count(&trace) == 1u);
    step = cm_projection_normalize_trace_step(&trace, 0u);
    assert(step != NULL && step->projection == projection_type
        && step->target == result.type
        && step->normalized_target == result.type
        && step->proof_origin == CM_TRAIT_PROOF_IMPL
        && step->param_env_fact_index == CM_TRAIT_PROOF_FACT_NONE
        && step->param_env_equality_index
            == CM_TRAIT_PROOF_EQUALITY_NONE
        && !cm_hir_def_id_is_none(step->impl_definition)
        && !cm_hir_def_id_is_none(step->impl_associated_definition));
    normalized = cm_typeck_get_type(typeck, result.type);
    assert(normalized != NULL && normalized->kind == CM_TYPECK_TYPE_INTEGER
        && normalized->data.integer_type == CM_HIR_INT_U8);
    result = cm_semantic_session_normalize_type_traced(&session, NULL,
        &substitution, projection_type,
        (CmProjectionNormalizeLimits){64u, 1u}, &trace);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE
        && cm_projection_normalize_trace_count(&trace) == 0u);
    result = cm_semantic_session_normalize_type(&session, typeck,
        &substitution, projection_type,
        (CmProjectionNormalizeLimits){64u, 1u});
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.projection_step_count == 1u);
    (void)add_scalar(&fixture.hir, CM_HIR_TYPE_INTEGER_KIND,
        CM_HIR_INT_U16);
    result = cm_semantic_session_normalize_type_traced(&session, typeck,
        &substitution, projection_type,
        (CmProjectionNormalizeLimits){64u, 1u}, &trace);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE
        && cm_projection_normalize_trace_count(&trace) == 0u);
    cm_projection_normalize_trace_destroy(&trace);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_open_session_solve_and_accessors();
    test_enclosing_owner_and_reinitialization();
    test_atomic_initialization_failures();
    test_foreign_and_malformed_data_rejected();
    test_append_and_rewind_staleness();
    test_complete_session_authentication();
    test_session_projection_normalization();
    puts("hir semantic session tests passed");
    return 0;
}
