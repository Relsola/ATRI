#include "ir.h"
#include "lexer.h"
#include "array.h"
#include <stdlib.h>
#include <string.h>

bool atri_ir_is_struct(AtriIRType type) { return type >= ATRI_IR_STRUCT && type < ATRI_IR_ARRAY; }
bool atri_ir_is_array(AtriIRType type) { return type >= ATRI_IR_ARRAY && type < ATRI_IR_PTR_STRUCT; }
bool atri_ir_is_aggregate(AtriIRType type) { return atri_ir_is_struct(type) || atri_ir_is_array(type); }
AtriIRType atri_ir_pointee(AtriIRType type) {
    return type == ATRI_IR_PTR_VOID ? ATRI_IR_VOID : type == ATRI_IR_PTR_U8 ? ATRI_IR_U8 : type == ATRI_IR_PTR_I32 ? ATRI_IR_I32 : type >= ATRI_IR_PTR_STRUCT ? (AtriIRType)(type-ATRI_IR_PTR_STRUCT+ATRI_IR_STRUCT) : ATRI_IR_INVALID;
}
AtriIRType atri_ir_pointer_type(AtriIRType type) {
    return type == ATRI_IR_VOID ? ATRI_IR_PTR_VOID : type == ATRI_IR_U8 ? ATRI_IR_PTR_U8 : type == ATRI_IR_I32 ? ATRI_IR_PTR_I32 : atri_ir_is_aggregate(type) ? (AtriIRType)(type-ATRI_IR_STRUCT+ATRI_IR_PTR_STRUCT) : ATRI_IR_INVALID;
}
bool atri_ir_valid_type(const AtriIRProgram *p, AtriIRType type) {
    if (atri_ir_is_struct(type)) { return (size_t)(type-ATRI_IR_STRUCT) < p->struct_count && !p->structs[type-ATRI_IR_STRUCT].opaque; }
    if (atri_ir_is_array(type)) { return (size_t)(type-ATRI_IR_ARRAY) < p->array_count; }
    if (type >= ATRI_IR_PTR_STRUCT) {
        AtriIRType target = atri_ir_pointee(type);
        return atri_ir_is_struct(target) ? (size_t)(target-ATRI_IR_STRUCT) < p->struct_count : atri_ir_is_array(target) && atri_ir_valid_type(p, target);
    }
    return atri_ir_is_integer(type) || type == ATRI_IR_BOOL || type == ATRI_IR_PTR_I32 || type == ATRI_IR_PTR_CHAR || type == ATRI_IR_PTR_U8 || type == ATRI_IR_PTR_VOID;
}
size_t atri_ir_type_size(const AtriIRProgram *p, AtriIRType type) {
    if (type == ATRI_IR_VOID) { return 0; }
    if (atri_ir_is_struct(type)) { return p->structs[type-ATRI_IR_STRUCT].size; }
    if (atri_ir_is_array(type)) {
        const AtriIRArray *a = &p->arrays[type-ATRI_IR_ARRAY];
        return a->length * atri_ir_type_size(p, a->element);
    }
    return type == ATRI_IR_BOOL || type == ATRI_IR_U8 ? 1 : type == ATRI_IR_I32 ? 4 : 8;
}
size_t atri_ir_type_align(const AtriIRProgram *p, AtriIRType type) {
    if (atri_ir_is_array(type)) { return atri_ir_type_align(p, p->arrays[type-ATRI_IR_ARRAY].element); }
    return atri_ir_is_struct(type) ? p->structs[type-ATRI_IR_STRUCT].alignment : atri_ir_type_size(p, type);
}
/* 布局阶段仅解释字面量与可见常量的整数运算；共享预算防止依赖图指数展开。 */
static AtriIRType constant_type(AtriSource source, AtriSpan span) {
    size_t n = span.end-span.start;
    const char *name = source.text+span.start;
    return n == 3 && !memcmp(name, "i32", 3) ? ATRI_IR_I32 :
        n == 3 && !memcmp(name, "i64", 3) ? ATRI_IR_I64 :
        n == 3 && !memcmp(name, "u64", 3) ? ATRI_IR_U64 :
        n == 2 && !memcmp(name, "u8", 2) ? ATRI_IR_U8 : ATRI_IR_INVALID;
}
static bool name_matches(AtriSource source, AtriSpan a, AtriSpan b) {
    return a.end-a.start == b.end-b.start && !memcmp(source.text+a.start, source.text+b.start, a.end-a.start);
}
static void length_binding(AtriSource source, const AtriExpr *use, const AtriStatement *decl, AtriSpan scope,
                           const AtriStatement **found, size_t *scope_size) {
    if ((decl->kind != ATRI_STMT_CONSTANT && decl->kind != ATRI_STMT_VARIABLE) ||
        decl->span.end > use->span.start || use->span.start < scope.start || use->span.end > scope.end ||
        !name_matches(source, decl->name, use->text)) { return; }
    size_t size = scope.end-scope.start;
    if (!*found || size < *scope_size || (size == *scope_size && decl->span.start > (*found)->span.start)) {
        *found = decl; *scope_size = size;
    }
}
/* 布局表达式使用同一词法可见性：先找最近已声明局部，再检查参数，最后找定义模块的全局。 */
static const AtriStatement *length_constant(AtriSource source, const AtriProgram *ast, const AtriExpr *e) {
    if (!e->qualifier.end) {
        const AtriStatement *found = NULL;
        size_t scope_size = SIZE_MAX;
        for (size_t b = 0; b < ast->block_count; ++b) {
            const AtriBlock *block = &ast->blocks[b];
            if (e->span.start < block->span.start || e->span.end > block->span.end) { continue; }
            for (size_t i = 0; i < block->statement_count; ++i) {
                const AtriStatement *decl = &ast->statements[ast->block_statements[block->first_statement+i]];
                length_binding(source, e, decl, block->span, &found, &scope_size);
            }
        }
        for (size_t i = 0; i < ast->statement_count; ++i) {
            const AtriStatement *loop = &ast->statements[i];
            if (loop->kind == ATRI_STMT_FOR && loop->initializer != ATRI_NONE) {
                length_binding(source, e, &ast->statements[loop->initializer], loop->span, &found, &scope_size);
            }
        }
        if (found) { return found->kind == ATRI_STMT_CONSTANT ? found : NULL; }
        for (size_t i = 0; i < ast->function_count; ++i) {
            const AtriFunction *f = &ast->functions[i];
            if (e->span.start < f->body.start || e->span.end > f->body.end) { continue; }
            for (size_t a = 0; a < f->parameter_count; ++a) {
                if (name_matches(source, ast->parameters[f->first_parameter+a].name, e->text)) { return NULL; }
            }
        }
    }
    bool global = false;
    AtriId g = atri_find_declaration(source, ast, e->span.start, e->text, e->qualifier, &global);
    return global && g != ATRI_NONE && ast->globals[g].declaration.kind == ATRI_STMT_CONSTANT ? &ast->globals[g].declaration : NULL;
}
static AtriIRType length_type(AtriSource source, const AtriProgram *ast, AtriId id, size_t depth, size_t *steps) {
    if (depth > 256 || !*steps) { return ATRI_IR_INVALID; }
    --*steps;
    const AtriExpr *e = &ast->expressions[id];
    if (e->kind == ATRI_EXPR_SIZEOF || e->kind == ATRI_EXPR_ALIGNOF) { return ATRI_IR_U64; }
    if (e->kind == ATRI_EXPR_CAST) { return constant_type(source, e->text); }
    if (e->kind == ATRI_EXPR_NAME) {
        const AtriStatement *decl = length_constant(source, ast, e);
        return decl ? constant_type(source, decl->type) : ATRI_IR_INVALID;
    }
    if (e->kind == ATRI_EXPR_UNARY || e->kind == ATRI_EXPR_RUN) { return length_type(source, ast, e->left, depth+1, steps); }
    if (e->kind == ATRI_EXPR_BINARY) {
        AtriIRType a = length_type(source, ast, e->left, depth+1, steps);
        AtriIRType b = length_type(source, ast, e->right, depth+1, steps);
        return a == ATRI_IR_U64 || b == ATRI_IR_U64 ? ATRI_IR_U64 : a == ATRI_IR_I64 || b == ATRI_IR_I64 ? ATRI_IR_I64 :
            a == ATRI_IR_I32 || b == ATRI_IR_I32 ? ATRI_IR_I32 : a == ATRI_IR_U8 || b == ATRI_IR_U8 ? ATRI_IR_U8 : ATRI_IR_INVALID;
    }
    return ATRI_IR_INVALID;
}
static bool length_literal(AtriSource source, AtriSpan text, uint64_t limit, uint64_t *out) {
    uint64_t value = 0;
    for (size_t i = text.start; i < text.end; ++i) {
        unsigned digit = (unsigned)(source.text[i]-'0');
        if (value > (limit-digit)/10) { return false; }
        value = value*10+digit;
    }
    *out = value; return true;
}
static bool source_type(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, AtriSpan span,
                        AtriIRType *out, AtriDiagnostic *d, size_t depth, size_t *steps);
static bool layout_type(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, AtriIRType type, size_t depth, AtriSpan span, AtriDiagnostic *d);
static bool length_value(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, AtriDiagnostic *d, AtriId id, AtriIRType type, size_t depth, size_t *steps, uint64_t *out) {
    if (depth > 256 || !*steps) {
        *d = (AtriDiagnostic){ast->expressions[id].span, "layout/type evaluation exceeded its resource limit (possible cyclic query)"}; return false;
    }
    --*steps;
    const AtriExpr *e = &ast->expressions[id];
    if (e->kind == ATRI_EXPR_SIZEOF || e->kind == ATRI_EXPR_ALIGNOF) {
        AtriIRType object;
        if (type != ATRI_IR_U64 || !source_type(source, ast, p, e->text, &object, d, depth+1, steps) ||
            !layout_type(source, ast, p, object, depth+1, e->span, d)) { return false; }
        *out = e->kind == ATRI_EXPR_SIZEOF ? atri_ir_type_size(p, object) : atri_ir_type_align(p, object); return true;
    }
    if (e->kind == ATRI_EXPR_CAST) {
        AtriIRType target = constant_type(source, e->text);
        AtriIRType operand = length_type(source, ast, e->left, depth+1, steps);
        if (operand == ATRI_IR_INVALID) { operand = ATRI_IR_I32; }
        uint64_t bits;
        if (target != type || !length_value(source, ast, p, d, e->left, operand, depth+1, steps, &bits)) { return false; }
        *out = atri_integer_convert(operand, target, bits); return true;
    }
    uint64_t limit = type == ATRI_IR_U64 ? UINT64_MAX : type == ATRI_IR_U8 ? UINT8_MAX : type == ATRI_IR_I32 ? INT32_MAX : INT64_MAX;
    if (e->kind == ATRI_EXPR_INTEGER) { return length_literal(source, e->text, limit, out); }
    if (e->kind == ATRI_EXPR_NAME) {
        const AtriStatement *decl = length_constant(source, ast, e);
        if (!decl) { return false; }
        return constant_type(source, decl->type) == type && length_value(source, ast, p, d, decl->expression, type, depth+1, steps, out);
    }
    if (e->kind == ATRI_EXPR_RUN) { return length_value(source, ast, p, d, e->left, type, depth+1, steps, out); }
    uint64_t a, b = 0;
    if (e->kind != ATRI_EXPR_UNARY && e->kind != ATRI_EXPR_BINARY) { return false; }
    if (e->kind == ATRI_EXPR_UNARY && e->op == ATRI_NEG && ast->expressions[e->left].kind == ATRI_EXPR_INTEGER) {
        if (type == ATRI_IR_I32 || type == ATRI_IR_I64) { ++limit; }
        return length_literal(source, ast->expressions[e->left].text, limit, &a) && atri_integer_compute(type, ATRI_NEG, a, 0, out);
    }
    if (!length_value(source, ast, p, d, e->left, type, depth+1, steps, &a) ||
        (e->kind == ATRI_EXPR_BINARY && !length_value(source, ast, p, d, e->right, type, depth+1, steps, &b))) { return false; }
    if (e->op == ATRI_ADD && e->kind == ATRI_EXPR_UNARY) { *out = a; return true; }
    return atri_integer_compute(type, e->op, a, b, out);
}
static bool layout(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, size_t index, size_t depth, AtriDiagnostic *d);
static bool layout_type(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, AtriIRType type, size_t depth, AtriSpan span, AtriDiagnostic *d) {
    if (type == ATRI_IR_VOID) { *d = (AtriDiagnostic){span, "void has no object layout"}; return false; }
    if (depth > 256) { *d = (AtriDiagnostic){span, "excessively deep aggregate layout"}; return false; }
    if (atri_ir_is_struct(type)) { return layout(source, ast, p, type-ATRI_IR_STRUCT, depth, d); }
    if (atri_ir_is_array(type)) {
        AtriIRArray array = p->arrays[type-ATRI_IR_ARRAY];
        if (!layout_type(source, ast, p, array.element, depth+1, span, d)) { return false; }
        size_t size = atri_ir_type_size(p, array.element);
        if (size && array.length > 4095/size) { *d = (AtriDiagnostic){span, "array exceeds the prototype size limit"}; return false; }
    }
    return true;
}
static bool source_type(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, AtriSpan span, AtriIRType *out, AtriDiagnostic *d, size_t depth, size_t *steps) {
    if (depth > 256 || !*steps) {
        *d = (AtriDiagnostic){span, "layout/type evaluation exceeded its resource limit (possible cyclic query)"}; return false;
    }
    --*steps;
    AtriLexer lexer = atri_lexer_create(source); lexer.offset = span.start;
    AtriToken token = atri_lexer_next(&lexer, d);
    if (token.kind == ATRI_TOKEN_STAR) {
        AtriLexer look = lexer;
        AtriToken next = atri_lexer_next(&look, d);
        if (next.kind == ATRI_TOKEN_LEFT_BRACKET) {
            AtriIRType element;
            if (!source_type(source, ast, p, (AtriSpan){next.span.start, span.end}, &element, d, depth+1, steps)) { return false; }
            *out = atri_ir_pointer_type(element); return true;
        }
    }
    if (token.kind == ATRI_TOKEN_LEFT_BRACKET) {
        const AtriArrayType *syntax = NULL;
        for (size_t i = 0; i < ast->array_type_count; ++i) {
            if (ast->array_types[i].span.start == span.start) { syntax = &ast->array_types[i]; break; }
        }
        uint64_t length;
        AtriIRType value_type = syntax ? length_type(source, ast, syntax->length, depth+1, steps) : ATRI_IR_INVALID;
        if (value_type == ATRI_IR_INVALID) { value_type = ATRI_IR_I64; }
        if (!syntax || !length_value(source, ast, p, d, syntax->length, value_type, depth+1, steps, &length) || length > 4096) {
            if (!d->message) { *d = (AtriDiagnostic){span, "array length requires a nonnegative constant integer up to 4096 (literal or integer constant arithmetic)"}; }
            return false;
        }
        AtriIRType element;
        if (!source_type(source, ast, p, syntax->element, &element, d, depth+1, steps)) { return false; }
        if (element == ATRI_IR_VOID || (atri_ir_is_struct(element) && p->structs[element-ATRI_IR_STRUCT].opaque)) {
            *d = (AtriDiagnostic){syntax->element, "array element requires a complete object type"}; return false;
        }
        for (size_t i = 0; i < p->array_count; ++i) {
            if (p->arrays[i].element == element && p->arrays[i].length == length) { *out = (AtriIRType)(ATRI_IR_ARRAY+i); return true; }
        }
        AtriIRArray array = {.element = element, .length = (size_t)length};
        if (p->array_count >= ATRI_IR_PTR_STRUCT-ATRI_IR_ARRAY ||
            !atri_append((void **)&p->arrays, &p->array_count, &p->array_capacity, sizeof(array), &array)) {
            *d = (AtriDiagnostic){span, "array type table allocation failed"}; return false;
        }
        *out = (AtriIRType)(ATRI_IR_ARRAY+p->array_count-1); return true;
    }
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
        if (n == 2 && !memcmp(text, "u8", 2)) { type = ATRI_IR_U8; }
        else if (n == 3 && !memcmp(text, "i32", 3)) { type = ATRI_IR_I32; }
        else if (n == 3 && !memcmp(text, "i64", 3)) { type = ATRI_IR_I64; }
        else if (n == 3 && !memcmp(text, "u64", 3)) { type = ATRI_IR_U64; }
        else if (n == 4 && !memcmp(text, "void", 4)) { type = ATRI_IR_VOID; }
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
        *d = (AtriDiagnostic){span, "unknown or private type; only u8, i32, i64, u64, bool, supported pointers, fixed arrays and declared structs are supported"}; return false;
    }
    if (pointers) { type = atri_ir_pointer_type(type); }
    if (type == ATRI_IR_INVALID) { *d = (AtriDiagnostic){span, "only pointers to u8, i32, char, arrays and structs are supported"}; return false; }
    *out = type; return true;
}
bool atri_type_from_source(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, AtriSpan span, AtriIRType *out, AtriDiagnostic *d) {
    size_t steps = 100000;
    if (!source_type(source, ast, p, span, out, d, 0, &steps)) { return false; }
    return layout_type(source, ast, p, *out, 0, span, d);
}
static bool layout(AtriSource source, const AtriProgram *ast, AtriIRProgram *p, size_t index, size_t depth, AtriDiagnostic *d) {
    AtriIRStruct *t = &p->structs[index];
    if (t->opaque) { *d = (AtriDiagnostic){t->name, "opaque struct has no complete object layout"}; return false; }
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
        size_t steps = 100000;
        if (!source_type(source, ast, p, ast->fields[id].type, &field->type, d, depth+1, &steps)) { return false; }
        if (!layout_type(source, ast, p, field->type, depth+1, ast->fields[id].type, d)) { return false; }
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
    if (ast->struct_count >= ATRI_IR_ARRAY-ATRI_IR_STRUCT) { *d = (AtriDiagnostic){{0}, "struct type table exceeds the prototype limit"}; return false; }
    p->struct_count = ast->struct_count; p->field_count = ast->field_count;
    p->structs = calloc(p->struct_count ? p->struct_count : 1, sizeof(*p->structs));
    p->fields = calloc(p->field_count ? p->field_count : 1, sizeof(*p->fields));
    if (!p->structs || !p->fields) { *d = (AtriDiagnostic){{0}, "out of memory"}; return false; }
    for (size_t i = 0; i < p->struct_count; ++i) {
        p->structs[i] = (AtriIRStruct){.name = ast->structs[i].name, .first_field = ast->structs[i].first_field, .field_count = ast->structs[i].field_count, .opaque = ast->structs[i].opaque, .alignment = 1};
    }
    for (size_t i = 0; i < p->struct_count; ++i) { if (!p->structs[i].opaque && !layout(source, ast, p, i, 0, d)) { return false; } }
    for (size_t i = 0; i < p->array_count; ++i) {
        if (!layout_type(source, ast, p, (AtriIRType)(ATRI_IR_ARRAY+i), 0, (AtriSpan){0}, d)) { return false; }
    }
    return true;
}
