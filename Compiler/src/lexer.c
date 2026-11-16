#include "lexer.h"

#include <string.h>

static bool is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static bool is_identifier_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_identifier_part(unsigned char c) {
    return is_identifier_start(c) || is_digit(c);
}

static bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f';
}

AtriLexer atri_lexer_create(AtriSource source) {
    AtriLexer lexer = {.source = source};
    if (source.length >= 3 && memcmp(source.text, "\xEF\xBB\xBF", 3) == 0) {
        lexer.offset = 3;
    }
    return lexer;
}

static AtriToken token(AtriTokenKind kind, size_t start, size_t end) {
    return (AtriToken){.kind = kind, .span = {start, end}};
}

static AtriToken error_token(AtriLexer *lexer, size_t start,
                             AtriDiagnostic *diagnostic,
                             const char *message) {
    AtriToken result = token(ATRI_TOKEN_ERROR, start, lexer->offset);
    *diagnostic = (AtriDiagnostic){.span = result.span, .message = message};
    return result;
}

AtriToken atri_lexer_next(AtriLexer *lexer, AtriDiagnostic *diagnostic) {
    const char *text = lexer->source.text;
    size_t length = lexer->source.length;
    while (lexer->offset < length &&
           is_space((unsigned char)text[lexer->offset])) {
        ++lexer->offset;
    }

    size_t start = lexer->offset;
    if (start == length) {
        return token(ATRI_TOKEN_EOF, start, start);
    }

    unsigned char c = (unsigned char)text[lexer->offset++];
    if (is_identifier_start(c)) {
        while (lexer->offset < length &&
               is_identifier_part((unsigned char)text[lexer->offset])) {
            ++lexer->offset;
        }
        size_t count = lexer->offset - start;
        AtriTokenKind kind = ATRI_TOKEN_IDENTIFIER;
        if (count == 2 && memcmp(text + start, "fn", 2) == 0) {
            kind = ATRI_TOKEN_FN;
        } else if (count == 6 && memcmp(text + start, "return", 6) == 0) {
            kind = ATRI_TOKEN_RETURN;
        } else if (count == 5 && memcmp(text + start, "const", 5) == 0) {
            kind = ATRI_TOKEN_CONST;
        } else if (count == 3 && memcmp(text + start, "let", 3) == 0) {
            kind = ATRI_TOKEN_LET;
        }
        return token(kind, start, lexer->offset);
    }

    if (is_digit(c)) {
        while (lexer->offset < length &&
               is_digit((unsigned char)text[lexer->offset])) {
            ++lexer->offset;
        }
        if (lexer->offset < length &&
            is_identifier_start((unsigned char)text[lexer->offset])) {
            while (lexer->offset < length &&
                   is_identifier_part((unsigned char)text[lexer->offset])) {
                ++lexer->offset;
            }
            return error_token(lexer, start, diagnostic,
                "only decimal integer literals without suffixes or separators "
                "are supported by this parser subset");
        }
        return token(ATRI_TOKEN_INTEGER, start, lexer->offset);
    }

    switch (c) {
        case '(': return token(ATRI_TOKEN_LEFT_PAREN, start, lexer->offset);
        case ')': return token(ATRI_TOKEN_RIGHT_PAREN, start, lexer->offset);
        case '{': return token(ATRI_TOKEN_LEFT_BRACE, start, lexer->offset);
        case '}': return token(ATRI_TOKEN_RIGHT_BRACE, start, lexer->offset);
        case ';': return token(ATRI_TOKEN_SEMICOLON, start, lexer->offset);
        case ':': return token(ATRI_TOKEN_COLON, start, lexer->offset);
        case ',': return token(ATRI_TOKEN_COMMA, start, lexer->offset);
        case '=': return token(ATRI_TOKEN_EQUAL, start, lexer->offset);
        case '+': return token(ATRI_TOKEN_PLUS, start, lexer->offset);
        case '*': return token(ATRI_TOKEN_STAR, start, lexer->offset);
        case '/': return token(ATRI_TOKEN_SLASH, start, lexer->offset);
        case '%': return token(ATRI_TOKEN_PERCENT, start, lexer->offset);
        case '-':
            if (lexer->offset < length && text[lexer->offset] == '>') {
                ++lexer->offset;
                return token(ATRI_TOKEN_ARROW, start, lexer->offset);
            }
            return token(ATRI_TOKEN_MINUS, start, lexer->offset);
        case 0:
            return error_token(lexer, start, diagnostic,
                               "NUL byte is not allowed in source text");
        default: break;
    }

    if (length - start >= 3 &&
        memcmp(text + start, "\xEF\xBC\x9B", 3) == 0) {
        lexer->offset = start + 3;
        return error_token(lexer, start, diagnostic,
                          "full-width semicolon is not allowed; use ASCII ';'");
    }
    return error_token(lexer, start, diagnostic,
                      "unexpected character in this parser subset");
}
