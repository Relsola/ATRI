#include "ir.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static bool is_main(AtriSource source, AtriSpan name) {
    return name.end - name.start == 4 && memcmp(source.text + name.start, "main", 4) == 0;
}
static void symbol(FILE *s, AtriSource source, const AtriIRProgram *p, AtriId f) {
    if (is_main(source, p->functions[f].name)) fputs("main", s);
    else fprintf(s, "atri_fn_%zu", f);
}
static void load(FILE *s, const AtriIRFunction *f, const size_t *slots,
                  AtriId id, const char *reg) {
    const AtriIRInst *v = &f->instructions[id];
    if (v->kind == ATRI_IR_CONST) fprintf(s, "    mov %s, %" PRId64 "\n", reg, atri_i32_signed(v->immediate));
    else fprintf(s, "    mov %s, DWORD PTR [rsp + %zu]\n", reg, slots[id]);
}
static bool frame_layout(const AtriIRFunction *f, size_t *slots, size_t *frame,
                          AtriDiagnostic *d) {
    size_t outgoing = 0, count = 0;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        const AtriIRInst *v = &f->instructions[i];
        if (v->kind == ATRI_IR_CALL) {
            if (v->argument_count > 256) {
                *d = (AtriDiagnostic){v->span, "x64 prototype supports at most 256 call arguments"}; return false;
            }
            size_t bytes = 32 + (v->argument_count > 4 ? (v->argument_count - 4) * 8 : 0);
            if (bytes > outgoing) outgoing = bytes;
        }
    }
    for (size_t i = 0; i < f->instruction_count; ++i) {
        AtriIRKind k = f->instructions[i].kind;
        if (k != ATRI_IR_CONST && k != ATRI_IR_NOP && k != ATRI_IR_RETURN) {
            if (count >= 1000) { *d = (AtriDiagnostic){f->name, "x64 prototype stack frame is too large"}; return false; }
            slots[i] = outgoing + count++ * 4;
        }
    }
    size_t needed = outgoing + count * 4;
    *frame = needed == 0 ? 0 : ((needed + 8 + 15) / 16) * 16 - 8;
    /* Larger frames require stack probing, which is outside this subset. */
    if (*frame >= 4096) { *d = (AtriDiagnostic){f->name, "x64 prototype requires a stack frame smaller than 4096 bytes"}; return false; }
    return true;
}
bool atri_emit_x64(FILE *s, AtriSource source, const AtriIRProgram *p, AtriDiagnostic *d) {
    if (!atri_ir_verify(p, d)) return false;
    /* Validate target limits before emitting any assembly. */
    for (size_t n = 0; n < p->function_count; ++n) {
        const AtriIRFunction *f = &p->functions[n];
        if (is_main(source, f->name) && f->parameter_count != 0) {
            *d = (AtriDiagnostic){f->name, "this prototype requires main() -> i32 without parameters"}; return false;
        }
        size_t frame;
        size_t *slots = calloc(f->instruction_count, sizeof(*slots));
        if (slots == NULL) { *d = (AtriDiagnostic){f->name, "out of memory"}; return false; }
        bool ok = frame_layout(f, slots, &frame, d); free(slots);
        if (!ok) return false;
    }
    fputs("# ATRI Windows x64; clang integrated assembler, Intel syntax\n.intel_syntax noprefix\n.text\n", s);
    const char *registers[] = {"ecx", "edx", "r8d", "r9d"};
    for (size_t n = 0; n < p->function_count; ++n) {
        const AtriIRFunction *f = &p->functions[n];
        size_t *slots = calloc(f->instruction_count, sizeof(*slots));
        if (slots == NULL) { *d = (AtriDiagnostic){f->name, "out of memory"}; return false; }
        size_t frame;
        if (!frame_layout(f, slots, &frame, d)) { free(slots); return false; }
        fputs("\n.globl ", s); symbol(s, source, p, n);
        fputs("\n.p2align 4\n.seh_proc ", s); symbol(s, source, p, n);
        fputc('\n', s); symbol(s, source, p, n); fputs(":\n", s);
        if (frame != 0) fprintf(s, "    sub rsp, %zu\n    .seh_stackalloc %zu\n", frame, frame);
        fputs("    .seh_endprologue\n", s);
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            switch (v->kind) {
                case ATRI_IR_NOP: case ATRI_IR_CONST: continue;
                case ATRI_IR_PARAM:
                    if (v->immediate < 4) fprintf(s, "    mov DWORD PTR [rsp + %zu], %s\n", slots[i], registers[v->immediate]);
                    else fprintf(s, "    mov eax, DWORD PTR [rsp + %zu]\n    mov DWORD PTR [rsp + %zu], eax\n",
                                 frame + 40 + ((size_t)v->immediate - 4) * 8, slots[i]);
                    continue;
                case ATRI_IR_UNARY:
                    load(s, f, slots, v->left, "eax"); fputs("    neg eax\n", s); break;
                case ATRI_IR_BINARY:
                    load(s, f, slots, v->left, "eax"); load(s, f, slots, v->right, "r10d");
                    switch (v->op) {
                        case ATRI_ADD: fputs("    add eax, r10d\n", s); break;
                        case ATRI_SUB: fputs("    sub eax, r10d\n", s); break;
                        case ATRI_MUL: fputs("    imul eax, r10d\n", s); break;
                        case ATRI_DIV: case ATRI_REM:
                            fputs("    cdq\n    idiv r10d\n", s);
                            if (v->op == ATRI_REM) fputs("    mov eax, edx\n", s);
                            break;
                        default: break;
                    }
                    break;
                case ATRI_IR_CALL:
                    for (size_t a = 4; a < v->argument_count; ++a) {
                        load(s, f, slots, f->arguments[v->first_argument + a], "eax");
                        fprintf(s, "    mov DWORD PTR [rsp + %zu], eax\n", 32 + (a - 4) * 8);
                    }
                    for (size_t a = 0; a < v->argument_count && a < 4; ++a)
                        load(s, f, slots, f->arguments[v->first_argument + a], registers[a]);
                    fputs("    call ", s); symbol(s, source, p, v->callee); fputc('\n', s); break;
                case ATRI_IR_RETURN:
                    load(s, f, slots, v->left, "eax");
                    if (frame != 0) fprintf(s, "    add rsp, %zu\n", frame);
                    fputs("    ret\n", s); continue;
            }
            fprintf(s, "    mov DWORD PTR [rsp + %zu], eax\n", slots[i]);
        }
        fputs("    .seh_endproc\n", s); free(slots);
    }
    return true;
}
