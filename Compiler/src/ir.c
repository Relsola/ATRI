#include "ir.h"
#include "array.h"
#include <inttypes.h>

int64_t atri_i32_signed(uint32_t bits) {
    return bits <= INT32_MAX ? (int64_t)bits : (int64_t)bits - INT64_C(4294967296);
}
int64_t atri_i64_signed(uint64_t bits) {
    /* 超过 INT64_MAX 的 uint64_t 不直接强转；从可表示的补数恢复负值，覆盖 INT64_MIN。 */
    return bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)(UINT64_MAX - bits);
}
unsigned atri_integer_width(AtriIRType type) {
    switch (type) {
        case ATRI_IR_I8: case ATRI_IR_U8: return 8;
        case ATRI_IR_I16: case ATRI_IR_U16: return 16;
        case ATRI_IR_I32: case ATRI_IR_U32: return 32;
        case ATRI_IR_I64: case ATRI_IR_U64: return 64;
        default: return 0;
    }
}
bool atri_ir_is_integer(AtriIRType type) { return atri_integer_width(type) != 0; }
bool atri_integer_is_signed(AtriIRType type) {
    return type == ATRI_IR_I8 || type == ATRI_IR_I16 || type == ATRI_IR_I32 || type == ATRI_IR_I64;
}
uint64_t atri_integer_mask(AtriIRType type) {
    unsigned width = atri_integer_width(type);
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width)-1;
}
uint64_t atri_integer_max(AtriIRType type) {
    return atri_integer_mask(type) >> (unsigned)atri_integer_is_signed(type);
}
int64_t atri_integer_signed(AtriIRType type, uint64_t bits) {
    uint64_t mask = atri_integer_mask(type);
    bits &= mask;
    return bits <= (mask >> 1) ? (int64_t)bits : -1-(int64_t)(mask-bits);
}
bool atri_integer_literal(AtriSource source, AtriSpan text, uint64_t limit, uint64_t *out) {
    unsigned base = 10;
    size_t i = text.start;
    if (text.end-i >= 2 && source.text[i] == '0') {
        char prefix = source.text[i+1];
        if (prefix == 'x' || prefix == 'X') { base = 16; i += 2; }
        else if (prefix == 'b' || prefix == 'B') { base = 2; i += 2; }
    }
    uint64_t value = 0;
    for (; i < text.end; ++i) {
        unsigned char byte = (unsigned char)source.text[i];
        if (byte == '_') { continue; }
        unsigned digit = byte <= '9' ? (unsigned)(byte-'0') : byte <= 'F' ? (unsigned)(byte-'A'+10) : (unsigned)(byte-'a'+10);
        if (digit >= base || digit > limit || value > (limit-digit)/base) { return false; }
        value = value*base+digit;
    }
    *out = value; return true;
}
bool atri_ir_is_pointer(AtriIRType type) {
    return type == ATRI_IR_PTR_I32 || type == ATRI_IR_PTR_U8 || type == ATRI_IR_PTR_VOID ||
        (type >= ATRI_IR_PTR_I8 && type <= ATRI_IR_PTR_BOOL) || type >= ATRI_IR_PTR_STRUCT;
}
bool atri_ir_has_value(AtriIRKind kind) {
    return kind == ATRI_IR_PARAM || kind == ATRI_IR_CONST || kind == ATRI_IR_UNARY ||
        kind == ATRI_IR_BINARY || kind == ATRI_IR_CAST || kind == ATRI_IR_C_PROMOTE || kind == ATRI_IR_CALL || kind == ATRI_IR_LOAD ||
        kind == ATRI_IR_PTR_OFFSET || kind == ATRI_IR_PTR_DIFF || kind == ATRI_IR_ADDRESS || kind == ATRI_IR_COMPARE || kind == ATRI_IR_STRING || kind == ATRI_IR_ZERO;
}
bool atri_compare(AtriIRType type, AtriOp op, uint64_t a, uint64_t b) {
    if (op == ATRI_EQ) return a == b;
    if (op == ATRI_NE) return a != b;
    if (!atri_integer_is_signed(type)) { return op == ATRI_LT ? a < b : op == ATRI_LE ? a <= b : op == ATRI_GT ? a > b : a >= b; }
    int64_t x = atri_integer_signed(type, a);
    int64_t y = atri_integer_signed(type, b);
    return op == ATRI_LT ? x < y : op == ATRI_LE ? x <= y : op == ATRI_GT ? x > y : x >= y;
}
const char *atri_ir_type_name(AtriIRType type) {
    switch (type) {
        case ATRI_IR_I8: return "i8";
        case ATRI_IR_I16: return "i16";
        case ATRI_IR_U16: return "u16";
        case ATRI_IR_U32: return "u32";
        case ATRI_IR_PTR_I8: return "*i8";
        case ATRI_IR_PTR_I16: return "*i16";
        case ATRI_IR_PTR_U16: return "*u16";
        case ATRI_IR_PTR_U32: return "*u32";
        case ATRI_IR_PTR_I64: return "*i64";
        case ATRI_IR_PTR_U64: return "*u64";
        case ATRI_IR_PTR_BOOL: return "*bool";
        default: break;
    }
    return type == ATRI_IR_VOID ? "void" : type == ATRI_IR_PTR_VOID ? "*void" : type == ATRI_IR_U64 ? "u64" : type == ATRI_IR_U8 ? "u8" : type == ATRI_IR_PTR_U8 ? "*u8" : atri_ir_is_array(type) ? "array" : type == ATRI_IR_I64 ? "i64" : type == ATRI_IR_PTR_I32 ? "*i32" :
        type == ATRI_IR_I32 ? "i32" : type == ATRI_IR_BOOL ? "bool" : atri_ir_is_struct(type) ? "struct" : type >= ATRI_IR_PTR_STRUCT ? (atri_ir_is_array(atri_ir_pointee(type)) ? "*array" : "*struct") : "<invalid>";
}
/* 以无符号位模式实现目标位宽的环绕，避免宿主 C 的有符号溢出改变编译期结果。 */
bool atri_i32_compute(AtriOp op, uint32_t a, uint32_t b, uint32_t *result) {
    uint64_t bits;
    if (!atri_integer_compute(ATRI_IR_I32, op, a, b, &bits)) { return false; }
    *result = (uint32_t)bits; return true;
}
uint64_t atri_integer_convert(AtriIRType source, AtriIRType target, uint64_t bits) {
    if (atri_integer_is_signed(source)) { bits = (uint64_t)atri_integer_signed(source, bits); }
    return bits & atri_integer_mask(target);
}
bool atri_integer_compute(AtriIRType type, AtriOp op, uint64_t a, uint64_t b, uint64_t *result) {
    if (!atri_ir_is_integer(type)) { return false; }
    uint64_t mask = atri_integer_mask(type);
    a &= mask;
    /* 移位数量保留自己的完整类型位模式，不能先截断到左操作数的位宽。 */
    if (op == ATRI_SHL || op == ATRI_SHR) {
        unsigned width = atri_integer_width(type);
        if (b >= width) { return false; }
        if (op == ATRI_SHL) { *result = (a << b) & mask; }
        else {
            *result = a >> b;
            if (b && atri_integer_is_signed(type) && (a & (UINT64_C(1) << (width-1)))) {
                *result |= mask ^ (mask >> b);
            }
        }
        return true;
    }
    b &= mask;
    switch (op) {
        case ATRI_ADD: *result = (a+b) & mask; return true;
        case ATRI_SUB: *result = (a-b) & mask; return true;
        case ATRI_MUL: *result = (a*b) & mask; return true;
        case ATRI_NEG: *result = (UINT64_C(0)-a) & mask; return true;
        case ATRI_BIT_NOT: *result = ~a & mask; return true;
        case ATRI_BIT_AND: *result = a & b; return true;
        case ATRI_BIT_OR: *result = a | b; return true;
        case ATRI_BIT_XOR: *result = a ^ b; return true;
        case ATRI_DIV: case ATRI_REM:
            if (!b) { return false; }
            if (!atri_integer_is_signed(type)) { *result = op == ATRI_DIV ? a/b : a%b; return true; }
            if (a == (UINT64_C(1) << (atri_integer_width(type)-1)) && b == mask) { return false; }
            *result = (uint64_t)(op == ATRI_DIV ? atri_integer_signed(type, a)/atri_integer_signed(type, b) :
                atri_integer_signed(type, a)%atri_integer_signed(type, b)) & mask;
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
    for (size_t i = 0; i < p->string_count; ++i) {
        free(p->strings[i].bytes);
    }
    if (p->globals) {
        for (size_t i = 0; i < p->global_count; ++i) { free(p->globals[i].data); }
    }
    free(p->enums); free(p->variants); free(p->arrays); free(p->structs); free(p->fields);
    free(p->globals); free(p->strings); free(p->functions); *p = (AtriIRProgram){0};
}
void atri_dump_ir(FILE *s, AtriSource source, const AtriIRProgram *p) {
    for (size_t i = 0; i < p->enum_count; ++i) {
        const AtriIREnum *item = &p->enums[i];
        fprintf(s, "enum @enum.%zu %.*s storage %s%s\n", i, (int)(item->name.end-item->name.start),
            source.text+item->name.start, atri_ir_type_name(item->underlying), item->flags ? " flags" : "");
        for (size_t j = 0; j < item->variant_count; ++j) {
            const AtriIREnumVariant *variant = &p->variants[item->first_variant+j];
            fprintf(s, "    %.*s = 0x%" PRIx64 "\n", (int)(variant->name.end-variant->name.start), source.text+variant->name.start, variant->bits);
        }
    }
    for (size_t i = 0; i < p->array_count; ++i) {
        fprintf(s, "type @array.%zu [%zu]%s size %zu align %zu\n", i, p->arrays[i].length,
            atri_ir_type_name(p->arrays[i].element), atri_ir_type_size(p, (AtriIRType)(ATRI_IR_ARRAY+i)),
            atri_ir_type_align(p, (AtriIRType)(ATRI_IR_ARRAY+i)));
    }
    for (size_t i = 0; i < p->struct_count; ++i) {
        const AtriIRStruct *t = &p->structs[i];
        if (t->opaque) { fprintf(s, "opaque @struct.%zu %s%.*s\n", i, t->is_union ? "union " : "", (int)(t->name.end-t->name.start), source.text+t->name.start); continue; }
        fprintf(s, "type @struct.%zu %s%.*s size %zu align %zu\n", i, t->is_union ? "union " : "", (int)(t->name.end-t->name.start), source.text+t->name.start, t->size, t->alignment);
        for (size_t j = 0; j < t->field_count; ++j) {
            const AtriIRField *field = &p->fields[t->first_field+j];
            fprintf(s, "    %.*s: %s offset %zu\n", (int)(field->name.end-field->name.start), source.text+field->name.start, atri_ir_type_name(field->type), field->offset);
        }
    }
    for (size_t i = 0; i < p->global_count; ++i) {
        const AtriIRGlobal *g = &p->globals[i];
        if (atri_ir_is_struct(g->type)) {
            fprintf(s, "global @global.%zu %.*s: struct.%u size %zu align %zu = bytes [", i,
                (int)(g->name.end-g->name.start), source.text+g->name.start, (unsigned)(g->type-ATRI_IR_STRUCT),
                g->data_size, atri_ir_type_align(p, g->type));
            for (size_t b = 0; b < g->data_size; ++b) { fprintf(s, "%s%02x", b ? " " : "", (unsigned)g->data[b]); }
            fputs("]\n", s); continue;
        }
        fprintf(s, "%s @global.%zu %.*s: %s = %" PRIu64 "\n", g->constant ? "constant" : "global", i,
            (int)(g->name.end-g->name.start), source.text+g->name.start, atri_ir_type_name(g->type), g->bits);
    }
    const char *ops[] = {"add", "sub", "mul", "sdiv", "srem", "neg", "eq", "ne", "lt", "le", "gt", "ge", "and", "or", "not", "bit_and", "bit_or", "bit_xor", "bit_not", "shl", "shr"};
    for (size_t i = 0; i < p->string_count; ++i) {
        fprintf(s, "data @str.%zu = \"", i);
        for (size_t b = 0; b <= p->strings[i].length; ++b) {
            fprintf(s, "\\x%02x", (unsigned)p->strings[i].bytes[b]);
        }
        fputs("\"\n", s);
    }
    for (size_t n = 0; n < p->function_count; ++n) {
        const AtriIRFunction *f = &p->functions[n];
        fprintf(s, "%sfunc @%.*s(", f->external_c ? "extern(C) " : "", (int)(f->name.end - f->name.start), source.text + f->name.start);
        for (size_t a = 0; a < f->parameter_count; ++a) fprintf(s, "%s%s", a == 0 ? "" : ", ", atri_ir_type_name(f->parameter_types[a]));
        if (f->variadic) { fputs(f->parameter_count ? ", ..." : "...", s); }
        fprintf(s, ") -> %s", atri_ir_type_name(f->return_type));
        if (f->external_c) {
            fputs(";\n", s); continue;
        }
        fputs(" {\nentry:\n", s);
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            if (v->kind == ATRI_IR_NOP) continue;
            if (v->kind == ATRI_IR_RETURN && v->type == ATRI_IR_VOID) { fputs("    ret void\n", s); continue; }
            if (v->kind == ATRI_IR_RETURN) { fprintf(s, "    ret %s %%%zu\n", atri_ir_type_name(v->type), v->left); continue; }
            if (v->kind == ATRI_IR_STORE) { fprintf(s, "    store %s %%%zu, %%%zu\n", atri_ir_type_name(v->type), v->right, v->left); continue; }
            if (v->kind == ATRI_IR_LABEL) { fprintf(s, "block%" PRIu64 ":\n", v->immediate); continue; }
            if (v->kind == ATRI_IR_JUMP) { fprintf(s, "    jump block%zu\n", v->target); continue; }
            if (v->kind == ATRI_IR_BRANCH) { fprintf(s, "    branch %%%zu, block%zu, block%zu\n", v->left, v->target, v->alternative); continue; }
            if (v->kind == ATRI_IR_UNREACHABLE) { fputs("    unreachable\n", s); continue; }
            if (v->kind == ATRI_IR_CALL && v->type == ATRI_IR_VOID) { fputs("    ", s); }
            else { fprintf(s, "    %%%zu = ", i); }
            switch (v->kind) {
                case ATRI_IR_PARAM: fprintf(s, "param %s %" PRIu64, atri_ir_type_name(v->type), v->immediate); break;
                case ATRI_IR_CONST:
                    if (atri_ir_is_pointer(v->type)) {
                        fprintf(s, "const %s null", atri_ir_type_name(v->type));
                    } else if (v->type == ATRI_IR_U64) {
                        fprintf(s, "const u64 %" PRIu64, v->immediate);
                    } else {
                        fprintf(s, "const %s %" PRId64, atri_ir_type_name(v->type),
                                 atri_integer_is_signed(v->type) ? atri_integer_signed(v->type, v->immediate) : (int64_t)v->immediate);
                    }
                    break;
                case ATRI_IR_ZERO: fprintf(s, "zero %s", atri_ir_type_name(v->type)); break;
                case ATRI_IR_INDEX: fprintf(s, "index_location %s %%%zu, %%%zu", atri_ir_type_name(v->type), v->left, v->right); break;
                case ATRI_IR_FIELD: fprintf(s, "field_location %s %%%zu field %" PRIu64, atri_ir_type_name(v->type), v->left, v->immediate); break;
                case ATRI_IR_LOCAL:
                    if (v->immediate) { fprintf(s, "global_location %s @global.%" PRIu64, atri_ir_type_name(v->type), v->immediate-1); }
                    else { fprintf(s, "local %s", atri_ir_type_name(v->type)); }
                    break;
                case ATRI_IR_STRING: fprintf(s, "string *u8 @str.%" PRIu64, v->immediate); break;
                case ATRI_IR_ADDRESS: fprintf(s, "address %s %%%zu", atri_ir_type_name(v->type), v->left); break;
                case ATRI_IR_LOAD: fprintf(s, "load %s %%%zu", atri_ir_type_name(v->type), v->left); break;
                case ATRI_IR_C_PROMOTE: fprintf(s, "c_promote i32 %%%zu", v->left); break;
                case ATRI_IR_CAST: fprintf(s, "cast %s %%%zu", atri_ir_type_name(v->type), v->left); break;
                case ATRI_IR_UNARY: fprintf(s, "%s %s %%%zu", ops[v->op], atri_ir_type_name(v->type), v->left); break;
                case ATRI_IR_COMPARE: fprintf(s, "cmp_%s %s %%%zu, %%%zu", ops[v->op], atri_ir_type_name(f->instructions[v->left].type), v->left, v->right); break;
                case ATRI_IR_BINARY: fprintf(s, "%s %s %%%zu, %%%zu", ops[v->op], atri_ir_type_name(v->type), v->left, v->right); break;
                case ATRI_IR_PTR_OFFSET: fprintf(s, "ptr_%s %s %%%zu, %%%zu, stride %zu", v->op == ATRI_ADD ? "add" : "sub", atri_ir_type_name(v->type), v->left, v->right, atri_ir_type_size(p, atri_ir_pointee(v->type))); break;
                case ATRI_IR_PTR_DIFF: fprintf(s, "ptr_diff i64 %%%zu, %%%zu, stride %zu", v->left, v->right, atri_ir_type_size(p, atri_ir_pointee(f->instructions[v->left].type))); break;
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
