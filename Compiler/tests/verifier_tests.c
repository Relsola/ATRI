#include "test_support.h"

/* 此组刻意破坏 IR，允许直接操作内部字段；普通语言行为用例使用具名预期，避免依赖内部布局。 */
static void invalid_value_and_terminator(void) {
    Compilation test = test_compile("invalid value", "fn main() -> i32 { return 1 + 2; }", TEST_NO_OPT);
    AtriIRFunction *main = test_function(&test.before, "main");
    AtriId sum = test_instruction(&test.before, "main", ATRI_IR_BINARY, 0);
    AtriId saved = main->instructions[sum].left;
    main->instructions[sum].left = main->instruction_count;
    EXPECT_INVALID_IR(test.before);
    main->instructions[sum].left = saved;
    EXPECT_VALID_IR(test.before);

    AtriId returned = test_instruction(&test.before, "main", ATRI_IR_RETURN, 0);
    main->instructions[returned].kind = ATRI_IR_CONST;
    EXPECT_INVALID_IR(test.before);
    test_destroy(&test);
}

static void invalid_memory_operands(void) {
    Compilation test = test_compile("invalid store",
        "fn write(p: *i32, x: i32) -> i32 { *p = x; return *p; }", TEST_NO_OPT);
    AtriIRFunction *write = test_function(&test.before, "write");
    AtriId store = test_instruction(&test.before, "write", ATRI_IR_STORE, 0);
    AtriId integer_parameter = test_instruction(&test.before, "write", ATRI_IR_PARAM, 1);
    write->instructions[store].left = integer_parameter;
    EXPECT_INVALID_IR(test.before);
    test_destroy(&test);

    test = test_compile("invalid pointer offset",
        "fn move(p: *i32, n: i32) -> *i32 { return p + n; }", TEST_NO_OPT);
    AtriIRFunction *move = test_function(&test.before, "move");
    AtriId offset = test_instruction(&test.before, "move", ATRI_IR_PTR_OFFSET, 0);
    move->instructions[offset].right = move->instructions[offset].left;
    EXPECT_INVALID_IR(test.before);
    test_destroy(&test);
}

static void invalid_control_flow(void) {
    Compilation test = test_compile("invalid branch",
        "fn choose(b: bool) -> i32 { if (b) { return 1; } else { return 2; } }", TEST_O1);
    AtriIRFunction *choose = test_function(&test.after, "choose");
    AtriId branch = test_instruction(&test.after, "choose", ATRI_IR_BRANCH, 0);
    AtriId first_value = test_instruction(&test.after, "choose", ATRI_IR_CONST, 0);
    AtriId last_return = test_instruction(&test.after, "choose", ATRI_IR_RETURN, 1);

    AtriId saved_target = choose->instructions[branch].target;
    choose->instructions[branch].target = choose->block_count;
    EXPECT_INVALID_IR(test.after);
    choose->instructions[branch].target = saved_target;
    EXPECT_VALID_IR(test.after);

    /* 左分支的定义不支配右分支；即使指令下标在前，也不能供另一分支的 return 使用。 */
    AtriId saved_value = choose->instructions[last_return].left;
    choose->instructions[last_return].left = first_value;
    EXPECT_INVALID_IR(test.after);
    choose->instructions[last_return].left = saved_value;
    EXPECT_VALID_IR(test.after);

    choose->instructions[branch].left = first_value;
    EXPECT_INVALID_IR(test.after);
    test_destroy(&test);
}

static void invalid_signature(void) {
    Compilation test = test_compile("invalid call signature",
        "fn caller(b: bool) -> i32 { return choose(b); }"
        "fn choose(b: bool) -> i32 { if (b) { return 1; } return 2; }", TEST_O1);
    AtriIRFunction *choose = test_function(&test.after, "choose");
    AtriIRType *saved_types = choose->parameter_types;
    choose->parameter_types = NULL;
    EXPECT_INVALID_IR(test.after);
    choose->parameter_types = saved_types;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
}

static void invalid_external_and_string(void) {
    Compilation test = test_compile("string reference", "fn text()->*char{return \"text\";}", TEST_O1);
    AtriIRFunction *text = test_function(&test.after, "text");
    AtriId string = test_instruction(&test.after, "text", ATRI_IR_STRING, 0);
    text->instructions[string].immediate = test.after.program.string_count;
    EXPECT_INVALID_IR(test.after);
    text->instructions[string].immediate = 0;
    EXPECT_VALID_IR(test.after);
    text->instructions[string].type = ATRI_IR_PTR_I32;
    EXPECT_INVALID_IR(test.after);
    text->instructions[string].type = ATRI_IR_PTR_CHAR;
    AtriIRString *data = &test.after.program.strings[0];
    data->bytes[data->length] = 1;
    EXPECT_INVALID_IR(test.after);
    data->bytes[data->length] = 0;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);

    test = test_compile("external body", "[[extern(C)]] fn ext()->i32;fn f()->i32{return 0;}", TEST_O1);
    test_function(&test.after, "f")->external_c = true;
    EXPECT_INVALID_IR(test.after);
    test_destroy(&test);
}
static void invalid_globals(void) {
    Compilation test = test_compile("global verifier", "export x: bool; fn main()->i32{if(x){return 1;}return 0;}", TEST_O1);
    test.after.program.globals[0].bits = 2;
    EXPECT_INVALID_IR(test.after);
    test.after.program.globals[0].bits = 0;
    EXPECT_VALID_IR(test.after);
    AtriIRFunction *main = test_function(&test.after, "main");
    AtriId location = test_instruction(&test.after, "main", ATRI_IR_LOCAL, 0);
    main->instructions[location].immediate = 2;
    EXPECT_INVALID_IR(test.after);
    main->instructions[location].immediate = 1;
    test.after.program.globals[0].constant = true;
    EXPECT_INVALID_IR(test.after);
    test.after.program.globals[0].constant = false;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
}
static void invalid_structs(void) {
    Compilation test = test_compile("struct verifier", "struct S{a:i32;b:i64;}fn get(s:S)->i64{return s.b;}", TEST_O1);
    EXPECT_VALID_IR(test.after);
    AtriIRFunction *get = test_function(&test.after, "get");
    AtriId field = test_instruction(&test.after, "get", ATRI_IR_FIELD, 0);
    uint64_t original = get->instructions[field].immediate;
    get->instructions[field].immediate = test.after.program.field_count;
    EXPECT_INVALID_IR(test.after);
    get->instructions[field].immediate = original;
    EXPECT_VALID_IR(test.after);
    size_t offset = test.after.program.fields[1].offset;
    test.after.program.fields[1].offset = 1;
    EXPECT_INVALID_IR(test.after);
    test.after.program.fields[1].offset = offset;
    EXPECT_VALID_IR(test.after);
    get->instructions[field].type = ATRI_IR_I32;
    EXPECT_INVALID_IR(test.after);
    get->instructions[field].type = ATRI_IR_I64;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
}
int main(void) {
    invalid_value_and_terminator();
    invalid_memory_operands();
    invalid_control_flow();
    invalid_signature();
    invalid_external_and_string();
    invalid_globals();
    invalid_structs();
    puts("IR verifier tests passed");
    return 0;
}
