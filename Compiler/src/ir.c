#include "ir.h"
#include "array.h"
#include <inttypes.h>

int64_t atri_i32_signed(uint32_t bits) {
    return bits <= INT32_MAX ? (int64_t)bits : (int64_t)bits - INT64_C(4294967296);
}
int64_t atri_i64_signed(uint64_t bits) {
    return bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)(UINT64_MAX - bits);
}
bool atri_ir_is_integer(AtriIRType type) { return type == ATRI_IR_I32 || type == ATRI_IR_I64; }
bool atri_ir_has_value(AtriIRKind kind) {
    return kind == ATRI_IR_PARAM || kind == ATRI_IR_CONST || kind == ATRI_IR_UNARY ||
        kind == ATRI_IR_BINARY || kind == ATRI_IR_CALL || kind == ATRI_IR_LOAD ||
        kind == ATRI_IR_PTR_OFFSET || kind == ATRI_IR_PTR_DIFF || kind == ATRI_IR_ADDRESS || kind == ATRI_IR_COMPARE;
}
bool atri_compare(AtriIRType type, AtriOp op, uint64_t a, uint64_t b) {
    if (op == ATRI_EQ) return a == b;
    if (op == ATRI_NE) return a != b;
    int64_t x = type == ATRI_IR_I32 ? atri_i32_signed((uint32_t)a) : atri_i64_signed(a);
    int64_t y = type == ATRI_IR_I32 ? atri_i32_signed((uint32_t)b) : atri_i64_signed(b);
    return op == ATRI_LT ? x < y : op == ATRI_LE ? x <= y : op == ATRI_GT ? x > y : x >= y;
}
const char *atri_ir_type_name(AtriIRType type) {
    return type == ATRI_IR_I64 ? "i64" : type == ATRI_IR_PTR_I32 ? "*i32" : type == ATRI_IR_I32 ? "i32" : type == ATRI_IR_BOOL ? "bool" : "<invalid>";
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
        default: return false;
    }
}
bool atri_integer_compute(AtriIRType type, AtriOp op, uint64_t a, uint64_t b, uint64_t *result) {
    if (type == ATRI_IR_I32) {
        uint32_t bits;
        if (!atri_i32_compute(op, (uint32_t)a, (uint32_t)b, &bits)) return false;
        *result = bits; return true;
    }
    if (type != ATRI_IR_I64) return false;
    switch (op) {
        case ATRI_ADD: *result = a + b; return true;
        case ATRI_SUB: *result = a - b; return true;
        case ATRI_MUL: *result = a * b; return true;
        case ATRI_NEG: *result = UINT64_C(0) - a; return true;
        case ATRI_DIV: case ATRI_REM:
            if (b == 0 || (a == (UINT64_C(1) << 63) && b == UINT64_MAX)) return false;
            *result = (uint64_t)(op == ATRI_DIV ? atri_i64_signed(a) / atri_i64_signed(b) : atri_i64_signed(a) % atri_i64_signed(b));
            return true;
        default: return false;
    }
}
bool atri_ir_append(AtriIRFunction *f, AtriIRInst i, AtriId *result) {
    *result = f->instruction_count;
    return atri_append((void **)&f->instructions, &f->instruction_count,
                       &f->instruction_capacity, sizeof(i), &i);
}
void atri_ir_destroy(AtriIRProgram *p) {
    for (size_t i = 0; i < p->function_count; ++i) {
        free(p->functions[i].instructions); free(p->functions[i].arguments);
        free(p->functions[i].constants);
        free(p->functions[i].parameter_types);
    }
    free(p->functions); *p = (AtriIRProgram){0};
}
void atri_dump_ir(FILE *s, AtriSource source, const AtriIRProgram *p) {
    const char *ops[] = {"add", "sub", "mul", "sdiv", "srem", "neg", "eq", "ne", "lt", "le", "gt", "ge", "and", "or", "not"};
    for (size_t n = 0; n < p->function_count; ++n) {
        const AtriIRFunction *f = &p->functions[n];
        fprintf(s, "func @%.*s(", (int)(f->name.end - f->name.start), source.text + f->name.start);
        for (size_t a = 0; a < f->parameter_count; ++a) fprintf(s, "%s%s", a == 0 ? "" : ", ", atri_ir_type_name(f->parameter_types[a]));
        fprintf(s, ") -> %s {\nentry:\n", atri_ir_type_name(f->return_type));
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            if (v->kind == ATRI_IR_NOP) continue;
            if (v->kind == ATRI_IR_RETURN) { fprintf(s, "    ret %s %%%zu\n", atri_ir_type_name(v->type), v->left); continue; }
            if (v->kind == ATRI_IR_STORE) { fprintf(s, "    store %s %%%zu, %%%zu\n", atri_ir_type_name(v->type), v->right, v->left); continue; }
            if (v->kind == ATRI_IR_LABEL) { fprintf(s, "block%" PRIu64 ":\n", v->immediate); continue; }
            if (v->kind == ATRI_IR_JUMP) { fprintf(s, "    jump block%zu\n", v->target); continue; }
            if (v->kind == ATRI_IR_BRANCH) { fprintf(s, "    branch %%%zu, block%zu, block%zu\n", v->left, v->target, v->alternative); continue; }
            if (v->kind == ATRI_IR_UNREACHABLE) { fputs("    unreachable\n", s); continue; }
            fprintf(s, "    %%%zu = ", i);
            switch (v->kind) {
                case ATRI_IR_PARAM: fprintf(s, "param %s %" PRIu64, atri_ir_type_name(v->type), v->immediate); break;
                case ATRI_IR_CONST:
                    if (v->type == ATRI_IR_PTR_I32) fputs("const *i32 null", s);
                    else fprintf(s, "const %s %" PRId64, atri_ir_type_name(v->type),
                                 v->type == ATRI_IR_I64 ? atri_i64_signed(v->immediate) : atri_i32_signed((uint32_t)v->immediate));
                    break;
                case ATRI_IR_LOCAL: fprintf(s, "local %s", atri_ir_type_name(v->type)); break;
                case ATRI_IR_ADDRESS: fprintf(s, "address *i32 %%%zu", v->left); break;
                case ATRI_IR_LOAD: fprintf(s, "load %s %%%zu", atri_ir_type_name(v->type), v->left); break;
                case ATRI_IR_UNARY: fprintf(s, "%s %s %%%zu", ops[v->op], atri_ir_type_name(v->type), v->left); break;
                case ATRI_IR_COMPARE: fprintf(s, "cmp_%s %s %%%zu, %%%zu", ops[v->op], atri_ir_type_name(f->instructions[v->left].type), v->left, v->right); break;
                case ATRI_IR_BINARY: fprintf(s, "%s %s %%%zu, %%%zu", ops[v->op], atri_ir_type_name(v->type), v->left, v->right); break;
                case ATRI_IR_PTR_OFFSET: fprintf(s, "ptr_%s *i32 %%%zu, %%%zu, stride 4", v->op == ATRI_ADD ? "add" : "sub", v->left, v->right); break;
                case ATRI_IR_PTR_DIFF: fprintf(s, "ptr_diff i64 %%%zu, %%%zu, stride 4", v->left, v->right); break;
                case ATRI_IR_CALL: {
                    AtriSpan name = p->functions[v->callee].name;
                    fprintf(s, "call %s @%.*s(", atri_ir_type_name(v->type), (int)(name.end - name.start), source.text + name.start);
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
