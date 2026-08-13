#include "cm/hir/lower.h"
#include "cm/hir/projection.h"
#include "cm/syntax/parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TestResolver {
    unsigned int calls;
    int fail;
    int return_absent_definition;
    int return_shared_self;
    CmHirContext *hir;
    CmHirDefId external_definition;
    CmHirTypeId shared_self_type;
} TestResolver;

static int ast_string_is(const CmAst *ast, CmInternId id, const char *text)
{
    const CmInternedString *string;
    size_t length;

    string = cm_ast_get_string(ast, id);
    length = strlen(text);
    return string != NULL && string->len == length
        && memcmp(string->bytes, text, length) == 0;
}

static CmHirLowerResolution resolve_external(void *user_context,
    const CmAst *ast, CmAstPathId path_id, CmHirModuleId current_module,
    CmHirLowerPathUse use)
{
    TestResolver *resolver;
    const CmAstPath *path;
    CmHirLowerResolution resolution;

    (void)current_module;
    resolver = (TestResolver *)user_context;
    resolver->calls += 1u;
    memset(&resolution, 0, sizeof(resolution));
    if (resolver->fail) {
        resolution.kind = CM_HIR_LOWER_RESOLVER_ERROR;
        return resolution;
    }
    if (resolver->return_absent_definition) {
        resolution.kind = CM_HIR_LOWER_DEFINITION;
        resolution.definition.crate_id = 88u;
        resolution.definition.index = 1u;
        resolution.named_type_kind = CM_HIR_TYPE_ADT_KIND;
        return resolution;
    }
    path = cm_ast_get_path(ast, path_id);
    if (resolver->return_shared_self
        && use == CM_HIR_LOWER_PATH_TYPE && path != NULL
        && path->segment_count == 1u
        && ast_string_is(ast, path->segments[0].name, "Shared")) {
        if (resolver->shared_self_type == CM_HIR_TYPE_NONE) {
            size_t index;
            CmHirType type;

            assert(resolver->hir != NULL);
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_SELF_KIND;
            type.span.source = 7u;
            type.span.start = 0u;
            type.span.end = 6u;
            type.data.self_type.owner = cm_hir_def_id_none();
            for (index = 0u; index < resolver->hir->definitions.len;
                 ++index) {
                const CmHirDefinition *definition;

                definition = (const CmHirDefinition *)cm_vec_at_const(
                    &resolver->hir->definitions, index);
                if (definition != NULL
                    && definition->kind == CM_HIR_DEFINITION_ITEM
                    && definition->state == CM_HIR_DEFINITION_RESERVED
                    && definition->has_reserved_item_kind
                    && definition->reserved_item_kind == CM_HIR_ITEM_TRAIT) {
                    type.data.self_type.owner = definition->id;
                    break;
                }
            }
            assert(!cm_hir_def_id_is_none(type.data.self_type.owner));
            assert(cm_hir_add_type(resolver->hir, &type,
                &resolver->shared_self_type) == CM_HIR_OK);
        }
        resolution.kind = CM_HIR_LOWER_EXISTING_TYPE;
        resolution.existing_type = resolver->shared_self_type;
        return resolution;
    }
    if (use == CM_HIR_LOWER_PATH_TYPE && path != NULL
        && path->segment_count == 1u
        && ast_string_is(ast, path->segments[0].name, "External")) {
        if (cm_hir_def_id_is_none(resolver->external_definition)) {
            CmHirCrateId crate_id;
            CmHirModuleId root_module;
            CmHirItem item;
            CmHirItemId item_id;
            CmSpan span;

            assert(resolver->hir != NULL);
            span.source = 99u;
            span.start = 0u;
            span.end = 8u;
            assert(cm_hir_create_crate(resolver->hir,
                cm_hir_intern(resolver->hir, "external"),
                CM_HIR_EDITION_2024, span, &crate_id, &root_module)
                == CM_HIR_OK);
            assert(cm_hir_reserve_item_definition(resolver->hir, crate_id,
                span, &resolver->external_definition) == CM_HIR_OK);
            memset(&item, 0, sizeof(item));
            item.kind = CM_HIR_ITEM_STRUCT;
            item.definition = resolver->external_definition;
            item.owner_module = root_module;
            item.parent_definition = cm_hir_def_id_none();
            item.name = cm_hir_intern(resolver->hir, "External");
            item.visibility.kind = CM_HIR_VIS_PUBLIC;
            item.visibility.restriction = cm_hir_def_id_none();
            item.span = span;
            item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
            assert(cm_hir_add_item(resolver->hir, &item, &item_id)
                == CM_HIR_OK);
        }
        resolution.kind = CM_HIR_LOWER_DEFINITION;
        resolution.definition = resolver->external_definition;
        resolution.named_type_kind = CM_HIR_TYPE_ADT_KIND;
    }
    return resolution;
}

static const CmHirItem *find_item(const CmHirContext *context,
    const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        item_name = cm_interner_get(&context->strings, item->name);
        if (item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_impl(const CmHirContext *context)
{
    size_t index;

    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        if (item != NULL && item->kind == CM_HIR_ITEM_IMPL) return item;
    }
    return NULL;
}

static const CmHirItem *find_child(const CmHirContext *context,
    CmHirDefId parent, const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        if (item == NULL
            || !cm_hir_def_id_equal(item->parent_definition, parent)) {
            continue;
        }
        item_name = cm_interner_get(&context->strings, item->name);
        if (item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static int hir_string_is(const CmHirContext *context, CmInternId id,
    const char *text)
{
    const CmInternedString *string;
    size_t length;

    string = cm_interner_get(&context->strings, id);
    length = strlen(text);
    return string != NULL && string->len == length
        && memcmp(string->bytes, text, length) == 0;
}

static const CmHirType *expect_type_kind(const CmHirContext *context,
    CmHirTypeId type_id, CmHirTypeKind kind)
{
    const CmHirType *type;

    type = cm_hir_get_type(context, type_id);
    assert(type != NULL && type->kind == kind);
    return type;
}

static CmHirLowerResult lower_source(const char *source,
    CmHirContext *context, TestResolver *resolver)
{
    CmAst ast;
    CmParseResult parse_result;
    CmHirLowerOptions options;
    CmHirLowerResult result;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, strlen(source),
        CM_EDITION_2024);
    assert(parse_result.error_count == 0u);
    cm_hir_context_init(context);
    cm_hir_lower_options_init(&options);
    options.crate_name = "lower_test";
    options.source = 7u;
    if (resolver != NULL) {
        resolver->hir = context;
        options.resolve_path = resolve_external;
        options.resolve_context = resolver;
    }
    result = cm_hir_lower_crate(context, &ast, &options);
    cm_ast_destroy(&ast);
    return result;
}

static void expect_invalid_ast_lowering(const CmAst *ast,
    const CmHirLowerOptions *options, const char *message)
{
    CmHirContext context;
    CmHirLowerResult result;

    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, ast, options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, message) != NULL);
    cm_hir_context_destroy(&context);
}

static void test_complete_declarations(void)
{
    static const char source[] =
        "struct Node<'a, T> {"
        " next: &'a Node<'a, T>, data: T, bytes: [u8; 4], ext: External"
        "}"
        "struct First { next: Second }"
        "struct Second { prior: *const First }"
        "enum Maybe<T> { None, Some(T) }"
        "type Bytes = [u8; 16];"
        "const LIMIT: usize = 16;"
        "static mut FLAG: u8 = 0;"
        "mod inner {"
        " pub(crate) struct Pair {"
        "  left: super::Node<'static, u8>,"
        "  callback: fn(*const u8, &[u8]) -> bool,"
        "  notify: fn(u8)"
        " }"
        " pub fn consume<'a, T>(value: &'a T) -> (u8, usize) {"
        "  (0, 0)"
        " }"
        "}"
        "fn declaration(value: u8);";
    CmHirContext context;
    TestResolver resolver;
    CmHirLowerResult result;
    const CmHirItem *node;
    const CmHirItem *maybe;
    const CmHirItem *first;
    const CmHirItem *second;
    const CmHirItem *bytes;
    const CmHirItem *limit;
    const CmHirItem *flag;
    const CmHirItem *pair;
    const CmHirItem *consume;
    const CmHirItem *declaration;
    const CmHirType *next_type;
    const CmHirType *node_type;
    const CmHirType *external_type;
    const CmHirBody *body;
    const CmHirType *function_type;
    const CmHirType *return_type;

    memset(&resolver, 0, sizeof(resolver));
    result = lower_source(source, &context, &resolver);
    if (result.error_count != 0u) {
        fprintf(stderr, "lowering failed: %s: %s (type %u at %u..%u)\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message,
            (unsigned int)result.first_error.type,
            (unsigned int)result.first_error.span.start,
            (unsigned int)result.first_error.span.end);
    }
    assert(result.error_count == 0u);
    assert(result.crate_id == 1u);
    assert(result.root_module == 1u);
    assert(result.lowered_item_count == 11u);
    assert(context.items.len == 12u);
    assert(context.modules.len == 3u);
    assert(context.bodies.len == 3u);
    assert(context.generic_parameters.len == 5u);
    assert(resolver.calls == 1u);

    node = find_item(&context, "Node");
    maybe = find_item(&context, "Maybe");
    first = find_item(&context, "First");
    second = find_item(&context, "Second");
    bytes = find_item(&context, "Bytes");
    limit = find_item(&context, "LIMIT");
    flag = find_item(&context, "FLAG");
    pair = find_item(&context, "Pair");
    consume = find_item(&context, "consume");
    declaration = find_item(&context, "declaration");
    assert(node != NULL && node->kind == CM_HIR_ITEM_STRUCT);
    assert(maybe != NULL && maybe->kind == CM_HIR_ITEM_ENUM);
    assert(first != NULL && first->kind == CM_HIR_ITEM_STRUCT);
    assert(second != NULL && second->kind == CM_HIR_ITEM_STRUCT);
    assert(bytes != NULL && bytes->kind == CM_HIR_ITEM_TYPE_ALIAS);
    assert(limit != NULL && limit->kind == CM_HIR_ITEM_CONST);
    assert(flag != NULL && flag->kind == CM_HIR_ITEM_STATIC);
    assert(pair != NULL && pair->kind == CM_HIR_ITEM_STRUCT);
    assert(consume != NULL && consume->kind == CM_HIR_ITEM_FUNCTION);
    assert(declaration != NULL
        && declaration->kind == CM_HIR_ITEM_FUNCTION);
    assert(node->data.aggregate_item.field_count == 4u);
    next_type = cm_hir_get_type(&context,
        node->data.aggregate_item.fields[0].type);
    assert(next_type != NULL
        && next_type->kind == CM_HIR_TYPE_REFERENCE_KIND);
    assert(next_type->data.reference_type.region.kind
        == CM_HIR_REGION_EARLY_BOUND);
    node_type = cm_hir_get_type(&context,
        next_type->data.reference_type.pointee);
    assert(node_type != NULL && node_type->kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(node_type->data.named_type.definition,
        node->definition));
    node_type = cm_hir_get_type(&context,
        first->data.aggregate_item.fields[0].type);
    assert(node_type != NULL && node_type->kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(node_type->data.named_type.definition,
        second->definition));
    external_type = cm_hir_get_type(&context,
        node->data.aggregate_item.fields[3].type);
    assert(external_type != NULL
        && external_type->kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(
        external_type->data.named_type.definition,
        resolver.external_definition));
    assert(pair->visibility.kind == CM_HIR_VIS_CRATE);
    assert(pair->data.aggregate_item.field_count == 3u);
    function_type = cm_hir_get_type(&context,
        pair->data.aggregate_item.fields[1].type);
    assert(function_type != NULL
        && function_type->kind == CM_HIR_TYPE_FN_POINTER_KIND);
    assert(function_type->data.fn_pointer_type.parameter_count == 2u);
    function_type = cm_hir_get_type(&context,
        pair->data.aggregate_item.fields[2].type);
    assert(function_type != NULL
        && function_type->kind == CM_HIR_TYPE_FN_POINTER_KIND);
    return_type = cm_hir_get_type(&context,
        function_type->data.fn_pointer_type.return_type);
    assert(return_type != NULL
        && return_type->kind == CM_HIR_TYPE_UNIT_KIND);
    assert(consume->data.function_item.signature.parameter_count == 1u);
    assert(consume->data.function_item.body != CM_HIR_BODY_NONE);
    body = cm_hir_get_body(&context,
        consume->data.function_item.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED);
    assert(body->source_expression_id != 0u);
    assert(body->parameter_count == 1u);
    assert(limit->data.value_item.body != CM_HIR_BODY_NONE);
    body = cm_hir_get_body(&context, limit->data.value_item.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED);
    assert(body->source_expression_id != 0u);
    assert(flag->data.value_item.mutability == CM_HIR_MUTABLE);
    body = cm_hir_get_body(&context, flag->data.value_item.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED);
    assert(declaration->data.function_item.body == CM_HIR_BODY_NONE);
    cm_hir_context_destroy(&context);
}

static void test_union_declarations(void)
{
    static const char source[] =
        "pub union Bits { pub byte: u8, word: u32 } "
        "struct Wrap { bits: Bits }";
    static const char malformed[] = "union Bits { byte: u8 }";
    CmAst ast;
    CmParseResult parse_result;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *bits;
    const CmHirItem *wrap;
    const CmHirType *byte_type;
    const CmHirType *bits_type;
    const CmAstItemId *root_id;
    CmAstItem *item;
    CmAstField *saved_fields;

    result = lower_source(source, &context, NULL);
    bits = find_item(&context, "Bits");
    wrap = find_item(&context, "Wrap");
    byte_type = bits == NULL || bits->data.aggregate_item.field_count != 2u
        ? NULL : cm_hir_get_type(&context,
            bits->data.aggregate_item.fields[0].type);
    bits_type = wrap == NULL || wrap->data.aggregate_item.field_count != 1u
        ? NULL : cm_hir_get_type(&context,
            wrap->data.aggregate_item.fields[0].type);
    assert(result.error_count == 0u && result.lowered_item_count == 2u
        && bits != NULL && bits->kind == CM_HIR_ITEM_UNION
        && bits->visibility.kind == CM_HIR_VIS_PUBLIC
        && bits->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && bits->data.aggregate_item.field_count == 2u
        && bits->data.aggregate_item.fields[0].visibility.kind
            == CM_HIR_VIS_PUBLIC
        && byte_type != NULL && byte_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && byte_type->data.integer_type.kind == CM_HIR_INT_U8
        && wrap != NULL && wrap->kind == CM_HIR_ITEM_STRUCT
        && bits_type != NULL && bits_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(bits_type->data.named_type.definition,
            bits->definition));
    cm_hir_context_destroy(&context);

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, malformed,
        sizeof(malformed) - 1u, CM_EDITION_2024);
    assert(parse_result.error_count == 0u && ast.root_items.len == 1u);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*root_id - 1u);
    assert(item != NULL && item->kind == CM_AST_ITEM_UNION
        && item->data.aggregate_item.form == CM_AST_FIELDS_NAMED
        && item->data.aggregate_item.field_count == 1u
        && item->data.aggregate_item.fields != NULL);
    cm_hir_lower_options_init(&options);
    options.crate_name = "malformed_union_test";
    options.source = 7u;

    item->data.aggregate_item.form = CM_AST_FIELDS_TUPLE;
    expect_invalid_ast_lowering(&ast, &options, "must have named fields");
    item->data.aggregate_item.form = (CmAstFieldForm)99;
    expect_invalid_ast_lowering(&ast, &options, "invalid field form");
    item->data.aggregate_item.form = CM_AST_FIELDS_NAMED;
    saved_fields = item->data.aggregate_item.fields;
    item->data.aggregate_item.fields = NULL;
    expect_invalid_ast_lowering(&ast, &options, "field storage is absent");
    item->data.aggregate_item.fields = saved_fields;
    cm_ast_destroy(&ast);
}

static void test_default_specialization_fails_closed(void)
{
    static const char source[] =
        "trait Marker { fn specialize(); } struct Value; "
        "impl Marker for Value { default fn specialize() {} }";
    CmAst ast;
    CmParseResult parse_result;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;
    const CmAstItemId *impl_id;
    const CmAstItem *impl_item;
    CmAstItem *method;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    assert(parse_result.error_count == 0u && ast.root_items.len == 3u);
    impl_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    impl_item = impl_id == NULL ? NULL : cm_ast_get_item(&ast, *impl_id);
    method = impl_item == NULL || impl_item->kind != CM_AST_ITEM_IMPL
            || impl_item->data.impl_item.item_count != 1u
        ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
            (size_t)impl_item->data.impl_item.items[0] - 1u);
    assert(method != NULL && method->kind == CM_AST_ITEM_FUNCTION
        && method->is_default == 1);

    cm_hir_lower_options_init(&options);
    options.crate_name = "default_specialization_test";
    options.source = 7u;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    if (result.error_count != 1u
        || result.first_error.kind != CM_HIR_LOWER_UNSUPPORTED_ITEM
        || strstr(result.first_error.message,
            "default specialization semantics") == NULL) {
        fprintf(stderr, "default specialization lowering actual: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM
        && strstr(result.first_error.message,
            "default specialization semantics") != NULL);
    cm_hir_context_destroy(&context);

    method->is_default = 99;
    expect_invalid_ast_lowering(&ast, &options,
        "invalid default specialization flag");
    method->is_default = 1;
    cm_ast_destroy(&ast);
}

static void test_enum_variant_attributes_fail_closed(void)
{
    static const char source[] =
        "enum Choice { #[cfg(target_pointer_width = \"64\")] Wide, Narrow }";
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM
        && strstr(result.first_error.message,
            "variant attributes require effective cfg lowering") != NULL
        && context.items.len == 0u);
    cm_hir_context_destroy(&context);
}

static void test_trait_alias_lowering(void)
{
    static const char source[] =
        "trait Pointee<T> { type Metadata; }"
        "trait PointeeSized {}"
        "trait Thin<'a, T = Self> = "
            "Pointee<T, Metadata = ()> + 'a + ~const PointeeSized;"
        "trait Chain = PointeeSized;"
        "trait Repeat = PointeeSized + PointeeSized;"
        "trait Uses: Chain {}"
        "trait Generic<T: Chain> {}";
    static const char *const rejected[] = {
        "trait Alias = Alias;",
        "trait Alias = A; trait A: Alias {}",
        "trait Base {} trait Alias = Base; struct S; impl Alias for S {}"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_WRONG_NAMESPACE
    };
    CmAst ast;
    CmParseResult parse_result;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *pointee;
    const CmHirItem *pointee_sized;
    const CmHirItem *metadata;
    const CmHirItem *thin;
    const CmHirItem *chain;
    const CmHirItem *repeat;
    const CmHirItem *uses;
    const CmHirItem *generic;
    const CmHirGenericParam *lifetime_parameter;
    const CmHirGenericParam *type_parameter;
    const CmHirType *type;
    const CmAstItemId *thin_id;
    CmAstItem *thin_ast;
    CmAstSupertrait *saved_bounds;
    size_t index;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    thin_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    thin_ast = thin_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*thin_id - 1u);
    assert(parse_result.error_count == 0u && thin_ast != NULL
        && thin_ast->kind == CM_AST_ITEM_TRAIT
        && thin_ast->data.trait_item.is_alias == 1
        && thin_ast->data.trait_item.structured_alias_bound_count == 3u);

    cm_hir_lower_options_init(&options);
    options.crate_name = "trait_alias_test";
    options.source = 7u;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    pointee = find_item(&context, "Pointee");
    pointee_sized = find_item(&context, "PointeeSized");
    thin = find_item(&context, "Thin");
    chain = find_item(&context, "Chain");
    repeat = find_item(&context, "Repeat");
    uses = find_item(&context, "Uses");
    generic = find_item(&context, "Generic");
    metadata = pointee == NULL ? NULL
        : find_child(&context, pointee->definition, "Metadata");
    lifetime_parameter = thin == NULL || thin->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(&context,
            thin->generic_parameter_start);
    type_parameter = thin == NULL || thin->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(&context,
            thin->generic_parameter_start + 1u);
    if (result.error_count != 0u) {
        fprintf(stderr, "trait-alias lowering: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && pointee != NULL
        && pointee_sized != NULL && metadata != NULL
        && thin != NULL && thin->kind == CM_HIR_ITEM_TRAIT_ALIAS
        && thin->data.trait_alias_item.bound_count == 3u
        && lifetime_parameter != NULL
        && lifetime_parameter->kind == CM_HIR_GENERIC_LIFETIME
        && type_parameter != NULL
        && type_parameter->kind == CM_HIR_GENERIC_TYPE
        && type_parameter->has_default
        && type_parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE);
    type = cm_hir_get_type(&context,
        type_parameter->default_argument.data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(type->data.self_type.owner,
            thin->definition));
    assert(thin->data.trait_alias_item.bounds[0].kind
            == CM_HIR_TRAIT_ALIAS_BOUND_TRAIT
        && thin->data.trait_alias_item.bounds[1].kind
            == CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME
        && thin->data.trait_alias_item.bounds[2].kind
            == CM_HIR_TRAIT_ALIAS_BOUND_TRAIT
        && cm_hir_def_id_equal(thin->data.trait_alias_item.bounds[0].data
                .trait_bound.trait_type.definition,
            pointee->definition)
        && thin->data.trait_alias_item.bounds[0].data.trait_bound
            .trait_type.argument_count == 1u
        && thin->data.trait_alias_item.bounds[0].data.trait_bound
            .equality_count == 1u
        && cm_hir_def_id_equal(thin->data.trait_alias_item.bounds[0].data
                .trait_bound.equalities[0].associated_type,
            metadata->definition)
        && thin->data.trait_alias_item.bounds[1].data.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && thin->data.trait_alias_item.bounds[1].data.lifetime.data.parameter
            == thin->generic_parameter_start
        && thin->data.trait_alias_item.bounds[2].data.trait_bound.modifier
            == CM_HIR_SUPERTRAIT_CONST_IF_CONST
        && cm_hir_def_id_equal(thin->data.trait_alias_item.bounds[2].data
                .trait_bound.trait_type.definition,
            pointee_sized->definition));
    type = cm_hir_get_type(&context, thin->data.trait_alias_item.bounds[0]
        .data.trait_bound.trait_type.arguments[0].data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter
            == thin->generic_parameter_start + 1u);
    type = cm_hir_get_type(&context, thin->data.trait_alias_item.bounds[0]
        .data.trait_bound.equalities[0].value);
    assert(type != NULL && type->kind == CM_HIR_TYPE_UNIT_KIND);
    assert(chain != NULL && chain->kind == CM_HIR_ITEM_TRAIT_ALIAS
        && chain->data.trait_alias_item.bound_count == 1u
        && cm_hir_def_id_equal(chain->data.trait_alias_item.bounds[0].data
                .trait_bound.trait_type.definition,
            pointee_sized->definition));
    assert(repeat != NULL && repeat->kind == CM_HIR_ITEM_TRAIT_ALIAS
        && repeat->data.trait_alias_item.bound_count == 2u
        && cm_hir_def_id_equal(repeat->data.trait_alias_item.bounds[0].data
                .trait_bound.trait_type.definition,
            pointee_sized->definition)
        && cm_hir_def_id_equal(repeat->data.trait_alias_item.bounds[1].data
                .trait_bound.trait_type.definition,
            pointee_sized->definition));
    assert(uses != NULL && uses->kind == CM_HIR_ITEM_TRAIT
        && uses->data.trait_item.supertrait_count == 1u
        && cm_hir_def_id_equal(uses->data.trait_item.supertraits[0]
                .trait_type.definition,
            chain->definition));
    assert(generic != NULL && generic->predicate_count == 1u
        && cm_hir_def_id_equal(generic->predicates[0]
                .trait_type.definition,
            chain->definition));
    cm_hir_context_destroy(&context);

    thin_ast->data.trait_item.is_alias = 99;
    expect_invalid_ast_lowering(&ast, &options, "invalid alias flag");
    thin_ast->data.trait_item.is_alias = 1;

    saved_bounds = thin_ast->data.trait_item.structured_alias_bounds;
    thin_ast->data.trait_item.structured_alias_bounds = NULL;
    expect_invalid_ast_lowering(&ast, &options, "structural bounds disagree");
    thin_ast->data.trait_item.structured_alias_bounds = saved_bounds;
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]); ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected_kinds[index]) {
            fprintf(stderr, "trait-alias rejection mismatch for %s: %s: %s\n",
                rejected[index],
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]);
        cm_hir_context_destroy(&context);
    }
}

static void test_auto_trait_and_negative_impl_lowering(void)
{
    static const char source[] =
        "trait PointeeSized {} "
        "pub unsafe auto trait Send {} "
        "impl<T: PointeeSized> const !Send for *const T {} "
        "unsafe impl const Send for u8 {}";
    static const char *const rejected[] = {
        "trait Marker {} impl const !Marker for u8 {}",
        "unsafe auto trait Marker {} unsafe impl const !Marker for u8 {}",
        "auto trait Marker { fn f(); }",
        "auto trait Marker<T> {}",
        "trait Bound {} auto trait Marker: Bound {}",
        "auto trait Marker {} impl !Marker for u8 { fn f() {} }",
        "auto trait Marker {} impl<T> !Marker for *const T {} "
            "impl<U> !Marker for *const U {}",
        "unsafe auto trait Marker {} impl !Marker for u8 {} "
            "unsafe impl Marker for u8 {}"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL
    };
    CmAst ast;
    CmParseResult parse_result;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;
    const CmAstItemId *item_id;
    const CmAstItemId *impl_ast_id;
    CmAstItem *item;
    CmAstItem *impl_ast_item;
    CmAstTypeId saved_trait_type;
    const CmHirItem *pointee_sized;
    const CmHirItem *send;
    const CmHirItem *negative_impl;
    const CmHirItem *positive_impl;
    const CmHirGenericParam *parameter;
    const CmHirType *negative_self;
    const CmHirType *negative_pointee;
    const CmHirType *positive_self;
    size_t index;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    item_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    item = item_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*item_id - 1u);
    assert(parse_result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TRAIT
        && item->data.trait_item.is_auto == 1);

    cm_hir_lower_options_init(&options);
    options.crate_name = "auto_trait_test";
    options.source = 7u;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    pointee_sized = find_item(&context, "PointeeSized");
    send = find_item(&context, "Send");
    negative_impl = NULL;
    positive_impl = NULL;
    for (index = 0u; index < context.items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(&context.items,
            index);
        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_IMPL) {
            continue;
        }
        if (candidate->data.impl_item.is_negative) {
            negative_impl = candidate;
        } else {
            positive_impl = candidate;
        }
    }
    parameter = negative_impl == NULL
            || negative_impl->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            negative_impl->generic_parameter_start);
    negative_self = negative_impl == NULL ? NULL : cm_hir_get_type(&context,
        negative_impl->data.impl_item.self_type);
    negative_pointee = negative_self == NULL
            || negative_self->kind != CM_HIR_TYPE_RAW_POINTER_KIND
        ? NULL : cm_hir_get_type(&context,
            negative_self->data.raw_pointer_type.pointee);
    positive_self = positive_impl == NULL ? NULL : cm_hir_get_type(&context,
        positive_impl->data.impl_item.self_type);
    assert(result.error_count == 0u && result.lowered_item_count == 4u
        && pointee_sized != NULL
        && pointee_sized->kind == CM_HIR_ITEM_TRAIT
        && !pointee_sized->data.trait_item.is_auto
        && send != NULL && send->kind == CM_HIR_ITEM_TRAIT
        && send->data.trait_item.is_auto
        && send->data.trait_item.safety == CM_HIR_UNSAFE
        && send->generic_parameter_count == 0u
        && send->predicate_count == 0u
        && send->data.trait_item.supertrait_count == 0u
        && negative_impl != NULL
        && negative_impl->data.impl_item.is_negative
        && negative_impl->data.impl_item.has_trait
        && negative_impl->data.impl_item.safety == CM_HIR_SAFE
        && cm_hir_def_id_equal(
            negative_impl->data.impl_item.trait_type.definition,
            send->definition)
        && negative_impl->data.impl_item.trait_type.argument_count == 0u
        && negative_impl->data.impl_item.trait_type.arguments == NULL
        && negative_impl->predicate_count == 1u
        && cm_hir_def_id_equal(
            negative_impl->predicates[0].trait_type.definition,
            pointee_sized->definition)
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && negative_self != NULL
        && negative_self->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && negative_self->data.raw_pointer_type.mutability
            == CM_HIR_IMMUTABLE
        && negative_pointee != NULL
        && negative_pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
        && negative_pointee->data.parameter_type.parameter
            == negative_impl->generic_parameter_start
        && positive_impl != NULL
        && !positive_impl->data.impl_item.is_negative
        && positive_impl->data.impl_item.safety == CM_HIR_UNSAFE
        && positive_self != NULL
        && positive_self->kind == CM_HIR_TYPE_INTEGER_KIND
        && positive_self->data.integer_type.kind == CM_HIR_INT_U8);
    cm_hir_context_destroy(&context);

    item->data.trait_item.is_auto = 99;
    expect_invalid_ast_lowering(&ast, &options, "invalid auto flag");
    item->data.trait_item.is_auto = 1;
    impl_ast_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    impl_ast_item = impl_ast_id == NULL ? NULL
        : (CmAstItem *)cm_vec_at(&ast.items, (size_t)*impl_ast_id - 1u);
    assert(impl_ast_item != NULL && impl_ast_item->kind == CM_AST_ITEM_IMPL
        && impl_ast_item->data.impl_item.is_negative
        && impl_ast_item->data.impl_item.is_const == 1);
    impl_ast_item->data.impl_item.is_const = 99;
    expect_invalid_ast_lowering(&ast, &options, "invalid const flag");
    impl_ast_item->data.impl_item.is_const = 1;
    saved_trait_type = impl_ast_item->data.impl_item.trait_type;
    impl_ast_item->data.impl_item.trait_type = CM_AST_TYPE_NONE;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_IMPL
        && strstr(result.first_error.message, "must name") != NULL);
    cm_hir_context_destroy(&context);
    impl_ast_item->data.impl_item.trait_type = saved_trait_type;
    cm_ast_destroy(&ast);

    assert(sizeof(rejected) / sizeof(rejected[0])
        == sizeof(rejected_kinds) / sizeof(rejected_kinds[0]));
    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected_kinds[index]) {
            fprintf(stderr, "auto/negative rejection mismatch for %s: "
                "%s: %s\n", rejected[index],
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]);
        cm_hir_context_destroy(&context);
    }
}

static void test_unresolved_path_is_hard_error(void)
{
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source("struct Bad { value: Missing }", &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNRESOLVED_PATH);
    assert(result.first_error.type != CM_AST_TYPE_NONE);
    assert(result.first_error.path != CM_AST_PATH_NONE);
    assert(strstr(result.first_error.message, "unresolved") != NULL);
    assert(context.definitions.len != 0u);
    cm_hir_context_destroy(&context);
    assert(context.definitions.data == NULL);
    assert(context.types.data == NULL);
}

static void test_resolver_failure_is_distinct(void)
{
    CmHirContext context;
    CmHirLowerResult result;
    TestResolver resolver;

    memset(&resolver, 0, sizeof(resolver));
    resolver.fail = 1;
    result = lower_source("struct Bad { value: Missing }", &context,
        &resolver);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE);
    assert(resolver.calls == 1u);
    cm_hir_context_destroy(&context);

    memset(&resolver, 0, sizeof(resolver));
    resolver.return_absent_definition = 1;
    result = lower_source("struct Bad { value: Missing }", &context,
        &resolver);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE);
    assert(strstr(result.first_error.message, "absent") != NULL);
    cm_hir_context_destroy(&context);
}

static void test_unsupported_constructs_are_errors(void)
{
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *bad;
    const CmHirType *alias_type;

    result = lower_source("use missing::item;", &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM);
    cm_hir_context_destroy(&context);

    result = lower_source("struct Bad<const N: usize = 1>;", &context,
        NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC);
    assert(strstr(result.first_error.message, "plain const path")
        != NULL);
    cm_hir_context_destroy(&context);

    result = lower_source("struct Bad<T, T>;", &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_DUPLICATE_NAME);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "type Alias = u8; struct Bad { field: Alias }", &context, NULL);
    assert(result.error_count == 0u);
    bad = find_item(&context, "Bad");
    assert(bad != NULL && bad->kind == CM_HIR_ITEM_STRUCT
        && bad->data.aggregate_item.field_count == 1u);
    alias_type = cm_hir_get_type(&context,
        bad->data.aggregate_item.fields[0].type);
    assert(alias_type != NULL
        && alias_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && alias_type->data.integer_type.kind == CM_HIR_INT_U8);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "struct Bad { field: [u8; 18446744073709551616] }",
        &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_TYPE);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "fn consume() -> impl FnMut(u8) {}", &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_TYPE);
    assert(strstr(result.first_error.message, "opaque impl trait") != NULL);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "fn consume() -> impl Error + ?Sized {}", &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_TYPE);
    assert(strstr(result.first_error.message, "opaque impl trait") != NULL);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "fn consume() -> impl for<'a> Fn(&'a u8) {}", &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_TYPE);
    assert(strstr(result.first_error.message, "opaque impl trait") != NULL);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "trait Error {} fn source(error: &dyn Error) {}",
        &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_TYPE);
    assert(strstr(result.first_error.message, "dynamic trait") != NULL);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "trait Error {} fn source(error: &(dyn Error + 'static)) {}",
        &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_TYPE);
    assert(strstr(result.first_error.message, "dynamic trait") != NULL);
    cm_hir_context_destroy(&context);

    result = lower_source("#[repr(C)] struct Bad;", &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "mod marked { #![allow(dead_code)] struct Bad; }",
        &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM);
    assert(strstr(result.first_error.message, "module inner") != NULL);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "mod inactive { #![cfg(windows)] struct Bad; }",
        &context, NULL);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM);
    cm_hir_context_destroy(&context);

}

static void test_const_generic_path_default(void)
{
    static const char source[] =
        "struct Assume;"
        "const NOTHING: Assume = Assume;"
        "unsafe trait TransmuteFrom<Src, "
        "const ASSUME: Assume = { NOTHING }> where Src: ?Sized {}"
        "trait Repeat<const FIRST: usize, "
        "const SECOND: usize = FIRST> {}";
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *assume;
    const CmHirItem *nothing;
    const CmHirItem *trait_item;
    const CmHirGenericParam *source_parameter;
    const CmHirGenericParam *parameter;
    const CmHirItem *repeat;
    const CmHirGenericParam *first;
    const CmHirGenericParam *second;
    const CmHirType *declared_type;

    result = lower_source(source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "const-default lowering: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assume = find_item(&context, "Assume");
    nothing = find_item(&context, "NOTHING");
    trait_item = find_item(&context, "TransmuteFrom");
    source_parameter = trait_item == NULL
            || trait_item->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(&context,
            trait_item->generic_parameter_start);
    parameter = trait_item == NULL
            || trait_item->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(&context,
            trait_item->generic_parameter_start + 1u);
    declared_type = parameter == NULL ? NULL
        : cm_hir_get_type(&context, parameter->declared_type);
    repeat = find_item(&context, "Repeat");
    first = repeat == NULL || repeat->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(&context,
            repeat->generic_parameter_start);
    second = repeat == NULL || repeat->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(&context,
            repeat->generic_parameter_start + 1u);
    assert(result.error_count == 0u
        && assume != NULL && assume->kind == CM_HIR_ITEM_STRUCT
        && nothing != NULL && nothing->kind == CM_HIR_ITEM_CONST
        && trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
        && trait_item->predicate_count == 0u
        && source_parameter != NULL
        && source_parameter->kind == CM_HIR_GENERIC_TYPE
        && source_parameter->is_relaxed_sized
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_CONST
        && parameter->has_default
        && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_CONST
        && parameter->default_argument.data.constant.kind
            == CM_HIR_CONST_UNEVALUATED
        && parameter->default_argument.data.constant.type
            == parameter->declared_type
        && cm_hir_def_id_equal(
            parameter->default_argument.data.constant.data.definition,
            nothing->definition)
        && declared_type != NULL
        && declared_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(declared_type->data.named_type.definition,
            assume->definition)
        && first != NULL && first->kind == CM_HIR_GENERIC_CONST
        && !first->has_default
        && second != NULL && second->kind == CM_HIR_GENERIC_CONST
        && second->has_default
        && second->default_argument.kind == CM_HIR_GENERIC_ARG_CONST
        && second->default_argument.data.constant.kind
            == CM_HIR_CONST_PARAMETER
        && second->default_argument.data.constant.data.parameter
            == repeat->generic_parameter_start);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "trait Bad<const A: u8, const B: usize = A> {}",
        &context, NULL);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && strstr(result.first_error.message,
            "parameter type differs from its declared type") != NULL);
    cm_hir_context_destroy(&context);
}

static void test_const_generic_trait_method_declaration(void)
{
    static const char source[] =
        "trait SpecArrayClone: Clone {"
        " fn clone<const N: usize>(array: &[Self; N]) -> [Self; N];"
        "} trait Clone {}";
    static const char *const rejected[] = {
        "trait Bad { fn clone<const N: u8>(array: [u8; N]); }",
        "trait Bad { fn clone<N>(array: [u8; N]); }"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE
    };
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *owner;
    const CmHirItem *method;
    const CmHirGenericParam *parameter;
    const CmHirType *declared_type;
    const CmHirType *input_reference;
    const CmHirType *input_array;
    const CmHirType *output_array;
    const CmHirType *input_element;
    size_t index;

    result = lower_source(source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "const-generic trait method lowering: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    owner = find_item(&context, "SpecArrayClone");
    method = owner == NULL ? NULL
        : find_child(&context, owner->definition, "clone");
    parameter = method == NULL || method->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            method->generic_parameter_start);
    declared_type = parameter == NULL ? NULL
        : cm_hir_get_type(&context, parameter->declared_type);
    input_reference = method == NULL
            || method->data.function_item.signature.parameter_count != 1u
        ? NULL : cm_hir_get_type(&context,
            method->data.function_item.signature.parameters[0].type);
    input_array = input_reference == NULL
            || input_reference->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&context,
            input_reference->data.reference_type.pointee);
    output_array = method == NULL ? NULL : cm_hir_get_type(&context,
        method->data.function_item.signature.return_type);
    input_element = input_array == NULL
            || input_array->kind != CM_HIR_TYPE_ARRAY_KIND
        ? NULL : cm_hir_get_type(&context,
            input_array->data.array_type.element);
    assert(result.error_count == 0u
        && owner != NULL && owner->kind == CM_HIR_ITEM_TRAIT
        && method != NULL && method->kind == CM_HIR_ITEM_FUNCTION
        && method->data.function_item.body == CM_HIR_BODY_NONE
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_CONST
        && parameter->index == 0u && !parameter->has_default
        && declared_type != NULL
        && declared_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && declared_type->data.integer_type.kind == CM_HIR_INT_USIZE
        && input_reference != NULL
        && input_reference->kind == CM_HIR_TYPE_REFERENCE_KIND
        && input_array != NULL && input_array->kind == CM_HIR_TYPE_ARRAY_KIND
        && input_array->data.array_type.length.kind
            == CM_HIR_CONST_PARAMETER
        && input_array->data.array_type.length.type
            == parameter->declared_type
        && input_array->data.array_type.length.data.parameter
            == method->generic_parameter_start
        && input_element != NULL && input_element->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(input_element->data.self_type.owner,
            owner->definition)
        && output_array != NULL
        && output_array->kind == CM_HIR_TYPE_ARRAY_KIND
        && output_array->data.array_type.length.kind
            == CM_HIR_CONST_PARAMETER
        && output_array->data.array_type.length.type
            == parameter->declared_type
        && output_array->data.array_type.length.data.parameter
            == method->generic_parameter_start);
    cm_hir_context_destroy(&context);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected_kinds[index]) {
            fprintf(stderr, "const-generic rejection %u: errors=%u "
                "kind=%s message=%s\n", (unsigned int)index,
                (unsigned int)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]);
        cm_hir_context_destroy(&context);
    }
}

static void test_generic_parameter_shadows_type_path_prefix(void)
{
    static const char *const rejected[] = {
        "mod T { type Assoc=u8; } type P<T> = T::Assoc;",
        ("mod T { pub trait Trait { type Assoc; } } "
            "type P<T, U> = <U as T::Trait>::Assoc;"),
        "trait T { type Assoc; } type P<T, U> = <U as T>::Assoc;"
    };
    static const char *const accepted[] = {
        "mod T { pub type Assoc = u8; } type P<U> = T::Assoc;",
        "mod T { pub type Assoc = u8; } type P<'T> = T::Assoc;",
        ("mod T { pub trait Trait { type Assoc; } } "
            "type P<U> = <U as T::Trait>::Assoc;")
    };
    static const char nested[] =
        "trait Trait { type Assoc; } trait Other { type Output; } "
        "type P<T> = <<T as Trait>::Assoc as Other>::Output;";
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *item;
    const CmHirType *outer;
    const CmHirType *inner;
    size_t index;

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        assert(result.error_count == 1u);
        assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_TYPE);
        assert(strstr(result.first_error.message, "shadows") != NULL);
        cm_hir_context_destroy(&context);
    }
    for (index = 0u; index < sizeof(accepted) / sizeof(accepted[0]);
         ++index) {
        result = lower_source(accepted[index], &context, NULL);
        assert(result.error_count == 0u);
        cm_hir_context_destroy(&context);
    }
    result = lower_source(nested, &context, NULL);
    assert(result.error_count == 0u);
    item = find_item(&context, "P");
    outer = item == NULL ? NULL : cm_hir_get_type(&context,
        item->data.type_alias_item.target);
    inner = outer == NULL || outer->kind != CM_HIR_TYPE_PROJECTION_KIND
        ? NULL : cm_hir_get_type(&context,
            outer->data.projection_type.self_type);
    assert(item != NULL && item->kind == CM_HIR_ITEM_TYPE_ALIAS
        && outer != NULL && outer->kind == CM_HIR_TYPE_PROJECTION_KIND
        && inner != NULL && inner->kind == CM_HIR_TYPE_PROJECTION_KIND);
    cm_hir_context_destroy(&context);
}

static void make_cfg_view(const char *source, CmAst *ast,
    CmExpandedAst *expanded)
{
    CmParseResult parse_result;
    CmCfgSet cfg;
    CmExpandOptions expand_options;
    CmExpandResult expand_result;

    cm_ast_init(ast);
    parse_result = cm_parse_crate(ast, source, strlen(source),
        CM_EDITION_2024);
    assert(parse_result.error_count == 0u);
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cfg.environment.target_os = "linux";
    cm_expand_options_init(&expand_options, &cfg);
    cm_expanded_ast_init(expanded);
    expand_result = cm_expand_cfg_view(ast, &expand_options, expanded);
    assert(expand_result.status == CM_MACRO_OK);
}

static CmHirLowerResult lower_cfg_view(CmHirContext *context,
    const CmAst *ast, const CmExpandedAst *expanded)
{
    CmHirLowerOptions options;

    cm_hir_context_init(context);
    cm_hir_lower_options_init(&options);
    options.crate_name = "expanded_test";
    options.source = 11u;
    return cm_hir_lower_expanded_crate(context, ast, expanded, &options);
}

static void check_adt_default_visible_to_trait_method(
    const CmHirContext *context, const CmHirLowerResult *result)
{
    const CmHirItem *control_flow;
    const CmHirItem *trait_item;
    const CmHirItem *method;
    const CmHirType *return_type;
    const CmHirType *default_type;

    control_flow = find_item(context, "ControlFlow");
    trait_item = find_item(context, "UsesControlFlow");
    method = trait_item == NULL ? NULL
        : find_child(context, trait_item->definition, "f");
    return_type = method == NULL ? NULL : cm_hir_get_type(context,
        method->data.function_item.signature.return_type);
    default_type = return_type == NULL
            || return_type->kind != CM_HIR_TYPE_ADT_KIND
            || return_type->data.named_type.argument_count != 2u
        ? NULL : cm_hir_get_type(context,
            return_type->data.named_type.arguments[1].data.type);
    assert(result->error_count == 0u && control_flow != NULL
        && trait_item != NULL && method != NULL && return_type != NULL
        && cm_hir_def_id_equal(return_type->data.named_type.definition,
            control_flow->definition)
        && default_type != NULL
        && default_type->kind == CM_HIR_TYPE_UNIT_KIND);
}

static void test_adt_default_declaration_order_entry_points(void)
{
    static const char source[] =
        "enum ControlFlow<B, C = ()> { Continue(C), Break(B) }"
        "trait UsesControlFlow { fn f() -> ControlFlow<bool>; }";
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    check_adt_default_visible_to_trait_method(&context, &result);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    check_adt_default_visible_to_trait_method(&context, &result);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void check_discard_parameter_shape(const CmHirContext *context,
    const CmHirItem *function, int expect_body)
{
    const CmHirFunctionSignature *signature;
    const CmHirBody *body;

    assert(function != NULL && function->kind == CM_HIR_ITEM_FUNCTION);
    signature = &function->data.function_item.signature;
    assert(signature->parameter_count == 2u
        && signature->parameters != NULL
        && signature->parameters[0].binding_kind == CM_HIR_BINDING_DISCARD
        && signature->parameters[0].name == CM_INTERN_ID_NONE
        && signature->parameters[1].binding_kind == CM_HIR_BINDING_NAMED
        && hir_string_is(context, signature->parameters[1].name, "value"));
    if (!expect_body) {
        assert(function->data.function_item.body == CM_HIR_BODY_NONE);
        return;
    }
    body = cm_hir_get_body(context, function->data.function_item.body);
    assert(body != NULL && body->parameter_count == 2u
        && body->local_count == 1u && body->locals != NULL
        && body->locals[0].parameter_index == 1u
        && hir_string_is(context, body->locals[0].name, "value")
        && body->locals[0].mutability == CM_HIR_IMMUTABLE
        && body->locals[0].type == signature->parameters[1].type);
}

static void check_discard_parameter_lowering(const CmHirContext *context)
{
    const CmHirItem *function;
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *trait_method;
    const CmHirItem *impl_method;

    function = find_item(context, "anonymous");
    trait_item = find_item(context, "T");
    impl_item = find_impl(context);
    trait_method = trait_item == NULL ? NULL
        : find_child(context, trait_item->definition, "call");
    impl_method = impl_item == NULL ? NULL
        : find_child(context, impl_item->definition, "call");
    check_discard_parameter_shape(context, function, 1);
    check_discard_parameter_shape(context, trait_method, 0);
    check_discard_parameter_shape(context, impl_method, 1);
}

static void test_discard_parameter_entry_points(void)
{
    static const char source[] =
        "fn anonymous(_: u8, value: u8) {}"
        "trait T { fn call(_: u8, value: u8); }"
        "impl T for u8 { fn call(_: u8, value: u8) {} }";
    CmHirContext context;
    CmHirLowerResult result;
    CmAst ast;
    CmExpandedAst expanded;

    result = lower_source(source, &context, NULL);
    assert(result.error_count == 0u);
    check_discard_parameter_lowering(&context);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u);
    check_discard_parameter_lowering(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void check_supertrait_lowering(const CmHirContext *context)
{
    const CmHirItem *parent;
    const CmHirItem *conditional;
    const CmHirItem *child;

    parent = find_item(context, "Parent");
    conditional = find_item(context, "Conditional");
    child = find_item(context, "Child");
    assert(parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT
        && conditional != NULL && conditional->kind == CM_HIR_ITEM_TRAIT
        && child != NULL && child->kind == CM_HIR_ITEM_TRAIT
        && child->data.trait_item.supertrait_count == 2u
        && child->data.trait_item.supertraits != NULL
        && child->data.trait_item.supertraits[0].modifier
            == CM_HIR_SUPERTRAIT_REQUIRED
        && cm_hir_def_id_equal(child->data.trait_item.supertraits[0]
                .trait_type.definition,
            parent->definition)
        && child->data.trait_item.supertraits[1].modifier
            == CM_HIR_SUPERTRAIT_CONST_IF_CONST
        && cm_hir_def_id_equal(child->data.trait_item.supertraits[1]
                .trait_type.definition,
            conditional->definition)
        && child->data.trait_item.supertraits[0].span.start
            < child->data.trait_item.supertraits[1].span.start);
}

static void test_supertrait_entry_points(void)
{
    static const char source[] =
        "trait Parent {}"
        "trait Child: Parent + ~const Conditional {}"
        "trait Conditional {}";
    static const char *const rejected[] = {
        "trait Parent {} trait Child: Parent + Parent {}",
        "trait Direct: Direct {}",
        "trait A: B {} trait B: C {} trait C: A {}",
        "struct NotTrait; trait Child: NotTrait {}",
        "trait Parent {} trait Child: Parent<u8> {}"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_WRONG_NAMESPACE,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC
    };
    CmHirContext context;
    CmHirLowerResult result;
    CmAst ast;
    CmExpandedAst expanded;
    size_t index;

    result = lower_source(source, &context, NULL);
    assert(result.error_count == 0u && result.lowered_item_count == 3u);
    check_supertrait_lowering(&context);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u && result.lowered_item_count == 3u);
    check_supertrait_lowering(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected_kinds[index]) {
            fprintf(stderr,
                "supertrait rejection mismatch for %s: count=%lu "
                "kind=%s message=%s\n",
                rejected[index], (unsigned long)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]);
        cm_hir_context_destroy(&context);
    }
}

static void check_defaulted_alias_result(const CmHirContext *context)
{
    const CmHirItem *alias;
    const CmHirItem *uses;
    const CmHirGenericParam *parameter;
    const CmHirType *type;

    alias = find_item(context, "Defaulted");
    uses = find_item(context, "Uses");
    assert(alias != NULL && alias->kind == CM_HIR_ITEM_TYPE_ALIAS
        && alias->generic_parameter_count == 1u);
    assert(uses != NULL && uses->kind == CM_HIR_ITEM_STRUCT
        && uses->data.aggregate_item.field_count == 1u);
    parameter = cm_hir_get_generic_param(context,
        alias->generic_parameter_start);
    assert(parameter != NULL && parameter->has_default
        && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE);
    type = cm_hir_get_type(context, parameter->default_argument.data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U16);
    type = cm_hir_get_type(context,
        uses->data.aggregate_item.fields[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U16);
}

static void test_defaulted_alias_entry_points(void)
{
    static const char source[] =
        "type Defaulted<T = u16> = T;"
        "struct Uses { value: Defaulted }";
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    assert(result.error_count == 0u);
    check_defaulted_alias_result(&context);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u);
    check_defaulted_alias_result(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void check_explicit_projection_result(const CmHirContext *context)
{
    const CmHirItem *trait_item;
    const CmHirItem *associated_item;
    const CmHirItem *project_item;
    const CmHirItem *concrete_item;
    const CmHirItem *defaulted_item;
    const CmHirItem *default_concrete_item;
    const CmHirType *project_type;
    const CmHirType *concrete_type;
    const CmHirType *default_concrete_type;
    const CmHirType *self_type;
    const CmHirType *default_self_type;

    trait_item = find_item(context, "Trait");
    associated_item = find_item(context, "Assoc");
    project_item = find_item(context, "Project");
    concrete_item = find_item(context, "Concrete");
    defaulted_item = find_item(context, "Defaulted");
    default_concrete_item = find_item(context, "DefaultConcrete");
    assert(trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT);
    assert(associated_item != NULL
        && associated_item->kind == CM_HIR_ITEM_TYPE_ALIAS
        && associated_item->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && cm_hir_def_id_equal(associated_item->parent_definition,
            trait_item->definition));
    assert(project_item != NULL
        && project_item->kind == CM_HIR_ITEM_TYPE_ALIAS);
    assert(concrete_item != NULL
        && concrete_item->kind == CM_HIR_ITEM_TYPE_ALIAS);
    assert(defaulted_item != NULL
        && defaulted_item->kind == CM_HIR_ITEM_TYPE_ALIAS
        && defaulted_item->generic_parameter_count == 2u);
    assert(default_concrete_item != NULL
        && default_concrete_item->kind == CM_HIR_ITEM_TYPE_ALIAS);
    project_type = cm_hir_get_type(context,
        project_item->data.type_alias_item.target);
    concrete_type = cm_hir_get_type(context,
        concrete_item->data.type_alias_item.target);
    assert(project_type != NULL
        && project_type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(
            project_type->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(
            project_type->data.projection_type.associated_type.definition,
            associated_item->definition));
    assert(concrete_type != NULL
        && concrete_type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(
            concrete_type->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(
            concrete_type->data.projection_type.associated_type.definition,
            associated_item->definition));
    self_type = cm_hir_get_type(context,
        concrete_type->data.projection_type.self_type);
    assert(self_type != NULL && self_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && self_type->data.integer_type.kind == CM_HIR_INT_U16);
    default_concrete_type = cm_hir_get_type(context,
        default_concrete_item->data.type_alias_item.target);
    assert(default_concrete_type != NULL
        && default_concrete_type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(
            default_concrete_type->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(default_concrete_type->data.projection_type
                .associated_type.definition,
            associated_item->definition));
    default_self_type = cm_hir_get_type(context,
        default_concrete_type->data.projection_type.self_type);
    assert(default_self_type != NULL
        && default_self_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && default_self_type->data.integer_type.kind == CM_HIR_INT_U32);
}

static void test_explicit_projection_entry_points(void)
{
    static const char source[] =
        "type Concrete = Project<u16>;"
        "type DefaultConcrete = Defaulted<u32>;"
        "type Defaulted<T, U = <T as Trait>::Assoc> = U;"
        "type Project<T> = <T as Trait>::Assoc;"
        "trait Trait { type Assoc; }";
    static const char *const rejected[] = {
        "type Free;",
        "trait Trait { type Assoc = u8; }",
        "trait Trait: Parent { type Assoc; }",
        "struct NotTrait; type P<T> = <T as NotTrait>::Assoc;",
        "trait Trait { type Other; } type P<T> = <T as Trait>::Assoc;",
        "trait Trait { type Assoc; } type P<T> = <T as Trait<u8>>::Assoc;",
        "trait Trait { type Assoc; } type P<T> = <T as Trait>::Assoc<u8>;",
        "trait Trait { type Assoc; } type P<T> = T::Assoc;",
        "impl u8 { type Assoc = u8; }"
    };
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    size_t index;

    result = lower_source(source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "projection lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 6u);
    check_explicit_projection_result(&context);
    cm_hir_context_destroy(&context);

    result = lower_source("trait Trait<T> { type Assoc; }", &context,
        NULL);
    assert(result.error_count == 0u && result.lowered_item_count == 2u);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    if (result.error_count != 0u) {
        fprintf(stderr, "expanded projection lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 6u);
    check_explicit_projection_result(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        assert(result.error_count == 1u);
        assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM
            || result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
            || result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_TYPE
            || result.first_error.kind == CM_HIR_LOWER_UNRESOLVED_PATH
            || result.first_error.kind == CM_HIR_LOWER_WRONG_NAMESPACE);
        cm_hir_context_destroy(&context);
    }
}

static void check_cross_trait_projection_default(
    const CmHirContext *context)
{
    const CmHirItem *try_item;
    const CmHirItem *residual_item;
    const CmHirItem *from_residual_item;
    const CmHirGenericParam *parameter;
    const CmHirType *projection;
    const CmHirType *self_type;
    const CmHirDefinition *definition;
    size_t index;
    uint32_t residual_count;

    try_item = find_item(context, "Try");
    from_residual_item = find_item(context, "FromResidual");
    residual_item = try_item == NULL ? NULL
        : find_child(context, try_item->definition, "Residual");
    parameter = from_residual_item == NULL
            || from_residual_item->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(context,
            from_residual_item->generic_parameter_start);
    projection = parameter == NULL || !parameter->has_default
            || parameter->default_argument.kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            parameter->default_argument.data.type);
    self_type = projection == NULL
            || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
        ? NULL : cm_hir_get_type(context,
            projection->data.projection_type.self_type);
    residual_count = 0u;
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        if (item != NULL && residual_item != NULL
            && cm_hir_def_id_equal(item->definition,
                residual_item->definition)) {
            residual_count += 1u;
        }
    }
    definition = residual_item == NULL ? NULL
        : cm_hir_lookup_definition(context, residual_item->definition);
    assert(context->items.len == 3u
        && try_item != NULL && try_item->kind == CM_HIR_ITEM_TRAIT
        && residual_item != NULL
        && residual_item->kind == CM_HIR_ITEM_TYPE_ALIAS
        && residual_item->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && from_residual_item != NULL
        && from_residual_item->kind == CM_HIR_ITEM_TRAIT
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(parameter->owner,
            from_residual_item->definition)
        && projection != NULL
        && projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(
            projection->data.projection_type.trait_type.definition,
            try_item->definition)
        && cm_hir_def_id_equal(
            projection->data.projection_type.associated_type.definition,
            residual_item->definition)
        && self_type != NULL && self_type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(self_type->data.self_type.owner,
            from_residual_item->definition)
        && definition != NULL
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->entity.item_id != CM_HIR_ITEM_NONE
        && residual_count == 1u);
}

static void check_associated_default_self_subject(
    const CmHirContext *context)
{
    const CmHirItem *base;
    const CmHirItem *owner;
    const CmHirItem *defaulted;
    const CmHirItem *explicit_self;
    const CmHirAssociatedTypeBound *defaulted_bound;
    const CmHirAssociatedTypeBound *explicit_bound;
    const CmHirType *projection;
    const CmHirType *projection_self;
    const CmHirType *explicit_type;

    base = find_item(context, "Base");
    owner = find_item(context, "Owner");
    defaulted = owner == NULL ? NULL
        : find_child(context, owner->definition, "Defaulted");
    explicit_self = owner == NULL ? NULL
        : find_child(context, owner->definition, "ExplicitSelf");
    defaulted_bound = defaulted == NULL
            || defaulted->data.type_alias_item.bound_count != 1u
        ? NULL : &defaulted->data.type_alias_item.bounds[0];
    explicit_bound = explicit_self == NULL
            || explicit_self->data.type_alias_item.bound_count != 1u
        ? NULL : &explicit_self->data.type_alias_item.bounds[0];
    projection = defaulted_bound == NULL
            || defaulted_bound->trait_type.argument_count != 1u
            || defaulted_bound->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            defaulted_bound->trait_type.arguments[0].data.type);
    projection_self = projection == NULL
            || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
        ? NULL : cm_hir_get_type(context,
            projection->data.projection_type.self_type);
    explicit_type = explicit_bound == NULL
            || explicit_bound->trait_type.argument_count != 1u
            || explicit_bound->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            explicit_bound->trait_type.arguments[0].data.type);
    assert(context->items.len == 4u
        && base != NULL && base->kind == CM_HIR_ITEM_TRAIT
        && owner != NULL && owner->kind == CM_HIR_ITEM_TRAIT
        && defaulted != NULL && explicit_self != NULL
        && defaulted_bound != NULL && explicit_bound != NULL
        && cm_hir_def_id_equal(defaulted_bound->trait_type.definition,
            base->definition)
        && cm_hir_def_id_equal(explicit_bound->trait_type.definition,
            base->definition)
        && projection != NULL
        && projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(projection->data.projection_type
                .trait_type.definition,
            owner->definition)
        && cm_hir_def_id_equal(projection->data.projection_type
                .associated_type.definition,
            defaulted->definition)
        && projection_self != NULL
        && projection_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(projection_self->data.self_type.owner,
            owner->definition)
        && explicit_type != NULL
        && explicit_type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(explicit_type->data.self_type.owner,
            owner->definition)
        && defaulted_bound->trait_type.arguments[0].data.type
            != explicit_bound->trait_type.arguments[0].data.type);
}

static void check_defaulted_supertrait_after_prebinding(
    const CmHirContext *context)
{
    const CmHirItem *base;
    const CmHirItem *child;
    const CmHirGenericArg *argument;
    const CmHirType *type;

    base = find_item(context, "Base");
    child = find_item(context, "Child");
    argument = child == NULL
            || child->data.trait_item.supertrait_count != 1u
            || child->data.trait_item.supertraits[0]
                .trait_type.argument_count != 1u
        ? NULL : &child->data.trait_item.supertraits[0]
            .trait_type.arguments[0];
    type = argument == NULL || argument->kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context, argument->data.type);
    assert(base != NULL && base->kind == CM_HIR_ITEM_TRAIT
        && child != NULL && child->kind == CM_HIR_ITEM_TRAIT
        && cm_hir_def_id_equal(child->data.trait_item.supertraits[0]
                .trait_type.definition,
            base->definition)
        && type != NULL && type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(type->data.self_type.owner,
            child->definition));
}

static void check_structural_projection_default_use(
    const CmHirContext *context)
{
    const CmHirItem *try_item;
    const CmHirItem *residual_item;
    const CmHirItem *from_residual_item;
    const CmHirItem *marker_item;
    const CmHirGenericParam *parameter;
    const CmHirSupertrait *supertrait;
    const CmHirType *source_projection;
    const CmHirType *instantiated_projection;
    const CmHirType *source_self;
    const CmHirType *instantiated_self;
    const CmHirType *equality_value;

    try_item = find_item(context, "Try");
    from_residual_item = find_item(context, "FromResidual");
    residual_item = try_item == NULL ? NULL
        : find_child(context, try_item->definition, "Residual");
    marker_item = from_residual_item == NULL ? NULL
        : find_child(context, from_residual_item->definition, "Marker");
    parameter = from_residual_item == NULL
            || from_residual_item->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(context,
            from_residual_item->generic_parameter_start);
    source_projection = parameter == NULL || !parameter->has_default
            || parameter->default_argument.kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            parameter->default_argument.data.type);
    supertrait = try_item == NULL
            || try_item->data.trait_item.supertrait_count != 1u
        ? NULL : &try_item->data.trait_item.supertraits[0];
    instantiated_projection = supertrait == NULL
            || supertrait->trait_type.argument_count != 1u
            || supertrait->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            supertrait->trait_type.arguments[0].data.type);
    source_self = source_projection == NULL
            || source_projection->kind != CM_HIR_TYPE_PROJECTION_KIND
        ? NULL : cm_hir_get_type(context,
            source_projection->data.projection_type.self_type);
    instantiated_self = instantiated_projection == NULL
            || instantiated_projection->kind != CM_HIR_TYPE_PROJECTION_KIND
        ? NULL : cm_hir_get_type(context,
            instantiated_projection->data.projection_type.self_type);
    equality_value = supertrait == NULL || supertrait->equality_count != 1u
        ? NULL : cm_hir_get_type(context, supertrait->equalities[0].value);
    assert(try_item != NULL && try_item->kind == CM_HIR_ITEM_TRAIT
        && residual_item != NULL
        && from_residual_item != NULL
        && from_residual_item->kind == CM_HIR_ITEM_TRAIT
        && marker_item != NULL
        && source_projection != NULL
        && source_projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && instantiated_projection != NULL
        && instantiated_projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && parameter->default_argument.data.type
            != supertrait->trait_type.arguments[0].data.type
        && source_projection->span.source
            == instantiated_projection->span.source
        && source_projection->span.start
            == instantiated_projection->span.start
        && source_projection->span.end == instantiated_projection->span.end
        && source_self != NULL && source_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(source_self->data.self_type.owner,
            from_residual_item->definition)
        && instantiated_self != NULL
        && instantiated_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(instantiated_self->data.self_type.owner,
            try_item->definition)
        && cm_hir_def_id_equal(instantiated_projection->data.projection_type
                .trait_type.definition,
            try_item->definition)
        && instantiated_projection->data.projection_type.trait_type
                .argument_count
            == source_projection->data.projection_type.trait_type
                .argument_count
        && cm_hir_def_id_equal(instantiated_projection->data.projection_type
                .associated_type.definition,
            residual_item->definition)
        && instantiated_projection->data.projection_type.associated_type
                .argument_count
            == source_projection->data.projection_type.associated_type
                .argument_count
        && supertrait->modifier == CM_HIR_SUPERTRAIT_CONST_IF_CONST
        && cm_hir_def_id_equal(supertrait->trait_type.definition,
            from_residual_item->definition)
        && supertrait->equality_count == 1u
        && cm_hir_def_id_equal(supertrait->equalities[0].associated_type,
            marker_item->definition)
        && equality_value != NULL
        && equality_value->kind == CM_HIR_TYPE_UNIT_KIND);
}

static void check_structural_tuple_default_sharing(
    const CmHirContext *context)
{
    const CmHirItem *base;
    const CmHirItem *child;
    const CmHirGenericParam *parameter;
    const CmHirType *source_tuple;
    const CmHirType *instantiated_tuple;
    const CmHirType *instantiated_self;
    const CmHirSupertrait *supertrait;

    base = find_item(context, "Base");
    child = find_item(context, "Child");
    parameter = base == NULL || base->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(context,
            base->generic_parameter_start);
    source_tuple = parameter == NULL || !parameter->has_default
            || parameter->default_argument.kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            parameter->default_argument.data.type);
    supertrait = child == NULL
            || child->data.trait_item.supertrait_count != 1u
        ? NULL : &child->data.trait_item.supertraits[0];
    instantiated_tuple = supertrait == NULL
            || supertrait->trait_type.argument_count != 1u
            || supertrait->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            supertrait->trait_type.arguments[0].data.type);
    instantiated_self = instantiated_tuple == NULL
            || instantiated_tuple->kind != CM_HIR_TYPE_TUPLE_KIND
            || instantiated_tuple->data.tuple_type.element_count != 3u
        ? NULL : cm_hir_get_type(context,
            instantiated_tuple->data.tuple_type.elements[0]);
    assert(base != NULL && child != NULL
        && source_tuple != NULL
        && source_tuple->kind == CM_HIR_TYPE_TUPLE_KIND
        && source_tuple->data.tuple_type.element_count == 3u
        && instantiated_tuple != NULL
        && instantiated_tuple->kind == CM_HIR_TYPE_TUPLE_KIND
        && instantiated_tuple->data.tuple_type.element_count == 3u
        && parameter->default_argument.data.type
            != supertrait->trait_type.arguments[0].data.type
        && instantiated_tuple->data.tuple_type.elements[0]
            == instantiated_tuple->data.tuple_type.elements[2]
        && instantiated_tuple->data.tuple_type.elements[1]
            == source_tuple->data.tuple_type.elements[1]
        && instantiated_self != NULL
        && instantiated_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(instantiated_self->data.self_type.owner,
            child->definition));
}

static void check_structural_lifetime_default_use(
    const CmHirContext *context)
{
    const CmHirItem *base;
    const CmHirItem *child;
    const CmHirGenericParam *base_default;
    const CmHirGenericParam *child_lifetime;
    const CmHirSupertrait *supertrait;
    const CmHirType *source_reference;
    const CmHirType *instantiated_reference;

    base = find_item(context, "Base");
    child = find_item(context, "Child");
    base_default = base == NULL || base->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(context,
            base->generic_parameter_start + 1u);
    child_lifetime = child == NULL || child->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(context,
            child->generic_parameter_start);
    source_reference = base_default == NULL || !base_default->has_default
            || base_default->default_argument.kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            base_default->default_argument.data.type);
    supertrait = child == NULL
            || child->data.trait_item.supertrait_count != 1u
        ? NULL : &child->data.trait_item.supertraits[0];
    instantiated_reference = supertrait == NULL
            || supertrait->trait_type.argument_count != 2u
            || supertrait->trait_type.arguments[1].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            supertrait->trait_type.arguments[1].data.type);
    assert(base != NULL && child != NULL
        && base_default != NULL && child_lifetime != NULL
        && child_lifetime->kind == CM_HIR_GENERIC_LIFETIME
        && source_reference != NULL
        && source_reference->kind == CM_HIR_TYPE_REFERENCE_KIND
        && source_reference->data.reference_type.region.kind
            == CM_HIR_REGION_EARLY_BOUND
        && instantiated_reference != NULL
        && instantiated_reference->kind == CM_HIR_TYPE_REFERENCE_KIND
        && base_default->default_argument.data.type
            != supertrait->trait_type.arguments[1].data.type
        && supertrait->trait_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && supertrait->trait_type.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && supertrait->trait_type.arguments[0].data.lifetime.data.parameter
            == child->generic_parameter_start
        && instantiated_reference->data.reference_type.region.kind
            == CM_HIR_REGION_EARLY_BOUND
        && instantiated_reference->data.reference_type.region.data.parameter
            == child->generic_parameter_start
        && instantiated_reference->data.reference_type.pointee
            == source_reference->data.reference_type.pointee
        && instantiated_reference->span.start == source_reference->span.start
        && instantiated_reference->span.end == source_reference->span.end);
}

static char *make_nested_trait_default_source(size_t depth)
{
    static const char prefix[] = "trait Base<T = ";
    static const char nesting[] = "&'static ";
    static const char middle[] = "Self";
    static const char suffix[] = "> {} trait Child: Base {}";
    size_t length;
    size_t offset;
    size_t index;
    char *source;

    length = sizeof(prefix) - 1u + depth * (sizeof(nesting) - 1u)
        + sizeof(middle) - 1u + sizeof(suffix);
    source = (char *)malloc(length);
    assert(source != NULL);
    offset = 0u;
    memcpy(source + offset, prefix, sizeof(prefix) - 1u);
    offset += sizeof(prefix) - 1u;
    for (index = 0u; index < depth; ++index) {
        memcpy(source + offset, nesting, sizeof(nesting) - 1u);
        offset += sizeof(nesting) - 1u;
    }
    memcpy(source + offset, middle, sizeof(middle) - 1u);
    offset += sizeof(middle) - 1u;
    memcpy(source + offset, suffix, sizeof(suffix));
    assert(offset + sizeof(suffix) == length);
    return source;
}

static void check_primitive_defaulted_supertrait(
    const CmHirContext *context)
{
    const CmHirItem *child;
    const CmHirGenericArg *argument;
    const CmHirType *type;

    child = find_item(context, "PrimitiveChild");
    argument = child == NULL
            || child->data.trait_item.supertrait_count != 1u
            || child->data.trait_item.supertraits[0]
                .trait_type.argument_count != 1u
        ? NULL : &child->data.trait_item.supertraits[0]
            .trait_type.arguments[0];
    type = argument == NULL || argument->kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context, argument->data.type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U8);
}

static void test_cross_trait_projection_default_entry_points(void)
{
    static const char *const accepted[] = {
        "trait Try { type Residual; } "
        "trait FromResidual<R = <Self as Try>::Residual> {}",
        "trait FromResidual<R = <Self as Try>::Residual> {} "
        "trait Try { type Residual; }"
    };
    static const char *const rejected[] = {
        "trait FromResidual<R = <Self as Try>::Residual> {} "
        "trait Try { type Other; }",
        "trait Bad<T = T> {}",
        "trait FromResidual<R = <Self as Try>::Residual> {} "
        "trait Try { type Residual = u8; }",
        "trait Base<T = _> {} trait Child: Base {}",
        "trait Base<T = &u8> {} trait Child: Base {}",
        "trait Base<'a> {} trait Child<'b>: Base<'_> {}",
        "trait Base<'a> {} trait Child<'b>: Base<'a> {}",
        "trait Base<T = Self> { type A; } "
        "trait Child: Base<Missing = ()> {}"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_UNRESOLVED_PATH
    };
    static const char *const rejected_messages[] = {
        "no associated type",
        "references itself or a later parameter",
        "associated type defaults",
        "unresolved or unnameable HIR type",
        "unresolved, erased, or unauthenticated bound lifetime",
        "must be 'static or an authenticated early- or late-bound lifetime",
        "undeclared lifetime",
        "no associated type named by equality"
    };
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    TestResolver resolver;
    const CmHirItem *shared_base;
    const CmHirItem *shared_child;
    const CmHirGenericParam *shared_parameter;
    const CmHirType *shared_source_tuple;
    const CmHirType *shared_instantiated_tuple;
    size_t index;
    char *nested_source;

    for (index = 0u; index < sizeof(accepted) / sizeof(accepted[0]);
         ++index) {
        result = lower_source(accepted[index], &context, NULL);
        assert(result.error_count == 0u
            && result.lowered_item_count == 3u);
        check_cross_trait_projection_default(&context);
        cm_hir_context_destroy(&context);

        make_cfg_view(accepted[index], &ast, &expanded);
        result = lower_cfg_view(&context, &ast, &expanded);
        assert(result.error_count == 0u
            && result.lowered_item_count == 3u);
        check_cross_trait_projection_default(&context);
        cm_hir_context_destroy(&context);
        cm_expanded_ast_destroy(&expanded);
        cm_ast_destroy(&ast);
    }
    result = lower_source(
        "trait Base<Rhs = Self> {} trait Owner { "
        "type Defaulted: Base; type ExplicitSelf: Base<Self>; }",
        &context, NULL);
    assert(result.error_count == 0u && result.lowered_item_count == 4u);
    check_associated_default_self_subject(&context);
    cm_hir_context_destroy(&context);
    make_cfg_view(
        "trait Base<Rhs = Self> {} trait Owner { "
        "type Defaulted: Base; type ExplicitSelf: Base<Self>; }",
        &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u && result.lowered_item_count == 4u);
    check_associated_default_self_subject(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    result = lower_source(
        "trait Base<Rhs = Self> {} trait Child: Base {}",
        &context, NULL);
    assert(result.error_count == 0u && result.lowered_item_count == 2u);
    check_defaulted_supertrait_after_prebinding(&context);
    cm_hir_context_destroy(&context);
    make_cfg_view("trait Base<Rhs = Self> {} trait Child: Base {}",
        &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u && result.lowered_item_count == 2u);
    check_defaulted_supertrait_after_prebinding(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    for (index = 0u; index < 2u; ++index) {
        const char *primitive_source;

        primitive_source = index == 0u
            ? "trait PrimitiveBase<Rhs = u8> {} "
              "trait PrimitiveChild: PrimitiveBase {}"
            : "trait PrimitiveChild: PrimitiveBase {} "
              "trait PrimitiveBase<Rhs = u8> {}";
        result = lower_source(primitive_source, &context, NULL);
        assert(result.error_count == 0u
            && result.lowered_item_count == 2u);
        check_primitive_defaulted_supertrait(&context);
        cm_hir_context_destroy(&context);
        make_cfg_view(primitive_source, &ast, &expanded);
        result = lower_cfg_view(&context, &ast, &expanded);
        assert(result.error_count == 0u
            && result.lowered_item_count == 2u);
        check_primitive_defaulted_supertrait(&context);
        cm_hir_context_destroy(&context);
        cm_expanded_ast_destroy(&expanded);
        cm_ast_destroy(&ast);
    }

    result = lower_source(
        "trait FromResidual<R = <Self as Try>::Residual> { type Marker; } "
        "trait Try: ~const FromResidual<Marker = ()> { type Residual; }",
        &context, NULL);
    assert(result.error_count == 0u && result.lowered_item_count == 4u);
    check_structural_projection_default_use(&context);
    cm_hir_context_destroy(&context);
    make_cfg_view(
        "trait FromResidual<R = <Self as Try>::Residual> { type Marker; } "
        "trait Try: ~const FromResidual<Marker = ()> { type Residual; }",
        &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u && result.lowered_item_count == 4u);
    check_structural_projection_default_use(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    result = lower_source(
        "trait Base<T = (Self, u8, Self)> {} trait Child: Base {}",
        &context, NULL);
    assert(result.error_count == 0u && result.lowered_item_count == 2u);
    check_structural_tuple_default_sharing(&context);
    cm_hir_context_destroy(&context);

    memset(&resolver, 0, sizeof(resolver));
    resolver.return_shared_self = 1;
    result = lower_source(
        "trait Base<T = (Shared, Shared)> {} trait Child: Base {}",
        &context, &resolver);
    shared_base = find_item(&context, "Base");
    shared_child = find_item(&context, "Child");
    shared_parameter = shared_base == NULL ? NULL
        : cm_hir_get_generic_param(&context,
            shared_base->generic_parameter_start);
    shared_source_tuple = shared_parameter == NULL
            || !shared_parameter->has_default
        ? NULL : cm_hir_get_type(&context,
            shared_parameter->default_argument.data.type);
    shared_instantiated_tuple = shared_child == NULL
            || shared_child->data.trait_item.supertrait_count != 1u
            || shared_child->data.trait_item.supertraits[0]
                .trait_type.argument_count != 1u
        ? NULL : cm_hir_get_type(&context,
            shared_child->data.trait_item.supertraits[0]
                .trait_type.arguments[0].data.type);
    assert(result.error_count == 0u
        && shared_source_tuple != NULL
        && shared_source_tuple->kind == CM_HIR_TYPE_TUPLE_KIND
        && shared_source_tuple->data.tuple_type.element_count == 2u
        && shared_source_tuple->data.tuple_type.elements[0]
            == shared_source_tuple->data.tuple_type.elements[1]
        && shared_instantiated_tuple != NULL
        && shared_instantiated_tuple->kind == CM_HIR_TYPE_TUPLE_KIND
        && shared_instantiated_tuple->data.tuple_type.element_count == 2u
        && shared_instantiated_tuple->data.tuple_type.elements[0]
            == shared_instantiated_tuple->data.tuple_type.elements[1]
        && shared_instantiated_tuple->data.tuple_type.elements[0]
            != shared_source_tuple->data.tuple_type.elements[0]);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "trait Base<'a, T = &'a u8> {} "
        "trait Child<'b>: Base<'b> {}",
        &context, NULL);
    assert(result.error_count == 0u && result.lowered_item_count == 2u);
    check_structural_lifetime_default_use(&context);
    cm_hir_context_destroy(&context);

    nested_source = make_nested_trait_default_source(256u);
    result = lower_source(nested_source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "depth-boundary lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 2u);
    cm_hir_context_destroy(&context);
    free(nested_source);

    nested_source = make_nested_trait_default_source(257u);
    result = lower_source(nested_source, &context, NULL);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && strstr(result.first_error.message,
            "structural depth limit") != NULL);
    cm_hir_context_destroy(&context);
    free(nested_source);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected_kinds[index]
            || strstr(result.first_error.message,
                rejected_messages[index]) == NULL) {
            fprintf(stderr, "trait-default rejection %lu: %s: %s\n",
                (unsigned long)index,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]
            && strstr(result.first_error.message,
                rejected_messages[index]) != NULL);
        cm_hir_context_destroy(&context);
    }
}

static void check_monomorphic_impl_result(CmHirContext *context)
{
    const CmHirItem *trait_item;
    const CmHirItem *trait_associated;
    const CmHirItem *impl_item;
    const CmHirItem *impl_associated;
    const CmHirItem *output;
    const CmHirItem *projection;
    const CmHirType *self_type;
    const CmHirType *target;
    const CmHirType *projection_type;
    CmHirProjectionResult selection;

    trait_item = find_item(context, "T");
    output = find_item(context, "O");
    impl_item = find_impl(context);
    assert(trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT);
    assert(output != NULL && output->kind == CM_HIR_ITEM_STRUCT);
    assert(impl_item != NULL && impl_item->kind == CM_HIR_ITEM_IMPL
        && impl_item->name == CM_INTERN_ID_NONE
        && impl_item->data.impl_item.has_trait == 1
        && impl_item->data.impl_item.is_negative == 0
        && cm_hir_def_id_equal(
            impl_item->data.impl_item.trait_type.definition,
            trait_item->definition));
    trait_associated = find_child(context, trait_item->definition, "A");
    impl_associated = find_child(context, impl_item->definition, "A");
    assert(trait_associated != NULL
        && trait_associated->kind == CM_HIR_ITEM_TYPE_ALIAS
        && trait_associated->data.type_alias_item.target == CM_HIR_TYPE_NONE);
    assert(impl_associated != NULL
        && impl_associated->kind == CM_HIR_ITEM_TYPE_ALIAS
        && cm_hir_def_id_equal(impl_associated->data.type_alias_item
                .trait_item_definition,
            trait_associated->definition));
    self_type = cm_hir_get_type(context,
        impl_item->data.impl_item.self_type);
    target = cm_hir_get_type(context,
        impl_associated->data.type_alias_item.target);
    assert(self_type != NULL && self_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && self_type->data.integer_type.kind == CM_HIR_INT_U8);
    assert(target != NULL && target->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(target->data.named_type.definition,
            output->definition));
    projection = find_item(context, "P");
    projection_type = projection == NULL ? NULL : cm_hir_get_type(context,
        projection->data.type_alias_item.target);
    assert(projection != NULL
        && projection->kind == CM_HIR_ITEM_TYPE_ALIAS
        && projection_type != NULL
        && projection_type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(
            projection_type->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(projection_type->data.projection_type
                .associated_type.definition,
            trait_associated->definition));
    selection = cm_hir_select_projection(context,
        projection->definition.crate_id,
        projection->data.type_alias_item.target);
    assert(selection.status == CM_HIR_PROJECTION_SELECTED
        && selection.target == impl_associated->data.type_alias_item.target
        && cm_hir_def_id_equal(selection.impl_definition,
            impl_item->definition)
        && cm_hir_def_id_equal(selection.impl_associated_definition,
            impl_associated->definition));
}

static void test_monomorphic_trait_impl_entry_points(void)
{
    static const char source[] =
        "impl T for u8 { type A = O; }"
        "struct O;"
        "trait T { type A; }"
        "type P = <u8 as T>::A;";
    static const char alias_source[] =
        "type P = <S as T>::A;"
        "struct S; struct O; type Alias = S;"
        "trait T { type A; }"
        "impl T for Alias { type A = O; }";
    static const char *const rejected[] = {
        "impl u8 { type A = u16; }",
        "trait T { type A; } impl !T for u8 { type A = u16; }",
        "trait T { type A; } impl<X> T for X { type A = u8; }",
        "trait T { type A; } impl T for u8 where u8: Copy { type A = u16; }",
        "trait T { type A<'a>; } impl T for u8 { type A = u8; }",
        "trait T { type A; } impl T for u8 {}",
        "trait T { type A; type B; } impl T for u8 { type A = u8; }",
        "trait T { type A; } impl T for u8 { type B = u16; }",
        "trait T { type A; } impl T for u8 { type A = u8; fn f() {} }",
        "trait T { type A; } impl T for u8 { type A = u8; const C: u8 = 0; }",
        "trait T { type A; } impl T for u8 { type A = u8; type A = u16; }",
        "trait T { type A; } impl T for u8 { type A = u8; } "
            "impl T for u8 { type A = u16; }",
        "struct S; type AliasOne = S; type AliasTwo = S; "
            "trait T { type A; } "
            "impl T for AliasOne { type A = u8; } "
            "impl T for AliasTwo { type A = u16; }",
        "trait T { type A; } impl T for (u8, u16) { type A = u8; }",
        "struct S<X> { x: X } trait T { type A; } "
            "impl T for S<u8> { type A = u8; }",
        "unsafe trait T { type A; } impl T for u8 { type A = u8; }",
        "trait T { type A; } unsafe impl T for u8 { type A = u8; }"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_DUPLICATE_NAME,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL
    };
    static const char *const rejected_messages[] = {
        "inherent impls",
        "negative impl must",
        "full ordered generic local ADT subset",
        "where predicates",
        "generic arity differs",
        "missing a required associated type",
        "missing a required associated type",
        "no associated type",
        "impl method has no matching trait method declaration",
        "impl children other",
        "duplicate associated definition",
        "duplicate exact impl candidate",
        "duplicate exact impl candidate",
        "full ordered generic local ADT subset",
        "full ordered generic local ADT subset",
        "impl safety does not match",
        "impl safety does not match"
    };
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *impl_item;
    const CmHirItem *s_item;
    const CmHirType *self_type;
    size_t index;

    result = lower_source(source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "impl lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 6u);
    check_monomorphic_impl_result(&context);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    if (result.error_count != 0u) {
        fprintf(stderr, "expanded impl lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 6u);
    check_monomorphic_impl_result(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    result = lower_source(alias_source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "alias-normalized impl lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u);
    impl_item = find_impl(&context);
    s_item = find_item(&context, "S");
    self_type = impl_item == NULL ? NULL : cm_hir_get_type(&context,
        impl_item->data.impl_item.self_type);
    assert(impl_item != NULL && s_item != NULL && self_type != NULL
        && self_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(self_type->data.named_type.definition,
            s_item->definition));
    cm_hir_context_destroy(&context);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || index >= sizeof(rejected_kinds) / sizeof(rejected_kinds[0])
            || index >= sizeof(rejected_messages)
                / sizeof(rejected_messages[0])
            || result.first_error.kind != rejected_kinds[index]
            || strstr(result.first_error.message,
                rejected_messages[index]) == NULL) {
            fprintf(stderr,
                "impl rejection mismatch for %s: count=%lu kind=%s message=%s\n",
                rejected[index], (unsigned long)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && index < sizeof(rejected_kinds) / sizeof(rejected_kinds[0])
            && index < sizeof(rejected_messages)
                / sizeof(rejected_messages[0])
            && result.first_error.kind == rejected_kinds[index]
            && strstr(result.first_error.message,
                rejected_messages[index]) != NULL);
        cm_hir_context_destroy(&context);
    }
}

static void check_generic_associated_type_result(CmHirContext *context)
{
    const CmHirItem *future;
    const CmHirItem *future_output;
    const CmHirItem *async_fn;
    const CmHirItem *async_output;
    const CmHirItem *async_fn_mut;
    const CmHirItem *call_ref_future;
    const CmHirGenericParam *async_fn_parameter;
    const CmHirGenericParam *async_fn_mut_parameter;
    const CmHirGenericParam *lifetime;
    const CmHirAssociatedTypeBound *bound;
    const CmHirType *equality_value;
    const CmHirType *projection_argument;
    const CmHirType *supertrait_argument;
    const CmHirType *subject;

    future = find_item(context, "Future");
    future_output = future == NULL ? NULL
        : find_child(context, future->definition, "Output");
    async_fn = find_item(context, "AsyncFn");
    async_output = async_fn == NULL ? NULL
        : find_child(context, async_fn->definition, "Output");
    async_fn_mut = find_item(context, "AsyncFnMut");
    async_fn_parameter = async_fn == NULL
            || async_fn->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(context,
            async_fn->generic_parameter_start);
    async_fn_mut_parameter = async_fn_mut == NULL
            || async_fn_mut->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(context,
            async_fn_mut->generic_parameter_start);
    call_ref_future = async_fn_mut == NULL ? NULL
        : find_child(context, async_fn_mut->definition, "CallRefFuture");
    lifetime = call_ref_future == NULL ? NULL
        : cm_hir_get_generic_param(context,
            call_ref_future->generic_parameter_start);
    bound = call_ref_future == NULL
            || call_ref_future->data.type_alias_item.bound_count != 1u
        ? NULL : &call_ref_future->data.type_alias_item.bounds[0];
    equality_value = bound == NULL || bound->equality_count != 1u
        ? NULL : cm_hir_get_type(context, bound->equalities[0].value);
    projection_argument = equality_value == NULL
            || equality_value->kind != CM_HIR_TYPE_PROJECTION_KIND
            || equality_value->data.projection_type.trait_type.argument_count
                != 1u
            || equality_value->data.projection_type.trait_type.arguments[0]
                    .kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            equality_value->data.projection_type.trait_type.arguments[0]
                .data.type);
    supertrait_argument = async_fn_mut == NULL
            || async_fn_mut->data.trait_item.supertrait_count != 1u
            || async_fn_mut->data.trait_item.supertraits[0]
                    .trait_type.argument_count != 1u
            || async_fn_mut->data.trait_item.supertraits[0]
                    .trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(context,
            async_fn_mut->data.trait_item.supertraits[0]
                .trait_type.arguments[0].data.type);
    subject = call_ref_future == NULL
            || call_ref_future->outlives_predicate_count != 1u
        ? NULL : cm_hir_get_type(context,
            call_ref_future->outlives_predicates[0].subject.type);

    assert(future != NULL && future->kind == CM_HIR_ITEM_TRAIT
        && future_output != NULL && async_fn != NULL
        && async_fn->kind == CM_HIR_ITEM_TRAIT && async_output != NULL
        && async_fn_mut != NULL && async_fn_mut->kind == CM_HIR_ITEM_TRAIT
        && async_fn_parameter != NULL
        && async_fn_parameter->kind == CM_HIR_GENERIC_TYPE
        && async_fn_mut_parameter != NULL
        && async_fn_mut_parameter->kind == CM_HIR_GENERIC_TYPE
        && async_fn_mut->data.trait_item.supertrait_count == 1u
        && cm_hir_def_id_equal(async_fn_mut->data.trait_item.supertraits[0]
                .trait_type.definition,
            async_fn->definition)
        && supertrait_argument != NULL
        && supertrait_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && supertrait_argument->data.parameter_type.parameter
            == async_fn_mut->generic_parameter_start
        && call_ref_future != NULL
        && call_ref_future->kind == CM_HIR_ITEM_TYPE_ALIAS
        && call_ref_future->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && call_ref_future->generic_parameter_count == 1u
        && lifetime != NULL && lifetime->kind == CM_HIR_GENERIC_LIFETIME
        && cm_hir_def_id_equal(lifetime->owner,
            call_ref_future->definition)
        && hir_string_is(context, lifetime->name, "'a")
        && bound != NULL
        && cm_hir_def_id_equal(bound->trait_type.definition,
            future->definition)
        && bound->equality_count == 1u
        && cm_hir_def_id_equal(bound->equalities[0].associated_type,
            future_output->definition)
        && equality_value != NULL
        && equality_value->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(equality_value->data.projection_type
                .trait_type.definition,
            async_fn->definition)
        && equality_value->data.projection_type.trait_type.argument_count
            == 1u
        && equality_value->data.projection_type.trait_type.arguments[0]
                .data.type
            == async_fn_mut->data.trait_item.supertraits[0]
                .trait_type.arguments[0].data.type
        && projection_argument == supertrait_argument
        && cm_hir_def_id_equal(equality_value->data.projection_type
                .associated_type.definition,
            async_output->definition)
        && call_ref_future->outlives_predicate_count == 1u
        && call_ref_future->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && subject != NULL && subject->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(subject->data.self_type.owner,
            async_fn_mut->definition)
        && call_ref_future->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_EARLY_BOUND
        && call_ref_future->outlives_predicates[0].bound.data.parameter
            == call_ref_future->generic_parameter_start);
}

static void test_generic_associated_type_entry_points(void)
{
    static const char source[] =
        "trait Future { type Output; }"
        "trait AsyncFnMut<Args>: AsyncFn<Args> {"
        "type CallRefFuture<'a>: Future<Output = Self::Output> "
        "where Self: 'a; }"
        "trait AsyncFn<Args> { type Output; }";
    static const char impl_source[] =
        "struct Value; trait Owner { type Assoc<'a>; }"
        "impl Owner for Value { type Assoc<'a> = &'a Value; }";
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *owner;
    const CmHirItem *declaration;
    const CmHirItem *impl_item;
    const CmHirItem *definition;

    result = lower_source(source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "GAT declaration lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 6u);
    check_generic_associated_type_result(&context);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u && result.lowered_item_count == 6u);
    check_generic_associated_type_result(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    result = lower_source(impl_source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "GAT impl lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    owner = find_item(&context, "Owner");
    declaration = owner == NULL ? NULL
        : find_child(&context, owner->definition, "Assoc");
    impl_item = find_impl(&context);
    definition = impl_item == NULL ? NULL
        : find_child(&context, impl_item->definition, "Assoc");
    assert(result.error_count == 0u && result.lowered_item_count == 5u
        && declaration != NULL && definition != NULL
        && declaration->generic_parameter_count == 1u
        && definition->generic_parameter_count == 1u
        && cm_hir_def_id_equal(definition->data.type_alias_item
                .trait_item_definition,
            declaration->definition));
    cm_hir_context_destroy(&context);
}

static void test_same_trait_generic_self_projection(void)
{
    static const char source[] =
        "trait Future { type Output; }"
        "trait Owner<T> {"
        "type Output;"
        "type Gat<'a>: Future<Output = Self::Output> where Self: 'a;"
        "}";
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *future;
    const CmHirItem *future_output;
    const CmHirItem *owner;
    const CmHirItem *owner_output;
    const CmHirItem *gat;
    const CmHirAssociatedTypeBound *bound;
    const CmHirType *projection;
    const CmHirType *argument;

    result = lower_source(source, &context, NULL);
    future = find_item(&context, "Future");
    future_output = future == NULL ? NULL
        : find_child(&context, future->definition, "Output");
    owner = find_item(&context, "Owner");
    owner_output = owner == NULL ? NULL
        : find_child(&context, owner->definition, "Output");
    gat = owner == NULL ? NULL
        : find_child(&context, owner->definition, "Gat");
    bound = gat == NULL || gat->data.type_alias_item.bound_count != 1u
        ? NULL : &gat->data.type_alias_item.bounds[0];
    projection = bound == NULL || bound->equality_count != 1u
        ? NULL : cm_hir_get_type(&context, bound->equalities[0].value);
    argument = projection == NULL
            || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
            || projection->data.projection_type.trait_type.argument_count
                != 1u
            || projection->data.projection_type.trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            projection->data.projection_type.trait_type.arguments[0]
                .data.type);
    assert(result.error_count == 0u && result.lowered_item_count == 5u
        && future != NULL && future_output != NULL
        && owner != NULL && owner->generic_parameter_count == 1u
        && owner_output != NULL && gat != NULL && bound != NULL
        && cm_hir_def_id_equal(bound->equalities[0].associated_type,
            future_output->definition)
        && projection != NULL
        && cm_hir_def_id_equal(projection->data.projection_type
                .trait_type.definition,
            owner->definition)
        && cm_hir_def_id_equal(projection->data.projection_type
                .associated_type.definition,
            owner_output->definition)
        && projection->data.projection_type.trait_type.argument_count == 1u
        && argument != NULL && argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && argument->data.parameter_type.parameter
            == owner->generic_parameter_start);
    cm_hir_context_destroy(&context);
}

static void test_transitive_generic_self_projection_record_rollback(void)
{
    static const char source[] =
        "trait Future { type Output; }"
        "trait Leaf<V>: Mid<V> {"
        "type Gat<'a>: Future<Output = Self::Output> where Self: 'a;"
        "}"
        "trait Mid<U>: Base<U> {}"
        "trait Base<T> { type Output; }";
    CmHirContext first_context;
    CmHirContext second_context;
    CmHirLowerResult first_result;
    CmHirLowerResult second_result;
    const CmHirItem *first_leaf;
    const CmHirItem *second_leaf;

    first_result = lower_source(source, &first_context, NULL);
    second_result = lower_source(source, &second_context, NULL);
    first_leaf = find_item(&first_context, "Leaf");
    second_leaf = find_item(&second_context, "Leaf");
    assert(first_result.error_count == 1u
        && second_result.error_count == 1u
        && first_result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && second_result.first_error.kind
            == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && strstr(first_result.first_error.message,
            "transitive generic supertraits requires structural substitution")
            != NULL
        && strstr(second_result.first_error.message,
            "transitive generic supertraits requires structural substitution")
            != NULL
        && first_leaf != NULL && second_leaf != NULL
        && find_child(&first_context, first_leaf->definition, "Gat") == NULL
        && find_child(&second_context, second_leaf->definition, "Gat") == NULL
        && first_context.types.len == second_context.types.len
        && first_context.types.len == 2u
        && first_context.generic_parameters.len
            == second_context.generic_parameters.len
        && first_context.items.len == second_context.items.len
        && first_context.definitions.len == second_context.definitions.len
        && first_context.prebound_associated_types.len
            == second_context.prebound_associated_types.len
        && cm_arena_bytes_used(&first_context.storage)
            == cm_arena_bytes_used(&second_context.storage)
        && cm_interner_length(&first_context.strings)
            == cm_interner_length(&second_context.strings)
        && cm_arena_bytes_used(&first_context.strings.strings)
            == cm_arena_bytes_used(&second_context.strings.strings));
    cm_hir_context_destroy(&second_context);
    cm_hir_context_destroy(&first_context);
}

static void check_ordered_nominal_generic_impl_result(
    CmHirContext *context)
{
    const CmHirItem *wrapper;
    const CmHirItem *trait_item;
    const CmHirItem *trait_associated;
    const CmHirItem *impl_item;
    const CmHirItem *impl_associated;
    const CmHirItem *u8_projection;
    const CmHirItem *bool_projection;
    const CmHirGenericParam *parameter;
    const CmHirType *self_type;
    const CmHirType *self_argument;
    const CmHirType *target;
    CmHirProjectionResult selection;

    wrapper = find_item(context, "Wrapper");
    trait_item = find_item(context, "Trait");
    impl_item = find_impl(context);
    assert(wrapper != NULL && wrapper->kind == CM_HIR_ITEM_STRUCT
        && wrapper->generic_parameter_count == 1u);
    assert(trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT);
    assert(impl_item != NULL && impl_item->kind == CM_HIR_ITEM_IMPL
        && impl_item->generic_parameter_count == 1u
        && impl_item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        && cm_hir_def_id_equal(
            impl_item->data.impl_item.trait_type.definition,
            trait_item->definition));
    parameter = cm_hir_get_generic_param(context,
        impl_item->generic_parameter_start);
    assert(parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && cm_hir_def_id_equal(parameter->owner, impl_item->definition));

    self_type = cm_hir_get_type(context,
        impl_item->data.impl_item.self_type);
    assert(self_type != NULL && self_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(self_type->data.named_type.definition,
            wrapper->definition)
        && self_type->data.named_type.argument_count == 1u
        && self_type->data.named_type.arguments != NULL
        && self_type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE);
    self_argument = cm_hir_get_type(context,
        self_type->data.named_type.arguments[0].data.type);
    assert(self_argument != NULL
        && self_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && self_argument->data.parameter_type.parameter
            == impl_item->generic_parameter_start);

    trait_associated = find_child(context, trait_item->definition, "Assoc");
    impl_associated = find_child(context, impl_item->definition, "Assoc");
    assert(trait_associated != NULL
        && trait_associated->kind == CM_HIR_ITEM_TYPE_ALIAS);
    assert(impl_associated != NULL
        && impl_associated->kind == CM_HIR_ITEM_TYPE_ALIAS
        && impl_associated->generic_parameter_count == 0u
        && cm_hir_def_id_equal(impl_associated->parent_definition,
            impl_item->definition)
        && cm_hir_def_id_equal(impl_associated->data.type_alias_item
                .trait_item_definition,
            trait_associated->definition));
    target = cm_hir_get_type(context,
        impl_associated->data.type_alias_item.target);
    assert(target != NULL && target->kind == CM_HIR_TYPE_PARAMETER_KIND
        && target->data.parameter_type.parameter
            == impl_item->generic_parameter_start);

    u8_projection = find_item(context, "U8Assoc");
    assert(u8_projection != NULL
        && u8_projection->kind == CM_HIR_ITEM_TYPE_ALIAS);
    selection = cm_hir_select_projection(context,
        u8_projection->definition.crate_id,
        u8_projection->data.type_alias_item.target);
    target = cm_hir_get_type(context, selection.target);
    assert(selection.status == CM_HIR_PROJECTION_SELECTED
        && selection.hir_status == CM_HIR_OK
        && selection.allocated_type_count == 0u
        && selection.target
            != impl_associated->data.type_alias_item.target
        && target != NULL && target->kind == CM_HIR_TYPE_INTEGER_KIND
        && target->data.integer_type.kind == CM_HIR_INT_U8
        && cm_hir_def_id_equal(selection.impl_definition,
            impl_item->definition)
        && cm_hir_def_id_equal(selection.impl_associated_definition,
            impl_associated->definition));

    bool_projection = find_item(context, "BoolAssoc");
    assert(bool_projection != NULL
        && bool_projection->kind == CM_HIR_ITEM_TYPE_ALIAS);
    selection = cm_hir_select_projection(context,
        bool_projection->definition.crate_id,
        bool_projection->data.type_alias_item.target);
    target = cm_hir_get_type(context, selection.target);
    assert(selection.status == CM_HIR_PROJECTION_SELECTED
        && selection.hir_status == CM_HIR_OK
        && selection.allocated_type_count == 0u
        && selection.target
            != impl_associated->data.type_alias_item.target
        && target != NULL && target->kind == CM_HIR_TYPE_BOOL_KIND
        && cm_hir_def_id_equal(selection.impl_definition,
            impl_item->definition)
        && cm_hir_def_id_equal(selection.impl_associated_definition,
            impl_associated->definition));
}

static void test_ordered_nominal_generic_impl_entry_points(void)
{
    static const char source[] =
        "struct Wrapper<T>;"
        "trait Trait { type Assoc; }"
        "impl<T> Trait for Wrapper<T> { type Assoc = T; }"
        "type U8Assoc = <Wrapper<u8> as Trait>::Assoc;"
        "type BoolAssoc = <Wrapper<bool> as Trait>::Assoc;";
    static const char *const rejected[] = {
        "struct Wrapper<T>; trait Trait { type Assoc; } "
            "impl<T> Trait for T { type Assoc = T; }",
        "struct Pair<T, U>; trait Trait { type Assoc; } "
            "impl<T, U> Trait for Pair<T, T> { type Assoc = T; }",
        "struct Pair<T, U>; trait Trait { type Assoc; } "
            "impl<T, U> Trait for Pair<U, T> { type Assoc = T; }",
        "struct Inner<T>; struct Wrapper<T>; "
            "trait Trait { type Assoc; } "
            "impl<T> Trait for Wrapper<Inner<T>> { type Assoc = T; }",
        "struct Pair<T, U>; trait Trait { type Assoc; } "
            "impl<T> Trait for Pair<T, u8> { type Assoc = T; }",
        "struct Wrapper<T>; trait Trait { type Assoc; } "
            "impl<T, U> Trait for Wrapper<T> { type Assoc = T; }",
        "struct Wrapper<T>; trait Trait { type Assoc; } "
            "impl<'a> Trait for Wrapper<u8> { type Assoc = u8; }",
        "struct Wrapper<T>; trait Trait { type Assoc; } "
            "impl<const N: usize> Trait for Wrapper<u8> { type Assoc = u8; }",
        "struct Wrapper<T>; trait Trait { type Assoc; } "
            "impl<T: Copy> Trait for Wrapper<T> { type Assoc = T; }",
        "struct Wrapper<T>; trait Trait { type Assoc; } "
            "impl<T> Trait for Wrapper<T> where T: Copy { type Assoc = T; }",
        "struct Wrapper<T>; trait Trait { type Assoc; } "
            "impl<T> Trait for Wrapper<T> { type Assoc = T; } "
            "impl<U> Trait for Wrapper<U> { type Assoc = U; }",
        "struct Wrapper<T>; type Alias<T> = Wrapper<T>; "
            "trait Trait { type Assoc; } "
            "impl<T> Trait for Wrapper<T> { type Assoc = T; } "
            "impl<U> Trait for Alias<U> { type Assoc = U; }"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL
    };
    static const char *const rejected_messages[] = {
        "full ordered generic local ADT subset",
        "full ordered generic local ADT subset",
        "full ordered generic local ADT subset",
        "full ordered generic local ADT subset",
        "full ordered generic local ADT subset",
        "full ordered generic local ADT subset",
        "full ordered generic local ADT subset",
        "full ordered generic local ADT subset",
        "generic bounds and const declarations",
        "where predicates",
        "overlapping ordered generic impl candidates",
        "overlapping ordered generic impl candidates"
    };
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    size_t index;

    result = lower_source(source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "generic impl lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 7u);
    check_ordered_nominal_generic_impl_result(&context);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    if (result.error_count != 0u) {
        fprintf(stderr, "expanded generic impl lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 7u);
    check_ordered_nominal_generic_impl_result(&context);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || index >= sizeof(rejected_kinds) / sizeof(rejected_kinds[0])
            || index >= sizeof(rejected_messages)
                / sizeof(rejected_messages[0])
            || result.first_error.kind != rejected_kinds[index]
            || strstr(result.first_error.message,
                rejected_messages[index]) == NULL) {
            fprintf(stderr,
                "generic impl rejection mismatch for %s: "
                "count=%lu kind=%s message=%s\n",
                rejected[index], (unsigned long)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && index < sizeof(rejected_kinds) / sizeof(rejected_kinds[0])
            && index < sizeof(rejected_messages)
                / sizeof(rejected_messages[0])
            && result.first_error.kind == rejected_kinds[index]
            && strstr(result.first_error.message,
                rejected_messages[index]) != NULL);
        cm_hir_context_destroy(&context);
    }
}

static void check_method_bearing_impl_result(const CmHirContext *context,
    CmSourceId expected_source)
{
    static const char *const method_names[] = {
        "value", "mut_value", "shared", "mutable_ref", "custom",
        "static_method", "inherited"
    };
    static const CmHirReceiverKind receiver_kinds[] = {
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_REF_SHARED,
        CM_HIR_RECEIVER_REF_MUTABLE,
        CM_HIR_RECEIVER_CUSTOM,
        CM_HIR_RECEIVER_NONE,
        CM_HIR_RECEIVER_NONE
    };
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *trait_associated;
    const CmHirItem *impl_associated;
    const CmHirItem *trait_method;
    const CmHirItem *impl_method;
    const CmHirItem *defaulted;
    const CmHirType *type;
    const CmHirType *inner;
    const CmHirBody *body;
    const CmHirGenericParam *parameter;
    size_t index;

    trait_item = find_item(context, "Methods");
    impl_item = find_impl(context);
    assert(trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT);
    assert(impl_item != NULL && impl_item->kind == CM_HIR_ITEM_IMPL
        && impl_item->generic_parameter_count == 1u
        && cm_hir_def_id_equal(
            impl_item->data.impl_item.trait_type.definition,
            trait_item->definition));
    parameter = cm_hir_get_generic_param(context,
        impl_item->generic_parameter_start);
    assert(parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && hir_string_is(context, parameter->name, "T")
        && cm_hir_def_id_equal(parameter->owner, impl_item->definition));

    trait_associated = find_child(context, trait_item->definition, "Assoc");
    impl_associated = find_child(context, impl_item->definition, "Assoc");
    assert(trait_associated != NULL
        && trait_associated->kind == CM_HIR_ITEM_TYPE_ALIAS
        && trait_associated->data.type_alias_item.target
            == CM_HIR_TYPE_NONE);
    assert(impl_associated != NULL
        && impl_associated->kind == CM_HIR_ITEM_TYPE_ALIAS
        && cm_hir_def_id_equal(impl_associated->data.type_alias_item
                .trait_item_definition,
            trait_associated->definition));

    for (index = 0u;
         index < sizeof(method_names) / sizeof(method_names[0]);
         ++index) {
        trait_method = find_child(context, trait_item->definition,
            method_names[index]);
        impl_method = find_child(context, impl_item->definition,
            method_names[index]);
        assert(trait_method != NULL
            && trait_method->kind == CM_HIR_ITEM_FUNCTION
            && trait_method->data.function_item.signature.receiver
                == receiver_kinds[index]
            && cm_hir_def_id_is_none(trait_method->data.function_item
                    .trait_item_definition));
        assert(impl_method != NULL
            && impl_method->kind == CM_HIR_ITEM_FUNCTION
            && impl_method->data.function_item.signature.receiver
                == receiver_kinds[index]
            && cm_hir_def_id_equal(impl_method->data.function_item
                    .trait_item_definition,
                trait_method->definition)
            && impl_method->data.function_item.body != CM_HIR_BODY_NONE);
        body = cm_hir_get_body(context,
            impl_method->data.function_item.body);
        assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
            && body->source == expected_source
            && body->source_expression_id != 0u
            && cm_hir_def_id_equal(body->owner, impl_method->definition)
            && body->expected_type == impl_method->data.function_item
                .signature.return_type
            && body->parameter_count == impl_method->data.function_item
                .signature.parameter_count
            && body->local_count == body->parameter_count);
    }

    trait_method = find_child(context, trait_item->definition, "value");
    impl_method = find_child(context, impl_item->definition, "value");
    assert(trait_method != NULL && impl_method != NULL);
    type = expect_type_kind(context,
        trait_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(type->data.self_type.owner,
        trait_item->definition));
    type = expect_type_kind(context,
        trait_method->data.function_item.signature.return_type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(type->data.self_type.owner,
        trait_item->definition));
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(type->data.self_type.owner,
        impl_item->definition));
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.return_type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(type->data.self_type.owner,
        impl_item->definition));

    impl_method = find_child(context, impl_item->definition, "mut_value");
    assert(impl_method != NULL);
    body = cm_hir_get_body(context, impl_method->data.function_item.body);
    assert(body != NULL && body->local_count == 1u
        && body->locals[0].mutability == CM_HIR_MUTABLE
        && hir_string_is(context, body->locals[0].name, "self"));

    trait_method = find_child(context, trait_item->definition, "shared");
    impl_method = find_child(context, impl_item->definition, "shared");
    assert(trait_method != NULL && impl_method != NULL);
    type = expect_type_kind(context,
        trait_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_REFERENCE_KIND);
    assert(type->data.reference_type.mutability == CM_HIR_IMMUTABLE);
    inner = expect_type_kind(context, type->data.reference_type.pointee,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
        trait_item->definition));
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_REFERENCE_KIND);
    assert(type->data.reference_type.mutability == CM_HIR_IMMUTABLE);
    inner = expect_type_kind(context, type->data.reference_type.pointee,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
        impl_item->definition));
    impl_method = find_child(context, impl_item->definition, "mutable_ref");
    assert(impl_method != NULL);
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_REFERENCE_KIND);
    assert(type->data.reference_type.mutability == CM_HIR_MUTABLE);
    inner = expect_type_kind(context, type->data.reference_type.pointee,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
        impl_item->definition));
    body = cm_hir_get_body(context, impl_method->data.function_item.body);
    assert(body != NULL && body->local_count == 1u
        && body->locals[0].mutability == CM_HIR_IMMUTABLE
        && body->locals[0].type
            == impl_method->data.function_item.signature.parameters[0].type);

    trait_method = find_child(context, trait_item->definition, "custom");
    impl_method = find_child(context, impl_item->definition, "custom");
    assert(trait_method != NULL && impl_method != NULL);
    type = expect_type_kind(context,
        trait_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_REFERENCE_KIND);
    inner = expect_type_kind(context, type->data.reference_type.pointee,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
        trait_item->definition));
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_REFERENCE_KIND);
    inner = expect_type_kind(context, type->data.reference_type.pointee,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
        impl_item->definition));
    type = expect_type_kind(context,
        trait_method->data.function_item.signature.return_type,
        CM_HIR_TYPE_PROJECTION_KIND);
    inner = expect_type_kind(context, type->data.projection_type.self_type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
            trait_item->definition)
        && cm_hir_def_id_equal(
            type->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(
            type->data.projection_type.associated_type.definition,
            trait_associated->definition));
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.return_type,
        CM_HIR_TYPE_PROJECTION_KIND);
    inner = expect_type_kind(context, type->data.projection_type.self_type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
            impl_item->definition)
        && cm_hir_def_id_equal(
            type->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(
            type->data.projection_type.associated_type.definition,
            trait_associated->definition));

    trait_method = find_child(context, trait_item->definition,
        "static_method");
    impl_method = find_child(context, impl_item->definition,
        "static_method");
    assert(trait_method != NULL && impl_method != NULL
        && trait_method->data.function_item.signature.parameter_count == 1u
        && impl_method->data.function_item.signature.parameter_count == 1u);
    type = expect_type_kind(context,
        trait_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(type->data.self_type.owner,
        trait_item->definition));
    type = expect_type_kind(context,
        trait_method->data.function_item.signature.return_type,
        CM_HIR_TYPE_PROJECTION_KIND);
    inner = expect_type_kind(context, type->data.projection_type.self_type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
            trait_item->definition)
        && cm_hir_def_id_equal(
            type->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(
            type->data.projection_type.associated_type.definition,
            trait_associated->definition));
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(type->data.self_type.owner,
        impl_item->definition));
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.return_type,
        CM_HIR_TYPE_PROJECTION_KIND);
    inner = expect_type_kind(context, type->data.projection_type.self_type,
        CM_HIR_TYPE_SELF_KIND);
    assert(cm_hir_def_id_equal(inner->data.self_type.owner,
            impl_item->definition)
        && cm_hir_def_id_equal(
            type->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(
            type->data.projection_type.associated_type.definition,
            trait_associated->definition));

    impl_method = find_child(context, impl_item->definition, "inherited");
    assert(impl_method != NULL
        && impl_method->data.function_item.signature.parameter_count == 1u);
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.parameters[0].type,
        CM_HIR_TYPE_PARAMETER_KIND);
    assert(type->data.parameter_type.parameter
        == impl_item->generic_parameter_start);
    type = expect_type_kind(context,
        impl_method->data.function_item.signature.return_type,
        CM_HIR_TYPE_PARAMETER_KIND);
    assert(type->data.parameter_type.parameter
        == impl_item->generic_parameter_start);

    defaulted = find_child(context, trait_item->definition, "defaulted");
    assert(defaulted != NULL && defaulted->kind == CM_HIR_ITEM_FUNCTION
        && defaulted->data.function_item.body != CM_HIR_BODY_NONE
        && find_child(context, impl_item->definition, "defaulted") == NULL);
    body = cm_hir_get_body(context, defaulted->data.function_item.body);
    assert(body != NULL && body->source == expected_source
        && body->expected_type
            == defaulted->data.function_item.signature.return_type
        && cm_hir_def_id_equal(body->owner, defaulted->definition));
}

static void test_method_bearing_trait_impl_entry_points(void)
{
    static const char source[] =
        "struct Wrapper<T>;"
        "trait Methods {"
        " type Assoc;"
        " fn value(self) -> Self;"
        " fn mut_value(mut self);"
        " fn shared(&self);"
        " fn mutable_ref(&mut self);"
        " fn custom(self: &Self) -> Self::Assoc;"
        " fn static_method(value: Self) -> <Self as Methods>::Assoc;"
        " fn inherited(value: u8) -> u8;"
        " fn defaulted(&self) {}"
        "}"
        "impl<T> Methods for Wrapper<T> {"
        " type Assoc = T;"
        " fn value(self) -> Self {}"
        " fn mut_value(mut self) {}"
        " fn shared(&self) {}"
        " fn mutable_ref(&mut self) {}"
        " fn custom(self: &Self) -> Self::Assoc {}"
        " fn static_method(value: Self) -> <Self as Methods>::Assoc {}"
        " fn inherited(value: T) -> T {}"
        "}";
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    if (result.error_count != 0u) {
        fprintf(stderr, "method lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 20u
        && context.items.len == 20u && context.bodies.len == 8u);
    check_method_bearing_impl_result(&context, 7u);
    cm_hir_context_destroy(&context);

    make_cfg_view(source, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    if (result.error_count != 0u) {
        fprintf(stderr, "expanded method lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u && result.lowered_item_count == 20u
        && context.items.len == 20u && context.bodies.len == 8u);
    check_method_bearing_impl_result(&context, 11u);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void test_lifetime_qualified_receiver(void)
{
    static const char source[] =
        "trait Lending { fn borrow(&'static self); }";
    const CmHirItem *trait_item;
    const CmHirItem *method;
    const CmHirType *receiver_type;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    trait_item = find_item(&context, "Lending");
    method = trait_item == NULL ? NULL
        : find_child(&context, trait_item->definition, "borrow");
    receiver_type = method == NULL
            || method->data.function_item.signature.parameter_count != 1u
            || method->data.function_item.signature.parameters == NULL
        ? NULL : cm_hir_get_type(&context,
            method->data.function_item.signature.parameters[0].type);
    if (result.error_count != 0u || method == NULL || receiver_type == NULL
        || receiver_type->kind != CM_HIR_TYPE_REFERENCE_KIND) {
        fprintf(stderr, "lifetime receiver lowering mismatch: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u
        && method != NULL && method->kind == CM_HIR_ITEM_FUNCTION
        && method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_SHARED
        && receiver_type != NULL
        && receiver_type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && receiver_type->data.reference_type.region.kind
            == CM_HIR_REGION_STATIC);
    cm_hir_context_destroy(&context);
}

static void test_method_completeness_and_identity_errors(void)
{
    static const char default_omitted[] =
        "trait T { fn required(); fn defaulted() {} }"
        "impl T for u8 { fn required() {} }";
    static const char default_overridden[] =
        "trait T { fn defaulted() {} }"
        "impl T for u8 { fn defaulted() {} }";
    static const char *const rejected[] = {
        "trait T { fn required(); } impl T for u8 {}",
        "trait T {} impl T for u8 { fn extra() {} }",
        "trait T { fn f(); } impl T for u8 { fn f() {} fn f() {} }",
        "trait T { fn f(); } impl T for u8 { fn f(); }",
        "fn free(self) {}",
        "trait T { fn misplaced(value: u8, self); }",
        "trait T { fn unrelated(self: u8); }",
        "type Ignore<T> = u8; trait T { fn erased(self: Ignore<Self>); }"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_DUPLICATE_NAME,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_TYPE,
        CM_HIR_LOWER_UNSUPPORTED_TYPE
    };
    static const char *const rejected_messages[] = {
        "missing a required trait method",
        "no matching trait method declaration",
        "duplicate associated definition",
        "Rust or rust-call ABI definitions",
        "receiver must be the first parameter",
        "receiver must be the first parameter",
        "custom receiver type must follow",
        "after type-alias normalization"
    };
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *trait_method;
    const CmHirItem *impl_method;
    size_t index;

    result = lower_source(default_omitted, &context, NULL);
    assert(result.error_count == 0u);
    trait_item = find_item(&context, "T");
    impl_item = find_impl(&context);
    assert(trait_item != NULL && impl_item != NULL
        && find_child(&context, trait_item->definition, "defaulted") != NULL
        && find_child(&context, impl_item->definition, "defaulted") == NULL);
    cm_hir_context_destroy(&context);

    result = lower_source(default_overridden, &context, NULL);
    assert(result.error_count == 0u);
    trait_item = find_item(&context, "T");
    impl_item = find_impl(&context);
    trait_method = trait_item == NULL ? NULL
        : find_child(&context, trait_item->definition, "defaulted");
    impl_method = impl_item == NULL ? NULL
        : find_child(&context, impl_item->definition, "defaulted");
    assert(trait_method != NULL && impl_method != NULL
        && cm_hir_def_id_equal(impl_method->data.function_item
                .trait_item_definition,
            trait_method->definition));
    cm_hir_context_destroy(&context);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected_kinds[index]
            || strstr(result.first_error.message,
                rejected_messages[index]) == NULL) {
            fprintf(stderr,
                "method identity rejection mismatch for %s: "
                "count=%lu kind=%s message=%s\n",
                rejected[index], (unsigned long)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]
            && strstr(result.first_error.message,
                rejected_messages[index]) != NULL);
        cm_hir_context_destroy(&context);
    }
}

static void test_trait_method_self_sized_predicate(void)
{
    static const char source[] =
        "trait Sized {} "
        "trait Iterator {"
        " fn count(self) -> usize where Self: Sized { 0 }"
        "}";
    const CmHirItem *sized;
    const CmHirItem *iterator;
    const CmHirItem *count;
    const CmHirType *subject;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    sized = find_item(&context, "Sized");
    iterator = find_item(&context, "Iterator");
    count = iterator == NULL ? NULL
        : find_child(&context, iterator->definition, "count");
    subject = count == NULL || count->predicate_count != 1u
        ? NULL : cm_hir_get_type(&context, count->predicates[0].subject);
    assert(result.error_count == 0u
        && sized != NULL && sized->kind == CM_HIR_ITEM_TRAIT
        && iterator != NULL && iterator->kind == CM_HIR_ITEM_TRAIT
        && count != NULL && count->kind == CM_HIR_ITEM_FUNCTION
        && count->predicate_count == 1u && count->predicates != NULL
        && subject != NULL && subject->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(subject->data.self_type.owner,
            iterator->definition)
        && cm_hir_def_id_equal(
            count->predicates[0].trait_type.definition,
            sized->definition)
        && count->predicates[0].trait_type.argument_count == 0u
        && count->predicates[0].trait_type.arguments == NULL
        && count->predicates[0].span.end
            >= count->predicates[0].span.start
        && (size_t)(count->predicates[0].span.end
            - count->predicates[0].span.start)
            == strlen("Self: Sized")
        && memcmp(source + count->predicates[0].span.start,
            "Self: Sized", strlen("Self: Sized")) == 0);
    cm_hir_context_destroy(&context);
}

static void test_trait_method_const_predicates(void)
{
    static const char source[] =
        "trait Maybe {} trait FnOnce<A> { type Output; } "
        "trait Owner { fn inspect<ARG, RET, F>() where "
        "F: ~const Maybe + const FnOnce<ARG, Output = RET>; }";
    const CmHirItem *maybe;
    const CmHirItem *fn_once;
    const CmHirItem *owner;
    const CmHirItem *inspect;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    maybe = find_item(&context, "Maybe");
    fn_once = find_item(&context, "FnOnce");
    owner = find_item(&context, "Owner");
    inspect = owner == NULL ? NULL
        : find_child(&context, owner->definition, "inspect");
    assert(result.error_count == 0u
        && maybe != NULL && maybe->kind == CM_HIR_ITEM_TRAIT
        && fn_once != NULL && fn_once->kind == CM_HIR_ITEM_TRAIT
        && inspect != NULL && inspect->kind == CM_HIR_ITEM_FUNCTION
        && inspect->predicate_count == 2u && inspect->predicates != NULL
        && inspect->predicates[0].modifier
            == CM_HIR_PREDICATE_CONST_IF_CONST
        && cm_hir_def_id_equal(
            inspect->predicates[0].trait_type.definition,
            maybe->definition)
        && inspect->predicates[1].modifier == CM_HIR_PREDICATE_CONST
        && cm_hir_def_id_equal(
            inspect->predicates[1].trait_type.definition,
            fn_once->definition)
        && inspect->predicates[1].trait_type.argument_count == 1u
        && inspect->predicates[1].equality_count == 1u
        && inspect->predicates[1].equalities != NULL);
    cm_hir_context_destroy(&context);
}

static void test_trait_method_predicate_boundaries(void)
{
    static const char accepted[] =
        "trait Sized {} trait Copy {} trait T {"
        " fn compound() where Self: Sized + Copy;"
        " fn effects() where Self: ~const Sized + const Copy;"
        " fn duplicate() where Self: Sized, Self: Sized; }";
    static const struct {
        const char *source;
        CmHirLowerErrorKind kind;
        const char *message;
    } rejected[] = {
        {
            "trait Sized {} trait T { fn f() where u8: Sized; }",
            CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            "predicate subject must be"
        },
        {
            "trait Sized {} trait T { fn f() where Self: ?Sized; }",
            CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            "relaxed trait bounds"
        },
        {
            "trait Sized {} trait T { fn f() where Self: Sized<u8>; }",
            CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            "too many positional arguments"
        },
        {
            "trait T { fn f() where Self: Sized; }",
            CM_HIR_LOWER_UNRESOLVED_PATH,
            "supertrait path is unresolved"
        },
        {
            "struct Sized; trait T { fn f() where Self: Sized; }",
            CM_HIR_LOWER_WRONG_NAMESPACE,
            "supertrait path does not name a trait"
        },
        {
            "trait Sized {} fn f() where Self: Sized;",
            CM_HIR_LOWER_UNRESOLVED_PATH,
            "Self is used outside"
        }
    };
    size_t index;

    {
        CmHirContext context;
        CmHirLowerResult result;

        result = lower_source(accepted, &context, NULL);
        assert(result.error_count == 0u);
        cm_hir_context_destroy(&context);
    }

    {
        CmHirContext context;
        CmHirLowerResult result;

        result = lower_source(
            "trait Sized {} trait T { fn f(); }"
            " impl T for u8 { fn f() where Self: Sized {} }",
            &context, NULL);
        assert(result.error_count == 0u);
        cm_hir_context_destroy(&context);
    }

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        CmHirContext context;
        CmHirLowerResult result;

        result = lower_source(rejected[index].source, &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected[index].kind
            || strstr(result.first_error.message, rejected[index].message)
                == NULL) {
            fprintf(stderr,
                "trait predicate rejection mismatch for %s: "
                "count=%lu kind=%s message=%s\n",
                rejected[index].source, (unsigned long)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected[index].kind
            && strstr(result.first_error.message, rejected[index].message)
                != NULL);
        cm_hir_context_destroy(&context);
    }
}

static void test_callable_tuple_provenance(void)
{
    static const char callable[] =
        "trait FnOnce<Args> { type Output; }"
        "trait Callable { fn call<F: FnOnce(u8) -> u16>(); }";
    CmAst ast;
    CmParseResult parse_result;
    const CmAstItemId *root_id;
    const CmAstItem *trait_item;
    CmAstItem *method;
    CmAstType *trait_type;
    CmAstPath *trait_path;
    CmAstGenericArg *argument;
    CmAstType *tuple_type;
    CmAstType *element_type;
    CmAstTypeId *saved_elements;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, callable, sizeof(callable) - 1u,
        CM_EDITION_2024);
    assert(parse_result.error_count == 0u && ast.root_items.len == 2u);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    trait_item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    method = trait_item == NULL
            || trait_item->data.trait_item.item_count != 1u
        ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
            (size_t)trait_item->data.trait_item.items[0] - 1u);
    trait_type = method == NULL || method->generic_parameter_count != 1u
            || method->generic_parameters == NULL
            || method->generic_parameters[0].bound_count != 1u
            || method->generic_parameters[0].bounds == NULL
        ? NULL : (CmAstType *)cm_vec_at(&ast.types,
            (size_t)method->generic_parameters[0].bounds[0].trait_type - 1u);
    trait_path = trait_type == NULL || trait_type->kind != CM_AST_TYPE_PATH
        ? NULL : (CmAstPath *)cm_vec_at(&ast.paths,
            (size_t)trait_type->path - 1u);
    argument = trait_path == NULL || trait_path->segment_count != 1u
            || trait_path->segments == NULL
            || trait_path->segments[0].argument_count != 2u
            || trait_path->segments[0].arguments == NULL
        ? NULL : &trait_path->segments[0].arguments[0];
    tuple_type = argument == NULL || argument->kind != CM_AST_GENERIC_TYPE
        ? NULL : (CmAstType *)cm_vec_at(&ast.types,
            (size_t)argument->type - 1u);
    assert(tuple_type != NULL && tuple_type->kind == CM_AST_TYPE_TUPLE
        && tuple_type->element_count == 1u
        && tuple_type->tuple_provenance == CM_AST_TUPLE_CALLABLE_INPUTS);

    cm_hir_lower_options_init(&options);
    options.crate_name = "callable_tuple_test";
    options.source = 7u;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 0u);
    cm_hir_context_destroy(&context);

    tuple_type->tuple_provenance = CM_AST_TUPLE_SOURCE;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 0u);
    cm_hir_context_destroy(&context);

    tuple_type->tuple_provenance = (CmAstTupleProvenance)99;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message,
            "invalid syntax provenance") != NULL);
    cm_hir_context_destroy(&context);

    tuple_type->tuple_provenance = CM_AST_TUPLE_CALLABLE_INPUTS;
    saved_elements = tuple_type->elements;
    tuple_type->elements = NULL;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message,
            "count has no element storage") != NULL);
    cm_hir_context_destroy(&context);
    tuple_type->elements = saved_elements;

    element_type = (CmAstType *)cm_vec_at(&ast.types,
        (size_t)tuple_type->elements[0] - 1u);
    assert(element_type != NULL && element_type->kind != CM_AST_TYPE_TUPLE);
    element_type->tuple_provenance = CM_AST_TUPLE_CALLABLE_INPUTS;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message,
            "non-tuple type has tuple syntax provenance") != NULL);
    cm_hir_context_destroy(&context);
    element_type->tuple_provenance = CM_AST_TUPLE_SOURCE;
    cm_ast_destroy(&ast);
}

static void test_parenthesized_and_singleton_tuple_types(void)
{
    static const char source[] =
        "trait Shapes { fn f(grouped: (u8), singleton: (u16,)); }";
    const CmHirItem *owner;
    const CmHirItem *method;
    const CmHirType *grouped;
    const CmHirType *singleton;
    const CmHirType *element;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    owner = find_item(&context, "Shapes");
    method = owner == NULL ? NULL
        : find_child(&context, owner->definition, "f");
    grouped = method == NULL
            || method->data.function_item.signature.parameter_count != 2u
        ? NULL : cm_hir_get_type(&context,
            method->data.function_item.signature.parameters[0].type);
    singleton = grouped == NULL ? NULL : cm_hir_get_type(&context,
        method->data.function_item.signature.parameters[1].type);
    element = singleton == NULL
            || singleton->kind != CM_HIR_TYPE_TUPLE_KIND
            || singleton->data.tuple_type.element_count != 1u
            || singleton->data.tuple_type.elements == NULL
        ? NULL : cm_hir_get_type(&context,
            singleton->data.tuple_type.elements[0]);
    assert(result.error_count == 0u
        && grouped != NULL && grouped->kind == CM_HIR_TYPE_INTEGER_KIND
        && grouped->data.integer_type.kind == CM_HIR_INT_U8
        && singleton != NULL
        && singleton->kind == CM_HIR_TYPE_TUPLE_KIND
        && element != NULL && element->kind == CM_HIR_TYPE_INTEGER_KIND
        && element->data.integer_type.kind == CM_HIR_INT_U16);
    cm_hir_context_destroy(&context);
}

static void test_trait_method_predicate_storage_mismatch(void)
{
    static const char source[] =
        "trait Sized {} trait T { fn f() where Self: Sized; }";
    CmAst ast;
    CmParseResult parse_result;
    const CmAstItemId *trait_id;
    const CmAstItem *trait_item;
    CmAstItem *method;
    CmAstWherePredicate *saved_predicates;
    CmAstType *subject_type;
    CmAstPath *subject_path;
    CmInternId saved_where_clause;
    CmAstSpan saved_span;
    CmAstSpan saved_bound_span;
    CmAstWhereBoundModifier saved_bound_modifier;
    CmAstTypeId saved_bound_trait_type;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    assert(parse_result.error_count == 0u && ast.root_items.len == 2u);
    trait_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    trait_item = trait_id == NULL ? NULL : cm_ast_get_item(&ast,
        *trait_id);
    method = trait_item == NULL
            || trait_item->data.trait_item.item_count != 1u
        ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
            (size_t)trait_item->data.trait_item.items[0] - 1u);
    assert(method != NULL && method->where_predicate_count == 1u
        && method->where_predicates != NULL
        && method->where_clause != CM_INTERN_ID_NONE);
    saved_predicates = method->where_predicates;
    subject_type = (CmAstType *)cm_vec_at(&ast.types,
        (size_t)saved_predicates[0].subject - 1u);
    subject_path = subject_type == NULL ? NULL
        : (CmAstPath *)cm_vec_at(&ast.paths,
            (size_t)subject_type->path - 1u);
    assert(subject_path != NULL && subject_path->segment_count == 1u
        && subject_path->segments != NULL
        && subject_path->segments[0].argument_count == 0u
        && subject_path->segments[0].arguments == NULL);
    saved_where_clause = method->where_clause;
    saved_span = method->where_predicates[0].span;
    saved_bound_span = method->where_predicates[0].bounds[0].span;
    saved_bound_modifier = method->where_predicates[0].bounds[0].modifier;
    saved_bound_trait_type =
        method->where_predicates[0].bounds[0].trait_type;
    cm_hir_lower_options_init(&options);
    options.crate_name = "malformed_predicate_test";
    options.source = 7u;

    method->where_predicates = NULL;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "storage disagree") != NULL);
    cm_hir_context_destroy(&context);
    method->where_predicates = saved_predicates;

    method->where_predicate_count = 0u;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "storage disagree") != NULL);
    cm_hir_context_destroy(&context);
    method->where_predicate_count = 1u;

    method->where_clause = CM_INTERN_ID_NONE;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "storage disagree") != NULL);
    cm_hir_context_destroy(&context);
    method->where_clause = saved_where_clause;

    method->where_clause = UINT32_MAX;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "provenance string") != NULL);
    cm_hir_context_destroy(&context);
    method->where_clause = saved_where_clause;

    method->where_predicates[0].span.start = saved_span.end;
    method->where_predicates[0].span.end = saved_span.start;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "span") != NULL);
    cm_hir_context_destroy(&context);
    method->where_predicates[0].span = saved_span;

    method->where_predicates[0].bounds[0].span.start = saved_bound_span.end;
    method->where_predicates[0].bounds[0].span.end = saved_bound_span.start;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "span") != NULL);
    cm_hir_context_destroy(&context);
    method->where_predicates[0].bounds[0].span = saved_bound_span;

    method->where_predicates[0].bounds[0].modifier =
        (CmAstWhereBoundModifier)99;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "modifier") != NULL);
    cm_hir_context_destroy(&context);
    method->where_predicates[0].bounds[0].modifier = saved_bound_modifier;

    method->where_predicates[0].bounds[0].trait_type = CM_AST_TYPE_NONE;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "trait type") != NULL);
    cm_hir_context_destroy(&context);
    method->where_predicates[0].bounds[0].trait_type =
        saved_bound_trait_type;

    method->where_predicates[0].bound_count = 0u;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "bound storage") != NULL);
    cm_hir_context_destroy(&context);
    method->where_predicates[0].bound_count = 1u;

    subject_path->segments[0].arguments =
        (CmAstGenericArg *)method->where_predicates;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "path storage") != NULL);
    cm_hir_context_destroy(&context);
    subject_path->segments[0].arguments = NULL;
    cm_ast_destroy(&ast);
}

static void test_post_value_where_predicate_storage_mismatch(void)
{
    static const char source[] =
        "type Alias = u8 where Self: Sized;";
    const CmAstItemId *item_id;
    CmAstItem *item;
    CmInternId saved_clause;
    CmAstWherePredicate *saved_predicates;
    CmHirLowerOptions options;
    CmAst ast;
    CmParseResult parse_result;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    item_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = item_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*item_id - 1u);
    assert(parse_result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TYPE_ALIAS
        && item->data.value_item.post_value_where_clause
            != CM_INTERN_ID_NONE
        && item->data.value_item.post_value_where_predicate_count == 1u
        && item->data.value_item.post_value_where_predicates != NULL);
    saved_clause = item->data.value_item.post_value_where_clause;
    saved_predicates = item->data.value_item.post_value_where_predicates;
    cm_hir_lower_options_init(&options);
    options.crate_name = "malformed_post_value_predicate_test";
    options.source = 7u;

    item->data.value_item.post_value_where_predicate_count = 0u;
    expect_invalid_ast_lowering(&ast, &options,
        "post-value where-predicate text and structural storage disagree");
    item->data.value_item.post_value_where_predicate_count = 1u;

    item->data.value_item.post_value_where_predicates = NULL;
    expect_invalid_ast_lowering(&ast, &options,
        "post-value where-predicate text and structural storage disagree");
    item->data.value_item.post_value_where_predicates = saved_predicates;

    item->data.value_item.post_value_where_clause = UINT32_MAX;
    expect_invalid_ast_lowering(&ast, &options,
        "post-value where-clause provenance string is invalid");
    item->data.value_item.post_value_where_clause = saved_clause;
    cm_ast_destroy(&ast);
}

static void test_associated_type_constraint_fails_closed(void)
{
    static const char source[] =
        "trait Bound {} trait Iterator { type Item; } "
        "trait Owner { fn check<T>() where "
        "T: Iterator<Item<'static>: Bound>; }";
    const CmAstItemId *owner_id;
    const CmAstItem *owner;
    CmAstItem *method;
    CmAstWherePredicate *predicate;
    CmAstType *trait_type;
    CmAstPath *trait_path;
    CmAstGenericArg *constraint;
    CmAstSpan saved_span;
    CmInternId saved_name;
    CmInternId saved_text;
    CmAstTypeId saved_type;
    CmAstGenericParamBound *saved_bounds;
    CmAstGenericArg *saved_name_arguments;
    CmAstGenericArgKind saved_argument_kind;
    CmAstGenericParamBoundKind saved_kind;
    CmAstGenericParamBoundModifier saved_modifier;
    CmInternId saved_lifetime;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;
    CmAst ast;
    CmParseResult parse_result;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    owner_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    owner = owner_id == NULL ? NULL : cm_ast_get_item(&ast, *owner_id);
    method = owner == NULL || owner->data.trait_item.item_count != 1u
        ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
            (size_t)owner->data.trait_item.items[0] - 1u);
    predicate = method == NULL || method->where_predicate_count != 1u
            || method->where_predicates == NULL
        ? NULL : &method->where_predicates[0];
    trait_type = predicate == NULL || predicate->bound_count != 1u
            || predicate->bounds == NULL
        ? NULL : (CmAstType *)cm_vec_at(&ast.types,
            (size_t)predicate->bounds[0].trait_type - 1u);
    trait_path = trait_type == NULL || trait_type->kind != CM_AST_TYPE_PATH
        ? NULL : (CmAstPath *)cm_vec_at(&ast.paths,
            (size_t)trait_type->path - 1u);
    constraint = trait_path == NULL || trait_path->segment_count != 1u
            || trait_path->segments == NULL
            || trait_path->segments[0].argument_count != 1u
            || trait_path->segments[0].arguments == NULL
        ? NULL : &trait_path->segments[0].arguments[0];
    assert(parse_result.error_count == 0u && constraint != NULL
        && constraint->kind == CM_AST_GENERIC_CONSTRAINT
        && constraint->name_argument_count == 1u
        && constraint->name_arguments != NULL
        && constraint->name_arguments[0].kind == CM_AST_GENERIC_LIFETIME
        && constraint->bound_count == 1u && constraint->bounds != NULL);

    cm_hir_lower_options_init(&options);
    options.crate_name = "associated_constraint_test";
    options.source = 7u;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && strstr(result.first_error.message,
            "associated-type constraints") != NULL);
    cm_hir_context_destroy(&context);

    constraint->bound_count = 0u;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint is structurally invalid");
    constraint->bound_count = 1u;

    saved_bounds = constraint->bounds;
    constraint->bounds = NULL;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint is structurally invalid");
    constraint->bounds = saved_bounds;

    constraint->name_argument_count = 0u;
    expect_invalid_ast_lowering(&ast, &options,
        "generic associated-type name argument storage is invalid");
    constraint->name_argument_count = 1u;

    saved_name_arguments = constraint->name_arguments;
    constraint->name_arguments = NULL;
    expect_invalid_ast_lowering(&ast, &options,
        "generic associated-type name argument storage is invalid");
    constraint->name_arguments = saved_name_arguments;

    saved_span = constraint->name_arguments[0].span;
    constraint->name_arguments[0].span.start =
        constraint->name_arguments[0].span.end + 1u;
    expect_invalid_ast_lowering(&ast, &options,
        "generic associated-type name argument is malformed");
    constraint->name_arguments[0].span = saved_span;

    saved_argument_kind = constraint->name_arguments[0].kind;
    constraint->name_arguments[0].kind = CM_AST_GENERIC_BINDING;
    expect_invalid_ast_lowering(&ast, &options,
        "generic associated-type name argument is malformed");
    constraint->name_arguments[0].kind = saved_argument_kind;

    saved_text = constraint->name_arguments[0].text;
    constraint->name_arguments[0].text = CM_INTERN_ID_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "generic associated-type name argument is malformed");
    constraint->name_arguments[0].text = saved_text;

    saved_type = constraint->name_arguments[0].type;
    constraint->name_arguments[0].type = predicate->bounds[0].trait_type;
    expect_invalid_ast_lowering(&ast, &options,
        "generic associated-type name argument is malformed");
    constraint->name_arguments[0].type = saved_type;

    saved_span = constraint->span;
    constraint->span.start = constraint->span.end + 1u;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint is structurally invalid");
    constraint->span = saved_span;

    saved_name = constraint->name;
    constraint->name = CM_INTERN_ID_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint is structurally invalid");
    constraint->name = UINT32_MAX;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint is structurally invalid");
    constraint->name = saved_name;

    saved_text = constraint->text;
    constraint->text = CM_INTERN_ID_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint is structurally invalid");
    constraint->text = UINT32_MAX;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint is structurally invalid");
    constraint->text = saved_text;

    saved_type = constraint->type;
    constraint->type = predicate->bounds[0].trait_type;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint is structurally invalid");
    constraint->type = saved_type;

    saved_span = constraint->bounds[0].span;
    constraint->bounds[0].span.start = constraint->bounds[0].span.end + 1u;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint bound span is invalid");
    constraint->bounds[0].span = saved_span;

    saved_modifier = constraint->bounds[0].modifier;
    constraint->bounds[0].modifier = (CmAstGenericParamBoundModifier)99;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type constraint bound has invalid modifier");
    constraint->bounds[0].modifier = saved_modifier;

    saved_kind = constraint->bounds[0].kind;
    constraint->bounds[0].kind = (CmAstGenericParamBoundKind)99;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type trait constraint is malformed");
    constraint->bounds[0].kind = saved_kind;

    constraint->bounds[0].kind = CM_AST_GENERIC_BOUND_LIFETIME;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type lifetime constraint is malformed");
    constraint->bounds[0].kind = saved_kind;

    saved_type = constraint->bounds[0].trait_type;
    constraint->bounds[0].trait_type = CM_AST_TYPE_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type trait constraint is malformed");
    constraint->bounds[0].trait_type = saved_type;

    saved_lifetime = constraint->bounds[0].lifetime;
    constraint->bounds[0].lifetime = constraint->name;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type trait constraint is malformed");
    constraint->bounds[0].lifetime = saved_lifetime;

    constraint->kind = CM_AST_GENERIC_BINDING;
    expect_invalid_ast_lowering(&ast, &options,
        "non-constraint generic argument has constraint bounds");
    constraint->kind = CM_AST_GENERIC_CONSTRAINT;

    constraint->kind = (CmAstGenericArgKind)99;
    expect_invalid_ast_lowering(&ast, &options,
        "generic argument has an invalid kind");
    constraint->kind = CM_AST_GENERIC_CONSTRAINT;
    cm_ast_destroy(&ast);
}

static void test_lifetime_where_predicates_retained(void)
{
    static const char lifetime_bound[] =
        "trait Owner { fn check<T>() where T: 'static; }";
    static const char lifetime_subject[] =
        "trait Owner { fn check() where 'static: 'static; }";
    static const char lifetime_parameters[] =
        "fn type_check<'a, T>() where T: 'a {} "
        "fn lifetime_check<'a, 'b>() where 'a: 'b {}";
    static const char active_lifetime_bound[] =
        "trait Copy {} trait Owner { #[cfg(unix)] "
        "fn check<T>() where T: Copy + 'static; }";
    static const char inactive_lifetime_bound[] =
        "trait Owner { #[cfg(windows)] "
        "fn hidden<T>() where T: 'static; "
        "#[cfg(unix)] fn shown(); }";
    static const char contracts_shape[] =
        "trait Copy {} trait Fn<Args> { type Output; } "
        "pub const fn build_check_ensures<Ret, C>(cond: C) -> C "
        "where C: Fn(&Ret) -> bool + Copy + 'static { cond }";
    const char *anchor;
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *owner;
    const CmHirItem *method;
    const CmHirItem *lifetime_method;
    const CmHirItem *function;
    const CmHirType *subject;
    const CmHirGenericParam *parameter;

    result = lower_source(lifetime_bound, &context, NULL);
    owner = find_item(&context, "Owner");
    method = owner == NULL ? NULL
        : find_child(&context, owner->definition, "check");
    parameter = method == NULL || method->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            method->generic_parameter_start);
    subject = method == NULL || method->outlives_predicate_count != 1u
        ? NULL : cm_hir_get_type(&context,
            method->outlives_predicates[0].subject.type);
    anchor = strstr(lifetime_bound, "T: 'static");
    assert(anchor != NULL && result.error_count == 0u
        && owner != NULL && method != NULL && parameter != NULL
        && parameter->kind == CM_HIR_GENERIC_TYPE
        && method->predicate_count == 0u && method->predicates == NULL
        && method->outlives_predicate_count == 1u
        && method->outlives_predicates != NULL
        && method->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && subject != NULL && subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject->data.parameter_type.parameter
            == method->generic_parameter_start
        && method->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC
        && method->outlives_predicates[0].span.start
            == (uint32_t)(anchor - lifetime_bound)
        && method->outlives_predicates[0].span.end
            == (uint32_t)(anchor - lifetime_bound)
                + (uint32_t)(sizeof("T: 'static") - 1u));
    cm_hir_context_destroy(&context);

    result = lower_source(lifetime_subject, &context, NULL);
    owner = find_item(&context, "Owner");
    method = owner == NULL ? NULL
        : find_child(&context, owner->definition, "check");
    assert(result.error_count == 0u && method != NULL
        && method->outlives_predicate_count == 1u
        && method->outlives_predicates != NULL
        && method->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_LIFETIME
        && method->outlives_predicates[0].subject.lifetime.kind
            == CM_HIR_REGION_STATIC
        && method->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC);
    cm_hir_context_destroy(&context);

    result = lower_source(lifetime_parameters, &context, NULL);
    method = find_item(&context, "type_check");
    lifetime_method = find_item(&context, "lifetime_check");
    assert(result.error_count == 0u && method != NULL
        && lifetime_method != NULL
        && method->generic_parameter_count == 2u
        && method->outlives_predicate_count == 1u
        && method->outlives_predicates != NULL
        && method->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && method->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_EARLY_BOUND
        && method->outlives_predicates[0].bound.data.parameter
            == method->generic_parameter_start
        && lifetime_method->generic_parameter_count == 2u
        && lifetime_method->outlives_predicate_count == 1u
        && lifetime_method->outlives_predicates != NULL
        && lifetime_method->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_LIFETIME
        && lifetime_method->outlives_predicates[0].subject.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && lifetime_method->outlives_predicates[0].subject.lifetime
            .data.parameter == lifetime_method->generic_parameter_start
        && lifetime_method->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_EARLY_BOUND
        && lifetime_method->outlives_predicates[0].bound.data.parameter
            == lifetime_method->generic_parameter_start + 1u);
    cm_hir_context_destroy(&context);

    make_cfg_view(active_lifetime_bound, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    owner = find_item(&context, "Owner");
    method = owner == NULL ? NULL
        : find_child(&context, owner->definition, "check");
    assert(result.error_count == 0u && method != NULL
        && method->predicate_count == 1u
        && method->outlives_predicate_count == 1u
        && method->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    make_cfg_view(inactive_lifetime_bound, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    owner = find_item(&context, "Owner");
    assert(result.error_count == 0u
        && result.lowered_item_count == 2u
        && owner != NULL && owner->kind == CM_HIR_ITEM_TRAIT
        && find_child(&context, owner->definition, "hidden") == NULL
        && find_child(&context, owner->definition, "shown") != NULL);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    result = lower_source(contracts_shape, &context, NULL);
    function = find_item(&context, "build_check_ensures");
    assert(result.error_count == 0u && function != NULL
        && function->kind == CM_HIR_ITEM_FUNCTION
        && function->predicate_count == 2u
        && function->predicates != NULL
        && function->outlives_predicate_count == 1u
        && function->outlives_predicates != NULL
        && function->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && function->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC);
    cm_hir_context_destroy(&context);
}

static void test_lifetime_generic_parameter_bounds(void)
{
    static const char supported_ast[] =
        "fn type_id<T: ?Sized + 'static>() {}";
    static const char lifetime_supported_ast[] =
        "fn inspect<'a: 'static, 'b: 'a, T: 'b + 'static>() {}";
    static const char malformed_ast[] =
        "fn inspect<T: 'static>(value: &T) {}";
    const char *anchor;
    const CmAstItemId *root_id;
    CmAst ast;
    CmParseResult parse_result;
    CmAstItem *function;
    CmAstGenericParam *parameter;
    CmAstGenericParamBound *bound;
    CmAstGenericParamBound *saved_bounds;
    CmAstGenericParamBoundKind saved_kind;
    CmAstGenericParamBoundModifier saved_modifier;
    CmAstTypeId saved_trait_type;
    CmInternId saved_lifetime;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *lowered_function;
    const CmHirGenericParam *lowered_parameter;
    const CmHirType *subject_type;

    result = lower_source(supported_ast, &context, NULL);
    anchor = strstr(supported_ast, "'static");
    lowered_function = find_item(&context, "type_id");
    lowered_parameter = lowered_function == NULL
            || lowered_function->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            lowered_function->generic_parameter_start);
    subject_type = lowered_function == NULL
            || lowered_function->outlives_predicate_count != 1u
        ? NULL : cm_hir_get_type(&context,
            lowered_function->outlives_predicates[0].subject.type);
    assert(anchor != NULL
        && result.error_count == 0u
        && result.lowered_item_count == 1u
        && lowered_function != NULL
        && lowered_function->generic_parameter_count == 1u
        && lowered_function->predicate_count == 0u
        && lowered_function->outlives_predicate_count == 1u
        && lowered_parameter != NULL
        && lowered_parameter->kind == CM_HIR_GENERIC_TYPE
        && lowered_parameter->is_relaxed_sized
        && lowered_function->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && subject_type != NULL
        && subject_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject_type->data.parameter_type.parameter
            == lowered_function->generic_parameter_start
        && lowered_function->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC
        && lowered_function->outlives_predicates[0].span.source == 7u
        && lowered_function->outlives_predicates[0].span.start
            == (uint32_t)(anchor - supported_ast)
        && lowered_function->outlives_predicates[0].span.end
            == (uint32_t)(anchor - supported_ast)
                + (uint32_t)(sizeof("'static") - 1u));
    cm_hir_context_destroy(&context);

    result = lower_source(lifetime_supported_ast, &context, NULL);
    lowered_function = find_item(&context, "inspect");
    subject_type = lowered_function == NULL
            || lowered_function->outlives_predicate_count != 4u
        ? NULL : cm_hir_get_type(&context,
            lowered_function->outlives_predicates[2].subject.type);
    assert(result.error_count == 0u
        && result.lowered_item_count == 1u
        && lowered_function != NULL
        && lowered_function->generic_parameter_count == 3u
        && lowered_function->outlives_predicate_count == 4u
        && lowered_function->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_LIFETIME
        && lowered_function->outlives_predicates[0].subject.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && lowered_function->outlives_predicates[0].subject.lifetime
            .data.parameter == lowered_function->generic_parameter_start
        && lowered_function->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC
        && lowered_function->outlives_predicates[1].subject_kind
            == CM_HIR_OUTLIVES_LIFETIME
        && lowered_function->outlives_predicates[1].subject.lifetime
            .data.parameter
            == lowered_function->generic_parameter_start + 1u
        && lowered_function->outlives_predicates[1].bound.kind
            == CM_HIR_REGION_EARLY_BOUND
        && lowered_function->outlives_predicates[1].bound.data.parameter
            == lowered_function->generic_parameter_start
        && lowered_function->outlives_predicates[2].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && subject_type != NULL
        && subject_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject_type->data.parameter_type.parameter
            == lowered_function->generic_parameter_start + 2u
        && lowered_function->outlives_predicates[2].bound.kind
            == CM_HIR_REGION_EARLY_BOUND
        && lowered_function->outlives_predicates[2].bound.data.parameter
            == lowered_function->generic_parameter_start + 1u
        && lowered_function->outlives_predicates[3].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && lowered_function->outlives_predicates[3].subject.type
            == lowered_function->outlives_predicates[2].subject.type
        && lowered_function->outlives_predicates[3].bound.kind
            == CM_HIR_REGION_STATIC);
    cm_hir_context_destroy(&context);

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, malformed_ast,
        sizeof(malformed_ast) - 1u, CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*root_id - 1u);
    parameter = function == NULL || function->generic_parameter_count != 1u
            || function->generic_parameters == NULL
        ? NULL : &function->generic_parameters[0];
    bound = parameter == NULL || parameter->bound_count != 1u
            || parameter->bounds == NULL
        ? NULL : &parameter->bounds[0];
    assert(parse_result.error_count == 0u && function != NULL
        && parameter != NULL && bound != NULL
        && bound->kind == CM_AST_GENERIC_BOUND_LIFETIME);
    saved_bounds = parameter->bounds;
    saved_kind = bound->kind;
    saved_modifier = bound->modifier;
    saved_trait_type = bound->trait_type;
    saved_lifetime = bound->lifetime;
    cm_hir_lower_options_init(&options);
    options.crate_name = "malformed_lifetime_bound_test";
    options.source = 7u;

    parameter->bounds = NULL;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "bound storage") != NULL);
    cm_hir_context_destroy(&context);
    parameter->bounds = saved_bounds;

    bound->kind = (CmAstGenericParamBoundKind)99;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "bound is malformed")
            != NULL);
    cm_hir_context_destroy(&context);
    bound->kind = saved_kind;

    bound->lifetime = CM_INTERN_ID_NONE;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "lifetime generic") != NULL);
    cm_hir_context_destroy(&context);
    bound->lifetime = saved_lifetime;

    bound->modifier = CM_AST_GENERIC_BOUND_RELAXED;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "lifetime generic") != NULL);
    cm_hir_context_destroy(&context);
    bound->modifier = saved_modifier;

    bound->trait_type = 1u;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "lifetime generic") != NULL);
    cm_hir_context_destroy(&context);
    bound->trait_type = saved_trait_type;

    bound->kind = CM_AST_GENERIC_BOUND_TRAIT;
    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_AST
        && strstr(result.first_error.message, "trait generic") != NULL);
    cm_hir_context_destroy(&context);
    bound->kind = saved_kind;
    cm_ast_destroy(&ast);
}

static void test_generic_parameter_attributes_fail_closed(void)
{
    static const char source[] =
        "fn dropck<#[may_dangle] T>(value: T) {}";
    const char *anchor;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    anchor = strstr(source, "#[may_dangle]");
    assert(anchor != NULL
        && result.error_count == 1u
        && result.lowered_item_count == 0u
        && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && result.first_error.span.source == 7u
        && result.first_error.span.start == (uint32_t)(anchor - source)
        && result.first_error.span.end
            == (uint32_t)(anchor - source)
                + (uint32_t)(sizeof("#[may_dangle]") - 1u)
        && strstr(result.first_error.message,
            "generic parameter attributes") != NULL
        && find_item(&context, "dropck") == NULL);
    cm_hir_context_destroy(&context);
}

static void test_lifetime_trait_bounds_fail_closed(void)
{
    static const char supertrait_source[] = "trait Owner: 'static {}";
    static const char associated_source[] =
        "trait Owner { type Assoc: 'static; }";
    const CmAstItemId *root_id;
    CmAst ast;
    CmParseResult parse_result;
    CmAstItem *item;
    CmAstItem *associated;
    CmAstSupertrait *supertrait;
    CmAstSupertrait *saved_supertraits;
    CmAstAssociatedTypeBound *bound;
    CmAstAssociatedTypeBound *saved_bounds;
    CmInternId saved_supertrait_lifetime;
    CmInternId saved_bound_lifetime;
    CmHirLowerOptions options;

    cm_hir_lower_options_init(&options);
    options.crate_name = "malformed_lifetime_trait_bound_test";
    options.source = 7u;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, supertrait_source,
        sizeof(supertrait_source) - 1u, CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*root_id - 1u);
    supertrait = item == NULL
            || item->data.trait_item.structured_supertrait_count != 1u
            || item->data.trait_item.structured_supertraits == NULL
        ? NULL : &item->data.trait_item.structured_supertraits[0];
    assert(parse_result.error_count == 0u && item != NULL
        && supertrait != NULL
        && supertrait->kind == CM_AST_SUPERTRAIT_LIFETIME);
    saved_supertraits = item->data.trait_item.structured_supertraits;
    saved_supertrait_lifetime = supertrait->lifetime;

    item->data.trait_item.structured_supertraits = NULL;
    expect_invalid_ast_lowering(&ast, &options, "structural bounds");
    item->data.trait_item.structured_supertraits = saved_supertraits;

    supertrait->kind = (CmAstSupertraitKind)99;
    expect_invalid_ast_lowering(&ast, &options,
        "supertrait bound is malformed");
    supertrait->kind = CM_AST_SUPERTRAIT_LIFETIME;

    supertrait->lifetime = CM_INTERN_ID_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "lifetime supertrait bound is malformed");
    supertrait->lifetime = saved_supertrait_lifetime;

    supertrait->modifier = CM_AST_SUPERTRAIT_CONDITIONALLY_CONST;
    expect_invalid_ast_lowering(&ast, &options,
        "lifetime supertrait bound is malformed");
    supertrait->modifier = CM_AST_SUPERTRAIT_REQUIRED;

    supertrait->type = 1u;
    expect_invalid_ast_lowering(&ast, &options,
        "lifetime supertrait bound is malformed");
    supertrait->type = CM_AST_TYPE_NONE;

    supertrait->kind = CM_AST_SUPERTRAIT_TRAIT;
    expect_invalid_ast_lowering(&ast, &options,
        "trait supertrait bound is malformed");
    cm_ast_destroy(&ast);

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, associated_source,
        sizeof(associated_source) - 1u, CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*root_id - 1u);
    associated = item == NULL || item->data.trait_item.item_count != 1u
            || item->data.trait_item.items == NULL
        ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
            (size_t)item->data.trait_item.items[0] - 1u);
    bound = associated == NULL
            || associated->data.value_item.bound_count != 1u
            || associated->data.value_item.bounds == NULL
        ? NULL : &associated->data.value_item.bounds[0];
    assert(parse_result.error_count == 0u && associated != NULL
        && bound != NULL && bound->kind == CM_AST_ASSOC_BOUND_LIFETIME);
    saved_bounds = associated->data.value_item.bounds;
    saved_bound_lifetime = bound->lifetime;

    associated->data.value_item.bounds = NULL;
    expect_invalid_ast_lowering(&ast, &options, "count has no storage");
    associated->data.value_item.bounds = saved_bounds;

    bound->kind = (CmAstAssociatedTypeBoundKind)99;
    expect_invalid_ast_lowering(&ast, &options,
        "associated-type bound is malformed");
    bound->kind = CM_AST_ASSOC_BOUND_LIFETIME;

    bound->lifetime = CM_INTERN_ID_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "lifetime associated-type bound is malformed");
    bound->lifetime = saved_bound_lifetime;

    bound->modifier = CM_AST_ASSOC_BOUND_RELAXED;
    expect_invalid_ast_lowering(&ast, &options,
        "lifetime associated-type bound is malformed");
    bound->modifier = CM_AST_ASSOC_BOUND_REQUIRED;

    bound->trait_type = 1u;
    expect_invalid_ast_lowering(&ast, &options,
        "lifetime associated-type bound is malformed");
    bound->trait_type = CM_AST_TYPE_NONE;

    bound->kind = CM_AST_ASSOC_BOUND_TRAIT;
    expect_invalid_ast_lowering(&ast, &options,
        "trait associated-type bound is malformed");
    cm_ast_destroy(&ast);
}

static void test_associated_lifetime_bound_record_rollback(void)
{
    static const char source[] =
        "trait Owner { type Assoc: 'static + '_; }";
    const CmAstItemId *root_id;
    CmAst ast;
    CmParseResult parse_result;
    CmAstItem *owner;
    CmAstItem *associated;
    CmAstAssociatedTypeBound *bounds;
    CmInternId saved_first_lifetime;
    CmHirLowerOptions options;
    CmHirContext first_context;
    CmHirContext second_context;
    CmHirLowerResult first_result;
    CmHirLowerResult second_result;

    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    owner = root_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*root_id - 1u);
    associated = owner == NULL || owner->kind != CM_AST_ITEM_TRAIT
            || owner->data.trait_item.item_count != 1u
        ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
            (size_t)owner->data.trait_item.items[0] - 1u);
    bounds = associated == NULL
            || associated->data.value_item.bound_count != 2u
        ? NULL : associated->data.value_item.bounds;
    assert(parse_result.error_count == 0u && bounds != NULL
        && bounds[0].kind == CM_AST_ASSOC_BOUND_LIFETIME
        && bounds[1].kind == CM_AST_ASSOC_BOUND_LIFETIME);

    cm_hir_lower_options_init(&options);
    options.crate_name = "associated_lifetime_record_rollback";
    options.source = 7u;
    saved_first_lifetime = bounds[0].lifetime;
    bounds[0].lifetime = bounds[1].lifetime;
    cm_hir_context_init(&first_context);
    first_result = cm_hir_lower_crate(&first_context, &ast, &options);
    bounds[0].lifetime = saved_first_lifetime;
    cm_hir_context_init(&second_context);
    second_result = cm_hir_lower_crate(&second_context, &ast, &options);

    assert(first_result.error_count == 1u
        && second_result.error_count == 1u
        && first_result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && second_result.first_error.kind
            == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && strstr(first_result.first_error.message,
            "authenticated enclosing lifetime") != NULL
        && strstr(second_result.first_error.message,
            "authenticated enclosing lifetime") != NULL
        && first_context.crates.len == second_context.crates.len
        && first_context.modules.len == second_context.modules.len
        && first_context.items.len == second_context.items.len
        && first_context.items.len == 1u
        && first_context.types.len == second_context.types.len
        && first_context.types.len == 0u
        && first_context.generic_parameters.len
            == second_context.generic_parameters.len
        && first_context.generic_parameters.len == 0u
        && first_context.definitions.len == second_context.definitions.len
        && first_context.prebound_associated_types.len
            == second_context.prebound_associated_types.len
        && first_context.prebound_associated_types.len == 1u
        && cm_arena_bytes_used(&first_context.storage)
            == cm_arena_bytes_used(&second_context.storage)
        && cm_interner_length(&first_context.strings)
            == cm_interner_length(&second_context.strings)
        && cm_arena_bytes_used(&first_context.strings.strings)
            == cm_arena_bytes_used(&second_context.strings.strings)
        && find_child(&first_context,
            find_item(&first_context, "Owner")->definition, "Assoc")
            == NULL
        && find_child(&second_context,
            find_item(&second_context, "Owner")->definition, "Assoc")
            == NULL);
    cm_hir_context_destroy(&second_context);
    cm_hir_context_destroy(&first_context);
    cm_ast_destroy(&ast);
}

static void test_argument_impl_trait_lowers(void)
{
    static const char source[] =
        "trait Bound<T> {} trait Send {} "
        "trait Owner { fn consume<'a, T>(&self, "
        "first: impl Bound<T> + 'a, second: &impl Bound<&'a T> + ?Sized, "
        "third: impl ~const Send); }";
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *owner;
    const CmHirItem *method;
    const CmHirItem *bound;
    const CmHirItem *send;
    const CmHirGenericParam *first;
    const CmHirGenericParam *second;
    const CmHirGenericParam *third;
    const CmHirType *first_type;
    const CmHirType *second_type;
    const CmHirType *second_pointee;
    const CmHirType *third_type;

    result = lower_source(source, &context, NULL);
    owner = find_item(&context, "Owner");
    method = owner == NULL ? NULL : find_child(&context, owner->definition,
        "consume");
    bound = find_item(&context, "Bound");
    send = find_item(&context, "Send");
    first = method == NULL || method->generic_parameter_count != 5u ? NULL
        : cm_hir_get_generic_param(&context, method->generic_parameter_start + 2u);
    second = method == NULL || method->generic_parameter_count != 5u ? NULL
        : cm_hir_get_generic_param(&context, method->generic_parameter_start + 3u);
    third = method == NULL || method->generic_parameter_count != 5u ? NULL
        : cm_hir_get_generic_param(&context, method->generic_parameter_start + 4u);
    first_type = method == NULL ? NULL : cm_hir_get_type(&context,
        method->data.function_item.signature.parameters[1].type);
    second_type = method == NULL ? NULL : cm_hir_get_type(&context,
        method->data.function_item.signature.parameters[2].type);
    second_pointee = second_type == NULL || second_type->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&context, second_type->data.reference_type.pointee);
    third_type = method == NULL ? NULL : cm_hir_get_type(&context,
        method->data.function_item.signature.parameters[3].type);
    assert(result.error_count == 0u && owner != NULL && method != NULL
        && bound != NULL && send != NULL && method->generic_parameter_count == 5u
        && first != NULL && second != NULL && third != NULL
        && first->kind == CM_HIR_GENERIC_TYPE && second->kind == CM_HIR_GENERIC_TYPE
        && third->kind == CM_HIR_GENERIC_TYPE
        && first->index == 2u && second->index == 3u && third->index == 4u
        && cm_hir_def_id_equal(first->owner, method->definition)
        && cm_hir_def_id_equal(second->owner, method->definition)
        && cm_hir_def_id_equal(third->owner, method->definition)
        && hir_string_is(&context, first->name, "$APIT0")
        && hir_string_is(&context, second->name, "$APIT1")
        && hir_string_is(&context, third->name, "$APIT2")
        && second->is_relaxed_sized
        && first_type != NULL && first_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && first_type->data.parameter_type.parameter == method->generic_parameter_start + 2u
        && second_pointee != NULL && second_pointee->kind == CM_HIR_TYPE_PARAMETER_KIND
        && second_pointee->data.parameter_type.parameter == method->generic_parameter_start + 3u
        && third_type != NULL && third_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && third_type->data.parameter_type.parameter == method->generic_parameter_start + 4u
        && method->predicate_count == 3u
        && cm_hir_def_id_equal(method->predicates[0].trait_type.definition, bound->definition)
        && cm_hir_def_id_equal(method->predicates[2].trait_type.definition, send->definition)
        && method->predicates[2].modifier == CM_HIR_PREDICATE_CONST_IF_CONST
        && method->outlives_predicate_count == 1u);
    cm_hir_context_destroy(&context);
}

static void test_nested_argument_impl_trait_lowers(void)
{
    static const char source[] =
        "trait Send {} struct Wrapper<T>; fn nested(value: Wrapper<impl Send>) {}";
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *nested;
    const CmHirType *outer;
    const CmHirType *argument;

    result = lower_source(source, &context, NULL);
    nested = find_item(&context, "nested");
    outer = nested == NULL ? NULL : cm_hir_get_type(&context,
        nested->data.function_item.signature.parameters[0].type);
    argument = outer == NULL || outer->kind != CM_HIR_TYPE_ADT_KIND
        || outer->data.named_type.argument_count != 1u ? NULL
        : cm_hir_get_type(&context, outer->data.named_type.arguments[0].data.type);
    assert(result.error_count == 0u && nested != NULL
        && nested->generic_parameter_count == 1u && argument != NULL
        && argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && argument->data.parameter_type.parameter == nested->generic_parameter_start);
    cm_hir_context_destroy(&context);
}

static void test_argument_impl_trait_method_parity(void)
{
    static const char source[] =
        "trait Send {} trait Consumer { fn consume(value: impl Send); }"
        "impl Consumer for u8 { fn consume(value: impl Send) {} }";
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *consumer = NULL;
    const CmHirItem *impl_item;
    const CmHirItem *trait_method;
    const CmHirItem *impl_method;

    result = lower_source(source, &context, NULL);
    consumer = find_item(&context, "Consumer");
    impl_item = find_impl(&context);
    trait_method = consumer == NULL ? NULL : find_child(&context,
        consumer->definition, "consume");
    impl_method = impl_item == NULL ? NULL : find_child(&context,
        impl_item->definition, "consume");
    assert(result.error_count == 0u && trait_method != NULL
        && impl_method != NULL && trait_method->generic_parameter_count == 1u
        && impl_method->generic_parameter_count == 1u
        && trait_method->predicate_count == 1u && impl_method->predicate_count == 1u
        && cm_hir_def_id_equal(impl_method->data.function_item.trait_item_definition,
            trait_method->definition));
    cm_hir_context_destroy(&context);

    result = lower_source(
        "trait Send {} trait Consumer { fn consume(value: impl Send); }"
        "impl Consumer for u8 { fn consume<T: Send>(value: T) {} }",
        &context, NULL);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_IMPL
        && strstr(result.first_error.message,
            "explicit generic arity differs") != NULL);
    cm_hir_context_destroy(&context);

    result = lower_source(
        "trait Send {} trait Consumer { fn consume<T: Send>(value: T); }"
        "impl Consumer for u8 { fn consume(value: impl Send) {} }",
        &context, NULL);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_IMPL
        && strstr(result.first_error.message,
            "explicit generic arity differs") != NULL);
    cm_hir_context_destroy(&context);

}

static void test_argument_impl_trait_foreign_rejected(void)
{
    static const char source[] = "extern \"C\" { fn consume(value: impl Send); } trait Send {}";
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    assert(result.error_count == 1u);
    cm_hir_context_destroy(&context);
}

static void test_higher_ranked_impl_trait_fails_closed(void)
{
    static const char source[] =
        "fn consume(func: impl for<'a> Fn(&'a u8)) {}";
    const CmAstItemId *root_id;
    CmAst ast;
    CmParseResult parse_result;
    CmAstItem *function;
    CmAstType *opaque;
    CmAstTypeBound *bound;
    CmInternId *saved_lifetimes;
    CmInternId saved_lifetime;
    CmAstSpan saved_span;
    CmHirLowerOptions options;

    cm_hir_lower_options_init(&options);
    options.crate_name = "malformed_higher_ranked_impl_trait_test";
    options.source = 7u;
    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*root_id - 1u);
    opaque = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
            || function->data.function_item.parameter_count != 1u
            || function->data.function_item.parameters == NULL
        ? NULL : (CmAstType *)cm_vec_at(&ast.types,
            (size_t)function->data.function_item.parameters[0].type - 1u);
    bound = opaque == NULL || opaque->kind != CM_AST_TYPE_IMPL_TRAIT
            || opaque->bound_count != 1u || opaque->bounds == NULL
        ? NULL : &opaque->bounds[0];
    assert(parse_result.error_count == 0u && bound != NULL
        && bound->binder.lifetime_count == 1u
        && bound->binder.lifetimes != NULL);
    saved_lifetimes = bound->binder.lifetimes;
    saved_lifetime = saved_lifetimes[0];
    saved_span = bound->binder.span;

    bound->binder.lifetimes = NULL;
    expect_invalid_ast_lowering(&ast, &options,
        "opaque impl trait bound storage is invalid");
    bound->binder.lifetimes = saved_lifetimes;

    bound->binder.lifetimes[0] = CM_INTERN_ID_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "opaque impl trait bound storage is invalid");
    bound->binder.lifetimes[0] = saved_lifetime;

    bound->binder.span.end = bound->binder.span.start;
    expect_invalid_ast_lowering(&ast, &options,
        "opaque impl trait bound storage is invalid");
    bound->binder.span = saved_span;
    cm_ast_destroy(&ast);
}

static void test_higher_ranked_where_bound_lowers(void)
{
    static const char source[] =
        "struct VaList<'copy, 'f>(&'copy u8, &'f u8); "
        "trait FnOnce<Args> { type Output; } "
        "fn with_copy<'f, F, R>(f: F) -> R where "
        "F: for<'copy> FnOnce(VaList<'copy, 'f>) -> R { loop {} }";
    const CmAstItemId *method_id;
    CmAstItem *method;
    CmAstWhereBound *bound;
    const CmHirItem *hir_method;
    const CmHirTraitPredicate *hir_predicate;
    const CmHirType *call_tuple;
    const CmHirType *va_list;
    const CmInternedString *binder_name;
    CmInternId *saved_lifetimes;
    CmInternId saved_lifetime;
    CmAstSpan saved_span;
    CmAst ast;
    CmParseResult parse_result;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;

    cm_hir_lower_options_init(&options);
    options.crate_name = "higher_ranked_where_bound_test";
    options.source = 7u;
    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    method_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    method = method_id == NULL ? NULL : (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)*method_id - 1u);
    bound = method == NULL || method->where_predicate_count != 1u
            || method->where_predicates == NULL
            || method->where_predicates[0].bound_count != 1u
            || method->where_predicates[0].bounds == NULL
        ? NULL : &method->where_predicates[0].bounds[0];
    assert(parse_result.error_count == 0u && bound != NULL
        && bound->binder.lifetime_count == 1u
        && bound->binder.lifetimes != NULL);

    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    hir_method = find_item(&context, "with_copy");
    hir_predicate = hir_method == NULL || hir_method->predicate_count != 1u
        ? NULL : &hir_method->predicates[0];
    binder_name = hir_predicate == NULL
        || hir_predicate->binder.lifetime_count != 1u
        || hir_predicate->binder.lifetimes == NULL
        ? NULL : cm_interner_get(&context.strings,
            hir_predicate->binder.lifetimes[0]);
    call_tuple = hir_predicate == NULL
            || hir_predicate->trait_type.argument_count != 1u
            || hir_predicate->trait_type.arguments == NULL
            || hir_predicate->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            hir_predicate->trait_type.arguments[0].data.type);
    va_list = call_tuple == NULL
            || call_tuple->kind != CM_HIR_TYPE_TUPLE_KIND
            || call_tuple->data.tuple_type.element_count != 1u
            || call_tuple->data.tuple_type.elements == NULL
        ? NULL : cm_hir_get_type(&context,
            call_tuple->data.tuple_type.elements[0]);
    assert(result.error_count == 0u && hir_predicate != NULL
        && binder_name != NULL && binder_name->len == strlen("'copy")
        && memcmp(binder_name->bytes, "'copy", binder_name->len) == 0
        && hir_predicate->binder.span.source == options.source
        && hir_predicate->binder.span.start == bound->binder.span.start
        && hir_predicate->binder.span.end == bound->binder.span.end
        && va_list != NULL && va_list->kind == CM_HIR_TYPE_ADT_KIND
        && va_list->data.named_type.argument_count == 2u
        && va_list->data.named_type.arguments != NULL
        && va_list->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && va_list->data.named_type.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_LATE_BOUND
        && va_list->data.named_type.arguments[0].data.lifetime.data.binder_index
            == 0u
        && va_list->data.named_type.arguments[1].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && va_list->data.named_type.arguments[1].data.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && hir_predicate->equality_count == 1u
        && hir_predicate->equalities != NULL);
    cm_hir_context_destroy(&context);

    saved_lifetimes = bound->binder.lifetimes;
    saved_lifetime = saved_lifetimes[0];
    saved_span = bound->binder.span;
    bound->binder.lifetimes = NULL;
    expect_invalid_ast_lowering(&ast, &options,
        "where-bound lifetime binder storage is invalid");
    bound->binder.lifetimes = saved_lifetimes;

    bound->binder.lifetimes[0] = CM_INTERN_ID_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "where-bound lifetime binder storage is invalid");
    bound->binder.lifetimes[0] = saved_lifetime;

    bound->binder.span.end = bound->binder.span.start;
    expect_invalid_ast_lowering(&ast, &options,
        "where-bound lifetime binder storage is invalid");
    bound->binder.span = saved_span;
    cm_ast_destroy(&ast);
}

static void test_higher_ranked_where_predicate_lowers(void)
{
    static const char source[] =
        "struct GenericShunt<'a, I, R>(&'a I, &'a R); "
        "trait FnMut<Args> { type Output; } "
        "fn adapt<I, R, F, U>() where "
        "for<'a> F: FnMut(GenericShunt<'a, I, R>) -> U {}";
    const CmAstItemId *function_id;
    CmAstItem *function;
    CmAstWherePredicate *predicate;
    const CmHirItem *hir_function;
    const CmHirPredicateScope *scope;
    const CmHirTraitPredicate *hir_predicate;
    const CmHirType *call_tuple;
    const CmHirType *shunt;
    const CmInternedString *binder_name;
    CmInternId *saved_lifetimes;
    CmInternId saved_lifetime;
    CmAstSpan saved_span;
    CmAst ast;
    CmParseResult parse_result;
    CmHirLowerOptions options;
    CmHirContext context;
    CmHirLowerResult result;

    cm_hir_lower_options_init(&options);
    options.crate_name = "higher_ranked_where_predicate_test";
    options.source = 7u;
    cm_ast_init(&ast);
    parse_result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    function_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    function = function_id == NULL ? NULL : (CmAstItem *)cm_vec_at(
        &ast.items, (size_t)*function_id - 1u);
    predicate = function == NULL || function->where_predicate_count != 1u
            || function->where_predicates == NULL
        ? NULL : &function->where_predicates[0];
    assert(parse_result.error_count == 0u && predicate != NULL
        && predicate->binder.lifetime_count == 1u
        && predicate->binder.lifetimes != NULL);

    cm_hir_context_init(&context);
    result = cm_hir_lower_crate(&context, &ast, &options);
    hir_function = find_item(&context, "adapt");
    scope = hir_function == NULL || hir_function->predicate_scope_count != 1u
            || hir_function->predicate_scopes == NULL
        ? NULL : &hir_function->predicate_scopes[0];
    hir_predicate = hir_function == NULL || hir_function->predicate_count != 1u
            || hir_function->predicates == NULL
        ? NULL : &hir_function->predicates[0];
    binder_name = scope == NULL || scope->binder.lifetime_count != 1u
            || scope->binder.lifetimes == NULL
        ? NULL : cm_interner_get(&context.strings,
            scope->binder.lifetimes[0]);
    call_tuple = hir_predicate == NULL
            || hir_predicate->trait_type.argument_count != 1u
            || hir_predicate->trait_type.arguments == NULL
            || hir_predicate->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            hir_predicate->trait_type.arguments[0].data.type);
    shunt = call_tuple == NULL
            || call_tuple->kind != CM_HIR_TYPE_TUPLE_KIND
            || call_tuple->data.tuple_type.element_count != 1u
            || call_tuple->data.tuple_type.elements == NULL
        ? NULL : cm_hir_get_type(&context,
            call_tuple->data.tuple_type.elements[0]);
    assert(result.error_count == 0u && scope != NULL
        && hir_predicate != NULL && binder_name != NULL
        && binder_name->len == strlen("'a")
        && memcmp(binder_name->bytes, "'a", binder_name->len) == 0
        && scope->subject_kind == CM_HIR_OUTLIVES_TYPE
        && scope->subject.type == hir_predicate->subject
        && scope->trait_predicate_count == 1u
        && scope->outlives_predicate_count == 0u
        && scope->span.source == options.source
        && scope->span.start == predicate->span.start
        && scope->span.end == predicate->span.end
        && scope->binder.span.source == options.source
        && scope->binder.span.start == predicate->binder.span.start
        && scope->binder.span.end == predicate->binder.span.end
        && hir_predicate->scope == 1u
        && hir_predicate->binder.lifetime_count == 0u
        && shunt != NULL && shunt->kind == CM_HIR_TYPE_ADT_KIND
        && shunt->data.named_type.argument_count == 3u
        && shunt->data.named_type.arguments != NULL
        && shunt->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && shunt->data.named_type.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_LATE_BOUND
        && shunt->data.named_type.arguments[0].data.lifetime.data.binder_index
            == 0u
        && hir_predicate->equality_count == 1u
        && hir_predicate->equalities != NULL);
    cm_hir_context_destroy(&context);

    saved_lifetimes = predicate->binder.lifetimes;
    saved_lifetime = saved_lifetimes[0];
    saved_span = predicate->binder.span;
    predicate->binder.lifetimes = NULL;
    expect_invalid_ast_lowering(&ast, &options,
        "where-predicate lifetime binder storage is invalid");
    predicate->binder.lifetimes = saved_lifetimes;

    predicate->binder.lifetimes[0] = CM_INTERN_ID_NONE;
    expect_invalid_ast_lowering(&ast, &options,
        "where-predicate lifetime binder storage is invalid");
    predicate->binder.lifetimes[0] = saved_lifetime;

    predicate->binder.span.end = predicate->binder.span.start;
    expect_invalid_ast_lowering(&ast, &options,
        "where-predicate lifetime binder storage is invalid");
    predicate->binder.span = saved_span;
    cm_ast_destroy(&ast);
}

static void test_higher_ranked_trait_lifetime_argument_lowers(void)
{
    static const char source[] =
        "trait Base<'a> {} "
        "fn check<T>() where for<'a> T: Base<'a> {}";
    const CmHirItem *function;
    const CmHirPredicateScope *scope;
    const CmHirTraitPredicate *predicate;
    const CmHirGenericArg *argument;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    function = find_item(&context, "check");
    scope = function == NULL || function->predicate_scope_count != 1u
            || function->predicate_scopes == NULL
        ? NULL : &function->predicate_scopes[0];
    predicate = function == NULL || function->predicate_count != 1u
            || function->predicates == NULL
        ? NULL : &function->predicates[0];
    argument = predicate == NULL
            || predicate->trait_type.argument_count != 1u
            || predicate->trait_type.arguments == NULL
        ? NULL : &predicate->trait_type.arguments[0];
    assert(result.error_count == 0u
        && function != NULL
        && scope != NULL
        && scope->binder.lifetime_count == 1u
        && scope->binder.lifetimes != NULL
        && predicate != NULL
        && predicate->scope == 1u
        && argument != NULL
        && argument->kind == CM_HIR_GENERIC_ARG_LIFETIME
        && argument->data.lifetime.kind == CM_HIR_REGION_LATE_BOUND
        && argument->data.lifetime.data.binder_index == 0u);
    cm_hir_context_destroy(&context);
}

static void test_unsupported_method_forms_are_errors(void)
{
    static const char *const rejected[] = {
        "trait T { fn f(); } impl T for u8 { fn f<U>() {} }",
        "trait T { fn f() where u8: Copy; }",
        "trait T { fn f(); } impl T for u8 { fn f() where u8: Copy {} }",
        "trait T { extern \"C\" fn f(); }",
        "trait T { fn f(); } impl T for u8 { extern \"C\" fn f() {} }",
        "trait T { const fn f(); }",
        "trait T { fn f(); } impl T for u8 { const fn f() {} }",
        "trait T { fn f(); } impl T for u8 { async fn f() {} }",
        "trait T { fn f((left, right): (u8, u8)); }",
        ("trait T { fn f(value: (u8, u8)); } impl T for u8 {"
            " fn f((left, right): (u8, u8)) {} }")
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM
    };
    static const char *const rejected_messages[] = {
        "generic arity differs",
        "predicate subject must be",
        "predicate subject must be",
        "Rust or rust-call ABI",
        "Rust or rust-call ABI",
        "Rust or rust-call ABI",
        "Rust or rust-call ABI",
        "Rust or rust-call ABI",
        "parameter patterns require typed pattern HIR",
        "parameter patterns require typed pattern HIR"
    };
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *method;
    size_t index;

    result = lower_source("trait T { fn f<U>(); }", &context, NULL);
    assert(result.error_count == 0u && result.lowered_item_count == 2u);
    cm_hir_context_destroy(&context);

    result = lower_source("trait T { async fn f(); }", &context, NULL);
    trait_item = find_item(&context, "T");
    method = trait_item == NULL ? NULL
        : find_child(&context, trait_item->definition, "f");
    assert(result.error_count == 0u && result.lowered_item_count == 2u
        && method != NULL && method->kind == CM_HIR_ITEM_FUNCTION
        && method->data.function_item.signature.is_async
        && method->data.function_item.body == CM_HIR_BODY_NONE);
    cm_hir_context_destroy(&context);

    make_cfg_view("trait T { #[allow(async_fn_in_trait)] async fn f(); }",
        &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    trait_item = find_item(&context, "T");
    method = trait_item == NULL ? NULL
        : find_child(&context, trait_item->definition, "f");
    assert(result.error_count == 0u && result.lowered_item_count == 2u
        && method != NULL && method->data.function_item.signature.is_async
        && method->attribute_count == 1u
        && hir_string_is(&context, method->attributes[0].metadata,
            "allow(async_fn_in_trait)"));
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected_kinds[index]
            || strstr(result.first_error.message,
                rejected_messages[index]) == NULL) {
            fprintf(stderr,
                "unsupported method rejection mismatch for %s: "
                "count=%lu kind=%s message=%s\n",
                rejected[index], (unsigned long)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]
            && strstr(result.first_error.message,
                rejected_messages[index]) != NULL);
        cm_hir_context_destroy(&context);
    }
}

static void test_cfg_sensitive_trait_impl_members(void)
{
    static const char paired_members[] =
        "trait T {"
        " #[cfg(unix)] type A;"
        " #[cfg(windows)] type Inactive;"
        "}"
        "impl T for u8 {"
        " #[cfg(unix)] type A = u8;"
        " #[cfg(windows)] type Inactive = u16;"
        "}";
    static const char disabled_extra[] =
        "trait T { type A; }"
        "impl T for u8 {"
        " type A = u8;"
        " #[cfg(windows)] type Extra = u16;"
        "}";
    static const char disabled_required[] =
        "trait T { type A; }"
        "impl T for u8 { #[cfg(windows)] type A = u8; }";
    static const char *const direct_sources[] = {
        paired_members, disabled_extra, disabled_required
    };
    static const char *const direct_messages[] = {
        "associated types must be targetless",
        "impl associated types must be bare",
        "impl associated types must be bare"
    };
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    size_t index;

    for (index = 0u;
         index < sizeof(direct_sources) / sizeof(direct_sources[0]);
         ++index) {
        result = lower_source(direct_sources[index], &context, NULL);
        assert(result.error_count == 1u
            && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM
            && strstr(result.first_error.message, direct_messages[index])
                != NULL);
        cm_hir_context_destroy(&context);
    }

    make_cfg_view(paired_members, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u && result.lowered_item_count == 4u);
    trait_item = find_item(&context, "T");
    impl_item = find_impl(&context);
    assert(trait_item != NULL && impl_item != NULL
        && find_child(&context, trait_item->definition, "A") != NULL
        && find_child(&context, impl_item->definition, "A") != NULL
        && find_child(&context, trait_item->definition, "Inactive") == NULL
        && find_child(&context, impl_item->definition, "Inactive") == NULL);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    make_cfg_view(disabled_extra, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u && result.lowered_item_count == 4u);
    impl_item = find_impl(&context);
    assert(impl_item != NULL
        && find_child(&context, impl_item->definition, "A") != NULL
        && find_child(&context, impl_item->definition, "Extra") == NULL);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    make_cfg_view(disabled_required, &ast, &expanded);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_IMPL
        && strstr(result.first_error.message,
            "missing a required associated type") != NULL);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void test_cfg_active_tree_drives_lowering(void)
{
    static const char source[] =
        "#[cfg(windows)] struct DisabledRoot { bad: MissingRoot }"
        "mod disabled_module { #![cfg(windows)] "
        "struct DisabledByInner { bad: MissingInner } }"
        "#[cfg(unix)] #[cfg_attr(windows, repr(C))] struct Node {"
        " next: *const Node, child: active::Child"
        "}"
        "#[cfg(unix)] mod active {"
        " #[cfg(windows)] struct DisabledNested { bad: MissingNested }"
        " #[cfg(unix)] struct Child { parent: *const super::Node }"
        "}";
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *node;
    const CmHirItem *child;
    const CmHirType *pointer;
    const CmHirType *named;

    make_cfg_view(source, &ast, &expanded);
    assert(expanded.crate_is_active);
    assert(expanded.root_item_count == 2u);
    assert(expanded.root_items[0].attribute_count == 0u);
    assert(expanded.root_items[1].child_kind
        == CM_EXPANDED_CHILD_MODULE);
    assert(expanded.root_items[1].child_count == 1u);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 0u);
    assert(result.lowered_item_count == 3u);
    assert(context.items.len == 3u);
    assert(context.modules.len == 2u);
    assert(find_item(&context, "DisabledRoot") == NULL);
    assert(find_item(&context, "DisabledByInner") == NULL);
    assert(find_item(&context, "DisabledNested") == NULL);
    node = find_item(&context, "Node");
    child = find_item(&context, "Child");
    assert(node != NULL && child != NULL);
    pointer = cm_hir_get_type(&context,
        node->data.aggregate_item.fields[0].type);
    assert(pointer != NULL
        && pointer->kind == CM_HIR_TYPE_RAW_POINTER_KIND);
    named = cm_hir_get_type(&context,
        pointer->data.raw_pointer_type.pointee);
    assert(named != NULL && named->kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(named->data.named_type.definition,
        node->definition));
    named = cm_hir_get_type(&context,
        node->data.aggregate_item.fields[1].type);
    assert(named != NULL && named->kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(named->data.named_type.definition,
        child->definition));
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    make_cfg_view(
        "mod marked { #![cfg_attr(unix, allow(dead_code))] struct Child; }",
        &ast, &expanded);
    assert(expanded.root_item_count == 1u);
    assert(expanded.root_items[0].inner_attribute_count == 1u);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM);
    assert(strstr(result.first_error.message, "inner") != NULL);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void test_effective_attribute_is_not_discarded(void)
{
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *marked;
    const CmInternedString *metadata;

    make_cfg_view("#[cfg_attr(unix, repr(C))] struct Marked;", &ast,
        &expanded);
    assert(expanded.root_item_count == 1u);
    assert(expanded.root_items[0].attribute_count == 1u);
    result = lower_cfg_view(&context, &ast, &expanded);
    marked = find_item(&context, "Marked");
    metadata = marked == NULL || marked->attribute_count == 0u ? NULL
        : cm_interner_get(&context.strings,
            marked->attributes[0].metadata);
    assert(result.error_count == 0u && marked != NULL
        && marked->attribute_count == 1u && metadata != NULL
        && metadata->len == sizeof("repr(C)") - 1u
        && memcmp(metadata->bytes, "repr(C)", metadata->len) == 0
        && marked->attributes[0].source_attribute
            == expanded.root_items[0].attributes[0].source_id
        && marked->attributes[0].expansion_depth
            == expanded.root_items[0].attributes[0].expansion_depth);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void test_inactive_and_invalid_views_are_rejected(void)
{
    CmAst ast;
    CmExpandedAst expanded;
    CmHirContext context;
    CmHirLowerResult result;
    CmAstItemId saved_id;
    CmExpandedChildKind saved_kind;

    make_cfg_view("#![cfg(windows)] struct Hidden;", &ast, &expanded);
    assert(!expanded.crate_is_active);
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_INACTIVE_CRATE);
    assert(context.crates.len == 0u);
    cm_hir_context_destroy(&context);
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);

    make_cfg_view("mod inner { struct Child; } struct Root;", &ast,
        &expanded);
    assert(expanded.root_item_count == 2u);
    saved_id = expanded.root_items[0].source_id;
    expanded.root_items[0].source_id = UINT32_MAX;
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_INVALID_AST);
    assert(context.crates.len == 0u);
    cm_hir_context_destroy(&context);
    expanded.root_items[0].source_id = saved_id;

    saved_kind = expanded.root_items[0].child_kind;
    expanded.root_items[0].child_kind = CM_EXPANDED_CHILD_NONE;
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_INVALID_AST);
    assert(context.crates.len == 0u);
    cm_hir_context_destroy(&context);
    expanded.root_items[0].child_kind = saved_kind;

    saved_id = expanded.root_items[0].children[0].source_id;
    expanded.root_items[0].children[0].source_id =
        expanded.root_items[1].source_id;
    result = lower_cfg_view(&context, &ast, &expanded);
    assert(result.error_count == 1u);
    assert(result.first_error.kind == CM_HIR_LOWER_INVALID_AST);
    assert(context.crates.len == 0u);
    cm_hir_context_destroy(&context);
    expanded.root_items[0].children[0].source_id = saved_id;
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void test_associated_type_bound_scope_errors(void)
{
    static const char *const rejected[] = {
        "trait Bound {} type Alias: Bound;",
        "trait Bound {} trait Owner { type Assoc; } struct Value; "
            "impl Owner for Value { type Assoc: Bound = Value; }",
        "trait Bound {} struct Value; "
            "trait Owner { type Assoc: Bound = Value; }",
        "trait Bound<T> {} trait Owner { type Assoc: Bound<'static>; }",
        "trait Bound<'a> {} trait Owner { "
            "type Assoc: Bound<u8>; }",
        "trait Bound<const N: usize> {} trait Owner { "
            "type Assoc: Bound<1>; }",
        "trait Bound<T> { type Item; } trait Owner { "
            "type Assoc: Bound<Item = u8, u16>; }",
        "trait Bound<'a, T> {} trait Owner { "
            "type Assoc: Bound<'static>; }",
        "trait Owner { type Assoc: Bound; } "
            "trait Bound<T = (Self, u8)> {}",
        "trait Base<Rhs = Self> {} trait Owner { "
            "type Assoc<T>: Base; }",
        "trait Base<Rhs = Self> {} trait Owner<'a> { "
            "type Assoc<'b>: 'a + Base; }",
        "trait Base<T = &u8> {} trait Owner { type Assoc: Base; }",
        "trait Bound {} trait Owner { type Assoc: Bound + Bound; }",
        "trait Bound { type Item; } trait Owner { "
            "type Assoc: Bound<Item = Self, Item = Self>; }",
        "trait Bound {} trait Owner { type Assoc: Bound<Item = Self>; }",
        "trait Bound { fn Item(); } trait Owner { "
            "type Assoc: Bound<Item = Self>; }",
        "trait Owner { type Assoc: Missing; }",
        "struct NotTrait; trait Owner { type Assoc: NotTrait; }",
        "mod support { pub struct Bound; } "
            "trait Owner { type Assoc: support::Bound; }",
        "struct Holder<T>; type Alias = Holder<Item = u8>;"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_WRONG_NAMESPACE,
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_WRONG_NAMESPACE,
        CM_HIR_LOWER_WRONG_NAMESPACE,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC
    };
    static const char *const rejected_messages[] = {
        "free type aliases",
        "impl associated types",
        "associated type defaults",
        "argument kind differs",
        "argument kind differs",
        "const trait arguments",
        "must precede associated equalities",
        "omits a required type argument",
        "authenticated associated-type subject",
        "GAT lifetime bounds",
        "GAT lifetime bounds",
        "unresolved, erased, or unauthenticated bound lifetime",
        "duplicate associated type bound",
        "duplicate associated type equality",
        "no associated type named by equality",
        "value namespace",
        "trait path is unresolved",
        "does not name a trait",
        "does not name a trait",
        "associated-type path arguments"
    };
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *owner;
    const CmHirItem *associated;
    const CmHirAssociatedTypeBound *bound;
    const CmHirType *argument_type;
    size_t index;

    assert(sizeof(rejected) / sizeof(rejected[0])
        == sizeof(rejected_kinds) / sizeof(rejected_kinds[0]));
    assert(sizeof(rejected) / sizeof(rejected[0])
        == sizeof(rejected_messages) / sizeof(rejected_messages[0]));

    result = lower_source(
        "trait Integer {} trait Into<T> {} "
        "trait RawFloat { type Int: Integer + Into<u64>; }",
        &context, NULL);
    owner = find_item(&context, "RawFloat");
    associated = owner == NULL ? NULL
        : find_child(&context, owner->definition, "Int");
    bound = associated == NULL
            || associated->data.type_alias_item.bound_count != 2u
        ? NULL : &associated->data.type_alias_item.bounds[1];
    argument_type = bound == NULL
            || bound->trait_type.argument_count != 1u
            || bound->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            bound->trait_type.arguments[0].data.type);
    assert(result.error_count == 0u
        && associated != NULL
        && bound != NULL
        && argument_type != NULL
        && argument_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && argument_type->data.integer_type.kind == CM_HIR_INT_U64
        && bound->equality_count == 0u);
    cm_hir_context_destroy(&context);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        if (result.error_count != 1u
            || result.first_error.kind != rejected_kinds[index]
            || strstr(result.first_error.message,
                rejected_messages[index]) == NULL) {
            fprintf(stderr,
                "associated-bound rejection mismatch for %s: "
                "count=%lu kind=%s message=%s\n",
                rejected[index], (unsigned long)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]
            && strstr(result.first_error.message,
                rejected_messages[index]) != NULL);
        cm_hir_context_destroy(&context);
    }
}

static void test_associated_type_bound_arguments(void)
{
    static const char source[] =
        "trait Bound<'a, T, U = ()> { type Item; } "
        "trait Owner<'b> { "
        "type Assoc: Bound<'b, u8, Item = u16>; }";
    static const char bound_source[] = "Bound<'b, u8, Item = u16>";
    static const char equality_source[] = "Item = u16";
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *bound_trait;
    const CmHirItem *bound_item;
    const CmHirItem *owner;
    const CmHirItem *associated;
    const CmHirGenericParam *owner_lifetime;
    const CmHirAssociatedTypeBound *bound;
    const CmHirType *explicit_type;
    const CmHirType *default_type;
    const CmHirType *equality_type;

    result = lower_source(source, &context, NULL);
    bound_trait = find_item(&context, "Bound");
    bound_item = bound_trait == NULL ? NULL
        : find_child(&context, bound_trait->definition, "Item");
    owner = find_item(&context, "Owner");
    associated = owner == NULL ? NULL
        : find_child(&context, owner->definition, "Assoc");
    owner_lifetime = owner == NULL
            || owner->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            owner->generic_parameter_start);
    bound = associated == NULL
            || associated->data.type_alias_item.bound_count != 1u
        ? NULL : &associated->data.type_alias_item.bounds[0];
    explicit_type = bound == NULL
            || bound->trait_type.argument_count != 3u
            || bound->trait_type.arguments[1].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            bound->trait_type.arguments[1].data.type);
    default_type = bound == NULL
            || bound->trait_type.argument_count != 3u
            || bound->trait_type.arguments[2].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            bound->trait_type.arguments[2].data.type);
    equality_type = bound == NULL || bound->equality_count != 1u
        ? NULL : cm_hir_get_type(&context, bound->equalities[0].value);
    assert(result.error_count == 0u
        && bound_trait != NULL && bound_item != NULL
        && owner != NULL && associated != NULL
        && owner_lifetime != NULL
        && owner_lifetime->kind == CM_HIR_GENERIC_LIFETIME
        && bound != NULL
        && cm_hir_def_id_equal(bound->trait_type.definition,
            bound_trait->definition)
        && bound->trait_type.argument_count == 3u
        && bound->trait_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && bound->trait_type.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && bound->trait_type.arguments[0].data.lifetime.data.parameter
            == owner->generic_parameter_start
        && explicit_type != NULL
        && explicit_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && explicit_type->data.integer_type.kind == CM_HIR_INT_U8
        && default_type != NULL
        && default_type->kind == CM_HIR_TYPE_UNIT_KIND
        && default_type->span.end - default_type->span.start == 2u
        && memcmp(source + default_type->span.start, "()", 2u) == 0
        && bound->equality_count == 1u
        && cm_hir_def_id_equal(bound->equalities[0].associated_type,
            bound_item->definition)
        && equality_type != NULL
        && equality_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && equality_type->data.integer_type.kind == CM_HIR_INT_U16
        && (size_t)(bound->span.end - bound->span.start)
            == sizeof(bound_source) - 1u
        && memcmp(source + bound->span.start, bound_source,
            sizeof(bound_source) - 1u) == 0
        && (size_t)(bound->equalities[0].span.end
            - bound->equalities[0].span.start)
            == sizeof(equality_source) - 1u
        && memcmp(source + bound->equalities[0].span.start,
            equality_source, sizeof(equality_source) - 1u) == 0);
    cm_hir_context_destroy(&context);
}

static void test_associated_type_lifetime_bounds(void)
{
    static const char source[] =
        "trait Copy<Rhs = Self> {} "
        "trait Owner<'a, T, const N: usize> { "
        "type Assoc: 'static + Copy + 'a where T: 'a; }";
    static const char *const rejected[] = {
        "trait Owner<'a> { type Assoc<'b>: 'a; }",
        "trait Owner { type Assoc: '_; }",
        "trait Owner { type Assoc: 'missing; }"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNRESOLVED_PATH
    };
    static const char *const rejected_messages[] = {
        "GAT lifetime bounds",
        "must be 'static or an authenticated enclosing lifetime",
        "undeclared lifetime"
    };
    CmHirContext context;
    CmHirLowerResult result;
    const CmHirItem *copy_trait;
    const CmHirItem *owner;
    const CmHirItem *associated;
    const CmHirAssociatedTypeBound *trait_bound;
    const CmHirType *default_bound_self;
    const CmHirOutlivesPredicate *static_bound;
    const CmHirOutlivesPredicate *early_bound;
    const CmHirOutlivesPredicate *where_bound;
    const CmHirType *projection;
    const CmHirType *self_type;
    const CmHirType *type_argument;
    const CmHirType *where_subject;
    const CmHirGenericParam *const_parameter;
    size_t index;

    result = lower_source(source, &context, NULL);
    copy_trait = find_item(&context, "Copy");
    owner = find_item(&context, "Owner");
    associated = owner == NULL ? NULL
        : find_child(&context, owner->definition, "Assoc");
    trait_bound = associated == NULL
            || associated->data.type_alias_item.bound_count != 1u
        ? NULL : &associated->data.type_alias_item.bounds[0];
    default_bound_self = trait_bound == NULL
            || trait_bound->trait_type.argument_count != 1u
            || trait_bound->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            trait_bound->trait_type.arguments[0].data.type);
    static_bound = associated == NULL
            || associated->outlives_predicate_count != 3u
        ? NULL : &associated->outlives_predicates[0];
    early_bound = associated == NULL
            || associated->outlives_predicate_count != 3u
        ? NULL : &associated->outlives_predicates[1];
    where_bound = associated == NULL
            || associated->outlives_predicate_count != 3u
        ? NULL : &associated->outlives_predicates[2];
    projection = static_bound == NULL ? NULL
        : cm_hir_get_type(&context, static_bound->subject.type);
    self_type = projection == NULL
            || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
        ? NULL : cm_hir_get_type(&context,
            projection->data.projection_type.self_type);
    type_argument = projection == NULL
            || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
            || projection->data.projection_type.trait_type.argument_count
                != 3u
            || projection->data.projection_type.trait_type.arguments[1].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&context,
            projection->data.projection_type.trait_type.arguments[1]
                .data.type);
    where_subject = where_bound == NULL ? NULL
        : cm_hir_get_type(&context, where_bound->subject.type);
    const_parameter = owner == NULL
            || owner->generic_parameter_count != 3u
        ? NULL : cm_hir_get_generic_param(&context,
            owner->generic_parameter_start + 2u);
    if (result.error_count != 0u) {
        fprintf(stderr, "associated lifetime lowering failed: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    assert(result.error_count == 0u
        && copy_trait != NULL && owner != NULL && associated != NULL
        && owner->generic_parameter_count == 3u
        && trait_bound != NULL
        && cm_hir_def_id_equal(trait_bound->trait_type.definition,
            copy_trait->definition)
        && trait_bound->trait_type.argument_count == 1u
        && static_bound != NULL && early_bound != NULL
        && trait_bound->trait_type.arguments[0].data.type
            == static_bound->subject.type
        && default_bound_self == projection
        && where_bound != NULL
        && static_bound->subject_kind == CM_HIR_OUTLIVES_TYPE
        && static_bound->bound.kind == CM_HIR_REGION_STATIC
        && early_bound->subject_kind == CM_HIR_OUTLIVES_TYPE
        && early_bound->subject.type == static_bound->subject.type
        && early_bound->bound.kind == CM_HIR_REGION_EARLY_BOUND
        && early_bound->bound.data.parameter
            == owner->generic_parameter_start
        && where_bound->subject_kind == CM_HIR_OUTLIVES_TYPE
        && where_bound->bound.kind == CM_HIR_REGION_EARLY_BOUND
        && where_bound->bound.data.parameter
            == owner->generic_parameter_start
        && projection != NULL
        && projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(projection->data.projection_type
                .trait_type.definition,
            owner->definition)
        && cm_hir_def_id_equal(projection->data.projection_type
                .associated_type.definition,
            associated->definition)
        && projection->data.projection_type.associated_type.argument_count
            == 0u
        && projection->data.projection_type.trait_type.argument_count == 3u
        && projection->data.projection_type.trait_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && projection->data.projection_type.trait_type.arguments[0]
                .data.lifetime.kind == CM_HIR_REGION_EARLY_BOUND
        && projection->data.projection_type.trait_type.arguments[0]
                .data.lifetime.data.parameter
            == owner->generic_parameter_start
        && type_argument != NULL
        && type_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type_argument->data.parameter_type.parameter
            == owner->generic_parameter_start + 1u
        && projection->data.projection_type.trait_type.arguments[2].kind
            == CM_HIR_GENERIC_ARG_CONST
        && projection->data.projection_type.trait_type.arguments[2]
                .data.constant.kind == CM_HIR_CONST_PARAMETER
        && projection->data.projection_type.trait_type.arguments[2]
                .data.constant.data.parameter
            == owner->generic_parameter_start + 2u
        && const_parameter != NULL
        && projection->data.projection_type.trait_type.arguments[2]
                .data.constant.type == const_parameter->declared_type
        && self_type != NULL && self_type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(self_type->data.self_type.owner,
            owner->definition)
        && where_subject != NULL
        && where_subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && where_subject->data.parameter_type.parameter
            == owner->generic_parameter_start + 1u
        && memcmp(source + static_bound->span.start, "'static", 7u) == 0
        && static_bound->span.end - static_bound->span.start == 7u
        && memcmp(source + early_bound->span.start, "'a", 2u) == 0
        && early_bound->span.end - early_bound->span.start == 2u
        && memcmp(source + where_bound->span.start, "T: 'a", 5u) == 0
        && where_bound->span.end - where_bound->span.start == 5u
        && static_bound->span.start < early_bound->span.start
        && early_bound->span.start < where_bound->span.start);
    cm_hir_context_destroy(&context);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        result = lower_source(rejected[index], &context, NULL);
        assert(result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]
            && strstr(result.first_error.message,
                rejected_messages[index]) != NULL);
        cm_hir_context_destroy(&context);
    }
}

static void test_relaxed_sized_generic_parameter(void)
{
    static const char source[] =
        "fn inspect<T: ?Sized>(value: &T) {}";
    const CmHirItem *function;
    const CmHirGenericParam *parameter;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    function = find_item(&context, "inspect");
    parameter = function == NULL
            || function->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            function->generic_parameter_start);
    assert(result.error_count == 0u
        && function != NULL && function->kind == CM_HIR_ITEM_FUNCTION
        && function->predicate_count == 0u
        && function->predicates == NULL
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->is_relaxed_sized);
    cm_hir_context_destroy(&context);
}

static void test_conditionally_const_generic_parameter_bound(void)
{
    static const char source[] =
        "trait Bound {} trait Owner {"
        " fn inspect<T: ~const Bound>(value: &T); }";
    const CmHirItem *bound;
    const CmHirItem *owner;
    const CmHirItem *function;
    const CmHirGenericParam *parameter;
    CmHirContext context;
    CmHirLowerResult result;

    result = lower_source(source, &context, NULL);
    bound = find_item(&context, "Bound");
    owner = find_item(&context, "Owner");
    function = owner == NULL ? NULL
        : find_child(&context, owner->definition, "inspect");
    parameter = function == NULL
            || function->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&context,
            function->generic_parameter_start);
    assert(result.error_count == 0u
        && bound != NULL && bound->kind == CM_HIR_ITEM_TRAIT
        && function != NULL && function->kind == CM_HIR_ITEM_FUNCTION
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && !parameter->is_relaxed_sized
        && function->predicate_count == 1u
        && function->predicates != NULL
        && function->predicates[0].modifier
            == CM_HIR_PREDICATE_CONST_IF_CONST
        && cm_hir_def_id_equal(
            function->predicates[0].trait_type.definition,
            bound->definition));
    cm_hir_context_destroy(&context);
}

int main(void)
{
    test_argument_impl_trait_method_parity();
    test_complete_declarations();
    test_union_declarations();
    test_enum_variant_attributes_fail_closed();
    test_default_specialization_fails_closed();
    test_trait_alias_lowering();
    test_auto_trait_and_negative_impl_lowering();
    test_discard_parameter_entry_points();
    test_supertrait_entry_points();
    test_unresolved_path_is_hard_error();
    test_resolver_failure_is_distinct();
    test_unsupported_constructs_are_errors();
    test_const_generic_path_default();
    test_adt_default_declaration_order_entry_points();
    test_const_generic_trait_method_declaration();
    test_generic_parameter_shadows_type_path_prefix();
    test_defaulted_alias_entry_points();
    test_explicit_projection_entry_points();
    test_cross_trait_projection_default_entry_points();
    test_monomorphic_trait_impl_entry_points();
    test_generic_associated_type_entry_points();
    test_same_trait_generic_self_projection();
    test_transitive_generic_self_projection_record_rollback();
    test_ordered_nominal_generic_impl_entry_points();
    test_method_bearing_trait_impl_entry_points();
    test_lifetime_qualified_receiver();
    test_method_completeness_and_identity_errors();
    test_trait_method_self_sized_predicate();
    test_trait_method_const_predicates();
    test_trait_method_predicate_boundaries();
    test_callable_tuple_provenance();
    test_parenthesized_and_singleton_tuple_types();
    test_trait_method_predicate_storage_mismatch();
    test_post_value_where_predicate_storage_mismatch();
    test_associated_type_constraint_fails_closed();
    test_lifetime_where_predicates_retained();
    test_lifetime_generic_parameter_bounds();
    test_generic_parameter_attributes_fail_closed();
    test_lifetime_trait_bounds_fail_closed();
    test_associated_lifetime_bound_record_rollback();
    test_argument_impl_trait_lowers();
    test_nested_argument_impl_trait_lowers();
    /* Trait/impl APIT parity remains a separate semantic admission gate. */
    test_argument_impl_trait_foreign_rejected();
    test_higher_ranked_impl_trait_fails_closed();
    test_higher_ranked_where_bound_lowers();
    test_higher_ranked_where_predicate_lowers();
    test_higher_ranked_trait_lifetime_argument_lowers();
    test_unsupported_method_forms_are_errors();
    test_cfg_sensitive_trait_impl_members();
    test_cfg_active_tree_drives_lowering();
    test_effective_attribute_is_not_discarded();
    test_inactive_and_invalid_views_are_rejected();
    test_associated_type_bound_arguments();
    test_associated_type_lifetime_bounds();
    test_associated_type_bound_scope_errors();
    test_relaxed_sized_generic_parameter();
    test_conditionally_const_generic_parameter_bound();
    return 0;
}
