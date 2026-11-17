#ifndef ATRI_IR_H
#define ATRI_IR_H
#include "frontend.h"
#include <stdint.h>

/* 类型描述 IR 值；LOCAL/FIELD/INDEX 例外，type 描述存储内容，本身不能作为普通值或指针使用。 */
typedef enum { ATRI_IR_I32, ATRI_IR_I64, ATRI_IR_PTR_I32, ATRI_IR_BOOL, ATRI_IR_PTR_CHAR, ATRI_IR_INVALID,
    ATRI_IR_U8, ATRI_IR_PTR_U8, ATRI_IR_U64, ATRI_IR_VOID, ATRI_IR_PTR_VOID, ATRI_IR_STRUCT = 256, ATRI_IR_ARRAY = 524288, ATRI_IR_PTR_STRUCT = 1048576 } AtriIRType;
typedef enum {
    ATRI_IR_NOP, ATRI_IR_PARAM, ATRI_IR_CONST, ATRI_IR_UNARY,
    ATRI_IR_BINARY, ATRI_IR_CALL, ATRI_IR_RETURN,
    ATRI_IR_LOCAL, ATRI_IR_LOAD, ATRI_IR_STORE,
    ATRI_IR_PTR_OFFSET, ATRI_IR_PTR_DIFF, ATRI_IR_ADDRESS, ATRI_IR_COMPARE,
    ATRI_IR_LABEL, ATRI_IR_JUMP, ATRI_IR_BRANCH, ATRI_IR_UNREACHABLE, ATRI_IR_STRING, ATRI_IR_ZERO, ATRI_IR_FIELD,
    ATRI_IR_INDEX, ATRI_IR_CAST, ATRI_IR_C_PROMOTE, ATRI_IR_LAZY_SELECT,
    ATRI_IR_LAZY_LOGICAL /* 常量初始化中的私有短路指令，公开可执行 IR 中不允许保留。 */
} AtriIRKind;
typedef struct {
    AtriIRKind kind;
    AtriIRType type;
    AtriSpan span;
    AtriOp op;
    uint64_t immediate; /* LOCAL: 0 为栈对象，非零为 globals 索引 + 1；FIELD 为 fields 索引。 */
    AtriId left, right, callee; /* 私有 LAZY_SELECT 的 callee 保存 false 分支值 ID。 */
    size_t first_argument, argument_count;
    AtriId target, alternative; /* 跳转的块 ID，与 left/right 使用的指令 ID 不同。 */
} AtriIRInst;
/* := 求值义务；result 是后续引用的占位，初始化区间 [first_instruction,result) 求值后删除。 */
typedef struct {
    AtriSpan span;
    AtriId expression, result;
    size_t first_instruction;
    uint64_t bits;
    unsigned char state;
} AtriIRConstant;

typedef struct {
    AtriSpan name;
    size_t parameter_count;
    AtriIRType *parameter_types;
    AtriIRType return_type;
    AtriIRInst *instructions;
    size_t instruction_count, instruction_capacity;
    AtriId *arguments;
    size_t argument_count, argument_capacity;
    AtriIRConstant *constants;
    size_t constant_count, constant_capacity;
    size_t block_count; /* 入口块 0 隐含存在，其余块由 LABEL.immediate 标识，物理顺序可以不同。 */
    AtriId module;
    bool variadic;
    bool external_c; /* 只有完整签名，不拥有函数体；链接符号使用源码名称。 */
} AtriIRFunction;
typedef struct {
    unsigned char *bytes;
    size_t length; /* 不含额外的终止零字节；数据由 IRProgram 拥有，可包含内嵌零。 */
} AtriIRString;
typedef struct { AtriSpan name; AtriIRType type; uint64_t bits; bool constant; } AtriIRGlobal;

typedef struct { AtriSpan name; AtriIRType type; size_t offset; uint64_t default_bits; } AtriIRField;
typedef struct { AtriSpan name; size_t first_field, field_count, size, alignment; unsigned state; bool opaque; } AtriIRStruct;
typedef struct { AtriIRType element; size_t length; } AtriIRArray;
typedef struct {
    AtriIRArray *arrays;
    size_t array_count, array_capacity;
    AtriIRStruct *structs;
    size_t struct_count;
    AtriIRField *fields;
    size_t field_count;
    AtriIRGlobal *globals;
    size_t global_count;
    AtriIRFunction *functions;
    size_t function_count;
    AtriIRString *strings;
    size_t string_count, string_capacity;
} AtriIRProgram;

bool atri_lower(AtriSource source, const AtriProgram *ast,
                AtriIRProgram *out, AtriDiagnostic *diagnostic);
bool atri_ir_verify(const AtriIRProgram *program, AtriDiagnostic *diagnostic);
bool atri_optimize(AtriIRProgram *program, bool inline_functions, AtriDiagnostic *diagnostic);
bool atri_ir_append(AtriIRFunction *f, AtriIRInst instruction, AtriId *result);
/* lowering 内部步骤：此时仍保留用于检查不可达源码的指令，不能提前跳过常量义务。 */
bool atri_resolve_constants(AtriIRProgram *program, AtriDiagnostic *diagnostic);
void atri_ir_destroy(AtriIRProgram *program);
void atri_dump_ir(FILE *stream, AtriSource source, const AtriIRProgram *program);
bool atri_emit_x64(FILE *stream, AtriSource source, const AtriIRProgram *program,
                   AtriDiagnostic *diagnostic);
int64_t atri_i32_signed(uint32_t bits);
int64_t atri_i64_signed(uint64_t bits);
bool atri_ir_is_struct(AtriIRType type);
bool atri_ir_is_array(AtriIRType type);
bool atri_ir_is_aggregate(AtriIRType type);
AtriIRType atri_ir_pointee(AtriIRType type);
AtriIRType atri_ir_pointer_type(AtriIRType type);
size_t atri_ir_type_size(const AtriIRProgram *program, AtriIRType type);
size_t atri_ir_type_align(const AtriIRProgram *program, AtriIRType type);
bool atri_ir_valid_type(const AtriIRProgram *program, AtriIRType type);
bool atri_type_from_source(AtriSource source, const AtriProgram *ast, AtriIRProgram *program, AtriSpan span, AtriIRType *type, AtriDiagnostic *diagnostic);
bool atri_layout_structs(AtriSource source, const AtriProgram *ast, AtriIRProgram *program, AtriDiagnostic *diagnostic);
bool atri_ir_is_integer(AtriIRType type);
bool atri_ir_is_pointer(AtriIRType type);
bool atri_ir_has_value(AtriIRKind kind);
bool atri_compare(AtriIRType type, AtriOp op, uint64_t a, uint64_t b);
const char *atri_ir_type_name(AtriIRType type);
uint64_t atri_integer_convert(AtriIRType source, AtriIRType target, uint64_t bits);
bool atri_integer_compute(AtriIRType type, AtriOp op, uint64_t a, uint64_t b, uint64_t *result);
/* 除零及有符号最小值 / 或 % -1 返回 false；其余结果按目标位模式保存。 */
bool atri_i32_compute(AtriOp op, uint32_t a, uint32_t b, uint32_t *result);
#endif
