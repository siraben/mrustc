#include "cm/syntax/ast.h"
#include "cm/syntax/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum DumpMode {
    DUMP_EXACT = 0,
    DUMP_SEMANTIC
};

enum TypeFeature {
    TYPE_INFER = 0, TYPE_NEVER, TYPE_PATH, TYPE_REFERENCE, TYPE_POINTER,
    TYPE_TUPLE, TYPE_SLICE, TYPE_ARRAY, TYPE_FUNCTION, TYPE_OTHER,
    TYPE_FEATURE_COUNT
};

enum ExprFeature {
    EXPR_LITERAL = 0, EXPR_BLOCK, EXPR_CALL, EXPR_METHOD, EXPR_FIELD,
    EXPR_INDEX, EXPR_UNARY, EXPR_BINARY, EXPR_ASSIGN, EXPR_CAST, EXPR_RANGE,
    EXPR_RETURN, EXPR_BREAK, EXPR_CONTINUE, EXPR_IF, EXPR_MATCH, EXPR_LOOP,
    EXPR_WHILE, EXPR_FOR, EXPR_CLOSURE, EXPR_TUPLE, EXPR_ARRAY, EXPR_STRUCT,
    EXPR_LET, EXPR_FEATURE_COUNT
};

enum PatternFeature {
    PAT_WILDCARD = 0, PAT_BINDING, PAT_LITERAL, PAT_PATH, PAT_REFERENCE,
    PAT_TUPLE, PAT_STRUCT, PAT_SLICE, PAT_OR, PAT_RANGE, PAT_FEATURE_COUNT
};

enum ItemFeature {
    ITEM_FUNCTION = 0, ITEM_STRUCT, ITEM_ENUM, ITEM_TYPE, ITEM_CONST,
    ITEM_STATIC, ITEM_MODULE, ITEM_USE, ITEM_EXTERN_CRATE, ITEM_EXTERN_BLOCK,
    ITEM_TRAIT, ITEM_IMPL, ITEM_UNION, ITEM_FEATURE_COUNT
};

typedef struct Features {
    unsigned long items[ITEM_FEATURE_COUNT];
    unsigned long types[TYPE_FEATURE_COUNT];
    unsigned long expressions[EXPR_FEATURE_COUNT];
    unsigned long patterns[PAT_FEATURE_COUNT];
    unsigned long generic_types;
    unsigned long generic_lifetimes;
    unsigned long generic_consts;
    unsigned long functions_const;
    unsigned long functions_async;
    unsigned long functions_unsafe;
    unsigned long functions_with_body;
} Features;

static const char *const item_names[ITEM_FEATURE_COUNT] = {
    "function", "struct", "enum", "type", "const", "static", "module",
    "use", "extern-crate", "extern-block", "trait", "impl", "union"
};
static const char *const type_names[TYPE_FEATURE_COUNT] = {
    "infer", "never", "path", "reference", "pointer", "tuple", "slice",
    "array", "function", "other"
};
static const char *const expr_names[EXPR_FEATURE_COUNT] = {
    "literal", "block", "call", "method", "field", "index", "unary",
    "binary", "assign", "cast", "range", "return", "break", "continue",
    "if", "match", "loop", "while", "for", "closure", "tuple", "array",
    "struct", "let"
};
static const char *const pattern_names[PAT_FEATURE_COUNT] = {
    "wildcard", "binding", "literal", "path", "reference", "tuple",
    "struct", "slice", "or", "range"
};

static unsigned char *read_file(const char *path, size_t *length_out)
{
    FILE *stream;
    long end;
    unsigned char *bytes;
    size_t length;

    stream = fopen(path, "rb");
    if (stream == NULL) return NULL;
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
    fclose(stream);
    bytes[length] = 0u;
    *length_out = length;
    return bytes;
}

static enum cm_edition parse_edition(const char *text)
{
    if (strcmp(text, "2015") == 0) return CM_EDITION_2015;
    if (strcmp(text, "2018") == 0) return CM_EDITION_2018;
    if (strcmp(text, "2021") == 0) return CM_EDITION_2021;
    if (strcmp(text, "2024") == 0) return CM_EDITION_2024;
    fprintf(stderr, "unsupported edition: %s\n", text);
    exit(2);
    return CM_EDITION_2015;
}

static enum DumpMode parse_mode(const char *text)
{
    if (strcmp(text, "exact") == 0) return DUMP_EXACT;
    if (strcmp(text, "semantic") == 0) return DUMP_SEMANTIC;
    fprintf(stderr, "unsupported mode: %s\n", text);
    exit(2);
    return DUMP_EXACT;
}

static void print_string(const CmAst *ast, CmInternId id)
{
    const CmInternedString *string;

    string = cm_ast_get_string(ast, id);
    if (string == NULL) {
        fputc('_', stdout);
        return;
    }
    (void)fwrite(string->bytes, 1u, string->len, stdout);
}

static const char *visibility_name(CmAstVisibilityKind kind)
{
    switch (kind) {
    case CM_AST_VIS_INHERITED: return "private";
    case CM_AST_VIS_PUBLIC: return "public";
    case CM_AST_VIS_CRATE: return "crate";
    case CM_AST_VIS_SELF: return "self";
    case CM_AST_VIS_SUPER: return "super";
    case CM_AST_VIS_RESTRICTED: return "restricted";
    }
    return "unknown";
}

static void print_path(const CmAst *ast, CmAstPathId id)
{
    const CmAstPath *path;
    uint32_t index;

    path = cm_ast_get_path(ast, id);
    if (path == NULL) {
        fputc('_', stdout);
        return;
    }
    if (path->absolute) fputs("::", stdout);
    for (index = 0u; index < path->segment_count; ++index) {
        if (index != 0u) fputs("::", stdout);
        print_string(ast, path->segments[index].name);
    }
}

static void count_type(const CmAst *ast, CmAstTypeId id, Features *features);

static enum TypeFeature type_feature(CmAstTypeKind kind)
{
    switch (kind) {
    case CM_AST_TYPE_INFER: return TYPE_INFER;
    case CM_AST_TYPE_NEVER: return TYPE_NEVER;
    case CM_AST_TYPE_PATH: return TYPE_PATH;
    case CM_AST_TYPE_REFERENCE: return TYPE_REFERENCE;
    case CM_AST_TYPE_POINTER: return TYPE_POINTER;
    case CM_AST_TYPE_TUPLE: return TYPE_TUPLE;
    case CM_AST_TYPE_SLICE: return TYPE_SLICE;
    case CM_AST_TYPE_ARRAY: return TYPE_ARRAY;
    case CM_AST_TYPE_FUNCTION: return TYPE_FUNCTION;
    case CM_AST_TYPE_IMPL_TRAIT: return TYPE_OTHER;
    case CM_AST_TYPE_DYN_TRAIT: return TYPE_OTHER;
    case CM_AST_TYPE_MACRO: return TYPE_OTHER;
    case CM_AST_TYPE_PROJECTION:
        /* Upstream represents an associated projection as a path type. */
        return TYPE_PATH;
    case CM_AST_TYPE_OTHER: return TYPE_OTHER;
    }
    return TYPE_OTHER;
}

static void print_type(const CmAst *ast, CmAstTypeId id)
{
    const CmAstType *type;
    uint32_t index;

    type = cm_ast_get_type(ast, id);
    if (type == NULL) {
        fputc('_', stdout);
        return;
    }
    switch (type->kind) {
    case CM_AST_TYPE_INFER: fputc('_', stdout); break;
    case CM_AST_TYPE_NEVER: fputc('!', stdout); break;
    case CM_AST_TYPE_PATH:
        fputs("path(", stdout); print_path(ast, type->path); fputc(')', stdout);
        break;
    case CM_AST_TYPE_REFERENCE:
        fputs(type->is_mutable ? "ref(mut," : "ref(shared,", stdout);
        print_type(ast, type->child); fputc(')', stdout);
        break;
    case CM_AST_TYPE_POINTER:
        fputs(type->is_mutable ? "ptr(mut," : "ptr(const,", stdout);
        print_type(ast, type->child); fputc(')', stdout);
        break;
    case CM_AST_TYPE_TUPLE:
        fputs("tuple(", stdout);
        for (index = 0u; index < type->element_count; ++index) {
            if (index != 0u) fputc(',', stdout);
            print_type(ast, type->elements[index]);
        }
        fputc(')', stdout);
        break;
    case CM_AST_TYPE_SLICE:
        fputs("slice(", stdout); print_type(ast, type->child); fputc(')', stdout);
        break;
    case CM_AST_TYPE_ARRAY:
        fputs("array(", stdout); print_type(ast, type->child); fputc(')', stdout);
        break;
    case CM_AST_TYPE_FUNCTION:
        if (type->binder.lifetime_count != 0u) {
            fputs("for<", stdout);
            for (index = 0u; index < type->binder.lifetime_count; ++index) {
                if (index != 0u) fputc(',', stdout);
                print_string(ast, type->binder.lifetimes[index]);
            }
            fputs(">;", stdout);
        }
        fputs(type->is_unsafe ? "fn(unsafe;" : "fn(safe;", stdout);
        for (index = 0u; index < type->element_count; ++index) {
            if (index != 0u) fputc(',', stdout);
            print_type(ast, type->elements[index]);
        }
        fputs("->", stdout); print_type(ast, type->child); fputc(')', stdout);
        break;
    case CM_AST_TYPE_IMPL_TRAIT:
        fputs("erased-type", stdout);
        break;
    case CM_AST_TYPE_DYN_TRAIT:
        fputs("trait-object", stdout);
        break;
    case CM_AST_TYPE_PROJECTION:
        /* Match upstream's parsed-AST normalization of UFCS projections. */
        fputs("path(", stdout);
        print_string(ast, type->projection.associated.name);
        fputc(')', stdout);
        break;
    case CM_AST_TYPE_OTHER:
    case CM_AST_TYPE_MACRO:
        fputs("other", stdout);
        break;
    }
}

static void count_type(const CmAst *ast, CmAstTypeId id, Features *features)
{
    const CmAstType *type;
    uint32_t index;

    type = cm_ast_get_type(ast, id);
    if (type == NULL) return;
    if (type->kind == CM_AST_TYPE_INFER) return;
    if (type->kind == CM_AST_TYPE_TUPLE && type->element_count == 0u) return;
    features->types[(unsigned int)type_feature(type->kind)] += 1u;
    switch (type->kind) {
    case CM_AST_TYPE_REFERENCE:
    case CM_AST_TYPE_POINTER:
    case CM_AST_TYPE_SLICE:
    case CM_AST_TYPE_ARRAY:
        count_type(ast, type->child, features);
        break;
    case CM_AST_TYPE_TUPLE:
    case CM_AST_TYPE_FUNCTION:
        for (index = 0u; index < type->element_count; ++index)
            count_type(ast, type->elements[index], features);
        if (type->kind == CM_AST_TYPE_FUNCTION)
            count_type(ast, type->child, features);
        break;
    case CM_AST_TYPE_PROJECTION:
        /* The common schema treats the complete UFCS type as one path. */
        break;
    case CM_AST_TYPE_INFER:
    case CM_AST_TYPE_NEVER:
    case CM_AST_TYPE_PATH:
    case CM_AST_TYPE_IMPL_TRAIT:
    case CM_AST_TYPE_DYN_TRAIT:
    case CM_AST_TYPE_OTHER:
    case CM_AST_TYPE_MACRO:
        break;
    }
}

static enum PatternFeature pattern_feature(CmAstPatternKind kind)
{
    switch (kind) {
    case CM_AST_PATTERN_WILDCARD: return PAT_WILDCARD;
    case CM_AST_PATTERN_BINDING: return PAT_BINDING;
    case CM_AST_PATTERN_LITERAL: return PAT_LITERAL;
    case CM_AST_PATTERN_PATH: return PAT_PATH;
    case CM_AST_PATTERN_REFERENCE: return PAT_REFERENCE;
    case CM_AST_PATTERN_TUPLE: return PAT_TUPLE;
    case CM_AST_PATTERN_STRUCT: return PAT_STRUCT;
    case CM_AST_PATTERN_SLICE: return PAT_SLICE;
    case CM_AST_PATTERN_OR: return PAT_OR;
    case CM_AST_PATTERN_RANGE: return PAT_RANGE;
    case CM_AST_PATTERN_REST:
        /* Upstream records rest within the containing tuple/slice pattern. */
        return PAT_FEATURE_COUNT;
    }
    return PAT_FEATURE_COUNT;
}

static const char *pattern_kind_name(const CmAst *ast, CmAstPatternId id)
{
    const CmAstPattern *pattern;

    pattern = cm_ast_get_pattern(ast, id);
    if (pattern == NULL) return "none";
    if (pattern->kind == CM_AST_PATTERN_REST) return "none";
    return pattern_names[(unsigned int)pattern_feature(pattern->kind)];
}

static void count_pattern(const CmAst *ast, CmAstPatternId id,
    Features *features)
{
    const CmAstPattern *pattern;
    uint32_t index;

    pattern = cm_ast_get_pattern(ast, id);
    if (pattern == NULL) return;
    if (pattern->kind == CM_AST_PATTERN_REST) return;
    features->patterns[(unsigned int)pattern_feature(pattern->kind)] += 1u;
    switch (pattern->kind) {
    case CM_AST_PATTERN_BINDING:
        count_pattern(ast, pattern->data.binding.subpattern, features);
        break;
    case CM_AST_PATTERN_REFERENCE:
        count_pattern(ast, pattern->data.reference.pattern, features);
        break;
    case CM_AST_PATTERN_TUPLE:
    case CM_AST_PATTERN_SLICE:
    case CM_AST_PATTERN_OR:
        for (index = 0u; index < pattern->data.list.pattern_count; ++index)
            count_pattern(ast, pattern->data.list.patterns[index], features);
        break;
    case CM_AST_PATTERN_STRUCT:
        for (index = 0u; index < pattern->data.struct_pattern.field_count;
             ++index)
            count_pattern(ast, pattern->data.struct_pattern.fields[index].pattern,
                features);
        break;
    case CM_AST_PATTERN_RANGE:
        count_pattern(ast, pattern->data.range.start, features);
        count_pattern(ast, pattern->data.range.end, features);
        break;
    case CM_AST_PATTERN_WILDCARD:
    case CM_AST_PATTERN_LITERAL:
    case CM_AST_PATTERN_PATH:
    case CM_AST_PATTERN_REST:
        break;
    }
}

static void count_expr(const CmAst *ast, CmAstExprId id, Features *features);

static void count_expr_list(const CmAst *ast, const CmAstExprId *ids,
    uint32_t count, Features *features)
{
    uint32_t index;
    for (index = 0u; index < count; ++index)
        count_expr(ast, ids[index], features);
}

static void count_expr(const CmAst *ast, CmAstExprId id, Features *features)
{
    const CmAstExpr *expression;
    uint32_t index;

    expression = cm_ast_get_expr(ast, id);
    if (expression == NULL) return;
    switch (expression->kind) {
    case CM_AST_EXPR_LITERAL: features->expressions[EXPR_LITERAL] += 1u; break;
    case CM_AST_EXPR_PATH:
    case CM_AST_EXPR_QUALIFIED_PATH:
        break;
    case CM_AST_EXPR_BLOCK:
        features->expressions[EXPR_BLOCK] += 1u;
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            const CmAstStmt *statement;
            statement = cm_ast_get_stmt(ast,
                expression->data.block.statements[index]);
            if (statement == NULL) continue;
            if (statement->kind == CM_AST_STMT_LET) {
                features->expressions[EXPR_LET] += 1u;
                count_pattern(ast, statement->data.let_stmt.pattern, features);
                count_type(ast, statement->data.let_stmt.type, features);
                count_expr(ast, statement->data.let_stmt.initializer, features);
            } else {
                count_expr(ast, statement->data.expr_stmt.expression, features);
            }
        }
        count_expr(ast, expression->data.block.tail, features);
        break;
    case CM_AST_EXPR_CALL:
        features->expressions[EXPR_CALL] += 1u;
        count_expr(ast, expression->data.call.callee, features);
        count_expr_list(ast, expression->data.call.arguments,
            expression->data.call.argument_count, features);
        break;
    case CM_AST_EXPR_METHOD_CALL:
        features->expressions[EXPR_METHOD] += 1u;
        count_expr(ast, expression->data.method_call.receiver, features);
        count_expr_list(ast, expression->data.method_call.arguments,
            expression->data.method_call.argument_count, features);
        break;
    case CM_AST_EXPR_FIELD:
    case CM_AST_EXPR_TUPLE_FIELD:
        features->expressions[EXPR_FIELD] += 1u;
        count_expr(ast, expression->kind == CM_AST_EXPR_FIELD ?
            expression->data.field.base : expression->data.tuple_field.base,
            features);
        break;
    case CM_AST_EXPR_INDEX:
        features->expressions[EXPR_INDEX] += 1u;
        count_expr(ast, expression->data.index.base, features);
        count_expr(ast, expression->data.index.index, features);
        break;
    case CM_AST_EXPR_UNARY:
        features->expressions[EXPR_UNARY] += 1u;
        count_expr(ast, expression->data.unary.operand, features);
        break;
    case CM_AST_EXPR_BINARY:
    case CM_AST_EXPR_ASSIGN:
        features->expressions[expression->kind == CM_AST_EXPR_ASSIGN ?
            EXPR_ASSIGN : EXPR_BINARY] += 1u;
        count_expr(ast, expression->data.binary.left, features);
        count_expr(ast, expression->data.binary.right, features);
        break;
    case CM_AST_EXPR_CAST:
        features->expressions[EXPR_CAST] += 1u;
        count_expr(ast, expression->data.cast.value, features);
        count_type(ast, expression->data.cast.type, features);
        break;
    case CM_AST_EXPR_TRY:
    case CM_AST_EXPR_RAW_REFERENCE:
        features->expressions[EXPR_UNARY] += 1u;
        count_expr(ast, expression->kind == CM_AST_EXPR_TRY ?
            expression->data.try_expr.operand :
            expression->data.raw_reference.operand, features);
        break;
    case CM_AST_EXPR_TRY_BLOCK:
        /* Upstream's try wrapper visits, but does not classify, its block. */
        count_expr(ast, expression->data.try_expr.operand, features);
        break;
    case CM_AST_EXPR_RANGE:
        features->expressions[EXPR_RANGE] += 1u;
        count_expr(ast, expression->data.range.start, features);
        count_expr(ast, expression->data.range.end, features);
        break;
    case CM_AST_EXPR_LET:
        features->expressions[EXPR_LET] += 1u;
        count_pattern(ast, expression->data.let_expr.pattern, features);
        count_expr(ast, expression->data.let_expr.initializer, features);
        break;
    case CM_AST_EXPR_RETURN:
    case CM_AST_EXPR_BREAK:
    case CM_AST_EXPR_CONTINUE:
        features->expressions[expression->kind == CM_AST_EXPR_RETURN ?
            EXPR_RETURN : (expression->kind == CM_AST_EXPR_BREAK ?
            EXPR_BREAK : EXPR_CONTINUE)] += 1u;
        count_expr(ast, expression->data.flow.value, features);
        break;
    case CM_AST_EXPR_IF:
        features->expressions[EXPR_IF] += 1u;
        if (expression->data.if_expr.pattern != CM_AST_PATTERN_NONE)
            features->expressions[EXPR_LET] += 1u;
        count_pattern(ast, expression->data.if_expr.pattern, features);
        count_expr(ast, expression->data.if_expr.condition, features);
        count_expr(ast, expression->data.if_expr.then_expr, features);
        count_expr(ast, expression->data.if_expr.else_expr, features);
        break;
    case CM_AST_EXPR_MATCH:
        features->expressions[EXPR_MATCH] += 1u;
        count_expr(ast, expression->data.match_expr.scrutinee, features);
        for (index = 0u; index < expression->data.match_expr.arm_count; ++index) {
            const CmAstMatchArm *arm;
            arm = &expression->data.match_expr.arms[index];
            count_pattern(ast, arm->pattern, features);
            if (arm->guard_pattern != CM_AST_PATTERN_NONE) {
                features->expressions[EXPR_LET] += 1u;
                count_pattern(ast, arm->guard_pattern, features);
                count_expr(ast, arm->guard_initializer, features);
            } else {
                count_expr(ast, arm->guard, features);
            }
            count_expr(ast, arm->body, features);
        }
        break;
    case CM_AST_EXPR_LOOP:
        features->expressions[EXPR_LOOP] += 1u;
        count_expr(ast, expression->data.loop_expr.body, features);
        break;
    case CM_AST_EXPR_WHILE:
        features->expressions[EXPR_WHILE] += 1u;
        if (expression->data.while_expr.pattern != CM_AST_PATTERN_NONE)
            features->expressions[EXPR_LET] += 1u;
        count_pattern(ast, expression->data.while_expr.pattern, features);
        count_expr(ast, expression->data.while_expr.condition, features);
        count_expr(ast, expression->data.while_expr.body, features);
        break;
    case CM_AST_EXPR_FOR:
        features->expressions[EXPR_FOR] += 1u;
        count_pattern(ast, expression->data.for_expr.pattern, features);
        count_expr(ast, expression->data.for_expr.iterable, features);
        count_expr(ast, expression->data.for_expr.body, features);
        break;
    case CM_AST_EXPR_CLOSURE:
        features->expressions[EXPR_CLOSURE] += 1u;
        for (index = 0u; index < expression->data.closure.parameter_count;
             ++index) {
            count_pattern(ast, expression->data.closure.parameters[index].pattern,
                features);
            count_type(ast, expression->data.closure.parameters[index].type,
                features);
        }
        count_type(ast, expression->data.closure.return_type, features);
        count_expr(ast, expression->data.closure.body, features);
        break;
    case CM_AST_EXPR_TUPLE:
    case CM_AST_EXPR_ARRAY:
        features->expressions[expression->kind == CM_AST_EXPR_TUPLE ?
            EXPR_TUPLE : EXPR_ARRAY] += 1u;
        if (expression->kind == CM_AST_EXPR_ARRAY &&
            expression->data.list.repeat_length != 0u) {
            count_expr(ast, expression->data.list.repeat_value, features);
            count_expr(ast, expression->data.list.repeat_length, features);
        } else {
            count_expr_list(ast, expression->data.list.elements,
                expression->data.list.element_count, features);
        }
        break;
    case CM_AST_EXPR_STRUCT:
        features->expressions[EXPR_STRUCT] += 1u;
        for (index = 0u; index < expression->data.struct_expr.field_count;
             ++index)
            count_expr(ast, expression->data.struct_expr.fields[index].value,
                features);
        count_expr(ast, expression->data.struct_expr.base, features);
        break;
    case CM_AST_EXPR_MACRO:
        /* Expansion-sensitive syntax is intentionally absent from v1 facts. */
        break;
    }
}

static enum ItemFeature item_feature(CmAstItemKind kind)
{
    return kind == CM_AST_ITEM_UNION ? ITEM_UNION : (enum ItemFeature)kind;
}

static const char *field_form_name(CmAstFieldForm form)
{
    switch (form) {
    case CM_AST_FIELDS_UNIT: return "unit";
    case CM_AST_FIELDS_TUPLE: return "tuple";
    case CM_AST_FIELDS_NAMED: return "named";
    }
    return "unknown";
}

static void count_generics(const CmAstItem *item, Features *features)
{
    uint32_t index;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        switch (item->generic_parameters[index].kind) {
        case CM_AST_PARAM_TYPE: features->generic_types += 1u; break;
        case CM_AST_PARAM_LIFETIME: features->generic_lifetimes += 1u; break;
        case CM_AST_PARAM_CONST: features->generic_consts += 1u; break;
        }
    }
}

static void print_features(const Features *features, const char *prefix)
{
    unsigned int index;
    for (index = 0u; index < EXPR_FEATURE_COUNT; ++index)
        if (features->expressions[index] != 0u)
            printf("%s\texpr.%s=%lu\n", prefix, expr_names[index],
                features->expressions[index]);
    for (index = 0u; index < PAT_FEATURE_COUNT; ++index)
        if (features->patterns[index] != 0u)
            printf("%s\tpattern.%s=%lu\n", prefix, pattern_names[index],
                features->patterns[index]);
}

static void visit_item(const CmAst *ast, CmAstItemId id, unsigned int depth,
    enum DumpMode mode, Features *all)
{
    const CmAstItem *item;
    enum ItemFeature feature;
    uint32_t index;

    item = cm_ast_get_item(ast, id);
    if (item == NULL) return;
    if (item->kind == CM_AST_ITEM_MACRO) {
        if (mode == DUMP_EXACT) {
            printf("item\t%u\tmacro\t%s\t", depth,
                visibility_name(item->visibility.kind));
            print_string(ast, item->name);
            fputs("\tpath=", stdout);
            print_path(ast, item->data.macro_item.path);
            printf("\tdelimiter=%u\tsemicolon=%d\n",
                (unsigned int)item->data.macro_item.delimiter,
                item->data.macro_item.has_semicolon);
        }
        return;
    }
    feature = item_feature(item->kind);
    all->items[(unsigned int)feature] += 1u;
    count_generics(item, all);
    if (mode == DUMP_EXACT) {
        printf("item\t%u\t%s\t%s\t", depth, item_names[(unsigned int)feature],
            visibility_name(item->visibility.kind));
        print_string(ast, item->name);
        printf("\tgenerics=%lu\n", (unsigned long)item->generic_parameter_count);
        if (item->is_default) printf("specialization\t%u\tdefault\n", depth);
    }
    switch (item->kind) {
    case CM_AST_ITEM_FUNCTION: {
        const CmAstFunction *function;
        Features local;
        function = &item->data.function_item;
        memset(&local, 0, sizeof(local));
        all->functions_const += (unsigned long)function->is_const;
        all->functions_async += (unsigned long)function->is_async;
        all->functions_unsafe += (unsigned long)function->is_unsafe;
        all->functions_with_body +=
            (unsigned long)(function->body != CM_AST_EXPR_NONE);
        for (index = 0u; index < function->parameter_count; ++index) {
            const CmAstFunctionParam *parameter;
            parameter = &function->parameters[index];
            if (!parameter->is_self) {
                count_pattern(ast, parameter->pattern, all);
                count_type(ast, parameter->type, all);
            }
            if (mode == DUMP_EXACT) {
                printf("param\t%u\tself=%d\t%s\t", depth,
                    parameter->is_self, pattern_kind_name(ast,
                    parameter->pattern));
                print_type(ast, parameter->type);
                fputc('\n', stdout);
            }
        }
        count_type(ast, function->return_type, all);
        count_expr(ast, function->body, all);
        count_expr(ast, function->body, &local);
        if (mode == DUMP_EXACT) {
            printf("function\t%u\tconst=%d\tasync=%d\tunsafe=%d\tabi=",
                depth, function->is_const, function->is_async,
                function->is_unsafe);
            if (function->abi == CM_INTERN_ID_NONE) fputs("Rust", stdout);
            else print_string(ast, function->abi);
            printf("\tparams=%lu\tbody=%d\treturn=",
                (unsigned long)function->parameter_count,
                function->body != CM_AST_EXPR_NONE);
            print_type(ast, function->return_type);
            fputc('\n', stdout);
            print_features(&local, "body");
        }
        break;
    }
    case CM_AST_ITEM_STRUCT:
    case CM_AST_ITEM_UNION:
        if (mode == DUMP_EXACT)
            printf("aggregate\t%u\t%s\tfields=%lu\n", depth,
                field_form_name(item->data.aggregate_item.form),
                (unsigned long)item->data.aggregate_item.field_count);
        for (index = 0u; index < item->data.aggregate_item.field_count;
             ++index) {
            const CmAstField *field = &item->data.aggregate_item.fields[index];
            count_type(ast, field->type, all);
            if (mode == DUMP_EXACT) {
                printf("field\t%u\t%s\t", depth,
                    visibility_name(field->visibility.kind));
                print_string(ast, field->name); fputc('\t', stdout);
                print_type(ast, field->type); fputc('\n', stdout);
            }
        }
        break;
    case CM_AST_ITEM_ENUM:
        for (index = 0u; index < item->data.enum_item.variant_count; ++index) {
            const CmAstVariant *variant = &item->data.enum_item.variants[index];
            uint32_t field_index;
            if (mode == DUMP_EXACT) {
                printf("variant\t%u\t%s\t", depth,
                    field_form_name(variant->form));
                print_string(ast, variant->name);
                printf("\tfields=%lu\tdiscriminant=%d\n",
                    (unsigned long)variant->field_count,
                    variant->discriminant != CM_INTERN_ID_NONE);
            }
            for (field_index = 0u; field_index < variant->field_count;
                 ++field_index)
                count_type(ast, variant->fields[field_index].type, all);
        }
        break;
    case CM_AST_ITEM_TYPE_ALIAS:
    case CM_AST_ITEM_CONST:
    case CM_AST_ITEM_STATIC:
        count_type(ast, item->data.value_item.type, all);
        count_expr(ast, item->data.value_item.initializer, all);
        if (mode == DUMP_EXACT) {
            printf("value\t%u\tmutable=%d\tinitialized=%d\ttype=", depth,
                item->data.value_item.is_mutable,
                item->data.value_item.has_value);
            print_type(ast, item->data.value_item.type); fputc('\n', stdout);
        }
        break;
    case CM_AST_ITEM_MODULE:
        if (mode == DUMP_EXACT)
            printf("module\t%u\tinline=%d\titems=%lu\n", depth,
                item->data.module_item.is_inline,
                (unsigned long)item->data.module_item.item_count);
        for (index = 0u; index < item->data.module_item.item_count; ++index)
            visit_item(ast, item->data.module_item.items[index], depth + 1u,
                mode, all);
        break;
    case CM_AST_ITEM_EXTERN_BLOCK:
        for (index = 0u; index < item->data.extern_block_item.item_count;
             ++index)
            visit_item(ast, item->data.extern_block_item.items[index],
                depth + 1u, mode, all);
        break;
    case CM_AST_ITEM_TRAIT:
        if (mode == DUMP_EXACT) {
            printf("trait\t%u\tunsafe=%d\talias=%d\titems=%lu"
                "\talias-bounds=%lu\n", depth,
                item->data.trait_item.is_unsafe,
                item->data.trait_item.is_alias,
                (unsigned long)item->data.trait_item.item_count,
                (unsigned long)item->data.trait_item
                    .structured_alias_bound_count);
        }
        for (index = 0u;
             index < item->data.trait_item.structured_supertrait_count;
             ++index) {
            if (item->data.trait_item.structured_supertraits[index].kind
                    == CM_AST_SUPERTRAIT_TRAIT) {
                count_type(ast,
                    item->data.trait_item.structured_supertraits[index].type,
                    all);
            }
        }
        for (index = 0u;
             index < item->data.trait_item.structured_alias_bound_count;
             ++index) {
            if (item->data.trait_item.structured_alias_bounds[index].kind
                    == CM_AST_SUPERTRAIT_TRAIT) {
                count_type(ast,
                    item->data.trait_item.structured_alias_bounds[index].type,
                    all);
            }
        }
        for (index = 0u; index < item->data.trait_item.item_count; ++index)
            visit_item(ast, item->data.trait_item.items[index], depth + 1u,
                mode, all);
        break;
    case CM_AST_ITEM_IMPL:
        count_type(ast, item->data.impl_item.trait_type, all);
        count_type(ast, item->data.impl_item.self_type, all);
        for (index = 0u; index < item->data.impl_item.item_count; ++index)
            visit_item(ast, item->data.impl_item.items[index], depth + 1u,
                mode, all);
        break;
    case CM_AST_ITEM_USE:
    case CM_AST_ITEM_EXTERN_CRATE:
        break;
    case CM_AST_ITEM_MACRO:
        break;
    }
}

static void print_semantic(const Features *features)
{
    unsigned int index;
    puts("schema\tcmrustc-ast-facts-v1");
    for (index = 0u; index < ITEM_FEATURE_COUNT; ++index)
        printf("item.%s\t%lu\n", item_names[index], features->items[index]);
    for (index = 0u; index < TYPE_FEATURE_COUNT; ++index)
        printf("type.%s\t%lu\n", type_names[index], features->types[index]);
    for (index = 0u; index < EXPR_FEATURE_COUNT; ++index)
        printf("expr.%s\t%lu\n", expr_names[index],
            features->expressions[index]);
    for (index = 0u; index < PAT_FEATURE_COUNT; ++index)
        printf("pattern.%s\t%lu\n", pattern_names[index],
            features->patterns[index]);
    printf("generic.type\t%lu\n", features->generic_types);
    printf("generic.lifetime\t%lu\n", features->generic_lifetimes);
    printf("generic.const\t%lu\n", features->generic_consts);
    printf("function.const\t%lu\n", features->functions_const);
    printf("function.async\t%lu\n", features->functions_async);
    printf("function.unsafe\t%lu\n", features->functions_unsafe);
    printf("function.body\t%lu\n", features->functions_with_body);
}

int main(int argc, char **argv)
{
    unsigned char *source;
    size_t source_length;
    enum DumpMode mode;
    CmAst ast;
    CmParseResult result;
    Features features;
    size_t index;

    if (argc != 4) {
        fprintf(stderr, "usage: %s MODE EDITION FILE\n", argv[0]);
        return 2;
    }
    mode = parse_mode(argv[1]);
    source = read_file(argv[3], &source_length);
    if (source == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[3]);
        return 2;
    }
    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, (const char *)source, source_length,
        parse_edition(argv[2]));
    if (result.error_count != 0u) {
        fprintf(stderr, "%s:%lu:%lu: %s (%lu errors)\n", argv[3],
            (unsigned long)result.first_error.line,
            (unsigned long)result.first_error.column,
            result.first_error.message, (unsigned long)result.error_count);
        cm_ast_destroy(&ast);
        free(source);
        return 1;
    }
    memset(&features, 0, sizeof(features));
    if (mode == DUMP_EXACT) puts("schema\tcmrustc-ast-facts-v1");
    for (index = 0u; index < ast.root_items.len; ++index) {
        const CmAstItemId *id;
        id = (const CmAstItemId *)cm_vec_at_const(&ast.root_items, index);
        if (id != NULL) visit_item(&ast, *id, 0u, mode, &features);
    }
    if (mode == DUMP_SEMANTIC) print_semantic(&features);
    cm_ast_destroy(&ast);
    free(source);
    return ferror(stdout) == 0 ? 0 : 1;
}
