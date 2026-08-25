#include "cm/driver/g3_options.h"

#include <string.h>

static CmG3OptionsResult cm_g3_result(CmG3OptionsStatus status, int index,
    const char *option)
{
    CmG3OptionsResult result;

    result.status = status;
    result.argument_index = index;
    result.option = option;
    return result;
}

static int cm_g3_ascii_identifier(const char *text, size_t length)
{
    size_t index;
    unsigned char character;

    if (text == NULL || length == 0u) return 0;
    character = (unsigned char)text[0];
    if (!((character >= (unsigned char)'A'
                && character <= (unsigned char)'Z')
            || (character >= (unsigned char)'a'
                && character <= (unsigned char)'z')
            || character == (unsigned char)'_')) return 0;
    for (index = 1u; index < length; ++index) {
        character = (unsigned char)text[index];
        if (!((character >= (unsigned char)'A'
                    && character <= (unsigned char)'Z')
                || (character >= (unsigned char)'a'
                    && character <= (unsigned char)'z')
                || (character >= (unsigned char)'0'
                    && character <= (unsigned char)'9')
                || character == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_g3_valid_edition(const char *text)
{
    return strcmp(text, "2015") == 0 || strcmp(text, "2018") == 0
        || strcmp(text, "2021") == 0 || strcmp(text, "2024") == 0;
}

static int cm_g3_view_equal(CmG3StringView left, const char *right,
    size_t right_length)
{
    return left.length == right_length
        && memcmp(left.data, right, right_length) == 0;
}

static CmG3OptionsResult cm_g3_parse_action(CmG3Options *parsed,
    CmG3Action action, const char *option, int option_index,
    const char *value)
{
    if (parsed->action == action) {
        return cm_g3_result(CM_G3_OPTIONS_DUPLICATE_OPTION, option_index,
            option);
    }
    if (parsed->action != CM_G3_ACTION_NONE) {
        return cm_g3_result(CM_G3_OPTIONS_CONFLICTING_ACTION, option_index,
            option);
    }
    parsed->action = action;
    parsed->input_path = value;
    return cm_g3_result(CM_G3_OPTIONS_OK, 0, NULL);
}

static CmG3OptionsResult cm_g3_add_extern(CmG3Options *parsed,
    const char *value, int value_index)
{
    const char *separator;
    size_t name_length;
    size_t index;

    separator = strchr(value, '=');
    if (separator == NULL || separator == value || separator[1] == '\0') {
        return cm_g3_result(CM_G3_OPTIONS_INVALID_EXTERN, value_index,
            "--extern-cmrlib");
    }
    name_length = (size_t)(separator - value);
    if (!cm_g3_ascii_identifier(value, name_length)) {
        return cm_g3_result(CM_G3_OPTIONS_INVALID_IDENTIFIER, value_index,
            "--extern-cmrlib");
    }
    for (index = 0u; index < parsed->extern_count; ++index) {
        if (cm_g3_view_equal(parsed->externs[index].name, value,
                name_length)) {
            return cm_g3_result(CM_G3_OPTIONS_DUPLICATE_EXTERN, value_index,
                "--extern-cmrlib");
        }
    }
    if (parsed->extern_count == CM_G3_OPTIONS_MAX_EXTERNS) {
        return cm_g3_result(CM_G3_OPTIONS_TOO_MANY_EXTERNS, value_index,
            "--extern-cmrlib");
    }
    parsed->externs[parsed->extern_count].name.data = value;
    parsed->externs[parsed->extern_count].name.length = name_length;
    parsed->externs[parsed->extern_count].path = separator + 1;
    parsed->extern_count += 1u;
    return cm_g3_result(CM_G3_OPTIONS_OK, 0, NULL);
}

CmG3OptionsResult cm_g3_options_parse(int argc, const char *const argv[],
    CmG3Options *options)
{
    CmG3Options parsed;
    CmG3OptionsResult result;
    int index;
    int c_compiler_index;
    int first_extern_index;

    if (argc < 1 || argv == NULL || options == NULL || argv[0] == NULL) {
        return cm_g3_result(CM_G3_OPTIONS_INVALID_ARGUMENT, 0, NULL);
    }
    memset(&parsed, 0, sizeof(parsed));
    c_compiler_index = -1;
    first_extern_index = -1;
    for (index = 1; index < argc; ++index) {
        const char *argument;
        const char *value;

        argument = argv[index];
        if (argument == NULL) {
            return cm_g3_result(CM_G3_OPTIONS_INVALID_ARGUMENT, index, NULL);
        }
        if (strcmp(argument, "--emit-c") == 0
            || strcmp(argument, "--emit-cmrlib") == 0
            || strcmp(argument, "-o") == 0
            || strcmp(argument, "--crate-name") == 0
            || strcmp(argument, "--cc") == 0
            || strcmp(argument, "--extern-cmrlib") == 0
            || strcmp(argument, "--edition") == 0
            || strcmp(argument, "--target") == 0) {
            if (index + 1 >= argc || argv[index + 1] == NULL
                || argv[index + 1][0] == '\0') {
                return cm_g3_result(CM_G3_OPTIONS_MISSING_VALUE, index,
                    argument);
            }
            value = argv[++index];
        } else {
            return cm_g3_result(CM_G3_OPTIONS_UNKNOWN_OPTION, index,
                argument);
        }

        if (strcmp(argument, "--emit-c") == 0) {
            result = cm_g3_parse_action(&parsed, CM_G3_ACTION_EMIT_C,
                argument, index - 1, value);
        } else if (strcmp(argument, "--emit-cmrlib") == 0) {
            result = cm_g3_parse_action(&parsed, CM_G3_ACTION_EMIT_CMRLIB,
                argument, index - 1, value);
        } else if (strcmp(argument, "-o") == 0) {
            if (parsed.output_path != NULL) {
                return cm_g3_result(CM_G3_OPTIONS_DUPLICATE_OPTION,
                    index - 1, argument);
            }
            parsed.output_path = value;
            result = cm_g3_result(CM_G3_OPTIONS_OK, 0, NULL);
        } else if (strcmp(argument, "--crate-name") == 0) {
            if (parsed.crate_name != NULL) {
                return cm_g3_result(CM_G3_OPTIONS_DUPLICATE_OPTION,
                    index - 1, argument);
            }
            if (!cm_g3_ascii_identifier(value, strlen(value))) {
                return cm_g3_result(CM_G3_OPTIONS_INVALID_IDENTIFIER,
                    index, argument);
            }
            parsed.crate_name = value;
            result = cm_g3_result(CM_G3_OPTIONS_OK, 0, NULL);
        } else if (strcmp(argument, "--cc") == 0) {
            if (parsed.c_compiler != NULL) {
                return cm_g3_result(CM_G3_OPTIONS_DUPLICATE_OPTION,
                    index - 1, argument);
            }
            parsed.c_compiler = value;
            c_compiler_index = index - 1;
            result = cm_g3_result(CM_G3_OPTIONS_OK, 0, NULL);
        } else if (strcmp(argument, "--extern-cmrlib") == 0) {
            result = cm_g3_add_extern(&parsed, value, index);
            if (result.status == CM_G3_OPTIONS_OK
                && first_extern_index < 0) first_extern_index = index - 1;
        } else if (strcmp(argument, "--edition") == 0) {
            if (parsed.edition != NULL) {
                return cm_g3_result(CM_G3_OPTIONS_DUPLICATE_OPTION,
                    index - 1, argument);
            }
            if (!cm_g3_valid_edition(value)) {
                return cm_g3_result(CM_G3_OPTIONS_INVALID_EDITION, index,
                    argument);
            }
            parsed.edition = value;
            result = cm_g3_result(CM_G3_OPTIONS_OK, 0, NULL);
        } else {
            if (parsed.target != NULL) {
                return cm_g3_result(CM_G3_OPTIONS_DUPLICATE_OPTION,
                    index - 1, argument);
            }
            parsed.target = value;
            result = cm_g3_result(CM_G3_OPTIONS_OK, 0, NULL);
        }
        if (result.status != CM_G3_OPTIONS_OK) return result;
    }

    if (parsed.action == CM_G3_ACTION_NONE) {
        return cm_g3_result(CM_G3_OPTIONS_MISSING_ACTION, argc, NULL);
    }
    if (parsed.output_path == NULL) {
        return cm_g3_result(CM_G3_OPTIONS_MISSING_REQUIRED_OPTION, argc,
            "-o");
    }
    if (parsed.crate_name == NULL) {
        return cm_g3_result(CM_G3_OPTIONS_MISSING_REQUIRED_OPTION, argc,
            "--crate-name");
    }
    if (parsed.action == CM_G3_ACTION_EMIT_CMRLIB) {
        if (parsed.c_compiler == NULL) {
            return cm_g3_result(CM_G3_OPTIONS_MISSING_REQUIRED_OPTION, argc,
                "--cc");
        }
        if (parsed.extern_count != 0u) {
            return cm_g3_result(CM_G3_OPTIONS_OPTION_NOT_ALLOWED,
                first_extern_index, "--extern-cmrlib");
        }
    } else if (parsed.c_compiler != NULL) {
        return cm_g3_result(CM_G3_OPTIONS_OPTION_NOT_ALLOWED,
            c_compiler_index, "--cc");
    }

    *options = parsed;
    return cm_g3_result(CM_G3_OPTIONS_OK, 0, NULL);
}

const char *cm_g3_options_status_name(CmG3OptionsStatus status)
{
    switch (status) {
    case CM_G3_OPTIONS_OK: return "ok";
    case CM_G3_OPTIONS_INVALID_ARGUMENT: return "invalid argument";
    case CM_G3_OPTIONS_UNKNOWN_OPTION: return "unknown option";
    case CM_G3_OPTIONS_MISSING_VALUE: return "missing value";
    case CM_G3_OPTIONS_DUPLICATE_OPTION: return "duplicate option";
    case CM_G3_OPTIONS_CONFLICTING_ACTION: return "conflicting action";
    case CM_G3_OPTIONS_MISSING_ACTION: return "missing action";
    case CM_G3_OPTIONS_MISSING_REQUIRED_OPTION:
        return "missing required option";
    case CM_G3_OPTIONS_INVALID_IDENTIFIER: return "invalid identifier";
    case CM_G3_OPTIONS_INVALID_EDITION: return "invalid edition";
    case CM_G3_OPTIONS_INVALID_EXTERN: return "invalid extern";
    case CM_G3_OPTIONS_DUPLICATE_EXTERN: return "duplicate extern";
    case CM_G3_OPTIONS_TOO_MANY_EXTERNS: return "too many externs";
    case CM_G3_OPTIONS_OPTION_NOT_ALLOWED: return "option not allowed";
    default: return "unknown g3 options status";
    }
}
