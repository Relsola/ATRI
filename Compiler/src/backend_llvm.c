#include "ir.h"
#include <inttypes.h>
#include <string.h>

/* ATRI 的位置和聚合快照在 LLVM 中用 ptr 表达。标量值保留目标位宽，
 * bool 使用 i8，以便与内存和 Win64 参数共用表示。 */
typedef struct { char text[64]; } Operand;
typedef struct {
    FILE *s;
    AtriSource source;
    const AtriIRProgram *p;
    const AtriIRFunction *f;
} Writer;

static bool indirect(const AtriIRProgram *p, AtriIRType t) {
    if (!atri_ir_is_aggregate(t)) { return false; }
    size_t size = atri_ir_type_size(p, t);
    return size && size != 1 && size != 2 && size != 4 && size != 8;
}
static const char *abi_type(const AtriIRProgram *p, AtriIRType t) {
    if (t == ATRI_IR_VOID) { return "void"; }
    if (atri_ir_is_pointer(t) || indirect(p, t)) { return "ptr"; }
    switch (atri_ir_type_size(p, t)) {
        case 8: return "i64";
        case 4: return "i32";
        case 2: return "i16";
        default: return "i8";
    }
}
static bool entry(Writer *w, const AtriIRFunction *f) {
    return !f->module && !f->external_c && f->name.end - f->name.start == 4 &&
           !memcmp(w->source.text + f->name.start, "main", 4);
}
static void symbol(Writer *w, size_t id) {
    const AtriIRFunction *f = &w->p->functions[id];
    if (f->external_c || entry(w, f)) {
        fprintf(w->s, "@%.*s", (int)(f->name.end - f->name.start), w->source.text + f->name.start);
    } else {
        fprintf(w->s, "@atri_fn_%zu", id);
    }
}
static Operand ref(const AtriIRFunction *f, size_t id) {
    Operand o;
    const AtriIRInst *v = &f->instructions[id];
    if (v->kind == ATRI_IR_CONST) {
        if (atri_ir_is_pointer(v->type)) { return (Operand){"null"}; }
        snprintf(o.text, sizeof(o.text), "%" PRIu64, v->immediate);
    } else if (v->kind == ATRI_IR_STRING) {
        snprintf(o.text, sizeof(o.text), "@.str.%" PRIu64, v->immediate);
    } else if (v->kind == ATRI_IR_LOCAL && v->immediate) {
        snprintf(o.text, sizeof(o.text), "@.g.%" PRIu64, v->immediate-1);
    } else if (v->kind == ATRI_IR_PARAM && !atri_ir_is_aggregate(v->type)) {
        snprintf(o.text, sizeof(o.text), "%%a%" PRIu64, v->immediate);
    } else {
        snprintf(o.text, sizeof(o.text), "%%v%zu", id);
    }
    return o;
}
static void copy(Writer *w, const char *dest, const char *src, size_t bytes) {
    if (bytes) {
        fprintf(w->s, "  call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %zu, i1 false)\n", dest, src, bytes);
    }
}
static void signature(Writer *w, const AtriIRFunction *f, bool named) {
    bool hidden = indirect(w->p, f->return_type);
    if (hidden) {
        fprintf(w->s, "ptr sret([%zu x i8])%s", atri_ir_type_size(w->p, f->return_type), named ? " %result" : "");
    }
    for (size_t i = 0; i < f->parameter_count; ++i) {
        fprintf(w->s, "%s%s", (i || hidden) ? ", " : "", abi_type(w->p, f->parameter_types[i]));
        if (named) { fprintf(w->s, " %%a%zu", i); }
    }
    if (f->variadic) { fputs((hidden || f->parameter_count) ? ", ..." : "...", w->s); }
}
static Operand resize(Writer *w, size_t id, size_t source, unsigned width) {
    const AtriIRFunction *f = w->f;
    AtriIRType t = f->instructions[source].type;
    unsigned bits = t == ATRI_IR_BOOL ? 8 : atri_integer_width(t);
    Operand old = ref(f, source);
    if (bits == width) { return old; }
    Operand result;
    snprintf(result.text, sizeof(result.text), "%%conv%zu", id);
    fprintf(w->s, "  %s = %s i%u %s to i%u\n", result.text,
            bits > width ? "trunc" : atri_integer_is_signed(t) ? "sext" : "zext", bits, old.text, width);
    return result;
}
static void emit_call(Writer *w, size_t id) {
    const AtriIRFunction *f = w->f;
    const AtriIRInst *v = &f->instructions[id];
    const AtriIRFunction *callee = &w->p->functions[v->callee];
    bool hidden = indirect(w->p, v->type), aggregate = atri_ir_is_aggregate(v->type);
    const char *result = hidden ? "void" : abi_type(w->p, v->type);
    for (size_t a = 0; a < v->argument_count; ++a) {
        AtriId arg = f->arguments[v->first_argument + a];
        AtriIRType t = f->instructions[arg].type;
        if (!atri_ir_is_aggregate(t)) { continue; }
        size_t size = atri_ir_type_size(w->p, t);
        if (indirect(w->p, t)) {
            Operand dest;
            snprintf(dest.text, sizeof(dest.text), "%%arg%zu_%zu", id, a);
            copy(w, dest.text, ref(f, arg).text, size);
        } else if (size) {
            fprintf(w->s, "  %%arg%zu_%zu = load %s, ptr %s, align 1\n", id, a, abi_type(w->p, t), ref(f, arg).text);
        }
    }
    fputs("  ", w->s);
    if (!hidden && v->type != ATRI_IR_VOID) { fprintf(w->s, "%%%s%zu = ", aggregate ? "returned" : "v", id); }
    fprintf(w->s, "call %s ", result);
    if (callee->variadic) {
        /* 调用点的函数类型只列固定参数类型，实参列表携带已提升的尾参。 */
        fputc('(', w->s);
        if (hidden) { fputs("ptr, ", w->s); }
        for (size_t a = 0; a < callee->parameter_count; ++a) {
            fprintf(w->s, "%s, ", abi_type(w->p, callee->parameter_types[a]));
        }
        fputs("...) ", w->s);
    }
    symbol(w, v->callee);
    fputc('(', w->s);
    if (hidden) { fprintf(w->s, "ptr sret([%zu x i8]) %%v%zu", atri_ir_type_size(w->p, v->type), id); }
    for (size_t a = 0; a < v->argument_count; ++a) {
        AtriId arg = f->arguments[v->first_argument+a];
        AtriIRType t = f->instructions[arg].type;
        fprintf(w->s, "%s%s ", (a || hidden) ? ", " : "", abi_type(w->p, t));
        if (atri_ir_is_aggregate(t)) {
            if (atri_ir_type_size(w->p, t)) { fprintf(w->s, "%%arg%zu_%zu", id, a); }
            else { fputc('0', w->s); }
        } else { fputs(ref(f, arg).text, w->s); }
    }
    fputs(")\n", w->s);
    if (aggregate && !hidden && atri_ir_type_size(w->p, v->type)) {
        fprintf(w->s, "  store %s %%returned%zu, ptr %%v%zu, align 1\n", result, id, id);
    }
}
static bool instruction(Writer *w, size_t i) {
    const AtriIRFunction *f = w->f;
    const AtriIRInst *v = &f->instructions[i];
    const char *t = abi_type(w->p, v->type);
    size_t size = v->type == ATRI_IR_VOID ? 0 : atri_ir_type_size(w->p, v->type);
    Operand dest = ref(f, i);
    switch (v->kind) {
        case ATRI_IR_NOP: case ATRI_IR_LOCAL: case ATRI_IR_STRING: case ATRI_IR_CONST: case ATRI_IR_PARAM:
            break;
        case ATRI_IR_LABEL: fprintf(w->s, "b%" PRIu64 ":\n", v->immediate); break;
        case ATRI_IR_JUMP: fprintf(w->s, "  br label %%b%zu\n", v->target); break;
        case ATRI_IR_BRANCH:
            fprintf(w->s, "  %%cond%zu = icmp ne i8 %s, 0\n  br i1 %%cond%zu, label %%b%zu, label %%b%zu\n",
                    i, ref(f,v->left).text, i, v->target, v->alternative);
            break;
        case ATRI_IR_UNREACHABLE: fputs("  unreachable\n", w->s); break;
        case ATRI_IR_ZERO:
            if (size) { fprintf(w->s, "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %zu, i1 false)\n", dest.text, size); }
            break;
        case ATRI_IR_FIELD: case ATRI_IR_ADDRESS: {
            size_t offset = v->kind == ATRI_IR_FIELD ? w->p->fields[v->immediate].offset : 0;
            fprintf(w->s, "  %s = getelementptr i8, ptr %s, i64 %zu\n", dest.text, ref(f,v->left).text, offset);
            break;
        }
        case ATRI_IR_LOAD:
            if (atri_ir_is_aggregate(v->type)) { copy(w, dest.text, ref(f,v->left).text, size); }
            else { fprintf(w->s, "  %s = load %s, ptr %s, align 1\n", dest.text, t, ref(f,v->left).text); }
            break;
        case ATRI_IR_STORE:
            if (atri_ir_is_aggregate(v->type)) { copy(w, ref(f,v->left).text, ref(f,v->right).text, size); }
            else { fprintf(w->s, "  store %s %s, ptr %s, align 1\n", t, ref(f,v->right).text, ref(f,v->left).text); }
            break;
        case ATRI_IR_PTR_OFFSET: case ATRI_IR_INDEX: {
            Operand index = resize(w, i, v->right, 64);
            if (v->kind == ATRI_IR_PTR_OFFSET) {
                size = atri_ir_type_size(w->p, atri_ir_pointee(v->type));
                if (v->op == ATRI_SUB) {
                    fprintf(w->s, "  %%neg%zu = sub i64 0, %s\n", i, index.text);
                    snprintf(index.text, sizeof(index.text), "%%neg%zu", i);
                }
            }
            fprintf(w->s, "  %s = getelementptr [%zu x i8], ptr %s, i64 %s\n", dest.text, size, ref(f,v->left).text, index.text);
            break;
        }
        case ATRI_IR_PTR_DIFF:
            fprintf(w->s, "  %%lhs%zu = ptrtoint ptr %s to i64\n  %%rhs%zu = ptrtoint ptr %s to i64\n",
                    i, ref(f,v->left).text, i, ref(f,v->right).text);
            fprintf(w->s, "  %%diff%zu = sub i64 %%lhs%zu, %%rhs%zu\n  %s = sdiv i64 %%diff%zu, %zu\n", i,i,i,dest.text,i,
                    atri_ir_type_size(w->p, atri_ir_pointee(f->instructions[v->left].type)));
            break;
        case ATRI_IR_CAST: case ATRI_IR_C_PROMOTE:
            if (atri_ir_is_pointer(v->type)) {
                fprintf(w->s, "  %s = getelementptr i8, ptr %s, i64 0\n", dest.text, ref(f,v->left).text);
            } else {
                Operand converted = resize(w, i, v->left, atri_integer_width(v->type));
                fprintf(w->s, "  %s = or %s %s, 0\n", dest.text, t, converted.text);
            }
            break;
        case ATRI_IR_UNARY:
            if (v->op == ATRI_NEG) { fprintf(w->s, "  %s = sub %s 0, %s\n", dest.text, t, ref(f,v->left).text); }
            else { fprintf(w->s, "  %s = xor %s %s, %d\n", dest.text, t, ref(f,v->left).text, v->op == ATRI_NOT ? 1 : -1); }
            break;
        case ATRI_IR_BINARY: {
            static const char *const ops[] = {[ATRI_ADD]="add", [ATRI_SUB]="sub", [ATRI_MUL]="mul",
                [ATRI_BIT_AND]="and", [ATRI_BIT_OR]="or", [ATRI_BIT_XOR]="xor", [ATRI_SHL]="shl", [ATRI_SHR]="lshr"};
            bool sign = atri_integer_is_signed(v->type);
            const char *op = v->op == ATRI_DIV ? (sign ? "sdiv" : "udiv") :
                v->op == ATRI_REM ? (sign ? "srem" : "urem") : v->op == ATRI_SHR && sign ? "ashr" : ops[v->op];
            Operand right = ref(f,v->right);
            if (v->op == ATRI_SHL || v->op == ATRI_SHR) { right = resize(w, i, v->right, atri_integer_width(v->type)); }
            fprintf(w->s, "  %s = %s %s %s, %s\n", dest.text, op, t, ref(f,v->left).text, right.text);
            break;
        }
        case ATRI_IR_COMPARE: {
            AtriIRType type = f->instructions[v->left].type;
            static const char *const sign[] = {"eq","ne","slt","sle","sgt","sge"};
            static const char *const unsign[] = {"eq","ne","ult","ule","ugt","uge"};
            const char *cmp = (atri_integer_is_signed(type) ? sign : unsign)[v->op-ATRI_EQ];
            fprintf(w->s, "  %%cmp%zu = icmp %s %s %s, %s\n  %s = zext i1 %%cmp%zu to i8\n", i, cmp,
                    abi_type(w->p, type), ref(f,v->left).text, ref(f,v->right).text, dest.text,i);
            break;
        }
        case ATRI_IR_CALL: emit_call(w, i); break;
        case ATRI_IR_RETURN:
            if (v->type == ATRI_IR_VOID) { fputs("  ret void\n", w->s); }
            else if (indirect(w->p, v->type)) {
                copy(w, "%result", ref(f,v->left).text, size);
                fputs("  ret void\n", w->s);
            } else if (atri_ir_is_aggregate(v->type) && size) {
                fprintf(w->s, "  %%ret%zu = load %s, ptr %s, align 1\n  ret %s %%ret%zu\n", i,t,ref(f,v->left).text,t,i);
            } else {
                fprintf(w->s, "  ret %s %s\n", t, atri_ir_is_aggregate(v->type) ? "0" : ref(f,v->left).text);
            }
            break;
        case ATRI_IR_LAZY_LOGICAL: case ATRI_IR_LAZY_SELECT: return false;
    }
    return true;
}
static void slots(Writer *w) {
    const AtriIRFunction *f = w->f;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        const AtriIRInst *v = &f->instructions[i];
        bool local = v->kind == ATRI_IR_LOCAL && !v->immediate;
        bool snapshot = atri_ir_is_aggregate(v->type) && atri_ir_has_value(v->kind);
        if (local || snapshot) {
            size_t n = atri_ir_type_size(w->p, v->type);
            fprintf(w->s, "  %%v%zu = alloca [%zu x i8], align %zu\n", i, n ? n : 1, atri_ir_type_align(w->p, v->type));
            if (v->kind == ATRI_IR_PARAM && n) {
                if (indirect(w->p, v->type)) {
                    Operand src;
                    snprintf(src.text, sizeof(src.text), "%%a%" PRIu64, v->immediate);
                    copy(w, ref(f,i).text, src.text, n);
                } else {
                    fprintf(w->s, "  store %s %%a%" PRIu64 ", ptr %%v%zu, align 1\n",
                            abi_type(w->p, v->type), v->immediate, i);
                }
            }
        }
        if (v->kind == ATRI_IR_CALL) {
            for (size_t a = 0; a < v->argument_count; ++a) {
                AtriIRType t = f->instructions[f->arguments[v->first_argument+a]].type;
                if (indirect(w->p,t)) {
                    fprintf(w->s, "  %%arg%zu_%zu = alloca [%zu x i8], align 16\n", i,a,atri_ir_type_size(w->p,t));
                }
            }
        }
    }
}
static void bytes(FILE *s, const unsigned char *data, size_t size) {
    if (!size) { fputs("[1 x i8] zeroinitializer", s); return; }
    fprintf(s, "[%zu x i8] c\"", size);
    for (size_t i = 0; i < size; ++i) { fprintf(s, "\\%02X", (unsigned)data[i]); }
    fputc('"', s);
}
static bool emit_module(FILE *s, AtriSource source, const AtriIRProgram *p, const AtriLiveSymbols *live, AtriDiagnostic *d) {
    Writer w = {.s=s, .source=source, .p=p};
    bool hosted = false;
    for (size_t i = 0; i < p->function_count; ++i) {
        const AtriIRFunction *f = &p->functions[i];
        if (entry(&w,f)) {
            if (f->return_type != ATRI_IR_I32 || f->parameter_count) {
                *d = (AtriDiagnostic){.span = f->name, .message = "hosted main must return i32 and take no parameters"};
                return false;
            }
            hosted = true;
        }
    }
    fputs("target triple = \"x86_64-pc-windows-msvc\"\n"
          "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n"
          "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)\n", s);
    for (size_t i = 0; i < p->global_count; ++i) {
        const AtriIRGlobal *g = &p->globals[i];
        if (g->constant || (live->globals && !live->globals[i])) { continue; }
        fprintf(s, "@.g.%zu = internal global ", i);
        if (atri_ir_is_aggregate(g->type)) { bytes(s,g->data,g->data_size); }
        else if (atri_ir_is_pointer(g->type)) { fputs("ptr null", s); }
        else { fprintf(s, "%s %" PRIu64, abi_type(p,g->type),g->bits); }
        fprintf(s, ", align %zu\n", atri_ir_type_align(p,g->type));
    }
    for (size_t i = 0; i < p->string_count; ++i) {
        if (live->strings && !live->strings[i]) { continue; }
        fprintf(s, "@.str.%zu = private constant ", i);
        bytes(s,p->strings[i].bytes,p->strings[i].length+1);
        fputc('\n', s);
    }
    for (size_t i = 0; i < p->function_count; ++i) {
        if (live->functions && !live->functions[i]) { continue; }
        const AtriIRFunction *f = w.f = &p->functions[i];
        if (f->external_c) {
            bool declared = false;
            size_t length = f->name.end-f->name.start;
            for (size_t j = 0; j < i; ++j) {
                const AtriIRFunction *previous = &p->functions[j];
                if ((!live->functions || live->functions[j]) && previous->external_c && previous->name.end-previous->name.start == length &&
                    !memcmp(source.text + f->name.start, source.text + previous->name.start, length)) {
                    declared = true;
                    break;
                }
            }
            if (declared) { continue; }
        }
        fprintf(s, "%s %s ", f->external_c ? "declare" : hosted && !entry(&w,f) ? "define internal" : "define",
                indirect(p,f->return_type) ? "void" : abi_type(p,f->return_type));
        symbol(&w,i);
        fputc('(',s);
        signature(&w,f,!f->external_c);
        fputc(')',s);
        if (f->external_c) { fputc('\n',s); continue; }
        fputs(" {\nentry:\n",s);
        slots(&w);
        fputs("  br label %b0\nb0:\n",s);
        for (size_t j = 0; j < f->instruction_count; ++j) {
            if (!instruction(&w,j)) {
                *d = (AtriDiagnostic){.span = f->instructions[j].span, .message = "unresolved compile-time operation in LLVM backend"};
                return false;
            }
        }
        fputs("}\n",s);
    }
    return !ferror(s);
}

bool atri_emit_llvm(FILE *s, AtriSource source, const AtriIRProgram *p, AtriDiagnostic *d) {
    AtriLiveSymbols live = {0};
    if (!atri_ir_verify(p, d) || !atri_runtime_symbols(source, p, &live, d)) { return false; }
    bool ok = emit_module(s, source, p, &live, d);
    atri_live_symbols_destroy(&live);
    return ok;
}
