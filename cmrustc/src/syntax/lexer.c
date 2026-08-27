#include "cm/syntax/lexer.h"

#include <stdint.h>

struct cm_lexer_state {
    const char *source;
    size_t length;
    size_t pos;
    size_t line;
    size_t column;
    struct cm_lexer_options options;
    cm_token_sink_fn sink;
    void *user;
    enum cm_token_kind previous_significant_kind;
    struct cm_lexer_result result;
};

static int cm_is_ascii_alpha(unsigned char c)
{
    return (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
        (c >= (unsigned char)'A' && c <= (unsigned char)'Z');
}

static int cm_is_ident_start(unsigned char c)
{
    /* Non-ASCII bytes are retained as identifier bytes. XID validation is a
     * later, replaceable Unicode-policy pass and does not belong in TCC. */
    return cm_is_ascii_alpha(c) || c == (unsigned char)'_' || c >= 0x80u;
}

static int cm_is_ident_continue(unsigned char c)
{
    return cm_is_ident_start(c) ||
        (c >= (unsigned char)'0' && c <= (unsigned char)'9');
}

static int cm_is_dec_digit(unsigned char c)
{
    return c >= (unsigned char)'0' && c <= (unsigned char)'9';
}

static int cm_is_digit_for_base(unsigned char c, unsigned base)
{
    if (c >= (unsigned char)'0' && c <= (unsigned char)'9')
        return (unsigned)(c - (unsigned char)'0') < base;
    if (c >= (unsigned char)'a' && c <= (unsigned char)'f')
        return base == 16u;
    if (c >= (unsigned char)'A' && c <= (unsigned char)'F')
        return base == 16u;
    return 0;
}

static unsigned char cm_peek(const struct cm_lexer_state *state, size_t ahead)
{
    size_t at;

    at = state->pos + ahead;
    if (at >= state->length || at < state->pos)
        return 0;
    return (unsigned char)state->source[at];
}

static void cm_advance(struct cm_lexer_state *state)
{
    unsigned char c;

    if (state->pos >= state->length)
        return;
    c = (unsigned char)state->source[state->pos++];
    if (c == (unsigned char)'\n') {
        state->line++;
        state->column = 1;
    } else {
        state->column++;
    }
}

static int cm_emit(struct cm_lexer_state *state, enum cm_token_kind kind,
    size_t start, size_t line, size_t column, size_t suffix_start,
    uint32_t flags, uint32_t detail)
{
    struct cm_token token;
    int stop;

    token.kind = kind;
    token.keyword = CM_KW_NONE;
    token.start = start;
    token.length = state->pos - start;
    token.line = line;
    token.column = column;
    token.suffix_start = suffix_start;
    token.flags = flags;
    token.detail = detail;

    if (kind == CM_TOKEN_IDENT) {
        token.keyword = cm_keyword_classify(state->source + start,
            token.length, state->options.edition, &token.flags);
    }

    state->result.token_count++;
    if ((flags & (CM_TOKEN_F_INVALID | CM_TOKEN_F_UNTERMINATED)) != 0u ||
        kind == CM_TOKEN_ERROR)
        state->result.error_count++;
    if (kind != CM_TOKEN_WHITESPACE && kind != CM_TOKEN_LINE_COMMENT
        && kind != CM_TOKEN_BLOCK_COMMENT) {
        state->previous_significant_kind = kind;
    }

    stop = 0;
    if (state->sink != NULL)
        stop = state->sink(state->user, &token);
    if (stop != 0)
        state->result.stopped = 1;
    return stop;
}

static void cm_skip_ident(struct cm_lexer_state *state)
{
    while (state->pos < state->length &&
        cm_is_ident_continue(cm_peek(state, 0)))
        cm_advance(state);
}

static int cm_scan_quoted(struct cm_lexer_state *state,
    enum cm_token_kind kind, size_t prefix_length, int is_char)
{
    size_t start;
    size_t line;
    size_t column;
    uint32_t flags;
    size_t i;

    start = state->pos;
    line = state->line;
    column = state->column;
    flags = 0;
    for (i = 0; i < prefix_length; i++)
        cm_advance(state);

    /* prefix_length includes the opening quote. */
    while (state->pos < state->length) {
        unsigned char c;

        c = cm_peek(state, 0);
        if (c == (unsigned char)'\\') {
            cm_advance(state);
            if (state->pos < state->length)
                cm_advance(state);
            else
                flags |= CM_TOKEN_F_UNTERMINATED;
            continue;
        }
        if ((!is_char && c == (unsigned char)'"') ||
            (is_char && c == (unsigned char)'\'')) {
            cm_advance(state);
            return cm_emit(state, kind, start, line, column, SIZE_MAX,
                flags, 0);
        }
        if (is_char && c == (unsigned char)'\n') {
            flags |= CM_TOKEN_F_UNTERMINATED;
            break;
        }
        cm_advance(state);
    }
    flags |= CM_TOKEN_F_UNTERMINATED;
    return cm_emit(state, kind, start, line, column, SIZE_MAX, flags, 0);
}

/* Returns nonzero and fills quote_ahead/hash_count for r###" prefixes. */
static int cm_raw_prefix(const struct cm_lexer_state *state,
    size_t prefix_letters, size_t *quote_ahead, uint32_t *hash_count)
{
    size_t i;
    uint32_t hashes;

    i = prefix_letters;
    hashes = 0;
    while (cm_peek(state, i) == (unsigned char)'#') {
        if (hashes == UINT32_MAX)
            return 0;
        hashes++;
        i++;
    }
    if (cm_peek(state, i) != (unsigned char)'"')
        return 0;
    *quote_ahead = i;
    *hash_count = hashes;
    return 1;
}

static int cm_scan_raw_string(struct cm_lexer_state *state,
    enum cm_token_kind kind, size_t quote_ahead, uint32_t hash_count)
{
    size_t start;
    size_t line;
    size_t column;
    size_t i;
    uint32_t flags;

    start = state->pos;
    line = state->line;
    column = state->column;
    flags = 0;
    for (i = 0; i <= quote_ahead; i++)
        cm_advance(state);

    while (state->pos < state->length) {
        if (cm_peek(state, 0) == (unsigned char)'"') {
            uint32_t h;
            int matches;

            matches = 1;
            for (h = 0; h < hash_count; h++) {
                if (cm_peek(state, (size_t)h + 1u) !=
                    (unsigned char)'#') {
                    matches = 0;
                    break;
                }
            }
            if (matches) {
                cm_advance(state);
                for (h = 0; h < hash_count; h++)
                    cm_advance(state);
                return cm_emit(state, kind, start, line, column,
                    SIZE_MAX, flags, hash_count);
            }
        }
        cm_advance(state);
    }

    flags |= CM_TOKEN_F_UNTERMINATED;
    return cm_emit(state, kind, start, line, column, SIZE_MAX, flags,
        hash_count);
}

static int cm_scan_number(struct cm_lexer_state *state)
{
    size_t start;
    size_t line;
    size_t column;
    size_t suffix_start;
    unsigned base;
    int is_float;
    int saw_digit;
    uint32_t flags;

    start = state->pos;
    line = state->line;
    column = state->column;
    suffix_start = SIZE_MAX;
    base = 10u;
    is_float = 0;
    saw_digit = 0;
    flags = 0;

    if (cm_peek(state, 0) == (unsigned char)'0') {
        unsigned char marker;

        marker = cm_peek(state, 1);
        if (marker == (unsigned char)'b' || marker == (unsigned char)'o' ||
            marker == (unsigned char)'x') {
            base = marker == (unsigned char)'b' ? 2u :
                (marker == (unsigned char)'o' ? 8u : 16u);
            cm_advance(state);
            cm_advance(state);
        }
    }

    while (state->pos < state->length) {
        unsigned char c;

        c = cm_peek(state, 0);
        if (cm_is_digit_for_base(c, base)) {
            saw_digit = 1;
            cm_advance(state);
        } else if (c == (unsigned char)'_') {
            cm_advance(state);
        } else {
            break;
        }
    }
    if (!saw_digit)
        flags |= CM_TOKEN_F_INVALID;

    if (base == 10u
        && state->previous_significant_kind != CM_TOKEN_DOT
        && cm_peek(state, 0) == (unsigned char)'.' &&
        cm_peek(state, 1) != (unsigned char)'.'
        /* `999.try_into()` is an integer and a method call, not a
         * float followed by garbage. */
        && !cm_is_ident_start(cm_peek(state, 1))) {
        is_float = 1;
        cm_advance(state);
        while (cm_is_dec_digit(cm_peek(state, 0)) ||
            cm_peek(state, 0) == (unsigned char)'_')
            cm_advance(state);
    }

    if (base == 10u && (cm_peek(state, 0) == (unsigned char)'e' ||
        cm_peek(state, 0) == (unsigned char)'E')) {
        size_t exponent_start;
        int exponent_digit;

        is_float = 1;
        exponent_start = state->pos;
        exponent_digit = 0;
        cm_advance(state);
        if (cm_peek(state, 0) == (unsigned char)'+' ||
            cm_peek(state, 0) == (unsigned char)'-')
            cm_advance(state);
        while (cm_is_dec_digit(cm_peek(state, 0)) ||
            cm_peek(state, 0) == (unsigned char)'_') {
            if (cm_is_dec_digit(cm_peek(state, 0)))
                exponent_digit = 1;
            cm_advance(state);
        }
        if (!exponent_digit && state->pos > exponent_start)
            flags |= CM_TOKEN_F_INVALID;
    }

    if (cm_is_ident_start(cm_peek(state, 0))) {
        suffix_start = state->pos;
        cm_skip_ident(state);
    }

    return cm_emit(state, is_float ? CM_TOKEN_FLOAT : CM_TOKEN_INTEGER,
        start, line, column, suffix_start, flags, (uint32_t)base);
}

static int cm_scan_comment(struct cm_lexer_state *state)
{
    size_t start;
    size_t line;
    size_t column;
    uint32_t flags;

    start = state->pos;
    line = state->line;
    column = state->column;
    flags = 0;

    if (cm_peek(state, 1) == (unsigned char)'/') {
        if (cm_peek(state, 2) == (unsigned char)'!' )
            flags |= CM_TOKEN_F_INNER_DOC;
        else if (cm_peek(state, 2) == (unsigned char)'/' &&
            cm_peek(state, 3) != (unsigned char)'/')
            flags |= CM_TOKEN_F_OUTER_DOC;
        cm_advance(state);
        cm_advance(state);
        while (state->pos < state->length &&
            cm_peek(state, 0) != (unsigned char)'\n')
            cm_advance(state);
        if (!state->options.emit_comments)
            return 0;
        return cm_emit(state, CM_TOKEN_LINE_COMMENT, start, line, column,
            SIZE_MAX, flags, 0);
    } else {
        unsigned depth;

        depth = 1u;
        if (cm_peek(state, 2) == (unsigned char)'!')
            flags |= CM_TOKEN_F_INNER_DOC;
        else if (cm_peek(state, 2) == (unsigned char)'*' &&
            cm_peek(state, 3) != (unsigned char)'*' &&
            cm_peek(state, 3) != (unsigned char)'/')
            flags |= CM_TOKEN_F_OUTER_DOC;
        cm_advance(state);
        cm_advance(state);
        while (state->pos < state->length && depth != 0u) {
            if (cm_peek(state, 0) == (unsigned char)'/' &&
                cm_peek(state, 1) == (unsigned char)'*') {
                if (depth != UINT32_MAX)
                    depth++;
                else
                    flags |= CM_TOKEN_F_INVALID;
                cm_advance(state);
                cm_advance(state);
            } else if (cm_peek(state, 0) == (unsigned char)'*' &&
                cm_peek(state, 1) == (unsigned char)'/') {
                depth--;
                cm_advance(state);
                cm_advance(state);
            } else {
                cm_advance(state);
            }
        }
        if (depth != 0u)
            flags |= CM_TOKEN_F_UNTERMINATED;
        if (!state->options.emit_comments) {
            if ((flags & (CM_TOKEN_F_INVALID |
                CM_TOKEN_F_UNTERMINATED)) != 0u)
                state->result.error_count++;
            return 0;
        }
        return cm_emit(state, CM_TOKEN_BLOCK_COMMENT, start, line, column,
            SIZE_MAX, flags, depth);
    }
}

static size_t cm_utf8_scalar_length(const struct cm_lexer_state *state)
{
    unsigned char first;
    unsigned char second;
    unsigned char third;
    unsigned char fourth;

    first = cm_peek(state, 1);
    second = cm_peek(state, 2);
    third = cm_peek(state, 3);
    fourth = cm_peek(state, 4);
    if (first >= 0xc2u && first <= 0xdfu
        && second >= 0x80u && second <= 0xbfu) {
        return 2u;
    }
    if (first == 0xe0u && second >= 0xa0u && second <= 0xbfu
        && third >= 0x80u && third <= 0xbfu) {
        return 3u;
    }
    if (((first >= 0xe1u && first <= 0xecu)
            || (first >= 0xeeu && first <= 0xefu))
        && second >= 0x80u && second <= 0xbfu
        && third >= 0x80u && third <= 0xbfu) {
        return 3u;
    }
    if (first == 0xedu && second >= 0x80u && second <= 0x9fu
        && third >= 0x80u && third <= 0xbfu) {
        return 3u;
    }
    if (first == 0xf0u && second >= 0x90u && second <= 0xbfu
        && third >= 0x80u && third <= 0xbfu
        && fourth >= 0x80u && fourth <= 0xbfu) {
        return 4u;
    }
    if (first >= 0xf1u && first <= 0xf3u
        && second >= 0x80u && second <= 0xbfu
        && third >= 0x80u && third <= 0xbfu
        && fourth >= 0x80u && fourth <= 0xbfu) {
        return 4u;
    }
    if (first == 0xf4u && second >= 0x80u && second <= 0x8fu
        && third >= 0x80u && third <= 0xbfu
        && fourth >= 0x80u && fourth <= 0xbfu) {
        return 4u;
    }
    return 0u;
}

static int cm_has_char_closer(const struct cm_lexer_state *state)
{
    size_t i;
    size_t scalar_length;
    unsigned char first;

    first = cm_peek(state, 1);
    if (first == 0 || first == (unsigned char)'\n')
        return 0;
    if (first == (unsigned char)'\\')
        return 1;
    if (cm_is_ascii_alpha(first) || first == (unsigned char)'_')
        return cm_peek(state, 2) == (unsigned char)'\'';
    if (first >= 0x80u) {
        scalar_length = cm_utf8_scalar_length(state);
        return scalar_length != 0u
            && cm_peek(state, scalar_length + 1u) == (unsigned char)'\'';
    }

    i = 2;
    while (state->pos + i < state->length && i < 8u) {
        unsigned char c;

        c = cm_peek(state, i);
        if (c == (unsigned char)'\'')
            return 1;
        if (c == (unsigned char)'\n' || c == (unsigned char)' ' ||
            c == (unsigned char)'\t')
            return 0;
        i++;
    }
    return 0;
}

static enum cm_token_kind cm_punctuation(struct cm_lexer_state *state,
    size_t *width)
{
    unsigned char a;
    unsigned char b;
    unsigned char c;

    a = cm_peek(state, 0);
    b = cm_peek(state, 1);
    c = cm_peek(state, 2);
    *width = 1;
    switch (a) {
    case '(': return CM_TOKEN_LPAREN;
    case ')': return CM_TOKEN_RPAREN;
    case '{': return CM_TOKEN_LBRACE;
    case '}': return CM_TOKEN_RBRACE;
    case '[': return CM_TOKEN_LBRACKET;
    case ']': return CM_TOKEN_RBRACKET;
    case ',': return CM_TOKEN_COMMA;
    case ';': return CM_TOKEN_SEMICOLON;
    case ':':
        if (b == ':') { *width = 2; return CM_TOKEN_PATH_SEP; }
        return CM_TOKEN_COLON;
    case '.':
        if (b == '.' && c == '=') { *width = 3; return CM_TOKEN_DOT_DOT_EQ; }
        if (b == '.' && c == '.') { *width = 3; return CM_TOKEN_DOT_DOT_DOT; }
        if (b == '.') { *width = 2; return CM_TOKEN_DOT_DOT; }
        return CM_TOKEN_DOT;
    case '@': return CM_TOKEN_AT;
    case '#': return CM_TOKEN_POUND;
    case '$': return CM_TOKEN_DOLLAR;
    case '?': return CM_TOKEN_QUESTION;
    case '~': return CM_TOKEN_TILDE;
    case '\'': return CM_TOKEN_APOSTROPHE;
    case '=':
        if (b == '=') { *width = 2; return CM_TOKEN_EQ_EQ; }
        if (b == '>') { *width = 2; return CM_TOKEN_FAT_ARROW; }
        return CM_TOKEN_EQ;
    case '!':
        if (b == '=') { *width = 2; return CM_TOKEN_NOT_EQ; }
        return CM_TOKEN_BANG;
    case '<':
        if (b == '<' && c == '=') { *width = 3; return CM_TOKEN_SHL_EQ; }
        if (b == '<') { *width = 2; return CM_TOKEN_SHL; }
        if (b == '=') { *width = 2; return CM_TOKEN_LT_EQ; }
        if (b == '-') { *width = 2; return CM_TOKEN_THIN_ARROW_LEFT; }
        return CM_TOKEN_LT;
    case '>':
        if (b == '>' && c == '=') { *width = 3; return CM_TOKEN_SHR_EQ; }
        if (b == '>') { *width = 2; return CM_TOKEN_SHR; }
        if (b == '=') { *width = 2; return CM_TOKEN_GT_EQ; }
        return CM_TOKEN_GT;
    case '+':
        if (b == '=') { *width = 2; return CM_TOKEN_PLUS_EQ; }
        return CM_TOKEN_PLUS;
    case '-':
        if (b == '=') { *width = 2; return CM_TOKEN_MINUS_EQ; }
        if (b == '>') { *width = 2; return CM_TOKEN_THIN_ARROW; }
        return CM_TOKEN_MINUS;
    case '*':
        if (b == '=') { *width = 2; return CM_TOKEN_STAR_EQ; }
        return CM_TOKEN_STAR;
    case '/':
        if (b == '=') { *width = 2; return CM_TOKEN_SLASH_EQ; }
        return CM_TOKEN_SLASH;
    case '%':
        if (b == '=') { *width = 2; return CM_TOKEN_PERCENT_EQ; }
        return CM_TOKEN_PERCENT;
    case '^':
        if (b == '=') { *width = 2; return CM_TOKEN_CARET_EQ; }
        return CM_TOKEN_CARET;
    case '&':
        if (b == '&') { *width = 2; return CM_TOKEN_AMP_AMP; }
        if (b == '=') { *width = 2; return CM_TOKEN_AMP_EQ; }
        return CM_TOKEN_AMP;
    case '|':
        if (b == '|') { *width = 2; return CM_TOKEN_PIPE_PIPE; }
        if (b == '=') { *width = 2; return CM_TOKEN_PIPE_EQ; }
        return CM_TOKEN_PIPE;
    default:
        return CM_TOKEN_ERROR;
    }
}

void cm_lexer_options_init(struct cm_lexer_options *options)
{
    if (options == NULL)
        return;
    options->edition = CM_EDITION_2021;
    options->emit_whitespace = 0;
    options->emit_comments = 0;
}

struct cm_lexer_result cm_lex(const char *source, size_t source_length,
    const struct cm_lexer_options *options, cm_token_sink_fn sink, void *user)
{
    struct cm_lexer_state state;
    struct cm_lexer_options defaults;

    cm_lexer_options_init(&defaults);
    state.source = source != NULL ? source : "";
    state.length = source != NULL ? source_length : 0;
    state.pos = 0;
    state.line = 1;
    state.column = 1;
    state.options = options != NULL ? *options : defaults;
    state.sink = sink;
    state.user = user;
    state.previous_significant_kind = CM_TOKEN_EOF;
    state.result.token_count = 0;
    state.result.error_count = 0;
    state.result.stopped = 0;

    while (state.pos < state.length && !state.result.stopped) {
        unsigned char ch;
        size_t start;
        size_t line;
        size_t column;

        ch = cm_peek(&state, 0);
        start = state.pos;
        line = state.line;
        column = state.column;

        if (ch == (unsigned char)' ' || ch == (unsigned char)'\t' ||
            ch == (unsigned char)'\r' || ch == (unsigned char)'\n' ||
            ch == (unsigned char)'\v' || ch == (unsigned char)'\f') {
            do {
                cm_advance(&state);
                ch = cm_peek(&state, 0);
            } while (ch == (unsigned char)' ' || ch == (unsigned char)'\t' ||
                ch == (unsigned char)'\r' || ch == (unsigned char)'\n' ||
                ch == (unsigned char)'\v' || ch == (unsigned char)'\f');
            if (state.options.emit_whitespace && cm_emit(&state,
                CM_TOKEN_WHITESPACE, start, line, column, SIZE_MAX, 0, 0))
                break;
            continue;
        }

        if (ch == (unsigned char)'/' &&
            (cm_peek(&state, 1) == (unsigned char)'/' ||
             cm_peek(&state, 1) == (unsigned char)'*')) {
            if (cm_scan_comment(&state))
                break;
            continue;
        }

        if (cm_is_dec_digit(ch)) {
            if (cm_scan_number(&state))
                break;
            continue;
        }

        if (ch == (unsigned char)'\'') {
            if (cm_has_char_closer(&state)) {
                if (cm_scan_quoted(&state, CM_TOKEN_CHAR, 1, 1))
                    break;
            } else if (cm_is_ident_start(cm_peek(&state, 1))) {
                cm_advance(&state);
                cm_skip_ident(&state);
                if (cm_emit(&state, CM_TOKEN_LIFETIME, start, line, column,
                    SIZE_MAX, 0, 0))
                    break;
            } else {
                cm_advance(&state);
                if (cm_emit(&state, CM_TOKEN_APOSTROPHE, start, line, column,
                    SIZE_MAX, 0, 0))
                    break;
            }
            continue;
        }

        if (ch == (unsigned char)'"') {
            if (cm_scan_quoted(&state, CM_TOKEN_STRING, 1, 0))
                break;
            continue;
        }

        /* Prefixed and raw literals must be recognized before identifiers. */
        if (ch == (unsigned char)'b' &&
            cm_peek(&state, 1) == (unsigned char)'\'') {
            if (cm_scan_quoted(&state, CM_TOKEN_BYTE_CHAR, 2, 1))
                break;
            continue;
        }
        if (ch == (unsigned char)'b' &&
            cm_peek(&state, 1) == (unsigned char)'"') {
            if (cm_scan_quoted(&state, CM_TOKEN_BYTE_STRING, 2, 0))
                break;
            continue;
        }
        if (ch == (unsigned char)'c' &&
            cm_peek(&state, 1) == (unsigned char)'"') {
            if (cm_scan_quoted(&state, CM_TOKEN_C_STRING, 2, 0))
                break;
            continue;
        }
        if (ch == (unsigned char)'r' ||
            (ch == (unsigned char)'b' &&
             cm_peek(&state, 1) == (unsigned char)'r') ||
            (ch == (unsigned char)'c' &&
             cm_peek(&state, 1) == (unsigned char)'r')) {
            size_t letters;
            size_t quote_ahead;
            uint32_t hashes;

            letters = ch == (unsigned char)'r' ? 1u : 2u;
            if (cm_raw_prefix(&state, letters, &quote_ahead, &hashes)) {
                enum cm_token_kind raw_kind;

                raw_kind = ch == (unsigned char)'b' ?
                    CM_TOKEN_RAW_BYTE_STRING :
                    (ch == (unsigned char)'c' ? CM_TOKEN_RAW_C_STRING :
                    CM_TOKEN_RAW_STRING);
                if (cm_scan_raw_string(&state, raw_kind, quote_ahead,
                    hashes))
                    break;
                continue;
            }
        }

        if (ch == (unsigned char)'r' &&
            cm_peek(&state, 1) == (unsigned char)'#' &&
            cm_is_ident_start(cm_peek(&state, 2))) {
            cm_advance(&state);
            cm_advance(&state);
            cm_skip_ident(&state);
            if (cm_emit(&state, CM_TOKEN_RAW_IDENT, start, line, column,
                SIZE_MAX, 0, 0))
                break;
            continue;
        }

        if (cm_is_ident_start(ch)) {
            cm_skip_ident(&state);
            if (cm_emit(&state, CM_TOKEN_IDENT, start, line, column,
                SIZE_MAX, 0, 0))
                break;
            continue;
        }

        {
            enum cm_token_kind punctuation;
            size_t width;
            size_t i;

            punctuation = cm_punctuation(&state, &width);
            for (i = 0; i < width; i++)
                cm_advance(&state);
            if (cm_emit(&state, punctuation, start, line, column, SIZE_MAX,
                punctuation == CM_TOKEN_ERROR ? CM_TOKEN_F_INVALID : 0,
                0))
                break;
        }
    }

    if (!state.result.stopped) {
        cm_emit(&state, CM_TOKEN_EOF, state.pos, state.line, state.column,
            SIZE_MAX, 0, 0);
    }
    return state.result;
}
