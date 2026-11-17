#include "ir.h"
#include "array.h"

static bool diagnostic(AtriDiagnostic *d, AtriSpan span, const char *message) {
    *d = (AtriDiagnostic){span, message}; return false;
}
static bool emit_fold(const AtriIRProgram *p, AtriIRFunction *out, AtriIRInst inst, AtriId *id, AtriDiagnostic *d) {
    if (inst.kind == ATRI_IR_C_PROMOTE && out->instructions[inst.left].kind == ATRI_IR_CONST) {
        inst.immediate = out->instructions[inst.left].type == ATRI_IR_BOOL ? out->instructions[inst.left].immediate :
            atri_integer_convert(out->instructions[inst.left].type, ATRI_IR_I32, out->instructions[inst.left].immediate);
        inst.kind = ATRI_IR_CONST;
    }
    if (inst.kind == ATRI_IR_CAST && atri_ir_is_integer(inst.type) && out->instructions[inst.left].kind == ATRI_IR_CONST) {
        inst.immediate = atri_integer_convert(out->instructions[inst.left].type, inst.type, out->instructions[inst.left].immediate);
        inst.kind = ATRI_IR_CONST;
    }
    if (inst.kind == ATRI_IR_COMPARE && out->instructions[inst.left].kind == ATRI_IR_CONST &&
        out->instructions[inst.right].kind == ATRI_IR_CONST) {
        inst.immediate = atri_compare(out->instructions[inst.left].type, inst.op,
            out->instructions[inst.left].immediate, out->instructions[inst.right].immediate);
        inst.kind = ATRI_IR_CONST;
    }
    if (inst.kind == ATRI_IR_PTR_OFFSET) {
        const AtriIRInst *offset = &out->instructions[inst.right];
        if (offset->kind == ATRI_IR_CONST) {
            int64_t count = atri_integer_signed(offset->type, offset->immediate);
            int64_t stride = (int64_t)atri_ir_type_size(p, atri_ir_pointee(inst.type));
            if (count < INT64_MIN/stride || count > INT64_MAX/stride) {
                return diagnostic(d, inst.span, "pointer byte offset exceeds the signed 64-bit range");
            }
        }
    }
    if (inst.kind == ATRI_IR_UNARY || inst.kind == ATRI_IR_BINARY) {
        const AtriIRInst *a = &out->instructions[inst.left];
        const AtriIRInst *b = inst.kind == ATRI_IR_UNARY ? a : &out->instructions[inst.right];
        if (inst.kind == ATRI_IR_BINARY && (inst.op == ATRI_DIV || inst.op == ATRI_REM) &&
            b->kind == ATRI_IR_CONST && b->immediate == 0)
            return diagnostic(d, inst.span, "integer division or remainder by zero");
        if ((inst.op == ATRI_SHL || inst.op == ATRI_SHR) && b->kind == ATRI_IR_CONST && b->immediate >= atri_integer_width(inst.type)) {
            return diagnostic(d, inst.span, "shift count is outside the operand width");
        }
        if (a->kind == ATRI_IR_CONST && b->kind == ATRI_IR_CONST) {
            uint64_t value;
            if (inst.op == ATRI_NOT) value = !a->immediate;
            else if (!atri_integer_compute(inst.type, inst.op, a->immediate, b->immediate, &value))
                return diagnostic(d, inst.span, "invalid signed integer division or remainder (INT_MIN / -1)");
            inst.kind = ATRI_IR_CONST; inst.immediate = value;
        }
    }
    return atri_ir_append(out, inst, id) || diagnostic(d, inst.span, "out of memory during optimization");
}
static bool is_leaf(const AtriIRFunction *f) {
    if (f->external_c || f->return_type == ATRI_IR_VOID || f->block_count > 1) {
        return false;
    }
    if (f->return_type >= ATRI_IR_STRUCT) { return false; }
    size_t active = 0;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        AtriIRKind kind = f->instructions[i].kind;
        if (f->instructions[i].type >= ATRI_IR_STRUCT || kind == ATRI_IR_FIELD || kind == ATRI_IR_INDEX) { return false; }
        if (kind == ATRI_IR_CALL || kind == ATRI_IR_LOCAL || kind == ATRI_IR_LOAD || kind == ATRI_IR_STORE) return false;
        if (f->instructions[i].kind != ATRI_IR_NOP) ++active;
    }
    return active <= 64 && f->instruction_count <= 4096;
}
static bool inline_leaf(const AtriIRProgram *p, AtriIRFunction *out, const AtriIRFunction *callee,
                        const AtriId *arguments, AtriId *value, AtriDiagnostic *d) {
    /* 克隆时重新分配指令 ID；map 把被调函数的参数/旧值映射到调用者的实参/新值。 */
    AtriId *map = malloc(callee->instruction_count * sizeof(*map));
    if (map == NULL) return diagnostic(d, callee->name, "out of memory during inlining");
    bool ok = true;
    for (size_t i = 0; i < callee->instruction_count; ++i) {
        AtriIRInst inst = callee->instructions[i];
        if (inst.kind == ATRI_IR_NOP) continue;
        if (inst.kind == ATRI_IR_PARAM) { map[i] = arguments[inst.immediate]; continue; }
        if (inst.kind == ATRI_IR_RETURN) { *value = map[inst.left]; break; }
        if (inst.kind == ATRI_IR_BINARY || inst.kind == ATRI_IR_COMPARE) { inst.left = map[inst.left]; inst.right = map[inst.right]; }
        if (inst.kind == ATRI_IR_PTR_OFFSET || inst.kind == ATRI_IR_PTR_DIFF) {
            inst.left = map[inst.left]; inst.right = map[inst.right];
        }
        if (inst.kind == ATRI_IR_UNARY || inst.kind == ATRI_IR_CAST || inst.kind == ATRI_IR_C_PROMOTE) inst.left = map[inst.left];
        if (!emit_fold(p, out, inst, &map[i], d)) { ok = false; break; }
    }
    free(map); return ok;
}
static bool eliminate_dead(AtriIRFunction *f, AtriDiagnostic *d) {
    bool *live = calloc(f->instruction_count, sizeof(*live));
    if (live == NULL) return diagnostic(d, f->name, "out of memory during dead code elimination");
    /* 单块中操作数总在使用前定义，因此从后向前传播依赖一次即可。
     * call 即使结果无用也保留：尚无纯函数/终止性证明，删除它可能丢失写入或改变不终止行为。 */
    for (size_t i = f->instruction_count; i-- > 0;) {
        AtriIRInst *v = &f->instructions[i];
        if (v->kind == ATRI_IR_RETURN || v->kind == ATRI_IR_CALL || v->kind == ATRI_IR_STORE) live[i] = true;
        if (!live[i]) { v->kind = ATRI_IR_NOP; continue; }
        if (v->kind == ATRI_IR_C_PROMOTE || v->kind == ATRI_IR_CAST || v->kind == ATRI_IR_UNARY || v->kind == ATRI_IR_BINARY || (v->kind == ATRI_IR_RETURN && v->type != ATRI_IR_VOID) ||
            v->kind == ATRI_IR_LOAD || v->kind == ATRI_IR_STORE || v->kind == ATRI_IR_ADDRESS || v->kind == ATRI_IR_COMPARE ||
            v->kind == ATRI_IR_PTR_OFFSET || v->kind == ATRI_IR_PTR_DIFF) live[v->left] = true;
        if (v->kind == ATRI_IR_BINARY || v->kind == ATRI_IR_STORE || v->kind == ATRI_IR_COMPARE ||
            v->kind == ATRI_IR_PTR_OFFSET || v->kind == ATRI_IR_PTR_DIFF) live[v->right] = true;
        if (v->kind == ATRI_IR_CALL)
            for (size_t a = 0; a < v->argument_count; ++a) live[f->arguments[v->first_argument + a]] = true;
    }
    free(live); return true;
}
bool atri_optimize(AtriIRProgram *p, bool inline_functions, AtriDiagnostic *d) {
    if (!atri_ir_verify(p, d)) return false;
    /* 某轮内联可能让调用者成为新的叶函数，继续迭代即可处理定义顺序任意的调用链；递归环保留。 */
    size_t passes = inline_functions ? p->function_count + 1 : 1;
    if (passes > 64) passes = 64;
    for (size_t pass = 0; pass < passes; ++pass) {
        bool changed = false;
        for (size_t n = 0; n < p->function_count; ++n) {
            AtriIRFunction *old = &p->functions[n];
            /* 上面的折叠/DCE 只适用于单块，不能跨分支套用：未执行路径中的非法运算也不能提前求值。 */
            bool aggregate = old->return_type >= ATRI_IR_STRUCT;
            for (size_t i = 0; i < old->instruction_count; ++i) {
                if (old->instructions[i].type >= ATRI_IR_STRUCT || old->instructions[i].kind == ATRI_IR_FIELD || old->instructions[i].kind == ATRI_IR_INDEX) { aggregate = true; }
            }
            if (old->external_c || old->block_count > 1 || aggregate) {
                continue;
            }
            AtriIRFunction out = {.name = old->name, .module = old->module, .parameter_count = old->parameter_count,
                .parameter_types = old->parameter_types, .return_type = old->return_type, .block_count = 1};
            /* 压紧 NOP 和插入内联体都会改变 ID，重建每条值引用以及调用实参索引。 */
            AtriId *map = malloc(old->instruction_count * sizeof(*map));
            if (map == NULL) return diagnostic(d, old->name, "out of memory during optimization");
            bool ok = true;
            for (size_t i = 0; i < old->instruction_count; ++i) {
                AtriIRInst inst = old->instructions[i];
                if (inst.kind == ATRI_IR_NOP) continue;
                if (inst.kind == ATRI_IR_C_PROMOTE || inst.kind == ATRI_IR_CAST || inst.kind == ATRI_IR_UNARY || inst.kind == ATRI_IR_BINARY || (inst.kind == ATRI_IR_RETURN && inst.type != ATRI_IR_VOID) ||
                    inst.kind == ATRI_IR_LOAD || inst.kind == ATRI_IR_STORE || inst.kind == ATRI_IR_ADDRESS || inst.kind == ATRI_IR_COMPARE ||
                    inst.kind == ATRI_IR_PTR_OFFSET || inst.kind == ATRI_IR_PTR_DIFF) inst.left = map[inst.left];
                if (inst.kind == ATRI_IR_BINARY || inst.kind == ATRI_IR_STORE || inst.kind == ATRI_IR_COMPARE ||
                    inst.kind == ATRI_IR_PTR_OFFSET || inst.kind == ATRI_IR_PTR_DIFF) inst.right = map[inst.right];
                if (inst.kind == ATRI_IR_CALL) {
                    AtriId *args = inst.argument_count == 0 ? NULL : malloc(inst.argument_count * sizeof(*args));
                    if (inst.argument_count != 0 && args == NULL) { ok = diagnostic(d, inst.span, "out of memory"); break; }
                    for (size_t a = 0; a < inst.argument_count; ++a) args[a] = map[old->arguments[inst.first_argument + a]];
                    if (inline_functions && inst.callee != n && out.instruction_count < 4096 && is_leaf(&p->functions[inst.callee])) {
                        ok = inline_leaf(p, &out, &p->functions[inst.callee], args, &map[i], d);
                        changed = true;
                    } else {
                        inst.first_argument = out.argument_count;
                        for (size_t a = 0; a < inst.argument_count; ++a)
                            if (!atri_append((void **)&out.arguments, &out.argument_count,
                                              &out.argument_capacity, sizeof(args[a]), &args[a])) { ok = diagnostic(d, inst.span, "out of memory"); break; }
                        if (ok) ok = emit_fold(p, &out, inst, &map[i], d);
                    }
                    free(args);
                } else ok = emit_fold(p, &out, inst, &map[i], d);
                if (!ok) break;
            }
            free(map);
            if (ok) ok = eliminate_dead(&out, d);
            if (!ok) { free(out.instructions); free(out.arguments); return false; }
            free(old->instructions); free(old->arguments); *old = out;
        }
        if (!atri_ir_verify(p, d)) return false;
        if (!changed) break;
    }
    return true;
}
