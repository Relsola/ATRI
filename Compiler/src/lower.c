#include "ir.h"
#include "array.h"

typedef struct {
    AtriSpan name;
    AtriId value, address;
    AtriIRType type;
    bool mutable, constant;
    size_t scope;
} Binding;
typedef struct { AtriId stop, again; } Loop;
typedef struct {
    AtriSource source;
    const AtriProgram *ast;
    AtriIRProgram *ir;
    AtriIRFunction *function;
    const AtriFunction *ast_function;
    Binding *bindings;
    size_t binding_count, binding_capacity, depth;
    bool constant_context;
    bool memory, closed;
    size_t scope;
    Loop loops[256];
    size_t loop_count;
    AtriDiagnostic *diagnostic;
} Lower;
static bool error(Lower *l, AtriSpan span, const char *message) {
    *l->diagnostic = (AtriDiagnostic){span, message}; return false;
}
static bool same(Lower *l, AtriSpan a, AtriSpan b) {
    return a.end - a.start == b.end - b.start &&
           memcmp(l->source.text + a.start, l->source.text + b.start, a.end - a.start) == 0;
}
static bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
static bool type(Lower *l, AtriSpan span, AtriIRType *out) {
    size_t i = span.start, pointers = 0;
    while (i < span.end) {
        if (whitespace(l->source.text[i])) ++i;
        else if (l->source.text[i] == '*') { ++pointers; ++i; }
        else break;
    }
    bool i32 = span.end - i == 3 && memcmp(l->source.text + i, "i32", 3) == 0;
    bool i64 = span.end - i == 3 && memcmp(l->source.text + i, "i64", 3) == 0;
    bool boolean = span.end - i == 4 && memcmp(l->source.text + i, "bool", 4) == 0;
    if (!i32 && !i64 && !boolean)
        return error(l, span, "only i32, i64, bool and *i32 are supported by the executable subset");
    if (pointers > 1) return error(l, span, "only one-level integer pointers (*i32) are supported yet");
    if (pointers != 0 && !i32) return error(l, span, "only pointers to i32 are supported yet");
    *out = pointers != 0 ? ATRI_IR_PTR_I32 : boolean ? ATRI_IR_BOOL : i64 ? ATRI_IR_I64 : ATRI_IR_I32; return true;
}
static size_t lookup(Lower *l, AtriSpan name) {
    for (size_t i = l->binding_count; i-- > 0;) if (same(l, name, l->bindings[i].name)) return i;
    return ATRI_NONE;
}
static bool bind(Lower *l, Binding b) {
    size_t previous = lookup(l, b.name);
    if (previous != ATRI_NONE && l->bindings[previous].scope == l->scope) return error(l, b.name, "duplicate local or parameter name");
    b.scope = l->scope;
    return atri_append((void **)&l->bindings, &l->binding_count, &l->binding_capacity,
                       sizeof(b), &b) || error(l, b.name, "out of memory");
}
static bool emit(Lower *l, AtriIRInst inst, AtriId *id) {
    return atri_ir_append(l->function, inst, id) || error(l, inst.span, "out of memory while lowering IR");
}
static bool literal(Lower *l, AtriSpan text, uint64_t limit, uint64_t *out) {
    uint64_t value = 0;
    for (size_t i = text.start; i < text.end; ++i) {
        unsigned digit = (unsigned)(l->source.text[i] - '0');
        if (value > (limit - digit) / 10) return error(l, text, limit <= UINT32_MAX ?
            "integer literal is outside the i32 range" : "integer literal is outside the i64 range");
        value = value * 10 + digit;
    }
    *out = value; return true;
}
/* Conservative name-based prepass; shadowing may cause extra local storage. */
static bool address_taken(Lower *l, AtriSpan name) {
    for (size_t i = 0; i < l->ast->expression_count; ++i) {
        const AtriExpr *e = &l->ast->expressions[i];
        if (e->span.start < l->ast_function->body.start || e->span.end > l->ast_function->body.end ||
            e->kind != ATRI_EXPR_ADDRESS) continue;
        const AtriExpr *target = &l->ast->expressions[e->left];
        if (target->kind == ATRI_EXPR_NAME && same(l, target->text, name)) return true;
    }
    return false;
}
static bool expression(Lower *l, AtriId id, AtriIRType expected, AtriId *out);
static bool store(Lower *l, AtriId pointer, AtriId value, AtriSpan span);
static AtriId new_block(Lower *l) { return l->function->block_count++; }
static bool label(Lower *l, AtriId block, AtriSpan span) {
    AtriId ignored; l->closed = false;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_LABEL, .immediate = block, .span = span}, &ignored);
}
static bool jump(Lower *l, AtriId target, AtriSpan span) {
    if (l->closed) return true;
    AtriId ignored; l->closed = true;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_JUMP, .target = target, .span = span}, &ignored);
}
static bool branch(Lower *l, AtriId condition, AtriId yes, AtriId no, AtriSpan span) {
    AtriId ignored; l->closed = true;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_BRANCH, .left = condition, .target = yes, .alternative = no, .span = span}, &ignored);
}
static bool logical(Lower *l, const AtriExpr *e, AtriId *out) {
    AtriId a, b;
    if (l->constant_context) {
        return expression(l, e->left, ATRI_IR_BOOL, &a) && expression(l, e->right, ATRI_IR_BOOL, &b) &&
            emit(l, (AtriIRInst){.kind = ATRI_IR_LAZY_LOGICAL, .type = ATRI_IR_BOOL, .span = e->span,
                .op = e->op, .left = a, .right = b}, out);
    }
    AtriId local, initial;
    AtriId rhs = new_block(l), shortcut = new_block(l), end = new_block(l);
    if (!emit(l, (AtriIRInst){.kind = ATRI_IR_LOCAL, .type = ATRI_IR_BOOL, .span = e->span}, &local) ||
        !expression(l, e->left, ATRI_IR_BOOL, &a) ||
        !branch(l, a, e->op == ATRI_AND ? rhs : shortcut, e->op == ATRI_AND ? shortcut : rhs, e->span) ||
        !label(l, shortcut, e->span) ||
        !emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = ATRI_IR_BOOL, .span = e->span,
            .immediate = e->op == ATRI_OR}, &initial) || !store(l, local, initial, e->span) ||
        !jump(l, end, e->span) || !label(l, rhs, e->span) ||
        !expression(l, e->right, ATRI_IR_BOOL, &b) || !store(l, local, b, e->span) ||
        !jump(l, end, e->span) || !label(l, end, e->span)) return false;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_LOAD, .type = ATRI_IR_BOOL, .left = local, .span = e->span}, out);
}
/* Predict result types without emitting/reordering expressions. Literals need context. */
static AtriIRType infer_type(Lower *l, AtriId id, AtriIRType hint) {
    const AtriExpr *e = &l->ast->expressions[id];
    switch (e->kind) {
        case ATRI_EXPR_BOOL: return ATRI_IR_BOOL;
        case ATRI_EXPR_INTEGER: return atri_ir_is_integer(hint) ? hint : ATRI_IR_INVALID;
        case ATRI_EXPR_NULL: case ATRI_EXPR_ADDRESS: return ATRI_IR_PTR_I32;
        case ATRI_EXPR_DEREFERENCE: return ATRI_IR_I32;
        case ATRI_EXPR_NAME: {
            size_t b = lookup(l, e->text); return b == ATRI_NONE ? ATRI_IR_INVALID : l->bindings[b].type;
        }
        case ATRI_EXPR_CALL:
            for (size_t i = 0; i < l->ast->function_count; ++i)
                if (same(l, e->text, l->ast->functions[i].name)) return l->ir->functions[i].return_type;
            return ATRI_IR_INVALID;
        case ATRI_EXPR_UNARY: return e->op == ATRI_NOT ? ATRI_IR_BOOL : infer_type(l, e->left, hint);
        case ATRI_EXPR_BINARY: {
            if (e->op >= ATRI_EQ) return ATRI_IR_BOOL;
            AtriIRType a = infer_type(l, e->left, ATRI_IR_INVALID), b = infer_type(l, e->right, ATRI_IR_INVALID);
            if (a == ATRI_IR_PTR_I32 || b == ATRI_IR_PTR_I32)
                return e->op == ATRI_SUB && a == ATRI_IR_PTR_I32 && b == ATRI_IR_PTR_I32 ? ATRI_IR_I64 : ATRI_IR_PTR_I32;
            if (a == ATRI_IR_I64 || b == ATRI_IR_I64) return ATRI_IR_I64;
            if (a == ATRI_IR_I32 || b == ATRI_IR_I32) return ATRI_IR_I32;
            return atri_ir_is_integer(hint) ? hint : ATRI_IR_INVALID;
        }
    }
    return ATRI_IR_INVALID;
}
static bool nonnull(Lower *l, AtriId pointer, AtriSpan span) {
    const AtriIRInst *v = &l->function->instructions[pointer];
    if (v->kind == ATRI_IR_CONST && v->immediate == 0)
        return error(l, span, "cannot dereference a statically known null pointer");
    return true;
}
static bool address(Lower *l, AtriId id, AtriId *out) {
    const AtriExpr *e = &l->ast->expressions[id];
    if (l->constant_context) return error(l, e->span, "compile-time pointer operations are not supported yet");
    if (e->kind == ATRI_EXPR_DEREFERENCE)
        return expression(l, e->left, ATRI_IR_PTR_I32, out);
    if (e->kind != ATRI_EXPR_NAME) return error(l, e->span, "address-of requires an addressable integer variable");
    size_t b = lookup(l, e->text);
    if (b == ATRI_NONE) return error(l, e->text, "unknown local name");
    Binding *binding = &l->bindings[b];
    if (!binding->mutable) return error(l, e->span, "cannot take the address of a compile-time constant or read-only parameter");
    if (binding->type == ATRI_IR_PTR_I32) return error(l, e->span, "taking the address of a pointer would require unsupported **i32");
    if (binding->type != ATRI_IR_I32) return error(l, e->span, "address-of currently supports only i32 variables");
    return emit(l, (AtriIRInst){.kind = ATRI_IR_ADDRESS, .type = ATRI_IR_PTR_I32,
        .left = binding->address, .span = e->span}, out);
}
static bool expression(Lower *l, AtriId id, AtriIRType expected, AtriId *out) {
    const AtriExpr *e = &l->ast->expressions[id];
    if (++l->depth > 256) { --l->depth; return error(l, e->span, "expression nesting limit exceeded"); }
    AtriIRInst inst = {.span = e->span, .type = ATRI_IR_I32};
    bool ok = false;
    switch (e->kind) {
        case ATRI_EXPR_BOOL:
            inst.kind = ATRI_IR_CONST; inst.type = ATRI_IR_BOOL; inst.immediate = e->boolean;
            ok = emit(l, inst, out); break;
        case ATRI_EXPR_INTEGER:
            inst.kind = ATRI_IR_CONST;
            inst.type = expected == ATRI_IR_I64 ? ATRI_IR_I64 : ATRI_IR_I32;
            if (literal(l, e->text, inst.type == ATRI_IR_I64 ? (uint64_t)INT64_MAX : INT32_MAX, &inst.immediate)) ok = emit(l, inst, out);
            break;
        case ATRI_EXPR_NULL:
            if (expected != ATRI_IR_PTR_I32) error(l, e->span, "null requires a pointer type context");
            else { inst.kind = ATRI_IR_CONST; inst.type = ATRI_IR_PTR_I32; ok = emit(l, inst, out); }
            break;
        case ATRI_EXPR_NAME: {
            size_t b = lookup(l, e->text);
            if (b == ATRI_NONE) error(l, e->text, "unknown local name (locals must be declared before use)");
            else if (l->constant_context && !l->bindings[b].constant)
                error(l, e->text, "compile-time constant initializer cannot read a runtime variable or parameter");
            else if (l->bindings[b].address != ATRI_NONE) {
                inst.kind = ATRI_IR_LOAD; inst.type = l->bindings[b].type; inst.left = l->bindings[b].address; ok = emit(l, inst, out);
            } else { *out = l->bindings[b].value; ok = true; }
            break;
        }
        case ATRI_EXPR_ADDRESS: ok = address(l, e->left, out); break;
        case ATRI_EXPR_DEREFERENCE:
            inst.kind = ATRI_IR_LOAD;
            if (expression(l, e->left, ATRI_IR_PTR_I32, &inst.left) && nonnull(l, inst.left, e->span))
                ok = emit(l, inst, out);
            break;
        case ATRI_EXPR_UNARY:
            if (e->op == ATRI_NOT) {
                inst.kind = ATRI_IR_UNARY; inst.type = ATRI_IR_BOOL; inst.op = ATRI_NOT;
                if (expression(l, e->left, ATRI_IR_BOOL, &inst.left)) ok = emit(l, inst, out);
                break;
            }
            inst.kind = ATRI_IR_UNARY; inst.op = ATRI_NEG;
            inst.type = infer_type(l, e->left, expected);
            if (!atri_ir_is_integer(inst.type)) inst.type = ATRI_IR_I32;
            if (e->op == ATRI_ADD) { ok = expression(l, e->left, inst.type, out); break; }
            if (l->ast->expressions[e->left].kind == ATRI_EXPR_INTEGER) {
                inst.kind = ATRI_IR_CONST;
                if (!literal(l, l->ast->expressions[e->left].text, inst.type == ATRI_IR_I64 ? (UINT64_C(1) << 63) : UINT32_C(2147483648), &inst.immediate)) break;
                uint64_t negated;
                if (!atri_integer_compute(inst.type, ATRI_NEG, inst.immediate, 0, &negated)) break;
                inst.immediate = negated; ok = emit(l, inst, out);
            } else if (expression(l, e->left, inst.type, &inst.left)) ok = emit(l, inst, out);
            break;
        case ATRI_EXPR_BINARY: {
            if (e->op == ATRI_AND || e->op == ATRI_OR) { ok = logical(l, e, out); break; }
            AtriIRType a = infer_type(l, e->left, ATRI_IR_INVALID), b = infer_type(l, e->right, ATRI_IR_INVALID);
            if (e->op >= ATRI_EQ && e->op <= ATRI_GE) {
                AtriIRType operand_type = a != ATRI_IR_INVALID ? a : b != ATRI_IR_INVALID ? b : ATRI_IR_I32;
                if (!atri_ir_is_integer(operand_type) && e->op != ATRI_EQ && e->op != ATRI_NE) {
                    error(l, e->span, "ordered comparisons require integer operands"); break;
                }
                inst.kind = ATRI_IR_COMPARE; inst.type = ATRI_IR_BOOL; inst.op = e->op;
                if (expression(l, e->left, operand_type, &inst.left) &&
                    expression(l, e->right, operand_type, &inst.right)) ok = emit(l, inst, out);
                break;
            }
            if (a == ATRI_IR_PTR_I32 || b == ATRI_IR_PTR_I32) {
                if (e->op != ATRI_ADD && e->op != ATRI_SUB) {
                    error(l, e->span, "pointer arithmetic supports only pointer +/- integer and pointer - pointer"); break;
                }
                if (e->op == ATRI_SUB && a != ATRI_IR_PTR_I32) {
                    error(l, e->span, "cannot subtract a pointer from an integer"); break;
                }
                if (a == ATRI_IR_PTR_I32 && b == ATRI_IR_PTR_I32) {
                    if (e->op != ATRI_SUB) { error(l, e->span, "cannot add two pointers"); break; }
                    inst.kind = ATRI_IR_PTR_DIFF; inst.type = ATRI_IR_I64;
                    if (expression(l, e->left, ATRI_IR_PTR_I32, &inst.left) &&
                        expression(l, e->right, ATRI_IR_PTR_I32, &inst.right) &&
                        nonnull(l, inst.left, e->span) && nonnull(l, inst.right, e->span)) ok = emit(l, inst, out);
                } else {
                    AtriIRType offset_type = a == ATRI_IR_PTR_I32 ? b : a;
                    if (!atri_ir_is_integer(offset_type)) offset_type = ATRI_IR_I64;
                    AtriId left, right;
                    /* Preserve left-to-right evaluation even when canonicalizing integer + pointer. */
                    if (!expression(l, e->left, a == ATRI_IR_PTR_I32 ? ATRI_IR_PTR_I32 : offset_type, &left) ||
                        !expression(l, e->right, b == ATRI_IR_PTR_I32 ? ATRI_IR_PTR_I32 : offset_type, &right)) break;
                    inst.kind = ATRI_IR_PTR_OFFSET; inst.type = ATRI_IR_PTR_I32; inst.op = e->op;
                    inst.left = a == ATRI_IR_PTR_I32 ? left : right; inst.right = a == ATRI_IR_PTR_I32 ? right : left;
                    if (!nonnull(l, inst.left, e->span)) break;
                    const AtriIRInst *offset = &l->function->instructions[inst.right];
                    int64_t amount = offset->type == ATRI_IR_I64 ? atri_i64_signed(offset->immediate) : atri_i32_signed((uint32_t)offset->immediate);
                    if (offset->kind == ATRI_IR_CONST && (amount < INT64_MIN / 4 || amount > INT64_MAX / 4)) {
                        error(l, e->span, "pointer offset times element size exceeds the signed 64-bit range"); break;
                    }
                    ok = emit(l, inst, out);
                }
            } else {
                inst.kind = ATRI_IR_BINARY; inst.op = e->op;
                inst.type = infer_type(l, id, expected);
                if (!atri_ir_is_integer(inst.type)) inst.type = ATRI_IR_I32;
                if (expression(l, e->left, inst.type, &inst.left) && expression(l, e->right, inst.type, &inst.right)) ok = emit(l, inst, out);
            }
            break;
        }
        case ATRI_EXPR_CALL: {
            if (lookup(l, e->text) != ATRI_NONE) { error(l, e->text, "local value is not callable"); break; }
            AtriId callee = ATRI_NONE;
            for (size_t i = 0; i < l->ast->function_count; ++i)
                if (same(l, e->text, l->ast->functions[i].name)) { callee = i; break; }
            if (callee == ATRI_NONE) { error(l, e->text, "unknown function name"); break; }
            const AtriIRFunction *f = &l->ir->functions[callee];
            if (e->argument_count != f->parameter_count) { error(l, e->span, "function argument count does not match its signature"); break; }
            AtriId *args = e->argument_count == 0 ? NULL : malloc(e->argument_count * sizeof(*args));
            if (e->argument_count != 0 && args == NULL) { error(l, e->span, "out of memory"); break; }
            ok = true;
            for (size_t a = 0; a < e->argument_count; ++a)
                if (!expression(l, l->ast->arguments[e->first_argument + a], f->parameter_types[a], &args[a])) { ok = false; break; }
            inst.kind = ATRI_IR_CALL; inst.callee = callee; inst.type = f->return_type;
            inst.first_argument = l->function->argument_count; inst.argument_count = e->argument_count;
            for (size_t a = 0; ok && a < e->argument_count; ++a)
                if (!atri_append((void **)&l->function->arguments, &l->function->argument_count,
                                  &l->function->argument_capacity, sizeof(args[a]), &args[a])) ok = error(l, e->span, "out of memory");
            free(args);
            if (ok) ok = emit(l, inst, out);
            break;
        }
    }
    if (ok && expected != ATRI_IR_INVALID && l->function->instructions[*out].type != expected)
        ok = error(l, e->span, "type mismatch: i32, i64, bool and *i32 do not implicitly convert");
    --l->depth; return ok;
}
static bool store(Lower *l, AtriId pointer, AtriId value, AtriSpan span) {
    AtriId ignored;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_STORE, .span = span,
        .left = pointer, .right = value, .type = l->function->instructions[value].type}, &ignored);
}
static bool statement(Lower *l, AtriId id);
static bool block(Lower *l, AtriId id, bool nested) {
    size_t saved = l->binding_count;
    if (nested) ++l->scope;
    const AtriBlock *b = &l->ast->blocks[id];
    for (size_t i = 0; i < b->statement_count; ++i) {
        if (l->closed && !label(l, new_block(l), b->span)) return false;
        if (!statement(l, l->ast->block_statements[b->first_statement+i])) return false;
    }
    if (nested) { l->binding_count = saved; --l->scope; }
    return true;
}
static bool simple(Lower *l, const AtriStatement *s) {
    if (s->kind == ATRI_STMT_EXPR) { AtriId ignored; return expression(l, s->expression, ATRI_IR_INVALID, &ignored); }
    AtriIRType expected;
    AtriId destination = ATRI_NONE, binding = ATRI_NONE;
    bool declaration = s->kind == ATRI_STMT_VARIABLE || s->kind == ATRI_STMT_CONSTANT;
    if (declaration) {
        if (!type(l, s->type, &expected)) return false;
        if (s->kind == ATRI_STMT_CONSTANT && expected == ATRI_IR_PTR_I32)
            return error(l, s->span, "compile-time pointer constants are not supported yet");
    } else if (s->kind == ATRI_STMT_RETURN) expected = l->function->return_type;
    else {
        const AtriExpr *target = &l->ast->expressions[s->target];
        if (target->kind == ATRI_EXPR_NAME) {
            binding = lookup(l, target->text);
            if (binding == ATRI_NONE) return error(l, target->span, "assignment to unknown name");
            if (!l->bindings[binding].mutable) return error(l, target->span, "cannot assign to a compile-time constant or read-only parameter");
            expected = l->bindings[binding].type; destination = l->bindings[binding].address;
        } else if (target->kind == ATRI_EXPR_DEREFERENCE) {
            expected = ATRI_IR_I32;
            if (!expression(l, target->left, ATRI_IR_PTR_I32, &destination) || !nonnull(l, destination, target->span)) return false;
        } else return error(l, target->span, "assignment requires a mutable local or a dereferenced pointer");
    }
    size_t first = l->function->instruction_count;
    AtriId value;
    l->constant_context = s->kind == ATRI_STMT_CONSTANT;
    if (s->expression == ATRI_NONE) {
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .span = s->span, .type = expected}, &value)) return false;
    } else if (!expression(l, s->expression, expected, &value)) return false;
    l->constant_context = false;
    if (s->kind == ATRI_STMT_CONSTANT) {
        AtriId result;
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .span = s->span, .type = expected}, &result)) return false;
        AtriIRConstant c = {.span = s->span, .expression = value, .result = result, .first_instruction = first};
        if (!atri_append((void **)&l->function->constants, &l->function->constant_count,
                          &l->function->constant_capacity, sizeof(c), &c)) return error(l, s->span, "out of memory");
        value = result;
    }
    if (declaration) {
        AtriId local = ATRI_NONE;
        if (s->kind == ATRI_STMT_VARIABLE && (l->memory || (expected == ATRI_IR_I32 && address_taken(l, s->name)))) {
            if (!emit(l, (AtriIRInst){.kind = ATRI_IR_LOCAL, .type = expected, .span = s->span}, &local) ||
                !store(l, local, value, s->span)) return false;
        }
        return bind(l, (Binding){.name = s->name, .value = value, .address = local, .type = expected,
            .mutable = s->kind == ATRI_STMT_VARIABLE, .constant = s->kind == ATRI_STMT_CONSTANT});
    }
    if (s->kind == ATRI_STMT_ASSIGN) {
        if (destination != ATRI_NONE) return store(l, destination, value, s->span);
        l->bindings[binding].value = value; return true;
    }
    if (expected == ATRI_IR_PTR_I32 && l->function->instructions[value].kind == ATRI_IR_ADDRESS)
        return error(l, s->span, "cannot directly return the address of a local variable");
    AtriId ignored; l->closed = true;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_RETURN, .span = s->span, .type = expected, .left = value}, &ignored);
}
static bool statement(Lower *l, AtriId id) {
    const AtriStatement *s = &l->ast->statements[id];
    if (l->function->block_count > 4090) return error(l, s->span, "control flow exceeds the prototype limit of 4096 blocks");
    switch (s->kind) {
        case ATRI_STMT_BLOCK: return block(l, s->body, true);
        case ATRI_STMT_BREAK: case ATRI_STMT_CONTINUE:
            if (!l->loop_count) return error(l, s->span, "break and continue require an enclosing loop");
            return jump(l, s->kind == ATRI_STMT_BREAK ? l->loops[l->loop_count-1].stop : l->loops[l->loop_count-1].again, s->span);
        case ATRI_STMT_IF: {
            AtriId cond, yes = new_block(l), no = new_block(l), end = new_block(l);
            if (!expression(l, s->expression, ATRI_IR_BOOL, &cond) || !branch(l, cond, yes, no, s->span) ||
                !label(l, yes, s->span) || !block(l, s->body, true) || !jump(l, end, s->span) ||
                !label(l, no, s->span)) return false;
            if (s->otherwise != ATRI_NONE && !statement(l, s->otherwise)) return false;
            return jump(l, end, s->span) && label(l, end, s->span);
        }
        case ATRI_STMT_WHILE: case ATRI_STMT_DO: case ATRI_STMT_FOR: case ATRI_STMT_LOOP: {
            size_t saved = l->binding_count; ++l->scope;
            if (s->initializer != ATRI_NONE && !statement(l, s->initializer)) return false;
            AtriId cond_block = new_block(l), body = new_block(l), step_block = new_block(l), end = new_block(l);
            if (!jump(l, s->kind == ATRI_STMT_DO ? body : cond_block, s->span) || !label(l, cond_block, s->span)) return false;
            if (s->kind == ATRI_STMT_LOOP || s->expression == ATRI_NONE) {
                if (!jump(l, body, s->span)) return false;
            } else {
                AtriId cond;
                if (!expression(l, s->expression, ATRI_IR_BOOL, &cond) || !branch(l, cond, body, end, s->span)) return false;
            }
            if (!label(l, body, s->span)) return false;
            if (l->loop_count == 256) return error(l, s->span, "loop nesting limit exceeded");
            l->loops[l->loop_count++] = (Loop){end, s->kind == ATRI_STMT_FOR ? step_block : cond_block};
            if (!block(l, s->body, true)) return false;
            --l->loop_count;
            if (!jump(l, step_block, s->span) || !label(l, step_block, s->span)) return false;
            if (s->step != ATRI_NONE && !statement(l, s->step)) return false;
            if (!jump(l, cond_block, s->span) || !label(l, end, s->span)) return false;
            l->binding_count = saved; --l->scope; return true;
        }
        default: return simple(l, s);
    }
}
static bool needs_memory(const AtriProgram *ast, const AtriFunction *f) {
    for (size_t i = f->first_statement; i < f->first_statement+f->statement_count; ++i)
        if (ast->statements[i].kind >= ATRI_STMT_BLOCK) return true;
    for (size_t i = 0; i < ast->expression_count; ++i) {
        const AtriExpr *e = &ast->expressions[i];
        if (e->span.start >= f->body.start && e->span.end <= f->body.end &&
            e->kind == ATRI_EXPR_BINARY && (e->op == ATRI_AND || e->op == ATRI_OR)) return true;
    }
    return false;
}
/* Source is fully checked first; discard blocks proven unreachable by literal branches. */
static bool prune(AtriIRFunction *f, AtriDiagnostic *d) {
    size_t count = f->block_count;
    bool *reachable = calloc(count, sizeof(*reachable));
    AtriId *map = malloc(count * sizeof(*map));
    if (!reachable || !map) { free(reachable); free(map); *d = (AtriDiagnostic){f->name, "out of memory"}; return false; }
    reachable[0] = true;
    bool changed;
    do {
        changed = false; AtriId block = 0;
        for (size_t i = 0; i < f->instruction_count; ++i) {
            AtriIRInst *v = &f->instructions[i];
            if (v->kind == ATRI_IR_LABEL) block = (size_t)v->immediate;
            if (!reachable[block]) continue;
            if (v->kind == ATRI_IR_BRANCH && f->instructions[v->left].kind == ATRI_IR_CONST) {
                v->target = f->instructions[v->left].immediate ? v->target : v->alternative;
                v->kind = ATRI_IR_JUMP;
            }
            if ((v->kind == ATRI_IR_JUMP || v->kind == ATRI_IR_BRANCH) && !reachable[v->target]) { reachable[v->target] = true; changed = true; }
            if (v->kind == ATRI_IR_BRANCH && !reachable[v->alternative]) { reachable[v->alternative] = true; changed = true; }
        }
    } while (changed);
    size_t next = 0;
    for (size_t b = 0; b < count; ++b) map[b] = reachable[b] ? next++ : ATRI_NONE;
    AtriId block = 0;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        AtriIRInst *v = &f->instructions[i];
        if (v->kind == ATRI_IR_LABEL) block = (size_t)v->immediate;
        if (!reachable[block]) { v->kind = ATRI_IR_NOP; continue; }
        if (v->kind == ATRI_IR_LABEL) v->immediate = map[block];
        if (v->kind == ATRI_IR_JUMP || v->kind == ATRI_IR_BRANCH) v->target = map[v->target];
        if (v->kind == ATRI_IR_BRANCH) v->alternative = map[v->alternative];
    }
    f->block_count = next;
    while (f->instruction_count && f->instructions[f->instruction_count-1].kind == ATRI_IR_NOP) --f->instruction_count;
    free(reachable); free(map); return true;
}
bool atri_lower(AtriSource source, const AtriProgram *ast, AtriIRProgram *out, AtriDiagnostic *d) {
    *out = (AtriIRProgram){0}; *d = (AtriDiagnostic){0};
    AtriIRProgram ir = {.function_count = ast->function_count};
    ir.functions = calloc(ast->function_count ? ast->function_count : 1, sizeof(*ir.functions));
    if (!ir.functions) { *d = (AtriDiagnostic){{0,0}, "out of memory"}; return false; }
    Lower l = {.source = source, .ast = ast, .ir = &ir, .diagnostic = d};
    for (size_t i = 0; i < ast->function_count; ++i) {
        const AtriFunction *f = &ast->functions[i];
        for (size_t j = 0; j < i; ++j)
            if (same(&l, f->name, ast->functions[j].name)) { error(&l, f->name, "duplicate function name"); goto failure; }
        AtriIRFunction *signature = &ir.functions[i];
        signature->name = f->name; signature->parameter_count = f->parameter_count; signature->block_count = 1;
        if (!type(&l, f->return_type, &signature->return_type)) goto failure;
        signature->parameter_types = calloc(f->parameter_count ? f->parameter_count : 1, sizeof(*signature->parameter_types));
        if (!signature->parameter_types) { error(&l, f->name, "out of memory"); goto failure; }
        for (size_t a = 0; a < f->parameter_count; ++a)
            if (!type(&l, ast->parameters[f->first_parameter+a].type, &signature->parameter_types[a])) goto failure;
    }
    for (size_t i = 0; i < ast->function_count; ++i) {
        const AtriFunction *f = &ast->functions[i];
        l.function = &ir.functions[i]; l.ast_function = f; l.binding_count = 0; l.scope = 0;
        l.closed = false; l.memory = needs_memory(ast, f); l.loop_count = 0;
        for (size_t a = 0; a < f->parameter_count; ++a) {
            AtriParameter p = ast->parameters[f->first_parameter+a];
            AtriId value; AtriIRType t = l.function->parameter_types[a];
            if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_PARAM, .type = t, .span = p.name, .immediate = a}, &value) ||
                !bind(&l, (Binding){.name = p.name, .value = value, .type = t, .address = ATRI_NONE})) goto failure;
        }
        if (!block(&l, f->block, false)) goto failure;
        if (!l.closed) {
            AtriId ignored;
            if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_UNREACHABLE, .span = f->body}, &ignored)) goto failure;
        }
    }
    free(l.bindings);
    if (!atri_resolve_constants(&ir, d) || !atri_ir_verify(&ir, d)) { atri_ir_destroy(&ir); return false; }
    for (size_t i = 0; i < ir.function_count; ++i)
        if (!prune(&ir.functions[i], d)) { atri_ir_destroy(&ir); return false; }
    if (!atri_ir_verify(&ir, d)) { atri_ir_destroy(&ir); return false; }
    *out = ir; return true;
failure:
    free(l.bindings); atri_ir_destroy(&ir); return false;
}
