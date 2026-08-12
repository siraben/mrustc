/*
 * Developer-only adapter around mrustc's existing lexer.  This file is not in
 * cmrustc's trusted C build and does not modify or replace the oracle binary.
 */
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "parse/tokenstream.hpp"
#include "target_version.hpp"

/* main.cpp is intentionally not part of bin/mrustc.a. Some pulled-in oracle
 * objects reference its target selector, even though the lexer does not. */
TargetVersion gTargetVersion = TargetVersion::Rustc1_90;

#define private public
#include "parse/lex.hpp"
#undef private

static std::ostream *canonical_output;

static AST::Edition parse_edition(const char *text)
{
    const std::string value(text);
    if (value == "2015") return AST::Edition::Rust2015;
    if (value == "2018") return AST::Edition::Rust2018;
    if (value == "2021") return AST::Edition::Rust2021;
    if (value == "2024") return AST::Edition::Rust2024;
    throw std::runtime_error("unsupported edition: " + value);
}

static std::string punctuation_text(const Token& token)
{
    switch (token.type()) {
    case TOK_DASH_EQUAL: return "-=";
    case TOK_DOUBLE_LT_EQUAL: return "<<=";
    case TOK_DOUBLE_GT_EQUAL: return ">>=";
    default: return token.to_str();
    }
}

static void dump_token(const Token& token)
{
    const eTokenType type = token.type();

    switch (type) {
    case TOK_NEWLINE:
    case TOK_WHITESPACE:
    case TOK_COMMENT:
        return;
    case TOK_EOF:
        *canonical_output << "EOF\n";
        return;
    case TOK_IDENT:
        *canonical_output << "IDENT\t" << token.ident().name << "\n";
        return;
    case TOK_UNDERSCORE:
        *canonical_output << "IDENT\t_\n";
        return;
    case TOK_LIFETIME:
        *canonical_output << "LIFETIME\t'" << token.ident().name << "\n";
        return;
    case TOK_INTEGER:
    case TOK_CHAR:
        *canonical_output << "INTEGER\n";
        return;
    case TOK_FLOAT:
        *canonical_output << "FLOAT\n";
        return;
    case TOK_STRING:
        *canonical_output << "STRING\n";
        return;
    case TOK_BYTESTRING:
        *canonical_output << "BYTESTRING\n";
        return;
    case TOK_CSTRING:
        *canonical_output << "CSTRING\n";
        return;
    default:
        if (Token::type_is_rword(type)) {
            if (type == TOK_RWORD_YIELD)
                *canonical_output << "KW\tyield\n";
            else
                *canonical_output << "KW\t" << token.to_str() << "\n";
        } else {
            *canonical_output << "PUNCT\t" << punctuation_text(token) << "\n";
        }
        return;
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " EDITION FILE OUTPUT\n";
        return 2;
    }
    try {
        std::ofstream output(argv[3]);
        if (!output)
            throw std::runtime_error("cannot open output file");
        canonical_output = &output;
        ParseState state;
        Lexer lexer(argv[2], parse_edition(argv[1]), state);
        for (;;) {
            Token token = lexer.getTokenInt();
            dump_token(token);
            if (token.type() == TOK_EOF)
                break;
        }
    }
    catch (const std::exception& error) {
        std::cerr << "mrustc lexer: " << error.what() << "\n";
        return 1;
    }
    catch (const char *error) {
        std::cerr << "mrustc lexer: " << error << "\n";
        return 1;
    }
    catch (...) {
        std::cerr << "mrustc lexer: unknown exception\n";
        return 1;
    }
    return 0;
}
