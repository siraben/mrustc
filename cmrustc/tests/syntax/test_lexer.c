#include "cm/syntax/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

struct token_buffer {
    struct cm_token tokens[256];
    size_t count;
};

static int failures;

static int collect_token(void *user, const struct cm_token *token)
{
    struct token_buffer *buffer;

    buffer = (struct token_buffer *)user;
    if (buffer->count >= ARRAY_COUNT(buffer->tokens))
        return 1;
    buffer->tokens[buffer->count++] = *token;
    return 0;
}

static void fail_at(const char *test, size_t index, const char *message)
{
    fprintf(stderr, "%s[%lu]: %s\n", test, (unsigned long)index, message);
    failures++;
}

static struct cm_lexer_result lex_text(const char *source,
    enum cm_edition edition, int trivia, struct token_buffer *buffer)
{
    struct cm_lexer_options options;

    memset(buffer, 0, sizeof(*buffer));
    cm_lexer_options_init(&options);
    options.edition = edition;
    options.emit_whitespace = trivia;
    options.emit_comments = trivia;
    return cm_lex(source, strlen(source), &options, collect_token, buffer);
}

static int token_text_eq(const char *source, const struct cm_token *token,
    const char *expected)
{
    size_t n;

    n = strlen(expected);
    return token->length == n &&
        memcmp(source + token->start, expected, n) == 0;
}

static void expect_kind(const char *test, const struct token_buffer *buffer,
    size_t index, enum cm_token_kind kind)
{
    if (index >= buffer->count) {
        fail_at(test, index, "missing token");
        return;
    }
    if (buffer->tokens[index].kind != kind) {
        char message[160];
        sprintf(message, "expected %s, got %s",
            cm_token_kind_name(kind),
            cm_token_kind_name(buffer->tokens[index].kind));
        fail_at(test, index, message);
    }
}

static void test_nested_comments(void)
{
    const char *source;
    struct token_buffer buffer;
    struct cm_lexer_result result;

    source = "/** outer /* nested /* deep */ nested */ done */x";
    result = lex_text(source, CM_EDITION_2021, 1, &buffer);
    if (result.error_count != 0)
        fail_at("nested_comments", 0, "unexpected lexer error");
    expect_kind("nested_comments", &buffer, 0, CM_TOKEN_BLOCK_COMMENT);
    expect_kind("nested_comments", &buffer, 1, CM_TOKEN_IDENT);
    if ((buffer.tokens[0].flags & CM_TOKEN_F_OUTER_DOC) == 0)
        fail_at("nested_comments", 0, "outer doc flag missing");

    source = "/* not closed /* nested */";
    result = lex_text(source, CM_EDITION_2021, 1, &buffer);
    if (result.error_count != 1)
        fail_at("nested_comments", 2, "unterminated comment not diagnosed");
    if ((buffer.tokens[0].flags & CM_TOKEN_F_UNTERMINATED) == 0)
        fail_at("nested_comments", 2, "unterminated flag missing");
}

static void test_strings(void)
{
    const char *source;
    const enum cm_token_kind expected[] = {
        CM_TOKEN_STRING,
        CM_TOKEN_BYTE_STRING,
        CM_TOKEN_C_STRING,
        CM_TOKEN_RAW_STRING,
        CM_TOKEN_RAW_BYTE_STRING,
        CM_TOKEN_RAW_C_STRING,
        CM_TOKEN_EOF
    };
    struct token_buffer buffer;
    struct cm_lexer_result result;
    size_t i;

    source = "\"a\\\"b\" b\"bytes\" c\"cstr\" r#\"raw \\\"# "
        "br##\"byte # raw\"## cr###\"C ## raw\"###";
    result = lex_text(source, CM_EDITION_2024, 0, &buffer);
    if (result.error_count != 0)
        fail_at("strings", 0, "unexpected lexer error");
    for (i = 0; i < ARRAY_COUNT(expected); i++)
        expect_kind("strings", &buffer, i, expected[i]);
    if (buffer.tokens[3].detail != 1 || buffer.tokens[4].detail != 2 ||
        buffer.tokens[5].detail != 3)
        fail_at("strings", 3, "raw delimiter hash count is wrong");

    source = "r###\"unterminated\"##";
    result = lex_text(source, CM_EDITION_2024, 0, &buffer);
    if (result.error_count != 1 ||
        (buffer.tokens[0].flags & CM_TOKEN_F_UNTERMINATED) == 0)
        fail_at("strings", 7, "unterminated raw string not diagnosed");
}

static void test_lifetimes_and_chars(void)
{
    const char *source;
    const enum cm_token_kind expected[] = {
        CM_TOKEN_LIFETIME,
        CM_TOKEN_LIFETIME,
        CM_TOKEN_LIFETIME,
        CM_TOKEN_CHAR,
        CM_TOKEN_CHAR,
        CM_TOKEN_BYTE_CHAR,
        CM_TOKEN_LIFETIME,
        CM_TOKEN_COLON,
        CM_TOKEN_EOF
    };
    struct token_buffer buffer;
    size_t i;

    source = "'a 'static '_ 'x' '\\n' b'z' 'label:";
    lex_text(source, CM_EDITION_2021, 0, &buffer);
    for (i = 0; i < ARRAY_COUNT(expected); i++)
        expect_kind("lifetimes_chars", &buffer, i, expected[i]);
}

static void test_unicode_lifetime_and_char_discrimination(void)
{
    const char *source;
    const enum cm_token_kind expected[] = {
        CM_TOKEN_CHAR,
        CM_TOKEN_CHAR,
        CM_TOKEN_LIFETIME,
        CM_TOKEN_EOF
    };
    struct token_buffer buffer;
    struct cm_lexer_result result;
    size_t i;

    source = "'Σ' '🦀' 'α";
    result = lex_text(source, CM_EDITION_2024, 0, &buffer);
    if (result.error_count != 0u)
        fail_at("unicode_lifetime_char", 0,
            "unexpected lexer error");
    for (i = 0u; i < ARRAY_COUNT(expected); ++i)
        expect_kind("unicode_lifetime_char", &buffer, i, expected[i]);
}

static void test_macro_signature_lifetimes(void)
{
    const char *source;
    const enum cm_token_kind expected[] = {
        CM_TOKEN_IDENT,
        CM_TOKEN_IDENT,
        CM_TOKEN_IDENT,
        CM_TOKEN_LT,
        CM_TOKEN_LIFETIME,
        CM_TOKEN_GT,
        CM_TOKEN_LPAREN,
        CM_TOKEN_AMP,
        CM_TOKEN_LIFETIME,
        CM_TOKEN_IDENT,
        CM_TOKEN_IDENT,
        CM_TOKEN_COMMA,
        CM_TOKEN_IDENT,
        CM_TOKEN_COLON,
        CM_TOKEN_AMP,
        CM_TOKEN_DOLLAR,
        CM_TOKEN_IDENT,
        CM_TOKEN_RPAREN,
        CM_TOKEN_THIN_ARROW,
        CM_TOKEN_AMP,
        CM_TOKEN_LIFETIME,
        CM_TOKEN_IDENT,
        CM_TOKEN_DOLLAR,
        CM_TOKEN_IDENT,
        CM_TOKEN_LBRACE,
        CM_TOKEN_EOF
    };
    struct token_buffer buffer;
    struct cm_lexer_result result;
    size_t i;

    source = "pub fn add<'a>(&'a mut self, other: &$name) -> "
        "&'a mut $name {";
    result = lex_text(source, CM_EDITION_2024, 0, &buffer);
    if (result.error_count != 0u)
        fail_at("macro_signature_lifetimes", 0u,
            "exact bignum signature produced a lexer error");
    for (i = 0u; i < ARRAY_COUNT(expected); ++i)
        expect_kind("macro_signature_lifetimes", &buffer, i, expected[i]);
    if (!token_text_eq(source, &buffer.tokens[4], "'a")
        || !token_text_eq(source, &buffer.tokens[8], "'a")
        || !token_text_eq(source, &buffer.tokens[20], "'a")) {
        fail_at("macro_signature_lifetimes", 4u,
            "signature lifetimes did not retain exact token spans");
    }
}

static void test_numbers(void)
{
    const char *source;
    const enum cm_token_kind expected[] = {
        CM_TOKEN_INTEGER,
        CM_TOKEN_INTEGER,
        CM_TOKEN_INTEGER,
        CM_TOKEN_INTEGER,
        CM_TOKEN_FLOAT,
        CM_TOKEN_FLOAT,
        CM_TOKEN_INTEGER,
        CM_TOKEN_DOT_DOT,
        CM_TOKEN_INTEGER,
        CM_TOKEN_EOF
    };
    const char *suffixes[] = { "u8", "u16", "u32", "usize", "f32", "f64" };
    struct token_buffer buffer;
    size_t i;

    source = "0b101u8 0o77u16 0xff_u32 12_3usize 1.0f32 2e3f64 1..2";
    lex_text(source, CM_EDITION_2021, 0, &buffer);
    for (i = 0; i < ARRAY_COUNT(expected); i++)
        expect_kind("numbers", &buffer, i, expected[i]);
    for (i = 0; i < ARRAY_COUNT(suffixes); i++) {
        const struct cm_token *token;

        token = &buffer.tokens[i];
        if (token->suffix_start == SIZE_MAX ||
            memcmp(source + token->suffix_start, suffixes[i],
                strlen(suffixes[i])) != 0)
            fail_at("numbers_suffix", i, "suffix offset is wrong");
    }
    if (buffer.tokens[0].detail != 2 || buffer.tokens[1].detail != 8 ||
        buffer.tokens[2].detail != 16 || buffer.tokens[3].detail != 10)
        fail_at("numbers", 0, "numeric base is wrong");
}

static void test_tuple_projection_numbers(void)
{
    const char *source;
    const enum cm_token_kind expected[] = {
        CM_TOKEN_IDENT, CM_TOKEN_DOT, CM_TOKEN_INTEGER, CM_TOKEN_DOT,
        CM_TOKEN_INTEGER,
        CM_TOKEN_IDENT, CM_TOKEN_DOT, CM_TOKEN_INTEGER, CM_TOKEN_DOT,
        CM_TOKEN_IDENT,
        CM_TOKEN_IDENT, CM_TOKEN_DOT, CM_TOKEN_INTEGER, CM_TOKEN_DOT,
        CM_TOKEN_INTEGER,
        CM_TOKEN_FLOAT, CM_TOKEN_EOF
    };
    struct token_buffer buffer;
    size_t index;

    source = "value.0.1 value.0.field value . /* keep */ 0 . 1 1.0";
    lex_text(source, CM_EDITION_2024, 0, &buffer);
    for (index = 0u; index < ARRAY_COUNT(expected); ++index)
        expect_kind("tuple_projection_numbers", &buffer, index,
            expected[index]);
    if (!token_text_eq(source, &buffer.tokens[2], "0")
        || !token_text_eq(source, &buffer.tokens[4], "1")
        || !token_text_eq(source, &buffer.tokens[7], "0")
        || !token_text_eq(source, &buffer.tokens[12], "0")
        || !token_text_eq(source, &buffer.tokens[14], "1")
        || !token_text_eq(source, &buffer.tokens[15], "1.0")) {
        fail_at("tuple_projection_numbers", 0u,
            "tuple selectors or ordinary float spans were incorrect");
    }
}

static void test_punctuation(void)
{
    const char *source;
    const enum cm_token_kind expected[] = {
        CM_TOKEN_PATH_SEP, CM_TOKEN_THIN_ARROW, CM_TOKEN_FAT_ARROW,
        CM_TOKEN_DOT_DOT, CM_TOKEN_DOT_DOT_EQ, CM_TOKEN_DOT_DOT_DOT,
        CM_TOKEN_EQ_EQ, CM_TOKEN_NOT_EQ, CM_TOKEN_LT_EQ, CM_TOKEN_GT_EQ,
        CM_TOKEN_AMP_AMP, CM_TOKEN_PIPE_PIPE,
        CM_TOKEN_PLUS_EQ, CM_TOKEN_MINUS_EQ, CM_TOKEN_STAR_EQ,
        CM_TOKEN_SLASH_EQ, CM_TOKEN_PERCENT_EQ, CM_TOKEN_CARET_EQ,
        CM_TOKEN_AMP_EQ, CM_TOKEN_PIPE_EQ,
        CM_TOKEN_SHL, CM_TOKEN_SHR, CM_TOKEN_SHL_EQ, CM_TOKEN_SHR_EQ,
        CM_TOKEN_THIN_ARROW_LEFT,
        CM_TOKEN_EOF
    };
    struct token_buffer buffer;
    size_t i;

    source = ":: -> => .. ..= ... == != <= >= && || += -= *= /= %= ^= "
        "&= |= << >> <<= >>= <-";
    lex_text(source, CM_EDITION_2021, 0, &buffer);
    for (i = 0; i < ARRAY_COUNT(expected); i++)
        expect_kind("punctuation", &buffer, i, expected[i]);
}

static void test_identifiers_and_keywords(void)
{
    const char *source;
    struct token_buffer buffer;

    source = "fn async await dyn try gen union raw safe macro_rules r#fn";
    lex_text(source, CM_EDITION_2015, 0, &buffer);
    if (buffer.tokens[0].keyword != CM_KW_FN ||
        buffer.tokens[1].keyword != CM_KW_NONE ||
        buffer.tokens[2].keyword != CM_KW_NONE ||
        buffer.tokens[3].keyword != CM_KW_DYN ||
        buffer.tokens[4].keyword != CM_KW_NONE ||
        buffer.tokens[5].keyword != CM_KW_NONE)
        fail_at("keywords_2015", 0, "edition classification is wrong");
    if ((buffer.tokens[3].flags & CM_TOKEN_F_WEAK_KEYWORD) == 0)
        fail_at("keywords_2015", 3, "2015 dyn should be contextual");
    expect_kind("raw_identifier", &buffer, 10, CM_TOKEN_RAW_IDENT);
    if (buffer.tokens[10].keyword != CM_KW_NONE ||
        !token_text_eq(source, &buffer.tokens[10], "r#fn"))
        fail_at("raw_identifier", 10, "raw identifier metadata is wrong");

    lex_text(source, CM_EDITION_2024, 0, &buffer);
    if (buffer.tokens[1].keyword != CM_KW_ASYNC ||
        buffer.tokens[2].keyword != CM_KW_AWAIT ||
        buffer.tokens[3].keyword != CM_KW_DYN ||
        buffer.tokens[4].keyword != CM_KW_TRY ||
        buffer.tokens[5].keyword != CM_KW_GEN)
        fail_at("keywords_2024", 0, "edition classification is wrong");
    if ((buffer.tokens[5].flags & CM_TOKEN_F_RESERVED_KEYWORD) == 0 ||
        buffer.tokens[6].keyword != CM_KW_UNION ||
        (buffer.tokens[6].flags & CM_TOKEN_F_WEAK_KEYWORD) == 0)
        fail_at("keywords_2024", 5, "reserved/weak flags are wrong");
}

static void test_sink_stop(void)
{
    struct token_buffer buffer;
    struct cm_lexer_result result;

    memset(&buffer, 0, sizeof(buffer));
    /* A full buffer makes the callback stop on its first invocation. */
    buffer.count = ARRAY_COUNT(buffer.tokens);
    result = cm_lex("fn main", 7, NULL, collect_token, &buffer);
    if (!result.stopped || result.token_count != 1)
        fail_at("sink_stop", 0, "callback did not stop lexing");
}

static void test_fixture(const char *path)
{
    FILE *file;
    long length;
    char *source;
    struct token_buffer buffer;
    struct cm_lexer_result result;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "fixture: cannot open %s\n", path);
        failures++;
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        failures++;
        return;
    }
    source = (char *)malloc((size_t)length + 1u);
    if (source == NULL) {
        fclose(file);
        failures++;
        return;
    }
    if (fread(source, 1, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(source);
        failures++;
        return;
    }
    fclose(file);
    source[length] = 0;
    result = lex_text(source, CM_EDITION_2024, 0, &buffer);
    if (result.error_count != 0 || result.stopped)
        fail_at("fixture", 0, "fixture did not lex cleanly");
    free(source);
}

int main(int argc, char **argv)
{
    const char *fixture;

    fixture = argc > 1 ? argv[1] :
        "cmrustc/tests/syntax/fixtures/lexer_cases.rs";
    test_nested_comments();
    test_strings();
    test_lifetimes_and_chars();
    test_unicode_lifetime_and_char_discrimination();
    test_macro_signature_lifetimes();
    test_numbers();
    test_tuple_projection_numbers();
    test_punctuation();
    test_identifiers_and_keywords();
    test_sink_stop();
    test_fixture(fixture);
    if (failures != 0) {
        fprintf(stderr, "%d lexer test(s) failed\n", failures);
        return 1;
    }
    puts("syntax lexer tests: ok");
    return 0;
}
