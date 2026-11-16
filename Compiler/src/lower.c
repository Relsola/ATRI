#include "ir.h"
#include "array.h"

typedef struct { AtriSpan name; AtriId value; bool mutable; } Binding;
typedef struct {
    AtriSource source;
    const AtriProgram *ast;
    AtriIRProgram *ir;
    AtriIRFunction *function;
    Binding *bindings;
    size_t binding_count, binding_capacity, depth;
    AtriDiagnostic *diagnostic;
} Lower;
static bool error(Lower *l, AtriSpan span, const char *message) {
    *l->diagnostic = (AtriDiagnostic){span, message}; return false;
}
static bool same(Lower *l, AtriSpan a, AtriSpan b) {
    return a.end - a.start == b.end - b.start &&
           memcmp(l->source.text + a.start, l->source.text + b.start, a.end - a.start) == 0;
}
static bool is_i32(Lower *l, AtriSpan type) {
    return type.end - type.start == 3 && memcmp(l->source.text + type.start, "i32", 3) == 0;
}
static size_t lookup(Lower *l, AtriSpan name) {
    for (size_t i = 0; i < l->binding_count; ++i) if (same(l, name, l->bindings[i].name)) return i;
    return ATRI_NONE;
}
static bool bind(Lower *l, Binding binding) {
    if (lookup(l, binding.name) != ATRI_NONE) return error(l, binding.name, "duplicate local or parameter name");
    return atri_append((void **)&l->bindings, &l->binding_count, &l->binding_capacity,
                       sizeof(binding), &binding) || error(l, binding.name, "out of memory");
}
static bool emit(Lower *l, AtriIRInst inst, AtriId *id) {
    return atri_ir_append(l->function, inst, id) || error(l, inst.span, "out of memory while lowering IR");
}
static bool literal(Lower *l, AtriSpan text, uint32_t limit, uint32_t *out) {
    uint32_t value = 0;
    for (size_t i = text.start; i < text.end; ++i) {
        unsigned digit = (unsigned)(l->source.text[i] - '0');
        if (value > (limit - digit) / 10) return error(l, text, "integer literal is outside the i32 range");
        value = value * 10 + digit;
    }
    *out = value; return true;
}
static bool expression(Lower *l, AtriId id, AtriId *out) {
    if (++l->depth > 256) { --l->depth; return error(l, l->ast->expressions[id].span, "expression nesting limit exceeded"); }
    const AtriExpr *e = &l->ast->expressions[id];
    AtriIRInst inst = {.span = e->span, .type = ATRI_IR_I32};
    bool success = false;
    switch (e->kind) {
        case ATRI_EXPR_INTEGER:
            inst.kind = ATRI_IR_CONST;
            if (literal(l, e->text, INT32_MAX, &inst.immediate)) success = emit(l, inst, out);
            break;
        case ATRI_EXPR_NAME: {
            size_t binding = lookup(l, e->text);
            if (binding == ATRI_NONE) error(l, e->text, "unknown local name (locals must be declared before use)");
            else { *out = l->bindings[binding].value; success = true; }
            break;
        }
        case ATRI_EXPR_UNARY:
            inst.kind = ATRI_IR_UNARY; inst.op = ATRI_NEG;
            if (l->ast->expressions[e->left].kind == ATRI_EXPR_INTEGER) {
                inst.kind = ATRI_IR_CONST;
                if (!literal(l, l->ast->expressions[e->left].text, UINT32_C(2147483648), &inst.immediate)) break;
                inst.immediate = UINT32_C(0) - inst.immediate;
                success = emit(l, inst, out);
            } else if (expression(l, e->left, &inst.left)) success = emit(l, inst, out);
            break;
        case ATRI_EXPR_BINARY:
            inst.kind = ATRI_IR_BINARY; inst.op = e->op;
            if (expression(l, e->left, &inst.left) && expression(l, e->right, &inst.right)) success = emit(l, inst, out);
            break;
        case ATRI_EXPR_CALL: {
            if (lookup(l, e->text) != ATRI_NONE) { error(l, e->text, "local value is not callable"); break; }
            AtriId callee = ATRI_NONE;
            for (size_t i = 0; i < l->ast->function_count; ++i)
                if (same(l, e->text, l->ast->functions[i].name)) { callee = i; break; }
            if (callee == ATRI_NONE) { error(l, e->text, "unknown function name"); break; }
            if (e->argument_count != l->ir->functions[callee].parameter_count) {
                error(l, e->span, "function argument count does not match its signature"); break;
            }
            AtriId *arguments = e->argument_count == 0 ? NULL : malloc(e->argument_count * sizeof(*arguments));
            if (e->argument_count != 0 && arguments == NULL) { error(l, e->span, "out of memory"); break; }
            bool ok = true;
            for (size_t a = 0; a < e->argument_count; ++a)
                if (!expression(l, l->ast->arguments[e->first_argument + a], &arguments[a])) { ok = false; break; }
            inst.kind = ATRI_IR_CALL; inst.callee = callee;
            inst.first_argument = l->function->argument_count; inst.argument_count = e->argument_count;
            for (size_t a = 0; ok && a < e->argument_count; ++a)
                if (!atri_append((void **)&l->function->arguments, &l->function->argument_count,
                                  &l->function->argument_capacity, sizeof(arguments[a]), &arguments[a])) {
                    ok = error(l, e->span, "out of memory");
                }
            free(arguments);
            if (ok) success = emit(l, inst, out);
            break;
        }
    }
    --l->depth; return success;
}
bool atri_lower(AtriSource source, const AtriProgram *ast, AtriIRProgram *out, AtriDiagnostic *d) {
    *out = (AtriIRProgram){0}; *d = (AtriDiagnostic){0};
    AtriIRProgram ir = {.function_count = ast->function_count};
    ir.functions = calloc(ast->function_count == 0 ? 1 : ast->function_count, sizeof(*ir.functions));
    if (ir.functions == NULL) { *d = (AtriDiagnostic){{0, 0}, "out of memory"}; return false; }
    Lower l = {.source = source, .ast = ast, .ir = &ir, .diagnostic = d};
    /* Discover/check every signature before resolving any body. */
    for (size_t i = 0; i < ast->function_count; ++i) {
        const AtriFunction *f = &ast->functions[i];
        for (size_t j = 0; j < i; ++j)
            if (same(&l, f->name, ast->functions[j].name)) { error(&l, f->name, "duplicate function name"); goto failure; }
        if (!is_i32(&l, f->return_type)) { error(&l, f->return_type, "only i32 is supported by the executable subset"); goto failure; }
        for (size_t j = 0; j < f->parameter_count; ++j) {
            AtriParameter a = ast->parameters[f->first_parameter + j];
            if (!is_i32(&l, a.type)) { error(&l, a.type, "only i32 parameters are supported"); goto failure; }
        }
        ir.functions[i].name = f->name; ir.functions[i].parameter_count = f->parameter_count;
    }
    for (size_t i = 0; i < ast->function_count; ++i) {
        const AtriFunction *f = &ast->functions[i];
        l.function = &ir.functions[i]; l.binding_count = 0;
        for (size_t a = 0; a < f->parameter_count; ++a) {
            if (a > UINT32_MAX) { error(&l, f->name, "too many parameters"); goto failure; }
            AtriParameter p = ast->parameters[f->first_parameter + a];
            AtriId value;
            if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_PARAM, .span = p.name, .immediate = (uint32_t)a}, &value) ||
                !bind(&l, (Binding){p.name, value, false})) goto failure;
        }
        bool returned = false;
        size_t live_end = 0;
        for (size_t s = 0; s < f->statement_count; ++s) {
            const AtriStatement *st = &ast->statements[f->first_statement + s];
            /* Analyze unreachable statements too, but omit them from emitted IR. */
            AtriId value;
            if (st->kind == ATRI_STMT_CONST || st->kind == ATRI_STMT_LET) {
                if (!is_i32(&l, st->type)) { error(&l, st->type, "only i32 local bindings are supported"); goto failure; }
                if (lookup(&l, st->name) != ATRI_NONE) { error(&l, st->name, "duplicate local or parameter name"); goto failure; }
            }
            if (st->expression == ATRI_NONE) {
                if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_CONST, .span = st->span}, &value)) goto failure;
            } else if (!expression(&l, st->expression, &value)) goto failure;
            if (st->kind == ATRI_STMT_CONST || st->kind == ATRI_STMT_LET) {
                if (!bind(&l, (Binding){st->name, value, st->kind == ATRI_STMT_LET})) goto failure;
            } else if (st->kind == ATRI_STMT_ASSIGN) {
                size_t b = lookup(&l, st->name);
                if (b == ATRI_NONE) { error(&l, st->name, "assignment to an unknown name"); goto failure; }
                if (!l.bindings[b].mutable) { error(&l, st->name, "cannot assign to const or a parameter"); goto failure; }
                l.bindings[b].value = value;
            } else if (!returned) {
                AtriId unused;
                if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_RETURN, .span = st->span, .left = value}, &unused)) goto failure;
            }
            if (st->kind == ATRI_STMT_RETURN && !returned) {
                returned = true;
                live_end = l.function->instruction_count;
            }
        }
        if (!returned) { error(&l, f->name, "i32 function must return a value"); goto failure; }
        l.function->instruction_count = live_end;
    }
    free(l.bindings);
    if (!atri_ir_verify(&ir, d)) { atri_ir_destroy(&ir); return false; }
    *out = ir; return true;
failure:
    free(l.bindings); atri_ir_destroy(&ir); return false;
}
