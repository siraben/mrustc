#include "cm/syntax/ast.h"
#include "cm/syntax/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ast_string_is(const CmAst *ast, CmInternId id,
    const char *expected)
{
    const CmInternedString *string;
    size_t length;

    string = cm_ast_get_string(ast, id);
    length = strlen(expected);
    return string != NULL && string->len == length
        && memcmp(string->bytes, expected, length) == 0;
}

static int ast_span_is(const char *source, CmAstSpan span,
    const char *expected)
{
    size_t length;

    length = strlen(expected);
    return span.end >= span.start
        && (size_t)(span.end - span.start) == length
        && memcmp(source + span.start, expected, length) == 0;
}

static int ast_path_segments_are(const CmAst *ast, CmAstTypeId type_id,
    const char *const *expected, uint32_t expected_count)
{
    const CmAstType *type;
    const CmAstPath *path;
    uint32_t index;

    type = cm_ast_get_type(ast, type_id);
    path = type == NULL ? NULL : cm_ast_get_path(ast, type->path);
    if (type == NULL || type->kind != CM_AST_TYPE_PATH || path == NULL
        || path->segment_count != expected_count) {
        return 0;
    }
    for (index = 0u; index < expected_count; ++index) {
        if (!ast_string_is(ast, path->segments[index].name,
                expected[index])) {
            return 0;
        }
    }
    return 1;
}

static int ast_expression_path_is(const CmAst *ast, CmAstExprId expression_id,
    const char *expected)
{
    const CmAstExpr *expression;
    const CmAstPath *path;

    expression = cm_ast_get_expr(ast, expression_id);
    path = expression == NULL || expression->kind != CM_AST_EXPR_PATH
        ? NULL : cm_ast_get_path(ast, expression->data.path.path);
    return path != NULL && path->segment_count == 1u
        && path->segments != NULL
        && ast_string_is(ast, path->segments[0].name, expected);
}

static unsigned char *read_file(const char *path, size_t *length_out)
{
    FILE *stream;
    long end;
    unsigned char *bytes;
    size_t length;

    stream = fopen(path, "rb");
    if (stream == NULL) {
        return NULL;
    }
    if (fseek(stream, 0L, SEEK_END) != 0) {
        fclose(stream);
        return NULL;
    }
    end = ftell(stream);
    if (end < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        fclose(stream);
        return NULL;
    }
    length = (size_t)end;
    bytes = (unsigned char *)malloc(length + 1u);
    if (bytes == NULL) {
        fclose(stream);
        return NULL;
    }
    if (fread(bytes, 1u, length, stream) != length) {
        free(bytes);
        fclose(stream);
        return NULL;
    }
    bytes[length] = 0u;
    fclose(stream);
    *length_out = length;
    return bytes;
}

static int compare_dump(FILE *dump, const char *expected_path)
{
    unsigned char *actual;
    unsigned char *expected;
    long end;
    size_t actual_length;
    size_t expected_length;
    int matches;

    if (fflush(dump) != 0 || fseek(dump, 0L, SEEK_END) != 0) {
        return 0;
    }
    end = ftell(dump);
    if (end < 0L || fseek(dump, 0L, SEEK_SET) != 0) {
        return 0;
    }
    actual_length = (size_t)end;
    actual = (unsigned char *)malloc(actual_length + 1u);
    if (actual == NULL) {
        return 0;
    }
    if (fread(actual, 1u, actual_length, dump) != actual_length) {
        free(actual);
        return 0;
    }
    actual[actual_length] = 0u;
    expected = read_file(expected_path, &expected_length);
    if (expected == NULL) {
        free(actual);
        return 0;
    }
    matches = actual_length == expected_length &&
        memcmp(actual, expected, actual_length) == 0;
    if (!matches) {
        fputs("canonical AST mismatch; actual dump follows:\n", stderr);
        fwrite(actual, 1u, actual_length, stderr);
    }
    free(expected);
    free(actual);
    return matches;
}

static int ast_dump_contains(const CmAst *ast, const char *needle)
{
    FILE *stream;
    char bytes[8192];
    size_t length;
    int found;

    stream = tmpfile();
    if (stream == NULL) return 0;
    found = 0;
    if (cm_ast_dump(stream, ast) && fflush(stream) == 0
        && fseek(stream, 0L, SEEK_SET) == 0) {
        length = fread(bytes, 1u, sizeof(bytes) - 1u, stream);
        bytes[length] = 0;
        found = strstr(bytes, needle) != NULL;
    }
    (void)fclose(stream);
    return found;
}

static int test_error_path(void)
{
    static const char broken[] = "fn broken(value: [u8; 4)";
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, broken, sizeof(broken) - 1u,
        CM_EDITION_2024);
    ok = result.error_count != 0u && result.first_error.line == 1u &&
        result.first_error.column != 0u && result.first_error.message[0] != 0;
    cm_ast_destroy(&ast);
    if (!ok) {
        fputs("invalid syntax did not produce a located error\n", stderr);
    }
    return ok;
}

static int test_macro_error_paths(void)
{
    static const char *const broken[] = {
        "broken!([)]);",
        "fn broken() { let value = broken!({]); }",
        "unterminated!({ nested: [1, 2] }",
        "pub macro {}",
        "pub macro named($token:tt) [not_a_body]",
        "pub macro unterminated(($token:tt) { body }"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u || result.first_error.message[0] == 0) {
            fprintf(stderr,
                "invalid macro token tree %lu did not produce an error\n",
                (unsigned long)index);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_macro_lifetime_signature_capture(void)
{
    static const char source[] =
        "macro_rules! define_bignum { ($name:ident) => { "
        "pub fn add<'a>(&'a mut self, other: &$name) -> &'a mut $name {} "
        "}; }";
    static const char arguments[] =
        "($name:ident) => { pub fn add<'a>(&'a mut self, other: &$name) "
        "-> &'a mut $name {} };";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *item;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    ok = result.error_count == 0u && ast.root_items.len == 1u
        && item != NULL && item->kind == CM_AST_ITEM_MACRO
        && item->data.macro_item.form == CM_AST_MACRO_RULES_DEFINITION
        && item->data.macro_item.delimiter == CM_AST_DELIMITER_BRACE
        && ast_string_is(&ast, item->name, "define_bignum")
        && ast_string_is(&ast, item->data.macro_item.arguments, arguments);
    if (!ok) {
        fprintf(stderr, "bignum macro signature capture was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_generic_default_error_paths(void)
{
    static const char *const broken[] = {
        "type A<'a = 'static> = &'a u8;",
        "type A<T: = u8> = T;",
        "type A<T = > = T;",
        "type A<T = u8 + 1> = T;",
        "type A<T = u8 ignored> = T;",
        "type A<T: Fn([)] = u8> = T;",
        "type A<T: Fn(>) = u8> = T;",
        "type A<T: Fn(>>) = u8> = T;",
        "struct MissingConstType<const N = 4>;",
        "struct EmptyConstDefault<const N: usize = >;",
        "fn invalid<T = u8>() {}",
        "fn invalid_const<const N: usize = 4>() {}",
        "impl<T = u8> Wrapper<T> {}",
        "impl<const N: usize = 4> Wrapper<N> {}"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u || result.first_error.message[0] == 0) {
            fprintf(stderr,
                "invalid generic default %lu did not produce an error\n",
                (unsigned long)index);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_structured_generic_parameter_bounds(void)
{
    char source[] =
        "type Tranche<T: alpha::Trait<A, Item = B> + Send, "
        "F: FnMut(A, B) -> Result<C, E>, U = u8, "
        "const N: usize = 4> where "
        "F: FnMut(A) -> Result<C, E> = T;";
    static const char *const first_path[] = { "alpha", "Trait" };
    static const char *const send_path[] = { "Send" };
    static const char *const a_path[] = { "A" };
    static const char *const b_path[] = { "B" };
    static const char *const usize_path[] = { "usize" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstGenericParam *parameter;
    const CmAstType *trait_type;
    const CmAstPath *trait_path;
    const CmAstPathSegment *segment;
    const CmAstGenericArg *argument;
    const CmAstWherePredicate *where_predicate;
    const CmAstType *tuple_type;
    const CmAstType *return_type;
    const CmAstPath *return_path;
    const CmAstExpr *default_const;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, strlen(source), CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    ok = result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TYPE_ALIAS
        && item->generic_parameter_count == 4u
        && item->generic_parameters != NULL;

    parameter = ok ? &item->generic_parameters[0] : NULL;
    trait_type = parameter == NULL || parameter->bound_count != 2u
        || parameter->bounds == NULL ? NULL
        : cm_ast_get_type(&ast, parameter->bounds[0].trait_type);
    trait_path = trait_type == NULL ? NULL
        : cm_ast_get_path(&ast, trait_type->path);
    segment = trait_path == NULL || trait_path->segment_count != 2u
        || trait_path->segments == NULL ? NULL
        : &trait_path->segments[1];
    argument = segment == NULL || segment->argument_count != 2u
        || segment->arguments == NULL ? NULL : segment->arguments;
    ok = ok && parameter != NULL
        && parameter->kind == CM_AST_PARAM_TYPE
        && parameter->bounds[0].kind == CM_AST_GENERIC_BOUND_TRAIT
        && parameter->bounds[1].kind == CM_AST_GENERIC_BOUND_TRAIT
        && ast_string_is(&ast, parameter->name, "T")
        && ast_string_is(&ast, parameter->declaration,
            "T: alpha::Trait<A, Item = B> + Send")
        && ast_string_is(&ast, parameter->constraint,
            "alpha::Trait<A, Item = B> + Send")
        && ast_span_is(source, parameter->bounds[0].span,
            "alpha::Trait<A, Item = B>")
        && ast_span_is(source, parameter->bounds[1].span, "Send")
        && ast_path_segments_are(&ast, parameter->bounds[0].trait_type,
            first_path, 2u)
        && ast_path_segments_are(&ast, parameter->bounds[1].trait_type,
            send_path, 1u)
        && argument != NULL
        && argument[0].kind == CM_AST_GENERIC_TYPE
        && ast_span_is(source, argument[0].span, "A")
        && ast_path_segments_are(&ast, argument[0].type,
            a_path, 1u)
        && argument[1].kind == CM_AST_GENERIC_BINDING
        && ast_string_is(&ast, argument[1].name, "Item")
        && ast_span_is(source, argument[1].span, "Item = B")
        && ast_path_segments_are(&ast, argument[1].type,
            b_path, 1u);

    parameter = ok ? &item->generic_parameters[1] : NULL;
    trait_type = parameter == NULL || parameter->bound_count != 1u
        || parameter->bounds == NULL ? NULL
        : cm_ast_get_type(&ast, parameter->bounds[0].trait_type);
    trait_path = trait_type == NULL ? NULL
        : cm_ast_get_path(&ast, trait_type->path);
    segment = trait_path == NULL || trait_path->segment_count != 1u
        || trait_path->segments == NULL ? NULL
        : &trait_path->segments[0];
    argument = segment == NULL || segment->argument_count != 2u
        || segment->arguments == NULL ? NULL : segment->arguments;
    tuple_type = argument == NULL ? NULL
        : cm_ast_get_type(&ast, argument[0].type);
    return_type = argument == NULL ? NULL
        : cm_ast_get_type(&ast, argument[1].type);
    return_path = return_type == NULL ? NULL
        : cm_ast_get_path(&ast, return_type->path);
    ok = ok && parameter != NULL
        && ast_string_is(&ast, parameter->constraint,
            "FnMut(A, B) -> Result<C, E>")
        && ast_span_is(source, parameter->bounds[0].span,
            "FnMut(A, B) -> Result<C, E>")
        && trait_type != NULL
        && ast_span_is(source, trait_type->span,
            "FnMut(A, B) -> Result<C, E>")
        && trait_path != NULL
        && ast_span_is(source, trait_path->span,
            "FnMut(A, B) -> Result<C, E>")
        && ast_string_is(&ast, segment->name, "FnMut")
        && argument != NULL
        && argument[0].kind == CM_AST_GENERIC_TYPE
        && ast_span_is(source, argument[0].span, "(A, B)")
        && tuple_type != NULL && tuple_type->kind == CM_AST_TYPE_TUPLE
        && tuple_type->tuple_provenance
            == CM_AST_TUPLE_CALLABLE_INPUTS
        && ast_span_is(source, tuple_type->span, "(A, B)")
        && tuple_type->element_count == 2u
        && tuple_type->elements != NULL
        && ast_path_segments_are(&ast, tuple_type->elements[0],
            a_path, 1u)
        && ast_path_segments_are(&ast, tuple_type->elements[1],
            b_path, 1u)
        && argument[1].kind == CM_AST_GENERIC_BINDING
        && ast_string_is(&ast, argument[1].name, "Output")
        && ast_span_is(source, argument[1].span, "-> Result<C, E>")
        && return_type != NULL && return_type->kind == CM_AST_TYPE_PATH
        && return_path != NULL && return_path->segment_count == 1u
        && ast_string_is(&ast, return_path->segments[0].name, "Result")
        && return_path->segments[0].argument_count == 2u;

    parameter = ok ? &item->generic_parameters[2] : NULL;
    ok = ok && parameter != NULL && parameter->bound_count == 0u
        && parameter->bounds == NULL
        && parameter->constraint == CM_INTERN_ID_NONE
        && parameter->default_type != CM_AST_TYPE_NONE;
    parameter = ok ? &item->generic_parameters[3] : NULL;
    default_const = parameter == NULL ? NULL
        : cm_ast_get_expr(&ast, parameter->default_const_expr);
    ok = ok && parameter != NULL && parameter->kind == CM_AST_PARAM_CONST
        && parameter->bound_count == 0u && parameter->bounds == NULL
        && ast_string_is(&ast, parameter->constraint, "usize")
        && ast_path_segments_are(&ast, parameter->declared_type,
            usize_path, 1u)
        && ast_string_is(&ast, parameter->default_const, "4")
        && default_const != NULL
        && default_const->kind == CM_AST_EXPR_LITERAL
        && ast_string_is(&ast, default_const->data.literal.text, "4")
        && ast_string_is(&ast, parameter->declaration,
            "const N: usize = 4");

    where_predicate = ok && item->where_predicate_count == 1u
            && item->where_predicates != NULL
        ? &item->where_predicates[0] : NULL;
    trait_type = where_predicate == NULL
            || where_predicate->bound_count != 1u
            || where_predicate->bounds == NULL
        ? NULL : cm_ast_get_type(&ast,
            where_predicate->bounds[0].trait_type);
    trait_path = trait_type == NULL ? NULL
        : cm_ast_get_path(&ast, trait_type->path);
    segment = trait_path == NULL || trait_path->segment_count != 1u
        || trait_path->segments == NULL ? NULL
        : &trait_path->segments[0];
    argument = segment == NULL || segment->argument_count != 2u
        || segment->arguments == NULL ? NULL : segment->arguments;
    tuple_type = argument == NULL ? NULL
        : cm_ast_get_type(&ast, argument[0].type);
    ok = ok && where_predicate != NULL
        && ast_string_is(&ast, item->where_clause,
            "F: FnMut(A) -> Result<C, E>")
        && ast_span_is(source, where_predicate->span,
            "F: FnMut(A) -> Result<C, E>")
        && ast_span_is(source, where_predicate->bounds[0].span,
            "FnMut(A) -> Result<C, E>")
        && argument != NULL
        && argument[0].kind == CM_AST_GENERIC_TYPE
        && tuple_type != NULL && tuple_type->kind == CM_AST_TYPE_TUPLE
        && tuple_type->tuple_provenance
            == CM_AST_TUPLE_CALLABLE_INPUTS
        && tuple_type->element_count == 1u
        && argument[1].kind == CM_AST_GENERIC_BINDING
        && ast_string_is(&ast, argument[1].name, "Output")
        && ast_span_is(source, argument[1].span, "-> Result<C, E>");

    if (ok) {
        memset(source, '?', sizeof(source) - 1u);
        ok = ast_string_is(&ast, item->generic_parameters[0].constraint,
                "alpha::Trait<A, Item = B> + Send")
            && ast_string_is(&ast, segment->name, "FnMut")
            && ast_string_is(&ast, argument[1].name, "Output");
    }
    if (!ok) {
        fprintf(stderr,
            "structured generic parameter bounds were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_generic_parameter_attributes(void)
{
    static const char *const broken[] = {
        "type Inner<#![not_outer] T> = T;",
        "type Unclosed<#[attribute T> = T;"
    };
    char source[] =
        "type Marked<#[may_dangle] T, #[first] #[second(value)] U> = "
        "(T, U);";
    static const char *const declarations[] = {
        "#[may_dangle] T", "#[first] #[second(value)] U"
    };
    static const char *const attribute_text[] = {
        "#[may_dangle]", "#[first]", "#[second(value)]"
    };
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstGenericParam *first;
    const CmAstGenericParam *second;
    const CmAstAttribute *attribute;
    CmAst ast;
    CmParseResult result;
    size_t index;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    first = item == NULL || item->generic_parameter_count != 2u
            || item->generic_parameters == NULL
        ? NULL : &item->generic_parameters[0];
    second = first == NULL ? NULL : &item->generic_parameters[1];
    ok = result.error_count == 0u && first != NULL && second != NULL
        && first->attribute_count == 1u && first->attributes != NULL
        && second->attribute_count == 2u && second->attributes != NULL
        && ast_string_is(&ast, first->declaration, declarations[0])
        && ast_string_is(&ast, second->declaration, declarations[1]);
    attribute = !ok ? NULL
        : cm_ast_get_attribute(&ast, first->attributes[0]);
    ok = ok && attribute != NULL
        && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, attribute_text[0])
        && ast_span_is(source, attribute->span, attribute_text[0]);
    attribute = !ok ? NULL
        : cm_ast_get_attribute(&ast, second->attributes[0]);
    ok = ok && attribute != NULL
        && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, attribute_text[1])
        && ast_span_is(source, attribute->span, attribute_text[1]);
    attribute = !ok ? NULL
        : cm_ast_get_attribute(&ast, second->attributes[1]);
    ok = ok && attribute != NULL
        && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, attribute_text[2])
        && ast_span_is(source, attribute->span, attribute_text[2])
        && ast_dump_contains(&ast, "(attribute outer \"#[may_dangle]\")");
    if (ok) {
        memset(source, '?', sizeof(source) - 1u);
        ok = ast_string_is(&ast, first->declaration, declarations[0])
            && ast_string_is(&ast, second->declaration, declarations[1])
            && ast_string_is(&ast, attribute->text, attribute_text[2]);
    }
    if (!ok) {
        fprintf(stderr, "generic parameter attributes were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u || result.first_error.message[0] == 0) {
            fprintf(stderr,
                "invalid generic parameter attribute %lu was accepted\n",
                (unsigned long)index);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_nested_associated_type_constraints(void)
{
    static const char source[] =
        "type ChangeOutputType<T: "
        "Try<Residual: Residual<V> + Copy>, V> = T;";
    static const char *const residual_path[] = { "Residual" };
    static const char *const copy_path[] = { "Copy" };
    static const char *const v_path[] = { "V" };
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstGenericParam *parameter;
    const CmAstType *try_type;
    const CmAstPath *try_path;
    const CmAstPathSegment *try_segment;
    const CmAstGenericArg *constraint;
    const CmAstType *residual_type;
    const CmAstPath *residual;
    const CmAstGenericArg *v_argument;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    parameter = item == NULL || item->generic_parameter_count != 2u
            || item->generic_parameters == NULL
        ? NULL : &item->generic_parameters[0];
    try_type = parameter == NULL || parameter->bound_count != 1u
            || parameter->bounds == NULL
        ? NULL : cm_ast_get_type(&ast, parameter->bounds[0].trait_type);
    try_path = try_type == NULL || try_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, try_type->path);
    try_segment = try_path == NULL || try_path->segment_count != 1u
            || try_path->segments == NULL
        ? NULL : &try_path->segments[0];
    constraint = try_segment == NULL || try_segment->argument_count != 1u
            || try_segment->arguments == NULL
        ? NULL : &try_segment->arguments[0];
    residual_type = constraint == NULL || constraint->bound_count != 2u
            || constraint->bounds == NULL
        ? NULL : cm_ast_get_type(&ast, constraint->bounds[0].trait_type);
    residual = residual_type == NULL
            || residual_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, residual_type->path);
    v_argument = residual == NULL || residual->segment_count != 1u
            || residual->segments == NULL
            || residual->segments[0].argument_count != 1u
            || residual->segments[0].arguments == NULL
        ? NULL : &residual->segments[0].arguments[0];
    ok = result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TYPE_ALIAS
        && constraint != NULL
        && constraint->kind == CM_AST_GENERIC_CONSTRAINT
        && ast_string_is(&ast, constraint->name, "Residual")
        && ast_string_is(&ast, constraint->text, "Residual<V> + Copy")
        && ast_span_is(source, constraint->span,
            "Residual: Residual<V> + Copy")
        && constraint->bounds[0].kind == CM_AST_GENERIC_BOUND_TRAIT
        && constraint->bounds[0].modifier == CM_AST_GENERIC_BOUND_REQUIRED
        && ast_path_segments_are(&ast,
            constraint->bounds[0].trait_type, residual_path, 1u)
        && constraint->bounds[1].kind == CM_AST_GENERIC_BOUND_TRAIT
        && ast_path_segments_are(&ast,
            constraint->bounds[1].trait_type, copy_path, 1u)
        && v_argument != NULL && v_argument->kind == CM_AST_GENERIC_TYPE
        && ast_path_segments_are(&ast, v_argument->type, v_path, 1u)
        && ast_dump_contains(&ast, "constraint \"Residual\":");
    if (!ok) {
        fprintf(stderr, "nested associated-type constraint was incorrect: "
            "%s\n", result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_generic_associated_type_constraint_names(void)
{
    static const char source[] =
        "type Search<'a, P: Pattern<Searcher<'a>: "
        "DoubleEndedSearcher<'a>>> = P; "
        "type Positional<'a, P: Pattern<Searcher<'a>>> = P;";
    static const char *const pattern_path[] = { "Pattern" };
    static const char *const double_ended_path[] = {
        "DoubleEndedSearcher"
    };
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstGenericParam *parameter;
    const CmAstType *pattern_type;
    const CmAstPath *pattern;
    const CmAstGenericArg *constraint;
    const CmAstType *double_ended_type;
    const CmAstPath *double_ended;
    const CmAstGenericArg *bound_lifetime;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    parameter = item == NULL || item->generic_parameter_count != 2u
            || item->generic_parameters == NULL
        ? NULL : &item->generic_parameters[1];
    pattern_type = parameter == NULL || parameter->bound_count != 1u
            || parameter->bounds == NULL
        ? NULL : cm_ast_get_type(&ast, parameter->bounds[0].trait_type);
    pattern = pattern_type == NULL
            || pattern_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, pattern_type->path);
    constraint = pattern == NULL || pattern->segment_count != 1u
            || pattern->segments == NULL
            || pattern->segments[0].argument_count != 1u
            || pattern->segments[0].arguments == NULL
        ? NULL : &pattern->segments[0].arguments[0];
    double_ended_type = constraint == NULL || constraint->bound_count != 1u
            || constraint->bounds == NULL
        ? NULL : cm_ast_get_type(&ast, constraint->bounds[0].trait_type);
    double_ended = double_ended_type == NULL
            || double_ended_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, double_ended_type->path);
    bound_lifetime = double_ended == NULL
            || double_ended->segment_count != 1u
            || double_ended->segments == NULL
            || double_ended->segments[0].argument_count != 1u
            || double_ended->segments[0].arguments == NULL
        ? NULL : &double_ended->segments[0].arguments[0];
    ok = result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TYPE_ALIAS
        && pattern != NULL
        && ast_path_segments_are(&ast, parameter->bounds[0].trait_type,
            pattern_path, 1u)
        && constraint != NULL
        && constraint->kind == CM_AST_GENERIC_CONSTRAINT
        && ast_string_is(&ast, constraint->name, "Searcher")
        && ast_string_is(&ast, constraint->text,
            "DoubleEndedSearcher<'a>")
        && ast_span_is(source, constraint->span,
            "Searcher<'a>: DoubleEndedSearcher<'a>")
        && constraint->name_argument_count == 1u
        && constraint->name_arguments != NULL
        && constraint->name_arguments[0].kind
            == CM_AST_GENERIC_LIFETIME
        && ast_string_is(&ast, constraint->name_arguments[0].text, "'a")
        && ast_span_is(source, constraint->name_arguments[0].span, "'a")
        && constraint->bounds[0].kind == CM_AST_GENERIC_BOUND_TRAIT
        && ast_path_segments_are(&ast,
            constraint->bounds[0].trait_type, double_ended_path, 1u)
        && bound_lifetime != NULL
        && bound_lifetime->kind == CM_AST_GENERIC_LIFETIME
        && ast_string_is(&ast, bound_lifetime->text, "'a")
        && ast_dump_contains(&ast,
            "constraint \"Searcher\"<lifetime=\"'a\">:");
    if (!ok) {
        fprintf(stderr, "generic associated-type constraint name was "
            "incorrect: %s\n", result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_lifetime_generic_parameter_bounds(void)
{
    static const char source[] =
        "type A<'a: 'b + 'static, 'b, T: ?Sized + 'a + Send> = &'a T;";
    static const char *const sized_path[] = { "Sized" };
    static const char *const send_path[] = { "Send" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstGenericParam *first;
    const CmAstGenericParam *third;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    first = item == NULL || item->generic_parameter_count != 3u
            || item->generic_parameters == NULL
        ? NULL : &item->generic_parameters[0];
    third = first == NULL ? NULL : &item->generic_parameters[2];
    ok = result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TYPE_ALIAS
        && first != NULL && first->kind == CM_AST_PARAM_LIFETIME
        && ast_string_is(&ast, first->name, "'a")
        && ast_string_is(&ast, first->constraint, "'b + 'static")
        && ast_string_is(&ast, first->declaration, "'a: 'b + 'static")
        && first->bound_count == 2u && first->bounds != NULL
        && first->bounds[0].kind == CM_AST_GENERIC_BOUND_LIFETIME
        && first->bounds[1].kind == CM_AST_GENERIC_BOUND_LIFETIME
        && first->bounds[0].modifier == CM_AST_GENERIC_BOUND_REQUIRED
        && first->bounds[1].modifier == CM_AST_GENERIC_BOUND_REQUIRED
        && first->bounds[0].trait_type == CM_AST_TYPE_NONE
        && first->bounds[1].trait_type == CM_AST_TYPE_NONE
        && ast_string_is(&ast, first->bounds[0].lifetime, "'b")
        && ast_string_is(&ast, first->bounds[1].lifetime, "'static")
        && ast_span_is(source, first->bounds[0].span, "'b")
        && ast_span_is(source, first->bounds[1].span, "'static")
        && item->generic_parameters[1].kind == CM_AST_PARAM_LIFETIME
        && item->generic_parameters[1].bound_count == 0u
        && item->generic_parameters[1].bounds == NULL
        && third != NULL && third->kind == CM_AST_PARAM_TYPE
        && ast_string_is(&ast, third->constraint, "?Sized + 'a + Send")
        && third->bound_count == 3u && third->bounds != NULL
        && third->bounds[0].kind == CM_AST_GENERIC_BOUND_TRAIT
        && third->bounds[0].modifier == CM_AST_GENERIC_BOUND_RELAXED
        && third->bounds[0].lifetime == CM_INTERN_ID_NONE
        && ast_path_segments_are(&ast, third->bounds[0].trait_type,
            sized_path, 1u)
        && third->bounds[1].kind == CM_AST_GENERIC_BOUND_LIFETIME
        && third->bounds[1].modifier == CM_AST_GENERIC_BOUND_REQUIRED
        && third->bounds[1].trait_type == CM_AST_TYPE_NONE
        && ast_string_is(&ast, third->bounds[1].lifetime, "'a")
        && ast_span_is(source, third->bounds[1].span, "'a")
        && third->bounds[2].kind == CM_AST_GENERIC_BOUND_TRAIT
        && third->bounds[2].modifier == CM_AST_GENERIC_BOUND_REQUIRED
        && third->bounds[2].lifetime == CM_INTERN_ID_NONE
        && ast_path_segments_are(&ast, third->bounds[2].trait_type,
            send_path, 1u)
        && ast_dump_contains(&ast,
            "(generic-bound required lifetime \"'static\")");
    if (!ok) {
        fprintf(stderr, "lifetime generic bounds were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_generic_parameter_bound_error_paths(void)
{
    static const struct {
        const char *source;
        const char *message;
    } broken[] = {
        { "type A<'a: Sized> = &'a u8;", "require lifetime bounds" },
        { "type A<'a: ?'static> = &'a u8;", "cannot have modifiers" },
        { "type A<T: ~const 'static> = T;", "cannot have modifiers" },
        { "type A<const N: usize + Copy> = [u8; N];",
            "const generic parameter bounds" },
        { "type A<T: const Trait> = T;", "const generic parameter bounds" },
        { "type A<T: ~Trait> = T;", "expected 'const' after '~'" },
        { "type A<T: for<'a> FnMut(&'a T)> = T;",
            "HRTB generic parameter bounds" },
        { "type A<T: use<U>> = T;", "use generic parameter bounds" },
        { "type A<T: Iterator<Item:, T>> = T;",
            "expected associated-type constraint" },
        { "type A<T: Iterator<Item: Copy +, T>> = T;",
            "expected bound after '+'" },
        { "type A<T: Iterator<: Copy>> = T;", "expected type" },
        { "type A<T: Iterator< = u8>> = T;", "expected type" },
        { "type A<T: Trait +> = T;", "expected bound after '+'" },
        { "type A<T: + Trait> = T;", "expected bound" },
        { "type A<T: Trait Other> = T;", "generic parameter terminator" },
        { "type A<T: FnMut(T) ->> = T;", "expected return type" },
        { "type A<T: Trait<U V>> = T;", "expected '>'" }
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index].source,
            strlen(broken[index].source), CM_EDITION_2024);
        if (result.error_count == 0u
            || strstr(result.first_error.message, broken[index].message)
                == NULL) {
            fprintf(stderr,
                "invalid generic bound %lu reported '%s'\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_nested_generic_associated_type_constraint(void)
{
    static const char source[] =
        "struct Flatten<I: Iterator<Item: IntoIterator>> { value: I }";
    static const char *const iterator_path[] = { "Iterator" };
    static const char *const into_iterator_path[] = { "IntoIterator" };
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstGenericParam *parameter;
    const CmAstType *trait_type;
    const CmAstPath *path;
    const CmAstGenericArg *constraint;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    parameter = item == NULL || item->generic_parameter_count != 1u
            || item->generic_parameters == NULL
        ? NULL : &item->generic_parameters[0];
    trait_type = parameter == NULL || parameter->bound_count != 1u
            || parameter->bounds == NULL
        ? NULL : cm_ast_get_type(&ast, parameter->bounds[0].trait_type);
    path = trait_type == NULL || trait_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, trait_type->path);
    constraint = path == NULL || path->segment_count != 1u
            || path->segments == NULL
            || path->segments[0].argument_count != 1u
            || path->segments[0].arguments == NULL
        ? NULL : &path->segments[0].arguments[0];
    ok = result.error_count == 0u && parameter != NULL
        && ast_path_segments_are(&ast, parameter->bounds[0].trait_type,
            iterator_path, 1u)
        && constraint != NULL && constraint->kind == CM_AST_GENERIC_CONSTRAINT
        && ast_string_is(&ast, constraint->name, "Item")
        && constraint->bound_count == 1u && constraint->bounds != NULL
        && ast_path_segments_are(&ast,
            constraint->bounds[0].trait_type, into_iterator_path, 1u)
        && ast_span_is(source, constraint->span, "Item: IntoIterator")
        && ast_dump_contains(&ast,
            "constraint \"Item\":path(\"IntoIterator\")");
    if (!ok) {
        fprintf(stderr, "nested associated-type constraint was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_relaxed_sized_generic_parameter_bounds(void)
{
    static const char source[] =
        "fn forget<T: ?Sized, U: ?Sized + Send>(_: T, _: U);";
    static const char *const sized_path[] = { "Sized" };
    static const char *const send_path[] = { "Send" };
    static const struct {
        const char *source;
        const char *message;
    } malformed[] = {
        { "type A<T: ?Trait> = T;", "only ?Sized" },
        { "type A<T: ?Sized<u8>> = T;", "only ?Sized" },
        { "type A<T: ?Sized<Item = u8>> = T;", "only ?Sized" }
    };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstGenericParam *first;
    const CmAstGenericParam *second;
    size_t index;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    first = item == NULL || item->generic_parameter_count != 2u
        || item->generic_parameters == NULL ? NULL
        : &item->generic_parameters[0];
    second = first == NULL ? NULL : &item->generic_parameters[1];
    ok = result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_FUNCTION
        && first != NULL && first->bound_count == 1u
        && first->bounds != NULL
        && first->bounds[0].modifier == CM_AST_GENERIC_BOUND_RELAXED
        && ast_span_is(source, first->bounds[0].span, "?Sized")
        && ast_path_segments_are(&ast, first->bounds[0].trait_type,
            sized_path, 1u)
        && ast_string_is(&ast, first->constraint, "?Sized")
        && second != NULL && second->bound_count == 2u
        && second->bounds != NULL
        && second->bounds[0].modifier == CM_AST_GENERIC_BOUND_RELAXED
        && second->bounds[1].modifier == CM_AST_GENERIC_BOUND_REQUIRED
        && ast_path_segments_are(&ast, second->bounds[0].trait_type,
            sized_path, 1u)
        && ast_path_segments_are(&ast, second->bounds[1].trait_type,
            send_path, 1u)
        && ast_dump_contains(&ast,
            "(generic-bound relaxed path(\"Sized\"))");
    if (!ok) {
        fprintf(stderr, "relaxed Sized generic bounds were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);

    for (index = 0u;
         index < sizeof(malformed) / sizeof(malformed[0]); ++index) {
        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, malformed[index].source,
            strlen(malformed[index].source), CM_EDITION_2024);
        if (result.error_count == 0u
            || strstr(result.first_error.message,
                malformed[index].message) == NULL) {
            fprintf(stderr, "invalid relaxed bound %lu reported '%s'\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_conditionally_const_generic_parameter_bounds(void)
{
    static const char source[] =
        "trait Carrier<T: ~const fallback::Bound + Copy> {}";
    static const char *const bound_path[] = { "fallback", "Bound" };
    static const char *const copy_path[] = { "Copy" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstGenericParam *parameter;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    parameter = item == NULL || item->generic_parameter_count != 1u
            || item->generic_parameters == NULL
        ? NULL : &item->generic_parameters[0];
    ok = result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TRAIT
        && parameter != NULL && parameter->bound_count == 2u
        && parameter->bounds != NULL
        && parameter->bounds[0].modifier
            == CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST
        && parameter->bounds[1].modifier == CM_AST_GENERIC_BOUND_REQUIRED
        && ast_span_is(source, parameter->bounds[0].span,
            "~const fallback::Bound")
        && ast_span_is(source, parameter->bounds[1].span, "Copy")
        && ast_path_segments_are(&ast, parameter->bounds[0].trait_type,
            bound_path, 2u)
        && ast_path_segments_are(&ast, parameter->bounds[1].trait_type,
            copy_path, 1u)
        && ast_string_is(&ast, parameter->constraint,
            "~const fallback::Bound + Copy")
        && ast_dump_contains(&ast,
            "(generic-bound conditionally-const "
            "path(\"fallback\"::\"Bound\"))");
    if (!ok) {
        fprintf(stderr,
            "conditionally const generic bounds were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_invalid_impl_prefixes(void)
{
    static const char *const broken[] = {
        "async impl T for u8 {}",
        "extern impl T for u8 {}",
        "extern \"C\" impl T for u8 {}",
        "unsafe async impl T for u8 {}"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u
            || strstr(result.first_error.message,
                "not permitted on impl blocks") == NULL) {
            fprintf(stderr,
                "invalid impl prefix %lu did not produce its hard error\n",
                (unsigned long)index);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_const_impl_modifiers(void)
{
    static const char source[] =
        "impl const From<Infallible> for TryFromIntError {}\n"
        "unsafe impl<T> const SliceIndex<[T]> for Range<T> {}\n"
        "impl !Trait for Type {}\n"
        "impl ! {}";
    static const char *const malformed[] = {
        "impl const const Trait for Type {}",
        "impl<T> const {}",
        "impl const Trait Type {}"
    };
    static const char *const from_path[] = { "From" };
    static const char *const error_path[] = { "TryFromIntError" };
    static const char *const slice_index_path[] = { "SliceIndex" };
    static const char *const range_path[] = { "Range" };
    static const char *const trait_path[] = { "Trait" };
    static const char *const type_path[] = { "Type" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *item_id;
    const CmAstItem *const_impl;
    const CmAstItem *unsafe_const_impl;
    const CmAstItem *negative_impl;
    const CmAstItem *never_impl;
    const CmAstType *never_type;
    size_t index;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    item_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    const_impl = item_id == NULL ? NULL : cm_ast_get_item(&ast, *item_id);
    item_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    unsafe_const_impl = item_id == NULL
        ? NULL : cm_ast_get_item(&ast, *item_id);
    item_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    negative_impl = item_id == NULL
        ? NULL : cm_ast_get_item(&ast, *item_id);
    item_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 3u);
    never_impl = item_id == NULL ? NULL : cm_ast_get_item(&ast, *item_id);
    never_type = never_impl == NULL ? NULL : cm_ast_get_type(&ast,
        never_impl->data.impl_item.self_type);
    ok = result.error_count == 0u && ast.root_items.len == 4u
        && const_impl != NULL && const_impl->kind == CM_AST_ITEM_IMPL
        && const_impl->data.impl_item.is_const
        && !const_impl->data.impl_item.is_unsafe
        && !const_impl->data.impl_item.is_negative
        && ast_path_segments_are(&ast,
            const_impl->data.impl_item.trait_type, from_path, 1u)
        && ast_path_segments_are(&ast,
            const_impl->data.impl_item.self_type, error_path, 1u)
        && ast_span_is(source, const_impl->span,
            "impl const From<Infallible> for TryFromIntError {}")
        && unsafe_const_impl != NULL
        && unsafe_const_impl->kind == CM_AST_ITEM_IMPL
        && unsafe_const_impl->data.impl_item.is_const
        && unsafe_const_impl->data.impl_item.is_unsafe
        && !unsafe_const_impl->data.impl_item.is_negative
        && unsafe_const_impl->generic_parameter_count == 1u
        && ast_path_segments_are(&ast,
            unsafe_const_impl->data.impl_item.trait_type,
            slice_index_path, 1u)
        && ast_path_segments_are(&ast,
            unsafe_const_impl->data.impl_item.self_type, range_path, 1u)
        && negative_impl != NULL
        && negative_impl->kind == CM_AST_ITEM_IMPL
        && !negative_impl->data.impl_item.is_const
        && negative_impl->data.impl_item.is_negative
        && ast_path_segments_are(&ast,
            negative_impl->data.impl_item.trait_type, trait_path, 1u)
        && ast_path_segments_are(&ast,
            negative_impl->data.impl_item.self_type, type_path, 1u)
        && never_impl != NULL && never_impl->kind == CM_AST_ITEM_IMPL
        && !never_impl->data.impl_item.is_const
        && !never_impl->data.impl_item.is_negative
        && never_impl->data.impl_item.trait_type == CM_AST_TYPE_NONE
        && never_type != NULL && never_type->kind == CM_AST_TYPE_NEVER
        && ast_dump_contains(&ast,
            "(impl-header unsafe=0 const=1 negative=0 ")
        && ast_dump_contains(&ast,
            "(impl-header unsafe=0 const=0 negative=1 ");
    if (!ok) {
        fprintf(stderr, "const impl AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(malformed) / sizeof(malformed[0]);
         ++index) {
        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, malformed[index],
            strlen(malformed[index]), CM_EDITION_2024);
        if (result.error_count == 0u) {
            fprintf(stderr,
                "malformed const impl %lu did not produce an error\n",
                (unsigned long)index);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_explicit_safe_foreign_function(void)
{
    static const char source[] =
        "unsafe extern \"C\" {\n"
        "    #[link_name = \"cbrt\"]\n"
        "    pub(crate) safe fn cbrt(n: f64) -> f64;\n"
        "    pub(crate) unsafe fn raw(n: *const u8);\n"
        "}";
    static const char *const malformed[] = {
        "safe fn local() {}",
        "unsafe extern \"C\" { safe unsafe fn both(); }",
        "unsafe extern \"C\" { safe const fn both(); }",
        "unsafe extern \"C\" { safe static VALUE: u8; }",
        "unsafe extern \"C\" { safe safe fn twice(); }"
    };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *extern_block;
    const CmAstItem *safe_function;
    const CmAstItem *unsafe_function;
    size_t index;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    extern_block = root_id == NULL ? NULL
        : cm_ast_get_item(&ast, *root_id);
    safe_function = extern_block == NULL
            || extern_block->kind != CM_AST_ITEM_EXTERN_BLOCK
            || extern_block->data.extern_block_item.item_count < 1u
        ? NULL : cm_ast_get_item(&ast,
            extern_block->data.extern_block_item.items[0]);
    unsafe_function = extern_block == NULL
            || extern_block->kind != CM_AST_ITEM_EXTERN_BLOCK
            || extern_block->data.extern_block_item.item_count < 2u
        ? NULL : cm_ast_get_item(&ast,
            extern_block->data.extern_block_item.items[1]);
    ok = result.error_count == 0u && ast.root_items.len == 1u
        && extern_block != NULL
        && extern_block->kind == CM_AST_ITEM_EXTERN_BLOCK
        && extern_block->data.extern_block_item.is_unsafe
        && extern_block->data.extern_block_item.item_count == 2u
        && ast_string_is(&ast,
            extern_block->data.extern_block_item.abi, "C")
        && ast_span_is(source, extern_block->span, source)
        && safe_function != NULL
        && safe_function->kind == CM_AST_ITEM_FUNCTION
        && safe_function->visibility.kind == CM_AST_VIS_CRATE
        && safe_function->attribute_count == 1u
        && safe_function->data.function_item.is_safe
        && !safe_function->data.function_item.is_unsafe
        && safe_function->data.function_item.body == CM_AST_EXPR_NONE
        && ast_span_is(source, safe_function->span,
            "pub(crate) safe fn cbrt(n: f64) -> f64;")
        && unsafe_function != NULL
        && unsafe_function->kind == CM_AST_ITEM_FUNCTION
        && !unsafe_function->data.function_item.is_safe
        && unsafe_function->data.function_item.is_unsafe
        && ast_dump_contains(&ast, "(safety unsafe)")
        && ast_dump_contains(&ast,
            "(flags const=0 async=0 unsafe=0 body=0 safe=1)");
    if (!ok) {
        fprintf(stderr, "explicit safe foreign function AST was incorrect: "
            "%s\n", result.first_error.message);
    }
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(malformed) / sizeof(malformed[0]);
         ++index) {
        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, malformed[index],
            strlen(malformed[index]), CM_EDITION_2024);
        if (result.error_count == 0u) {
            fprintf(stderr,
                "malformed safe function %lu did not produce an error\n",
                (unsigned long)index);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_const_default_comparisons(void)
{
    static const char *const valid[] = {
        "struct A<const N: bool = { 2 > 1 }>;",
        "struct B<const N: bool = { 1 < 2 }>;"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, valid[index], strlen(valid[index]),
            CM_EDITION_2024);
        if (result.error_count != 0u) {
            fprintf(stderr,
                "valid const default comparison %lu produced an error: %s\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_projection_error_paths(void)
{
    static const char *const broken[] = {
        "type MissingAs<T> = <T>::Assoc;",
        "type MissingSelf = <as Trait>::Assoc;",
        "type MissingTrait<T> = <T as>::Assoc;",
        "type DanglingTrait<T> = <T as Trait::>::Assoc;",
        "type MissingSeparator<T> = <T as Trait>Assoc;",
        "type MissingAssoc<T> = <T as Trait>::;",
        "type MultiSegment<T> = <T as Trait>::Assoc::Nested;",
        "type Turbofish<T, U> = <T as Trait>::Assoc::<U>;",
        "type BadConstParen<T> = <T as Trait>::Assoc<{ (] }>;",
        "type BadConstBracket<T> = <T as Trait<{ [) }>>::Assoc;",
        "type BadConstBrace<T> = <T as Trait>::Assoc<{ { ] } }>;",
        "type UnclosedConstParen<T> = <T as Trait>::Assoc<{ (",
        "type UnclosedConstBracket<T> = <T as Trait>::Assoc<{ [",
        "type UnclosedConstBrace<T> = <T as Trait>::Assoc<{ {"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u || result.first_error.message[0] == 0) {
            fprintf(stderr,
                "invalid or unsupported projection %lu did not produce an error\n",
                (unsigned long)index);
            ok = 0;
        }
        if (index >= 8u && index <= 10u
            && strcmp(result.first_error.message, "mismatched delimiter")
                != 0) {
            fprintf(stderr,
                "mismatched projection delimiter %lu reported '%s'\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        if (index >= 11u && index <= 13u
            && strcmp(result.first_error.message,
                "unterminated delimited group") != 0) {
            fprintf(stderr,
                "unterminated projection delimiter %lu reported '%s'\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    {
        static const char split_error[] = "type X<T> = <Vec<T>>;";
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, split_error,
            sizeof(split_error) - 1u, CM_EDITION_2024);
        if (result.error_count == 0u
            || result.first_error.offset != 19u
            || result.first_error.line != 1u
            || result.first_error.column != 20u
            || strcmp(result.first_error.message,
                "expected 'as' in explicit type projection") != 0) {
            fprintf(stderr,
                "pending split '>' diagnostic was %lu:%lu at offset %lu: %s\n",
                (unsigned long)result.first_error.line,
                (unsigned long)result.first_error.column,
                (unsigned long)result.first_error.offset,
                result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_nested_projection_paths(void)
{
    static const char *const valid[] = {
        "type Nested<T> = <<T as Trait>::Assoc as Other>::Output;",
        "type Argument<T> = Wrapper<<T as Trait>::Assoc>;",
        "type DeepArgument<T> = Outer<Inner<<T as Trait>::Assoc>>;",
        "type SplitEnd<T, U> = <T as Trait<U>>::Assoc;",
        ("type ConstArgument<T> = <T as Trait>::Assoc<"
            "{ if 1 < 2 { (3) } else { [4][0] } }>;")
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, valid[index], strlen(valid[index]),
            CM_EDITION_2024);
        if (result.error_count != 0u) {
            fprintf(stderr,
                "valid nested projection %lu produced an error: %s\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        if (index == 0u && result.error_count == 0u) {
            const CmAstItemId *root_id;
            const CmAstItem *item;
            const CmAstType *outer;
            const CmAstType *inner;
            const char *opening;

            root_id = (const CmAstItemId *)cm_vec_at_const(
                &ast.root_items, 0u);
            item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
            outer = item == NULL ? NULL : cm_ast_get_type(&ast,
                item->data.value_item.type);
            inner = outer == NULL ? NULL : cm_ast_get_type(&ast,
                outer->projection.self_type);
            opening = strstr(valid[index], "<<");
            if (item == NULL || item->kind != CM_AST_ITEM_TYPE_ALIAS
                || outer == NULL || outer->kind != CM_AST_TYPE_PROJECTION
                || inner == NULL || inner->kind != CM_AST_TYPE_PROJECTION
                || opening == NULL
                || outer->span.start != (uint32_t)(opening - valid[index])
                || inner->span.start != outer->span.start + 1u) {
                fprintf(stderr,
                    "split nested projection has incorrect AST spans "
                    "(outer=%lu, inner=%lu, expected=%lu)\n",
                    (unsigned long)(outer == NULL ? 0u : outer->span.start),
                    (unsigned long)(inner == NULL ? 0u : inner->span.start),
                    (unsigned long)(opening == NULL ? 0u
                        : (size_t)(opening - valid[index])));
                ok = 0;
            }
        }
        if (index == 3u && result.error_count == 0u) {
            const CmAstItemId *root_id;
            const CmAstItem *item;
            const CmAstType *projection;
            const CmAstPath *trait_path;
            const char *trait_text;
            const char *semicolon;

            root_id = (const CmAstItemId *)cm_vec_at_const(
                &ast.root_items, 0u);
            item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
            projection = item == NULL ? NULL : cm_ast_get_type(&ast,
                item->data.value_item.type);
            trait_path = projection == NULL ? NULL : cm_ast_get_path(&ast,
                projection->projection.trait_path);
            trait_text = strstr(valid[index], "Trait<U>");
            semicolon = strchr(valid[index], ';');
            if (projection == NULL
                || projection->kind != CM_AST_TYPE_PROJECTION
                || trait_path == NULL || trait_text == NULL
                || semicolon == NULL
                || trait_path->span.end != (uint32_t)(
                    (size_t)(trait_text - valid[index])
                    + strlen("Trait<U>"))
                || projection->span.end
                    != (uint32_t)(semicolon - valid[index])) {
                fputs("split '>>' produced incorrect path/type span ends\n",
                    stderr);
                ok = 0;
            }
        }
        cm_ast_destroy(&ast);
    }
    {
        static const char shift[] = "left << right";
        CmAst ast;
        CmExpressionFragment fragment;
        const CmAstExpr *expression;

        cm_ast_init(&ast);
        fragment = cm_parse_expression_fragment(&ast, shift,
            sizeof(shift) - 1u, CM_EDITION_2024);
        expression = cm_ast_get_expr(&ast, fragment.expression);
        if (fragment.parse.error_count != 0u || expression == NULL
            || expression->kind != CM_AST_EXPR_BINARY
            || !ast_string_is(&ast, expression->data.binary.operator_name,
                "<<")) {
            fputs("type-context '<' splitting changed expression shift parsing\n",
                stderr);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_post_value_type_alias_where_clause(void)
{
    static const char source[] =
        "type Alias<'a, T> where T: Copy = &'a T where T: 'a;";
    static const char *const t_path[] = { "T" };
    static const char *const copy_path[] = { "Copy" };
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstWherePredicate *before;
    const CmAstWherePredicate *after;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    before = item == NULL || item->where_predicate_count != 1u
            || item->where_predicates == NULL
        ? NULL : &item->where_predicates[0];
    after = item == NULL
            || item->data.value_item.post_value_where_predicate_count != 1u
            || item->data.value_item.post_value_where_predicates == NULL
        ? NULL : &item->data.value_item.post_value_where_predicates[0];
    ok = result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TYPE_ALIAS
        && item->data.value_item.has_value == 1
        && ast_string_is(&ast, item->where_clause, "T: Copy")
        && before != NULL
        && ast_path_segments_are(&ast, before->subject, t_path, 1u)
        && before->bound_count == 1u && before->bounds != NULL
        && before->bounds[0].kind == CM_AST_WHERE_BOUND_TRAIT
        && ast_path_segments_are(&ast, before->bounds[0].trait_type,
            copy_path, 1u)
        && ast_string_is(&ast,
            item->data.value_item.post_value_where_clause, "T: 'a")
        && after != NULL
        && ast_path_segments_are(&ast, after->subject, t_path, 1u)
        && after->bound_count == 1u && after->bounds != NULL
        && after->bounds[0].kind == CM_AST_WHERE_BOUND_LIFETIME
        && ast_string_is(&ast, after->bounds[0].lifetime, "'a")
        && ast_span_is(source, after->span, "T: 'a")
        && ast_dump_contains(&ast, "(post-value-where \"T: 'a\")")
        && ast_dump_contains(&ast,
            "(post-value-where-predicate path(\"T\")");
    if (!ok) {
        fprintf(stderr, "post-value where clause was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_post_value_associated_type_where_clause(void)
{
    static const char source[] =
        "trait AsyncFnMut { type CallRefFuture<'a>: "
        "Future<Output = Self::Output> where Self: 'a; } "
        "impl<F> AsyncFnMut for F { type CallRefFuture<'a> = "
        "F::CallRefFuture<'a> where Self: 'a; }";
    static const char *const self_path[] = { "Self" };
    const CmAstItemId *root_id;
    const CmAstItem *trait_item;
    const CmAstItem *impl_item;
    const CmAstItem *declaration;
    const CmAstItem *definition;
    const CmAstWherePredicate *predicate;
    const CmAstType *target;
    const CmAstPath *target_path;
    const CmAstPathSegment *target_segment;
    const CmAstGenericArg *target_argument;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    trait_item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    impl_item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    declaration = trait_item == NULL
            || trait_item->data.trait_item.item_count != 1u
            || trait_item->data.trait_item.items == NULL
        ? NULL : cm_ast_get_item(&ast,
            trait_item->data.trait_item.items[0]);
    definition = impl_item == NULL
            || impl_item->data.impl_item.item_count != 1u
            || impl_item->data.impl_item.items == NULL
        ? NULL : cm_ast_get_item(&ast, impl_item->data.impl_item.items[0]);
    predicate = definition == NULL
            || definition->data.value_item.post_value_where_predicate_count
                != 1u
            || definition->data.value_item.post_value_where_predicates
                == NULL
        ? NULL : &definition->data.value_item
            .post_value_where_predicates[0];
    target = definition == NULL ? NULL : cm_ast_get_type(&ast,
        definition->data.value_item.type);
    target_path = target == NULL || target->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, target->path);
    target_segment = target_path == NULL || target_path->segment_count != 2u
            || target_path->segments == NULL
        ? NULL : &target_path->segments[1];
    target_argument = target_segment == NULL
            || target_segment->argument_count != 1u
            || target_segment->arguments == NULL
        ? NULL : &target_segment->arguments[0];
    ok = result.error_count == 0u && ast.root_items.len == 2u
        && declaration != NULL
        && declaration->kind == CM_AST_ITEM_TYPE_ALIAS
        && ast_string_is(&ast, declaration->where_clause, "Self: 'a")
        && declaration->data.value_item.post_value_where_clause
            == CM_INTERN_ID_NONE
        && definition != NULL
        && definition->kind == CM_AST_ITEM_TYPE_ALIAS
        && definition->generic_parameter_count == 1u
        && definition->data.value_item.has_value == 1
        && target != NULL && ast_span_is(source, target->span,
            "F::CallRefFuture<'a>")
        && target_argument != NULL
        && target_argument->kind == CM_AST_GENERIC_LIFETIME
        && ast_string_is(&ast, target_argument->text, "'a")
        && ast_span_is(source, target_argument->span, "'a")
        && definition->where_clause == CM_INTERN_ID_NONE
        && ast_string_is(&ast,
            definition->data.value_item.post_value_where_clause,
            "Self: 'a")
        && predicate != NULL
        && ast_path_segments_are(&ast, predicate->subject, self_path, 1u)
        && predicate->bound_count == 1u && predicate->bounds != NULL
        && predicate->bounds[0].kind == CM_AST_WHERE_BOUND_LIFETIME
        && ast_string_is(&ast, predicate->bounds[0].lifetime, "'a")
        && ast_span_is(source, predicate->span, "Self: 'a")
        && ast_span_is(source, predicate->bounds[0].span, "'a");
    if (!ok) {
        fprintf(stderr, "associated post-value where clause was incorrect: "
            "%s\n", result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_structured_supertraits(void)
{
    static const char source[] =
        "trait T: A + module::B + ~const C where Self: D {}";
    static const char *const bound_text[] = {
        "A", "module::B", "~const C"
    };
    static const char *const type_text[] = {
        "A", "module::B", "C"
    };
    static const CmAstSupertraitModifier modifiers[] = {
        CM_AST_SUPERTRAIT_REQUIRED,
        CM_AST_SUPERTRAIT_REQUIRED,
        CM_AST_SUPERTRAIT_CONDITIONALLY_CONST
    };
    static const char *const first_path[] = { "A" };
    static const char *const second_path[] = { "module", "B" };
    static const char *const third_path[] = { "C" };
    const char *const *paths[] = { first_path, second_path, third_path };
    static const uint32_t path_counts[] = { 1u, 2u, 1u };
    const CmAstItemId *root_id;
    const CmAstItem *item;
    CmAst ast;
    CmParseResult result;
    uint32_t index;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    ok = result.error_count == 0u && item != NULL
        && item->kind == CM_AST_ITEM_TRAIT
        && item->data.trait_item.structured_supertrait_count == 3u
        && ast_string_is(&ast, item->data.trait_item.supertraits,
            "A + module::B + ~const C")
        && ast_string_is(&ast, item->where_clause, "Self: D");
    for (index = 0u; ok && index < 3u; ++index) {
        const CmAstSupertrait *supertrait;
        const CmAstType *type;

        supertrait = &item->data.trait_item.structured_supertraits[index];
        type = cm_ast_get_type(&ast, supertrait->type);
        ok = supertrait->modifier == modifiers[index]
            && ast_span_is(source, supertrait->span, bound_text[index])
            && type != NULL
            && ast_span_is(source, type->span, type_text[index])
            && ast_path_segments_are(&ast, supertrait->type, paths[index],
                path_counts[index]);
    }
    if (!ok) {
        fprintf(stderr, "structured supertrait AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_trait_alias_items(void)
{
    static const char source[] =
        "#[unstable] pub trait Thin = "
        "Pointee<Metadata = ()> + PointeeSized + 'static;";
    static const char *const pointee_path[] = { "Pointee" };
    static const char *const sized_path[] = { "PointeeSized" };
    static const char *const broken[] = {
        "trait Empty =;",
        "unsafe trait UnsafeAlias = Send;",
        "trait MissingSemicolon = Send {}"
    };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstSupertrait *bounds;
    size_t index;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    bounds = item == NULL
            || item->data.trait_item.structured_alias_bound_count != 3u
            || item->data.trait_item.structured_alias_bounds == NULL
        ? NULL : item->data.trait_item.structured_alias_bounds;
    ok = result.error_count == 0u && ast.root_items.len == 1u
        && item != NULL && item->kind == CM_AST_ITEM_TRAIT
        && item->visibility.kind == CM_AST_VIS_PUBLIC
        && item->attribute_count == 1u
        && item->data.trait_item.is_alias == 1
        && item->data.trait_item.is_unsafe == 0
        && item->data.trait_item.supertraits == CM_INTERN_ID_NONE
        && item->data.trait_item.structured_supertrait_count == 0u
        && item->data.trait_item.items == NULL
        && item->data.trait_item.item_count == 0u
        && ast_string_is(&ast, item->data.trait_item.alias_bounds,
            "Pointee<Metadata = ()> + PointeeSized + 'static")
        && bounds != NULL
        && bounds[0].kind == CM_AST_SUPERTRAIT_TRAIT
        && ast_path_segments_are(&ast, bounds[0].type, pointee_path, 1u)
        && bounds[1].kind == CM_AST_SUPERTRAIT_TRAIT
        && ast_path_segments_are(&ast, bounds[1].type, sized_path, 1u)
        && bounds[2].kind == CM_AST_SUPERTRAIT_LIFETIME
        && ast_string_is(&ast, bounds[2].lifetime, "'static")
        && ast_span_is(source, item->span,
            "pub trait Thin = "
            "Pointee<Metadata = ()> + PointeeSized + 'static;")
        && ast_dump_contains(&ast, "(trait-alias)")
        && ast_dump_contains(&ast,
            "(alias-bound required path(\"PointeeSized\"))");
    if (!ok) {
        fprintf(stderr, "trait alias AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u) {
            fprintf(stderr, "invalid trait alias %lu was accepted\n",
                (unsigned long)index);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_auto_trait_items(void)
{
    static const char source[] =
        "pub unsafe auto trait Send {} fn auto() {}";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *trait_item;
    const CmAstItem *function;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    trait_item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    ok = result.error_count == 0u && ast.root_items.len == 2u
        && trait_item != NULL && trait_item->kind == CM_AST_ITEM_TRAIT
        && trait_item->visibility.kind == CM_AST_VIS_PUBLIC
        && trait_item->data.trait_item.is_unsafe == 1
        && trait_item->data.trait_item.is_auto == 1
        && trait_item->data.trait_item.is_alias == 0
        && function != NULL && function->kind == CM_AST_ITEM_FUNCTION
        && ast_string_is(&ast, function->name, "auto")
        && ast_dump_contains(&ast, "(auto-trait)");
    if (!ok) {
        fprintf(stderr, "auto trait AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_lifetime_trait_bounds(void)
{
    static const char source[] =
        "trait T: Copy + 'static { "
        "type Item: ?Sized + 'static + Send; }";
    static const char *const copy_path[] = { "Copy" };
    static const char *const sized_path[] = { "Sized" };
    static const char *const send_path[] = { "Send" };
    const CmAstItemId *root_id;
    const CmAstItem *trait_item;
    const CmAstItem *associated_type;
    const CmAstSupertrait *supertraits;
    const CmAstAssociatedTypeBound *bounds;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    trait_item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    associated_type = trait_item == NULL
            || trait_item->data.trait_item.item_count != 1u
            || trait_item->data.trait_item.items == NULL
        ? NULL : cm_ast_get_item(&ast,
            trait_item->data.trait_item.items[0]);
    supertraits = trait_item == NULL
            || trait_item->data.trait_item.structured_supertrait_count != 2u
        ? NULL : trait_item->data.trait_item.structured_supertraits;
    bounds = associated_type == NULL
            || associated_type->data.value_item.bound_count != 3u
        ? NULL : associated_type->data.value_item.bounds;
    ok = result.error_count == 0u && trait_item != NULL
        && trait_item->kind == CM_AST_ITEM_TRAIT
        && ast_string_is(&ast, trait_item->data.trait_item.supertraits,
            "Copy + 'static")
        && supertraits != NULL
        && supertraits[0].kind == CM_AST_SUPERTRAIT_TRAIT
        && supertraits[0].modifier == CM_AST_SUPERTRAIT_REQUIRED
        && supertraits[0].lifetime == CM_INTERN_ID_NONE
        && ast_path_segments_are(&ast, supertraits[0].type,
            copy_path, 1u)
        && ast_span_is(source, supertraits[0].span, "Copy")
        && supertraits[1].kind == CM_AST_SUPERTRAIT_LIFETIME
        && supertraits[1].modifier == CM_AST_SUPERTRAIT_REQUIRED
        && supertraits[1].type == CM_AST_TYPE_NONE
        && ast_string_is(&ast, supertraits[1].lifetime, "'static")
        && ast_span_is(source, supertraits[1].span, "'static")
        && associated_type != NULL
        && associated_type->kind == CM_AST_ITEM_TYPE_ALIAS
        && bounds != NULL
        && bounds[0].kind == CM_AST_ASSOC_BOUND_TRAIT
        && bounds[0].modifier == CM_AST_ASSOC_BOUND_RELAXED
        && bounds[0].lifetime == CM_INTERN_ID_NONE
        && ast_path_segments_are(&ast, bounds[0].trait_type,
            sized_path, 1u)
        && ast_span_is(source, bounds[0].span, "?Sized")
        && bounds[1].kind == CM_AST_ASSOC_BOUND_LIFETIME
        && bounds[1].modifier == CM_AST_ASSOC_BOUND_REQUIRED
        && bounds[1].trait_type == CM_AST_TYPE_NONE
        && ast_string_is(&ast, bounds[1].lifetime, "'static")
        && ast_span_is(source, bounds[1].span, "'static")
        && bounds[2].kind == CM_AST_ASSOC_BOUND_TRAIT
        && bounds[2].modifier == CM_AST_ASSOC_BOUND_REQUIRED
        && bounds[2].lifetime == CM_INTERN_ID_NONE
        && ast_path_segments_are(&ast, bounds[2].trait_type,
            send_path, 1u)
        && ast_span_is(source, bounds[2].span, "Send")
        && ast_dump_contains(&ast,
            "(supertrait required lifetime \"'static\")")
        && ast_dump_contains(&ast,
            "(associated-type-bound required lifetime \"'static\")");
    if (!ok) {
        fprintf(stderr, "lifetime trait bounds were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_structured_where_predicates(void)
{
    static const char source[] =
        "trait T { fn f(&self) where "
        "Self: Sized + ~const Marker + const Always, "
        "Self::Item: ?Sized,; }";
    static const char *const self_path[] = { "Self" };
    static const char *const item_path[] = { "Self", "Item" };
    static const char *const sized_path[] = { "Sized" };
    static const char *const marker_path[] = { "Marker" };
    static const char *const always_path[] = { "Always" };
    const CmAstItemId *root_id;
    const CmAstItem *trait_item;
    const CmAstItem *method;
    const CmAstWherePredicate *first;
    const CmAstWherePredicate *second;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    trait_item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    method = trait_item == NULL
            || trait_item->data.trait_item.item_count != 1u
        ? NULL : cm_ast_get_item(&ast,
            trait_item->data.trait_item.items[0]);
    first = method == NULL || method->where_predicate_count != 2u
        ? NULL : &method->where_predicates[0];
    second = first == NULL ? NULL : &method->where_predicates[1];
    ok = result.error_count == 0u
        && method != NULL && method->kind == CM_AST_ITEM_FUNCTION
        && ast_string_is(&ast, method->where_clause,
            "Self: Sized + ~const Marker + const Always, "
            "Self::Item: ?Sized,")
        && first != NULL && first->bound_count == 3u
        && first->bounds != NULL
        && ast_span_is(source, first->span,
            "Self: Sized + ~const Marker + const Always")
        && ast_path_segments_are(&ast, first->subject, self_path, 1u)
        && first->bounds[0].modifier == CM_AST_WHERE_BOUND_REQUIRED
        && ast_span_is(source, first->bounds[0].span, "Sized")
        && ast_path_segments_are(&ast, first->bounds[0].trait_type,
            sized_path, 1u)
        && first->bounds[1].modifier
            == CM_AST_WHERE_BOUND_CONDITIONALLY_CONST
        && ast_span_is(source, first->bounds[1].span, "~const Marker")
        && ast_path_segments_are(&ast, first->bounds[1].trait_type,
            marker_path, 1u)
        && first->bounds[2].modifier == CM_AST_WHERE_BOUND_CONST
        && ast_span_is(source, first->bounds[2].span, "const Always")
        && ast_path_segments_are(&ast, first->bounds[2].trait_type,
            always_path, 1u)
        && ast_dump_contains(&ast,
            "(where-bound const path(\"Always\"))")
        && second != NULL && second->bound_count == 1u
        && second->bounds != NULL
        && ast_span_is(source, second->span, "Self::Item: ?Sized")
        && ast_path_segments_are(&ast, second->subject, item_path, 2u)
        && second->bounds[0].modifier == CM_AST_WHERE_BOUND_RELAXED
        && ast_span_is(source, second->bounds[0].span, "?Sized")
        && ast_path_segments_are(&ast, second->bounds[0].trait_type,
            sized_path, 1u);
    if (!ok) {
        fprintf(stderr, "structured where predicates were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_higher_ranked_where_bound(void)
{
    static const char source[] =
        "fn with_copy<F, R>() where "
        "F: for<'copy> FnOnce(VaList<'copy, 'f>) -> R {}";
    static const char *const f_path[] = { "F" };
    static const char *const fn_once_path[] = { "FnOnce" };
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstWherePredicate *predicate;
    const CmAstWhereBound *bound;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    predicate = function == NULL || function->where_predicate_count != 1u
            || function->where_predicates == NULL
        ? NULL : &function->where_predicates[0];
    bound = predicate == NULL || predicate->bound_count != 1u
            || predicate->bounds == NULL
        ? NULL : &predicate->bounds[0];
    ok = result.error_count == 0u && function != NULL
        && predicate != NULL
        && ast_path_segments_are(&ast, predicate->subject, f_path, 1u)
        && bound != NULL && bound->kind == CM_AST_WHERE_BOUND_TRAIT
        && bound->modifier == CM_AST_WHERE_BOUND_REQUIRED
        && ast_path_segments_are(&ast, bound->trait_type,
            fn_once_path, 1u)
        && bound->binder.lifetime_count == 1u
        && bound->binder.lifetimes != NULL
        && ast_string_is(&ast, bound->binder.lifetimes[0], "'copy")
        && ast_span_is(source, bound->binder.span, "for<'copy>")
        && ast_span_is(source, bound->span,
            "for<'copy> FnOnce(VaList<'copy, 'f>) -> R")
        && ast_dump_contains(&ast,
            "(where-bound required for<\"'copy\"> path(\"FnOnce\"");
    if (!ok) {
        fprintf(stderr, "higher-ranked where bound was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_higher_ranked_where_predicate(void)
{
    static const char source[] =
        "fn process<F, U>() where "
        "for<'a> F: FnMut(GenericShunt<'a>) -> U {}";
    static const char *const f_path[] = { "F" };
    static const char *const fn_mut_path[] = { "FnMut" };
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstWherePredicate *predicate;
    const CmAstWhereBound *bound;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    predicate = function == NULL || function->where_predicate_count != 1u
            || function->where_predicates == NULL
        ? NULL : &function->where_predicates[0];
    bound = predicate == NULL || predicate->bound_count != 1u
            || predicate->bounds == NULL
        ? NULL : &predicate->bounds[0];
    ok = result.error_count == 0u && predicate != NULL
        && predicate->kind == CM_AST_WHERE_PREDICATE_TYPE
        && ast_path_segments_are(&ast, predicate->subject, f_path, 1u)
        && predicate->binder.lifetime_count == 1u
        && predicate->binder.lifetimes != NULL
        && ast_string_is(&ast, predicate->binder.lifetimes[0], "'a")
        && ast_span_is(source, predicate->binder.span, "for<'a>")
        && ast_span_is(source, predicate->span,
            "for<'a> F: FnMut(GenericShunt<'a>) -> U")
        && bound != NULL && bound->kind == CM_AST_WHERE_BOUND_TRAIT
        && bound->binder.lifetime_count == 0u
        && bound->binder.lifetimes == NULL
        && ast_path_segments_are(&ast, bound->trait_type,
            fn_mut_path, 1u)
        && ast_dump_contains(&ast,
            "(where-predicate for<\"'a\"> path(\"F\")");
    if (!ok) {
        fprintf(stderr, "higher-ranked where predicate was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_where_predicate_error_paths(void)
{
    static const struct {
        const char *source;
        const char *message;
    } broken[] = {
        { "fn f() where;", "expected predicate" },
        { "fn f() where Self;", "expected ':'" },
        { "fn f() where Self:;", "expected trait bound" },
        { "fn f() where Self: + Sized;", "expected trait path" },
        { "fn f<'a>() where 'a: Sized;", "require lifetime bounds" },
        { "fn f<'a>() where Self: ?'a;", "cannot have modifiers" },
        { "fn f<'a>() where 'a: ~const 'static;",
            "cannot have modifiers" },
        { "fn f<'a>() where 'a: const 'static;",
            "cannot have modifiers" },
        { "fn f() where Self: const;", "expected trait path" },
        { "fn f() where Self: use<T>;", "use where bound" },
        { "fn f() where Self: Sized +;", "expected trait bound" },
        { "fn f() where Self: Sized Self: Copy;", "expected ','" },
        { "fn f<F>() where F: FnMut(F) ->;", "expected return type" },
        { "type A = u8 where;", "expected predicate" },
        { "type A = u8 where Self;", "expected ':'" },
        { "type A = u8 where Self:;", "expected trait bound" },
        { "type A = u8 where Self: Sized Self: Copy;", "expected ','" },
        { "type A = u8 where Self: Sized +;", "expected trait bound" }
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index].source,
            strlen(broken[index].source), CM_EDITION_2024);
        if (result.error_count == 0u
            || strstr(result.first_error.message, broken[index].message)
                == NULL) {
            fprintf(stderr, "invalid where predicate %lu reported '%s'\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_lifetime_where_predicates(void)
{
    static const char source[] =
        "fn contract_like<'a, 'b, Ret, C>() where "
        "C: Fn(&Ret) -> bool + Copy + 'static, "
        "'a: 'b + 'static, {}";
    static const char *const c_path[] = { "C" };
    static const char *const fn_path[] = { "Fn" };
    static const char *const copy_path[] = { "Copy" };
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstWherePredicate *type_predicate;
    const CmAstWherePredicate *lifetime_predicate;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    type_predicate = item == NULL || item->where_predicate_count != 2u
        ? NULL : &item->where_predicates[0];
    lifetime_predicate = type_predicate == NULL
        ? NULL : &item->where_predicates[1];
    ok = result.error_count == 0u
        && item != NULL && item->kind == CM_AST_ITEM_FUNCTION
        && ast_string_is(&ast, item->where_clause,
            "C: Fn(&Ret) -> bool + Copy + 'static, "
            "'a: 'b + 'static,")
        && type_predicate != NULL
        && type_predicate->kind == CM_AST_WHERE_PREDICATE_TYPE
        && type_predicate->subject_lifetime == CM_INTERN_ID_NONE
        && ast_path_segments_are(&ast, type_predicate->subject,
            c_path, 1u)
        && type_predicate->bound_count == 3u
        && type_predicate->bounds != NULL
        && type_predicate->bounds[0].kind == CM_AST_WHERE_BOUND_TRAIT
        && ast_path_segments_are(&ast,
            type_predicate->bounds[0].trait_type, fn_path, 1u)
        && ast_span_is(source, type_predicate->bounds[0].span,
            "Fn(&Ret) -> bool")
        && type_predicate->bounds[1].kind == CM_AST_WHERE_BOUND_TRAIT
        && ast_path_segments_are(&ast,
            type_predicate->bounds[1].trait_type, copy_path, 1u)
        && type_predicate->bounds[2].kind == CM_AST_WHERE_BOUND_LIFETIME
        && type_predicate->bounds[2].trait_type == CM_AST_TYPE_NONE
        && ast_string_is(&ast, type_predicate->bounds[2].lifetime,
            "'static")
        && ast_span_is(source, type_predicate->bounds[2].span, "'static")
        && lifetime_predicate != NULL
        && lifetime_predicate->kind == CM_AST_WHERE_PREDICATE_LIFETIME
        && lifetime_predicate->subject == CM_AST_TYPE_NONE
        && ast_string_is(&ast, lifetime_predicate->subject_lifetime, "'a")
        && ast_span_is(source, lifetime_predicate->span,
            "'a: 'b + 'static")
        && lifetime_predicate->bound_count == 2u
        && lifetime_predicate->bounds != NULL
        && lifetime_predicate->bounds[0].kind
            == CM_AST_WHERE_BOUND_LIFETIME
        && ast_string_is(&ast, lifetime_predicate->bounds[0].lifetime,
            "'b")
        && lifetime_predicate->bounds[1].kind
            == CM_AST_WHERE_BOUND_LIFETIME
        && ast_string_is(&ast, lifetime_predicate->bounds[1].lifetime,
            "'static");
    if (!ok) {
        fprintf(stderr, "lifetime where predicates were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_supertrait_error_paths(void)
{
    static const char *const broken[] = {
        "trait T: {}",
        "trait T: + A {}",
        "trait T: A + {}",
        "trait T: ?A {}",
        "trait T: ~const 'static {}",
        "trait T: for<'a> A {}",
        "trait T: ~A {}",
        "trait T: ~const {}"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u || result.first_error.message[0] == 0) {
            fprintf(stderr,
                "invalid supertrait list %lu did not produce an error\n",
                (unsigned long)index);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_structured_associated_type_bounds(void)
{
    static const char source[] =
        "trait T { type Item; type IntoIter: Iterator<Item = Self::Item>; "
        "type Target: ?Sized; type Ordered: First + Second; }";
    static const char *const first_path[] = { "First" };
    static const char *const second_path[] = { "Second" };
    const CmAstItemId *root_id;
    const CmAstItem *trait_item;
    const CmAstItem *into_iter;
    const CmAstItem *target;
    const CmAstItem *ordered;
    const CmAstAssociatedTypeBound *bound;
    const CmAstType *bound_type;
    const CmAstPath *bound_path;
    const CmAstGenericArg *binding;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    trait_item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    into_iter = trait_item == NULL
        || trait_item->data.trait_item.item_count < 2u ? NULL
        : cm_ast_get_item(&ast, trait_item->data.trait_item.items[1]);
    target = trait_item == NULL
        || trait_item->data.trait_item.item_count < 3u ? NULL
        : cm_ast_get_item(&ast, trait_item->data.trait_item.items[2]);
    ordered = trait_item == NULL
        || trait_item->data.trait_item.item_count < 4u ? NULL
        : cm_ast_get_item(&ast, trait_item->data.trait_item.items[3]);
    bound = into_iter == NULL || into_iter->data.value_item.bound_count != 1u
        ? NULL : &into_iter->data.value_item.bounds[0];
    bound_type = bound == NULL ? NULL
        : cm_ast_get_type(&ast, bound->trait_type);
    bound_path = bound_type == NULL ? NULL
        : cm_ast_get_path(&ast, bound_type->path);
    binding = bound_path == NULL || bound_path->segment_count != 1u
        || bound_path->segments == NULL
        || bound_path->segments[0].argument_count != 1u
        ? NULL : &bound_path->segments[0].arguments[0];
    ok = result.error_count == 0u
        && trait_item != NULL && trait_item->kind == CM_AST_ITEM_TRAIT
        && into_iter != NULL && into_iter->kind == CM_AST_ITEM_TYPE_ALIAS
        && bound != NULL
        && bound->modifier == CM_AST_ASSOC_BOUND_REQUIRED
        && ast_span_is(source, bound->span,
            "Iterator<Item = Self::Item>")
        && binding != NULL && binding->kind == CM_AST_GENERIC_BINDING
        && ast_string_is(&ast, binding->name, "Item")
        && ast_span_is(source, binding->span, "Item = Self::Item")
        && target != NULL && target->kind == CM_AST_ITEM_TYPE_ALIAS
        && target->data.value_item.bound_count == 1u
        && target->data.value_item.bounds != NULL
        && target->data.value_item.bounds[0].modifier
            == CM_AST_ASSOC_BOUND_RELAXED
        && ast_span_is(source, target->data.value_item.bounds[0].span,
            "?Sized")
        && ordered != NULL && ordered->kind == CM_AST_ITEM_TYPE_ALIAS
        && ordered->data.value_item.bound_count == 2u
        && ordered->data.value_item.bounds != NULL
        && ordered->data.value_item.bounds[0].modifier
            == CM_AST_ASSOC_BOUND_REQUIRED
        && ordered->data.value_item.bounds[1].modifier
            == CM_AST_ASSOC_BOUND_REQUIRED
        && ast_path_segments_are(&ast,
            ordered->data.value_item.bounds[0].trait_type,
            first_path, 1u)
        && ast_path_segments_are(&ast,
            ordered->data.value_item.bounds[1].trait_type,
            second_path, 1u)
        && ast_span_is(source, ordered->data.value_item.bounds[0].span,
            "First")
        && ast_span_is(source, ordered->data.value_item.bounds[1].span,
            "Second");
    if (!ok) {
        fprintf(stderr,
            "structured associated-type bounds were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_associated_type_bound_error_paths(void)
{
    static const struct {
        const char *source;
        const char *message;
    } broken[] = {
        { "trait T { type A:; }", "expected associated-type bound" },
        { "trait T { type A: + Sized; }", "expected associated-type bound" },
        { "trait T { type A: Sized +; }", "expected associated-type bound" },
        { "trait T { type A: for<'a> Trait; }", "HRTB" },
        { "trait T { type A: ?'static; }", "cannot have modifiers" },
        { "trait T { type A: ~const Trait; }", "~const" },
        { "trait T { type A: use<T>; }", "use" },
        { "trait T { type A: ?Trait; }", "only ?Sized" },
        { "trait T { type A: ?Sized<Item = u8>; }", "only ?Sized" }
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        CmAst ast;
        CmParseResult result;

        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index].source,
            strlen(broken[index].source), CM_EDITION_2024);
        if (result.error_count == 0u
            || strstr(result.first_error.message, broken[index].message)
                == NULL) {
            fprintf(stderr,
                "invalid associated-type bound %lu reported '%s'\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_block_local_const_item(void)
{
    static const char source[] =
        "pub const fn to_degrees(self) -> Self {\n"
        "    #[rustfmt::skip]\n"
        "    const PIS_IN_180: f128 =\n"
        "        57.2957795130823208767981548141051703324054724665643215491602_f128;\n"
        "    self * PIS_IN_180\n"
        "}";
    static const char literal_text[] =
        "57.2957795130823208767981548141051703324054724665643215491602_f128";
    static const char local_text[] =
        "#[rustfmt::skip]\n"
        "    const PIS_IN_180: f128 =\n"
        "        57.2957795130823208767981548141051703324054724665643215491602_f128;";
    static const char local_item_text[] =
        "const PIS_IN_180: f128 =\n"
        "        57.2957795130823208767981548141051703324054724665643215491602_f128;";
    static const char *const f128_path[] = { "f128" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstItem *local;
    const CmAstExpr *initializer;
    const CmAstAttribute *attribute;
    const CmAstExpr *tail;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->data.block.statement_count != 1u
        || body->data.block.statements == NULL ? NULL
        : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    local = statement == NULL || statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast, statement->data.item_stmt.item);
    initializer = local == NULL ? NULL : cm_ast_get_expr(&ast,
        local->data.value_item.initializer);
    attribute = local == NULL || local->attribute_count != 1u
        || local->attributes == NULL ? NULL
        : cm_ast_get_attribute(&ast, local->attributes[0]);
    tail = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    ok = result.error_count == 0u
        && ast.root_items.len == 1u && ast.items.len == 2u
        && function != NULL && function->kind == CM_AST_ITEM_FUNCTION
        && function->data.function_item.is_const
        && ast_string_is(&ast, function->name, "to_degrees")
        && body != NULL && body->kind == CM_AST_EXPR_BLOCK
        && !body->data.block.is_unsafe && !body->data.block.is_const
        && statement != NULL && local != NULL
        && local->kind == CM_AST_ITEM_CONST
        && ast_string_is(&ast, local->name, "PIS_IN_180")
        && attribute != NULL && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, "#[rustfmt::skip]")
        && ast_span_is(source, attribute->span, "#[rustfmt::skip]")
        && ast_span_is(source, statement->span, local_text)
        && ast_span_is(source, local->span, local_item_text)
        && ast_path_segments_are(&ast, local->data.value_item.type,
            f128_path, 1u)
        && local->data.value_item.has_value
        && initializer != NULL && initializer->kind == CM_AST_EXPR_LITERAL
        && ast_string_is(&ast, initializer->data.literal.text, literal_text)
        && ast_span_is(source, initializer->span, literal_text)
        && tail != NULL && tail->kind == CM_AST_EXPR_BINARY
        && ast_string_is(&ast, tail->data.binary.operator_name, "*")
        && ast_expression_path_is(&ast, tail->data.binary.left, "self")
        && ast_expression_path_is(&ast, tail->data.binary.right,
            "PIS_IN_180")
        && ast_span_is(source, tail->span, "self * PIS_IN_180");
    if (!ok) {
        fprintf(stderr, "block-local f128 const AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_local_static_item(void)
{
    static const char source[] =
        "fn lower_t(x: usize) -> u8 {\n"
        "    static SBOX: [u8; 2] = [0xd6, 0x90];\n"
        "    SBOX[x]\n"
        "}";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstItem *local;
    const CmAstExpr *initializer;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->kind != CM_AST_EXPR_BLOCK
            || body->data.block.statement_count != 1u
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    local = statement == NULL || statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast, statement->data.item_stmt.item);
    initializer = local == NULL ? NULL : cm_ast_get_expr(&ast,
        local->data.value_item.initializer);
    ok = result.error_count == 0u && ast.root_items.len == 1u
        && ast.items.len == 2u && function != NULL
        && function->kind == CM_AST_ITEM_FUNCTION
        && local != NULL && local->kind == CM_AST_ITEM_STATIC
        && ast_string_is(&ast, local->name, "SBOX")
        && local->data.value_item.has_value
        && initializer != NULL && initializer->kind == CM_AST_EXPR_ARRAY
        && initializer->data.list.element_count == 2u
        && ast_span_is(source, statement->span,
            "static SBOX: [u8; 2] = [0xd6, 0x90];");
    if (!ok) {
        fprintf(stderr, "block-local static AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_local_function_item(void)
{
    static const char source[] =
        "fn outer(value: u32) -> u32 {\n"
        "    #[inline]\n"
        "    fn inner(value: u32) -> u32 { value }\n"
        "    inner(value)\n"
        "}";
    static const char statement_text[] =
        "#[inline]\n"
        "    fn inner(value: u32) -> u32 { value }";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *outer;
    const CmAstExpr *outer_body;
    const CmAstStmt *statement;
    const CmAstItem *inner;
    const CmAstAttribute *attribute;
    const CmAstExpr *inner_body;
    const CmAstExpr *inner_tail;
    const CmAstExpr *outer_tail;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    outer = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    outer_body = outer == NULL ? NULL : cm_ast_get_expr(&ast,
        outer->data.function_item.body);
    statement = outer_body == NULL
            || outer_body->data.block.statement_count != 1u
        ? NULL : cm_ast_get_stmt(&ast,
            outer_body->data.block.statements[0]);
    inner = statement == NULL || statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast, statement->data.item_stmt.item);
    attribute = inner == NULL || inner->attribute_count != 1u
        || inner->attributes == NULL ? NULL
        : cm_ast_get_attribute(&ast, inner->attributes[0]);
    inner_body = inner == NULL ? NULL : cm_ast_get_expr(&ast,
        inner->data.function_item.body);
    inner_tail = inner_body == NULL ? NULL : cm_ast_get_expr(&ast,
        inner_body->data.block.tail);
    outer_tail = outer_body == NULL ? NULL : cm_ast_get_expr(&ast,
        outer_body->data.block.tail);
    ok = result.error_count == 0u
        && ast.root_items.len == 1u && ast.items.len == 2u
        && outer != NULL && outer->kind == CM_AST_ITEM_FUNCTION
        && statement != NULL && inner != NULL
        && inner->kind == CM_AST_ITEM_FUNCTION
        && ast_string_is(&ast, inner->name, "inner")
        && ast_span_is(source, statement->span, statement_text)
        && ast_span_is(source, inner->span,
            "fn inner(value: u32) -> u32 { value }")
        && attribute != NULL && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, "#[inline]")
        && inner_body != NULL && inner_body->kind == CM_AST_EXPR_BLOCK
        && inner_tail != NULL && inner_tail->kind == CM_AST_EXPR_PATH
        && ast_expression_path_is(&ast, inner_body->data.block.tail,
            "value")
        && outer_tail != NULL && outer_tail->kind == CM_AST_EXPR_CALL
        && ast_expression_path_is(&ast, outer_tail->data.call.callee,
            "inner");
    if (!ok) {
        fprintf(stderr, "block-local function AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_local_trait_item(void)
{
    static const char source[] =
        "fn advance_by() {\n"
        "    trait SpecAdvanceBy {\n"
        "        fn spec_advance_by(&mut self, n: usize) "
        "-> Result<(), NonZero<usize>>;\n"
        "    }\n"
        "}";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstItem *local_trait;
    const CmAstItem *method;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->kind != CM_AST_EXPR_BLOCK
            || body->data.block.statement_count != 1u
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    local_trait = statement == NULL || statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast, statement->data.item_stmt.item);
    method = local_trait == NULL || local_trait->kind != CM_AST_ITEM_TRAIT
            || local_trait->data.trait_item.item_count != 1u
            || local_trait->data.trait_item.items == NULL
        ? NULL : cm_ast_get_item(&ast,
            local_trait->data.trait_item.items[0]);
    ok = result.error_count == 0u
        && ast.root_items.len == 1u && ast.items.len == 3u
        && function != NULL && function->kind == CM_AST_ITEM_FUNCTION
        && statement != NULL && local_trait != NULL
        && local_trait->kind == CM_AST_ITEM_TRAIT
        && ast_string_is(&ast, local_trait->name, "SpecAdvanceBy")
        && ast_span_is(source, local_trait->span,
            "trait SpecAdvanceBy {\n"
            "        fn spec_advance_by(&mut self, n: usize) "
            "-> Result<(), NonZero<usize>>;\n"
            "    }")
        && method != NULL && method->kind == CM_AST_ITEM_FUNCTION
        && ast_string_is(&ast, method->name, "spec_advance_by")
        && method->data.function_item.parameter_count == 2u
        && method->data.function_item.body == CM_AST_EXPR_NONE
        && ast_span_is(source, method->span,
            "fn spec_advance_by(&mut self, n: usize) "
            "-> Result<(), NonZero<usize>>;");
    if (!ok) {
        fprintf(stderr, "block-local trait AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_local_use_item(void)
{
    static const char source[] =
        "fn debug() {\n"
        "    use AsciiChar::{Apostrophe, ReverseSolidus as Backslash};\n"
        "    Backslash\n"
        "}";
    static const char tree[] =
        "AsciiChar::{Apostrophe, ReverseSolidus as Backslash}";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstItem *local;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->kind != CM_AST_EXPR_BLOCK
            || body->data.block.statement_count != 1u
            || body->data.block.statements == NULL
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    local = statement == NULL || statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast, statement->data.item_stmt.item);
    ok = result.error_count == 0u
        && ast.root_items.len == 1u && ast.items.len == 2u
        && function != NULL && function->kind == CM_AST_ITEM_FUNCTION
        && statement != NULL && local != NULL
        && local->kind == CM_AST_ITEM_USE
        && ast_string_is(&ast, local->data.use_item.tree, tree)
        && ast_span_is(source, statement->span,
            "use AsciiChar::{Apostrophe, ReverseSolidus as Backslash};")
        && ast_span_is(source, local->span,
            "use AsciiChar::{Apostrophe, ReverseSolidus as Backslash};")
        && body->data.block.tail != CM_AST_EXPR_NONE
        && ast_expression_path_is(&ast, body->data.block.tail, "Backslash")
        && ast_dump_contains(&ast,
            "(tree \"AsciiChar::{Apostrophe, ReverseSolidus as "
            "Backslash}\")");
    if (!ok) {
        fprintf(stderr, "block-local use AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_local_struct_and_impl_items(void)
{
    static const char source[] =
        "fn initialize<T, F>() {\n"
        "    struct PoisonOnPanic<T, F>(*mut State<T, F>);\n"
        "    impl<T, F> Drop for PoisonOnPanic<T, F> {}\n"
        "}";
    static const char *const drop_path[] = { "Drop" };
    static const char *const poison_path[] = { "PoisonOnPanic" };
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *struct_statement;
    const CmAstStmt *impl_statement;
    const CmAstItem *local_struct;
    const CmAstItem *local_impl;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    struct_statement = body == NULL || body->kind != CM_AST_EXPR_BLOCK
            || body->data.block.statement_count != 2u
            || body->data.block.statements == NULL
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    impl_statement = body == NULL || body->kind != CM_AST_EXPR_BLOCK
            || body->data.block.statement_count != 2u
            || body->data.block.statements == NULL
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[1]);
    local_struct = struct_statement == NULL
            || struct_statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast,
            struct_statement->data.item_stmt.item);
    local_impl = impl_statement == NULL
            || impl_statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast,
            impl_statement->data.item_stmt.item);
    ok = result.error_count == 0u
        && ast.root_items.len == 1u && ast.items.len == 3u
        && local_struct != NULL && local_struct->kind == CM_AST_ITEM_STRUCT
        && ast_string_is(&ast, local_struct->name, "PoisonOnPanic")
        && local_struct->generic_parameter_count == 2u
        && local_struct->data.aggregate_item.form == CM_AST_FIELDS_TUPLE
        && local_struct->data.aggregate_item.field_count == 1u
        && ast_span_is(source, local_struct->span,
            "struct PoisonOnPanic<T, F>(*mut State<T, F>);")
        && local_impl != NULL && local_impl->kind == CM_AST_ITEM_IMPL
        && local_impl->generic_parameter_count == 2u
        && ast_path_segments_are(&ast,
            local_impl->data.impl_item.trait_type, drop_path, 1u)
        && ast_path_segments_are(&ast,
            local_impl->data.impl_item.self_type, poison_path, 1u)
        && ast_span_is(source, local_impl->span,
            "impl<T, F> Drop for PoisonOnPanic<T, F> {}")
        && ast_dump_contains(&ast,
            "trait=path(\"Drop\") self=path(\"PoisonOnPanic\"");
    if (!ok) {
        fprintf(stderr,
            "block-local struct/impl AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_local_unsafe_extern_item(void)
{
    static const char source[] =
        "fn promise(ptr: *const (), align: usize) {\n"
        "    unsafe extern \"Rust\" {\n"
        "        #[link_name = \"promise_alignment\"]\n"
        "        fn promise_alignment(ptr: *const (), align: usize);\n"
        "    }\n"
        "    unsafe {}\n"
        "}";
    static const char item_text[] =
        "unsafe extern \"Rust\" {\n"
        "        #[link_name = \"promise_alignment\"]\n"
        "        fn promise_alignment(ptr: *const (), align: usize);\n"
        "    }";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *outer;
    const CmAstExpr *body;
    const CmAstStmt *foreign_statement;
    const CmAstItem *foreign_block;
    const CmAstItem *foreign_function;
    const CmAstExpr *unsafe_block;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    outer = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = outer == NULL ? NULL : cm_ast_get_expr(&ast,
        outer->data.function_item.body);
    foreign_statement = body == NULL
            || body->data.block.statement_count != 1u
            || body->data.block.statements == NULL
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    foreign_block = foreign_statement == NULL
            || foreign_statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast,
            foreign_statement->data.item_stmt.item);
    foreign_function = foreign_block == NULL
            || foreign_block->kind != CM_AST_ITEM_EXTERN_BLOCK
            || foreign_block->data.extern_block_item.item_count != 1u
            || foreign_block->data.extern_block_item.items == NULL
        ? NULL : cm_ast_get_item(&ast,
            foreign_block->data.extern_block_item.items[0]);
    unsafe_block = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    ok = result.error_count == 0u && ast.root_items.len == 1u
        && ast.items.len == 3u
        && outer != NULL && outer->kind == CM_AST_ITEM_FUNCTION
        && body != NULL && body->kind == CM_AST_EXPR_BLOCK
        && foreign_statement != NULL && foreign_block != NULL
        && ast_span_is(source, foreign_statement->span, item_text)
        && ast_span_is(source, foreign_block->span, item_text)
        && foreign_block->data.extern_block_item.is_unsafe
        && ast_string_is(&ast, foreign_block->data.extern_block_item.abi,
            "Rust")
        && foreign_function != NULL
        && foreign_function->kind == CM_AST_ITEM_FUNCTION
        && foreign_function->attribute_count == 1u
        && ast_string_is(&ast, foreign_function->name,
            "promise_alignment")
        && foreign_function->data.function_item.body == CM_AST_EXPR_NONE
        && unsafe_block != NULL && unsafe_block->kind == CM_AST_EXPR_BLOCK
        && unsafe_block->data.block.is_unsafe
        && !unsafe_block->data.block.is_const
        && ast_dump_contains(&ast,
            "item-stmt(\n  (extern-block inherited _");
    if (!ok) {
        fprintf(stderr, "block-local unsafe extern AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_local_union_item(void)
{
    static const char source[] =
        "fn transmute<Src, Dst>() -> u8 {\n"
        "    #[repr(C)]\n"
        "    union Transmute<Src, Dst> {\n"
        "        src: ManuallyDrop<Src>,\n"
        "        dst: ManuallyDrop<Dst>,\n"
        "    }\n"
        "    0u8\n"
        "}";
    static const char statement_text[] =
        "#[repr(C)]\n"
        "    union Transmute<Src, Dst> {\n"
        "        src: ManuallyDrop<Src>,\n"
        "        dst: ManuallyDrop<Dst>,\n"
        "    }";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstItem *local;
    const CmAstField *src;
    const CmAstField *dst;
    const CmAstExpr *tail;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->data.block.statement_count != 1u
            || body->data.block.statements == NULL
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    local = statement == NULL || statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast, statement->data.item_stmt.item);
    src = local == NULL || local->kind != CM_AST_ITEM_UNION
            || local->data.aggregate_item.field_count != 2u
            || local->data.aggregate_item.fields == NULL
        ? NULL : &local->data.aggregate_item.fields[0];
    dst = src == NULL ? NULL : &local->data.aggregate_item.fields[1];
    tail = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    ok = result.error_count == 0u && ast.root_items.len == 1u
        && ast.items.len == 2u
        && function != NULL && function->kind == CM_AST_ITEM_FUNCTION
        && body != NULL && body->kind == CM_AST_EXPR_BLOCK
        && statement != NULL && local != NULL
        && ast_span_is(source, statement->span, statement_text)
        && local->kind == CM_AST_ITEM_UNION
        && ast_string_is(&ast, local->name, "Transmute")
        && local->attribute_count == 1u
        && local->generic_parameter_count == 2u
        && local->data.aggregate_item.form == CM_AST_FIELDS_NAMED
        && src != NULL && ast_string_is(&ast, src->name, "src")
        && dst != NULL && ast_string_is(&ast, dst->name, "dst")
        && tail != NULL && tail->kind == CM_AST_EXPR_LITERAL
        && ast_string_is(&ast, tail->data.literal.text, "0u8")
        && ast_dump_contains(&ast,
            "item-stmt(\n  (union inherited \"Transmute\"");
    if (!ok) {
        fprintf(stderr, "block-local union AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_local_macro_rules_item(void)
{
    static const char source[] =
        "fn main() -> u8 {\n"
        "    macro_rules! first { ($value:expr) => { $value }; }\n"
        "    first!(1u8)\n"
        "}";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstItem *local;
    const CmAstExpr *tail;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->data.block.statement_count != 1u
            || body->data.block.statements == NULL
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    local = statement == NULL || statement->kind != CM_AST_STMT_ITEM
        ? NULL : cm_ast_get_item(&ast, statement->data.item_stmt.item);
    tail = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    ok = result.error_count == 0u && ast.root_items.len == 1u
        && ast.items.len == 2u
        && local != NULL && local->kind == CM_AST_ITEM_MACRO
        && local->data.macro_item.form == CM_AST_MACRO_RULES_DEFINITION
        && ast_string_is(&ast, local->name, "first")
        && local->data.macro_item.delimiter == CM_AST_DELIMITER_BRACE
        && tail != NULL && tail->kind == CM_AST_EXPR_MACRO
        && ast_dump_contains(&ast,
            "item-stmt(\n  (macro inherited \"first\"");
    if (!ok) {
        fprintf(stderr, "block-local macro_rules AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_const_block_discrimination(void)
{
    static const char source[] =
        "fn f() -> i32 { const { 1i32 } } "
        "fn g() -> u32 { const r#type: u32 = 1u32; r#type }";
    static const char malformed[] =
        "fn broken() { const LOCAL u32 = 1u32; }";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *first_id;
    const CmAstItemId *second_id;
    const CmAstItem *first;
    const CmAstItem *second;
    const CmAstExpr *first_body;
    const CmAstExpr *const_block;
    const CmAstExpr *second_body;
    const CmAstStmt *raw_statement;
    const CmAstItem *raw_local;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    first_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    second_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    first = first_id == NULL ? NULL : cm_ast_get_item(&ast, *first_id);
    second = second_id == NULL ? NULL : cm_ast_get_item(&ast, *second_id);
    first_body = first == NULL ? NULL : cm_ast_get_expr(&ast,
        first->data.function_item.body);
    const_block = first_body == NULL ? NULL : cm_ast_get_expr(&ast,
        first_body->data.block.tail);
    second_body = second == NULL ? NULL : cm_ast_get_expr(&ast,
        second->data.function_item.body);
    raw_statement = second_body == NULL
        || second_body->data.block.statement_count != 1u
        || second_body->data.block.statements == NULL ? NULL
        : cm_ast_get_stmt(&ast, second_body->data.block.statements[0]);
    raw_local = raw_statement == NULL
        || raw_statement->kind != CM_AST_STMT_ITEM ? NULL
        : cm_ast_get_item(&ast, raw_statement->data.item_stmt.item);
    ok = result.error_count == 0u && ast.root_items.len == 2u
        && ast.items.len == 3u
        && first_body != NULL && first_body->kind == CM_AST_EXPR_BLOCK
        && first_body->data.block.statement_count == 0u
        && const_block != NULL && const_block->kind == CM_AST_EXPR_BLOCK
        && const_block->data.block.is_const
        && !const_block->data.block.is_unsafe
        && const_block->data.block.statement_count == 0u
        && ast_span_is(source, const_block->span, "const { 1i32 }")
        && raw_local != NULL && raw_local->kind == CM_AST_ITEM_CONST
        && ast_string_is(&ast, raw_local->name, "type")
        && ast_expression_path_is(&ast, second_body->data.block.tail,
            "type");
    if (!ok) {
        fprintf(stderr, "const block/local raw const split was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, malformed, sizeof(malformed) - 1u,
        CM_EDITION_2024);
    ok = ok && result.error_count != 0u
        && strstr(result.first_error.message,
            "expected ':' after value name") != NULL;
    if (result.error_count == 0u) {
        fputs("malformed block-local const was accepted\n", stderr);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_attributed_tail_expression(void)
{
    static const char source[] =
        "fn fmt(self, f: F) -> R {\n"
        "    #[allow(deprecated)]\n"
        "    self.description().fmt(f)\n"
        "}";
    static const char inner_source[] =
        "fn value() -> u32 { #![allow(deprecated)] 1u32 }";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *tail;
    const CmAstExpr *receiver;
    const CmAstAttribute *attribute;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    tail = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    receiver = tail == NULL || tail->kind != CM_AST_EXPR_METHOD_CALL
        ? NULL : cm_ast_get_expr(&ast, tail->data.method_call.receiver);
    attribute = tail == NULL || tail->attribute_count != 1u
        || tail->attributes == NULL ? NULL
        : cm_ast_get_attribute(&ast, tail->attributes[0]);
    ok = result.error_count == 0u && ast.attributes.len == 1u
        && body != NULL && body->kind == CM_AST_EXPR_BLOCK
        && body->data.block.statement_count == 0u
        && tail != NULL && tail->kind == CM_AST_EXPR_METHOD_CALL
        && ast_string_is(&ast, tail->data.method_call.name, "fmt")
        && tail->data.method_call.argument_count == 1u
        && ast_expression_path_is(&ast, tail->data.method_call.arguments[0],
            "f")
        && receiver != NULL && receiver->kind == CM_AST_EXPR_METHOD_CALL
        && ast_string_is(&ast, receiver->data.method_call.name,
            "description")
        && receiver->attribute_count == 0u
        && attribute != NULL && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, "#[allow(deprecated)]")
        && ast_span_is(source, attribute->span, "#[allow(deprecated)]")
        && ast_span_is(source, tail->span, "self.description().fmt(f)")
        && ast_dump_contains(&ast,
            "attributed(attributes(outer=\"#[allow(deprecated)]\"), "
            "method(");
    if (!ok) {
        fprintf(stderr, "attributed tail expression AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, inner_source, sizeof(inner_source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    tail = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    attribute = body == NULL || body->data.block.inner_attribute_count != 1u
            || body->data.block.inner_attributes == NULL
        ? NULL : cm_ast_get_attribute(&ast,
            body->data.block.inner_attributes[0]);
    ok = ok && result.error_count == 0u
        && ast.attributes.len == 1u && attribute != NULL
        && body != NULL && body->kind == CM_AST_EXPR_BLOCK
        && body->attribute_count == 0u
        && body->data.block.statement_count == 0u
        && tail != NULL && tail->kind == CM_AST_EXPR_LITERAL
        && attribute->style == CM_AST_ATTR_INNER
        && ast_string_is(&ast, attribute->text, "#![allow(deprecated)]")
        && ast_span_is(inner_source, attribute->span,
            "#![allow(deprecated)]")
        && ast_dump_contains(&ast,
            "block(attributes(inner=\"#![allow(deprecated)]\"), "
            "tail=literal(\"1u32\"))");
    if (!ok) {
        fprintf(stderr, "inner block attribute AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_attributed_call_argument(void)
{
    static const char source[] =
        "fn count<I>(iter: I) -> usize { iter.fold(\n"
        "    0,\n"
        "    #[rustc_inherit_overflow_checks]\n"
        "    |count, _| count + 1,\n"
        ") }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *method;
    const CmAstExpr *closure;
    const CmAstExpr *closure_body;
    const CmAstAttribute *attribute;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    method = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    closure = method == NULL || method->kind != CM_AST_EXPR_METHOD_CALL
            || method->data.method_call.argument_count != 2u
        ? NULL : cm_ast_get_expr(&ast,
            method->data.method_call.arguments[1]);
    closure_body = closure == NULL || closure->kind != CM_AST_EXPR_CLOSURE
        ? NULL : cm_ast_get_expr(&ast, closure->data.closure.body);
    attribute = closure == NULL || closure->attribute_count != 1u
            || closure->attributes == NULL
        ? NULL : cm_ast_get_attribute(&ast, closure->attributes[0]);
    ok = result.error_count == 0u && ast.attributes.len == 1u
        && body != NULL && body->kind == CM_AST_EXPR_BLOCK
        && method != NULL && method->kind == CM_AST_EXPR_METHOD_CALL
        && ast_string_is(&ast, method->data.method_call.name, "fold")
        && closure != NULL && closure->kind == CM_AST_EXPR_CLOSURE
        && closure->data.closure.parameter_count == 2u
        && closure_body != NULL && closure_body->kind == CM_AST_EXPR_BINARY
        && attribute != NULL && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text,
            "#[rustc_inherit_overflow_checks]")
        && ast_span_is(source, attribute->span,
            "#[rustc_inherit_overflow_checks]")
        && ast_span_is(source, closure->span, "|count, _| count + 1")
        && ast_dump_contains(&ast,
            "attributed(attributes(outer=\"#[rustc_inherit_overflow_checks]\"), "
            "closure(");
    if (!ok) {
        fprintf(stderr, "attributed call argument AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_attributed_let_initializer(void)
{
    static const char source[] =
        "fn scan() { let check = #[cold] "
        "|mask: u16| -> bool { mask != 0 }; check }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstExpr *initializer;
    const CmAstExpr *closure_body;
    const CmAstAttribute *attribute;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->kind != CM_AST_EXPR_BLOCK
            || body->data.block.statement_count != 1u
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    initializer = statement == NULL || statement->kind != CM_AST_STMT_LET
        ? NULL : cm_ast_get_expr(&ast,
            statement->data.let_stmt.initializer);
    closure_body = initializer == NULL
            || initializer->kind != CM_AST_EXPR_CLOSURE
        ? NULL : cm_ast_get_expr(&ast, initializer->data.closure.body);
    attribute = initializer == NULL || initializer->attribute_count != 1u
            || initializer->attributes == NULL
        ? NULL : cm_ast_get_attribute(&ast, initializer->attributes[0]);
    ok = result.error_count == 0u && statement != NULL
        && initializer != NULL && initializer->kind == CM_AST_EXPR_CLOSURE
        && initializer->data.closure.parameter_count == 1u
        && initializer->data.closure.return_type != CM_AST_TYPE_NONE
        && closure_body != NULL && closure_body->kind == CM_AST_EXPR_BLOCK
        && attribute != NULL && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, "#[cold]")
        && ast_span_is(source, attribute->span, "#[cold]")
        && ast_span_is(source, initializer->span,
            "|mask: u16| -> bool { mask != 0 }")
        && ast_dump_contains(&ast,
            "attributed(attributes(outer=\"#[cold]\"), closure(");
    if (!ok) {
        fprintf(stderr, "attributed let initializer was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_attributed_let_statement(void)
{
    static const char source[] =
        "fn shuffle(a: i8x32, b: i8x32) {\n"
        "    #[rustfmt::skip]\n"
        "    let r: i8x32 = simd_shuffle!(a, b, [8, 40, 9, 41]);\n"
        "    r\n"
        "}";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstExpr *initializer;
    const CmAstAttribute *attribute;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->kind != CM_AST_EXPR_BLOCK
            || body->data.block.statement_count != 1u
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    initializer = statement == NULL
            || statement->kind != CM_AST_STMT_LET
        ? NULL : cm_ast_get_expr(&ast,
            statement->data.let_stmt.initializer);
    attribute = statement == NULL || statement->attribute_count != 1u
            || statement->attributes == NULL
        ? NULL : cm_ast_get_attribute(&ast, statement->attributes[0]);
    ok = result.error_count == 0u && statement != NULL
        && statement->kind == CM_AST_STMT_LET
        && initializer != NULL && initializer->kind == CM_AST_EXPR_MACRO
        && attribute != NULL && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, "#[rustfmt::skip]")
        && ast_span_is(source, attribute->span, "#[rustfmt::skip]")
        && ast_span_is(source, statement->span,
            "#[rustfmt::skip]\n"
            "    let r: i8x32 = simd_shuffle!(a, b, [8, 40, 9, 41]);")
        && ast_dump_contains(&ast,
            "attributed-stmt(attributes(outer=\"#[rustfmt::skip]\"), "
            "let(bind(\"r\"): path(\"i8x32\") = "
            "macro(form=invocation, path=\"simd_shuffle\"");
    if (!ok) {
        fprintf(stderr, "attributed let statement AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_attributed_enum_variants(void)
{
    static const char source[] =
        "enum Choice {\n"
        "    #[cfg(target_pointer_width = \"64\")]\n"
        "    Wide(u64),\n"
        "    #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
        "    Narrow = 1,\n"
        "}";
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstVariant *wide;
    const CmAstVariant *narrow;
    const CmAstAttribute *wide_attribute;
    const CmAstAttribute *narrow_attribute;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    wide = item == NULL || item->kind != CM_AST_ITEM_ENUM
            || item->data.enum_item.variant_count != 2u
        ? NULL : &item->data.enum_item.variants[0];
    narrow = wide == NULL ? NULL : &item->data.enum_item.variants[1];
    wide_attribute = wide == NULL || wide->attribute_count != 1u
        ? NULL : cm_ast_get_attribute(&ast, wide->attributes[0]);
    narrow_attribute = narrow == NULL || narrow->attribute_count != 1u
        ? NULL : cm_ast_get_attribute(&ast, narrow->attributes[0]);
    ok = result.error_count == 0u && wide_attribute != NULL
        && narrow_attribute != NULL
        && ast_string_is(&ast, wide_attribute->text,
            "#[cfg(target_pointer_width = \"64\")]")
        && ast_string_is(&ast, narrow_attribute->text,
            "#[stable(feature = \"choice\", since = \"1.0.0\")]")
        && ast_span_is(source, wide->span,
            "#[cfg(target_pointer_width = \"64\")]\n    Wide(u64)")
        && ast_span_is(source, narrow->span,
            "#[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "    Narrow = 1")
        && ast_dump_contains(&ast,
            "(variant tuple \"Wide\"\n      (attribute outer "
            "\"#[cfg(target_pointer_width = \\\"64\\\")]\")");
    if (!ok) {
        fprintf(stderr, "attributed enum variant AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_attributed_match_arm(void)
{
    static const char source[] =
        "fn pick(v: u8) { match v { #[cfg(fast)] 0 => 1, _ => 2 } }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *match_expression;
    const CmAstMatchArm *arm;
    const CmAstAttribute *attribute;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    match_expression = body == NULL || body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, body->data.block.tail);
    arm = match_expression == NULL
            || match_expression->kind != CM_AST_EXPR_MATCH
            || match_expression->data.match_expr.arm_count != 2u
        ? NULL : &match_expression->data.match_expr.arms[0];
    attribute = arm == NULL || arm->attribute_count != 1u
            || arm->attributes == NULL
        ? NULL : cm_ast_get_attribute(&ast, arm->attributes[0]);
    ok = result.error_count == 0u && arm != NULL
        && attribute != NULL && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text, "#[cfg(fast)]")
        && ast_span_is(source, attribute->span, "#[cfg(fast)]")
        && cm_ast_get_pattern(&ast, arm->pattern) != NULL
        && cm_ast_get_pattern(&ast, arm->pattern)->kind
            == CM_AST_PATTERN_LITERAL
        && ast_dump_contains(&ast,
            "arm(attributes(outer=\"#[cfg(fast)]\"), literal(\"0\") =>");
    if (!ok) {
        fprintf(stderr, "attributed match arm was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_structured_match_let_guard(void)
{
    static const char source[] =
        "fn parse(s: S, negative: bool) -> R {\n"
        "    match parse_number(s) {\n"
        "        Some(r) => r,\n"
        "        None if let Some(value) = parse_inf_nan(s, negative) "
        "=> value,\n"
        "        None if negative => fallback,\n"
        "        None => other,\n"
        "    }\n"
        "}";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *match_expression;
    const CmAstMatchArm *let_arm;
    const CmAstMatchArm *ordinary_arm;
    const CmAstPattern *guard_pattern;
    const CmAstPattern *binding;
    const CmAstPath *guard_path;
    const CmAstExpr *initializer;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    match_expression = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    let_arm = match_expression == NULL
        || match_expression->kind != CM_AST_EXPR_MATCH
        || match_expression->data.match_expr.arm_count != 4u ? NULL
        : &match_expression->data.match_expr.arms[1];
    ordinary_arm = match_expression == NULL
        || match_expression->kind != CM_AST_EXPR_MATCH
        || match_expression->data.match_expr.arm_count != 4u ? NULL
        : &match_expression->data.match_expr.arms[2];
    guard_pattern = let_arm == NULL ? NULL
        : cm_ast_get_pattern(&ast, let_arm->guard_pattern);
    guard_path = guard_pattern == NULL
        || guard_pattern->kind != CM_AST_PATTERN_STRUCT ? NULL
        : cm_ast_get_path(&ast, guard_pattern->data.struct_pattern.path);
    binding = guard_pattern == NULL
        || guard_pattern->kind != CM_AST_PATTERN_STRUCT
        || guard_pattern->data.struct_pattern.field_count != 1u
        || guard_pattern->data.struct_pattern.fields == NULL ? NULL
        : cm_ast_get_pattern(&ast,
            guard_pattern->data.struct_pattern.fields[0].pattern);
    initializer = let_arm == NULL ? NULL
        : cm_ast_get_expr(&ast, let_arm->guard_initializer);
    ok = result.error_count == 0u && body != NULL
        && body->data.block.statement_count == 0u
        && match_expression != NULL
        && let_arm != NULL && let_arm->guard == CM_AST_EXPR_NONE
        && let_arm->guard_pattern != CM_AST_PATTERN_NONE
        && let_arm->guard_initializer != CM_AST_EXPR_NONE
        && ast_span_is(source, let_arm->guard_span,
            "if let Some(value) = parse_inf_nan(s, negative)")
        && guard_pattern != NULL && guard_pattern->data.struct_pattern.is_tuple
        && ast_span_is(source, guard_pattern->span, "Some(value)")
        && guard_path != NULL && guard_path->segment_count == 1u
        && ast_string_is(&ast, guard_path->segments[0].name, "Some")
        && binding != NULL && binding->kind == CM_AST_PATTERN_BINDING
        && ast_string_is(&ast, binding->data.binding.name, "value")
        && initializer != NULL && initializer->kind == CM_AST_EXPR_CALL
        && initializer->data.call.argument_count == 2u
        && ast_expression_path_is(&ast, initializer->data.call.callee,
            "parse_inf_nan")
        && ast_span_is(source, initializer->span,
            "parse_inf_nan(s, negative)")
        && ordinary_arm != NULL
        && ordinary_arm->guard != CM_AST_EXPR_NONE
        && ordinary_arm->guard_pattern == CM_AST_PATTERN_NONE
        && ordinary_arm->guard_initializer == CM_AST_EXPR_NONE
        && ast_expression_path_is(&ast, ordinary_arm->guard, "negative")
        && ast_span_is(source, ordinary_arm->guard_span, "if negative")
        && ast_dump_contains(&ast,
            "if let tuple-struct(\"Some\" (bind(\"value\"))) = call(");
    if (!ok) {
        fprintf(stderr, "structured match let guard AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_chained_let_condition(void)
{
    static const char source[] =
        "fn nth(v: V, n: usize, size: usize) { "
        "if let Some(rest) = v.get(n..) "
        "&& let Some(nth) = rest.get(..size) "
        "{ nth } else { None } }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *if_expression;
    const CmAstExpr *condition;
    const CmAstExpr *let_condition;
    const CmAstExpr *initializer;
    const CmAstPattern *first_pattern;
    const CmAstPattern *second_pattern;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    if_expression = body == NULL || body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, body->data.block.tail);
    condition = if_expression == NULL || if_expression->kind != CM_AST_EXPR_IF
        ? NULL : cm_ast_get_expr(&ast,
            if_expression->data.if_expr.condition);
    let_condition = condition == NULL
            || condition->kind != CM_AST_EXPR_BINARY
        ? NULL : cm_ast_get_expr(&ast, condition->data.binary.right);
    initializer = let_condition == NULL
            || let_condition->kind != CM_AST_EXPR_LET
        ? NULL : cm_ast_get_expr(&ast,
            let_condition->data.let_expr.initializer);
    first_pattern = if_expression == NULL
        ? NULL : cm_ast_get_pattern(&ast,
            if_expression->data.if_expr.pattern);
    second_pattern = let_condition == NULL
            || let_condition->kind != CM_AST_EXPR_LET
        ? NULL : cm_ast_get_pattern(&ast,
            let_condition->data.let_expr.pattern);
    ok = result.error_count == 0u && if_expression != NULL
        && first_pattern != NULL
        && first_pattern->kind == CM_AST_PATTERN_STRUCT
        && condition != NULL && condition->kind == CM_AST_EXPR_BINARY
        && ast_string_is(&ast, condition->data.binary.operator_name, "&&")
        && let_condition != NULL
        && let_condition->kind == CM_AST_EXPR_LET
        && ast_span_is(source, let_condition->span,
            "let Some(nth) = rest.get(..size)")
        && second_pattern != NULL
        && second_pattern->kind == CM_AST_PATTERN_STRUCT
        && initializer != NULL
        && initializer->kind == CM_AST_EXPR_METHOD_CALL
        && ast_string_is(&ast, initializer->data.method_call.name, "get")
        && ast_dump_contains(&ast,
            "let(tuple-struct(\"Some\" (bind(\"nth\"))) = method(");
    if (!ok) {
        fprintf(stderr, "chained let condition was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_labeled_loop_expression(void)
{
    static const char source[] =
        "fn search() { 'search: loop { break 'search; } }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *loop_expression;
    const CmAstExpr *loop_body;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    loop_expression = body == NULL || body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, body->data.block.tail);
    loop_body = loop_expression == NULL
            || loop_expression->kind != CM_AST_EXPR_LOOP
        ? NULL : cm_ast_get_expr(&ast,
            loop_expression->data.loop_expr.body);
    ok = result.error_count == 0u && loop_expression != NULL
        && loop_expression->kind == CM_AST_EXPR_LOOP
        && ast_string_is(&ast, loop_expression->data.loop_expr.label,
            "'search")
        && ast_span_is(source, loop_expression->span,
            "'search: loop { break 'search; }")
        && loop_body != NULL && loop_body->kind == CM_AST_EXPR_BLOCK
        && ast_dump_contains(&ast,
            "loop(label=\"'search\", block(stmt(break(\"'search\"))))");
    if (!ok) {
        fprintf(stderr, "labeled loop expression was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_or_pattern_with_range_alternatives(void)
{
    static const char source[] =
        "fn is_ascii(c: char) -> bool { match c { "
        "'a'..='z' | 'A'..='Z' => true, _ => false } }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *match_expression;
    const CmAstMatchArm *arm;
    const CmAstPattern *or_pattern;
    const CmAstPattern *lower_range;
    const CmAstPattern *upper_range;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    match_expression = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    arm = match_expression == NULL
            || match_expression->kind != CM_AST_EXPR_MATCH
            || match_expression->data.match_expr.arm_count != 2u
        ? NULL : &match_expression->data.match_expr.arms[0];
    or_pattern = arm == NULL ? NULL
        : cm_ast_get_pattern(&ast, arm->pattern);
    lower_range = or_pattern == NULL
            || or_pattern->kind != CM_AST_PATTERN_OR
            || or_pattern->data.list.pattern_count != 2u
            || or_pattern->data.list.patterns == NULL
        ? NULL : cm_ast_get_pattern(&ast, or_pattern->data.list.patterns[0]);
    upper_range = lower_range == NULL ? NULL
        : cm_ast_get_pattern(&ast, or_pattern->data.list.patterns[1]);
    ok = result.error_count == 0u && match_expression != NULL
        && or_pattern != NULL && lower_range != NULL
        && lower_range->kind == CM_AST_PATTERN_RANGE
        && lower_range->data.range.is_inclusive
        && ast_span_is(source, lower_range->span, "'a'..='z'")
        && upper_range != NULL && upper_range->kind == CM_AST_PATTERN_RANGE
        && upper_range->data.range.is_inclusive
        && ast_span_is(source, upper_range->span, "'A'..='Z'")
        && ast_span_is(source, or_pattern->span,
            "'a'..='z' | 'A'..='Z'")
        && ast_dump_contains(&ast,
            "or(range(literal(\"'a'\")..=literal(\"'z'\")), "
            "range(literal(\"'A'\")..=literal(\"'Z'\")))");
    if (!ok) {
        fprintf(stderr, "or-pattern range AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_omitted_start_range_pattern(void)
{
    static const char source[] =
        "fn len(code: u32) -> usize { match code { "
        "..MAX_ONE_B => 1, ..=MAX_TWO_B => 2, _ => 3 } }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *match_expression;
    const CmAstPattern *exclusive_range;
    const CmAstPattern *inclusive_range;
    const CmAstPattern *exclusive_end;
    const CmAstPattern *inclusive_end;
    const CmAstPath *exclusive_path;
    const CmAstPath *inclusive_path;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    match_expression = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    exclusive_range = match_expression == NULL
            || match_expression->kind != CM_AST_EXPR_MATCH
            || match_expression->data.match_expr.arm_count != 3u
        ? NULL : cm_ast_get_pattern(&ast,
            match_expression->data.match_expr.arms[0].pattern);
    inclusive_range = exclusive_range == NULL ? NULL
        : cm_ast_get_pattern(&ast,
            match_expression->data.match_expr.arms[1].pattern);
    exclusive_end = exclusive_range == NULL
            || exclusive_range->kind != CM_AST_PATTERN_RANGE
        ? NULL : cm_ast_get_pattern(&ast, exclusive_range->data.range.end);
    inclusive_end = inclusive_range == NULL
            || inclusive_range->kind != CM_AST_PATTERN_RANGE
        ? NULL : cm_ast_get_pattern(&ast, inclusive_range->data.range.end);
    exclusive_path = exclusive_end == NULL
            || exclusive_end->kind != CM_AST_PATTERN_PATH
        ? NULL : cm_ast_get_path(&ast, exclusive_end->data.path.path);
    inclusive_path = inclusive_end == NULL
            || inclusive_end->kind != CM_AST_PATTERN_PATH
        ? NULL : cm_ast_get_path(&ast, inclusive_end->data.path.path);
    ok = result.error_count == 0u && match_expression != NULL
        && exclusive_range != NULL
        && exclusive_range->data.range.start == CM_AST_PATTERN_NONE
        && !exclusive_range->data.range.is_inclusive
        && ast_span_is(source, exclusive_range->span, "..MAX_ONE_B")
        && exclusive_path != NULL && exclusive_path->segment_count == 1u
        && ast_string_is(&ast, exclusive_path->segments[0].name,
            "MAX_ONE_B")
        && inclusive_range != NULL
        && inclusive_range->data.range.start == CM_AST_PATTERN_NONE
        && inclusive_range->data.range.is_inclusive
        && ast_span_is(source, inclusive_range->span, "..=MAX_TWO_B")
        && inclusive_path != NULL && inclusive_path->segment_count == 1u
        && ast_string_is(&ast, inclusive_path->segments[0].name,
            "MAX_TWO_B")
        && ast_dump_contains(&ast, "range(..path(\"MAX_ONE_B\"))")
        && ast_dump_contains(&ast, "range(..=path(\"MAX_TWO_B\"))");
    if (!ok) {
        fprintf(stderr, "omitted-start range AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_binding_rest_subpattern(void)
{
    static const char source[] =
        "fn split<T>(value: &[T]) { "
        "if let [first, tail @ ..] = value { tail } }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *if_expression;
    const CmAstPattern *slice;
    const CmAstPattern *first;
    const CmAstPattern *tail;
    const CmAstPattern *rest;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    if_expression = body == NULL || body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, body->data.block.tail);
    slice = if_expression == NULL || if_expression->kind != CM_AST_EXPR_IF
        ? NULL : cm_ast_get_pattern(&ast,
            if_expression->data.if_expr.pattern);
    first = slice == NULL || slice->kind != CM_AST_PATTERN_SLICE
            || slice->data.list.pattern_count != 2u
            || slice->data.list.patterns == NULL
        ? NULL : cm_ast_get_pattern(&ast, slice->data.list.patterns[0]);
    tail = first == NULL ? NULL
        : cm_ast_get_pattern(&ast, slice->data.list.patterns[1]);
    rest = tail == NULL || tail->kind != CM_AST_PATTERN_BINDING
        ? NULL : cm_ast_get_pattern(&ast, tail->data.binding.subpattern);
    ok = result.error_count == 0u && if_expression != NULL
        && slice != NULL && slice->kind == CM_AST_PATTERN_SLICE
        && !slice->data.list.has_rest
        && ast_span_is(source, slice->span, "[first, tail @ ..]")
        && first != NULL && first->kind == CM_AST_PATTERN_BINDING
        && ast_string_is(&ast, first->data.binding.name, "first")
        && tail != NULL && tail->kind == CM_AST_PATTERN_BINDING
        && ast_string_is(&ast, tail->data.binding.name, "tail")
        && ast_span_is(source, tail->span, "tail @ ..")
        && rest != NULL && rest->kind == CM_AST_PATTERN_REST
        && ast_span_is(source, rest->span, "..")
        && ast_dump_contains(&ast,
            "if(let slice(bind(\"first\"), bind(\"tail\" @ ..))");
    if (!ok) {
        fprintf(stderr, "binding rest subpattern was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_nested_reference_pattern_token_splitting(void)
{
    static const char source[] = "fn take() { consume(|&&b| b) }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *call;
    const CmAstExpr *closure;
    const CmAstPattern *outer;
    const CmAstPattern *inner;
    const CmAstPattern *binding;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    call = body == NULL || body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, body->data.block.tail);
    closure = call == NULL || call->kind != CM_AST_EXPR_CALL
            || call->data.call.argument_count != 1u
            || call->data.call.arguments == NULL
        ? NULL : cm_ast_get_expr(&ast, call->data.call.arguments[0]);
    outer = closure == NULL || closure->kind != CM_AST_EXPR_CLOSURE
            || closure->data.closure.parameter_count != 1u
            || closure->data.closure.parameters == NULL
        ? NULL : cm_ast_get_pattern(&ast,
            closure->data.closure.parameters[0].pattern);
    inner = outer == NULL || outer->kind != CM_AST_PATTERN_REFERENCE
        ? NULL : cm_ast_get_pattern(&ast, outer->data.reference.pattern);
    binding = inner == NULL || inner->kind != CM_AST_PATTERN_REFERENCE
        ? NULL : cm_ast_get_pattern(&ast, inner->data.reference.pattern);
    ok = result.error_count == 0u && closure != NULL
        && outer != NULL && outer->kind == CM_AST_PATTERN_REFERENCE
        && !outer->data.reference.is_mutable
        && ast_span_is(source, outer->span, "&&b")
        && inner != NULL && inner->kind == CM_AST_PATTERN_REFERENCE
        && !inner->data.reference.is_mutable
        && ast_span_is(source, inner->span, "&b")
        && binding != NULL && binding->kind == CM_AST_PATTERN_BINDING
        && ast_string_is(&ast, binding->data.binding.name, "b")
        && ast_span_is(source, binding->span, "b")
        && ast_expression_path_is(&ast, closure->data.closure.body, "b")
        && ast_dump_contains(&ast,
            "closure(|ref(ref(bind(\"b\")))| path(\"b\"))");
    if (!ok) {
        fprintf(stderr, "nested reference pattern split was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_block_match_arm_without_comma(void)
{
    static const char source[] =
        "fn choose(pair: P, value: R) -> R {\n"
        "    match pair {\n"
        "        (P::First, _) => { value }\n"
        "        (_, P::Second) => if ready { value } else { fallback }\n"
        "        [head, ..] => head,\n"
        "    }\n"
        "}";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *match_expression;
    const CmAstMatchArm *first_arm;
    const CmAstMatchArm *second_arm;
    const CmAstMatchArm *third_arm;
    const CmAstPattern *second_pattern;
    const CmAstPattern *third_pattern;
    const CmAstExpr *first_body;
    const CmAstExpr *second_body;
    const CmAstExpr *third_body;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    match_expression = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    first_arm = match_expression == NULL
        || match_expression->kind != CM_AST_EXPR_MATCH
        || match_expression->data.match_expr.arm_count != 3u ? NULL
        : &match_expression->data.match_expr.arms[0];
    second_arm = first_arm == NULL
        ? NULL : &match_expression->data.match_expr.arms[1];
    third_arm = first_arm == NULL
        ? NULL : &match_expression->data.match_expr.arms[2];
    second_pattern = second_arm == NULL ? NULL
        : cm_ast_get_pattern(&ast, second_arm->pattern);
    third_pattern = third_arm == NULL ? NULL
        : cm_ast_get_pattern(&ast, third_arm->pattern);
    first_body = first_arm == NULL ? NULL
        : cm_ast_get_expr(&ast, first_arm->body);
    second_body = second_arm == NULL ? NULL
        : cm_ast_get_expr(&ast, second_arm->body);
    third_body = third_arm == NULL ? NULL
        : cm_ast_get_expr(&ast, third_arm->body);
    ok = result.error_count == 0u
        && match_expression != NULL && first_arm != NULL
        && second_pattern != NULL
        && second_pattern->kind == CM_AST_PATTERN_TUPLE
        && third_pattern != NULL
        && third_pattern->kind == CM_AST_PATTERN_SLICE
        && first_body != NULL && first_body->kind == CM_AST_EXPR_BLOCK
        && ast_span_is(source, first_body->span, "{ value }")
        && second_body != NULL && second_body->kind == CM_AST_EXPR_IF
        && ast_span_is(source, second_body->span,
            "if ready { value } else { fallback }")
        && third_body != NULL && third_body->kind == CM_AST_EXPR_PATH;
    if (!ok) {
        fprintf(stderr, "block match-arm comma elision was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_nonblock_match_arm_requires_comma(void)
{
    static const char source[] =
        "fn choose(value: P) -> R { "
        "match value { P::First => first P::Second => second } }";
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    ok = result.error_count != 0u
        && strcmp(result.first_error.message,
            "expected ',' after match arm") == 0;
    if (!ok) {
        fprintf(stderr, "non-block match arm omitted-comma error was lost: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_method_turbofish_arguments(void)
{
    static const char source[] =
        "fn fast<F>(num: Num) -> R { num.try_fast_path::<F>() }\n"
        "fn plain(num: Num) -> R { num.try_fast_path() }";
    static const char *const f_path[] = { "F" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *fast_id;
    const CmAstItemId *plain_id;
    const CmAstItem *fast_function;
    const CmAstItem *plain_function;
    const CmAstExpr *fast_body;
    const CmAstExpr *plain_body;
    const CmAstExpr *fast_method;
    const CmAstExpr *plain_method;
    const CmAstGenericArg *argument;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    fast_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    plain_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    fast_function = fast_id == NULL ? NULL
        : cm_ast_get_item(&ast, *fast_id);
    plain_function = plain_id == NULL ? NULL
        : cm_ast_get_item(&ast, *plain_id);
    fast_body = fast_function == NULL ? NULL : cm_ast_get_expr(&ast,
        fast_function->data.function_item.body);
    plain_body = plain_function == NULL ? NULL : cm_ast_get_expr(&ast,
        plain_function->data.function_item.body);
    fast_method = fast_body == NULL ? NULL : cm_ast_get_expr(&ast,
        fast_body->data.block.tail);
    plain_method = plain_body == NULL ? NULL : cm_ast_get_expr(&ast,
        plain_body->data.block.tail);
    argument = fast_method == NULL
        || fast_method->kind != CM_AST_EXPR_METHOD_CALL
        || fast_method->data.method_call.generic_argument_count != 1u
        || fast_method->data.method_call.generic_arguments == NULL ? NULL
        : &fast_method->data.method_call.generic_arguments[0];
    ok = result.error_count == 0u && ast.root_items.len == 2u
        && fast_method != NULL
        && fast_method->kind == CM_AST_EXPR_METHOD_CALL
        && ast_expression_path_is(&ast,
            fast_method->data.method_call.receiver, "num")
        && ast_string_is(&ast, fast_method->data.method_call.name,
            "try_fast_path")
        && fast_method->data.method_call.argument_count == 0u
        && ast_span_is(source,
            fast_method->data.method_call.generic_argument_span, "::<F>")
        && ast_span_is(source, fast_method->span,
            "num.try_fast_path::<F>()")
        && argument != NULL && argument->kind == CM_AST_GENERIC_TYPE
        && ast_span_is(source, argument->span, "F")
        && ast_path_segments_are(&ast, argument->type, f_path, 1u)
        && plain_method != NULL
        && plain_method->kind == CM_AST_EXPR_METHOD_CALL
        && plain_method->data.method_call.generic_arguments == NULL
        && plain_method->data.method_call.generic_argument_count == 0u
        && plain_method->data.method_call.generic_argument_span.start == 0u
        && plain_method->data.method_call.generic_argument_span.end == 0u
        && ast_span_is(source, plain_method->span, "num.try_fast_path()")
        && ast_dump_contains(&ast,
            ".\"try_fast_path\"::<type=path(\"F\")>");
    if (!ok) {
        fprintf(stderr, "method turbofish AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_structured_impl_trait_type(void)
{
    static const char source[] =
        "fn parse(func: impl FnMut(u8) -> bool + Send) {}";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstType *opaque;
    const CmAstType *callable;
    const CmAstPath *callable_path;
    const CmAstPathSegment *segment;
    const CmAstGenericArg *arguments;
    const CmAstType *inputs;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    opaque = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
        || function->data.function_item.parameter_count != 1u
        || function->data.function_item.parameters == NULL ? NULL
        : cm_ast_get_type(&ast,
            function->data.function_item.parameters[0].type);
    callable = opaque == NULL || opaque->kind != CM_AST_TYPE_IMPL_TRAIT
        || opaque->bound_count != 2u || opaque->bounds == NULL ? NULL
        : cm_ast_get_type(&ast, opaque->bounds[0].trait_type);
    callable_path = callable == NULL ? NULL
        : cm_ast_get_path(&ast, callable->path);
    segment = callable_path == NULL || callable_path->segment_count != 1u
        || callable_path->segments == NULL ? NULL
        : &callable_path->segments[0];
    arguments = segment == NULL || segment->argument_count != 2u
        || segment->arguments == NULL ? NULL : segment->arguments;
    inputs = arguments == NULL ? NULL
        : cm_ast_get_type(&ast, arguments[0].type);
    ok = result.error_count == 0u && opaque != NULL
        && ast_span_is(source, opaque->span,
            "impl FnMut(u8) -> bool + Send")
        && ast_span_is(source, opaque->bounds[0].span,
            "FnMut(u8) -> bool")
        && ast_span_is(source, opaque->bounds[1].span, "Send")
        && callable != NULL && callable->kind == CM_AST_TYPE_PATH
        && segment != NULL && ast_string_is(&ast, segment->name, "FnMut")
        && arguments != NULL
        && arguments[0].kind == CM_AST_GENERIC_TYPE
        && inputs != NULL && inputs->kind == CM_AST_TYPE_TUPLE
        && inputs->tuple_provenance == CM_AST_TUPLE_CALLABLE_INPUTS
        && inputs->element_count == 1u && inputs->elements != NULL
        && arguments[1].kind == CM_AST_GENERIC_BINDING
        && ast_string_is(&ast, arguments[1].name, "Output")
        && ast_dump_contains(&ast,
            "impl(path(\"FnMut\"<type=callable-tuple(path(\"u8\")), "
            "binding \"Output\"=path(\"bool\")>) + path(\"Send\"))");
    if (!ok) {
        fprintf(stderr, "structured impl trait AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_parenthesized_and_singleton_tuple_types(void)
{
    static const char source[] =
        "fn shapes(grouped: (u8), singleton: (u16,)) {}";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstType *grouped;
    const CmAstType *singleton;
    const CmAstType *element;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    grouped = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
            || function->data.function_item.parameter_count != 2u
            || function->data.function_item.parameters == NULL
        ? NULL : cm_ast_get_type(&ast,
            function->data.function_item.parameters[0].type);
    singleton = grouped == NULL ? NULL : cm_ast_get_type(&ast,
        function->data.function_item.parameters[1].type);
    element = singleton == NULL || singleton->kind != CM_AST_TYPE_TUPLE
            || singleton->element_count != 1u
            || singleton->elements == NULL
        ? NULL : cm_ast_get_type(&ast, singleton->elements[0]);
    ok = result.error_count == 0u
        && grouped != NULL && grouped->kind == CM_AST_TYPE_PATH
        && ast_span_is(source, grouped->span, "u8")
        && singleton != NULL && singleton->kind == CM_AST_TYPE_TUPLE
        && singleton->tuple_provenance == CM_AST_TUPLE_SOURCE
        && ast_span_is(source, singleton->span, "(u16,)")
        && element != NULL && element->kind == CM_AST_TYPE_PATH
        && ast_span_is(source, element->span, "u16");
    if (!ok) {
        fprintf(stderr,
            "parenthesized/singleton tuple type AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_conditionally_const_impl_trait_bounds(void)
{
    static const char source[] =
        "fn is_some_and<T>(f: impl ~const FnOnce(T) -> bool + "
        "~const Destruct) {}";
    static const char *const fn_once_path[] = { "FnOnce" };
    static const char *const destruct_path[] = { "Destruct" };
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstType *opaque;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    opaque = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
            || function->data.function_item.parameter_count != 1u
            || function->data.function_item.parameters == NULL
        ? NULL : cm_ast_get_type(&ast,
            function->data.function_item.parameters[0].type);
    ok = result.error_count == 0u && opaque != NULL
        && opaque->kind == CM_AST_TYPE_IMPL_TRAIT
        && opaque->bound_count == 2u && opaque->bounds != NULL
        && opaque->bounds[0].modifier
            == CM_AST_TYPE_BOUND_CONDITIONALLY_CONST
        && ast_path_segments_are(&ast, opaque->bounds[0].trait_type,
            fn_once_path, 1u)
        && ast_span_is(source, opaque->bounds[0].span,
            "~const FnOnce(T) -> bool")
        && opaque->bounds[1].modifier
            == CM_AST_TYPE_BOUND_CONDITIONALLY_CONST
        && ast_path_segments_are(&ast, opaque->bounds[1].trait_type,
            destruct_path, 1u)
        && ast_span_is(source, opaque->bounds[1].span, "~const Destruct")
        && ast_span_is(source, opaque->span,
            "impl ~const FnOnce(T) -> bool + ~const Destruct")
        && ast_dump_contains(&ast,
            "impl(~const path(\"FnOnce\"<")
        && ast_dump_contains(&ast,
            "+ ~const path(\"Destruct\"))");
    if (!ok) {
        fprintf(stderr,
            "conditionally-const impl trait bounds were incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_unsafe_function_pointer_type(void)
{
    static const char source[] =
        "fn register(formatter: unsafe fn(NonNull<()>, "
        "&mut Formatter<'_>) -> Result) {}";
    static const char *const non_null_path[] = { "NonNull" };
    static const char *const formatter_path[] = { "Formatter" };
    static const char *const result_path[] = { "Result" };
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstType *pointer;
    const CmAstType *first_parameter;
    const CmAstType *second_parameter;
    const CmAstType *formatter;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    pointer = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
            || function->data.function_item.parameter_count != 1u
            || function->data.function_item.parameters == NULL
        ? NULL : cm_ast_get_type(&ast,
            function->data.function_item.parameters[0].type);
    first_parameter = pointer == NULL || pointer->kind != CM_AST_TYPE_FUNCTION
            || pointer->element_count != 2u || pointer->elements == NULL
        ? NULL : cm_ast_get_type(&ast, pointer->elements[0]);
    second_parameter = pointer == NULL || pointer->kind != CM_AST_TYPE_FUNCTION
            || pointer->element_count != 2u || pointer->elements == NULL
        ? NULL : cm_ast_get_type(&ast, pointer->elements[1]);
    formatter = second_parameter == NULL
            || second_parameter->kind != CM_AST_TYPE_REFERENCE
        ? NULL : cm_ast_get_type(&ast, second_parameter->child);
    ok = result.error_count == 0u && pointer != NULL
        && pointer->kind == CM_AST_TYPE_FUNCTION && pointer->is_unsafe
        && ast_span_is(source, pointer->span,
            "unsafe fn(NonNull<()>, &mut Formatter<'_>) -> Result")
        && first_parameter != NULL
        && ast_path_segments_are(&ast, pointer->elements[0],
            non_null_path, 1u)
        && second_parameter != NULL
        && second_parameter->kind == CM_AST_TYPE_REFERENCE
        && second_parameter->is_mutable
        && formatter != NULL
        && ast_path_segments_are(&ast, second_parameter->child,
            formatter_path, 1u)
        && ast_path_segments_are(&ast, pointer->child, result_path, 1u)
        && ast_dump_contains(&ast, "unsafe-fn(");
    if (!ok) {
        fprintf(stderr, "unsafe function pointer type was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_higher_ranked_impl_trait_type(void)
{
    static const char source[] =
        "fn drain<T, R>(func: impl for<'a> FnOnce(Drain<'a, T>) -> R) {}";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstType *opaque;
    const CmAstType *callable;
    const CmAstPath *path;
    const CmAstPathSegment *segment;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    opaque = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
            || function->data.function_item.parameter_count != 1u
            || function->data.function_item.parameters == NULL
        ? NULL : cm_ast_get_type(&ast,
            function->data.function_item.parameters[0].type);
    callable = opaque == NULL || opaque->kind != CM_AST_TYPE_IMPL_TRAIT
            || opaque->bound_count != 1u || opaque->bounds == NULL
        ? NULL : cm_ast_get_type(&ast, opaque->bounds[0].trait_type);
    path = callable == NULL ? NULL : cm_ast_get_path(&ast, callable->path);
    segment = path == NULL || path->segment_count != 1u
            || path->segments == NULL
        ? NULL : &path->segments[0];
    ok = result.error_count == 0u && opaque != NULL
        && ast_span_is(source, opaque->span,
            "impl for<'a> FnOnce(Drain<'a, T>) -> R")
        && ast_span_is(source, opaque->bounds[0].span,
            "for<'a> FnOnce(Drain<'a, T>) -> R")
        && opaque->bounds[0].binder.lifetime_count == 1u
        && opaque->bounds[0].binder.lifetimes != NULL
        && ast_string_is(&ast,
            opaque->bounds[0].binder.lifetimes[0], "'a")
        && ast_span_is(source, opaque->bounds[0].binder.span, "for<'a>")
        && callable != NULL && callable->kind == CM_AST_TYPE_PATH
        && segment != NULL && ast_string_is(&ast, segment->name, "FnOnce")
        && ast_dump_contains(&ast,
            "impl(for<\"'a\"> path(\"FnOnce\"<type=callable-tuple(");
    if (!ok) {
        fprintf(stderr, "higher-ranked impl trait AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_impl_trait_lifetime_bound(void)
{
    static const char source[] =
        "fn adapt<'a>(fold: impl FnMut(u8) -> bool + 'a) "
        "-> impl Copy + 'a { fold }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstType *parameter_type;
    const CmAstType *return_type;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    parameter_type = function == NULL
            || function->data.function_item.parameter_count != 1u
            || function->data.function_item.parameters == NULL
        ? NULL : cm_ast_get_type(&ast,
            function->data.function_item.parameters[0].type);
    return_type = function == NULL ? NULL
        : cm_ast_get_type(&ast, function->data.function_item.return_type);
    ok = result.error_count == 0u && parameter_type != NULL
        && parameter_type->kind == CM_AST_TYPE_IMPL_TRAIT
        && parameter_type->bound_count == 2u
        && parameter_type->bounds != NULL
        && parameter_type->bounds[1].trait_type == CM_AST_TYPE_NONE
        && ast_string_is(&ast, parameter_type->bounds[1].lifetime, "'a")
        && ast_span_is(source, parameter_type->bounds[1].span, "'a")
        && return_type != NULL
        && return_type->kind == CM_AST_TYPE_IMPL_TRAIT
        && return_type->bound_count == 2u && return_type->bounds != NULL
        && return_type->bounds[1].trait_type == CM_AST_TYPE_NONE
        && ast_string_is(&ast, return_type->bounds[1].lifetime, "'a")
        && ast_dump_contains(&ast,
            "impl(path(\"FnMut\"<type=callable-tuple(path(\"u8\")), "
            "binding \"Output\"=path(\"bool\")>) + \"'a\")")
        && ast_dump_contains(&ast,
            "return impl(path(\"Copy\") + \"'a\")");
    if (!ok) {
        fprintf(stderr, "impl-trait lifetime bound was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_zero_parameter_closure_with_never_turbofish(void)
{
    static const char source[] =
        "fn get<T, F>(f: F) { "
        "self.get_or_try_init(|| Ok::<T, !>(f())) }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *method;
    const CmAstExpr *closure;
    const CmAstExpr *call;
    const CmAstExpr *callee;
    const CmAstPath *path;
    const CmAstPathSegment *segment;
    const CmAstType *never_type;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    method = body == NULL || body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, body->data.block.tail);
    closure = method == NULL || method->kind != CM_AST_EXPR_METHOD_CALL
            || method->data.method_call.argument_count != 1u
            || method->data.method_call.arguments == NULL
        ? NULL : cm_ast_get_expr(&ast,
            method->data.method_call.arguments[0]);
    call = closure == NULL || closure->kind != CM_AST_EXPR_CLOSURE
            || closure->data.closure.parameter_count != 0u
        ? NULL : cm_ast_get_expr(&ast, closure->data.closure.body);
    callee = call == NULL || call->kind != CM_AST_EXPR_CALL
        ? NULL : cm_ast_get_expr(&ast, call->data.call.callee);
    path = callee == NULL || callee->kind != CM_AST_EXPR_PATH
        ? NULL : cm_ast_get_path(&ast, callee->data.path.path);
    segment = path == NULL || path->segment_count != 1u
            || path->segments == NULL
        ? NULL : &path->segments[0];
    never_type = segment == NULL || segment->argument_count != 2u
            || segment->arguments == NULL
            || segment->arguments[1].kind != CM_AST_GENERIC_TYPE
        ? NULL : cm_ast_get_type(&ast, segment->arguments[1].type);
    ok = result.error_count == 0u && method != NULL
        && closure != NULL && closure->data.closure.parameters == NULL
        && ast_span_is(source, closure->span, "|| Ok::<T, !>(f())")
        && call != NULL && callee != NULL
        && segment != NULL && ast_string_is(&ast, segment->name, "Ok")
        && segment->arguments[0].kind == CM_AST_GENERIC_TYPE
        && never_type != NULL && never_type->kind == CM_AST_TYPE_NEVER
        && ast_span_is(source, never_type->span, "!")
        && ast_dump_contains(&ast,
            "closure(|| call(path(\"Ok\"<type=path(\"T\"), "
            "type=!>)");
    if (!ok) {
        fprintf(stderr,
            "zero-parameter closure/never turbofish AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_structured_dyn_trait_type(void)
{
    static const char source[] =
        "fn source(error: &(dyn Error + Send + 'static)) {}";
    static const char *const error_path[] = { "Error" };
    static const char *const send_path[] = { "Send" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstType *reference;
    const CmAstType *dynamic;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    reference = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
            || function->data.function_item.parameter_count != 1u
            || function->data.function_item.parameters == NULL
        ? NULL : cm_ast_get_type(&ast,
            function->data.function_item.parameters[0].type);
    dynamic = reference == NULL || reference->kind != CM_AST_TYPE_REFERENCE
        ? NULL : cm_ast_get_type(&ast, reference->child);
    ok = result.error_count == 0u && dynamic != NULL
        && dynamic->kind == CM_AST_TYPE_DYN_TRAIT
        && dynamic->bound_count == 3u && dynamic->bounds != NULL
        && ast_span_is(source, reference->span,
            "&(dyn Error + Send + 'static)")
        && ast_span_is(source, dynamic->span,
            "dyn Error + Send + 'static")
        && ast_path_segments_are(&ast, dynamic->bounds[0].trait_type,
            error_path, 1u)
        && dynamic->bounds[0].lifetime == CM_INTERN_ID_NONE
        && ast_span_is(source, dynamic->bounds[0].span, "Error")
        && ast_path_segments_are(&ast, dynamic->bounds[1].trait_type,
            send_path, 1u)
        && dynamic->bounds[1].lifetime == CM_INTERN_ID_NONE
        && ast_span_is(source, dynamic->bounds[1].span, "Send")
        && dynamic->bounds[2].trait_type == CM_AST_TYPE_NONE
        && ast_string_is(&ast, dynamic->bounds[2].lifetime, "'static")
        && ast_span_is(source, dynamic->bounds[2].span, "'static")
        && ast_dump_contains(&ast,
            "dyn(path(\"Error\") + path(\"Send\") + \"'static\")");
    if (!ok) {
        fprintf(stderr, "structured dyn trait AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_relaxed_impl_trait_type_bound(void)
{
    static const char source[] =
        "fn request(err: &(impl Error + ?Sized)) {}";
    static const char *const error_path[] = { "Error" };
    static const char *const sized_path[] = { "Sized" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstType *reference;
    const CmAstType *opaque;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    reference = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
            || function->data.function_item.parameter_count != 1u
            || function->data.function_item.parameters == NULL
        ? NULL : cm_ast_get_type(&ast,
            function->data.function_item.parameters[0].type);
    opaque = reference == NULL || reference->kind != CM_AST_TYPE_REFERENCE
        ? NULL : cm_ast_get_type(&ast, reference->child);
    ok = result.error_count == 0u && opaque != NULL
        && opaque->kind == CM_AST_TYPE_IMPL_TRAIT
        && opaque->bound_count == 2u && opaque->bounds != NULL
        && opaque->bounds[0].modifier == CM_AST_TYPE_BOUND_REQUIRED
        && ast_path_segments_are(&ast, opaque->bounds[0].trait_type,
            error_path, 1u)
        && opaque->bounds[1].modifier == CM_AST_TYPE_BOUND_RELAXED
        && ast_path_segments_are(&ast, opaque->bounds[1].trait_type,
            sized_path, 1u)
        && ast_span_is(source, opaque->bounds[1].span, "?Sized")
        && ast_dump_contains(&ast,
            "impl(path(\"Error\") + ?path(\"Sized\"))");
    if (!ok) {
        fprintf(stderr, "relaxed impl trait bound was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_lifetime_qualified_self_receiver(void)
{
    static const char source[] = "fn borrow<'a>(&'a mut self) {}";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstFunctionParam *parameter;
    const CmAstPattern *reference;
    const CmAstPattern *binding;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    parameter = function == NULL || function->kind != CM_AST_ITEM_FUNCTION
            || function->data.function_item.parameter_count != 1u
            || function->data.function_item.parameters == NULL
        ? NULL : &function->data.function_item.parameters[0];
    reference = parameter == NULL ? NULL
        : cm_ast_get_pattern(&ast, parameter->pattern);
    binding = reference == NULL
            || reference->kind != CM_AST_PATTERN_REFERENCE
        ? NULL : cm_ast_get_pattern(&ast,
            reference->data.reference.pattern);
    ok = result.error_count == 0u && parameter != NULL
        && parameter->is_self
        && ast_string_is(&ast, parameter->receiver_lifetime, "'a")
        && reference != NULL && reference->data.reference.is_mutable
        && ast_span_is(source, reference->span, "&'a mut self")
        && binding != NULL && binding->kind == CM_AST_PATTERN_BINDING
        && ast_string_is(&ast, binding->data.binding.name, "self")
        && ast_dump_contains(&ast,
            "(self lifetime=\"'a\" ref(mut bind(\"self\")) <none>)");
    if (!ok) {
        fprintf(stderr,
            "lifetime-qualified self receiver was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_postfix_try_expression(void)
{
    static const char source[] =
        "fn parse(value: R) -> R { checked(value)? }";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *try_expression;
    const CmAstExpr *operand;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    try_expression = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    operand = try_expression == NULL
        || try_expression->kind != CM_AST_EXPR_TRY ? NULL
        : cm_ast_get_expr(&ast, try_expression->data.try_expr.operand);
    ok = result.error_count == 0u && body != NULL
        && body->kind == CM_AST_EXPR_BLOCK
        && body->data.block.statement_count == 0u
        && try_expression != NULL
        && ast_span_is(source, try_expression->span, "checked(value)?")
        && operand != NULL && operand->kind == CM_AST_EXPR_CALL
        && ast_span_is(source, operand->span, "checked(value)")
        && operand->data.call.argument_count == 1u
        && ast_expression_path_is(&ast, operand->data.call.callee,
            "checked")
        && ast_expression_path_is(&ast, operand->data.call.arguments[0],
            "value")
        && ast_dump_contains(&ast,
            "try(call(path(\"checked\"), path(\"value\")))");
    if (!ok) {
        fprintf(stderr, "postfix try AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_try_block_expression(void)
{
    static const char source[] =
        "fn branch(acc: B) -> T { ControlFlow::Break(try { acc }) }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *call;
    const CmAstExpr *try_block;
    const CmAstExpr *block;
    const CmAstExpr *tail;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    call = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    try_block = call == NULL || call->kind != CM_AST_EXPR_CALL
            || call->data.call.argument_count != 1u
            || call->data.call.arguments == NULL
        ? NULL : cm_ast_get_expr(&ast, call->data.call.arguments[0]);
    block = try_block == NULL || try_block->kind != CM_AST_EXPR_TRY_BLOCK
        ? NULL : cm_ast_get_expr(&ast, try_block->data.try_expr.operand);
    tail = block == NULL || block->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, block->data.block.tail);
    ok = result.error_count == 0u && call != NULL && try_block != NULL
        && ast_span_is(source, try_block->span, "try { acc }")
        && block != NULL && ast_span_is(source, block->span, "{ acc }")
        && tail != NULL && ast_expression_path_is(&ast,
            block->data.block.tail, "acc")
        && ast_dump_contains(&ast,
            "try-block(block(tail=path(\"acc\")))");
    if (!ok) {
        fprintf(stderr, "try-block AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_tuple_index_projection(void)
{
    static const char source[] =
        "fn first(self) -> u16 { self.0 }";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *projection;
    const CmAstExpr *base;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    projection = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    base = projection == NULL
        || projection->kind != CM_AST_EXPR_TUPLE_FIELD ? NULL
        : cm_ast_get_expr(&ast, projection->data.tuple_field.base);
    ok = result.error_count == 0u && body != NULL
        && body->kind == CM_AST_EXPR_BLOCK
        && body->data.block.statement_count == 0u
        && projection != NULL
        && projection->kind == CM_AST_EXPR_TUPLE_FIELD
        && projection->data.tuple_field.index == 0u
        && ast_span_is(source, projection->span, "self.0")
        && ast_span_is(source, projection->data.tuple_field.index_span, "0")
        && base != NULL && base->kind == CM_AST_EXPR_PATH
        && ast_span_is(source, base->span, "self")
        && ast_expression_path_is(&ast,
            projection->data.tuple_field.base, "self")
        && ast_dump_contains(&ast,
            "tuple-field(path(\"self\").0)");
    if (!ok) {
        fprintf(stderr, "tuple-index projection AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_qualified_expression_path(void)
{
    static const char source[] =
        "fn decode<T>() -> D { "
        "<T as DecodableFloat>::min_pos_norm_value().integer_decode() }";
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *method;
    const CmAstExpr *call;
    const CmAstExpr *qualified;
    const CmAstType *self_type;
    const CmAstPath *self_path;
    const CmAstPath *trait_path;
    const CmAstPath *associated_path;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    method = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    call = method == NULL || method->kind != CM_AST_EXPR_METHOD_CALL
        ? NULL : cm_ast_get_expr(&ast,
            method->data.method_call.receiver);
    qualified = call == NULL || call->kind != CM_AST_EXPR_CALL
        ? NULL : cm_ast_get_expr(&ast, call->data.call.callee);
    self_type = qualified == NULL
            || qualified->kind != CM_AST_EXPR_QUALIFIED_PATH
        ? NULL : cm_ast_get_type(&ast,
            qualified->data.qualified_path.self_type);
    self_path = self_type == NULL || self_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, self_type->path);
    trait_path = qualified == NULL
            || qualified->kind != CM_AST_EXPR_QUALIFIED_PATH
        ? NULL : cm_ast_get_path(&ast,
            qualified->data.qualified_path.trait_path);
    associated_path = qualified == NULL
            || qualified->kind != CM_AST_EXPR_QUALIFIED_PATH
        ? NULL : cm_ast_get_path(&ast,
            qualified->data.qualified_path.associated_path);
    ok = result.error_count == 0u && method != NULL
        && method->kind == CM_AST_EXPR_METHOD_CALL
        && ast_string_is(&ast, method->data.method_call.name,
            "integer_decode")
        && call != NULL && call->data.call.argument_count == 0u
        && qualified != NULL
        && qualified->kind == CM_AST_EXPR_QUALIFIED_PATH
        && ast_span_is(source, qualified->span,
            "<T as DecodableFloat>::min_pos_norm_value")
        && ast_span_is(source,
            qualified->data.qualified_path.qualifier_span,
            "<T as DecodableFloat>")
        && self_path != NULL && self_path->segment_count == 1u
        && ast_string_is(&ast, self_path->segments[0].name, "T")
        && trait_path != NULL && trait_path->segment_count == 1u
        && ast_string_is(&ast, trait_path->segments[0].name,
            "DecodableFloat")
        && associated_path != NULL
        && associated_path->segment_count == 1u
        && ast_string_is(&ast, associated_path->segments[0].name,
            "min_pos_norm_value")
        && ast_dump_contains(&ast,
            "qualified-path(self=path(\"T\"), "
            "trait=\"DecodableFloat\", "
            "associated=\"min_pos_norm_value\")");
    if (!ok) {
        fprintf(stderr, "qualified expression path AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_nested_qualified_expression_path(void)
{
    static const char source[] =
        "fn size<I>() -> usize { "
        "<<I as Iterator>::Item as ConstSizeIntoIterator>::size() }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *call;
    const CmAstExpr *qualified;
    const CmAstType *self_type;
    const CmAstType *inner_self;
    const CmAstPath *outer_trait;
    const CmAstPath *associated;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    call = body == NULL ? NULL : cm_ast_get_expr(&ast,
        body->data.block.tail);
    qualified = call == NULL || call->kind != CM_AST_EXPR_CALL
        ? NULL : cm_ast_get_expr(&ast, call->data.call.callee);
    self_type = qualified == NULL
            || qualified->kind != CM_AST_EXPR_QUALIFIED_PATH
        ? NULL : cm_ast_get_type(&ast,
            qualified->data.qualified_path.self_type);
    inner_self = self_type == NULL
            || self_type->kind != CM_AST_TYPE_PROJECTION
        ? NULL : cm_ast_get_type(&ast, self_type->projection.self_type);
    outer_trait = qualified == NULL ? NULL : cm_ast_get_path(&ast,
        qualified->data.qualified_path.trait_path);
    associated = qualified == NULL ? NULL : cm_ast_get_path(&ast,
        qualified->data.qualified_path.associated_path);
    ok = result.error_count == 0u && call != NULL
        && call->data.call.argument_count == 0u
        && qualified != NULL
        && ast_span_is(source, qualified->span,
            "<<I as Iterator>::Item as ConstSizeIntoIterator>::size")
        && ast_span_is(source,
            qualified->data.qualified_path.qualifier_span,
            "<<I as Iterator>::Item as ConstSizeIntoIterator>")
        && self_type != NULL
        && ast_span_is(source, self_type->span, "<I as Iterator>::Item")
        && inner_self != NULL && inner_self->kind == CM_AST_TYPE_PATH
        && outer_trait != NULL && outer_trait->segment_count == 1u
        && ast_string_is(&ast, outer_trait->segments[0].name,
            "ConstSizeIntoIterator")
        && associated != NULL && associated->segment_count == 1u
        && ast_string_is(&ast, associated->segments[0].name, "size")
        && ast_dump_contains(&ast,
            "qualified-path(self=projection(self=path(\"I\"), "
            "trait=path(\"Iterator\"), associated=\"Item\"), "
            "trait=\"ConstSizeIntoIterator\", associated=\"size\")");
    if (!ok) {
        fprintf(stderr, "nested qualified expression path was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_raw_reference_expressions(void)
{
    static const char source[] =
        "fn probe(mut value: u32) { "
        "(&raw const value, &raw mut value, &value, &mut value) }";
    static const char invalid_source[] =
        "fn probe(value: u32) { &raw value }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *tuple;
    const CmAstExpr *raw_const;
    const CmAstExpr *raw_mut;
    const CmAstExpr *shared;
    const CmAstExpr *mutable;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    tuple = body == NULL || body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, body->data.block.tail);
    raw_const = tuple == NULL || tuple->kind != CM_AST_EXPR_TUPLE
            || tuple->data.list.element_count != 4u
            || tuple->data.list.elements == NULL
        ? NULL : cm_ast_get_expr(&ast, tuple->data.list.elements[0]);
    raw_mut = tuple == NULL || tuple->kind != CM_AST_EXPR_TUPLE
            || tuple->data.list.element_count != 4u
            || tuple->data.list.elements == NULL
        ? NULL : cm_ast_get_expr(&ast, tuple->data.list.elements[1]);
    shared = tuple == NULL || tuple->kind != CM_AST_EXPR_TUPLE
            || tuple->data.list.element_count != 4u
            || tuple->data.list.elements == NULL
        ? NULL : cm_ast_get_expr(&ast, tuple->data.list.elements[2]);
    mutable = tuple == NULL || tuple->kind != CM_AST_EXPR_TUPLE
            || tuple->data.list.element_count != 4u
            || tuple->data.list.elements == NULL
        ? NULL : cm_ast_get_expr(&ast, tuple->data.list.elements[3]);
    ok = result.error_count == 0u
        && raw_const != NULL
        && raw_const->kind == CM_AST_EXPR_RAW_REFERENCE
        && raw_const->data.raw_reference.kind == CM_AST_RAW_REFERENCE_CONST
        && ast_expression_path_is(&ast,
            raw_const->data.raw_reference.operand, "value")
        && ast_span_is(source, raw_const->span, "&raw const value")
        && raw_mut != NULL && raw_mut->kind == CM_AST_EXPR_RAW_REFERENCE
        && raw_mut->data.raw_reference.kind == CM_AST_RAW_REFERENCE_MUT
        && ast_expression_path_is(&ast,
            raw_mut->data.raw_reference.operand, "value")
        && ast_span_is(source, raw_mut->span, "&raw mut value")
        && shared != NULL && shared->kind == CM_AST_EXPR_UNARY
        && ast_string_is(&ast, shared->data.unary.operator_name, "&")
        && ast_expression_path_is(&ast, shared->data.unary.operand, "value")
        && mutable != NULL && mutable->kind == CM_AST_EXPR_UNARY
        && ast_string_is(&ast, mutable->data.unary.operator_name, "&mut")
        && ast_expression_path_is(&ast, mutable->data.unary.operand,
            "value")
        && ast_dump_contains(&ast,
            "raw-reference(const, path(\"value\"))")
        && ast_dump_contains(&ast,
            "raw-reference(mut, path(\"value\"))");
    if (!ok) {
        fprintf(stderr, "raw reference AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, invalid_source,
        sizeof(invalid_source) - 1u, CM_EDITION_2024);
    ok = ok && result.error_count != 0u
        && strstr(result.first_error.message,
            "expected 'const' or 'mut' after '&raw'") != NULL;
    cm_ast_destroy(&ast);
    return ok;
}

static int test_nested_reference_type_token_splitting(void)
{
    static const char source[] =
        "fn refs(value: &&B) -> &&B { value } "
        "fn logic(left: bool, right: bool) -> bool { left && right } "
        "fn nested(value: [u8; 1]) { &&value[..] }";
    static const char *const b_path[] = { "B" };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *refs_id;
    const CmAstItemId *logic_id;
    const CmAstItemId *nested_id;
    const CmAstItem *refs;
    const CmAstItem *logic;
    const CmAstItem *nested;
    const CmAstType *parameter_outer;
    const CmAstType *parameter_inner;
    const CmAstType *return_outer;
    const CmAstType *return_inner;
    const CmAstExpr *logic_body;
    const CmAstExpr *logical_and;
    const CmAstExpr *nested_body;
    const CmAstExpr *outer_borrow;
    const CmAstExpr *inner_borrow;
    const CmAstExpr *index;
    const CmAstExpr *range;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    refs_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    logic_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 1u);
    nested_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    refs = refs_id == NULL ? NULL : cm_ast_get_item(&ast, *refs_id);
    logic = logic_id == NULL ? NULL : cm_ast_get_item(&ast, *logic_id);
    nested = nested_id == NULL ? NULL : cm_ast_get_item(&ast, *nested_id);
    parameter_outer = refs == NULL
            || refs->data.function_item.parameter_count != 1u
        ? NULL : cm_ast_get_type(&ast,
            refs->data.function_item.parameters[0].type);
    parameter_inner = parameter_outer == NULL ? NULL
        : cm_ast_get_type(&ast, parameter_outer->child);
    return_outer = refs == NULL ? NULL : cm_ast_get_type(&ast,
        refs->data.function_item.return_type);
    return_inner = return_outer == NULL ? NULL
        : cm_ast_get_type(&ast, return_outer->child);
    logic_body = logic == NULL ? NULL : cm_ast_get_expr(&ast,
        logic->data.function_item.body);
    logical_and = logic_body == NULL ? NULL : cm_ast_get_expr(&ast,
        logic_body->data.block.tail);
    nested_body = nested == NULL ? NULL : cm_ast_get_expr(&ast,
        nested->data.function_item.body);
    outer_borrow = nested_body == NULL
            || nested_body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, nested_body->data.block.tail);
    inner_borrow = outer_borrow == NULL
            || outer_borrow->kind != CM_AST_EXPR_UNARY
        ? NULL : cm_ast_get_expr(&ast, outer_borrow->data.unary.operand);
    index = inner_borrow == NULL
            || inner_borrow->kind != CM_AST_EXPR_UNARY
        ? NULL : cm_ast_get_expr(&ast, inner_borrow->data.unary.operand);
    range = index == NULL || index->kind != CM_AST_EXPR_INDEX
        ? NULL : cm_ast_get_expr(&ast, index->data.index.index);
    ok = result.error_count == 0u && ast.root_items.len == 3u
        && parameter_outer != NULL
        && parameter_outer->kind == CM_AST_TYPE_REFERENCE
        && ast_span_is(source, parameter_outer->span, "&&B")
        && parameter_inner != NULL
        && parameter_inner->kind == CM_AST_TYPE_REFERENCE
        && ast_span_is(source, parameter_inner->span, "&B")
        && ast_path_segments_are(&ast, parameter_inner->child, b_path, 1u)
        && return_outer != NULL
        && return_outer->kind == CM_AST_TYPE_REFERENCE
        && ast_span_is(source, return_outer->span, "&&B")
        && return_inner != NULL
        && return_inner->kind == CM_AST_TYPE_REFERENCE
        && ast_path_segments_are(&ast, return_inner->child, b_path, 1u)
        && logical_and != NULL && logical_and->kind == CM_AST_EXPR_BINARY
        && ast_string_is(&ast, logical_and->data.binary.operator_name, "&&")
        && ast_expression_path_is(&ast, logical_and->data.binary.left,
            "left")
        && ast_expression_path_is(&ast, logical_and->data.binary.right,
            "right")
        && outer_borrow != NULL
        && ast_string_is(&ast, outer_borrow->data.unary.operator_name, "&")
        && ast_span_is(source, outer_borrow->span, "&&value[..]")
        && inner_borrow != NULL
        && ast_string_is(&ast, inner_borrow->data.unary.operator_name, "&")
        && ast_span_is(source, inner_borrow->span, "&value[..]")
        && index != NULL
        && ast_expression_path_is(&ast, index->data.index.base, "value")
        && range != NULL && range->kind == CM_AST_EXPR_RANGE
        && range->data.range.start == CM_AST_EXPR_NONE
        && range->data.range.end == CM_AST_EXPR_NONE
        && !range->data.range.is_inclusive
        && ast_span_is(source, range->span, "..");
    if (!ok) {
        fprintf(stderr, "nested reference token split was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_let_else_statement(void)
{
    static const char source[] =
        "fn take(value: u32) -> u32 { "
        "let item = value else { return 0u32; }; item }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstStmt *statement;
    const CmAstExpr *else_block;
    CmAst ast;
    CmParseResult result;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    statement = body == NULL || body->kind != CM_AST_EXPR_BLOCK
            || body->data.block.statement_count != 1u
            || body->data.block.statements == NULL
        ? NULL : cm_ast_get_stmt(&ast, body->data.block.statements[0]);
    else_block = statement == NULL
            || statement->kind != CM_AST_STMT_LET
        ? NULL : cm_ast_get_expr(&ast,
            statement->data.let_stmt.else_block);
    ok = result.error_count == 0u && statement != NULL
        && statement->kind == CM_AST_STMT_LET
        && ast_span_is(source, statement->span,
            "let item = value else { return 0u32; };")
        && ast_expression_path_is(&ast,
            statement->data.let_stmt.initializer, "value")
        && else_block != NULL && else_block->kind == CM_AST_EXPR_BLOCK
        && ast_span_is(source, else_block->span, "{ return 0u32; }")
        && body->data.block.tail != CM_AST_EXPR_NONE
        && ast_expression_path_is(&ast, body->data.block.tail, "item")
        && ast_dump_contains(&ast,
            "let(bind(\"item\") = path(\"value\") else block(");
    if (!ok) {
        fprintf(stderr, "let-else statement AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_attributed_struct_expression_field(void)
{
    static const char source[] =
        "fn new(value: u32) -> Pair { Pair { value, "
        "#[cfg(feature = \"debug\")] debug: value } }";
    const CmAstItemId *root_id;
    const CmAstItem *function;
    const CmAstExpr *body;
    const CmAstExpr *aggregate;
    const CmAstExprField *field;
    const CmAstAttribute *attribute;
    int ok;
    CmAst ast;
    CmParseResult result;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    function = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    body = function == NULL ? NULL : cm_ast_get_expr(&ast,
        function->data.function_item.body);
    aggregate = body == NULL || body->kind != CM_AST_EXPR_BLOCK
        ? NULL : cm_ast_get_expr(&ast, body->data.block.tail);
    field = aggregate == NULL || aggregate->kind != CM_AST_EXPR_STRUCT
            || aggregate->data.struct_expr.field_count != 2u
            || aggregate->data.struct_expr.fields == NULL
        ? NULL : &aggregate->data.struct_expr.fields[1];
    attribute = field == NULL || field->attribute_count != 1u
            || field->attributes == NULL
        ? NULL : cm_ast_get_attribute(&ast, field->attributes[0]);
    ok = result.error_count == 0u && aggregate != NULL
        && aggregate->data.struct_expr.fields[0].attribute_count == 0u
        && aggregate->data.struct_expr.fields[0].attributes == NULL
        && field != NULL && ast_string_is(&ast, field->name, "debug")
        && ast_span_is(source, field->span, "debug: value")
        && attribute != NULL && attribute->style == CM_AST_ATTR_OUTER
        && ast_string_is(&ast, attribute->text,
            "#[cfg(feature = \"debug\")]")
        && ast_span_is(source, attribute->span,
            "#[cfg(feature = \"debug\")]")
        && ast_expression_path_is(&ast, field->value, "value")
        && ast_dump_contains(&ast,
            "attribute(\"#[cfg(feature = \\\"debug\\\")]\") "
            "\"debug\": path(\"value\")");
    if (!ok) {
        fprintf(stderr,
            "attributed struct expression field AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);
    return ok;
}

static int test_union_items(void)
{
    static const char source[] =
        "#[repr(C)] pub union Slot<'a, T> where T: Copy { "
        "pub value: T, marker: &'a T, }";
    static const char *const broken[] = {
        "union Tuple(u8);",
        "union Unit;",
        "union Missing<T> where T: Copy;"
    };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *item;
    const CmAstField *value;
    const CmAstField *marker;
    const CmAstType *marker_type;
    size_t index;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u);
    item = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    value = item == NULL || item->data.aggregate_item.field_count != 2u
            || item->data.aggregate_item.fields == NULL
        ? NULL : &item->data.aggregate_item.fields[0];
    marker = value == NULL ? NULL : &item->data.aggregate_item.fields[1];
    marker_type = marker == NULL ? NULL
        : cm_ast_get_type(&ast, marker->type);
    ok = result.error_count == 0u && ast.root_items.len == 1u
        && item != NULL && item->kind == CM_AST_ITEM_UNION
        && item->visibility.kind == CM_AST_VIS_PUBLIC
        && item->attribute_count == 1u
        && ast_string_is(&ast, item->name, "Slot")
        && item->generic_parameter_count == 2u
        && item->where_predicate_count == 1u
        && item->data.aggregate_item.form == CM_AST_FIELDS_NAMED
        && value != NULL && value->visibility.kind == CM_AST_VIS_PUBLIC
        && ast_string_is(&ast, value->name, "value")
        && marker != NULL
        && marker->visibility.kind == CM_AST_VIS_INHERITED
        && ast_string_is(&ast, marker->name, "marker")
        && marker_type != NULL
        && marker_type->kind == CM_AST_TYPE_REFERENCE
        && ast_span_is(source, item->span,
            "pub union Slot<'a, T> where T: Copy { "
            "pub value: T, marker: &'a T, }")
        && ast_dump_contains(&ast, "(union public \"Slot\"")
        && ast_dump_contains(&ast, "(fields named");
    if (!ok) {
        fprintf(stderr, "union item AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u
            || strstr(result.first_error.message,
                "expected '{' after union header") == NULL) {
            fprintf(stderr, "invalid union form %lu was accepted: %s\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

static int test_default_specialization_items(void)
{
    static const char source[] =
        "struct Value; trait Marker {} "
        "default impl Marker for Value {} "
        "impl Value { default fn specialize() {} "
        "default type Output = u8; "
        "default const FLAG: bool = true; } "
        "default!(); fn default() {}";
    static const char *const broken[] = {
        "default fn misplaced() {}",
        "trait Marker { default fn misplaced(); }",
        "struct Value; impl Value { default default fn duplicate() {} }",
        "struct Value; impl Value { default unsafe struct Nested; }"
    };
    CmAst ast;
    CmParseResult result;
    const CmAstItemId *root_id;
    const CmAstItem *default_impl;
    const CmAstItem *inherent_impl;
    const CmAstItem *method;
    const CmAstItem *type;
    const CmAstItem *constant;
    const CmAstItem *macro;
    const CmAstItem *named_default;
    size_t index;
    int ok;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 2u);
    default_impl = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 3u);
    inherent_impl = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    method = inherent_impl == NULL
            || inherent_impl->data.impl_item.item_count != 3u
        ? NULL : cm_ast_get_item(&ast,
            inherent_impl->data.impl_item.items[0]);
    type = method == NULL ? NULL : cm_ast_get_item(&ast,
        inherent_impl->data.impl_item.items[1]);
    constant = type == NULL ? NULL : cm_ast_get_item(&ast,
        inherent_impl->data.impl_item.items[2]);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 4u);
    macro = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    root_id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, 5u);
    named_default = root_id == NULL ? NULL : cm_ast_get_item(&ast, *root_id);
    ok = result.error_count == 0u && ast.root_items.len == 6u
        && default_impl != NULL && default_impl->kind == CM_AST_ITEM_IMPL
        && default_impl->is_default == 1
        && inherent_impl != NULL && inherent_impl->kind == CM_AST_ITEM_IMPL
        && inherent_impl->is_default == 0
        && method != NULL && method->kind == CM_AST_ITEM_FUNCTION
        && method->is_default == 1
        && type != NULL && type->kind == CM_AST_ITEM_TYPE_ALIAS
        && type->is_default == 1
        && constant != NULL && constant->kind == CM_AST_ITEM_CONST
        && constant->is_default == 1
        && macro != NULL && macro->kind == CM_AST_ITEM_MACRO
        && macro->is_default == 0
        && named_default != NULL
        && named_default->kind == CM_AST_ITEM_FUNCTION
        && named_default->is_default == 0
        && ast_string_is(&ast, named_default->name, "default")
        && ast_dump_contains(&ast, "(specialization default)");
    if (!ok) {
        fprintf(stderr, "default specialization AST was incorrect: %s\n",
            result.first_error.message);
    }
    cm_ast_destroy(&ast);

    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index) {
        cm_ast_init(&ast);
        result = cm_parse_crate(&ast, broken[index], strlen(broken[index]),
            CM_EDITION_2024);
        if (result.error_count == 0u
            || (strstr(result.first_error.message,
                    "only permitted on an impl") == NULL
                && strstr(result.first_error.message,
                    "duplicate default") == NULL)) {
            fprintf(stderr,
                "invalid default specialization %lu was accepted: %s\n",
                (unsigned long)index, result.first_error.message);
            ok = 0;
        }
        cm_ast_destroy(&ast);
    }
    return ok;
}

int main(int argc, char **argv)
{
    unsigned char *source;
    size_t source_length;
    CmAst ast;
    CmParseResult result;
    FILE *dump;
    int ok;

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s SOURCE [EXPECTED]\n", argv[0]);
        return 2;
    }
    source = read_file(argv[1], &source_length);
    if (source == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, (const char *)source, source_length,
        CM_EDITION_2024);
    if (result.error_count != 0u) {
        fprintf(stderr, "%s:%lu:%lu: parse error: %s (%lu total)\n",
            argv[1], (unsigned long)result.first_error.line,
            (unsigned long)result.first_error.column,
            result.first_error.message, (unsigned long)result.error_count);
        cm_ast_destroy(&ast);
        free(source);
        return 1;
    }
    if (ast.root_items.len == 0u || ast.paths.len == 0u ||
        ast.types.len == 0u || ast.items.len < ast.root_items.len ||
        ast.patterns.len == 0u || ast.expressions.len == 0u) {
        fputs("AST ID tables or nested item ownership are incomplete\n", stderr);
        cm_ast_destroy(&ast);
        free(source);
        return 1;
    }
    if (argc == 2) {
        ok = cm_ast_dump(stdout, &ast);
    } else {
        dump = tmpfile();
        if (dump == NULL) {
            fputs("tmpfile failed\n", stderr);
            cm_ast_destroy(&ast);
            free(source);
            return 2;
        }
        ok = cm_ast_dump(dump, &ast) && compare_dump(dump, argv[2]);
        fclose(dump);
    }
    cm_ast_destroy(&ast);
    free(source);
    ok = ok && test_error_path() && test_macro_error_paths()
        && test_macro_lifetime_signature_capture()
        && test_generic_default_error_paths()
        && test_structured_generic_parameter_bounds()
        && test_generic_parameter_attributes()
        && test_nested_associated_type_constraints()
        && test_generic_associated_type_constraint_names()
        && test_lifetime_generic_parameter_bounds()
        && test_post_value_type_alias_where_clause()
        && test_post_value_associated_type_where_clause()
        && test_generic_parameter_bound_error_paths()
        && test_nested_generic_associated_type_constraint()
        && test_relaxed_sized_generic_parameter_bounds()
        && test_conditionally_const_generic_parameter_bounds()
        && test_invalid_impl_prefixes()
        && test_const_impl_modifiers()
        && test_explicit_safe_foreign_function()
        && test_const_default_comparisons()
        && test_projection_error_paths()
        && test_nested_projection_paths()
        && test_structured_supertraits()
        && test_trait_alias_items()
        && test_auto_trait_items()
        && test_lifetime_trait_bounds()
        && test_structured_where_predicates()
        && test_higher_ranked_where_bound()
        && test_higher_ranked_where_predicate()
        && test_lifetime_where_predicates()
        && test_where_predicate_error_paths()
        && test_supertrait_error_paths()
        && test_structured_associated_type_bounds()
        && test_associated_type_bound_error_paths()
        && test_block_local_const_item()
        && test_block_local_static_item()
        && test_block_local_function_item()
        && test_block_local_trait_item()
        && test_block_local_use_item()
        && test_block_local_struct_and_impl_items()
        && test_block_local_unsafe_extern_item()
        && test_block_local_union_item()
        && test_block_local_macro_rules_item()
        && test_const_block_discrimination()
        && test_attributed_tail_expression()
        && test_attributed_call_argument()
        && test_attributed_let_initializer()
        && test_attributed_let_statement()
        && test_attributed_enum_variants()
        && test_attributed_match_arm()
        && test_structured_match_let_guard()
        && test_chained_let_condition()
        && test_labeled_loop_expression()
        && test_or_pattern_with_range_alternatives()
        && test_omitted_start_range_pattern()
        && test_binding_rest_subpattern()
        && test_nested_reference_pattern_token_splitting()
        && test_block_match_arm_without_comma()
        && test_nonblock_match_arm_requires_comma()
        && test_method_turbofish_arguments()
        && test_structured_impl_trait_type()
        && test_parenthesized_and_singleton_tuple_types()
        && test_conditionally_const_impl_trait_bounds()
        && test_unsafe_function_pointer_type()
        && test_higher_ranked_impl_trait_type()
        && test_impl_trait_lifetime_bound()
        && test_zero_parameter_closure_with_never_turbofish()
        && test_structured_dyn_trait_type()
        && test_relaxed_impl_trait_type_bound()
        && test_lifetime_qualified_self_receiver()
        && test_postfix_try_expression()
        && test_try_block_expression()
        && test_tuple_index_projection()
        && test_qualified_expression_path()
        && test_nested_qualified_expression_path()
        && test_raw_reference_expressions()
        && test_nested_reference_type_token_splitting()
        && test_let_else_statement()
        && test_attributed_struct_expression_field()
        && test_union_items()
        && test_default_specialization_items();
    if (ok) {
        puts("syntax parser tests: ok");
    }
    return ok ? 0 : 1;
}
