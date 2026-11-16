#include "ir.h"
#include "array.h"

static bool diagnostic(AtriDiagnostic *d, AtriSpan span, const char *message) {
    *d = (AtriDiagnostic){span, message}; return false;
}
static bool emit_fold(AtriIRFunction *out, AtriIRInst inst, AtriId *id, AtriDiagnostic *d) {
    if (inst.kind == ATRI_IR_UNARY || inst.kind == ATRI_IR_BINARY) {
        const AtriIRInst *a = &out->instructions[inst.left];
        const AtriIRInst *b = inst.kind == ATRI_IR_UNARY ? a : &out->instructions[inst.right];
        if (inst.kind == ATRI_IR_BINARY && (inst.op == ATRI_DIV || inst.op == ATRI_REM) &&
            b->kind == ATRI_IR_CONST && b->immediate == 0)
            return diagnostic(d, inst.span, "integer division or remainder by zero");
        if (a->kind == ATRI_IR_CONST && b->kind == ATRI_IR_CONST) {
            uint32_t value;
            if (!atri_i32_compute(inst.op, a->immediate, b->immediate, &value))
                return diagnostic(d, inst.span, "invalid i32 division or remainder (INT_MIN / -1)");
            inst.kind = ATRI_IR_CONST; inst.immediate = value;
        }
    }
    return atri_ir_append(out, inst, id) || diagnostic(d, inst.span, "out of memory during optimization");
}
static bool is_leaf(const AtriIRFunction *f) {
    size_t active = 0;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        if (f->instructions[i].kind == ATRI_IR_CALL) return false;
        if (f->instructions[i].kind != ATRI_IR_NOP) ++active;
    }
    return active <= 64 && f->instruction_count <= 4096;
}
static bool inline_leaf(AtriIRFunction *out, const AtriIRFunction *callee,
                        const AtriId *arguments, AtriId *value, AtriDiagnostic *d) {
    AtriId *map = malloc(callee->instruction_count * sizeof(*map));
    if (map == NULL) return diagnostic(d, callee->name, "out of memory during inlining");
    bool ok = true;
    for (size_t i = 0; i < callee->instruction_count; ++i) {
        AtriIRInst inst = callee->instructions[i];
        if (inst.kind == ATRI_IR_NOP) continue;
        if (inst.kind == ATRI_IR_PARAM) { map[i] = arguments[inst.immediate]; continue; }
        if (inst.kind == ATRI_IR_RETURN) { *value = map[inst.left]; break; }
        if (inst.kind == ATRI_IR_BINARY) { inst.left = map[inst.left]; inst.right = map[inst.right]; }
        if (inst.kind == ATRI_IR_UNARY) inst.left = map[inst.left];
        if (!emit_fold(out, inst, &map[i], d)) { ok = false; break; }
    }
    free(map); return ok;
}
static bool eliminate_dead(AtriIRFunction *f, AtriDiagnostic *d) {
    bool *live = calloc(f->instruction_count, sizeof(*live));
    if (live == NULL) return diagnostic(d, f->name, "out of memory during dead code elimination");
    for (size_t i = f->instruction_count; i-- > 0;) {
        AtriIRInst *v = &f->instructions[i];
        if (v->kind == ATRI_IR_RETURN || v->kind == ATRI_IR_CALL) live[i] = true;
        if (!live[i]) { v->kind = ATRI_IR_NOP; continue; }
        if (v->kind == ATRI_IR_UNARY || v->kind == ATRI_IR_BINARY || v->kind == ATRI_IR_RETURN) live[v->left] = true;
        if (v->kind == ATRI_IR_BINARY) live[v->right] = true;
        if (v->kind == ATRI_IR_CALL)
            for (size_t a = 0; a < v->argument_count; ++a) live[f->arguments[v->first_argument + a]] = true;
    }
    free(live); return true;
}
bool atri_optimize(AtriIRProgram *p, bool inline_functions, AtriDiagnostic *d) {
    if (!atri_ir_verify(p, d)) return false;
    /* Repeated bottom-up leaf inlining handles declaration order and preserves cycles. */
    size_t passes = inline_functions ? p->function_count + 1 : 1;
    if (passes > 64) passes = 64;
    for (size_t pass = 0; pass < passes; ++pass) {
        bool changed = false;
        for (size_t n = 0; n < p->function_count; ++n) {
            AtriIRFunction *old = &p->functions[n];
            AtriIRFunction out = {.name = old->name, .parameter_count = old->parameter_count};
            AtriId *map = malloc(old->instruction_count * sizeof(*map));
            if (map == NULL) return diagnostic(d, old->name, "out of memory during optimization");
            bool ok = true;
            for (size_t i = 0; i < old->instruction_count; ++i) {
                AtriIRInst inst = old->instructions[i];
                if (inst.kind == ATRI_IR_NOP) continue;
                if (inst.kind == ATRI_IR_UNARY || inst.kind == ATRI_IR_BINARY || inst.kind == ATRI_IR_RETURN) inst.left = map[inst.left];
                if (inst.kind == ATRI_IR_BINARY) inst.right = map[inst.right];
                if (inst.kind == ATRI_IR_CALL) {
                    AtriId *args = inst.argument_count == 0 ? NULL : malloc(inst.argument_count * sizeof(*args));
                    if (inst.argument_count != 0 && args == NULL) { ok = diagnostic(d, inst.span, "out of memory"); break; }
                    for (size_t a = 0; a < inst.argument_count; ++a) args[a] = map[old->arguments[inst.first_argument + a]];
                    if (inline_functions && inst.callee != n && out.instruction_count < 4096 && is_leaf(&p->functions[inst.callee])) {
                        ok = inline_leaf(&out, &p->functions[inst.callee], args, &map[i], d);
                        changed = true;
                    } else {
                        inst.first_argument = out.argument_count;
                        for (size_t a = 0; a < inst.argument_count; ++a)
                            if (!atri_append((void **)&out.arguments, &out.argument_count,
                                              &out.argument_capacity, sizeof(args[a]), &args[a])) { ok = diagnostic(d, inst.span, "out of memory"); break; }
                        if (ok) ok = emit_fold(&out, inst, &map[i], d);
                    }
                    free(args);
                } else ok = emit_fold(&out, inst, &map[i], d);
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
