#include "cm/syntax/ast.h"

#include "cm/alloc.h"

#include <string.h>

static const void *cm_ast_get_vector_id(const CmVec *vector, uint32_t id)
{
    if (id == 0u || (size_t)id > vector->len) {
        return NULL;
    }
    return cm_vec_at_const(vector, (size_t)id - 1u);
}

static uint32_t cm_ast_push_id(CmVec *vector, const void *value)
{
    if (vector->len >= (size_t)UINT32_MAX) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    (void)cm_vec_push(vector, value);
    return (uint32_t)vector->len;
}

void cm_ast_init(CmAst *ast)
{
    memset(ast, 0, sizeof(*ast));
    cm_arena_init(&ast->storage, 4096u);
    cm_interner_init(&ast->strings, 4096u);
    cm_vec_init(&ast->paths, sizeof(CmAstPath));
    cm_vec_init(&ast->types, sizeof(CmAstType));
    cm_vec_init(&ast->attributes, sizeof(CmAstAttribute));
    cm_vec_init(&ast->patterns, sizeof(CmAstPattern));
    cm_vec_init(&ast->expressions, sizeof(CmAstExpr));
    cm_vec_init(&ast->statements, sizeof(CmAstStmt));
    cm_vec_init(&ast->items, sizeof(CmAstItem));
    cm_vec_init(&ast->crate_attributes, sizeof(CmAstAttributeId));
    cm_vec_init(&ast->root_items, sizeof(CmAstItemId));
}

void cm_ast_destroy(CmAst *ast)
{
    cm_vec_destroy(&ast->root_items);
    cm_vec_destroy(&ast->crate_attributes);
    cm_vec_destroy(&ast->items);
    cm_vec_destroy(&ast->statements);
    cm_vec_destroy(&ast->expressions);
    cm_vec_destroy(&ast->patterns);
    cm_vec_destroy(&ast->attributes);
    cm_vec_destroy(&ast->types);
    cm_vec_destroy(&ast->paths);
    cm_interner_destroy(&ast->strings);
    cm_arena_destroy(&ast->storage);
    memset(ast, 0, sizeof(*ast));
}

CmAstPathId cm_ast_add_path(CmAst *ast, const CmAstPath *path)
{
    return cm_ast_push_id(&ast->paths, path);
}

CmAstTypeId cm_ast_add_type(CmAst *ast, const CmAstType *type)
{
    return cm_ast_push_id(&ast->types, type);
}

CmAstAttributeId cm_ast_add_attribute(CmAst *ast,
    const CmAstAttribute *attribute)
{
    return cm_ast_push_id(&ast->attributes, attribute);
}

CmAstPatternId cm_ast_add_pattern(CmAst *ast, const CmAstPattern *pattern)
{
    return cm_ast_push_id(&ast->patterns, pattern);
}

CmAstExprId cm_ast_add_expr(CmAst *ast, const CmAstExpr *expression)
{
    return cm_ast_push_id(&ast->expressions, expression);
}

CmAstStmtId cm_ast_add_stmt(CmAst *ast, const CmAstStmt *statement)
{
    return cm_ast_push_id(&ast->statements, statement);
}

CmAstItemId cm_ast_add_item(CmAst *ast, const CmAstItem *item)
{
    return cm_ast_push_id(&ast->items, item);
}

const CmAstPath *cm_ast_get_path(const CmAst *ast, CmAstPathId id)
{
    return (const CmAstPath *)cm_ast_get_vector_id(&ast->paths, id);
}

const CmAstType *cm_ast_get_type(const CmAst *ast, CmAstTypeId id)
{
    return (const CmAstType *)cm_ast_get_vector_id(&ast->types, id);
}

const CmAstAttribute *cm_ast_get_attribute(const CmAst *ast,
    CmAstAttributeId id)
{
    return (const CmAstAttribute *)cm_ast_get_vector_id(&ast->attributes, id);
}

const CmAstPattern *cm_ast_get_pattern(const CmAst *ast, CmAstPatternId id)
{
    return (const CmAstPattern *)cm_ast_get_vector_id(&ast->patterns, id);
}

const CmAstExpr *cm_ast_get_expr(const CmAst *ast, CmAstExprId id)
{
    return (const CmAstExpr *)cm_ast_get_vector_id(&ast->expressions, id);
}

const CmAstStmt *cm_ast_get_stmt(const CmAst *ast, CmAstStmtId id)
{
    return (const CmAstStmt *)cm_ast_get_vector_id(&ast->statements, id);
}

const CmAstItem *cm_ast_get_item(const CmAst *ast, CmAstItemId id)
{
    return (const CmAstItem *)cm_ast_get_vector_id(&ast->items, id);
}

const CmInternedString *cm_ast_get_string(const CmAst *ast, CmInternId id)
{
    return cm_interner_get(&ast->strings, id);
}

static void cm_dump_indent(FILE *stream, unsigned int depth)
{
    unsigned int index;

    for (index = 0u; index < depth; ++index) {
        fputs("  ", stream);
    }
}

static void cm_dump_string(FILE *stream, const CmAst *ast, CmInternId id)
{
    const CmInternedString *string;
    size_t index;

    string = cm_ast_get_string(ast, id);
    fputc('"', stream);
    if (string != NULL) {
        for (index = 0u; index < string->len; ++index) {
            unsigned char byte;

            byte = string->bytes[index];
            switch (byte) {
            case (unsigned char)'\\':
                fputs("\\\\", stream);
                break;
            case (unsigned char)'"':
                fputs("\\\"", stream);
                break;
            case (unsigned char)'\n':
                fputs("\\n", stream);
                break;
            case (unsigned char)'\r':
                fputs("\\r", stream);
                break;
            case (unsigned char)'\t':
                fputs("\\t", stream);
                break;
            default:
                if (byte >= 0x20u && byte < 0x7fu) {
                    fputc((int)byte, stream);
                } else {
                    fprintf(stream, "\\x%02x", (unsigned int)byte);
                }
                break;
            }
        }
    }
    fputc('"', stream);
}

static const char *cm_dump_visibility_name(CmAstVisibilityKind kind)
{
    switch (kind) {
    case CM_AST_VIS_INHERITED:
        return "inherited";
    case CM_AST_VIS_PUBLIC:
        return "public";
    case CM_AST_VIS_CRATE:
        return "crate";
    case CM_AST_VIS_SELF:
        return "self";
    case CM_AST_VIS_SUPER:
        return "super";
    case CM_AST_VIS_RESTRICTED:
        return "restricted";
    }
    return "unknown";
}

static void cm_dump_type(FILE *stream, const CmAst *ast, CmAstTypeId id);

static void cm_dump_generic_argument(FILE *stream, const CmAst *ast,
    const CmAstGenericArg *argument)
{
    uint32_t name_argument_index;

    switch (argument->kind) {
    case CM_AST_GENERIC_TYPE:
        fputs("type=", stream);
        cm_dump_type(stream, ast, argument->type);
        break;
    case CM_AST_GENERIC_LIFETIME:
        fputs("lifetime=", stream);
        cm_dump_string(stream, ast, argument->text);
        break;
    case CM_AST_GENERIC_CONST:
        fputs("const=", stream);
        cm_dump_string(stream, ast, argument->text);
        break;
    case CM_AST_GENERIC_BINDING:
        fputs("binding ", stream);
        cm_dump_string(stream, ast, argument->name);
        if (argument->name_argument_count != 0u) {
            fputc('<', stream);
            for (name_argument_index = 0u;
                 name_argument_index < argument->name_argument_count;
                 ++name_argument_index) {
                if (name_argument_index != 0u) fputs(", ", stream);
                cm_dump_generic_argument(stream, ast,
                    &argument->name_arguments[name_argument_index]);
            }
            fputc('>', stream);
        }
        fputc('=', stream);
        cm_dump_type(stream, ast, argument->type);
        break;
    case CM_AST_GENERIC_CONSTRAINT:
    {
        uint32_t index;

        fputs("constraint ", stream);
        cm_dump_string(stream, ast, argument->name);
        if (argument->name_argument_count != 0u) {
            fputc('<', stream);
            for (name_argument_index = 0u;
                 name_argument_index < argument->name_argument_count;
                 ++name_argument_index) {
                if (name_argument_index != 0u) fputs(", ", stream);
                cm_dump_generic_argument(stream, ast,
                    &argument->name_arguments[name_argument_index]);
            }
            fputc('>', stream);
        }
        fputc(':', stream);
        for (index = 0u; index < argument->bound_count; ++index) {
            const CmAstGenericParamBound *bound;

            if (index != 0u) fputc('+', stream);
            bound = &argument->bounds[index];
            if (bound->modifier == CM_AST_GENERIC_BOUND_RELAXED) {
                fputc('?', stream);
            } else if (bound->modifier
                    == CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST) {
                fputs("~const ", stream);
            }
            if (bound->kind == CM_AST_GENERIC_BOUND_LIFETIME) {
                cm_dump_string(stream, ast, bound->lifetime);
            } else {
                cm_dump_type(stream, ast, bound->trait_type);
            }
        }
        break;
    }
    }
}

static void cm_dump_path_segment(FILE *stream, const CmAst *ast,
    const CmAstPathSegment *segment)
{
    uint32_t argument_index;

    cm_dump_string(stream, ast, segment->name);
    if (segment->argument_count == 0u) {
        return;
    }
    fputc('<', stream);
    for (argument_index = 0u; argument_index < segment->argument_count;
         ++argument_index) {
        if (argument_index != 0u) {
            fputs(", ", stream);
        }
        cm_dump_generic_argument(stream, ast,
            &segment->arguments[argument_index]);
    }
    fputc('>', stream);
}

static void cm_dump_path(FILE *stream, const CmAst *ast, CmAstPathId id)
{
    const CmAstPath *path;
    uint32_t segment_index;

    path = cm_ast_get_path(ast, id);
    if (path == NULL) {
        fputs("<no-path>", stream);
        return;
    }
    if (path->absolute) {
        fputs("::", stream);
    }
    for (segment_index = 0u; segment_index < path->segment_count;
         ++segment_index) {
        const CmAstPathSegment *segment;

        segment = &path->segments[segment_index];
        if (segment_index != 0u) {
            fputs("::", stream);
        }
        cm_dump_path_segment(stream, ast, segment);
    }
}

static const char *cm_dump_macro_delimiter(CmAstDelimiter delimiter)
{
    switch (delimiter) {
    case CM_AST_DELIMITER_PAREN:
        return "paren";
    case CM_AST_DELIMITER_BRACE:
        return "brace";
    case CM_AST_DELIMITER_BRACKET:
        return "bracket";
    }
    return "unknown";
}

static const char *cm_dump_macro_form(CmAstMacroForm form)
{
    switch (form) {
    case CM_AST_MACRO_INVOCATION:
        return "invocation";
    case CM_AST_MACRO_RULES_DEFINITION:
        return "macro-rules-definition";
    case CM_AST_MACRO_DECLARATIVE_DEFINITION:
        return "declarative-definition";
    }
    return "unknown";
}

static void cm_dump_macro_invocation(FILE *stream, const CmAst *ast,
    const CmAstMacroInvocation *invocation)
{
    fprintf(stream, "macro(form=%s, path=",
        cm_dump_macro_form(invocation->form));
    cm_dump_path(stream, ast, invocation->path);
    fputs(", parameters=", stream);
    cm_dump_string(stream, ast, invocation->parameters);
    fprintf(stream, ", delimiter=%s, semicolon=%d, arguments=",
        cm_dump_macro_delimiter(invocation->delimiter),
        invocation->has_semicolon);
    cm_dump_string(stream, ast, invocation->arguments);
    fputc(')', stream);
}

static void cm_dump_type_list(FILE *stream, const CmAst *ast,
    const CmAstType *type)
{
    uint32_t index;

    for (index = 0u; index < type->element_count; ++index) {
        if (index != 0u) {
            fputs(", ", stream);
        }
        cm_dump_type(stream, ast, type->elements[index]);
    }
}

static void cm_dump_type(FILE *stream, const CmAst *ast, CmAstTypeId id)
{
    const CmAstType *type;
    uint32_t index;

    type = cm_ast_get_type(ast, id);
    if (type == NULL) {
        fputs("<none>", stream);
        return;
    }
    switch (type->kind) {
    case CM_AST_TYPE_INFER:
        fputc('_', stream);
        break;
    case CM_AST_TYPE_NEVER:
        fputc('!', stream);
        break;
    case CM_AST_TYPE_PATH:
        fputs("path(", stream);
        cm_dump_path(stream, ast, type->path);
        fputc(')', stream);
        break;
    case CM_AST_TYPE_REFERENCE:
        fputs("ref(", stream);
        if (type->lifetime != CM_INTERN_ID_NONE) {
            cm_dump_string(stream, ast, type->lifetime);
            fputc(' ', stream);
        }
        if (type->is_mutable) {
            fputs("mut ", stream);
        }
        cm_dump_type(stream, ast, type->child);
        fputc(')', stream);
        break;
    case CM_AST_TYPE_POINTER:
        fputs(type->is_mutable ? "ptr(mut " : "ptr(const ", stream);
        cm_dump_type(stream, ast, type->child);
        fputc(')', stream);
        break;
    case CM_AST_TYPE_TUPLE:
        fputs(type->tuple_provenance == CM_AST_TUPLE_CALLABLE_INPUTS
            ? "callable-tuple(" : "tuple(", stream);
        cm_dump_type_list(stream, ast, type);
        fputc(')', stream);
        break;
    case CM_AST_TYPE_SLICE:
        fputs("slice(", stream);
        cm_dump_type(stream, ast, type->child);
        fputc(')', stream);
        break;
    case CM_AST_TYPE_ARRAY:
        fputs("array(", stream);
        cm_dump_type(stream, ast, type->child);
        fputs("; ", stream);
        cm_dump_string(stream, ast, type->text);
        fputc(')', stream);
        break;
    case CM_AST_TYPE_FUNCTION:
        if (type->binder.lifetime_count != 0u) {
            uint32_t lifetime_index;

            fputs("for<", stream);
            for (lifetime_index = 0u;
                 lifetime_index < type->binder.lifetime_count;
                 ++lifetime_index) {
                if (lifetime_index != 0u) fputs(", ", stream);
                cm_dump_string(stream, ast,
                    type->binder.lifetimes[lifetime_index]);
            }
            fputs("> ", stream);
        }
        fputs(type->is_unsafe ? "unsafe-fn(" : "fn(", stream);
        cm_dump_type_list(stream, ast, type);
        fputs(")->", stream);
        cm_dump_type(stream, ast, type->child);
        break;
    case CM_AST_TYPE_IMPL_TRAIT:
        fputs("impl(", stream);
        for (index = 0u; index < type->bound_count; ++index) {
            uint32_t lifetime_index;

            if (index != 0u) fputs(" + ", stream);
            if (type->bounds[index].modifier
                == CM_AST_TYPE_BOUND_RELAXED) fputc('?', stream);
            else if (type->bounds[index].modifier
                == CM_AST_TYPE_BOUND_CONDITIONALLY_CONST)
                fputs("~const ", stream);
            if (type->bounds[index].binder.lifetime_count != 0u) {
                fputs("for<", stream);
                for (lifetime_index = 0u;
                     lifetime_index
                        < type->bounds[index].binder.lifetime_count;
                     ++lifetime_index) {
                    if (lifetime_index != 0u) fputs(", ", stream);
                    cm_dump_string(stream, ast,
                        type->bounds[index].binder
                            .lifetimes[lifetime_index]);
                }
                fputs("> ", stream);
            }
            if (type->bounds[index].lifetime != CM_INTERN_ID_NONE) {
                cm_dump_string(stream, ast, type->bounds[index].lifetime);
            } else {
                cm_dump_type(stream, ast,
                    type->bounds[index].trait_type);
            }
        }
        fputc(')', stream);
        break;
    case CM_AST_TYPE_DYN_TRAIT:
        fputs("dyn(", stream);
        for (index = 0u; index < type->bound_count; ++index) {
            uint32_t lifetime_index;

            if (index != 0u) fputs(" + ", stream);
            if (type->bounds[index].modifier
                == CM_AST_TYPE_BOUND_RELAXED) fputc('?', stream);
            else if (type->bounds[index].modifier
                == CM_AST_TYPE_BOUND_CONDITIONALLY_CONST)
                fputs("~const ", stream);
            if (type->bounds[index].binder.lifetime_count != 0u) {
                fputs("for<", stream);
                for (lifetime_index = 0u;
                     lifetime_index
                        < type->bounds[index].binder.lifetime_count;
                     ++lifetime_index) {
                    if (lifetime_index != 0u) fputs(", ", stream);
                    cm_dump_string(stream, ast,
                        type->bounds[index].binder
                            .lifetimes[lifetime_index]);
                }
                fputs("> ", stream);
            }
            if (type->bounds[index].lifetime != CM_INTERN_ID_NONE) {
                cm_dump_string(stream, ast, type->bounds[index].lifetime);
            } else {
                cm_dump_type(stream, ast,
                    type->bounds[index].trait_type);
            }
        }
        fputc(')', stream);
        break;
    case CM_AST_TYPE_PROJECTION:
        fputs("projection(self=", stream);
        cm_dump_type(stream, ast, type->projection.self_type);
        fputs(", trait=path(", stream);
        cm_dump_path(stream, ast, type->projection.trait_path);
        fputs("), associated=", stream);
        cm_dump_path_segment(stream, ast, &type->projection.associated);
        fputc(')', stream);
        break;
    case CM_AST_TYPE_MACRO:
        cm_dump_macro_invocation(stream, ast, &type->macro_type);
        break;
    case CM_AST_TYPE_OTHER:
        fputs("other(", stream);
        cm_dump_string(stream, ast, type->text);
        fputc(')', stream);
        break;
    }
}

static void cm_dump_pattern(FILE *stream, const CmAst *ast,
    CmAstPatternId id);
static void cm_dump_expr(FILE *stream, const CmAst *ast, CmAstExprId id);

static void cm_dump_pattern(FILE *stream, const CmAst *ast,
    CmAstPatternId id)
{
    const CmAstPattern *pattern;
    uint32_t index;

    pattern = cm_ast_get_pattern(ast, id);
    if (pattern == NULL) {
        fputs("<no-pattern>", stream);
        return;
    }
    switch (pattern->kind) {
    case CM_AST_PATTERN_REST:
        fputs("..", stream);
        break;
    case CM_AST_PATTERN_WILDCARD:
        fputc('_', stream);
        break;
    case CM_AST_PATTERN_BINDING:
        fputs("bind(", stream);
        if (pattern->data.binding.is_ref) fputs("ref ", stream);
        if (pattern->data.binding.is_mutable) fputs("mut ", stream);
        cm_dump_string(stream, ast, pattern->data.binding.name);
        if (pattern->data.binding.subpattern != CM_AST_PATTERN_NONE) {
            fputs(" @ ", stream);
            cm_dump_pattern(stream, ast, pattern->data.binding.subpattern);
        }
        fputc(')', stream);
        break;
    case CM_AST_PATTERN_LITERAL:
        fputs("literal(", stream);
        cm_dump_string(stream, ast, pattern->data.literal.text);
        fputc(')', stream);
        break;
    case CM_AST_PATTERN_PATH:
        fputs("path(", stream);
        cm_dump_path(stream, ast, pattern->data.path.path);
        fputc(')', stream);
        break;
    case CM_AST_PATTERN_REFERENCE:
        fputs(pattern->data.reference.is_mutable ? "ref(mut " : "ref(",
            stream);
        cm_dump_pattern(stream, ast, pattern->data.reference.pattern);
        fputc(')', stream);
        break;
    case CM_AST_PATTERN_TUPLE:
    case CM_AST_PATTERN_SLICE:
    case CM_AST_PATTERN_OR:
    {
        int emitted;

        fputs(pattern->kind == CM_AST_PATTERN_TUPLE ? "tuple(" :
            (pattern->kind == CM_AST_PATTERN_SLICE ? "slice(" : "or("),
            stream);
        emitted = 0;
        for (index = 0u; index < pattern->data.list.pattern_count; ++index) {
            if (pattern->data.list.has_rest &&
                pattern->data.list.rest_index == index) {
                if (emitted) fputs(", ", stream);
                fputs("..", stream);
                emitted = 1;
            }
            if (emitted) fputs(", ", stream);
            cm_dump_pattern(stream, ast, pattern->data.list.patterns[index]);
            emitted = 1;
        }
        if (pattern->data.list.has_rest &&
            pattern->data.list.rest_index ==
                pattern->data.list.pattern_count) {
            if (emitted) fputs(", ", stream);
            fputs("..", stream);
        }
        fputc(')', stream);
        break;
    }
    case CM_AST_PATTERN_STRUCT:
        fputs(pattern->data.struct_pattern.is_tuple ? "tuple-struct(" :
            "struct(", stream);
        cm_dump_path(stream, ast, pattern->data.struct_pattern.path);
        fputs(pattern->data.struct_pattern.is_tuple ? " (" : " {", stream);
        for (index = 0u; index < pattern->data.struct_pattern.field_count;
             ++index) {
            const CmAstPatternField *field;

            field = &pattern->data.struct_pattern.fields[index];
            if (index != 0u) fputs(", ", stream);
            if (field->name != CM_INTERN_ID_NONE)
                cm_dump_string(stream, ast, field->name);
            if (field->name == CM_INTERN_ID_NONE) {
                cm_dump_pattern(stream, ast, field->pattern);
            } else if (!field->is_shorthand) {
                fputs(": ", stream);
                cm_dump_pattern(stream, ast, field->pattern);
            }
        }
        if (pattern->data.struct_pattern.has_rest) {
            if (pattern->data.struct_pattern.field_count != 0u)
                fputs(", ", stream);
            fputs("..", stream);
        }
        fputs(pattern->data.struct_pattern.is_tuple ? "))" : "})", stream);
        break;
    case CM_AST_PATTERN_RANGE:
        fputs("range(", stream);
        if (pattern->data.range.start != CM_AST_PATTERN_NONE)
            cm_dump_pattern(stream, ast, pattern->data.range.start);
        fputs(pattern->data.range.is_inclusive ? "..=" : "..", stream);
        if (pattern->data.range.end != CM_AST_PATTERN_NONE)
            cm_dump_pattern(stream, ast, pattern->data.range.end);
        fputc(')', stream);
        break;
    }
}

static void cm_dump_expr_list(FILE *stream, const CmAst *ast,
    const CmAstExprId *expressions, uint32_t count)
{
    uint32_t index;

    for (index = 0u; index < count; ++index) {
        if (index != 0u) fputs(", ", stream);
        cm_dump_expr(stream, ast, expressions[index]);
    }
}

static void cm_dump_item(FILE *stream, const CmAst *ast, CmAstItemId id,
    unsigned int depth);
static void cm_dump_expression_attributes(FILE *stream, const CmAst *ast,
    const CmAstAttributeId *ids, uint32_t count);

static void cm_dump_stmt(FILE *stream, const CmAst *ast, CmAstStmtId id)
{
    const CmAstStmt *statement;
    int attributed;

    statement = cm_ast_get_stmt(ast, id);
    if (statement == NULL) {
        fputs("<no-stmt>", stream);
        return;
    }
    attributed = statement->attribute_count != 0u;
    if (attributed) {
        fputs("attributed-stmt(", stream);
        cm_dump_expression_attributes(stream, ast, statement->attributes,
            statement->attribute_count);
        fputs(", ", stream);
    }
    if (statement->kind == CM_AST_STMT_LET) {
        fputs("let(", stream);
        cm_dump_pattern(stream, ast, statement->data.let_stmt.pattern);
        if (statement->data.let_stmt.type != CM_AST_TYPE_NONE) {
            fputs(": ", stream);
            cm_dump_type(stream, ast, statement->data.let_stmt.type);
        }
        if (statement->data.let_stmt.initializer != CM_AST_EXPR_NONE) {
            fputs(" = ", stream);
            cm_dump_expr(stream, ast,
                statement->data.let_stmt.initializer);
        }
        if (statement->data.let_stmt.else_block != CM_AST_EXPR_NONE) {
            fputs(" else ", stream);
            cm_dump_expr(stream, ast,
                statement->data.let_stmt.else_block);
        }
        fputc(')', stream);
    } else if (statement->kind == CM_AST_STMT_ITEM) {
        fputs("item-stmt(\n", stream);
        cm_dump_item(stream, ast, statement->data.item_stmt.item, 1u);
        fputc(')', stream);
    } else {
        fputs(statement->data.expr_stmt.has_semicolon ? "stmt(" : "expr(",
            stream);
        cm_dump_expr(stream, ast, statement->data.expr_stmt.expression);
        fputc(')', stream);
    }
    if (attributed) fputc(')', stream);
}

static void cm_dump_expression_attributes(FILE *stream, const CmAst *ast,
    const CmAstAttributeId *ids, uint32_t count)
{
    uint32_t index;

    fputs("attributes(", stream);
    for (index = 0u; index < count; ++index) {
        const CmAstAttribute *attribute;

        if (index != 0u) fputs(", ", stream);
        attribute = cm_ast_get_attribute(ast, ids[index]);
        if (attribute == NULL) {
            fputs("<no-attribute>", stream);
            continue;
        }
        fputs(attribute->style == CM_AST_ATTR_INNER ? "inner=" :
            "outer=", stream);
        cm_dump_string(stream, ast, attribute->text);
    }
    fputc(')', stream);
}

static void cm_dump_expr(FILE *stream, const CmAst *ast, CmAstExprId id)
{
    const CmAstExpr *expression;
    uint32_t index;

    expression = cm_ast_get_expr(ast, id);
    if (expression == NULL) {
        fputs("<no-expr>", stream);
        return;
    }
    if (expression->attribute_count != 0u) {
        fputs("attributed(", stream);
        cm_dump_expression_attributes(stream, ast, expression->attributes,
            expression->attribute_count);
        fputs(", ", stream);
    }
    switch (expression->kind) {
    case CM_AST_EXPR_LITERAL:
        fputs("literal(", stream);
        cm_dump_string(stream, ast, expression->data.literal.text);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_PATH:
        fputs("path(", stream);
        cm_dump_path(stream, ast, expression->data.path.path);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_QUALIFIED_PATH:
        fputs("qualified-path(self=", stream);
        cm_dump_type(stream, ast, expression->data.qualified_path.self_type);
        fputs(", trait=", stream);
        cm_dump_path(stream, ast,
            expression->data.qualified_path.trait_path);
        fputs(", associated=", stream);
        cm_dump_path(stream, ast,
            expression->data.qualified_path.associated_path);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_BLOCK:
        if (expression->data.block.is_const) {
            fputs("const-block(", stream);
        } else if (expression->data.block.is_unsafe) {
            fputs("unsafe-block(", stream);
        } else {
            fputs("block(", stream);
        }
        if (expression->data.block.inner_attribute_count != 0u) {
            cm_dump_expression_attributes(stream, ast,
                expression->data.block.inner_attributes,
                expression->data.block.inner_attribute_count);
        }
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            if (index != 0u
                || expression->data.block.inner_attribute_count != 0u) {
                fputs(", ", stream);
            }
            cm_dump_stmt(stream, ast, expression->data.block.statements[index]);
        }
        if (expression->data.block.tail != CM_AST_EXPR_NONE) {
            if (expression->data.block.statement_count != 0u
                || expression->data.block.inner_attribute_count != 0u) {
                fputs(", ", stream);
            }
            fputs("tail=", stream);
            cm_dump_expr(stream, ast, expression->data.block.tail);
        }
        fputc(')', stream);
        break;
    case CM_AST_EXPR_CALL:
        fputs("call(", stream);
        cm_dump_expr(stream, ast, expression->data.call.callee);
        if (expression->data.call.argument_count != 0u) fputs(", ", stream);
        cm_dump_expr_list(stream, ast, expression->data.call.arguments,
            expression->data.call.argument_count);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_METHOD_CALL:
        fputs("method(", stream);
        cm_dump_expr(stream, ast, expression->data.method_call.receiver);
        fputc('.', stream);
        cm_dump_string(stream, ast, expression->data.method_call.name);
        if (expression->data.method_call.generic_argument_count != 0u) {
            fputs("::<", stream);
            for (index = 0u;
                 index < expression->data.method_call.generic_argument_count;
                 ++index) {
                if (index != 0u) fputs(", ", stream);
                cm_dump_generic_argument(stream, ast,
                    &expression->data.method_call.generic_arguments[index]);
            }
            fputc('>', stream);
        }
        if (expression->data.method_call.argument_count != 0u)
            fputs(", ", stream);
        cm_dump_expr_list(stream, ast, expression->data.method_call.arguments,
            expression->data.method_call.argument_count);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_FIELD:
        fputs("field(", stream);
        cm_dump_expr(stream, ast, expression->data.field.base);
        fputc('.', stream);
        cm_dump_string(stream, ast, expression->data.field.name);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_TUPLE_FIELD:
        fputs("tuple-field(", stream);
        cm_dump_expr(stream, ast, expression->data.tuple_field.base);
        fprintf(stream, ".%lu)",
            (unsigned long)expression->data.tuple_field.index);
        break;
    case CM_AST_EXPR_INDEX:
        fputs("index(", stream);
        cm_dump_expr(stream, ast, expression->data.index.base);
        fputs(", ", stream);
        cm_dump_expr(stream, ast, expression->data.index.index);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_UNARY:
        fputs("unary(", stream);
        cm_dump_string(stream, ast, expression->data.unary.operator_name);
        fputs(", ", stream);
        cm_dump_expr(stream, ast, expression->data.unary.operand);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_RAW_REFERENCE:
        fputs(expression->data.raw_reference.kind == CM_AST_RAW_REFERENCE_MUT
            ? "raw-reference(mut, " : "raw-reference(const, ", stream);
        cm_dump_expr(stream, ast, expression->data.raw_reference.operand);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_BINARY:
    case CM_AST_EXPR_ASSIGN:
        fputs(expression->kind == CM_AST_EXPR_ASSIGN ? "assign(" :
            "binary(", stream);
        cm_dump_string(stream, ast, expression->data.binary.operator_name);
        fputs(", ", stream);
        cm_dump_expr(stream, ast, expression->data.binary.left);
        fputs(", ", stream);
        cm_dump_expr(stream, ast, expression->data.binary.right);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_CAST:
        fputs("cast(", stream);
        cm_dump_expr(stream, ast, expression->data.cast.value);
        fputs(" as ", stream);
        cm_dump_type(stream, ast, expression->data.cast.type);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_TRY:
        fputs("try(", stream);
        cm_dump_expr(stream, ast, expression->data.try_expr.operand);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_TRY_BLOCK:
        fputs("try-block(", stream);
        cm_dump_expr(stream, ast, expression->data.try_expr.operand);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_RANGE:
        fputs("range(", stream);
        cm_dump_expr(stream, ast, expression->data.range.start);
        fputs(expression->data.range.is_inclusive ? "..=" : "..", stream);
        cm_dump_expr(stream, ast, expression->data.range.end);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_LET:
        fputs("let(", stream);
        cm_dump_pattern(stream, ast, expression->data.let_expr.pattern);
        fputs(" = ", stream);
        cm_dump_expr(stream, ast, expression->data.let_expr.initializer);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_RETURN:
    case CM_AST_EXPR_BREAK:
    case CM_AST_EXPR_CONTINUE:
        fputs(expression->kind == CM_AST_EXPR_RETURN ? "return(" :
            (expression->kind == CM_AST_EXPR_BREAK ? "break(" :
             "continue("), stream);
        if (expression->data.flow.label != CM_INTERN_ID_NONE) {
            cm_dump_string(stream, ast, expression->data.flow.label);
            if (expression->data.flow.value != CM_AST_EXPR_NONE)
                fputs(", ", stream);
        }
        if (expression->data.flow.value != CM_AST_EXPR_NONE)
            cm_dump_expr(stream, ast, expression->data.flow.value);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_IF:
        fputs("if(", stream);
        if (expression->data.if_expr.pattern != CM_AST_PATTERN_NONE) {
            fputs("let ", stream);
            cm_dump_pattern(stream, ast, expression->data.if_expr.pattern);
            fputs(" = ", stream);
        }
        cm_dump_expr(stream, ast, expression->data.if_expr.condition);
        fputs(", ", stream);
        cm_dump_expr(stream, ast, expression->data.if_expr.then_expr);
        if (expression->data.if_expr.else_expr != CM_AST_EXPR_NONE) {
            fputs(", else=", stream);
            cm_dump_expr(stream, ast, expression->data.if_expr.else_expr);
        }
        fputc(')', stream);
        break;
    case CM_AST_EXPR_MATCH:
        fputs("match(", stream);
        cm_dump_expr(stream, ast, expression->data.match_expr.scrutinee);
        for (index = 0u; index < expression->data.match_expr.arm_count;
             ++index) {
            const CmAstMatchArm *arm;

            arm = &expression->data.match_expr.arms[index];
            fputs(", arm(", stream);
            if (arm->attribute_count != 0u) {
                cm_dump_expression_attributes(stream, ast,
                    arm->attributes, arm->attribute_count);
                fputs(", ", stream);
            }
            cm_dump_pattern(stream, ast, arm->pattern);
            if (arm->guard_pattern != CM_AST_PATTERN_NONE) {
                fputs(" if let ", stream);
                cm_dump_pattern(stream, ast, arm->guard_pattern);
                fputs(" = ", stream);
                cm_dump_expr(stream, ast, arm->guard_initializer);
            } else if (arm->guard != CM_AST_EXPR_NONE) {
                fputs(" if ", stream);
                cm_dump_expr(stream, ast, arm->guard);
            }
            fputs(" => ", stream);
            cm_dump_expr(stream, ast, arm->body);
            fputc(')', stream);
        }
        fputc(')', stream);
        break;
    case CM_AST_EXPR_LOOP:
        fputs("loop(", stream);
        if (expression->data.loop_expr.label != CM_INTERN_ID_NONE) {
            fputs("label=", stream);
            cm_dump_string(stream, ast, expression->data.loop_expr.label);
            fputs(", ", stream);
        }
        cm_dump_expr(stream, ast, expression->data.loop_expr.body);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_WHILE:
        fputs("while(", stream);
        if (expression->data.while_expr.pattern != CM_AST_PATTERN_NONE) {
            fputs("let ", stream);
            cm_dump_pattern(stream, ast, expression->data.while_expr.pattern);
            fputs(" = ", stream);
        }
        cm_dump_expr(stream, ast, expression->data.while_expr.condition);
        fputs(", ", stream);
        cm_dump_expr(stream, ast, expression->data.while_expr.body);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_FOR:
        fputs("for(", stream);
        cm_dump_pattern(stream, ast, expression->data.for_expr.pattern);
        fputs(" in ", stream);
        cm_dump_expr(stream, ast, expression->data.for_expr.iterable);
        fputs(", ", stream);
        cm_dump_expr(stream, ast, expression->data.for_expr.body);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_CLOSURE:
        fputs(expression->data.closure.is_move ? "closure(move |" :
            "closure(|", stream);
        for (index = 0u; index < expression->data.closure.parameter_count;
             ++index) {
            const CmAstClosureParam *parameter;

            parameter = &expression->data.closure.parameters[index];
            if (index != 0u) fputs(", ", stream);
            cm_dump_pattern(stream, ast, parameter->pattern);
            if (parameter->type != CM_AST_TYPE_NONE) {
                fputs(": ", stream);
                cm_dump_type(stream, ast, parameter->type);
            }
        }
        fputs("| ", stream);
        cm_dump_expr(stream, ast, expression->data.closure.body);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_TUPLE:
    case CM_AST_EXPR_ARRAY:
        if (expression->kind == CM_AST_EXPR_ARRAY &&
            expression->data.list.repeat_length != 0u) {
            fputs("array-repeat(", stream);
            cm_dump_expr(stream, ast, expression->data.list.repeat_value);
            fputs("; ", stream);
            cm_dump_expr(stream, ast, expression->data.list.repeat_length);
            fputc(')', stream);
            break;
        }
        fputs(expression->kind == CM_AST_EXPR_TUPLE ? "tuple(" : "array(",
            stream);
        cm_dump_expr_list(stream, ast, expression->data.list.elements,
            expression->data.list.element_count);
        fputc(')', stream);
        break;
    case CM_AST_EXPR_STRUCT:
        fputs("struct(", stream);
        cm_dump_path(stream, ast, expression->data.struct_expr.path);
        for (index = 0u; index < expression->data.struct_expr.field_count;
             ++index) {
            const CmAstExprField *field;
            uint32_t attribute_index;

            field = &expression->data.struct_expr.fields[index];
            fputs(index == 0u ? " {" : ", ", stream);
            for (attribute_index = 0u;
                 attribute_index < field->attribute_count;
                 ++attribute_index) {
                const CmAstAttribute *attribute;

                attribute = cm_ast_get_attribute(ast,
                    field->attributes[attribute_index]);
                fputs("attribute(", stream);
                if (attribute == NULL) {
                    fputs("<none>", stream);
                } else {
                    cm_dump_string(stream, ast, attribute->text);
                }
                fputs(") ", stream);
            }
            cm_dump_string(stream, ast, field->name);
            if (!field->is_shorthand) {
                fputs(": ", stream);
                cm_dump_expr(stream, ast, field->value);
            }
        }
        if (expression->data.struct_expr.base != CM_AST_EXPR_NONE) {
            fputs(expression->data.struct_expr.field_count == 0u ?
                " {.." : ", ..", stream);
            cm_dump_expr(stream, ast, expression->data.struct_expr.base);
        }
        if (expression->data.struct_expr.field_count != 0u ||
            expression->data.struct_expr.base != CM_AST_EXPR_NONE) {
            fputc('}', stream);
        }
        fputc(')', stream);
        break;
    case CM_AST_EXPR_MACRO:
        cm_dump_macro_invocation(stream, ast,
            &expression->data.macro_expr);
        break;
    }
    if (expression->attribute_count != 0u) fputc(')', stream);
}

static void cm_dump_attributes(FILE *stream, const CmAst *ast,
    const CmAstAttributeId *ids, uint32_t count, unsigned int depth)
{
    uint32_t index;

    for (index = 0u; index < count; ++index) {
        const CmAstAttribute *attribute;

        attribute = cm_ast_get_attribute(ast, ids[index]);
        if (attribute == NULL) {
            continue;
        }
        cm_dump_indent(stream, depth);
        fputs("(attribute ", stream);
        fputs(attribute->style == CM_AST_ATTR_INNER ? "inner " : "outer ",
            stream);
        cm_dump_string(stream, ast, attribute->text);
        fputs(")\n", stream);
    }
}

static void cm_dump_where_clause(FILE *stream, const CmAst *ast,
    CmInternId clause, const CmAstWherePredicate *predicates,
    uint32_t predicate_count, const char *clause_name,
    const char *predicate_name, unsigned int depth)
{
    uint32_t index;

    if (clause != CM_INTERN_ID_NONE) {
        cm_dump_indent(stream, depth);
        fprintf(stream, "(%s ", clause_name);
        cm_dump_string(stream, ast, clause);
        fputs(")\n", stream);
    }
    for (index = 0u; index < predicate_count; ++index) {
        const CmAstWherePredicate *predicate;
        uint32_t bound_index;

        predicate = &predicates[index];
        cm_dump_indent(stream, depth);
        fprintf(stream, "(%s ", predicate_name);
        if (predicate->binder.lifetime_count != 0u) {
            uint32_t lifetime_index;

            fputs("for<", stream);
            for (lifetime_index = 0u;
                 lifetime_index < predicate->binder.lifetime_count;
                 ++lifetime_index) {
                if (lifetime_index != 0u) fputs(", ", stream);
                cm_dump_string(stream, ast,
                    predicate->binder.lifetimes[lifetime_index]);
            }
            fputs("> ", stream);
        }
        if (predicate->kind == CM_AST_WHERE_PREDICATE_LIFETIME) {
            fputs("lifetime ", stream);
            cm_dump_string(stream, ast, predicate->subject_lifetime);
        } else {
            cm_dump_type(stream, ast, predicate->subject);
        }
        fputc('\n', stream);
        for (bound_index = 0u; bound_index < predicate->bound_count;
             ++bound_index) {
            const CmAstWhereBound *bound;
            const char *modifier;

            bound = &predicate->bounds[bound_index];
            modifier = bound->modifier == CM_AST_WHERE_BOUND_RELAXED
                ? "relaxed"
                : bound->modifier
                    == CM_AST_WHERE_BOUND_CONDITIONALLY_CONST
                    ? "conditionally-const"
                    : bound->modifier == CM_AST_WHERE_BOUND_CONST
                        ? "const" : "required";
            cm_dump_indent(stream, depth + 1u);
            fprintf(stream, "(where-bound %s ", modifier);
            if (bound->kind == CM_AST_WHERE_BOUND_LIFETIME) {
                fputs("lifetime ", stream);
                cm_dump_string(stream, ast, bound->lifetime);
            } else {
                uint32_t lifetime_index;

                if (bound->binder.lifetime_count != 0u) {
                    fputs("for<", stream);
                    for (lifetime_index = 0u;
                         lifetime_index < bound->binder.lifetime_count;
                         ++lifetime_index) {
                        if (lifetime_index != 0u) fputs(", ", stream);
                        cm_dump_string(stream, ast,
                            bound->binder.lifetimes[lifetime_index]);
                    }
                    fputs("> ", stream);
                }
                cm_dump_type(stream, ast, bound->trait_type);
            }
            fputs(")\n", stream);
        }
        cm_dump_indent(stream, depth);
        fputs(")\n", stream);
    }
}

static void cm_dump_generics(FILE *stream, const CmAst *ast,
    const CmAstItem *item, unsigned int depth)
{
    uint32_t index;

    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmAstGenericParam *parameter;
        const char *kind;

        parameter = &item->generic_parameters[index];
        kind = parameter->kind == CM_AST_PARAM_TYPE ? "type" :
            (parameter->kind == CM_AST_PARAM_LIFETIME ? "lifetime" :
             "const");
        cm_dump_attributes(stream, ast, parameter->attributes,
            parameter->attribute_count, depth);
        cm_dump_indent(stream, depth);
        fprintf(stream, "(generic %s ", kind);
        cm_dump_string(stream, ast, parameter->name);
        fputc(' ', stream);
        cm_dump_string(stream, ast, parameter->declaration);
        if (parameter->constraint != CM_INTERN_ID_NONE) {
            fputs(" constraint=", stream);
            cm_dump_string(stream, ast, parameter->constraint);
        }
        if (parameter->declared_type != CM_AST_TYPE_NONE) {
            fputs(" type=", stream);
            cm_dump_type(stream, ast, parameter->declared_type);
        }
        if (parameter->default_const != CM_INTERN_ID_NONE) {
            fputs(" default-const=", stream);
            cm_dump_string(stream, ast, parameter->default_const);
        }
        if (parameter->default_const_expr != CM_AST_EXPR_NONE) {
            fputs(" default-const-expr=", stream);
            cm_dump_expr(stream, ast, parameter->default_const_expr);
        }
        if (parameter->default_type != CM_AST_TYPE_NONE) {
            fputs(" default=", stream);
            cm_dump_type(stream, ast, parameter->default_type);
        }
        if (parameter->bound_count == 0u) {
            fputs(")\n", stream);
        } else {
            uint32_t bound_index;

            fputc('\n', stream);
            for (bound_index = 0u; bound_index < parameter->bound_count;
                ++bound_index) {
                cm_dump_indent(stream, depth + 1u);
                if (parameter->bounds[bound_index].kind
                    == CM_AST_GENERIC_BOUND_LIFETIME) {
                    fputs("(generic-bound required lifetime ", stream);
                    cm_dump_string(stream, ast,
                        parameter->bounds[bound_index].lifetime);
                    fputs(")\n", stream);
                    continue;
                }
                fputs(parameter->bounds[bound_index].modifier
                        == CM_AST_GENERIC_BOUND_RELAXED
                    ? "(generic-bound relaxed "
                    : (parameter->bounds[bound_index].modifier
                            == CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST
                        ? "(generic-bound conditionally-const "
                        : "(generic-bound required "), stream);
                if (parameter->bounds[bound_index].binder.lifetime_count
                        != 0u) {
                    uint32_t lifetime_index;

                    fputs("for<", stream);
                    for (lifetime_index = 0u;
                         lifetime_index < parameter->bounds[bound_index]
                            .binder.lifetime_count;
                         ++lifetime_index) {
                        if (lifetime_index != 0u) fputs(", ", stream);
                        cm_dump_string(stream, ast,
                            parameter->bounds[bound_index].binder
                                .lifetimes[lifetime_index]);
                    }
                    fputs("> ", stream);
                }
                cm_dump_type(stream, ast,
                    parameter->bounds[bound_index].trait_type);
                fputs(")\n", stream);
            }
            cm_dump_indent(stream, depth);
            fputs(")\n", stream);
        }
    }
    cm_dump_where_clause(stream, ast, item->where_clause,
        item->where_predicates, item->where_predicate_count,
        "where", "where-predicate", depth);
}

static void cm_dump_field(FILE *stream, const CmAst *ast,
    const CmAstField *field, unsigned int depth)
{
    cm_dump_indent(stream, depth);
    fprintf(stream, "(field %s ",
        cm_dump_visibility_name(field->visibility.kind));
    if (field->name == CM_INTERN_ID_NONE) {
        fputs("_ ", stream);
    } else {
        cm_dump_string(stream, ast, field->name);
        fputc(' ', stream);
    }
    cm_dump_type(stream, ast, field->type);
    fputs(")\n", stream);
}

static void cm_dump_item_ids(FILE *stream, const CmAst *ast,
    const CmAstItemId *ids, uint32_t count, unsigned int depth)
{
    uint32_t index;

    for (index = 0u; index < count; ++index) {
        cm_dump_item(stream, ast, ids[index], depth);
    }
}

static const char *cm_dump_item_kind(CmAstItemKind kind)
{
    switch (kind) {
    case CM_AST_ITEM_FUNCTION: return "function";
    case CM_AST_ITEM_STRUCT: return "struct";
    case CM_AST_ITEM_ENUM: return "enum";
    case CM_AST_ITEM_TYPE_ALIAS: return "type";
    case CM_AST_ITEM_CONST: return "const";
    case CM_AST_ITEM_STATIC: return "static";
    case CM_AST_ITEM_MODULE: return "module";
    case CM_AST_ITEM_USE: return "use";
    case CM_AST_ITEM_EXTERN_CRATE: return "extern-crate";
    case CM_AST_ITEM_EXTERN_BLOCK: return "extern-block";
    case CM_AST_ITEM_TRAIT: return "trait";
    case CM_AST_ITEM_IMPL: return "impl";
    case CM_AST_ITEM_MACRO: return "macro";
    case CM_AST_ITEM_UNION: return "union";
    }
    return "item";
}

static const char *cm_dump_field_form(CmAstFieldForm form)
{
    switch (form) {
    case CM_AST_FIELDS_UNIT: return "unit";
    case CM_AST_FIELDS_TUPLE: return "tuple";
    case CM_AST_FIELDS_NAMED: return "named";
    }
    return "unknown";
}

static void cm_dump_item(FILE *stream, const CmAst *ast, CmAstItemId id,
    unsigned int depth)
{
    const CmAstItem *item;
    uint32_t index;

    item = cm_ast_get_item(ast, id);
    if (item == NULL) {
        return;
    }
    cm_dump_indent(stream, depth);
    fprintf(stream, "(%s %s ", cm_dump_item_kind(item->kind),
        cm_dump_visibility_name(item->visibility.kind));
    if (item->name == CM_INTERN_ID_NONE) {
        fputc('_', stream);
    } else {
        cm_dump_string(stream, ast, item->name);
    }
    fputc('\n', stream);
    cm_dump_attributes(stream, ast, item->attributes, item->attribute_count,
        depth + 1u);
    cm_dump_generics(stream, ast, item, depth + 1u);
    if (item->is_default) {
        cm_dump_indent(stream, depth + 1u);
        fputs("(specialization default)\n", stream);
    }

    switch (item->kind) {
    case CM_AST_ITEM_FUNCTION:
        cm_dump_indent(stream, depth + 1u);
        fprintf(stream, "(flags const=%d async=%d unsafe=%d body=%d",
            item->data.function_item.is_const,
            item->data.function_item.is_async,
            item->data.function_item.is_unsafe,
            item->data.function_item.body != CM_AST_EXPR_NONE);
        if (item->data.function_item.is_safe) {
            fputs(" safe=1", stream);
        }
        if (item->data.function_item.abi != CM_INTERN_ID_NONE) {
            fputs(" abi=", stream);
            cm_dump_string(stream, ast, item->data.function_item.abi);
        }
        fputs(")\n", stream);
        for (index = 0u;
             index < item->data.function_item.parameter_count; ++index) {
            const CmAstFunctionParam *parameter;

            parameter = &item->data.function_item.parameters[index];
            cm_dump_indent(stream, depth + 1u);
            fputs(parameter->is_self ? "(self " : "(parameter ", stream);
            if (parameter->receiver_lifetime != CM_INTERN_ID_NONE) {
                fputs("lifetime=", stream);
                cm_dump_string(stream, ast, parameter->receiver_lifetime);
                fputc(' ', stream);
            }
            cm_dump_pattern(stream, ast, parameter->pattern);
            fputc(' ', stream);
            cm_dump_type(stream, ast, parameter->type);
            fputs(")\n", stream);
        }
        cm_dump_indent(stream, depth + 1u);
        fputs("(return ", stream);
        cm_dump_type(stream, ast, item->data.function_item.return_type);
        fputs(")\n", stream);
        if (item->data.function_item.body != CM_AST_EXPR_NONE) {
            cm_dump_indent(stream, depth + 1u);
            fputs("(body ", stream);
            cm_dump_expr(stream, ast, item->data.function_item.body);
            fputs(")\n", stream);
        }
        break;
    case CM_AST_ITEM_STRUCT:
    case CM_AST_ITEM_UNION:
        cm_dump_indent(stream, depth + 1u);
        fprintf(stream, "(fields %s\n",
            cm_dump_field_form(item->data.aggregate_item.form));
        for (index = 0u; index < item->data.aggregate_item.field_count;
             ++index) {
            cm_dump_field(stream, ast,
                &item->data.aggregate_item.fields[index], depth + 2u);
        }
        cm_dump_indent(stream, depth + 1u);
        fputs(")\n", stream);
        break;
    case CM_AST_ITEM_ENUM:
        for (index = 0u; index < item->data.enum_item.variant_count;
             ++index) {
            const CmAstVariant *variant;
            uint32_t field_index;

            variant = &item->data.enum_item.variants[index];
            cm_dump_indent(stream, depth + 1u);
            fprintf(stream, "(variant %s ", cm_dump_field_form(variant->form));
            cm_dump_string(stream, ast, variant->name);
            if (variant->discriminant != CM_INTERN_ID_NONE) {
                fputs(" = ", stream);
                cm_dump_string(stream, ast, variant->discriminant);
            }
            fputc('\n', stream);
            cm_dump_attributes(stream, ast, variant->attributes,
                variant->attribute_count, depth + 2u);
            for (field_index = 0u; field_index < variant->field_count;
                 ++field_index) {
                cm_dump_field(stream, ast, &variant->fields[field_index],
                    depth + 2u);
            }
            cm_dump_indent(stream, depth + 1u);
            fputs(")\n", stream);
        }
        break;
    case CM_AST_ITEM_TYPE_ALIAS:
    case CM_AST_ITEM_CONST:
    case CM_AST_ITEM_STATIC:
        cm_dump_indent(stream, depth + 1u);
        fputs("(value ", stream);
        cm_dump_type(stream, ast, item->data.value_item.type);
        fprintf(stream, " initialized=%d mutable=%d)\n",
            item->data.value_item.has_value,
            item->data.value_item.is_mutable);
        if (item->data.value_item.initializer != CM_AST_EXPR_NONE) {
            cm_dump_indent(stream, depth + 1u);
            fputs("(initializer ", stream);
            cm_dump_expr(stream, ast, item->data.value_item.initializer);
            fputs(")\n", stream);
        }
        if (item->kind == CM_AST_ITEM_TYPE_ALIAS) {
            for (index = 0u; index < item->data.value_item.bound_count;
                 ++index) {
                const CmAstAssociatedTypeBound *bound;

                bound = &item->data.value_item.bounds[index];
                cm_dump_indent(stream, depth + 1u);
                fputs("(associated-type-bound ", stream);
                if (bound->kind == CM_AST_ASSOC_BOUND_LIFETIME) {
                    fputs("required lifetime ", stream);
                    cm_dump_string(stream, ast, bound->lifetime);
                    fputs(")\n", stream);
                    continue;
                }
                fputs(bound->modifier == CM_AST_ASSOC_BOUND_RELAXED
                    ? "relaxed " : "required ", stream);
                cm_dump_type(stream, ast, bound->trait_type);
                fputs(")\n", stream);
            }
            cm_dump_where_clause(stream, ast,
                item->data.value_item.post_value_where_clause,
                item->data.value_item.post_value_where_predicates,
                item->data.value_item.post_value_where_predicate_count,
                "post-value-where", "post-value-where-predicate",
                depth + 1u);
        }
        break;
    case CM_AST_ITEM_MODULE:
        cm_dump_indent(stream, depth + 1u);
        fprintf(stream, "(module-body inline=%d\n",
            item->data.module_item.is_inline);
        cm_dump_attributes(stream, ast,
            item->data.module_item.inner_attributes,
            item->data.module_item.inner_attribute_count, depth + 2u);
        cm_dump_item_ids(stream, ast, item->data.module_item.items,
            item->data.module_item.item_count, depth + 2u);
        cm_dump_indent(stream, depth + 1u);
        fputs(")\n", stream);
        break;
    case CM_AST_ITEM_USE:
        cm_dump_indent(stream, depth + 1u);
        fputs("(tree ", stream);
        cm_dump_string(stream, ast, item->data.use_item.tree);
        fputs(")\n", stream);
        break;
    case CM_AST_ITEM_EXTERN_CRATE:
        if (item->data.extern_crate_item.alias != CM_INTERN_ID_NONE) {
            cm_dump_indent(stream, depth + 1u);
            fputs("(alias ", stream);
            cm_dump_string(stream, ast, item->data.extern_crate_item.alias);
            fputs(")\n", stream);
        }
        break;
    case CM_AST_ITEM_EXTERN_BLOCK:
        if (item->data.extern_block_item.is_unsafe) {
            cm_dump_indent(stream, depth + 1u);
            fputs("(safety unsafe)\n", stream);
        }
        cm_dump_indent(stream, depth + 1u);
        fputs("(abi ", stream);
        cm_dump_string(stream, ast, item->data.extern_block_item.abi);
        fputs(")\n", stream);
        cm_dump_item_ids(stream, ast, item->data.extern_block_item.items,
            item->data.extern_block_item.item_count, depth + 1u);
        break;
    case CM_AST_ITEM_TRAIT:
        if (item->data.trait_item.is_auto) {
            cm_dump_indent(stream, depth + 1u);
            fputs("(auto-trait)\n", stream);
        }
        if (item->data.trait_item.is_alias) {
            cm_dump_indent(stream, depth + 1u);
            fputs("(trait-alias)\n", stream);
        }
        if (item->data.trait_item.supertraits != CM_INTERN_ID_NONE) {
            cm_dump_indent(stream, depth + 1u);
            fputs("(supertraits ", stream);
            cm_dump_string(stream, ast, item->data.trait_item.supertraits);
            fputs(")\n", stream);
        }
        for (index = 0u;
             index < item->data.trait_item.structured_supertrait_count;
             ++index) {
            const CmAstSupertrait *supertrait;

            supertrait = &item->data.trait_item.structured_supertraits[index];
            cm_dump_indent(stream, depth + 1u);
            fputs("(supertrait ", stream);
            if (supertrait->kind == CM_AST_SUPERTRAIT_LIFETIME) {
                fputs("required lifetime ", stream);
                cm_dump_string(stream, ast, supertrait->lifetime);
                fputs(")\n", stream);
                continue;
            }
            fputs(supertrait->modifier
                    == CM_AST_SUPERTRAIT_CONDITIONALLY_CONST
                ? "conditionally-const " : "required ", stream);
            cm_dump_type(stream, ast, supertrait->type);
            fputs(")\n", stream);
        }
        if (item->data.trait_item.alias_bounds != CM_INTERN_ID_NONE) {
            cm_dump_indent(stream, depth + 1u);
            fputs("(alias-bounds ", stream);
            cm_dump_string(stream, ast, item->data.trait_item.alias_bounds);
            fputs(")\n", stream);
        }
        for (index = 0u;
             index < item->data.trait_item.structured_alias_bound_count;
             ++index) {
            const CmAstSupertrait *bound;

            bound = &item->data.trait_item.structured_alias_bounds[index];
            cm_dump_indent(stream, depth + 1u);
            fputs("(alias-bound ", stream);
            if (bound->kind == CM_AST_SUPERTRAIT_LIFETIME) {
                fputs("required lifetime ", stream);
                cm_dump_string(stream, ast, bound->lifetime);
                fputs(")\n", stream);
                continue;
            }
            fputs(bound->modifier
                    == CM_AST_SUPERTRAIT_CONDITIONALLY_CONST
                ? "conditionally-const " : "required ", stream);
            cm_dump_type(stream, ast, bound->type);
            fputs(")\n", stream);
        }
        cm_dump_item_ids(stream, ast, item->data.trait_item.items,
            item->data.trait_item.item_count, depth + 1u);
        break;
    case CM_AST_ITEM_IMPL:
        cm_dump_indent(stream, depth + 1u);
        fprintf(stream,
            "(impl-header unsafe=%d const=%d negative=%d trait=",
            item->data.impl_item.is_unsafe,
            item->data.impl_item.is_const,
            item->data.impl_item.is_negative);
        cm_dump_type(stream, ast, item->data.impl_item.trait_type);
        fputs(" self=", stream);
        cm_dump_type(stream, ast, item->data.impl_item.self_type);
        fputs(")\n", stream);
        cm_dump_item_ids(stream, ast, item->data.impl_item.items,
            item->data.impl_item.item_count, depth + 1u);
        break;
    case CM_AST_ITEM_MACRO:
        cm_dump_indent(stream, depth + 1u);
        cm_dump_macro_invocation(stream, ast, &item->data.macro_item);
        fputc('\n', stream);
        break;
    }
    cm_dump_indent(stream, depth);
    fputs(")\n", stream);
}

int cm_ast_dump(FILE *stream, const CmAst *ast)
{
    size_t index;

    if (stream == NULL || ast == NULL) {
        return 0;
    }
    fputs("(crate\n", stream);
    cm_dump_attributes(stream, ast,
        (const CmAstAttributeId *)ast->crate_attributes.data,
        (uint32_t)ast->crate_attributes.len, 1u);
    for (index = 0u; index < ast->root_items.len; ++index) {
        const CmAstItemId *id;

        id = (const CmAstItemId *)cm_vec_at_const(&ast->root_items, index);
        if (id != NULL) {
            cm_dump_item(stream, ast, *id, 1u);
        }
    }
    fputs(")\n", stream);
    return ferror(stream) == 0;
}
