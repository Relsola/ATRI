#include "ir.h"
#include "array.h"
#include <inttypes.h>

int64_t atri_i32_signed(uint32_t bits) {
    return bits <= INT32_MAX ? (int64_t)bits : (int64_t)bits - INT64_C(4294967296);
}
bool atri_i32_compute(AtriOp op, uint32_t a, uint32_t b, uint32_t *result) {
    switch (op) {
        case ATRI_ADD: *result = a + b; return true;
        case ATRI_SUB: *result = a - b; return true;
        case ATRI_MUL: *result = (uint32_t)((uint64_t)a * b); return true;
        case ATRI_NEG: *result = UINT32_C(0) - a; return true;
        case ATRI_DIV: case ATRI_REM: {
            if (b == 0 || (a == UINT32_C(2147483648) && b == UINT32_MAX)) return false;
            int64_t x = atri_i32_signed(a), y = atri_i32_signed(b);
            *result = (uint32_t)(op == ATRI_DIV ? x / y : x % y);
            return true;
        }
    }
    return false;
}
bool atri_ir_append(AtriIRFunction *f, AtriIRInst i, AtriId *result) {
    *result = f->instruction_count;
    return atri_append((void **)&f->instructions, &f->instruction_count,
                       &f->instruction_capacity, sizeof(i), &i);
}
void atri_ir_destroy(AtriIRProgram *p) {
    for (size_t i = 0; i < p->function_count; ++i) {
        free(p->functions[i].instructions); free(p->functions[i].arguments);
    }
    free(p->functions); *p = (AtriIRProgram){0};
}
static bool bad(AtriDiagnostic *d, AtriSpan span, const char *message) {
    *d = (AtriDiagnostic){span, message}; return false;
}
static bool operand(const AtriIRFunction *f, size_t use, AtriId id) {
    return id < use && f->instructions[id].kind != ATRI_IR_NOP &&
           f->instructions[id].kind != ATRI_IR_RETURN &&
           f->instructions[id].type == ATRI_IR_I32;
}
bool atri_ir_verify(const AtriIRProgram *p, AtriDiagnostic *d) {
    *d = (AtriDiagnostic){0};
    for (size_t n = 0; n < p->function_count; ++n) {
        const AtriIRFunction *f = &p->functions[n];
        if (f->instruction_count == 0 ||
            f->instructions[f->instruction_count - 1].kind != ATRI_IR_RETURN)
            return bad(d, f->name, "IR verifier: entry block must end with return");
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            if (v->type != ATRI_IR_I32) return bad(d, v->span, "IR verifier: invalid type");
            switch (v->kind) {
                case ATRI_IR_NOP: case ATRI_IR_CONST: break;
                case ATRI_IR_PARAM:
                    if (v->immediate >= f->parameter_count)
                        return bad(d, v->span, "IR verifier: invalid parameter index");
                    break;
                case ATRI_IR_UNARY: case ATRI_IR_RETURN:
                    if (!operand(f, i, v->left)) return bad(d, v->span, "IR verifier: invalid operand");
                    if (v->kind == ATRI_IR_UNARY && v->op != ATRI_NEG)
                        return bad(d, v->span, "IR verifier: invalid unary operator");
                    if (v->kind == ATRI_IR_RETURN && i + 1 != f->instruction_count)
                        return bad(d, v->span, "IR verifier: return must terminate the block");
                    break;
                case ATRI_IR_BINARY:
                    if (!operand(f, i, v->left) || !operand(f, i, v->right) || v->op < ATRI_ADD || v->op > ATRI_REM)
                        return bad(d, v->span, "IR verifier: invalid binary operands");
                    break;
                case ATRI_IR_CALL:
                    if (v->callee >= p->function_count ||
                        v->argument_count != p->functions[v->callee].parameter_count ||
                        v->first_argument > f->argument_count ||
                        v->argument_count > f->argument_count - v->first_argument)
                        return bad(d, v->span, "IR verifier: invalid call signature");
                    for (size_t a = 0; a < v->argument_count; ++a)
                        if (!operand(f, i, f->arguments[v->first_argument + a]))
                            return bad(d, v->span, "IR verifier: invalid call argument");
                    break;
                default: return bad(d, v->span, "IR verifier: invalid instruction kind");
            }
        }
    }
    return true;
}
void atri_dump_ir(FILE *s, AtriSource source, const AtriIRProgram *p) {
    const char *ops[] = {"add", "sub", "mul", "sdiv", "srem", "neg"};
    for (size_t n = 0; n < p->function_count; ++n) {
        const AtriIRFunction *f = &p->functions[n];
        fprintf(s, "func @%.*s(", (int)(f->name.end - f->name.start), source.text + f->name.start);
        for (size_t a = 0; a < f->parameter_count; ++a) fprintf(s, "%si32", a == 0 ? "" : ", ");
        fputs(") -> i32 {\nentry:\n", s);
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            if (v->kind == ATRI_IR_NOP) continue;
            if (v->kind == ATRI_IR_RETURN) { fprintf(s, "    ret i32 %%%zu\n", v->left); continue; }
            fprintf(s, "    %%%zu = ", i);
            switch (v->kind) {
                case ATRI_IR_PARAM: fprintf(s, "param i32 %" PRIu32, v->immediate); break;
                case ATRI_IR_CONST: fprintf(s, "const i32 %" PRId64, atri_i32_signed(v->immediate)); break;
                case ATRI_IR_UNARY: fprintf(s, "neg i32 %%%zu", v->left); break;
                case ATRI_IR_BINARY: fprintf(s, "%s i32 %%%zu, %%%zu", ops[v->op], v->left, v->right); break;
                case ATRI_IR_CALL: {
                    AtriSpan name = p->functions[v->callee].name;
                    fprintf(s, "call i32 @%.*s(", (int)(name.end - name.start), source.text + name.start);
                    for (size_t a = 0; a < v->argument_count; ++a)
                        fprintf(s, "%s%%%zu", a == 0 ? "" : ", ", f->arguments[v->first_argument + a]);
                    fputc(')', s); break;
                }
                default: break;
            }
            fputc('\n', s);
        }
        fputs("}\n", s);
    }
}
