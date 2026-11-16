#include "frontend.h"
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static AtriSource source(const char *s) { return (AtriSource){"test.txt", s, strlen(s)}; }
static bool text(AtriSource s, AtriSpan span, const char *expected) {
    return span.end <= s.length && span.end - span.start == strlen(expected) &&
           memcmp(s.text + span.start, expected, strlen(expected)) == 0;
}
static AtriDiagnostic reject(AtriSource s, const char *message) {
    AtriProgram p; AtriDiagnostic d;
    CHECK(!atri_parse(s, &p, &d)); CHECK(d.message != NULL && strstr(d.message, message) != NULL);
    CHECK(d.span.start <= d.span.end && d.span.end <= s.length);
    CHECK(p.function_count == 0 && p.functions == NULL && p.expressions == NULL);
    atri_program_destroy(&p); return d;
}
static void ast_and_precedence(void) {
    AtriSource s = source("fn main()->i32{const a:i32=1;return add(a,2*3-4);}\nfn add(a:i32,b:i32)->i32{return a+b;}");
    AtriProgram p; AtriDiagnostic d;
    CHECK(atri_parse(s, &p, &d)); CHECK(p.function_count == 2 && p.parameter_count == 2);
    CHECK(p.functions[0].statement_count == 2 && p.functions[1].first_statement == 2);
    CHECK(text(s, p.parameters[0].name, "a") && text(s, p.parameters[1].type, "i32"));
    CHECK(p.statements[0].kind == ATRI_STMT_CONST && text(s, p.statements[0].name, "a"));
    const AtriExpr *call = &p.expressions[p.statements[1].expression];
    CHECK(call->kind == ATRI_EXPR_CALL && call->argument_count == 2 && text(s, call->text, "add"));
    const AtriExpr *sub = &p.expressions[p.arguments[call->first_argument + 1]];
    CHECK(sub->kind == ATRI_EXPR_BINARY && sub->op == ATRI_SUB);
    CHECK(p.expressions[sub->left].op == ATRI_MUL);
    CHECK(text(s, call->span, "add(a,2*3-4)"));
    atri_program_destroy(&p);

    s = source("fn f()->UserType{return outer(inner(1,2),3,inner(4,5));}");
    CHECK(atri_parse(s, &p, &d));
    call = &p.expressions[p.statements[0].expression];
    CHECK(call->argument_count == 3);
    CHECK(text(s, p.expressions[p.arguments[call->first_argument + 1]].text, "3"));
    const AtriExpr *inner = &p.expressions[p.arguments[call->first_argument + 2]];
    CHECK(inner->argument_count == 2 && text(s, p.expressions[p.arguments[inner->first_argument]].text, "4"));
    atri_program_destroy(&p);
}
static void lexical_and_locations(void) {
    const char *inputs[] = {"", " \t\r\n", "\xEF\xBB\xBF", ("\xEF\xBB\xBF" "fn main()->i32{return 0;}")};
    for (size_t i = 0; i < 4; ++i) {
        AtriProgram p; AtriDiagnostic d; AtriSource s = source(inputs[i]);
        CHECK(atri_parse(s, &p, &d)); CHECK(p.function_count == (i == 3 ? 1U : 0U));
        if (i == 3) CHECK(atri_source_location(s, p.functions[0].name.start).column == 4);
        atri_program_destroy(&p);
    }
    AtriSource s = source("fn main() -> i32 {\r\n  return 0\xEF\xBC\x9B\r\n}");
    AtriDiagnostic d = reject(s, "full-width semicolon");
    AtriLocation loc = atri_source_location(s, d.span.start);
    CHECK(loc.line == 2 && loc.column == 11 && d.span.end - d.span.start == 3);
    s = source("fn f()->i32{\rreturn 0"); d = reject(s, "expected ';'");
    loc = atri_source_location(s, d.span.start); CHECK(loc.line == 2 && loc.column == 9);
    const char nul[] = "fn main()->i32{return 0;}\0fn bad()->i32{}";
    reject((AtriSource){"nul.txt", nul, sizeof(nul) - 1}, "NUL byte");
    AtriLexer lexer = atri_lexer_create(source("fn fnmain return return0 const constant let letter : , + - * / % ->"));
    AtriTokenKind kinds[] = {ATRI_TOKEN_FN, ATRI_TOKEN_IDENTIFIER, ATRI_TOKEN_RETURN, ATRI_TOKEN_IDENTIFIER,
        ATRI_TOKEN_CONST, ATRI_TOKEN_IDENTIFIER, ATRI_TOKEN_LET, ATRI_TOKEN_IDENTIFIER,
        ATRI_TOKEN_COLON, ATRI_TOKEN_COMMA, ATRI_TOKEN_PLUS, ATRI_TOKEN_MINUS,
        ATRI_TOKEN_STAR, ATRI_TOKEN_SLASH, ATRI_TOKEN_PERCENT, ATRI_TOKEN_ARROW, ATRI_TOKEN_EOF};
    size_t previous = 0;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) {
        AtriToken t = atri_lexer_next(&lexer, &d); CHECK(t.kind == kinds[i]);
        CHECK(t.span.start >= previous); previous = t.span.end;
    }
}
static void errors_and_growth(void) {
    struct { const char *input, *message; } cases[] = {
        {"fn i32 main(){}", "expected '('"}, {"fn f(i32 a)->i32{}", "expected ':'"},
        {"fn f(a:i32,)->i32{}", "parameter name"}, {"fn f()->i32{const i32 a=0;}", "expected ':'"},
        {"fn f()->i32{const a:i32;}", "requires an initializer"}, {"fn f()->i32{return;}", "integer expression"},
        {"fn f()->i32{return 1+;}", "integer expression"}, {"fn f()->i32{return g(1,);}", "integer expression"},
        {"fn f()->i32{return (1+2;}", "expected ')'"}, {"fn f()->i32{return 0}", "expected ';'"},
        {"fn f()->i32{return 1_000;}", "only decimal"}, {"fn f()->i32{return 0x10;}", "only decimal"},
        {"fn f()->i32{return 1.5;}", "unexpected character"}, {"fn f()->i32{", "expected '}'"},
        {"fn f()->i32{}@", "unexpected character"}, {"fnmain()->i32{}", "top level"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) reject(source(cases[i].input), cases[i].message);
    char large_literal[350] = "fn f()->i32{return ";
    size_t start = strlen(large_literal);
    memset(large_literal + start, '9', 300); memcpy(large_literal + start + 300, ";}", 3);
    AtriProgram p; AtriDiagnostic d;
    CHECK(atri_parse(source(large_literal), &p, &d));
    CHECK(p.expressions[0].text.end - p.expressions[0].text.start == 300); atri_program_destroy(&p);
    char *many = malloc(50000); CHECK(many != NULL); size_t used = 0;
    for (size_t i = 0; i < 1000; ++i) used += (size_t)snprintf(many + used, 50000 - used, "fn f%zu()->i32{return %zu;}\n", i, i);
    CHECK(atri_parse(source(many), &p, &d)); CHECK(p.function_count == 1000);
    CHECK(text(source(many), p.functions[999].name, "f999")); atri_program_destroy(&p); free(many);
    char deep[2000] = "fn f()->i32{return 1";
    used = strlen(deep);
    for (size_t i = 0; i < 300; ++i) { memcpy(deep + used, "+1", 2); used += 2; }
    memcpy(deep + used, ";}", 3); reject(source(deep), "limit of 256");
}
int main(void) {
    ast_and_precedence(); lexical_and_locations(); errors_and_growth();
    puts("frontend tests passed"); return 0;
}
