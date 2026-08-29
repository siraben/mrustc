#include "cm/syntax/parser.h"

#include "cm/alloc.h"
#include "cm/syntax/lexer.h"

#include <stdio.h>
#include <string.h>

typedef struct CmParser {
    CmAst *ast;
    const char *source;
    size_t source_length;
    CmVec tokens;
    size_t position;
    unsigned int pending_less;
    struct cm_token split_less_token;
    unsigned int pending_greater;
    struct cm_token split_greater_token;
    unsigned int pending_amp;
    struct cm_token split_amp_token;
    int has_synthetic_previous;
    struct cm_token synthetic_previous_token;
    unsigned int extern_block_depth;
    int next_item_is_impl_member;
    CmParseResult result;
} CmParser;

typedef struct CmItemPrefix {
    CmAstVisibility visibility;
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
    int is_const;
    int is_async;
    int is_safe;
    int is_unsafe;
    int is_default;
    int is_auto;
    CmInternId abi;
} CmItemPrefix;

static int cm_parser_collect_token(void *context, const struct cm_token *token)
{
    CmParser *parser;

    parser = (CmParser *)context;
    (void)cm_vec_push(&parser->tokens, token);
    return 0;
}

static const struct cm_token *cm_parser_token(const CmParser *parser)
{
    if (parser->pending_less != 0u) {
        return &parser->split_less_token;
    }
    if (parser->pending_greater != 0u) {
        return &parser->split_greater_token;
    }
    if (parser->pending_amp != 0u) {
        return &parser->split_amp_token;
    }
    if (parser->position >= parser->tokens.len) {
        return NULL;
    }
    return (const struct cm_token *)cm_vec_at_const(&parser->tokens,
        parser->position);
}

static enum cm_token_kind cm_parser_kind(const CmParser *parser)
{
    const struct cm_token *token;

    if (parser->pending_less != 0u) {
        return CM_TOKEN_LT;
    }
    if (parser->pending_greater != 0u) {
        return CM_TOKEN_GT;
    }
    if (parser->pending_amp != 0u) {
        return CM_TOKEN_AMP;
    }
    token = cm_parser_token(parser);
    return token == NULL ? CM_TOKEN_EOF : token->kind;
}

static const struct cm_token *cm_parser_previous(const CmParser *parser)
{
    if (parser->has_synthetic_previous) {
        return &parser->synthetic_previous_token;
    }
    if (parser->position == 0u || parser->position > parser->tokens.len) {
        return NULL;
    }
    return (const struct cm_token *)cm_vec_at_const(&parser->tokens,
        parser->position - 1u);
}

static void cm_parser_bump(CmParser *parser)
{
    if (parser->pending_less != 0u) {
        parser->synthetic_previous_token = parser->split_less_token;
        parser->has_synthetic_previous = 1;
        --parser->pending_less;
    } else if (parser->pending_greater != 0u) {
        parser->synthetic_previous_token = parser->split_greater_token;
        parser->has_synthetic_previous = 1;
        --parser->pending_greater;
    } else if (parser->pending_amp != 0u) {
        parser->synthetic_previous_token = parser->split_amp_token;
        parser->has_synthetic_previous = 1;
        --parser->pending_amp;
    } else if (parser->position < parser->tokens.len) {
        ++parser->position;
        parser->has_synthetic_previous = 0;
    }
}

static int cm_parser_eat(CmParser *parser, enum cm_token_kind kind)
{
    if (kind == CM_TOKEN_GT && parser->pending_greater != 0u) {
        cm_parser_bump(parser);
        return 1;
    }
    if (cm_parser_kind(parser) == kind) {
        cm_parser_bump(parser);
        return 1;
    }
    if (kind == CM_TOKEN_GT && cm_parser_kind(parser) == CM_TOKEN_SHR) {
        const struct cm_token *token;

        token = cm_parser_token(parser);
        parser->synthetic_previous_token = *token;
        parser->synthetic_previous_token.kind = CM_TOKEN_GT;
        parser->synthetic_previous_token.keyword = CM_KW_NONE;
        parser->synthetic_previous_token.length = 1u;
        parser->split_greater_token = parser->synthetic_previous_token;
        parser->split_greater_token.start += 1u;
        parser->split_greater_token.column += 1u;
        parser->has_synthetic_previous = 1;
        ++parser->position;
        parser->pending_greater = 1u;
        return 1;
    }
    return 0;
}

static int cm_parser_type_left_angle(const CmParser *parser)
{
    enum cm_token_kind kind;

    kind = cm_parser_kind(parser);
    return kind == CM_TOKEN_LT || kind == CM_TOKEN_SHL;
}

static int cm_parser_eat_type_amp(CmParser *parser)
{
    const struct cm_token *token;

    if (cm_parser_eat(parser, CM_TOKEN_AMP)) return 1;
    if (cm_parser_kind(parser) != CM_TOKEN_AMP_AMP) return 0;
    token = cm_parser_token(parser);
    parser->synthetic_previous_token = *token;
    parser->synthetic_previous_token.kind = CM_TOKEN_AMP;
    parser->synthetic_previous_token.keyword = CM_KW_NONE;
    parser->synthetic_previous_token.length = 1u;
    parser->split_amp_token = parser->synthetic_previous_token;
    parser->split_amp_token.start += 1u;
    parser->split_amp_token.column += 1u;
    parser->has_synthetic_previous = 1;
    ++parser->position;
    parser->pending_amp = 1u;
    return 1;
}

static int cm_parser_eat_type_left_angle(CmParser *parser)
{
    const struct cm_token *token;

    if (cm_parser_eat(parser, CM_TOKEN_LT)) {
        return 1;
    }
    if (cm_parser_kind(parser) == CM_TOKEN_SHL) {
        token = cm_parser_token(parser);
        parser->synthetic_previous_token = *token;
        parser->synthetic_previous_token.kind = CM_TOKEN_LT;
        parser->synthetic_previous_token.keyword = CM_KW_NONE;
        parser->synthetic_previous_token.length = 1u;
        parser->split_less_token = parser->synthetic_previous_token;
        parser->split_less_token.kind = CM_TOKEN_LT;
        parser->split_less_token.keyword = CM_KW_NONE;
        parser->split_less_token.start += 1u;
        parser->split_less_token.length = 1u;
        parser->split_less_token.column += 1u;
        parser->has_synthetic_previous = 1;
        ++parser->position;
        parser->pending_less = 1u;
        return 1;
    }
    return 0;
}

static int cm_parser_keyword(const CmParser *parser, enum cm_keyword keyword)
{
    const struct cm_token *token;

    token = cm_parser_token(parser);
    return token != NULL && token->keyword == keyword;
}

static int cm_parser_eat_keyword(CmParser *parser, enum cm_keyword keyword)
{
    if (cm_parser_keyword(parser, keyword)) {
        cm_parser_bump(parser);
        return 1;
    }
    return 0;
}

static const struct cm_token *cm_parser_next_token(const CmParser *parser);

static int cm_parser_text_is(const CmParser *parser, const char *text)
{
    const struct cm_token *token;
    size_t length;

    token = cm_parser_token(parser);
    length = strlen(text);
    return token != NULL && token->length == length &&
        memcmp(parser->source + token->start, text, length) == 0;
}

static int cm_parser_default_starts_item(const CmParser *parser)
{
    const struct cm_token *next;
    size_t length;

    if (!cm_parser_text_is(parser, "default")) return 0;
    next = cm_parser_next_token(parser);
    if (next == NULL) return 0;
    if (next->keyword == CM_KW_FN || next->keyword == CM_KW_TYPE
        || next->keyword == CM_KW_CONST || next->keyword == CM_KW_IMPL
        || next->keyword == CM_KW_UNSAFE || next->keyword == CM_KW_ASYNC
        || next->keyword == CM_KW_EXTERN) {
        return 1;
    }
    length = sizeof("default") - 1u;
    return next->length == length
        && memcmp(parser->source + next->start, "default", length) == 0;
}

static int cm_parser_auto_starts_trait(const CmParser *parser)
{
    const struct cm_token *next;

    if (!cm_parser_text_is(parser, "auto")) return 0;
    next = cm_parser_next_token(parser);
    return next != NULL && next->keyword == CM_KW_TRAIT;
}

static void cm_parser_error(CmParser *parser, const char *message)
{
    const struct cm_token *token;

    token = cm_parser_token(parser);
    ++parser->result.error_count;
    if (parser->result.error_count != 1u) {
        return;
    }
    if (token != NULL) {
        parser->result.first_error.offset = token->start;
        parser->result.first_error.line = token->line;
        parser->result.first_error.column = token->column;
    }
    (void)snprintf(parser->result.first_error.message,
        sizeof(parser->result.first_error.message), "%s", message);
}

static int cm_parser_expect_type_left_angle(CmParser *parser,
    const char *message)
{
    if (cm_parser_eat_type_left_angle(parser)) {
        return 1;
    }
    cm_parser_error(parser, message);
    return 0;
}

static int cm_parser_expect(CmParser *parser, enum cm_token_kind kind,
    const char *message)
{
    if (cm_parser_eat(parser, kind)) {
        return 1;
    }
    cm_parser_error(parser, message);
    return 0;
}

static int cm_parser_expect_keyword(CmParser *parser,
    enum cm_keyword keyword, const char *message)
{
    if (cm_parser_eat_keyword(parser, keyword)) {
        return 1;
    }
    cm_parser_error(parser, message);
    return 0;
}

static uint32_t cm_parser_offset_u32(CmParser *parser, size_t offset)
{
    if (offset > (size_t)UINT32_MAX) {
        cm_parser_error(parser, "source offset exceeds the AST format");
        return UINT32_MAX;
    }
    return (uint32_t)offset;
}

static size_t cm_token_end(const struct cm_token *token)
{
    return token == NULL ? 0u : token->start + token->length;
}

static int cm_parser_parse_tuple_index(CmParser *parser,
    const struct cm_token *token, uint32_t *out_index)
{
    uint32_t value;
    size_t index;

    *out_index = 0u;
    if (token == NULL || token->kind != CM_TOKEN_INTEGER
        || token->detail != 10u || token->suffix_start != SIZE_MAX
        || (token->flags & CM_TOKEN_F_INVALID) != 0u) {
        cm_parser_error(parser,
            "tuple index must be an unsuffixed decimal integer");
        return 0;
    }
    if (token->length == 0u
        || (token->length > 1u
            && parser->source[token->start] == '0')) {
        cm_parser_error(parser,
            "tuple index cannot contain leading zeros");
        return 0;
    }
    value = 0u;
    for (index = 0u; index < token->length; ++index) {
        unsigned int digit;
        unsigned char byte;

        byte = (unsigned char)parser->source[token->start + index];
        if (byte < (unsigned char)'0' || byte > (unsigned char)'9') {
            cm_parser_error(parser,
                "tuple index cannot contain underscores");
            return 0;
        }
        digit = (unsigned int)(byte - (unsigned char)'0');
        if (value > (UINT32_MAX - digit) / 10u) {
            cm_parser_error(parser, "tuple index exceeds the AST format");
            return 0;
        }
        value = value * 10u + digit;
    }
    *out_index = value;
    return 1;
}

static CmInternId cm_parser_intern_range(CmParser *parser, size_t start,
    size_t end)
{
    if (start > end || end > parser->source_length) {
        cm_parser_error(parser, "invalid source range");
        return CM_INTERN_ID_NONE;
    }
    while (start < end &&
        (parser->source[start] == ' ' || parser->source[start] == '\t' ||
         parser->source[start] == '\r' || parser->source[start] == '\n')) {
        ++start;
    }
    while (end > start &&
        (parser->source[end - 1u] == ' ' ||
         parser->source[end - 1u] == '\t' ||
         parser->source[end - 1u] == '\r' ||
         parser->source[end - 1u] == '\n')) {
        --end;
    }
    return cm_interner_intern(&parser->ast->strings,
        parser->source + start, end - start);
}

static CmInternId cm_parser_intern_token(CmParser *parser,
    const struct cm_token *token)
{
    size_t start;
    size_t length;

    if (token == NULL) {
        return CM_INTERN_ID_NONE;
    }
    start = token->start;
    length = token->length;
    if (token->kind == CM_TOKEN_RAW_IDENT && length >= 2u) {
        start += 2u;
        length -= 2u;
    }
    return cm_interner_intern(&parser->ast->strings,
        parser->source + start, length);
}

static void *cm_parser_copy_array(CmParser *parser, const CmVec *values)
{
    size_t size;
    void *copy;

    if (values->len == 0u) {
        return NULL;
    }
    if (!cm_size_mul(values->len, values->elem_size, &size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    copy = cm_arena_alloc(&parser->ast->storage, size, sizeof(void *));
    memcpy(copy, values->data, size);
    return copy;
}

static uint32_t cm_parser_count_u32(const CmVec *values)
{
    if (values->len > (size_t)UINT32_MAX) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    return (uint32_t)values->len;
}

static int cm_parser_is_name(const CmParser *parser)
{
    enum cm_token_kind kind;

    kind = cm_parser_kind(parser);
    return kind == CM_TOKEN_IDENT || kind == CM_TOKEN_RAW_IDENT;
}

static CmInternId cm_parser_parse_name(CmParser *parser,
    const char *message)
{
    const struct cm_token *token;
    CmInternId name;

    if (!cm_parser_is_name(parser)) {
        cm_parser_error(parser, message);
        return CM_INTERN_ID_NONE;
    }
    token = cm_parser_token(parser);
    name = cm_parser_intern_token(parser, token);
    cm_parser_bump(parser);
    return name;
}

static size_t cm_parser_skip_balanced(CmParser *parser,
    enum cm_token_kind opening, enum cm_token_kind closing)
{
    CmVec closings;
    const struct cm_token *previous;

    if (!cm_parser_expect(parser, opening, "expected opening delimiter")) {
        return 0u;
    }
    cm_vec_init(&closings, sizeof(enum cm_token_kind));
    (void)cm_vec_push(&closings, &closing);
    while (closings.len != 0u && cm_parser_kind(parser) != CM_TOKEN_EOF) {
        enum cm_token_kind kind;
        enum cm_token_kind nested_closing;

        kind = cm_parser_kind(parser);
        nested_closing = CM_TOKEN_EOF;
        if (kind == CM_TOKEN_LPAREN) {
            nested_closing = CM_TOKEN_RPAREN;
        } else if (kind == CM_TOKEN_LBRACKET) {
            nested_closing = CM_TOKEN_RBRACKET;
        } else if (kind == CM_TOKEN_LBRACE) {
            nested_closing = CM_TOKEN_RBRACE;
        }
        if (nested_closing != CM_TOKEN_EOF) {
            (void)cm_vec_push(&closings, &nested_closing);
            cm_parser_bump(parser);
            continue;
        }
        if (kind == CM_TOKEN_RPAREN || kind == CM_TOKEN_RBRACKET ||
            kind == CM_TOKEN_RBRACE) {
            const enum cm_token_kind *expected;

            expected = (const enum cm_token_kind *)cm_vec_at_const(
                &closings, closings.len - 1u);
            if (expected == NULL || *expected != kind) {
                cm_parser_error(parser, "mismatched delimiter");
                cm_vec_destroy(&closings);
                return cm_token_end(cm_parser_previous(parser));
            }
            --closings.len;
        }
        cm_parser_bump(parser);
    }
    if (closings.len != 0u) {
        cm_parser_error(parser, "unterminated delimited group");
    }
    cm_vec_destroy(&closings);
    previous = cm_parser_previous(parser);
    return cm_token_end(previous);
}

static CmInternId cm_parser_capture_until(CmParser *parser,
    enum cm_token_kind first_stop, enum cm_token_kind second_stop,
    enum cm_token_kind third_stop)
{
    const struct cm_token *first;
    const struct cm_token *previous;
    unsigned int parens;
    unsigned int brackets;
    unsigned int braces;
    unsigned int angles;
    size_t start;

    first = cm_parser_token(parser);
    if (first == NULL) {
        return CM_INTERN_ID_NONE;
    }
    start = first->start;
    previous = NULL;
    parens = 0u;
    brackets = 0u;
    braces = 0u;
    angles = 0u;
    for (;;) {
        enum cm_token_kind kind;

        kind = cm_parser_kind(parser);
        if (kind == CM_TOKEN_EOF) {
            break;
        }
        if (parens == 0u && brackets == 0u && braces == 0u && angles == 0u &&
            (kind == first_stop || kind == second_stop ||
             kind == third_stop)) {
            break;
        }
        if (kind == CM_TOKEN_LPAREN) ++parens;
        else if (kind == CM_TOKEN_RPAREN && parens != 0u) --parens;
        else if (kind == CM_TOKEN_LBRACKET) ++brackets;
        else if (kind == CM_TOKEN_RBRACKET && brackets != 0u) --brackets;
        else if (kind == CM_TOKEN_LBRACE) ++braces;
        else if (kind == CM_TOKEN_RBRACE && braces != 0u) --braces;
        else if (kind == CM_TOKEN_LT) ++angles;
        else if (kind == CM_TOKEN_GT && angles != 0u) --angles;
        previous = cm_parser_token(parser);
        cm_parser_bump(parser);
    }
    if (previous == NULL) {
        return CM_INTERN_ID_NONE;
    }
    return cm_parser_intern_range(parser, start, cm_token_end(previous));
}

static int cm_parser_macro_delimiter(enum cm_token_kind opening,
    enum cm_token_kind *closing, CmAstDelimiter *delimiter)
{
    if (opening == CM_TOKEN_LPAREN) {
        *closing = CM_TOKEN_RPAREN;
        *delimiter = CM_AST_DELIMITER_PAREN;
        return 1;
    }
    if (opening == CM_TOKEN_LBRACE) {
        *closing = CM_TOKEN_RBRACE;
        *delimiter = CM_AST_DELIMITER_BRACE;
        return 1;
    }
    if (opening == CM_TOKEN_LBRACKET) {
        *closing = CM_TOKEN_RBRACKET;
        *delimiter = CM_AST_DELIMITER_BRACKET;
        return 1;
    }
    return 0;
}

static int cm_parser_parse_macro_arguments(CmParser *parser,
    CmAstMacroInvocation *invocation, int consume_semicolon)
{
    const struct cm_token *opening_token;
    CmVec closings;
    enum cm_token_kind closing;
    size_t arguments_start;

    opening_token = cm_parser_token(parser);
    if (opening_token == NULL || !cm_parser_macro_delimiter(
        opening_token->kind, &closing, &invocation->delimiter)) {
        cm_parser_error(parser,
            "expected a delimited token tree after macro '!'");
        return 0;
    }
    cm_vec_init(&closings, sizeof(enum cm_token_kind));
    (void)cm_vec_push(&closings, &closing);
    arguments_start = cm_token_end(opening_token);
    cm_parser_bump(parser);
    while (closings.len != 0u && cm_parser_kind(parser) != CM_TOKEN_EOF) {
        const struct cm_token *token;
        enum cm_token_kind nested_closing;
        CmAstDelimiter nested_delimiter;

        token = cm_parser_token(parser);
        if (cm_parser_macro_delimiter(cm_parser_kind(parser),
            &nested_closing, &nested_delimiter)) {
            (void)nested_delimiter;
            (void)cm_vec_push(&closings, &nested_closing);
            cm_parser_bump(parser);
            continue;
        }
        if (cm_parser_kind(parser) == CM_TOKEN_RPAREN ||
            cm_parser_kind(parser) == CM_TOKEN_RBRACE ||
            cm_parser_kind(parser) == CM_TOKEN_RBRACKET) {
            const enum cm_token_kind *expected;

            expected = (const enum cm_token_kind *)cm_vec_at_const(
                &closings, closings.len - 1u);
            if (expected == NULL || *expected != cm_parser_kind(parser)) {
                cm_parser_error(parser,
                    "mismatched delimiter in macro token tree");
                cm_vec_destroy(&closings);
                return 0;
            }
            --closings.len;
            if (closings.len == 0u) {
                invocation->arguments = cm_parser_intern_range(parser,
                    arguments_start, token->start);
                cm_parser_bump(parser);
                invocation->has_semicolon = consume_semicolon &&
                    cm_parser_eat(parser, CM_TOKEN_SEMICOLON);
                cm_vec_destroy(&closings);
                return 1;
            }
            cm_parser_bump(parser);
            continue;
        }
        cm_parser_bump(parser);
    }
    cm_parser_error(parser, "unterminated macro token tree");
    cm_vec_destroy(&closings);
    return 0;
}

static CmAstTypeId cm_parser_parse_type(CmParser *parser);
static void cm_parser_parse_lifetime_binder(CmParser *parser,
    CmAstLifetimeBinder *binder);
static void cm_parser_parse_trait_type_bounds(CmParser *parser,
    CmAstType *type);
static int cm_parser_type_is_plain_sized_path(const CmParser *parser,
    CmAstTypeId type_id);
static int cm_parser_at_generic_constraint_end(const CmParser *parser);
static void cm_parser_parse_generic_constraint_bounds(CmParser *parser,
    CmAstGenericParamKind parameter_kind,
    CmAstGenericParamBound **out_bounds, uint32_t *out_bound_count);

static int cm_parser_generic_associated_name_has_separator(
    const CmParser *parser)
{
    CmParser probe;
    unsigned int angle_depth;
    unsigned int brace_depth;

    probe = *parser;
    if (!cm_parser_eat_type_left_angle(&probe)) return 0;
    angle_depth = 1u;
    brace_depth = 0u;
    while (cm_parser_kind(&probe) != CM_TOKEN_EOF) {
        enum cm_token_kind kind;

        kind = cm_parser_kind(&probe);
        if (brace_depth != 0u) {
            if (kind == CM_TOKEN_LBRACE) {
                ++brace_depth;
            } else if (kind == CM_TOKEN_RBRACE) {
                --brace_depth;
            }
            cm_parser_bump(&probe);
            continue;
        }
        if (kind == CM_TOKEN_LBRACE) {
            brace_depth = 1u;
            cm_parser_bump(&probe);
            continue;
        }
        if (cm_parser_type_left_angle(&probe)) {
            (void)cm_parser_eat_type_left_angle(&probe);
            ++angle_depth;
            continue;
        }
        if (kind == CM_TOKEN_GT || kind == CM_TOKEN_SHR) {
            (void)cm_parser_eat(&probe, CM_TOKEN_GT);
            --angle_depth;
            if (angle_depth == 0u) {
                kind = cm_parser_kind(&probe);
                return kind == CM_TOKEN_COLON || kind == CM_TOKEN_EQ;
            }
            continue;
        }
        cm_parser_bump(&probe);
    }
    return 0;
}

static void cm_parser_parse_generic_arguments(CmParser *parser,
    CmAstPathSegment *segment)
{
    CmVec arguments;

    cm_vec_init(&arguments, sizeof(CmAstGenericArg));
    if (!cm_parser_expect_type_left_angle(parser, "expected '<'")) {
        cm_vec_destroy(&arguments);
        return;
    }
    while (cm_parser_kind(parser) != CM_TOKEN_GT &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        const struct cm_token *argument_first;
        const struct cm_token *argument_last;
        CmAstGenericArg argument;

        memset(&argument, 0, sizeof(argument));
        argument_first = cm_parser_token(parser);
        if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
            const struct cm_token *token;

            token = cm_parser_token(parser);
            argument.kind = CM_AST_GENERIC_LIFETIME;
            argument.text = cm_parser_intern_token(parser, token);
            cm_parser_bump(parser);
        } else if (cm_parser_kind(parser) == CM_TOKEN_INTEGER ||
                   cm_parser_kind(parser) == CM_TOKEN_CHAR ||
                   cm_parser_kind(parser) == CM_TOKEN_STRING ||
                   cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
            const struct cm_token *first;
            const struct cm_token *last;

            first = cm_parser_token(parser);
            if (cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
                (void)cm_parser_skip_balanced(parser, CM_TOKEN_LBRACE,
                    CM_TOKEN_RBRACE);
            } else {
                cm_parser_bump(parser);
            }
            last = cm_parser_previous(parser);
            argument.kind = CM_AST_GENERIC_CONST;
            argument.text = cm_parser_intern_range(parser, first->start,
                cm_token_end(last));
        } else {
            const struct cm_token *name_token;
            size_t saved_position;
            unsigned int saved_less;
            unsigned int saved_greater;
            unsigned int saved_amp;
            struct cm_token saved_split_less_token;
            struct cm_token saved_split_greater_token;
            struct cm_token saved_split_amp_token;
            int saved_has_synthetic_previous;
            struct cm_token saved_synthetic_previous_token;
            CmAstPathSegment associated_name;
            enum cm_token_kind name_separator;
            int saw_name;

            name_token = cm_parser_token(parser);
            saved_position = parser->position;
            saved_less = parser->pending_less;
            saved_greater = parser->pending_greater;
            saved_amp = parser->pending_amp;
            saved_split_less_token = parser->split_less_token;
            saved_split_greater_token = parser->split_greater_token;
            saved_split_amp_token = parser->split_amp_token;
            saved_has_synthetic_previous = parser->has_synthetic_previous;
            saved_synthetic_previous_token =
                parser->synthetic_previous_token;
            memset(&associated_name, 0, sizeof(associated_name));
            saw_name = cm_parser_is_name(parser);
            if (saw_name) {
                cm_parser_bump(parser);
            }
            name_separator = saw_name ? cm_parser_kind(parser)
                : CM_TOKEN_EOF;
            if (saw_name && cm_parser_type_left_angle(parser)
                && cm_parser_generic_associated_name_has_separator(parser)) {
                cm_parser_parse_generic_arguments(parser, &associated_name);
                name_separator = cm_parser_kind(parser);
            }
            if (saw_name && name_separator == CM_TOKEN_EQ
                && cm_parser_eat(parser, CM_TOKEN_EQ)) {
                argument.kind = CM_AST_GENERIC_BINDING;
                argument.name = cm_parser_intern_token(parser, name_token);
                argument.name_arguments = associated_name.arguments;
                argument.name_argument_count =
                    associated_name.argument_count;
                argument.type = cm_parser_parse_type(parser);
            } else if (saw_name && name_separator == CM_TOKEN_COLON
                && cm_parser_eat(parser, CM_TOKEN_COLON)) {
                const struct cm_token *constraint_first;
                const struct cm_token *constraint_last;

                argument.kind = CM_AST_GENERIC_CONSTRAINT;
                argument.name = cm_parser_intern_token(parser, name_token);
                argument.name_arguments = associated_name.arguments;
                argument.name_argument_count =
                    associated_name.argument_count;
                constraint_first = cm_parser_token(parser);
                if (cm_parser_at_generic_constraint_end(parser)) {
                    cm_parser_error(parser,
                        "expected associated-type constraint after ':'");
                }
                cm_parser_parse_generic_constraint_bounds(parser,
                    CM_AST_PARAM_TYPE, &argument.bounds,
                    &argument.bound_count);
                constraint_last = cm_parser_previous(parser);
                if (constraint_first != NULL && constraint_last != NULL
                    && constraint_last->start >= constraint_first->start) {
                    argument.text = cm_parser_intern_range(parser,
                        constraint_first->start,
                        cm_token_end(constraint_last));
                }
            } else {
                parser->position = saved_position;
                parser->pending_less = saved_less;
                parser->pending_greater = saved_greater;
                parser->pending_amp = saved_amp;
                parser->split_less_token = saved_split_less_token;
                parser->split_greater_token = saved_split_greater_token;
                parser->split_amp_token = saved_split_amp_token;
                parser->has_synthetic_previous =
                    saved_has_synthetic_previous;
                parser->synthetic_previous_token =
                    saved_synthetic_previous_token;
                argument.kind = CM_AST_GENERIC_TYPE;
                argument.type = cm_parser_parse_type(parser);
            }
        }
        argument_last = cm_parser_previous(parser);
        argument.span.start = cm_parser_offset_u32(parser,
            argument_first == NULL ? 0u : argument_first->start);
        argument.span.end = cm_parser_offset_u32(parser,
            cm_token_end(argument_last));
        (void)cm_vec_push(&arguments, &argument);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) {
            break;
        }
    }
    (void)cm_parser_expect(parser, CM_TOKEN_GT,
        "expected '>' after generic arguments");
    segment->arguments = (CmAstGenericArg *)cm_parser_copy_array(parser,
        &arguments);
    segment->argument_count = cm_parser_count_u32(&arguments);
    cm_vec_destroy(&arguments);
}

static CmAstPathId cm_parser_parse_path(CmParser *parser,
    int split_left_shift)
{
    CmAstPath path;
    CmVec segments;
    const struct cm_token *first;
    const struct cm_token *last;

    memset(&path, 0, sizeof(path));
    cm_vec_init(&segments, sizeof(CmAstPathSegment));
    first = cm_parser_token(parser);
    if (cm_parser_eat(parser, CM_TOKEN_PATH_SEP)) {
        path.absolute = 1;
    }
    while (cm_parser_is_name(parser)) {
        CmAstPathSegment segment;

        memset(&segment, 0, sizeof(segment));
        segment.name = cm_parser_parse_name(parser, "expected path segment");
        if (cm_parser_kind(parser) == CM_TOKEN_LT ||
            (split_left_shift && cm_parser_kind(parser) == CM_TOKEN_SHL)) {
            cm_parser_parse_generic_arguments(parser, &segment);
        }
        (void)cm_vec_push(&segments, &segment);
        if (!cm_parser_eat(parser, CM_TOKEN_PATH_SEP)) {
            break;
        }
        if ((cm_parser_kind(parser) == CM_TOKEN_LT ||
                (split_left_shift &&
                    cm_parser_kind(parser) == CM_TOKEN_SHL))
            && segments.len != 0u) {
            CmAstPathSegment *last_segment;

            last_segment = (CmAstPathSegment *)cm_vec_at(&segments,
                segments.len - 1u);
            cm_parser_parse_generic_arguments(parser, last_segment);
            if (!cm_parser_eat(parser, CM_TOKEN_PATH_SEP)) {
                break;
            }
        }
    }
    if (segments.len == 0u) {
        cm_parser_error(parser, "expected path");
        cm_vec_destroy(&segments);
        return CM_AST_PATH_NONE;
    }
    path.segments = (CmAstPathSegment *)cm_parser_copy_array(parser,
        &segments);
    path.segment_count = cm_parser_count_u32(&segments);
    last = cm_parser_previous(parser);
    path.span.start = cm_parser_offset_u32(parser,
        first == NULL ? 0u : first->start);
    path.span.end = cm_parser_offset_u32(parser, cm_token_end(last));
    cm_vec_destroy(&segments);
    return cm_ast_add_path(parser->ast, &path);
}

static CmAstTypeId cm_parser_add_simple_type(CmParser *parser,
    CmAstTypeKind kind, size_t start, size_t end)
{
    CmAstType type;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span.start = cm_parser_offset_u32(parser, start);
    type.span.end = cm_parser_offset_u32(parser, end);
    return cm_ast_add_type(parser->ast, &type);
}

static void cm_parser_parse_projection_type(CmParser *parser,
    CmAstType *type)
{
    const struct cm_token *previous;

    type->kind = CM_AST_TYPE_PROJECTION;
    if (cm_parser_keyword(parser, CM_KW_AS) ||
        cm_parser_kind(parser) == CM_TOKEN_GT ||
        cm_parser_kind(parser) == CM_TOKEN_EOF) {
        cm_parser_error(parser, "expected self type after '<'");
    } else {
        type->projection.self_type = cm_parser_parse_type(parser);
    }
    (void)cm_parser_expect_keyword(parser, CM_KW_AS,
        "expected 'as' in explicit type projection");
    if (cm_parser_kind(parser) == CM_TOKEN_GT ||
        cm_parser_kind(parser) == CM_TOKEN_EOF) {
        cm_parser_error(parser, "expected trait path after 'as'");
    } else {
        type->projection.trait_path = cm_parser_parse_path(parser, 1);
        previous = cm_parser_previous(parser);
        if (previous != NULL && previous->kind == CM_TOKEN_PATH_SEP) {
            cm_parser_error(parser,
                "expected trait path segment after '::'");
        }
    }
    (void)cm_parser_expect(parser, CM_TOKEN_GT,
        "expected '>' after projection trait path");
    (void)cm_parser_expect(parser, CM_TOKEN_PATH_SEP,
        "expected '::' after projection qualifier");
    type->projection.associated.name = cm_parser_parse_name(parser,
        "expected associated type name after projection qualifier");
    if (cm_parser_type_left_angle(parser)) {
        cm_parser_parse_generic_arguments(parser,
            &type->projection.associated);
    }
    if (cm_parser_kind(parser) == CM_TOKEN_PATH_SEP) {
        cm_parser_error(parser,
            "multi-segment associated type projections are unsupported");
    }
}

static CmAstTypeId cm_parser_parse_type(CmParser *parser)
{
    const struct cm_token *first;
    CmAstType type;
    size_t first_start;

    first = cm_parser_token(parser);
    first_start = first != NULL ? first->start
        : 0u;
    memset(&type, 0, sizeof(type));
    if (cm_parser_eat_type_amp(parser)) {
        type.kind = CM_AST_TYPE_REFERENCE;
        if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
            type.lifetime = cm_parser_intern_token(parser,
                cm_parser_token(parser));
            cm_parser_bump(parser);
        }
        type.is_mutable = cm_parser_eat_keyword(parser, CM_KW_MUT);
        type.child = cm_parser_parse_type(parser);
    } else if (cm_parser_eat(parser, CM_TOKEN_STAR)) {
        type.kind = CM_AST_TYPE_POINTER;
        if (cm_parser_eat_keyword(parser, CM_KW_MUT)) {
            type.is_mutable = 1;
        } else {
            (void)cm_parser_expect_keyword(parser, CM_KW_CONST,
                "expected const or mut after '*'");
        }
        type.child = cm_parser_parse_type(parser);
    } else if (cm_parser_eat(parser, CM_TOKEN_LPAREN)) {
        CmVec elements;

        type.kind = CM_AST_TYPE_TUPLE;
        cm_vec_init(&elements, sizeof(CmAstTypeId));
        if (cm_parser_kind(parser) != CM_TOKEN_RPAREN
            && cm_parser_kind(parser) != CM_TOKEN_EOF) {
            CmAstTypeId element;

            element = cm_parser_parse_type(parser);
            if (cm_parser_kind(parser) == CM_TOKEN_RPAREN) {
                cm_parser_bump(parser);
                cm_vec_destroy(&elements);
                return element;
            }
            (void)cm_vec_push(&elements, &element);
            while (cm_parser_eat(parser, CM_TOKEN_COMMA)
                   && cm_parser_kind(parser) != CM_TOKEN_RPAREN
                   && cm_parser_kind(parser) != CM_TOKEN_EOF) {
                element = cm_parser_parse_type(parser);
                (void)cm_vec_push(&elements, &element);
            }
        }
        (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
            "expected ')' after tuple type");
        type.elements = (CmAstTypeId *)cm_parser_copy_array(parser,
            &elements);
        type.element_count = cm_parser_count_u32(&elements);
        cm_vec_destroy(&elements);
    } else if (cm_parser_eat(parser, CM_TOKEN_LBRACKET)) {
        type.child = cm_parser_parse_type(parser);
        if (cm_parser_eat(parser, CM_TOKEN_SEMICOLON)) {
            type.kind = CM_AST_TYPE_ARRAY;
            type.text = cm_parser_capture_until(parser, CM_TOKEN_RBRACKET,
                CM_TOKEN_EOF, CM_TOKEN_EOF);
        } else {
            type.kind = CM_AST_TYPE_SLICE;
        }
        (void)cm_parser_expect(parser, CM_TOKEN_RBRACKET,
            "expected ']' after array or slice type");
    } else if (cm_parser_eat_keyword(parser, CM_KW_IMPL)) {
        type.kind = CM_AST_TYPE_IMPL_TRAIT;
        cm_parser_parse_trait_type_bounds(parser, &type);
    } else if (cm_parser_eat_keyword(parser, CM_KW_DYN)) {
        type.kind = CM_AST_TYPE_DYN_TRAIT;
        cm_parser_parse_trait_type_bounds(parser, &type);
    } else if (cm_parser_keyword(parser, CM_KW_FOR)
        || cm_parser_keyword(parser, CM_KW_UNSAFE)
        || cm_parser_keyword(parser, CM_KW_FN)) {
        CmVec parameters;

        type.kind = CM_AST_TYPE_FUNCTION;
        if (cm_parser_keyword(parser, CM_KW_FOR)) {
            cm_parser_parse_lifetime_binder(parser, &type.binder);
        }
        type.is_unsafe = cm_parser_eat_keyword(parser, CM_KW_UNSAFE);
        (void)cm_parser_expect_keyword(parser, CM_KW_FN,
            "expected 'fn' in function pointer type");
        cm_vec_init(&parameters, sizeof(CmAstTypeId));
        (void)cm_parser_expect(parser, CM_TOKEN_LPAREN,
            "expected '(' after fn type");
        while (cm_parser_kind(parser) != CM_TOKEN_RPAREN &&
               cm_parser_kind(parser) != CM_TOKEN_EOF) {
            CmAstTypeId parameter;

            parameter = cm_parser_parse_type(parser);
            (void)cm_vec_push(&parameters, &parameter);
            if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) {
                break;
            }
        }
        (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
            "expected ')' after fn type parameters");
        if (cm_parser_eat(parser, CM_TOKEN_THIN_ARROW)) {
            type.child = cm_parser_parse_type(parser);
        } else {
            type.child = cm_parser_add_simple_type(parser, CM_AST_TYPE_TUPLE,
                first_start, first_start);
        }
        type.elements = (CmAstTypeId *)cm_parser_copy_array(parser,
            &parameters);
        type.element_count = cm_parser_count_u32(&parameters);
        cm_vec_destroy(&parameters);
    } else if (cm_parser_kind(parser) == CM_TOKEN_BANG) {
        const struct cm_token *token;

        token = cm_parser_token(parser);
        cm_parser_bump(parser);
        return cm_parser_add_simple_type(parser, CM_AST_TYPE_NEVER,
            token->start, cm_token_end(token));
    } else if (cm_parser_text_is(parser, "_")) {
        const struct cm_token *token;

        token = cm_parser_token(parser);
        cm_parser_bump(parser);
        return cm_parser_add_simple_type(parser, CM_AST_TYPE_INFER,
            token->start, cm_token_end(token));
    } else if (cm_parser_eat_type_left_angle(parser)) {
        cm_parser_parse_projection_type(parser, &type);
    } else if (cm_parser_is_name(parser) ||
               cm_parser_kind(parser) == CM_TOKEN_PATH_SEP) {
        type.kind = CM_AST_TYPE_PATH;
        type.path = cm_parser_parse_path(parser, 1);
        if (cm_parser_eat(parser, CM_TOKEN_BANG)) {
            type.kind = CM_AST_TYPE_MACRO;
            type.macro_type.path = type.path;
            type.path = CM_AST_PATH_NONE;
            (void)cm_parser_parse_macro_arguments(parser,
                &type.macro_type, 0);
        }
    } else {
        const struct cm_token *token;

        token = cm_parser_token(parser);
        cm_parser_error(parser, "expected type");
        if (token != NULL) {
            type.kind = CM_AST_TYPE_OTHER;
            type.text = cm_parser_intern_token(parser, token);
            cm_parser_bump(parser);
        }
    }
    type.span.start = cm_parser_offset_u32(parser, first_start);
    type.span.end = cm_parser_offset_u32(parser,
        cm_token_end(cm_parser_previous(parser)));
    return cm_ast_add_type(parser->ast, &type);
}

static int cm_parser_is_literal(const CmParser *parser)
{
    enum cm_token_kind kind;

    kind = cm_parser_kind(parser);
    return kind == CM_TOKEN_INTEGER || kind == CM_TOKEN_FLOAT ||
        kind == CM_TOKEN_CHAR || kind == CM_TOKEN_BYTE_CHAR ||
        kind == CM_TOKEN_STRING || kind == CM_TOKEN_BYTE_STRING ||
        kind == CM_TOKEN_C_STRING || kind == CM_TOKEN_RAW_STRING ||
        kind == CM_TOKEN_RAW_BYTE_STRING || kind == CM_TOKEN_RAW_C_STRING ||
        cm_parser_keyword(parser, CM_KW_TRUE) ||
        cm_parser_keyword(parser, CM_KW_FALSE);
}

static CmAstSpan cm_parser_span_from(CmParser *parser,
    const struct cm_token *first)
{
    CmAstSpan span;

    span.start = cm_parser_offset_u32(parser,
        first == NULL ? 0u : first->start);
    span.end = cm_parser_offset_u32(parser,
        cm_token_end(cm_parser_previous(parser)));
    return span;
}

static CmAstPatternId cm_parser_parse_pattern(CmParser *parser);
static CmAstPatternId cm_parser_parse_pattern_atom(CmParser *parser);
static CmAstPatternId cm_parser_parse_range_pattern(CmParser *parser);
static CmAstExprId cm_parser_parse_expression(CmParser *parser);
static CmAstExprId cm_parser_parse_expression_bp(CmParser *parser,
    unsigned int minimum_precedence);
static CmAstExprId cm_parser_parse_expression_bp_mode(CmParser *parser,
    unsigned int minimum_precedence, int allow_struct_literal,
    int stop_after_block_like);
static CmAstExprId cm_parser_parse_expression_without_struct(
    CmParser *parser);
static CmAstExprId cm_parser_parse_block(CmParser *parser);
static CmAstExprId cm_parser_parse_block_mode(CmParser *parser,
    const struct cm_token *first, int is_unsafe, int is_const);
static CmAstItemId cm_parser_parse_item(CmParser *parser);
static void cm_parser_parse_inner_attributes(CmParser *parser,
    CmVec *attributes);
static void cm_parser_parse_attributes(CmParser *parser, CmVec *attributes);
static void cm_parser_attach_expression_attributes(CmParser *parser,
    CmAstExprId expression_id, const CmVec *attributes);

static CmAstPatternId cm_parser_add_list_pattern(CmParser *parser,
    CmAstPatternKind kind, const struct cm_token *first, CmVec *patterns,
    int has_rest, uint32_t rest_index)
{
    CmAstPattern pattern;

    memset(&pattern, 0, sizeof(pattern));
    pattern.kind = kind;
    pattern.span = cm_parser_span_from(parser, first);
    pattern.data.list.patterns = (CmAstPatternId *)cm_parser_copy_array(
        parser, patterns);
    pattern.data.list.pattern_count = cm_parser_count_u32(patterns);
    pattern.data.list.has_rest = has_rest;
    pattern.data.list.rest_index = rest_index;
    return cm_ast_add_pattern(parser->ast, &pattern);
}

static CmAstPatternId cm_parser_parse_pattern_atom(CmParser *parser)
{
    const struct cm_token *first;
    CmAstPattern pattern;
    int binding_ref;
    int binding_mut;

    first = cm_parser_token(parser);
    memset(&pattern, 0, sizeof(pattern));
    binding_ref = cm_parser_eat_keyword(parser, CM_KW_REF);
    binding_mut = cm_parser_eat_keyword(parser, CM_KW_MUT);
    if (binding_ref || binding_mut) {
        pattern.kind = CM_AST_PATTERN_BINDING;
        pattern.data.binding.is_ref = binding_ref;
        pattern.data.binding.is_mutable = binding_mut;
        pattern.data.binding.name = cm_parser_parse_name(parser,
            "expected binding name");
        /* `x @ lo..=hi`: the subpattern includes a range; `x @ ..` is
         * the rest pattern. */
        if (cm_parser_eat(parser, CM_TOKEN_AT))
            pattern.data.binding.subpattern =
                cm_parser_kind(parser) == CM_TOKEN_DOT_DOT
                ? cm_parser_parse_pattern_atom(parser)
                : cm_parser_parse_range_pattern(parser);
        pattern.span = cm_parser_span_from(parser, first);
        return cm_ast_add_pattern(parser->ast, &pattern);
    }
    if (cm_parser_eat(parser, CM_TOKEN_DOT_DOT)) {
        pattern.kind = CM_AST_PATTERN_REST;
        pattern.span = cm_parser_span_from(parser, first);
        return cm_ast_add_pattern(parser->ast, &pattern);
    }
    if (cm_parser_text_is(parser, "_")) {
        pattern.kind = CM_AST_PATTERN_WILDCARD;
        cm_parser_bump(parser);
        pattern.span = cm_parser_span_from(parser, first);
        return cm_ast_add_pattern(parser->ast, &pattern);
    }
    if (cm_parser_eat_type_amp(parser)) {
        pattern.kind = CM_AST_PATTERN_REFERENCE;
        pattern.data.reference.is_mutable = cm_parser_eat_keyword(parser,
            CM_KW_MUT);
        pattern.data.reference.pattern = cm_parser_parse_pattern_atom(parser);
        pattern.span = cm_parser_span_from(parser, first);
        return cm_ast_add_pattern(parser->ast, &pattern);
    }
    if (cm_parser_is_literal(parser) ||
        (cm_parser_kind(parser) == CM_TOKEN_MINUS &&
         parser->position + 1u < parser->tokens.len &&
         ((const struct cm_token *)cm_vec_at_const(&parser->tokens,
             parser->position + 1u))->kind == CM_TOKEN_INTEGER)) {
        const struct cm_token *last;

        if (cm_parser_kind(parser) == CM_TOKEN_MINUS) cm_parser_bump(parser);
        cm_parser_bump(parser);
        last = cm_parser_previous(parser);
        pattern.kind = CM_AST_PATTERN_LITERAL;
        pattern.data.literal.text = cm_parser_intern_range(parser,
            first->start, cm_token_end(last));
        pattern.span = cm_parser_span_from(parser, first);
        return cm_ast_add_pattern(parser->ast, &pattern);
    }
    if (cm_parser_eat(parser, CM_TOKEN_LPAREN)) {
        CmVec elements;
        int has_rest;
        uint32_t rest_index;

        cm_vec_init(&elements, sizeof(CmAstPatternId));
        has_rest = 0;
        rest_index = 0u;
        while (cm_parser_kind(parser) != CM_TOKEN_RPAREN &&
               cm_parser_kind(parser) != CM_TOKEN_EOF) {
            CmAstPatternId element;

            if (cm_parser_eat(parser, CM_TOKEN_DOT_DOT)) {
                has_rest = 1;
                rest_index = cm_parser_count_u32(&elements);
            } else {
                element = cm_parser_parse_pattern(parser);
                (void)cm_vec_push(&elements, &element);
            }
            if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
        }
        (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
            "expected ')' after tuple pattern");
        {
            CmAstPatternId id;

            id = cm_parser_add_list_pattern(parser, CM_AST_PATTERN_TUPLE,
                first, &elements, has_rest, rest_index);
            cm_vec_destroy(&elements);
            return id;
        }
    }
    if (cm_parser_eat(parser, CM_TOKEN_LBRACKET)) {
        CmVec elements;
        int has_rest;
        uint32_t rest_index;

        cm_vec_init(&elements, sizeof(CmAstPatternId));
        has_rest = 0;
        rest_index = 0u;
        while (cm_parser_kind(parser) != CM_TOKEN_RBRACKET &&
               cm_parser_kind(parser) != CM_TOKEN_EOF) {
            CmAstPatternId element;

            if (cm_parser_eat(parser, CM_TOKEN_DOT_DOT)) {
                has_rest = 1;
                rest_index = cm_parser_count_u32(&elements);
            } else {
                element = cm_parser_parse_pattern(parser);
                (void)cm_vec_push(&elements, &element);
            }
            if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
        }
        (void)cm_parser_expect(parser, CM_TOKEN_RBRACKET,
            "expected ']' after slice pattern");
        {
            CmAstPatternId id;

            id = cm_parser_add_list_pattern(parser, CM_AST_PATTERN_SLICE,
                first, &elements, has_rest, rest_index);
            cm_vec_destroy(&elements);
            return id;
        }
    }
    if (cm_parser_is_name(parser) ||
        cm_parser_kind(parser) == CM_TOKEN_PATH_SEP) {
        const struct cm_token *name_token;
        CmAstPathId path_id;
        const CmAstPath *path;

        name_token = cm_parser_token(parser);
        path_id = cm_parser_parse_path(parser, 0);
        path = cm_ast_get_path(parser->ast, path_id);
        if (cm_parser_eat(parser, CM_TOKEN_LPAREN)) {
            CmVec fields;

            cm_vec_init(&fields, sizeof(CmAstPatternField));
            while (cm_parser_kind(parser) != CM_TOKEN_RPAREN &&
                   cm_parser_kind(parser) != CM_TOKEN_EOF) {
                CmAstPatternField field;

                memset(&field, 0, sizeof(field));
                if (cm_parser_eat(parser, CM_TOKEN_DOT_DOT)) {
                    pattern.data.struct_pattern.has_rest = 1;
                } else {
                    field.pattern = cm_parser_parse_pattern(parser);
                    (void)cm_vec_push(&fields, &field);
                }
                if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
            }
            (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
                "expected ')' after tuple-struct pattern");
            pattern.kind = CM_AST_PATTERN_STRUCT;
            pattern.data.struct_pattern.path = path_id;
            pattern.data.struct_pattern.is_tuple = 1;
            pattern.data.struct_pattern.fields =
                (CmAstPatternField *)cm_parser_copy_array(parser, &fields);
            pattern.data.struct_pattern.field_count =
                cm_parser_count_u32(&fields);
            cm_vec_destroy(&fields);
        } else if (cm_parser_eat(parser, CM_TOKEN_LBRACE)) {
            CmVec fields;

            cm_vec_init(&fields, sizeof(CmAstPatternField));
            while (cm_parser_kind(parser) != CM_TOKEN_RBRACE &&
                   cm_parser_kind(parser) != CM_TOKEN_EOF) {
                CmAstPatternField field;

                memset(&field, 0, sizeof(field));
                if (cm_parser_eat(parser, CM_TOKEN_DOT_DOT)) {
                    pattern.data.struct_pattern.has_rest = 1;
                } else if (cm_parser_keyword(parser, CM_KW_REF) ||
                           cm_parser_keyword(parser, CM_KW_MUT)) {
                    const CmAstPattern *binding;

                    field.pattern = cm_parser_parse_pattern_atom(parser);
                    binding = cm_ast_get_pattern(parser->ast,
                        field.pattern);
                    if (binding != NULL &&
                        binding->kind == CM_AST_PATTERN_BINDING) {
                        field.name = binding->data.binding.name;
                    }
                    (void)cm_vec_push(&fields, &field);
                } else {
                    field.name = cm_parser_parse_name(parser,
                        "expected pattern field");
                    if (cm_parser_eat(parser, CM_TOKEN_COLON)) {
                        field.pattern = cm_parser_parse_pattern(parser);
                    } else {
                        CmAstPattern shorthand;

                        memset(&shorthand, 0, sizeof(shorthand));
                        shorthand.kind = CM_AST_PATTERN_BINDING;
                        shorthand.data.binding.name = field.name;
                        shorthand.span = cm_parser_span_from(parser, first);
                        field.pattern = cm_ast_add_pattern(parser->ast,
                            &shorthand);
                        field.is_shorthand = 1;
                    }
                    (void)cm_vec_push(&fields, &field);
                }
                if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
            }
            (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
                "expected '}' after struct pattern");
            pattern.kind = CM_AST_PATTERN_STRUCT;
            pattern.data.struct_pattern.path = path_id;
            pattern.data.struct_pattern.fields =
                (CmAstPatternField *)cm_parser_copy_array(parser, &fields);
            pattern.data.struct_pattern.field_count =
                cm_parser_count_u32(&fields);
            cm_vec_destroy(&fields);
        } else if (path != NULL && path->segment_count == 1u &&
                   !path->absolute && name_token != NULL &&
                   name_token->length != 0u &&
                   ((parser->source[name_token->start] >= 'a' &&
                     parser->source[name_token->start] <= 'z') ||
                    (parser->source[name_token->start] == '_' &&
                     name_token->length > 1u))) {
            pattern.kind = CM_AST_PATTERN_BINDING;
            pattern.data.binding.name = path->segments[0].name;
            if (cm_parser_eat(parser, CM_TOKEN_AT))
                pattern.data.binding.subpattern =
                    cm_parser_kind(parser) == CM_TOKEN_DOT_DOT
                    ? cm_parser_parse_pattern_atom(parser)
                    : cm_parser_parse_range_pattern(parser);
        } else {
            pattern.kind = CM_AST_PATTERN_PATH;
            pattern.data.path.path = path_id;
        }
        pattern.span = cm_parser_span_from(parser, first);
        return cm_ast_add_pattern(parser->ast, &pattern);
    }
    cm_parser_error(parser, "expected pattern");
    if (cm_parser_kind(parser) != CM_TOKEN_EOF) cm_parser_bump(parser);
    pattern.kind = CM_AST_PATTERN_WILDCARD;
    pattern.span = cm_parser_span_from(parser, first);
    return cm_ast_add_pattern(parser->ast, &pattern);
}

static CmAstPatternId cm_parser_parse_range_pattern(CmParser *parser)
{
    const struct cm_token *first;
    CmAstPatternId left;

    first = cm_parser_token(parser);
    if (cm_parser_kind(parser) == CM_TOKEN_DOT_DOT ||
        cm_parser_kind(parser) == CM_TOKEN_DOT_DOT_EQ ||
        cm_parser_kind(parser) == CM_TOKEN_DOT_DOT_DOT) {
        CmAstPattern pattern;
        enum cm_token_kind operator_kind;

        memset(&pattern, 0, sizeof(pattern));
        operator_kind = cm_parser_kind(parser);
        cm_parser_bump(parser);
        pattern.kind = CM_AST_PATTERN_RANGE;
        pattern.data.range.end = cm_parser_parse_pattern_atom(parser);
        pattern.data.range.is_inclusive =
            operator_kind != CM_TOKEN_DOT_DOT;
        pattern.span = cm_parser_span_from(parser, first);
        return cm_ast_add_pattern(parser->ast, &pattern);
    }
    left = cm_parser_parse_pattern_atom(parser);
    if (cm_parser_kind(parser) == CM_TOKEN_DOT_DOT ||
        cm_parser_kind(parser) == CM_TOKEN_DOT_DOT_EQ ||
        cm_parser_kind(parser) == CM_TOKEN_DOT_DOT_DOT) {
        CmAstPattern pattern;
        enum cm_token_kind operator_kind;

        memset(&pattern, 0, sizeof(pattern));
        operator_kind = cm_parser_kind(parser);
        cm_parser_bump(parser);
        pattern.kind = CM_AST_PATTERN_RANGE;
        pattern.data.range.start = left;
        pattern.data.range.end = cm_parser_parse_pattern_atom(parser);
        pattern.data.range.is_inclusive =
            operator_kind != CM_TOKEN_DOT_DOT;
        pattern.span = cm_parser_span_from(parser, first);
        left = cm_ast_add_pattern(parser->ast, &pattern);
    }
    return left;
}

static CmAstPatternId cm_parser_parse_pattern(CmParser *parser)
{
    const struct cm_token *first;
    CmAstPatternId left;

    first = cm_parser_token(parser);
    (void)cm_parser_eat(parser, CM_TOKEN_PIPE);
    left = cm_parser_parse_range_pattern(parser);
    if (cm_parser_eat(parser, CM_TOKEN_PIPE)) {
        CmVec alternatives;

        cm_vec_init(&alternatives, sizeof(CmAstPatternId));
        (void)cm_vec_push(&alternatives, &left);
        do {
            CmAstPatternId alternative;

            alternative = cm_parser_parse_range_pattern(parser);
            (void)cm_vec_push(&alternatives, &alternative);
        } while (cm_parser_eat(parser, CM_TOKEN_PIPE));
        left = cm_parser_add_list_pattern(parser, CM_AST_PATTERN_OR, first,
            &alternatives, 0, 0u);
        cm_vec_destroy(&alternatives);
    }
    return left;
}

static int cm_parser_pattern_is_self(const CmParser *parser,
    CmAstPatternId id)
{
    const CmAstPattern *pattern;

    pattern = cm_ast_get_pattern(parser->ast, id);
    if (pattern == NULL) return 0;
    if (pattern->kind == CM_AST_PATTERN_REFERENCE)
        return cm_parser_pattern_is_self(parser,
            pattern->data.reference.pattern);
    if (pattern->kind == CM_AST_PATTERN_BINDING) {
        const CmInternedString *name;

        name = cm_ast_get_string(parser->ast, pattern->data.binding.name);
        return name != NULL && name->len == 4u &&
            memcmp(name->bytes, "self", 4u) == 0;
    }
    return 0;
}

static int cm_parser_ordinary_identifier(const struct cm_token *token)
{
    if (token == NULL) return 0;
    if (token->kind == CM_TOKEN_RAW_IDENT) return 1;
    return token->kind == CM_TOKEN_IDENT
        && (token->keyword == CM_KW_NONE
            || (token->flags & CM_TOKEN_F_WEAK_KEYWORD) != 0u);
}

static const struct cm_token *cm_parser_next_token(const CmParser *parser)
{
    if (parser->position + 1u >= parser->tokens.len) return NULL;
    return (const struct cm_token *)cm_vec_at_const(&parser->tokens,
        parser->position + 1u);
}

static const struct cm_token *cm_parser_token_at(const CmParser *parser,
    size_t position)
{
    if (position >= parser->tokens.len) return NULL;
    return (const struct cm_token *)cm_vec_at_const(&parser->tokens,
        position);
}

static int cm_parser_token_text_is(const CmParser *parser,
    const struct cm_token *token, const char *text)
{
    size_t length;

    if (token == NULL) return 0;
    length = strlen(text);
    return token->length == length
        && memcmp(parser->source + token->start, text, length) == 0;
}

static int cm_parser_starts_local_item(const CmParser *parser)
{
    const struct cm_token *token;
    size_t position;

    position = parser->position;
    token = cm_parser_token_at(parser, position);
    while (token != NULL && token->kind == CM_TOKEN_POUND) {
        const struct cm_token *opening;
        unsigned int depth;

        opening = cm_parser_token_at(parser, position + 1u);
        if (opening == NULL || opening->kind != CM_TOKEN_LBRACKET) return 0;
        position += 2u;
        depth = 1u;
        while (depth != 0u) {
            token = cm_parser_token_at(parser, position);
            if (token == NULL || token->kind == CM_TOKEN_EOF) return 0;
            if (token->kind == CM_TOKEN_LBRACKET) ++depth;
            if (token->kind == CM_TOKEN_RBRACKET) --depth;
            ++position;
        }
        token = cm_parser_token_at(parser, position);
    }
    if (token == NULL) return 0;
    if (cm_parser_token_text_is(parser, token, "macro_rules")) return 1;
    if (token->keyword == CM_KW_FN || token->keyword == CM_KW_IMPL
        || token->keyword == CM_KW_STATIC
        || token->keyword == CM_KW_STRUCT
        || token->keyword == CM_KW_TRAIT
        || token->keyword == CM_KW_UNION
        || token->keyword == CM_KW_USE) return 1;
    if (token->keyword == CM_KW_CONST) {
        const struct cm_token *next;

        next = cm_parser_token_at(parser, position + 1u);
        /* `const unsafe fn` / `const async fn` / `const extern "C" fn`
         * nested in a body (core's `align_offset` keeps a `const unsafe
         * fn mod_inv`) are items, not a `const { .. }` block. */
        return cm_parser_ordinary_identifier(next)
            || (next != NULL && (next->keyword == CM_KW_FN
                || next->keyword == CM_KW_UNSAFE
                || next->keyword == CM_KW_ASYNC
                || next->keyword == CM_KW_EXTERN));
    }
    if (token->keyword == CM_KW_ASYNC
        || token->keyword == CM_KW_UNSAFE) {
        const struct cm_token *next;

        next = cm_parser_token_at(parser, position + 1u);
        if (next != NULL && next->keyword == CM_KW_FN) return 1;
        if (token->keyword == CM_KW_UNSAFE && next != NULL
            && next->keyword == CM_KW_EXTERN) {
            position += 2u;
            token = cm_parser_token_at(parser, position);
            if (token != NULL && token->kind == CM_TOKEN_STRING) {
                position += 1u;
                token = cm_parser_token_at(parser, position);
            }
            return token != NULL && token->kind == CM_TOKEN_LBRACE;
        }
        return 0;
    }
    return 0;
}

static int cm_parser_const_starts_block(const CmParser *parser)
{
    const struct cm_token *next;

    next = cm_parser_next_token(parser);
    return cm_parser_keyword(parser, CM_KW_CONST)
        && next != NULL && next->kind == CM_TOKEN_LBRACE;
}

static int cm_parser_struct_path_segment_valid(
    const struct cm_token *token)
{
    if (cm_parser_ordinary_identifier(token)) return 1;
    if (token == NULL || token->kind != CM_TOKEN_IDENT) return 0;
    return token->keyword == CM_KW_CRATE
        || token->keyword == CM_KW_SELF_VALUE
        || token->keyword == CM_KW_SELF_TYPE
        || token->keyword == CM_KW_SUPER;
}

static CmAstPathId cm_parser_parse_expression_path(CmParser *parser,
    int *out_struct_path_valid)
{
    CmAstPath path;
    CmVec segments;
    const struct cm_token *first;

    memset(&path, 0, sizeof(path));
    cm_vec_init(&segments, sizeof(CmAstPathSegment));
    first = cm_parser_token(parser);
    if (out_struct_path_valid != NULL) *out_struct_path_valid = 1;
    path.absolute = cm_parser_eat(parser, CM_TOKEN_PATH_SEP);
    while (cm_parser_is_name(parser)) {
        CmAstPathSegment segment;

        memset(&segment, 0, sizeof(segment));
        if (out_struct_path_valid != NULL
            && !cm_parser_struct_path_segment_valid(
                cm_parser_token(parser))) {
            *out_struct_path_valid = 0;
        }
        segment.name = cm_parser_parse_name(parser,
            "expected expression path segment");
        (void)cm_vec_push(&segments, &segment);
        if (!cm_parser_eat(parser, CM_TOKEN_PATH_SEP)) break;
        /*
         * In expression position, generic arguments require Rust's
         * turbofish separator.  Parsing a bare `name<T>` here would steal a
         * comparison expression, while `name::<T>` is unambiguous.  Attach
         * the arguments to the segment immediately preceding `::`, then
         * continue only when another path separator follows the closing `>`.
         */
        if ((cm_parser_kind(parser) == CM_TOKEN_LT
                || cm_parser_kind(parser) == CM_TOKEN_SHL)
            && segments.len != 0u) {
            CmAstPathSegment *last_segment;

            last_segment = (CmAstPathSegment *)cm_vec_at(&segments,
                segments.len - 1u);
            cm_parser_parse_generic_arguments(parser, last_segment);
            if (!cm_parser_eat(parser, CM_TOKEN_PATH_SEP)) break;
        }
    }
    if (segments.len == 0u) {
        cm_parser_error(parser, "expected expression path");
        cm_vec_destroy(&segments);
        return CM_AST_PATH_NONE;
    }
    path.segments = (CmAstPathSegment *)cm_parser_copy_array(parser,
        &segments);
    path.segment_count = cm_parser_count_u32(&segments);
    path.span = cm_parser_span_from(parser, first);
    cm_vec_destroy(&segments);
    return cm_ast_add_path(parser->ast, &path);
}

static int cm_parser_expression_terminator(const CmParser *parser)
{
    enum cm_token_kind kind;

    kind = cm_parser_kind(parser);
    return kind == CM_TOKEN_EOF || kind == CM_TOKEN_SEMICOLON ||
        kind == CM_TOKEN_COMMA || kind == CM_TOKEN_RPAREN ||
        kind == CM_TOKEN_RBRACKET || kind == CM_TOKEN_RBRACE ||
        kind == CM_TOKEN_FAT_ARROW;
}

static CmInternId cm_parser_take_operator(CmParser *parser)
{
    const struct cm_token *token;
    CmInternId name;

    token = cm_parser_token(parser);
    name = cm_parser_intern_token(parser, token);
    cm_parser_bump(parser);
    return name;
}

static CmAstExprId *cm_parser_parse_call_arguments(CmParser *parser,
    uint32_t *count)
{
    CmVec arguments;

    cm_vec_init(&arguments, sizeof(CmAstExprId));
    while (cm_parser_kind(parser) != CM_TOKEN_RPAREN &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        CmVec attributes;
        CmAstExprId argument;

        cm_vec_init(&attributes, sizeof(CmAstAttributeId));
        cm_parser_parse_attributes(parser, &attributes);
        argument = cm_parser_parse_expression(parser);
        cm_parser_attach_expression_attributes(parser, argument,
            &attributes);
        cm_vec_destroy(&attributes);
        (void)cm_vec_push(&arguments, &argument);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
    }
    (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
        "expected ')' after call arguments");
    *count = cm_parser_count_u32(&arguments);
    {
        CmAstExprId *copy;

        copy = (CmAstExprId *)cm_parser_copy_array(parser, &arguments);
        cm_vec_destroy(&arguments);
        return copy;
    }
}

static CmAstExprId cm_parser_parse_control_flow(CmParser *parser,
    CmAstExprKind kind, const struct cm_token *first)
{
    CmAstExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = kind;
    if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
        expression.data.flow.label = cm_parser_intern_token(parser,
            cm_parser_token(parser));
        cm_parser_bump(parser);
    }
    if (kind != CM_AST_EXPR_CONTINUE &&
        !cm_parser_expression_terminator(parser)) {
        expression.data.flow.value = cm_parser_parse_expression(parser);
    }
    expression.span = cm_parser_span_from(parser, first);
    return cm_ast_add_expr(parser->ast, &expression);
}

static CmAstExprId cm_parser_parse_if(CmParser *parser,
    const struct cm_token *first)
{
    CmAstExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_AST_EXPR_IF;
    if (cm_parser_eat_keyword(parser, CM_KW_LET)) {
        expression.data.if_expr.pattern = cm_parser_parse_pattern(parser);
        (void)cm_parser_expect(parser, CM_TOKEN_EQ,
            "expected '=' in if let expression");
    }
    expression.data.if_expr.condition =
        cm_parser_parse_expression_without_struct(parser);
    expression.data.if_expr.then_expr = cm_parser_parse_block(parser);
    if (cm_parser_eat_keyword(parser, CM_KW_ELSE)) {
        if (cm_parser_eat_keyword(parser, CM_KW_IF)) {
            expression.data.if_expr.else_expr = cm_parser_parse_if(parser,
                cm_parser_previous(parser));
        } else {
            expression.data.if_expr.else_expr = cm_parser_parse_block(parser);
        }
    }
    expression.span = cm_parser_span_from(parser, first);
    return cm_ast_add_expr(parser->ast, &expression);
}

static int cm_parser_expr_is_block_like(const CmParser *parser,
    CmAstExprId id)
{
    const CmAstExpr *expression;

    expression = cm_ast_get_expr(parser->ast, id);
    if (expression == NULL) return 0;
    return expression->kind == CM_AST_EXPR_BLOCK ||
        expression->kind == CM_AST_EXPR_IF ||
        expression->kind == CM_AST_EXPR_MATCH ||
        expression->kind == CM_AST_EXPR_LOOP ||
        expression->kind == CM_AST_EXPR_WHILE ||
        expression->kind == CM_AST_EXPR_FOR ||
        expression->kind == CM_AST_EXPR_TRY_BLOCK;
}

static CmAstExprId cm_parser_parse_match(CmParser *parser,
    const struct cm_token *first)
{
    CmAstExpr expression;
    CmVec arms;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_AST_EXPR_MATCH;
    expression.data.match_expr.scrutinee =
        cm_parser_parse_expression_without_struct(parser);
    (void)cm_parser_expect(parser, CM_TOKEN_LBRACE,
        "expected '{' after match expression");
    cm_vec_init(&arms, sizeof(CmAstMatchArm));
    while (cm_parser_kind(parser) != CM_TOKEN_RBRACE &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        CmAstMatchArm arm;
        CmVec attributes;

        memset(&arm, 0, sizeof(arm));
        cm_vec_init(&attributes, sizeof(CmAstAttributeId));
        cm_parser_parse_attributes(parser, &attributes);
        arm.attributes = (CmAstAttributeId *)cm_parser_copy_array(parser,
            &attributes);
        arm.attribute_count = cm_parser_count_u32(&attributes);
        cm_vec_destroy(&attributes);
        arm.pattern = cm_parser_parse_pattern(parser);
        if (cm_parser_eat_keyword(parser, CM_KW_IF)) {
            const struct cm_token *guard_first;

            guard_first = cm_parser_previous(parser);
            if (cm_parser_eat_keyword(parser, CM_KW_LET)) {
                arm.guard_pattern = cm_parser_parse_pattern(parser);
                (void)cm_parser_expect(parser, CM_TOKEN_EQ,
                    "expected '=' in match let guard");
                arm.guard_initializer = cm_parser_parse_expression(parser);
            } else {
                arm.guard = cm_parser_parse_expression(parser);
            }
            arm.guard_span = cm_parser_span_from(parser, guard_first);
        }
        (void)cm_parser_expect(parser, CM_TOKEN_FAT_ARROW,
            "expected '=>' after match pattern");
        arm.body = cm_parser_parse_expression_bp_mode(parser, 1u, 1, 1);
        (void)cm_vec_push(&arms, &arm);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA) &&
            cm_parser_kind(parser) != CM_TOKEN_RBRACE &&
            !cm_parser_expr_is_block_like(parser, arm.body)) {
            cm_parser_error(parser, "expected ',' after match arm");
            break;
        }
    }
    (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
        "expected '}' after match arms");
    expression.data.match_expr.arms =
        (CmAstMatchArm *)cm_parser_copy_array(parser, &arms);
    expression.data.match_expr.arm_count = cm_parser_count_u32(&arms);
    cm_vec_destroy(&arms);
    expression.span = cm_parser_span_from(parser, first);
    return cm_ast_add_expr(parser->ast, &expression);
}

static CmAstExprId cm_parser_parse_closure(CmParser *parser,
    const struct cm_token *first, int is_move)
{
    CmAstExpr expression;
    CmVec parameters;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_AST_EXPR_CLOSURE;
    expression.data.closure.is_move = is_move;
    cm_vec_init(&parameters, sizeof(CmAstClosureParam));
    if (!cm_parser_eat(parser, CM_TOKEN_PIPE_PIPE)) {
        (void)cm_parser_expect(parser, CM_TOKEN_PIPE,
            "expected '|' before closure parameters");
        while (cm_parser_kind(parser) != CM_TOKEN_PIPE &&
               cm_parser_kind(parser) != CM_TOKEN_EOF) {
            CmAstClosureParam parameter;

            memset(&parameter, 0, sizeof(parameter));
            parameter.pattern = cm_parser_parse_pattern_atom(parser);
            if (cm_parser_eat(parser, CM_TOKEN_COLON))
                parameter.type = cm_parser_parse_type(parser);
            (void)cm_vec_push(&parameters, &parameter);
            if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
        }
        (void)cm_parser_expect(parser, CM_TOKEN_PIPE,
            "expected '|' after closure parameters");
    }
    if (cm_parser_eat(parser, CM_TOKEN_THIN_ARROW))
        expression.data.closure.return_type = cm_parser_parse_type(parser);
    expression.data.closure.body = cm_parser_parse_expression(parser);
    expression.data.closure.parameters =
        (CmAstClosureParam *)cm_parser_copy_array(parser, &parameters);
    expression.data.closure.parameter_count = cm_parser_count_u32(&parameters);
    cm_vec_destroy(&parameters);
    expression.span = cm_parser_span_from(parser, first);
    return cm_ast_add_expr(parser->ast, &expression);
}

static CmAstExprId cm_parser_add_shorthand_expression(CmParser *parser,
    const struct cm_token *name_token, CmInternId name)
{
    CmAstPathSegment segment;
    CmAstPath path;
    CmAstExpr expression;

    memset(&segment, 0, sizeof(segment));
    memset(&path, 0, sizeof(path));
    memset(&expression, 0, sizeof(expression));
    segment.name = name;
    path.segments = (CmAstPathSegment *)cm_arena_alloc(
        &parser->ast->storage, sizeof(segment), sizeof(void *));
    path.segments[0] = segment;
    path.segment_count = 1u;
    path.span.start = cm_parser_offset_u32(parser,
        name_token == NULL ? 0u : name_token->start);
    path.span.end = cm_parser_offset_u32(parser,
        cm_token_end(name_token));
    expression.kind = CM_AST_EXPR_PATH;
    expression.data.path.path = cm_ast_add_path(parser->ast, &path);
    expression.span = path.span;
    return cm_ast_add_expr(parser->ast, &expression);
}

static CmAstExprId cm_parser_parse_struct_expression(CmParser *parser,
    const struct cm_token *first, CmAstPathId path)
{
    CmAstExpr expression;
    CmVec fields;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_AST_EXPR_STRUCT;
    expression.data.struct_expr.path = path;
    cm_vec_init(&fields, sizeof(CmAstExprField));
    (void)cm_parser_expect(parser, CM_TOKEN_LBRACE,
        "expected '{' after struct path");
    while (cm_parser_kind(parser) != CM_TOKEN_RBRACE &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        CmVec attributes;

        cm_vec_init(&attributes, sizeof(CmAstAttributeId));
        cm_parser_parse_attributes(parser, &attributes);
        if (cm_parser_eat(parser, CM_TOKEN_DOT_DOT)) {
            if (attributes.len != 0u) {
                cm_parser_error(parser,
                    "attributes on a struct update base are unsupported");
            }
            expression.data.struct_expr.base =
                cm_parser_parse_expression(parser);
            if (cm_parser_eat(parser, CM_TOKEN_COMMA) &&
                cm_parser_kind(parser) != CM_TOKEN_RBRACE) {
                cm_parser_error(parser,
                    "struct update base must be the last element");
            } else if (cm_parser_kind(parser) != CM_TOKEN_RBRACE) {
                cm_parser_error(parser,
                    "struct update base must be the last element");
            }
            cm_vec_destroy(&attributes);
            break;
        } else {
            const struct cm_token *name_token;
            const CmAstExpr *value;
            CmAstExprField field;

            memset(&field, 0, sizeof(field));
            field.attributes = (CmAstAttributeId *)cm_parser_copy_array(
                parser, &attributes);
            field.attribute_count = cm_parser_count_u32(&attributes);
            name_token = cm_parser_token(parser);
            if (!cm_parser_ordinary_identifier(name_token)) {
                cm_parser_error(parser,
                    "expected struct expression field name");
            }
            field.name = cm_parser_parse_name(parser,
                "expected struct expression field name");
            field.span.start = cm_parser_offset_u32(parser,
                name_token == NULL ? 0u : name_token->start);
            if (cm_parser_eat(parser, CM_TOKEN_COLON)) {
                field.value = cm_parser_parse_expression(parser);
                value = cm_ast_get_expr(parser->ast, field.value);
                if (value != NULL) field.span.end = value->span.end;
            } else {
                field.is_shorthand = 1;
                field.value = cm_parser_add_shorthand_expression(parser,
                    name_token, field.name);
                field.span.end = cm_parser_offset_u32(parser,
                    cm_token_end(name_token));
            }
            (void)cm_vec_push(&fields, &field);
        }
        cm_vec_destroy(&attributes);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
    }
    (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
        "expected '}' after struct expression");
    expression.data.struct_expr.fields =
        (CmAstExprField *)cm_parser_copy_array(parser, &fields);
    expression.data.struct_expr.field_count = cm_parser_count_u32(&fields);
    cm_vec_destroy(&fields);
    expression.span = cm_parser_span_from(parser, first);
    return cm_ast_add_expr(parser->ast, &expression);
}

static CmAstExprId cm_parser_parse_qualified_expression_path(
    CmParser *parser, const struct cm_token *first)
{
    const struct cm_token *previous;
    CmAstExpr expression;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_AST_EXPR_QUALIFIED_PATH;
    (void)cm_parser_expect_type_left_angle(parser,
        "expected '<' before qualified expression path");
    if (cm_parser_keyword(parser, CM_KW_AS)
        || cm_parser_kind(parser) == CM_TOKEN_GT
        || cm_parser_kind(parser) == CM_TOKEN_EOF) {
        cm_parser_error(parser,
            "expected self type in qualified expression path");
    } else {
        expression.data.qualified_path.self_type =
            cm_parser_parse_type(parser);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_AS)) {
        if (cm_parser_kind(parser) == CM_TOKEN_GT
            || cm_parser_kind(parser) == CM_TOKEN_EOF) {
            cm_parser_error(parser,
                "expected trait path in qualified expression path");
        } else {
            expression.data.qualified_path.trait_path =
                cm_parser_parse_path(parser, 1);
            previous = cm_parser_previous(parser);
            if (previous != NULL && previous->kind == CM_TOKEN_PATH_SEP) {
                cm_parser_error(parser,
                    "expected trait path segment after '::'");
            }
        }
    }
    (void)cm_parser_expect(parser, CM_TOKEN_GT,
        "expected '>' after qualified expression trait path");
    expression.data.qualified_path.qualifier_span =
        cm_parser_span_from(parser, first);
    (void)cm_parser_expect(parser, CM_TOKEN_PATH_SEP,
        "expected '::' after qualified expression path");
    expression.data.qualified_path.associated_path =
        cm_parser_parse_expression_path(parser, NULL);
    expression.span = cm_parser_span_from(parser, first);
    return cm_ast_add_expr(parser->ast, &expression);
}

static CmAstExprId cm_parser_parse_prefix(CmParser *parser,
    int allow_struct_literal)
{
    const struct cm_token *first;
    CmAstExpr expression;

    first = cm_parser_token(parser);
    memset(&expression, 0, sizeof(expression));
    if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
        expression.kind = CM_AST_EXPR_LOOP;
        expression.data.loop_expr.label = cm_parser_intern_token(parser,
            cm_parser_token(parser));
        cm_parser_bump(parser);
        (void)cm_parser_expect(parser, CM_TOKEN_COLON,
            "expected ':' after loop label");
        (void)cm_parser_expect_keyword(parser, CM_KW_LOOP,
            "expected 'loop' after loop label");
        expression.data.loop_expr.body = cm_parser_parse_block(parser);
        expression.span = cm_parser_span_from(parser, first);
        return cm_ast_add_expr(parser->ast, &expression);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_RETURN))
        return cm_parser_parse_control_flow(parser, CM_AST_EXPR_RETURN,
            first);
    if (cm_parser_eat_keyword(parser, CM_KW_BREAK))
        return cm_parser_parse_control_flow(parser, CM_AST_EXPR_BREAK,
            first);
    if (cm_parser_eat_keyword(parser, CM_KW_CONTINUE))
        return cm_parser_parse_control_flow(parser, CM_AST_EXPR_CONTINUE,
            first);
    if (cm_parser_eat_keyword(parser, CM_KW_LET)) {
        expression.kind = CM_AST_EXPR_LET;
        expression.data.let_expr.pattern = cm_parser_parse_pattern(parser);
        (void)cm_parser_expect(parser, CM_TOKEN_EQ,
            "expected '=' in let condition");
        expression.data.let_expr.initializer =
            cm_parser_parse_expression_bp_mode(parser, 5u,
                allow_struct_literal, 0);
        expression.span = cm_parser_span_from(parser, first);
        return cm_ast_add_expr(parser->ast, &expression);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_IF))
        return cm_parser_parse_if(parser, first);
    if (cm_parser_eat_keyword(parser, CM_KW_MATCH))
        return cm_parser_parse_match(parser, first);
    if (cm_parser_eat_keyword(parser, CM_KW_LOOP)) {
        expression.kind = CM_AST_EXPR_LOOP;
        expression.data.loop_expr.body = cm_parser_parse_block(parser);
    } else if (cm_parser_eat_keyword(parser, CM_KW_WHILE)) {
        expression.kind = CM_AST_EXPR_WHILE;
        if (cm_parser_eat_keyword(parser, CM_KW_LET)) {
            expression.data.while_expr.pattern = cm_parser_parse_pattern(
                parser);
            (void)cm_parser_expect(parser, CM_TOKEN_EQ,
                "expected '=' in while let expression");
        }
        expression.data.while_expr.condition =
            cm_parser_parse_expression_without_struct(parser);
        expression.data.while_expr.body = cm_parser_parse_block(parser);
    } else if (cm_parser_eat_keyword(parser, CM_KW_FOR)) {
        expression.kind = CM_AST_EXPR_FOR;
        expression.data.for_expr.pattern = cm_parser_parse_pattern(parser);
        (void)cm_parser_expect_keyword(parser, CM_KW_IN,
            "expected 'in' in for expression");
        expression.data.for_expr.iterable =
            cm_parser_parse_expression_without_struct(parser);
        expression.data.for_expr.body = cm_parser_parse_block(parser);
    } else if (cm_parser_eat_keyword(parser, CM_KW_TRY)) {
        expression.kind = CM_AST_EXPR_TRY_BLOCK;
        expression.data.try_expr.operand = cm_parser_parse_block(parser);
    } else if (cm_parser_eat_keyword(parser, CM_KW_UNSAFE)) {
        return cm_parser_parse_block_mode(parser, first, 1, 0);
    } else if (cm_parser_const_starts_block(parser)) {
        cm_parser_bump(parser);
        return cm_parser_parse_block_mode(parser, first, 0, 1);
    } else if (cm_parser_keyword(parser, CM_KW_MOVE)) {
        cm_parser_bump(parser);
        return cm_parser_parse_closure(parser, first, 1);
    } else if (cm_parser_kind(parser) == CM_TOKEN_PIPE
        || cm_parser_kind(parser) == CM_TOKEN_PIPE_PIPE) {
        return cm_parser_parse_closure(parser, first, 0);
    } else if (cm_parser_kind(parser) == CM_TOKEN_AMP
               && cm_parser_next_token(parser) != NULL
               && cm_parser_next_token(parser)->keyword == CM_KW_RAW) {
        expression.kind = CM_AST_EXPR_RAW_REFERENCE;
        cm_parser_bump(parser);
        cm_parser_bump(parser);
        if (cm_parser_eat_keyword(parser, CM_KW_CONST)) {
            expression.data.raw_reference.kind =
                CM_AST_RAW_REFERENCE_CONST;
        } else if (cm_parser_eat_keyword(parser, CM_KW_MUT)) {
            expression.data.raw_reference.kind = CM_AST_RAW_REFERENCE_MUT;
        } else {
            cm_parser_error(parser,
                "expected 'const' or 'mut' after '&raw'");
        }
        expression.data.raw_reference.operand =
            cm_parser_parse_expression_bp_mode(parser, 14u,
                allow_struct_literal, 0);
    } else if (cm_parser_kind(parser) == CM_TOKEN_BANG ||
               cm_parser_kind(parser) == CM_TOKEN_MINUS ||
               cm_parser_kind(parser) == CM_TOKEN_STAR ||
               cm_parser_kind(parser) == CM_TOKEN_AMP ||
               cm_parser_kind(parser) == CM_TOKEN_AMP_AMP) {
        expression.kind = CM_AST_EXPR_UNARY;
        if (first->kind == CM_TOKEN_AMP
            || first->kind == CM_TOKEN_AMP_AMP) {
            const struct cm_token *mut_token;

            (void)cm_parser_eat_type_amp(parser);
            expression.data.unary.operator_name = cm_parser_intern_token(
                parser, cm_parser_previous(parser));
            if (cm_parser_keyword(parser, CM_KW_MUT)) {
                mut_token = cm_parser_token(parser);
                cm_parser_bump(parser);
                expression.data.unary.operator_name = cm_parser_intern_range(
                    parser, first->start, cm_token_end(mut_token));
            }
        } else {
            expression.data.unary.operator_name =
                cm_parser_take_operator(parser);
        }
        expression.data.unary.operand = cm_parser_parse_expression_bp_mode(
            parser, 14u, allow_struct_literal, 0);
    } else if (cm_parser_eat(parser, CM_TOKEN_DOT_DOT) ||
               cm_parser_eat(parser, CM_TOKEN_DOT_DOT_EQ)) {
        enum cm_token_kind previous_kind;

        previous_kind = cm_parser_previous(parser)->kind;
        expression.kind = CM_AST_EXPR_RANGE;
        expression.data.range.is_inclusive =
            previous_kind == CM_TOKEN_DOT_DOT_EQ;
        if (!cm_parser_expression_terminator(parser))
            expression.data.range.end = cm_parser_parse_expression_bp_mode(
                parser, 3u, allow_struct_literal, 0);
    } else if (cm_parser_type_left_angle(parser)) {
        return cm_parser_parse_qualified_expression_path(parser, first);
    } else if (cm_parser_is_literal(parser)) {
        expression.kind = CM_AST_EXPR_LITERAL;
        expression.data.literal.text = cm_parser_intern_token(parser,
            cm_parser_token(parser));
        cm_parser_bump(parser);
    } else if (cm_parser_eat(parser, CM_TOKEN_LPAREN)) {
        CmVec elements;
        CmAstExprId first_element;

        if (cm_parser_eat(parser, CM_TOKEN_RPAREN)) {
            expression.kind = CM_AST_EXPR_TUPLE;
        } else {
            cm_vec_init(&elements, sizeof(CmAstExprId));
            first_element = cm_parser_parse_expression(parser);
            (void)cm_vec_push(&elements, &first_element);
            if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) {
                (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
                    "expected ')' after grouped expression");
                cm_vec_destroy(&elements);
                return first_element;
            }
            while (cm_parser_kind(parser) != CM_TOKEN_RPAREN &&
                   cm_parser_kind(parser) != CM_TOKEN_EOF) {
                CmAstExprId element;

                element = cm_parser_parse_expression(parser);
                (void)cm_vec_push(&elements, &element);
                if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
            }
            (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
                "expected ')' after tuple expression");
            expression.kind = CM_AST_EXPR_TUPLE;
            expression.data.list.elements =
                (CmAstExprId *)cm_parser_copy_array(parser, &elements);
            expression.data.list.element_count = cm_parser_count_u32(&elements);
            cm_vec_destroy(&elements);
        }
    } else if (cm_parser_eat(parser, CM_TOKEN_LBRACKET)) {
        CmVec elements;

        expression.kind = CM_AST_EXPR_ARRAY;
        cm_vec_init(&elements, sizeof(CmAstExprId));
        if (cm_parser_kind(parser) != CM_TOKEN_RBRACKET &&
            cm_parser_kind(parser) != CM_TOKEN_EOF) {
            CmAstExprId first_element;

            first_element = cm_parser_parse_expression(parser);
            if (cm_parser_eat(parser, CM_TOKEN_SEMICOLON)) {
                expression.data.list.repeat_value = first_element;
                expression.data.list.repeat_length =
                    cm_parser_parse_expression(parser);
            } else {
                (void)cm_vec_push(&elements, &first_element);
                while (cm_parser_eat(parser, CM_TOKEN_COMMA) &&
                       cm_parser_kind(parser) != CM_TOKEN_RBRACKET &&
                       cm_parser_kind(parser) != CM_TOKEN_EOF) {
                    CmAstExprId element;

                    element = cm_parser_parse_expression(parser);
                    (void)cm_vec_push(&elements, &element);
                }
            }
        }
        (void)cm_parser_expect(parser, CM_TOKEN_RBRACKET,
            "expected ']' after array expression");
        expression.data.list.elements =
            (CmAstExprId *)cm_parser_copy_array(parser, &elements);
        expression.data.list.element_count = cm_parser_count_u32(&elements);
        cm_vec_destroy(&elements);
    } else if (cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
        return cm_parser_parse_block(parser);
    } else if (cm_parser_is_name(parser) ||
               cm_parser_kind(parser) == CM_TOKEN_PATH_SEP) {
        CmAstPathId path;
        int struct_path_valid;

        path = cm_parser_parse_expression_path(parser, &struct_path_valid);
        if (cm_parser_eat(parser, CM_TOKEN_BANG)) {
            expression.kind = CM_AST_EXPR_MACRO;
            expression.data.macro_expr.path = path;
            (void)cm_parser_parse_macro_arguments(parser,
                &expression.data.macro_expr, 0);
        } else if (allow_struct_literal && struct_path_valid &&
                   cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
            return cm_parser_parse_struct_expression(parser, first, path);
        } else {
            expression.kind = CM_AST_EXPR_PATH;
            expression.data.path.path = path;
        }
    } else {
        cm_parser_error(parser, "expected expression");
        if (cm_parser_kind(parser) != CM_TOKEN_EOF) cm_parser_bump(parser);
        expression.kind = CM_AST_EXPR_LITERAL;
        expression.data.literal.text = cm_interner_intern_c_str(
            &parser->ast->strings, "<error>");
    }
    expression.span = cm_parser_span_from(parser, first);
    return cm_ast_add_expr(parser->ast, &expression);
}

static unsigned int cm_parser_binary_precedence(enum cm_token_kind kind,
    int *right_associative, CmAstExprKind *expression_kind)
{
    *right_associative = 0;
    *expression_kind = CM_AST_EXPR_BINARY;
    switch (kind) {
    case CM_TOKEN_EQ: case CM_TOKEN_PLUS_EQ: case CM_TOKEN_MINUS_EQ:
    case CM_TOKEN_STAR_EQ: case CM_TOKEN_SLASH_EQ: case CM_TOKEN_PERCENT_EQ:
    case CM_TOKEN_CARET_EQ: case CM_TOKEN_AMP_EQ: case CM_TOKEN_PIPE_EQ:
    case CM_TOKEN_SHL_EQ: case CM_TOKEN_SHR_EQ:
        *right_associative = 1;
        *expression_kind = CM_AST_EXPR_ASSIGN;
        return 1u;
    case CM_TOKEN_DOT_DOT: case CM_TOKEN_DOT_DOT_EQ: return 2u;
    case CM_TOKEN_PIPE_PIPE: return 3u;
    case CM_TOKEN_AMP_AMP: return 4u;
    /* Rust binds bitwise operators tighter than comparisons, and all
     * comparisons share one non-associative level. */
    case CM_TOKEN_EQ_EQ: case CM_TOKEN_NOT_EQ:
    case CM_TOKEN_LT: case CM_TOKEN_LT_EQ: case CM_TOKEN_GT:
    case CM_TOKEN_GT_EQ: return 5u;
    case CM_TOKEN_PIPE: return 6u;
    case CM_TOKEN_CARET: return 7u;
    case CM_TOKEN_AMP: return 8u;
    case CM_TOKEN_SHL: case CM_TOKEN_SHR: return 10u;
    case CM_TOKEN_PLUS: case CM_TOKEN_MINUS: return 11u;
    case CM_TOKEN_STAR: case CM_TOKEN_SLASH: case CM_TOKEN_PERCENT: return 12u;
    default: return 0u;
    }
}

static CmAstExprId cm_parser_parse_expression_bp(CmParser *parser,
    unsigned int minimum_precedence)
{
    return cm_parser_parse_expression_bp_mode(parser, minimum_precedence, 1,
        0);
}

static CmAstExprId cm_parser_parse_expression_bp_mode(CmParser *parser,
    unsigned int minimum_precedence, int allow_struct_literal,
    int stop_after_block_like)
{
    const struct cm_token *first;
    CmAstExprId left;

    first = cm_parser_token(parser);
    left = cm_parser_parse_prefix(parser, allow_struct_literal);
    for (;;) {
        if (stop_after_block_like
            && cm_parser_expr_is_block_like(parser, left)) {
            /* Statement position: a block-like expression ends the
             * statement unless continued by `.` or `?`, exactly as in
             * rustc — but only when the statement *begins* with the
             * block construct: `(unsafe { .. }) != 0` is an ordinary
             * expression statement (parentheses are transparent, so
             * compare the expression's start to the first token). */
            const CmAstExpr *left_expr = cm_ast_get_expr(parser->ast, left);
            if (left_expr != NULL && left_expr->span.start
                    == cm_parser_offset_u32(parser, first->start)) {
                enum cm_token_kind next = cm_parser_kind(parser);
                if (next != CM_TOKEN_DOT && next != CM_TOKEN_QUESTION)
                    break;
            }
        }
        if (cm_parser_kind(parser) == CM_TOKEN_LPAREN) {
            CmAstExpr expression;

            memset(&expression, 0, sizeof(expression));
            expression.kind = CM_AST_EXPR_CALL;
            expression.data.call.callee = left;
            cm_parser_bump(parser);
            expression.data.call.arguments = cm_parser_parse_call_arguments(
                parser, &expression.data.call.argument_count);
            expression.span = cm_parser_span_from(parser, first);
            left = cm_ast_add_expr(parser->ast, &expression);
            continue;
        }
        if (cm_parser_eat(parser, CM_TOKEN_LBRACKET)) {
            CmAstExpr expression;

            memset(&expression, 0, sizeof(expression));
            expression.kind = CM_AST_EXPR_INDEX;
            expression.data.index.base = left;
            expression.data.index.index = cm_parser_parse_expression(parser);
            (void)cm_parser_expect(parser, CM_TOKEN_RBRACKET,
                "expected ']' after index expression");
            expression.span = cm_parser_span_from(parser, first);
            left = cm_ast_add_expr(parser->ast, &expression);
            continue;
        }
        if (cm_parser_eat(parser, CM_TOKEN_DOT)) {
            const struct cm_token *name_token;
            const struct cm_token *generic_first;
            CmInternId name;
            CmAstPathSegment generic_segment;
            CmAstSpan generic_span;
            CmAstExpr expression;

            name_token = cm_parser_token(parser);
            if (cm_parser_kind(parser) == CM_TOKEN_INTEGER) {
                uint32_t tuple_index;

                tuple_index = 0u;
                (void)cm_parser_parse_tuple_index(parser, name_token,
                    &tuple_index);
                cm_parser_bump(parser);
                memset(&expression, 0, sizeof(expression));
                expression.kind = CM_AST_EXPR_TUPLE_FIELD;
                expression.data.tuple_field.base = left;
                expression.data.tuple_field.index = tuple_index;
                expression.data.tuple_field.index_span.start =
                    cm_parser_offset_u32(parser, name_token->start);
                expression.data.tuple_field.index_span.end =
                    cm_parser_offset_u32(parser, cm_token_end(name_token));
                expression.span = cm_parser_span_from(parser, first);
                left = cm_ast_add_expr(parser->ast, &expression);
                continue;
            }
            if (cm_parser_is_name(parser)) {
                name = cm_parser_intern_token(parser, name_token);
                cm_parser_bump(parser);
            } else {
                cm_parser_error(parser, "expected field or method name");
                name = CM_INTERN_ID_NONE;
            }
            generic_first = NULL;
            memset(&generic_segment, 0, sizeof(generic_segment));
            memset(&generic_span, 0, sizeof(generic_span));
            if (cm_parser_eat(parser, CM_TOKEN_PATH_SEP)) {
                generic_first = cm_parser_previous(parser);
                if (cm_parser_type_left_angle(parser)) {
                    cm_parser_parse_generic_arguments(parser,
                        &generic_segment);
                } else {
                    cm_parser_error(parser,
                        "expected method generic arguments after '::'");
                }
                generic_span = cm_parser_span_from(parser, generic_first);
            }
            memset(&expression, 0, sizeof(expression));
            if (cm_parser_eat(parser, CM_TOKEN_LPAREN)) {
                expression.kind = CM_AST_EXPR_METHOD_CALL;
                expression.data.method_call.receiver = left;
                expression.data.method_call.name = name;
                expression.data.method_call.generic_arguments =
                    generic_segment.arguments;
                expression.data.method_call.generic_argument_count =
                    generic_segment.argument_count;
                expression.data.method_call.generic_argument_span =
                    generic_span;
                expression.data.method_call.arguments =
                    cm_parser_parse_call_arguments(parser,
                        &expression.data.method_call.argument_count);
            } else {
                expression.kind = CM_AST_EXPR_FIELD;
                expression.data.field.base = left;
                expression.data.field.name = name;
                expression.data.field.name_span.start =
                    cm_parser_offset_u32(parser,
                        name_token == NULL ? 0u : name_token->start);
                expression.data.field.name_span.end =
                    cm_parser_offset_u32(parser, cm_token_end(name_token));
            }
            expression.span = cm_parser_span_from(parser, first);
            left = cm_ast_add_expr(parser->ast, &expression);
            continue;
        }
        if (cm_parser_keyword(parser, CM_KW_AS)) {
            CmAstExpr expression;

            if (13u < minimum_precedence) break;
            memset(&expression, 0, sizeof(expression));
            cm_parser_bump(parser);
            expression.kind = CM_AST_EXPR_CAST;
            expression.data.cast.value = left;
            expression.data.cast.type = cm_parser_parse_type(parser);
            expression.span = cm_parser_span_from(parser, first);
            left = cm_ast_add_expr(parser->ast, &expression);
            continue;
        }
        if (cm_parser_eat(parser, CM_TOKEN_QUESTION)) {
            CmAstExpr expression;

            memset(&expression, 0, sizeof(expression));
            expression.kind = CM_AST_EXPR_TRY;
            expression.data.try_expr.operand = left;
            expression.span = cm_parser_span_from(parser, first);
            left = cm_ast_add_expr(parser->ast, &expression);
            continue;
        }
        {
            int right_associative;
            CmAstExprKind expression_kind;
            unsigned int precedence;
            enum cm_token_kind operator_kind;
            CmInternId operator_name;
            CmAstExpr expression;

            operator_kind = cm_parser_kind(parser);
            precedence = cm_parser_binary_precedence(operator_kind,
                &right_associative, &expression_kind);
            if (precedence == 0u || precedence < minimum_precedence) break;
            operator_name = cm_parser_take_operator(parser);
            memset(&expression, 0, sizeof(expression));
            if (operator_kind == CM_TOKEN_DOT_DOT ||
                operator_kind == CM_TOKEN_DOT_DOT_EQ) {
                expression.kind = CM_AST_EXPR_RANGE;
                expression.data.range.start = left;
                expression.data.range.is_inclusive =
                    operator_kind == CM_TOKEN_DOT_DOT_EQ;
                if (!cm_parser_expression_terminator(parser))
                    expression.data.range.end =
                        cm_parser_parse_expression_bp_mode(parser,
                            precedence + 1u, allow_struct_literal,
                            stop_after_block_like);
            } else {
                expression.kind = expression_kind;
                expression.data.binary.operator_name = operator_name;
                expression.data.binary.left = left;
                expression.data.binary.right =
                    cm_parser_parse_expression_bp_mode(parser,
                        right_associative ? precedence : precedence + 1u,
                        allow_struct_literal, stop_after_block_like);
            }
            expression.span = cm_parser_span_from(parser, first);
            left = cm_ast_add_expr(parser->ast, &expression);
        }
    }
    return left;
}

static CmAstExprId cm_parser_parse_expression(CmParser *parser)
{
    return cm_parser_parse_expression_bp(parser, 1u);
}

static CmAstExprId cm_parser_parse_expression_without_struct(
    CmParser *parser)
{
    return cm_parser_parse_expression_bp_mode(parser, 1u, 0, 0);
}

static void cm_parser_attach_expression_attributes(CmParser *parser,
    CmAstExprId expression_id, const CmVec *attributes)
{
    CmAstExpr *expression;

    if (attributes->len == 0u) return;
    expression = expression_id == CM_AST_EXPR_NONE ? NULL
        : (CmAstExpr *)cm_vec_at(&parser->ast->expressions,
            (size_t)expression_id - 1u);
    if (expression == NULL) {
        cm_parser_error(parser,
            "cannot attach attributes to missing expression");
        return;
    }
    expression->attributes = (CmAstAttributeId *)cm_parser_copy_array(
        parser, attributes);
    expression->attribute_count = cm_parser_count_u32(attributes);
}

static CmAstExprId cm_parser_parse_block_mode(CmParser *parser,
    const struct cm_token *first, int is_unsafe, int is_const)
{
    CmAstExpr expression;
    CmVec inner_attributes;
    CmVec statements;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_AST_EXPR_BLOCK;
    expression.data.block.is_unsafe = is_unsafe;
    expression.data.block.is_const = is_const;
    cm_vec_init(&inner_attributes, sizeof(CmAstAttributeId));
    cm_vec_init(&statements, sizeof(CmAstStmtId));
    if (!cm_parser_expect(parser, CM_TOKEN_LBRACE,
        "expected block expression")) {
        cm_vec_destroy(&inner_attributes);
        cm_vec_destroy(&statements);
        return CM_AST_EXPR_NONE;
    }
    cm_parser_parse_inner_attributes(parser, &inner_attributes);
    while (cm_parser_kind(parser) != CM_TOKEN_RBRACE &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        const struct cm_token *statement_first;
        CmAstStmt statement;
        CmAstStmtId statement_id;

        statement_first = cm_parser_token(parser);
        memset(&statement, 0, sizeof(statement));
        if (cm_parser_starts_local_item(parser)) {
            const CmAstItem *item;

            statement.kind = CM_AST_STMT_ITEM;
            statement.data.item_stmt.item = cm_parser_parse_item(parser);
            item = cm_ast_get_item(parser->ast,
                statement.data.item_stmt.item);
            if (item == NULL
                || (item->kind != CM_AST_ITEM_CONST
                    && item->kind != CM_AST_ITEM_STATIC
                    && item->kind != CM_AST_ITEM_FUNCTION
                    && item->kind != CM_AST_ITEM_IMPL
                    && item->kind != CM_AST_ITEM_EXTERN_BLOCK
                    && item->kind != CM_AST_ITEM_MACRO
                    && item->kind != CM_AST_ITEM_STRUCT
                    && item->kind != CM_AST_ITEM_TRAIT
                    && item->kind != CM_AST_ITEM_USE
                    && item->kind != CM_AST_ITEM_UNION)) {
                cm_parser_error(parser,
                    "expected supported item in block statement");
            }
        } else {
            CmVec attributes;

            cm_vec_init(&attributes, sizeof(CmAstAttributeId));
            cm_parser_parse_attributes(parser, &attributes);
            if (cm_parser_eat_keyword(parser, CM_KW_LET)) {
                statement.kind = CM_AST_STMT_LET;
                statement.attributes =
                    (CmAstAttributeId *)cm_parser_copy_array(parser,
                        &attributes);
                statement.attribute_count =
                    cm_parser_count_u32(&attributes);
                statement.data.let_stmt.pattern =
                    cm_parser_parse_pattern(parser);
                if (cm_parser_eat(parser, CM_TOKEN_COLON)) {
                    statement.data.let_stmt.type =
                        cm_parser_parse_type(parser);
                }
                if (cm_parser_eat(parser, CM_TOKEN_EQ)) {
                    CmVec initializer_attributes;

                    cm_vec_init(&initializer_attributes,
                        sizeof(CmAstAttributeId));
                    cm_parser_parse_attributes(parser,
                        &initializer_attributes);
                    statement.data.let_stmt.initializer =
                        cm_parser_parse_expression(parser);
                    cm_parser_attach_expression_attributes(parser,
                        statement.data.let_stmt.initializer,
                        &initializer_attributes);
                    cm_vec_destroy(&initializer_attributes);
                }
                if (cm_parser_eat_keyword(parser, CM_KW_ELSE)) {
                    if (statement.data.let_stmt.initializer
                        == CM_AST_EXPR_NONE) {
                        cm_parser_error(parser,
                            "let-else statement requires an initializer");
                    }
                    statement.data.let_stmt.else_block =
                        cm_parser_parse_block(parser);
                }
                (void)cm_parser_expect(parser, CM_TOKEN_SEMICOLON,
                    "expected ';' after let statement");
            } else {
                CmAstExprId value;

                /* Statement position: a block-like expression ends the
                 * statement; a following `(`/`[`/operator starts a new
                 * statement rather than a call, index, or binary op. */
                value = cm_parser_parse_expression_bp_mode(parser, 1u, 1,
                    1);
                cm_parser_attach_expression_attributes(parser, value,
                    &attributes);
                if (cm_parser_eat(parser, CM_TOKEN_SEMICOLON)) {
                    statement.kind = CM_AST_STMT_EXPR;
                    statement.data.expr_stmt.expression = value;
                    statement.data.expr_stmt.has_semicolon = 1;
                } else if (cm_parser_kind(parser) == CM_TOKEN_RBRACE) {
                    expression.data.block.tail = value;
                    cm_vec_destroy(&attributes);
                    break;
                } else {
                    statement.kind = CM_AST_STMT_EXPR;
                    statement.data.expr_stmt.expression = value;
                }
            }
            cm_vec_destroy(&attributes);
        }
        statement.span = cm_parser_span_from(parser, statement_first);
        statement_id = cm_ast_add_stmt(parser->ast, &statement);
        (void)cm_vec_push(&statements, &statement_id);
    }
    (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
        "expected '}' after block expression");
    expression.data.block.inner_attributes =
        (CmAstAttributeId *)cm_parser_copy_array(parser, &inner_attributes);
    expression.data.block.inner_attribute_count =
        cm_parser_count_u32(&inner_attributes);
    expression.data.block.statements =
        (CmAstStmtId *)cm_parser_copy_array(parser, &statements);
    expression.data.block.statement_count = cm_parser_count_u32(&statements);
    cm_vec_destroy(&inner_attributes);
    cm_vec_destroy(&statements);
    expression.span = cm_parser_span_from(parser, first);
    return cm_ast_add_expr(parser->ast, &expression);
}

static CmAstExprId cm_parser_parse_block(CmParser *parser)
{
    return cm_parser_parse_block_mode(parser, cm_parser_token(parser), 0, 0);
}

static int cm_parser_starts_inner_attribute(const CmParser *parser)
{
    const struct cm_token *next;

    if (cm_parser_kind(parser) != CM_TOKEN_POUND) return 0;
    next = parser->position + 1u < parser->tokens.len
        ? (const struct cm_token *)cm_vec_at_const(&parser->tokens,
            parser->position + 1u)
        : NULL;
    return next != NULL && next->kind == CM_TOKEN_BANG;
}

static int cm_parser_parse_one_attribute(CmParser *parser,
    CmAstAttributeId *out_id, CmAstAttributeStyle *out_style)
{
    const struct cm_token *first;
    const struct cm_token *last;
    CmAstAttribute attribute;

    memset(&attribute, 0, sizeof(attribute));
    first = cm_parser_token(parser);
    if (first == NULL || first->kind != CM_TOKEN_POUND) return 0;
    cm_parser_bump(parser);
    if (cm_parser_eat(parser, CM_TOKEN_BANG)) {
        attribute.style = CM_AST_ATTR_INNER;
    }
    if (cm_parser_kind(parser) != CM_TOKEN_LBRACKET) {
        cm_parser_error(parser, "expected '[' after '#'");
        return 0;
    }
    (void)cm_parser_skip_balanced(parser, CM_TOKEN_LBRACKET,
        CM_TOKEN_RBRACKET);
    last = cm_parser_previous(parser);
    attribute.text = cm_parser_intern_range(parser, first->start,
        cm_token_end(last));
    attribute.span.start = cm_parser_offset_u32(parser, first->start);
    attribute.span.end = cm_parser_offset_u32(parser, cm_token_end(last));
    *out_id = cm_ast_add_attribute(parser->ast, &attribute);
    *out_style = attribute.style;
    return 1;
}

static void cm_parser_parse_inner_attributes(CmParser *parser,
    CmVec *attributes)
{
    while (cm_parser_starts_inner_attribute(parser)) {
        CmAstAttributeId id;
        CmAstAttributeStyle style;

        if (!cm_parser_parse_one_attribute(parser, &id, &style)) break;
        (void)style;
        (void)cm_vec_push(attributes, &id);
    }
}

static void cm_parser_parse_attributes(CmParser *parser, CmVec *attributes)
{
    while (cm_parser_kind(parser) == CM_TOKEN_POUND) {
        CmAstAttributeId id;
        CmAstAttributeStyle style;

        if (!cm_parser_parse_one_attribute(parser, &id, &style)) break;
        if (style == CM_AST_ATTR_INNER) {
            cm_parser_error(parser,
                "inner attribute is only supported at a crate or module start");
        } else {
            (void)cm_vec_push(attributes, &id);
        }
    }
}

static CmAstVisibility cm_parser_parse_visibility(CmParser *parser)
{
    CmAstVisibility visibility;

    memset(&visibility, 0, sizeof(visibility));
    if (cm_parser_eat_keyword(parser, CM_KW_PUB)) {
        visibility.kind = CM_AST_VIS_PUBLIC;
        if (cm_parser_eat(parser, CM_TOKEN_LPAREN)) {
            if (cm_parser_eat_keyword(parser, CM_KW_CRATE)) {
                visibility.kind = CM_AST_VIS_CRATE;
            } else if (cm_parser_eat_keyword(parser, CM_KW_SELF_VALUE)) {
                visibility.kind = CM_AST_VIS_SELF;
            } else if (cm_parser_eat_keyword(parser, CM_KW_SUPER)) {
                visibility.kind = CM_AST_VIS_SUPER;
            } else {
                visibility.kind = CM_AST_VIS_RESTRICTED;
                (void)cm_parser_eat_keyword(parser, CM_KW_IN);
                visibility.restriction = cm_parser_parse_path(parser, 0);
            }
            (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
                "expected ')' after visibility");
        }
    } else if (cm_parser_keyword(parser, CM_KW_CRATE)) {
        const struct cm_token *next;

        next = parser->position + 1u < parser->tokens.len ?
            (const struct cm_token *)cm_vec_at_const(&parser->tokens,
                parser->position + 1u) : NULL;
        if (next == NULL || next->kind != CM_TOKEN_PATH_SEP) {
            cm_parser_bump(parser);
            visibility.kind = CM_AST_VIS_CRATE;
        }
    }
    return visibility;
}

/* Const blocks disable angle heuristics so comparison operators stay tokens. */
static void cm_parser_scan_generic_fragment(CmParser *parser,
    int stop_at_equal, int angle_delimiters)
{
    CmVec closing_tokens;
    unsigned int brace_depth;

    cm_vec_init(&closing_tokens, sizeof(enum cm_token_kind));
    brace_depth = 0u;
    while (cm_parser_kind(parser) != CM_TOKEN_EOF) {
        enum cm_token_kind expected;
        enum cm_token_kind kind;

        kind = cm_parser_kind(parser);
        if (closing_tokens.len == 0u
            && (kind == CM_TOKEN_COMMA || kind == CM_TOKEN_GT
                || (stop_at_equal && kind == CM_TOKEN_EQ))) {
            break;
        }
        if (angle_delimiters && brace_depth == 0u
            && kind == CM_TOKEN_LT) {
            expected = CM_TOKEN_GT;
            (void)cm_vec_push(&closing_tokens, &expected);
            cm_parser_bump(parser);
            continue;
        }
        if (kind == CM_TOKEN_LPAREN) {
            expected = CM_TOKEN_RPAREN;
            (void)cm_vec_push(&closing_tokens, &expected);
            cm_parser_bump(parser);
            continue;
        }
        if (kind == CM_TOKEN_LBRACKET) {
            expected = CM_TOKEN_RBRACKET;
            (void)cm_vec_push(&closing_tokens, &expected);
            cm_parser_bump(parser);
            continue;
        }
        if (kind == CM_TOKEN_LBRACE) {
            expected = CM_TOKEN_RBRACE;
            (void)cm_vec_push(&closing_tokens, &expected);
            ++brace_depth;
            cm_parser_bump(parser);
            continue;
        }
        if (kind == CM_TOKEN_GT || kind == CM_TOKEN_RPAREN
            || kind == CM_TOKEN_RBRACKET || kind == CM_TOKEN_RBRACE
            || kind == CM_TOKEN_SHR) {
            if (closing_tokens.len != 0u) {
                expected = *(const enum cm_token_kind *)cm_vec_at_const(
                    &closing_tokens, closing_tokens.len - 1u);
            } else {
                expected = CM_TOKEN_EOF;
            }
            if ((kind == CM_TOKEN_GT || kind == CM_TOKEN_SHR)
                && expected == CM_TOKEN_GT) {
                --closing_tokens.len;
                (void)cm_parser_eat(parser, CM_TOKEN_GT);
                continue;
            }
            if (kind == expected) {
                --closing_tokens.len;
                if (kind == CM_TOKEN_RBRACE) --brace_depth;
                cm_parser_bump(parser);
                continue;
            }
            if (brace_depth == 0u || (kind != CM_TOKEN_GT
                    && kind != CM_TOKEN_SHR)) {
                cm_parser_error(parser,
                    "mismatched delimiter in generic parameter");
            }
        }
        cm_parser_bump(parser);
    }
    if (closing_tokens.len != 0u) {
        cm_parser_error(parser,
            "unclosed delimiter in generic parameter");
    }
    cm_vec_destroy(&closing_tokens);
}

static CmAstType *cm_parser_get_type_mut(CmParser *parser, CmAstTypeId id)
{
    if (id == CM_AST_TYPE_NONE || (size_t)id > parser->ast->types.len) {
        return NULL;
    }
    return (CmAstType *)cm_vec_at(&parser->ast->types, (size_t)id - 1u);
}

static CmAstPath *cm_parser_get_path_mut(CmParser *parser, CmAstPathId id)
{
    if (id == CM_AST_PATH_NONE || (size_t)id > parser->ast->paths.len) {
        return NULL;
    }
    return (CmAstPath *)cm_vec_at(&parser->ast->paths, (size_t)id - 1u);
}

static void cm_parser_parse_callable_trait_arguments(CmParser *parser,
    CmAstTypeId trait_type)
{
    const struct cm_token *tuple_first;
    const struct cm_token *arrow;
    CmAstType tuple_type;
    CmAstTypeId tuple_id;
    CmVec tuple_elements;
    CmVec arguments;
    CmAstGenericArg argument;
    CmAstType *type;
    CmAstPath *path;
    CmAstPathId path_id;
    uint32_t segment_index;
    uint32_t old_argument_count;
    uint32_t index;

    type = cm_parser_get_type_mut(parser, trait_type);
    if (type == NULL || type->kind != CM_AST_TYPE_PATH) return;
    path_id = type->path;
    path = cm_parser_get_path_mut(parser, path_id);
    if (path == NULL || path->segment_count == 0u
        || path->segments == NULL) {
        return;
    }
    segment_index = path->segment_count - 1u;
    old_argument_count = path->segments[segment_index].argument_count;
    if (old_argument_count != 0u) {
        cm_parser_error(parser,
            "callable trait notation cannot follow generic arguments");
    }

    tuple_first = cm_parser_token(parser);
    memset(&tuple_type, 0, sizeof(tuple_type));
    tuple_type.kind = CM_AST_TYPE_TUPLE;
    tuple_type.tuple_provenance = CM_AST_TUPLE_CALLABLE_INPUTS;
    cm_vec_init(&tuple_elements, sizeof(CmAstTypeId));
    cm_parser_bump(parser);
    while (cm_parser_kind(parser) != CM_TOKEN_RPAREN
           && cm_parser_kind(parser) != CM_TOKEN_EOF) {
        CmAstTypeId element;

        element = cm_parser_parse_type(parser);
        (void)cm_vec_push(&tuple_elements, &element);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
    }
    (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
        "expected ')' after callable trait inputs");
    tuple_type.elements = (CmAstTypeId *)cm_parser_copy_array(parser,
        &tuple_elements);
    tuple_type.element_count = cm_parser_count_u32(&tuple_elements);
    tuple_type.span.start = cm_parser_offset_u32(parser,
        tuple_first == NULL ? 0u : tuple_first->start);
    tuple_type.span.end = cm_parser_offset_u32(parser,
        cm_token_end(cm_parser_previous(parser)));
    tuple_id = cm_ast_add_type(parser->ast, &tuple_type);
    cm_vec_destroy(&tuple_elements);

    cm_vec_init(&arguments, sizeof(CmAstGenericArg));
    path = cm_parser_get_path_mut(parser, path_id);
    if (path != NULL && path->segments != NULL) {
        for (index = 0u; index < old_argument_count; ++index) {
            (void)cm_vec_push(&arguments,
                &path->segments[segment_index].arguments[index]);
        }
    }
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_AST_GENERIC_TYPE;
    argument.type = tuple_id;
    argument.span = tuple_type.span;
    (void)cm_vec_push(&arguments, &argument);

    arrow = cm_parser_token(parser);
    if (cm_parser_eat(parser, CM_TOKEN_THIN_ARROW)) {
        const struct cm_token *return_first;

        return_first = cm_parser_token(parser);
        memset(&argument, 0, sizeof(argument));
        argument.kind = CM_AST_GENERIC_BINDING;
        argument.name = cm_interner_intern(&parser->ast->strings,
            "Output", strlen("Output"));
        if (cm_parser_kind(parser) == CM_TOKEN_PLUS
            || cm_parser_kind(parser) == CM_TOKEN_COMMA
            || cm_parser_kind(parser) == CM_TOKEN_GT
            || cm_parser_kind(parser) == CM_TOKEN_EQ
            || cm_parser_kind(parser) == CM_TOKEN_SEMICOLON
            || cm_parser_kind(parser) == CM_TOKEN_LBRACE
            || cm_parser_kind(parser) == CM_TOKEN_RBRACE
            || cm_parser_kind(parser) == CM_TOKEN_EOF) {
            cm_parser_error(parser,
                "expected return type after callable trait '->'");
        } else {
            argument.type = cm_parser_parse_type(parser);
        }
        argument.span.start = cm_parser_offset_u32(parser,
            arrow == NULL ? 0u : arrow->start);
        argument.span.end = cm_parser_offset_u32(parser,
            argument.type == CM_AST_TYPE_NONE
                ? (return_first == NULL ? 0u : return_first->start)
                : cm_token_end(cm_parser_previous(parser)));
        (void)cm_vec_push(&arguments, &argument);
    }

    path = cm_parser_get_path_mut(parser, path_id);
    if (path != NULL && path->segments != NULL) {
        path->segments[segment_index].arguments =
            (CmAstGenericArg *)cm_parser_copy_array(parser, &arguments);
        path->segments[segment_index].argument_count =
            cm_parser_count_u32(&arguments);
        path->span.end = cm_parser_offset_u32(parser,
            cm_token_end(cm_parser_previous(parser)));
    }
    type = cm_parser_get_type_mut(parser, trait_type);
    if (type != NULL) {
        type->span.end = cm_parser_offset_u32(parser,
            cm_token_end(cm_parser_previous(parser)));
    }
    cm_vec_destroy(&arguments);
}

static void cm_parser_parse_lifetime_binder(CmParser *parser,
    CmAstLifetimeBinder *binder)
{
    const struct cm_token *first;
    const struct cm_token *last;
    CmVec lifetimes;

    first = cm_parser_token(parser);
    cm_vec_init(&lifetimes, sizeof(CmInternId));
    (void)cm_parser_expect_keyword(parser, CM_KW_FOR,
        "expected 'for' before lifetime binder");
    if (!cm_parser_expect_type_left_angle(parser,
            "expected '<' after 'for'")) {
        cm_vec_destroy(&lifetimes);
        return;
    }
    while (cm_parser_kind(parser) != CM_TOKEN_GT
           && cm_parser_kind(parser) != CM_TOKEN_EOF) {
        CmInternId lifetime;

        if (cm_parser_kind(parser) != CM_TOKEN_LIFETIME) {
            cm_parser_error(parser,
                "expected lifetime parameter in higher-ranked binder");
            cm_parser_bump(parser);
        } else {
            lifetime = cm_parser_intern_token(parser,
                cm_parser_token(parser));
            (void)cm_vec_push(&lifetimes, &lifetime);
            cm_parser_bump(parser);
        }
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) {
            break;
        }
    }
    if (lifetimes.len == 0u) {
        cm_parser_error(parser,
            "higher-ranked binder requires a lifetime parameter");
    }
    (void)cm_parser_expect(parser, CM_TOKEN_GT,
        "expected '>' after higher-ranked lifetime binder");
    last = cm_parser_previous(parser);
    binder->lifetimes = (CmInternId *)cm_parser_copy_array(parser,
        &lifetimes);
    binder->lifetime_count = cm_parser_count_u32(&lifetimes);
    binder->span.start = cm_parser_offset_u32(parser,
        first == NULL ? 0u : first->start);
    binder->span.end = cm_parser_offset_u32(parser, cm_token_end(last));
    cm_vec_destroy(&lifetimes);
}

static void cm_parser_parse_trait_type_bounds(CmParser *parser,
    CmAstType *type)
{
    CmVec bounds;

    cm_vec_init(&bounds, sizeof(CmAstTypeBound));
    for (;;) {
        const struct cm_token *bound_first;
        const struct cm_token *bound_last;
        const CmAstType *trait_type;
        CmAstTypeBound bound;

        memset(&bound, 0, sizeof(bound));
        bound_first = cm_parser_token(parser);
        if (cm_parser_kind(parser) == CM_TOKEN_PLUS
            || cm_parser_kind(parser) == CM_TOKEN_COMMA
            || cm_parser_kind(parser) == CM_TOKEN_RPAREN
            || cm_parser_kind(parser) == CM_TOKEN_SEMICOLON
            || cm_parser_kind(parser) == CM_TOKEN_EQ
            || cm_parser_kind(parser) == CM_TOKEN_LBRACE
            || cm_parser_kind(parser) == CM_TOKEN_EOF) {
            cm_parser_error(parser,
                type->kind == CM_AST_TYPE_IMPL_TRAIT
                    ? "expected trait or lifetime bound after 'impl'"
                    : "expected trait or lifetime bound after 'dyn'");
            break;
        }
        if (cm_parser_eat(parser, CM_TOKEN_QUESTION)) {
            bound.modifier = CM_AST_TYPE_BOUND_RELAXED;
        } else if (cm_parser_eat(parser, CM_TOKEN_TILDE)) {
            bound.modifier = CM_AST_TYPE_BOUND_CONDITIONALLY_CONST;
            (void)cm_parser_expect_keyword(parser, CM_KW_CONST,
                "expected 'const' after '~' in type bound");
        }
        if (cm_parser_keyword(parser, CM_KW_FOR)) {
            cm_parser_parse_lifetime_binder(parser, &bound.binder);
        }
        if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
            if (bound.binder.lifetime_count != 0u) {
                cm_parser_error(parser,
                    "higher-ranked binder requires a trait path");
            }
            if (bound.modifier != CM_AST_TYPE_BOUND_REQUIRED) {
                cm_parser_error(parser,
                    "a lifetime type bound cannot have modifiers");
            }
            bound.lifetime = cm_parser_intern_token(parser,
                cm_parser_token(parser));
            cm_parser_bump(parser);
            trait_type = NULL;
        } else {
            bound.trait_type = cm_parser_parse_type(parser);
            trait_type = cm_ast_get_type(parser->ast, bound.trait_type);
            if (trait_type != NULL && trait_type->kind == CM_AST_TYPE_PATH
                && cm_parser_kind(parser) == CM_TOKEN_LPAREN) {
                cm_parser_parse_callable_trait_arguments(parser,
                    bound.trait_type);
                trait_type = cm_ast_get_type(parser->ast,
                    bound.trait_type);
            }
            if (trait_type == NULL
                || trait_type->kind != CM_AST_TYPE_PATH) {
                cm_parser_error(parser,
                    type->kind == CM_AST_TYPE_IMPL_TRAIT
                        ? "expected a path or lifetime in impl trait bound"
                        : "expected a path or lifetime in dyn trait bound");
            }
        }
        bound_last = cm_parser_previous(parser);
        bound.span.start = cm_parser_offset_u32(parser,
            bound_first == NULL ? 0u : bound_first->start);
        bound.span.end = cm_parser_offset_u32(parser,
            cm_token_end(bound_last));
        (void)cm_vec_push(&bounds, &bound);
        if (!cm_parser_eat(parser, CM_TOKEN_PLUS)) break;
    }
    type->bounds = (CmAstTypeBound *)cm_parser_copy_array(parser, &bounds);
    type->bound_count = cm_parser_count_u32(&bounds);
    cm_vec_destroy(&bounds);
}

static int cm_parser_at_generic_constraint_end(const CmParser *parser)
{
    return cm_parser_kind(parser) == CM_TOKEN_EQ
        || cm_parser_kind(parser) == CM_TOKEN_COMMA
        || cm_parser_kind(parser) == CM_TOKEN_GT
        || cm_parser_kind(parser) == CM_TOKEN_SHR
        || cm_parser_kind(parser) == CM_TOKEN_EOF;
}

static void cm_parser_parse_generic_constraint_bounds(CmParser *parser,
    CmAstGenericParamKind parameter_kind,
    CmAstGenericParamBound **out_bounds, uint32_t *out_bound_count)
{
    CmVec bounds;

    cm_vec_init(&bounds, sizeof(CmAstGenericParamBound));
    while (!cm_parser_at_generic_constraint_end(parser)) {
        const struct cm_token *bound_first;
        const struct cm_token *bound_last;
        const CmAstType *type;
        CmAstGenericParamBound bound;
        int unsupported;

        memset(&bound, 0, sizeof(bound));
        bound_first = cm_parser_token(parser);
        bound.modifier = CM_AST_GENERIC_BOUND_REQUIRED;
        unsupported = 0;
        if (cm_parser_eat(parser, CM_TOKEN_QUESTION)) {
            bound.modifier = CM_AST_GENERIC_BOUND_RELAXED;
        } else if (cm_parser_eat(parser, CM_TOKEN_TILDE)) {
            bound.modifier =
                CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST;
            (void)cm_parser_expect_keyword(parser, CM_KW_CONST,
                "expected 'const' after '~' in generic parameter bound");
        }
        if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
            bound.kind = CM_AST_GENERIC_BOUND_LIFETIME;
            bound.lifetime = cm_parser_intern_token(parser,
                cm_parser_token(parser));
            if (bound.modifier != CM_AST_GENERIC_BOUND_REQUIRED) {
                cm_parser_error(parser,
                    "lifetime generic parameter bounds cannot have "
                    "modifiers");
            }
            cm_parser_bump(parser);
        } else if (cm_parser_keyword(parser, CM_KW_CONST)) {
            cm_parser_error(parser,
                "const generic parameter bounds are unsupported");
            cm_parser_bump(parser);
            unsupported = 1;
        } else if (cm_parser_keyword(parser, CM_KW_FOR)) {
            cm_parser_error(parser,
                "HRTB generic parameter bounds are unsupported");
            cm_parser_bump(parser);
            unsupported = 1;
        } else if (cm_parser_keyword(parser, CM_KW_USE)) {
            cm_parser_error(parser,
                "use generic parameter bounds are unsupported");
            cm_parser_bump(parser);
            unsupported = 1;
        }
        if (unsupported) {
            cm_parser_scan_generic_fragment(parser, 1, 1);
            break;
        }
        if (bound.kind == CM_AST_GENERIC_BOUND_TRAIT) {
            if (cm_parser_kind(parser) == CM_TOKEN_PLUS) {
                cm_parser_error(parser,
                    "expected bound in generic parameter constraint");
                break;
            }
            bound.trait_type = cm_parser_parse_type(parser);
            type = cm_ast_get_type(parser->ast, bound.trait_type);
            if (type == NULL || type->kind != CM_AST_TYPE_PATH) {
                cm_parser_error(parser,
                    "expected a path in generic parameter trait bound");
            }
            if (type != NULL && type->kind == CM_AST_TYPE_PATH
                && cm_parser_kind(parser) == CM_TOKEN_LPAREN) {
                cm_parser_parse_callable_trait_arguments(parser,
                    bound.trait_type);
            }
            if (parameter_kind == CM_AST_PARAM_LIFETIME) {
                cm_parser_error(parser,
                    "lifetime generic parameters require lifetime bounds");
            }
            if (bound.modifier == CM_AST_GENERIC_BOUND_RELAXED
                && !cm_parser_type_is_plain_sized_path(parser,
                    bound.trait_type)) {
                cm_parser_error(parser,
                    "only ?Sized relaxed generic parameter bounds are "
                    "supported");
            }
        }
        bound_last = cm_parser_previous(parser);
        bound.span.start = cm_parser_offset_u32(parser,
            bound_first == NULL ? 0u : bound_first->start);
        bound.span.end = cm_parser_offset_u32(parser,
            cm_token_end(bound_last));
        (void)cm_vec_push(&bounds, &bound);
        if (cm_parser_eat(parser, CM_TOKEN_PLUS)) {
            if (cm_parser_at_generic_constraint_end(parser)
                || cm_parser_kind(parser) == CM_TOKEN_PLUS) {
                cm_parser_error(parser,
                    "expected bound after '+' in generic parameter");
                break;
            }
            continue;
        }
        if (!cm_parser_at_generic_constraint_end(parser)) {
            cm_parser_error(parser,
                "expected '+' or generic parameter terminator after bound");
            cm_parser_scan_generic_fragment(parser, 1, 1);
        }
        break;
    }
    *out_bounds = (CmAstGenericParamBound *)cm_parser_copy_array(
        parser, &bounds);
    *out_bound_count = cm_parser_count_u32(&bounds);
    cm_vec_destroy(&bounds);
}

static void cm_parser_parse_generic_parameter_bounds(CmParser *parser,
    CmAstGenericParam *parameter)
{
    cm_parser_parse_generic_constraint_bounds(parser, parameter->kind,
        &parameter->bounds, &parameter->bound_count);
}

static void cm_parser_parse_generic_parameters(CmParser *parser,
    CmAstItem *item)
{
    CmVec parameters;

    if (cm_parser_kind(parser) != CM_TOKEN_LT) {
        return;
    }
    cm_vec_init(&parameters, sizeof(CmAstGenericParam));
    cm_parser_bump(parser);
    while (cm_parser_kind(parser) != CM_TOKEN_GT &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        const struct cm_token *first;
        const struct cm_token *last;
        const struct cm_token *const_default_first;
        CmAstGenericParam parameter;
        CmVec attributes;
        int had_constraint;
        int parsed_const_default;
        int parsed_type_default;

        memset(&parameter, 0, sizeof(parameter));
        had_constraint = 0;
        parsed_const_default = 0;
        parsed_type_default = 0;
        const_default_first = NULL;
        first = cm_parser_token(parser);
        cm_vec_init(&attributes, sizeof(CmAstAttributeId));
        cm_parser_parse_attributes(parser, &attributes);
        parameter.attributes = (CmAstAttributeId *)cm_parser_copy_array(
            parser, &attributes);
        parameter.attribute_count = cm_parser_count_u32(&attributes);
        cm_vec_destroy(&attributes);
        if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
            parameter.kind = CM_AST_PARAM_LIFETIME;
            parameter.name = cm_parser_intern_token(parser,
                cm_parser_token(parser));
            cm_parser_bump(parser);
        } else if (cm_parser_eat_keyword(parser, CM_KW_CONST)) {
            parameter.kind = CM_AST_PARAM_CONST;
            parameter.name = cm_parser_parse_name(parser,
                "expected const parameter name");
        } else {
            parameter.kind = CM_AST_PARAM_TYPE;
            parameter.name = cm_parser_parse_name(parser,
                "expected generic parameter");
        }
        if (cm_parser_eat(parser, CM_TOKEN_COLON)) {
            const struct cm_token *constraint_first;
            const struct cm_token *constraint_last;

            had_constraint = 1;
            constraint_first = cm_parser_token(parser);
            if (cm_parser_kind(parser) == CM_TOKEN_EQ
                || cm_parser_kind(parser) == CM_TOKEN_COMMA
                || cm_parser_kind(parser) == CM_TOKEN_GT
                || cm_parser_kind(parser) == CM_TOKEN_EOF) {
                cm_parser_error(parser,
                    "expected generic parameter constraint after ':'");
            }
            if (parameter.kind == CM_AST_PARAM_TYPE) {
                cm_parser_parse_generic_parameter_bounds(parser,
                    &parameter);
            } else if (parameter.kind == CM_AST_PARAM_CONST) {
                if (!cm_parser_at_generic_constraint_end(parser)) {
                    parameter.declared_type = cm_parser_parse_type(parser);
                }
                if (cm_parser_kind(parser) == CM_TOKEN_PLUS) {
                    cm_parser_error(parser,
                        "const generic parameter bounds are unsupported");
                    cm_parser_scan_generic_fragment(parser, 1, 1);
                } else if (!cm_parser_at_generic_constraint_end(parser)) {
                    cm_parser_error(parser,
                        "unexpected tokens after const generic parameter "
                        "type");
                    cm_parser_scan_generic_fragment(parser, 1, 1);
                }
            } else {
                cm_parser_parse_generic_parameter_bounds(parser,
                    &parameter);
            }
            constraint_last = cm_parser_previous(parser);
            if (constraint_first != NULL && constraint_last != NULL
                && constraint_last->start >= constraint_first->start) {
                parameter.constraint = cm_parser_intern_range(parser,
                    constraint_first->start, cm_token_end(constraint_last));
            }
        }
        if (parameter.kind == CM_AST_PARAM_CONST && !had_constraint) {
            cm_parser_error(parser,
                "expected ':' and a type after const generic parameter");
        }
        if (parameter.kind == CM_AST_PARAM_CONST
            && cm_parser_eat(parser, CM_TOKEN_EQ)) {
            parsed_const_default = 1;
            const_default_first = cm_parser_token(parser);
            if (cm_parser_kind(parser) == CM_TOKEN_COMMA
                || cm_parser_kind(parser) == CM_TOKEN_GT
                || cm_parser_kind(parser) == CM_TOKEN_EOF) {
                cm_parser_error(parser,
                    "expected const generic default after '='");
            }
            if (item->kind == CM_AST_ITEM_FUNCTION
                || item->kind == CM_AST_ITEM_IMPL) {
                cm_parser_error(parser,
                    "generic const defaults are not allowed on this item");
            }
        } else if (parameter.kind == CM_AST_PARAM_TYPE
            && cm_parser_eat(parser, CM_TOKEN_EQ)) {
            parameter.default_type = cm_parser_parse_type(parser);
            parsed_type_default = 1;
            if (item->kind == CM_AST_ITEM_FUNCTION
                || item->kind == CM_AST_ITEM_IMPL) {
                cm_parser_error(parser,
                    "generic type defaults are not allowed on this item");
            }
        } else if (parameter.kind == CM_AST_PARAM_LIFETIME
            && cm_parser_kind(parser) == CM_TOKEN_EQ) {
            cm_parser_error(parser,
                "lifetime generic parameters cannot have defaults");
            cm_parser_bump(parser);
        }
        if (parsed_type_default
            && cm_parser_kind(parser) != CM_TOKEN_COMMA
            && cm_parser_kind(parser) != CM_TOKEN_GT
            && cm_parser_kind(parser) != CM_TOKEN_EOF) {
            cm_parser_error(parser,
                "unexpected tokens after generic type default");
        }
        if (parsed_const_default) {
            parameter.default_const_expr =
                cm_parser_parse_expression_bp_mode(parser, 14u, 1, 1);
            if (cm_parser_kind(parser) != CM_TOKEN_COMMA
                && cm_parser_kind(parser) != CM_TOKEN_GT
                && cm_parser_kind(parser) != CM_TOKEN_EOF) {
                cm_parser_error(parser,
                    "unexpected tokens after generic const default");
                cm_parser_scan_generic_fragment(parser, 0, 0);
            }
            last = cm_parser_previous(parser);
            if (const_default_first != NULL && last != NULL
                && last->start >= const_default_first->start) {
                parameter.default_const = cm_parser_intern_range(parser,
                    const_default_first->start, cm_token_end(last));
            }
        } else if (!parsed_type_default) {
            cm_parser_scan_generic_fragment(parser, 0, 1);
        }
        last = cm_parser_previous(parser);
        parameter.declaration = cm_parser_intern_range(parser, first->start,
            cm_token_end(last));
        (void)cm_vec_push(&parameters, &parameter);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) {
            break;
        }
    }
    (void)cm_parser_expect(parser, CM_TOKEN_GT,
        "expected '>' after generic parameters");
    item->generic_parameters = (CmAstGenericParam *)cm_parser_copy_array(
        parser, &parameters);
    item->generic_parameter_count = cm_parser_count_u32(&parameters);
    cm_vec_destroy(&parameters);
}

static int cm_parser_at_where_clause_end(const CmParser *parser,
    enum cm_token_kind stop)
{
    return cm_parser_kind(parser) == stop
        || cm_parser_kind(parser) == CM_TOKEN_SEMICOLON
        || cm_parser_kind(parser) == CM_TOKEN_EQ
        || cm_parser_kind(parser) == CM_TOKEN_EOF;
}

static void cm_parser_parse_where_clause_into(CmParser *parser,
    CmInternId *out_clause, CmAstWherePredicate **out_predicates,
    uint32_t *out_predicate_count, enum cm_token_kind stop)
{
    const struct cm_token *first;
    size_t last_end;
    CmVec predicates;

    if (!cm_parser_eat_keyword(parser, CM_KW_WHERE)) return;
    first = cm_parser_token(parser);
    last_end = first == NULL ? 0u : first->start;
    cm_vec_init(&predicates, sizeof(CmAstWherePredicate));
    if (cm_parser_at_where_clause_end(parser, stop)) {
        cm_parser_error(parser, "expected predicate after 'where'");
    }
    while (!cm_parser_at_where_clause_end(parser, stop)) {
        const struct cm_token *predicate_first;
        const struct cm_token *predicate_last;
        CmAstWherePredicate predicate;
        CmVec bounds;

        memset(&predicate, 0, sizeof(predicate));
        predicate_first = cm_parser_token(parser);
        if (cm_parser_keyword(parser, CM_KW_FOR)) {
            cm_parser_parse_lifetime_binder(parser, &predicate.binder);
        }
        if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
            predicate.kind = CM_AST_WHERE_PREDICATE_LIFETIME;
            predicate.subject_lifetime = cm_parser_intern_token(parser,
                cm_parser_token(parser));
            cm_parser_bump(parser);
        } else {
            predicate.subject = cm_parser_parse_type(parser);
        }
        (void)cm_parser_expect(parser, CM_TOKEN_COLON,
            "expected ':' after where-predicate subject");
        cm_vec_init(&bounds, sizeof(CmAstWhereBound));
        if (cm_parser_at_where_clause_end(parser, stop)
            || cm_parser_kind(parser) == CM_TOKEN_COMMA) {
            cm_parser_error(parser,
                "expected trait bound after where-predicate ':'");
        }
        while (!cm_parser_at_where_clause_end(parser, stop)
               && cm_parser_kind(parser) != CM_TOKEN_COMMA) {
            const struct cm_token *bound_first;
            const struct cm_token *bound_last;
            const CmAstType *type;
            CmAstWhereBound bound;
            int has_modifier;

            memset(&bound, 0, sizeof(bound));
            bound_first = cm_parser_token(parser);
            bound.modifier = CM_AST_WHERE_BOUND_REQUIRED;
            has_modifier = 0;
            if (cm_parser_eat(parser, CM_TOKEN_QUESTION)) {
                bound.modifier = CM_AST_WHERE_BOUND_RELAXED;
                has_modifier = 1;
            } else if (cm_parser_eat(parser, CM_TOKEN_TILDE)) {
                bound.modifier =
                    CM_AST_WHERE_BOUND_CONDITIONALLY_CONST;
                has_modifier = 1;
                (void)cm_parser_expect_keyword(parser, CM_KW_CONST,
                    "expected 'const' after '~' in where bound");
            } else if (cm_parser_eat_keyword(parser, CM_KW_CONST)) {
                bound.modifier = CM_AST_WHERE_BOUND_CONST;
                has_modifier = 1;
            }
            if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
                const struct cm_token *lifetime;

                lifetime = cm_parser_token(parser);
                bound.kind = CM_AST_WHERE_BOUND_LIFETIME;
                bound.lifetime = cm_parser_intern_token(parser, lifetime);
                if (has_modifier) {
                    cm_parser_error(parser,
                        "lifetime where bounds cannot have modifiers");
                }
                cm_parser_bump(parser);
            } else {
                if (predicate.kind == CM_AST_WHERE_PREDICATE_LIFETIME) {
                    cm_parser_error(parser,
                        "lifetime where predicates require lifetime bounds");
                }
                if (cm_parser_keyword(parser, CM_KW_FOR)) {
                    cm_parser_parse_lifetime_binder(parser, &bound.binder);
                } else if (cm_parser_keyword(parser, CM_KW_USE)) {
                    cm_parser_error(parser,
                        "use where bounds are unsupported");
                    cm_parser_bump(parser);
                }
                if (cm_parser_at_where_clause_end(parser, stop)
                    || cm_parser_kind(parser) == CM_TOKEN_COMMA
                    || cm_parser_kind(parser) == CM_TOKEN_PLUS) {
                    cm_parser_error(parser,
                        "expected trait path in where bound");
                    break;
                }
                bound.trait_type = cm_parser_parse_type(parser);
                type = cm_ast_get_type(parser->ast, bound.trait_type);
                if (type != NULL && type->kind == CM_AST_TYPE_PATH
                    && cm_parser_kind(parser) == CM_TOKEN_LPAREN) {
                    cm_parser_parse_callable_trait_arguments(parser,
                        bound.trait_type);
                    type = cm_ast_get_type(parser->ast, bound.trait_type);
                }
                if (type == NULL || type->kind != CM_AST_TYPE_PATH) {
                    cm_parser_error(parser,
                        "expected a path in where trait bound");
                }
            }
            bound_last = cm_parser_previous(parser);
            bound.span.start = cm_parser_offset_u32(parser,
                bound_first == NULL ? 0u : bound_first->start);
            bound.span.end = cm_parser_offset_u32(parser,
                cm_token_end(bound_last));
            (void)cm_vec_push(&bounds, &bound);
            if (!cm_parser_eat(parser, CM_TOKEN_PLUS)) break;
            if (cm_parser_at_where_clause_end(parser, stop)
                || cm_parser_kind(parser) == CM_TOKEN_COMMA
                || cm_parser_kind(parser) == CM_TOKEN_PLUS) {
                cm_parser_error(parser,
                    "expected trait bound after '+' in where predicate");
                break;
            }
        }
        predicate.bounds = (CmAstWhereBound *)cm_parser_copy_array(parser,
            &bounds);
        predicate.bound_count = cm_parser_count_u32(&bounds);
        cm_vec_destroy(&bounds);
        predicate_last = cm_parser_previous(parser);
        predicate.span.start = cm_parser_offset_u32(parser,
            predicate_first == NULL ? 0u : predicate_first->start);
        predicate.span.end = cm_parser_offset_u32(parser,
            cm_token_end(predicate_last));
        last_end = cm_token_end(predicate_last);
        (void)cm_vec_push(&predicates, &predicate);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) break;
        last_end = cm_token_end(cm_parser_previous(parser));
        if (cm_parser_at_where_clause_end(parser, stop)) break;
    }
    if (!cm_parser_at_where_clause_end(parser, stop)) {
        cm_parser_error(parser,
            "expected ',' or item terminator after where predicate");
    }
    *out_clause = cm_parser_intern_range(parser,
        first == NULL ? 0u : first->start, last_end);
    *out_predicates = (CmAstWherePredicate *)cm_parser_copy_array(
        parser, &predicates);
    *out_predicate_count = cm_parser_count_u32(&predicates);
    cm_vec_destroy(&predicates);
}

static void cm_parser_parse_where_clause(CmParser *parser, CmAstItem *item,
    enum cm_token_kind stop)
{
    cm_parser_parse_where_clause_into(parser, &item->where_clause,
        &item->where_predicates, &item->where_predicate_count, stop);
}

static void cm_parser_parse_post_value_where_clause(CmParser *parser,
    CmAstItem *item)
{
    cm_parser_parse_where_clause_into(parser,
        &item->data.value_item.post_value_where_clause,
        &item->data.value_item.post_value_where_predicates,
        &item->data.value_item.post_value_where_predicate_count,
        CM_TOKEN_SEMICOLON);
}

static int cm_parser_at_associated_type_bound_end(const CmParser *parser)
{
    return cm_parser_kind(parser) == CM_TOKEN_SEMICOLON
        || cm_parser_kind(parser) == CM_TOKEN_EQ
        || cm_parser_kind(parser) == CM_TOKEN_EOF
        || cm_parser_keyword(parser, CM_KW_WHERE);
}

static int cm_parser_type_is_plain_sized_path(const CmParser *parser,
    CmAstTypeId type_id)
{
    const CmAstType *type;
    const CmAstPath *path;
    const CmInternedString *name;

    type = cm_ast_get_type(parser->ast, type_id);
    path = type == NULL ? NULL : cm_ast_get_path(parser->ast, type->path);
    if (type == NULL || type->kind != CM_AST_TYPE_PATH || path == NULL
        || path->absolute || path->segment_count != 1u
        || path->segments == NULL
        || path->segments[0].argument_count != 0u) {
        return 0;
    }
    name = cm_ast_get_string(parser->ast, path->segments[0].name);
    return name != NULL && name->len == sizeof("Sized") - 1u
        && memcmp(name->bytes, "Sized", sizeof("Sized") - 1u) == 0;
}

static void cm_parser_parse_associated_type_bounds(CmParser *parser,
    CmAstItem *item)
{
    CmVec bounds;

    cm_vec_init(&bounds, sizeof(CmAstAssociatedTypeBound));
    if (cm_parser_at_associated_type_bound_end(parser)) {
        cm_parser_error(parser, "expected associated-type bound after ':'");
    }
    while (!cm_parser_at_associated_type_bound_end(parser)) {
        const struct cm_token *bound_first;
        const struct cm_token *bound_last;
        const CmAstType *type;
        CmAstAssociatedTypeBound bound;

        memset(&bound, 0, sizeof(bound));
        bound_first = cm_parser_token(parser);
        bound.modifier = CM_AST_ASSOC_BOUND_REQUIRED;
        if (cm_parser_eat(parser, CM_TOKEN_QUESTION)) {
            bound.modifier = CM_AST_ASSOC_BOUND_RELAXED;
        } else if (cm_parser_eat(parser, CM_TOKEN_TILDE)) {
            cm_parser_error(parser,
                "~const associated-type bounds are unsupported");
            (void)cm_parser_eat_keyword(parser, CM_KW_CONST);
        }
        if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
            bound.kind = CM_AST_ASSOC_BOUND_LIFETIME;
            bound.lifetime = cm_parser_intern_token(parser,
                cm_parser_token(parser));
            if (bound.modifier != CM_AST_ASSOC_BOUND_REQUIRED) {
                cm_parser_error(parser,
                    "lifetime associated-type bounds cannot have "
                    "modifiers");
            }
            cm_parser_bump(parser);
        } else if (cm_parser_keyword(parser, CM_KW_FOR)) {
            cm_parser_error(parser,
                "HRTB associated-type bounds are unsupported");
            cm_parser_bump(parser);
        } else if (cm_parser_keyword(parser, CM_KW_USE)) {
            cm_parser_error(parser,
                "use associated-type bounds are unsupported");
            cm_parser_bump(parser);
        }
        if (bound.kind == CM_AST_ASSOC_BOUND_TRAIT) {
            if (cm_parser_at_associated_type_bound_end(parser)
                || cm_parser_kind(parser) == CM_TOKEN_PLUS) {
                cm_parser_error(parser,
                    "expected associated-type bound path");
                break;
            }
            bound.trait_type = cm_parser_parse_type(parser);
            type = cm_ast_get_type(parser->ast, bound.trait_type);
            if (type == NULL || type->kind != CM_AST_TYPE_PATH) {
                cm_parser_error(parser,
                    "expected a path in associated-type bound");
            }
            if (bound.modifier == CM_AST_ASSOC_BOUND_RELAXED
                && !cm_parser_type_is_plain_sized_path(parser,
                    bound.trait_type)) {
                cm_parser_error(parser,
                    "only ?Sized relaxed associated-type bounds are "
                    "supported");
            }
        }
        bound_last = cm_parser_previous(parser);
        bound.span.start = cm_parser_offset_u32(parser,
            bound_first == NULL ? 0u : bound_first->start);
        bound.span.end = cm_parser_offset_u32(parser,
            cm_token_end(bound_last));
        (void)cm_vec_push(&bounds, &bound);
        if (!cm_parser_eat(parser, CM_TOKEN_PLUS)) {
            break;
        }
        if (cm_parser_at_associated_type_bound_end(parser)
            || cm_parser_kind(parser) == CM_TOKEN_PLUS) {
            cm_parser_error(parser,
                "expected associated-type bound after '+'");
            break;
        }
    }
    item->data.value_item.bounds =
        (CmAstAssociatedTypeBound *)cm_parser_copy_array(parser, &bounds);
    item->data.value_item.bound_count = cm_parser_count_u32(&bounds);
    cm_vec_destroy(&bounds);
}

static int cm_parser_at_supertrait_end(const CmParser *parser)
{
    return cm_parser_kind(parser) == CM_TOKEN_LBRACE
        || cm_parser_kind(parser) == CM_TOKEN_SEMICOLON
        || cm_parser_kind(parser) == CM_TOKEN_EQ
        || cm_parser_kind(parser) == CM_TOKEN_EOF
        || cm_parser_keyword(parser, CM_KW_WHERE);
}

static void cm_parser_parse_trait_bounds(CmParser *parser,
    CmInternId *text, CmAstSupertrait **structured, uint32_t *count)
{
    const struct cm_token *first;
    size_t last_end;
    CmVec supertraits;

    first = cm_parser_token(parser);
    last_end = first == NULL ? 0u : first->start;
    cm_vec_init(&supertraits, sizeof(CmAstSupertrait));
    if (cm_parser_at_supertrait_end(parser)) {
        cm_parser_error(parser, "expected supertrait after ':'");
    }
    while (!cm_parser_at_supertrait_end(parser)) {
        const struct cm_token *bound_first;
        const struct cm_token *bound_last;
        const CmAstType *type;
        CmAstSupertrait supertrait;

        memset(&supertrait, 0, sizeof(supertrait));
        bound_first = cm_parser_token(parser);
        if (cm_parser_eat(parser, CM_TOKEN_TILDE)) {
            supertrait.modifier =
                CM_AST_SUPERTRAIT_CONDITIONALLY_CONST;
            (void)cm_parser_expect_keyword(parser, CM_KW_CONST,
                "expected 'const' after '~' in supertrait");
        } else {
            supertrait.modifier = CM_AST_SUPERTRAIT_REQUIRED;
        }
        if (cm_parser_kind(parser) == CM_TOKEN_QUESTION) {
            cm_parser_error(parser,
                "optional supertrait bounds are unsupported");
            cm_parser_bump(parser);
        }
        if (cm_parser_kind(parser) == CM_TOKEN_LIFETIME) {
            supertrait.kind = CM_AST_SUPERTRAIT_LIFETIME;
            supertrait.lifetime = cm_parser_intern_token(parser,
                cm_parser_token(parser));
            if (supertrait.modifier != CM_AST_SUPERTRAIT_REQUIRED) {
                cm_parser_error(parser,
                    "lifetime supertrait bounds cannot have modifiers");
            }
            cm_parser_bump(parser);
        } else if (cm_parser_keyword(parser, CM_KW_FOR)) {
            cm_parser_error(parser, "HRTB supertrait bounds are unsupported");
            cm_parser_bump(parser);
        }
        if (supertrait.kind == CM_AST_SUPERTRAIT_TRAIT) {
            if (cm_parser_at_supertrait_end(parser)
                || cm_parser_kind(parser) == CM_TOKEN_PLUS) {
                cm_parser_error(parser, "expected supertrait path");
                break;
            }
            supertrait.type = cm_parser_parse_type(parser);
            type = cm_ast_get_type(parser->ast, supertrait.type);
            if (type == NULL || type->kind != CM_AST_TYPE_PATH) {
                cm_parser_error(parser,
                    "expected a path in supertrait bound");
            }
        }
        bound_last = cm_parser_previous(parser);
        supertrait.span.start = cm_parser_offset_u32(parser,
            bound_first == NULL ? 0u : bound_first->start);
        supertrait.span.end = cm_parser_offset_u32(parser,
            cm_token_end(bound_last));
        last_end = cm_token_end(bound_last);
        (void)cm_vec_push(&supertraits, &supertrait);
        if (!cm_parser_eat(parser, CM_TOKEN_PLUS)) {
            break;
        }
        if (cm_parser_at_supertrait_end(parser)
            || cm_parser_kind(parser) == CM_TOKEN_PLUS) {
            cm_parser_error(parser, "expected supertrait after '+'");
            break;
        }
    }
    if (supertraits.len != 0u) {
        *text = cm_parser_intern_range(parser, first->start, last_end);
        *structured =
            (CmAstSupertrait *)cm_parser_copy_array(parser, &supertraits);
        *count = cm_parser_count_u32(&supertraits);
    }
    cm_vec_destroy(&supertraits);
}

static CmAstField *cm_parser_parse_fields(CmParser *parser,
    CmAstFieldForm form, uint32_t *count)
{
    CmVec fields;
    enum cm_token_kind closing;

    cm_vec_init(&fields, sizeof(CmAstField));
    closing = form == CM_AST_FIELDS_NAMED ? CM_TOKEN_RBRACE :
        CM_TOKEN_RPAREN;
    while (cm_parser_kind(parser) != closing &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        CmVec ignored_attributes;
        CmAstField field;

        cm_vec_init(&ignored_attributes, sizeof(CmAstAttributeId));
        cm_parser_parse_attributes(parser, &ignored_attributes);
        cm_vec_destroy(&ignored_attributes);
        memset(&field, 0, sizeof(field));
        field.visibility = cm_parser_parse_visibility(parser);
        if (form == CM_AST_FIELDS_NAMED) {
            field.name = cm_parser_parse_name(parser,
                "expected field name");
            (void)cm_parser_expect(parser, CM_TOKEN_COLON,
                "expected ':' after field name");
        }
        field.type = cm_parser_parse_type(parser);
        (void)cm_vec_push(&fields, &field);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) {
            break;
        }
    }
    (void)cm_parser_expect(parser, closing,
        "expected end of field list");
    *count = cm_parser_count_u32(&fields);
    {
        CmAstField *copy;

        copy = (CmAstField *)cm_parser_copy_array(parser, &fields);
        cm_vec_destroy(&fields);
        return copy;
    }
}

static void cm_parser_recover_item(CmParser *parser)
{
    while (cm_parser_kind(parser) != CM_TOKEN_EOF &&
           cm_parser_kind(parser) != CM_TOKEN_RBRACE) {
        if (cm_parser_eat(parser, CM_TOKEN_SEMICOLON)) {
            return;
        }
        if (cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
            (void)cm_parser_skip_balanced(parser, CM_TOKEN_LBRACE,
                CM_TOKEN_RBRACE);
            return;
        }
        cm_parser_bump(parser);
    }
}

static CmAstItemId *cm_parser_parse_item_list(CmParser *parser,
    enum cm_token_kind closing, uint32_t *count, int impl_members)
{
    CmVec items;

    cm_vec_init(&items, sizeof(CmAstItemId));
    while (cm_parser_kind(parser) != closing &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        size_t before;
        CmAstItemId item;

        before = parser->position;
        parser->next_item_is_impl_member = impl_members;
        item = cm_parser_parse_item(parser);
        parser->next_item_is_impl_member = 0;
        if (item != CM_AST_ITEM_NONE) {
            (void)cm_vec_push(&items, &item);
        } else {
            cm_parser_recover_item(parser);
        }
        if (parser->position == before && parser->pending_greater == 0u) {
            cm_parser_bump(parser);
        }
    }
    *count = cm_parser_count_u32(&items);
    {
        CmAstItemId *copy;

        copy = (CmAstItemId *)cm_parser_copy_array(parser, &items);
        cm_vec_destroy(&items);
        return copy;
    }
}

static void cm_parser_init_item(CmAstItem *item, CmItemPrefix *prefix,
    const struct cm_token *first)
{
    memset(item, 0, sizeof(*item));
    item->visibility = prefix->visibility;
    item->attributes = prefix->attributes;
    item->attribute_count = prefix->attribute_count;
    item->is_default = prefix->is_default;
    item->span.start = first == NULL ? 0u : (uint32_t)first->start;
}

static void cm_parser_finish_item(CmParser *parser, CmAstItem *item)
{
    const struct cm_token *last;

    last = cm_parser_previous(parser);
    item->span.end = cm_parser_offset_u32(parser, cm_token_end(last));
}

static CmAstPatternId cm_parser_parse_lifetime_receiver_pattern(
    CmParser *parser, CmInternId *lifetime)
{
    const struct cm_token *first;
    CmAstPattern pattern;

    first = cm_parser_token(parser);
    memset(&pattern, 0, sizeof(pattern));
    (void)cm_parser_expect(parser, CM_TOKEN_AMP,
        "expected '&' before receiver lifetime");
    *lifetime = cm_parser_intern_token(parser, cm_parser_token(parser));
    (void)cm_parser_expect(parser, CM_TOKEN_LIFETIME,
        "expected receiver lifetime after '&'");
    pattern.kind = CM_AST_PATTERN_REFERENCE;
    pattern.data.reference.is_mutable = cm_parser_eat_keyword(parser,
        CM_KW_MUT);
    pattern.data.reference.pattern = cm_parser_parse_pattern_atom(parser);
    pattern.span = cm_parser_span_from(parser, first);
    return cm_ast_add_pattern(parser->ast, &pattern);
}

static CmAstItemId cm_parser_parse_function(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;
    CmVec parameters;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_FUNCTION;
    item.data.function_item.is_const = prefix->is_const;
    item.data.function_item.is_async = prefix->is_async;
    item.data.function_item.is_safe = prefix->is_safe;
    item.data.function_item.is_unsafe = prefix->is_unsafe;
    item.data.function_item.abi = prefix->abi;
    item.name = cm_parser_parse_name(parser, "expected function name");
    cm_parser_parse_generic_parameters(parser, &item);
    if (!cm_parser_expect(parser, CM_TOKEN_LPAREN,
        "expected '(' after function name")) {
        return CM_AST_ITEM_NONE;
    }
    cm_vec_init(&parameters, sizeof(CmAstFunctionParam));
    while (cm_parser_kind(parser) != CM_TOKEN_RPAREN &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        CmAstFunctionParam parameter;

        memset(&parameter, 0, sizeof(parameter));
        if (cm_parser_kind(parser) == CM_TOKEN_AMP
            && cm_parser_next_token(parser) != NULL
            && cm_parser_next_token(parser)->kind == CM_TOKEN_LIFETIME) {
            parameter.pattern = cm_parser_parse_lifetime_receiver_pattern(
                parser, &parameter.receiver_lifetime);
        } else {
            parameter.pattern = cm_parser_parse_pattern(parser);
        }
        parameter.is_self = cm_parser_pattern_is_self(parser,
            parameter.pattern);
        if (parameter.receiver_lifetime != CM_INTERN_ID_NONE
            && !parameter.is_self) {
            cm_parser_error(parser,
                "a lifetime-qualified receiver must target self");
        }
        if (cm_parser_eat(parser, CM_TOKEN_COLON)) {
            parameter.type = cm_parser_parse_type(parser);
        }
        (void)cm_vec_push(&parameters, &parameter);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) {
            break;
        }
    }
    (void)cm_parser_expect(parser, CM_TOKEN_RPAREN,
        "expected ')' after function parameters");
    item.data.function_item.parameters =
        (CmAstFunctionParam *)cm_parser_copy_array(parser, &parameters);
    item.data.function_item.parameter_count = cm_parser_count_u32(&parameters);
    cm_vec_destroy(&parameters);
    if (cm_parser_eat(parser, CM_TOKEN_THIN_ARROW)) {
        item.data.function_item.return_type = cm_parser_parse_type(parser);
    }
    cm_parser_parse_where_clause(parser, &item, CM_TOKEN_LBRACE);
    if (cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
        item.data.function_item.body = cm_parser_parse_block(parser);
    } else {
        (void)cm_parser_expect(parser, CM_TOKEN_SEMICOLON,
            "expected function body or ';'");
    }
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_struct(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_STRUCT;
    item.name = cm_parser_parse_name(parser, "expected struct name");
    cm_parser_parse_generic_parameters(parser, &item);
    cm_parser_parse_where_clause(parser, &item, CM_TOKEN_LBRACE);
    if (cm_parser_eat(parser, CM_TOKEN_LBRACE)) {
        item.data.aggregate_item.form = CM_AST_FIELDS_NAMED;
        item.data.aggregate_item.fields = cm_parser_parse_fields(parser,
            CM_AST_FIELDS_NAMED, &item.data.aggregate_item.field_count);
    } else if (cm_parser_eat(parser, CM_TOKEN_LPAREN)) {
        item.data.aggregate_item.form = CM_AST_FIELDS_TUPLE;
        item.data.aggregate_item.fields = cm_parser_parse_fields(parser,
            CM_AST_FIELDS_TUPLE, &item.data.aggregate_item.field_count);
        cm_parser_parse_where_clause(parser, &item, CM_TOKEN_SEMICOLON);
        (void)cm_parser_expect(parser, CM_TOKEN_SEMICOLON,
            "expected ';' after tuple struct");
    } else {
        item.data.aggregate_item.form = CM_AST_FIELDS_UNIT;
        (void)cm_parser_expect(parser, CM_TOKEN_SEMICOLON,
            "expected struct fields or ';'");
    }
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_union(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_UNION;
    item.name = cm_parser_parse_name(parser, "expected union name");
    cm_parser_parse_generic_parameters(parser, &item);
    cm_parser_parse_where_clause(parser, &item, CM_TOKEN_LBRACE);
    if (!cm_parser_expect(parser, CM_TOKEN_LBRACE,
        "expected '{' after union header")) {
        return CM_AST_ITEM_NONE;
    }
    item.data.aggregate_item.form = CM_AST_FIELDS_NAMED;
    item.data.aggregate_item.fields = cm_parser_parse_fields(parser,
        CM_AST_FIELDS_NAMED, &item.data.aggregate_item.field_count);
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_enum(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;
    CmVec variants;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_ENUM;
    item.name = cm_parser_parse_name(parser, "expected enum name");
    cm_parser_parse_generic_parameters(parser, &item);
    cm_parser_parse_where_clause(parser, &item, CM_TOKEN_LBRACE);
    if (!cm_parser_expect(parser, CM_TOKEN_LBRACE,
        "expected '{' after enum header")) {
        return CM_AST_ITEM_NONE;
    }
    cm_vec_init(&variants, sizeof(CmAstVariant));
    while (cm_parser_kind(parser) != CM_TOKEN_RBRACE &&
           cm_parser_kind(parser) != CM_TOKEN_EOF) {
        const struct cm_token *variant_first;
        CmVec attributes;
        CmAstVariant variant;

        variant_first = cm_parser_token(parser);
        cm_vec_init(&attributes, sizeof(CmAstAttributeId));
        cm_parser_parse_attributes(parser, &attributes);
        memset(&variant, 0, sizeof(variant));
        variant.attributes = (CmAstAttributeId *)cm_parser_copy_array(parser,
            &attributes);
        variant.attribute_count = cm_parser_count_u32(&attributes);
        cm_vec_destroy(&attributes);
        variant.name = cm_parser_parse_name(parser,
            "expected enum variant name");
        if (cm_parser_eat(parser, CM_TOKEN_LPAREN)) {
            variant.form = CM_AST_FIELDS_TUPLE;
            variant.fields = cm_parser_parse_fields(parser,
                CM_AST_FIELDS_TUPLE, &variant.field_count);
        } else if (cm_parser_eat(parser, CM_TOKEN_LBRACE)) {
            variant.form = CM_AST_FIELDS_NAMED;
            variant.fields = cm_parser_parse_fields(parser,
                CM_AST_FIELDS_NAMED, &variant.field_count);
        }
        if (cm_parser_eat(parser, CM_TOKEN_EQ)) {
            variant.discriminant = cm_parser_capture_until(parser,
                CM_TOKEN_COMMA, CM_TOKEN_RBRACE, CM_TOKEN_EOF);
        }
        variant.span = cm_parser_span_from(parser, variant_first);
        (void)cm_vec_push(&variants, &variant);
        if (!cm_parser_eat(parser, CM_TOKEN_COMMA)) {
            break;
        }
    }
    (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
        "expected '}' after enum variants");
    item.data.enum_item.variants = (CmAstVariant *)cm_parser_copy_array(
        parser, &variants);
    item.data.enum_item.variant_count = cm_parser_count_u32(&variants);
    cm_vec_destroy(&variants);
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_value_item(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first, CmAstItemKind kind)
{
    CmAstItem item;

    cm_parser_init_item(&item, prefix, first);
    item.kind = kind;
    if (kind == CM_AST_ITEM_STATIC) {
        item.data.value_item.is_mutable =
            cm_parser_eat_keyword(parser, CM_KW_MUT);
    }
    item.name = cm_parser_parse_name(parser, "expected item name");
    if (kind == CM_AST_ITEM_TYPE_ALIAS) {
        cm_parser_parse_generic_parameters(parser, &item);
        if (cm_parser_eat(parser, CM_TOKEN_COLON)) {
            cm_parser_parse_associated_type_bounds(parser, &item);
        }
        cm_parser_parse_where_clause(parser, &item, CM_TOKEN_EQ);
        if (cm_parser_eat(parser, CM_TOKEN_EQ)) {
            item.data.value_item.type = cm_parser_parse_type(parser);
            item.data.value_item.has_value = 1;
            cm_parser_parse_post_value_where_clause(parser, &item);
        }
    } else {
        (void)cm_parser_expect(parser, CM_TOKEN_COLON,
            "expected ':' after value name");
        item.data.value_item.type = cm_parser_parse_type(parser);
        if (cm_parser_eat(parser, CM_TOKEN_EQ)) {
            item.data.value_item.has_value = 1;
            item.data.value_item.initializer =
                cm_parser_parse_expression(parser);
        }
    }
    (void)cm_parser_expect(parser, CM_TOKEN_SEMICOLON,
        "expected ';' after value item");
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_module(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;
    CmVec inner_attributes;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_MODULE;
    item.name = cm_parser_parse_name(parser, "expected module name");
    if (cm_parser_eat(parser, CM_TOKEN_SEMICOLON)) {
        item.data.module_item.is_inline = 0;
    } else if (cm_parser_eat(parser, CM_TOKEN_LBRACE)) {
        item.data.module_item.is_inline = 1;
        cm_vec_init(&inner_attributes, sizeof(CmAstAttributeId));
        cm_parser_parse_inner_attributes(parser, &inner_attributes);
        item.data.module_item.inner_attributes =
            (CmAstAttributeId *)cm_parser_copy_array(parser,
                &inner_attributes);
        item.data.module_item.inner_attribute_count =
            cm_parser_count_u32(&inner_attributes);
        cm_vec_destroy(&inner_attributes);
        item.data.module_item.items = cm_parser_parse_item_list(parser,
            CM_TOKEN_RBRACE, &item.data.module_item.item_count, 0);
        (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
            "expected '}' after module");
    } else {
        cm_parser_error(parser, "expected ';' or module body");
    }
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_use(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_USE;
    item.data.use_item.tree = cm_parser_capture_until(parser,
        CM_TOKEN_SEMICOLON, CM_TOKEN_EOF, CM_TOKEN_EOF);
    (void)cm_parser_expect(parser, CM_TOKEN_SEMICOLON,
        "expected ';' after use tree");
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_extern_crate(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_EXTERN_CRATE;
    item.name = cm_parser_parse_name(parser, "expected crate name");
    if (cm_parser_eat_keyword(parser, CM_KW_AS)) {
        item.data.extern_crate_item.alias = cm_parser_parse_name(parser,
            "expected crate alias");
    }
    (void)cm_parser_expect(parser, CM_TOKEN_SEMICOLON,
        "expected ';' after extern crate");
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_extern_block(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_EXTERN_BLOCK;
    item.data.extern_block_item.abi = prefix->abi;
    item.data.extern_block_item.is_unsafe = prefix->is_unsafe;
    if (!cm_parser_expect(parser, CM_TOKEN_LBRACE,
        "expected extern block")) {
        return CM_AST_ITEM_NONE;
    }
    parser->extern_block_depth += 1u;
    item.data.extern_block_item.items = cm_parser_parse_item_list(parser,
        CM_TOKEN_RBRACE, &item.data.extern_block_item.item_count, 0);
    parser->extern_block_depth -= 1u;
    (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
        "expected '}' after extern block");
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_trait(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_TRAIT;
    item.data.trait_item.is_unsafe = prefix->is_unsafe;
    item.data.trait_item.is_auto = prefix->is_auto;
    item.name = cm_parser_parse_name(parser, "expected trait name");
    cm_parser_parse_generic_parameters(parser, &item);
    if (cm_parser_eat(parser, CM_TOKEN_COLON)) {
        cm_parser_parse_trait_bounds(parser,
            &item.data.trait_item.supertraits,
            &item.data.trait_item.structured_supertraits,
            &item.data.trait_item.structured_supertrait_count);
    }
    cm_parser_parse_where_clause(parser, &item, CM_TOKEN_LBRACE);
    if (cm_parser_eat(parser, CM_TOKEN_EQ)) {
        item.data.trait_item.is_alias = 1;
        if (item.data.trait_item.is_unsafe) {
            cm_parser_error(parser, "trait aliases cannot be unsafe");
        }
        cm_parser_parse_trait_bounds(parser,
            &item.data.trait_item.alias_bounds,
            &item.data.trait_item.structured_alias_bounds,
            &item.data.trait_item.structured_alias_bound_count);
        (void)cm_parser_expect(parser, CM_TOKEN_SEMICOLON,
            "expected ';' after trait alias");
        cm_parser_finish_item(parser, &item);
        return cm_ast_add_item(parser->ast, &item);
    }
    if (!cm_parser_expect(parser, CM_TOKEN_LBRACE,
        "expected trait body")) {
        return CM_AST_ITEM_NONE;
    }
    item.data.trait_item.items = cm_parser_parse_item_list(parser,
        CM_TOKEN_RBRACE, &item.data.trait_item.item_count, 0);
    (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
        "expected '}' after trait");
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_impl(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;
    CmAstTypeId first_type;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_IMPL;
    item.data.impl_item.is_unsafe = prefix->is_unsafe;
    cm_parser_parse_generic_parameters(parser, &item);
    if (cm_parser_eat_keyword(parser, CM_KW_CONST)) {
        item.data.impl_item.is_const = 1;
    }
    if (cm_parser_eat(parser, CM_TOKEN_BANG)) {
        const struct cm_token *bang;

        bang = cm_parser_previous(parser);
        if (cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
            first_type = cm_parser_add_simple_type(parser,
                CM_AST_TYPE_NEVER, bang->start, cm_token_end(bang));
        } else {
            item.data.impl_item.is_negative = 1;
            first_type = cm_parser_parse_type(parser);
        }
    } else {
        first_type = cm_parser_parse_type(parser);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_FOR)) {
        item.data.impl_item.trait_type = first_type;
        item.data.impl_item.self_type = cm_parser_parse_type(parser);
    } else {
        item.data.impl_item.self_type = first_type;
    }
    cm_parser_parse_where_clause(parser, &item, CM_TOKEN_LBRACE);
    if (!cm_parser_expect(parser, CM_TOKEN_LBRACE,
        "expected impl body")) {
        return CM_AST_ITEM_NONE;
    }
    item.data.impl_item.items = cm_parser_parse_item_list(parser,
        CM_TOKEN_RBRACE, &item.data.impl_item.item_count, 1);
    (void)cm_parser_expect(parser, CM_TOKEN_RBRACE,
        "expected '}' after impl");
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static int cm_parser_path_is(const CmParser *parser, CmAstPathId path_id,
    const char *name)
{
    const CmAstPath *path;
    const CmInternedString *segment;
    size_t length;

    path = cm_ast_get_path(parser->ast, path_id);
    if (path == NULL || path->absolute || path->segment_count != 1u) {
        return 0;
    }
    segment = cm_ast_get_string(parser->ast, path->segments[0].name);
    length = strlen(name);
    return segment != NULL && segment->len == length &&
        memcmp(segment->bytes, name, length) == 0;
}

static CmAstItemId cm_parser_parse_macro_item(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_MACRO;
    item.data.macro_item.path = cm_parser_parse_expression_path(parser,
        NULL);
    if (!cm_parser_expect(parser, CM_TOKEN_BANG,
        "expected '!' after macro path")) {
        return CM_AST_ITEM_NONE;
    }
    if (cm_parser_path_is(parser, item.data.macro_item.path,
        "macro_rules")) {
        item.data.macro_item.form = CM_AST_MACRO_RULES_DEFINITION;
        item.name = cm_parser_parse_name(parser,
            "expected macro_rules name");
    }
    if (!cm_parser_parse_macro_arguments(parser, &item.data.macro_item, 1)) {
        return CM_AST_ITEM_NONE;
    }
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmAstItemId cm_parser_parse_declarative_macro(CmParser *parser,
    CmItemPrefix *prefix, const struct cm_token *first)
{
    CmAstItem item;
    CmAstMacroInvocation parameters;

    cm_parser_init_item(&item, prefix, first);
    item.kind = CM_AST_ITEM_MACRO;
    item.data.macro_item.form = CM_AST_MACRO_DECLARATIVE_DEFINITION;
    item.name = cm_parser_parse_name(parser,
        "expected declarative macro name");
    if (item.name == CM_INTERN_ID_NONE) {
        return CM_AST_ITEM_NONE;
    }
    if (cm_parser_kind(parser) == CM_TOKEN_LPAREN) {
        memset(&parameters, 0, sizeof(parameters));
        if (!cm_parser_parse_macro_arguments(parser, &parameters, 0)) {
            return CM_AST_ITEM_NONE;
        }
        item.data.macro_item.parameters = parameters.arguments;
    }
    if (cm_parser_kind(parser) != CM_TOKEN_LBRACE) {
        cm_parser_error(parser, "expected declarative macro body");
        return CM_AST_ITEM_NONE;
    }
    if (!cm_parser_parse_macro_arguments(parser, &item.data.macro_item, 0)) {
        return CM_AST_ITEM_NONE;
    }
    cm_parser_finish_item(parser, &item);
    return cm_ast_add_item(parser->ast, &item);
}

static CmInternId cm_parser_parse_abi(CmParser *parser)
{
    const struct cm_token *token;

    token = cm_parser_token(parser);
    if (token != NULL && token->kind == CM_TOKEN_STRING) {
        CmInternId abi;

        if (token->length >= 2u) {
            abi = cm_interner_intern(&parser->ast->strings,
                parser->source + token->start + 1u, token->length - 2u);
        } else {
            abi = cm_parser_intern_token(parser, token);
        }
        cm_parser_bump(parser);
        return abi;
    }
    return cm_interner_intern_c_str(&parser->ast->strings, "C");
}

static int cm_parser_const_starts_function(const CmParser *parser)
{
    size_t position;
    int saw_extern;

    position = parser->position + 1u;
    saw_extern = 0;
    while (position < parser->tokens.len) {
        const struct cm_token *token;

        token = (const struct cm_token *)cm_vec_at_const(&parser->tokens,
            position);
        if (token == NULL) return 0;
        if (token->keyword == CM_KW_FN) return 1;
        if (!saw_extern && (token->keyword == CM_KW_UNSAFE
                || token->keyword == CM_KW_ASYNC)) {
            position += 1u;
            continue;
        }
        if (!saw_extern && token->keyword == CM_KW_EXTERN) {
            saw_extern = 1;
            position += 1u;
            if (position < parser->tokens.len) {
                token = (const struct cm_token *)cm_vec_at_const(
                    &parser->tokens, position);
                if (token != NULL && token->kind == CM_TOKEN_STRING)
                    position += 1u;
            }
            continue;
        }
        return 0;
    }
    return 0;
}

static CmAstItemId cm_parser_parse_item(CmParser *parser)
{
    CmVec attributes;
    CmItemPrefix prefix;
    const struct cm_token *first;
    int saw_extern;
    int is_impl_member;

    is_impl_member = parser->next_item_is_impl_member;
    parser->next_item_is_impl_member = 0;
    cm_vec_init(&attributes, sizeof(CmAstAttributeId));
    cm_parser_parse_attributes(parser, &attributes);
    if (cm_parser_kind(parser) == CM_TOKEN_EOF ||
        cm_parser_kind(parser) == CM_TOKEN_RBRACE) {
        cm_vec_destroy(&attributes);
        return CM_AST_ITEM_NONE;
    }
    memset(&prefix, 0, sizeof(prefix));
    first = cm_parser_token(parser);
    prefix.visibility = cm_parser_parse_visibility(parser);
    prefix.attributes = (CmAstAttributeId *)cm_parser_copy_array(parser,
        &attributes);
    prefix.attribute_count = cm_parser_count_u32(&attributes);
    cm_vec_destroy(&attributes);

    saw_extern = 0;
    for (;;) {
        if (cm_parser_eat_keyword(parser, CM_KW_UNSAFE)) {
            prefix.is_unsafe = 1;
        } else if (cm_parser_keyword(parser, CM_KW_SAFE)
            && (parser->extern_block_depth != 0u
                || (cm_parser_next_token(parser) != NULL
                    && cm_parser_next_token(parser)->keyword
                        == CM_KW_FN))) {
            if (prefix.is_safe) {
                cm_parser_error(parser,
                    "duplicate safe function modifier");
                return CM_AST_ITEM_NONE;
            }
            prefix.is_safe = 1;
            cm_parser_bump(parser);
        } else if (cm_parser_eat_keyword(parser, CM_KW_ASYNC)) {
            prefix.is_async = 1;
        } else if (cm_parser_keyword(parser, CM_KW_CONST)) {
            if (cm_parser_const_starts_function(parser)) {
                prefix.is_const = 1;
                cm_parser_bump(parser);
            } else {
                break;
            }
        } else if (cm_parser_eat_keyword(parser, CM_KW_EXTERN)) {
            saw_extern = 1;
            prefix.abi = cm_parser_parse_abi(parser);
            if (cm_parser_keyword(parser, CM_KW_CRATE) ||
                cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
                break;
            }
        } else if (cm_parser_default_starts_item(parser)) {
            if (prefix.is_default) {
                cm_parser_error(parser,
                    "duplicate default specialization modifier");
                return CM_AST_ITEM_NONE;
            }
            prefix.is_default = 1;
            cm_parser_bump(parser);
        } else if (cm_parser_auto_starts_trait(parser)) {
            prefix.is_auto = 1;
            cm_parser_bump(parser);
        } else {
            break;
        }
    }

    if (prefix.is_safe
        && (parser->extern_block_depth == 0u || prefix.is_unsafe
            || prefix.is_const || prefix.is_async || saw_extern
            || !cm_parser_keyword(parser, CM_KW_FN))) {
        cm_parser_error(parser,
            "safe modifier is only permitted by itself on a foreign "
            "function");
        return CM_AST_ITEM_NONE;
    }
    if (prefix.is_default
        && !((cm_parser_keyword(parser, CM_KW_IMPL) && !is_impl_member)
            || (is_impl_member
                && (cm_parser_keyword(parser, CM_KW_FN)
                    || cm_parser_keyword(parser, CM_KW_TYPE)
                    || cm_parser_keyword(parser, CM_KW_CONST))))) {
        cm_parser_error(parser,
            "default specialization is only permitted on an impl or an "
            "impl item");
        return CM_AST_ITEM_NONE;
    }

    if (saw_extern && cm_parser_eat_keyword(parser, CM_KW_CRATE)) {
        return cm_parser_parse_extern_crate(parser, &prefix, first);
    }
    if (saw_extern && cm_parser_kind(parser) == CM_TOKEN_LBRACE) {
        return cm_parser_parse_extern_block(parser, &prefix, first);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_FN)) {
        return cm_parser_parse_function(parser, &prefix, first);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_STRUCT)) {
        return cm_parser_parse_struct(parser, &prefix, first);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_UNION)) {
        return cm_parser_parse_union(parser, &prefix, first);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_ENUM)) {
        return cm_parser_parse_enum(parser, &prefix, first);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_TYPE)) {
        return cm_parser_parse_value_item(parser, &prefix, first,
            CM_AST_ITEM_TYPE_ALIAS);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_CONST)) {
        return cm_parser_parse_value_item(parser, &prefix, first,
            CM_AST_ITEM_CONST);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_STATIC)) {
        return cm_parser_parse_value_item(parser, &prefix, first,
            CM_AST_ITEM_STATIC);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_MOD)) {
        return cm_parser_parse_module(parser, &prefix, first);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_USE)) {
        return cm_parser_parse_use(parser, &prefix, first);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_TRAIT)) {
        return cm_parser_parse_trait(parser, &prefix, first);
    }
    if (cm_parser_keyword(parser, CM_KW_IMPL)
        && (prefix.is_async || saw_extern)) {
        cm_parser_error(parser,
            "async/extern modifiers are not permitted on impl blocks");
        return CM_AST_ITEM_NONE;
    }
    if (cm_parser_eat_keyword(parser, CM_KW_IMPL)) {
        return cm_parser_parse_impl(parser, &prefix, first);
    }
    if (cm_parser_eat_keyword(parser, CM_KW_MACRO)) {
        return cm_parser_parse_declarative_macro(parser, &prefix, first);
    }
    if (cm_parser_is_name(parser) ||
        cm_parser_kind(parser) == CM_TOKEN_PATH_SEP) {
        return cm_parser_parse_macro_item(parser, &prefix, first);
    }
    cm_parser_error(parser, "expected top-level item");
    return CM_AST_ITEM_NONE;
}

static void cm_parser_direct_error(CmParser *parser, const char *message)
{
    parser->result.error_count += 1u;
    if (parser->result.error_count != 1u) {
        return;
    }
    parser->result.first_error.offset = 0u;
    parser->result.first_error.line = 1u;
    parser->result.first_error.column = 1u;
    (void)snprintf(parser->result.first_error.message,
        sizeof(parser->result.first_error.message), "%s", message);
}

static int cm_parser_begin(CmParser *parser, CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition)
{
    struct cm_lexer_options options;
    struct cm_lexer_result lex_result;

    memset(parser, 0, sizeof(*parser));
    parser->ast = ast;
    parser->source = source == NULL ? "" : source;
    parser->source_length = source_length;
    cm_vec_init(&parser->tokens, sizeof(struct cm_token));
    if (ast == NULL) {
        cm_parser_direct_error(parser, "parser requires an AST");
        return 0;
    }
    if (source == NULL && source_length != 0u) {
        cm_parser_direct_error(parser,
            "nonempty parser input requires source bytes");
        return 0;
    }
    if (source_length > (size_t)UINT32_MAX) {
        cm_parser_direct_error(parser,
            "source is larger than the AST format");
        return 0;
    }
    cm_lexer_options_init(&options);
    options.edition = edition;
    lex_result = cm_lex(parser->source, source_length, &options,
        cm_parser_collect_token, parser);
    if (lex_result.error_count != 0u) {
        cm_parser_error(parser, "lexical error");
    }
    return 1;
}

static void cm_parser_end(CmParser *parser)
{
    cm_vec_destroy(&parser->tokens);
}

CmParseResult cm_parse_crate(CmAst *ast, const char *source,
    size_t source_length, enum cm_edition edition)
{
    CmParser parser;
    CmAstItemId *items;
    uint32_t item_count;
    uint32_t index;

    if (!cm_parser_begin(&parser, ast, source, source_length, edition)) {
        cm_parser_end(&parser);
        return parser.result;
    }
    cm_parser_parse_inner_attributes(&parser, &ast->crate_attributes);
    items = cm_parser_parse_item_list(&parser, CM_TOKEN_EOF, &item_count, 0);
    for (index = 0u; index < item_count; ++index) {
        (void)cm_vec_push(&ast->root_items, &items[index]);
    }
    cm_parser_end(&parser);
    return parser.result;
}

CmExpressionFragment cm_parse_expression_fragment(CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition)
{
    CmParser parser;
    CmExpressionFragment fragment;
    CmAstExprId expression;

    memset(&fragment, 0, sizeof(fragment));
    if (!cm_parser_begin(&parser, ast, source, source_length, edition)) {
        fragment.parse = parser.result;
        cm_parser_end(&parser);
        return fragment;
    }
    expression = CM_AST_EXPR_NONE;
    if (cm_parser_kind(&parser) == CM_TOKEN_EOF) {
        cm_parser_error(&parser, "expected expression fragment");
    } else {
        expression = cm_parser_parse_expression(&parser);
        if (cm_parser_kind(&parser) != CM_TOKEN_EOF) {
            cm_parser_error(&parser,
                "unexpected trailing input after expression fragment");
        }
    }
    fragment.parse = parser.result;
    if (fragment.parse.error_count == 0u) {
        fragment.expression = expression;
    }
    cm_parser_end(&parser);
    return fragment;
}

CmTypeFragment cm_parse_type_fragment(CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition)
{
    CmParser parser;
    CmTypeFragment fragment;
    CmAstTypeId type;

    memset(&fragment, 0, sizeof(fragment));
    if (!cm_parser_begin(&parser, ast, source, source_length, edition)) {
        fragment.parse = parser.result;
        cm_parser_end(&parser);
        return fragment;
    }
    type = CM_AST_TYPE_NONE;
    if (cm_parser_kind(&parser) == CM_TOKEN_EOF) {
        cm_parser_error(&parser, "expected type fragment");
    } else {
        type = cm_parser_parse_type(&parser);
        if (cm_parser_kind(&parser) != CM_TOKEN_EOF) {
            cm_parser_error(&parser,
                "unexpected trailing input after type fragment");
        }
    }
    fragment.parse = parser.result;
    if (fragment.parse.error_count == 0u) fragment.type = type;
    cm_parser_end(&parser);
    return fragment;
}

CmItemListFragment cm_parse_item_list_fragment_in_context(CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition,
    CmItemListFragmentContext context)
{
    CmParser parser;
    CmItemListFragment fragment;
    CmAstItemId *items;
    uint32_t item_count;

    memset(&fragment, 0, sizeof(fragment));
    if (!cm_parser_begin(&parser, ast, source, source_length, edition)) {
        fragment.parse = parser.result;
        cm_parser_end(&parser);
        return fragment;
    }
    if (context != CM_ITEM_LIST_FRAGMENT_ROOT
        && context != CM_ITEM_LIST_FRAGMENT_IMPL) {
        cm_parser_direct_error(&parser,
            "invalid item-list fragment context");
        fragment.parse = parser.result;
        cm_parser_end(&parser);
        return fragment;
    }
    items = cm_parser_parse_item_list(&parser, CM_TOKEN_EOF, &item_count,
        context == CM_ITEM_LIST_FRAGMENT_IMPL);
    if (cm_parser_kind(&parser) != CM_TOKEN_EOF) {
        cm_parser_error(&parser,
            "unexpected trailing input after item-list fragment");
    }
    fragment.parse = parser.result;
    if (fragment.parse.error_count == 0u) {
        fragment.items = items;
        fragment.item_count = item_count;
    }
    cm_parser_end(&parser);
    return fragment;
}

CmItemListFragment cm_parse_item_list_fragment(CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition)
{
    return cm_parse_item_list_fragment_in_context(ast, source,
        source_length, edition, CM_ITEM_LIST_FRAGMENT_ROOT);
}
