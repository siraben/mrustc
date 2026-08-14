#include "cm/hir/semantic_body.h"
#include "cm/hir/body.h"

#include "../../src/hir/semantic_body_internal.h"
#include "../../src/hir/instance_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirTypeId u32_type;
    CmHirTypeId u8_type;
    CmHirTypeId bool_type;
    CmHirTypeId infer_type;
    CmHirDefId present_trait;
    CmHirDefId missing_trait;
    CmHirDefId present_impl;
    CmHirDefId present_associated;
    CmHirDefId present_impl_associated;
    CmHirDefId projected_callee;
    CmHirBodyId projected_callee_body;
    CmHirDefId projected_caller;
    CmHirBodyId projected_body;
    CmHirExprId projected_call;
    CmHirDefId present_callee;
    CmHirDefId missing_callee;
    CmHirBodyId present_callee_body;
    CmHirBodyId missing_callee_body;
    CmHirDefId present_caller;
    CmHirDefId missing_caller;
    CmHirBodyId present_body;
    CmHirBodyId missing_body;
    CmHirExprId present_call;
    CmHirExprId missing_call;
} TestFixture;

typedef struct WritebackProbe {
    const CmHirContext *hir;
    CmHirBodyId expected_body;
    CmSemanticBodyWritebackStatus result;
    size_t invocation_count;
    size_t owned_term_count;
    CmTypeckTypeId first_owned_term;
    CmTypeckTypeId binding_variable;
    CmTypeckTypeId added_term;
    size_t observed_type_count;
    int mutate_typeck;
    int require_integer_kind;
    CmHirIntType expected_integer_kind;
    int require_qualified_callable;
    int require_method_callable;
    int require_trait_arguments;
    CmHirDefId expected_trait;
    CmHirDefId expected_declared_callable;
    CmHirDefId expected_impl;
    CmHirDefId expected_selected_callable;
    uint32_t expected_enclosing_impl_argument_count;
    CmHirIntType expected_enclosing_impl_integer_kind;
    uint32_t expected_callable_argument_count;
    CmHirIntType expected_callable_integer_kind;
    CmHirIntType expected_trait_argument_integer_kind;
} WritebackProbe;

static void fixture_init(TestFixture *fixture);
static void fixture_destroy(TestFixture *fixture);
static CmHirItem *mutable_item(TestFixture *fixture,
    CmHirDefId definition);
static CmSemanticSessionOptions session_options(const TestFixture *fixture,
    CmHirDefId owner);

static CmSemanticBodyWritebackStatus probe_writeback(void *context,
    CmSemanticSession *session, CmHirBodyId body,
    const CmSemanticCheckedBodyFacts *facts)
{
    WritebackProbe *probe;
    const CmTypeckContext *typeck;
    const CmTypeckTypeId *expression_terms;
    size_t expression_term_count;
    size_t index;

    probe = (WritebackProbe *)context;
    expression_terms = facts == NULL ? NULL : facts->expression_terms;
    expression_term_count = facts == NULL ? 0u
        : facts->expression_term_count;
    assert(probe != NULL && session != NULL
        && cm_semantic_session_is_current(session)
        && cm_semantic_session_hir(session) == probe->hir
        && body == probe->expected_body
        && expression_terms != NULL
        && expression_term_count == probe->hir->expressions.len);
    typeck = cm_semantic_session_typeck(session);
    assert(typeck != NULL);
    probe->observed_type_count = cm_typeck_type_count(typeck);
    if (probe->require_qualified_callable) {
        const CmSemanticCheckedCallableFacts *callable;
        const CmTypeckType *resolved_type;
        uint32_t parameter_index;

        assert(facts->call_count == 0u && facts->callable_count == 1u
            && facts->callables != NULL);
        callable = &facts->callables[0];
        assert(callable->syntax == CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD
            && cm_hir_def_id_equal(callable->requested_trait,
                probe->expected_trait)
            && cm_hir_def_id_equal(callable->declared_trait_callable,
                probe->expected_declared_callable)
            && cm_hir_def_id_equal(callable->selected_impl,
                probe->expected_impl)
            && cm_hir_def_id_equal(callable->selected_callable,
                probe->expected_selected_callable)
            && cm_hir_def_id_equal(callable->body_definition,
                probe->expected_selected_callable)
            && callable->receiver_argument == 0u
            && callable->argument_count
                == probe->expected_callable_argument_count
            && callable->parameter_count
                == probe->expected_callable_argument_count
            && callable->argument_expressions != NULL
            && callable->parameter_types != NULL
            && callable->receiver_expression
                == callable->argument_expressions[0]
            && callable->requested_self_type != CM_TYPECK_TYPE_NONE
            && callable->return_type != CM_TYPECK_TYPE_NONE);
        assert(callable->item_argument_count == 0u
            && callable->item_argument_inputs == NULL
            && callable->item_arguments == NULL
            && callable->method_argument_count == 0u
            && callable->method_argument_inputs == NULL
            && callable->method_arguments == NULL
            && callable->implemented_trait_argument_count
                == (probe->require_trait_arguments ? 1u : 0u)
            && (probe->require_trait_arguments
                ? callable->implemented_trait_argument_inputs != NULL
                    && callable->implemented_trait_arguments != NULL
                : callable->implemented_trait_argument_inputs == NULL
                    && callable->implemented_trait_arguments == NULL));
        if (probe->require_trait_arguments) {
            const CmTypeckType *argument_type;

            argument_type = cm_typeck_get_type(typeck,
                callable->implemented_trait_arguments[0].data.type);
            assert(callable->implemented_trait_arguments[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE
                && callable->implemented_trait_argument_inputs[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE
                && argument_type != NULL
                && argument_type->kind == CM_TYPECK_TYPE_INTEGER
                && argument_type->data.integer_type
                    == probe->expected_trait_argument_integer_kind);
        }
        assert(callable->enclosing_impl_argument_count
                == probe->expected_enclosing_impl_argument_count);
        if (callable->enclosing_impl_argument_count != 0u) {
            const CmTypeckType *argument_type;

            assert(callable->enclosing_impl_arguments != NULL
                && callable->enclosing_impl_argument_inputs != NULL
                && callable->enclosing_impl_arguments[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE
                && callable->enclosing_impl_argument_inputs[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE);
            argument_type = cm_typeck_get_type(typeck,
                callable->enclosing_impl_arguments[0].data.type);
            assert(argument_type != NULL
                && argument_type->kind == CM_TYPECK_TYPE_INTEGER
                && argument_type->data.integer_type
                    == probe->expected_enclosing_impl_integer_kind);
        }
        resolved_type = cm_typeck_get_type(typeck, callable->return_type);
        assert(resolved_type != NULL
            && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
            && resolved_type->data.integer_type
                == probe->expected_callable_integer_kind);
        for (parameter_index = 0u;
             parameter_index < callable->parameter_count;
             ++parameter_index) {
            resolved_type = cm_typeck_get_type(typeck,
                callable->parameter_types[parameter_index]);
            assert(resolved_type != NULL
                && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
                && resolved_type->data.integer_type
                    == probe->expected_callable_integer_kind);
        }
    }
    if (probe->require_method_callable) {
        const CmSemanticCheckedCallableFacts *callable;
        const CmTypeckType *resolved_type;
        uint32_t parameter_index;

        assert(facts->call_count == 0u && facts->callable_count == 1u
            && facts->callables != NULL);
        callable = &facts->callables[0];
        assert(callable->syntax == CM_HIR_CALLABLE_DOT_METHOD
            && cm_hir_def_id_equal(callable->requested_trait,
                probe->expected_trait)
            && cm_hir_def_id_equal(callable->declared_trait_callable,
                probe->expected_declared_callable)
            && cm_hir_def_id_equal(callable->selected_impl,
                probe->expected_impl)
            && cm_hir_def_id_equal(callable->selected_callable,
                probe->expected_selected_callable)
            && cm_hir_def_id_equal(callable->body_definition,
                probe->expected_selected_callable)
            && callable->receiver_argument == 0u
            && callable->argument_count
                == probe->expected_callable_argument_count
            && callable->parameter_count
                == probe->expected_callable_argument_count
            && callable->argument_expressions != NULL
            && callable->parameter_types != NULL
            && callable->receiver_expression
                == callable->argument_expressions[0]
            && callable->requested_self_type != CM_TYPECK_TYPE_NONE
            && callable->return_type != CM_TYPECK_TYPE_NONE);
        assert(callable->item_argument_count == 0u
            && callable->item_argument_inputs == NULL
            && callable->item_arguments == NULL
            && callable->method_argument_count == 0u
            && callable->method_argument_inputs == NULL
            && callable->method_arguments == NULL
            && callable->implemented_trait_argument_count
                == (probe->require_trait_arguments ? 1u : 0u)
            && (probe->require_trait_arguments
                ? callable->implemented_trait_argument_inputs != NULL
                    && callable->implemented_trait_arguments != NULL
                : callable->implemented_trait_argument_inputs == NULL
                    && callable->implemented_trait_arguments == NULL));
        if (probe->require_trait_arguments) {
            const CmTypeckType *argument_type;

            argument_type = cm_typeck_get_type(typeck,
                callable->implemented_trait_arguments[0].data.type);
            assert(callable->implemented_trait_arguments[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE
                && callable->implemented_trait_argument_inputs[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE
                && argument_type != NULL
                && argument_type->kind == CM_TYPECK_TYPE_INTEGER
                && argument_type->data.integer_type
                    == probe->expected_trait_argument_integer_kind);
        }
        assert(callable->enclosing_impl_argument_count
                == probe->expected_enclosing_impl_argument_count);
        if (callable->enclosing_impl_argument_count != 0u) {
            const CmTypeckType *argument_type;

            assert(callable->enclosing_impl_arguments != NULL
                && callable->enclosing_impl_argument_inputs != NULL
                && callable->enclosing_impl_arguments[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE
                && callable->enclosing_impl_argument_inputs[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE);
            argument_type = cm_typeck_get_type(typeck,
                callable->enclosing_impl_arguments[0].data.type);
            assert(argument_type != NULL
                && argument_type->kind == CM_TYPECK_TYPE_INTEGER
                && argument_type->data.integer_type
                    == probe->expected_enclosing_impl_integer_kind);
        }
        resolved_type = cm_typeck_get_type(typeck, callable->return_type);
        assert(resolved_type != NULL
            && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
            && resolved_type->data.integer_type
                == probe->expected_callable_integer_kind);
        for (parameter_index = 0u;
             parameter_index < callable->parameter_count;
             ++parameter_index) {
            resolved_type = cm_typeck_get_type(typeck,
                callable->parameter_types[parameter_index]);
            assert(resolved_type != NULL
                && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
                && resolved_type->data.integer_type
                    == probe->expected_callable_integer_kind);
        }
    }
    if (probe->require_integer_kind) {
        CmTypeckTypeId resolved;
        const CmTypeckType *resolved_type;
        size_t call_index;
        uint32_t parameter_index;

        assert(facts->signature_return_type != CM_TYPECK_TYPE_NONE
            && facts->signature_parameter_count != 0u
            && facts->signature_parameter_types != NULL
            && cm_typeck_resolve(typeck, facts->signature_return_type,
                &resolved) == CM_TYPECK_OK);
        resolved_type = cm_typeck_get_type(typeck, resolved);
        assert(resolved_type != NULL
            && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
            && resolved_type->data.integer_type
                == probe->expected_integer_kind);
        for (parameter_index = 0u;
             parameter_index < facts->signature_parameter_count;
             ++parameter_index) {
            assert(cm_typeck_resolve(typeck,
                    facts->signature_parameter_types[parameter_index],
                    &resolved) == CM_TYPECK_OK);
            resolved_type = cm_typeck_get_type(typeck, resolved);
            assert(resolved_type != NULL
                && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
                && resolved_type->data.integer_type
                    == probe->expected_integer_kind);
        }
        for (call_index = 0u; call_index < facts->call_count; ++call_index) {
            const CmSemanticCheckedCallFacts *call;

            call = &facts->calls[call_index];
            assert(call->return_type != CM_TYPECK_TYPE_NONE
                && (call->parameter_count == 0u)
                    == (call->parameter_types == NULL)
                && cm_typeck_resolve(typeck, call->return_type,
                    &resolved) == CM_TYPECK_OK);
            resolved_type = cm_typeck_get_type(typeck, resolved);
            assert(resolved_type != NULL
                && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
                && resolved_type->data.integer_type
                    == probe->expected_integer_kind);
            for (parameter_index = 0u;
                 parameter_index < call->parameter_count;
                 ++parameter_index) {
                assert(cm_typeck_resolve(typeck,
                        call->parameter_types[parameter_index], &resolved)
                    == CM_TYPECK_OK);
                resolved_type = cm_typeck_get_type(typeck, resolved);
                assert(resolved_type != NULL
                    && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
                    && resolved_type->data.integer_type
                        == probe->expected_integer_kind);
            }
        }
    }
    probe->invocation_count += 1u;
    probe->owned_term_count = 0u;
    probe->first_owned_term = CM_TYPECK_TYPE_NONE;
    for (index = 0u; index < expression_term_count; ++index) {
        const CmHirExpr *expression;
        CmTypeckTypeId resolved;

        expression = cm_hir_get_expr(probe->hir,
            (CmHirExprId)(index + 1u));
        assert(expression != NULL);
        if (expression->owner_body != body) {
            assert(expression_terms[index] == CM_TYPECK_TYPE_NONE);
            continue;
        }
        assert(expression_terms[index] != CM_TYPECK_TYPE_NONE
            && cm_typeck_resolve(typeck, expression_terms[index], &resolved)
                == CM_TYPECK_OK
            && cm_typeck_get_type(typeck, resolved) != NULL);
        if (probe->require_integer_kind) {
            const CmTypeckType *resolved_type;

            resolved_type = cm_typeck_get_type(typeck, resolved);
            assert(resolved_type->kind == CM_TYPECK_TYPE_INTEGER
                && resolved_type->data.integer_type
                    == probe->expected_integer_kind);
        }
        if (probe->first_owned_term == CM_TYPECK_TYPE_NONE) {
            probe->first_owned_term = expression_terms[index];
        }
        probe->owned_term_count += 1u;
    }
    assert(probe->owned_term_count != 0u
        && probe->first_owned_term != CM_TYPECK_TYPE_NONE);
    if (probe->mutate_typeck) {
        CmTypeckContext *mutable_typeck;
        CmTypeckType added;

        mutable_typeck = cm_semantic_session_typeck(session);
        memset(&added, 0, sizeof(added));
        added.kind = CM_TYPECK_TYPE_UNIT;
        added.span.source = 1u;
        added.span.start = 2u;
        added.span.end = 5u;
        assert(probe->binding_variable != CM_TYPECK_TYPE_NONE
            && cm_typeck_unify(mutable_typeck, probe->binding_variable,
                probe->first_owned_term) == CM_TYPECK_OK
            && cm_typeck_add_type(mutable_typeck, &added,
                &probe->added_term) == CM_TYPECK_OK);
    }
    return probe->result;
}

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
    item->name = name == NULL ? CM_INTERN_ID_NONE
        : cm_hir_intern(hir, name);
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = test_span(1u, 240u);
}

static CmHirTypeId add_type(CmHirContext *hir, CmHirTypeKind kind)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(2u, 5u);
    if (kind == CM_HIR_TYPE_INTEGER_KIND) {
        type.data.integer_type.kind = CM_HIR_INT_U32;
    } else if (kind == CM_HIR_TYPE_INFER_KIND) {
        type.data.infer_type.kind = CM_HIR_INFER_GENERAL;
        type.data.infer_type.variable = 1u;
    }
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirTypeId add_integer_type(CmHirContext *hir,
    CmHirIntType integer_kind)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(2u, 5u);
    type.data.integer_type.kind = integer_kind;
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirTypeId add_parameter_type(CmHirContext *hir,
    CmHirGenericParamId parameter)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(2u, 5u);
    type.data.parameter_type.parameter = parameter;
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirTypeId add_static_reference_type(CmHirContext *hir,
    CmHirTypeId pointee)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(2u, 5u);
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type.data.reference_type.pointee = pointee;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirTypeId add_pair_type(CmHirContext *hir, CmHirTypeId first,
    CmHirTypeId second)
{
    CmHirType type;
    CmHirTypeId elements[2];
    CmHirTypeId id;

    elements[0] = first;
    elements[1] = second;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(2u, 5u);
    type.data.tuple_type.elements = elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirStatus add_local_expression(CmHirContext *hir,
    CmHirBodyId body, uint32_t local_index, CmHirTypeId type, CmSpan span,
    CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.local.local_index = local_index;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirStatus add_call_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirDefId callee,
    const CmHirTypeId *type_substitutions, uint32_t substitution_count,
    const CmHirExprId *arguments, uint32_t argument_count,
    CmHirTypeId result_type, CmSpan span, CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = body;
    expression.type = result_type;
    expression.span = span;
    expression.data.call.callee = callee;
    expression.data.call.type_substitutions =
        (CmHirTypeId *)type_substitutions;
    expression.data.call.type_substitution_count = substitution_count;
    expression.data.call.arguments = (CmHirExprId *)arguments;
    expression.data.call.argument_count = argument_count;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirStatus add_integer_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirTypeId type, uint64_t value, CmSpan span,
    CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.integer.low_bits = value;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirStatus add_block_expression(CmHirContext *hir,
    CmHirBodyId body, const CmHirStatement *statements,
    uint32_t statement_count, CmHirExprId tail, CmHirTypeId type,
    CmSpan span, CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BLOCK;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.block.statements = (CmHirStatement *)statements;
    expression.data.block.statement_count = statement_count;
    expression.data.block.tail_expression = tail;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirStatus add_binary_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirBinaryOperator operator_kind,
    CmHirExprId left, CmHirExprId right, CmHirTypeId type, CmSpan span,
    CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.binary.operator_kind = operator_kind;
    expression.data.binary.left = left;
    expression.data.binary.right = right;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirStatus add_if_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirExprId condition, CmHirExprId then_expression,
    CmHirExprId else_expression, CmHirTypeId type, CmSpan span,
    CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_IF;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.if_expr.condition = condition;
    expression.data.if_expr.then_expression = then_expression;
    expression.data.if_expr.else_expression = else_expression;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirStatus add_aggregate_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirDefId definition,
    const CmHirAggregateFieldValue *fields, uint32_t field_count,
    CmHirTypeId type, CmSpan span, CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_AGGREGATE;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.aggregate.definition = definition;
    expression.data.aggregate.fields = (CmHirAggregateFieldValue *)fields;
    expression.data.aggregate.field_count = field_count;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirStatus add_field_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirExprId base, CmHirDefId definition,
    uint32_t field_index, CmHirTypeId type, CmSpan span,
    CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_FIELD;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.field.base = base;
    expression.data.field.definition = definition;
    expression.data.field.field_index = field_index;
    return cm_hir_add_expr(hir, &expression, out_expression);
}

static CmHirDefId add_trait(TestFixture *fixture, const char *name)
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

static void test_tuple_parameter_definition_lockstep(void)
{
    TestFixture fixture;
    CmHirDefId definition;
    CmHirType tuple_value;
    CmHirTypeId tuple_elements[2];
    CmHirTypeId tuple_type;
    CmHirFunctionParameter parameter;
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirBody *stored_body;
    CmHirBodyId body_id;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId root;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmInternId saved_name;
    CmSpan saved_span;

    fixture_init(&fixture);
    tuple_elements[0] = fixture.u32_type;
    tuple_elements[1] = fixture.u32_type;
    memset(&tuple_value, 0, sizeof(tuple_value));
    tuple_value.kind = CM_HIR_TYPE_TUPLE_KIND;
    tuple_value.span = test_span(10u, 20u);
    tuple_value.data.tuple_type.elements = tuple_elements;
    tuple_value.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(&fixture.hir, &tuple_value, &tuple_type)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(10u, 38u),
        &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.type = tuple_type;
    parameter.span = test_span(14u, 28u);
    parameter.binding_kind = CM_HIR_BINDING_TUPLE_PATTERN;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameter.tuple_bindings[0].name =
        cm_hir_intern(&fixture.hir, "left");
    parameter.tuple_bindings[0].span = test_span(15u, 19u);
    parameter.tuple_bindings[1].name =
        cm_hir_intern(&fixture.hir, "right");
    parameter.tuple_bindings[1].span = test_span(21u, 26u);
    memset(locals, 0, sizeof(locals));
    locals[0].name = parameter.tuple_bindings[0].name;
    locals[0].type = tuple_elements[0];
    locals[0].span = parameter.tuple_bindings[0].span;
    locals[0].parameter_index = 0u;
    locals[0].parameter_binding_index = 0u;
    locals[1].name = parameter.tuple_bindings[1].name;
    locals[1].type = tuple_elements[1];
    locals[1].span = parameter.tuple_bindings[1].span;
    locals[1].parameter_index = 0u;
    locals[1].parameter_binding_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = fixture.u32_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 10u;
    body.span = test_span(10u, 38u);
    assert(cm_hir_add_body(&fixture.hir, &body, &body_id) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, definition, fixture.root,
        "tuple_first", &fixture.hir);
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = fixture.u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK
        && add_local_expression(&fixture.hir, body_id, 0u,
            fixture.u32_type, test_span(30u, 34u), &root) == CM_HIR_OK
        && cm_hir_set_body_root_expression(&fixture.hir, body_id, root)
            == CM_HIR_OK
        && cm_hir_body_function_owner_kind(&fixture.hir,
            cm_hir_get_item(&fixture.hir, item_id))
            == CM_HIR_BODY_FUNCTION_OWNER_FREE);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_OK);

    stored_body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
        (size_t)body_id - 1u);
    assert(stored_body != NULL);
    stored_body->locals[1].parameter_binding_index = 0u;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    stored_body->locals[1].parameter_binding_index = 1u;
    saved_name = stored_body->locals[1].name;
    stored_body->locals[1].name = stored_body->locals[0].name;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    stored_body->locals[1].name = saved_name;
    saved_span = stored_body->locals[1].span;
    stored_body->locals[1].span.start += 1u;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    stored_body->locals[1].span = saved_span;
    stored_body->locals[1].type = fixture.bool_type;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH);
    stored_body->locals[1].type = fixture.u32_type;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_OK);

    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_newtype_parameter_definition_lockstep(void)
{
    TestFixture fixture;
    CmHirDefId newtype_definition;
    CmHirDefId function_definition;
    CmHirGenericParam generic;
    CmHirGenericParamId generic_id;
    CmHirTypeId generic_type;
    CmHirField field;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirGenericArg argument;
    CmHirType applied_value;
    CmHirTypeId applied_type;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBody *stored_body;
    CmHirBodyId body_id;
    CmHirExprId root;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmSpan saved_span;

    fixture_init(&fixture);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_STRUCT, test_span(10u, 35u),
        &newtype_definition) == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = newtype_definition;
    generic.name = cm_hir_intern(&fixture.hir, "T");
    generic.span = test_span(18u, 19u);
    assert(cm_hir_add_generic_param(&fixture.hir, &generic, &generic_id)
        == CM_HIR_OK);
    generic_type = add_parameter_type(&fixture.hir, generic_id);
    memset(&field, 0, sizeof(field));
    field.type = generic_type;
    field.visibility.kind = CM_HIR_VIS_PUBLIC;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(22u, 28u);
    init_item(&item, CM_HIR_ITEM_STRUCT, newtype_definition, fixture.root,
        "Newtype", &fixture.hir);
    item.generic_parameter_start = generic_id;
    item.generic_parameter_count = 1u;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = fixture.u32_type;
    memset(&applied_value, 0, sizeof(applied_value));
    applied_value.kind = CM_HIR_TYPE_ADT_KIND;
    applied_value.span = test_span(40u, 52u);
    applied_value.data.named_type.definition = newtype_definition;
    applied_value.data.named_type.arguments = &argument;
    applied_value.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&fixture.hir, &applied_value, &applied_type)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(40u, 80u),
        &function_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.type = applied_type;
    parameter.span = test_span(44u, 60u);
    parameter.binding_kind = CM_HIR_BINDING_NEWTYPE_PATTERN;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameter.newtype_binding.name = cm_hir_intern(&fixture.hir, "value");
    parameter.newtype_binding.span = test_span(52u, 57u);
    memset(&local, 0, sizeof(local));
    local.name = parameter.newtype_binding.name;
    local.type = fixture.u32_type;
    local.mutability = CM_HIR_IMMUTABLE;
    local.span = parameter.newtype_binding.span;
    local.parameter_index = 0u;
    local.parameter_binding_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = function_definition;
    body.origin = cm_hir_body_origin_item_source(function_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = fixture.u32_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 10u;
    body.span = test_span(40u, 80u);
    assert(cm_hir_add_body(&fixture.hir, &body, &body_id) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, function_definition,
        fixture.root, "unwrap_newtype", &fixture.hir);
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = fixture.u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK
        && add_local_expression(&fixture.hir, body_id, 0u,
            fixture.u32_type, test_span(65u, 70u), &root) == CM_HIR_OK
        && cm_hir_set_body_root_expression(&fixture.hir, body_id, root)
            == CM_HIR_OK);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, function_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    stored_body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
        (size_t)body_id - 1u);
    assert(stored_body != NULL);
    saved_span = stored_body->locals[0].span;
    stored_body->locals[0].span.start += 1u;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    stored_body->locals[0].span = saved_span;
    stored_body->locals[0].type = fixture.bool_type;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH);
    stored_body->locals[0].type = fixture.u32_type;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_closed_trait_default_definition_mode(void)
{
    TestFixture fixture;
    CmHirDefId trait_definition;
    CmHirDefId method_definition;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExpr expression;
    CmHirExpr saved_expression;
    CmHirExprId leaf;
    CmHirExprId root;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    size_t type_count;

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "Closed");
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(30u, 60u),
        &method_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture.hir, "value");
    parameter.type = fixture.u32_type;
    parameter.span = test_span(40u, 45u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = fixture.u32_type;
    local.span = parameter.span;
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = method_definition;
    body.origin = cm_hir_body_origin_item_source(method_definition);
    body.source = 1u;
    body.source_expression_id = 1u;
    body.expected_type = fixture.u32_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.span = test_span(30u, 60u);
    assert(cm_hir_add_body(&fixture.hir, &body, &body_id) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, method_definition,
        fixture.root, "value", &fixture.hir);
    item.parent_definition = trait_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = fixture.u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK
        && add_local_expression(&fixture.hir, body_id, 0u,
            fixture.u32_type, test_span(50u, 55u), &leaf) == CM_HIR_OK
        && add_block_expression(&fixture.hir, body_id, NULL, 0u, leaf,
            fixture.u32_type, test_span(48u, 57u), &root) == CM_HIR_OK
        && cm_hir_set_body_root_expression(&fixture.hir, body_id, root)
            == CM_HIR_OK
        && cm_hir_body_function_owner_kind(&fixture.hir,
            cm_hir_get_item(&fixture.hir, item_id))
            == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, method_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN
        && cm_hir_def_id_equal(cm_semantic_session_enclosing_owner(&session),
            trait_definition));
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    type_count = cm_typeck_type_count(typeck);

    expression = *(CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)leaf - 1u);
    saved_expression = expression;
    expression.kind = CM_HIR_EXPR_AGGREGATE;
    expression.data.aggregate.definition = trait_definition;
    expression.data.aggregate.fields = NULL;
    expression.data.aggregate.field_count = 0u;
    expression.data.aggregate.owned_storage = NULL;
    *(CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)leaf - 1u) = expression;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_UNSUPPORTED
        && cm_typeck_type_count(typeck) == type_count);

    expression = saved_expression;
    expression.kind = CM_HIR_EXPR_CALL;
    expression.data.call.callee = method_definition;
    expression.data.call.type_substitutions = NULL;
    expression.data.call.type_substitution_count = 0u;
    expression.data.call.arguments = NULL;
    expression.data.call.argument_count = 0u;
    expression.data.call.owned_storage = NULL;
    *(CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)leaf - 1u) = expression;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_UNSUPPORTED
        && cm_typeck_type_count(typeck) == type_count);

    *(CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)leaf - 1u) = saved_expression;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, body_id, NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static CmHirDefId add_impl(TestFixture *fixture, CmHirDefId trait_definition)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(20u, 35u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root, NULL,
        &fixture->hir);
    item.data.impl_item.self_type = fixture->u32_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_blanket_impl(TestFixture *fixture,
    CmHirDefId trait_definition, CmHirTypeId *out_parameter_type)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirItem item;
    CmHirItemId item_id;

    assert(out_parameter_type != NULL
        && cm_hir_reserve_item_definition_as(&fixture->hir,
            fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(20u, 35u),
            &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&fixture->hir, "T");
    parameter.span = test_span(22u, 23u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    *out_parameter_type = add_parameter_type(&fixture->hir, parameter_id);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root, NULL,
        &fixture->hir);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.impl_item.self_type = *out_parameter_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void test_newtype_parameter_enclosing_impl_substitution(void)
{
    TestFixture fixture;
    CmHirDefId newtype_definition;
    CmHirDefId trait_definition;
    CmHirDefId declared_definition;
    CmHirDefId impl_definition;
    CmHirDefId selected_definition;
    CmHirGenericParam generic;
    CmHirGenericParamId newtype_parameter_id;
    CmHirGenericParamId trait_parameter_id;
    CmHirGenericParamId impl_parameter_id;
    CmHirTypeId newtype_parameter_type;
    CmHirTypeId trait_parameter_type;
    CmHirTypeId trait_self_type;
    CmHirTypeId impl_parameter_type;
    CmHirTypeId applied_impl_type;
    CmHirTypeId applied_u32_type;
    CmHirType type;
    CmHirGenericArg applied_impl_argument;
    CmHirGenericArg applied_u32_argument;
    CmHirGenericArg trait_impl_argument;
    CmHirGenericArg instance_impl_argument;
    CmHirGenericArg instance_trait_argument;
    CmHirField field;
    CmHirFunctionParameter declared_parameter;
    CmHirFunctionParameter selected_parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBody *stored_body;
    CmHirBodyId body_id;
    CmHirItem item;
    CmHirItem *stored_newtype;
    CmHirItemId newtype_item_id;
    CmHirItemId item_id;
    CmHirExprId root;
    CmHirInstanceSpec spec;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyEvidenceWriteback evidence;
    CmSemanticBodyResult result;
    WritebackProbe probe;

    fixture_init(&fixture);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_STRUCT, test_span(10u, 30u),
        &newtype_definition) == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = newtype_definition;
    generic.name = cm_hir_intern(&fixture.hir, "T");
    generic.span = test_span(14u, 15u);
    assert(cm_hir_add_generic_param(&fixture.hir, &generic,
        &newtype_parameter_id) == CM_HIR_OK);
    newtype_parameter_type = add_parameter_type(&fixture.hir,
        newtype_parameter_id);
    memset(&field, 0, sizeof(field));
    field.type = newtype_parameter_type;
    field.visibility.kind = CM_HIR_VIS_PUBLIC;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(20u, 25u);
    init_item(&item, CM_HIR_ITEM_STRUCT, newtype_definition, fixture.root,
        "Newtype", &fixture.hir);
    item.generic_parameter_start = newtype_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&fixture.hir, &item, &newtype_item_id)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_TRAIT, test_span(31u, 50u),
        &trait_definition) == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = trait_definition;
    generic.name = cm_hir_intern(&fixture.hir, "R");
    generic.span = test_span(35u, 36u);
    assert(cm_hir_add_generic_param(&fixture.hir, &generic,
        &trait_parameter_id) == CM_HIR_OK);
    trait_parameter_type = add_parameter_type(&fixture.hir,
        trait_parameter_id);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(40u, 44u);
    type.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&fixture.hir, &type, &trait_self_type)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, trait_definition, fixture.root,
        "FromResidual", &fixture.hir);
    item.generic_parameter_start = trait_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(51u, 70u),
        &declared_definition) == CM_HIR_OK);
    memset(&declared_parameter, 0, sizeof(declared_parameter));
    declared_parameter.name = cm_hir_intern(&fixture.hir, "residual");
    declared_parameter.type = trait_parameter_type;
    declared_parameter.span = test_span(56u, 64u);
    declared_parameter.binding_kind = CM_HIR_BINDING_NAMED;
    declared_parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    init_item(&item, CM_HIR_ITEM_FUNCTION, declared_definition,
        fixture.root, "from_residual", &fixture.hir);
    item.parent_definition = trait_definition;
    item.data.function_item.signature.parameters = &declared_parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = trait_self_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_IMPL, test_span(71u, 95u),
        &impl_definition) == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = impl_definition;
    generic.name = cm_hir_intern(&fixture.hir, "E");
    generic.span = test_span(76u, 77u);
    assert(cm_hir_add_generic_param(&fixture.hir, &generic,
        &impl_parameter_id) == CM_HIR_OK);
    impl_parameter_type = add_parameter_type(&fixture.hir,
        impl_parameter_id);
    memset(&applied_impl_argument, 0, sizeof(applied_impl_argument));
    applied_impl_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    applied_impl_argument.data.type = impl_parameter_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(80u, 90u);
    type.data.named_type.definition = newtype_definition;
    type.data.named_type.arguments = &applied_impl_argument;
    type.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&fixture.hir, &type, &applied_impl_type)
        == CM_HIR_OK);
    memset(&trait_impl_argument, 0, sizeof(trait_impl_argument));
    trait_impl_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_impl_argument.data.type = applied_impl_type;
    init_item(&item, CM_HIR_ITEM_IMPL, impl_definition, fixture.root, NULL,
        &fixture.hir);
    item.generic_parameter_start = impl_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.impl_item.self_type = impl_parameter_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.trait_type.arguments = &trait_impl_argument;
    item.data.impl_item.trait_type.argument_count = 1u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(96u, 140u),
        &selected_definition) == CM_HIR_OK);
    memset(&selected_parameter, 0, sizeof(selected_parameter));
    selected_parameter.type = applied_impl_type;
    selected_parameter.span = test_span(104u, 122u);
    selected_parameter.binding_kind = CM_HIR_BINDING_NEWTYPE_PATTERN;
    selected_parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    selected_parameter.newtype_binding.name =
        cm_hir_intern(&fixture.hir, "value");
    selected_parameter.newtype_binding.span = test_span(112u, 117u);
    memset(&local, 0, sizeof(local));
    local.name = selected_parameter.newtype_binding.name;
    local.type = impl_parameter_type;
    local.mutability = CM_HIR_IMMUTABLE;
    local.span = selected_parameter.newtype_binding.span;
    local.parameter_index = 0u;
    local.parameter_binding_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = selected_definition;
    body.origin = cm_hir_body_origin_item_source(selected_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = impl_parameter_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 10u;
    body.span = test_span(96u, 140u);
    assert(cm_hir_add_body(&fixture.hir, &body, &body_id) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, selected_definition,
        fixture.root, "from_residual", &fixture.hir);
    item.parent_definition = impl_definition;
    item.data.function_item.signature.parameters = &selected_parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = impl_parameter_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    item.data.function_item.trait_item_definition = declared_definition;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK
        && add_local_expression(&fixture.hir, body_id, 0u,
            impl_parameter_type, test_span(128u, 133u), &root) == CM_HIR_OK
        && cm_hir_set_body_root_expression(&fixture.hir, body_id, root)
            == CM_HIR_OK
        && cm_hir_body_function_owner_kind(&fixture.hir,
            cm_hir_get_item(&fixture.hir, item_id))
            == CM_HIR_BODY_FUNCTION_OWNER_TYPE_GENERIC_TRAIT_IMPL_METHOD);

    memset(&applied_u32_argument, 0, sizeof(applied_u32_argument));
    applied_u32_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    applied_u32_argument.data.type = fixture.u32_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(80u, 90u);
    type.data.named_type.definition = newtype_definition;
    type.data.named_type.arguments = &applied_u32_argument;
    type.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&fixture.hir, &type, &applied_u32_type)
        == CM_HIR_OK);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, selected_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN
        && cm_hir_def_id_equal(cm_semantic_session_enclosing_owner(&session),
            impl_definition));
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_OK);

    memset(&instance_impl_argument, 0, sizeof(instance_impl_argument));
    instance_impl_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    instance_impl_argument.data.type = fixture.u32_type;
    memset(&instance_trait_argument, 0, sizeof(instance_trait_argument));
    instance_trait_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    instance_trait_argument.data.type = applied_u32_type;
    memset(&spec, 0, sizeof(spec));
    spec.selected_callable = selected_definition;
    spec.body_definition = selected_definition;
    spec.declared_trait_callable = declared_definition;
    spec.enclosing_impl = impl_definition;
    spec.enclosing_impl_arguments = &instance_impl_argument;
    spec.enclosing_impl_argument_count = 1u;
    spec.implemented_trait = trait_definition;
    spec.implemented_trait_arguments = &instance_trait_argument;
    spec.implemented_trait_argument_count = 1u;
    spec.self_owner = impl_definition;
    spec.self_type = fixture.u32_type;
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = body_id;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    memset(&evidence, 0, sizeof(evidence));
    evidence.context = &probe;
    evidence.checked_body = probe_writeback;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        body_id, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);

    stored_body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
        (size_t)body_id - 1u);
    assert(stored_body != NULL);
    stored_body->locals[0].type = fixture.bool_type;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH);
    stored_body->locals[0].type = impl_parameter_type;

    stored_newtype = (CmHirItem *)cm_vec_at(&fixture.hir.items,
        (size_t)newtype_item_id - 1u);
    assert(stored_newtype != NULL);
    stored_newtype->data.aggregate_item.fields[0].type = fixture.u32_type;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    stored_newtype->data.aggregate_item.fields[0].type =
        newtype_parameter_type;
    result = cm_semantic_body_check_definition(&session, body_id);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_explicit_qualified_callable_selection(void)
{
    TestFixture fixture;
    CmHirDefId trait_definition;
    CmHirDefId impl_definition;
    CmHirDefId declared_definition;
    CmHirDefId selected_definition;
    CmHirDefId caller_definition;
    CmHirDefId method_caller_definition;
    CmHirType self_type_value;
    CmHirTypeId trait_self_type;
    CmHirTypeId impl_self_type;
    CmHirTypeId impl_parameter_type;
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirBodyId impl_body;
    CmHirBodyId caller_body;
    CmHirBodyId method_caller_body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId impl_root;
    CmHirExprId arguments[2];
    CmHirExprId call;
    CmHirExprId receiver;
    CmHirExprId value;
    CmHirExprId method_call;
    CmHirExpr qualified;
    CmHirExpr method;
    CmHirDefId in_scope_traits[1];
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    WritebackProbe probe;

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "Value");
    impl_definition = add_blanket_impl(&fixture, trait_definition,
        &impl_parameter_type);
    memset(&self_type_value, 0, sizeof(self_type_value));
    self_type_value.kind = CM_HIR_TYPE_SELF_KIND;
    self_type_value.span = test_span(20u, 24u);
    self_type_value.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&fixture.hir, &self_type_value,
        &trait_self_type) == CM_HIR_OK);
    self_type_value.data.self_type.owner = impl_definition;
    assert(cm_hir_add_type(&fixture.hir, &self_type_value,
        &impl_self_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(30u, 45u),
        &declared_definition) == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = cm_hir_intern(&fixture.hir, "self");
    parameters[0].type = trait_self_type;
    parameters[0].span = test_span(34u, 38u);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[1].name = cm_hir_intern(&fixture.hir, "value");
    parameters[1].type = trait_self_type;
    parameters[1].span = test_span(39u, 44u);
    parameters[1].binding_kind = CM_HIR_BINDING_NAMED;
    init_item(&item, CM_HIR_ITEM_FUNCTION, declared_definition,
        fixture.root, "value", &fixture.hir);
    item.parent_definition = trait_definition;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = trait_self_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(46u, 65u),
        &selected_definition) == CM_HIR_OK);
    memset(locals, 0, sizeof(locals));
    locals[0].name = parameters[0].name;
    locals[0].type = impl_self_type;
    locals[0].span = test_span(50u, 54u);
    locals[0].parameter_index = 0u;
    locals[1].name = parameters[1].name;
    locals[1].type = impl_parameter_type;
    locals[1].span = test_span(55u, 60u);
    locals[1].parameter_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = selected_definition;
    body.origin = cm_hir_body_origin_item_source(selected_definition);
    body.source = 1u;
    body.source_expression_id = 1u;
    body.expected_type = impl_parameter_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.span = test_span(46u, 65u);
    assert(cm_hir_add_body(&fixture.hir, &body, &impl_body) == CM_HIR_OK);
    parameters[0].type = impl_self_type;
    parameters[1].type = impl_parameter_type;
    init_item(&item, CM_HIR_ITEM_FUNCTION, selected_definition,
        fixture.root, "value", &fixture.hir);
    item.parent_definition = impl_definition;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = impl_parameter_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = impl_body;
    item.data.function_item.trait_item_definition = declared_definition;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture.hir, impl_body, 1u,
        impl_parameter_type, test_span(61u, 62u), &impl_root)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture.hir, impl_body,
        impl_root) == CM_HIR_OK);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, selected_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_definition(&session, impl_body);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    cm_semantic_session_destroy(&session);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(70u, 100u),
        &caller_definition) == CM_HIR_OK);
    locals[0].name = cm_hir_intern(&fixture.hir, "receiver");
    locals[0].type = fixture.u32_type;
    locals[0].span = test_span(75u, 76u);
    locals[0].parameter_index = 0u;
    locals[1].name = cm_hir_intern(&fixture.hir, "value");
    locals[1].type = fixture.u32_type;
    locals[1].span = test_span(77u, 78u);
    locals[1].parameter_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = caller_definition;
    body.origin = cm_hir_body_origin_item_source(caller_definition);
    body.source = 1u;
    body.source_expression_id = 2u;
    body.expected_type = fixture.u32_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.span = test_span(70u, 100u);
    assert(cm_hir_add_body(&fixture.hir, &body, &caller_body) == CM_HIR_OK);
    parameters[0].name = locals[0].name;
    parameters[0].type = fixture.u32_type;
    parameters[0].span = locals[0].span;
    parameters[1].name = locals[1].name;
    parameters[1].type = fixture.u32_type;
    parameters[1].span = locals[1].span;
    init_item(&item, CM_HIR_ITEM_FUNCTION, caller_definition,
        fixture.root, "qualified_caller", &fixture.hir);
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.return_type = fixture.u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = caller_body;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture.hir, caller_body, 0u,
        fixture.u32_type, test_span(82u, 83u), &arguments[0]) == CM_HIR_OK
        && add_local_expression(&fixture.hir, caller_body, 1u,
            fixture.u32_type, test_span(84u, 85u), &arguments[1])
            == CM_HIR_OK);
    memset(&qualified, 0, sizeof(qualified));
    qualified.kind = CM_HIR_EXPR_QUALIFIED_CALL;
    qualified.owner_body = caller_body;
    qualified.type = fixture.u32_type;
    qualified.span = test_span(80u, 95u);
    qualified.data.qualified_call.syntax =
        CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD;
    qualified.data.qualified_call.requested_self_type = fixture.u32_type;
    qualified.data.qualified_call.requested_trait = trait_definition;
    qualified.data.qualified_call.declared_trait_callable =
        declared_definition;
    qualified.data.qualified_call.arguments = arguments;
    qualified.data.qualified_call.argument_count = 2u;
    qualified.data.qualified_call.receiver_argument = 0u;
    assert(cm_hir_add_expr(&fixture.hir, &qualified, &call) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture.hir, caller_body, call)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(101u, 130u),
        &method_caller_definition) == CM_HIR_OK);
    memset(&body, 0, sizeof(body));
    body.owner = method_caller_definition;
    body.origin = cm_hir_body_origin_item_source(method_caller_definition);
    body.source = 1u;
    body.source_expression_id = 3u;
    body.expected_type = fixture.u32_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.span = test_span(101u, 130u);
    assert(cm_hir_add_body(&fixture.hir, &body, &method_caller_body)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, method_caller_definition,
        fixture.root, "method_caller", &fixture.hir);
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.return_type = fixture.u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = method_caller_body;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK
        && add_local_expression(&fixture.hir, method_caller_body, 0u,
            fixture.u32_type, test_span(110u, 111u), &receiver)
            == CM_HIR_OK
        && add_local_expression(&fixture.hir, method_caller_body, 1u,
            fixture.u32_type, test_span(112u, 113u), &value)
            == CM_HIR_OK);
    in_scope_traits[0] = trait_definition;
    memset(&method, 0, sizeof(method));
    method.kind = CM_HIR_EXPR_METHOD_CALL;
    method.owner_body = method_caller_body;
    method.type = fixture.u32_type;
    method.span = test_span(110u, 125u);
    method.data.method_call.syntax = CM_HIR_CALLABLE_DOT_METHOD;
    method.data.method_call.method_name = cm_hir_intern(&fixture.hir,
        "value");
    method.data.method_call.receiver = receiver;
    method.data.method_call.arguments = &value;
    method.data.method_call.argument_count = 1u;
    method.data.method_call.in_scope_traits = in_scope_traits;
    method.data.method_call.in_scope_trait_count = 1u;
    assert(cm_hir_add_expr(&fixture.hir, &method, &method_call) == CM_HIR_OK
        && cm_hir_set_body_root_expression(&fixture.hir,
            method_caller_body, method_call) == CM_HIR_OK);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, caller_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_qualified_callable = 1;
    probe.expected_trait = trait_definition;
    probe.expected_declared_callable = declared_definition;
    probe.expected_impl = impl_definition;
    probe.expected_selected_callable = selected_definition;
    probe.expected_enclosing_impl_argument_count = 1u;
    probe.expected_enclosing_impl_integer_kind = CM_HIR_INT_U32;
    probe.expected_callable_argument_count = 2u;
    probe.expected_callable_integer_kind = CM_HIR_INT_U32;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        caller_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);
    cm_semantic_session_destroy(&session);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, method_caller_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = method_caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_method_callable = 1;
    probe.expected_trait = trait_definition;
    probe.expected_declared_callable = declared_definition;
    probe.expected_impl = impl_definition;
    probe.expected_selected_callable = selected_definition;
    probe.expected_enclosing_impl_argument_count = 1u;
    probe.expected_enclosing_impl_integer_kind = CM_HIR_INT_U32;
    probe.expected_callable_argument_count = 2u;
    probe.expected_callable_integer_kind = CM_HIR_INT_U32;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        method_caller_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);
    cm_semantic_session_destroy(&session);

    {
        CmHirItem *qualified_caller;
        CmHirItem *method_caller;
        CmHirBody *qualified_caller_body;
        CmHirBody *dot_caller_body;
        CmHirExpr *qualified_receiver;
        CmHirExpr *qualified_value;
        CmHirExpr *qualified_call;
        CmHirExpr *dot_receiver;
        CmHirExpr *dot_value;
        CmHirExpr *dot_call;

        qualified_caller = mutable_item(&fixture, caller_definition);
        method_caller = mutable_item(&fixture, method_caller_definition);
        qualified_caller_body = (CmHirBody *)cm_vec_at(
            &fixture.hir.bodies, (size_t)caller_body - 1u);
        dot_caller_body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
            (size_t)method_caller_body - 1u);
        qualified_receiver = (CmHirExpr *)cm_vec_at(
            &fixture.hir.expressions, (size_t)arguments[0] - 1u);
        qualified_value = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
            (size_t)arguments[1] - 1u);
        qualified_call = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
            (size_t)call - 1u);
        dot_receiver = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
            (size_t)receiver - 1u);
        dot_value = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
            (size_t)value - 1u);
        dot_call = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
            (size_t)method_call - 1u);
        assert(qualified_caller != NULL && method_caller != NULL
            && qualified_caller_body != NULL && dot_caller_body != NULL
            && qualified_receiver != NULL && qualified_value != NULL
            && qualified_call != NULL && dot_receiver != NULL
            && dot_value != NULL && dot_call != NULL);
        qualified_caller->data.function_item.signature.parameters[0].type =
            fixture.u8_type;
        qualified_caller->data.function_item.signature.parameters[1].type =
            fixture.u8_type;
        qualified_caller->data.function_item.signature.return_type =
            fixture.u8_type;
        qualified_caller_body->locals[0].type = fixture.u8_type;
        qualified_caller_body->locals[1].type = fixture.u8_type;
        qualified_caller_body->expected_type = fixture.u8_type;
        qualified_receiver->type = fixture.u8_type;
        qualified_value->type = fixture.u8_type;
        qualified_call->type = fixture.u8_type;
        qualified_call->data.qualified_call.requested_self_type =
            fixture.u8_type;
        method_caller->data.function_item.signature.parameters[0].type =
            fixture.u8_type;
        method_caller->data.function_item.signature.parameters[1].type =
            fixture.u8_type;
        method_caller->data.function_item.signature.return_type =
            fixture.u8_type;
        dot_caller_body->locals[0].type = fixture.u8_type;
        dot_caller_body->locals[1].type = fixture.u8_type;
        dot_caller_body->expected_type = fixture.u8_type;
        dot_receiver->type = fixture.u8_type;
        dot_value->type = fixture.u8_type;
        dot_call->type = fixture.u8_type;
    }

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, caller_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_INVALID;
    probe.require_qualified_callable = 1;
    probe.expected_trait = trait_definition;
    probe.expected_declared_callable = declared_definition;
    probe.expected_impl = impl_definition;
    probe.expected_selected_callable = selected_definition;
    probe.expected_enclosing_impl_argument_count = 1u;
    probe.expected_enclosing_impl_integer_kind = CM_HIR_INT_U8;
    probe.expected_callable_argument_count = 2u;
    probe.expected_callable_integer_kind = CM_HIR_INT_U8;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        caller_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.invocation_count == 1u);
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        caller_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 2u);
    cm_semantic_session_destroy(&session);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, method_caller_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = method_caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_method_callable = 1;
    probe.expected_trait = trait_definition;
    probe.expected_declared_callable = declared_definition;
    probe.expected_impl = impl_definition;
    probe.expected_selected_callable = selected_definition;
    probe.expected_enclosing_impl_argument_count = 1u;
    probe.expected_enclosing_impl_integer_kind = CM_HIR_INT_U8;
    probe.expected_callable_argument_count = 2u;
    probe.expected_callable_integer_kind = CM_HIR_INT_U8;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        method_caller_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_generic_trait_argument_callable(void)
{
    TestFixture fixture;
    CmHirDefId trait_definition;
    CmHirDefId impl_definition;
    CmHirDefId declared_definition;
    CmHirDefId selected_definition;
    CmHirDefId caller_definition;
    CmHirGenericParam generic_parameter;
    CmHirGenericParamId trait_parameter_id;
    CmHirGenericParamId impl_parameter_id;
    CmHirTypeId impl_parameter_type;
    CmHirTypeId impl_self_type;
    CmHirTypeId trait_self_type;
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirBodyId selected_body;
    CmHirBodyId caller_body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId value_expression;
    CmHirExprId receiver_expression;
    CmHirExprId argument_expression;
    CmHirExprId call_expression;
    CmHirExpr qualified;
    CmHirGenericArg trait_arguments[1];
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    WritebackProbe probe;

    fixture_init(&fixture);
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &trait_definition) == CM_HIR_OK);
    memset(&generic_parameter, 0, sizeof(generic_parameter));
    generic_parameter.kind = CM_HIR_GENERIC_TYPE;
    generic_parameter.owner = trait_definition;
    generic_parameter.name = cm_hir_intern(&fixture.hir, "T");
    generic_parameter.span = test_span(4u, 5u);
    assert(cm_hir_add_generic_param(&fixture.hir, &generic_parameter,
        &trait_parameter_id) == CM_HIR_OK);
    (void)add_parameter_type(&fixture.hir, trait_parameter_id);
    init_item(&item, CM_HIR_ITEM_TRAIT, trait_definition, fixture.root,
        "GenericValue", &fixture.hir);
    item.generic_parameter_start = trait_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    {
        CmHirType self_type;
        CmHirTypeId *out_self;

        memset(&self_type, 0, sizeof(self_type));
        self_type.kind = CM_HIR_TYPE_SELF_KIND;
        self_type.span = test_span(6u, 7u);
        self_type.data.self_type.owner = trait_definition;
        out_self = &trait_self_type;
        assert(cm_hir_add_type(&fixture.hir, &self_type, out_self)
            == CM_HIR_OK);
    }
    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(21u, 40u),
        &declared_definition) == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = cm_hir_intern(&fixture.hir, "self");
    parameters[0].type = trait_self_type;
    parameters[0].span = test_span(24u, 28u);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[1].name = cm_hir_intern(&fixture.hir, "value");
    parameters[1].type = fixture.u8_type;
    parameters[1].span = test_span(29u, 34u);
    parameters[1].binding_kind = CM_HIR_BINDING_NAMED;
    init_item(&item, CM_HIR_ITEM_FUNCTION, declared_definition, fixture.root,
        "value", &fixture.hir);
    item.parent_definition = trait_definition;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = fixture.u8_type;
    item.data.function_item.signature.abi = cm_hir_intern(&fixture.hir,
        "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_IMPL, test_span(41u, 60u),
        &impl_definition) == CM_HIR_OK);
    memset(&generic_parameter, 0, sizeof(generic_parameter));
    generic_parameter.kind = CM_HIR_GENERIC_TYPE;
    generic_parameter.owner = impl_definition;
    generic_parameter.name = cm_hir_intern(&fixture.hir, "U");
    generic_parameter.span = test_span(44u, 45u);
    assert(cm_hir_add_generic_param(&fixture.hir, &generic_parameter,
        &impl_parameter_id) == CM_HIR_OK);
    impl_parameter_type = add_parameter_type(&fixture.hir,
        impl_parameter_id);
    {
        CmHirType self_type;

        memset(&self_type, 0, sizeof(self_type));
        self_type.kind = CM_HIR_TYPE_SELF_KIND;
        self_type.span = test_span(46u, 47u);
        self_type.data.self_type.owner = impl_definition;
        assert(cm_hir_add_type(&fixture.hir, &self_type, &impl_self_type)
            == CM_HIR_OK);
    }
    memset(trait_arguments, 0, sizeof(trait_arguments));
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[0].data.type = impl_parameter_type;
    init_item(&item, CM_HIR_ITEM_IMPL, impl_definition, fixture.root, NULL,
        &fixture.hir);
    item.generic_parameter_start = impl_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.impl_item.self_type = impl_parameter_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.trait_type.arguments = trait_arguments;
    item.data.impl_item.trait_type.argument_count = 1u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(61u, 90u),
        &selected_definition) == CM_HIR_OK);
    memset(&locals, 0, sizeof(locals));
    locals[0].name = parameters[0].name;
    locals[0].type = impl_self_type;
    locals[0].span = test_span(65u, 69u);
    locals[0].parameter_index = 0u;
    locals[1].name = parameters[1].name;
    locals[1].type = impl_parameter_type;
    locals[1].span = test_span(70u, 75u);
    locals[1].parameter_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = selected_definition;
    body.origin = cm_hir_body_origin_item_source(selected_definition);
    body.source = 1u;
    body.source_expression_id = 1u;
    body.expected_type = impl_parameter_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.span = test_span(61u, 90u);
    assert(cm_hir_add_body(&fixture.hir, &body, &selected_body)
        == CM_HIR_OK);
    parameters[0].type = impl_self_type;
    parameters[1].type = impl_parameter_type;
    init_item(&item, CM_HIR_ITEM_FUNCTION, selected_definition, fixture.root,
        "value", &fixture.hir);
    item.parent_definition = impl_definition;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = impl_parameter_type;
    item.data.function_item.signature.abi = cm_hir_intern(&fixture.hir,
        "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = selected_body;
    item.data.function_item.trait_item_definition = declared_definition;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture.hir, selected_body, 1u,
        impl_parameter_type, test_span(85u, 86u), &value_expression)
        == CM_HIR_OK
        && cm_hir_set_body_root_expression(&fixture.hir, selected_body,
            value_expression) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(91u, 120u),
        &caller_definition) == CM_HIR_OK);
    parameters[0].name = cm_hir_intern(&fixture.hir, "receiver");
    parameters[1].name = cm_hir_intern(&fixture.hir, "value");
    parameters[0].type = fixture.u8_type;
    parameters[1].type = fixture.u8_type;
    memset(&locals, 0, sizeof(locals));
    locals[0].name = cm_hir_intern(&fixture.hir, "receiver");
    locals[0].type = fixture.u8_type;
    locals[0].span = test_span(95u, 96u);
    locals[0].parameter_index = 0u;
    locals[1].name = cm_hir_intern(&fixture.hir, "value");
    locals[1].type = fixture.u8_type;
    locals[1].span = test_span(97u, 98u);
    locals[1].parameter_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = caller_definition;
    body.origin = cm_hir_body_origin_item_source(caller_definition);
    body.source = 1u;
    body.source_expression_id = 2u;
    body.expected_type = fixture.u8_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.span = test_span(91u, 120u);
    assert(cm_hir_add_body(&fixture.hir, &body, &caller_body)
        == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, caller_definition, fixture.root,
        "caller", &fixture.hir);
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.return_type = fixture.u8_type;
    item.data.function_item.signature.abi = cm_hir_intern(&fixture.hir,
        "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = caller_body;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture.hir, caller_body, 0u,
        fixture.u8_type, test_span(105u, 106u), &receiver_expression)
        == CM_HIR_OK
        && add_local_expression(&fixture.hir, caller_body, 1u,
            fixture.u8_type, test_span(107u, 108u), &argument_expression)
            == CM_HIR_OK);
    memset(&qualified, 0, sizeof(qualified));
    qualified.kind = CM_HIR_EXPR_QUALIFIED_CALL;
    qualified.owner_body = caller_body;
    qualified.type = fixture.u8_type;
    qualified.span = test_span(103u, 115u);
    qualified.data.qualified_call.syntax =
        CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD;
    qualified.data.qualified_call.requested_self_type = fixture.u8_type;
    qualified.data.qualified_call.requested_trait = trait_definition;
    qualified.data.qualified_call.declared_trait_callable = declared_definition;
    {
        CmHirExprId arguments[2];

        arguments[0] = receiver_expression;
        arguments[1] = argument_expression;
        qualified.data.qualified_call.arguments = arguments;
        qualified.data.qualified_call.argument_count = 2u;
        qualified.data.qualified_call.receiver_argument = 0u;
        assert(cm_hir_add_expr(&fixture.hir, &qualified, &call_expression)
            == CM_HIR_OK);
    }
    assert(cm_hir_set_body_root_expression(&fixture.hir, caller_body,
        call_expression) == CM_HIR_OK);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, caller_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_qualified_callable = 1;
    probe.require_trait_arguments = 1;
    probe.expected_trait = trait_definition;
    probe.expected_declared_callable = declared_definition;
    probe.expected_impl = impl_definition;
    probe.expected_selected_callable = selected_definition;
    probe.expected_enclosing_impl_argument_count = 1u;
    probe.expected_enclosing_impl_integer_kind = CM_HIR_INT_U8;
    probe.expected_trait_argument_integer_kind = CM_HIR_INT_U8;
    probe.expected_callable_argument_count = 2u;
    probe.expected_callable_integer_kind = CM_HIR_INT_U8;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        caller_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_generic_impl_method_instance_spec(void)
{
    TestFixture fixture;
    CmHirDefId trait_definition;
    CmHirDefId impl_definition;
    CmHirDefId declared_definition;
    CmHirDefId selected_definition;
    CmHirType self_type_value;
    CmHirTypeId trait_self_type;
    CmHirTypeId impl_self_type;
    CmHirTypeId impl_parameter_type;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBodyId impl_body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId impl_root;
    CmHirGenericArg impl_argument;
    CmHirInstanceSpec spec;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyEvidenceWriteback evidence;
    CmSemanticBodyResult result;
    WritebackProbe probe;
    CmTypeckContext *typeck;
    size_t type_count;

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "ExactEcho");
    impl_definition = add_blanket_impl(&fixture, trait_definition,
        &impl_parameter_type);
    memset(&self_type_value, 0, sizeof(self_type_value));
    self_type_value.kind = CM_HIR_TYPE_SELF_KIND;
    self_type_value.span = test_span(20u, 24u);
    self_type_value.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&fixture.hir, &self_type_value,
        &trait_self_type) == CM_HIR_OK);
    self_type_value.data.self_type.owner = impl_definition;
    assert(cm_hir_add_type(&fixture.hir, &self_type_value,
        &impl_self_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(30u, 45u),
        &declared_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture.hir, "self");
    parameter.type = trait_self_type;
    parameter.span = test_span(34u, 38u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    init_item(&item, CM_HIR_ITEM_FUNCTION, declared_definition,
        fixture.root, "echo", &fixture.hir);
    item.parent_definition = trait_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = trait_self_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(46u, 65u),
        &selected_definition) == CM_HIR_OK);
    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = impl_self_type;
    local.span = test_span(50u, 54u);
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = selected_definition;
    body.origin = cm_hir_body_origin_item_source(selected_definition);
    body.source = 1u;
    body.source_expression_id = 1u;
    body.expected_type = impl_self_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.span = test_span(46u, 65u);
    assert(cm_hir_add_body(&fixture.hir, &body, &impl_body) == CM_HIR_OK);
    parameter.type = impl_self_type;
    init_item(&item, CM_HIR_ITEM_FUNCTION, selected_definition,
        fixture.root, "echo", &fixture.hir);
    item.parent_definition = impl_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = impl_self_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = impl_body;
    item.data.function_item.trait_item_definition = declared_definition;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture.hir, impl_body, 0u,
        impl_self_type, test_span(61u, 62u), &impl_root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture.hir, impl_body,
        impl_root) == CM_HIR_OK);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, selected_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    memset(&evidence, 0, sizeof(evidence));
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = impl_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_integer_kind = 1;
    evidence.context = &probe;
    evidence.checked_body = probe_writeback;
    memset(&impl_argument, 0, sizeof(impl_argument));
    impl_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    memset(&spec, 0, sizeof(spec));
    spec.selected_callable = cm_hir_def_id_none();
    spec.declared_trait_callable = cm_hir_def_id_none();
    spec.enclosing_impl = cm_hir_def_id_none();
    spec.implemented_trait = cm_hir_def_id_none();
    spec.self_owner = cm_hir_def_id_none();
    spec.selected_callable = selected_definition;
    spec.body_definition = selected_definition;
    spec.declared_trait_callable = declared_definition;
    spec.enclosing_impl = impl_definition;
    spec.enclosing_impl_arguments = &impl_argument;
    spec.enclosing_impl_argument_count = 1u;
    spec.implemented_trait = trait_definition;
    spec.self_owner = impl_definition;

    impl_argument.data.type = fixture.u32_type;
    spec.self_type = fixture.u32_type;
    probe.expected_integer_kind = CM_HIR_INT_U32;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);

    impl_argument.data.type = fixture.u8_type;
    spec.self_type = fixture.u8_type;
    probe.expected_integer_kind = CM_HIR_INT_U8;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 2u);

    type_count = cm_typeck_type_count(typeck);
    spec.enclosing_impl_argument_count = 0u;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);
    spec.enclosing_impl_argument_count = 1u;

    impl_argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    impl_argument.data.lifetime.kind = CM_HIR_REGION_STATIC;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_SUBSTITUTION
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);
    impl_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    impl_argument.data.type = fixture.u8_type;

    spec.enclosing_impl = trait_definition;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);
    spec.enclosing_impl = impl_definition;

    spec.self_owner = trait_definition;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);
    spec.self_owner = impl_definition;

    spec.self_type = fixture.u32_type;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);

    spec.self_type = fixture.u8_type;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_INVALID;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.invocation_count == 3u
        && cm_typeck_type_count(typeck) == type_count);
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    result = cm_semantic_body_check_instance_spec_with_evidence(&session,
        impl_body, &spec, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 4u);

    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_generic_impl_method_instance_parts(void)
{
    TestFixture fixture;
    CmHirDefId trait_definition;
    CmHirDefId impl_definition;
    CmHirDefId declared_definition;
    CmHirDefId selected_definition;
    CmHirType self_type_value;
    CmHirTypeId trait_self_type;
    CmHirTypeId impl_self_type;
    CmHirTypeId impl_parameter_type;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBodyId impl_body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId impl_root;
    CmHirCanonicalArgumentPart impl_argument;
    CmHirCanonicalInstanceParts parts;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyEvidenceWriteback evidence;
    CmSemanticBodyResult result;
    WritebackProbe probe;
    CmTypeckContext *typeck;
    size_t type_count;
    unsigned char u32_payload[] = {
        (unsigned char)CM_HIR_TYPE_INTEGER_KIND,
        (unsigned char)CM_HIR_INT_U32
    };
    unsigned char u8_payload[] = {
        (unsigned char)CM_HIR_TYPE_INTEGER_KIND,
        (unsigned char)CM_HIR_INT_U8
    };
    unsigned char malformed_payload[] = {
        (unsigned char)CM_HIR_TYPE_INTEGER_KIND
    };
    unsigned char trailing_payload[] = {
        (unsigned char)CM_HIR_TYPE_INTEGER_KIND,
        (unsigned char)CM_HIR_INT_U8,
        0u
    };
    unsigned char unresolved_payload[] = {
        (unsigned char)CM_HIR_TYPE_PARAMETER_KIND
    };

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "ExactPartsEcho");
    impl_definition = add_blanket_impl(&fixture, trait_definition,
        &impl_parameter_type);
    memset(&self_type_value, 0, sizeof(self_type_value));
    self_type_value.kind = CM_HIR_TYPE_SELF_KIND;
    self_type_value.span = test_span(20u, 24u);
    self_type_value.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&fixture.hir, &self_type_value,
        &trait_self_type) == CM_HIR_OK);
    self_type_value.data.self_type.owner = impl_definition;
    assert(cm_hir_add_type(&fixture.hir, &self_type_value,
        &impl_self_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(30u, 45u),
        &declared_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture.hir, "self");
    parameter.type = trait_self_type;
    parameter.span = test_span(34u, 38u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    init_item(&item, CM_HIR_ITEM_FUNCTION, declared_definition,
        fixture.root, "echo", &fixture.hir);
    item.parent_definition = trait_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = trait_self_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture.hir,
        fixture.crate_id, CM_HIR_ITEM_FUNCTION, test_span(46u, 65u),
        &selected_definition) == CM_HIR_OK);
    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = impl_self_type;
    local.span = test_span(50u, 54u);
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = selected_definition;
    body.origin = cm_hir_body_origin_item_source(selected_definition);
    body.source = 1u;
    body.source_expression_id = 1u;
    body.expected_type = impl_self_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.span = test_span(46u, 65u);
    assert(cm_hir_add_body(&fixture.hir, &body, &impl_body) == CM_HIR_OK);
    parameter.type = impl_self_type;
    init_item(&item, CM_HIR_ITEM_FUNCTION, selected_definition,
        fixture.root, "echo", &fixture.hir);
    item.parent_definition = impl_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = impl_self_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = impl_body;
    item.data.function_item.trait_item_definition = declared_definition;
    assert(cm_hir_add_item(&fixture.hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture.hir, impl_body, 0u,
        impl_self_type, test_span(61u, 62u), &impl_root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture.hir, impl_body,
        impl_root) == CM_HIR_OK);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, selected_definition);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    memset(&evidence, 0, sizeof(evidence));
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = impl_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_integer_kind = 1;
    evidence.context = &probe;
    evidence.checked_body = probe_writeback;
    memset(&impl_argument, 0, sizeof(impl_argument));
    impl_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    memset(&parts, 0, sizeof(parts));
    parts.selected_callable = selected_definition;
    parts.body_definition = selected_definition;
    parts.declared_trait_callable = declared_definition;
    parts.enclosing_impl = impl_definition;
    parts.enclosing_impl_arguments = &impl_argument;
    parts.enclosing_impl_argument_count = 1u;
    parts.implemented_trait = trait_definition;
    parts.self_owner = impl_definition;

    impl_argument.bytes = u32_payload;
    impl_argument.size = sizeof(u32_payload);
    parts.self_type = u32_payload;
    parts.self_type_size = sizeof(u32_payload);
    probe.expected_integer_kind = CM_HIR_INT_U32;
    result = cm_semantic_body_check_instance_parts_with_evidence(&session,
        impl_body, &parts, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);

    impl_argument.bytes = u8_payload;
    impl_argument.size = sizeof(u8_payload);
    parts.self_type = u8_payload;
    parts.self_type_size = sizeof(u8_payload);
    probe.expected_integer_kind = CM_HIR_INT_U8;
    result = cm_semantic_body_check_instance_parts_with_evidence(&session,
        impl_body, &parts, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 2u);

    type_count = cm_typeck_type_count(typeck);
    impl_argument.bytes = malformed_payload;
    impl_argument.size = sizeof(malformed_payload);
    result = cm_semantic_body_check_instance_parts_with_evidence(&session,
        impl_body, &parts, &evidence);
    assert(result.status != CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);

    impl_argument.bytes = trailing_payload;
    impl_argument.size = sizeof(trailing_payload);
    result = cm_semantic_body_check_instance_parts_with_evidence(&session,
        impl_body, &parts, &evidence);
    assert(result.status != CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);

    impl_argument.bytes = unresolved_payload;
    impl_argument.size = sizeof(unresolved_payload);
    result = cm_semantic_body_check_instance_parts_with_evidence(&session,
        impl_body, &parts, &evidence);
    assert(result.status != CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);

    impl_argument.bytes = u8_payload;
    impl_argument.size = sizeof(u8_payload);
    parts.self_owner = trait_definition;
    result = cm_semantic_body_check_instance_parts_with_evidence(&session,
        impl_body, &parts, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);
    parts.self_owner = impl_definition;

    parts.self_type = u32_payload;
    parts.self_type_size = sizeof(u32_payload);
    result = cm_semantic_body_check_instance_parts_with_evidence(&session,
        impl_body, &parts, &evidence);
    assert(result.status != CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 2u
        && cm_typeck_type_count(typeck) == type_count);

    parts.self_type = u8_payload;
    parts.self_type_size = sizeof(u8_payload);
    result = cm_semantic_body_check_instance_parts_with_evidence(&session,
        impl_body, &parts, &evidence);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 3u);

    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

typedef struct MethodFixture {
    TestFixture base;
    CmHirDefId trait_definition;
    CmHirDefId impl_definition;
    CmHirDefId declared_definition;
    CmHirDefId selected_definition;
    CmHirDefId caller_definition;
    CmHirBodyId caller_body;
    CmHirExprId receiver;
} MethodFixture;

static void method_fixture_init(MethodFixture *fixture, const char *trait_name,
    const char *method_name)
{
    CmHirType self_value;
    CmHirTypeId trait_self;
    CmHirTypeId impl_self;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBodyId selected_body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId selected_root;

    fixture_init(&fixture->base);
    fixture->trait_definition = add_trait(&fixture->base, trait_name);
    fixture->impl_definition = add_impl(&fixture->base,
        fixture->trait_definition);
    memset(&self_value, 0, sizeof(self_value));
    self_value.kind = CM_HIR_TYPE_SELF_KIND;
    self_value.span = test_span(20u, 24u);
    self_value.data.self_type.owner = fixture->trait_definition;
    assert(cm_hir_add_type(&fixture->base.hir, &self_value, &trait_self)
        == CM_HIR_OK);
    self_value.data.self_type.owner = fixture->impl_definition;
    assert(cm_hir_add_type(&fixture->base.hir, &self_value, &impl_self)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture->base.hir,
        fixture->base.crate_id, CM_HIR_ITEM_FUNCTION,
        test_span(30u, 45u), &fixture->declared_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture->base.hir, "self");
    parameter.type = trait_self;
    parameter.span = test_span(34u, 38u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    init_item(&item, CM_HIR_ITEM_FUNCTION, fixture->declared_definition,
        fixture->base.root, method_name, &fixture->base.hir);
    item.parent_definition = fixture->trait_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = fixture->base.u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->base.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->base.hir, &item, &item_id)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture->base.hir,
        fixture->base.crate_id, CM_HIR_ITEM_FUNCTION,
        test_span(46u, 65u), &fixture->selected_definition) == CM_HIR_OK);
    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = impl_self;
    local.span = test_span(50u, 54u);
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = fixture->selected_definition;
    body.origin = cm_hir_body_origin_item_source(
        fixture->selected_definition);
    body.source = 1u;
    body.source_expression_id = 1u;
    body.expected_type = fixture->base.u32_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.span = test_span(46u, 65u);
    assert(cm_hir_add_body(&fixture->base.hir, &body, &selected_body)
        == CM_HIR_OK);
    parameter.type = impl_self;
    init_item(&item, CM_HIR_ITEM_FUNCTION, fixture->selected_definition,
        fixture->base.root, method_name, &fixture->base.hir);
    item.parent_definition = fixture->impl_definition;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = fixture->base.u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->base.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = selected_body;
    item.data.function_item.trait_item_definition =
        fixture->declared_definition;
    assert(cm_hir_add_item(&fixture->base.hir, &item, &item_id)
        == CM_HIR_OK);
    assert(add_integer_expression(&fixture->base.hir, selected_body,
        fixture->base.u32_type, 1u, test_span(56u, 60u), &selected_root)
        == CM_HIR_OK
        && cm_hir_set_body_root_expression(&fixture->base.hir,
            selected_body, selected_root) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&fixture->base.hir,
        fixture->base.crate_id, CM_HIR_ITEM_FUNCTION,
        test_span(70u, 100u), &fixture->caller_definition) == CM_HIR_OK);
    local.name = cm_hir_intern(&fixture->base.hir, "x");
    local.type = fixture->base.u32_type;
    local.span = test_span(75u, 76u);
    memset(&body, 0, sizeof(body));
    body.owner = fixture->caller_definition;
    body.origin = cm_hir_body_origin_item_source(
        fixture->caller_definition);
    body.source = 1u;
    body.source_expression_id = 2u;
    body.expected_type = fixture->base.u32_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.span = test_span(70u, 100u);
    assert(cm_hir_add_body(&fixture->base.hir, &body, &fixture->caller_body)
        == CM_HIR_OK);
    parameter.name = local.name;
    parameter.type = local.type;
    parameter.span = local.span;
    init_item(&item, CM_HIR_ITEM_FUNCTION, fixture->caller_definition,
        fixture->base.root, "method_caller", &fixture->base.hir);
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = fixture->base.u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->base.hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = fixture->caller_body;
    assert(cm_hir_add_item(&fixture->base.hir, &item, &item_id)
        == CM_HIR_OK);
    assert(add_local_expression(&fixture->base.hir, fixture->caller_body,
        0u, fixture->base.u32_type, test_span(82u, 83u),
        &fixture->receiver) == CM_HIR_OK);
}

static CmHirExprId method_fixture_add_call(MethodFixture *fixture,
    const char *method_name, const CmHirDefId *traits,
    uint32_t trait_count)
{
    CmHirExpr expression;
    CmHirExprId call;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_METHOD_CALL;
    expression.owner_body = fixture->caller_body;
    expression.type = fixture->base.u32_type;
    expression.span = test_span(82u, 95u);
    expression.data.method_call.syntax = CM_HIR_CALLABLE_DOT_METHOD;
    expression.data.method_call.method_name = cm_hir_intern(
        &fixture->base.hir, method_name);
    expression.data.method_call.receiver = fixture->receiver;
    expression.data.method_call.in_scope_traits = (CmHirDefId *)traits;
    expression.data.method_call.in_scope_trait_count = trait_count;
    assert(cm_hir_add_expr(&fixture->base.hir, &expression, &call)
        == CM_HIR_OK
        && cm_hir_set_body_root_expression(&fixture->base.hir,
            fixture->caller_body, call) == CM_HIR_OK);
    return call;
}

static CmSemanticBodyResult method_fixture_check(MethodFixture *fixture,
    WritebackProbe *probe)
{
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture->base, fixture->caller_definition);
    assert(cm_semantic_session_init(&session, &fixture->base.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = probe == NULL
        ? cm_semantic_body_check_definition(&session, fixture->caller_body)
        : cm_semantic_body_check_definition_with_writeback(&session,
            fixture->caller_body, probe_writeback, probe);
    cm_semantic_session_destroy(&session);
    return result;
}

static void test_dot_method_callable_selection(void)
{
    MethodFixture fixture;
    CmHirDefId traits[1];
    CmSemanticBodyResult result;
    WritebackProbe probe;

    method_fixture_init(&fixture, "Value", "value");
    traits[0] = fixture.trait_definition;
    (void)method_fixture_add_call(&fixture, "value", traits, 1u);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.base.hir;
    probe.expected_body = fixture.caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_method_callable = 1;
    probe.expected_trait = fixture.trait_definition;
    probe.expected_declared_callable = fixture.declared_definition;
    probe.expected_impl = fixture.impl_definition;
    probe.expected_selected_callable = fixture.selected_definition;
    probe.expected_callable_argument_count = 1u;
    probe.expected_callable_integer_kind = CM_HIR_INT_U32;
    result = method_fixture_check(&fixture, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u);
    fixture_destroy(&fixture.base);
}

static void test_dot_method_ambiguity_is_order_independent(void)
{
    MethodFixture fixture;
    CmHirDefId second_trait;
    CmHirDefId second_impl;
    CmHirDefId traits[2];
    CmHirType self_value;
    CmHirTypeId trait_self;
    CmHirTypeId impl_self;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBodyId selected_body;
    CmHirDefId declared;
    CmHirDefId selected;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId selected_root;
    CmSemanticBodyResult result;
    unsigned int order;

    for (order = 0u; order < 2u; ++order) {
        method_fixture_init(&fixture, "First", "value");
        second_trait = add_trait(&fixture.base, "Second");
        second_impl = add_impl(&fixture.base, second_trait);
        memset(&self_value, 0, sizeof(self_value));
        self_value.kind = CM_HIR_TYPE_SELF_KIND;
        self_value.span = test_span(101u, 105u);
        self_value.data.self_type.owner = second_trait;
        assert(cm_hir_add_type(&fixture.base.hir, &self_value, &trait_self)
            == CM_HIR_OK);
        self_value.data.self_type.owner = second_impl;
        assert(cm_hir_add_type(&fixture.base.hir, &self_value, &impl_self)
            == CM_HIR_OK);
        assert(cm_hir_reserve_item_definition_as(&fixture.base.hir,
            fixture.base.crate_id, CM_HIR_ITEM_FUNCTION,
            test_span(106u, 115u), &declared) == CM_HIR_OK);
        memset(&parameter, 0, sizeof(parameter));
        parameter.name = cm_hir_intern(&fixture.base.hir, "self");
        parameter.type = trait_self;
        parameter.span = test_span(108u, 112u);
        parameter.binding_kind = CM_HIR_BINDING_NAMED;
        init_item(&item, CM_HIR_ITEM_FUNCTION, declared, fixture.base.root,
            "value", &fixture.base.hir);
        item.parent_definition = second_trait;
        item.data.function_item.signature.parameters = &parameter;
        item.data.function_item.signature.parameter_count = 1u;
        item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
        item.data.function_item.signature.return_type = fixture.base.u32_type;
        item.data.function_item.signature.abi =
            cm_hir_intern(&fixture.base.hir, "Rust");
        item.data.function_item.signature.safety = CM_HIR_SAFE;
        assert(cm_hir_add_item(&fixture.base.hir, &item, &item_id)
            == CM_HIR_OK);
        assert(cm_hir_reserve_item_definition_as(&fixture.base.hir,
            fixture.base.crate_id, CM_HIR_ITEM_FUNCTION,
            test_span(116u, 135u), &selected) == CM_HIR_OK);
        memset(&local, 0, sizeof(local));
        local.name = parameter.name;
        local.type = impl_self;
        local.span = test_span(120u, 124u);
        local.parameter_index = 0u;
        memset(&body, 0, sizeof(body));
        body.owner = selected;
        body.origin = cm_hir_body_origin_item_source(selected);
        body.source = 1u;
        body.source_expression_id = 3u;
        body.expected_type = fixture.base.u32_type;
        body.locals = &local;
        body.local_count = 1u;
        body.parameter_count = 1u;
        body.span = test_span(116u, 135u);
        assert(cm_hir_add_body(&fixture.base.hir, &body, &selected_body)
            == CM_HIR_OK);
        parameter.type = impl_self;
        init_item(&item, CM_HIR_ITEM_FUNCTION, selected, fixture.base.root,
            "value", &fixture.base.hir);
        item.parent_definition = second_impl;
        item.data.function_item.signature.parameters = &parameter;
        item.data.function_item.signature.parameter_count = 1u;
        item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
        item.data.function_item.signature.return_type = fixture.base.u32_type;
        item.data.function_item.signature.abi =
            cm_hir_intern(&fixture.base.hir, "Rust");
        item.data.function_item.signature.safety = CM_HIR_SAFE;
        item.data.function_item.body = selected_body;
        item.data.function_item.trait_item_definition = declared;
        assert(cm_hir_add_item(&fixture.base.hir, &item, &item_id)
            == CM_HIR_OK
            && add_integer_expression(&fixture.base.hir, selected_body,
                fixture.base.u32_type, 2u, test_span(126u, 130u),
                &selected_root) == CM_HIR_OK
            && cm_hir_set_body_root_expression(&fixture.base.hir,
                selected_body, selected_root) == CM_HIR_OK);
        traits[order] = fixture.trait_definition;
        traits[1u - order] = second_trait;
        (void)method_fixture_add_call(&fixture, "value", traits, 2u);
        result = method_fixture_check(&fixture, NULL);
        assert(result.status == CM_SEMANTIC_BODY_AMBIGUOUS);
        fixture_destroy(&fixture.base);
    }
}

static void test_dot_method_negative_and_no_solution(void)
{
    MethodFixture fixture;
    CmHirDefId traits[1];
    CmSemanticBodyResult result;
    WritebackProbe probe;
    size_t item_index;

    method_fixture_init(&fixture, "Value", "value");
    traits[0] = fixture.trait_definition;
    (void)method_fixture_add_call(&fixture, "missing", traits, 1u);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.base.hir;
    probe.expected_body = fixture.caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    result = method_fixture_check(&fixture, &probe);
    assert(result.status == CM_SEMANTIC_BODY_NO_SOLUTION
        && probe.invocation_count == 0u);
    fixture_destroy(&fixture.base);

    method_fixture_init(&fixture, "Value", "value");
    for (item_index = 0u; item_index < fixture.base.hir.items.len;
         ++item_index) {
        CmHirItem *impl_item;

        impl_item = (CmHirItem *)cm_vec_at(&fixture.base.hir.items,
            item_index);
        if (impl_item != NULL && cm_hir_def_id_equal(
                impl_item->definition, fixture.impl_definition)) {
            /* This post-construction polarity forgery retains the linked
             * method, so the solver authority must not authenticate it as
             * itemless negative evidence. */
            impl_item->data.impl_item.is_negative = 1;
        }
    }
    traits[0] = fixture.trait_definition;
    (void)method_fixture_add_call(&fixture, "value", traits, 1u);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.base.hir;
    probe.expected_body = fixture.caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    result = method_fixture_check(&fixture, &probe);
    assert(result.status == CM_SEMANTIC_BODY_UNSUPPORTED
        && probe.invocation_count == 0u);
    fixture_destroy(&fixture.base);
}

static void test_dot_method_unsupported_shape_has_no_writeback(void)
{
    MethodFixture fixture;
    CmHirDefId traits[1];
    CmSemanticBodyResult result;
    WritebackProbe probe;
    size_t index;

    method_fixture_init(&fixture, "Value", "value");
    for (index = 0u; index < fixture.base.hir.items.len; ++index) {
        CmHirItem *item;

        item = (CmHirItem *)cm_vec_at(&fixture.base.hir.items, index);
        if (item != NULL && cm_hir_def_id_equal(item->definition,
                fixture.declared_definition)) {
            item->data.function_item.signature.receiver =
                CM_HIR_RECEIVER_REF_SHARED;
        }
    }
    traits[0] = fixture.trait_definition;
    (void)method_fixture_add_call(&fixture, "value", traits, 1u);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.base.hir;
    probe.expected_body = fixture.caller_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    result = method_fixture_check(&fixture, &probe);
    assert(result.status == CM_SEMANTIC_BODY_UNSUPPORTED
        && probe.invocation_count == 0u);
    fixture_destroy(&fixture.base);
}

static CmHirDefId add_trait_associated(TestFixture *fixture,
    CmHirDefId trait_definition, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition(&fixture->hir,
        fixture->crate_id, test_span(20u, 35u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        name, &fixture->hir);
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
    const CmHirDefinition *trait_record;
    const CmHirItem *trait_associated;
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirItem item;
    CmHirItemId item_id;

    trait_record = cm_hir_lookup_definition(&fixture->hir,
        trait_associated_definition);
    assert(trait_record != NULL
        && trait_record->kind == CM_HIR_DEFINITION_ITEM);
    trait_associated = cm_hir_get_item(&fixture->hir,
        trait_record->entity.item_id);
    assert(trait_associated != NULL);
    assert(cm_hir_reserve_item_definition(&fixture->hir,
        fixture->crate_id, test_span(20u, 35u), &definition) == CM_HIR_OK);
    parameter_id = CM_HIR_GENERIC_PARAM_NONE;
    if (trait_associated->generic_parameter_count != 0u) {
        assert(trait_associated->generic_parameter_count == 1u);
        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = CM_HIR_GENERIC_TYPE;
        parameter.owner = definition;
        parameter.index = 0u;
        parameter.name = cm_hir_intern(&fixture->hir, "U");
        parameter.span = test_span(20u, 21u);
        assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
            &parameter_id) == CM_HIR_OK);
    }
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        NULL, &fixture->hir);
    item.name = trait_associated->name;
    item.parent_definition = impl_definition;
    item.data.type_alias_item.target = target;
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count =
        trait_associated->generic_parameter_count;
    item.data.type_alias_item.trait_item_definition =
        trait_associated_definition;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_bounded_identity(TestFixture *fixture,
    const char *name, CmHirDefId trait_definition, uint32_t base,
    const CmHirAssociatedTypeEquality *equalities, uint32_t equality_count,
    CmHirBodyId *out_body)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type_value;
    CmHirTypeId parameter_type;
    CmHirTraitPredicate predicate;
    CmHirFunctionParameter function_parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId root;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_FUNCTION,
        test_span(base, base + 39u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.index = 0u;
    parameter.name = cm_hir_intern(&fixture->hir, "T");
    parameter.span = test_span(base + 1u, base + 2u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    memset(&parameter_type_value, 0, sizeof(parameter_type_value));
    parameter_type_value.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type_value.span = parameter.span;
    parameter_type_value.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&fixture->hir, &parameter_type_value,
        &parameter_type) == CM_HIR_OK);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = parameter_type;
    predicate.trait_type.definition = trait_definition;
    predicate.span = test_span(base + 3u, base + 8u);
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    predicate.equalities = (CmHirAssociatedTypeEquality *)equalities;
    predicate.equality_count = equality_count;
    memset(&function_parameter, 0, sizeof(function_parameter));
    function_parameter.name = cm_hir_intern(&fixture->hir, "value");
    function_parameter.type = parameter_type;
    function_parameter.span = test_span(base + 10u, base + 15u);
    function_parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&local, 0, sizeof(local));
    local.name = function_parameter.name;
    local.type = parameter_type;
    local.span = function_parameter.span;
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = parameter_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = base;
    body.span = test_span(base, base + 39u);
    assert(cm_hir_add_body(&fixture->hir, &body, out_body) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, definition, fixture->root, name,
        &fixture->hir);
    item.span = body.span;
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.function_item.signature.parameters = &function_parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = parameter_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, *out_body, 0u,
        parameter_type, test_span(base + 20u, base + 25u), &root)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->hir, *out_body, root)
        == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_caller(TestFixture *fixture, const char *name,
    CmHirDefId callee, uint32_t base, CmHirBodyId *out_body,
    CmHirExprId *out_call)
{
    CmHirDefId definition;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId argument;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_FUNCTION,
        test_span(base, base + 39u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture->hir, "input");
    parameter.type = fixture->u32_type;
    parameter.span = test_span(base + 2u, base + 7u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = fixture->u32_type;
    local.span = parameter.span;
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = fixture->u32_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = base;
    body.span = test_span(base, base + 39u);
    assert(cm_hir_add_body(&fixture->hir, &body, out_body) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, definition, fixture->root, name,
        &fixture->hir);
    item.span = body.span;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = fixture->u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, *out_body, 0u,
        fixture->u32_type, test_span(base + 15u, base + 20u), &argument)
        == CM_HIR_OK);
    assert(add_call_expression(&fixture->hir, *out_body,
        callee, &fixture->u32_type, 1u, &argument, 1u,
        fixture->u32_type, test_span(base + 10u, base + 25u), out_call)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->hir, *out_body,
        *out_call) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_two_parameter_nested_caller(TestFixture *fixture,
    CmHirBodyId *out_body, CmHirExprId *out_call,
    CmHirExprId *out_second_argument)
{
    CmHirDefId callee_definition;
    CmHirDefId caller_definition;
    CmHirGenericParam generic;
    CmHirGenericParamId callee_parameters[2];
    CmHirGenericParamId caller_parameters[2];
    CmHirTypeId callee_types[4];
    CmHirTypeId caller_types[4];
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId arguments[2];
    uint32_t index;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_FUNCTION, test_span(600u, 639u),
        &callee_definition) == CM_HIR_OK);
    for (index = 0u; index < 2u; ++index) {
        memset(&generic, 0, sizeof(generic));
        generic.kind = CM_HIR_GENERIC_TYPE;
        generic.owner = callee_definition;
        generic.index = index;
        generic.name = cm_hir_intern(&fixture->hir,
            index == 0u ? "A" : "B");
        generic.span = test_span(601u + index, 602u + index);
        assert(cm_hir_add_generic_param(&fixture->hir, &generic,
            &callee_parameters[index]) == CM_HIR_OK);
    }
    callee_types[0] = add_parameter_type(&fixture->hir,
        callee_parameters[0]);
    callee_types[1] = add_parameter_type(&fixture->hir,
        callee_parameters[1]);
    callee_types[2] = add_static_reference_type(&fixture->hir,
        callee_types[0]);
    callee_types[3] = add_pair_type(&fixture->hir, callee_types[1],
        callee_types[2]);
    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = cm_hir_intern(&fixture->hir, "left");
    parameters[0].type = callee_types[2];
    parameters[0].span = test_span(610u, 614u);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[1].name = cm_hir_intern(&fixture->hir, "right");
    parameters[1].type = callee_types[1];
    parameters[1].span = test_span(615u, 619u);
    parameters[1].binding_kind = CM_HIR_BINDING_NAMED;
    init_item(&item, CM_HIR_ITEM_FUNCTION, callee_definition,
        fixture->root, "nested_swap", &fixture->hir);
    item.span = test_span(600u, 639u);
    item.generic_parameter_start = callee_parameters[0];
    item.generic_parameter_count = 2u;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.return_type = callee_types[3];
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_FUNCTION, test_span(640u, 679u),
        &caller_definition) == CM_HIR_OK);
    for (index = 0u; index < 2u; ++index) {
        memset(&generic, 0, sizeof(generic));
        generic.kind = CM_HIR_GENERIC_TYPE;
        generic.owner = caller_definition;
        generic.index = index;
        generic.name = cm_hir_intern(&fixture->hir,
            index == 0u ? "X" : "Y");
        generic.span = test_span(641u + index, 642u + index);
        assert(cm_hir_add_generic_param(&fixture->hir, &generic,
            &caller_parameters[index]) == CM_HIR_OK);
    }
    caller_types[0] = add_parameter_type(&fixture->hir,
        caller_parameters[0]);
    caller_types[1] = add_parameter_type(&fixture->hir,
        caller_parameters[1]);
    caller_types[2] = add_static_reference_type(&fixture->hir,
        caller_types[0]);
    caller_types[3] = add_pair_type(&fixture->hir, caller_types[1],
        caller_types[2]);
    memset(parameters, 0, sizeof(parameters));
    memset(locals, 0, sizeof(locals));
    for (index = 0u; index < 2u; ++index) {
        parameters[index].name = cm_hir_intern(&fixture->hir,
            index == 0u ? "x" : "y");
        parameters[index].type = index == 0u
            ? caller_types[2] : caller_types[1];
        parameters[index].span = test_span(650u + index * 3u,
            652u + index * 3u);
        parameters[index].binding_kind = CM_HIR_BINDING_NAMED;
        locals[index].name = parameters[index].name;
        locals[index].type = parameters[index].type;
        locals[index].span = parameters[index].span;
        locals[index].parameter_index = index;
    }
    memset(&body, 0, sizeof(body));
    body.owner = caller_definition;
    body.origin = cm_hir_body_origin_item_source(caller_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = caller_types[3];
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 640u;
    body.span = test_span(640u, 679u);
    assert(cm_hir_add_body(&fixture->hir, &body, out_body) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, caller_definition,
        fixture->root, "nested_caller", &fixture->hir);
    item.span = body.span;
    item.generic_parameter_start = caller_parameters[0];
    item.generic_parameter_count = 2u;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.return_type = caller_types[3];
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, *out_body, 0u,
        caller_types[2], test_span(660u, 663u), &arguments[0]) == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, *out_body, 1u,
        caller_types[1], test_span(664u, 667u), &arguments[1]) == CM_HIR_OK);
    assert(add_call_expression(&fixture->hir, *out_body, callee_definition,
        caller_types, 2u, arguments, 2u, caller_types[3],
        test_span(658u, 670u), out_call) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->hir, *out_body,
        *out_call) == CM_HIR_OK);
    *out_second_argument = arguments[1];
    return caller_definition;
}

typedef struct ConstraintBody {
    CmHirDefId owner;
    CmHirBodyId body;
    CmHirExprId root;
    CmHirExprId initializer_left;
    CmHirExprId initializer;
    CmHirExprId condition;
    CmHirExprId then_block;
    CmHirExprId else_block;
    CmHirDefId aggregate_definition;
    CmHirExprId aggregate;
    CmHirExprId field_expression;
    CmHirExprId if_expression;
} ConstraintBody;

static ConstraintBody add_constraint_body(TestFixture *fixture)
{
    ConstraintBody built;
    CmHirFunctionParameter parameters[2];
    CmHirField aggregate_field;
    CmHirLocal locals[3];
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirStatement statement;
    CmHirExprId one;
    CmHirExprId condition_left;
    CmHirExprId condition_right;
    CmHirExprId then_tail;
    CmHirExprId aggregate_value;
    CmHirType aggregate_type_value;
    CmHirTypeId aggregate_type;
    CmHirAggregateFieldValue aggregate_field_value;

    memset(&built, 0, sizeof(built));
    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_STRUCT, test_span(680u, 699u),
        &built.aggregate_definition) == CM_HIR_OK);
    memset(&aggregate_field, 0, sizeof(aggregate_field));
    aggregate_field.name = cm_hir_intern(&fixture->hir, "value");
    aggregate_field.type = fixture->u32_type;
    aggregate_field.visibility.kind = CM_HIR_VIS_PRIVATE;
    aggregate_field.visibility.restriction = cm_hir_def_id_none();
    aggregate_field.span = test_span(685u, 690u);
    init_item(&item, CM_HIR_ITEM_STRUCT, built.aggregate_definition,
        fixture->root, "Boxed", &fixture->hir);
    item.span = test_span(680u, 699u);
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = &aggregate_field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    memset(&aggregate_type_value, 0, sizeof(aggregate_type_value));
    aggregate_type_value.kind = CM_HIR_TYPE_ADT_KIND;
    aggregate_type_value.span = test_span(680u, 699u);
    aggregate_type_value.data.named_type.definition =
        built.aggregate_definition;
    assert(cm_hir_add_type(&fixture->hir, &aggregate_type_value,
        &aggregate_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_FUNCTION, test_span(700u, 780u),
        &built.owner) == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = cm_hir_intern(&fixture->hir, "left");
    parameters[0].type = fixture->u32_type;
    parameters[0].span = test_span(701u, 704u);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[1].name = cm_hir_intern(&fixture->hir, "right");
    parameters[1].type = fixture->u32_type;
    parameters[1].span = test_span(705u, 708u);
    parameters[1].binding_kind = CM_HIR_BINDING_NAMED;
    memset(locals, 0, sizeof(locals));
    locals[0].name = parameters[0].name;
    locals[0].type = parameters[0].type;
    locals[0].span = parameters[0].span;
    locals[0].parameter_index = 0u;
    locals[1].name = parameters[1].name;
    locals[1].type = parameters[1].type;
    locals[1].span = parameters[1].span;
    locals[1].parameter_index = 1u;
    locals[2].name = cm_hir_intern(&fixture->hir, "sum");
    locals[2].type = fixture->u32_type;
    locals[2].span = test_span(710u, 718u);
    locals[2].parameter_index = CM_HIR_PARAMETER_INDEX_NONE;
    memset(&body, 0, sizeof(body));
    body.owner = built.owner;
    body.origin = cm_hir_body_origin_item_source(built.owner);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = fixture->u32_type;
    body.locals = locals;
    body.local_count = 3u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 700u;
    body.span = test_span(700u, 780u);
    assert(cm_hir_add_body(&fixture->hir, &body, &built.body) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_FUNCTION, built.owner, fixture->root,
        "constraint_body", &fixture->hir);
    item.span = body.span;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.return_type = fixture->u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = built.body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);

    assert(add_local_expression(&fixture->hir, built.body, 0u,
        fixture->u32_type, test_span(711u, 712u),
        &built.initializer_left) == CM_HIR_OK);
    assert(add_integer_expression(&fixture->hir, built.body,
        fixture->u32_type, 1u, test_span(713u, 714u), &one) == CM_HIR_OK);
    assert(add_binary_expression(&fixture->hir, built.body,
        CM_HIR_BINARY_ADD, built.initializer_left, one, fixture->u32_type,
        test_span(710u, 715u), &built.initializer) == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, built.body, 1u,
        fixture->u32_type, test_span(721u, 722u), &condition_left)
        == CM_HIR_OK);
    assert(add_integer_expression(&fixture->hir, built.body,
        fixture->u32_type, 0u, test_span(723u, 724u), &condition_right)
        == CM_HIR_OK);
    assert(add_binary_expression(&fixture->hir, built.body,
        CM_HIR_BINARY_EQUAL, condition_left, condition_right,
        fixture->bool_type, test_span(720u, 725u), &built.condition)
        == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, built.body, 2u,
        fixture->u32_type, test_span(731u, 732u), &then_tail) == CM_HIR_OK);
    assert(add_block_expression(&fixture->hir, built.body, NULL, 0u,
        then_tail, fixture->u32_type, test_span(730u, 735u),
        &built.then_block) == CM_HIR_OK);
    assert(add_local_expression(&fixture->hir, built.body, 0u,
        fixture->u32_type, test_span(741u, 742u), &aggregate_value)
        == CM_HIR_OK);
    memset(&aggregate_field_value, 0, sizeof(aggregate_field_value));
    aggregate_field_value.field_index = 0u;
    aggregate_field_value.value = aggregate_value;
    aggregate_field_value.span = test_span(741u, 742u);
    assert(add_aggregate_expression(&fixture->hir, built.body,
        built.aggregate_definition, &aggregate_field_value, 1u,
        aggregate_type, test_span(740u, 743u), &built.aggregate)
        == CM_HIR_OK);
    assert(add_field_expression(&fixture->hir, built.body,
        built.aggregate, built.aggregate_definition, 0u,
        fixture->u32_type, test_span(740u, 744u),
        &built.field_expression) == CM_HIR_OK);
    assert(add_block_expression(&fixture->hir, built.body, NULL, 0u,
        built.field_expression, fixture->u32_type, test_span(740u, 745u),
        &built.else_block) == CM_HIR_OK);
    assert(add_if_expression(&fixture->hir, built.body,
        built.condition, built.then_block, built.else_block,
        fixture->u32_type, test_span(720u, 746u), &built.if_expression)
        == CM_HIR_OK);
    memset(&statement, 0, sizeof(statement));
    statement.kind = CM_HIR_STATEMENT_LET;
    statement.span = test_span(709u, 718u);
    statement.data.let_statement.local_index = 2u;
    statement.data.let_statement.initializer = built.initializer;
    assert(add_block_expression(&fixture->hir, built.body, &statement, 1u,
        built.if_expression, fixture->u32_type, test_span(709u, 750u),
        &built.root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->hir, built.body,
        built.root) == CM_HIR_OK);
    return built;
}

static void fixture_init(TestFixture *fixture)
{
    CmHirAssociatedTypeEquality equality;

    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "semantic_body"),
        CM_HIR_EDITION_2024, test_span(0u, 240u), &fixture->crate_id,
        &fixture->root) == CM_HIR_OK);
    fixture->u32_type = add_type(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND);
    fixture->u8_type = add_integer_type(&fixture->hir, CM_HIR_INT_U8);
    fixture->bool_type = add_type(&fixture->hir, CM_HIR_TYPE_BOOL_KIND);
    fixture->infer_type = add_type(&fixture->hir, CM_HIR_TYPE_INFER_KIND);
    fixture->present_trait = add_trait(fixture, "Present");
    fixture->missing_trait = add_trait(fixture, "Missing");
    fixture->present_associated = add_trait_associated(fixture,
        fixture->present_trait, "Output");
    fixture->present_impl = add_impl(fixture, fixture->present_trait);
    fixture->present_impl_associated = add_impl_associated(fixture,
        fixture->present_impl, fixture->present_associated,
        fixture->u8_type);
    fixture->present_callee = add_bounded_identity(fixture, "present_id",
        fixture->present_trait, 40u, NULL, 0u,
        &fixture->present_callee_body);
    fixture->missing_callee = add_bounded_identity(fixture, "missing_id",
        fixture->missing_trait, 80u, NULL, 0u,
        &fixture->missing_callee_body);
    memset(&equality, 0, sizeof(equality));
    equality.associated_type = fixture->present_associated;
    equality.value = fixture->u8_type;
    equality.span = test_span(112u, 118u);
    fixture->projected_callee = add_bounded_identity(fixture,
        "projected_id", fixture->present_trait, 110u, &equality, 1u,
        &fixture->projected_callee_body);
    fixture->present_caller = add_caller(fixture, "present_caller",
        fixture->present_callee, 120u, &fixture->present_body,
        &fixture->present_call);
    fixture->missing_caller = add_caller(fixture, "missing_caller",
        fixture->missing_callee, 170u, &fixture->missing_body,
        &fixture->missing_call);
    fixture->projected_caller = add_caller(fixture, "projected_caller",
        fixture->projected_callee, 220u, &fixture->projected_body,
        &fixture->projected_call);
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

static CmHirItem *mutable_item(TestFixture *fixture, CmHirDefId definition)
{
    const CmHirDefinition *record;

    record = cm_hir_lookup_definition(&fixture->hir, definition);
    assert(record != NULL && record->kind == CM_HIR_DEFINITION_ITEM);
    return (CmHirItem *)cm_vec_at(&fixture->hir.items,
        (size_t)record->entity.item_id - 1u);
}

static CmHirTypeId add_projection_type(TestFixture *fixture,
    CmHirDefId trait_definition, CmHirDefId associated_definition,
    CmHirTypeId self_type)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = test_span(2u, 5u);
    type.data.projection_type.self_type = self_type;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_hir_add_type(&fixture->hir, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static void make_associated_generic(TestFixture *fixture,
    CmHirDefId associated_definition)
{
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirItem *associated;

    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = associated_definition;
    parameter.index = 0u;
    parameter.name = cm_hir_intern(&fixture->hir, "U");
    parameter.span = test_span(2u, 5u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    associated = mutable_item(fixture, associated_definition);
    assert(associated != NULL && associated->kind == CM_HIR_ITEM_TYPE_ALIAS);
    associated->generic_parameter_start = parameter_id;
    associated->generic_parameter_count = 1u;
}

static void fill_typeck_for_reallocation(CmTypeckContext *typeck)
{
    CmTypeckType type;
    CmTypeckTypeId type_id;
    uint32_t index;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_UNIT;
    type.span = test_span(2u, 5u);
    for (index = 0u; index < 64u; ++index) {
        assert(cm_typeck_add_type(typeck, &type, &type_id) == CM_TYPECK_OK);
        assert(type_id != CM_TYPECK_TYPE_NONE);
    }
}

static void test_positive_impl_and_missing_metadata(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.present_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    cm_semantic_session_destroy(&session);

    options = session_options(&fixture, fixture.missing_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_METADATA
        && result.expression == fixture.missing_call
        && cm_hir_def_id_equal(result.callee, fixture.missing_callee)
        && result.predicate_index == 0u
        && result.solver_kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && cm_typeck_type_count(typeck) == type_count);
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_METADATA
        && cm_typeck_type_count(typeck) == type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_intrinsic_body_projection_normalization(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    CmHirItem *owner;
    CmHirBody *body;
    CmHirExpr *root;
    CmHirTypeId projection;
    CmHirDefId missing_associated;
    CmTypeckTypeId resolved;
    const CmTypeckType *resolved_type;
    WritebackProbe probe;
    size_t type_count;

    fixture_init(&fixture);
    projection = add_projection_type(&fixture, fixture.present_trait,
        fixture.present_associated, fixture.u32_type);
    owner = mutable_item(&fixture, fixture.present_callee);
    body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
        (size_t)fixture.present_callee_body - 1u);
    root = body == NULL ? NULL : (CmHirExpr *)cm_vec_at(
        &fixture.hir.expressions, (size_t)body->root_expression - 1u);
    assert(owner != NULL && body != NULL && root != NULL
        && owner->data.function_item.signature.parameter_count == 1u
        && body->local_count == 1u);
    owner->data.function_item.signature.parameters[0].type = projection;
    owner->data.function_item.signature.return_type = projection;
    body->locals[0].type = projection;
    body->expected_type = projection;
    root->type = projection;

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.present_callee);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.present_callee_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        fixture.present_callee_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u
        && probe.owned_term_count == 1u
        && cm_typeck_resolve(typeck, probe.first_owned_term, &resolved)
            == CM_TYPECK_OK);
    resolved_type = cm_typeck_get_type(typeck, resolved);
    assert(resolved_type != NULL
        && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
        && resolved_type->data.integer_type == CM_HIR_INT_U8);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    missing_associated = add_trait_associated(&fixture,
        fixture.missing_trait, "MissingOutput");
    projection = add_projection_type(&fixture, fixture.missing_trait,
        missing_associated, fixture.u32_type);
    owner = mutable_item(&fixture, fixture.present_callee);
    body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
        (size_t)fixture.present_callee_body - 1u);
    root = body == NULL ? NULL : (CmHirExpr *)cm_vec_at(
        &fixture.hir.expressions, (size_t)body->root_expression - 1u);
    assert(owner != NULL && body != NULL && root != NULL);
    owner->data.function_item.signature.parameters[0].type = projection;
    owner->data.function_item.signature.return_type = projection;
    body->locals[0].type = projection;
    body->expected_type = projection;
    root->type = projection;
    options = session_options(&fixture, fixture.present_callee);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.present_callee_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        fixture.present_callee_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_METADATA
        && result.solver_kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && probe.invocation_count == 0u
        && cm_typeck_type_count(typeck) == type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_projected_call_substitution_writeback(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmHirItem *callee;
    CmHirItem *caller;
    CmHirBody *body;
    CmHirExpr *call;
    CmHirExpr *argument;
    CmHirTypeId projection;
    WritebackProbe probe;

    fixture_init(&fixture);
    projection = add_projection_type(&fixture, fixture.present_trait,
        fixture.present_associated, fixture.u32_type);
    callee = mutable_item(&fixture, fixture.present_callee);
    caller = mutable_item(&fixture, fixture.present_caller);
    body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
        (size_t)fixture.present_body - 1u);
    call = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)fixture.present_call - 1u);
    argument = call == NULL || call->data.call.argument_count != 1u
        ? NULL : (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
            (size_t)call->data.call.arguments[0] - 1u);
    assert(callee != NULL && caller != NULL && body != NULL
        && call != NULL && argument != NULL && body->local_count == 1u);
    /* Isolate substitution/signature normalization from the callee bound. */
    callee->predicates = NULL;
    callee->predicate_count = 0u;
    caller->data.function_item.signature.parameters[0].type =
        fixture.u8_type;
    caller->data.function_item.signature.return_type = fixture.u8_type;
    body->locals[0].type = fixture.u8_type;
    body->expected_type = fixture.u8_type;
    argument->type = fixture.u8_type;
    call->type = fixture.u8_type;
    call->data.call.type_substitutions[0] = projection;

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.present_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.present_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_integer_kind = 1;
    probe.expected_integer_kind = CM_HIR_INT_U8;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        fixture.present_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN
        && probe.invocation_count == 1u
        && probe.owned_term_count == 2u);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_projection_equality_proof_mismatch_and_rollback(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    CmHirItem *callee;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.projected_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, fixture.projected_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    (void)add_type(&fixture.hir, CM_HIR_TYPE_INTEGER_KIND);
    assert(!cm_semantic_session_is_current(&session));
    result = cm_semantic_body_check_calls(&session, fixture.projected_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_STALE);
    cm_semantic_session_destroy(&session);

    callee = mutable_item(&fixture, fixture.projected_callee);
    assert(callee != NULL && callee->predicate_count == 1u
        && callee->predicates[0].equality_count == 1u);
    callee->predicates[0].equalities[0].value = fixture.u32_type;
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    fill_typeck_for_reallocation(typeck);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_calls(&session, fixture.projected_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_NO_SOLUTION
        && result.solver_kind == CM_TRAIT_SOLVER_NO_SOLUTION
        && result.expression == fixture.projected_call
        && result.predicate_index == 0u
        && cm_typeck_type_count(typeck) == type_count);
    result = cm_semantic_body_check_calls(&session, fixture.projected_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_NO_SOLUTION
        && cm_typeck_type_count(typeck) == type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_projection_fail_closed_shapes_and_overlap(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmHirItem *callee;
    CmHirTypeId nested_projection;
    CmHirDefId gat_associated;
    CmHirDefId gat_impl_associated;
    CmHirDefId gat_callee;
    CmHirBodyId gat_callee_body;
    CmHirDefId gat_caller;
    CmHirBodyId gat_body;
    CmHirExprId gat_call;
    CmHirAssociatedTypeEquality equality;
    CmHirDefId overlap_impl;
    CmTypeckContext *typeck;
    size_t type_count;

    fixture_init(&fixture);
    nested_projection = add_projection_type(&fixture,
        fixture.present_trait, fixture.present_associated,
        fixture.u32_type);
    callee = mutable_item(&fixture, fixture.projected_callee);
    callee->predicates[0].equalities[0].value = nested_projection;
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.projected_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_calls(&session, fixture.projected_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, fixture.projected_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN
        && cm_typeck_type_count(typeck) >= type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    gat_associated = add_trait_associated(&fixture, fixture.present_trait,
        "GatOutput");
    make_associated_generic(&fixture, gat_associated);
    gat_impl_associated = add_impl_associated(&fixture,
        fixture.present_impl, gat_associated, fixture.u8_type);
    assert(!cm_hir_def_id_is_none(gat_impl_associated));
    memset(&equality, 0, sizeof(equality));
    equality.associated_type = gat_associated;
    equality.value = fixture.u8_type;
    equality.span = test_span(300u, 306u);
    gat_callee = add_bounded_identity(&fixture, "gat_id",
        fixture.present_trait, 290u, NULL, 0u, &gat_callee_body);
    gat_caller = add_caller(&fixture, "gat_caller", gat_callee, 340u,
        &gat_body, &gat_call);
    callee = mutable_item(&fixture, gat_callee);
    callee->predicates[0].equalities = &equality;
    callee->predicates[0].equality_count = 1u;
    assert(!cm_hir_def_id_is_none(gat_caller)
        && gat_call != CM_HIR_EXPR_NONE);
    options = session_options(&fixture, gat_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, gat_body, NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_UNSUPPORTED
        && result.solver_kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    overlap_impl = add_impl(&fixture, fixture.present_trait);
    (void)add_impl_associated(&fixture, overlap_impl,
        fixture.present_associated, fixture.u8_type);
    options = session_options(&fixture, fixture.projected_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_calls(&session, fixture.projected_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_AMBIGUOUS
        && result.solver_kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && cm_typeck_type_count(typeck) == type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_projection_equality_order(void)
{
    TestFixture fixture;
    CmHirDefId gat_associated;
    CmHirAssociatedTypeEquality normal_first[2];
    CmHirAssociatedTypeEquality gat_first[2];
    CmHirDefId first_callee;
    CmHirDefId second_callee;
    CmHirBodyId ignored_callee_body;
    CmHirDefId first_caller;
    CmHirDefId second_caller;
    CmHirBodyId first_body;
    CmHirBodyId second_body;
    CmHirExprId ignored_call;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;

    fixture_init(&fixture);
    gat_associated = add_trait_associated(&fixture, fixture.present_trait,
        "OrderedGat");
    make_associated_generic(&fixture, gat_associated);
    (void)add_impl_associated(&fixture, fixture.present_impl,
        gat_associated, fixture.u8_type);
    memset(normal_first, 0, sizeof(normal_first));
    normal_first[0].associated_type = fixture.present_associated;
    normal_first[0].value = fixture.u32_type;
    normal_first[0].span = test_span(390u, 396u);
    normal_first[1].associated_type = gat_associated;
    normal_first[1].value = fixture.u8_type;
    normal_first[1].span = test_span(397u, 403u);
    gat_first[0] = normal_first[1];
    gat_first[1] = normal_first[0];
    first_callee = add_bounded_identity(&fixture, "normal_first",
        fixture.present_trait, 380u, NULL, 0u,
        &ignored_callee_body);
    second_callee = add_bounded_identity(&fixture, "gat_first",
        fixture.present_trait, 430u, NULL, 0u,
        &ignored_callee_body);
    first_caller = add_caller(&fixture, "normal_first_caller",
        first_callee, 480u, &first_body, &ignored_call);
    second_caller = add_caller(&fixture, "gat_first_caller",
        second_callee, 530u, &second_body, &ignored_call);
    mutable_item(&fixture, first_callee)->predicates[0].equalities =
        normal_first;
    mutable_item(&fixture, first_callee)->predicates[0].equality_count = 2u;
    mutable_item(&fixture, second_callee)->predicates[0].equalities =
        gat_first;
    mutable_item(&fixture, second_callee)->predicates[0].equality_count = 2u;

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, first_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, first_body, NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_NO_SOLUTION
        && result.solver_kind == CM_TRAIT_SOLVER_NO_SOLUTION);
    cm_semantic_session_destroy(&session);

    options = session_options(&fixture, second_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, second_body, NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_UNSUPPORTED
        && result.solver_kind == CM_TRAIT_SOLVER_UNSUPPORTED);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_callee_environment_cannot_self_prove(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmHirTypeId substitution;
    size_t type_count;
    CmTypeckContext *typeck;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.missing_callee);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && cm_typeck_type_count(typeck) == type_count);
    substitution = fixture.u32_type;
    result = cm_semantic_body_check_calls(&session,
        fixture.missing_callee_body, &substitution, 1u);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_concrete_instance_writeback(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmHirTypeId substitution;
    WritebackProbe probe;
    size_t type_count;
    CmTypeckContext *typeck;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.present_callee);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    substitution = fixture.u32_type;
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.present_callee_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    probe.require_integer_kind = 1;
    probe.expected_integer_kind = CM_HIR_INT_U32;
    result = cm_semantic_body_check_instance_with_writeback(&session,
        fixture.present_callee_body, &substitution, 1u,
        probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN
        && probe.invocation_count == 1u
        && probe.owned_term_count == 1u);
    cm_semantic_session_destroy(&session);

    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.missing_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    type_count = cm_typeck_type_count(typeck);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.missing_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;
    result = cm_semantic_body_check_instance_with_writeback(&session,
        fixture.missing_body, NULL, 0u, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_METADATA
        && result.expression == fixture.missing_call
        && cm_hir_def_id_equal(result.callee, fixture.missing_callee)
        && result.predicate_index == 0u
        && result.solver_kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
        && probe.invocation_count == 0u
        && cm_typeck_type_count(typeck) == type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_pending_shapes_and_atomicity(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    CmHirItem *callee;
    CmHirExpr *call;
    CmInternId binder_name;
    CmHirAssociatedTypeEquality equality;
    CmHirOutlivesPredicate outlives;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.missing_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    callee = mutable_item(&fixture, fixture.missing_callee);
    call = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)fixture.missing_call - 1u);
    assert(callee != NULL && call != NULL);

    callee->predicates[0].modifier = CM_HIR_PREDICATE_CONST;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_MODIFIER);
    callee->predicates[0].modifier = CM_HIR_PREDICATE_REQUIRED;

    binder_name = cm_hir_intern(&fixture.hir, "late");
    /* The append above makes the old session stale; rebuild before checking. */
    cm_semantic_session_destroy(&session);
    options = session_options(&fixture, fixture.missing_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    callee->predicates[0].binder.lifetimes = &binder_name;
    callee->predicates[0].binder.lifetime_count = 1u;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED);
    callee->predicates[0].binder.lifetimes = NULL;
    callee->predicates[0].binder.lifetime_count = 0u;

    memset(&equality, 0, sizeof(equality));
    equality.associated_type = fixture.missing_trait;
    equality.value = fixture.u32_type;
    equality.span = test_span(1u, 2u);
    callee->predicates[0].equalities = &equality;
    callee->predicates[0].equality_count = 1u;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_METADATA
        && result.solver_kind == CM_TRAIT_SOLVER_DEFERRED_METADATA);
    callee->predicates[0].equalities = NULL;
    callee->predicates[0].equality_count = 0u;

    memset(&outlives, 0, sizeof(outlives));
    outlives.subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives.subject.type = fixture.u32_type;
    outlives.bound.kind = CM_HIR_REGION_STATIC;
    callee->outlives_predicates = &outlives;
    callee->outlives_predicate_count = 1u;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_OUTLIVES);
    callee->outlives_predicates = NULL;
    callee->outlives_predicate_count = 0u;

    call->data.call.type_substitutions[0] = fixture.infer_type;
    result = cm_semantic_body_check_calls(&session, fixture.missing_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_INFERENCE);
    call->data.call.type_substitutions[0] = fixture.u32_type;
    assert(cm_typeck_type_count(typeck) == type_count);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_two_parameter_nested_substitution_and_rollback(void)
{
    TestFixture fixture;
    CmHirDefId caller;
    CmHirBodyId body;
    CmHirExprId call;
    CmHirExprId second_argument;
    CmHirExpr *argument;
    CmHirTypeId original_type;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    size_t type_count;

    fixture_init(&fixture);
    caller = add_two_parameter_nested_caller(&fixture, &body, &call,
        &second_argument);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_definition(&session, body);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && result.expression == CM_HIR_EXPR_NONE
        && result.solver_kind == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_definition(&session, body);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    type_count = cm_typeck_type_count(typeck);

    argument = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)second_argument - 1u);
    assert(argument != NULL && call != CM_HIR_EXPR_NONE);
    original_type = argument->type;
    argument->type = fixture.u32_type;
    result = cm_semantic_body_check_definition(&session, body);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH
        && result.expression == second_argument
        && cm_typeck_type_count(typeck) == type_count);
    result = cm_semantic_body_check_definition(&session, body);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && cm_typeck_type_count(typeck) == type_count);
    argument->type = original_type;
    result = cm_semantic_body_check_definition(&session, body);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_whole_body_constraints_and_rollback(void)
{
    TestFixture fixture;
    ConstraintBody built;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    CmHirBody *body;
    CmHirExpr *root;
    CmHirExpr *initializer_left;
    CmHirExpr *initializer;
    CmHirExpr *condition;
    CmHirExpr *then_block;
    CmHirExpr *else_block;
    CmHirExpr *aggregate;
    CmHirExpr *field_expression;
    CmHirExpr *if_expression;
    CmHirTypeId saved_type;
    CmHirExprId saved_expression;
    uint32_t saved_local;
    size_t type_count;

    fixture_init(&fixture);
    built = add_constraint_body(&fixture);
    body = (CmHirBody *)cm_vec_at(&fixture.hir.bodies,
        (size_t)built.body - 1u);
    root = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.root - 1u);
    initializer_left = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.initializer_left - 1u);
    initializer = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.initializer - 1u);
    condition = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.condition - 1u);
    then_block = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.then_block - 1u);
    else_block = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.else_block - 1u);
    aggregate = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.aggregate - 1u);
    field_expression = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.field_expression - 1u);
    if_expression = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)built.if_expression - 1u);
    assert(body != NULL && root != NULL && initializer_left != NULL
        && initializer != NULL && condition != NULL && then_block != NULL
        && else_block != NULL && aggregate != NULL
        && field_expression != NULL && if_expression != NULL);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, built.owner);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    type_count = cm_typeck_type_count(typeck);

    saved_local = initializer_left->data.local.local_index;
    initializer_left->data.local.local_index = body->local_count;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && result.expression == built.initializer_left
        && cm_typeck_type_count(typeck) == type_count);
    initializer_left->data.local.local_index = saved_local;

    saved_local = body->locals[1].parameter_index;
    body->locals[1].parameter_index = 0u;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && cm_typeck_type_count(typeck) == type_count);
    body->locals[1].parameter_index = saved_local;

    saved_type = initializer->type;
    initializer->type = fixture.u8_type;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH
        && result.expression == built.initializer
        && cm_typeck_type_count(typeck) == type_count);
    initializer->type = saved_type;

    saved_type = root->type;
    root->type = fixture.u8_type;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH
        && result.expression == built.root
        && cm_typeck_type_count(typeck) == type_count);
    root->type = saved_type;

    saved_type = condition->type;
    condition->type = fixture.u32_type;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && result.expression == built.condition
        && cm_typeck_type_count(typeck) == type_count);
    condition->type = saved_type;

    saved_type = else_block->type;
    else_block->type = fixture.u8_type;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH
        && result.expression == built.else_block
        && cm_typeck_type_count(typeck) == type_count);
    else_block->type = saved_type;

    saved_local = aggregate->data.aggregate.fields[0].field_index;
    aggregate->data.aggregate.fields[0].field_index = 1u;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && result.expression == built.aggregate
        && cm_typeck_type_count(typeck) == type_count);
    aggregate->data.aggregate.fields[0].field_index = saved_local;

    saved_type = field_expression->type;
    field_expression->type = fixture.u8_type;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && result.typeck_status == CM_TYPECK_TYPE_MISMATCH
        && result.expression == built.field_expression
        && cm_typeck_type_count(typeck) == type_count);
    field_expression->type = saved_type;

    saved_expression = if_expression->data.if_expr.then_expression;
    if_expression->data.if_expr.then_expression = built.condition;
    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && result.expression == built.if_expression
        && cm_typeck_type_count(typeck) == type_count);
    if_expression->data.if_expr.then_expression = saved_expression;

    result = cm_semantic_body_check_definition(&session, built.body);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_malformed_foreign_and_stale(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    CmHirExpr *call;
    CmHirItem *callee;
    CmHirDefId original_callee;
    CmHirTypeId extra;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.present_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_OK);
    type_count = cm_typeck_type_count(typeck);
    call = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)fixture.present_call - 1u);
    callee = mutable_item(&fixture, fixture.present_callee);
    assert(call != NULL && callee != NULL);

    original_callee = call->data.call.callee;
    call->data.call.callee.crate_id = fixture.crate_id + 100u;
    call->data.call.callee.index = 1u;
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && cm_typeck_type_count(typeck) == type_count);
    call->data.call.callee = original_callee;

    call->data.call.type_substitution_count = 0u;
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_SUBSTITUTION
        && cm_typeck_type_count(typeck) == type_count);
    call->data.call.type_substitution_count = 1u;

    callee->predicates[0].binder.lifetimes = (CmInternId *)&extra;
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && cm_typeck_type_count(typeck) == type_count);
    callee->predicates[0].binder.lifetimes = NULL;

    extra = add_type(&fixture.hir, CM_HIR_TYPE_BOOL_KIND);
    assert(!cm_semantic_session_is_current(&session));
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_STALE);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_definition_writeback_contract_and_rollback(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    CmTypeckContext *typeck;
    CmHirExpr *root;
    CmHirTypeId saved_type;
    CmTypeckTypeId variable;
    CmTypeckTypeId resolved;
    const CmTypeckType *resolved_type;
    WritebackProbe probe;
    size_t type_count;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    options = session_options(&fixture, fixture.present_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    typeck = cm_semantic_session_typeck(&session);
    assert(typeck != NULL);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.present_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_OK;

    root = (CmHirExpr *)cm_vec_at(&fixture.hir.expressions,
        (size_t)fixture.present_call - 1u);
    assert(root != NULL);
    saved_type = root->type;
    root->type = fixture.u8_type;
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_definition_with_writeback(&session,
        fixture.present_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_TYPECK_FAILURE
        && probe.invocation_count == 0u
        && cm_typeck_type_count(typeck) == type_count);
    root->type = saved_type;

    result = cm_semantic_body_check_definition(&session,
        fixture.present_body);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 0u);

    result = cm_semantic_body_check_definition_with_writeback(&session,
        fixture.present_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_OK
        && probe.invocation_count == 1u
        && probe.owned_term_count == 2u);

    assert(cm_typeck_new_variable(typeck, CM_HIR_INFER_GENERAL,
        test_span(1u, 2u), &variable) == CM_TYPECK_OK);
    type_count = cm_typeck_type_count(typeck);
    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.present_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_INVALID;
    probe.binding_variable = variable;
    probe.mutate_typeck = 1;
    result = cm_semantic_body_check_definition_with_writeback(&session,
        fixture.present_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_INVALID
        && probe.invocation_count == 1u
        && probe.added_term != CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(typeck) == type_count
        && cm_typeck_resolve(typeck, variable, &resolved) == CM_TYPECK_OK);
    resolved_type = cm_typeck_get_type(typeck, resolved);
    assert(resolved == variable && resolved_type != NULL
        && resolved_type->kind == CM_TYPECK_TYPE_VARIABLE);

    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.present_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_DEFERRED_INFERENCE;
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_definition_with_writeback(&session,
        fixture.present_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_DEFERRED_INFERENCE
        && probe.invocation_count == 1u
        && cm_typeck_type_count(typeck) == type_count);

    memset(&probe, 0, sizeof(probe));
    probe.hir = &fixture.hir;
    probe.expected_body = fixture.present_body;
    probe.result = CM_SEMANTIC_BODY_WRITEBACK_PENDING_PROJECTION;
    type_count = cm_typeck_type_count(typeck);
    result = cm_semantic_body_check_definition_with_writeback(&session,
        fixture.present_body, probe_writeback, &probe);
    assert(result.status == CM_SEMANTIC_BODY_PENDING_PROJECTION
        && probe.invocation_count == 1u
        && cm_typeck_type_count(typeck) == type_count);

    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

static void test_invalid_api_and_status_names(void)
{
    TestFixture fixture;
    CmSemanticSession session;
    CmSemanticSessionOptions options;
    CmSemanticBodyResult result;
    unsigned int status;

    fixture_init(&fixture);
    memset(&session, 0, sizeof(session));
    result = cm_semantic_body_check_calls(NULL, fixture.present_body,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    options = session_options(&fixture, fixture.present_caller);
    assert(cm_semantic_session_init(&session, &fixture.hir, &options)
        == CM_TRAIT_SOLVER_PROVEN);
    result = cm_semantic_body_check_calls(&session, CM_HIR_BODY_NONE,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    result = cm_semantic_body_check_calls(&session, fixture.present_body,
        &fixture.u32_type, 1u);
    assert(result.status == CM_SEMANTIC_BODY_INVALID);
    for (status = 0u; status <= (unsigned int)CM_SEMANTIC_BODY_INVALID;
         ++status) {
        assert(strcmp(cm_semantic_body_status_name(
            (CmSemanticBodyStatus)status), "unknown") != 0);
    }
    assert(strcmp(cm_semantic_body_status_name(CM_SEMANTIC_BODY_NEGATIVE),
        "negative") == 0);
    cm_semantic_session_destroy(&session);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_tuple_parameter_definition_lockstep();
    test_newtype_parameter_definition_lockstep();
    test_newtype_parameter_enclosing_impl_substitution();
    test_closed_trait_default_definition_mode();
    test_explicit_qualified_callable_selection();
    test_generic_trait_argument_callable();
    test_generic_impl_method_instance_spec();
    test_generic_impl_method_instance_parts();
    test_dot_method_callable_selection();
    test_dot_method_ambiguity_is_order_independent();
    test_dot_method_negative_and_no_solution();
    test_dot_method_unsupported_shape_has_no_writeback();
    test_positive_impl_and_missing_metadata();
    test_intrinsic_body_projection_normalization();
    test_projected_call_substitution_writeback();
    test_projection_equality_proof_mismatch_and_rollback();
    test_projection_fail_closed_shapes_and_overlap();
    test_projection_equality_order();
    test_callee_environment_cannot_self_prove();
    test_concrete_instance_writeback();
    test_pending_shapes_and_atomicity();
    test_two_parameter_nested_substitution_and_rollback();
    test_whole_body_constraints_and_rollback();
    test_malformed_foreign_and_stale();
    test_definition_writeback_contract_and_rollback();
    test_invalid_api_and_status_names();
    puts("hir semantic body tests passed");
    return 0;
}
