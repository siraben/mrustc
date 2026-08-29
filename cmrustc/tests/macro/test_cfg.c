#include "cm/macro.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "cfg/%s: %s\n", test, message);
    failures += 1;
}

static CmCfgEvaluation evaluate(const CmCfgEnvironment *environment,
    const char *predicate)
{
    return cm_cfg_evaluate(environment, predicate, strlen(predicate));
}

static void expect_value(const CmCfgEnvironment *environment,
    const char *test, const char *predicate, int expected)
{
    CmCfgEvaluation evaluation;

    evaluation = evaluate(environment, predicate);
    if (evaluation.status != CM_MACRO_OK) {
        fprintf(stderr, "cfg/%s: unexpected %s at %lu: %s\n", test,
            cm_macro_status_name(evaluation.status),
            (unsigned long)evaluation.diagnostic.offset,
            evaluation.diagnostic.message);
        failures += 1;
    } else if (evaluation.value != expected) {
        fail(test, "predicate produced the wrong value");
    }
}

static void test_target_features(void)
{
    static const char *features[] = { "serde", "std" };
    static const char *target_features[] = { "sse2", "avx" };
    static const CmCfgEntry entries[] = {
        { "test", NULL },
        { "panic", "abort" }
    };
    CmCfgEnvironment environment;

    cm_cfg_environment_init(&environment);
    environment.target_arch = "x86_64";
    environment.target_os = "linux";
    environment.target_abi = "";
    environment.target_env = "gnu";
    environment.target_vendor = "unknown";
    environment.target_family = "unix";
    environment.target_pointer_width = "64";
    environment.target_endian = "little";
    environment.features = features;
    environment.feature_count = CM_ARRAY_LEN(features);
    environment.target_features = target_features;
    environment.target_feature_count = CM_ARRAY_LEN(target_features);
    environment.entries = entries;
    environment.entry_count = CM_ARRAY_LEN(entries);

    expect_value(&environment, "all", "all(unix, target_arch = \"x86_64\", target_os=\"linux\", target_env=\"gnu\", target_abi=\"\", target_vendor=\"unknown\", target_pointer_width=\"64\", target_endian=\"little\")", 1);
    expect_value(&environment, "any", "any(feature=\"missing\", feature = \"serde\")", 1);
    expect_value(&environment, "not", "not(windows)", 1);
    expect_value(&environment, "target-feature", "target_feature = \"avx\"", 1);
    expect_value(&environment, "custom-word", "test", 1);
    expect_value(&environment, "custom-value-presence", "panic", 1);
    expect_value(&environment, "custom-value", "panic = \"abort\"", 1);
    expect_value(&environment, "unknown", "unknown_cfg", 0);
    expect_value(&environment, "wrong-value", "target_os = \"windows\"", 0);
    expect_value(&environment, "empty-all", "all()", 1);
    expect_value(&environment, "empty-any", "any()", 0);
    expect_value(&environment, "trailing-comma", "all(unix,)", 1);
    expect_value(&environment, "line-comment",
        "all(\n    unix, // FIXME(ctest): comment inside the predicate\n"
        "    not(windows),\n)", 1);
    expect_value(&environment, "block-comment",
        "any(/* first */ windows, /* second */ unix)", 1);
}

static void test_cfg_attr_decision(void)
{
    CmCfgEnvironment environment;
    CmCfgEvaluation evaluation;
    const char *predicate;

    cm_cfg_environment_init(&environment);
    environment.target_family = "unix";
    predicate = "all(unix, not(windows))";
    evaluation = cm_cfg_attr_decide(&environment,
        predicate, strlen(predicate));
    if (evaluation.status != CM_MACRO_OK || evaluation.value != 1)
        fail("cfg-attr-true", "active cfg_attr was not selected");
    predicate = "any(windows, target_os = \"windows\")";
    evaluation = cm_cfg_attr_decide(&environment,
        predicate, strlen(predicate));
    if (evaluation.status != CM_MACRO_OK || evaluation.value != 0)
        fail("cfg-attr-false", "inactive cfg_attr was selected");
}

static void expect_error(const CmCfgEnvironment *environment,
    const char *test, const char *predicate,
    CmMacroDiagnosticCode expected_code)
{
    CmCfgEvaluation evaluation;

    evaluation = evaluate(environment, predicate);
    if (evaluation.status == CM_MACRO_OK
        || evaluation.diagnostic.code != expected_code
        || evaluation.diagnostic.message == NULL
        || evaluation.diagnostic.message[0] == '\0') {
        fail(test, "cfg error did not carry the expected diagnostic");
    }
}

static void test_diagnostics(void)
{
    CmCfgEnvironment environment;
    CmCfgEvaluation evaluation;

    cm_cfg_environment_init(&environment);
    expect_error(&environment, "not-arity", "not(unix, windows)",
        CM_MACRO_DIAG_CFG_NOT_ARITY);
    expect_error(&environment, "unknown-operator", "xor(unix, windows)",
        CM_MACRO_DIAG_CFG_UNKNOWN_OPERATOR);
    expect_error(&environment, "missing-string", "target_os = linux",
        CM_MACRO_DIAG_CFG_EXPECTED_STRING);
    expect_error(&environment, "invalid-escape", "feature = \"bad\\q\"",
        CM_MACRO_DIAG_CFG_INVALID_ESCAPE);
    expect_error(&environment, "trailing", "unix extra",
        CM_MACRO_DIAG_CFG_TRAILING_INPUT);

    evaluation = cm_cfg_evaluate(NULL, "unix", 4);
    if (evaluation.status != CM_MACRO_INVALID_ARGUMENT
        || evaluation.diagnostic.code != CM_MACRO_DIAG_INVALID_ARGUMENT)
        fail("invalid-argument", "invalid environment was not diagnosed");
}

int main(void)
{
    test_target_features();
    test_cfg_attr_decision();
    test_diagnostics();
    if (failures != 0) {
        fprintf(stderr, "cfg macro tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("cfg macro tests: ok");
    return 0;
}
