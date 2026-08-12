#ifndef CM_SYNTAX_LEXER_H
#define CM_SYNTAX_LEXER_H

#include <stddef.h>
#include "cm/syntax/token.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cm_lexer_options {
    enum cm_edition edition;
    int emit_whitespace;
    int emit_comments;
};

struct cm_lexer_result {
    size_t token_count;
    size_t error_count;
    int stopped;
};

/* Return nonzero from a sink to stop lexing cleanly. */
typedef int (*cm_token_sink_fn)(void *user, const struct cm_token *token);

void cm_lexer_options_init(struct cm_lexer_options *options);

struct cm_lexer_result cm_lex(const char *source, size_t source_length,
    const struct cm_lexer_options *options, cm_token_sink_fn sink, void *user);

#ifdef __cplusplus
}
#endif

#endif
