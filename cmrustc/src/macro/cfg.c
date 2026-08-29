#include "cm/macro.h"

#include <string.h>

#define CM_CFG_MAX_DEPTH 64u

typedef struct CmCfgParser {
    const CmCfgEnvironment *environment;
    const char *text;
    size_t length;
    size_t position;
    CmMacroStatus status;
    CmMacroDiagnostic diagnostic;
} CmCfgParser;

static int cm_cfg_is_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static int cm_cfg_is_ident_start(char byte)
{
    return (byte >= 'a' && byte <= 'z')
        || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

static int cm_cfg_is_ident_continue(char byte)
{
    return cm_cfg_is_ident_start(byte) || (byte >= '0' && byte <= '9');
}

static int cm_cfg_slice_equal(const char *left, size_t left_length,
    const char *right)
{
    size_t right_length;

    if (right == NULL) {
        return 0;
    }
    right_length = strlen(right);
    return left_length == right_length
        && memcmp(left, right, left_length) == 0;
}

/* Whitespace and comments: a `#[cfg(all(.., // FIXME .. \n ..))]` written
 * over several lines (libc's primitives, hashbrown's group selection) keeps
 * its line comments in the predicate text. */
static void cm_cfg_skip_space(CmCfgParser *parser)
{
    for (;;) {
        while (parser->position < parser->length
            && cm_cfg_is_space(parser->text[parser->position])) {
            parser->position += 1;
        }
        if (parser->position + 1u < parser->length
            && parser->text[parser->position] == '/'
            && parser->text[parser->position + 1u] == '/') {
            while (parser->position < parser->length
                && parser->text[parser->position] != '\n') {
                parser->position += 1;
            }
            continue;
        }
        if (parser->position + 1u < parser->length
            && parser->text[parser->position] == '/'
            && parser->text[parser->position + 1u] == '*') {
            parser->position += 2u;
            while (parser->position + 1u < parser->length
                && !(parser->text[parser->position] == '*'
                    && parser->text[parser->position + 1u] == '/')) {
                parser->position += 1;
            }
            parser->position = parser->position + 2u > parser->length
                ? parser->length : parser->position + 2u;
            continue;
        }
        return;
    }
}

static void cm_cfg_error(CmCfgParser *parser, CmMacroDiagnosticCode code,
    size_t offset, const char *message)
{
    if (parser->status != CM_MACRO_OK) {
        return;
    }
    parser->status = CM_MACRO_SYNTAX_ERROR;
    parser->diagnostic.code = code;
    parser->diagnostic.offset = offset;
    parser->diagnostic.message = message;
}

static int cm_cfg_parse_identifier(CmCfgParser *parser,
    size_t *start, size_t *length)
{
    size_t begin;

    cm_cfg_skip_space(parser);
    begin = parser->position;
    if (begin >= parser->length
        || !cm_cfg_is_ident_start(parser->text[begin])) {
        cm_cfg_error(parser, CM_MACRO_DIAG_CFG_EXPECTED_PREDICATE,
            begin, "expected a cfg predicate name");
        return 0;
    }
    parser->position += 1;
    while (parser->position < parser->length
        && cm_cfg_is_ident_continue(parser->text[parser->position])) {
        parser->position += 1;
    }
    *start = begin;
    *length = parser->position - begin;
    return 1;
}

static int cm_cfg_hex_value(char byte)
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

static int cm_cfg_parse_string(CmCfgParser *parser, CmStrBuf *value)
{
    size_t escape_offset;
    int high;
    int low;
    char byte;

    cm_str_buf_clear(value);
    cm_cfg_skip_space(parser);
    if (parser->position >= parser->length
        || parser->text[parser->position] != '"') {
        cm_cfg_error(parser, CM_MACRO_DIAG_CFG_EXPECTED_STRING,
            parser->position, "expected a quoted cfg value");
        return 0;
    }
    parser->position += 1;
    while (parser->position < parser->length) {
        byte = parser->text[parser->position];
        parser->position += 1;
        if (byte == '"') {
            return 1;
        }
        if (byte != '\\') {
            cm_str_buf_push(value, byte);
            continue;
        }
        escape_offset = parser->position - 1;
        if (parser->position >= parser->length) {
            cm_cfg_error(parser, CM_MACRO_DIAG_CFG_INVALID_ESCAPE,
                escape_offset, "unterminated escape in cfg value");
            return 0;
        }
        byte = parser->text[parser->position];
        parser->position += 1;
        switch (byte) {
        case '\\':
        case '"':
            cm_str_buf_push(value, byte);
            break;
        case 'n':
            cm_str_buf_push(value, '\n');
            break;
        case 'r':
            cm_str_buf_push(value, '\r');
            break;
        case 't':
            cm_str_buf_push(value, '\t');
            break;
        case '0':
            cm_str_buf_push(value, '\0');
            break;
        case 'x':
            if (parser->length - parser->position < 2) {
                cm_cfg_error(parser, CM_MACRO_DIAG_CFG_INVALID_ESCAPE,
                    escape_offset, "short hexadecimal escape in cfg value");
                return 0;
            }
            high = cm_cfg_hex_value(parser->text[parser->position]);
            low = cm_cfg_hex_value(parser->text[parser->position + 1]);
            if (high < 0 || low < 0) {
                cm_cfg_error(parser, CM_MACRO_DIAG_CFG_INVALID_ESCAPE,
                    escape_offset, "invalid hexadecimal escape in cfg value");
                return 0;
            }
            cm_str_buf_push(value, (char)((high * 16) + low));
            parser->position += 2;
            break;
        default:
            cm_cfg_error(parser, CM_MACRO_DIAG_CFG_INVALID_ESCAPE,
                escape_offset, "unsupported escape in cfg value");
            return 0;
        }
    }
    cm_cfg_error(parser, CM_MACRO_DIAG_CFG_EXPECTED_STRING,
        parser->position, "unterminated quoted cfg value");
    return 0;
}

static int cm_cfg_list_contains(const char *const *values, size_t count,
    const char *value, size_t value_length)
{
    size_t index;

    if (values == NULL) {
        return 0;
    }
    for (index = 0; index < count; index += 1) {
        if (cm_cfg_slice_equal(value, value_length, values[index])) {
            return 1;
        }
    }
    return 0;
}

static int cm_cfg_match_entry(const CmCfgEnvironment *environment,
    const char *name, size_t name_length,
    const char *value, size_t value_length, int has_value)
{
    size_t index;
    const CmCfgEntry *entry;

    if (environment->entries == NULL) {
        return 0;
    }
    for (index = 0; index < environment->entry_count; index += 1) {
        entry = &environment->entries[index];
        if (!cm_cfg_slice_equal(name, name_length, entry->name)) {
            continue;
        }
        if (!has_value) {
            return 1;
        }
        if (has_value && entry->value != NULL
            && cm_cfg_slice_equal(value, value_length, entry->value)) {
            return 1;
        }
    }
    return 0;
}

static int cm_cfg_match_atom(const CmCfgEnvironment *environment,
    const char *name, size_t name_length,
    const char *value, size_t value_length, int has_value)
{
    if (cm_cfg_match_entry(environment, name, name_length,
        value, value_length, has_value)) {
        return 1;
    }
    if (!has_value) {
        if (cm_cfg_slice_equal(name, name_length, "unix")) {
            return environment->target_family != NULL
                && strcmp(environment->target_family, "unix") == 0;
        }
        if (cm_cfg_slice_equal(name, name_length, "windows")) {
            return environment->target_family != NULL
                && strcmp(environment->target_family, "windows") == 0;
        }
        return 0;
    }
    if (cm_cfg_slice_equal(name, name_length, "target_arch")) {
        return cm_cfg_slice_equal(value, value_length,
            environment->target_arch);
    }
    if (cm_cfg_slice_equal(name, name_length, "target_os")) {
        return cm_cfg_slice_equal(value, value_length,
            environment->target_os);
    }
    if (cm_cfg_slice_equal(name, name_length, "target_env")) {
        return cm_cfg_slice_equal(value, value_length,
            environment->target_env);
    }
    if (cm_cfg_slice_equal(name, name_length, "target_abi")) {
        return cm_cfg_slice_equal(value, value_length,
            environment->target_abi);
    }
    if (cm_cfg_slice_equal(name, name_length, "target_vendor")) {
        return cm_cfg_slice_equal(value, value_length,
            environment->target_vendor);
    }
    if (cm_cfg_slice_equal(name, name_length, "target_family")) {
        return cm_cfg_slice_equal(value, value_length,
            environment->target_family);
    }
    if (cm_cfg_slice_equal(name, name_length, "target_pointer_width")) {
        return cm_cfg_slice_equal(value, value_length,
            environment->target_pointer_width);
    }
    if (cm_cfg_slice_equal(name, name_length, "target_endian")) {
        return cm_cfg_slice_equal(value, value_length,
            environment->target_endian);
    }
    if (cm_cfg_slice_equal(name, name_length, "feature")) {
        return cm_cfg_list_contains(environment->features,
            environment->feature_count, value, value_length);
    }
    if (cm_cfg_slice_equal(name, name_length, "target_feature")) {
        return cm_cfg_list_contains(environment->target_features,
            environment->target_feature_count, value, value_length);
    }
    return 0;
}

static int cm_cfg_parse_predicate(CmCfgParser *parser,
    unsigned int depth, int *result)
{
    size_t name_start;
    size_t name_length;
    size_t argument_count;
    int is_all;
    int is_any;
    int is_not;
    int argument;
    int combined;
    CmStrBuf value;

    if (depth >= CM_CFG_MAX_DEPTH) {
        cm_cfg_error(parser, CM_MACRO_DIAG_CFG_NESTING_LIMIT,
            parser->position, "cfg predicate nesting limit exceeded");
        return 0;
    }
    if (!cm_cfg_parse_identifier(parser, &name_start, &name_length)) {
        return 0;
    }
    cm_cfg_skip_space(parser);
    if (parser->position < parser->length
        && parser->text[parser->position] == '=') {
        parser->position += 1;
        cm_str_buf_init(&value);
        if (!cm_cfg_parse_string(parser, &value)) {
            cm_str_buf_destroy(&value);
            return 0;
        }
        *result = cm_cfg_match_atom(parser->environment,
            parser->text + name_start, name_length,
            cm_str_buf_c_str(&value), value.len, 1);
        cm_str_buf_destroy(&value);
        return 1;
    }
    if (parser->position >= parser->length
        || parser->text[parser->position] != '(') {
        *result = cm_cfg_match_atom(parser->environment,
            parser->text + name_start, name_length, NULL, 0, 0);
        return 1;
    }

    is_all = cm_cfg_slice_equal(parser->text + name_start,
        name_length, "all");
    is_any = cm_cfg_slice_equal(parser->text + name_start,
        name_length, "any");
    is_not = cm_cfg_slice_equal(parser->text + name_start,
        name_length, "not");
    if (!is_all && !is_any && !is_not) {
        cm_cfg_error(parser, CM_MACRO_DIAG_CFG_UNKNOWN_OPERATOR,
            name_start, "unknown cfg predicate operator");
        return 0;
    }
    parser->position += 1;
    argument_count = 0;
    combined = is_all;
    cm_cfg_skip_space(parser);
    while (parser->position < parser->length
        && parser->text[parser->position] != ')') {
        if (!cm_cfg_parse_predicate(parser, depth + 1u, &argument)) {
            return 0;
        }
        argument_count += 1;
        if (is_all) {
            combined = combined && argument;
        } else if (is_any) {
            combined = combined || argument;
        } else {
            combined = argument;
        }
        cm_cfg_skip_space(parser);
        if (parser->position < parser->length
            && parser->text[parser->position] == ',') {
            parser->position += 1;
            cm_cfg_skip_space(parser);
            continue;
        }
        if (parser->position >= parser->length
            || parser->text[parser->position] != ')') {
            cm_cfg_error(parser,
                CM_MACRO_DIAG_CFG_EXPECTED_COMMA_OR_CLOSE,
                parser->position, "expected ',' or ')' in cfg predicate");
            return 0;
        }
    }
    if (parser->position >= parser->length
        || parser->text[parser->position] != ')') {
        cm_cfg_error(parser, CM_MACRO_DIAG_CFG_EXPECTED_COMMA_OR_CLOSE,
            parser->position, "expected ')' to close cfg predicate");
        return 0;
    }
    parser->position += 1;
    if (is_not && argument_count != 1) {
        cm_cfg_error(parser, CM_MACRO_DIAG_CFG_NOT_ARITY,
            name_start, "not() requires exactly one cfg predicate");
        return 0;
    }
    if (is_not) {
        combined = !combined;
    }
    *result = combined;
    return 1;
}

static CmCfgEvaluation cm_cfg_invalid_argument(void)
{
    CmCfgEvaluation evaluation;

    evaluation.status = CM_MACRO_INVALID_ARGUMENT;
    evaluation.value = 0;
    evaluation.diagnostic.code = CM_MACRO_DIAG_INVALID_ARGUMENT;
    evaluation.diagnostic.offset = 0;
    evaluation.diagnostic.message = "invalid cfg evaluator argument";
    return evaluation;
}

void cm_cfg_environment_init(CmCfgEnvironment *environment)
{
    if (environment == NULL) {
        return;
    }
    memset(environment, 0, sizeof(*environment));
}

CmCfgEvaluation cm_cfg_evaluate(const CmCfgEnvironment *environment,
    const char *predicate, size_t predicate_length)
{
    CmCfgEvaluation evaluation;
    CmCfgParser parser;
    int value;

    if (environment == NULL || (predicate == NULL && predicate_length != 0)
        || (environment->feature_count != 0
            && environment->features == NULL)
        || (environment->target_feature_count != 0
            && environment->target_features == NULL)
        || (environment->entry_count != 0
            && environment->entries == NULL)) {
        return cm_cfg_invalid_argument();
    }
    parser.environment = environment;
    parser.text = predicate == NULL ? "" : predicate;
    parser.length = predicate_length;
    parser.position = 0;
    parser.status = CM_MACRO_OK;
    parser.diagnostic.code = CM_MACRO_DIAG_NONE;
    parser.diagnostic.offset = 0;
    parser.diagnostic.message = "";
    value = 0;
    if (cm_cfg_parse_predicate(&parser, 0u, &value)) {
        cm_cfg_skip_space(&parser);
        if (parser.position != parser.length) {
            cm_cfg_error(&parser, CM_MACRO_DIAG_CFG_TRAILING_INPUT,
                parser.position, "unexpected input after cfg predicate");
        }
    }
    evaluation.status = parser.status;
    evaluation.value = parser.status == CM_MACRO_OK ? value : 0;
    evaluation.diagnostic = parser.diagnostic;
    return evaluation;
}

CmCfgEvaluation cm_cfg_attr_decide(const CmCfgEnvironment *environment,
    const char *predicate, size_t predicate_length)
{
    return cm_cfg_evaluate(environment, predicate, predicate_length);
}
