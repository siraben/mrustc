#include "cm/macro.h"

#include <string.h>

typedef struct CmBuiltinArgParser {
    const char *text;
    size_t length;
    size_t position;
    CmMacroStatus status;
    CmMacroDiagnostic diagnostic;
} CmBuiltinArgParser;

static int cm_builtin_is_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static void cm_builtin_skip_space(CmBuiltinArgParser *parser)
{
    while (parser->position < parser->length
        && cm_builtin_is_space(parser->text[parser->position])) {
        parser->position += 1;
    }
}

static void cm_builtin_error(CmBuiltinArgParser *parser,
    CmMacroDiagnosticCode code, size_t offset, const char *message)
{
    if (parser->status != CM_MACRO_OK) {
        return;
    }
    parser->status = CM_MACRO_SYNTAX_ERROR;
    parser->diagnostic.code = code;
    parser->diagnostic.offset = offset;
    parser->diagnostic.message = message;
}

static int cm_builtin_hex_value(char byte)
{
    if (byte >= '0' && byte <= '9') {
        return (int)(byte - '0');
    }
    if (byte >= 'a' && byte <= 'f') {
        return (int)(byte - 'a') + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return (int)(byte - 'A') + 10;
    }
    return -1;
}

static int cm_builtin_parse_quoted(CmBuiltinArgParser *parser,
    char quote, CmStrBuf *decoded, CmMacroDiagnosticCode expected_code,
    const char *expected_message)
{
    size_t escape_offset;
    int high;
    int low;
    char byte;

    cm_str_buf_clear(decoded);
    cm_builtin_skip_space(parser);
    if (parser->position >= parser->length
        || parser->text[parser->position] != quote) {
        cm_builtin_error(parser, expected_code, parser->position,
            expected_message);
        return 0;
    }
    parser->position += 1;
    while (parser->position < parser->length) {
        byte = parser->text[parser->position];
        parser->position += 1;
        if (byte == quote) {
            return 1;
        }
        if (byte != '\\') {
            cm_str_buf_push(decoded, byte);
            continue;
        }
        escape_offset = parser->position - 1;
        if (parser->position >= parser->length) {
            cm_builtin_error(parser,
                CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL,
                escape_offset, "unterminated escape in macro literal");
            return 0;
        }
        byte = parser->text[parser->position];
        parser->position += 1;
        switch (byte) {
        case '\\':
        case '"':
        case '\'':
            cm_str_buf_push(decoded, byte);
            break;
        case 'n':
            cm_str_buf_push(decoded, '\n');
            break;
        case 'r':
            cm_str_buf_push(decoded, '\r');
            break;
        case 't':
            cm_str_buf_push(decoded, '\t');
            break;
        case '0':
            cm_str_buf_push(decoded, '\0');
            break;
        case 'x':
            if (parser->length - parser->position < 2) {
                cm_builtin_error(parser,
                    CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL,
                    escape_offset, "short hexadecimal escape in macro literal");
                return 0;
            }
            high = cm_builtin_hex_value(parser->text[parser->position]);
            low = cm_builtin_hex_value(parser->text[parser->position + 1]);
            if (high < 0 || low < 0) {
                cm_builtin_error(parser,
                    CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL,
                    escape_offset, "invalid hexadecimal escape in macro literal");
                return 0;
            }
            cm_str_buf_push(decoded, (char)((high * 16) + low));
            parser->position += 2;
            break;
        default:
            cm_builtin_error(parser,
                CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL,
                escape_offset, "unsupported escape in macro literal");
            return 0;
        }
    }
    cm_builtin_error(parser, expected_code, parser->position,
        "unterminated macro literal");
    return 0;
}

static void cm_builtin_append_hex_escape(CmStrBuf *output,
    unsigned char byte)
{
    static const char digits[] = "0123456789abcdef";

    cm_str_buf_append(output, "\\x");
    cm_str_buf_push(output, digits[(byte >> 4) & 15u]);
    cm_str_buf_push(output, digits[byte & 15u]);
}

static void cm_builtin_append_rust_string(CmStrBuf *output,
    const char *text, size_t length)
{
    size_t index;
    unsigned char byte;

    cm_str_buf_push(output, '"');
    for (index = 0; index < length; index += 1) {
        byte = (unsigned char)text[index];
        switch (byte) {
        case '\\':
            cm_str_buf_append(output, "\\\\");
            break;
        case '"':
            cm_str_buf_append(output, "\\\"");
            break;
        case '\n':
            cm_str_buf_append(output, "\\n");
            break;
        case '\r':
            cm_str_buf_append(output, "\\r");
            break;
        case '\t':
            cm_str_buf_append(output, "\\t");
            break;
        case '\0':
            cm_str_buf_append(output, "\\0");
            break;
        default:
            if (byte < 0x20u || byte == 0x7fu) {
                cm_builtin_append_hex_escape(output, byte);
            } else {
                cm_str_buf_push(output, (char)byte);
            }
            break;
        }
    }
    cm_str_buf_push(output, '"');
}

static void cm_builtin_append_decimal(CmStrBuf *output, size_t value)
{
    char digits[sizeof(size_t) * 3u];
    size_t count;

    count = 0;
    do {
        digits[count] = (char)('0' + (value % 10u));
        count += 1;
        value /= 10u;
    } while (value != 0);
    while (count != 0) {
        count -= 1;
        cm_str_buf_push(output, digits[count]);
    }
}

static CmMacroExpansion cm_builtin_make_result(CmBuiltinMacroKind kind)
{
    CmMacroExpansion result;

    result.status = CM_MACRO_OK;
    result.kind = kind;
    result.diagnostic.code = CM_MACRO_DIAG_NONE;
    result.diagnostic.offset = 0;
    result.diagnostic.message = "";
    return result;
}

static void cm_builtin_result_error(CmMacroExpansion *result,
    CmMacroStatus status, CmMacroDiagnosticCode code,
    size_t offset, const char *message)
{
    result->status = status;
    result->diagnostic.code = code;
    result->diagnostic.offset = offset;
    result->diagnostic.message = message;
}

static int cm_builtin_arguments_empty(CmBuiltinArgParser *parser)
{
    cm_builtin_skip_space(parser);
    if (parser->position == parser->length) {
        return 1;
    }
    cm_builtin_error(parser, CM_MACRO_DIAG_BUILTIN_EXPECTED_EMPTY,
        parser->position, "builtin macro does not accept arguments");
    return 0;
}

static int cm_builtin_is_numeric_literal(const char *text, size_t length)
{
    size_t index;

    if (length == 0) {
        return 0;
    }
    index = 0;
    if (text[index] == '+' || text[index] == '-') {
        index += 1;
    }
    return index < length && text[index] >= '0' && text[index] <= '9';
}

static int cm_builtin_expand_concat(CmBuiltinArgParser *parser,
    CmStrBuf *output)
{
    CmStrBuf joined;
    CmStrBuf decoded;
    size_t token_start;
    size_t token_length;
    char quote;

    cm_str_buf_init(&joined);
    cm_str_buf_init(&decoded);
    cm_builtin_skip_space(parser);
    while (parser->position < parser->length) {
        quote = parser->text[parser->position];
        if (quote == '"' || quote == '\'') {
            if (!cm_builtin_parse_quoted(parser, quote, &decoded,
                CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL,
                "expected a literal argument to concat!")) {
                cm_str_buf_destroy(&decoded);
                cm_str_buf_destroy(&joined);
                return 0;
            }
            if (quote == '\'' && decoded.len == 0) {
                cm_builtin_error(parser,
                    CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL,
                    parser->position, "empty character literal in concat!");
                cm_str_buf_destroy(&decoded);
                cm_str_buf_destroy(&joined);
                return 0;
            }
            cm_str_buf_append_n(&joined,
                cm_str_buf_c_str(&decoded), decoded.len);
        } else {
            token_start = parser->position;
            while (parser->position < parser->length
                && parser->text[parser->position] != ','
                && !cm_builtin_is_space(parser->text[parser->position])) {
                parser->position += 1;
            }
            token_length = parser->position - token_start;
            if (!cm_builtin_is_numeric_literal(
                    parser->text + token_start, token_length)
                && !(token_length == 4
                    && memcmp(parser->text + token_start, "true", 4) == 0)
                && !(token_length == 5
                    && memcmp(parser->text + token_start, "false", 5) == 0)) {
                cm_builtin_error(parser,
                    CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL,
                    token_start, "expected a literal argument to concat!");
                cm_str_buf_destroy(&decoded);
                cm_str_buf_destroy(&joined);
                return 0;
            }
            cm_str_buf_append_n(&joined,
                parser->text + token_start, token_length);
        }
        cm_builtin_skip_space(parser);
        if (parser->position == parser->length) {
            break;
        }
        if (parser->text[parser->position] != ',') {
            cm_builtin_error(parser, CM_MACRO_DIAG_BUILTIN_EXPECTED_COMMA,
                parser->position, "expected ',' between concat! arguments");
            cm_str_buf_destroy(&decoded);
            cm_str_buf_destroy(&joined);
            return 0;
        }
        parser->position += 1;
        cm_builtin_skip_space(parser);
    }
    cm_builtin_append_rust_string(output,
        cm_str_buf_c_str(&joined), joined.len);
    cm_str_buf_destroy(&decoded);
    cm_str_buf_destroy(&joined);
    return 1;
}

static int cm_builtin_parse_env_arguments(CmBuiltinArgParser *parser,
    int allow_message, CmStrBuf *name)
{
    CmStrBuf message;

    if (!cm_builtin_parse_quoted(parser, '"', name,
        CM_MACRO_DIAG_BUILTIN_EXPECTED_STRING,
        "expected an environment variable name string")) {
        return 0;
    }
    cm_builtin_skip_space(parser);
    if (parser->position == parser->length) {
        return 1;
    }
    if (parser->text[parser->position] != ',') {
        cm_builtin_error(parser, CM_MACRO_DIAG_BUILTIN_EXPECTED_COMMA,
            parser->position, "expected ',' after environment variable name");
        return 0;
    }
    parser->position += 1;
    cm_builtin_skip_space(parser);
    if (parser->position == parser->length) {
        return 1;
    }
    if (!allow_message) {
        cm_builtin_error(parser, CM_MACRO_DIAG_BUILTIN_EXPECTED_STRING,
            parser->position, "option_env! accepts only one string argument");
        return 0;
    }
    cm_str_buf_init(&message);
    if (!cm_builtin_parse_quoted(parser, '"', &message,
        CM_MACRO_DIAG_BUILTIN_EXPECTED_STRING,
        "expected a diagnostic message string")) {
        cm_str_buf_destroy(&message);
        return 0;
    }
    cm_str_buf_destroy(&message);
    cm_builtin_skip_space(parser);
    if (parser->position < parser->length
        && parser->text[parser->position] == ',') {
        parser->position += 1;
        cm_builtin_skip_space(parser);
    }
    if (parser->position != parser->length) {
        cm_builtin_error(parser, CM_MACRO_DIAG_BUILTIN_EXPECTED_COMMA,
            parser->position, "unexpected argument to environment macro");
        return 0;
    }
    return 1;
}

static int cm_builtin_name_equal(const char *name, size_t name_length,
    const char *expected)
{
    size_t expected_length;

    expected_length = strlen(expected);
    return name_length == expected_length
        && memcmp(name, expected, name_length) == 0;
}

const char *cm_macro_status_name(CmMacroStatus status)
{
    switch (status) {
    case CM_MACRO_OK:
        return "ok";
    case CM_MACRO_INVALID_ARGUMENT:
        return "invalid-argument";
    case CM_MACRO_SYNTAX_ERROR:
        return "syntax-error";
    case CM_MACRO_UNSUPPORTED:
        return "unsupported";
    case CM_MACRO_ENV_NOT_FOUND:
        return "environment-not-found";
    case CM_MACRO_NO_MATCH:
        return "no-match";
    case CM_MACRO_LIMIT_EXCEEDED:
        return "limit-exceeded";
    }
    return "unknown";
}

const char *cm_macro_diagnostic_code_name(CmMacroDiagnosticCode code)
{
    switch (code) {
    case CM_MACRO_DIAG_NONE:
        return "none";
    case CM_MACRO_DIAG_INVALID_ARGUMENT:
        return "invalid-argument";
    case CM_MACRO_DIAG_CFG_EXPECTED_PREDICATE:
        return "cfg-expected-predicate";
    case CM_MACRO_DIAG_CFG_EXPECTED_STRING:
        return "cfg-expected-string";
    case CM_MACRO_DIAG_CFG_INVALID_ESCAPE:
        return "cfg-invalid-escape";
    case CM_MACRO_DIAG_CFG_EXPECTED_COMMA_OR_CLOSE:
        return "cfg-expected-comma-or-close";
    case CM_MACRO_DIAG_CFG_UNKNOWN_OPERATOR:
        return "cfg-unknown-operator";
    case CM_MACRO_DIAG_CFG_NOT_ARITY:
        return "cfg-not-arity";
    case CM_MACRO_DIAG_CFG_NESTING_LIMIT:
        return "cfg-nesting-limit";
    case CM_MACRO_DIAG_CFG_TRAILING_INPUT:
        return "cfg-trailing-input";
    case CM_MACRO_DIAG_BUILTIN_UNKNOWN:
        return "builtin-unknown";
    case CM_MACRO_DIAG_BUILTIN_EXPECTED_EMPTY:
        return "builtin-expected-empty";
    case CM_MACRO_DIAG_BUILTIN_EXPECTED_STRING:
        return "builtin-expected-string";
    case CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL:
        return "builtin-expected-literal";
    case CM_MACRO_DIAG_BUILTIN_EXPECTED_COMMA:
        return "builtin-expected-comma";
    case CM_MACRO_DIAG_BUILTIN_ENV_MISSING:
        return "builtin-environment-missing";
    case CM_MACRO_DIAG_BUILTIN_INVALID_CONTEXT:
        return "builtin-invalid-context";
    case CM_MACRO_DIAG_RULES_INVALID_TREE:
        return "rules-invalid-tree";
    case CM_MACRO_DIAG_RULES_EXPECTED_MATCHER:
        return "rules-expected-matcher";
    case CM_MACRO_DIAG_RULES_EXPECTED_ARROW:
        return "rules-expected-arrow";
    case CM_MACRO_DIAG_RULES_EXPECTED_TRANSCRIBER:
        return "rules-expected-transcriber";
    case CM_MACRO_DIAG_RULES_EXPECTED_FRAGMENT:
        return "rules-expected-fragment";
    case CM_MACRO_DIAG_RULES_UNKNOWN_FRAGMENT:
        return "rules-unknown-fragment";
    case CM_MACRO_DIAG_RULES_EXPECTED_REPEAT_OPERATOR:
        return "rules-expected-repeat-operator";
    case CM_MACRO_DIAG_RULES_INVALID_SEPARATOR:
        return "rules-invalid-separator";
    case CM_MACRO_DIAG_RULES_DUPLICATE_BINDING:
        return "rules-duplicate-binding";
    case CM_MACRO_DIAG_RULES_UNKNOWN_BINDING:
        return "rules-unknown-binding";
    case CM_MACRO_DIAG_RULES_NESTING_LIMIT:
        return "rules-nesting-limit";
    case CM_MACRO_DIAG_RULES_BACKTRACK_LIMIT:
        return "rules-backtrack-limit";
    case CM_MACRO_DIAG_RULES_REPETITION_LIMIT:
        return "rules-repetition-limit";
    case CM_MACRO_DIAG_RULES_EMPTY_REPETITION:
        return "rules-empty-repetition";
    case CM_MACRO_DIAG_RULES_NO_MATCH:
        return "rules-no-match";
    case CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION:
        return "rules-transcribe-repetition";
    }
    return "unknown";
}

void cm_builtin_context_init(CmBuiltinContext *context)
{
    if (context == NULL) {
        return;
    }
    context->file = "";
    context->file_length = 0;
    context->line = 0;
    context->column = 0;
    context->module_path = NULL;
    context->module_path_length = 0u;
    context->cfg = NULL;
    context->env_lookup = NULL;
    context->env_context = NULL;
}

CmBuiltinMacroKind cm_builtin_macro_classify(const char *name,
    size_t name_length)
{
    if (name == NULL) {
        return CM_BUILTIN_MACRO_UNKNOWN;
    }
    if (name_length != 0 && name[name_length - 1] == '!') {
        name_length -= 1;
    }
    if (cm_builtin_name_equal(name, name_length, "line")) {
        return CM_BUILTIN_MACRO_LINE;
    }
    if (cm_builtin_name_equal(name, name_length, "column")) {
        return CM_BUILTIN_MACRO_COLUMN;
    }
    if (cm_builtin_name_equal(name, name_length, "file")) {
        return CM_BUILTIN_MACRO_FILE;
    }
    if (cm_builtin_name_equal(name, name_length, "stringify")) {
        return CM_BUILTIN_MACRO_STRINGIFY;
    }
    if (cm_builtin_name_equal(name, name_length, "concat")) {
        return CM_BUILTIN_MACRO_CONCAT;
    }
    if (cm_builtin_name_equal(name, name_length, "env")) {
        return CM_BUILTIN_MACRO_ENV;
    }
    if (cm_builtin_name_equal(name, name_length, "option_env")) {
        return CM_BUILTIN_MACRO_OPTION_ENV;
    }
    if (cm_builtin_name_equal(name, name_length, "module_path")) {
        return CM_BUILTIN_MACRO_MODULE_PATH;
    }
    if (cm_builtin_name_equal(name, name_length, "cfg")) {
        return CM_BUILTIN_MACRO_CFG;
    }
    return CM_BUILTIN_MACRO_UNKNOWN;
}

const char *cm_builtin_macro_kind_name(CmBuiltinMacroKind kind)
{
    switch (kind) {
    case CM_BUILTIN_MACRO_UNKNOWN:
        return "unknown";
    case CM_BUILTIN_MACRO_LINE:
        return "line";
    case CM_BUILTIN_MACRO_COLUMN:
        return "column";
    case CM_BUILTIN_MACRO_FILE:
        return "file";
    case CM_BUILTIN_MACRO_STRINGIFY:
        return "stringify";
    case CM_BUILTIN_MACRO_CONCAT:
        return "concat";
    case CM_BUILTIN_MACRO_ENV:
        return "env";
    case CM_BUILTIN_MACRO_OPTION_ENV:
        return "option_env";
    case CM_BUILTIN_MACRO_MODULE_PATH:
        return "module_path";
    case CM_BUILTIN_MACRO_CFG:
        return "cfg";
    }
    return "unknown";
}

CmMacroExpansion cm_builtin_macro_expand(CmBuiltinMacroKind kind,
    const char *arguments, size_t argument_length,
    const CmBuiltinContext *context, CmStrBuf *output)
{
    CmMacroExpansion result;
    CmBuiltinArgParser parser;
    CmStrBuf decoded;
    const char *value;
    size_t value_length;
    size_t start;
    size_t end;

    result = cm_builtin_make_result(kind);
    if (output != NULL) {
        cm_str_buf_clear(output);
    }
    if (output == NULL || (arguments == NULL && argument_length != 0)) {
        cm_builtin_result_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0,
            "invalid builtin macro expansion argument");
        return result;
    }
    parser.text = arguments == NULL ? "" : arguments;
    parser.length = argument_length;
    parser.position = 0;
    parser.status = CM_MACRO_OK;
    parser.diagnostic.code = CM_MACRO_DIAG_NONE;
    parser.diagnostic.offset = 0;
    parser.diagnostic.message = "";

    if (kind == CM_BUILTIN_MACRO_UNKNOWN
        || (unsigned int)kind
            > (unsigned int)CM_BUILTIN_MACRO_CFG) {
        cm_builtin_result_error(&result, CM_MACRO_UNSUPPORTED,
            CM_MACRO_DIAG_BUILTIN_UNKNOWN, 0,
            "unknown builtin macro");
        return result;
    }
    if (kind == CM_BUILTIN_MACRO_LINE
        || kind == CM_BUILTIN_MACRO_COLUMN
        || kind == CM_BUILTIN_MACRO_FILE
        || kind == CM_BUILTIN_MACRO_MODULE_PATH) {
        if (!cm_builtin_arguments_empty(&parser)) {
            result.status = parser.status;
            result.diagnostic = parser.diagnostic;
            return result;
        }
        if (context == NULL
            || (kind == CM_BUILTIN_MACRO_LINE && context->line == 0)
            || (kind == CM_BUILTIN_MACRO_COLUMN && context->column == 0)
            || (kind == CM_BUILTIN_MACRO_FILE && context->file == NULL)
            || (kind == CM_BUILTIN_MACRO_MODULE_PATH
                && context->module_path == NULL)) {
            cm_builtin_result_error(&result, CM_MACRO_INVALID_ARGUMENT,
                CM_MACRO_DIAG_BUILTIN_INVALID_CONTEXT, 0,
                "builtin macro requires a valid source location");
            return result;
        }
        if (kind == CM_BUILTIN_MACRO_LINE) {
            cm_builtin_append_decimal(output, context->line);
        } else if (kind == CM_BUILTIN_MACRO_COLUMN) {
            cm_builtin_append_decimal(output, context->column);
        } else if (kind == CM_BUILTIN_MACRO_FILE) {
            cm_builtin_append_rust_string(output,
                context->file, context->file_length);
        } else {
            cm_builtin_append_rust_string(output, context->module_path,
                context->module_path_length);
        }
        return result;
    }
    if (kind == CM_BUILTIN_MACRO_CFG) {
        CmCfgEvaluation evaluation;

        if (context == NULL || context->cfg == NULL) {
            cm_builtin_result_error(&result, CM_MACRO_INVALID_ARGUMENT,
                CM_MACRO_DIAG_BUILTIN_INVALID_CONTEXT, 0u,
                "cfg! requires an explicit cfg environment");
            return result;
        }
        evaluation = cm_cfg_evaluate(context->cfg, parser.text,
            parser.length);
        if (evaluation.status != CM_MACRO_OK) {
            result.status = evaluation.status;
            result.diagnostic = evaluation.diagnostic;
            return result;
        }
        cm_str_buf_append(output, evaluation.value ? "true" : "false");
        return result;
    }
    if (kind == CM_BUILTIN_MACRO_STRINGIFY) {
        start = 0;
        end = argument_length;
        while (start < end && cm_builtin_is_space(parser.text[start])) {
            start += 1;
        }
        while (end > start && cm_builtin_is_space(parser.text[end - 1])) {
            end -= 1;
        }
        cm_builtin_append_rust_string(output,
            parser.text + start, end - start);
        return result;
    }
    if (kind == CM_BUILTIN_MACRO_CONCAT) {
        if (!cm_builtin_expand_concat(&parser, output)) {
            cm_str_buf_clear(output);
            result.status = parser.status;
            result.diagnostic = parser.diagnostic;
        }
        return result;
    }

    cm_str_buf_init(&decoded);
    if (!cm_builtin_parse_env_arguments(&parser,
        kind == CM_BUILTIN_MACRO_ENV, &decoded)) {
        cm_str_buf_destroy(&decoded);
        result.status = parser.status;
        result.diagnostic = parser.diagnostic;
        return result;
    }
    value = NULL;
    value_length = 0;
    if (context != NULL && context->env_lookup != NULL) {
        value = context->env_lookup(context->env_context,
            cm_str_buf_c_str(&decoded), decoded.len, &value_length);
    }
    cm_str_buf_destroy(&decoded);
    if (value == NULL) {
        if (kind == CM_BUILTIN_MACRO_OPTION_ENV) {
            cm_str_buf_append(output, "None");
            return result;
        }
        cm_builtin_result_error(&result, CM_MACRO_ENV_NOT_FOUND,
            CM_MACRO_DIAG_BUILTIN_ENV_MISSING, 0,
            "environment variable is not defined");
        return result;
    }
    if (kind == CM_BUILTIN_MACRO_OPTION_ENV) {
        cm_str_buf_append(output, "Some(");
        cm_builtin_append_rust_string(output, value, value_length);
        cm_str_buf_push(output, ')');
    } else {
        cm_builtin_append_rust_string(output, value, value_length);
    }
    return result;
}
