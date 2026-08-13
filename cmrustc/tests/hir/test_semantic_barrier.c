#include "cm/hir/semantic_barrier.h"
#include "cm/hir/lower.h"
#include "cm/source.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    CmSourceSet sources;
    CmSourceId source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmHirContext hir;
    CmHirModuleMap modules;
} Fixture;

static void fixture_init(Fixture *fixture, const char *source)
{
    CmModuleGraphOptions graph_options;
    CmImportResult imports;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower;

    memset(fixture, 0, sizeof(*fixture));
    cm_source_set_init(&fixture->sources);
    assert(cm_source_add_memory(&fixture->sources, "barrier/lib.rs",
        (const unsigned char *)source, strlen(source), &fixture->source)
        == CM_SOURCE_OK);
    cm_cfg_set_init(&fixture->cfg);
    cm_module_graph_init(&fixture->graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2021;
    graph_options.cfg = &fixture->cfg;
    fixture->graph_result = cm_module_graph_build(&fixture->graph,
        &fixture->sources, fixture->source, &graph_options);
    assert(fixture->graph_result.error_count == 0u);
    cm_import_resolver_init(&fixture->imports);
    imports = cm_import_resolve(&fixture->imports, &fixture->graph,
        fixture->graph_result.revision);
    assert(imports.error_count == 0u);
    cm_hir_context_init(&fixture->hir);
    cm_hir_module_map_init(&fixture->modules);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "barrier_test";
    lower_options.edition = CM_HIR_EDITION_2021;
    lower = cm_hir_lower_module_graph(&fixture->hir, &fixture->graph,
        fixture->graph_result.revision, &fixture->imports,
        &fixture->modules, &lower_options);
    assert(lower.error_count == 0u && lower.crate_id == 1u);
}

static void fixture_destroy(Fixture *fixture)
{
    cm_hir_module_map_destroy(&fixture->modules);
    cm_hir_context_destroy(&fixture->hir);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
}

static CmSemanticBarrierResult init_barrier(Fixture *fixture,
    CmSemanticBarrier *barrier)
{
    return cm_semantic_barrier_init_structural(barrier, &fixture->hir, 1u,
        &fixture->graph, fixture->graph_result.revision,
        &fixture->imports, &fixture->modules);
}

static CmSemanticBarrierResult advance_typed(Fixture *fixture,
    CmSemanticBarrier *barrier)
{
    return cm_semantic_barrier_advance_typed(barrier, &fixture->graph,
        fixture->graph_result.revision, &fixture->imports,
        &fixture->modules);
}

static CmSemanticBarrierResult advance_marked(CmSemanticBarrier *barrier)
{
    return cm_semantic_barrier_advance_marked(barrier);
}

static CmHirExpr *mutable_expr(Fixture *fixture, CmHirExprId expression)
{
    if (expression == CM_HIR_EXPR_NONE
        || (size_t)expression > fixture->hir.expressions.len) return NULL;
    return (CmHirExpr *)cm_vec_at(&fixture->hir.expressions,
        (size_t)expression - 1u);
}

static void assert_manifest_evidence(const Fixture *fixture,
    CmHirValueUsage expected_or_unknown)
{
    size_t index;

    for (index = 0u; index < fixture->hir.expressions.len; ++index) {
        const CmHirExpr *expression;

        expression = (const CmHirExpr *)cm_vec_at_const(
            &fixture->hir.expressions, index);
        assert(expression != NULL && expression->usage != (CmHirValueUsage)99);
        if (expected_or_unknown == CM_HIR_USAGE_UNKNOWN) {
            assert(expression->usage == CM_HIR_USAGE_UNKNOWN
                && expression->static_borrow_state
                    == CM_HIR_STATIC_BORROW_UNKNOWN);
        } else {
            assert(expression->usage != CM_HIR_USAGE_UNKNOWN
                && expression->static_borrow_state
                    == CM_HIR_STATIC_BORROW_NOT_PROMOTED);
        }
    }
}

static void test_marked_usage_rules_and_dump(void)
{
    static const char source[] =
        "fn identity(value: u32) -> u32 { value } "
        "fn marked(left: u32, right: u32) -> u32 { "
        "if left == right { left + right } else { left - right } } "
        "fn copied(left: u32) -> u32 { let copy: u32 = left; copy } "
        "fn called(value: u32) -> u32 { identity(value) }";
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirBody *copied_body;
    const CmHirExpr *copied_root;
    const CmHirExpr *copy_initializer;
    const CmHirExpr *if_expression;
    const CmHirExpr *condition;
    const CmHirExpr *then_block;
    const CmHirExpr *then_binary;
    const CmHirExpr *else_block;
    const CmHirExpr *else_binary;
    const CmHirBody *called_body;
    const CmHirExpr *called_root;
    const CmHirExpr *called_call;
    uint64_t generation;
    uint64_t rewind_generation;
    uint64_t capability;
    FILE *stream;
    char dump[8192];
    size_t dump_size;

    fixture_init(&fixture, source);
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && cm_semantic_barrier_atom_count(&barrier) == 4u);
    result = advance_typed(&fixture, &barrier);
    generation = fixture.hir.semantic_generation;
    rewind_generation = fixture.hir.rewind_generation;
    capability = cm_semantic_barrier_capability_id(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && result.phase == CM_SEMANTIC_BARRIER_TYPED
        && capability != 0u);
    assert_manifest_evidence(&fixture, CM_HIR_USAGE_UNKNOWN);
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && result.phase == CM_SEMANTIC_BARRIER_MARKED
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_MARKED
        && cm_semantic_barrier_is_current(&barrier)
        && fixture.hir.semantic_generation == generation + UINT64_C(1)
        && fixture.hir.rewind_generation == rewind_generation
        && cm_semantic_barrier_generation(&barrier)
            == fixture.hir.semantic_generation
        && cm_semantic_barrier_capability_id(&barrier) != 0u
        && cm_semantic_barrier_capability_id(&barrier) != capability);
    assert_manifest_evidence(&fixture, CM_HIR_USAGE_MOVE);

    body = cm_hir_get_body(&fixture.hir, 2u);
    root = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    copied_body = cm_hir_get_body(&fixture.hir, 3u);
    copied_root = copied_body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, copied_body->root_expression);
    copy_initializer = copied_root == NULL
            || copied_root->kind != CM_HIR_EXPR_BLOCK
            || copied_root->data.block.statement_count != 1u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            copied_root->data.block.statements[0]
                .data.let_statement.initializer);
    if_expression = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    condition = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.condition);
    then_block = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.then_expression);
    else_block = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.else_expression);
    then_binary = then_block == NULL || then_block->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            then_block->data.block.tail_expression);
    else_binary = else_block == NULL || else_block->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            else_block->data.block.tail_expression);
    called_body = cm_hir_get_body(&fixture.hir, 4u);
    called_root = called_body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, called_body->root_expression);
    called_call = called_root == NULL
            || called_root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            called_root->data.block.tail_expression);
    assert(root != NULL && root->usage == CM_HIR_USAGE_MOVE
        && copy_initializer != NULL
        && copy_initializer->usage == CM_HIR_USAGE_BORROW
        && if_expression != NULL && if_expression->usage == CM_HIR_USAGE_MOVE
        && condition != NULL && condition->usage == CM_HIR_USAGE_BORROW
        && then_block != NULL && then_block->usage == CM_HIR_USAGE_MOVE
        && then_binary != NULL && then_binary->usage == CM_HIR_USAGE_MOVE
        && else_block != NULL && else_block->usage == CM_HIR_USAGE_MOVE
        && else_binary != NULL && else_binary->usage == CM_HIR_USAGE_MOVE
        && called_root != NULL && called_root->usage == CM_HIR_USAGE_MOVE
        && called_call != NULL && called_call->kind == CM_HIR_EXPR_CALL
        && called_call->usage == CM_HIR_USAGE_MOVE
        && called_call->data.call.argument_count == 1u
        && cm_hir_get_expr(&fixture.hir,
            called_call->data.call.arguments[0])->usage
            == CM_HIR_USAGE_MOVE);
    assert(condition->kind == CM_HIR_EXPR_BINARY
        && cm_hir_get_expr(&fixture.hir,
            condition->data.binary.left)->usage == CM_HIR_USAGE_BORROW
        && cm_hir_get_expr(&fixture.hir,
            condition->data.binary.right)->usage == CM_HIR_USAGE_BORROW
        && cm_hir_get_expr(&fixture.hir,
            then_binary->data.binary.right)->usage == CM_HIR_USAGE_MOVE
        && cm_hir_get_expr(&fixture.hir,
            else_binary->data.binary.right)->usage == CM_HIR_USAGE_MOVE);

    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &fixture.hir) == 0
        && fseek(stream, 0L, SEEK_SET) == 0);
    dump_size = fread(dump, 1u, sizeof(dump) - 1u, stream);
    dump[dump_size] = '\0';
    assert(strncmp(dump, "hir-v28\n", strlen("hir-v28\n")) == 0
        && strstr(dump, "usage=move static-borrow=not-promoted") != NULL
        && strstr(dump, "usage=borrow static-borrow=not-promoted") != NULL);
    assert(fclose(stream) == 0);
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_PHASE_ORDER
        && result.phase == CM_SEMANTIC_BARRIER_MARKED);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_marked_preflight_is_atomic(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmHirBody *body;
    CmHirExpr *root;
    CmHirExpr *tail;
    CmHirExpr saved_tail;
    CmHirExprId saved_root_tail;
    CmHirExprId initializer;
    uint64_t generation;
    uint64_t rewind_generation;
    uint64_t capability;

    fixture_init(&fixture,
        "fn first() -> u32 { 1u32 } "
        "fn second() -> u32 { let value: u32 = 2u32; value }");
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK);
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_PHASE_ORDER
        && result.phase == CM_SEMANTIC_BARRIER_STRUCTURAL);
    result = advance_typed(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK);
    body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies, 1u);
    root = body == NULL ? NULL
        : mutable_expr(&fixture, body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : mutable_expr(&fixture, root->data.block.tail_expression);
    assert(root != NULL && root->data.block.statement_count == 1u
        && tail != NULL);
    initializer = root->data.block.statements[0]
        .data.let_statement.initializer;
    saved_tail = *tail;
    saved_root_tail = root->data.block.tail_expression;
    generation = fixture.hir.semantic_generation;
    rewind_generation = fixture.hir.rewind_generation;
    capability = cm_semantic_barrier_capability_id(&barrier);

    tail->kind = CM_HIR_EXPR_BORROW_SHARED;
    tail->data.borrow_shared.operand = initializer;
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_HIR
        && result.phase == CM_SEMANTIC_BARRIER_TYPED);
    *tail = saved_tail;
    assert(fixture.hir.semantic_generation == generation
        && fixture.hir.rewind_generation == rewind_generation
        && cm_semantic_barrier_capability_id(&barrier) == capability
        && cm_semantic_barrier_phase(&barrier) == CM_SEMANTIC_BARRIER_TYPED);
    assert_manifest_evidence(&fixture, CM_HIR_USAGE_UNKNOWN);

    root->data.block.tail_expression = initializer;
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_HIR);
    root->data.block.tail_expression = saved_root_tail;
    assert(fixture.hir.semantic_generation == generation
        && cm_semantic_barrier_capability_id(&barrier) == capability);
    assert_manifest_evidence(&fixture, CM_HIR_USAGE_UNKNOWN);

    root->data.block.tail_expression = body->root_expression;
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_HIR);
    root->data.block.tail_expression = saved_root_tail;
    assert(fixture.hir.semantic_generation == generation
        && cm_semantic_barrier_capability_id(&barrier) == capability);

    tail->owner_body = 1u;
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_HIR);
    tail->owner_body = 2u;
    assert_manifest_evidence(&fixture, CM_HIR_USAGE_UNKNOWN);

    tail->usage = CM_HIR_USAGE_BORROW;
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_HIR);
    tail->usage = CM_HIR_USAGE_UNKNOWN;
    assert(fixture.hir.semantic_generation == generation
        && fixture.hir.rewind_generation == rewind_generation
        && cm_semantic_barrier_capability_id(&barrier) == capability
        && cm_semantic_barrier_phase(&barrier) == CM_SEMANTIC_BARRIER_TYPED);
    assert_manifest_evidence(&fixture, CM_HIR_USAGE_UNKNOWN);

    assert(tail->kind == CM_HIR_EXPR_LOCAL
        && tail->data.local.local_index < body->local_count);
    tail->data.local.local_index = body->local_count;
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_HIR
        && result.phase == CM_SEMANTIC_BARRIER_TYPED
        && fixture.hir.semantic_generation == generation
        && cm_semantic_barrier_capability_id(&barrier) == capability);
    tail->data.local.local_index = body->local_count - 1u;

    root->data.block.statement_count = UINT32_MAX;
    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_HIR
        && result.phase == CM_SEMANTIC_BARRIER_TYPED
        && fixture.hir.semantic_generation == generation
        && cm_semantic_barrier_capability_id(&barrier) == capability);
    root->data.block.statement_count = 1u;

    result = advance_marked(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && fixture.hir.semantic_generation == generation + UINT64_C(1)
        && cm_semantic_barrier_capability_id(&barrier) != capability);
    assert_manifest_evidence(&fixture, CM_HIR_USAGE_MOVE);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static int atom_equal(const CmSemanticAtomView *left,
    const CmSemanticAtomView *right)
{
    return left != NULL && right != NULL && left->kind == right->kind
        && cm_hir_def_id_equal(left->owner, right->owner)
        && left->body == right->body
        && left->declared_type == right->declared_type
        && left->source == right->source
        && left->source_expression == right->source_expression;
}

static void test_manifest_is_complete_stable_and_immutable(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmSemanticAtomView before[3];
    CmSemanticAtomView after;
    size_t index;

    fixture_init(&fixture,
        "fn first() -> i32 { 1 } "
        "trait Value { fn required() -> i32; fn provided() -> i32 { 2 } } "
        "#[cfg(any())] fn removed() -> i32 { true } "
        "fn last() -> i32 { 3 }");
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && result.phase == CM_SEMANTIC_BARRIER_STRUCTURAL
        && cm_semantic_barrier_is_current(&barrier)
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_STRUCTURAL
        && cm_semantic_barrier_atom_count(&barrier) == 3u);
    for (index = 0u; index < 3u; ++index) {
        assert(cm_semantic_barrier_atom_at(&barrier, index, &before[index])
                == CM_SEMANTIC_BARRIER_OK
            && before[index].kind == CM_SEMANTIC_ATOM_FUNCTION
            && before[index].body != CM_HIR_BODY_NONE
            && before[index].source == fixture.source
            && before[index].source_expression != 0u
            && cm_semantic_barrier_contains_body(&barrier,
                before[index].body, &after)
            && atom_equal(&before[index], &after));
    }
    result = advance_typed(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && result.phase == CM_SEMANTIC_BARRIER_TYPED
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_TYPED
        && cm_semantic_barrier_is_current(&barrier));
    for (index = 0u; index < 3u; ++index) {
        assert(cm_semantic_barrier_atom_at(&barrier, index, &after)
                == CM_SEMANTIC_BARRIER_OK
            && atom_equal(&before[index], &after));
    }
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_typed_success_and_phase_order(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmSemanticAtomView atom;
    uint64_t capability;

    fixture_init(&fixture,
        "fn first() -> i32 { 1 } fn second() -> i32 { 2 }");
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    capability = cm_semantic_barrier_capability_id(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK && capability != 0u);
    result = advance_typed(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && result.phase == CM_SEMANTIC_BARRIER_TYPED
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_TYPED
        && cm_semantic_barrier_capability_id(&barrier) != 0u
        && cm_semantic_barrier_capability_id(&barrier) != capability);
    result = advance_typed(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_PHASE_ORDER
        && result.phase == CM_SEMANTIC_BARRIER_TYPED
        && cm_semantic_barrier_atom_count(&barrier) == 2u
        && cm_semantic_barrier_atom_at(&barrier, 1u, &atom)
            == CM_SEMANTIC_BARRIER_OK);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_const_and_static_typed(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmSemanticAtomView atom;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *tail;

    fixture_init(&fixture,
        "fn ready() -> i32 { 1 } const VALUE: i32 = 2; "
        "static SLOT: i32 = 3;");
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && cm_semantic_barrier_atom_count(&barrier) == 3u
        && cm_semantic_barrier_atom_at(&barrier, 1u, &atom)
            == CM_SEMANTIC_BARRIER_OK
        && atom.kind == CM_SEMANTIC_ATOM_CONST
        && cm_semantic_barrier_atom_at(&barrier, 2u, &atom)
            == CM_SEMANTIC_BARRIER_OK
        && atom.kind == CM_SEMANTIC_ATOM_STATIC);
    result = advance_typed(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && result.phase == CM_SEMANTIC_BARRIER_TYPED
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_TYPED);
    body = cm_hir_get_body(&fixture.hir, 2u);
    root = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->local_count == 0u && body->parameter_count == 0u
        && root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && root->data.block.statement_count == 0u
        && tail != NULL && tail->kind == CM_HIR_EXPR_INTEGER
        && tail->type == body->expected_type
        && tail->data.integer.low_bits == UINT64_C(2));
    body = cm_hir_get_body(&fixture.hir, 3u);
    root = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && tail != NULL && tail->kind == CM_HIR_EXPR_INTEGER
        && tail->data.integer.low_bits == UINT64_C(3));
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_const_initializer_failure_rolls_back_all_bodies(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    size_t expressions;
    uint64_t capability;

    fixture_init(&fixture,
        "fn ready() -> i32 { 1 } const BAD: i32 = 2u32; "
        "static SLOT: i32 = 3;");
    expressions = fixture.hir.expressions.len;
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    capability = cm_semantic_barrier_capability_id(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK);
    result = advance_typed(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_BODY_FAILURE
        && result.phase == CM_SEMANTIC_BARRIER_STRUCTURAL
        && result.atom_index == 1u
        && result.atom.kind == CM_SEMANTIC_ATOM_CONST
        && result.local_bodies.body_result.status
            == CM_HIR_BODY_LOWER_INVALID_LITERAL
        && fixture.hir.expressions.len == expressions
        && cm_hir_get_body(&fixture.hir, 1u)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&fixture.hir, 2u)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&fixture.hir, 3u)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_semantic_barrier_is_current(&barrier)
        && cm_semantic_barrier_capability_id(&barrier) != capability);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_trait_default_failure_rolls_back_all_bodies(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmSemanticAtomView atom;
    size_t expressions;
    size_t types;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    uint64_t capability;

    fixture_init(&fixture,
        "fn helper(value: u32) -> u32 { value } "
        "trait Closed { fn bad(value: u32) -> u32 { helper(value) } } "
        "fn last() -> u32 { 2u32 }");
    expressions = fixture.hir.expressions.len;
    types = fixture.hir.types.len;
    semantic_generation = fixture.hir.semantic_generation;
    rewind_generation = fixture.hir.rewind_generation;
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    capability = cm_semantic_barrier_capability_id(&barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && capability != 0u
        && cm_semantic_barrier_atom_count(&barrier) == 3u
        && cm_semantic_barrier_atom_at(&barrier, 1u, &atom)
            == CM_SEMANTIC_BARRIER_OK
        && atom.kind == CM_SEMANTIC_ATOM_FUNCTION);
    result = advance_typed(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_BODY_FAILURE
        && result.phase == CM_SEMANTIC_BARRIER_STRUCTURAL
        && result.atom_index < 3u
        && result.atom.kind == CM_SEMANTIC_ATOM_FUNCTION
        && result.atom.body == result.local_bodies.body
        && result.local_bodies.body_result.status
            == CM_HIR_BODY_LOWER_UNSUPPORTED_BODY
        && fixture.hir.expressions.len == expressions
        && fixture.hir.types.len == types
        && fixture.hir.semantic_generation > semantic_generation
        && fixture.hir.rewind_generation > rewind_generation
        && cm_hir_get_body(&fixture.hir, 1u)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&fixture.hir, 2u)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&fixture.hir, 3u)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_semantic_barrier_is_current(&barrier)
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_STRUCTURAL
        && cm_semantic_barrier_capability_id(&barrier) != 0u
        && cm_semantic_barrier_capability_id(&barrier) != capability);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_typed_failure_rolls_back_and_remains_structural(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    size_t expressions;
    size_t types;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    uint64_t capability;

    fixture_init(&fixture,
        "fn first() -> i32 { 1 } fn bad() -> i32 { true }");
    expressions = fixture.hir.expressions.len;
    types = fixture.hir.types.len;
    semantic_generation = fixture.hir.semantic_generation;
    rewind_generation = fixture.hir.rewind_generation;
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK);
    capability = cm_semantic_barrier_capability_id(&barrier);
    result = advance_typed(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_BODY_FAILURE
        && fixture.hir.expressions.len == expressions
        && fixture.hir.types.len == types
        && fixture.hir.semantic_generation > semantic_generation
        && fixture.hir.rewind_generation > rewind_generation
        && cm_hir_get_body(&fixture.hir, 1u)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&fixture.hir, 2u)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_semantic_barrier_is_current(&barrier)
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_STRUCTURAL
        && cm_semantic_barrier_capability_id(&barrier) != 0u
        && cm_semantic_barrier_capability_id(&barrier) != capability
        && cm_semantic_barrier_generation(&barrier)
            == fixture.hir.semantic_generation
        && cm_semantic_barrier_atom_count(&barrier) == 2u);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_source_snapshot_generation_aba(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmImportResult imports;
    CmHirModuleMapEntry entries[2];
    uint64_t import_generation;
    uint64_t map_generation;
    size_t index;

    fixture_init(&fixture, "mod child {} fn value() -> i32 { 1 }");
    assert(cm_module_graph_lifetime_id(&fixture.graph) != UINT64_C(0)
        && cm_import_resolver_lifetime_id(&fixture.imports)
            != UINT64_C(0)
        && cm_import_resolver_graph_lifetime_id(&fixture.imports)
            == cm_module_graph_lifetime_id(&fixture.graph)
        && cm_hir_module_map_lifetime_id(&fixture.modules)
            != UINT64_C(0)
        && cm_hir_module_map_graph_lifetime_id(&fixture.modules)
            == cm_module_graph_lifetime_id(&fixture.graph)
        && cm_hir_module_map_hir_lifetime_id(&fixture.modules)
            == fixture.hir.storage.lifetime_id);
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && cm_semantic_barrier_is_current(&barrier));

    import_generation = cm_import_resolver_generation(&fixture.imports);
    imports = cm_import_resolve(&fixture.imports, &fixture.graph,
        fixture.graph_result.revision);
    assert(imports.error_count == 0u
        && cm_import_resolver_generation(&fixture.imports)
            == import_generation + UINT64_C(1)
        && !cm_semantic_barrier_is_current(&barrier));
    cm_semantic_barrier_destroy(&barrier);

    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && cm_module_graph_module_count(&fixture.graph) == 2u
        && cm_hir_module_map_count(&fixture.modules) == 2u);
    for (index = 0u; index < 2u; ++index) {
        assert(cm_hir_module_map_get(&fixture.modules, &fixture.graph,
            fixture.graph_result.revision, &fixture.hir, index,
            &entries[index]) == CM_HIR_MODULE_MAP_OK);
    }
    map_generation = cm_hir_module_map_generation(&fixture.modules);
    cm_hir_module_map_clear(&fixture.modules);
    for (index = 0u; index < 2u; ++index) {
        assert(cm_hir_module_map_bind(&fixture.modules, &fixture.graph,
            fixture.graph_result.revision, entries[index].module,
            &fixture.hir, entries[index].hir_module)
            == CM_HIR_MODULE_MAP_OK);
    }
    assert(cm_hir_module_map_generation(&fixture.modules)
            == map_generation + UINT64_C(3)
        && !cm_semantic_barrier_is_current(&barrier));
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_source_snapshot_lifetime_aba(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmModuleGraphOptions graph_options;
    CmImportResult imports;
    CmModuleId graph_root;
    const CmHirCrate *crate_value;
    uint64_t old_lifetime;

    fixture_init(&fixture, "fn value() -> i32 { 1 }");
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK);
    old_lifetime = cm_import_resolver_lifetime_id(&fixture.imports);
    cm_import_resolver_destroy(&fixture.imports);
    cm_import_resolver_init(&fixture.imports);
    imports = cm_import_resolve(&fixture.imports, &fixture.graph,
        fixture.graph_result.revision);
    assert(imports.error_count == 0u
        && cm_import_resolver_lifetime_id(&fixture.imports) != old_lifetime
        && !cm_semantic_barrier_is_current(&barrier));
    cm_semantic_barrier_destroy(&barrier);

    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && cm_module_graph_get_root(&fixture.graph, &graph_root));
    crate_value = cm_hir_get_crate(&fixture.hir, 1u);
    assert(crate_value != NULL);
    old_lifetime = cm_hir_module_map_lifetime_id(&fixture.modules);
    cm_hir_module_map_destroy(&fixture.modules);
    cm_hir_module_map_init(&fixture.modules);
    assert(cm_hir_module_map_bind(&fixture.modules, &fixture.graph,
            fixture.graph_result.revision, graph_root, &fixture.hir,
            crate_value->root_module) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lifetime_id(&fixture.modules) != old_lifetime
        && !cm_semantic_barrier_is_current(&barrier));
    cm_semantic_barrier_destroy(&barrier);

    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK);
    old_lifetime = cm_module_graph_lifetime_id(&fixture.graph);
    cm_module_graph_destroy(&fixture.graph);
    cm_module_graph_init(&fixture.graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2021;
    graph_options.cfg = &fixture.cfg;
    fixture.graph_result = cm_module_graph_build(&fixture.graph,
        &fixture.sources, fixture.source, &graph_options);
    assert(fixture.graph_result.error_count == 0u
        && fixture.graph_result.revision == UINT64_C(1)
        && cm_module_graph_lifetime_id(&fixture.graph) != old_lifetime
        && !cm_semantic_barrier_is_current(&barrier));
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_append_rewind_and_capability_aba(void)
{
    Fixture fixture;
    CmSemanticBarrier first;
    CmSemanticBarrier second;
    CmSemanticBarrierResult result;
    CmHirContextMark mark;
    CmHirType type;
    CmHirTypeId type_id;
    uint64_t first_capability;
    uint64_t second_capability;

    fixture_init(&fixture, "fn value() -> i32 { 1 }");
    memset(&first, 0, sizeof(first));
    result = init_barrier(&fixture, &first);
    first_capability = cm_semantic_barrier_capability_id(&first);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && first_capability != 0u);
    cm_semantic_barrier_destroy(&first);
    memset(&second, 0, sizeof(second));
    result = init_barrier(&fixture, &second);
    second_capability = cm_semantic_barrier_capability_id(&second);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && second_capability != 0u
        && second_capability != first_capability);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = (CmSpan){ fixture.source, 0u, 1u };
    assert(cm_hir_add_type(&fixture.hir, &type, &type_id) == CM_HIR_OK
        && !cm_semantic_barrier_is_current(&second));
    cm_semantic_barrier_destroy(&second);

    memset(&second, 0, sizeof(second));
    result = init_barrier(&fixture, &second);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK
        && cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK
        && !cm_semantic_barrier_is_current(&second));
    cm_semantic_barrier_destroy(&second);
    fixture_destroy(&fixture);
}

static void test_source_snapshots_stale_barrier(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmSemanticAtomView atom;

    fixture_init(&fixture, "fn value() -> i32 { 1 }");
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_OK
        && cm_semantic_barrier_is_current(&barrier));
    cm_hir_module_map_clear(&fixture.modules);
    memset(&atom, 0xA5, sizeof(atom));
    assert(!cm_semantic_barrier_is_current(&barrier)
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_NONE
        && cm_semantic_barrier_atom_at(&barrier, 0u, &atom)
            == CM_SEMANTIC_BARRIER_STALE
        && atom.kind == CM_SEMANTIC_ATOM_NONE
        && cm_hir_def_id_is_none(atom.owner));
    memset(&atom, 0xA5, sizeof(atom));
    assert(!cm_semantic_barrier_contains_body(&barrier, 1u, &atom)
        && atom.kind == CM_SEMANTIC_ATOM_NONE
        && cm_hir_def_id_is_none(atom.owner));
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&fixture);
}

static void test_structural_preflight_failure_is_read_only(void)
{
    Fixture fixture;
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    CmHirBody *body;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    size_t items;
    size_t bodies;
    size_t expressions;
    size_t types;

    fixture_init(&fixture, "fn value() -> i32 { 1 }");
    body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies, 0u);
    assert(body != NULL);
    body->owner = cm_hir_def_id_none();
    semantic_generation = fixture.hir.semantic_generation;
    rewind_generation = fixture.hir.rewind_generation;
    items = fixture.hir.items.len;
    bodies = fixture.hir.bodies.len;
    expressions = fixture.hir.expressions.len;
    types = fixture.hir.types.len;
    memset(&barrier, 0, sizeof(barrier));
    result = init_barrier(&fixture, &barrier);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_HIR
        && barrier.state == NULL
        && fixture.hir.semantic_generation == semantic_generation
        && fixture.hir.rewind_generation == rewind_generation
        && fixture.hir.items.len == items
        && fixture.hir.bodies.len == bodies
        && fixture.hir.expressions.len == expressions
        && fixture.hir.types.len == types);
    fixture_destroy(&fixture);
}

static void test_invalid_api_and_names(void)
{
    CmSemanticBarrier barrier;
    CmSemanticBarrierResult result;
    unsigned int status;
    unsigned int phase;

    memset(&barrier, 0, sizeof(barrier));
    result = cm_semantic_barrier_init_structural(&barrier, NULL, 1u, NULL,
        CM_MODULE_GRAPH_REVISION_NONE, NULL, NULL);
    assert(result.status == CM_SEMANTIC_BARRIER_INVALID_ARGUMENT
        && barrier.state == NULL);
    for (status = 0u;
         status <= (unsigned int)CM_SEMANTIC_BARRIER_PHASE_ORDER; ++status) {
        assert(strcmp(cm_semantic_barrier_status_name(
            (CmSemanticBarrierStatus)status), "unknown") != 0);
    }
    for (phase = 0u;
         phase <= (unsigned int)CM_SEMANTIC_BARRIER_VALIDATED; ++phase) {
        assert(strcmp(cm_semantic_barrier_phase_name(
            (CmSemanticBarrierPhase)phase), "unknown") != 0);
    }
    assert(strcmp(cm_semantic_barrier_phase_name(CM_SEMANTIC_BARRIER_MARKED),
        "marked") == 0);
}

int main(void)
{
    test_marked_usage_rules_and_dump();
    test_marked_preflight_is_atomic();
    test_manifest_is_complete_stable_and_immutable();
    test_typed_success_and_phase_order();
    test_const_and_static_typed();
    test_const_initializer_failure_rolls_back_all_bodies();
    test_trait_default_failure_rolls_back_all_bodies();
    test_typed_failure_rolls_back_and_remains_structural();
    test_append_rewind_and_capability_aba();
    test_source_snapshots_stale_barrier();
    test_source_snapshot_generation_aba();
    test_source_snapshot_lifetime_aba();
    test_structural_preflight_failure_is_read_only();
    test_invalid_api_and_names();
    puts("hir semantic barrier tests passed");
    return 0;
}
