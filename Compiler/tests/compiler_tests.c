#include "test_support.h"

static void expect_fold_i32(const char *expression, int64_t expected) {
    char code[1024];
    snprintf(code, sizeof(code), "fn main() -> i32 { return %s; }", expression);
    Compilation test = test_compile(expression, code, TEST_O1);
    EXPECT_RETURN_I32(test.after, "main", expected);
    test_destroy(&test);
}

static void expect_constant_i32(const char *code, int64_t expected) {
    Compilation test = test_compile("i32 constant", code, TEST_O0);
    EXPECT_RETURN_I32(test.before, "main", expected);
    EXPECT_NO_CALLS(test.before, "main");
    EXPECT_RETURN_I32(test.after, "main", expected);
    EXPECT_OPERATIONS(test.after, "main", ATRI_IR_CONST, 1);
    EXPECT_OPERATIONS(test.after, "main", ATRI_IR_RETURN, 1);
    test_destroy(&test);
}

static void expect_constant_i64(const char *expression, int64_t expected) {
    char code[1024];
    snprintf(code, sizeof(code), "fn wide() -> i64 { n: i64 := %s; return n; }", expression);
    Compilation test = test_compile(expression, code, TEST_O1);
    EXPECT_RETURN_I64(test.after, "wide", expected);
    test_destroy(&test);
}

static void arithmetic(void) {
    expect_fold_i32("1+2*3", 7);
    expect_fold_i32("(1+2)*3", 9);
    expect_fold_i32("10-3-2", 5);
    expect_fold_i32("20/3/2", 3);
    expect_fold_i32("-17/5", -3);
    expect_fold_i32("-17%5", -2);
    expect_fold_i32("17/-5", -3);
    expect_fold_i32("17%-5", 2);
    expect_fold_i32("-(-3)+ +2", 5);
    expect_fold_i32("2147483647+1", INT64_C(-2147483648));
    expect_fold_i32("-2147483648-1", INT32_MAX);
    expect_fold_i32("65536*65536", 0);
    expect_fold_i32("-(-2147483648)", INT64_C(-2147483648));
    EXPECT_COMPILE_ERROR("fn f()->i32{return 2147483648;}", "outside the i32");
    EXPECT_COMPILE_ERROR("fn f()->i32{return -2147483649;}", "outside the i32");
    EXPECT_OPT_ERROR("fn f()->i32{return 1/0;}", "by zero");
    EXPECT_OPT_ERROR("fn f()->i32{return -2147483648%-1;}", "INT_MIN");
    EXPECT_OPT_ERROR("fn f()->i32{return -2147483648/-1;}", "INT_MIN");
    EXPECT_OPT_ERROR("fn f(x:i32)->i32{return x/0;}", "by zero");
}
static void names_and_types(void) {
    EXPECT_COMPILE_ERROR("fn f()->i32{x: i32 :=1;x=2;return x;}", "cannot assign");
    EXPECT_COMPILE_ERROR("fn f(x:i32)->i32{x=2;return x;}", "cannot assign");
    EXPECT_COMPILE_ERROR("fn f()->i32{x:i16=1;return x;}", "unknown or private type");
    EXPECT_COMPILE_ERROR("fn f()->i32{g: i32 :=1;return g();}", "not callable");
    EXPECT_COMPILE_ERROR("fn f()->i32{x:i32;}", "must return");
    EXPECT_COMPILE_ERROR("fn f()->i32{return a;a: i32 :=1;}", "unknown local");
    EXPECT_COMPILE_ERROR("fn f()->i32{return missing();}", "unknown function");
    EXPECT_COMPILE_ERROR("fn f()->i32{return g(1);}fn g()->i32{return 0;}", "argument count");
    EXPECT_COMPILE_ERROR("fn f()->i32{return 0;}fn f()->i32{return 1;}", "duplicate function");
    EXPECT_COMPILE_ERROR("fn f(a:i32,a:i32)->i32{return a;}", "duplicate local");
    EXPECT_COMPILE_ERROR("fn f()->i32{a: i32 :=1;a: i32 :=2;return a;}", "duplicate local");
    EXPECT_COMPILE_ERROR("fn f()->i32{a: i32 :=1;a=2;return a;}", "cannot assign");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:i32=a;return a;}", "unknown local");
    EXPECT_COMPILE_ERROR("fn f()->i32{a=1;return 0;}", "unknown name");
    EXPECT_COMPILE_ERROR("fn f()->i32{g: i32 :=1;return g();}", "not callable");
    EXPECT_COMPILE_ERROR("fn f()->i16{return 1;}", "unknown or private type");
    EXPECT_COMPILE_ERROR("fn f(a:f32)->i32{return 1;}", "unknown or private type");
    EXPECT_COMPILE_ERROR("fn f()->i32{a: i16 :=1;return 0;}", "unknown or private type");
    EXPECT_COMPILE_ERROR("fn f()->i32{}", "must return");
    EXPECT_COMPILE_ERROR("fn f()->i32{return 0;return missing;}", "unknown local");
}
static void pointers(void) {
    Compilation test = test_compile("pointer aliases",
        "fn main() -> i32 {\n"
        "    a: i32 = 1;\n"
        "    p: *i32 = &a;\n"
        "    *p = 2;\n"
        "    return a;\n"
        "}\n", TEST_O1);
    EXPECT_OPERATIONS(test.before, "main", ATRI_IR_LOCAL, 1);
    EXPECT_OPERATIONS(test.before, "main", ATRI_IR_STORE, 2);
    EXPECT_OPERATIONS(test.before, "main", ATRI_IR_LOAD, 1);
    AtriIRFunction *main = test_function(&test.after, "main");
    AtriId returned = test_instruction(&test.after, "main", ATRI_IR_RETURN, 0);
    CHECK(main->instructions[main->instructions[returned].left].kind == ATRI_IR_LOAD);
    test_destroy(&test);

    test = test_compile("pointer signature", "fn identity(p: *i32) -> *i32 { return p; }", TEST_O1);
    AtriIRFunction *identity = test_function(&test.after, "identity");
    CHECK(identity->parameter_types[0] == ATRI_IR_PTR_I32);
    CHECK(identity->return_type == ATRI_IR_PTR_I32);
    test_destroy(&test);

    EXPECT_COMPILE_ERROR("fn f()->i32{x:i32=1;p:*i32=x;return 0;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*i32=0;return 0;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*i32=null;return p;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{return *1;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*i32;return p+1;}", "null pointer");
    EXPECT_COMPILE_ERROR("fn f()->i32{x:i32;return &x;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{x:i32;return g(&x);}fn g(x:i32)->i32{return x;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:**i32;return 0;}", "one-level");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*i32;pp:*i32=&p;return 0;}", "taking the address");
    EXPECT_COMPILE_ERROR("fn f()->i32{x:i32 :=1;p:*i32=&x;return 0;}", "cannot take the address");
    EXPECT_COMPILE_ERROR("fn f(x:i32)->i32{p:*i32=&x;return 0;}", "cannot take the address");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*i32=&(1+2);return 0;}", "addressable");
    EXPECT_COMPILE_ERROR("fn f()->i32{(1+2)=3;return 0;}", "assignment requires");
    EXPECT_COMPILE_ERROR("fn f()->*i32{x:i32=1;return &x;}", "address of a local");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*i32;return *p;}", "null pointer");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*i32=null;*p=1;return 0;}", "null pointer");
    EXPECT_COMPILE_ERROR("fn f()->i32{x:i32=null;return x;}", "null requires a pointer");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*i32 :=null;return 0;}", "compile-time pointer constants");
    EXPECT_COMPILE_ERROR("fn main()->i32{n:i32 :=f();return n;}fn f()->i32{x:i32;p:*i32=&x;*p=3;return x;}", "compile-time pointer operations");
}
static void pointer_arithmetic(void) {
    Compilation test = test_compile("pointer arithmetic types",
        "fn move(p: *i32, n: i32) -> *i32 { return n + p; }\n"
        "fn distance(p: *i32, q: *i32) -> i64 { return p - q; }\n", TEST_O1);
    AtriIRFunction *move = test_function(&test.before, "move");
    AtriId offset_id = test_instruction(&test.before, "move", ATRI_IR_PTR_OFFSET, 0);
    const AtriIRInst *offset = &move->instructions[offset_id];
    CHECK(offset->type == ATRI_IR_PTR_I32);
    CHECK(move->instructions[offset->left].type == ATRI_IR_PTR_I32);
    CHECK(move->instructions[offset->right].type == ATRI_IR_I32);
    EXPECT_OPERATIONS(test.before, "distance", ATRI_IR_PTR_DIFF, 1);
    CHECK(test_function(&test.before, "distance")->return_type == ATRI_IR_I64);
    test_destroy(&test);

    EXPECT_COMPILE_ERROR("fn f(p:*i32,q:*i32)->*i32{return p+q;}", "cannot add two pointers");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->*i32{return 1-p;}", "subtract a pointer from an integer");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->*i32{return p*2;}", "supports only");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->*i32{return +p;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->*i32{return +null;}", "null requires a pointer");
    EXPECT_COMPILE_ERROR("fn f(p:*i32,q:*i32)->i32{return p-q;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->*i32{return null+0;}", "null pointer");
    EXPECT_COMPILE_ERROR("fn f()->i64{return null-null;}", "null pointer");
    EXPECT_OPT_ERROR("fn f(p:*i32)->*i32{return p+9223372036854775807;}", "signed 64-bit range");
    EXPECT_OPT_ERROR("fn f(p:*i32)->*i32{return p-(-9223372036854775808);}", "signed 64-bit range");
    EXPECT_COMPILE_ERROR("fn f(x:i32,y:i64)->i64{return x+y;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->i32{x:i32=p-p;return x;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i64{return 9223372036854775808;}", "i64 range");
    EXPECT_COMPILE_ERROR("fn f()->i64{return -9223372036854775809;}", "i64 range");
    EXPECT_COMPILE_ERROR("fn f()->i64{n:i64 :=1/0;return n;}", "by zero");
    EXPECT_COMPILE_ERROR("fn f()->i64{n:i64 :=-9223372036854775808/-1;return n;}", "INT_MIN");
    EXPECT_COMPILE_ERROR("fn f()->i64{n:i64 :=-9223372036854775808%-1;return n;}", "INT_MIN");
    EXPECT_COMPILE_ERROR("fn f()->i64{x:i64=1;p:*i32=&x;return x;}", "only i32 variables");
    expect_constant_i64("5000000000+7", INT64_C(5000000007));
    expect_constant_i64("+5000000000", INT64_C(5000000000));
    expect_constant_i64("9223372036854775807+1", INT64_MIN);
    expect_constant_i64("-9223372036854775808-1", INT64_MAX);
    expect_constant_i64("4294967296*4294967296", 0);
    expect_constant_i64("-(-9223372036854775808)", INT64_MIN);
    expect_constant_i64("-17/5", -3); expect_constant_i64("-17%5", -2);
}
static void compile_time_constants(void) {
    expect_constant_i32("fn main()->i32{a:i32 :=1;b:i32 :=a+2;return b;}", 3);
    expect_constant_i32("fn main()->i32{n:i32 :=add(1,2);return n;}fn add(a:i32,b:i32)->i32{return a+b;}", 3);
    expect_constant_i32("fn main()->i32{n:i32 :=helper(3);return n;}"
        "fn helper(x:i32)->i32{a:i32=x;a=a*2;k:i32 :=4;return a+k;}", 10);
    expect_constant_i32("fn main()->i32{n:i32 :=f(2);return n;}"
        "fn f(x:i32)->i32{return g(x)+1;}fn g(x:i32)->i32{return x*3;}", 7);
    expect_constant_i32("fn main()->i32{n:i32 :=2147483647+1;return n;}", INT64_C(-2147483648));
    expect_constant_i32("fn main()->i32{n:i32 :=-17%5;return n;}", -2);
    EXPECT_COMPILE_ERROR("fn f(x:i32)->i32{n:i32 :=x+1;return n;}", "runtime variable or parameter");
    EXPECT_COMPILE_ERROR("fn main()->i32{a:i32=1;n:i32 :=a+1;return n;}", "runtime variable or parameter");
    EXPECT_COMPILE_ERROR("fn main()->i32{n:i32 :=divide(0);return n;}fn divide(x:i32)->i32{return 1/x;}", "by zero");
    EXPECT_COMPILE_ERROR("fn main()->i32{n:i32 :=1/0;return n;}", "by zero");
    EXPECT_COMPILE_ERROR("fn main()->i32{n:i32 :=-2147483648/-1;return n;}", "INT_MIN");
    EXPECT_COMPILE_ERROR("fn main()->i32{return 1;n:i32 :=1/0;}", "by zero");
    EXPECT_COMPILE_ERROR("fn main()->i32{n:i32 :=recurse(0);return n;}fn recurse(x:i32)->i32{return recurse(x);}", "depth limit");
    EXPECT_COMPILE_ERROR("fn main()->i32{n:i32 :=f();return n;}fn f()->i32{m:i32 :=main();return m;}", "cyclic compile-time");
    EXPECT_COMPILE_ERROR("fn main()->i32{n:i32 :=f();return n;}fn f()->i32{x:i32 :=recurse();return 3;}fn recurse()->i32{return recurse();}", "depth limit");
    EXPECT_COMPILE_ERROR("fn main()->i32{n:i32 :=f();return n;}fn f()->i32{x:i32=recurse();return 3;}fn recurse()->i32{return recurse();}", "depth limit");
    char large[4096] = "fn main()->i32{n:i32 :=large(1);return n;}fn large(x:i32)->i32{a:i32=x;";
    size_t used = strlen(large);
    for (size_t i = 0; i < 80; ++i) { memcpy(large + used, "a=a+1;", 6); used += 6; }
    memcpy(large + used, "return a;}", 11);
    expect_constant_i32(large, 81);
}
static void c_calls_and_strings(void) {
    const char *code = "[[extern(C)]] fn puts(text:*char)->i32;"
        "fn text()->*char{return \"A\\n\\x42\\0tail\";}"
        "fn main()->i32{puts(text());return 0;}";
    for (TestMode mode = TEST_O0; mode <= TEST_O1; ++mode) {
        Compilation test = test_compile("external puts", code, mode);
        AtriIRFunction *puts_function = test_function(&test.after, "puts");
        CHECK(puts_function->external_c && puts_function->instruction_count == 0 && puts_function->block_count == 0);
        CHECK(puts_function->parameter_types[0] == ATRI_IR_PTR_CHAR);
        EXPECT_CALL(test.after, "main", "puts");
        CHECK(test.after.program.string_count == 1);
        const unsigned char expected[] = {'A', '\n', 'B', 0, 't', 'a', 'i', 'l', 0};
        CHECK(test.after.program.strings[0].length == sizeof(expected)-1);
        CHECK(memcmp(test.after.program.strings[0].bytes, expected, sizeof(expected)) == 0);
        EXPECT_VALID_IR(test.after);
        test_destroy(&test);
    }
    Compilation test = test_compile("empty C string", "fn empty()->*char{return \"\";}", TEST_O1);
    CHECK(test.after.program.strings[0].length == 0 && test.after.program.strings[0].bytes[0] == 0);
    test_destroy(&test);
    test = test_compile("UTF-8 bytes", "fn text()->*char{return \"你好\";}", TEST_O1);
    const unsigned char utf8[] = {0xe4, 0xbd, 0xa0, 0xe5, 0xa5, 0xbd, 0};
    CHECK(test.after.program.strings[0].length == 6);
    CHECK(memcmp(test.after.program.strings[0].bytes, utf8, sizeof(utf8)) == 0);
    test_destroy(&test);
    test = test_compile("arbitrary string bytes", "fn bytes()->*char{return \"\\xff\\x80\\x00\";}", TEST_O1);
    const unsigned char bytes[] = {0xff, 0x80, 0, 0};
    CHECK(test.after.program.strings[0].length == sizeof(bytes)-1);
    CHECK(memcmp(test.after.program.strings[0].bytes, bytes, sizeof(bytes)) == 0);
    test_destroy(&test);
    test = test_compile("forward C declaration", "fn caller()->i32{return ext();}[[extern(C)]] fn ext()->i32;", TEST_O1);
    EXPECT_CALL(test.after, "caller", "ext");
    test_destroy(&test);
    test = test_compile("declaration only", "[[extern(C)]] fn ext()->i32;", TEST_O1);
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
    test = test_compile("character pointer arithmetic",
        "fn move(p:*char,n:i64)->*char{return n+p;}"
        "fn distance(p:*char,q:*char)->i64{return p-q;}"
        "fn is_null(p:*char)->bool{return null==p;}"
        "fn default_null()->bool{p:*char;return p==null;}", TEST_O1);
    CHECK(test_function(&test.after, "move")->return_type == ATRI_IR_PTR_CHAR);
    EXPECT_OPERATIONS(test.after, "distance", ATRI_IR_PTR_DIFF, 1);
    EXPECT_RETURN_BOOL(test.after, "default_null", true);
    test_destroy(&test);

    EXPECT_COMPILE_ERROR("[[extern(C)]] fn puts(p:*char)->i32;fn f()->i32{return puts(1);}", "type mismatch");
    EXPECT_COMPILE_ERROR("[[extern(C)]] fn puts(p:*char)->i32;fn f()->i32{return puts();}", "argument count");
    EXPECT_COMPILE_ERROR("[[extern(C)]] fn f()->i32;[[extern(C)]] fn f()->i64;", "duplicate function");
    EXPECT_COMPILE_ERROR("[[extern(C)]] fn f(a:i32,a:i32)->i32;", "duplicate local");
    EXPECT_COMPILE_ERROR("fn f()->i32{return \"text\";}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->*char{return p;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->char{return 1;}", "char values");
    EXPECT_COMPILE_ERROR("fn f()->*char{p:*char :=\"text\";return p;}", "compile-time pointer constants");
    EXPECT_COMPILE_ERROR("fn f(p:*char,q:*i32)->i64{return p-q;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f(p:*char)->bool{return p<null;}", "ordered comparisons");
    EXPECT_COMPILE_ERROR("fn f()->*char{return null+1;}", "null pointer");
    EXPECT_COMPILE_ERROR("[[extern(C)]] fn ext()->i32;fn f()->i32{n:i32 :=ext();return n;}", "cannot execute at compile time");
    EXPECT_COMPILE_ERROR("[[extern(C)]] fn ext()->i32;fn wrapper()->i32{ext();return 1;}"
        "fn f()->i32{n:i32 :=wrapper();return n;}", "cannot execute at compile time");
    EXPECT_COMPILE_ERROR("[[extern(C)]] fn puts(p:*char)->i32;fn f()->i32{n:i32 :=puts(\"text\");return n;}", "cannot execute at compile time");
}
static void arrays_and_run(void) {
    Compilation test = test_compile("run and conditional",
        "fn max(a:i32,b:i32)->i32{return a>b?a:b;}"
        "run a:i32 := max(1,2);fn main()->i32{return a;}", TEST_O0);
    EXPECT_RETURN_I32(test.before, "main", 2);
    EXPECT_NO_CALLS(test.before, "main");
    test_destroy(&test);
    test = test_compile("run expression", "fn f()->i64{return 9;}fn main()->i64{return run f();}", TEST_O0);
    EXPECT_RETURN_I64(test.before, "main", 9);
    EXPECT_NO_CALLS(test.before, "main");
    test_destroy(&test);
    test = test_compile("lazy conditional", "[[extern(C)]]fn ext()->i32;fn main()->i32{n:i32 := false?ext():7;return n;}", TEST_O0);
    EXPECT_RETURN_I32(test.before, "main", 7);
    EXPECT_NO_CALLS(test.before, "main");
    test_destroy(&test);
    test = test_compile("constant byte", "fn byte()->u8{n:u8 := 250+10;return n;}", TEST_O1);
    AtriIRFunction *byte = test_function(&test.after, "byte");
    CHECK(byte->instructions[byte->instruction_count-2].immediate == 4);
    test_destroy(&test);
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32;return a[2];}", "out of bounds");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32;return a[-1];}", "out of bounds");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32;return a[1+1];}", "out of bounds");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32;n:i32 := 2;return a[n];}", "out of bounds");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[0]i32;return a[0];}", "out of bounds");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32={1,2,3};return 0;}", "too many array");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32={.x=1};return 0;}", "positional");
    EXPECT_COMPILE_ERROR("struct S{x:i32;}fn f()->i32{s:S={1};return 0;}", "named fields");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32;return a[true];}", "index requires");
    EXPECT_COMPILE_ERROR("fn f(a:[2]i32)->i32{a[0]=1;return 0;}", "read-only");
    EXPECT_COMPILE_ERROR("fn f()->[2]i32{return {1,2};}fn g()->i32{f()[0]=1;return 0;}", "temporary array");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32;b:[3]i32=a;return 0;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[-1]i32;return 0;}", "array length");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[4096]i32;return 0;}", "size limit");
    EXPECT_COMPILE_ERROR("N:i32=2;fn f()->i32{a:[N]i32;return 0;}", "array length");
    EXPECT_COMPILE_ERROR("struct S{a:[1]S;}fn f()->i32{return 0;}", "cyclic");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]u8={256};return 0;}", "outside");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]u8;return a[0];}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->*i32{a:[2]i32;return &a[0];}", "address of a local");
    EXPECT_COMPILE_ERROR("fn f(a:i32)->i32{return run a;}", "runtime variable or parameter");
    EXPECT_COMPILE_ERROR("[[extern(C)]]fn ext()->i32;fn f()->i32{return run ext();}", "cannot execute at compile time");
    EXPECT_COMPILE_ERROR("fn f()->i32{return run (1/0);}", "by zero");
    EXPECT_COMPILE_ERROR("fn f()->i32{return 1?2:3;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{return true?1:false;}", "type mismatch");
    test = test_compile("mutable run declaration",
        "fn max(a:i32,b:i32)->i32{return a>b?a:b;}"
        "fn main()->i32{run a:i32=max(1,2)+1;a=a+4;return a;}", TEST_O0);
    EXPECT_NO_CALLS(test.before, "main");
    EXPECT_RETURN_I32(test.after, "main", 7);
    test_destroy(&test);
    EXPECT_PARSE_ERROR("fn f()->i32{run a:i32;return a;}", "explicit compile-time initializer");
    EXPECT_COMPILE_ERROR("fn f(x:i32)->i32{run a:i32=x+1;return a;}", "runtime variable or parameter");
    EXPECT_COMPILE_ERROR("fn f()->i32{x:i32=1;run a:i32=x+1;return a;}", "runtime variable or parameter");
    EXPECT_COMPILE_ERROR("[[extern(C)]]fn ext()->i32;fn f()->i32{run a:i32=ext();return a;}", "cannot execute at compile time");
    EXPECT_COMPILE_ERROR("fn f()->i32{run a:i32=1/0;return a;}", "by zero");
    EXPECT_COMPILE_ERROR("fn f()->i32{run a:i32:=1;a=2;return a;}", "cannot assign");
    EXPECT_COMPILE_ERROR("N:i32 :=2;fn f(N:i32)->i32{a:[N]i32;return 0;}", "array length");
    EXPECT_COMPILE_ERROR("N:i32 :=2;fn f()->i32{N:i32=3;a:[N]i32;return 0;}", "array length");
    EXPECT_COMPILE_ERROR("N:i32 :=2;fn f()->i32{for(N:i32=3;false;){a:[N]i32;}return 0;}", "array length");
    EXPECT_COMPILE_ERROR("[[extern(C)]]fn f(a:[2]i32)->i32;", "arrays and zero-size");
    EXPECT_COMPILE_ERROR("a:[2]i32;fn main()->i32{return 0;}", "array storage");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32 := {1,2};return 0;}", "array constants");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32;return run a[0];}", "runtime variable or parameter");
    EXPECT_COMPILE_ERROR("fn f()->i32{n:i32 :=f();a:[n]i32;return 0;}", "array length");
    EXPECT_COMPILE_ERROR("N:i32 :=-1;fn f()->i32{a:[N]i32;return 0;}", "array length");
    test = test_compile("recursive array pointer", "struct Node{next:*[2]Node;}fn f()->i32{return 0;}", TEST_O0);
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);

}
static void conversions_and_layout(void) {
    expect_fold_i32("cast(i32, cast(u8, 300))", 44);
    expect_fold_i32("cast(i32, sizeof([3]i32))", 12);
    expect_constant_i32("fn main()->i32{n:i32 :=cast(i32, cast(u64,-1));return n;}", -1);
    expect_constant_i32("fn main()->i32{n:i32 :=cast(i32, sizeof([0]i64));return n;}", 0);
    Compilation test = test_compile("u64 constant", "fn f()->u64{n:u64 :=18446744073709551615+2;return n;}", TEST_O0);
    AtriIRFunction *f = test_function(&test.before, "f");
    CHECK(f->instructions[f->instructions[f->instruction_count-1].left].immediate == 1);
    test_destroy(&test);
    test = test_compile("run conversion", "fn f()->u64{return run cast(u64,-1);}", TEST_O0);
    f = test_function(&test.before, "f");
    CHECK(f->instructions[f->instructions[f->instruction_count-1].left].immediate == UINT64_MAX);
    test_destroy(&test);
    test = test_compile("constant unsigned comparison", "fn f()->bool{n:bool := cast(u64,-1)>0;return n;}", TEST_O0);
    EXPECT_RETURN_BOOL(test.before, "f", true);
    test_destroy(&test);
    test = test_compile("grouped type spelling", "fn f()->i32{u8:i32=2;return (u8)+3;}", TEST_O1);
    EXPECT_RETURN_I32(test.after, "f", 5);
    test_destroy(&test);
    EXPECT_COMPILE_ERROR("fn f()->u64{return 18446744073709551616;}", "u64 range");
    EXPECT_COMPILE_ERROR("fn f()->i64{x:u64=1;return x;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->u64{x:i64=1;return x;}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{return sizeof(i32);}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->i32{return cast(i32,true);}", "integer operands");
    EXPECT_COMPILE_ERROR("fn f()->bool{return cast(bool,1);}", "integer target");
    EXPECT_COMPILE_ERROR("fn f(p:*i32)->u64{return cast(u64,p);}", "integer operands");
    EXPECT_COMPILE_ERROR("fn f()->*i32{return cast(*i32,1);}", "pointer operands");
    EXPECT_COMPILE_ERROR("fn f()->u64{return sizeof(void);}", "no object layout");
    EXPECT_COMPILE_ERROR("fn f()->u64{return alignof(Missing);}", "unknown or private type");
    EXPECT_COMPILE_ERROR("struct S{bytes:[sizeof(S)]u8;}", "cyclic");
    EXPECT_COMPILE_ERROR("N:u64 :=sizeof([N]u8);fn f()->i32{a:[N]u8;return 0;}", "resource limit");
    EXPECT_COMPILE_ERROR("fn f()->i32{a:[2]i32;i:u64 :=18446744073709551615;return a[i];}", "out of bounds");
    EXPECT_COMPILE_ERROR("fn f()->i32{n:u64 :=1/0;return 0;}", "by zero");
    EXPECT_COMPILE_ERROR("fn f(p:*i32,n:u64)->*i32{return p+n;}", "pointer offset requires");
    EXPECT_PARSE_ERROR("fn f()->u8{return cast<u8>1;}", "expected '('");
    EXPECT_PARSE_ERROR("fn f()->u8{return cast(u8 1);}", "expected ','");
    EXPECT_PARSE_ERROR("fn f()->u64{return sizeof(1);}", "object type");
}
static void c_variadic_and_void(void) {
    Compilation test = test_compile("C promotions", "[[extern(C)]]fn c(tag:i32,...)->i32;"
        "fn f(b:u8,flag:bool)->i32{return c(1,b,flag);}", TEST_O0);
    AtriIRFunction *f = test_function(&test.before, "f");
    AtriId call = test_instruction(&test.before, "f", ATRI_IR_CALL, 0);
    CHECK(test_function(&test.before, "c")->variadic);
    CHECK(f->instructions[call].argument_count == 3);
    EXPECT_OPERATIONS(test.before, "f", ATRI_IR_C_PROMOTE, 2);
    EXPECT_VALID_IR(test.before);
    test_destroy(&test);
    expect_constant_i32("fn cleanup()->void{return;}fn eval()->i32{cleanup();return 9;}fn main()->i32{n:i32 :=eval();return n;}", 9);
    test = test_compile("opaque pointer", "struct Handle;fn f(p:*Handle)->*void{return cast(*void,p);}", TEST_O1);
    CHECK(test.before.program.structs[0].opaque);
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
    test = test_compile("void null", "p:*void=cast(*void,null);fn f()->u64{return sizeof(*void);}", TEST_O0);
    CHECK(test.before.program.globals[0].type == ATRI_IR_PTR_VOID);
    test_destroy(&test);
    EXPECT_COMPILE_ERROR("fn f()->i32{return;}", "must return a value");
    EXPECT_COMPILE_ERROR("fn f()->void{return 1;}", "without a value");
    EXPECT_COMPILE_ERROR("fn f()->void{x:void;}", "no object layout");
    EXPECT_COMPILE_ERROR("fn f(a:void)->i32{return 0;}", "no object layout");
    EXPECT_COMPILE_ERROR("fn f()->void{}fn g()->i32{return f();}", "type mismatch");
    EXPECT_COMPILE_ERROR("fn f()->void{}fn g()->i32{return run f();}", "type mismatch");
    EXPECT_COMPILE_ERROR("struct O;fn f()->u64{return sizeof(O);}", "opaque struct");
    EXPECT_COMPILE_ERROR("struct O;fn f()->i32{x:O;return 0;}", "opaque struct");
    EXPECT_COMPILE_ERROR("struct O;struct S{x:O;}", "opaque struct");
    EXPECT_COMPILE_ERROR("struct O;fn f(p:*O)->O{return *p;}", "opaque struct");
    EXPECT_COMPILE_ERROR("struct O;fn f(p:*O)->*O{return p+1;}", "incomplete");
    EXPECT_COMPILE_ERROR("fn f(p:*void)->i32{return *p;}", "complete object");
    EXPECT_COMPILE_ERROR("fn f(p:*[1]void)->i32{return 0;}", "complete object");
    EXPECT_COMPILE_ERROR("struct F;fn f(p:*[1]F)->i32{return 0;}", "complete object");
    EXPECT_COMPILE_ERROR("fn f(p:*void)->*void{return p+1;}", "incomplete");
    EXPECT_COMPILE_ERROR("fn f(x:i32)->*void{return cast(*void,x);}", "pointer operands");
    EXPECT_COMPILE_ERROR("fn f()->*void{a:i32;return cast(*void,&a);}", "address of a local");
    EXPECT_COMPILE_ERROR("fn f()->i32{p:*void=null;return *cast(*i32,p);}", "null pointer");
    EXPECT_COMPILE_ERROR("fn f(p:*[1]void)->i32{return 0;}", "complete object");
    EXPECT_COMPILE_ERROR("struct O;fn f(p:*[1]O)->i32{return 0;}", "complete object");
    EXPECT_COMPILE_ERROR("[[extern(C)]]fn c(x:i32,...)->i32;fn f()->i32{return c();}", "argument count");
    EXPECT_COMPILE_ERROR("[[extern(C)]]fn c(x:i32,...)->i32;fn f(b:u8)->i32{return c(b);}", "type mismatch");
    EXPECT_COMPILE_ERROR("[[extern(C)]]fn c(x:i32,...)->i32;fn f()->i32{a:[2]i32;return c(0,a);}", "explicit element pointer");
    EXPECT_COMPILE_ERROR("struct E{}[[extern(C)]]fn c(x:i32,...)->i32;fn f()->i32{e:E;return c(0,e);}", "supported C ABI value");
    EXPECT_COMPILE_ERROR("[[extern(C)]]fn c(x:i32,...)->i32;fn f()->i32{return run c(0,1);}", "cannot execute at compile time");
    EXPECT_COMPILE_ERROR("[[extern(C)]]fn c(x:i32,...)->i32;fn f()->i32{return c(0,null);}", "pointer type context");
    EXPECT_PARSE_ERROR("fn f(x:i32,...)->i32{return 0;}", "require an extern(C)");
    EXPECT_PARSE_ERROR("[[extern(C)]]fn f(x:i32,...,y:i32)->i32;", "last parameter");
}
int main(void) {
    c_variadic_and_void();
    conversions_and_layout();
    arrays_and_run();
    arithmetic();
    names_and_types();
    compile_time_constants();
    pointers();
    pointer_arithmetic();
    c_calls_and_strings();
    puts("compiler tests passed");
    return 0;
}
