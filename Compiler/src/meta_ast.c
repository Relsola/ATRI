#define ATRI_BUILD_HOST
#include "meta_ast.h"
#include "lexer.h"
#include "array.h"

static thread_local AtriAstView *active;
static thread_local uint64_t serial;
void atri_ast_begin(AtriAstView *v, AtriSource src, const AtriProgram *ast, AtriSpan range, uint64_t event) {
    *v = (AtriAstView){.source = src, .ast = ast, .range = range, .event = event, .generation = ++serial};
    active = v;
}
void atri_ast_end(AtriAstView *v) {
    for (size_t i = 0; i < v->count; ++i) {
        free(v->strings[i]);
    }
    free(v->strings);
    free(v->changed);
    free(v->invalid);
    atri_program_destroy(&v->parsed);
    active = NULL;
}
static const AtriExpr *expr(uint64_t handle) {
    uint32_t n = (uint32_t)handle;
    if (!active || handle >> 32 != active->generation || !n || n > active->ast->expression_count) {
        return NULL;
    }
    return &active->ast->expressions[n - 1];
}
static const char *remember(char *s) {
    if (!active) {
        free(s);
        return NULL;
    }
    if (!s || !atri_append((void **)&active->strings, &active->count, &active->capacity, sizeof(s), &s)) {
        free(s);
        active->failed = true;
        return NULL;
    }
    return s;
}
static const char *spelling(AtriSpan span) {
    size_t n = span.end - span.start;
    char *s = malloc(n + 1);
    if (s) {
        memcpy(s, active->source.text + span.start, n);
        s[n] = 0;
    }
    return remember(s);
}
ATRI_HOST_EXPORT int32_t atri_ast_has_annotation_bytes(uint64_t decl, const char *name, uint64_t size) {
    if (!active || decl != active->event || !name) {
        return 0;
    }
    const AtriDeclaration *item = &active->ast->declarations[0];
    for (size_t i = 0; i < item->annotation_count; ++i) {
        AtriSpan at = active->ast->annotations[item->first_annotation + i];
        if (at.end - at.start == size && !memcmp(active->source.text + at.start, name, size)) {
            return 1;
        }
    }
    return 0;
}
ATRI_HOST_EXPORT int32_t atri_ast_has_annotation(uint64_t decl, const char *name) {
    return name ? atri_ast_has_annotation_bytes(decl, name, strlen(name)) : 0;
}
static bool unary(const AtriExpr *e) {
    return e->kind == ATRI_EXPR_UNARY || e->kind == ATRI_EXPR_ADDRESS || e->kind == ATRI_EXPR_DEREFERENCE;
}
ATRI_HOST_EXPORT uint64_t atri_ast_expression_count(uint64_t decl) {
    return active && active->event == decl ? active->ast->expression_count : 0;
}
ATRI_HOST_EXPORT uint64_t atri_ast_expression(uint64_t decl, uint64_t index) {
    if (!active || decl != active->event || index >= active->ast->expression_count) {
        return 0;
    }
    return (active->generation << 32) | (index + 1);
}
ATRI_HOST_EXPORT const char *atri_ast_operator(uint64_t node) {
    const AtriExpr *e = expr(node);
    return e && (e->kind == ATRI_EXPR_BINARY || unary(e)) ? spelling(e->text) : "";
}
ATRI_HOST_EXPORT uint64_t atri_ast_operand(uint64_t node, uint32_t side) {
    const AtriExpr *e = expr(node);
    if (!e || (e->kind != ATRI_EXPR_BINARY && (!unary(e) || side)) || side > 1) {
        return 0;
    }
    return atri_ast_expression(active->event, side ? e->right : e->left);
}
ATRI_HOST_EXPORT const char *atri_ast_source(uint64_t node) {
    if (!active) {
        return NULL;
    }
    const AtriExpr *e = expr(node);
    if (!e && node != active->event) {
        return NULL;
    }
    const AtriSpan span = e ? e->span : active->range;
    /* 输入已解析，保留显式括号及字面量拼写。按词法单元规范化空白，
     * 运算符节点的区间用于恢复本声明的多字符自定义符号。 */
    size_t n = span.end - span.start;
    if (n > (SIZE_MAX - 1) / 2) {
        active->failed = true;
        return NULL;
    }
    char *out = malloc(2 * n + 1);
    if (!out) {
        active->failed = true;
        return NULL;
    }
    AtriLexer lexer = atri_lexer_create(active->source);
    lexer.offset = span.start;
    lexer.source.length = span.end;
    AtriSyntaxScope scope = {0};
    const AtriDeclaration *decl = &active->ast->declarations[0];
    AtriDiagnostic ignored;
    for (size_t i = 0; i < decl->annotation_count; ++i) {
        AtriSpan name = active->ast->annotations[decl->first_annotation + i];
        if (!atri_activate_annotation(&scope, active->source, name, &ignored)) {
            free(out);
            free(scope.ids);
            active->failed = true;
            return NULL;
        }
    }
    lexer.scope = &scope;
    size_t used = 0;
    AtriTokenKind prev = ATRI_TOKEN_EOF;
    for (;;) {
        AtriToken t = atri_lexer_next(&lexer, &ignored);
        if (t.kind == ATRI_TOKEN_EOF) {
            break;
        }
        bool tight_left = t.kind == ATRI_TOKEN_RIGHT_PAREN || t.kind == ATRI_TOKEN_RIGHT_BRACKET ||
                          t.kind == ATRI_TOKEN_SEMICOLON || t.kind == ATRI_TOKEN_COMMA ||
                          t.kind == ATRI_TOKEN_DOT || t.kind == ATRI_TOKEN_SCOPE || t.kind == ATRI_TOKEN_COLON ||
                          (t.kind == ATRI_TOKEN_LEFT_PAREN && prev == ATRI_TOKEN_IDENTIFIER);
        bool tight_right = prev == ATRI_TOKEN_LEFT_PAREN || prev == ATRI_TOKEN_LEFT_BRACKET ||
                           prev == ATRI_TOKEN_DOT || prev == ATRI_TOKEN_SCOPE || prev == ATRI_TOKEN_ANNOTATION;
        if (used && !tight_left && !tight_right) {
            out[used++] = ' ';
        }
        const char *token = active->source.text + t.span.start;
        size_t size = t.span.end - t.span.start;
        const char *directive = t.kind == ATRI_TOKEN_RUN       ? "#run"
                                : t.kind == ATRI_TOKEN_EXTERN  ? "#extern"
                                : t.kind == ATRI_TOKEN_KEYWORD ? "#keyword"
                                : t.kind == ATRI_TOKEN_THROUGH ? "#through"
                                                               : NULL;
        if (directive) {
            token = directive;
            size = strlen(directive);
        }
        memcpy(out + used, token, size);
        used += size;
        prev = t.kind;
    }
    free(scope.ids);
    out[used] = 0;
    return remember(out);
}
ATRI_HOST_EXPORT int32_t atri_ast_replace(uint64_t node, const char *text) {
    if (!active || active->failed || !text) {
        return 0;
    }
    const AtriExpr *e = expr(node);
    if (!e && node != active->event) {
        return 0;
    }
    AtriSpan root = active->range, old = e ? e->span : root;
    size_t lead = old.start - root.start, tail = root.end - old.end, n = strlen(text);
    if (n >= SIZE_MAX - lead - tail - 3) {
        return 0;
    }
    char *code = malloc(lead + n + tail + 3);
    if (!code) {
        active->failed = true;
        return 0;
    }
    memcpy(code, active->source.text + root.start, lead);
    size_t pos = lead;
    if (e) {
        code[pos++] = '(';
    }
    memcpy(code + pos, text, n);
    pos += n;
    if (e) {
        code[pos++] = ')';
    }
    memcpy(code + pos, active->source.text + old.end, tail);
    pos += tail;
    code[pos] = 0;
    AtriSource src = {"<plugin AST edit>", code, pos};
    AtriProgram ast = {0};
    AtriDiagnostic d = {0};
    if (!atri_parse_sources(src, NULL, 0, true, &ast, &d) || ast.declaration_count != 1) {
        if (!d.message) {
            d = (AtriDiagnostic){.span = {0, pos}, .message = "replacement must contain exactly one declaration"};
        }
        active->invalid = code;
        active->invalid_diagnostic = d;
        atri_program_destroy(&ast);
        active->failed = true;
        return 0;
    }
    free(active->changed);
    atri_program_destroy(&active->parsed);
    active->changed = code;
    active->source = src;
    active->parsed = ast;
    active->ast = &active->parsed;
    active->range = (AtriSpan){0, pos};
    active->generation = ++serial;
    return 1;
}
