#include "test_support.h"

static AtriSource source(const char *code) {
    return (AtriSource){"control flow", code, strlen(code)};
}

static void computed(const char *code, int64_t expected, AtriIRType type) {
    Compilation test = test_compile("control-flow constant", code, TEST_O1);
    test_expect_return(&test.before, "f", type, expected, __FILE__, __LINE__);
    test_expect_return(&test.after, "f", type, expected, __FILE__, __LINE__);
    test_destroy(&test);
}

static void computed_file(const char *path, int64_t expected, AtriIRType type) {
    Compilation test = test_compile_file(path, TEST_O1);
    test_expect_return(&test.before, "f", type, expected, __FILE__, __LINE__);
    test_expect_return(&test.after, "f", type, expected, __FILE__, __LINE__);
    test_destroy(&test);
}

static void syntax(void) {
    EXPECT_PARSE_ERROR("fn f()->i32{if(true)return 1;return 0;}", "require braces");
    EXPECT_PARSE_ERROR("fn f()->i32{if(true){}else return 1;return 0;}", "after else");
    EXPECT_PARSE_ERROR("fn f()->i32{while(true);return 0;}", "require braces");
    EXPECT_PARSE_ERROR("fn f()->i32{loop break;return 0;}", "require braces");
    EXPECT_PARSE_ERROR("fn f()->i32{for(;;)break;return 0;}", "require braces");
    EXPECT_PARSE_ERROR("fn f()->i32{do return 0;while(false);}", "require braces");
    EXPECT_PARSE_ERROR("fn f()->i32{do{}while(false) return 0;}", "expected ';'");
    EXPECT_PARSE_ERROR("fn f()->i32{for(;true;i:i32=1){}return 0;}", "cannot declare");
    EXPECT_PARSE_ERROR("fn f()->i32{if true {return 1;}return 0;}", "before condition");
    EXPECT_PARSE_ERROR("fn f()->i32{break label;return 0;}", "expected ';'");
    EXPECT_PARSE_ERROR("fn f()->i32{return if(true){1}else{2};}", "expression");
    EXPECT_PARSE_ERROR("fn f()->i32{for(i:i32=0 i<3;i=i+1){}return 0;}", "after for initializer");
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
    EXPECT_COMPILE_ERROR("fn f()->i32{if(1){return 0;}return 1;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->i32{while(p){}return 0;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->bool{return !1;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->bool{return p&&true;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->bool{return true||1;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{return true;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->bool{return 0;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->bool{return true+false;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->bool{return p<null;}", "ordered comparisons");
    EXPECT_COMPILE_ERROR("fn f(a:i64,b:i32)->bool{return a==b;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->bool{return 1<2<3;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{break;return 0;}", "enclosing loop");
    EXPECT_COMPILE_ERROR("fn f()->i32{continue;return 0;}", "enclosing loop");
    EXPECT_COMPILE_ERROR("fn f()->i32{{x:i32=1;}return x;}", "unknown local");
    EXPECT_COMPILE_ERROR("fn f()->i32{for(i:i32;false;){}return i;}", "unknown local");
    EXPECT_COMPILE_ERROR("fn f()->i32{for(;;x=x+1){x:i32=1;break;}return 0;}", "unknown name");
    EXPECT_COMPILE_ERROR("fn f()->i32{do{x:bool=true;}while(x);return 0;}", "unknown local");
    EXPECT_COMPILE_ERROR("fn f()->i32{x:i32;{x:i32;x:i32;}return x;}", "duplicate local");
    EXPECT_COMPILE_ERROR("fn f(a:bool)->i32{if(a){return 1;}}", "every reachable path");
    EXPECT_COMPILE_ERROR("fn f()->i32{while(false){return 1;}}", "every reachable path");
    EXPECT_COMPILE_ERROR("fn f()->i32{loop{break;}}", "every reachable path");
    EXPECT_COMPILE_ERROR("fn f()->i32{if(false){n:i32 :=1/0;}return 0;}", "by zero");
    EXPECT_COMPILE_ERROR("fn f()->bool{b:bool :=false&&missing;return b;}", "unknown local");
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
        Compilation test = test_compile("accepted control flow", programs[i], TEST_O1);
        test_destroy(&test);
    }
}
static void constants(void) {
    computed_file("cases/constants/loop_sum.txt", 8, ATRI_IR_I32);
    computed_file("cases/constants/factorial.txt", 120, ATRI_IR_I32);
    computed_file("cases/constants/short_circuit.txt", false, ATRI_IR_BOOL);
    computed_file("cases/constants/wide_loop.txt", INT64_C(15000000000), ATRI_IR_I64);
    computed_file("cases/constants/do_continue.txt", 3, ATRI_IR_I32);

    computed("fn f()->bool{v:bool :=false&&(1/0==0);return v;}", false, ATRI_IR_BOOL);
    computed("fn f()->bool{v:bool :=true||(1/0==0);return v;}", true, ATRI_IR_BOOL);
    computed("fn f()->bool{v:bool :=true||bad();return v;}"
        "fn bad()->bool{x:i32;p:*i32=&x;return *p==0;}", true, ATRI_IR_BOOL);
    computed("fn f()->i32{v:i32 :=g();return v;}fn g()->i32{if(false){return 1/0;}return 7;}", 7, ATRI_IR_I32);
    EXPECT_COMPILE_ERROR("fn f()->i32{n:i32 :=spin();return n;}fn spin()->i32{loop{}}", "step limit");
    EXPECT_COMPILE_ERROR("fn f()->bool{n:bool :=true&&(1/0==0);return n;}", "by zero");
    EXPECT_COMPILE_ERROR("fn f()->bool{n:bool :=false||(1/0==0);return n;}", "by zero");
    EXPECT_COMPILE_ERROR("fn f(n:i32)->bool{b:bool :=false&&(n==0);return b;}", "runtime variable");
}
int main(void) {
    syntax();
    types_and_scopes();
    constants();
    puts("control flow tests passed");
    return 0;
}
