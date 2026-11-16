#ifndef ATRI_IR_H
#define ATRI_IR_H
#include "frontend.h"
#include <stdint.h>

/* First executable IR subset: one entry block, all values explicitly i32. */
typedef enum { ATRI_IR_I32 } AtriIRType;
typedef enum {
    ATRI_IR_NOP, ATRI_IR_PARAM, ATRI_IR_CONST, ATRI_IR_UNARY,
    ATRI_IR_BINARY, ATRI_IR_CALL, ATRI_IR_RETURN
} AtriIRKind;
typedef struct {
    AtriIRKind kind;
    AtriIRType type;
    AtriSpan span;
    AtriOp op;
    uint32_t immediate;
    AtriId left, right, callee;
    size_t first_argument, argument_count;
} AtriIRInst;
typedef struct {
    AtriSpan name;
    size_t parameter_count;
    AtriIRInst *instructions;
    size_t instruction_count, instruction_capacity;
    AtriId *arguments;
    size_t argument_count, argument_capacity;
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
void atri_ir_destroy(AtriIRProgram *program);
void atri_dump_ir(FILE *stream, AtriSource source, const AtriIRProgram *program);
bool atri_emit_x64(FILE *stream, AtriSource source, const AtriIRProgram *program,
                   AtriDiagnostic *diagnostic);
int64_t atri_i32_signed(uint32_t bits);
/* false for division by zero and signed-minimum / or % -1. */
bool atri_i32_compute(AtriOp op, uint32_t a, uint32_t b, uint32_t *result);
#endif
