#include "cm/macro_rules.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "macro_rules/%s: %s\n", test, message);
    failures += 1;
}

static void build_tree(struct cm_token_tree *tree, const char *source)
{
    struct cm_lexer_options options;
    struct cm_token_tree_result result;

    cm_token_tree_init(tree);
    cm_lexer_options_init(&options);
    options.edition = CM_EDITION_2024;
    result = cm_token_tree_build(tree, source, strlen(source), &options);
    if (result.lexer_error_count != 0 || result.delimiter_error_count != 0)
        fail("fixture", "test fixture did not form a valid token tree");
}

static cm_tt_id first_child(const struct cm_token_tree *tree, cm_tt_id parent)
{
    const struct cm_tt_node *node;

    node = cm_token_tree_node(tree, parent);
    return node == NULL ? CM_TT_ID_NONE : node->first_child;
}

static CmMacroRulesParseResult parse_rules(
    CmMacroRulesDefinition *definition,
    struct cm_token_tree *tree,
    const char *source,
    const CmMacroRulesLimits *limits)
{
    build_tree(tree, source);
    cm_macro_rules_definition_init(definition);
    return cm_macro_rules_parse(definition, tree, source, strlen(source),
        first_child(tree, tree->root), limits);
}

static void report_parse_failure(const char *test,
    CmMacroRulesParseResult result)
{
    fprintf(stderr, "macro_rules/%s: parse %s at %lu: %s\n", test,
        cm_macro_status_name(result.status),
        (unsigned long)result.diagnostic.offset,
        result.diagnostic.message);
    failures += 1;
}

static void test_fragment_parser(void)
{
    static const CmMacroFragmentKind expected[] = {
        CM_MACRO_FRAGMENT_IDENT,
        CM_MACRO_FRAGMENT_EXPR,
        CM_MACRO_FRAGMENT_TY,
        CM_MACRO_FRAGMENT_PAT,
        CM_MACRO_FRAGMENT_PATH,
        CM_MACRO_FRAGMENT_TT,
        CM_MACRO_FRAGMENT_ITEM,
        CM_MACRO_FRAGMENT_BLOCK,
        CM_MACRO_FRAGMENT_LITERAL,
        CM_MACRO_FRAGMENT_VIS,
        CM_MACRO_FRAGMENT_LIFETIME,
        CM_MACRO_FRAGMENT_META
    };
    const char *source;
    struct cm_token_tree tree;
    CmMacroRulesDefinition definition;
    CmMacroRulesParseResult result;
    const CmMacroBinding *binding;
    size_t index;

    source = "{ ($i:ident, $e:expr, $t:ty, $p:pat, $path:path, $tt:tt, $item:item, $block:block, $lit:literal, $vis:vis, $life:lifetime, $meta:meta) => { $i }; }";
    result = parse_rules(&definition, &tree, source, NULL);
    if (result.status != CM_MACRO_OK) {
        report_parse_failure("fragments", result);
    } else if (result.arm_count != 1
        || result.binding_count != CM_ARRAY_LEN(expected)) {
        fail("fragments", "wrong arm or binding count");
    } else {
        for (index = 0; index < CM_ARRAY_LEN(expected); index += 1) {
            binding = cm_macro_rules_binding(&definition,
                (CmMacroBindingId)(index + 1));
            if (binding == NULL || binding->fragment != expected[index]
                || strcmp(cm_macro_fragment_kind_name(binding->fragment),
                    "unknown") == 0) {
                fail("fragments", "fragment order is not deterministic");
                break;
            }
        }
    }
    cm_macro_rules_definition_destroy(&definition);
    cm_token_tree_destroy(&tree);
}

static int match_and_transcribe(const char *test,
    const char *rules, const char *input, const char *expected,
    size_t expected_arm, size_t expected_captures,
    size_t *emitted_repetitions)
{
    struct cm_token_tree rule_tree;
    struct cm_token_tree input_tree;
    CmMacroRulesDefinition definition;
    CmMacroRulesParseResult parse_result;
    CmMacroCaptureSet captures;
    CmMacroRulesMatchResult match_result;
    CmMacroRulesTranscribeResult transcribe_result;
    CmStrBuf output;
    int success;

    success = 0;
    parse_result = parse_rules(&definition, &rule_tree, rules, NULL);
    if (parse_result.status != CM_MACRO_OK) {
        report_parse_failure(test, parse_result);
        cm_macro_rules_definition_destroy(&definition);
        cm_token_tree_destroy(&rule_tree);
        return 0;
    }
    build_tree(&input_tree, input);
    cm_macro_capture_set_init(&captures);
    match_result = cm_macro_rules_match(&definition, &input_tree,
        input, strlen(input), first_child(&input_tree, input_tree.root),
        &captures);
    if (match_result.status != CM_MACRO_OK) {
        fprintf(stderr, "macro_rules/%s: match %s at %lu after %lu steps: "
            "%s\n",
            test, cm_macro_status_name(match_result.status),
            (unsigned long)match_result.diagnostic.offset,
            (unsigned long)match_result.backtrack_steps,
            match_result.diagnostic.message);
        failures += 1;
    } else if (match_result.arm_index != expected_arm
        || captures.captures.len != expected_captures) {
        fail(test, "wrong arm or capture count");
    } else {
        cm_str_buf_init(&output);
        transcribe_result = cm_macro_rules_transcribe(
            &definition, &captures, &output);
        if (transcribe_result.status != CM_MACRO_OK) {
            fprintf(stderr, "macro_rules/%s: transcribe %s: %s\n", test,
                cm_macro_status_name(transcribe_result.status),
                transcribe_result.diagnostic.message);
            failures += 1;
        } else if (strcmp(cm_str_buf_c_str(&output), expected) != 0) {
            fprintf(stderr, "macro_rules/%s: expected [%s], got [%s]\n",
                test, expected, cm_str_buf_c_str(&output));
            failures += 1;
        } else {
            if (emitted_repetitions != NULL) {
                *emitted_repetitions =
                    transcribe_result.emitted_repetitions;
            }
            success = 1;
        }
        cm_str_buf_destroy(&output);
    }
    cm_macro_capture_set_destroy(&captures);
    cm_token_tree_destroy(&input_tree);
    cm_macro_rules_definition_destroy(&definition);
    cm_token_tree_destroy(&rule_tree);
    return success;
}

static void test_simple_and_opaque(void)
{
    const char *rules;

    rules = "{ () => { zero }; ($name:ident = $value:literal) => { const $name: i32 = $value; }; }";
    (void)match_and_transcribe("simple", rules, "(ANSWER = 42)",
        "const ANSWER : i32 = 42 ;", 1, 2, NULL);
    (void)match_and_transcribe("opaque-expr",
        "{ ($e:expr) => { $e }; }", "(a + call(1))",
        "a + call(1)", 0, 1, NULL);
    (void)match_and_transcribe("opaque-type-arm",
        "{ ($self_t:ty, unsigned) => { narrow $self_t }; "
        "($self_t:ty, $wide_t:ty, unsigned) => { "
        "wide $self_t $wide_t }; }",
        "(u8, u16, unsigned)", "wide u8 u16", 1, 2, NULL);
    (void)match_and_transcribe("opaque-type-generic-comma",
        "{ ($self_t:ty, unsigned) => { narrow $self_t }; "
        "($self_t:ty, $wide_t:ty, unsigned) => { "
        "wide $self_t $wide_t }; }",
        "(Result<A, B>, unsigned)", "narrow Result<A, B>",
        0, 1, NULL);
    (void)match_and_transcribe("nested-macro-bindings",
        "{ ($outer:ident) => { macro_rules! inner { "
        "($inner:expr) => { pair $outer $inner }; } }; }",
        "(captured)",
        "macro_rules ! inner {($ inner : expr) => "
        "{pair captured $ inner} ;}", 0, 1, NULL);
    (void)match_and_transcribe("nested-macro-shadow",
        "{ ($value:ident) => { macro_rules! inner { "
        "($value:ty) => { shadow $value }; "
        "($other:expr) => { outer $value $other }; } }; }",
        "(captured)",
        "macro_rules ! inner {($ value : ty) => "
        "{shadow $ value} ; ($ other : expr) => "
        "{outer captured $ other} ;}", 0, 1, NULL);
    (void)match_and_transcribe("sip-compress",
        "{ ($state:expr) => { one $state }; "
        "($v0:expr, $v1:expr, $v2:expr, $v3:expr) => { "
        "four $v0 $v1 $v2 $v3 }; }",
        "(s.v0, s.v1, s.v2, s.v3)", "four s.v0 s.v1 s.v2 s.v3", 1, 4, NULL);
    (void)match_and_transcribe("expr-closure-comma",
        "{ ($t:ty, $x:expr, $f:expr) => { arg $t $x $f }; }",
        "(T, x, |_: &T, _| Ok(()))", "arg T x |_: &T, _| Ok(())", 0, 3,
        NULL);
    (void)match_and_transcribe("expr-turbofish-comma",
        "{ ($e:expr, $n:ident) => { got $e $n }; }",
        "(f::<A, B>(1), tail)", "got f::<A, B>(1) tail", 0, 2, NULL);
    (void)match_and_transcribe("negative-literal",
        "{ ($value:literal) => { $value }; }", "(-12)",
        "-12", 0, 1, NULL);
    (void)match_and_transcribe("strict-fragments",
        "{ ($vis:vis struct $name:ident, $life:lifetime, $body:block, $one:tt) => { $vis $name $life $body $one }; }",
        "(pub(crate) struct Thing, 'a, {body}, [x])",
        "pub(crate) Thing 'a {body} [x]", 0, 5, NULL);
    (void)match_and_transcribe("meta-fragment",
        "{ (#[$attr:meta] $name:ident) => { "
        "#[$attr] struct $name; }; }",
        "(#[stable] Thing)", "# [stable] struct Thing ;", 0, 2, NULL);
}

static void test_flat_repetition(void)
{
    const char *rules;
    struct cm_token_tree rule_tree;
    struct cm_token_tree input_tree;
    CmMacroRulesDefinition definition;
    CmMacroRulesParseResult parse_result;
    CmMacroCaptureSet captures;
    CmMacroRulesMatchResult match_result;
    const CmMacroCapture *capture;
    size_t index;

    rules = "{ ($( $x:ident ),*) => { [$( stringify!($x) ),*] }; }";
    if (match_and_transcribe("flat-repeat", rules, "(a,b,c)",
        "[stringify ! (a) , stringify ! (b) , stringify ! (c)]",
        0, 3, NULL)) {
        parse_result = parse_rules(&definition, &rule_tree, rules, NULL);
        if (parse_result.status == CM_MACRO_OK) {
            build_tree(&input_tree, "(a,b,c)");
            cm_macro_capture_set_init(&captures);
            match_result = cm_macro_rules_match(&definition, &input_tree,
                "(a,b,c)", 7,
                first_child(&input_tree, input_tree.root), &captures);
            if (match_result.status == CM_MACRO_OK) {
                for (index = 0; index < 3; index += 1) {
                    capture = cm_macro_capture(&captures, index);
                    if (capture == NULL || capture->repetition_depth != 1
                        || capture->repetition_indices[0] != index) {
                        fail("capture-order",
                            "repetition captures are not source ordered");
                        break;
                    }
                }
            }
            cm_macro_capture_set_destroy(&captures);
            cm_token_tree_destroy(&input_tree);
        }
        cm_macro_rules_definition_destroy(&definition);
        cm_token_tree_destroy(&rule_tree);
    }
    (void)match_and_transcribe("zero-repeat", rules, "()",
        "[]", 0, 0, NULL);
    (void)match_and_transcribe("outer-capture-in-repeat",
        "{ ($head:ident; $( $tail:ident ),*) => { "
        "$( pair $head $tail ),* }; }",
        "(h; a, b)", "pair h a , pair h b", 0, 3, NULL);
    (void)match_and_transcribe("adjacent-generic-types",
        "{ ($( $item:ty )*) => { $( wrap $item ; )* }; }",
        "(Wrapping<i8> Wrapping<Result<u16, E>>)",
        "wrap Wrapping<i8> ; wrap Wrapping<Result<u16, E>> ;",
        0, 2, NULL);
    (void)match_and_transcribe("metavariable-concat",
        "{ ($name:ident) => { fn ${concat(prefix_, $name)}() {} }; }",
        "(item)", "fn prefix_item () {}", 0, 1, NULL);
    (void)match_and_transcribe("metavariable-count-ignore-index",
        "{ ($( $name:ident ),+) => {"
        " const N: usize = ${count($name)};"
        " $( ${ignore($name)} field.${index()}; )+"
        " }; }",
        "(a, b, c)",
        "const N : usize = 3 ; field . 0 ; field . 1 ; field . 2 ;",
        0, 3, NULL);
    (void)match_and_transcribe("crate-metavariable",
        "{ ($name:ident) => { type $name = $crate::module::Value; }; }",
        "(Alias)", "type Alias = crate :: module :: Value ;",
        0, 1, NULL);
    (void)match_and_transcribe("optional-repeat",
        "{ ($( $x:ident )?) => { $( $x )? }; }", "(value)",
        "value", 0, 1, NULL);
    (void)match_and_transcribe("optional-empty",
        "{ ($( $x:ident )?) => { $( $x )? }; }", "()",
        "", 0, 0, NULL);
}

static void test_nested_repetition(void)
{
    size_t emitted;

    emitted = 0;
    if (match_and_transcribe("nested-repeat",
        "{ ($( $( $x:ident ),+ ; )+) => { $([$( $x ),+]);+ }; }",
        "(a,b;c;d,e,f;)",
        "[a , b] ; [c] ; [d , e , f]", 0, 6, &emitted)
        && emitted != 9) {
        fail("nested-repeat", "nested repetition count is not deterministic");
    }
    (void)match_and_transcribe("nested-empty-repeat",
        "{ ($( $(#[$m:meta])* $name:ident; )+) => { "
        "$( $(#[$m])* struct $name; )+ }; }",
        "(One; Two;)", "struct One ; struct Two ;", 0, 2, NULL);
    (void)match_and_transcribe("nested-gapped-repeat",
        "{ ($( $(#[$m:meta])* $name:ident; )+) => { "
        "$( $(#[$m])* struct $name; )+ }; }",
        "(#[first] One; Two; #[third] Three;)",
        "# [first] struct One ; struct Two ; # [third] struct Three ;",
        0, 5, NULL);
    (void)match_and_transcribe("nested-empty-trailing-separator",
        "{ ($( $($x:ident)* ),+) => { $( [ $($x)* ] )+ }; }",
        "(a a, a,)", "[a a] [a]", 0, 3, NULL);
}

static void expect_parse_error(const char *test, const char *source,
    CmMacroRulesLimits *limits, CmMacroDiagnosticCode code)
{
    struct cm_token_tree tree;
    CmMacroRulesDefinition definition;
    CmMacroRulesParseResult result;

    result = parse_rules(&definition, &tree, source, limits);
    if (result.status == CM_MACRO_OK || result.diagnostic.code != code
        || result.diagnostic.message == NULL
        || result.diagnostic.message[0] == '\0') {
        fail(test, "parse failure did not carry the expected diagnostic");
    }
    cm_macro_rules_definition_destroy(&definition);
    cm_token_tree_destroy(&tree);
}

static void test_diagnostics_and_limits(void)
{
    struct cm_token_tree rule_tree;
    struct cm_token_tree input_tree;
    CmMacroRulesDefinition definition;
    CmMacroRulesLimits limits;
    CmMacroRulesParseResult parse_result;
    CmMacroCaptureSet captures;
    CmMacroRulesMatchResult match_result;
    const char *rules;

    expect_parse_error("duplicate",
        "{ ($x:ident $x:tt) => { $x }; }", NULL,
        CM_MACRO_DIAG_RULES_DUPLICATE_BINDING);
    expect_parse_error("fragment",
        "{ ($x:unknown) => { $x }; }", NULL,
        CM_MACRO_DIAG_RULES_UNKNOWN_FRAGMENT);
    expect_parse_error("unknown-binding",
        "{ () => { $missing }; }", NULL,
        CM_MACRO_DIAG_RULES_UNKNOWN_BINDING);
    expect_parse_error("question-separator",
        "{ ($( $x:ident ),?) => { $x }; }", NULL,
        CM_MACRO_DIAG_RULES_INVALID_SEPARATOR);

    cm_macro_rules_limits_init(&limits);
    limits.max_nesting = 1;
    expect_parse_error("nesting-limit",
        "{ ((($x:ident))) => { $x }; }", &limits,
        CM_MACRO_DIAG_RULES_NESTING_LIMIT);

    rules = "{ ($e:expr) => { $e }; }";
    cm_macro_rules_limits_init(&limits);
    limits.max_backtrack_steps = 1;
    parse_result = parse_rules(&definition, &rule_tree, rules, &limits);
    build_tree(&input_tree, "(a + b)");
    cm_macro_capture_set_init(&captures);
    match_result = cm_macro_rules_match(&definition, &input_tree,
        "(a + b)", 7, first_child(&input_tree, input_tree.root), &captures);
    if (parse_result.status != CM_MACRO_OK
        || match_result.status != CM_MACRO_LIMIT_EXCEEDED
        || match_result.diagnostic.code
            != CM_MACRO_DIAG_RULES_BACKTRACK_LIMIT
        || match_result.backtrack_steps != 1) {
        fail("backtrack-limit", "backtracking limit was not exact");
    }
    cm_macro_capture_set_destroy(&captures);
    cm_token_tree_destroy(&input_tree);
    cm_macro_rules_definition_destroy(&definition);
    cm_token_tree_destroy(&rule_tree);

    rules = "{ (expected) => { ok }; }";
    parse_result = parse_rules(&definition, &rule_tree, rules, NULL);
    build_tree(&input_tree, "(other)");
    cm_macro_capture_set_init(&captures);
    match_result = cm_macro_rules_match(&definition, &input_tree,
        "(other)", 7, first_child(&input_tree, input_tree.root), &captures);
    if (parse_result.status != CM_MACRO_OK
        || match_result.status != CM_MACRO_NO_MATCH
        || match_result.diagnostic.code != CM_MACRO_DIAG_RULES_NO_MATCH) {
        fail("no-match", "ordinary mismatch was not distinguished from error");
    }
    cm_macro_capture_set_destroy(&captures);
    cm_token_tree_destroy(&input_tree);
    cm_macro_rules_definition_destroy(&definition);
    cm_token_tree_destroy(&rule_tree);
}

int main(void)
{
    test_fragment_parser();
    test_simple_and_opaque();
    test_flat_repetition();
    test_nested_repetition();
    test_diagnostics_and_limits();
    if (failures != 0) {
        fprintf(stderr, "macro_rules tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("macro_rules tests: ok");
    return 0;
}
