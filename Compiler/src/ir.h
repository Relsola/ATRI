#ifndef ATRI_IR_H
#define ATRI_IR_H
#include "frontend.h"
#include <stdint.h>

/* Typed values, explicit local objects and basic-block control flow. */
typedef enum { ATRI_IR_I32, ATRI_IR_I64, ATRI_IR_PTR_I32, ATRI_IR_BOOL, ATRI_IR_INVALID } AtriIRType;
typedef enum {
    ATRI_IR_NOP, ATRI_IR_PARAM, ATRI_IR_CONST, ATRI_IR_UNARY,
    ATRI_IR_BINARY, ATRI_IR_CALL, ATRI_IR_RETURN,
    ATRI_IR_LOCAL, ATRI_IR_LOAD, ATRI_IR_STORE,
    ATRI_IR_PTR_OFFSET, ATRI_IR_PTR_DIFF, ATRI_IR_ADDRESS, ATRI_IR_COMPARE,
    ATRI_IR_LABEL, ATRI_IR_JUMP, ATRI_IR_BRANCH, ATRI_IR_UNREACHABLE,
    ATRI_IR_LAZY_LOGICAL /* private constant-initializer operation */
} AtriIRKind;
typedef struct {
    AtriIRKind kind;
    AtriIRType type;
    AtriSpan span;
    AtriOp op;
    uint64_t immediate;
    AtriId left, right, callee;
    size_t first_argument, argument_count;
    AtriId target, alternative;
} AtriIRInst;
/* Temporary constant obligations, resolved before executable IR is published. */
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
    size_t block_count;
} AtriIRFunction;
typedef struct {
    AtriIRFunction *functions;
    size_t function_count;
} AtriIRProgram;

bool atri_lower(AtriSource source, const AtriProgram *ast,
                AtriIRProgram *out, AtriDiagnostic *diagnostic);
bool atri_ir_verify(const AtriIRProgram *program, AtriDiagnostic *diagnostic);
bool atri_optimize(AtriIRProgram *program, bool inline_functions, AtriDiagnostic *diagnostic);
bool atri_ir_append(AtriIRFunction *f, AtriIRInst instruction, AtriId *result);
/* Internal lowering step; input may contain unreachable analysis instructions. */
bool atri_resolve_constants(AtriIRProgram *program, AtriDiagnostic *diagnostic);
void atri_ir_destroy(AtriIRProgram *program);
void atri_dump_ir(FILE *stream, AtriSource source, const AtriIRProgram *program);
bool atri_emit_x64(FILE *stream, AtriSource source, const AtriIRProgram *program,
                   AtriDiagnostic *diagnostic);
int64_t atri_i32_signed(uint32_t bits);
int64_t atri_i64_signed(uint64_t bits);
bool atri_ir_is_integer(AtriIRType type);
bool atri_ir_has_value(AtriIRKind kind);
bool atri_compare(AtriIRType type, AtriOp op, uint64_t a, uint64_t b);
const char *atri_ir_type_name(AtriIRType type);
bool atri_integer_compute(AtriIRType type, AtriOp op, uint64_t a, uint64_t b, uint64_t *result);
/* false for division by zero and signed-minimum / or % -1. */
bool atri_i32_compute(AtriOp op, uint32_t a, uint32_t b, uint32_t *result);
#endif
