#include "cm/hir/body.h"

#include "cm/alloc.h"
#include "cm/hir/typeck.h"

#include <string.h>

static CmSpan cm_hir_body_ast_span(CmSourceId source, CmAstSpan ast_span)
{
    CmSpan span;

    span.source = source;
    span.start = ast_span.start;
    span.end = ast_span.end;
    return span;
}

static CmHirBodyLowerResult cm_hir_body_result(CmHirBodyLowerStatus status,
    CmHirBodyId body, CmSpan span)
{
    CmHirBodyLowerResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.body = body;
    result.span = span;
    return result;
}

static CmHirBodyLowerStatus cm_hir_parse_integer_literal(const CmAst *ast,
    CmInternId text_id, CmHirIntType expected_kind, uint64_t *out_value)
{
    const CmInternedString *text;
    const char *suffix;
    size_t suffix_length;
    uint64_t maximum;
    size_t digit_count;
    size_t index;
    uint64_t value;

    suffix = NULL;
    maximum = 0u;
    if (expected_kind == CM_HIR_INT_I32) {
        suffix = "i32";
        maximum = UINT64_C(2147483647);
    } else if (expected_kind == CM_HIR_INT_U32) {
        suffix = "u32";
        maximum = UINT64_C(4294967295);
    } else if (expected_kind == CM_HIR_INT_USIZE) {
        suffix = "usize";
        maximum = UINT64_MAX;
    }
    text = cm_ast_get_string(ast, text_id);
    if (suffix == NULL || text == NULL || text->len == 0u) {
        return CM_HIR_BODY_LOWER_INVALID_LITERAL;
    }
    digit_count = 0u;
    while (digit_count < text->len
        && text->bytes[digit_count] >= (unsigned char)'0'
        && text->bytes[digit_count] <= (unsigned char)'9') {
        ++digit_count;
    }
    /* The caller's exact expected kind authenticates the no-suffix case. */
    suffix_length = strlen(suffix);
    if (digit_count == 0u
        || (digit_count != text->len
            && (text->len - digit_count != suffix_length
                || memcmp(text->bytes + digit_count, suffix,
                    suffix_length) != 0))) {
        return CM_HIR_BODY_LOWER_INVALID_LITERAL;
    }
    value = 0u;
    for (index = 0u; index < digit_count; ++index) {
        uint64_t digit;

        if (text->bytes[index] < (unsigned char)'0'
            || text->bytes[index] > (unsigned char)'9') {
            return CM_HIR_BODY_LOWER_INVALID_LITERAL;
        }
        digit = (uint64_t)(text->bytes[index] - (unsigned char)'0');
        if (value > (maximum - digit) / 10u) {
            return CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE;
        }
        value = value * 10u + digit;
    }
    *out_value = value;
    return CM_HIR_BODY_LOWER_OK;
}

static int cm_hir_body_ast_hir_text_equal(const CmAst *ast,
    CmInternId ast_text, const CmHirContext *hir, CmInternId hir_text)
{
    const CmInternedString *left;
    const CmInternedString *right;

    left = cm_ast_get_string(ast, ast_text);
    right = cm_interner_get(&hir->strings, hir_text);
    return left != NULL && right != NULL && left->len == right->len
        && memcmp(left->bytes, right->bytes, left->len) == 0;
}

static int cm_hir_body_ast_text_is(const CmAst *ast, CmInternId text,
    const char *expected)
{
    const CmInternedString *value;
    size_t expected_length;

    value = cm_ast_get_string(ast, text);
    expected_length = strlen(expected);
    return value != NULL && value->len == expected_length
        && memcmp(value->bytes, expected, expected_length) == 0;
}

static int cm_hir_body_hir_text_is(const CmHirContext *hir,
    CmInternId text, const char *expected)
{
    const CmInternedString *value;
    size_t expected_length;

    value = cm_interner_get(&hir->strings, text);
    expected_length = strlen(expected);
    return value != NULL && value->len == expected_length
        && memcmp(value->bytes, expected, expected_length) == 0;
}

typedef struct CmHirBodyLetPlan {
    CmInternId ast_name;
    CmAstExprId initializer;
    CmAstSpan statement_span;
    CmAstSpan binding_span;
    CmTypeckTypeId inference_term;
    CmHirTypeId resolved_type;
    CmHirIntType resolved_integer_kind;
    int has_resolved_integer_kind;
} CmHirBodyLetPlan;

static const CmAstPathSegment *cm_hir_body_exact_path_segment(
    const CmAst *ast, CmAstPathId path_id)
{
    const CmAstPath *path;

    path = cm_ast_get_path(ast, path_id);
    if (path == NULL || path->absolute || path->segment_count != 1u
        || path->segments == NULL) {
        return NULL;
    }
    return &path->segments[0];
}

static int cm_hir_body_find_local(const CmHirContext *context,
    const CmHirBody *body, const CmAst *ast,
    const CmHirBodyLetPlan *let_plans, uint32_t visible_let_count,
    uint32_t base_local_count, CmInternId name, uint32_t *out_index)
{
    uint32_t index;
    uint32_t found;

    found = UINT32_MAX;
    for (index = 0u; index < visible_let_count; ++index) {
        if (let_plans[index].ast_name != name) continue;
        if (found != UINT32_MAX) return 0;
        found = base_local_count + index;
    }
    for (index = 0u; index < base_local_count; ++index) {
        if (!cm_hir_body_ast_hir_text_equal(ast, name, context,
                body->locals[index].name)) {
            continue;
        }
        if (found != UINT32_MAX) return 0;
        found = index;
    }
    if (found == UINT32_MAX) return 0;
    *out_index = found;
    return 1;
}

static const CmHirItem *cm_hir_body_find_free_function(
    const CmHirContext *context, CmHirModuleId module, const CmAst *ast,
    CmInternId name)
{
    const CmHirItem *found;
    size_t index;

    found = NULL;
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
            || item->owner_module != module
            || !cm_hir_def_id_is_none(item->parent_definition)
            || !cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            || !cm_hir_body_ast_hir_text_equal(ast, name, context,
                item->name)) {
            continue;
        }
        if (found != NULL) return NULL;
        found = item;
    }
    return found;
}

static int cm_hir_body_is_u32(const CmHirContext *context,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(context, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32;
}

static int cm_hir_body_is_usize(const CmHirContext *context,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(context, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE;
}

static int cm_hir_body_is_wrapping_unsigned(const CmHirContext *context,
    CmHirTypeId type_id)
{
    return cm_hir_body_is_u32(context, type_id)
        || cm_hir_body_is_usize(context, type_id);
}

static int cm_hir_body_is_bool(const CmHirContext *context,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(context, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND;
}

static const CmHirGenericParam *cm_hir_body_owned_type_parameter(
    const CmHirContext *context, const CmHirItem *owner_item,
    CmHirTypeId type_id)
{
    const CmHirType *type;
    const CmHirGenericParam *parameter;

    type = cm_hir_get_type(context, type_id);
    parameter = type == NULL || type->kind != CM_HIR_TYPE_PARAMETER_KIND
        ? NULL : cm_hir_get_generic_param(context,
            type->data.parameter_type.parameter);
    return owner_item != NULL && parameter != NULL
        && parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(parameter->owner, owner_item->definition)
        && parameter->index < owner_item->generic_parameter_count
        ? parameter : NULL;
}

static CmHirTypeId cm_hir_body_find_bool_type(
    const CmHirContext *context)
{
    size_t index;

    for (index = 0u; index < context->types.len; ++index) {
        const CmHirType *type;

        type = (const CmHirType *)cm_vec_at_const(&context->types, index);
        if (type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND) {
            return (CmHirTypeId)(index + 1u);
        }
    }
    return CM_HIR_TYPE_NONE;
}

static CmHirTypeId cm_hir_body_find_integer_type(
    const CmHirContext *context, CmHirIntType kind)
{
    size_t index;

    for (index = 0u; index < context->types.len; ++index) {
        const CmHirType *type;

        type = (const CmHirType *)cm_vec_at_const(&context->types, index);
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == kind) {
            return (CmHirTypeId)(index + 1u);
        }
    }
    return CM_HIR_TYPE_NONE;
}

static CmHirStatus cm_hir_body_add_integer_type(CmHirContext *context,
    CmHirIntType kind, CmSpan span, CmHirTypeId *out_type)
{
    CmHirType type;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = span;
    type.data.integer_type.kind = kind;
    return cm_hir_add_type(context, &type, out_type);
}

static int cm_hir_body_type_equal(const CmHirContext *context,
    CmHirTypeId left_id, CmHirTypeId right_id)
{
    const CmHirType *left;
    const CmHirType *right;

    if (left_id == right_id) return 1;
    left = cm_hir_get_type(context, left_id);
    right = cm_hir_get_type(context, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    if (left->kind == CM_HIR_TYPE_BOOL_KIND) return 1;
    if (left->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return left->data.integer_type.kind == right->data.integer_type.kind;
    }
    if (left->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        return left->data.parameter_type.parameter
            == right->data.parameter_type.parameter;
    }
    if (left->kind == CM_HIR_TYPE_ADT_KIND) {
        return left->data.named_type.argument_count == 0u
            && left->data.named_type.arguments == NULL
            && right->data.named_type.argument_count == 0u
            && right->data.named_type.arguments == NULL
            && cm_hir_def_id_equal(left->data.named_type.definition,
                right->data.named_type.definition);
    }
    return 0;
}

static int cm_hir_body_ast_type_is_integer(const CmAst *ast,
    CmAstTypeId type_id, CmHirIntType expected_kind)
{
    const CmAstType *type;
    const CmAstPathSegment *segment;
    const char *expected;

    type = cm_ast_get_type(ast, type_id);
    segment = type == NULL || type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_hir_body_exact_path_segment(ast, type->path);
    expected = expected_kind == CM_HIR_INT_U32 ? "u32"
        : (expected_kind == CM_HIR_INT_USIZE ? "usize" : NULL);
    return segment != NULL && segment->argument_count == 0u
        && expected != NULL
        && cm_hir_body_ast_text_is(ast, segment->name, expected);
}

static int cm_hir_body_ast_type_matches(const CmHirContext *context,
    const CmAst *ast, CmAstTypeId ast_type_id, CmHirTypeId hir_type_id)
{
    const CmHirType *type;
    const CmAstType *ast_type;
    const CmAstPathSegment *segment;

    type = cm_hir_get_type(context, hir_type_id);
    ast_type = cm_ast_get_type(ast, ast_type_id);
    segment = ast_type == NULL || ast_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_hir_body_exact_path_segment(ast, ast_type->path);
    if (type == NULL || segment == NULL || segment->argument_count != 0u) {
        return 0;
    }
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return cm_hir_body_ast_type_is_integer(ast, ast_type_id,
            type->data.integer_type.kind);
    }
    return type->kind == CM_HIR_TYPE_BOOL_KIND
        && cm_hir_body_ast_text_is(ast, segment->name, "bool");
}

static void cm_hir_body_rollback_expressions(CmHirContext *context,
    size_t expression_count)
{
    size_t index;

    for (index = expression_count; index < context->expressions.len;
         ++index) {
        CmHirExpr *expression;

        expression = (CmHirExpr *)cm_vec_at(&context->expressions, index);
        cm_hir_release_expr_owned_storage(expression);
    }
    cm_vec_resize(&context->expressions, expression_count);
}

CmHirStatus cm_hir_body_add_local_expression(CmHirContext *context,
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
    return cm_hir_add_expr(context, &expression, out_expression);
}

CmHirStatus cm_hir_body_add_call_expression(CmHirContext *context,
    CmHirBodyId body, CmHirDefId callee,
    const CmHirTypeId *type_substitutions,
    uint32_t type_substitution_count, const CmHirExprId *arguments,
    uint32_t argument_count, CmHirTypeId result_type, CmSpan span,
    CmHirExprId *out_expression)
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
    expression.data.call.type_substitution_count = type_substitution_count;
    expression.data.call.arguments = (CmHirExprId *)arguments;
    expression.data.call.argument_count = argument_count;
    return cm_hir_add_expr(context, &expression, out_expression);
}

CmHirStatus cm_hir_body_add_binary_expression(CmHirContext *context,
    CmHirBodyId body, CmHirBinaryOperator operator_kind,
    CmHirExprId left, CmHirExprId right, CmHirTypeId result_type,
    CmSpan span, CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = body;
    expression.type = result_type;
    expression.span = span;
    expression.data.binary.operator_kind = operator_kind;
    expression.data.binary.left = left;
    expression.data.binary.right = right;
    return cm_hir_add_expr(context, &expression, out_expression);
}

CmHirStatus cm_hir_body_add_if_expression(CmHirContext *context,
    CmHirBodyId body, CmHirExprId condition, CmHirExprId then_expression,
    CmHirExprId else_expression, CmHirTypeId result_type, CmSpan span,
    CmHirExprId *out_expression)
{
    CmHirExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_IF;
    expression.owner_body = body;
    expression.type = result_type;
    expression.span = span;
    expression.data.if_expr.condition = condition;
    expression.data.if_expr.then_expression = then_expression;
    expression.data.if_expr.else_expression = else_expression;
    return cm_hir_add_expr(context, &expression, out_expression);
}

typedef struct CmHirBodyCallPlan {
    CmHirDefId callee;
    CmAstExprId arguments[2];
    CmHirTypeId argument_types[2];
    uint32_t argument_count;
    uint32_t substitution_count;
} CmHirBodyCallPlan;

typedef struct CmHirBodyQualifiedCallPlan {
    CmHirTypeId requested_self_type;
    CmHirDefId requested_trait;
    CmHirDefId declared_trait_callable;
    CmAstExprId arguments[2];
    CmHirTypeId argument_types[2];
    uint32_t argument_count;
    uint32_t receiver_argument;
} CmHirBodyQualifiedCallPlan;

typedef struct CmHirBodyExpressionCounts {
    size_t expression_count;
    size_t call_word_count;
    size_t method_argument_count;
    size_t method_trait_count;
    size_t method_call_count;
    size_t aggregate_field_count;
    int needs_bool_type;
} CmHirBodyExpressionCounts;

typedef struct CmHirBodyBuildState {
    CmHirContext *context;
    CmHirBodyId body_id;
    const CmHirBody *body;
    const CmHirItem *owner_item;
    const CmAst *ast;
    const CmModuleGraph *graph;
    const CmImportResolver *imports;
    const CmHirModuleMap *modules;
    CmModuleGraphRevision revision;
    CmModuleId graph_module;
    CmSourceId source;
    CmHirTypeId bool_type;
    /* The sole IF admitted by this slice: the outer function block tail. */
    CmAstExprId allowed_if_expression;
    const CmHirBodyLetPlan *let_plans;
    uint32_t visible_let_count;
    uint32_t base_local_count;
    uint32_t *call_storage;
    size_t call_storage_words;
    size_t call_storage_cursor;
    CmHirExprId *method_argument_storage;
    size_t method_argument_storage_count;
    size_t method_argument_storage_cursor;
    CmHirDefId *method_trait_storage;
    size_t method_trait_storage_count;
    size_t method_trait_storage_cursor;
    CmHirAggregateFieldValue *aggregate_storage;
    size_t aggregate_storage_count;
    size_t aggregate_storage_cursor;
    void *transaction_storage;
    int transaction_storage_adopted;
    int trait_default_closed_slice;
    CmResolvePathSegmentView *path_segments;
    size_t path_segment_capacity;
} CmHirBodyBuildState;

static int cm_hir_body_trait_default_expression_supported(
    const CmAst *ast, CmAstExprId expression_id, size_t depth)
{
    const CmAstExpr *expression;

    expression = cm_ast_get_expr(ast, expression_id);
    if (ast == NULL || expression == NULL || depth >= ast->expressions.len
        || expression->attribute_count != 0u
        || expression->attributes != NULL) {
        return 0;
    }
    switch (expression->kind) {
    case CM_AST_EXPR_LITERAL:
        return 1;
    case CM_AST_EXPR_PATH:
        return cm_hir_body_exact_path_segment(ast,
            expression->data.path.path) != NULL;
    case CM_AST_EXPR_BINARY:
        return expression->data.binary.left < expression_id
            && expression->data.binary.right < expression_id
            && cm_hir_body_trait_default_expression_supported(ast,
                expression->data.binary.left, depth + 1u)
            && cm_hir_body_trait_default_expression_supported(ast,
                expression->data.binary.right, depth + 1u);
    case CM_AST_EXPR_BLOCK:
        return !expression->data.block.is_unsafe
            && !expression->data.block.is_const
            && expression->data.block.inner_attribute_count == 0u
            && expression->data.block.inner_attributes == NULL
            && expression->data.block.statement_count == 0u
            && expression->data.block.statements == NULL
            && expression->data.block.tail != CM_AST_EXPR_NONE
            && expression->data.block.tail < expression_id
            && cm_hir_body_trait_default_expression_supported(ast,
                expression->data.block.tail, depth + 1u);
    case CM_AST_EXPR_IF:
        return expression->data.if_expr.pattern == CM_AST_PATTERN_NONE
            && expression->data.if_expr.condition < expression_id
            && expression->data.if_expr.then_expr < expression_id
            && expression->data.if_expr.else_expr != CM_AST_EXPR_NONE
            && expression->data.if_expr.else_expr < expression_id
            && cm_hir_body_trait_default_expression_supported(ast,
                expression->data.if_expr.condition, depth + 1u)
            && cm_hir_body_trait_default_expression_supported(ast,
                expression->data.if_expr.then_expr, depth + 1u)
            && cm_hir_body_trait_default_expression_supported(ast,
                expression->data.if_expr.else_expr, depth + 1u);
    default:
        return 0;
    }
}

static const CmHirItem *cm_hir_body_bound_item(
    const CmHirContext *context, CmHirDefId definition_id)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(context, definition_id);
    item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(context, definition->entity.item_id);
    return item != NULL
            && cm_hir_def_id_equal(item->definition, definition_id)
        ? item : NULL;
}

static int cm_hir_body_method_scope_contains(const CmHirDefId *traits,
    size_t count, CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (cm_hir_def_id_equal(traits[index], definition)) return 1;
    }
    return 0;
}

static CmHirBodyLowerStatus cm_hir_body_method_scope_add(
    const CmHirContext *context, CmHirDefId definition,
    CmHirDefId *traits, size_t capacity, size_t *count)
{
    const CmHirItem *item;

    item = cm_hir_body_bound_item(context, definition);
    if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if (cm_hir_body_method_scope_contains(traits, *count, definition)) {
        return CM_HIR_BODY_LOWER_OK;
    }
    if (traits != NULL) {
        if (*count >= capacity) return CM_HIR_BODY_LOWER_HIR_FAILURE;
        traits[*count] = definition;
    }
    ++*count;
    return CM_HIR_BODY_LOWER_OK;
}

static int cm_hir_body_method_named_import_is_effective(
    const CmHirBodyBuildState *state, const CmHirImportBinding *binding)
{
    const CmInternedString *name;
    const CmHirItem *target;
    const CmAst *target_ast;
    const CmAstItem *source_trait;
    CmResolvePathSegmentView segment;
    CmResolvedBinding resolved;
    CmHirModuleId hir_module;
    CmImportLookupStatus lookup;

    name = cm_interner_get(&state->context->strings, binding->name);
    target = cm_hir_body_bound_item(state->context, binding->target);
    if (name == NULL || name->len == 0u || target == NULL
        || target->kind != CM_HIR_ITEM_TRAIT) return 0;
    segment.bytes = name->bytes;
    segment.length = name->len;
    lookup = cm_import_resolve_path_checked(state->imports, state->graph,
        state->revision, state->graph_module, 0, &segment, 1u,
        CM_RESOLVE_NAMESPACE_TYPE, &resolved);
    if (lookup != CM_IMPORT_LOOKUP_OK || resolved.is_ambiguous
        || resolved.is_anonymous || resolved.item_kind != CM_AST_ITEM_TRAIT
        || resolved.declaration.source == 0u
        || resolved.declaration.item == CM_AST_ITEM_NONE
        || cm_hir_module_map_lookup_hir(state->modules, state->graph,
            state->revision, resolved.module, state->context,
            &hir_module) != CM_HIR_MODULE_MAP_OK
        || hir_module != target->owner_module
        || !cm_module_graph_borrow_item_ast(state->graph, resolved.module,
            resolved.declaration, &target_ast)) {
        return 0;
    }
    source_trait = cm_ast_get_item(target_ast, resolved.declaration.item);
    return source_trait != NULL && source_trait->kind == CM_AST_ITEM_TRAIT
        && target->span.source == resolved.declaration.source
        && target->span.start == source_trait->span.start
        && target->span.end == source_trait->span.end
        && cm_hir_body_ast_hir_text_equal(target_ast, source_trait->name,
            state->context, target->name);
}

static CmHirBodyLowerStatus cm_hir_body_method_trait_scope(
    const CmHirBodyBuildState *state, CmHirDefId *traits,
    size_t capacity, size_t *out_count)
{
    const CmHirModule *module;
    const CmHirItem *parent;
    size_t count;
    size_t index;

    *out_count = 0u;
    module = cm_hir_get_module(state->context,
        state->owner_item->owner_module);
    if (module == NULL) return CM_HIR_BODY_LOWER_INVALID_BODY;
    if (traits == NULL) {
        CmHirDefId *scratch;
        size_t allocation_bytes;
        size_t maximum;
        size_t import_index;
        CmHirBodyLowerStatus status;

        maximum = state->context->items.len + 1u;
        for (import_index = 0u; import_index < module->import_count;
             ++import_index) {
            if (!cm_size_add(maximum,
                    (size_t)module->imports[import_index].binding_count,
                    &maximum)) return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
        if (!cm_size_mul(maximum, sizeof(CmHirDefId),
                &allocation_bytes)) {
            return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
        scratch = allocation_bytes == 0u ? NULL
            : (CmHirDefId *)cm_alloc(allocation_bytes);
        status = cm_hir_body_method_trait_scope(state, scratch, maximum,
            out_count);
        cm_free(scratch);
        return status;
    }
    count = 0u;
    for (index = 0u; index < state->context->items.len; ++index) {
        const CmHirItem *item;
        CmHirBodyLowerStatus status;

        item = (const CmHirItem *)cm_vec_at_const(&state->context->items,
            index);
        if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT
            || item->owner_module != state->owner_item->owner_module) {
            continue;
        }
        status = cm_hir_body_method_scope_add(state->context,
            item->definition, traits, capacity, &count);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
    }
    for (index = 0u; index < module->import_count; ++index) {
        const CmHirImport *import_value;
        uint32_t binding_index;

        import_value = &module->imports[index];
        for (binding_index = 0u;
             binding_index < import_value->binding_count; ++binding_index) {
            const CmHirImportBinding *binding;
            const CmHirItem *target;
            CmHirBodyLowerStatus status;

            binding = &import_value->bindings[binding_index];
            if (binding->namespace_kind != CM_HIR_NAMESPACE_TYPE
                || cm_hir_def_id_is_none(binding->target)) continue;
            target = cm_hir_body_bound_item(state->context,
                binding->target);
            if (target == NULL || target->kind != CM_HIR_ITEM_TRAIT) {
                continue;
            }
            if (!binding->is_anonymous
                && !cm_hir_body_method_named_import_is_effective(state,
                    binding)) continue;
            status = cm_hir_body_method_scope_add(state->context,
                binding->target, traits, capacity, &count);
            if (status != CM_HIR_BODY_LOWER_OK) return status;
        }
    }
    parent = cm_hir_def_id_is_none(state->owner_item->parent_definition)
        ? NULL : cm_hir_body_bound_item(state->context,
            state->owner_item->parent_definition);
    if (parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT) {
        CmHirBodyLowerStatus status;

        status = cm_hir_body_method_scope_add(state->context,
            parent->definition, traits, capacity, &count);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
    } else if (parent != NULL && parent->kind == CM_HIR_ITEM_IMPL
        && parent->data.impl_item.has_trait) {
        CmHirBodyLowerStatus status;

        status = cm_hir_body_method_scope_add(state->context,
            parent->data.impl_item.trait_type.definition, traits, capacity,
            &count);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
    }
    *out_count = count;
    return CM_HIR_BODY_LOWER_OK;
}

static int cm_hir_body_module_within(const CmHirContext *context,
    CmHirModuleId module_id, CmHirModuleId ancestor_id)
{
    size_t depth;

    for (depth = 0u; depth < context->modules.len; ++depth) {
        const CmHirModule *module;

        if (module_id == ancestor_id) return 1;
        module = cm_hir_get_module(context, module_id);
        if (module == NULL || module->parent == CM_HIR_MODULE_NONE) {
            return 0;
        }
        module_id = module->parent;
    }
    return 0;
}

static CmHirModuleId cm_hir_body_definition_module(
    const CmHirContext *context, CmHirDefId definition_id)
{
    const CmHirDefinition *definition;

    definition = cm_hir_lookup_definition(context, definition_id);
    return definition != NULL
            && definition->kind == CM_HIR_DEFINITION_MODULE
            && definition->state == CM_HIR_DEFINITION_BOUND
        ? definition->entity.module_id : CM_HIR_MODULE_NONE;
}

static int cm_hir_body_visibility_allows(const CmHirContext *context,
    CmHirModuleId use_module, CmHirModuleId owner_module,
    CmHirVisibility visibility)
{
    CmHirModuleId restriction;

    switch (visibility.kind) {
    case CM_HIR_VIS_PUBLIC:
    case CM_HIR_VIS_CRATE:
        return 1;
    case CM_HIR_VIS_PRIVATE:
        return cm_hir_body_module_within(context, use_module,
            owner_module);
    case CM_HIR_VIS_RESTRICTED:
        restriction = cm_hir_body_definition_module(context,
            visibility.restriction);
        return restriction != CM_HIR_MODULE_NONE
            && cm_hir_body_module_within(context, use_module,
                restriction);
    }
    return 0;
}

static CmHirBodyLowerStatus cm_hir_body_resolve_struct_expression(
    const CmHirBodyBuildState *state, const CmAstExpr *expression,
    CmHirTypeId expected_type_id, const CmHirItem **out_item)
{
    const CmHirType *expected_type;
    const CmHirItem *item;
    const CmAstPath *path;
    const CmHirDefinition *body_owner;
    CmResolvedBinding binding;
    CmImportLookupStatus lookup;
    CmModuleId target_graph_module;
    CmResolveModuleInfo target_module;
    const CmAst *target_ast;
    const CmAstItem *source_item;
    size_t index;

    if (out_item != NULL) *out_item = NULL;
    if (state == NULL || expression == NULL || out_item == NULL
        || expression->kind != CM_AST_EXPR_STRUCT
        || expression->data.struct_expr.base != CM_AST_EXPR_NONE) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    expected_type = cm_hir_get_type(state->context, expected_type_id);
    item = expected_type == NULL
            || expected_type->kind != CM_HIR_TYPE_ADT_KIND
            || expected_type->data.named_type.argument_count != 0u
            || expected_type->data.named_type.arguments != NULL
        ? NULL : cm_hir_body_bound_item(state->context,
            expected_type->data.named_type.definition);
    body_owner = cm_hir_lookup_definition(state->context,
        state->body->owner);
    if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->generic_parameter_count != 0u
        || item->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
        || (item->data.aggregate_item.field_count == 0u)
            != (item->data.aggregate_item.fields == NULL)
        || body_owner == NULL
        || expected_type->data.named_type.definition.crate_id
            != body_owner->id.crate_id
        || !cm_hir_body_visibility_allows(state->context,
            state->owner_item->owner_module, item->owner_module,
            item->visibility)) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
    }
    path = cm_ast_get_path(state->ast,
        expression->data.struct_expr.path);
    if (path == NULL || path->segment_count == 0u || path->segments == NULL
        || path->span.start != expression->span.start
        || path->span.start >= path->span.end
        || path->span.end >= expression->span.end
        || path->segment_count > state->path_segment_capacity) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    for (index = 0u; index < path->segment_count; ++index) {
        const CmAstPathSegment *segment;
        const CmInternedString *name;

        segment = &path->segments[index];
        if (segment->argument_count != 0u || segment->arguments != NULL) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
        }
        name = cm_ast_get_string(state->ast, segment->name);
        if (name == NULL || name->len == 0u) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        state->path_segments[index].bytes = name->bytes;
        state->path_segments[index].length = name->len;
    }
    lookup = cm_import_resolve_path_checked(state->imports, state->graph,
        state->revision, state->graph_module, path->absolute,
        state->path_segments, path->segment_count,
        CM_RESOLVE_NAMESPACE_TYPE, &binding);
    if (lookup == CM_IMPORT_LOOKUP_NOT_FOUND
        || lookup == CM_IMPORT_LOOKUP_AMBIGUOUS
        || lookup == CM_IMPORT_LOOKUP_CYCLE) {
        return CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
    }
    if (lookup == CM_IMPORT_LOOKUP_STALE_REVISION
        || lookup == CM_IMPORT_LOOKUP_FAILED_BUILD) {
        return CM_HIR_BODY_LOWER_SOURCE_MISMATCH;
    }
    if (lookup != CM_IMPORT_LOOKUP_OK
        || binding.item_kind != CM_AST_ITEM_STRUCT
        || binding.is_ambiguous || binding.is_anonymous
        || binding.declaration.source == 0u
        || binding.declaration.item == CM_AST_ITEM_NONE) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if (cm_hir_module_map_lookup_module(state->modules, state->graph,
            state->revision, state->context, item->owner_module,
            &target_graph_module) != CM_HIR_MODULE_MAP_OK
        || !cm_module_graph_get_module(state->graph, target_graph_module,
            &target_module)
        || !cm_module_graph_borrow_ast(state->graph, target_graph_module,
            &target_ast)) {
        return CM_HIR_BODY_LOWER_SOURCE_MISMATCH;
    }
    if (target_module.source != binding.declaration.source) {
        return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
    }
    source_item = cm_ast_get_item(target_ast, binding.declaration.item);
    if (source_item == NULL || source_item->kind != CM_AST_ITEM_STRUCT
        || source_item->data.aggregate_item.form != CM_AST_FIELDS_NAMED
        || source_item->data.aggregate_item.field_count
            != item->data.aggregate_item.field_count
        || (source_item->data.aggregate_item.field_count == 0u)
            != (source_item->data.aggregate_item.fields == NULL)
        || item->span.source != target_module.source) {
        return CM_HIR_BODY_LOWER_SOURCE_MISMATCH;
    }
    if (item->span.start != source_item->span.start
        || item->span.end != source_item->span.end
        || !cm_hir_body_ast_hir_text_equal(target_ast, source_item->name,
            state->context, item->name)) {
        return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
    }
    *out_item = item;
    return CM_HIR_BODY_LOWER_OK;
}

static int cm_hir_body_find_struct_field(const CmHirContext *context,
    const CmAst *ast, const CmHirItem *item, CmInternId name,
    uint32_t *out_index)
{
    uint32_t index;
    uint32_t found;

    found = UINT32_MAX;
    for (index = 0u; index < item->data.aggregate_item.field_count;
         ++index) {
        if (!cm_hir_body_ast_hir_text_equal(ast, name, context,
                item->data.aggregate_item.fields[index].name)) {
            continue;
        }
        if (found != UINT32_MAX) return 0;
        found = index;
    }
    if (found == UINT32_MAX) return 0;
    *out_index = found;
    return 1;
}

static CmHirBodyLowerStatus cm_hir_body_infer_expression_type(
    const CmHirBodyBuildState *state, CmAstExprId expression_id,
    size_t depth, CmHirTypeId *out_type);

static CmHirBodyLowerStatus cm_hir_body_literal_integer_kind(
    const CmAst *ast, CmInternId text_id, int *out_has_suffix,
    CmHirIntType *out_kind);

static CmHirBodyLowerStatus cm_hir_body_resolve_field_expression(
    const CmHirBodyBuildState *state, const CmAstExpr *expression,
    size_t depth, CmHirTypeId *out_base_type, const CmHirItem **out_item,
    uint32_t *out_field_index)
{
    const CmAstExpr *base;
    const CmHirType *base_type;
    const CmHirItem *item;
    const CmInternedString *name;
    CmHirTypeId base_type_id;
    CmHirBodyLowerStatus status;
    uint32_t field_index;

    if (out_base_type != NULL) *out_base_type = CM_HIR_TYPE_NONE;
    if (out_item != NULL) *out_item = NULL;
    if (out_field_index != NULL) *out_field_index = 0u;
    if (state == NULL || expression == NULL || out_base_type == NULL
        || out_item == NULL || out_field_index == NULL
        || expression->kind != CM_AST_EXPR_FIELD
        || expression->data.field.base == CM_AST_EXPR_NONE
        || (size_t)expression->data.field.base
            > state->ast->expressions.len
        || depth >= state->ast->expressions.len) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    base = cm_ast_get_expr(state->ast, expression->data.field.base);
    name = cm_ast_get_string(state->ast, expression->data.field.name);
    if (base == NULL || name == NULL || name->len == 0u
        || base->span.start != expression->span.start
        || base->span.start > base->span.end
        || base->span.end >= expression->data.field.name_span.start
        || expression->data.field.name_span.start
            >= expression->data.field.name_span.end
        || expression->data.field.name_span.end != expression->span.end) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    status = cm_hir_body_infer_expression_type(state,
        expression->data.field.base, depth + 1u, &base_type_id);
    if (status != CM_HIR_BODY_LOWER_OK) return status;
    base_type = cm_hir_get_type(state->context, base_type_id);
    item = base_type == NULL || base_type->kind != CM_HIR_TYPE_ADT_KIND
            || base_type->data.named_type.argument_count != 0u
            || base_type->data.named_type.arguments != NULL
        ? NULL : cm_hir_body_bound_item(state->context,
            base_type->data.named_type.definition);
    if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT
        || item->generic_parameter_count != 0u
        || item->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
    }
    if (!cm_hir_body_find_struct_field(state->context, state->ast, item,
            expression->data.field.name, &field_index)) {
        return CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
    }
    if (!cm_hir_body_visibility_allows(state->context,
            state->owner_item->owner_module, item->owner_module,
            item->data.aggregate_item.fields[field_index].visibility)) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    *out_base_type = base_type_id;
    *out_item = item;
    *out_field_index = field_index;
    return CM_HIR_BODY_LOWER_OK;
}

static CmHirBodyLowerStatus cm_hir_body_infer_expression_type(
    const CmHirBodyBuildState *state, CmAstExprId expression_id,
    size_t depth, CmHirTypeId *out_type)
{
    const CmAstExpr *expression;

    if (out_type != NULL) *out_type = CM_HIR_TYPE_NONE;
    expression = state == NULL ? NULL : cm_ast_get_expr(state->ast,
        expression_id);
    if (state == NULL || out_type == NULL || expression == NULL
        || depth >= state->ast->expressions.len) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if (expression->kind == CM_AST_EXPR_PATH) {
        const CmAstPathSegment *segment;
        uint32_t local_index;

        segment = cm_hir_body_exact_path_segment(state->ast,
            expression->data.path.path);
        if (segment == NULL || segment->argument_count != 0u
            || !cm_hir_body_find_local(state->context, state->body,
                state->ast, state->let_plans, state->visible_let_count,
                state->base_local_count, segment->name, &local_index)) {
            return CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
        }
        if (local_index < state->base_local_count) {
            *out_type = state->body->locals[local_index].type;
        } else if (local_index < state->base_local_count
                + state->visible_let_count) {
            *out_type = state->body->expected_type;
        } else {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        return cm_hir_get_type(state->context, *out_type) == NULL
            ? CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE
            : CM_HIR_BODY_LOWER_OK;
    }
    if (expression->kind == CM_AST_EXPR_STRUCT) {
        const CmHirItem *found_item;
        CmHirTypeId found_type;
        CmHirBodyLowerStatus fallback;
        size_t index;

        found_item = NULL;
        found_type = CM_HIR_TYPE_NONE;
        fallback = CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
        for (index = 0u; index < state->context->types.len; ++index) {
            const CmHirType *candidate;
            const CmHirItem *candidate_item;
            CmHirBodyLowerStatus status;
            CmHirTypeId candidate_id;

            candidate = (const CmHirType *)cm_vec_at_const(
                &state->context->types, index);
            if (candidate == NULL || candidate->kind != CM_HIR_TYPE_ADT_KIND
                || candidate->data.named_type.argument_count != 0u
                || candidate->data.named_type.arguments != NULL
                || index >= (size_t)UINT32_MAX) {
                continue;
            }
            candidate_id = (CmHirTypeId)(index + 1u);
            status = cm_hir_body_resolve_struct_expression(state,
                expression, candidate_id, &candidate_item);
            if (status == CM_HIR_BODY_LOWER_OK) {
                if (found_item != NULL
                    && !cm_hir_def_id_equal(found_item->definition,
                        candidate_item->definition)) {
                    return CM_HIR_BODY_LOWER_INVALID_BODY;
                }
                if (found_item == NULL) {
                    found_item = candidate_item;
                    found_type = candidate_id;
                }
            } else if (status == CM_HIR_BODY_LOWER_SOURCE_MISMATCH
                || status == CM_HIR_BODY_LOWER_INVALID_BODY
                || status == CM_HIR_BODY_LOWER_UNRESOLVED_PATH) {
                fallback = status;
            }
        }
        if (found_item == NULL) return fallback;
        *out_type = found_type;
        return CM_HIR_BODY_LOWER_OK;
    }
    if (expression->kind == CM_AST_EXPR_FIELD) {
        const CmHirItem *item;
        CmHirTypeId base_type;
        uint32_t field_index;
        CmHirBodyLowerStatus status;

        status = cm_hir_body_resolve_field_expression(state, expression,
            depth, &base_type, &item, &field_index);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        *out_type = item->data.aggregate_item.fields[field_index].type;
        return CM_HIR_BODY_LOWER_OK;
    }
    if (expression->kind == CM_AST_EXPR_LITERAL) {
        CmHirIntType integer_kind;
        int has_suffix;
        CmHirBodyLowerStatus status;

        status = cm_hir_body_literal_integer_kind(state->ast,
            expression->data.literal.text, &has_suffix, &integer_kind);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        if (!has_suffix) return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        *out_type = cm_hir_body_find_integer_type(state->context,
            integer_kind);
        return *out_type == CM_HIR_TYPE_NONE
            ? CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE
            : CM_HIR_BODY_LOWER_OK;
    }
    return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
}

static int cm_hir_body_exact_identity_signature(
    const CmHirContext *context, const CmHirItem *item)
{
    const CmHirFunctionSignature *signature;
    const CmHirGenericParam *parameter;
    const CmHirType *parameter_type;
    const CmHirType *return_type;

    if (item->generic_parameter_count != 1u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) {
        return 0;
    }
    signature = &item->data.function_item.signature;
    if (signature->parameter_count != 1u || signature->parameters == NULL) {
        return 0;
    }
    parameter = cm_hir_get_generic_param(context,
        item->generic_parameter_start);
    parameter_type = cm_hir_get_type(context,
        signature->parameters[0].type);
    return_type = cm_hir_get_type(context, signature->return_type);
    return parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && cm_hir_def_id_equal(parameter->owner, item->definition)
        && parameter_type != NULL
        && parameter_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && parameter_type->data.parameter_type.parameter
            == item->generic_parameter_start
        && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && return_type->data.parameter_type.parameter
            == item->generic_parameter_start;
}

static int cm_hir_body_supported_monomorphic_argument_type(
    const CmHirContext *context, const CmHirItem *owner_item,
    CmHirTypeId type_id)
{
    const CmHirType *type;
    const CmHirItem *item;
    const CmHirModule *module;

    if (cm_hir_body_is_u32(context, type_id)) return 1;
    type = cm_hir_get_type(context, type_id);
    item = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
            || type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
        ? NULL : cm_hir_body_bound_item(context,
            type->data.named_type.definition);
    module = item == NULL ? NULL
        : cm_hir_get_module(context, item->owner_module);
    return owner_item != NULL && item != NULL
        && item->kind == CM_HIR_ITEM_STRUCT
        && cm_hir_def_id_is_none(item->parent_definition)
        && item->generic_parameter_count == 0u
        && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && (item->data.aggregate_item.field_count == 0u)
            == (item->data.aggregate_item.fields == NULL)
        && type->data.named_type.definition.crate_id
            == owner_item->definition.crate_id
        && module != NULL
        && module->crate_id == type->data.named_type.definition.crate_id;
}

static int cm_hir_body_qualified_path_valid(const CmAst *ast,
    const CmAstExpr *expression)
{
    const CmAstType *self_type;
    const CmAstPath *trait_path;
    const CmAstPath *associated_path;

    if (expression == NULL
        || expression->kind != CM_AST_EXPR_QUALIFIED_PATH) {
        return 0;
    }
    self_type = cm_ast_get_type(ast,
        expression->data.qualified_path.self_type);
    trait_path = expression->data.qualified_path.trait_path
            == CM_AST_PATH_NONE
        ? NULL : cm_ast_get_path(ast,
            expression->data.qualified_path.trait_path);
    associated_path = cm_ast_get_path(ast,
        expression->data.qualified_path.associated_path);
    return self_type != NULL && associated_path != NULL
        && (trait_path == NULL
            || (trait_path->segment_count != 0u
                && trait_path->segments != NULL))
        && associated_path->segment_count != 0u
        && associated_path->segments != NULL
        && expression->data.qualified_path.qualifier_span.start
            == expression->span.start
        && self_type->span.start
            > expression->data.qualified_path.qualifier_span.start
        && (trait_path == NULL
            ? self_type->span.end
                < expression->data.qualified_path.qualifier_span.end
            : self_type->span.end <= trait_path->span.start
                && trait_path->span.end
                    < expression->data.qualified_path.qualifier_span.end)
        && expression->data.qualified_path.qualifier_span.end
            < associated_path->span.start
        && associated_path->span.end == expression->span.end;
}

static CmHirBodyLowerStatus cm_hir_body_load_resolve_path(
    const CmHirBodyBuildState *state, const CmAstPath *path)
{
    size_t index;

    if (path == NULL || path->segment_count == 0u || path->segments == NULL
        || path->segment_count > state->path_segment_capacity) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    for (index = 0u; index < path->segment_count; ++index) {
        const CmAstPathSegment *segment;
        const CmInternedString *name;

        segment = &path->segments[index];
        name = cm_ast_get_string(state->ast, segment->name);
        if (segment->argument_count != 0u || segment->arguments != NULL
            || name == NULL || name->len == 0u) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
        }
        state->path_segments[index].bytes = name->bytes;
        state->path_segments[index].length = name->len;
    }
    return CM_HIR_BODY_LOWER_OK;
}

static CmHirTypeId cm_hir_body_resolve_qualified_self_type(
    const CmHirBodyBuildState *state, CmAstTypeId ast_type_id)
{
    const CmAstType *ast_type;
    const CmAstPathSegment *segment;
    size_t index;

    ast_type = cm_ast_get_type(state->ast, ast_type_id);
    segment = ast_type == NULL || ast_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_hir_body_exact_path_segment(state->ast,
            ast_type->path);
    if (segment == NULL || segment->argument_count != 0u) {
        return CM_HIR_TYPE_NONE;
    }
    for (index = 0u; index < state->context->types.len; ++index) {
        CmHirTypeId candidate;

        if (index >= (size_t)UINT32_MAX) return CM_HIR_TYPE_NONE;
        candidate = (CmHirTypeId)(index + 1u);
        if (cm_hir_body_ast_type_matches(state->context, state->ast,
                ast_type_id, candidate)) {
            return candidate;
        }
    }
    return CM_HIR_TYPE_NONE;
}

static CmHirTypeId cm_hir_body_qualified_declared_type(
    const CmHirContext *context, CmHirDefId trait_definition,
    CmHirTypeId requested_self, CmHirTypeId declared)
{
    const CmHirType *type;

    type = cm_hir_get_type(context, declared);
    if (type == NULL) return CM_HIR_TYPE_NONE;
    if (type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(type->data.self_type.owner,
            trait_definition)) {
        return requested_self;
    }
    return type->kind == CM_HIR_TYPE_BOOL_KIND
            || type->kind == CM_HIR_TYPE_INTEGER_KIND
            || (type->kind == CM_HIR_TYPE_ADT_KIND
                && type->data.named_type.argument_count == 0u
                && type->data.named_type.arguments == NULL)
        ? declared : CM_HIR_TYPE_NONE;
}

static CmHirBodyLowerStatus cm_hir_body_resolve_qualified_call(
    const CmHirBodyBuildState *state, const CmAstExpr *call,
    CmHirTypeId expected_type, CmHirBodyQualifiedCallPlan *out_plan)
{
    const CmAstExpr *callee;
    const CmAstPath *trait_path;
    const CmAstPath *associated_path;
    const CmAstPathSegment *associated;
    const CmHirItem *trait_item;
    const CmHirItem *declared;
    const CmHirFunctionSignature *signature;
    const CmAst *trait_ast;
    const CmAstItem *source_trait;
    CmResolvedBinding binding;
    CmImportLookupStatus lookup;
    CmHirModuleId hir_module;
    CmHirTypeId requested_self;
    CmHirTypeId declared_return;
    size_t index;

    memset(out_plan, 0, sizeof(*out_plan));
    out_plan->receiver_argument = CM_HIR_CALLABLE_RECEIVER_NONE;
    callee = cm_ast_get_expr(state->ast, call->data.call.callee);
    if (callee == NULL || !cm_hir_body_qualified_path_valid(state->ast,
            callee) || call->data.call.argument_count > 2u
        || (call->data.call.argument_count == 0u)
            != (call->data.call.arguments == NULL)) {
        return callee == NULL
            ? CM_HIR_BODY_LOWER_INVALID_BODY
            : CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    trait_path = cm_ast_get_path(state->ast,
        callee->data.qualified_path.trait_path);
    associated_path = cm_ast_get_path(state->ast,
        callee->data.qualified_path.associated_path);
    associated = associated_path == NULL
            || associated_path->absolute
            || associated_path->segment_count != 1u
            || associated_path->segments == NULL
        ? NULL : &associated_path->segments[0];
    if (trait_path == NULL || associated == NULL
        || associated->argument_count != 0u
        || associated->arguments != NULL) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    if (cm_hir_body_load_resolve_path(state, trait_path)
            != CM_HIR_BODY_LOWER_OK) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
    }
    lookup = cm_import_resolve_path_checked(state->imports, state->graph,
        state->revision, state->graph_module, trait_path->absolute,
        state->path_segments, trait_path->segment_count,
        CM_RESOLVE_NAMESPACE_TYPE, &binding);
    if (lookup == CM_IMPORT_LOOKUP_STALE_REVISION
        || lookup == CM_IMPORT_LOOKUP_FAILED_BUILD) {
        return CM_HIR_BODY_LOWER_SOURCE_MISMATCH;
    }
    if (lookup != CM_IMPORT_LOOKUP_OK || binding.is_ambiguous
        || binding.is_anonymous || binding.item_kind != CM_AST_ITEM_TRAIT
        || binding.declaration.source == 0u
        || binding.declaration.item == CM_AST_ITEM_NONE) {
        return CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
    }
    if (cm_hir_module_map_lookup_hir(state->modules, state->graph,
            state->revision, binding.module, state->context,
            &hir_module) != CM_HIR_MODULE_MAP_OK
        || !cm_module_graph_borrow_item_ast(state->graph, binding.module,
            binding.declaration, &trait_ast)) {
        return CM_HIR_BODY_LOWER_SOURCE_MISMATCH;
    }
    source_trait = cm_ast_get_item(trait_ast, binding.declaration.item);
    trait_item = NULL;
    for (index = 0u; index < state->context->items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(
            &state->context->items, index);
        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_TRAIT
            || candidate->owner_module != hir_module
            || candidate->span.source != binding.declaration.source
            || source_trait == NULL
            || candidate->span.start != source_trait->span.start
            || candidate->span.end != source_trait->span.end
            || !cm_hir_body_ast_hir_text_equal(trait_ast,
                source_trait->name, state->context, candidate->name)) {
            continue;
        }
        if (trait_item != NULL) return CM_HIR_BODY_LOWER_INVALID_BODY;
        trait_item = candidate;
    }
    requested_self = cm_hir_body_resolve_qualified_self_type(state,
        callee->data.qualified_path.self_type);
    if (trait_item == NULL || source_trait == NULL
        || source_trait->kind != CM_AST_ITEM_TRAIT
        || requested_self == CM_HIR_TYPE_NONE
        || trait_item->generic_parameter_count != 0u
        || trait_item->predicate_scope_count != 0u
        || trait_item->predicate_count != 0u
        || trait_item->outlives_predicate_count != 0u) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
    }
    declared = NULL;
    for (index = 0u; index < state->context->items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(
            &state->context->items, index);
        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_FUNCTION
            || !cm_hir_def_id_equal(candidate->parent_definition,
                trait_item->definition)
            || !cm_hir_body_ast_hir_text_equal(state->ast,
                associated->name, state->context, candidate->name)) {
            continue;
        }
        if (declared != NULL) return CM_HIR_BODY_LOWER_INVALID_BODY;
        declared = candidate;
    }
    if (declared == NULL || declared->generic_parameter_count != 0u
        || declared->data.function_item.body != CM_HIR_BODY_NONE
        || declared->predicate_scope_count != 0u
        || declared->predicate_count != 0u
        || declared->outlives_predicate_count != 0u
        || !cm_hir_def_id_is_none(
            declared->data.function_item.trait_item_definition)) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    signature = &declared->data.function_item.signature;
    declared_return = cm_hir_body_qualified_declared_type(state->context,
        trait_item->definition, requested_self, signature->return_type);
    if (signature->parameter_count != call->data.call.argument_count
        || (signature->parameter_count == 0u)
            != (signature->parameters == NULL)
        || declared_return == CM_HIR_TYPE_NONE
        || !cm_hir_body_type_equal(state->context, declared_return,
            expected_type)) {
        return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
    }
    for (index = 0u; index < signature->parameter_count; ++index) {
        out_plan->argument_types[index] =
            cm_hir_body_qualified_declared_type(state->context,
                trait_item->definition, requested_self,
                signature->parameters[index].type);
        if (out_plan->argument_types[index] == CM_HIR_TYPE_NONE) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
        }
        out_plan->arguments[index] = call->data.call.arguments[index];
    }
    out_plan->requested_self_type = requested_self;
    out_plan->requested_trait = trait_item->definition;
    out_plan->declared_trait_callable = declared->definition;
    out_plan->argument_count = call->data.call.argument_count;
    out_plan->receiver_argument = signature->receiver == CM_HIR_RECEIVER_NONE
        ? CM_HIR_CALLABLE_RECEIVER_NONE : 0u;
    return CM_HIR_BODY_LOWER_OK;
}

static CmHirBodyLowerStatus cm_hir_body_resolve_scalar_call(
    const CmHirContext *context, const CmHirItem *owner_item,
    const CmAst *ast,
    const CmAstExpr *call, CmHirTypeId expected_type,
    CmHirBodyCallPlan *out_plan)
{
    const CmAstExpr *callee_ast;
    const CmAstPathSegment *callee_segment;
    const CmHirItem *callee_item;
    const CmHirGenericParam *expected_parameter;
    uint32_t index;

    memset(out_plan, 0, sizeof(*out_plan));
    expected_parameter = cm_hir_body_owned_type_parameter(context,
        owner_item, expected_type);
    if (call->data.call.argument_count == 0u
        || call->data.call.argument_count > 2u
        || call->data.call.arguments == NULL
        || (!cm_hir_body_is_wrapping_unsigned(context, expected_type)
            && expected_parameter == NULL)) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    callee_ast = cm_ast_get_expr(ast, call->data.call.callee);
    if (callee_ast == NULL || callee_ast->span.start > callee_ast->span.end
        || callee_ast->span.start < call->span.start
        || callee_ast->span.end > call->span.end) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    callee_segment = callee_ast->kind != CM_AST_EXPR_PATH ? NULL
        : cm_hir_body_exact_path_segment(ast,
            callee_ast->data.path.path);
    if (callee_segment == NULL) {
        return CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
    }
    callee_item = cm_hir_body_find_free_function(context,
        owner_item->owner_module, ast, callee_segment->name);
    if (callee_item == NULL) {
        return CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
    }
    if (callee_item->generic_parameter_count == 0u) {
        const CmHirFunctionSignature *signature;

        if (callee_segment->argument_count != 0u) {
            return CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION;
        }
        signature = &callee_item->data.function_item.signature;
        if (signature->parameter_count != call->data.call.argument_count
            || signature->parameters == NULL
            || !cm_hir_body_type_equal(context, signature->return_type,
                expected_type)) {
            return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
        }
        for (index = 0u; index < signature->parameter_count; ++index) {
            if (cm_hir_body_is_usize(context, expected_type)
                ? !cm_hir_body_is_usize(context,
                    signature->parameters[index].type)
                : !cm_hir_body_supported_monomorphic_argument_type(context,
                    owner_item, signature->parameters[index].type)) {
                return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
            }
            if (!cm_hir_body_is_wrapping_unsigned(context,
                    signature->parameters[index].type)
                && !cm_hir_body_hir_text_is(context, signature->abi,
                    "Rust")) {
                return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
            }
            out_plan->argument_types[index] =
                signature->parameters[index].type;
        }
        out_plan->substitution_count = 0u;
    } else if (cm_hir_body_exact_identity_signature(context,
            callee_item)) {
        const CmAstType *substitution;
        const CmAstPathSegment *substitution_segment;
        int valid_substitution;

        substitution = callee_segment->argument_count == 1u
                && callee_segment->arguments != NULL
                && callee_segment->arguments[0].kind == CM_AST_GENERIC_TYPE
            ? cm_ast_get_type(ast, callee_segment->arguments[0].type)
            : NULL;
        substitution_segment = substitution == NULL
                || substitution->kind != CM_AST_TYPE_PATH
            ? NULL : cm_hir_body_exact_path_segment(ast,
                substitution->path);
        valid_substitution = substitution != NULL
            && (cm_hir_body_is_u32(context, expected_type)
                ? cm_hir_body_ast_type_is_integer(ast,
                    callee_segment->arguments[0].type, CM_HIR_INT_U32)
                : expected_parameter != NULL
                    && substitution_segment != NULL
                    && substitution_segment->argument_count == 0u
                    && cm_hir_body_ast_hir_text_equal(ast,
                        substitution_segment->name, context,
                        expected_parameter->name));
        if (call->data.call.argument_count != 1u
            || callee_segment->argument_count != 1u
            || callee_segment->arguments == NULL
            || callee_segment->arguments[0].kind != CM_AST_GENERIC_TYPE
            || !valid_substitution) {
            return CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION;
        }
        out_plan->argument_types[0] = expected_type;
        out_plan->substitution_count = 1u;
    } else {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    if (callee_item->data.function_item.body == CM_HIR_BODY_NONE) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    for (index = 0u; index < call->data.call.argument_count; ++index) {
        out_plan->arguments[index] = call->data.call.arguments[index];
    }
    out_plan->callee = callee_item->definition;
    out_plan->argument_count = call->data.call.argument_count;
    return CM_HIR_BODY_LOWER_OK;
}

static int cm_hir_body_counts_add(CmHirBodyExpressionCounts *total,
    const CmHirBodyExpressionCounts *part)
{
    int result;

    result = cm_size_add(total->expression_count, part->expression_count,
            &total->expression_count)
        && cm_size_add(total->call_word_count, part->call_word_count,
            &total->call_word_count)
        && cm_size_add(total->method_trait_count,
            part->method_trait_count, &total->method_trait_count)
        && cm_size_add(total->method_argument_count,
            part->method_argument_count, &total->method_argument_count)
        && cm_size_add(total->method_call_count, part->method_call_count,
            &total->method_call_count)
        && cm_size_add(total->aggregate_field_count,
            part->aggregate_field_count, &total->aggregate_field_count);
    if (result && part->needs_bool_type) total->needs_bool_type = 1;
    return result;
}

static CmHirBodyLowerStatus cm_hir_body_preflight_typed_expression(
    const CmHirBodyBuildState *state, CmHirTypeId expected_type,
    CmAstExprId expression_id, CmAstSpan parent_span, size_t depth,
    CmHirBodyExpressionCounts *out_counts);

static CmHirBodyLowerStatus cm_hir_body_literal_integer_kind(
    const CmAst *ast, CmInternId text_id, int *out_has_suffix,
    CmHirIntType *out_kind)
{
    const CmInternedString *text;
    size_t digit_count;
    size_t suffix_length;

    *out_has_suffix = 0;
    *out_kind = CM_HIR_INT_I32;
    text = cm_ast_get_string(ast, text_id);
    if (text == NULL || text->len == 0u) {
        return CM_HIR_BODY_LOWER_INVALID_LITERAL;
    }
    digit_count = 0u;
    while (digit_count < text->len
        && text->bytes[digit_count] >= (unsigned char)'0'
        && text->bytes[digit_count] <= (unsigned char)'9') {
        ++digit_count;
    }
    if (digit_count == 0u) return CM_HIR_BODY_LOWER_INVALID_LITERAL;
    if (digit_count == text->len) return CM_HIR_BODY_LOWER_OK;
    suffix_length = text->len - digit_count;
    *out_has_suffix = 1;
    if (suffix_length == 3u
        && memcmp(text->bytes + digit_count, "i32", 3u) == 0) {
        *out_kind = CM_HIR_INT_I32;
    } else if (suffix_length == 3u
        && memcmp(text->bytes + digit_count, "u32", 3u) == 0) {
        *out_kind = CM_HIR_INT_U32;
    } else if (suffix_length == 5u
        && memcmp(text->bytes + digit_count, "usize", 5u) == 0) {
        *out_kind = CM_HIR_INT_USIZE;
    } else {
        return CM_HIR_BODY_LOWER_INVALID_LITERAL;
    }
    return CM_HIR_BODY_LOWER_OK;
}

static CmHirBodyLowerStatus cm_hir_body_typeck_status(
    CmTypeckStatus status)
{
    if (status == CM_TYPECK_TYPE_MISMATCH
        || status == CM_TYPECK_KIND_CONFLICT) {
        return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
    }
    if (status == CM_TYPECK_UNRESOLVED
        || status == CM_TYPECK_UNSUPPORTED_HIR_TYPE
        || status == CM_TYPECK_UNSUPPORTED_CONSTANT) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
    }
    return CM_HIR_BODY_LOWER_HIR_FAILURE;
}

static CmHirBodyLowerStatus cm_hir_body_inference_path_term(
    const CmHirBodyBuildState *state, CmTypeckContext *typeck,
    CmAstExprId expression_id, CmTypeckTypeId *out_term)
{
    const CmAstExpr *expression;
    const CmAstPathSegment *segment;
    uint32_t local_index;
    CmHirTypeId local_type;
    CmTypeckStatus status;

    *out_term = CM_TYPECK_TYPE_NONE;
    expression = cm_ast_get_expr(state->ast, expression_id);
    segment = expression == NULL || expression->kind != CM_AST_EXPR_PATH
        ? NULL : cm_hir_body_exact_path_segment(state->ast,
            expression->data.path.path);
    if (segment == NULL || segment->argument_count != 0u
        || !cm_hir_body_find_local(state->context, state->body,
            state->ast, state->let_plans, state->visible_let_count,
            state->base_local_count, segment->name, &local_index)) {
        return CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
    }
    if (local_index < state->base_local_count) {
        local_type = state->body->locals[local_index].type;
    } else {
        const CmHirBodyLetPlan *plan;

        plan = &state->let_plans[local_index - state->base_local_count];
        if (plan->inference_term != CM_TYPECK_TYPE_NONE) {
            *out_term = plan->inference_term;
            return CM_HIR_BODY_LOWER_OK;
        }
        local_type = plan->resolved_type;
    }
    status = cm_typeck_import_hir_type(typeck, local_type, out_term);
    return status == CM_TYPECK_OK ? CM_HIR_BODY_LOWER_OK
        : cm_hir_body_typeck_status(status);
}

static CmHirBodyLowerStatus cm_hir_body_inference_expression_term(
    const CmHirBodyBuildState *state, CmTypeckContext *typeck,
    CmAstExprId expression_id, size_t depth, CmTypeckTypeId *out_term)
{
    const CmAstExpr *expression;
    CmHirBodyLowerStatus lower_status;
    CmTypeckStatus typeck_status;
    CmHirIntType integer_kind;
    CmTypeckType scratch_type;
    int has_suffix;

    *out_term = CM_TYPECK_TYPE_NONE;
    expression = cm_ast_get_expr(state->ast, expression_id);
    if (expression == NULL || depth >= state->ast->expressions.len) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if (expression->kind == CM_AST_EXPR_PATH) {
        return cm_hir_body_inference_path_term(state, typeck,
            expression_id, out_term);
    }
    if (expression->kind == CM_AST_EXPR_BINARY) {
        CmTypeckTypeId left_term;
        CmTypeckTypeId right_term;

        if (expression->data.binary.left == CM_AST_EXPR_NONE
            || expression->data.binary.right == CM_AST_EXPR_NONE
            || expression->data.binary.left >= expression_id
            || expression->data.binary.right >= expression_id) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        if (!cm_hir_body_ast_text_is(state->ast,
                expression->data.binary.operator_name, "+")
            && !cm_hir_body_ast_text_is(state->ast,
                expression->data.binary.operator_name, "-")) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR;
        }
        lower_status = cm_hir_body_inference_expression_term(state, typeck,
            expression->data.binary.left, depth + 1u, &left_term);
        if (lower_status != CM_HIR_BODY_LOWER_OK) return lower_status;
        lower_status = cm_hir_body_inference_expression_term(state, typeck,
            expression->data.binary.right, depth + 1u, &right_term);
        if (lower_status != CM_HIR_BODY_LOWER_OK) return lower_status;
        typeck_status = cm_typeck_unify(typeck, left_term, right_term);
        if (typeck_status != CM_TYPECK_OK) {
            return cm_hir_body_typeck_status(typeck_status);
        }
        *out_term = left_term;
        return CM_HIR_BODY_LOWER_OK;
    }
    if (expression->kind != CM_AST_EXPR_LITERAL) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    lower_status = cm_hir_body_literal_integer_kind(state->ast,
        expression->data.literal.text, &has_suffix, &integer_kind);
    if (lower_status != CM_HIR_BODY_LOWER_OK) return lower_status;
    if (!has_suffix) {
        typeck_status = cm_typeck_new_variable(typeck,
            CM_HIR_INFER_INTEGER,
            cm_hir_body_ast_span(state->source, expression->span),
            out_term);
    } else {
        memset(&scratch_type, 0, sizeof(scratch_type));
        scratch_type.kind = CM_TYPECK_TYPE_INTEGER;
        scratch_type.span = cm_hir_body_ast_span(state->source,
            expression->span);
        scratch_type.data.integer_type = integer_kind;
        typeck_status = cm_typeck_add_type(typeck, &scratch_type, out_term);
    }
    return typeck_status == CM_TYPECK_OK ? CM_HIR_BODY_LOWER_OK
        : cm_hir_body_typeck_status(typeck_status);
}

static CmHirBodyLowerStatus cm_hir_body_prepare_let_inference(
    CmHirBodyBuildState *state, const CmAstExpr *block,
    CmHirBodyLetPlan *let_plans)
{
    CmTypeckContext typeck;
    CmTypeckTypeId expected_return;
    CmTypeckTypeId default_i32;
    CmTypeckType scratch_i32;
    CmHirBodyLowerStatus lower_status;
    CmTypeckStatus typeck_status;
    uint32_t index;
    int needs_inference;

    needs_inference = 0;
    for (index = 0u; index < block->data.block.statement_count; ++index) {
        const CmAstStmt *statement;

        statement = cm_ast_get_stmt(state->ast,
            block->data.block.statements[index]);
        if (statement != NULL && statement->kind == CM_AST_STMT_LET
            && statement->data.let_stmt.type == CM_AST_TYPE_NONE) {
            needs_inference = 1;
            break;
        }
    }
    if (!needs_inference) return CM_HIR_BODY_LOWER_OK;
    cm_typeck_context_init(&typeck, state->context);
    typeck_status = cm_typeck_import_hir_type(&typeck,
        state->body->expected_type, &expected_return);
    if (typeck_status != CM_TYPECK_OK) {
        lower_status = cm_hir_body_typeck_status(typeck_status);
        goto fail_typeck;
    }
    for (index = 0u; index < block->data.block.statement_count; ++index) {
        const CmAstStmt *statement;
        const CmAstPattern *pattern;
        CmTypeckTypeId initializer_term;

        statement = cm_ast_get_stmt(state->ast,
            block->data.block.statements[index]);
        pattern = statement == NULL || statement->kind != CM_AST_STMT_LET
            ? NULL : cm_ast_get_pattern(state->ast,
                statement->data.let_stmt.pattern);
        if (statement == NULL || pattern == NULL
            || statement->data.let_stmt.initializer == CM_AST_EXPR_NONE) {
            lower_status = CM_HIR_BODY_LOWER_INVALID_BODY;
            goto fail_typeck;
        }
        let_plans[index].ast_name = pattern->kind == CM_AST_PATTERN_BINDING
            ? pattern->data.binding.name : CM_INTERN_ID_NONE;
        let_plans[index].initializer = statement->data.let_stmt.initializer;
        let_plans[index].statement_span = statement->span;
        let_plans[index].binding_span = pattern->span;
        if (statement->data.let_stmt.type != CM_AST_TYPE_NONE) {
            if (!cm_hir_body_ast_type_matches(state->context, state->ast,
                    statement->data.let_stmt.type,
                    state->body->expected_type)) {
                lower_status = CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
                goto fail_typeck;
            }
            let_plans[index].resolved_type = state->body->expected_type;
            continue;
        }
        typeck_status = cm_typeck_new_variable(&typeck,
            CM_HIR_INFER_GENERAL,
            cm_hir_body_ast_span(state->source, pattern->span),
            &let_plans[index].inference_term);
        if (typeck_status != CM_TYPECK_OK) {
            lower_status = cm_hir_body_typeck_status(typeck_status);
            goto fail_typeck;
        }
        state->visible_let_count = index;
        lower_status = cm_hir_body_inference_expression_term(state,
            &typeck, let_plans[index].initializer, 0u, &initializer_term);
        if (lower_status != CM_HIR_BODY_LOWER_OK) goto fail_typeck;
        typeck_status = cm_typeck_unify(&typeck,
            let_plans[index].inference_term, initializer_term);
        if (typeck_status != CM_TYPECK_OK) {
            lower_status = cm_hir_body_typeck_status(typeck_status);
            goto fail_typeck;
        }
    }
    /* A tail-local use is an exact return context and must beat fallback. */
    state->visible_let_count = block->data.block.statement_count;
    if (cm_ast_get_expr(state->ast, block->data.block.tail) != NULL
        && cm_ast_get_expr(state->ast, block->data.block.tail)->kind
            == CM_AST_EXPR_PATH) {
        CmTypeckTypeId tail_term;

        lower_status = cm_hir_body_inference_path_term(state, &typeck,
            block->data.block.tail, &tail_term);
        if (lower_status != CM_HIR_BODY_LOWER_OK) goto fail_typeck;
        typeck_status = cm_typeck_unify(&typeck, tail_term,
            expected_return);
        if (typeck_status != CM_TYPECK_OK) {
            lower_status = cm_hir_body_typeck_status(typeck_status);
            goto fail_typeck;
        }
    }
    memset(&scratch_i32, 0, sizeof(scratch_i32));
    scratch_i32.kind = CM_TYPECK_TYPE_INTEGER;
    scratch_i32.span = state->body->span;
    scratch_i32.data.integer_type = CM_HIR_INT_I32;
    typeck_status = cm_typeck_add_type(&typeck, &scratch_i32, &default_i32);
    if (typeck_status != CM_TYPECK_OK) {
        lower_status = cm_hir_body_typeck_status(typeck_status);
        goto fail_typeck;
    }
    for (index = 0u; index < block->data.block.statement_count; ++index) {
        CmTypeckTypeId resolved;
        const CmTypeckType *resolved_type;

        if (let_plans[index].inference_term == CM_TYPECK_TYPE_NONE) continue;
        typeck_status = cm_typeck_resolve(&typeck,
            let_plans[index].inference_term, &resolved);
        resolved_type = typeck_status == CM_TYPECK_OK
            ? cm_typeck_get_type(&typeck, resolved) : NULL;
        if (typeck_status != CM_TYPECK_OK || resolved_type == NULL) {
            lower_status = cm_hir_body_typeck_status(typeck_status);
            goto fail_typeck;
        }
        if (resolved_type->kind == CM_TYPECK_TYPE_VARIABLE
            && resolved_type->data.variable.class_kind
                == CM_HIR_INFER_INTEGER) {
            typeck_status = cm_typeck_unify(&typeck, resolved, default_i32);
            if (typeck_status != CM_TYPECK_OK) {
                lower_status = cm_hir_body_typeck_status(typeck_status);
                goto fail_typeck;
            }
        } else if (resolved_type->kind == CM_TYPECK_TYPE_VARIABLE) {
            lower_status = CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
            goto fail_typeck;
        }
    }
    for (index = 0u; index < block->data.block.statement_count; ++index) {
        CmTypeckTypeId resolved;
        const CmTypeckType *resolved_type;

        if (let_plans[index].inference_term == CM_TYPECK_TYPE_NONE) continue;
        typeck_status = cm_typeck_resolve(&typeck,
            let_plans[index].inference_term, &resolved);
        resolved_type = typeck_status == CM_TYPECK_OK
            ? cm_typeck_get_type(&typeck, resolved) : NULL;
        if (typeck_status != CM_TYPECK_OK || resolved_type == NULL
            || resolved_type->kind != CM_TYPECK_TYPE_INTEGER) {
            lower_status = typeck_status == CM_TYPECK_OK
                ? CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE
                : cm_hir_body_typeck_status(typeck_status);
            goto fail_typeck;
        }
        let_plans[index].resolved_integer_kind =
            resolved_type->data.integer_type;
        let_plans[index].has_resolved_integer_kind = 1;
        let_plans[index].resolved_type = cm_hir_body_find_integer_type(
            state->context, resolved_type->data.integer_type);
    }
    cm_typeck_context_destroy(&typeck);
    return CM_HIR_BODY_LOWER_OK;

fail_typeck:
    cm_typeck_context_destroy(&typeck);
    return lower_status;
}

/*
 * Keep this first control-flow slice free of calls, nested control flow, and
 * aggregate construction.  MIR can therefore replay every admitted child as
 * an exact literal/local/field/add/sub value tree without inventing ordering.
 */
static int cm_hir_body_if_value_shape_supported(const CmAst *ast,
    CmAstExprId expression_id, size_t depth)
{
    const CmAstExpr *expression;

    expression = cm_ast_get_expr(ast, expression_id);
    if (depth >= ast->expressions.len || expression == NULL) return 0;
    switch (expression->kind) {
    case CM_AST_EXPR_LITERAL:
    case CM_AST_EXPR_PATH:
        return 1;
    case CM_AST_EXPR_BINARY:
        return expression->data.binary.left < expression_id
            && expression->data.binary.right < expression_id
            && cm_hir_body_if_value_shape_supported(ast,
                expression->data.binary.left, depth + 1u)
            && cm_hir_body_if_value_shape_supported(ast,
                expression->data.binary.right, depth + 1u);
    case CM_AST_EXPR_FIELD:
        return expression->data.field.base < expression_id
            && cm_hir_body_if_value_shape_supported(ast,
                expression->data.field.base, depth + 1u);
    default:
        return 0;
    }
}

static CmHirBodyLowerStatus cm_hir_body_preflight_comparison(
    const CmHirBodyBuildState *state, CmHirTypeId operand_type,
    const char *operator_name,
    CmAstExprId expression_id, CmAstSpan parent_span, size_t depth,
    CmHirBodyExpressionCounts *out_counts)
{
    const CmAstExpr *expression;
    CmHirBodyExpressionCounts left;
    CmHirBodyExpressionCounts right;
    CmHirBodyLowerStatus status;

    memset(out_counts, 0, sizeof(*out_counts));
    expression = cm_ast_get_expr(state->ast, expression_id);
    if (depth >= state->ast->expressions.len || expression == NULL
        || expression->span.start > expression->span.end
        || expression->span.start < parent_span.start
        || expression->span.end > parent_span.end
        || expression->kind != CM_AST_EXPR_BINARY) {
        return expression == NULL ? CM_HIR_BODY_LOWER_INVALID_BODY
            : CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    if (!cm_hir_body_ast_text_is(state->ast,
            expression->data.binary.operator_name, operator_name)) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR;
    }
    if (expression->data.binary.left >= expression_id
        || expression->data.binary.right >= expression_id) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if (!cm_hir_body_if_value_shape_supported(state->ast,
            expression->data.binary.left, depth + 1u)
        || !cm_hir_body_if_value_shape_supported(state->ast,
            expression->data.binary.right, depth + 1u)) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    status = cm_hir_body_preflight_typed_expression(state, operand_type,
        expression->data.binary.left, expression->span, depth + 1u, &left);
    if (status != CM_HIR_BODY_LOWER_OK) return status;
    status = cm_hir_body_preflight_typed_expression(state, operand_type,
        expression->data.binary.right, expression->span, depth + 1u,
        &right);
    if (status != CM_HIR_BODY_LOWER_OK) return status;
    out_counts->expression_count = 1u;
    out_counts->needs_bool_type = 1;
    if (!cm_hir_body_counts_add(out_counts, &left)
        || !cm_hir_body_counts_add(out_counts, &right)) {
        return CM_HIR_BODY_LOWER_HIR_FAILURE;
    }
    return CM_HIR_BODY_LOWER_OK;
}

static CmHirBodyLowerStatus cm_hir_body_preflight_value_block(
    const CmHirBodyBuildState *state, CmHirTypeId expected_type,
    CmAstExprId expression_id, CmAstSpan parent_span, size_t depth,
    CmHirBodyExpressionCounts *out_counts)
{
    const CmAstExpr *expression;
    CmHirBodyExpressionCounts tail;
    CmHirBodyLowerStatus status;

    memset(out_counts, 0, sizeof(*out_counts));
    expression = cm_ast_get_expr(state->ast, expression_id);
    if (depth >= state->ast->expressions.len || expression == NULL
        || expression->span.start > expression->span.end
        || expression->span.start < parent_span.start
        || expression->span.end > parent_span.end) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if (expression->kind != CM_AST_EXPR_BLOCK
        || expression->data.block.is_unsafe
        || expression->data.block.is_const
        || expression->data.block.statement_count != 0u
        || expression->data.block.statements != NULL
        || expression->data.block.tail == CM_AST_EXPR_NONE) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    if (expression->data.block.tail >= expression_id) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if (!cm_hir_body_if_value_shape_supported(state->ast,
            expression->data.block.tail, depth + 1u)) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    status = cm_hir_body_preflight_typed_expression(state, expected_type,
        expression->data.block.tail, expression->span, depth + 1u, &tail);
    if (status != CM_HIR_BODY_LOWER_OK) return status;
    out_counts->expression_count = 1u;
    if (!cm_hir_body_counts_add(out_counts, &tail)) {
        return CM_HIR_BODY_LOWER_HIR_FAILURE;
    }
    return CM_HIR_BODY_LOWER_OK;
}

static CmHirBodyLowerStatus cm_hir_body_preflight_typed_expression(
    const CmHirBodyBuildState *state, CmHirTypeId expected_type,
    CmAstExprId expression_id, CmAstSpan parent_span, size_t depth,
    CmHirBodyExpressionCounts *out_counts)
{
    const CmAstExpr *expression;
    CmHirBodyLowerStatus status;
    CmHirBodyExpressionCounts left;
    CmHirBodyExpressionCounts right;
    CmHirBodyExpressionCounts child;
    const CmHirType *type;
    uint64_t ignored_value;

    expression = cm_ast_get_expr(state->ast, expression_id);
    memset(out_counts, 0, sizeof(*out_counts));
    type = cm_hir_get_type(state->context, expected_type);
    if (depth >= state->ast->expressions.len || expression == NULL
        || type == NULL
        || expression->span.start > expression->span.end
        || expression->span.start < parent_span.start
        || expression->span.end > parent_span.end) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if ((expression->attribute_count == 0u
            && expression->attributes != NULL)
        || (expression->attribute_count != 0u
            && expression->attributes == NULL)) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    if (expression->attribute_count != 0u) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    if (state->trait_default_closed_slice
        && !cm_hir_body_trait_default_expression_supported(state->ast,
            expression_id, depth)) {
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    switch (expression->kind) {
    case CM_AST_EXPR_RAW_REFERENCE:
    {
        const CmAstExpr *operand;

        if ((unsigned int)expression->data.raw_reference.kind
                > (unsigned int)CM_AST_RAW_REFERENCE_MUT
            || expression->data.raw_reference.operand == CM_AST_EXPR_NONE
            || expression->data.raw_reference.operand >= expression_id) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        operand = cm_ast_get_expr(state->ast,
            expression->data.raw_reference.operand);
        if (operand == NULL || operand->span.start > operand->span.end
            || operand->span.start <= expression->span.start
            || operand->span.end != expression->span.end) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    case CM_AST_EXPR_LITERAL:
        if (type->kind != CM_HIR_TYPE_INTEGER_KIND
            || (type->data.integer_type.kind != CM_HIR_INT_I32
                && type->data.integer_type.kind != CM_HIR_INT_U32
                && type->data.integer_type.kind != CM_HIR_INT_USIZE)) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
        }
        status = cm_hir_parse_integer_literal(state->ast,
            expression->data.literal.text, type->data.integer_type.kind,
            &ignored_value);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        out_counts->expression_count = 1u;
        return CM_HIR_BODY_LOWER_OK;
    case CM_AST_EXPR_QUALIFIED_PATH:
        if (!cm_hir_body_qualified_path_valid(state->ast, expression)) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    case CM_AST_EXPR_PATH:
    {
        const CmAstPathSegment *segment;
        uint32_t local_index;

        segment = cm_hir_body_exact_path_segment(state->ast,
            expression->data.path.path);
        if (segment == NULL || segment->argument_count != 0u
            || !cm_hir_body_find_local(state->context, state->body,
                state->ast, state->let_plans, state->visible_let_count,
                state->base_local_count, segment->name,
                &local_index)) {
            return CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
        }
        if (local_index >= state->base_local_count
                + state->visible_let_count
            || (local_index < state->base_local_count
                && !cm_hir_body_type_equal(state->context,
                    state->body->locals[local_index].type,
                    expected_type))
            || (local_index >= state->base_local_count
                && !cm_hir_body_type_equal(state->context,
                    state->let_plans[local_index
                        - state->base_local_count].resolved_type,
                    expected_type))) {
            return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
        }
        out_counts->expression_count = 1u;
        return CM_HIR_BODY_LOWER_OK;
    }
    case CM_AST_EXPR_BINARY:
        if (expression->data.binary.left >= expression_id
            || expression->data.binary.right >= expression_id) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        if (!cm_hir_body_is_wrapping_unsigned(state->context,
                expected_type)) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
        }
        if (!cm_hir_body_ast_text_is(state->ast,
                expression->data.binary.operator_name, "+")
            && !cm_hir_body_ast_text_is(state->ast,
                expression->data.binary.operator_name, "-")) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR;
        }
        status = cm_hir_body_preflight_typed_expression(state,
            expected_type, expression->data.binary.left, expression->span,
            depth + 1u, &left);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        status = cm_hir_body_preflight_typed_expression(state,
            expected_type, expression->data.binary.right, expression->span,
            depth + 1u, &right);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        out_counts->expression_count = 1u;
        if (!cm_hir_body_counts_add(out_counts, &left)
            || !cm_hir_body_counts_add(out_counts, &right)) {
            return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
        return CM_HIR_BODY_LOWER_OK;
    case CM_AST_EXPR_IF:
    {
        const CmAstExpr *then_expression;
        const CmAstExpr *else_expression;
        CmHirBodyExpressionCounts condition;
        CmHirBodyExpressionCounts then_branch;
        CmHirBodyExpressionCounts else_branch;

        if (expression_id != state->allowed_if_expression) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        }
        if (!cm_hir_body_is_wrapping_unsigned(state->context,
                expected_type)) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
        }
        if (expression->data.if_expr.pattern != CM_AST_PATTERN_NONE
            || expression->data.if_expr.else_expr == CM_AST_EXPR_NONE) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        }
        if (expression->data.if_expr.condition >= expression_id
            || expression->data.if_expr.then_expr >= expression_id
            || expression->data.if_expr.else_expr >= expression_id) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        then_expression = cm_ast_get_expr(state->ast,
            expression->data.if_expr.then_expr);
        else_expression = cm_ast_get_expr(state->ast,
            expression->data.if_expr.else_expr);
        if (then_expression == NULL || else_expression == NULL) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        if (then_expression->kind != CM_AST_EXPR_BLOCK
            || else_expression->kind != CM_AST_EXPR_BLOCK) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        }
        if (then_expression->span.start < expression->span.start
            || then_expression->span.end > expression->span.end
            || else_expression->span.start < expression->span.start
            || else_expression->span.end > expression->span.end
            || then_expression->span.end > else_expression->span.start) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        status = cm_hir_body_preflight_comparison(state, expected_type,
            cm_hir_body_is_u32(state->context, expected_type) ? "==" : "<",
            expression->data.if_expr.condition, expression->span,
            depth + 1u, &condition);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        {
            const CmAstExpr *condition_expression;

            condition_expression = cm_ast_get_expr(state->ast,
                expression->data.if_expr.condition);
            if (condition_expression == NULL
                || condition_expression->span.end
                    > then_expression->span.start) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
        }
        status = cm_hir_body_preflight_value_block(state, expected_type,
            expression->data.if_expr.then_expr, expression->span,
            depth + 1u, &then_branch);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        status = cm_hir_body_preflight_value_block(state, expected_type,
            expression->data.if_expr.else_expr, expression->span,
            depth + 1u, &else_branch);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        out_counts->expression_count = 1u;
        if (!cm_hir_body_counts_add(out_counts, &condition)
            || !cm_hir_body_counts_add(out_counts, &then_branch)
            || !cm_hir_body_counts_add(out_counts, &else_branch)) {
            return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
        return CM_HIR_BODY_LOWER_OK;
    }
    case CM_AST_EXPR_CALL:
    {
        CmHirBodyCallPlan plan;
        CmHirBodyQualifiedCallPlan qualified_plan;
        const CmAstExpr *callee;
        uint32_t index;
        size_t own_words;

        if (expression->data.call.callee >= expression_id) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        callee = cm_ast_get_expr(state->ast,
            expression->data.call.callee);
        if (callee != NULL
            && callee->kind == CM_AST_EXPR_QUALIFIED_PATH) {
            status = cm_hir_body_resolve_qualified_call(state, expression,
                expected_type, &qualified_plan);
            if (status != CM_HIR_BODY_LOWER_OK) return status;
            out_counts->expression_count = 1u;
            out_counts->call_word_count = qualified_plan.argument_count;
            for (index = 0u; index < qualified_plan.argument_count;
                 ++index) {
                if (qualified_plan.arguments[index] >= expression_id) {
                    return CM_HIR_BODY_LOWER_INVALID_BODY;
                }
                status = cm_hir_body_preflight_typed_expression(state,
                    qualified_plan.argument_types[index],
                    qualified_plan.arguments[index], expression->span,
                    depth + 1u, &child);
                if (status != CM_HIR_BODY_LOWER_OK) return status;
                if (!cm_hir_body_counts_add(out_counts, &child)) {
                    return CM_HIR_BODY_LOWER_HIR_FAILURE;
                }
            }
            return CM_HIR_BODY_LOWER_OK;
        }
        status = cm_hir_body_resolve_scalar_call(state->context,
            state->owner_item, state->ast, expression, expected_type,
            &plan);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        out_counts->expression_count = 1u;
        if (!cm_size_add((size_t)plan.substitution_count,
                (size_t)plan.argument_count, &own_words)) {
            return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
        out_counts->call_word_count = own_words;
        for (index = 0u; index < plan.argument_count; ++index) {
            if (plan.arguments[index] >= expression_id) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
            status = cm_hir_body_preflight_typed_expression(state,
                plan.argument_types[index], plan.arguments[index],
                expression->span, depth + 1u, &child);
            if (status != CM_HIR_BODY_LOWER_OK) return status;
            if (!cm_hir_body_counts_add(out_counts, &child)) {
                return CM_HIR_BODY_LOWER_HIR_FAILURE;
            }
        }
        return CM_HIR_BODY_LOWER_OK;
    }
    case CM_AST_EXPR_STRUCT:
    {
        const CmHirItem *item;
        const CmAstPath *path;
        uint32_t index;

        status = cm_hir_body_resolve_struct_expression(state, expression,
            expected_type, &item);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        path = cm_ast_get_path(state->ast,
            expression->data.struct_expr.path);
        if (path == NULL) return CM_HIR_BODY_LOWER_INVALID_BODY;
        if (expression->data.struct_expr.field_count
                != item->data.aggregate_item.field_count
            || (expression->data.struct_expr.field_count == 0u)
                != (expression->data.struct_expr.fields == NULL)) {
            return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
        }
        out_counts->expression_count = 1u;
        out_counts->aggregate_field_count =
            expression->data.struct_expr.field_count;
        for (index = 0u; index < expression->data.struct_expr.field_count;
             ++index) {
            const CmAstExprField *field;
            const CmAstExpr *value;
            const CmAstAttribute *attribute;
            uint32_t attribute_index;
            uint32_t field_index;
            uint32_t prior;

            field = &expression->data.struct_expr.fields[index];
            value = cm_ast_get_expr(state->ast, field->value);
            if ((field->attribute_count == 0u
                    && field->attributes != NULL)
                || (field->attribute_count != 0u
                    && field->attributes == NULL)) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
            for (attribute_index = 0u;
                 attribute_index < field->attribute_count;
                 ++attribute_index) {
                attribute = cm_ast_get_attribute(state->ast,
                    field->attributes[attribute_index]);
                if (attribute == NULL
                    || attribute->style != CM_AST_ATTR_OUTER
                    || attribute->span.start >= attribute->span.end
                    || attribute->span.start <= path->span.end
                    || attribute->span.end > field->span.start
                    || (attribute_index != 0u
                        && cm_ast_get_attribute(state->ast,
                            field->attributes[attribute_index - 1u])
                            ->span.end >= attribute->span.start)) {
                    return CM_HIR_BODY_LOWER_INVALID_BODY;
                }
            }
            if (field->attribute_count != 0u) {
                return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
            }
            if (field->value >= expression_id || value == NULL
                || field->span.start >= field->span.end
                || field->span.start <= path->span.end
                || field->span.end >= expression->span.end
                || value->span.start < field->span.start
                || value->span.end != field->span.end
                || (index != 0u
                    && expression->data.struct_expr.fields[index - 1u]
                        .span.end >= field->span.start)
                || !cm_hir_body_find_struct_field(state->context,
                    state->ast, item, field->name, &field_index)
                || !cm_hir_body_visibility_allows(state->context,
                    state->owner_item->owner_module, item->owner_module,
                    item->data.aggregate_item.fields[field_index]
                        .visibility)) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
            if (field->is_shorthand) {
                const CmAstPathSegment *segment;

                segment = value->kind == CM_AST_EXPR_PATH
                    ? cm_hir_body_exact_path_segment(state->ast,
                        value->data.path.path) : NULL;
                if (segment == NULL || segment->argument_count != 0u
                    || segment->name != field->name
                    || field->span.start != value->span.start) {
                    return CM_HIR_BODY_LOWER_INVALID_BODY;
                }
            } else if (field->span.start >= value->span.start) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
            for (prior = 0u; prior < index; ++prior) {
                uint32_t prior_field_index;

                if (!cm_hir_body_find_struct_field(state->context,
                        state->ast, item,
                        expression->data.struct_expr.fields[prior].name,
                        &prior_field_index)
                    || prior_field_index == field_index
                    || expression->data.struct_expr.fields[prior].value
                        == field->value) {
                    return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
                }
            }
            status = cm_hir_body_preflight_typed_expression(state,
                item->data.aggregate_item.fields[field_index].type,
                field->value, field->span, depth + 1u, &child);
            if (status != CM_HIR_BODY_LOWER_OK) return status;
            if (!cm_hir_body_counts_add(out_counts, &child)) {
                return CM_HIR_BODY_LOWER_HIR_FAILURE;
            }
        }
        return CM_HIR_BODY_LOWER_OK;
    }
    case CM_AST_EXPR_FIELD:
    {
        const CmHirItem *item;
        CmHirTypeId base_type;
        uint32_t field_index;

        if (expression->data.field.base >= expression_id) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        status = cm_hir_body_resolve_field_expression(state, expression,
            depth, &base_type, &item, &field_index);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        if (!cm_hir_body_type_equal(state->context,
                item->data.aggregate_item.fields[field_index].type,
                expected_type)) {
            return CM_HIR_BODY_LOWER_TYPE_MISMATCH;
        }
        status = cm_hir_body_preflight_typed_expression(state, base_type,
            expression->data.field.base, expression->span, depth + 1u,
            &child);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        out_counts->expression_count = 1u;
        if (!cm_hir_body_counts_add(out_counts, &child)) {
            return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
        return CM_HIR_BODY_LOWER_OK;
    }
    case CM_AST_EXPR_TUPLE_FIELD:
    {
        const CmAstExpr *base;

        base = cm_ast_get_expr(state->ast,
            expression->data.tuple_field.base);
        if (expression->data.tuple_field.base >= expression_id
            || base == NULL
            || base->span.start != expression->span.start
            || base->span.end
                >= expression->data.tuple_field.index_span.start
            || expression->data.tuple_field.index_span.start
                >= expression->data.tuple_field.index_span.end
            || expression->data.tuple_field.index_span.end
                != expression->span.end) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    case CM_AST_EXPR_TRY:
    {
        const CmAstExpr *operand;

        operand = cm_ast_get_expr(state->ast,
            expression->data.try_expr.operand);
        if (expression->data.try_expr.operand >= expression_id
            || operand == NULL
            || operand->span.start != expression->span.start
            || operand->span.end >= expression->span.end) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    case CM_AST_EXPR_TRY_BLOCK:
    {
        const CmAstExpr *operand;

        operand = cm_ast_get_expr(state->ast,
            expression->data.try_expr.operand);
        if (expression->data.try_expr.operand >= expression_id
            || operand == NULL || operand->kind != CM_AST_EXPR_BLOCK
            || operand->span.start <= expression->span.start
            || operand->span.end != expression->span.end) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    case CM_AST_EXPR_METHOD_CALL:
    {
        const CmInternedString *method_name;
        const CmAstExpr *receiver;
        CmHirTypeId receiver_type;
        size_t trait_count;
        uint32_t prior_end;
        uint32_t index;
        int has_generic_arguments;

        method_name = cm_ast_get_string(state->ast,
            expression->data.method_call.name);
        if ((expression->data.method_call.generic_argument_count == 0u)
                != (expression->data.method_call.generic_arguments == NULL)
            || (expression->data.method_call.argument_count == 0u)
                != (expression->data.method_call.arguments == NULL)
            || expression->data.method_call.receiver >= expression_id
            || method_name == NULL || method_name->len == 0u) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        has_generic_arguments =
            expression->data.method_call.generic_argument_span.start != 0u
            || expression->data.method_call.generic_argument_span.end != 0u;
        if (has_generic_arguments) {
            if (expression->data.method_call.generic_argument_span.start
                    >= expression->data.method_call.generic_argument_span.end
                || expression->data.method_call.generic_argument_span.start
                    < expression->span.start
                || expression->data.method_call.generic_argument_span.end
                    > expression->span.end) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
            for (index = 0u;
                 index < expression->data.method_call.generic_argument_count;
                 ++index) {
                const CmAstGenericArg *argument;

                argument = &expression->data.method_call
                    .generic_arguments[index];
                if (argument->span.start < expression->data.method_call
                        .generic_argument_span.start
                    || argument->span.end > expression->data.method_call
                        .generic_argument_span.end
                    || argument->span.start >= argument->span.end) {
                    return CM_HIR_BODY_LOWER_INVALID_BODY;
                }
            }
            return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        }
        receiver = cm_ast_get_expr(state->ast,
            expression->data.method_call.receiver);
        if (receiver == NULL || receiver->span.start != expression->span.start
            || receiver->span.end >= expression->span.end) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        status = cm_hir_body_infer_expression_type(state,
            expression->data.method_call.receiver, depth + 1u,
            &receiver_type);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        status = cm_hir_body_preflight_typed_expression(state,
            receiver_type, expression->data.method_call.receiver,
            expression->span, depth + 1u, &child);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        out_counts->expression_count = 1u;
        out_counts->method_call_count = 1u;
        if (!cm_hir_body_counts_add(out_counts, &child)) {
            return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
        prior_end = receiver->span.end;
        for (index = 0u;
             index < expression->data.method_call.argument_count; ++index) {
            const CmAstExpr *argument;
            CmHirBodyExpressionCounts argument_counts;
            CmHirTypeId argument_type;

            if (expression->data.method_call.arguments[index]
                    >= expression_id) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
            argument = cm_ast_get_expr(state->ast,
                expression->data.method_call.arguments[index]);
            if (argument == NULL || argument->span.start < prior_end
                || argument->span.end > expression->span.end) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
            status = cm_hir_body_infer_expression_type(state,
                expression->data.method_call.arguments[index], depth + 1u,
                &argument_type);
            if (status != CM_HIR_BODY_LOWER_OK) return status;
            status = cm_hir_body_preflight_typed_expression(state,
                argument_type,
                expression->data.method_call.arguments[index],
                expression->span, depth + 1u, &argument_counts);
            if (status != CM_HIR_BODY_LOWER_OK) return status;
            if (!cm_hir_body_counts_add(out_counts, &argument_counts)) {
                return CM_HIR_BODY_LOWER_HIR_FAILURE;
            }
            prior_end = argument->span.end;
        }
        status = cm_hir_body_method_trait_scope(state, NULL, 0u,
            &trait_count);
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        if (trait_count > (size_t)UINT32_MAX) {
            return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
        out_counts->method_trait_count = trait_count;
        out_counts->method_argument_count =
            expression->data.method_call.argument_count;
        return CM_HIR_BODY_LOWER_OK;
    }
    case CM_AST_EXPR_MATCH:
    {
        uint32_t index;

        if ((expression->data.match_expr.arm_count == 0u)
                != (expression->data.match_expr.arms == NULL)) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        for (index = 0u; index < expression->data.match_expr.arm_count;
             ++index) {
            const CmAstMatchArm *arm;
            const CmAstPattern *pattern;
            const CmAstExpr *initializer;

            arm = &expression->data.match_expr.arms[index];
            if (arm->guard_pattern == CM_AST_PATTERN_NONE) continue;
            pattern = cm_ast_get_pattern(state->ast, arm->guard_pattern);
            initializer = cm_ast_get_expr(state->ast,
                arm->guard_initializer);
            if (arm->guard != CM_AST_EXPR_NONE
                || arm->guard_initializer == CM_AST_EXPR_NONE
                || arm->guard_initializer >= expression_id
                || pattern == NULL || initializer == NULL
                || arm->guard_span.start >= arm->guard_span.end
                || arm->guard_span.start < expression->span.start
                || arm->guard_span.end > expression->span.end
                || pattern->span.start < arm->guard_span.start
                || pattern->span.end > initializer->span.start
                || initializer->span.end != arm->guard_span.end) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
        }
        /* Match and structured let-guard semantics are not lowered yet. */
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
    default:
        return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
    }
}

static CmHirBodyLowerStatus cm_hir_body_preflight_let_block(
    CmHirBodyBuildState *state, const CmAstExpr *block,
    CmHirBodyLetPlan *let_plans, CmHirBodyExpressionCounts *out_counts)
{
    CmHirBodyExpressionCounts child;
    CmHirBodyLowerStatus status;
    uint32_t index;

    memset(out_counts, 0, sizeof(*out_counts));
    if (block->data.block.statement_count == 0u
        || block->data.block.statements == NULL
        || block->data.block.tail == CM_AST_EXPR_NONE
        || let_plans == NULL) {
        return CM_HIR_BODY_LOWER_INVALID_BODY;
    }
    for (index = 0u; index < block->data.block.statement_count; ++index) {
        const CmAstStmt *statement;
        const CmAstPattern *pattern;
        const CmAstExpr *else_block;
        uint32_t prior_index;

        statement = cm_ast_get_stmt(state->ast,
            block->data.block.statements[index]);
        if (statement == NULL || statement->span.start > statement->span.end
            || statement->span.start < block->span.start
            || statement->span.end > block->span.end
            || ((statement->attribute_count == 0u)
                != (statement->attributes == NULL))) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        if (statement->attribute_count != 0u) {
            uint32_t attribute_index;

            for (attribute_index = 0u;
                    attribute_index < statement->attribute_count;
                    ++attribute_index) {
                const CmAstAttribute *attribute;

                attribute = cm_ast_get_attribute(state->ast,
                    statement->attributes[attribute_index]);
                if (attribute == NULL
                    || attribute->style != CM_AST_ATTR_OUTER
                    || attribute->span.start > attribute->span.end
                    || attribute->span.start < statement->span.start
                    || attribute->span.end > statement->span.end) {
                    return CM_HIR_BODY_LOWER_INVALID_BODY;
                }
            }
            return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        }
        if (statement->kind != CM_AST_STMT_LET) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        }
        if (statement->data.let_stmt.else_block != CM_AST_EXPR_NONE) {
            else_block = cm_ast_get_expr(state->ast,
                statement->data.let_stmt.else_block);
            if (statement->data.let_stmt.initializer == CM_AST_EXPR_NONE
                || else_block == NULL
                || else_block->kind != CM_AST_EXPR_BLOCK
                || else_block->span.start > else_block->span.end
                || else_block->span.start < statement->span.start
                || else_block->span.end > statement->span.end) {
                return CM_HIR_BODY_LOWER_INVALID_BODY;
            }
            return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        }
        pattern = cm_ast_get_pattern(state->ast,
            statement->data.let_stmt.pattern);
        if (pattern == NULL || pattern->span.start > pattern->span.end
            || pattern->span.start < statement->span.start
            || pattern->span.end > statement->span.end) {
            return CM_HIR_BODY_LOWER_INVALID_BODY;
        }
        if (pattern->kind != CM_AST_PATTERN_BINDING
            || pattern->data.binding.name == CM_INTERN_ID_NONE
            || pattern->data.binding.subpattern != CM_AST_PATTERN_NONE
            || pattern->data.binding.is_ref
            || pattern->data.binding.is_mutable
            || statement->data.let_stmt.initializer == CM_AST_EXPR_NONE) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        }
        if (statement->data.let_stmt.type == CM_AST_TYPE_NONE) {
            if (let_plans[index].resolved_type == CM_HIR_TYPE_NONE
                && !let_plans[index].has_resolved_integer_kind) {
                return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
            }
        } else if (!cm_hir_body_ast_type_matches(state->context,
                state->ast, statement->data.let_stmt.type,
                state->body->expected_type)) {
            return CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE;
        }
        for (prior_index = 0u; prior_index < state->body->local_count;
             ++prior_index) {
            if (cm_hir_body_ast_hir_text_equal(state->ast,
                    pattern->data.binding.name, state->context,
                    state->body->locals[prior_index].name)) {
                return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
            }
        }
        for (prior_index = 0u; prior_index < index; ++prior_index) {
            if (let_plans[prior_index].ast_name
                    == pattern->data.binding.name) {
                return CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
            }
        }
        let_plans[index].ast_name = pattern->data.binding.name;
        let_plans[index].initializer =
            statement->data.let_stmt.initializer;
        let_plans[index].statement_span = statement->span;
        let_plans[index].binding_span = pattern->span;
        if (statement->data.let_stmt.type != CM_AST_TYPE_NONE) {
            let_plans[index].resolved_type = state->body->expected_type;
        }
        state->visible_let_count = index;
        if (let_plans[index].resolved_type != CM_HIR_TYPE_NONE) {
            status = cm_hir_body_preflight_typed_expression(state,
                let_plans[index].resolved_type,
                let_plans[index].initializer, statement->span, 0u, &child);
        } else {
            const CmAstExpr *initializer;
            uint64_t ignored_value;

            memset(&child, 0, sizeof(child));
            initializer = cm_ast_get_expr(state->ast,
                let_plans[index].initializer);
            if (initializer == NULL
                || initializer->span.start > initializer->span.end
                || initializer->span.start < statement->span.start
                || initializer->span.end > statement->span.end) {
                status = CM_HIR_BODY_LOWER_INVALID_BODY;
            } else if (initializer->attribute_count != 0u
                || initializer->attributes != NULL) {
                status = CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
            } else if (initializer->kind == CM_AST_EXPR_LITERAL) {
                status = cm_hir_parse_integer_literal(state->ast,
                    initializer->data.literal.text,
                    let_plans[index].resolved_integer_kind,
                    &ignored_value);
                child.expression_count = status == CM_HIR_BODY_LOWER_OK
                    ? 1u : 0u;
            } else if (initializer->kind == CM_AST_EXPR_PATH) {
                const CmAstPathSegment *segment;
                uint32_t local_index;
                CmHirIntType local_kind;
                int matches;

                segment = cm_hir_body_exact_path_segment(state->ast,
                    initializer->data.path.path);
                matches = segment != NULL && segment->argument_count == 0u
                    && cm_hir_body_find_local(state->context, state->body,
                        state->ast, state->let_plans,
                        state->visible_let_count, state->base_local_count,
                        segment->name, &local_index);
                if (matches && local_index < state->base_local_count) {
                    const CmHirType *local_type;

                    local_type = cm_hir_get_type(state->context,
                        state->body->locals[local_index].type);
                    matches = local_type != NULL
                        && local_type->kind == CM_HIR_TYPE_INTEGER_KIND;
                    local_kind = matches
                        ? local_type->data.integer_type.kind
                        : CM_HIR_INT_I32;
                } else if (matches) {
                    const CmHirBodyLetPlan *local_plan;
                    const CmHirType *local_type;

                    local_plan = &let_plans[local_index
                        - state->base_local_count];
                    local_type = cm_hir_get_type(state->context,
                        local_plan->resolved_type);
                    matches = local_plan->has_resolved_integer_kind
                        || (local_type != NULL
                            && local_type->kind
                                == CM_HIR_TYPE_INTEGER_KIND);
                    local_kind = local_plan->has_resolved_integer_kind
                        ? local_plan->resolved_integer_kind
                        : (matches ? local_type->data.integer_type.kind
                            : CM_HIR_INT_I32);
                }
                if (!matches) {
                    status = CM_HIR_BODY_LOWER_UNRESOLVED_PATH;
                } else if (local_kind
                        != let_plans[index].resolved_integer_kind) {
                    status = CM_HIR_BODY_LOWER_TYPE_MISMATCH;
                } else {
                    status = CM_HIR_BODY_LOWER_OK;
                    child.expression_count = 1u;
                }
            } else {
                status = CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
            }
        }
        if (status != CM_HIR_BODY_LOWER_OK) return status;
        if (!cm_hir_body_counts_add(out_counts, &child)) {
            return CM_HIR_BODY_LOWER_HIR_FAILURE;
        }
    }
    state->visible_let_count = block->data.block.statement_count;
    status = cm_hir_body_preflight_typed_expression(state,
        state->body->expected_type, block->data.block.tail, block->span,
        0u, &child);
    if (status != CM_HIR_BODY_LOWER_OK) return status;
    if (!cm_hir_body_counts_add(out_counts, &child)) {
        return CM_HIR_BODY_LOWER_HIR_FAILURE;
    }
    return CM_HIR_BODY_LOWER_OK;
}

static CmHirStatus cm_hir_body_build_typed_expression(
    CmHirBodyBuildState *state, CmHirTypeId expected_type,
    CmAstExprId expression_id, CmHirExprId *out_expression);

static CmHirStatus cm_hir_body_build_comparison(
    CmHirBodyBuildState *state, CmHirTypeId operand_type,
    CmAstExprId expression_id, CmHirExprId *out_expression)
{
    const CmAstExpr *expression;
    CmHirExprId left;
    CmHirExprId right;
    CmHirStatus status;

    expression = cm_ast_get_expr(state->ast, expression_id);
    if (expression == NULL || expression->kind != CM_AST_EXPR_BINARY
        || state->bool_type == CM_HIR_TYPE_NONE
        || !cm_hir_body_is_bool(state->context, state->bool_type)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    status = cm_hir_body_build_typed_expression(state, operand_type,
        expression->data.binary.left, &left);
    if (status != CM_HIR_OK) return status;
    status = cm_hir_body_build_typed_expression(state, operand_type,
        expression->data.binary.right, &right);
    if (status != CM_HIR_OK) return status;
    return cm_hir_body_add_binary_expression(state->context,
        state->body_id,
        cm_hir_body_is_u32(state->context, operand_type)
            ? CM_HIR_BINARY_EQUAL : CM_HIR_BINARY_LESS,
        left, right,
        state->bool_type,
        cm_hir_body_ast_span(state->source, expression->span),
        out_expression);
}

static CmHirStatus cm_hir_body_build_value_block(
    CmHirBodyBuildState *state, CmHirTypeId expected_type,
    CmAstExprId expression_id, CmHirExprId *out_expression)
{
    const CmAstExpr *expression;
    CmHirExpr block;
    CmHirExprId tail;
    CmHirStatus status;

    expression = cm_ast_get_expr(state->ast, expression_id);
    if (expression == NULL || expression->kind != CM_AST_EXPR_BLOCK
        || expression->data.block.tail == CM_AST_EXPR_NONE) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    status = cm_hir_body_build_typed_expression(state, expected_type,
        expression->data.block.tail, &tail);
    if (status != CM_HIR_OK) return status;
    memset(&block, 0, sizeof(block));
    block.kind = CM_HIR_EXPR_BLOCK;
    block.owner_body = state->body_id;
    block.type = expected_type;
    block.span = cm_hir_body_ast_span(state->source, expression->span);
    block.data.block.tail_expression = tail;
    return cm_hir_add_expr(state->context, &block, out_expression);
}

static CmHirStatus cm_hir_body_build_typed_expression(
    CmHirBodyBuildState *state, CmHirTypeId expected_type,
    CmAstExprId expression_id, CmHirExprId *out_expression)
{
    const CmAstExpr *expression;
    const CmHirType *type;

    expression = cm_ast_get_expr(state->ast, expression_id);
    type = cm_hir_get_type(state->context, expected_type);
    if (expression == NULL || type == NULL) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (expression->kind == CM_AST_EXPR_LITERAL) {
        CmHirExpr literal;
        uint64_t value;

        if (type->kind != CM_HIR_TYPE_INTEGER_KIND
            || (type->data.integer_type.kind != CM_HIR_INT_I32
                && type->data.integer_type.kind != CM_HIR_INT_U32
                && type->data.integer_type.kind != CM_HIR_INT_USIZE)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        if (cm_hir_parse_integer_literal(state->ast,
                expression->data.literal.text,
                type->data.integer_type.kind,
                &value) != CM_HIR_BODY_LOWER_OK) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        memset(&literal, 0, sizeof(literal));
        literal.kind = CM_HIR_EXPR_INTEGER;
        literal.owner_body = state->body_id;
        literal.type = expected_type;
        literal.span = cm_hir_body_ast_span(state->source,
            expression->span);
        literal.data.integer.low_bits = (uint64_t)value;
        return cm_hir_add_expr(state->context, &literal, out_expression);
    }
    if (expression->kind == CM_AST_EXPR_PATH) {
        const CmAstPathSegment *segment;
        uint32_t local_index;

        segment = cm_hir_body_exact_path_segment(state->ast,
            expression->data.path.path);
        if (segment == NULL || !cm_hir_body_find_local(state->context,
                state->body, state->ast, state->let_plans,
                state->visible_let_count, state->base_local_count,
                segment->name, &local_index)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        return cm_hir_body_add_local_expression(state->context,
            state->body_id, local_index, expected_type,
            cm_hir_body_ast_span(state->source, expression->span),
            out_expression);
    }
    if (expression->kind == CM_AST_EXPR_BINARY) {
        CmHirExprId left;
        CmHirExprId right;
        CmHirBinaryOperator operator_kind;
        CmHirStatus status;

        status = cm_hir_body_build_typed_expression(state, expected_type,
            expression->data.binary.left, &left);
        if (status != CM_HIR_OK) return status;
        status = cm_hir_body_build_typed_expression(state, expected_type,
            expression->data.binary.right, &right);
        if (status != CM_HIR_OK) return status;
        operator_kind = cm_hir_body_ast_text_is(state->ast,
                expression->data.binary.operator_name, "+")
            ? CM_HIR_BINARY_ADD : CM_HIR_BINARY_SUBTRACT;
        return cm_hir_body_add_binary_expression(state->context,
            state->body_id, operator_kind, left, right,
            expected_type,
            cm_hir_body_ast_span(state->source, expression->span),
            out_expression);
    }
    if (expression->kind == CM_AST_EXPR_IF) {
        CmHirExprId condition;
        CmHirExprId then_expression;
        CmHirExprId else_expression;
        CmHirStatus status;

        status = cm_hir_body_build_comparison(state, expected_type,
            expression->data.if_expr.condition, &condition);
        if (status != CM_HIR_OK) return status;
        status = cm_hir_body_build_value_block(state, expected_type,
            expression->data.if_expr.then_expr, &then_expression);
        if (status != CM_HIR_OK) return status;
        status = cm_hir_body_build_value_block(state, expected_type,
            expression->data.if_expr.else_expr, &else_expression);
        if (status != CM_HIR_OK) return status;
        return cm_hir_body_add_if_expression(state->context,
            state->body_id, condition, then_expression, else_expression,
            expected_type,
            cm_hir_body_ast_span(state->source, expression->span),
            out_expression);
    }
    if (expression->kind == CM_AST_EXPR_CALL) {
        CmHirBodyCallPlan plan;
        CmHirBodyQualifiedCallPlan qualified_plan;
        CmHirExpr call;
        CmHirExprId arguments[2];
        const CmAstExpr *callee;
        CmHirBodyLowerStatus lower_status;
        CmHirStatus status;
        uint32_t *call_words;
        size_t own_word_count;
        size_t next_cursor;
        uint32_t index;

        callee = cm_ast_get_expr(state->ast,
            expression->data.call.callee);
        if (callee != NULL
            && callee->kind == CM_AST_EXPR_QUALIFIED_PATH) {
            lower_status = cm_hir_body_resolve_qualified_call(state,
                expression, expected_type, &qualified_plan);
            if (lower_status != CM_HIR_BODY_LOWER_OK) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            for (index = 0u; index < qualified_plan.argument_count;
                 ++index) {
                status = cm_hir_body_build_typed_expression(state,
                    qualified_plan.argument_types[index],
                    qualified_plan.arguments[index], &arguments[index]);
                if (status != CM_HIR_OK) return status;
            }
            if (!cm_size_add(state->call_storage_cursor,
                    (size_t)qualified_plan.argument_count, &next_cursor)
                || next_cursor > state->call_storage_words
                || (qualified_plan.argument_count != 0u
                    && state->call_storage == NULL)) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            call_words = qualified_plan.argument_count == 0u ? NULL
                : state->call_storage + state->call_storage_cursor;
            for (index = 0u; index < qualified_plan.argument_count;
                 ++index) {
                call_words[index] = arguments[index];
            }
            memset(&call, 0, sizeof(call));
            call.kind = CM_HIR_EXPR_QUALIFIED_CALL;
            call.owner_body = state->body_id;
            call.type = expected_type;
            call.span = cm_hir_body_ast_span(state->source,
                expression->span);
            call.data.qualified_call.syntax =
                CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD;
            call.data.qualified_call.requested_self_type =
                qualified_plan.requested_self_type;
            call.data.qualified_call.requested_trait =
                qualified_plan.requested_trait;
            call.data.qualified_call.declared_trait_callable =
                qualified_plan.declared_trait_callable;
            call.data.qualified_call.arguments =
                (CmHirExprId *)call_words;
            call.data.qualified_call.argument_count =
                qualified_plan.argument_count;
            call.data.qualified_call.receiver_argument =
                qualified_plan.receiver_argument;
            if (state->aggregate_storage_count == 0u
                && state->call_storage_cursor == 0u) {
                call.data.qualified_call.owned_storage =
                    (uint32_t *)state->transaction_storage;
            }
            state->call_storage_cursor = next_cursor;
            status = cm_hir_add_owned_qualified_call_expr(state->context,
                &call, out_expression);
            if (status == CM_HIR_OK
                && call.data.qualified_call.owned_storage != NULL) {
                state->transaction_storage_adopted = 1;
            }
            return status;
        }
        lower_status = cm_hir_body_resolve_scalar_call(state->context,
            state->owner_item, state->ast, expression,
            expected_type, &plan);
        if (lower_status != CM_HIR_BODY_LOWER_OK) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        for (index = 0u; index < plan.argument_count; ++index) {
            status = cm_hir_body_build_typed_expression(state,
                plan.argument_types[index], plan.arguments[index],
                &arguments[index]);
            if (status != CM_HIR_OK) return status;
        }
        if (!cm_size_add((size_t)plan.substitution_count,
                (size_t)plan.argument_count, &own_word_count)
            || !cm_size_add(state->call_storage_cursor, own_word_count,
                &next_cursor)
            || next_cursor > state->call_storage_words
            || state->call_storage == NULL) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        call_words = state->call_storage + state->call_storage_cursor;
        if (plan.substitution_count != 0u) {
            call_words[0] = expected_type;
        }
        for (index = 0u; index < plan.argument_count; ++index) {
            call_words[plan.substitution_count + index] = arguments[index];
        }
        memset(&call, 0, sizeof(call));
        call.kind = CM_HIR_EXPR_CALL;
        call.owner_body = state->body_id;
        call.type = expected_type;
        call.span = cm_hir_body_ast_span(state->source, expression->span);
        call.data.call.callee = plan.callee;
        call.data.call.type_substitutions = (CmHirTypeId *)call_words;
        call.data.call.type_substitution_count = plan.substitution_count;
        call.data.call.arguments = (CmHirExprId *)(call_words
            + plan.substitution_count);
        call.data.call.argument_count = plan.argument_count;
        if (state->aggregate_storage_count == 0u
            && state->call_storage_cursor == 0u) {
            call.data.call.owned_storage =
                (uint32_t *)state->transaction_storage;
        }
        state->call_storage_cursor = next_cursor;
        status = cm_hir_add_owned_call_expr(state->context, &call,
            out_expression);
        if (status == CM_HIR_OK
            && call.data.call.owned_storage != NULL) {
            state->transaction_storage_adopted = 1;
        }
        return status;
    }
    if (expression->kind == CM_AST_EXPR_METHOD_CALL) {
        const CmInternedString *method_name;
        CmHirExpr method_call;
        CmHirExprId receiver;
        CmHirBodyLowerStatus lower_status;
        CmHirTypeId receiver_type;
        size_t argument_start;
        size_t argument_next;
        size_t trait_start;
        size_t trait_next;
        size_t trait_count;
        uint32_t index;

        method_name = cm_ast_get_string(state->ast,
            expression->data.method_call.name);
        lower_status = cm_hir_body_infer_expression_type(state,
            expression->data.method_call.receiver, 1u, &receiver_type);
        if (method_name == NULL || method_name->len == 0u
            || lower_status != CM_HIR_BODY_LOWER_OK
            || !cm_size_add(state->method_argument_storage_cursor,
                (size_t)expression->data.method_call.argument_count,
                &argument_next)
            || argument_next > state->method_argument_storage_count
            || (expression->data.method_call.argument_count != 0u
                && state->method_argument_storage == NULL)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        lower_status = cm_hir_body_method_trait_scope(state, NULL, 0u,
            &trait_count);
        if (lower_status != CM_HIR_BODY_LOWER_OK
            || trait_count > (size_t)UINT32_MAX
            || !cm_size_add(state->method_trait_storage_cursor,
                trait_count, &trait_next)
            || trait_next > state->method_trait_storage_count
            || (trait_count != 0u
                && state->method_trait_storage == NULL)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        argument_start = state->method_argument_storage_cursor;
        trait_start = state->method_trait_storage_cursor;
        state->method_argument_storage_cursor = argument_next;
        state->method_trait_storage_cursor = trait_next;
        if (trait_count != 0u) {
            size_t actual_trait_count;

            lower_status = cm_hir_body_method_trait_scope(state,
                state->method_trait_storage + trait_start, trait_count,
                &actual_trait_count);
            if (lower_status != CM_HIR_BODY_LOWER_OK
                || actual_trait_count != trait_count) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
        }
        if (cm_hir_body_build_typed_expression(state, receiver_type,
                expression->data.method_call.receiver, &receiver)
                != CM_HIR_OK) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        for (index = 0u; index < expression->data.method_call.argument_count;
             ++index) {
            CmHirTypeId argument_type;
            CmHirStatus status;

            lower_status = cm_hir_body_infer_expression_type(state,
                expression->data.method_call.arguments[index], 1u,
                &argument_type);
            if (lower_status != CM_HIR_BODY_LOWER_OK) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            status = cm_hir_body_build_typed_expression(state,
                argument_type, expression->data.method_call.arguments[index],
                &state->method_argument_storage[argument_start + index]);
            if (status != CM_HIR_OK) return status;
        }
        memset(&method_call, 0, sizeof(method_call));
        method_call.kind = CM_HIR_EXPR_METHOD_CALL;
        method_call.owner_body = state->body_id;
        method_call.type = expected_type;
        method_call.span = cm_hir_body_ast_span(state->source,
            expression->span);
        method_call.data.method_call.syntax = CM_HIR_CALLABLE_DOT_METHOD;
        method_call.data.method_call.method_name = cm_interner_intern(
            &state->context->strings, method_name->bytes, method_name->len);
        method_call.data.method_call.receiver = receiver;
        method_call.data.method_call.arguments =
            expression->data.method_call.argument_count == 0u ? NULL
            : state->method_argument_storage + argument_start;
        method_call.data.method_call.argument_count =
            expression->data.method_call.argument_count;
        method_call.data.method_call.in_scope_traits = trait_count == 0u
            ? NULL : state->method_trait_storage + trait_start;
        method_call.data.method_call.in_scope_trait_count =
            (uint32_t)trait_count;
        return cm_hir_add_expr(state->context, &method_call,
            out_expression);
    }
    if (expression->kind == CM_AST_EXPR_STRUCT) {
        const CmHirItem *item;
        CmHirAggregateFieldValue *fields;
        CmHirExpr aggregate;
        CmHirBodyLowerStatus lower_status;
        CmHirStatus status;
        size_t slice_start;
        size_t next_cursor;
        uint32_t index;

        lower_status = cm_hir_body_resolve_struct_expression(state,
            expression, expected_type, &item);
        if (lower_status != CM_HIR_BODY_LOWER_OK || item == NULL
            || item->data.aggregate_item.field_count
                != expression->data.struct_expr.field_count
            || !cm_size_add(state->aggregate_storage_cursor,
                (size_t)expression->data.struct_expr.field_count,
                &next_cursor)
            || next_cursor > state->aggregate_storage_count) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        slice_start = state->aggregate_storage_cursor;
        fields = expression->data.struct_expr.field_count == 0u ? NULL
            : state->aggregate_storage + slice_start;
        state->aggregate_storage_cursor = next_cursor;
        for (index = 0u; index < expression->data.struct_expr.field_count;
             ++index) {
            const CmAstExprField *field;
            uint32_t field_index;

            field = &expression->data.struct_expr.fields[index];
            if (!cm_hir_body_find_struct_field(state->context, state->ast,
                    item, field->name, &field_index)) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            status = cm_hir_body_build_typed_expression(state,
                item->data.aggregate_item.fields[field_index].type,
                field->value, &fields[index].value);
            if (status != CM_HIR_OK) return status;
            fields[index].field_index = field_index;
            fields[index].span = cm_hir_body_ast_span(state->source,
                field->span);
        }
        memset(&aggregate, 0, sizeof(aggregate));
        aggregate.kind = CM_HIR_EXPR_AGGREGATE;
        aggregate.owner_body = state->body_id;
        aggregate.type = expected_type;
        aggregate.span = cm_hir_body_ast_span(state->source,
            expression->span);
        aggregate.data.aggregate.definition = item->definition;
        aggregate.data.aggregate.fields = fields;
        aggregate.data.aggregate.field_count =
            expression->data.struct_expr.field_count;
        if (fields != NULL && slice_start == 0u) {
            aggregate.data.aggregate.owned_storage =
                (CmHirAggregateFieldValue *)state->transaction_storage;
        }
        status = cm_hir_add_owned_aggregate_expr(state->context,
            &aggregate, out_expression);
        if (status == CM_HIR_OK
            && aggregate.data.aggregate.owned_storage != NULL) {
            state->transaction_storage_adopted = 1;
        }
        return status;
    }
    if (expression->kind == CM_AST_EXPR_FIELD) {
        const CmHirItem *item;
        CmHirExpr field;
        CmHirExprId base;
        CmHirTypeId base_type;
        CmHirStatus status;
        uint32_t field_index;

        if (cm_hir_body_resolve_field_expression(state, expression, 0u,
                &base_type, &item, &field_index)
                != CM_HIR_BODY_LOWER_OK
            || !cm_hir_body_type_equal(state->context,
                item->data.aggregate_item.fields[field_index].type,
                expected_type)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        status = cm_hir_body_build_typed_expression(state, base_type,
            expression->data.field.base, &base);
        if (status != CM_HIR_OK) return status;
        memset(&field, 0, sizeof(field));
        field.kind = CM_HIR_EXPR_FIELD;
        field.owner_body = state->body_id;
        field.type = item->data.aggregate_item.fields[field_index].type;
        field.span = cm_hir_body_ast_span(state->source,
            expression->span);
        field.data.field.base = base;
        field.data.field.definition = item->definition;
        field.data.field.field_index = field_index;
        return cm_hir_add_expr(state->context, &field, out_expression);
    }
    return CM_HIR_INVARIANT_VIOLATION;
}

static size_t cm_hir_body_max_resolve_path_segments(const CmAst *ast)
{
    size_t index;
    size_t maximum;

    maximum = 0u;
    for (index = 0u; index < ast->expressions.len; ++index) {
        const CmAstExpr *expression;
        const CmAstPath *path;

        expression = (const CmAstExpr *)cm_vec_at_const(
            &ast->expressions, index);
        path = NULL;
        if (expression != NULL && expression->kind == CM_AST_EXPR_STRUCT) {
            path = cm_ast_get_path(ast, expression->data.struct_expr.path);
        } else if (expression != NULL
            && expression->kind == CM_AST_EXPR_QUALIFIED_PATH
            && expression->data.qualified_path.trait_path
                != CM_AST_PATH_NONE) {
            path = cm_ast_get_path(ast,
                expression->data.qualified_path.trait_path);
        }
        if (path != NULL && path->segment_count > maximum) {
            maximum = path->segment_count;
        }
    }
    return maximum;
}

typedef union CmHirBodyStorageAlignment {
    void *pointer;
    size_t size_value;
    uint32_t word;
    CmHirAggregateFieldValue aggregate_field;
    CmResolvePathSegmentView path_segment;
} CmHirBodyStorageAlignment;

static int cm_hir_body_align_storage_size(size_t size, size_t *out_size)
{
    size_t alignment;
    size_t remainder;

    alignment = sizeof(CmHirBodyStorageAlignment);
    remainder = size % alignment;
    if (remainder == 0u) {
        *out_size = size;
        return 1;
    }
    return cm_size_add(size, alignment - remainder, out_size);
}

CmHirBodyLowerResult cm_hir_lower_body(CmHirContext *context,
    CmHirBodyId body_id, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules)
{
    const CmHirBody *body;
    CmHirBody *mutable_body;
    const CmHirDefinition *owner_definition;
    const CmHirItem *owner_item;
    const CmHirType *expected_type;
    const CmAst *ast;
    const CmAstExpr *block_ast;
    const CmAstExpr *tail_ast;
    CmAstExprId tail_ast_id;
    CmHirBodyLowerResult result;
    CmHirBodyLowerStatus literal_status;
    CmHirExpr block;
    CmHirExprId tail_id;
    CmHirExprId block_id;
    CmHirBodyExpressionCounts expression_counts;
    CmHirBodyBuildState build_state;
    CmHirBodyLetPlan *let_plans;
    CmHirStatement *hir_statements;
    CmHirLocal *old_locals;
    uint32_t old_local_count;
    CmArenaMark storage_mark;
    CmInternerMark strings_mark;
    CmHirStatus hir_status;
    CmHirModuleMapStatus map_status;
    CmModuleId graph_module;
    CmResolveModuleInfo module_info;
    size_t expression_count;
    size_t type_count;
    size_t reserved_type_count;
    size_t reserved_expression_count;
    size_t expression_add_count;
    size_t call_storage_bytes;
    size_t method_argument_storage_bytes;
    size_t method_trait_storage_bytes;
    size_t aggregate_storage_bytes;
    size_t payload_storage_bytes;
    size_t transaction_storage_bytes;
    size_t path_storage_bytes;
    size_t path_storage_offset;
    size_t max_resolve_path_segments;
    CmSourceId source;
    uint32_t statement_count;
    uint32_t statement_index;
    size_t inferred_type_add_count;
    int transaction_marked;
    int add_bool_type;
    int value_body;

    memset(&result, 0, sizeof(result));
    if (context == NULL || graph == NULL || imports == NULL || modules == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE
        || body_id == CM_HIR_BODY_NONE) {
        return cm_hir_body_result(CM_HIR_BODY_LOWER_INVALID_ARGUMENT,
            body_id, (CmSpan){ 0u, 0u, 0u });
    }
    body = cm_hir_get_body(context, body_id);
    if (!cm_import_resolver_matches_graph(imports, graph)
        || cm_import_resolver_revision(imports) != revision) {
        return cm_hir_body_result(CM_HIR_BODY_LOWER_SOURCE_MISMATCH,
            body_id, body == NULL ? (CmSpan){ 0u, 0u, 0u } : body->span);
    }
    if (body == NULL || body->state != CM_HIR_BODY_UNLOWERED
        || body->root_expression != CM_HIR_EXPR_NONE) {
        return cm_hir_body_result(CM_HIR_BODY_LOWER_INVALID_BODY,
            body_id, body == NULL ? (CmSpan){ 0u, 0u, 0u } : body->span);
    }
    owner_definition = cm_hir_lookup_definition(context, body->owner);
    owner_item = owner_definition == NULL
        || owner_definition->kind != CM_HIR_DEFINITION_ITEM
        || owner_definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(context,
            owner_definition->entity.item_id);
    if (owner_item == NULL
        || !cm_hir_def_id_equal(owner_item->definition, body->owner)) {
        return cm_hir_body_result(CM_HIR_BODY_LOWER_INVALID_BODY,
            body_id, body->span);
    }
    value_body = owner_item->kind == CM_HIR_ITEM_CONST
        || owner_item->kind == CM_HIR_ITEM_STATIC;
    if ((!value_body
            && (owner_item->kind != CM_HIR_ITEM_FUNCTION
                || owner_item->data.function_item.body != body_id
                || cm_hir_body_function_owner_kind(context, owner_item)
                    == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED))
        || (value_body
            && (owner_item->data.value_item.body != body_id
                || cm_hir_body_value_owner_kind(context, owner_item)
                    == CM_HIR_BODY_VALUE_OWNER_UNSUPPORTED))) {
        return cm_hir_body_result(CM_HIR_BODY_LOWER_UNSUPPORTED_BODY,
            body_id, body->span);
    }
    graph_module = CM_MODULE_NONE;
    map_status = cm_hir_module_map_lookup_module(modules, graph, revision,
        context, owner_item->owner_module, &graph_module);
    if (map_status != CM_HIR_MODULE_MAP_OK
        || !cm_module_graph_get_module(graph, graph_module, &module_info)
        || !cm_module_graph_borrow_ast(graph, graph_module, &ast)
        || ast == NULL || module_info.source != body->source
        || body->span.source != body->source) {
        return cm_hir_body_result(CM_HIR_BODY_LOWER_SOURCE_MISMATCH,
            body_id, body->span);
    }
    source = module_info.source;
    expected_type = cm_hir_get_type(context, body->expected_type);
    if (expected_type == NULL) {
        return cm_hir_body_result(CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE,
            body_id, body->span);
    }
    block_ast = cm_ast_get_expr(ast,
        (CmAstExprId)body->source_expression_id);
    if (block_ast == NULL || block_ast->span.start > block_ast->span.end
        || block_ast->span.start < body->span.start
        || block_ast->span.end > body->span.end) {
        return cm_hir_body_result(CM_HIR_BODY_LOWER_INVALID_BODY,
            body_id, body->span);
    }
    result = cm_hir_body_result(CM_HIR_BODY_LOWER_UNSUPPORTED_BODY,
        body_id, cm_hir_body_ast_span(source, block_ast->span));
    if (value_body) {
        statement_count = 0u;
        tail_ast_id = (CmAstExprId)body->source_expression_id;
        if (block_ast->kind == CM_AST_EXPR_BLOCK) {
            if (block_ast->data.block.is_unsafe
                || block_ast->data.block.is_const
                || (block_ast->attribute_count == 0u
                    && block_ast->attributes != NULL)
                || block_ast->attribute_count != 0u
                || block_ast->data.block.statement_count != 0u
                || block_ast->data.block.statements != NULL
                || block_ast->data.block.tail == CM_AST_EXPR_NONE) {
                return result;
            }
            tail_ast_id = block_ast->data.block.tail;
        }
    } else {
        if (block_ast->kind != CM_AST_EXPR_BLOCK
            || block_ast->data.block.tail == CM_AST_EXPR_NONE) {
            return result;
        }
        statement_count = block_ast->data.block.statement_count;
        tail_ast_id = block_ast->data.block.tail;
    }
    let_plans = NULL;
    hir_statements = NULL;
    transaction_marked = 0;
    old_locals = NULL;
    old_local_count = 0u;
    if (statement_count != 0u
        && (block_ast->data.block.statements == NULL
            || statement_count > UINT32_MAX - body->local_count)) {
        result.status = CM_HIR_BODY_LOWER_INVALID_BODY;
        return result;
    }
    tail_ast = cm_ast_get_expr(ast, tail_ast_id);
    if (tail_ast == NULL || tail_ast->span.start > tail_ast->span.end
        || tail_ast->span.start < block_ast->span.start
        || tail_ast->span.end > block_ast->span.end) {
        result.status = CM_HIR_BODY_LOWER_INVALID_BODY;
        return result;
    }
    result.span = cm_hir_body_ast_span(source, tail_ast->span);

    expression_count = context->expressions.len;
    type_count = context->types.len;
    memset(&expression_counts, 0, sizeof(expression_counts));
    memset(&build_state, 0, sizeof(build_state));
    build_state.context = context;
    build_state.body_id = body_id;
    build_state.body = body;
    build_state.owner_item = owner_item;
    build_state.trait_default_closed_slice =
        cm_hir_body_function_owner_kind(context, owner_item)
            == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT;
    build_state.ast = ast;
    build_state.graph = graph;
    build_state.imports = imports;
    build_state.modules = modules;
    build_state.revision = revision;
    build_state.graph_module = graph_module;
    build_state.source = source;
    build_state.base_local_count = body->local_count;
    build_state.allowed_if_expression = tail_ast->kind == CM_AST_EXPR_IF
        ? tail_ast_id : CM_AST_EXPR_NONE;
    max_resolve_path_segments = cm_hir_body_max_resolve_path_segments(ast);
    if (!cm_size_mul(max_resolve_path_segments,
            sizeof(CmResolvePathSegmentView), &path_storage_bytes)) {
        result.status = CM_HIR_BODY_LOWER_HIR_FAILURE;
        result.hir_status = CM_HIR_ID_EXHAUSTED;
        return result;
    }
    if (path_storage_bytes != 0u) {
        build_state.path_segments = (CmResolvePathSegmentView *)cm_alloc(
            path_storage_bytes);
        build_state.path_segment_capacity = max_resolve_path_segments;
    }
    if (statement_count != 0u) {
        let_plans = (CmHirBodyLetPlan *)cm_alloc_zeroed(statement_count,
            sizeof(CmHirBodyLetPlan));
        build_state.let_plans = let_plans;
        literal_status = cm_hir_body_prepare_let_inference(&build_state,
            block_ast, let_plans);
        if (literal_status == CM_HIR_BODY_LOWER_OK) {
            literal_status = cm_hir_body_preflight_let_block(&build_state,
                block_ast, let_plans, &expression_counts);
        }
    } else {
        literal_status = cm_hir_body_preflight_typed_expression(&build_state,
            body->expected_type, tail_ast_id,
            block_ast->span, 0u, &expression_counts);
    }
    if (literal_status != CM_HIR_BODY_LOWER_OK) {
        cm_free(build_state.path_segments);
        build_state.path_segments = NULL;
        cm_free(let_plans);
        result.status = literal_status;
        return result;
    }
    if (statement_count != 0u && expression_counts.needs_bool_type) {
        cm_free(build_state.path_segments);
        build_state.path_segments = NULL;
        cm_free(let_plans);
        result.status = CM_HIR_BODY_LOWER_UNSUPPORTED_BODY;
        return result;
    }
    cm_free(build_state.path_segments);
    build_state.path_segments = NULL;
    build_state.bool_type = cm_hir_body_find_bool_type(context);
    add_bool_type = expression_counts.needs_bool_type
        && build_state.bool_type == CM_HIR_TYPE_NONE;
    inferred_type_add_count = 0u;
    for (statement_index = 0u; statement_index < statement_count;
         ++statement_index) {
        uint32_t prior;
        int already_needed;

        if (!let_plans[statement_index].has_resolved_integer_kind
            || let_plans[statement_index].resolved_type
                != CM_HIR_TYPE_NONE) {
            continue;
        }
        already_needed = 0;
        for (prior = 0u; prior < statement_index; ++prior) {
            if (let_plans[prior].has_resolved_integer_kind
                && let_plans[prior].resolved_type == CM_HIR_TYPE_NONE
                && let_plans[prior].resolved_integer_kind
                    == let_plans[statement_index].resolved_integer_kind) {
                already_needed = 1;
                break;
            }
        }
        if (!already_needed) ++inferred_type_add_count;
    }
    reserved_type_count = context->types.len;
    if (!cm_size_add(context->types.len, inferred_type_add_count,
                &reserved_type_count)
        || (add_bool_type && !cm_size_add(reserved_type_count, 1u,
                &reserved_type_count))
        || reserved_type_count > (size_t)UINT32_MAX) {
        cm_free(let_plans);
        result.status = CM_HIR_BODY_LOWER_HIR_FAILURE;
        result.hir_status = CM_HIR_ID_EXHAUSTED;
        return result;
    }
    if (reserved_type_count != context->types.len) {
        /* Reserve before any semantic mutation; the later append cannot OOM. */
        cm_vec_reserve(&context->types, reserved_type_count);
    }
    if (!cm_size_add(expression_counts.expression_count, 1u,
            &expression_add_count)
        || !cm_size_add(expression_count, expression_add_count,
            &reserved_expression_count)
        || !cm_size_mul(expression_counts.call_word_count,
            sizeof(uint32_t), &call_storage_bytes)
        || !cm_size_mul(expression_counts.method_argument_count,
            sizeof(CmHirExprId), &method_argument_storage_bytes)
        || !cm_size_mul(expression_counts.method_trait_count,
            sizeof(CmHirDefId), &method_trait_storage_bytes)
        || !cm_size_mul(expression_counts.aggregate_field_count,
            sizeof(CmHirAggregateFieldValue), &aggregate_storage_bytes)
        || !cm_size_add(aggregate_storage_bytes, call_storage_bytes,
            &payload_storage_bytes)
        || !cm_hir_body_align_storage_size(payload_storage_bytes,
            &path_storage_offset)
        || !cm_size_add(path_storage_offset, path_storage_bytes,
            &transaction_storage_bytes)) {
        cm_free(let_plans);
        result.status = CM_HIR_BODY_LOWER_HIR_FAILURE;
        result.hir_status = CM_HIR_ID_EXHAUSTED;
        return result;
    }
    /* Any recoverable OOM occurs before the first semantic mutation. */
    cm_vec_reserve(&context->expressions, reserved_expression_count);
    if (statement_count != 0u) {
        hir_statements = (CmHirStatement *)cm_alloc_zeroed(statement_count,
            sizeof(CmHirStatement));
    }
    build_state.call_storage_words = expression_counts.call_word_count;
    build_state.method_argument_storage_count =
        expression_counts.method_argument_count;
    build_state.method_trait_storage_count =
        expression_counts.method_trait_count;
    build_state.aggregate_storage_count =
        expression_counts.aggregate_field_count;
    if (method_argument_storage_bytes != 0u) {
        build_state.method_argument_storage =
            (CmHirExprId *)cm_alloc(method_argument_storage_bytes);
    }
    if (method_trait_storage_bytes != 0u) {
        build_state.method_trait_storage =
            (CmHirDefId *)cm_alloc(method_trait_storage_bytes);
    }
    if (transaction_storage_bytes != 0u) {
        unsigned char *storage;

        build_state.transaction_storage = cm_alloc(
            transaction_storage_bytes);
        storage = (unsigned char *)build_state.transaction_storage;
        if (aggregate_storage_bytes != 0u) {
            build_state.aggregate_storage =
                (CmHirAggregateFieldValue *)storage;
            if (call_storage_bytes != 0u) {
                build_state.call_storage = (uint32_t *)(storage
                    + aggregate_storage_bytes);
            }
        } else {
            build_state.call_storage = (uint32_t *)storage;
        }
        if (path_storage_bytes != 0u) {
            build_state.path_segments = (CmResolvePathSegmentView *)(storage
                + path_storage_offset);
        }
    }
    hir_status = CM_HIR_OK;
    tail_id = CM_HIR_EXPR_NONE;
    mutable_body = (CmHirBody *)cm_vec_at(&context->bodies,
        (size_t)body_id - 1u);
    if (mutable_body == NULL) {
        hir_status = CM_HIR_INVALID_ID;
        goto fail_body_transaction;
    }
    if (statement_count != 0u || add_bool_type
        || expression_counts.method_call_count != 0u
        || inferred_type_add_count != 0u) {
        storage_mark = cm_arena_mark(&context->storage);
        strings_mark = cm_interner_mark(&context->strings);
        transaction_marked = 1;
    }
    if (add_bool_type) {
        CmHirType bool_type;

        memset(&bool_type, 0, sizeof(bool_type));
        bool_type.kind = CM_HIR_TYPE_BOOL_KIND;
        bool_type.span = result.span;
        hir_status = cm_hir_add_type(context, &bool_type,
            &build_state.bool_type);
        if (hir_status != CM_HIR_OK) goto fail_body_transaction;
    }
    for (statement_index = 0u; statement_index < statement_count;
         ++statement_index) {
        CmHirTypeId concrete_type;
        uint32_t update_index;

        if (!let_plans[statement_index].has_resolved_integer_kind
            || let_plans[statement_index].resolved_type
                != CM_HIR_TYPE_NONE) {
            continue;
        }
        concrete_type = cm_hir_body_find_integer_type(context,
            let_plans[statement_index].resolved_integer_kind);
        if (concrete_type == CM_HIR_TYPE_NONE) {
            hir_status = cm_hir_body_add_integer_type(context,
                let_plans[statement_index].resolved_integer_kind,
                cm_hir_body_ast_span(source,
                    let_plans[statement_index].binding_span),
                &concrete_type);
            if (hir_status != CM_HIR_OK) goto fail_body_transaction;
        }
        for (update_index = statement_index;
             update_index < statement_count; ++update_index) {
            if (let_plans[update_index].has_resolved_integer_kind
                && let_plans[update_index].resolved_integer_kind
                    == let_plans[statement_index].resolved_integer_kind) {
                let_plans[update_index].resolved_type = concrete_type;
            }
        }
    }
    if (expression_counts.needs_bool_type
        && !cm_hir_body_is_bool(context, build_state.bool_type)) {
        hir_status = CM_HIR_INVARIANT_VIOLATION;
        goto fail_body_transaction;
    }
    if (statement_count != 0u) {
        CmHirLocal *expanded_locals;
        uint32_t expanded_local_count;

        old_locals = mutable_body->locals;
        old_local_count = mutable_body->local_count;
        expanded_local_count = old_local_count + statement_count;
        expanded_locals = (CmHirLocal *)cm_arena_alloc_zeroed(
            &context->storage, expanded_local_count, sizeof(CmHirLocal),
            16u);
        if (old_local_count != 0u) {
            memcpy(expanded_locals, old_locals,
                (size_t)old_local_count * sizeof(CmHirLocal));
        }
        for (statement_index = 0u; statement_index < statement_count;
             ++statement_index) {
            const CmInternedString *name;
            CmHirLocal *local;

            name = cm_ast_get_string(ast,
                let_plans[statement_index].ast_name);
            if (name == NULL) {
                hir_status = CM_HIR_INVARIANT_VIOLATION;
                goto fail_body_transaction;
            }
            local = &expanded_locals[old_local_count + statement_index];
            local->name = cm_interner_intern(&context->strings,
                name->bytes, name->len);
            local->type = let_plans[statement_index].resolved_type;
            local->mutability = CM_HIR_IMMUTABLE;
            local->span = cm_hir_body_ast_span(source,
                let_plans[statement_index].binding_span);
            local->parameter_index = CM_HIR_PARAMETER_INDEX_NONE;
        }
        mutable_body->locals = expanded_locals;
        mutable_body->local_count = expanded_local_count;
        build_state.body = mutable_body;
        for (statement_index = 0u; statement_index < statement_count;
             ++statement_index) {
            build_state.visible_let_count = statement_index;
            hir_status = cm_hir_body_build_typed_expression(&build_state,
                let_plans[statement_index].resolved_type,
                let_plans[statement_index].initializer,
                &hir_statements[statement_index].data.let_statement
                    .initializer);
            if (hir_status != CM_HIR_OK) goto fail_body_transaction;
            hir_statements[statement_index].kind = CM_HIR_STATEMENT_LET;
            hir_statements[statement_index].span = cm_hir_body_ast_span(
                source, let_plans[statement_index].statement_span);
            hir_statements[statement_index].data.let_statement.local_index =
                old_local_count + statement_index;
        }
        build_state.visible_let_count = statement_count;
        hir_status = cm_hir_body_build_typed_expression(&build_state,
            body->expected_type, tail_ast_id, &tail_id);
    } else {
        hir_status = cm_hir_body_build_typed_expression(&build_state,
            body->expected_type, tail_ast_id, &tail_id);
    }
    if (hir_status == CM_HIR_OK
        && (build_state.call_storage_cursor
                != build_state.call_storage_words
            || build_state.aggregate_storage_cursor
                != build_state.aggregate_storage_count
            || build_state.method_argument_storage_cursor
                != build_state.method_argument_storage_count
            || build_state.method_trait_storage_cursor
                != build_state.method_trait_storage_count
            || (payload_storage_bytes != 0u
                && !build_state.transaction_storage_adopted))) {
        hir_status = CM_HIR_INVARIANT_VIOLATION;
    }
    if (hir_status == CM_HIR_OK) {
        memset(&block, 0, sizeof(block));
        block.kind = CM_HIR_EXPR_BLOCK;
        block.owner_body = body_id;
        block.type = body->expected_type;
        block.span = cm_hir_body_ast_span(source, block_ast->span);
        block.data.block.statements = hir_statements;
        block.data.block.statement_count = statement_count;
        block.data.block.tail_expression = tail_id;
        hir_status = cm_hir_add_expr(context, &block, &block_id);
    }
    if (hir_status != CM_HIR_OK) goto fail_body_transaction;

    hir_status = cm_hir_set_body_root_expression(context, body_id, block_id);
    if (hir_status != CM_HIR_OK) goto fail_body_transaction;
    if (transaction_marked) {
        cm_interner_discard_mark(&context->strings, strings_mark);
        cm_arena_discard_mark(&context->storage, storage_mark);
        transaction_marked = 0;
    }
    if (!build_state.transaction_storage_adopted) {
        cm_free(build_state.transaction_storage);
        build_state.transaction_storage = NULL;
    }
    cm_free(hir_statements);
    cm_free(let_plans);
    cm_free(build_state.method_argument_storage);
    cm_free(build_state.method_trait_storage);
    result.status = CM_HIR_BODY_LOWER_OK;
    result.root_expression = block_id;
    result.span = block.span;
    return result;

fail_body_transaction:
    if (!build_state.transaction_storage_adopted) {
        cm_free(build_state.transaction_storage);
        build_state.transaction_storage = NULL;
    }
    if (transaction_marked) {
        mutable_body->locals = old_locals;
        mutable_body->local_count = old_local_count;
        cm_interner_rewind(&context->strings, strings_mark);
        cm_interner_discard_mark(&context->strings, strings_mark);
        cm_arena_rewind(&context->storage, storage_mark);
        cm_arena_discard_mark(&context->storage, storage_mark);
        transaction_marked = 0;
    }
    cm_hir_body_rollback_expressions(context, expression_count);
    if (context->types.len > type_count) {
        cm_vec_resize(&context->types, type_count);
    }
    cm_free(hir_statements);
    cm_free(let_plans);
    cm_free(build_state.method_argument_storage);
    cm_free(build_state.method_trait_storage);
    result.status = CM_HIR_BODY_LOWER_HIR_FAILURE;
    result.hir_status = hir_status;
    result.root_expression = CM_HIR_EXPR_NONE;
    return result;
}

static CmHirLocalBodiesResult cm_hir_local_bodies_result(
    CmHirLocalBodiesStatus status)
{
    CmHirLocalBodiesResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.item = CM_HIR_ITEM_NONE;
    result.owner = cm_hir_def_id_none();
    result.body = CM_HIR_BODY_NONE;
    result.body_result = cm_hir_body_result(
        CM_HIR_BODY_LOWER_INVALID_ARGUMENT, CM_HIR_BODY_NONE,
        (CmSpan){ 0u, 0u, 0u });
    result.hir_status = CM_HIR_OK;
    return result;
}

static CmHirBodyId cm_hir_local_item_body(const CmHirItem *item)
{
    if (item->kind == CM_HIR_ITEM_FUNCTION)
        return item->data.function_item.body;
    if (item->kind == CM_HIR_ITEM_CONST || item->kind == CM_HIR_ITEM_STATIC)
        return item->data.value_item.body;
    return CM_HIR_BODY_NONE;
}

static int cm_hir_local_item_identity_valid(const CmHirContext *context,
    CmHirCrateId local_crate, CmHirItemId item_id, const CmHirItem *item)
{
    const CmHirDefinition *definition;
    const CmHirModule *module;

    if (item == NULL || item->definition.crate_id != local_crate) return 0;
    definition = cm_hir_lookup_definition(context, item->definition);
    module = cm_hir_get_module(context, item->owner_module);
    return definition != NULL
        && cm_hir_def_id_equal(definition->id, item->definition)
        && definition->kind == CM_HIR_DEFINITION_ITEM
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->entity.item_id == item_id
        && module != NULL && module->crate_id == local_crate;
}

static int cm_hir_local_body_shape_valid(const CmHirContext *context,
    CmHirBodyId body_id, const CmHirBody *body)
{
    const CmHirExpr *root;

    if (body == NULL || body->source == 0u
        || body->source_expression_id == 0u
        || body->span.source != body->source
        || body->span.start > body->span.end
        || cm_hir_get_type(context, body->expected_type) == NULL
        || (body->local_count != 0u && body->locals == NULL)
        || body->error_reason != CM_INTERN_ID_NONE) return 0;
    if (body->state == CM_HIR_BODY_UNLOWERED)
        return body->root_expression == CM_HIR_EXPR_NONE;
    if (body->state != CM_HIR_BODY_TYPED
        || body->root_expression == CM_HIR_EXPR_NONE) return 0;
    root = cm_hir_get_expr(context, body->root_expression);
    return root != NULL && root->owner_body == body_id
        && root->type == body->expected_type
        && root->span.source == body->source
        && root->span.start >= body->span.start
        && root->span.end <= body->span.end;
}

static CmHirStatus cm_hir_local_bodies_rollback(CmHirContext *context,
    CmHirContextMark *mark, const CmHirBody *journal, size_t body_count)
{
    if (context->bodies.len < body_count
        || (body_count != 0u && journal == NULL))
        return CM_HIR_INVARIANT_VIOLATION;
    if (body_count != 0u)
        memcpy(context->bodies.data, journal,
            body_count * sizeof(*journal));
    return cm_hir_context_rewind(context, mark);
}

CmHirLocalBodiesResult cm_hir_lower_local_bodies(CmHirContext *context,
    CmHirCrateId local_crate, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules)
{
    CmHirLocalBodiesResult result;
    const CmHirCrate *crate_value;
    CmModuleId graph_root;
    CmHirModuleMapStatus map_status;
    CmHirItemId *body_owners;
    CmHirBody *journal;
    CmHirContextMark mark;
    size_t body_bytes;
    size_t item_index;
    size_t body_index;
    size_t captured_body_count;
    CmHirStatus hir_status;
    int mark_active;

    result = cm_hir_local_bodies_result(
        CM_HIR_LOCAL_BODIES_INVALID_ARGUMENT);
    if (context == NULL || graph == NULL || imports == NULL || modules == NULL
        || local_crate == CM_HIR_CRATE_NONE
        || revision == CM_MODULE_GRAPH_REVISION_NONE) return result;
    crate_value = cm_hir_get_crate(context, local_crate);
    if (crate_value == NULL || crate_value->root_module == CM_HIR_MODULE_NONE) {
        result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
        return result;
    }
    if (!cm_import_resolver_matches_graph(imports, graph)
        || cm_import_resolver_revision(imports) != revision) {
        result.status = CM_HIR_LOCAL_BODIES_SOURCE_MISMATCH;
        return result;
    }
    map_status = cm_hir_module_map_lookup_module(modules, graph, revision,
        context, crate_value->root_module, &graph_root);
    if (map_status != CM_HIR_MODULE_MAP_OK || graph_root == CM_MODULE_NONE) {
        result.status = CM_HIR_LOCAL_BODIES_SOURCE_MISMATCH;
        return result;
    }
    captured_body_count = context->bodies.len;
    if (context->items.len > (size_t)UINT32_MAX
        || captured_body_count > (size_t)UINT32_MAX
        || !cm_size_mul(captured_body_count, sizeof(*journal), &body_bytes)) {
        result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
        result.hir_status = CM_HIR_ID_EXHAUSTED;
        return result;
    }
    body_owners = (CmHirItemId *)cm_alloc_zeroed(
        captured_body_count == 0u ? 1u : captured_body_count,
        sizeof(*body_owners));
    journal = NULL;
    mark_active = 0;

    for (item_index = 0u; item_index < context->items.len; ++item_index) {
        const CmHirItem *item;
        const CmHirModule *owner_module;
        CmHirItemId item_id;
        CmHirBodyId body_id;
        const CmHirBody *body;

        item_id = (CmHirItemId)(item_index + 1u);
        item = cm_hir_get_item(context, item_id);
        if (item == NULL) {
            result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
            result.item = item_id;
            goto finish;
        }
        owner_module = cm_hir_get_module(context, item->owner_module);
        if (item->definition.crate_id != local_crate) {
            if (owner_module != NULL && owner_module->crate_id == local_crate) {
                result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
                result.item = item_id;
                result.owner = item->definition;
                goto finish;
            }
            continue;
        }
        if (!cm_hir_local_item_identity_valid(context, local_crate,
                item_id, item)) {
            result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
            result.item = item_id;
            result.owner = item->definition;
            goto finish;
        }
        body_id = cm_hir_local_item_body(item);
        if (body_id == CM_HIR_BODY_NONE) continue;
        result.item = item_id;
        result.owner = item->definition;
        result.body = body_id;
        if ((item->kind == CM_HIR_ITEM_FUNCTION
                && cm_hir_body_function_owner_kind(context, item)
                    == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED)
            || ((item->kind == CM_HIR_ITEM_CONST
                    || item->kind == CM_HIR_ITEM_STATIC)
                && cm_hir_body_value_owner_kind(context, item)
                    == CM_HIR_BODY_VALUE_OWNER_UNSUPPORTED)) {
            result.status = CM_HIR_LOCAL_BODIES_UNSUPPORTED_OWNER;
            goto finish;
        }
        if ((size_t)body_id > captured_body_count
            || body_owners[(size_t)body_id - 1u] != CM_HIR_ITEM_NONE) {
            result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
            goto finish;
        }
        body = cm_hir_get_body(context, body_id);
        if (body == NULL
            || !cm_hir_def_id_equal(body->owner, item->definition)
            || !cm_hir_local_body_shape_valid(context, body_id, body)) {
            result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
            goto finish;
        }
        body_owners[(size_t)body_id - 1u] = item_id;
        result = cm_hir_local_bodies_result(CM_HIR_LOCAL_BODIES_OK);
    }
    for (body_index = 0u; body_index < captured_body_count; ++body_index) {
        const CmHirBody *body;

        body = cm_hir_get_body(context, (CmHirBodyId)(body_index + 1u));
        if (body == NULL) {
            result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
            result.body = (CmHirBodyId)(body_index + 1u);
            goto finish;
        }
        if (body->owner.crate_id == local_crate
            && body_owners[body_index] == CM_HIR_ITEM_NONE) {
            result.status = CM_HIR_LOCAL_BODIES_INVALID_HIR;
            result.owner = body->owner;
            result.body = (CmHirBodyId)(body_index + 1u);
            goto finish;
        }
    }

    journal = body_bytes == 0u ? NULL : (CmHirBody *)cm_alloc(body_bytes);
    if (body_bytes != 0u) memcpy(journal, context->bodies.data, body_bytes);
    memset(&mark, 0, sizeof(mark));
    hir_status = cm_hir_context_mark(context, &mark);
    if (hir_status != CM_HIR_OK) {
        result.status = CM_HIR_LOCAL_BODIES_HIR_FAILURE;
        result.hir_status = hir_status;
        goto finish;
    }
    mark_active = 1;
    for (item_index = 0u; item_index < context->items.len; ++item_index) {
        const CmHirItem *item;
        const CmHirBody *body;
        CmHirItemId item_id;
        CmHirBodyId body_id;

        item_id = (CmHirItemId)(item_index + 1u);
        item = cm_hir_get_item(context, item_id);
        if (item == NULL || item->definition.crate_id != local_crate
            || (item->kind != CM_HIR_ITEM_FUNCTION
                && item->kind != CM_HIR_ITEM_CONST
                && item->kind != CM_HIR_ITEM_STATIC)) continue;
        body_id = cm_hir_local_item_body(item);
        if (body_id == CM_HIR_BODY_NONE) continue;
        body = cm_hir_get_body(context, body_id);
        if (body != NULL && body->state == CM_HIR_BODY_TYPED) continue;
        result.item = item_id;
        result.owner = item->definition;
        result.body = body_id;
        result.body_result = cm_hir_lower_body(context, body_id, graph,
            revision, imports, modules);
        if (result.body_result.status != CM_HIR_BODY_LOWER_OK) {
            result.status = CM_HIR_LOCAL_BODIES_BODY_FAILURE;
            goto rollback;
        }
    }
    hir_status = cm_hir_context_commit(context, &mark);
    if (hir_status != CM_HIR_OK) {
        result.status = CM_HIR_LOCAL_BODIES_HIR_FAILURE;
        result.hir_status = hir_status;
        goto rollback;
    }
    mark_active = 0;
    result = cm_hir_local_bodies_result(CM_HIR_LOCAL_BODIES_OK);
    goto finish;

rollback:
    if (mark_active) {
        hir_status = cm_hir_local_bodies_rollback(context, &mark, journal,
            captured_body_count);
        mark_active = 0;
        if (hir_status != CM_HIR_OK) {
            result.status = CM_HIR_LOCAL_BODIES_HIR_FAILURE;
            result.hir_status = hir_status;
        }
    }
finish:
    cm_free(journal);
    cm_free(body_owners);
    return result;
}

const char *cm_hir_body_lower_status_name(CmHirBodyLowerStatus status)
{
    switch (status) {
    case CM_HIR_BODY_LOWER_OK:
        return "ok";
    case CM_HIR_BODY_LOWER_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_HIR_BODY_LOWER_INVALID_BODY:
        return "invalid body";
    case CM_HIR_BODY_LOWER_SOURCE_MISMATCH:
        return "source mismatch";
    case CM_HIR_BODY_LOWER_UNSUPPORTED_BODY:
        return "unsupported body";
    case CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE:
        return "unsupported type";
    case CM_HIR_BODY_LOWER_UNRESOLVED_PATH:
        return "unresolved path";
    case CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION:
        return "invalid substitution";
    case CM_HIR_BODY_LOWER_TYPE_MISMATCH:
        return "type mismatch";
    case CM_HIR_BODY_LOWER_INVALID_LITERAL:
        return "invalid literal";
    case CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE:
        return "literal out of range";
    case CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR:
        return "unsupported operator";
    case CM_HIR_BODY_LOWER_HIR_FAILURE:
        return "HIR failure";
    }
    return "unknown body lowering status";
}

const char *cm_hir_local_bodies_status_name(CmHirLocalBodiesStatus status)
{
    switch (status) {
    case CM_HIR_LOCAL_BODIES_OK: return "ok";
    case CM_HIR_LOCAL_BODIES_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_LOCAL_BODIES_SOURCE_MISMATCH: return "source mismatch";
    case CM_HIR_LOCAL_BODIES_INVALID_HIR: return "invalid HIR";
    case CM_HIR_LOCAL_BODIES_UNSUPPORTED_OWNER:
        return "unsupported body owner";
    case CM_HIR_LOCAL_BODIES_BODY_FAILURE: return "body lowering failure";
    case CM_HIR_LOCAL_BODIES_HIR_FAILURE: return "HIR failure";
    }
    return "unknown local body lowering status";
}
