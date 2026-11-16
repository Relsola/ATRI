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
static bool wide(AtriIRType type) { return type == ATRI_IR_I64 || type == ATRI_IR_PTR_I32; }
static const char *word(AtriIRType type) { return wide(type) ? "QWORD" : "DWORD"; }
static void load(FILE *s, const AtriIRFunction *f, const size_t *slots,
                  AtriId id, const char *reg32, const char *reg64) {
    const AtriIRInst *v = &f->instructions[id];
    const char *reg = wide(v->type) ? reg64 : reg32;
    if (v->kind == ATRI_IR_ADDRESS) fprintf(s, "    lea %s, [rsp + %zu]\n", reg64, slots[v->left]);
    else if (v->kind == ATRI_IR_CONST) fprintf(s, "    mov %s, %" PRId64 "\n", reg,
        wide(v->type) ? atri_i64_signed(v->immediate) : atri_i32_signed((uint32_t)v->immediate));
    else fprintf(s, "    mov %s, %s PTR [rsp + %zu]\n", reg,
                 word(v->type), slots[id]);
}
static bool frame_layout(const AtriIRFunction *f, size_t *slots, size_t *frame,
                          AtriDiagnostic *d) {
    size_t outgoing = 0;
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
    size_t needed = outgoing;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        const AtriIRInst *v = &f->instructions[i];
        AtriIRKind k = v->kind;
        if (k == ATRI_IR_LOCAL || (atri_ir_has_value(k) && k != ATRI_IR_CONST && k != ATRI_IR_ADDRESS)) {
            size_t width = k == ATRI_IR_LOCAL && v->type == ATRI_IR_BOOL ? 1 : wide(v->type) ? 8 : 4;
            needed = (needed + width - 1) / width * width;
            slots[i] = needed; needed += width;
            if (needed >= 4096) { *d = (AtriDiagnostic){f->name, "x64 prototype stack frame is too large"}; return false; }
        }
    }
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
        if (is_main(source, f->name) && (f->parameter_count != 0 || f->return_type != ATRI_IR_I32)) {
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
    const char *wide_registers[] = {"rcx", "rdx", "r8", "r9"};
    const char *byte_registers[] = {"cl", "dl", "r8b", "r9b"};
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
        if (f->block_count > 1) fprintf(s, ".Latri_%zu_0:\n", n);
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            switch (v->kind) {
                case ATRI_IR_NOP: case ATRI_IR_CONST: case ATRI_IR_LOCAL: case ATRI_IR_ADDRESS: continue;
                case ATRI_IR_LABEL: fprintf(s, ".Latri_%zu_%" PRIu64 ":\n", n, v->immediate); continue;
                case ATRI_IR_JUMP: fprintf(s, "    jmp .Latri_%zu_%zu\n", n, v->target); continue;
                case ATRI_IR_BRANCH:
                    load(s, f, slots, v->left, "eax", "rax");
                    fprintf(s, "    test eax, eax\n    jnz .Latri_%zu_%zu\n    jmp .Latri_%zu_%zu\n", n, v->target, n, v->alternative); continue;
                case ATRI_IR_UNREACHABLE: fputs("    ud2\n", s); continue;
                case ATRI_IR_PARAM: {
                    if (v->type == ATRI_IR_BOOL) {
                        if (v->immediate < 4) fprintf(s, "    movzx eax, %s\n", byte_registers[v->immediate]);
                        else fprintf(s, "    movzx eax, BYTE PTR [rsp + %zu]\n", frame + 40 + ((size_t)v->immediate - 4)*8);
                        fprintf(s, "    mov DWORD PTR [rsp + %zu], eax\n", slots[i]); continue;
                    }
                    const char *word = v->type == ATRI_IR_I32 ? "DWORD" : "QWORD";
                    const char *reg = v->type == ATRI_IR_I32 ? "eax" : "rax";
                    if (v->immediate < 4) fprintf(s, "    mov %s PTR [rsp + %zu], %s\n", word, slots[i],
                        v->type == ATRI_IR_I32 ? registers[v->immediate] : wide_registers[v->immediate]);
                    else fprintf(s, "    mov %s, %s PTR [rsp + %zu]\n    mov %s PTR [rsp + %zu], %s\n",
                                 reg, word, frame + 40 + ((size_t)v->immediate - 4) * 8, word, slots[i], reg);
                    continue;
                }
                case ATRI_IR_LOAD:
                    if (f->instructions[v->left].kind == ATRI_IR_LOCAL) {
                        if (v->type == ATRI_IR_BOOL) fprintf(s, "    movzx eax, BYTE PTR [rsp + %zu]\n", slots[v->left]);
                        else fprintf(s, "    mov %s, %s PTR [rsp + %zu]\n", wide(v->type) ? "rax" : "eax", word(v->type), slots[v->left]);
                    } else {
                        load(s, f, slots, v->left, "r11d", "r11"); fputs("    mov eax, DWORD PTR [r11]\n", s);
                    }
                    break;
                case ATRI_IR_STORE:
                    if (f->instructions[v->left].kind != ATRI_IR_LOCAL) load(s, f, slots, v->left, "r11d", "r11");
                    load(s, f, slots, v->right, "eax", "rax");
                    if (f->instructions[v->left].kind == ATRI_IR_LOCAL)
                        fprintf(s, "    mov %s PTR [rsp + %zu], %s\n", v->type == ATRI_IR_BOOL ? "BYTE" : word(v->type), slots[v->left],
                            v->type == ATRI_IR_BOOL ? "al" : wide(v->type) ? "rax" : "eax");
                    else fputs("    mov DWORD PTR [r11], eax\n", s);
                    continue;
                case ATRI_IR_UNARY:
                    load(s, f, slots, v->left, "eax", "rax");
                    fputs(v->op == ATRI_NOT ? "    xor eax, 1\n" : v->type == ATRI_IR_I64 ? "    neg rax\n" : "    neg eax\n", s); break;
                case ATRI_IR_COMPARE: {
                    load(s, f, slots, v->left, "eax", "rax"); load(s, f, slots, v->right, "r10d", "r10");
                    bool is_wide = wide(f->instructions[v->left].type);
                    fputs(is_wide ? "    cmp rax, r10\n" : "    cmp eax, r10d\n", s);
                    static const char *conditions[] = {"e", "ne", "l", "le", "g", "ge"};
                    fprintf(s, "    set%s al\n    movzx eax, al\n", conditions[v->op - ATRI_EQ]); break;
                }
                case ATRI_IR_BINARY:
                    load(s, f, slots, v->left, "eax", "rax"); load(s, f, slots, v->right, "r10d", "r10");
                    switch (v->op) {
                        case ATRI_ADD: fputs(v->type == ATRI_IR_I64 ? "    add rax, r10\n" : "    add eax, r10d\n", s); break;
                        case ATRI_SUB: fputs(v->type == ATRI_IR_I64 ? "    sub rax, r10\n" : "    sub eax, r10d\n", s); break;
                        case ATRI_MUL: fputs(v->type == ATRI_IR_I64 ? "    imul rax, r10\n" : "    imul eax, r10d\n", s); break;
                        case ATRI_DIV: case ATRI_REM:
                            fputs(v->type == ATRI_IR_I64 ? "    cqo\n    idiv r10\n" : "    cdq\n    idiv r10d\n", s);
                            if (v->op == ATRI_REM) fputs(v->type == ATRI_IR_I64 ? "    mov rax, rdx\n" : "    mov eax, edx\n", s);
                            break;
                        default: break;
                    }
                    break;
                case ATRI_IR_PTR_OFFSET:
                    load(s, f, slots, v->left, "eax", "rax");
                    load(s, f, slots, v->right, "r10d", "r10");
                    if (f->instructions[v->right].type == ATRI_IR_I32) fputs("    movsxd r10, r10d\n", s);
                    fputs("    shl r10, 2\n", s);
                    fputs(v->op == ATRI_ADD ? "    add rax, r10\n" : "    sub rax, r10\n", s);
                    break;
                case ATRI_IR_PTR_DIFF:
                    load(s, f, slots, v->left, "eax", "rax");
                    load(s, f, slots, v->right, "r10d", "r10");
                    fputs("    sub rax, r10\n    sar rax, 2\n", s);
                    break;
                case ATRI_IR_CALL:
                    for (size_t a = 4; a < v->argument_count; ++a) {
                        AtriId argument = f->arguments[v->first_argument + a];
                        load(s, f, slots, argument, "eax", "rax");
                        bool is_wide = wide(f->instructions[argument].type);
                        fprintf(s, "    mov %s PTR [rsp + %zu], %s\n", is_wide ? "QWORD" : "DWORD",
                                32 + (a - 4) * 8, is_wide ? "rax" : "eax");
                    }
                    for (size_t a = 0; a < v->argument_count && a < 4; ++a)
                        load(s, f, slots, f->arguments[v->first_argument + a], registers[a], wide_registers[a]);
                    fputs("    call ", s); symbol(s, source, p, v->callee); fputc('\n', s); break;
                case ATRI_IR_RETURN:
                    load(s, f, slots, v->left, "eax", "rax");
                    if (frame != 0) fprintf(s, "    add rsp, %zu\n", frame);
                    fputs("    ret\n", s); continue;
                case ATRI_IR_LAZY_LOGICAL: free(slots); return false; /* verifier rejects private IR */
            }
            fprintf(s, "    mov %s PTR [rsp + %zu], %s\n", word(v->type), slots[i], wide(v->type) ? "rax" : "eax");
        }
        fputs("    .seh_endproc\n", s); free(slots);
    }
    return true;
}
