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
        if (lexer->offset + 1 >= length || text[lexer->offset] != '/') { break; }
        if (text[lexer->offset+1] == '/') {
            while (lexer->offset < length && text[lexer->offset] != '\n' && text[lexer->offset] != '\r') { ++lexer->offset; }
        } else if (text[lexer->offset+1] == '*') {
            size_t comment = lexer->offset;
            lexer->offset += 2;
            while (lexer->offset + 1 < length && !(text[lexer->offset] == '*' && text[lexer->offset+1] == '/')) { ++lexer->offset; }
            if (lexer->offset + 1 >= length) {
                lexer->offset = length;
                return error_token(lexer,comment,diagnostic,"unterminated block comment");
            }
            lexer->offset += 2;
        } else { break; }
    }
    size_t start = lexer->offset;
    if (start == length) {
        return token(ATRI_TOKEN_EOF, start, start);
    }

    if (lexer->scope && lexer->scope->count) {
        const AtriOperator *op = atri_match_operator(lexer->scope,text+start,length-start);
        if (op) {
            lexer->offset += op->size;
            return (AtriToken){.kind=ATRI_TOKEN_CUSTOM,.span={start,lexer->offset},.custom=op};
        }
    }
    unsigned char c = (unsigned char)text[lexer->offset++];
    if (is_identifier_start(c)) {
        while (lexer->offset < length &&
               is_identifier_part((unsigned char)text[lexer->offset])) {
            ++lexer->offset;
        }
        size_t count = lexer->offset - start;
        AtriTokenKind kind = ATRI_TOKEN_IDENTIFIER;
        static const struct { const char *name; AtriTokenKind kind; } keywords[] = {
            {"enum", ATRI_TOKEN_ENUM}, {"switch", ATRI_TOKEN_SWITCH}, {"case", ATRI_TOKEN_CASE}, {"default", ATRI_TOKEN_DEFAULT},
            {"cast", ATRI_TOKEN_CAST}, {"sizeof", ATRI_TOKEN_SIZEOF}, {"alignof", ATRI_TOKEN_ALIGNOF},
            {"struct", ATRI_TOKEN_STRUCT}, {"union", ATRI_TOKEN_UNION}, {"import", ATRI_TOKEN_IMPORT}, {"export", ATRI_TOKEN_EXPORT}, {"fn", ATRI_TOKEN_FN}, {"return", ATRI_TOKEN_RETURN}, {"null", ATRI_TOKEN_NULL},
            {"true", ATRI_TOKEN_TRUE}, {"false", ATRI_TOKEN_FALSE},
            {"if", ATRI_TOKEN_IF}, {"else", ATRI_TOKEN_ELSE}, {"while", ATRI_TOKEN_WHILE},
            {"do", ATRI_TOKEN_DO}, {"for", ATRI_TOKEN_FOR}, {"loop", ATRI_TOKEN_LOOP},
            {"break", ATRI_TOKEN_BREAK}, {"continue", ATRI_TOKEN_CONTINUE}, {"defer", ATRI_TOKEN_DEFER}
        };
        for (size_t i = 0; i < sizeof(keywords) / sizeof(*keywords); ++i)
            if (count == strlen(keywords[i].name) && memcmp(text + start, keywords[i].name, count) == 0) {
                kind = keywords[i].kind; break;
            }
        return token(kind, start, lexer->offset);
    }

    if (is_digit(c)) {
        unsigned base = 10;
        if (c == '0' && lexer->offset < length) {
            unsigned char prefix = (unsigned char)text[lexer->offset];
            if (prefix == 'x' || prefix == 'X') { base = 16; ++lexer->offset; }
            else if (prefix == 'b' || prefix == 'B') { base = 2; ++lexer->offset; }
        }
        bool digit_before = base == 10;
        while (lexer->offset < length) {
            unsigned char byte = (unsigned char)text[lexer->offset];
            bool digit = base == 16 ? is_hex_digit(byte) : base == 2 ? byte == '0' || byte == '1' : is_digit(byte);
            if (digit) { digit_before = true; ++lexer->offset; continue; }
            if (byte == '_') {
                if (!digit_before) { return error_token(lexer, start, diagnostic, "integer separator must occur between digits"); }
                digit_before = false; ++lexer->offset; continue;
            }
            if (is_identifier_part(byte)) {
                ++lexer->offset;
                return error_token(lexer, start, diagnostic, "invalid integer digit or unsupported integer suffix");
            }
            break;
        }
        if (!digit_before) { return error_token(lexer, start, diagnostic, "integer requires digits with separators only between digits"); }
        if (base == 10 && lexer->offset + 1 < length && text[lexer->offset] == '.' && is_digit((unsigned char)text[lexer->offset+1])) {
            ++lexer->offset;
            while (lexer->offset < length && is_digit((unsigned char)text[lexer->offset])) { ++lexer->offset; }
            return error_token(lexer, start, diagnostic, "floating-point literals are not supported yet");
        }
        return token(ATRI_TOKEN_INTEGER, start, lexer->offset);
    }

    switch (c) {
        case '@': return token(ATRI_TOKEN_ANNOTATION, start, lexer->offset);
        case '#': {
            while (lexer->offset < length && is_space((unsigned char)text[lexer->offset])) { ++lexer->offset; }
            size_t name = lexer->offset;
            while (lexer->offset < length && is_identifier_part((unsigned char)text[lexer->offset])) { ++lexer->offset; }
            size_t count = lexer->offset-name;
            static const struct { const char *name; AtriTokenKind kind; } directives[] = {
                {"insert", ATRI_TOKEN_INSERT}, {"keyword", ATRI_TOKEN_KEYWORD}, {"run", ATRI_TOKEN_RUN}, {"extern", ATRI_TOKEN_EXTERN}, {"through", ATRI_TOKEN_THROUGH}
            };
            for (size_t i = 0; i < sizeof(directives)/sizeof(*directives); ++i) {
                if (count == strlen(directives[i].name) && !memcmp(text+name, directives[i].name, count)) {
                    return token(directives[i].kind, start, lexer->offset);
                }
            }
            return error_token(lexer, start, diagnostic, "unknown compiler directive after '#'");
        }
        case '\'': case '"': {
            unsigned char quote = c;
            size_t bytes = 0;
            while (lexer->offset < length) {
                unsigned char byte = (unsigned char)text[lexer->offset++];
                if (byte == quote) {
                    if (quote == '\'' && bytes != 1) { return error_token(lexer, start, diagnostic, "character literal requires exactly one decoded byte"); }
                    return token(quote == '\'' ? ATRI_TOKEN_CHARACTER : ATRI_TOKEN_STRING, start, lexer->offset);
                }
                ++bytes;
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
                    if (length-lexer->offset < 2 || !is_hex_digit((unsigned char)text[lexer->offset]) ||
                        !is_hex_digit((unsigned char)text[lexer->offset+1])) {
                        return error_token(lexer, start, diagnostic, "escape \\x requires exactly two hexadecimal digits");
                    }
                    lexer->offset += 2;
                } else if (strchr("0nrtabfv\\\"'", byte) == NULL) {
                    return error_token(lexer, start, diagnostic, "unsupported string escape; octal and binary escapes are not supported");
                }
            }
            return error_token(lexer, start, diagnostic, "unterminated string or character literal");
        }
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
            if (lexer->offset < length && text[lexer->offset] == c) {
                ++lexer->offset;
                bool assign = lexer->offset < length && text[lexer->offset] == '=';
                if (assign) { ++lexer->offset; }
                return token(c == '<' ? (assign ? ATRI_TOKEN_SHL_ASSIGN : ATRI_TOKEN_SHL) :
                    (assign ? ATRI_TOKEN_SHR_ASSIGN : ATRI_TOKEN_SHR), start, lexer->offset);
            }
            bool equal = lexer->offset < length && text[lexer->offset] == '=';
            if (equal) { ++lexer->offset; }
            return token(c == '<' ? (equal ? ATRI_TOKEN_LE : ATRI_TOKEN_LT) :
                (equal ? ATRI_TOKEN_GE : ATRI_TOKEN_GT), start, lexer->offset);
        }
        case '+': case '*': case '/': case '%': case '^': {
            bool assign = lexer->offset < length && text[lexer->offset] == '=';
            if (assign) { ++lexer->offset; }
            AtriTokenKind kind = c == '+' ? (assign ? ATRI_TOKEN_ADD_ASSIGN : ATRI_TOKEN_PLUS) :
                c == '*' ? (assign ? ATRI_TOKEN_MUL_ASSIGN : ATRI_TOKEN_STAR) :
                c == '/' ? (assign ? ATRI_TOKEN_DIV_ASSIGN : ATRI_TOKEN_SLASH) :
                c == '%' ? (assign ? ATRI_TOKEN_REM_ASSIGN : ATRI_TOKEN_PERCENT) :
                (assign ? ATRI_TOKEN_XOR_ASSIGN : ATRI_TOKEN_CARET);
            return token(kind, start, lexer->offset);
        }
        case '~': return token(ATRI_TOKEN_TILDE, start, lexer->offset);
        case '&':
            if (lexer->offset < length && text[lexer->offset] == '&') {
                ++lexer->offset; return token(ATRI_TOKEN_AND, start, lexer->offset);
            }
            if (lexer->offset < length && text[lexer->offset] == '=') {
                ++lexer->offset; return token(ATRI_TOKEN_AND_ASSIGN, start, lexer->offset);
            }
            return token(ATRI_TOKEN_AMPERSAND, start, lexer->offset);
        case '|':
            if (lexer->offset < length && text[lexer->offset] == '|') {
                ++lexer->offset; return token(ATRI_TOKEN_OR, start, lexer->offset);
            }
            if (lexer->offset < length && text[lexer->offset] == '=') {
                ++lexer->offset; return token(ATRI_TOKEN_OR_ASSIGN, start, lexer->offset);
            }
            return token(ATRI_TOKEN_PIPE, start, lexer->offset);
        case '-':
            if (lexer->offset < length && text[lexer->offset] == '>') {
                ++lexer->offset;
                return token(ATRI_TOKEN_ARROW, start, lexer->offset);
            }
            if (lexer->offset < length && text[lexer->offset] == '=') {
                ++lexer->offset; return token(ATRI_TOKEN_SUB_ASSIGN, start, lexer->offset);
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

static unsigned hex_value(unsigned char c) {
    return c <= '9' ? (unsigned)(c-'0') : c <= 'F' ? (unsigned)(c-'A'+10) : (unsigned)(c-'a'+10);
}
/* Lexer 已验证转义；按目标原始字节解码，不依赖宿主 char 的有符号性。 */
unsigned char atri_literal_byte(AtriSource source, size_t *cursor) {
    unsigned char byte = (unsigned char)source.text[(*cursor)++];
    if (byte != '\\') { return byte; }
    byte = (unsigned char)source.text[(*cursor)++];
    switch (byte) {
        case '0': return 0;
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'a': return '\a';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        case 'x': {
            unsigned high = hex_value((unsigned char)source.text[(*cursor)++]);
            unsigned low = hex_value((unsigned char)source.text[(*cursor)++]);
            return (unsigned char)(high*16+low);
        }
        default: return byte;
    }
}
