#include "ir.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static bool is_main(AtriSource source, const AtriIRFunction *f) {
    AtriSpan name = f->name;
    return f->module == 0 && name.end - name.start == 4 && memcmp(source.text + name.start, "main", 4) == 0;
}
static void symbol(FILE *s, AtriSource source, const AtriIRProgram *p, AtriId f) {
    if (p->functions[f].external_c) {
        AtriSpan name = p->functions[f].name;
        fwrite(source.text+name.start, 1, name.end-name.start, s);
    } else if (is_main(source, &p->functions[f])) {
        fputs("main", s);
    } else {
        fprintf(s, "atri_fn_%zu", f);
    }
}
static bool wide(AtriIRType type) { return type == ATRI_IR_I64 || type == ATRI_IR_U64 || atri_ir_is_pointer(type); }
static const char *word(AtriIRType type) { return wide(type) ? "QWORD" : "DWORD"; }
static const char *width_word(size_t width) { return width == 1 ? "BYTE" : width == 2 ? "WORD" : width == 4 ? "DWORD" : "QWORD"; }
/* 所有窄整数临时值规范化到 32 位；对象本身仍按目标类型的真实宽度读写。 */
static void normalize(FILE *s, AtriIRType type) {
    unsigned width = atri_integer_width(type);
    if (width == 8 || width == 16) {
        fprintf(s, "    %s eax, %s\n", atri_integer_is_signed(type) ? "movsx" : "movzx", width == 8 ? "al" : "ax");
    }
}
static unsigned stride_shift(size_t stride) {
    unsigned shift = 0;
    while (stride > 1) { stride >>= 1; ++shift; }
    return shift;
}
static bool indirect(const AtriIRProgram *p, AtriIRType type) {
    if (!atri_ir_is_aggregate(type)) { return false; }
    size_t size = atri_ir_type_size(p, type);
    return size != 0 && size != 1 && size != 2 && size != 4 && size != 8;
}
static void load(FILE *s, const AtriIRProgram *p, const AtriIRFunction *f, const size_t *slots, AtriId id, const char *reg32, const char *reg64);
static void object_address(FILE *s, const AtriIRProgram *p, const AtriIRFunction *f, const size_t *slots, AtriId id, const char *reg) {
    const AtriIRInst *v = &f->instructions[id];
    if (v->kind == ATRI_IR_INDEX) {
        fprintf(s, "    mov %s, QWORD PTR [rsp + %zu]\n", reg, slots[id]);
    } else if (v->kind == ATRI_IR_FIELD) {
        object_address(s, p, f, slots, v->left, reg);
        if (p->fields[v->immediate].offset) { fprintf(s, "    add %s, %zu\n", reg, p->fields[v->immediate].offset); }
    } else if (v->kind == ATRI_IR_LOCAL && v->immediate) {
        fprintf(s, "    lea %s, [rip + .Latri_global_%" PRIu64 "]\n", reg, v->immediate-1);
    } else if (v->kind == ATRI_IR_LOCAL || atri_ir_is_aggregate(v->type)) {
        fprintf(s, "    lea %s, [rsp + %zu]\n", reg, slots[id]);
    } else { load(s, p, f, slots, id, "eax", reg); }
}
/* 仅使用易失寄存器 r10；源 r11 与目标 rax 指向值快照和目标对象，复制不会跨调用存活。 */
static void copy_object(FILE *s, size_t size) {
    for (size_t offset = 0; offset < size;) {
        size_t width = size-offset >= 8 ? 8 : size-offset >= 4 ? 4 : size-offset >= 2 ? 2 : 1;
        const char *reg = width == 8 ? "r10" : width == 4 ? "r10d" : width == 2 ? "r10w" : "r10b";
        fprintf(s, "    mov %s, %s PTR [r11 + %zu]\n    mov %s PTR [rax + %zu], %s\n", reg, width_word(width), offset, width_word(width), offset, reg);
        offset += width;
    }
}
static void load(FILE *s, const AtriIRProgram *p, const AtriIRFunction *f, const size_t *slots,
                  AtriId id, const char *reg32, const char *reg64) {
    const AtriIRInst *v = &f->instructions[id];
    const char *reg = wide(v->type) ? reg64 : reg32;
    if (v->kind == ATRI_IR_ADDRESS) { object_address(s, p, f, slots, v->left, reg64); }
    else if (v->kind == ATRI_IR_STRING) { fprintf(s, "    lea %s, [rip + .Latri_string_%" PRIu64 "]\n", reg64, v->immediate); }
    else if (v->kind == ATRI_IR_CONST) {
        fprintf(s, "    mov %s, %" PRId64 "\n", reg, wide(v->type) ? atri_i64_signed(v->immediate) : atri_integer_is_signed(v->type) ? atri_integer_signed(v->type, v->immediate) : (int64_t)v->immediate);
    } else if (atri_ir_is_aggregate(v->type)) {
        size_t size = atri_ir_type_size(p, v->type);
        if (size == 0) { fprintf(s, "    xor %s, %s\n", reg32, reg32); }
        else { fprintf(s, "    %s %s, %s PTR [rsp + %zu]\n", size < 4 ? "movzx" : "mov", size == 8 ? reg64 : reg32, width_word(size), slots[id]); }
    } else { fprintf(s, "    mov %s, %s PTR [rsp + %zu]\n", reg, word(v->type), slots[id]); }
}
static size_t outgoing_size(const AtriIRProgram *p, const AtriIRFunction *f) {
    size_t size = 0;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        const AtriIRInst *v = &f->instructions[i];
        if (v->kind == ATRI_IR_CALL) {
            size_t arguments = v->argument_count + (size_t)indirect(p, v->type);
            size_t bytes = 32 + (arguments > 4 ? (arguments-4)*8 : 0);
            if (bytes > size) { size = bytes; }
        }
    }
    return size;
}
static bool frame_layout(const AtriIRProgram *p, const AtriIRFunction *f, size_t *slots, size_t *frame,
                          AtriDiagnostic *d) {
    size_t outgoing = outgoing_size(p, f), copies = 0;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        const AtriIRInst *v = &f->instructions[i];
        if (v->kind != ATRI_IR_CALL) { continue; }
        if (v->argument_count > 256) { *d = (AtriDiagnostic){v->span, "x64 prototype supports at most 256 call arguments"}; return false; }
        size_t bytes = 0;
        for (size_t a = 0; a < v->argument_count; ++a) {
            AtriIRType type = f->instructions[f->arguments[v->first_argument+a]].type;
            if (indirect(p, type)) { bytes += (atri_ir_type_size(p, type)+15)/16*16; }
        }
        if (bytes > copies) { copies = bytes; }
    }
    size_t needed = copies ? (outgoing+15)/16*16+copies : outgoing;
    if (indirect(p, f->return_type)) {
        needed = (needed+7)/8*8; slots[f->instruction_count] = needed; needed += 8;
    }
    for (size_t i = 0; i < f->instruction_count; ++i) {
        const AtriIRInst *v = &f->instructions[i];
        AtriIRKind k = v->kind;
        if (k == ATRI_IR_INDEX || (k == ATRI_IR_LOCAL && v->immediate == 0) || (atri_ir_has_value(k) && v->type != ATRI_IR_VOID && k != ATRI_IR_CONST && k != ATRI_IR_ADDRESS && k != ATRI_IR_STRING)) {
            /* 标量对象使用真实布局；窄整数/bool 临时值用 4 字节，address 按需 lea。 */
            size_t width = k == ATRI_IR_INDEX ? 8 : atri_ir_is_aggregate(v->type) ? atri_ir_type_size(p, v->type) : k == ATRI_IR_LOCAL ? atri_ir_type_size(p, v->type) : wide(v->type) ? 8 : 4;
            size_t alignment = k == ATRI_IR_INDEX ? 8 : atri_ir_is_aggregate(v->type) ? 16 : width;
            if (!width) { width = 1; }
            needed = (needed + alignment - 1) / alignment * alignment;
            slots[i] = needed; needed += width;
            if (needed >= 4096) { *d = (AtriDiagnostic){f->name, "x64 prototype stack frame is too large"}; return false; }
        }
    }
    /* call 压入 8 字节返回地址，入口 RSP 为 8 mod 16；非空帧取 8 mod 16，使调用前恢复 16 字节对齐。 */
    *frame = needed == 0 ? 0 : ((needed + 8 + 15) / 16) * 16 - 8;
    /* 大帧需要栈探测；当前未生成该代码，因此不能仅增大这里的上限。 */
    if (*frame >= 4096) { *d = (AtriDiagnostic){f->name, "x64 prototype requires a stack frame smaller than 4096 bytes"}; return false; }
    return true;
}
/* reachable 为 NULL 时保留全部定义，供没有 main 的库源码使用。 */
static bool emit_reachable_x64(FILE *s, AtriSource source, const AtriIRProgram *p,
                               const bool *reachable, AtriDiagnostic *d) {
    /* 先检查全部保留函数的目标限制，避免后面的函数失败时留下残缺汇编。 */
    for (size_t n = 0; n < p->function_count; ++n) {
        if (reachable != NULL && !reachable[n]) {
            continue;
        }
        const AtriIRFunction *f = &p->functions[n];
        if (is_main(source, f) && f->return_type != ATRI_IR_I32) {
            *d = (AtriDiagnostic){f->name, "hosted main must return i32 for C startup"}; return false;
        }
        if (f->external_c) {
            for (size_t defined = 0; defined < p->function_count; ++defined) {
                if ((reachable != NULL && !reachable[defined]) || p->functions[defined].external_c ||
                    is_main(source, &p->functions[defined])) {
                    continue;
                }
                char generated[64];
                int length = snprintf(generated, sizeof(generated), "atri_fn_%zu", defined);
                if (f->name.end-f->name.start == (size_t)length && memcmp(source.text+f->name.start, generated, (size_t)length) == 0) {
                    *d = (AtriDiagnostic){f->name, "extern(C) symbol collides with a generated function symbol"}; return false;
                }
            }
            continue;
        }
        if (is_main(source, f) && (f->parameter_count != 0 || f->return_type != ATRI_IR_I32)) {
            *d = (AtriDiagnostic){f->name, "this prototype requires main() -> i32 without parameters"}; return false;
        }
        size_t frame;
        size_t *slots = calloc(f->instruction_count+1, sizeof(*slots));
        if (slots == NULL) { *d = (AtriDiagnostic){f->name, "out of memory"}; return false; }
        bool ok = frame_layout(p, f, slots, &frame, d); free(slots);
        if (!ok) return false;
    }
    fputs("# ATRI Windows x64; clang integrated assembler, Intel syntax\n.intel_syntax noprefix\n.text\n", s);
    if (p->global_count) {
        fputs(".data\n", s);
        for (size_t i = 0; i < p->global_count; ++i) {
            const AtriIRGlobal *g = &p->globals[i];
            if (g->constant) { continue; }
            if (atri_ir_is_aggregate(g->type)) {
                fprintf(s, ".balign %zu\n.Latri_global_%zu:\n", atri_ir_type_align(p, g->type), i);
                /* 零大小对象保留一个地址锚点，不改变语言可见的 sizeof。 */
                if (!g->data_size) { fputs("    .byte 0\n", s); }
                for (size_t b = 0; b < g->data_size; ++b) {
                    if (b % 16 == 0) { fputs("    .byte ", s); }
                    fprintf(s, "%s%u", b % 16 ? ", " : "", (unsigned)g->data[b]);
                    if (b % 16 == 15 || b+1 == g->data_size) { fputc('\n', s); }
                }
                continue;
            }
            unsigned width = (unsigned)atri_ir_type_size(p, g->type);
            fprintf(s, ".balign %u\n.Latri_global_%zu:\n    %s %" PRIu64 "\n", width, i,
                width == 1 ? ".byte" : width == 2 ? ".short" : width == 8 ? ".quad" : ".long", g->bits);
        }
        fputs(".text\n", s);
    }
    if (p->string_count) {
        fputs(".section .rdata,\"dr\"\n", s);
        for (size_t n = 0; n < p->string_count; ++n) {
            fprintf(s, ".Latri_string_%zu:\n", n);
            for (size_t b = 0; b <= p->strings[n].length; ++b) {
                if (b % 16 == 0) {
                    fputs("    .byte ", s);
                }
                fprintf(s, "%s%u", b % 16 ? ", " : "", (unsigned)p->strings[n].bytes[b]);
                if (b % 16 == 15 || b == p->strings[n].length) {
                    fputc('\n', s);
                }
            }
        }
        fputs(".text\n", s);
    }
    const char *registers[] = {"ecx", "edx", "r8d", "r9d"};
    const char *wide_registers[] = {"rcx", "rdx", "r8", "r9"};
    const char *byte_registers[] = {"cl", "dl", "r8b", "r9b"};
    const char *short_registers[] = {"cx", "dx", "r8w", "r9w"};
    for (size_t n = 0; n < p->function_count; ++n) {
        if (reachable != NULL && !reachable[n]) {
            continue;
        }
        const AtriIRFunction *f = &p->functions[n];
        if (f->external_c) {
            fputs(".extern ", s); symbol(s, source, p, n); fputc('\n', s); continue;
        }
        size_t *slots = calloc(f->instruction_count+1, sizeof(*slots));
        if (slots == NULL) { *d = (AtriDiagnostic){f->name, "out of memory"}; return false; }
        size_t frame;
        if (!frame_layout(p, f, slots, &frame, d)) { free(slots); return false; }
        fputs("\n.globl ", s); symbol(s, source, p, n);
        fputs("\n.p2align 4\n.seh_proc ", s); symbol(s, source, p, n);
        fputc('\n', s); symbol(s, source, p, n); fputs(":\n", s);
        if (frame != 0) fprintf(s, "    sub rsp, %zu\n    .seh_stackalloc %zu\n", frame, frame);
        fputs("    .seh_endprologue\n", s);
        if (indirect(p, f->return_type)) { fprintf(s, "    mov QWORD PTR [rsp + %zu], rcx\n", slots[f->instruction_count]); }
        if (f->block_count > 1) fprintf(s, ".Latri_%zu_0:\n", n);
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            switch (v->kind) {
                case ATRI_IR_NOP: case ATRI_IR_CONST: case ATRI_IR_FIELD: case ATRI_IR_LOCAL: case ATRI_IR_ADDRESS: case ATRI_IR_STRING: continue;
                case ATRI_IR_LABEL: fprintf(s, ".Latri_%zu_%" PRIu64 ":\n", n, v->immediate); continue;
                case ATRI_IR_JUMP: fprintf(s, "    jmp .Latri_%zu_%zu\n", n, v->target); continue;
                case ATRI_IR_BRANCH:
                    load(s, p, f, slots, v->left, "eax", "rax");
                    fprintf(s, "    test eax, eax\n    jnz .Latri_%zu_%zu\n    jmp .Latri_%zu_%zu\n", n, v->target, n, v->alternative); continue;
                case ATRI_IR_UNREACHABLE: fputs("    ud2\n", s); continue;
                case ATRI_IR_ZERO: {
                    size_t size = atri_ir_type_size(p, v->type);
                    for (size_t offset = 0; offset < size;) {
                        size_t width = size-offset >= 8 ? 8 : size-offset >= 4 ? 4 : size-offset >= 2 ? 2 : 1;
                        fprintf(s, "    mov %s PTR [rsp + %zu], 0\n", width_word(width), slots[i]+offset); offset += width;
                    }
                    continue;
                }
                case ATRI_IR_PARAM: {
                    /* 大结构体返回占用首个实参位置；第 5 个 ABI 实参在入口 RSP+40。 */
                    size_t position = (size_t)v->immediate + (size_t)indirect(p, f->return_type);
                    size_t incoming = frame+40+(position >= 4 ? position-4 : 0)*8;
                    if (atri_ir_is_aggregate(v->type)) {
                        size_t size = atri_ir_type_size(p, v->type);
                        if (indirect(p, v->type)) {
                            if (position < 4) { fprintf(s, "    mov r11, %s\n", wide_registers[position]); }
                            else { fprintf(s, "    mov r11, QWORD PTR [rsp + %zu]\n", incoming); }
                            fprintf(s, "    lea rax, [rsp + %zu]\n", slots[i]); copy_object(s, size);
                        } else if (size) {
                            if (position < 4) { fprintf(s, "    mov rax, %s\n", wide_registers[position]); }
                            else { fprintf(s, "    mov rax, QWORD PTR [rsp + %zu]\n", incoming); }
                            fprintf(s, "    mov %s PTR [rsp + %zu], %s\n", width_word(size), slots[i], size == 8 ? "rax" : size == 4 ? "eax" : size == 2 ? "ax" : "al");
                        }
                        continue;
                    }
                    size_t width = atri_ir_type_size(p, v->type);
                    if (width < 4) {
                        const char *extend = atri_integer_is_signed(v->type) ? "movsx" : "movzx";
                        if (position < 4) { fprintf(s, "    %s eax, %s\n", extend, width == 1 ? byte_registers[position] : short_registers[position]); }
                        else { fprintf(s, "    %s eax, %s PTR [rsp + %zu]\n", extend, width_word(width), incoming); }
                        fprintf(s, "    mov DWORD PTR [rsp + %zu], eax\n", slots[i]); continue;
                    }
                    const char *size = wide(v->type) ? "QWORD" : "DWORD";
                    const char *reg = wide(v->type) ? "rax" : "eax";
                    if (position < 4) { fprintf(s, "    mov %s PTR [rsp + %zu], %s\n", size, slots[i], wide(v->type) ? wide_registers[position] : registers[position]); }
                    else { fprintf(s, "    mov %s, %s PTR [rsp + %zu]\n    mov %s PTR [rsp + %zu], %s\n", reg, size, incoming, size, slots[i], reg); }
                    continue;
                }
                case ATRI_IR_INDEX: {
                    /* 位置在本指令执行时固定，后续地址使用不会重新计算或覆盖调用实参寄存器。 */
                    object_address(s, p, f, slots, v->left, "r11");
                    load(s, p, f, slots, v->right, "r10d", "r10");
                    if (atri_integer_is_signed(f->instructions[v->right].type) && atri_integer_width(f->instructions[v->right].type) <= 32) { fputs("    movsxd r10, r10d\n", s); }
                    fprintf(s, "    imul r10, %zu\n    add r11, r10\n    mov QWORD PTR [rsp + %zu], r11\n",
                        atri_ir_type_size(p, v->type), slots[i]);
                    continue;
                }
                case ATRI_IR_LOAD:
                    object_address(s, p, f, slots, v->left, "r11");
                    if (atri_ir_is_aggregate(v->type)) {
                        fprintf(s, "    lea rax, [rsp + %zu]\n", slots[i]); copy_object(s, atri_ir_type_size(p, v->type)); continue;
                    }
                    if (atri_ir_type_size(p, v->type) < 4) {
                        fprintf(s, "    %s eax, %s PTR [r11]\n", atri_integer_is_signed(v->type) ? "movsx" : "movzx", width_word(atri_ir_type_size(p, v->type)));
                    } else { fprintf(s, "    mov %s, %s PTR [r11]\n", wide(v->type) ? "rax" : "eax", word(v->type)); }
                    break;
                case ATRI_IR_STORE:
                    if (atri_ir_is_aggregate(v->type)) {
                        object_address(s, p, f, slots, v->right, "r11");
                        object_address(s, p, f, slots, v->left, "rax"); copy_object(s, atri_ir_type_size(p, v->type));
                    } else {
                        load(s, p, f, slots, v->right, "eax", "rax"); object_address(s, p, f, slots, v->left, "r11");
                        size_t width = atri_ir_type_size(p, v->type);
                        fprintf(s, "    mov %s PTR [r11], %s\n", width_word(width), width == 1 ? "al" : width == 2 ? "ax" : wide(v->type) ? "rax" : "eax");
                    }
                    continue;
                case ATRI_IR_C_PROMOTE: case ATRI_IR_CAST:
                    load(s, p, f, slots, v->left, "eax", "rax");
                    if (atri_integer_is_signed(f->instructions[v->left].type) && atri_integer_width(f->instructions[v->left].type) <= 32 && wide(v->type)) { fputs("    movsxd rax, eax\n", s); }
                    break;
                case ATRI_IR_UNARY:
                    load(s, p, f, slots, v->left, "eax", "rax");
                    fputs(v->op == ATRI_NOT ? "    xor eax, 1\n" : v->op == ATRI_BIT_NOT ? (wide(v->type) ? "    not rax\n" : "    not eax\n") : wide(v->type) ? "    neg rax\n" : "    neg eax\n", s); break;
                case ATRI_IR_COMPARE: {
                    load(s, p, f, slots, v->left, "eax", "rax"); load(s, p, f, slots, v->right, "r10d", "r10");
                    bool is_wide = wide(f->instructions[v->left].type);
                    fputs(is_wide ? "    cmp rax, r10\n" : "    cmp eax, r10d\n", s);
                    static const char *conditions[] = {"e", "ne", "l", "le", "g", "ge"};
                    fprintf(s, "    set%s al\n    movzx eax, al\n", !atri_integer_is_signed(f->instructions[v->left].type) ?
                        (const char *[]){"e", "ne", "b", "be", "a", "ae"}[v->op-ATRI_EQ] : conditions[v->op - ATRI_EQ]); break;
                }
                case ATRI_IR_BINARY:
                    load(s, p, f, slots, v->left, "eax", "rax"); load(s, p, f, slots, v->right, "r10d", "r10");
                    switch (v->op) {
                        case ATRI_ADD: fputs(wide(v->type) ? "    add rax, r10\n" : "    add eax, r10d\n", s); break;
                        case ATRI_SUB: fputs(wide(v->type) ? "    sub rax, r10\n" : "    sub eax, r10d\n", s); break;
                        case ATRI_MUL: fputs(wide(v->type) ? "    imul rax, r10\n" : "    imul eax, r10d\n", s); break;
                        case ATRI_BIT_AND: fputs(wide(v->type) ? "    and rax, r10\n" : "    and eax, r10d\n", s); break;
                        case ATRI_BIT_OR: fputs(wide(v->type) ? "    or rax, r10\n" : "    or eax, r10d\n", s); break;
                        case ATRI_BIT_XOR: fputs(wide(v->type) ? "    xor rax, r10\n" : "    xor eax, r10d\n", s); break;
                        case ATRI_SHL: case ATRI_SHR:
                            fputs("    mov ecx, r10d\n", s);
                            fprintf(s, "    %s %s, cl\n", v->op == ATRI_SHL ? "shl" : atri_integer_is_signed(v->type) ? "sar" : "shr", wide(v->type) ? "rax" : "eax"); break;
                        case ATRI_DIV: case ATRI_REM:
                            fputs(!atri_integer_is_signed(v->type) ? (wide(v->type) ? "    xor edx, edx\n    div r10\n" : "    xor edx, edx\n    div r10d\n") : wide(v->type) ? "    cqo\n    idiv r10\n" : "    cdq\n    idiv r10d\n", s);
                            if (v->op == ATRI_REM) fputs(wide(v->type) ? "    mov rax, rdx\n" : "    mov eax, edx\n", s);
                            break;
                        default: break;
                    }
                    break;
                case ATRI_IR_PTR_OFFSET: {
                    load(s, p, f, slots, v->left, "eax", "rax");
                    load(s, p, f, slots, v->right, "r10d", "r10");
                    /* 负偏移先符号扩展再缩放，避免在 32 位中先乘 4 而错误环绕。 */
                    if (f->instructions[v->right].type == ATRI_IR_I32) fputs("    movsxd r10, r10d\n", s);
                    size_t stride = atri_ir_type_size(p, atri_ir_pointee(v->type));
                    if (stride > 1 && !(stride & (stride-1))) { fprintf(s, "    shl r10, %u\n", stride_shift(stride)); }
                    else if (stride > 1) { fprintf(s, "    imul r10, %zu\n", stride); }
                    fputs(v->op == ATRI_ADD ? "    add rax, r10\n" : "    sub rax, r10\n", s);
                    break;
                }
                case ATRI_IR_PTR_DIFF:
                    load(s, p, f, slots, v->left, "eax", "rax");
                    load(s, p, f, slots, v->right, "r10d", "r10");
                    /* 合法字节差为元素大小的整数倍；按目标真实步长计算带符号元素距离。 */
                    fputs("    sub rax, r10\n", s);
                    {
                        size_t stride = atri_ir_type_size(p, atri_ir_pointee(f->instructions[v->left].type));
                        if (stride > 1 && !(stride & (stride-1))) { fprintf(s, "    sar rax, %u\n", stride_shift(stride)); }
                        else if (stride > 1) { fprintf(s, "    cqo\n    mov r10, %zu\n    idiv r10\n", stride); }
                    }
                    break;
                case ATRI_IR_CALL: {
                    size_t hidden = (size_t)indirect(p, v->type), copy_base = (outgoing_size(p, f)+15)/16*16;
                    size_t argument_copies[256];
                    for (size_t a = 0; a < v->argument_count; ++a) {
                        AtriId argument = f->arguments[v->first_argument+a];
                        AtriIRType type = f->instructions[argument].type;
                        argument_copies[a] = copy_base;
                        if (indirect(p, type)) {
                            object_address(s, p, f, slots, argument, "r11"); fprintf(s, "    lea rax, [rsp + %zu]\n", copy_base);
                            copy_object(s, atri_ir_type_size(p, type)); copy_base += (atri_ir_type_size(p, type)+15)/16*16;
                        }
                    }
                    /* 先写栈参数，再装寄存器；副本复制已完成，不会破坏先装入的寄存器实参。 */
                    for (size_t pass = 0; pass < 2; ++pass) {
                        for (size_t a = 0; a < v->argument_count; ++a) {
                            size_t position = a+hidden;
                            if ((position >= 4) != (pass == 0)) { continue; }
                            AtriId argument = f->arguments[v->first_argument+a];
                            AtriIRType type = f->instructions[argument].type;
                            const char *r32 = position < 4 ? registers[position] : "eax";
                            const char *r64 = position < 4 ? wide_registers[position] : "rax";
                            if (indirect(p, type)) { fprintf(s, "    lea %s, [rsp + %zu]\n", r64, argument_copies[a]); }
                            else { load(s, p, f, slots, argument, r32, r64); }
                            if (position >= 4) { fprintf(s, "    mov QWORD PTR [rsp + %zu], rax\n", 32+(position-4)*8); }
                        }
                    }
                    if (hidden) { fprintf(s, "    lea rcx, [rsp + %zu]\n", slots[i]); }
                    fputs("    call ", s); symbol(s, source, p, v->callee); fputc('\n', s);
                    if (v->type == ATRI_IR_VOID) { continue; }
                    if (atri_ir_is_aggregate(v->type)) {
                        size_t size = atri_ir_type_size(p, v->type);
                        if (!hidden && size) { fprintf(s, "    mov %s PTR [rsp + %zu], %s\n", width_word(size), slots[i], size == 8 ? "rax" : size == 4 ? "eax" : size == 2 ? "ax" : "al"); }
                        continue;
                    }
                    if (v->type == ATRI_IR_BOOL) { fputs("    movzx eax, al\n", s); }
                    break;
                }
                case ATRI_IR_RETURN:
                    if (v->type == ATRI_IR_VOID) {
                        if (frame != 0) { fprintf(s, "    add rsp, %zu\n", frame); }
                        fputs("    ret\n", s); continue;
                    }
                    if (indirect(p, v->type)) {
                        object_address(s, p, f, slots, v->left, "r11");
                        fprintf(s, "    mov rax, QWORD PTR [rsp + %zu]\n", slots[f->instruction_count]); copy_object(s, atri_ir_type_size(p, v->type));
                    } else { load(s, p, f, slots, v->left, "eax", "rax"); }
                    if (frame != 0) { fprintf(s, "    add rsp, %zu\n", frame); }
                    fputs("    ret\n", s); continue;
                case ATRI_IR_LAZY_SELECT: case ATRI_IR_LAZY_LOGICAL: free(slots); return false; /* 私有指令应已被前面的 Verifier 拒绝。 */
            }
            normalize(s, v->type);
            fprintf(s, "    mov %s PTR [rsp + %zu], %s\n", word(v->type), slots[i], wide(v->type) ? "rax" : "eax");
        }
        fputs("    .seh_endproc\n", s); free(slots);
    }
    return true;
}

bool atri_emit_x64(FILE *s, AtriSource source, const AtriIRProgram *p, AtriDiagnostic *d) {
    if (!atri_ir_verify(p, d)) {
        return false;
    }
    AtriId entry = SIZE_MAX;
    for (size_t n = 0; n < p->function_count; ++n) {
        if (!p->functions[n].external_c && is_main(source, &p->functions[n])) {
            entry = n;
            break;
        }
    }
    if (entry == SIZE_MAX) {
        return emit_reachable_x64(s, source, p, NULL, d);
    }
    bool *reachable = calloc(p->function_count, sizeof(*reachable));
    AtriId *pending = malloc(p->function_count * sizeof(*pending));
    if (reachable == NULL || pending == NULL) {
        free(reachable);
        free(pending);
        *d = (AtriDiagnostic){p->functions[entry].name, "out of memory during function reachability analysis"};
        return false;
    }
    /* 按优化后剩余的调用边遍历；入队即标记，每个函数只扫描一次，递归环不会反复入队。
     * 只筛选输出，不压紧函数数组，以保持 call.callee 及临时符号编号稳定。 */
    size_t pending_count = 1;
    pending[0] = entry;
    reachable[entry] = true;
    for (size_t next = 0; next < pending_count; ++next) {
        const AtriIRFunction *f = &p->functions[pending[next]];
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *inst = &f->instructions[i];
            if (inst->kind == ATRI_IR_CALL && !reachable[inst->callee]) {
                reachable[inst->callee] = true;
                pending[pending_count++] = inst->callee;
            }
        }
    }
    free(pending);
    bool ok = emit_reachable_x64(s, source, p, reachable, d);
    free(reachable);
    return ok;
}
