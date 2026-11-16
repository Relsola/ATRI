#include "ir.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static AtriSource source(const char *s) { return (AtriSource){"test.txt", s, strlen(s)}; }
static void compile(const char *s, AtriProgram *ast, AtriIRProgram *ir) {
    AtriDiagnostic d; CHECK(atri_parse(source(s), ast, &d));
    if (!atri_lower(source(s), ast, ir, &d)) { fprintf(stderr, "%s\n", d.message); CHECK(false); }
}
static void folded(const char *expression, int64_t expected) {
    char s[1024]; snprintf(s, sizeof(s), "fn main()->i32{return %s;}", expression);
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d; compile(s, &ast, &ir);
    CHECK(atri_optimize(&ir, true, &d));
    AtriIRFunction *f = &ir.functions[0]; AtriIRInst ret = f->instructions[f->instruction_count - 1];
    CHECK(f->instructions[ret.left].kind == ATRI_IR_CONST);
    CHECK(atri_i32_signed(f->instructions[ret.left].immediate) == expected);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
}
static void fail(const char *s, const char *message, bool optimize) {
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d;
    CHECK(atri_parse(source(s), &ast, &d));
    bool ok = atri_lower(source(s), &ast, &ir, &d);
    if (ok && optimize) ok = atri_optimize(&ir, true, &d);
    CHECK(!ok && d.message != NULL && strstr(d.message, message) != NULL);
    CHECK(d.span.end <= strlen(s)); atri_ir_destroy(&ir); atri_program_destroy(&ast);
}
static void arithmetic(void) {
    folded("1+2*3", 7); folded("(1+2)*3", 9); folded("10-3-2", 5);
    folded("20/3/2", 3); folded("-17/5", -3); folded("-17%5", -2);
    folded("17/-5", -3); folded("17%-5", 2); folded("-(-3)+ +2", 5);
    folded("2147483647+1", INT64_C(-2147483648)); folded("-2147483648-1", INT32_MAX);
    folded("65536*65536", 0); folded("-(-2147483648)", INT64_C(-2147483648));
    fail("fn f()->i32{return 2147483648;}", "outside the i32", false);
    fail("fn f()->i32{return -2147483649;}", "outside the i32", false);
    fail("fn f()->i32{return 1/0;}", "by zero", true);
    fail("fn f()->i32{return -2147483648%-1;}", "INT_MIN", true);
    fail("fn f()->i32{return -2147483648/-1;}", "INT_MIN", true);
    fail("fn f(x:i32)->i32{return x/0;}", "by zero", true);
}
static void names_and_types(void) {
    fail("fn f()->i32{const x:i32=1;x=2;return x;}", "cannot assign", false);
    fail("fn f(x:i32)->i32{x=2;return x;}", "cannot assign", false);
    fail("fn f()->i32{let x:u64=1;return x;}", "only i32", false);
    fail("fn f()->i32{const g:i32=1;return g();}", "not callable", false);
    fail("fn f()->i32{let x:i32;}", "must return", false);
    fail("fn f()->i32{return a;const a:i32=1;}", "unknown local", false);
    fail("fn f()->i32{return missing();}", "unknown function", false);
    fail("fn f()->i32{return g(1);}fn g()->i32{return 0;}", "argument count", false);
    fail("fn f()->i32{return 0;}fn f()->i32{return 1;}", "duplicate function", false);
    fail("fn f(a:i32,a:i32)->i32{return a;}", "duplicate local", false);
    fail("fn f()->i32{const a:i32=1;const a:i32=2;return a;}", "duplicate local", false);
    fail("fn f()->i32{const a:i32=1;a=2;return a;}", "cannot assign", false);
    fail("fn f()->i32{let a:i32=a;return a;}", "unknown local", false);
    fail("fn f()->i32{a=1;return 0;}", "unknown name", false);
    fail("fn f()->i32{const g:i32=1;return g();}", "not callable", false);
    fail("fn f()->u64{return 1;}", "only i32", false);
    fail("fn f(a:f32)->i32{return 1;}", "only i32", false);
    fail("fn f()->i32{const a:u64=1;return 0;}", "only i32", false);
    fail("fn f()->i32{}", "must return", false);
    fail("fn f()->i32{return 0;return missing;}", "unknown local", false);
}
static void calls_and_inline(void) {
    const char *s = "fn main()->i32{const a:i32=1;const b:i32=2;return add(a,b);}"
        "fn add(a:i32,b:i32)->i32{return a+b;}";
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d; compile(s, &ast, &ir);
    CHECK(ir.functions[0].instructions[2].kind == ATRI_IR_CALL);
    CHECK(atri_optimize(&ir, true, &d)); CHECK(ir.functions[0].instruction_count == 2);
    CHECK(ir.functions[0].instructions[0].kind == ATRI_IR_CONST && ir.functions[0].instructions[0].immediate == 3);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
    compile(s, &ast, &ir); CHECK(atri_optimize(&ir, false, &d));
    CHECK(ir.functions[0].instructions[2].kind == ATRI_IR_CALL); atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn main()->i32{let a:i32;a=a+2;return twice(a);}"
        "fn twice(x:i32)->i32{const y:i32=x+1;return y*2;}";
    compile(s, &ast, &ir); CHECK(atri_optimize(&ir, true, &d));
    AtriIRFunction *f = &ir.functions[0]; CHECK(f->instructions[f->instructions[f->instruction_count-1].left].immediate == 6);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn main()->i32{return a();}fn a()->i32{return b();}fn b()->i32{return c();}fn c()->i32{return 3;}";
    compile(s, &ast, &ir); CHECK(atri_optimize(&ir, true, &d));
    CHECK(ir.functions[0].instructions[0].immediate == 3); atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn a(x:i32)->i32{return b(x);}fn b(x:i32)->i32{return a(x);}";
    compile(s, &ast, &ir); CHECK(atri_optimize(&ir, true, &d)); CHECK(ir.functions[0].instructions[1].kind == ATRI_IR_CALL);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn f()->i32{return 1;let x:i32=3;let y:i32=x+2;return y;}";
    compile(s, &ast, &ir); CHECK(ir.functions[0].instruction_count == 2); atri_ir_destroy(&ir); atri_program_destroy(&ast);
    fail("fn main()->i32{return divide(0);}fn divide(x:i32)->i32{return 10/x;}", "by zero", true);
}
static void verifier(void) {
    const char *s = "fn main()->i32{return 1+2;}";
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d; compile(s, &ast, &ir);
    ir.functions[0].instructions[2].left = 99; CHECK(!atri_ir_verify(&ir, &d));
    ir.functions[0].instructions[2].left = 0;
    ir.functions[0].instructions[3].kind = ATRI_IR_CONST; CHECK(!atri_ir_verify(&ir, &d));
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
}
int main(void) { arithmetic(); names_and_types(); calls_and_inline(); verifier(); puts("compiler tests passed"); return 0; }
