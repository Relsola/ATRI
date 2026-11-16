#include "ir.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static AtriSource source(const char *s) { return (AtriSource){"control.txt", s, strlen(s)}; }
static void accept(const char *s, AtriIRProgram *ir) {
    AtriProgram ast; AtriDiagnostic d;
    if (!atri_parse(source(s), &ast, &d)) { atri_print_diagnostic(stderr, source(s), d); CHECK(false); }
    bool ok = atri_lower(source(s), &ast, ir, &d);
    atri_program_destroy(&ast);
    if (!ok) { atri_print_diagnostic(stderr, source(s), d); CHECK(false); }
    CHECK(atri_ir_verify(ir, &d)); CHECK(atri_optimize(ir, true, &d));
}
static void reject(const char *s, const char *message, bool syntax) {
    AtriProgram ast; AtriIRProgram ir = {0}; AtriDiagnostic d;
    bool ok = atri_parse(source(s), &ast, &d);
    if (syntax) CHECK(!ok);
    else { CHECK(ok); ok = atri_lower(source(s), &ast, &ir, &d); CHECK(!ok); }
    if (ok || !d.message || !strstr(d.message, message)) {
        fprintf(stderr, "Expected %s: %s\n", message, s); atri_print_diagnostic(stderr, source(s), d); CHECK(false);
    }
    CHECK(d.span.end <= strlen(s));
    atri_ir_destroy(&ir); atri_program_destroy(&ast);
}
static void computed(const char *s, uint64_t expected, AtriIRType type) {
    AtriIRProgram ir; accept(s, &ir);
    const AtriIRFunction *f = &ir.functions[0];
    const AtriIRInst *ret = &f->instructions[f->instruction_count-1];
    CHECK(ret->kind == ATRI_IR_RETURN);
    CHECK(f->instructions[ret->left].kind == ATRI_IR_CONST && f->instructions[ret->left].type == type);
    CHECK(f->instructions[ret->left].immediate == expected);
    atri_ir_destroy(&ir);
}
static void syntax(void) {
    reject("fn f()->i32{if(true)return 1;return 0;}", "require braces", true);
    reject("fn f()->i32{if(true){}else return 1;return 0;}", "after else", true);
    reject("fn f()->i32{while(true);return 0;}", "require braces", true);
    reject("fn f()->i32{loop break;return 0;}", "require braces", true);
    reject("fn f()->i32{for(;;)break;return 0;}", "require braces", true);
    reject("fn f()->i32{do return 0;while(false);}", "require braces", true);
    reject("fn f()->i32{do{}while(false) return 0;}", "expected ';'", true);
    reject("fn f()->i32{for(;true;i:i32=1){}return 0;}", "cannot declare", true);
    reject("fn f()->i32{if true {return 1;}return 0;}", "before condition", true);
    reject("fn f()->i32{break label;return 0;}", "expected ';'", true);
    reject("fn f()->i32{return if(true){1}else{2};}", "expression", true);
    reject("fn f()->i32{for(i:i32=0 i<3;i=i+1){}return 0;}", "after for initializer", true);
    AtriProgram ast; AtriDiagnostic d;
    const char *s = "fn f(a:i32,b:i32,c:bool)->bool{return a+1<b*2==!c||false&&true;}";
    CHECK(atri_parse(source(s), &ast, &d));
    const AtriExpr *e = &ast.expressions[ast.statements[0].expression];
    CHECK(e->op == ATRI_OR && ast.expressions[e->right].op == ATRI_AND);
    const AtriExpr *equal = &ast.expressions[e->left];
    CHECK(equal->op == ATRI_EQ && ast.expressions[equal->left].op == ATRI_LT);
    const AtriExpr *less = &ast.expressions[equal->left];
    CHECK(ast.expressions[less->left].op == ATRI_ADD && ast.expressions[less->right].op == ATRI_MUL);
    atri_program_destroy(&ast);
}
static void types_and_scopes(void) {
    reject("fn f()->i32{if(1){return 0;}return 1;}", "type mismatch", false);
    reject("fn f(p:*i32)->i32{while(p){}return 0;}", "type mismatch", false);
    reject("fn f()->bool{return !1;}", "type mismatch", false);
    reject("fn f(p:*i32)->bool{return p&&true;}", "type mismatch", false);
    reject("fn f()->bool{return true||1;}", "type mismatch", false);
    reject("fn f()->i32{return true;}", "type mismatch", false);
    reject("fn f()->bool{return 0;}", "type mismatch", false);
    reject("fn f()->bool{return true+false;}", "type mismatch", false);
    reject("fn f(p:*i32)->bool{return p<null;}", "ordered comparisons", false);
    reject("fn f(a:i64,b:i32)->bool{return a==b;}", "type mismatch", false);
    reject("fn f()->bool{return 1<2<3;}", "ordered comparisons", false);
    reject("fn f()->i32{break;return 0;}", "enclosing loop", false);
    reject("fn f()->i32{continue;return 0;}", "enclosing loop", false);
    reject("fn f()->i32{{x:i32=1;}return x;}", "unknown local", false);
    reject("fn f()->i32{for(i:i32;false;){}return i;}", "unknown local", false);
    reject("fn f()->i32{for(;;x=x+1){x:i32=1;break;}return 0;}", "unknown name", false);
    reject("fn f()->i32{do{x:bool=true;}while(x);return 0;}", "unknown local", false);
    reject("fn f()->i32{x:i32;{x:i32;x:i32;}return x;}", "duplicate local", false);
    reject("fn f(a:bool)->i32{if(a){return 1;}}", "every reachable path", false);
    reject("fn f()->i32{while(false){return 1;}}", "every reachable path", false);
    reject("fn f()->i32{loop{break;}}", "every reachable path", false);
    reject("fn f()->i32{if(false){n:i32 :=1/0;}return 0;}", "by zero", false);
    reject("fn f()->bool{b:bool :=false&&missing;return b;}", "unknown local", false);
    const char *programs[] = {
        "fn f(a:bool)->i32{if(a){return 1;}else{return 2;}}",
        "fn f()->i32{if(true){return 1;}}",
        "fn f()->i32{loop{continue;}}",
        "fn f()->i32{while(true){}}",
        "fn f()->i32{for(;;){}}",
        "fn f()->i32{do{return 3;}while(false);}",
        "fn f()->i32{for(;;){break;}return 0;}",
        "fn f()->i32{x:i32=1;{x:i32=2;}return x;}",
        "fn f()->i32{for(start();false;start()){}return 0;}fn start()->i32{return 1;}",
        "fn f()->bool{b:bool;return b;}",
        "fn f(p:*i32)->bool{return null!=p;}",
        "fn f()->i32{for(k:i32 :=3;false;){}return 0;}"
    };
    for (size_t i = 0; i < sizeof(programs)/sizeof(*programs); ++i) {
        AtriIRProgram ir; accept(programs[i], &ir); atri_ir_destroy(&ir);
    }
}
static void constants(void) {
    computed("fn f()->i32{v:i32 :=sum(5);return v;}"
        "fn sum(n:i32)->i32{s:i32;for(i:i32;i<n;i=i+1){if(i==2){continue;}s=s+i;}return s;}", 8, ATRI_IR_I32);
    computed("fn f()->i32{v:i32 :=fact(5);return v;}"
        "fn fact(n:i32)->i32{if(n<=1){return 1;}return n*fact(n-1);}", 120, ATRI_IR_I32);
    computed("fn f()->bool{v:bool :=false&&(1/0==0);return v;}", 0, ATRI_IR_BOOL);
    computed("fn f()->bool{v:bool :=true||(1/0==0);return v;}", 1, ATRI_IR_BOOL);
    computed("fn f()->bool{v:bool :=safe(0);return v;}"
        "fn safe(n:i32)->bool{return n!=0&&10/n>1;}", 0, ATRI_IR_BOOL);
    computed("fn f()->bool{v:bool :=true||bad();return v;}"
        "fn bad()->bool{x:i32;p:*i32=&x;return *p==0;}", 1, ATRI_IR_BOOL);
    computed("fn f()->i32{v:i32 :=g();return v;}fn g()->i32{if(false){return 1/0;}return 7;}", 7, ATRI_IR_I32);
    computed("fn f()->i64{v:i64 :=g();return v;}fn g()->i64{x:i64;while(x<3){x=x+1;}return x*5000000000;}",
        UINT64_C(15000000000), ATRI_IR_I64);
    computed("fn f()->i32{v:i32 :=g();return v;}fn g()->i32{i:i32;do{i=i+1;continue;}while(i<3);return i;}", 3, ATRI_IR_I32);
    reject("fn f()->i32{n:i32 :=spin();return n;}fn spin()->i32{loop{}}", "step limit", false);
    reject("fn f()->bool{n:bool :=true&&(1/0==0);return n;}", "by zero", false);
    reject("fn f()->bool{n:bool :=false||(1/0==0);return n;}", "by zero", false);
    reject("fn f(n:i32)->bool{b:bool :=false&&(n==0);return b;}", "runtime variable", false);
}
static void verifier(void) {
    AtriIRProgram ir; AtriDiagnostic d;
    accept("fn f(b:bool)->i32{if(b){return 1;}else{return 2;}}", &ir);
    AtriIRFunction *f = &ir.functions[0];
    AtriId branch = ATRI_NONE, first_value = ATRI_NONE, last_return = ATRI_NONE;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        if (f->instructions[i].kind == ATRI_IR_BRANCH) branch = i;
        if (f->instructions[i].kind == ATRI_IR_CONST && first_value == ATRI_NONE) first_value = i;
        if (f->instructions[i].kind == ATRI_IR_RETURN) last_return = i;
    }
    CHECK(branch != ATRI_NONE && first_value != ATRI_NONE && last_return != ATRI_NONE);
    AtriId saved = f->instructions[branch].target;
    f->instructions[branch].target = f->block_count;
    CHECK(!atri_ir_verify(&ir, &d)); f->instructions[branch].target = saved;
    AtriId saved_value = f->instructions[last_return].left;
    f->instructions[last_return].left = first_value;
    CHECK(!atri_ir_verify(&ir, &d)); /* definition in the other branch does not dominate */
    f->instructions[last_return].left = saved_value;
    f->instructions[branch].left = first_value;
    CHECK(!atri_ir_verify(&ir, &d));
    atri_ir_destroy(&ir);
    accept("fn f(b:bool)->i32{return g(b);}fn g(b:bool)->i32{if(b){return 1;}return 2;}", &ir);
    AtriIRType *types = ir.functions[1].parameter_types;
    ir.functions[1].parameter_types = NULL;
    CHECK(!atri_ir_verify(&ir, &d));
    ir.functions[1].parameter_types = types;
    atri_ir_destroy(&ir);
}
int main(void) { syntax(); types_and_scopes(); constants(); verifier(); puts("control flow tests passed"); return 0; }
