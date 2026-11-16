#ifndef ATRI_LEXER_H
#define ATRI_LEXER_H

#include "frontend.h"

typedef enum {
    ATRI_TOKEN_EOF,
    ATRI_TOKEN_ERROR,
    ATRI_TOKEN_FN,
    ATRI_TOKEN_RETURN,
    ATRI_TOKEN_CONST,
    ATRI_TOKEN_LET,
    ATRI_TOKEN_IDENTIFIER,
    ATRI_TOKEN_INTEGER,
    ATRI_TOKEN_LEFT_PAREN,
    ATRI_TOKEN_RIGHT_PAREN,
    ATRI_TOKEN_LEFT_BRACE,
    ATRI_TOKEN_RIGHT_BRACE,
    ATRI_TOKEN_ARROW,
    ATRI_TOKEN_SEMICOLON,
    ATRI_TOKEN_COLON, ATRI_TOKEN_COMMA, ATRI_TOKEN_EQUAL,
    ATRI_TOKEN_PLUS, ATRI_TOKEN_MINUS, ATRI_TOKEN_STAR,
    ATRI_TOKEN_SLASH, ATRI_TOKEN_PERCENT
} AtriTokenKind;

typedef struct {
    AtriTokenKind kind;
    AtriSpan span;
} AtriToken;

typedef struct {
    AtriSource source;
    size_t offset;
} AtriLexer;

AtriLexer atri_lexer_create(AtriSource source);
AtriToken atri_lexer_next(AtriLexer *lexer, AtriDiagnostic *diagnostic);

#endif
