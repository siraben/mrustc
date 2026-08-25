#include "cm/driver/g3_options.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "g3-options/%s: %s\n", test, message);
    failures += 1;
}

static void expect_status(const char *test, int argc,
    const char *const argv[], CmG3OptionsStatus expected,
    const char *expected_option)
{
    CmG3Options options;
    CmG3Options before;
    CmG3OptionsResult result;

    memset(&options, 0xa5, sizeof(options));
    before = options;
    result = cm_g3_options_parse(argc, argv, &options);
    if (result.status != expected) {
        fail(test, cm_g3_options_status_name(result.status));
    }
    if (expected != CM_G3_OPTIONS_OK
        && memcmp(&options, &before, sizeof(options)) != 0) {
        fail(test, "failed parse modified the destination");
    }
    if (expected_option != NULL
        && (result.option == NULL
            || strcmp(result.option, expected_option) != 0)) {
        fail(test, "error did not identify its option");
    }
}

static void test_emit_c_any_order(void)
{
    const char *argv[] = {
        "cmrustc", "--extern-cmrlib", "alpha=one.cmrlib",
        "--target", "x86_64-unknown-linux-gnu", "-o", "out.c",
        "--crate-name", "consumer_1", "--edition", "2021",
        "--emit-c", "consumer.rs", "--extern-cmrlib",
        "beta_2=two.cmrlib"
    };
    CmG3Options options;
    CmG3OptionsResult result;

    result = cm_g3_options_parse((int)CM_ARRAY_LEN(argv), argv, &options);
    if (result.status != CM_G3_OPTIONS_OK) {
        fail("emit-c", cm_g3_options_status_name(result.status));
        return;
    }
    if (options.action != CM_G3_ACTION_EMIT_C
        || options.input_path != argv[12] || options.output_path != argv[6]
        || options.crate_name != argv[8] || options.edition != argv[10]
        || options.target != argv[4] || options.c_compiler != NULL
        || options.extern_count != 2u) {
        fail("emit-c", "parsed fields differ");
    }
    if (options.externs[0].name.data != argv[2]
        || options.externs[0].name.length != 5u
        || strcmp(options.externs[0].path, "one.cmrlib") != 0
        || options.externs[1].name.data != argv[14]
        || options.externs[1].name.length != 6u
        || strcmp(options.externs[1].path, "two.cmrlib") != 0) {
        fail("emit-c", "extern views are not borrowed argv slices");
    }
}

static void test_emit_cmrlib_any_order(void)
{
    const char *argv[] = {
        "cmrustc", "--cc", "tcc", "--target",
        "x86_64-unknown-linux-gnu", "--emit-cmrlib", "producer.rs",
        "--edition", "2015", "--crate-name", "provider", "-o",
        "provider.cmrlib"
    };
    CmG3Options options;
    CmG3OptionsResult result;

    result = cm_g3_options_parse((int)CM_ARRAY_LEN(argv), argv, &options);
    if (result.status != CM_G3_OPTIONS_OK
        || options.action != CM_G3_ACTION_EMIT_CMRLIB
        || options.c_compiler != argv[2] || options.target != argv[4]
        || options.input_path != argv[6]
        || options.extern_count != 0u) {
        fail("emit-cmrlib", cm_g3_options_status_name(result.status));
    }
}

static void test_shape_errors(void)
{
    const char *unknown[] = {
        "cmrustc", "--emit-c", "in.rs", "--crate-name", "x", "-o",
        "x.c", "loose"
    };
    const char *missing_value[] = { "cmrustc", "--emit-c" };
    const char *conflict[] = {
        "cmrustc", "--emit-c", "a.rs", "--emit-cmrlib", "b.rs",
        "--crate-name", "x", "-o", "x"
    };
    const char *duplicate[] = {
        "cmrustc", "--emit-c", "a.rs", "-o", "a.c", "-o", "b.c",
        "--crate-name", "x"
    };
    const char *bad_name[] = {
        "cmrustc", "--emit-c", "a.rs", "-o", "a.c", "--crate-name",
        "not-a-name"
    };
    const char *bad_edition[] = {
        "cmrustc", "--emit-c", "a.rs", "-o", "a.c", "--crate-name",
        "x", "--edition", "2030"
    };

    expect_status("unknown", (int)CM_ARRAY_LEN(unknown), unknown,
        CM_G3_OPTIONS_UNKNOWN_OPTION, "loose");
    expect_status("missing-value", (int)CM_ARRAY_LEN(missing_value),
        missing_value, CM_G3_OPTIONS_MISSING_VALUE, "--emit-c");
    expect_status("conflict", (int)CM_ARRAY_LEN(conflict), conflict,
        CM_G3_OPTIONS_CONFLICTING_ACTION, "--emit-cmrlib");
    expect_status("duplicate", (int)CM_ARRAY_LEN(duplicate), duplicate,
        CM_G3_OPTIONS_DUPLICATE_OPTION, "-o");
    expect_status("bad-name", (int)CM_ARRAY_LEN(bad_name), bad_name,
        CM_G3_OPTIONS_INVALID_IDENTIFIER, "--crate-name");
    expect_status("bad-edition", (int)CM_ARRAY_LEN(bad_edition), bad_edition,
        CM_G3_OPTIONS_INVALID_EDITION, "--edition");
}

static void test_required_and_action_rules(void)
{
    const char *no_action[] = {
        "cmrustc", "--crate-name", "x", "-o", "x.c"
    };
    const char *no_output[] = {
        "cmrustc", "--emit-c", "a.rs", "--crate-name", "x"
    };
    const char *no_name[] = {
        "cmrustc", "--emit-c", "a.rs", "-o", "a.c"
    };
    const char *no_cc[] = {
        "cmrustc", "--emit-cmrlib", "a.rs", "--crate-name", "x", "-o",
        "x.cmrlib"
    };
    const char *cc_on_c[] = {
        "cmrustc", "--emit-c", "a.rs", "--crate-name", "x", "-o",
        "x.c", "--cc", "cc"
    };
    const char *extern_on_rlib[] = {
        "cmrustc", "--emit-cmrlib", "a.rs", "--crate-name", "x", "-o",
        "x.cmrlib", "--cc", "cc", "--extern-cmrlib", "dep=d.cmrlib"
    };

    expect_status("no-action", (int)CM_ARRAY_LEN(no_action), no_action,
        CM_G3_OPTIONS_MISSING_ACTION, NULL);
    expect_status("no-output", (int)CM_ARRAY_LEN(no_output), no_output,
        CM_G3_OPTIONS_MISSING_REQUIRED_OPTION, "-o");
    expect_status("no-name", (int)CM_ARRAY_LEN(no_name), no_name,
        CM_G3_OPTIONS_MISSING_REQUIRED_OPTION, "--crate-name");
    expect_status("no-cc", (int)CM_ARRAY_LEN(no_cc), no_cc,
        CM_G3_OPTIONS_MISSING_REQUIRED_OPTION, "--cc");
    expect_status("cc-on-c", (int)CM_ARRAY_LEN(cc_on_c), cc_on_c,
        CM_G3_OPTIONS_OPTION_NOT_ALLOWED, "--cc");
    expect_status("extern-on-rlib", (int)CM_ARRAY_LEN(extern_on_rlib),
        extern_on_rlib, CM_G3_OPTIONS_OPTION_NOT_ALLOWED,
        "--extern-cmrlib");
}

static void test_extern_errors(void)
{
    const char *malformed[] = {
        "cmrustc", "--emit-c", "a.rs", "--crate-name", "x", "-o",
        "x.c", "--extern-cmrlib", "dep"
    };
    const char *empty_path[] = {
        "cmrustc", "--emit-c", "a.rs", "--crate-name", "x", "-o",
        "x.c", "--extern-cmrlib", "dep="
    };
    const char *bad_name[] = {
        "cmrustc", "--emit-c", "a.rs", "--crate-name", "x", "-o",
        "x.c", "--extern-cmrlib", "9dep=d.cmrlib"
    };
    const char *duplicate[] = {
        "cmrustc", "--emit-c", "a.rs", "--crate-name", "x", "-o",
        "x.c", "--extern-cmrlib", "dep=a.cmrlib", "--extern-cmrlib",
        "dep=b.cmrlib"
    };

    expect_status("extern-shape", (int)CM_ARRAY_LEN(malformed), malformed,
        CM_G3_OPTIONS_INVALID_EXTERN, "--extern-cmrlib");
    expect_status("extern-empty-path", (int)CM_ARRAY_LEN(empty_path),
        empty_path, CM_G3_OPTIONS_INVALID_EXTERN, "--extern-cmrlib");
    expect_status("extern-name", (int)CM_ARRAY_LEN(bad_name), bad_name,
        CM_G3_OPTIONS_INVALID_IDENTIFIER, "--extern-cmrlib");
    expect_status("extern-duplicate", (int)CM_ARRAY_LEN(duplicate),
        duplicate, CM_G3_OPTIONS_DUPLICATE_EXTERN, "--extern-cmrlib");
}

static void test_extern_bound(void)
{
    const char *argv[9u + CM_G3_OPTIONS_MAX_EXTERNS * 2u];
    char values[CM_G3_OPTIONS_MAX_EXTERNS + 1u][32];
    CmG3Options options;
    CmG3Options before;
    CmG3OptionsResult result;
    size_t index;
    size_t argument_count;

    argument_count = 0u;
    argv[argument_count++] = "cmrustc";
    argv[argument_count++] = "--emit-c";
    argv[argument_count++] = "a.rs";
    argv[argument_count++] = "--crate-name";
    argv[argument_count++] = "x";
    argv[argument_count++] = "-o";
    argv[argument_count++] = "x.c";
    for (index = 0u; index <= CM_G3_OPTIONS_MAX_EXTERNS; ++index) {
        (void)snprintf(values[index], sizeof(values[index]), "dep%lu=p",
            (unsigned long)index);
        argv[argument_count++] = "--extern-cmrlib";
        argv[argument_count++] = values[index];
    }
    memset(&options, 0x3c, sizeof(options));
    before = options;
    result = cm_g3_options_parse((int)argument_count, argv, &options);
    if (result.status != CM_G3_OPTIONS_TOO_MANY_EXTERNS
        || result.option == NULL
        || strcmp(result.option, "--extern-cmrlib") != 0) {
        fail("extern-bound", cm_g3_options_status_name(result.status));
    }
    if (memcmp(&options, &before, sizeof(options)) != 0) {
        fail("extern-bound", "overflow failure modified destination");
    }
}

static void test_invalid_api(void)
{
    CmG3Options options;
    const char *argv[] = { "cmrustc" };

    if (cm_g3_options_parse(0, argv, &options).status
            != CM_G3_OPTIONS_INVALID_ARGUMENT
        || cm_g3_options_parse(1, NULL, &options).status
            != CM_G3_OPTIONS_INVALID_ARGUMENT
        || cm_g3_options_parse(1, argv, NULL).status
            != CM_G3_OPTIONS_INVALID_ARGUMENT) {
        fail("api", "invalid API input was accepted");
    }
    if (strcmp(cm_g3_options_status_name((CmG3OptionsStatus)999),
            "unknown g3 options status") != 0) {
        fail("api", "unknown status name differs");
    }
}

int main(void)
{
    test_emit_c_any_order();
    test_emit_cmrlib_any_order();
    test_shape_errors();
    test_required_and_action_rules();
    test_extern_errors();
    test_extern_bound();
    test_invalid_api();
    if (failures != 0) {
        fprintf(stderr, "g3 options tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("g3 options tests: ok");
    return 0;
}
