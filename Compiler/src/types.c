#include "ir.h"
#include "lexer.h"
#include <stdlib.h>
#include <string.h>

bool atri_ir_is_struct(AtriIRType type) { return type >= ATRI_IR_STRUCT && type < ATRI_IR_PTR_STRUCT; }
AtriIRType atri_ir_pointee(AtriIRType type) {
    return type == ATRI_IR_PTR_I32 ? ATRI_IR_I32 : type >= ATRI_IR_PTR_STRUCT ? (AtriIRType)(type-ATRI_IR_PTR_STRUCT+ATRI_IR_STRUCT) : ATRI_IR_INVALID;
}
AtriIRType atri_ir_pointer_type(AtriIRType type) {
    return type == ATRI_IR_I32 ? ATRI_IR_PTR_I32 : atri_ir_is_struct(type) ? (AtriIRType)(type-ATRI_IR_STRUCT+ATRI_IR_PTR_STRUCT) : ATRI_IR_INVALID;
}
bool atri_ir_valid_type(const AtriIRProgram *p, AtriIRType type) {
    if (atri_ir_is_struct(type)) { return (size_t)(type-ATRI_IR_STRUCT) < p->struct_count; }
    if (type >= ATRI_IR_PTR_STRUCT) { return (size_t)(type-ATRI_IR_PTR_STRUCT) < p->struct_count; }
    return atri_ir_is_integer(type) || type == ATRI_IR_BOOL || type == ATRI_IR_PTR_I32 || type == ATRI_IR_PTR_CHAR;
}
size_t atri_ir_type_size(const AtriIRProgram *p, AtriIRType type) {
    if (atri_ir_is_struct(type)) { return p->structs[type-ATRI_IR_STRUCT].size; }
    return type == ATRI_IR_BOOL ? 1 : type == ATRI_IR_I32 ? 4 : 8;
}
size_t atri_ir_type_align(const AtriIRProgram *p, AtriIRType type) {
    return atri_ir_is_struct(type) ? p->structs[type-ATRI_IR_STRUCT].alignment : atri_ir_type_size(p, type);
}
bool atri_type_from_source(AtriSource source, const AtriProgram *ast, AtriSpan span, AtriIRType *out, AtriDiagnostic *d) {
    AtriLexer lexer = atri_lexer_create(source); lexer.offset = span.start;
    AtriToken token = atri_lexer_next(&lexer, d);
    size_t pointers = 0;
    while (token.kind == ATRI_TOKEN_STAR) { ++pointers; token = atri_lexer_next(&lexer, d); }
    if (pointers > 1) { *d = (AtriDiagnostic){span, "only one-level pointers are supported yet"}; return false; }
    AtriSpan name = token.span, qualifier = {0};
    AtriToken next = atri_lexer_next(&lexer, d);
    if (next.kind == ATRI_TOKEN_SCOPE && next.span.start < span.end) {
        qualifier = name; token = atri_lexer_next(&lexer, d); name = token.span;
    }
    AtriIRType type = ATRI_IR_INVALID;
    if (!qualifier.end) {
        size_t n = name.end-name.start; const char *text = source.text+name.start;
        if (n == 3 && !memcmp(text, "i32", 3)) { type = ATRI_IR_I32; }
        else if (n == 3 && !memcmp(text, "i64", 3)) { type = ATRI_IR_I64; }
        else if (n == 4 && !memcmp(text, "bool", 4)) { type = ATRI_IR_BOOL; }
        else if (n == 4 && !memcmp(text, "char", 4)) {
            if (pointers) { *out = ATRI_IR_PTR_CHAR; return true; }
            *d = (AtriDiagnostic){span, "char values are not supported yet; use *char"}; return false;
        }
    }
    if (type == ATRI_IR_INVALID) {
        AtriId structure = atri_find_struct(source, ast, span.start, name, qualifier);
        if (structure != ATRI_NONE) { type = (AtriIRType)(ATRI_IR_STRUCT+structure); }
    }
    if (type == ATRI_IR_INVALID) {
        *d = (AtriDiagnostic){span, "unknown or private type; only i32, i64, bool, pointers and declared structs are supported"}; return false;
    }
    if (pointers) { type = atri_ir_pointer_type(type); }
    if (type == ATRI_IR_INVALID) { *d = (AtriDiagnostic){span, "only pointers to i32, char and structs are supported"}; return false; }
    *out = type; return true;
}
static bool layout(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, size_t index, size_t depth, AtriDiagnostic *d) {
    AtriIRStruct *t = &p->structs[index];
    if (t->state == 2) { return true; }
    if (t->state == 1 || depth > 256) { *d = (AtriDiagnostic){ast->structs[index].name, "cyclic or excessively deep by-value struct layout"}; return false; }
    if (t->field_count > 4096) { *d = (AtriDiagnostic){t->name, "struct exceeds the prototype field count limit"}; return false; }
    t->state = 1; t->alignment = 1;
    for (size_t i = 0; i < t->field_count; ++i) {
        size_t id = t->first_field+i;
        AtriIRField *field = &p->fields[id];
        field->name = ast->fields[id].name;
        for (size_t j = 0; j < i; ++j) {
            AtriSpan other = p->fields[t->first_field+j].name;
            if (other.end-other.start == field->name.end-field->name.start && !memcmp(source.text+other.start, source.text+field->name.start, other.end-other.start)) {
                *d = (AtriDiagnostic){field->name, "duplicate struct field name"}; return false;
            }
        }
        if (!atri_type_from_source(source, ast, ast->fields[id].type, &field->type, d)) { return false; }
        if (atri_ir_is_struct(field->type) && !layout(source, ast, p, field->type-ATRI_IR_STRUCT, depth+1, d)) { return false; }
        size_t alignment = atri_ir_type_align(p, field->type), size = atri_ir_type_size(p, field->type);
        t->size = (t->size+alignment-1)/alignment*alignment;
        field->offset = t->size; t->size += size;
        if (alignment > t->alignment) { t->alignment = alignment; }
        if (t->size >= 4096) { *d = (AtriDiagnostic){t->name, "struct exceeds the prototype size limit"}; return false; }
    }
    t->size = (t->size+t->alignment-1)/t->alignment*t->alignment;
    if (t->size >= 4096) { *d = (AtriDiagnostic){t->name, "struct exceeds the prototype size limit"}; return false; }
    t->state = 2; return true;
}
bool atri_layout_structs(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, AtriDiagnostic *d) {
    if (ast->struct_count >= ATRI_IR_PTR_STRUCT-ATRI_IR_STRUCT) { *d = (AtriDiagnostic){{0}, "struct type table exceeds the prototype limit"}; return false; }
    p->struct_count = ast->struct_count; p->field_count = ast->field_count;
    p->structs = calloc(p->struct_count ? p->struct_count : 1, sizeof(*p->structs));
    p->fields = calloc(p->field_count ? p->field_count : 1, sizeof(*p->fields));
    if (!p->structs || !p->fields) { *d = (AtriDiagnostic){{0}, "out of memory"}; return false; }
    for (size_t i = 0; i < p->struct_count; ++i) {
        p->structs[i] = (AtriIRStruct){.name = ast->structs[i].name, .first_field = ast->structs[i].first_field, .field_count = ast->structs[i].field_count};
    }
    for (size_t i = 0; i < p->struct_count; ++i) { if (!layout(source, ast, p, i, 0, d)) { return false; } }
    return true;
}
