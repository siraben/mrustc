#ifndef CM_SYNTAX_TOKEN_H
#define CM_SYNTAX_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum cm_edition {
    CM_EDITION_2015 = 2015,
    CM_EDITION_2018 = 2018,
    CM_EDITION_2021 = 2021,
    CM_EDITION_2024 = 2024
};

/*
 * Keywords are recorded as metadata on identifier tokens.  Keeping the token
 * kind as IDENT mirrors Rust's contextual-keyword rules and lets the parser
 * decide whether weak keywords are active at a particular grammar position.
 */
enum cm_keyword {
    CM_KW_NONE = 0,
    CM_KW_AS,
    CM_KW_BREAK,
    CM_KW_CONST,
    CM_KW_CONTINUE,
    CM_KW_CRATE,
    CM_KW_ELSE,
    CM_KW_ENUM,
    CM_KW_EXTERN,
    CM_KW_FALSE,
    CM_KW_FN,
    CM_KW_FOR,
    CM_KW_IF,
    CM_KW_IMPL,
    CM_KW_IN,
    CM_KW_LET,
    CM_KW_LOOP,
    CM_KW_MATCH,
    CM_KW_MOD,
    CM_KW_MOVE,
    CM_KW_MUT,
    CM_KW_PUB,
    CM_KW_REF,
    CM_KW_RETURN,
    CM_KW_SELF_VALUE,
    CM_KW_SELF_TYPE,
    CM_KW_STATIC,
    CM_KW_STRUCT,
    CM_KW_SUPER,
    CM_KW_TRAIT,
    CM_KW_TRUE,
    CM_KW_TYPE,
    CM_KW_UNSAFE,
    CM_KW_USE,
    CM_KW_WHERE,
    CM_KW_WHILE,

    /* Strict from Rust 2018. */
    CM_KW_ASYNC,
    CM_KW_AWAIT,
    CM_KW_DYN,

    /* Reserved words. */
    CM_KW_ABSTRACT,
    CM_KW_BECOME,
    CM_KW_BOX,
    CM_KW_DO,
    CM_KW_FINAL,
    CM_KW_GEN,
    CM_KW_MACRO,
    CM_KW_OVERRIDE,
    CM_KW_PRIV,
    CM_KW_TRY,
    CM_KW_TYPEOF,
    CM_KW_UNSIZED,
    CM_KW_VIRTUAL,
    CM_KW_YIELD,

    /* Context-sensitive (weak) keywords. */
    CM_KW_MACRO_RULES,
    CM_KW_RAW,
    CM_KW_SAFE,
    CM_KW_UNION
};

enum cm_token_kind {
    CM_TOKEN_EOF = 0,
    CM_TOKEN_ERROR,
    CM_TOKEN_WHITESPACE,
    CM_TOKEN_LINE_COMMENT,
    CM_TOKEN_BLOCK_COMMENT,
    CM_TOKEN_IDENT,
    CM_TOKEN_RAW_IDENT,
    CM_TOKEN_LIFETIME,
    CM_TOKEN_INTEGER,
    CM_TOKEN_FLOAT,
    CM_TOKEN_CHAR,
    CM_TOKEN_BYTE_CHAR,
    CM_TOKEN_STRING,
    CM_TOKEN_BYTE_STRING,
    CM_TOKEN_C_STRING,
    CM_TOKEN_RAW_STRING,
    CM_TOKEN_RAW_BYTE_STRING,
    CM_TOKEN_RAW_C_STRING,

    CM_TOKEN_LPAREN,
    CM_TOKEN_RPAREN,
    CM_TOKEN_LBRACE,
    CM_TOKEN_RBRACE,
    CM_TOKEN_LBRACKET,
    CM_TOKEN_RBRACKET,
    CM_TOKEN_COMMA,
    CM_TOKEN_SEMICOLON,
    CM_TOKEN_COLON,
    CM_TOKEN_PATH_SEP,
    CM_TOKEN_DOT,
    CM_TOKEN_DOT_DOT,
    CM_TOKEN_DOT_DOT_DOT,
    CM_TOKEN_DOT_DOT_EQ,
    CM_TOKEN_AT,
    CM_TOKEN_POUND,
    CM_TOKEN_DOLLAR,
    CM_TOKEN_QUESTION,
    CM_TOKEN_TILDE,
    CM_TOKEN_APOSTROPHE,

    CM_TOKEN_EQ,
    CM_TOKEN_EQ_EQ,
    CM_TOKEN_FAT_ARROW,
    CM_TOKEN_BANG,
    CM_TOKEN_NOT_EQ,
    CM_TOKEN_LT,
    CM_TOKEN_LT_EQ,
    CM_TOKEN_SHL,
    CM_TOKEN_SHL_EQ,
    CM_TOKEN_GT,
    CM_TOKEN_GT_EQ,
    CM_TOKEN_SHR,
    CM_TOKEN_SHR_EQ,
    CM_TOKEN_PLUS,
    CM_TOKEN_PLUS_EQ,
    CM_TOKEN_MINUS,
    CM_TOKEN_MINUS_EQ,
    CM_TOKEN_THIN_ARROW,
    CM_TOKEN_THIN_ARROW_LEFT,
    CM_TOKEN_STAR,
    CM_TOKEN_STAR_EQ,
    CM_TOKEN_SLASH,
    CM_TOKEN_SLASH_EQ,
    CM_TOKEN_PERCENT,
    CM_TOKEN_PERCENT_EQ,
    CM_TOKEN_CARET,
    CM_TOKEN_CARET_EQ,
    CM_TOKEN_AMP,
    CM_TOKEN_AMP_AMP,
    CM_TOKEN_AMP_EQ,
    CM_TOKEN_PIPE,
    CM_TOKEN_PIPE_PIPE,
    CM_TOKEN_PIPE_EQ
};

enum cm_token_flags {
    CM_TOKEN_F_NONE = 0,
    CM_TOKEN_F_UNTERMINATED = 1u << 0,
    CM_TOKEN_F_INVALID = 1u << 1,
    CM_TOKEN_F_INNER_DOC = 1u << 2,
    CM_TOKEN_F_OUTER_DOC = 1u << 3,
    CM_TOKEN_F_WEAK_KEYWORD = 1u << 4,
    CM_TOKEN_F_RESERVED_KEYWORD = 1u << 5,
    CM_TOKEN_F_EDITION_KEYWORD = 1u << 6
};

struct cm_token {
    enum cm_token_kind kind;
    enum cm_keyword keyword;
    size_t start;
    size_t length;
    size_t line;
    size_t column;

    /* SIZE_MAX when absent; otherwise an absolute byte offset. */
    size_t suffix_start;

    uint32_t flags;

    /* Numeric base for integers, raw-string hash count for raw strings. */
    uint32_t detail;
};

const char *cm_token_kind_name(enum cm_token_kind kind);
const char *cm_keyword_name(enum cm_keyword keyword);

/* Classifies an exact identifier spelling. Raw identifiers bypass this. */
enum cm_keyword cm_keyword_classify(const char *text, size_t length,
    enum cm_edition edition, uint32_t *flags_out);

#ifdef __cplusplus
}
#endif

#endif
