#include "cm/macro.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "builtin/%s: %s\n", test, message);
    failures += 1;
}

static const char *lookup_env(void *context, const char *name,
    size_t name_length, size_t *value_length)
{
    const char *value;

    (void)context;
    if (name_length != 7 || memcmp(name, "PRESENT", 7) != 0) {
        return NULL;
    }
    value = "v\"\\";
    *value_length = strlen(value);
    return value;
}

static CmMacroExpansion expand(CmBuiltinMacroKind kind,
    const char *arguments, const CmBuiltinContext *context,
    CmStrBuf *output)
{
    return cm_builtin_macro_expand(kind, arguments, strlen(arguments),
        context, output);
}

static void expect_output(const char *test, CmBuiltinMacroKind kind,
    const char *arguments, const CmBuiltinContext *context,
    const char *expected)
{
    CmStrBuf output;
    CmMacroExpansion expansion;

    cm_str_buf_init(&output);
    expansion = expand(kind, arguments, context, &output);
    if (expansion.status != CM_MACRO_OK) {
        fprintf(stderr, "builtin/%s: unexpected %s at %lu: %s\n", test,
            cm_macro_status_name(expansion.status),
            (unsigned long)expansion.diagnostic.offset,
            expansion.diagnostic.message);
        failures += 1;
    } else if (strcmp(cm_str_buf_c_str(&output), expected) != 0) {
        fprintf(stderr, "builtin/%s: expected [%s], got [%s]\n", test,
            expected, cm_str_buf_c_str(&output));
        failures += 1;
    }
    cm_str_buf_destroy(&output);
}

static void test_classification(void)
{
    struct Classification {
        const char *name;
        CmBuiltinMacroKind kind;
    } cases[] = {
        { "line!", CM_BUILTIN_MACRO_LINE },
        { "column", CM_BUILTIN_MACRO_COLUMN },
        { "file!", CM_BUILTIN_MACRO_FILE },
        { "stringify!", CM_BUILTIN_MACRO_STRINGIFY },
        { "concat!", CM_BUILTIN_MACRO_CONCAT },
        { "env!", CM_BUILTIN_MACRO_ENV },
        { "option_env!", CM_BUILTIN_MACRO_OPTION_ENV },
        { "module_path!", CM_BUILTIN_MACRO_MODULE_PATH },
        { "cfg!", CM_BUILTIN_MACRO_CFG },
        { "include!", CM_BUILTIN_MACRO_UNKNOWN }
    };
    size_t index;

    for (index = 0; index < CM_ARRAY_LEN(cases); index += 1) {
        if (cm_builtin_macro_classify(cases[index].name,
            strlen(cases[index].name)) != cases[index].kind) {
            fail("classify", cases[index].name);
        }
    }
}

static void test_location_and_tokens(void)
{
    CmBuiltinContext context;
    CmCfgEnvironment cfg;

    cm_builtin_context_init(&context);
    context.file = "src/\"main\".rs";
    context.file_length = strlen(context.file);
    context.line = 17;
    context.column = 9;
    context.module_path = "crate::nested";
    context.module_path_length = strlen(context.module_path);
    cm_cfg_environment_init(&cfg);
    cfg.target_family = "unix";
    cfg.target_os = "linux";
    context.cfg = &cfg;

    expect_output("line", CM_BUILTIN_MACRO_LINE, "  ", &context, "17");
    expect_output("column", CM_BUILTIN_MACRO_COLUMN, "", &context, "9");
    expect_output("file", CM_BUILTIN_MACRO_FILE, "", &context,
        "\"src/\\\"main\\\".rs\"");
    expect_output("module-path", CM_BUILTIN_MACRO_MODULE_PATH, "", &context,
        "\"crate::nested\"");
    expect_output("cfg-true", CM_BUILTIN_MACRO_CFG,
        "all(unix, target_os = \"linux\")", &context, "true");
    expect_output("cfg-false", CM_BUILTIN_MACRO_CFG,
        "windows", &context, "false");
    expect_output("stringify", CM_BUILTIN_MACRO_STRINGIFY,
        "  value + \"x\"\n", NULL, "\"value + \\\"x\\\"\"");
    expect_output("stringify-empty", CM_BUILTIN_MACRO_STRINGIFY,
        " \t", NULL, "\"\"");
    expect_output("concat", CM_BUILTIN_MACRO_CONCAT,
        "\"ab\", '-', 42u8, true, \"\\n\",", NULL,
        "\"ab-42u8true\\n\"");
    expect_output("concat-empty", CM_BUILTIN_MACRO_CONCAT,
        "", NULL, "\"\"");
}

static void test_environment(void)
{
    CmBuiltinContext context;

    cm_builtin_context_init(&context);
    context.env_lookup = lookup_env;
    expect_output("env", CM_BUILTIN_MACRO_ENV,
        "\"PRESENT\"", &context, "\"v\\\"\\\\\"");
    expect_output("env-message", CM_BUILTIN_MACRO_ENV,
        "\"PRESENT\", \"must exist\"", &context, "\"v\\\"\\\\\"");
    expect_output("option-some", CM_BUILTIN_MACRO_OPTION_ENV,
        "\"PRESENT\"", &context, "Some(\"v\\\"\\\\\")");
    expect_output("option-none", CM_BUILTIN_MACRO_OPTION_ENV,
        "\"ABSENT\"", &context, "None");
}

static void expect_error(const char *test, CmBuiltinMacroKind kind,
    const char *arguments, const CmBuiltinContext *context,
    CmMacroStatus status, CmMacroDiagnosticCode code)
{
    CmStrBuf output;
    CmMacroExpansion expansion;

    cm_str_buf_init(&output);
    cm_str_buf_append(&output, "seed");
    expansion = expand(kind, arguments, context, &output);
    if (expansion.status != status || expansion.diagnostic.code != code
        || expansion.diagnostic.message == NULL
        || expansion.diagnostic.message[0] == '\0') {
        fail(test, "failure did not carry the expected diagnostic");
    }
    if (output.len != 0)
        fail(test, "failed expansion left fallback output behind");
    cm_str_buf_destroy(&output);
}

static void test_diagnostics(void)
{
    CmBuiltinContext context;
    CmStrBuf output;
    CmMacroExpansion expansion;

    cm_builtin_context_init(&context);
    expect_error("unknown", CM_BUILTIN_MACRO_UNKNOWN, "", NULL,
        CM_MACRO_UNSUPPORTED, CM_MACRO_DIAG_BUILTIN_UNKNOWN);
    expect_error("line-arguments", CM_BUILTIN_MACRO_LINE, "1", &context,
        CM_MACRO_SYNTAX_ERROR, CM_MACRO_DIAG_BUILTIN_EXPECTED_EMPTY);
    expect_error("line-context", CM_BUILTIN_MACRO_LINE, "", &context,
        CM_MACRO_INVALID_ARGUMENT,
        CM_MACRO_DIAG_BUILTIN_INVALID_CONTEXT);
    expect_error("concat-nonliteral", CM_BUILTIN_MACRO_CONCAT,
        "some_name", NULL, CM_MACRO_SYNTAX_ERROR,
        CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL);
    expect_error("env-name", CM_BUILTIN_MACRO_ENV, "PRESENT", &context,
        CM_MACRO_SYNTAX_ERROR, CM_MACRO_DIAG_BUILTIN_EXPECTED_STRING);
    expect_error("env-missing", CM_BUILTIN_MACRO_ENV,
        "\"ABSENT\"", &context, CM_MACRO_ENV_NOT_FOUND,
        CM_MACRO_DIAG_BUILTIN_ENV_MISSING);
    expect_error("option-extra", CM_BUILTIN_MACRO_OPTION_ENV,
        "\"PRESENT\", \"message\"", &context, CM_MACRO_SYNTAX_ERROR,
        CM_MACRO_DIAG_BUILTIN_EXPECTED_STRING);
    expect_error("module-path-context", CM_BUILTIN_MACRO_MODULE_PATH,
        "", &context, CM_MACRO_INVALID_ARGUMENT,
        CM_MACRO_DIAG_BUILTIN_INVALID_CONTEXT);
    expect_error("cfg-context", CM_BUILTIN_MACRO_CFG,
        "unix", &context, CM_MACRO_INVALID_ARGUMENT,
        CM_MACRO_DIAG_BUILTIN_INVALID_CONTEXT);

    cm_str_buf_init(&output);
    cm_str_buf_append(&output, "seed");
    expansion = cm_builtin_macro_expand(CM_BUILTIN_MACRO_LINE,
        NULL, 1, &context, &output);
    if (expansion.status != CM_MACRO_INVALID_ARGUMENT
        || expansion.diagnostic.code != CM_MACRO_DIAG_INVALID_ARGUMENT)
        fail("invalid-argument", "invalid slice was not diagnosed");
    if (output.len != 0)
        fail("invalid-argument", "invalid slice left stale output behind");
    cm_str_buf_destroy(&output);
}

int main(void)
{
    test_classification();
    test_location_and_tokens();
    test_environment();
    test_diagnostics();
    if (failures != 0) {
        fprintf(stderr, "builtin macro tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("builtin macro tests: ok");
    return 0;
}
