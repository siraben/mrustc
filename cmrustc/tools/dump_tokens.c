#include "cm/syntax/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dump_context {
    const char *source;
};

static void dump_payload(const char *tag, const char *text, size_t length)
{
    fputs(tag, stdout);
    fputc('\t', stdout);
    if (length != 0)
        fwrite(text, 1, length, stdout);
    fputc('\n', stdout);
}

static int dump_token(void *user, const struct cm_token *token)
{
    const struct dump_context *context;
    const char *text;
    size_t length;

    context = (const struct dump_context *)user;
    text = context->source + token->start;
    length = token->length;

    switch (token->kind) {
    case CM_TOKEN_WHITESPACE:
    case CM_TOKEN_LINE_COMMENT:
    case CM_TOKEN_BLOCK_COMMENT:
        return 0;
    case CM_TOKEN_EOF:
        puts("EOF");
        return 0;
    case CM_TOKEN_IDENT:
        if (token->keyword != CM_KW_NONE &&
            (token->flags & CM_TOKEN_F_WEAK_KEYWORD) == 0u &&
            token->keyword != CM_KW_SELF_TYPE) {
            dump_payload("KW", text, length);
        } else {
            dump_payload("IDENT", text, length);
        }
        return 0;
    case CM_TOKEN_RAW_IDENT:
        if (length >= 2u)
            dump_payload("IDENT", text + 2, length - 2u);
        else
            dump_payload("IDENT", text, length);
        return 0;
    case CM_TOKEN_LIFETIME:
        dump_payload("LIFETIME", text, length);
        return 0;
    case CM_TOKEN_INTEGER:
    case CM_TOKEN_CHAR:
    case CM_TOKEN_BYTE_CHAR:
        puts("INTEGER");
        return 0;
    case CM_TOKEN_FLOAT:
        puts("FLOAT");
        return 0;
    case CM_TOKEN_STRING:
    case CM_TOKEN_RAW_STRING:
        puts("STRING");
        return 0;
    case CM_TOKEN_BYTE_STRING:
    case CM_TOKEN_RAW_BYTE_STRING:
        puts("BYTESTRING");
        return 0;
    case CM_TOKEN_C_STRING:
    case CM_TOKEN_RAW_C_STRING:
        puts("CSTRING");
        return 0;
    case CM_TOKEN_ERROR:
        dump_payload("ERROR", text, length);
        return 0;
    default:
        dump_payload("PUNCT", text, length);
        return 0;
    }
}

static enum cm_edition parse_edition(const char *text)
{
    if (strcmp(text, "2015") == 0)
        return CM_EDITION_2015;
    if (strcmp(text, "2018") == 0)
        return CM_EDITION_2018;
    if (strcmp(text, "2021") == 0)
        return CM_EDITION_2021;
    if (strcmp(text, "2024") == 0)
        return CM_EDITION_2024;
    return (enum cm_edition)0;
}

static char *read_file(const char *path, size_t *length_out)
{
    FILE *file;
    long length;
    char *data;

    file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (char *)malloc((size_t)length + 1u);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    data[length] = 0;
    *length_out = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    struct cm_lexer_options options;
    struct cm_lexer_result result;
    struct dump_context context;
    enum cm_edition edition;
    size_t source_length;
    char *source;

    if (argc != 3) {
        fprintf(stderr, "usage: %s EDITION FILE\n", argv[0]);
        return 2;
    }
    edition = parse_edition(argv[1]);
    if ((int)edition == 0) {
        fprintf(stderr, "unsupported edition: %s\n", argv[1]);
        return 2;
    }
    source = read_file(argv[2], &source_length);
    if (source == NULL) {
        fprintf(stderr, "cannot read: %s\n", argv[2]);
        return 2;
    }

    cm_lexer_options_init(&options);
    options.edition = edition;
    options.emit_whitespace = 0;
    options.emit_comments = 0;
    context.source = source;
    result = cm_lex(source, source_length, &options, dump_token, &context);
    free(source);
    if (result.stopped)
        return 2;
    return result.error_count == 0 ? 0 : 1;
}
