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
    fail("fn f()->i32{x: i32 :=1;x=2;return x;}", "cannot assign", false);
    fail("fn f(x:i32)->i32{x=2;return x;}", "cannot assign", false);
    fail("fn f()->i32{x:u64=1;return x;}", "only i32", false);
    fail("fn f()->i32{g: i32 :=1;return g();}", "not callable", false);
    fail("fn f()->i32{x:i32;}", "must return", false);
    fail("fn f()->i32{return a;a: i32 :=1;}", "unknown local", false);
    fail("fn f()->i32{return missing();}", "unknown function", false);
    fail("fn f()->i32{return g(1);}fn g()->i32{return 0;}", "argument count", false);
    fail("fn f()->i32{return 0;}fn f()->i32{return 1;}", "duplicate function", false);
    fail("fn f(a:i32,a:i32)->i32{return a;}", "duplicate local", false);
    fail("fn f()->i32{a: i32 :=1;a: i32 :=2;return a;}", "duplicate local", false);
    fail("fn f()->i32{a: i32 :=1;a=2;return a;}", "cannot assign", false);
    fail("fn f()->i32{a:i32=a;return a;}", "unknown local", false);
    fail("fn f()->i32{a=1;return 0;}", "unknown name", false);
    fail("fn f()->i32{g: i32 :=1;return g();}", "not callable", false);
    fail("fn f()->u64{return 1;}", "only i32", false);
    fail("fn f(a:f32)->i32{return 1;}", "only i32", false);
    fail("fn f()->i32{a: u64 :=1;return 0;}", "only i32", false);
    fail("fn f()->i32{}", "must return", false);
    fail("fn f()->i32{return 0;return missing;}", "unknown local", false);
}
static void calls_and_inline(void) {
    const char *s = "fn main()->i32{a: i32 :=1;b: i32 :=2;return add(a,b);}"
        "fn add(a:i32,b:i32)->i32{return a+b;}";
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d; compile(s, &ast, &ir);
    CHECK(ir.functions[0].instructions[ir.functions[0].instruction_count - 2].kind == ATRI_IR_CALL);
    CHECK(atri_optimize(&ir, true, &d)); CHECK(ir.functions[0].instruction_count == 2);
    CHECK(ir.functions[0].instructions[0].kind == ATRI_IR_CONST && ir.functions[0].instructions[0].immediate == 3);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
    compile(s, &ast, &ir); CHECK(atri_optimize(&ir, false, &d));
    CHECK(ir.functions[0].instructions[2].kind == ATRI_IR_CALL); atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn main()->i32{a:i32;a=a+2;return twice(a);}"
        "fn twice(x:i32)->i32{y:i32=x+1;return y*2;}";
    compile(s, &ast, &ir); CHECK(atri_optimize(&ir, true, &d));
    AtriIRFunction *f = &ir.functions[0]; CHECK(f->instructions[f->instructions[f->instruction_count-1].left].immediate == 6);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn main()->i32{return a();}fn a()->i32{return b();}fn b()->i32{return c();}fn c()->i32{return 3;}";
    compile(s, &ast, &ir); CHECK(atri_optimize(&ir, true, &d));
    CHECK(ir.functions[0].instructions[0].immediate == 3); atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn a(x:i32)->i32{return b(x);}fn b(x:i32)->i32{return a(x);}";
    compile(s, &ast, &ir); CHECK(atri_optimize(&ir, true, &d)); CHECK(ir.functions[0].instructions[1].kind == ATRI_IR_CALL);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn f()->i32{return 1;x:i32=3;y:i32=x+2;return y;}";
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
static void pointers(void) {
    const char *s = "fn main()->i32{a:i32=1;p:*i32=&a;*p=2;return a;}";
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d; compile(s, &ast, &ir);
    size_t locals = 0, stores = 0, loads = 0;
    for (size_t i = 0; i < ir.functions[0].instruction_count; ++i) {
        AtriIRKind kind = ir.functions[0].instructions[i].kind;
        locals += kind == ATRI_IR_LOCAL; stores += kind == ATRI_IR_STORE; loads += kind == ATRI_IR_LOAD;
    }
    CHECK(locals == 1 && stores == 2 && loads == 1);
    CHECK(atri_optimize(&ir, true, &d));
    AtriIRFunction *f = &ir.functions[0];
    CHECK(f->instructions[f->instructions[f->instruction_count - 1].left].kind == ATRI_IR_LOAD);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
    s = "fn f(p:*i32)->*i32{return p;}";
    compile(s, &ast, &ir); CHECK(ir.functions[0].parameter_types[0] == ATRI_IR_PTR_I32 && ir.functions[0].return_type == ATRI_IR_PTR_I32);
    CHECK(atri_optimize(&ir, true, &d)); atri_ir_destroy(&ir); atri_program_destroy(&ast);
    fail("fn f()->i32{x:i32=1;p:*i32=x;return 0;}", "type mismatch", false);
    fail("fn f()->i32{p:*i32=0;return 0;}", "type mismatch", false);
    fail("fn f()->i32{p:*i32=null;return p;}", "type mismatch", false);
    fail("fn f()->i32{return *1;}", "type mismatch", false);
    fail("fn f()->i32{p:*i32;return p+1;}", "null pointer", false);
    fail("fn f()->i32{x:i32;return &x;}", "type mismatch", false);
    fail("fn f()->i32{x:i32;return g(&x);}fn g(x:i32)->i32{return x;}", "type mismatch", false);
    fail("fn f()->i32{p:**i32;return 0;}", "one-level", false);
    fail("fn f()->i32{p:*i32;pp:*i32=&p;return 0;}", "taking the address", false);
    fail("fn f()->i32{x:i32 :=1;p:*i32=&x;return 0;}", "cannot take the address", false);
    fail("fn f(x:i32)->i32{p:*i32=&x;return 0;}", "cannot take the address", false);
    fail("fn f()->i32{p:*i32=&(1+2);return 0;}", "addressable", false);
    fail("fn f()->i32{(1+2)=3;return 0;}", "assignment requires", false);
    fail("fn f()->*i32{x:i32=1;return &x;}", "address of a local", false);
    fail("fn f()->i32{p:*i32;return *p;}", "null pointer", false);
    fail("fn f()->i32{p:*i32=null;*p=1;return 0;}", "null pointer", false);
    fail("fn f()->i32{x:i32=null;return x;}", "null requires a pointer", false);
    fail("fn f()->i32{p:*i32 :=null;return 0;}", "compile-time pointer constants", false);
    fail("fn main()->i32{n:i32 :=f();return n;}fn f()->i32{x:i32;p:*i32=&x;*p=3;return x;}", "compile-time pointer operations", false);
    s = "fn f(p:*i32,x:i32)->i32{*p=x;return *p;}";
    compile(s, &ast, &ir);
    for (size_t i = 0; i < ir.functions[0].instruction_count; ++i)
        if (ir.functions[0].instructions[i].kind == ATRI_IR_STORE) ir.functions[0].instructions[i].left = 1;
    CHECK(!atri_ir_verify(&ir, &d)); atri_ir_destroy(&ir); atri_program_destroy(&ast);
}
static void constant_program(const char *s, int64_t expected) {
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d; compile(s, &ast, &ir);
    AtriIRFunction *f = &ir.functions[0];
    AtriIRInst ret = f->instructions[f->instruction_count - 1];
    CHECK(f->instructions[ret.left].kind == ATRI_IR_CONST);
    CHECK(atri_i32_signed(f->instructions[ret.left].immediate) == expected);
    for (size_t i = 0; i < f->instruction_count; ++i) CHECK(f->instructions[i].kind != ATRI_IR_CALL);
    CHECK(atri_optimize(&ir, false, &d));
    ret = f->instructions[f->instruction_count - 1];
    CHECK(f->instructions[ret.left].kind == ATRI_IR_CONST);
    CHECK(atri_i32_signed(f->instructions[ret.left].immediate) == expected);
    size_t live = 0;
    for (size_t i = 0; i < f->instruction_count; ++i)
        if (f->instructions[i].kind != ATRI_IR_NOP) ++live;
    CHECK(live == 2);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
}
static void wide_integer(const char *expression, int64_t expected) {
    char s[1024]; snprintf(s, sizeof(s), "fn wide()->i64{n:i64 :=%s;return n;}", expression);
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d; compile(s, &ast, &ir);
    CHECK(atri_optimize(&ir, true, &d));
    AtriIRFunction *f = &ir.functions[0];
    const AtriIRInst *ret = &f->instructions[f->instruction_count - 1];
    const AtriIRInst *v = &f->instructions[ret->left];
    CHECK(v->kind == ATRI_IR_CONST && v->type == ATRI_IR_I64 && atri_i64_signed(v->immediate) == expected);
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
}
static void pointer_arithmetic(void) {
    AtriProgram ast; AtriIRProgram ir; AtriDiagnostic d;
    const char *s = "fn move(p:*i32,n:i32)->*i32{return n+p;}fn distance(p:*i32,q:*i32)->i64{return p-q;}";
    compile(s, &ast, &ir);
    AtriIRInst *offset = &ir.functions[0].instructions[2];
    CHECK(offset->kind == ATRI_IR_PTR_OFFSET && offset->type == ATRI_IR_PTR_I32);
    CHECK(ir.functions[0].instructions[offset->left].type == ATRI_IR_PTR_I32);
    CHECK(ir.functions[0].instructions[offset->right].type == ATRI_IR_I32);
    CHECK(ir.functions[1].instructions[2].kind == ATRI_IR_PTR_DIFF && ir.functions[1].return_type == ATRI_IR_I64);
    AtriId saved = offset->right; offset->right = offset->left;
    CHECK(!atri_ir_verify(&ir, &d)); offset->right = saved;
    CHECK(atri_optimize(&ir, true, &d)); atri_ir_destroy(&ir); atri_program_destroy(&ast);
    fail("fn f(p:*i32,q:*i32)->*i32{return p+q;}", "cannot add two pointers", false);
    fail("fn f(p:*i32)->*i32{return 1-p;}", "subtract a pointer from an integer", false);
    fail("fn f(p:*i32)->*i32{return p*2;}", "supports only", false);
    fail("fn f(p:*i32)->*i32{return +p;}", "type mismatch", false);
    fail("fn f()->*i32{return +null;}", "null requires a pointer", false);
    fail("fn f(p:*i32,q:*i32)->i32{return p-q;}", "type mismatch", false);
    fail("fn f()->*i32{return null+0;}", "null pointer", false);
    fail("fn f()->i64{return null-null;}", "null pointer", false);
    fail("fn f(p:*i32)->*i32{return p+9223372036854775807;}", "signed 64-bit range", true);
    fail("fn f(p:*i32)->*i32{return p-(-9223372036854775808);}", "signed 64-bit range", true);
    fail("fn f(x:i32,y:i64)->i64{return x+y;}", "type mismatch", false);
    fail("fn f(p:*i32)->i32{x:i32=p-p;return x;}", "type mismatch", false);
    fail("fn f()->i64{return 9223372036854775808;}", "i64 range", false);
    fail("fn f()->i64{return -9223372036854775809;}", "i64 range", false);
    fail("fn f()->i64{n:i64 :=1/0;return n;}", "by zero", false);
    fail("fn f()->i64{n:i64 :=-9223372036854775808/-1;return n;}", "INT_MIN", false);
    fail("fn f()->i64{n:i64 :=-9223372036854775808%-1;return n;}", "INT_MIN", false);
    fail("fn f()->i64{x:i64=1;p:*i32=&x;return x;}", "only i32 variables", false);
    wide_integer("5000000000+7", INT64_C(5000000007));
    wide_integer("+5000000000", INT64_C(5000000000));
    wide_integer("9223372036854775807+1", INT64_MIN);
    wide_integer("-9223372036854775808-1", INT64_MAX);
    wide_integer("4294967296*4294967296", 0);
    wide_integer("-(-9223372036854775808)", INT64_MIN);
    wide_integer("-17/5", -3); wide_integer("-17%5", -2);
}
static void compile_time_constants(void) {
    constant_program("fn main()->i32{a:i32 :=1;b:i32 :=a+2;return b;}", 3);
    constant_program("fn main()->i32{n:i32 :=add(1,2);return n;}fn add(a:i32,b:i32)->i32{return a+b;}", 3);
    constant_program("fn main()->i32{n:i32 :=helper(3);return n;}"
        "fn helper(x:i32)->i32{a:i32=x;a=a*2;k:i32 :=4;return a+k;}", 10);
    constant_program("fn main()->i32{n:i32 :=f(2);return n;}"
        "fn f(x:i32)->i32{return g(x)+1;}fn g(x:i32)->i32{return x*3;}", 7);
    constant_program("fn main()->i32{n:i32 :=2147483647+1;return n;}", INT64_C(-2147483648));
    constant_program("fn main()->i32{n:i32 :=-17%5;return n;}", -2);
    fail("fn f(x:i32)->i32{n:i32 :=x+1;return n;}", "runtime variable or parameter", false);
    fail("fn main()->i32{a:i32=1;n:i32 :=a+1;return n;}", "runtime variable or parameter", false);
    fail("fn main()->i32{n:i32 :=divide(0);return n;}fn divide(x:i32)->i32{return 1/x;}", "by zero", false);
    fail("fn main()->i32{n:i32 :=1/0;return n;}", "by zero", false);
    fail("fn main()->i32{n:i32 :=-2147483648/-1;return n;}", "INT_MIN", false);
    fail("fn main()->i32{return 1;n:i32 :=1/0;}", "by zero", false);
    fail("fn main()->i32{n:i32 :=recurse(0);return n;}fn recurse(x:i32)->i32{return recurse(x);}", "depth limit", false);
    fail("fn main()->i32{n:i32 :=f();return n;}fn f()->i32{m:i32 :=main();return m;}", "cyclic compile-time", false);
    fail("fn main()->i32{n:i32 :=f();return n;}fn f()->i32{x:i32 :=recurse();return 3;}fn recurse()->i32{return recurse();}", "depth limit", false);
    fail("fn main()->i32{n:i32 :=f();return n;}fn f()->i32{x:i32=recurse();return 3;}fn recurse()->i32{return recurse();}", "depth limit", false);
    char large[4096] = "fn main()->i32{n:i32 :=large(1);return n;}fn large(x:i32)->i32{a:i32=x;";
    size_t used = strlen(large);
    for (size_t i = 0; i < 80; ++i) { memcpy(large + used, "a=a+1;", 6); used += 6; }
    memcpy(large + used, "return a;}", 11);
    constant_program(large, 81);
}
int main(void) { arithmetic(); names_and_types(); calls_and_inline(); compile_time_constants(); pointers(); pointer_arithmetic(); verifier(); puts("compiler tests passed"); return 0; }
