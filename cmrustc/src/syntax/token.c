#include "cm/syntax/token.h"

#include <string.h>

struct cm_keyword_entry {
    const char *text;
    enum cm_keyword keyword;
    unsigned char category;
};

enum {
    CM_KCAT_STRICT = 0,
    CM_KCAT_2018,
    CM_KCAT_TRY_2018,
    CM_KCAT_GEN_2024,
    CM_KCAT_RESERVED,
    CM_KCAT_WEAK
};

static const struct cm_keyword_entry cm_keywords[] = {
    { "as", CM_KW_AS, CM_KCAT_STRICT },
    { "break", CM_KW_BREAK, CM_KCAT_STRICT },
    { "const", CM_KW_CONST, CM_KCAT_STRICT },
    { "continue", CM_KW_CONTINUE, CM_KCAT_STRICT },
    { "crate", CM_KW_CRATE, CM_KCAT_STRICT },
    { "else", CM_KW_ELSE, CM_KCAT_STRICT },
    { "enum", CM_KW_ENUM, CM_KCAT_STRICT },
    { "extern", CM_KW_EXTERN, CM_KCAT_STRICT },
    { "false", CM_KW_FALSE, CM_KCAT_STRICT },
    { "fn", CM_KW_FN, CM_KCAT_STRICT },
    { "for", CM_KW_FOR, CM_KCAT_STRICT },
    { "if", CM_KW_IF, CM_KCAT_STRICT },
    { "impl", CM_KW_IMPL, CM_KCAT_STRICT },
    { "in", CM_KW_IN, CM_KCAT_STRICT },
    { "let", CM_KW_LET, CM_KCAT_STRICT },
    { "loop", CM_KW_LOOP, CM_KCAT_STRICT },
    { "match", CM_KW_MATCH, CM_KCAT_STRICT },
    { "mod", CM_KW_MOD, CM_KCAT_STRICT },
    { "move", CM_KW_MOVE, CM_KCAT_STRICT },
    { "mut", CM_KW_MUT, CM_KCAT_STRICT },
    { "pub", CM_KW_PUB, CM_KCAT_STRICT },
    { "ref", CM_KW_REF, CM_KCAT_STRICT },
    { "return", CM_KW_RETURN, CM_KCAT_STRICT },
    { "self", CM_KW_SELF_VALUE, CM_KCAT_STRICT },
    { "Self", CM_KW_SELF_TYPE, CM_KCAT_STRICT },
    { "static", CM_KW_STATIC, CM_KCAT_STRICT },
    { "struct", CM_KW_STRUCT, CM_KCAT_STRICT },
    { "super", CM_KW_SUPER, CM_KCAT_STRICT },
    { "trait", CM_KW_TRAIT, CM_KCAT_STRICT },
    { "true", CM_KW_TRUE, CM_KCAT_STRICT },
    { "type", CM_KW_TYPE, CM_KCAT_STRICT },
    { "unsafe", CM_KW_UNSAFE, CM_KCAT_STRICT },
    { "use", CM_KW_USE, CM_KCAT_STRICT },
    { "where", CM_KW_WHERE, CM_KCAT_STRICT },
    { "while", CM_KW_WHILE, CM_KCAT_STRICT },
    { "async", CM_KW_ASYNC, CM_KCAT_2018 },
    { "await", CM_KW_AWAIT, CM_KCAT_2018 },
    { "dyn", CM_KW_DYN, CM_KCAT_2018 },
    { "abstract", CM_KW_ABSTRACT, CM_KCAT_RESERVED },
    { "become", CM_KW_BECOME, CM_KCAT_RESERVED },
    { "box", CM_KW_BOX, CM_KCAT_RESERVED },
    { "do", CM_KW_DO, CM_KCAT_RESERVED },
    { "final", CM_KW_FINAL, CM_KCAT_RESERVED },
    { "gen", CM_KW_GEN, CM_KCAT_GEN_2024 },
    { "macro", CM_KW_MACRO, CM_KCAT_RESERVED },
    { "override", CM_KW_OVERRIDE, CM_KCAT_RESERVED },
    { "priv", CM_KW_PRIV, CM_KCAT_RESERVED },
    { "try", CM_KW_TRY, CM_KCAT_TRY_2018 },
    { "typeof", CM_KW_TYPEOF, CM_KCAT_RESERVED },
    { "unsized", CM_KW_UNSIZED, CM_KCAT_RESERVED },
    { "virtual", CM_KW_VIRTUAL, CM_KCAT_RESERVED },
    { "yield", CM_KW_YIELD, CM_KCAT_RESERVED },
    { "macro_rules", CM_KW_MACRO_RULES, CM_KCAT_WEAK },
    { "raw", CM_KW_RAW, CM_KCAT_WEAK },
    { "safe", CM_KW_SAFE, CM_KCAT_WEAK },
    { "union", CM_KW_UNION, CM_KCAT_WEAK }
};

enum cm_keyword cm_keyword_classify(const char *text, size_t length,
    enum cm_edition edition, uint32_t *flags_out)
{
    size_t i;
    uint32_t flags;

    flags = 0;
    for (i = 0; i < sizeof(cm_keywords) / sizeof(cm_keywords[0]); i++) {
        const struct cm_keyword_entry *entry;
        size_t entry_length;

        entry = &cm_keywords[i];
        entry_length = strlen(entry->text);
        if (entry_length != length || memcmp(entry->text, text, length) != 0)
            continue;

        switch (entry->category) {
        case CM_KCAT_2018:
            if (edition < CM_EDITION_2018) {
                if (entry->keyword != CM_KW_DYN)
                    return CM_KW_NONE;
                flags |= CM_TOKEN_F_WEAK_KEYWORD;
            } else {
                flags |= CM_TOKEN_F_EDITION_KEYWORD;
            }
            break;
        case CM_KCAT_TRY_2018:
            if (edition < CM_EDITION_2018)
                return CM_KW_NONE;
            flags |= CM_TOKEN_F_RESERVED_KEYWORD |
                CM_TOKEN_F_EDITION_KEYWORD;
            break;
        case CM_KCAT_GEN_2024:
            if (edition < CM_EDITION_2024)
                return CM_KW_NONE;
            flags |= CM_TOKEN_F_RESERVED_KEYWORD |
                CM_TOKEN_F_EDITION_KEYWORD;
            break;
        case CM_KCAT_RESERVED:
            flags |= CM_TOKEN_F_RESERVED_KEYWORD;
            break;
        case CM_KCAT_WEAK:
            flags |= CM_TOKEN_F_WEAK_KEYWORD;
            break;
        default:
            break;
        }

        if (flags_out != NULL)
            *flags_out |= flags;
        return entry->keyword;
    }
    return CM_KW_NONE;
}

const char *cm_keyword_name(enum cm_keyword keyword)
{
    size_t i;

    if (keyword == CM_KW_NONE)
        return "";
    for (i = 0; i < sizeof(cm_keywords) / sizeof(cm_keywords[0]); i++) {
        if (cm_keywords[i].keyword == keyword)
            return cm_keywords[i].text;
    }
    return "<unknown-keyword>";
}

#define CM_TOKEN_NAME_CASE(name) case CM_TOKEN_##name: return #name

const char *cm_token_kind_name(enum cm_token_kind kind)
{
    switch (kind) {
    CM_TOKEN_NAME_CASE(EOF);
    CM_TOKEN_NAME_CASE(ERROR);
    CM_TOKEN_NAME_CASE(WHITESPACE);
    CM_TOKEN_NAME_CASE(LINE_COMMENT);
    CM_TOKEN_NAME_CASE(BLOCK_COMMENT);
    CM_TOKEN_NAME_CASE(IDENT);
    CM_TOKEN_NAME_CASE(RAW_IDENT);
    CM_TOKEN_NAME_CASE(LIFETIME);
    CM_TOKEN_NAME_CASE(INTEGER);
    CM_TOKEN_NAME_CASE(FLOAT);
    CM_TOKEN_NAME_CASE(CHAR);
    CM_TOKEN_NAME_CASE(BYTE_CHAR);
    CM_TOKEN_NAME_CASE(STRING);
    CM_TOKEN_NAME_CASE(BYTE_STRING);
    CM_TOKEN_NAME_CASE(C_STRING);
    CM_TOKEN_NAME_CASE(RAW_STRING);
    CM_TOKEN_NAME_CASE(RAW_BYTE_STRING);
    CM_TOKEN_NAME_CASE(RAW_C_STRING);
    CM_TOKEN_NAME_CASE(LPAREN);
    CM_TOKEN_NAME_CASE(RPAREN);
    CM_TOKEN_NAME_CASE(LBRACE);
    CM_TOKEN_NAME_CASE(RBRACE);
    CM_TOKEN_NAME_CASE(LBRACKET);
    CM_TOKEN_NAME_CASE(RBRACKET);
    CM_TOKEN_NAME_CASE(COMMA);
    CM_TOKEN_NAME_CASE(SEMICOLON);
    CM_TOKEN_NAME_CASE(COLON);
    CM_TOKEN_NAME_CASE(PATH_SEP);
    CM_TOKEN_NAME_CASE(DOT);
    CM_TOKEN_NAME_CASE(DOT_DOT);
    CM_TOKEN_NAME_CASE(DOT_DOT_DOT);
    CM_TOKEN_NAME_CASE(DOT_DOT_EQ);
    CM_TOKEN_NAME_CASE(AT);
    CM_TOKEN_NAME_CASE(POUND);
    CM_TOKEN_NAME_CASE(DOLLAR);
    CM_TOKEN_NAME_CASE(QUESTION);
    CM_TOKEN_NAME_CASE(TILDE);
    CM_TOKEN_NAME_CASE(APOSTROPHE);
    CM_TOKEN_NAME_CASE(EQ);
    CM_TOKEN_NAME_CASE(EQ_EQ);
    CM_TOKEN_NAME_CASE(FAT_ARROW);
    CM_TOKEN_NAME_CASE(BANG);
    CM_TOKEN_NAME_CASE(NOT_EQ);
    CM_TOKEN_NAME_CASE(LT);
    CM_TOKEN_NAME_CASE(LT_EQ);
    CM_TOKEN_NAME_CASE(SHL);
    CM_TOKEN_NAME_CASE(SHL_EQ);
    CM_TOKEN_NAME_CASE(GT);
    CM_TOKEN_NAME_CASE(GT_EQ);
    CM_TOKEN_NAME_CASE(SHR);
    CM_TOKEN_NAME_CASE(SHR_EQ);
    CM_TOKEN_NAME_CASE(PLUS);
    CM_TOKEN_NAME_CASE(PLUS_EQ);
    CM_TOKEN_NAME_CASE(MINUS);
    CM_TOKEN_NAME_CASE(MINUS_EQ);
    CM_TOKEN_NAME_CASE(THIN_ARROW);
    CM_TOKEN_NAME_CASE(THIN_ARROW_LEFT);
    CM_TOKEN_NAME_CASE(STAR);
    CM_TOKEN_NAME_CASE(STAR_EQ);
    CM_TOKEN_NAME_CASE(SLASH);
    CM_TOKEN_NAME_CASE(SLASH_EQ);
    CM_TOKEN_NAME_CASE(PERCENT);
    CM_TOKEN_NAME_CASE(PERCENT_EQ);
    CM_TOKEN_NAME_CASE(CARET);
    CM_TOKEN_NAME_CASE(CARET_EQ);
    CM_TOKEN_NAME_CASE(AMP);
    CM_TOKEN_NAME_CASE(AMP_AMP);
    CM_TOKEN_NAME_CASE(AMP_EQ);
    CM_TOKEN_NAME_CASE(PIPE);
    CM_TOKEN_NAME_CASE(PIPE_PIPE);
    CM_TOKEN_NAME_CASE(PIPE_EQ);
    }
    return "<unknown-token>";
}

#undef CM_TOKEN_NAME_CASE
