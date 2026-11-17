#include "lexer.h"

#include <string.h>

static bool is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex_digit(unsigned char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
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
    for (;;) {
        while (lexer->offset < length && is_space((unsigned char)text[lexer->offset])) { ++lexer->offset; }
        if (lexer->offset + 1 >= length || text[lexer->offset] != '/' || text[lexer->offset+1] != '/') { break; }
        while (lexer->offset < length && text[lexer->offset] != '\n' && text[lexer->offset] != '\r') { ++lexer->offset; }
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
        if (count == 3 && memcmp(text + start, "run", 3) == 0) kind = ATRI_TOKEN_RUN;
        static const struct { const char *name; AtriTokenKind kind; } keywords[] = {
            {"cast", ATRI_TOKEN_CAST}, {"sizeof", ATRI_TOKEN_SIZEOF}, {"alignof", ATRI_TOKEN_ALIGNOF},
            {"struct", ATRI_TOKEN_STRUCT}, {"import", ATRI_TOKEN_IMPORT}, {"export", ATRI_TOKEN_EXPORT}, {"fn", ATRI_TOKEN_FN}, {"return", ATRI_TOKEN_RETURN}, {"null", ATRI_TOKEN_NULL},
            {"true", ATRI_TOKEN_TRUE}, {"false", ATRI_TOKEN_FALSE},
            {"if", ATRI_TOKEN_IF}, {"else", ATRI_TOKEN_ELSE}, {"while", ATRI_TOKEN_WHILE},
            {"do", ATRI_TOKEN_DO}, {"for", ATRI_TOKEN_FOR}, {"loop", ATRI_TOKEN_LOOP},
            {"break", ATRI_TOKEN_BREAK}, {"continue", ATRI_TOKEN_CONTINUE}
        };
        for (size_t i = 0; i < sizeof(keywords) / sizeof(*keywords); ++i)
            if (count == strlen(keywords[i].name) && memcmp(text + start, keywords[i].name, count) == 0) {
                kind = keywords[i].kind; break;
            }
        return token(kind, start, lexer->offset);
    }

    if (is_digit(c)) {
        while (lexer->offset < length &&
               is_digit((unsigned char)text[lexer->offset])) {
            ++lexer->offset;
        }
        if (lexer->offset + 1 < length && text[lexer->offset] == '.' && is_digit((unsigned char)text[lexer->offset+1])) {
            ++lexer->offset;
            while (lexer->offset < length && is_digit((unsigned char)text[lexer->offset])) { ++lexer->offset; }
            return error_token(lexer, start, diagnostic, "floating-point literals are not supported yet");
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
        case '"':
            while (lexer->offset < length) {
                unsigned char byte = (unsigned char)text[lexer->offset++];
                if (byte == '"') {
                    return token(ATRI_TOKEN_STRING, start, lexer->offset);
                }
                if (byte == 0) {
                    return error_token(lexer, start, diagnostic, "NUL byte is not allowed in source text");
                }
                if (byte == '\r' || byte == '\n') {
                    return error_token(lexer, start, diagnostic, "newline is not allowed in a string literal; use \\n");
                }
                if (byte != '\\') {
                    continue;
                }
                if (lexer->offset == length) {
                    break;
                }
                byte = (unsigned char)text[lexer->offset++];
                if (byte == 0) {
                    return error_token(lexer, start, diagnostic, "NUL byte is not allowed in source text");
                }
                if (byte == 'x') {
                    if (length - lexer->offset < 2 || !is_hex_digit((unsigned char)text[lexer->offset]) ||
                        !is_hex_digit((unsigned char)text[lexer->offset+1])) {
                        return error_token(lexer, start, diagnostic, "string escape \\x requires exactly two hexadecimal digits");
                    }
                    lexer->offset += 2;
                } else if (strchr("0nrtabfv\\\"'", byte) == NULL) {
                    return error_token(lexer, start, diagnostic, "unsupported string escape");
                }
            }
            return error_token(lexer, start, diagnostic, "unterminated string literal");
        case '[':
            return token(ATRI_TOKEN_LEFT_BRACKET, start, lexer->offset);
        case ']':
            return token(ATRI_TOKEN_RIGHT_BRACKET, start, lexer->offset);
        case '?': return token(ATRI_TOKEN_QUESTION, start, lexer->offset);
        case '(': return token(ATRI_TOKEN_LEFT_PAREN, start, lexer->offset);
        case ')': return token(ATRI_TOKEN_RIGHT_PAREN, start, lexer->offset);
        case '{': return token(ATRI_TOKEN_LEFT_BRACE, start, lexer->offset);
        case '}': return token(ATRI_TOKEN_RIGHT_BRACE, start, lexer->offset);
        case ';': return token(ATRI_TOKEN_SEMICOLON, start, lexer->offset);
        case ':':
            if (lexer->offset < length && text[lexer->offset] == ':') {
                ++lexer->offset; return token(ATRI_TOKEN_SCOPE, start, lexer->offset);
            }
            if (lexer->offset < length && text[lexer->offset] == '=') {
                ++lexer->offset;
                return token(ATRI_TOKEN_CONSTANT_INIT, start, lexer->offset);
            }
            return token(ATRI_TOKEN_COLON, start, lexer->offset);
        case '.':
            if (lexer->offset + 1 < length && text[lexer->offset] == '.' && text[lexer->offset+1] == '.') {
                lexer->offset += 2; return token(ATRI_TOKEN_ELLIPSIS, start, lexer->offset);
            }
            return token(ATRI_TOKEN_DOT, start, lexer->offset);
        case ',': return token(ATRI_TOKEN_COMMA, start, lexer->offset);
        case '=':
            if (lexer->offset < length && text[lexer->offset] == '=') {
                ++lexer->offset; return token(ATRI_TOKEN_EQ, start, lexer->offset);
            }
            return token(ATRI_TOKEN_EQUAL, start, lexer->offset);
        case '!':
            if (lexer->offset < length && text[lexer->offset] == '=') {
                ++lexer->offset; return token(ATRI_TOKEN_NE, start, lexer->offset);
            }
            return token(ATRI_TOKEN_NOT, start, lexer->offset);
        case '<': case '>': {
            bool equal = lexer->offset < length && text[lexer->offset] == '=';
            if (equal) ++lexer->offset;
            return token(c == '<' ? (equal ? ATRI_TOKEN_LE : ATRI_TOKEN_LT) :
                                   (equal ? ATRI_TOKEN_GE : ATRI_TOKEN_GT), start, lexer->offset);
        }
        case '+': return token(ATRI_TOKEN_PLUS, start, lexer->offset);
        case '*': return token(ATRI_TOKEN_STAR, start, lexer->offset);
        case '/': return token(ATRI_TOKEN_SLASH, start, lexer->offset);
        case '%': return token(ATRI_TOKEN_PERCENT, start, lexer->offset);
        case '&':
            if (lexer->offset < length && text[lexer->offset] == '&') {
                ++lexer->offset; return token(ATRI_TOKEN_AND, start, lexer->offset);
            }
            return token(ATRI_TOKEN_AMPERSAND, start, lexer->offset);
        case '|':
            if (lexer->offset < length && text[lexer->offset] == '|') {
                ++lexer->offset; return token(ATRI_TOKEN_OR, start, lexer->offset);
            }
            break;
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
