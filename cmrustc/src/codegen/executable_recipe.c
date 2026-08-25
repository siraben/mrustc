#include "cm/codegen/executable_recipe.h"

#include "cm/hir/semantic_results.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct CmExecutableRecipePlan {
    const CmInternedString *name;
    const char *c_type;
} CmExecutableRecipePlan;

static int cm_recipe_text_equal(const CmHirContext *hir, CmInternId id,
    const char *expected)
{
    const CmInternedString *text;
    size_t length;

    if (hir == NULL || expected == NULL) return 0;
    text = cm_interner_get(&hir->strings, id);
    length = strlen(expected);
    return text != NULL && text->len == length
        && memcmp(text->bytes, expected, length) == 0;
}

static int cm_recipe_target_supported(const CmTargetDesc *target)
{
    if (target == NULL || target->triple == NULL
        || target->architecture == NULL
        || target->operating_system == NULL
        || target->environment == NULL || target->endian != CM_ENDIAN_LITTLE
        || strcmp(target->operating_system, "linux") != 0) return 0;
    if (strcmp(target->triple, "i386-unknown-linux-musl") == 0
        || strcmp(target->triple, "i686-unknown-linux-musl") == 0) {
        return strcmp(target->architecture, "x86") == 0
            && strcmp(target->environment, "musl") == 0
            && target->pointer_bits == 32u;
    }
    if (strcmp(target->triple, "x86_64-unknown-linux-gnu") == 0) {
        return strcmp(target->architecture, "x86_64") == 0
            && strcmp(target->environment, "gnu") == 0
            && target->pointer_bits == 64u;
    }
    if (strcmp(target->triple, "x86_64-unknown-linux-musl") == 0) {
        return strcmp(target->architecture, "x86_64") == 0
            && strcmp(target->environment, "musl") == 0
            && target->pointer_bits == 64u;
    }
    return 0;
}

static int cm_recipe_crate_envelope(const CmHirContext *hir,
    const CmHirCrate *crate_value)
{
    int feature = 0;
    int no_core = 0;
    int no_main = 0;
    uint32_t index;

    if (crate_value == NULL || crate_value->inner_attribute_count != 3u
        || crate_value->inner_attributes == NULL) return 0;
    for (index = 0u; index < 3u; ++index) {
        CmInternId metadata = crate_value->inner_attributes[index].metadata;
        if (cm_recipe_text_equal(hir, metadata, "feature(no_core)")) {
            if (feature) return 0;
            feature = 1;
        } else if (cm_recipe_text_equal(hir, metadata, "no_core")) {
            if (no_core) return 0;
            no_core = 1;
        } else if (cm_recipe_text_equal(hir, metadata, "no_main")) {
            if (no_main) return 0;
            no_main = 1;
        } else {
            return 0;
        }
    }
    return feature && no_core && no_main;
}

static const CmHirItem *cm_recipe_item(const CmHirContext *hir,
    CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = hir == NULL ? NULL : cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static int cm_recipe_identifier(const CmHirContext *hir, CmInternId id,
    const CmInternedString **out_text)
{
    static const char helper_prefix[] = "cmrustc_g3_recipe_";
    static const char *const keywords[] = {
        "auto", "break", "case", "char", "const", "continue",
        "default", "do", "double", "else", "enum", "extern", "float",
        "for", "goto", "if", "inline", "int", "long", "register",
        "restrict", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while", "_Bool", "_Complex", "_Imaginary"
    };
    const CmInternedString *text;
    size_t index;

    text = hir == NULL ? NULL : cm_interner_get(&hir->strings, id);
    if (out_text == NULL || text == NULL || text->len == 0u
        || text->len > 128u || text->bytes[0] == '_'
        || (text->len >= sizeof(helper_prefix) - 1u
            && memcmp(text->bytes, helper_prefix,
                sizeof(helper_prefix) - 1u) == 0)) return 0;
    for (index = 0u; index < text->len; ++index) {
        unsigned char byte = (unsigned char)text->bytes[index];
        if (!((byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
                || (byte >= (unsigned char)'A'
                    && byte <= (unsigned char)'Z')
                || byte == (unsigned char)'_'
                || (index != 0u && byte >= (unsigned char)'0'
                    && byte <= (unsigned char)'9'))) return 0;
    }
    for (index = 0u; index < sizeof(keywords) / sizeof(keywords[0]); ++index) {
        size_t length = strlen(keywords[index]);
        if (text->len == length
            && memcmp(text->bytes, keywords[index], length) == 0) return 0;
    }
    *out_text = text;
    return 1;
}

static const char *cm_recipe_c_type(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type = cm_hir_get_type(hir, type_id);

    if (type == NULL) return NULL;
    if (type->kind == CM_HIR_TYPE_BOOL_KIND) return "uint8_t";
    if (type->kind != CM_HIR_TYPE_INTEGER_KIND) return NULL;
    switch (type->data.integer_type.kind) {
    case CM_HIR_INT_I8: return "int8_t";
    case CM_HIR_INT_U8: return "uint8_t";
    case CM_HIR_INT_I16: return "int16_t";
    case CM_HIR_INT_U16: return "uint16_t";
    case CM_HIR_INT_I32: return "int32_t";
    case CM_HIR_INT_U32: return "uint32_t";
    case CM_HIR_INT_I64: return "int64_t";
    case CM_HIR_INT_U64: return "uint64_t";
    case CM_HIR_INT_ISIZE: return "intptr_t";
    case CM_HIR_INT_USIZE: return "uintptr_t";
    default: return NULL;
    }
}

static int cm_recipe_same_primitive(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id)
{
    const CmHirType *left = cm_hir_get_type(hir, left_id);
    const CmHirType *right = cm_hir_get_type(hir, right_id);

    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    if (left->kind == CM_HIR_TYPE_BOOL_KIND) return 1;
    if (left->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return left->data.integer_type.kind == right->data.integer_type.kind;
    }
    if (left->kind == CM_HIR_TYPE_FLOAT_KIND) {
        return left->data.float_type.kind == right->data.float_type.kind;
    }
    return 0;
}

static int cm_recipe_exact_attribute(const CmHirContext *hir,
    const CmHirItem *item)
{
    return item->attribute_count == 1u && item->attributes != NULL
        && cm_recipe_text_equal(hir, item->attributes[0].metadata,
            "no_mangle");
}

static int cm_recipe_exact_import(const CmHirContext *hir,
    const CmHirItem *wrapper, CmHirDefId callee)
{
    const CmHirModule *module;
    const CmHirImport *import_value;
    const CmHirImportBinding *binding;

    module = cm_hir_get_module(hir, wrapper->owner_module);
    if (module == NULL || module->import_count != 1u
        || module->imports == NULL) return 0;
    import_value = &module->imports[0];
    if (import_value->kind != CM_HIR_IMPORT_USE
        || import_value->binding_count != 1u
        || import_value->bindings == NULL) return 0;
    binding = &import_value->bindings[0];
    return binding->namespace_kind == CM_HIR_NAMESPACE_VALUE
        && binding->primitive_kind == CM_HIR_PRIMITIVE_NONE
        && !binding->is_anonymous
        && cm_hir_def_id_equal(binding->target, callee);
}

static int cm_recipe_metadata_body(const CmHirContext *hir,
    const CmHirItem *callee, CmHirGenericParamId generic)
{
    const CmHirBody *body;
    const CmHirExpr *block;
    const CmHirExpr *local;
    const CmHirType *expected;
    size_t index;
    int nonzero_identity = 0;

    if (callee->data.function_item.body == CM_HIR_BODY_NONE) return 0;
    body = cm_hir_get_body(hir, callee->data.function_item.body);
    if (body == NULL || body->state != CM_HIR_BODY_TYPED
        || body->origin.kind != CM_HIR_BODY_ORIGIN_METADATA_RECIPE
        || !cm_hir_def_id_equal(body->owner, callee->definition)
        || !cm_hir_def_id_equal(body->origin.definition, callee->definition)
        || !cm_hir_def_id_equal(body->origin.enclosing_definition,
            callee->definition)
        || !cm_hir_def_id_equal(
            body->origin.data.metadata_recipe.item_definition,
            callee->definition)
        || body->origin.data.metadata_recipe.recipe_index == 0u
        || body->origin.data.metadata_recipe.argument_index != 0u
        || body->source == 0u || body->source_expression_id != 0u
        || body->parameter_count != 1u || body->local_count != 1u
        || body->locals == NULL || body->locals[0].parameter_index != 0u
        || body->locals[0].parameter_binding_index != 0u
        || body->root_expression == CM_HIR_EXPR_NONE) return 0;
    for (index = 0u; index < CM_HIR_ARTIFACT_IDENTITY_SIZE; ++index) {
        nonzero_identity |= body->origin.data.metadata_recipe
            .artifact_identity[index] != 0u;
    }
    if (!nonzero_identity) return 0;
    expected = cm_hir_get_type(hir, body->expected_type);
    if (expected == NULL || expected->kind != CM_HIR_TYPE_PARAMETER_KIND
        || expected->data.parameter_type.parameter != generic
        || body->locals[0].type != body->expected_type) return 0;
    block = cm_hir_get_expr(hir, body->root_expression);
    local = block == NULL || block->kind != CM_HIR_EXPR_BLOCK
            || block->owner_body != callee->data.function_item.body
            || block->type != body->expected_type
            || block->data.block.statement_count != 0u
            || block->data.block.statements != NULL
            || block->data.block.tail_expression == CM_HIR_EXPR_NONE
        ? NULL : cm_hir_get_expr(hir, block->data.block.tail_expression);
    return local != NULL && local->kind == CM_HIR_EXPR_LOCAL
        && local->owner_body == callee->data.function_item.body
        && local->type == body->expected_type
        && local->data.local.local_index == 0u;
}

static int cm_recipe_marker_impl(const CmHirContext *hir,
    CmHirDefId trait_definition, CmHirTypeId concrete)
{
    const CmHirItem *trait_item;
    size_t index;
    uint32_t matches = 0u;

    trait_item = cm_recipe_item(hir, trait_definition);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || trait_item->generic_parameter_count != 0u
        || trait_item->predicate_scope_count != 0u
        || trait_item->predicate_count != 0u
        || trait_item->outlives_predicate_count != 0u
        || trait_item->data.trait_item.supertrait_count != 0u
        || trait_item->data.trait_item.is_auto
        || trait_item->data.trait_item.is_const) return 0;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        if (item == NULL) return 0;
        if (!cm_hir_def_id_is_none(item->parent_definition)
            && cm_hir_def_id_equal(item->parent_definition,
                trait_definition)) return 0;
        if (item->kind != CM_HIR_ITEM_IMPL
            || !item->data.impl_item.has_trait
            || !cm_hir_def_id_equal(item->data.impl_item.trait_type.definition,
                trait_definition)
            || !cm_recipe_same_primitive(hir,
                item->data.impl_item.self_type, concrete)) continue;
        if (item->definition.crate_id != trait_definition.crate_id
            || item->generic_parameter_count != 0u
            || item->predicate_scope_count != 0u
            || item->predicate_count != 0u
            || item->outlives_predicate_count != 0u
            || item->data.impl_item.trait_type.argument_count != 0u
            || item->data.impl_item.polarity != CM_HIR_IMPL_POSITIVE
            || item->data.impl_item.is_const
            || item->data.impl_item.safety != CM_HIR_SAFE) return 0;
        ++matches;
    }
    return matches == 1u;
}

static int cm_recipe_callee(const CmHirContext *hir,
    CmHirCrateId local_crate, const CmHirItem *callee,
    CmHirTypeId concrete)
{
    const CmHirFunctionSignature *signature;
    const CmHirGenericParam *generic;
    const CmHirType *parameter_type;
    const CmHirTraitPredicate *predicate;

    if (callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION
        || callee->definition.crate_id == local_crate
        || cm_hir_def_id_is_none(callee->definition)
        || !cm_hir_def_id_is_none(callee->parent_definition)
        || !cm_hir_def_id_is_none(
            callee->data.function_item.trait_item_definition)
        || callee->visibility.kind != CM_HIR_VIS_PUBLIC
        || !cm_hir_def_id_is_none(callee->visibility.restriction)
        || callee->attribute_count != 0u
        || callee->generic_parameter_count != 1u
        || callee->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || callee->predicate_scope_count != 0u
        || callee->predicate_count != 1u || callee->predicates == NULL
        || callee->outlives_predicate_count != 0u) return 0;
    generic = cm_hir_get_generic_param(hir,
        callee->generic_parameter_start);
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
        || !cm_hir_def_id_equal(generic->owner, callee->definition)
        || generic->index != 0u || generic->has_default
        || generic->is_relaxed_sized) return 0;
    signature = &callee->data.function_item.signature;
    if (signature->parameter_count != 1u || signature->parameters == NULL
        || signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->safety != CM_HIR_SAFE || signature->is_const
        || signature->is_async || signature->is_variadic
        || !cm_recipe_text_equal(hir, signature->abi, "Rust")
        || signature->return_type != signature->parameters[0].type) return 0;
    parameter_type = cm_hir_get_type(hir, signature->return_type);
    if (parameter_type == NULL
        || parameter_type->kind != CM_HIR_TYPE_PARAMETER_KIND
        || parameter_type->data.parameter_type.parameter
            != callee->generic_parameter_start) return 0;
    predicate = &callee->predicates[0];
    if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
        || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
        || predicate->subject != signature->return_type
        || cm_hir_def_id_is_none(predicate->trait_type.definition)
        || predicate->trait_type.argument_count != 0u) return 0;
    return cm_recipe_metadata_body(hir, callee,
            callee->generic_parameter_start)
        && cm_recipe_marker_impl(hir, predicate->trait_type.definition,
            concrete);
}

static int cm_recipe_semantic_type(const CmSemanticResults *results,
    const CmSemanticAdmission *authority, const CmSemanticTypeView *view,
    CmHirTypeId type)
{
    int equal = 0;
    return cm_semantic_type_view_matches_monomorphic_hir(results, authority,
            view, type, &equal) == CM_SEMANTIC_RESULTS_OK && equal;
}

static int cm_recipe_wrapper(const CmHirContext *hir,
    const CmSemanticAdmission *authority, const CmSemanticResults *results,
    CmHirCrateId local_crate, const CmHirItem *item,
    CmExecutableRecipePlan *out_plan)
{
    const CmHirFunctionSignature *signature;
    const CmHirBody *body;
    const CmHirExpr *block;
    const CmHirExpr *call;
    const CmHirExpr *argument;
    const CmHirItem *callee;
    CmSemanticBodyView body_view;
    CmSemanticExpressionView expression_view;
    CmSemanticDirectCallView direct_call;
    CmSemanticTypeView direct_parameter;
    const CmInternedString *name;
    const char *c_type;

    if (item == NULL || out_plan == NULL
        || item->kind != CM_HIR_ITEM_FUNCTION
        || item->definition.crate_id != local_crate
        || !cm_hir_def_id_is_none(item->parent_definition)
        || !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || item->visibility.kind != CM_HIR_VIS_PUBLIC
        || !cm_hir_def_id_is_none(item->visibility.restriction)
        || !cm_recipe_exact_attribute(hir, item)
        || item->generic_parameter_count != 0u
        || item->predicate_scope_count != 0u || item->predicate_count != 0u
        || item->outlives_predicate_count != 0u
        || !cm_recipe_identifier(hir, item->name, &name)) return 0;
    signature = &item->data.function_item.signature;
    c_type = signature->parameter_count == 1u
        && signature->parameters != NULL
        ? cm_recipe_c_type(hir, signature->parameters[0].type) : NULL;
    if (c_type == NULL || signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->safety != CM_HIR_SAFE || signature->is_const
        || signature->is_async || signature->is_variadic
        || !cm_recipe_text_equal(hir, signature->abi, "C")
        || !cm_recipe_same_primitive(hir, signature->parameters[0].type,
            signature->return_type)
        || item->data.function_item.body == CM_HIR_BODY_NONE) return 0;
    body = cm_hir_get_body(hir, item->data.function_item.body);
    if (body == NULL || body->state != CM_HIR_BODY_TYPED
        || body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        || !cm_hir_def_id_equal(body->owner, item->definition)
        || !cm_hir_def_id_equal(body->origin.definition, item->definition)
        || !cm_hir_def_id_equal(body->origin.enclosing_definition,
            item->definition)
        || !cm_hir_def_id_equal(body->origin.data.item_source.item_definition,
            item->definition)
        || body->source == 0u || body->source_expression_id == 0u
        || body->expected_type != signature->return_type
        || body->parameter_count != 1u || body->local_count != 1u
        || body->locals == NULL || body->locals[0].parameter_index != 0u
        || body->locals[0].parameter_binding_index != 0u
        || !cm_recipe_same_primitive(hir, body->locals[0].type,
            signature->parameters[0].type)) return 0;
    block = cm_hir_get_expr(hir, body->root_expression);
    call = block == NULL || block->kind != CM_HIR_EXPR_BLOCK
            || block->owner_body != item->data.function_item.body
            || block->data.block.statement_count != 0u
            || block->data.block.statements != NULL
            || !cm_recipe_same_primitive(hir, block->type,
                signature->return_type)
        ? NULL : cm_hir_get_expr(hir, block->data.block.tail_expression);
    argument = call == NULL || call->kind != CM_HIR_EXPR_CALL
            || call->owner_body != item->data.function_item.body
            || call->data.call.type_substitution_count != 1u
            || call->data.call.type_substitutions == NULL
            || call->data.call.argument_count != 1u
            || call->data.call.arguments == NULL
            || !cm_recipe_same_primitive(hir,
                call->data.call.type_substitutions[0],
                signature->return_type)
            || !cm_recipe_same_primitive(hir, call->type,
                signature->return_type)
        ? NULL : cm_hir_get_expr(hir, call->data.call.arguments[0]);
    if (argument == NULL || argument->kind != CM_HIR_EXPR_LOCAL
        || argument->owner_body != item->data.function_item.body
        || argument->data.local.local_index != 0u
        || !cm_recipe_same_primitive(hir, argument->type,
            signature->parameters[0].type)) return 0;
    callee = cm_recipe_item(hir, call->data.call.callee);
    if (!cm_recipe_exact_import(hir, item, call->data.call.callee)
        || !cm_recipe_callee(hir, local_crate, callee,
            call->data.call.type_substitutions[0])) return 0;

    memset(&body_view, 0, sizeof(body_view));
    memset(&expression_view, 0, sizeof(expression_view));
    memset(&direct_call, 0, sizeof(direct_call));
    memset(&direct_parameter, 0, sizeof(direct_parameter));
    if (cm_semantic_results_body(results, authority,
            item->data.function_item.body, &body_view)
                != CM_SEMANTIC_RESULTS_OK
        || body_view.expression_count != 3u
        || !cm_hir_def_id_equal(body_view.owner, item->definition)
        || cm_semantic_results_expression(results, authority,
            item->data.function_item.body, block->data.block.tail_expression,
            &expression_view) != CM_SEMANTIC_RESULTS_OK
        || expression_view.adjustment_count != 0u
        || !expression_view.has_direct_callable
        || !cm_hir_def_id_equal(expression_view.direct_callable,
            callee->definition)
        || !cm_recipe_semantic_type(results, authority,
            &expression_view.adjusted_type, signature->return_type)
        || cm_semantic_results_direct_call(results, authority,
            item->data.function_item.body, block->data.block.tail_expression,
            &direct_call) != CM_SEMANTIC_RESULTS_OK
        || direct_call.parameter_count != 1u
        || !cm_hir_def_id_equal(direct_call.callee, callee->definition)
        || !cm_recipe_semantic_type(results, authority,
            &direct_call.return_type, signature->return_type)
        || cm_semantic_results_direct_call_parameter(results, authority,
            item->data.function_item.body, block->data.block.tail_expression,
            0u, &direct_parameter) != CM_SEMANTIC_RESULTS_OK
        || !cm_recipe_semantic_type(results, authority, &direct_parameter,
            signature->parameters[0].type)) return 0;
    out_plan->name = name;
    out_plan->c_type = c_type;
    return 1;
}

static void cm_recipe_emit_header(CmStrBuf *output,
    const CmTargetDesc *target)
{
    cm_str_buf_append(output,
        "#include <limits.h>\n"
        "#include <stdint.h>\n\n"
        "#if CHAR_BIT != 8\n"
        "# error \"cmrustc requires 8-bit bytes\"\n"
        "#endif\n");
    if (target->pointer_bits == 32u) {
        cm_str_buf_append(output,
            "#if UINTPTR_MAX != UINT32_MAX\n"
            "# error \"cmrustc target requires 32-bit pointers\"\n"
            "#endif\n\n");
    } else {
        cm_str_buf_append(output,
            "#if UINTPTR_MAX != UINT64_MAX\n"
            "# error \"cmrustc target requires 64-bit pointers\"\n"
            "#endif\n\n");
    }
}

CmExecutableRecipeEmitStatus cm_c_emit_executable_recipe_program(
    const CmHirContext *hir, const CmSemanticAdmission *authority,
    CmHirCrateId local_crate, const CmTargetDesc *target,
    CmStrBuf *output)
{
    const CmSemanticResults *results;
    const CmHirCrate *crate_value;
    CmExecutableRecipePlan plan;
    size_t index;
    size_t module_count = 0u;
    size_t wrapper_count = 0u;

    if (hir == NULL || authority == NULL || target == NULL || output == NULL
        || local_crate == CM_HIR_CRATE_NONE) {
        return CM_EXECUTABLE_RECIPE_EMIT_INVALID_ARGUMENT;
    }
    if (!cm_recipe_target_supported(target)) {
        return CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_TARGET;
    }
    if (!cm_semantic_admission_is_current(authority)
        || cm_semantic_admission_hir(authority) != hir
        || cm_semantic_admission_crate(authority) != local_crate
        || cm_semantic_admission_generation(authority)
            != hir->semantic_generation
        || cm_semantic_admission_capability_id(authority) == UINT64_C(0)
        || cm_semantic_admission_barrier_capability_id(authority)
            == UINT64_C(0)
        || cm_semantic_admission_parent_capability_id(authority)
            != UINT64_C(0)) {
        return CM_EXECUTABLE_RECIPE_EMIT_INVALID_AUTHORITY;
    }
    results = cm_semantic_admission_results(authority);
    if (results == NULL || !cm_semantic_results_is_current(results, authority)
        || cm_semantic_results_hir(results, authority) != hir
        || cm_semantic_results_crate(results, authority) != local_crate) {
        return CM_EXECUTABLE_RECIPE_EMIT_INVALID_AUTHORITY;
    }
    crate_value = cm_hir_get_crate(hir, local_crate);
    if (!cm_recipe_crate_envelope(hir, crate_value)) {
        return CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_PROGRAM;
    }
    for (index = 0u; index < hir->modules.len; ++index) {
        const CmHirModule *module = (const CmHirModule *)cm_vec_at_const(
            &hir->modules, index);
        if (module == NULL) return CM_EXECUTABLE_RECIPE_EMIT_INVALID_HIR;
        if (module->crate_id == local_crate) ++module_count;
    }
    if (module_count != 1u || crate_value->root_module == CM_HIR_MODULE_NONE
        || cm_hir_get_module(hir, crate_value->root_module) == NULL
        || cm_hir_get_module(hir, crate_value->root_module)->parent
            != CM_HIR_MODULE_NONE) {
        return CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_PROGRAM;
    }
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        size_t previous;

        if (item == NULL) return CM_EXECUTABLE_RECIPE_EMIT_INVALID_HIR;
        if (item->definition.crate_id != local_crate) continue;
        if (!cm_recipe_wrapper(hir, authority, results, local_crate, item,
                &plan)) {
            return CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_PROGRAM;
        }
        for (previous = 0u; previous < index; ++previous) {
            const CmHirItem *other = (const CmHirItem *)cm_vec_at_const(
                &hir->items, previous);
            const CmInternedString *other_name;
            if (other == NULL) return CM_EXECUTABLE_RECIPE_EMIT_INVALID_HIR;
            if (other->definition.crate_id == local_crate
                && cm_recipe_identifier(hir, other->name, &other_name)
                && other_name->len == plan.name->len
                && memcmp(other_name->bytes, plan.name->bytes,
                    plan.name->len) == 0) {
                return CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_PROGRAM;
            }
        }
        ++wrapper_count;
    }
    if (wrapper_count == 0u) {
        return CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_PROGRAM;
    }

    /* Every fallible check precedes the first append. */
    cm_recipe_emit_header(output, target);
    wrapper_count = 0u;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &hir->items, index);
        char ordinal[32];
        int length;

        if (item->definition.crate_id != local_crate) continue;
        (void)cm_recipe_wrapper(hir, authority, results, local_crate, item,
            &plan);
        length = snprintf(ordinal, sizeof(ordinal), "%lu",
            (unsigned long)wrapper_count);
        cm_str_buf_append(output, "static ");
        cm_str_buf_append(output, plan.c_type);
        cm_str_buf_append(output, " cmrustc_g3_recipe_");
        cm_str_buf_append_n(output, ordinal, (size_t)length);
        cm_str_buf_append(output, "(");
        cm_str_buf_append(output, plan.c_type);
        cm_str_buf_append(output, " value)\n{\n    return value;\n}\n\n");
        cm_str_buf_append(output, plan.c_type);
        cm_str_buf_append(output, " ");
        cm_str_buf_append_n(output, (const char *)plan.name->bytes,
            plan.name->len);
        cm_str_buf_append(output, "(");
        cm_str_buf_append(output, plan.c_type);
        cm_str_buf_append(output, " value)\n{\n    return cmrustc_g3_recipe_");
        cm_str_buf_append_n(output, ordinal, (size_t)length);
        cm_str_buf_append(output, "(value);\n}\n\n");
        ++wrapper_count;
    }
    return CM_EXECUTABLE_RECIPE_EMIT_OK;
}

const char *cm_executable_recipe_emit_status_name(
    CmExecutableRecipeEmitStatus status)
{
    switch (status) {
    case CM_EXECUTABLE_RECIPE_EMIT_OK: return "ok";
    case CM_EXECUTABLE_RECIPE_EMIT_INVALID_ARGUMENT:
        return "invalid-argument";
    case CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_TARGET:
        return "unsupported-target";
    case CM_EXECUTABLE_RECIPE_EMIT_INVALID_AUTHORITY:
        return "invalid-authority";
    case CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_PROGRAM:
        return "unsupported-program";
    case CM_EXECUTABLE_RECIPE_EMIT_INVALID_HIR: return "invalid-hir";
    }
    return "unknown";
}
