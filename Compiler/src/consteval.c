#include "ir.h"
#include <stdlib.h>

typedef struct {
    AtriIRProgram *program;
    AtriDiagnostic *diagnostic;
    size_t remaining_steps, depth;
} Evaluation;
typedef struct {
    Evaluation *evaluation;
    AtriIRFunction *function;
    const uint64_t *parameters;
    /* values 缓存本次指令执行的结果；locals 保存可被 store 修改的对象内容，两者不能共用。 */
    uint64_t *values;
    uint64_t *locals;
    unsigned char *states; /* 0 未求值，1 正在求值，2 已缓存；状态 1 再次被依赖表示循环。 */
} Frame;

static bool error(Evaluation *e, AtriSpan span, const char *message) {
    *e->diagnostic = (AtriDiagnostic){span, message}; return false;
}
static bool step(Evaluation *e, AtriSpan span) {
    if (e->remaining_steps == 0)
        return error(e, span, "compile-time evaluation exceeded its step limit");
    --e->remaining_steps; return true;
}
static bool create_frame(Evaluation *e, AtriIRFunction *f,
                          const uint64_t *parameters, Frame *out) {
    *out = (Frame){.evaluation = e, .function = f, .parameters = parameters};
    out->values = calloc(f->instruction_count, sizeof(*out->values));
    out->states = calloc(f->instruction_count, sizeof(*out->states));
    out->locals = calloc(f->instruction_count, sizeof(*out->locals));
    if (out->values == NULL || out->states == NULL || out->locals == NULL) {
        free(out->values); free(out->states); free(out->locals);
        return error(e, f->name, "out of memory during compile-time evaluation");
    }
    return true;
}
static void destroy_frame(Frame *f) { free(f->values); free(f->states); free(f->locals); }
static bool value(Frame *frame, AtriId id, uint64_t *out);

static bool constant(Evaluation *e, AtriIRFunction *f, AtriIRConstant *c, uint64_t *out) {
    if (c->state == 2) { *out = c->bits; return true; }
    if (c->state == 1) return error(e, c->span, "cyclic compile-time constant dependency");
    c->state = 1;
    Frame frame;
    /* 声明常量独立求值，不继承当前调用的运行期参数；函数调用内部的已知实参仍可正常使用。 */
    if (!create_frame(e, f, NULL, &frame)) return false;
    bool ok = value(&frame, c->expression, &c->bits);
    destroy_frame(&frame);
    if (ok) { c->state = 2; *out = c->bits; }
    return ok;
}

/* 按程序计数器执行实际路径，包括结果未使用的调用；仅递归求最终返回值会漏掉副作用或不终止行为。 */
static bool execute(Evaluation *e, AtriId callee, const uint64_t *arguments, uint64_t *out) {
    AtriIRFunction *f = &e->program->functions[callee];
    Frame frame;
    if (!create_frame(e, f, arguments, &frame)) return false;
    size_t blocks = f->block_count ? f->block_count : 1;
    size_t *labels = calloc(blocks, sizeof(*labels));
    if (!labels) { destroy_frame(&frame); return error(e, f->name, "out of memory during compile-time evaluation"); }
    for (size_t i = 0; i < f->instruction_count; ++i)
        if (f->instructions[i].kind == ATRI_IR_LABEL) labels[f->instructions[i].immediate] = i;
    bool ok = false;
    for (size_t i = 0; i < f->instruction_count;) {
        /* := 初始化是独立义务，不属于每次调用的运行期语句；跳到结果占位后按需取得常量。 */
        for (size_t c = 0; c < f->constant_count; ++c)
            if (f->constants[c].first_instruction == i && i < f->constants[c].result) {
                i = f->constants[c].result; break;
            }
        const AtriIRInst *inst = &f->instructions[i];
        if (!step(e, inst->span)) break;
        if (inst->kind == ATRI_IR_RETURN) {
            if (inst->type == ATRI_IR_VOID) { *out = 0; ok = true; }
            else { ok = value(&frame, inst->left, out); }
            break;
        }
        if (inst->kind == ATRI_IR_JUMP) { i = labels[inst->target]; continue; }
        if (inst->kind == ATRI_IR_BRANCH) {
            uint64_t condition;
            if (!value(&frame, inst->left, &condition)) break;
            i = labels[condition ? inst->target : inst->alternative]; continue;
        }
        if (inst->kind == ATRI_IR_UNREACHABLE) { error(e, inst->span, "compile-time function reached its end without returning"); break; }
        if (inst->kind == ATRI_IR_NOP || inst->kind == ATRI_IR_LABEL) { ++i; continue; }
        if (inst->kind == ATRI_IR_LOCAL && inst->immediate == 0) {
            if (atri_ir_is_pointer(inst->type)) { error(e, inst->span, "compile-time pointer operations are not supported yet"); break; }
            frame.locals[i] = 0; ++i; continue;
        }
        if (inst->kind == ATRI_IR_STORE) {
            uint64_t stored;
            if (f->instructions[inst->left].kind != ATRI_IR_LOCAL || f->instructions[inst->left].immediate != 0) {
                error(e, inst->span, "compile-time pointer operations are not supported yet"); break;
            }
            if (!value(&frame, inst->right, &stored)) break;
            frame.locals[inst->left] = stored; ++i; continue;
        }
        /* 同一指令可被循环再次执行，必须刷新它的结果；已执行的操作数缓存则代表当前迭代的值。 */
        frame.states[i] = 0;
        uint64_t ignored;
        if (!value(&frame, i, &ignored)) break;
        ++i;
    }
    free(labels); destroy_frame(&frame); return ok;
}

static bool value(Frame *frame, AtriId id, uint64_t *out) {
    Evaluation *e = frame->evaluation;
    AtriIRFunction *f = frame->function;
    const AtriIRInst *inst = &f->instructions[id];
    if (atri_ir_is_aggregate(inst->type) || inst->kind == ATRI_IR_FIELD || inst->kind == ATRI_IR_INDEX) { return error(e, inst->span, "compile-time struct and array values are not supported yet"); }
    if ((!atri_ir_is_integer(inst->type) && inst->type != ATRI_IR_BOOL && !(inst->kind == ATRI_IR_CALL && inst->type == ATRI_IR_VOID)) || inst->kind == ATRI_IR_LOCAL ||
        inst->kind == ATRI_IR_STORE || inst->kind == ATRI_IR_PTR_DIFF || inst->kind == ATRI_IR_ADDRESS ||
        (inst->kind == ATRI_IR_LOAD && (f->instructions[inst->left].kind != ATRI_IR_LOCAL || f->instructions[inst->left].immediate != 0)))
        return error(e, inst->span, "compile-time pointer operations are not supported yet");
    if (!step(e, inst->span)) return false;
    if (++e->depth > 256) {
        --e->depth;
        return error(e, inst->span, "compile-time evaluation exceeded its recursion/dependency depth limit");
    }
    bool ok = false;
    if (frame->states[id] == 2) { *out = frame->values[id]; ok = true; goto done; }
    if (frame->states[id] == 1) { error(e, inst->span, "cyclic compile-time value dependency"); goto done; }
    frame->states[id] = 1;
    for (size_t c = 0; c < f->constant_count; ++c)
        if (f->constants[c].result == id) {
            ok = constant(e, f, &f->constants[c], out); goto save;
        }
    switch (inst->kind) {
        case ATRI_IR_C_PROMOTE: ok = value(frame, inst->left, out); break;
        case ATRI_IR_CAST: {
            uint64_t source;
            if (value(frame, inst->left, &source)) {
                *out = atri_integer_convert(f->instructions[inst->left].type, inst->type, source); ok = true;
            }
            break;
        }
        case ATRI_IR_LOAD: *out = frame->locals[inst->left]; ok = true; break;
        case ATRI_IR_LAZY_SELECT: {
            uint64_t condition;
            if (value(frame, inst->left, &condition)) { ok = value(frame, condition ? inst->right : inst->callee, out); }
            break;
        }
        case ATRI_IR_LAZY_LOGICAL: {
            uint64_t a;
            if (!value(frame, inst->left, &a)) break;
            if ((inst->op == ATRI_AND && !a) || (inst->op == ATRI_OR && a)) { *out = a; ok = true; }
            else ok = value(frame, inst->right, out);
            break;
        }
        case ATRI_IR_COMPARE: {
            uint64_t a, b;
            if (value(frame, inst->left, &a) && value(frame, inst->right, &b)) {
                *out = atri_compare(f->instructions[inst->left].type, inst->op, a, b); ok = true;
            }
            break;
        }
        case ATRI_IR_CONST: *out = inst->immediate; ok = true; break;
        case ATRI_IR_PARAM:
            if (frame->parameters == NULL) error(e, inst->span, "compile-time expression depends on a runtime parameter");
            else { *out = frame->parameters[inst->immediate]; ok = true; }
            break;
        case ATRI_IR_UNARY: case ATRI_IR_BINARY: {
            uint64_t a, b = 0;
            if (!value(frame, inst->left, &a)) break;
            if (inst->kind == ATRI_IR_BINARY && !value(frame, inst->right, &b)) break;
            if (inst->op == ATRI_NOT) { *out = !a; ok = true; break; }
            if (!atri_integer_compute(inst->type, inst->op, a, b, out))
                error(e, inst->span, b == 0 ? "compile-time division or remainder by zero" :
                      "invalid compile-time signed integer division or remainder (INT_MIN / -1)");
            else ok = true;
            break;
        }
        case ATRI_IR_CALL: {
            if (e->program->functions[inst->callee].external_c) {
                error(e, inst->span, "extern(C) calls cannot execute at compile time"); break;
            }
            uint64_t *args = inst->argument_count == 0 ? NULL : malloc(inst->argument_count * sizeof(*args));
            if (inst->argument_count != 0 && args == NULL) { error(e, inst->span, "out of memory during compile-time call"); break; }
            ok = true;
            for (size_t a = 0; a < inst->argument_count; ++a)
                if (!value(frame, f->arguments[inst->first_argument + a], &args[a])) { ok = false; break; }
            if (ok) ok = execute(e, inst->callee, args, out);
            free(args); break;
        }
        default: error(e, inst->span, "invalid instruction in compile-time expression"); break;
    }
save:
    if (ok) { frame->values[id] = *out; frame->states[id] = 2; }
done:
    --e->depth; return ok;
}

bool atri_resolve_constants(AtriIRProgram *p, AtriDiagnostic *d) {
    /* 预算由整次求值共享，避免嵌套调用每次重新获得额度而绕过限制。 */
    Evaluation e = {.program = p, .diagnostic = d, .remaining_steps = 100000};
    /* 先求完所有声明，再改写 IR；包括 return 后的声明，也避免依赖求值读到已被删除的初始化。 */
    for (size_t n = 0; n < p->function_count; ++n) {
        AtriIRFunction *f = &p->functions[n];
        for (size_t c = 0; c < f->constant_count; ++c) {
            uint64_t result;
            if (!constant(&e, f, &f->constants[c], &result)) return false;
        }
    }
    for (size_t n = 0; n < p->function_count; ++n) {
        AtriIRFunction *f = &p->functions[n];
        for (size_t c = 0; c < f->constant_count; ++c) {
            const AtriIRConstant *binding = &f->constants[c];
            for (size_t i = binding->first_instruction; i < binding->result; ++i)
                f->instructions[i].kind = ATRI_IR_NOP;
            f->instructions[binding->result] = (AtriIRInst){.kind = ATRI_IR_CONST,
                .span = binding->span, .type = f->instructions[binding->result].type, .immediate = binding->bits};
        }
        free(f->constants); f->constants = NULL;
        f->constant_count = 0; f->constant_capacity = 0;
    }
    return true;
}
