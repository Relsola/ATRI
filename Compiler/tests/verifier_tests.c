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
    Compilation test = test_compile("string reference", "fn text()->*u8{return \"text\";}", TEST_O1);
    AtriIRFunction *text = test_function(&test.after, "text");
    AtriId string = test_instruction(&test.after, "text", ATRI_IR_STRING, 0);
    text->instructions[string].immediate = test.after.program.string_count;
    EXPECT_INVALID_IR(test.after);
    text->instructions[string].immediate = 0;
    EXPECT_VALID_IR(test.after);
    text->instructions[string].type = ATRI_IR_PTR_I32;
    EXPECT_INVALID_IR(test.after);
    text->instructions[string].type = ATRI_IR_PTR_U8;
    AtriIRString *data = &test.after.program.strings[0];
    data->bytes[data->length] = 1;
    EXPECT_INVALID_IR(test.after);
    data->bytes[data->length] = 0;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);

    test = test_compile("external body", "#extern(C) fn ext()->i32;fn f()->i32{return 0;}", TEST_O1);
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
static void invalid_arrays(void) {
    Compilation test = test_compile("array verifier", "fn f(a:[2]i32,i:i32)->i32{return a[i];}", TEST_O0);
    AtriIRFunction *f = test_function(&test.after, "f");
    AtriId index = test_instruction(&test.after, "f", ATRI_IR_INDEX, 0);
    EXPECT_VALID_IR(test.after);
    f->instructions[index].type = ATRI_IR_U8;
    EXPECT_INVALID_IR(test.after);
    f->instructions[index].type = ATRI_IR_I32;
    AtriId original = f->instructions[index].right;
    f->instructions[index].right = index;
    EXPECT_INVALID_IR(test.after);
    f->instructions[index].right = original;
    test.after.program.arrays[0].element = ATRI_IR_ARRAY;
    EXPECT_INVALID_IR(test.after);
    test.after.program.arrays[0].element = ATRI_IR_I32;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
}
static void invalid_conversions(void) {
    Compilation test = test_compile("cast verifier", "fn f(a:i32)->u64{return cast(u64,a);}", TEST_O0);
    AtriIRFunction *f = test_function(&test.after, "f");
    AtriId cast = test_instruction(&test.after, "f", ATRI_IR_CAST, 0);
    EXPECT_VALID_IR(test.after);
    f->instructions[cast].left = cast;
    EXPECT_INVALID_IR(test.after);
    f->instructions[cast].left = 0;
    f->instructions[cast].type = ATRI_IR_BOOL;
    EXPECT_INVALID_IR(test.after);
    f->instructions[cast].type = ATRI_IR_U64;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
}
static void invalid_variadic_and_void(void) {
    Compilation test = test_compile("vararg verifier", "#extern(C)fn c(x:i32,...)->void;"
        "fn f(b:u8)->void{c(1,b);}", TEST_O0);
    AtriIRFunction *f = test_function(&test.after, "f");
    AtriId call = test_instruction(&test.after, "f", ATRI_IR_CALL, 0);
    AtriId promoted = f->arguments[f->instructions[call].first_argument+1];
    EXPECT_VALID_IR(test.after);
    f->arguments[f->instructions[call].first_argument+1] = f->instructions[promoted].left;
    EXPECT_INVALID_IR(test.after);
    f->arguments[f->instructions[call].first_argument+1] = promoted;
    f->instructions[promoted].type = ATRI_IR_U8;
    EXPECT_INVALID_IR(test.after);
    f->instructions[promoted].type = ATRI_IR_I32;
    f->instructions[f->instruction_count-1].left = 0;
    EXPECT_INVALID_IR(test.after);
    f->instructions[f->instruction_count-1].left = ATRI_NONE;
    f->variadic = true;
    EXPECT_INVALID_IR(test.after);
    f->variadic = false;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
    test = test_compile("opaque verifier", "struct O;fn f(p:*O)->*O{return p;}", TEST_O0);
    test.after.program.structs[0].size = 1;
    EXPECT_INVALID_IR(test.after);
    test.after.program.structs[0].size = 0;
    EXPECT_VALID_IR(test.after);
    test_destroy(&test);
}
static void invalid_integer_widths_and_shifts(void) {
    Compilation test = test_compile("narrow constant verifier", "fn f()->i8{return -1;}", TEST_NO_OPT);
    AtriIRFunction *f = test_function(&test.before, "f");
    AtriId constant = test_instruction(&test.before, "f", ATRI_IR_CONST, 0);
    f->instructions[constant].immediate = 256;
    EXPECT_INVALID_IR(test.before);
    f->instructions[constant].immediate = 255;
    EXPECT_VALID_IR(test.before);
    test_destroy(&test);
    test = test_compile("independent shift count", "fn f(a:u16,n:u64)->u16{return a<<n;}", TEST_NO_OPT);
    f = test_function(&test.before, "f");
    AtriId operation = test_instruction(&test.before, "f", ATRI_IR_BINARY, 0);
    AtriId count = f->instructions[operation].right;
    EXPECT_VALID_IR(test.before);
    f->parameter_types[1] = ATRI_IR_BOOL;
    f->instructions[count].type = ATRI_IR_BOOL;
    EXPECT_INVALID_IR(test.before);
    f->parameter_types[1] = ATRI_IR_U64;
    f->instructions[count].type = ATRI_IR_U64;
    f->instructions[operation].op = ATRI_BIT_AND;
    EXPECT_INVALID_IR(test.before);
    f->instructions[operation].op = ATRI_SHL;
    EXPECT_VALID_IR(test.before);
    test_destroy(&test);
    test = test_compile("incomplete pointer arithmetic", "fn f(p:*u16,n:i64)->*u16{return p+n;}", TEST_NO_OPT);
    f = test_function(&test.before, "f");
    f->parameter_types[0] = ATRI_IR_PTR_VOID;
    f->return_type = ATRI_IR_PTR_VOID;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        if (f->instructions[i].type == ATRI_IR_PTR_U16) { f->instructions[i].type = ATRI_IR_PTR_VOID; }
    }
    EXPECT_INVALID_IR(test.before);
    test_destroy(&test);
}
static void invalid_static_structs(void) {
    Compilation test = test_compile("static object verifier",
        "struct S{flag:bool;p:*i32;flags:[2]bool;}g:S={.flag=true};", TEST_NO_OPT);
    AtriIRGlobal *global = &test.before.program.globals[0];
    EXPECT_VALID_IR(test.before);
    size_t size = global->data_size;
    --global->data_size;
    EXPECT_INVALID_IR(test.before);
    global->data_size = size;
    global->data[0] = 2;
    EXPECT_INVALID_IR(test.before);
    global->data[0] = 1;
    global->data[8] = 1;
    EXPECT_INVALID_IR(test.before);
    global->data[8] = 0;
    global->data[17] = 2;
    EXPECT_INVALID_IR(test.before);
    global->data[17] = 0;
    global->constant = true;
    EXPECT_INVALID_IR(test.before);
    global->constant = false;
    unsigned char *data = global->data;
    global->data = NULL;
    EXPECT_INVALID_IR(test.before);
    global->data = data;
    EXPECT_VALID_IR(test.before);
    test_destroy(&test);
}
static void invalid_union_layout(void) {
    Compilation test = test_compile("union verifier",
        "union U{flag:bool;pointer:*i32;bits:u64;bytes:[9]u8;}"
        "struct Box{u:U;flag:bool;}g:Box={.u={.bits=18446744073709551615}};", TEST_NO_OPT);
    AtriIRProgram *p = &test.before.program;
    AtriIRStruct *t = &p->structs[0];
    EXPECT_VALID_IR(test.before); /* bits 初值不要求同时是合法 bool 或 null 指针。 */
    p->fields[1].offset = 8;
    EXPECT_INVALID_IR(test.before);
    p->fields[1].offset = 0;
    t->size = 24;
    EXPECT_INVALID_IR(test.before);
    t->size = 16;
    t->alignment = 4;
    EXPECT_INVALID_IR(test.before);
    t->alignment = 8;
    p->fields[0].default_bits = 1;
    EXPECT_INVALID_IR(test.before);
    p->fields[0].default_bits = 0;
    t->is_union = false;
    EXPECT_INVALID_IR(test.before);
    t->is_union = true;
    p->globals[0].data[16] = 2;
    EXPECT_INVALID_IR(test.before);
    p->globals[0].data[16] = 0;
    EXPECT_VALID_IR(test.before);
    test_destroy(&test);
}
static void invalid_enum_metadata(void) {
    Compilation enumeration = test_compile("enum metadata verifier", "F::enum(bit8){A,B}", TEST_NO_OPT);
    AtriIRProgram *enum_ir = &enumeration.before.program;
    EXPECT_VALID_IR(enumeration.before);
    enum_ir->variants[0].bits = 0;
    EXPECT_INVALID_IR(enumeration.before);
    enum_ir->variants[0].bits = 1;
    enum_ir->variants[1].bits = 1;
    EXPECT_INVALID_IR(enumeration.before);
    enum_ir->variants[1].bits = 2;
    enum_ir->enums[0].underlying = ATRI_IR_BOOL;
    EXPECT_INVALID_IR(enumeration.before);
    enum_ir->enums[0].underlying = ATRI_IR_U8;
    ++enum_ir->enums[0].variant_count;
    EXPECT_INVALID_IR(enumeration.before);
    --enum_ir->enums[0].variant_count;
    enum_ir->enums[0].state = 1;
    EXPECT_INVALID_IR(enumeration.before);
    enum_ir->enums[0].state = 2;
    EXPECT_VALID_IR(enumeration.before);
    test_destroy(&enumeration);
}
int main(void) {
    invalid_enum_metadata();
    invalid_union_layout();
    invalid_static_structs();
    invalid_integer_widths_and_shifts();
    invalid_value_and_terminator();
    invalid_memory_operands();
    invalid_control_flow();
    invalid_signature();
    invalid_external_and_string();
    invalid_globals();
    invalid_structs();
    invalid_arrays();
    invalid_variadic_and_void();
    invalid_conversions();
    puts("IR verifier tests passed");
    return 0;
}
