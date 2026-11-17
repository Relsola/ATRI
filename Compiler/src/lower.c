#include "ir.h"
#include "array.h"

typedef struct {
    AtriSpan name;
    /* address != ATRI_NONE 时是 IR local 的 ID；名称读写也必须经由它，才能观察指针别名修改。 */
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
    bool memory, closed; /* memory 为可变局部分配存储；closed 表示当前块已有终结指令。 */
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
static bool type(Lower *l, AtriSpan span, AtriIRType *out) {
    return atri_type_from_source(l->source, l->ast, l->ir, span, out, l->diagnostic);
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
static unsigned hex_value(unsigned char c) {
    return c <= '9' ? (unsigned)(c-'0') : c <= 'F' ? (unsigned)(c-'A'+10) : (unsigned)(c-'a'+10);
}
static bool string_literal(Lower *l, const AtriExpr *e, AtriId *out) {
    AtriIRString string = {.bytes = malloc(e->text.end-e->text.start)};
    if (!string.bytes) {
        return error(l, e->span, "out of memory while decoding string literal");
    }
    /* Lexer 已验证转义；保留普通 UTF-8 字节，IR 仅引用数据索引，不把宿主地址当作目标指针。 */
    for (size_t i = e->text.start+1; i < e->text.end-1; ++i) {
        unsigned char byte = (unsigned char)l->source.text[i];
        if (byte == '\\') {
            byte = (unsigned char)l->source.text[++i];
            switch (byte) {
                case '0': byte = 0; break;
                case 'n': byte = '\n'; break;
                case 'r': byte = '\r'; break;
                case 't': byte = '\t'; break;
                case 'a': byte = '\a'; break;
                case 'b': byte = '\b'; break;
                case 'f': byte = '\f'; break;
                case 'v': byte = '\v'; break;
                case 'x':
                    byte = (unsigned char)(hex_value((unsigned char)l->source.text[i+1])*16 +
                        hex_value((unsigned char)l->source.text[i+2]));
                    i += 2; break;
                default: break;
            }
        }
        string.bytes[string.length++] = byte;
    }
    string.bytes[string.length] = 0;
    size_t index = l->ir->string_count;
    if (!atri_append((void **)&l->ir->strings, &l->ir->string_count, &l->ir->string_capacity, sizeof(string), &string)) {
        free(string.bytes); return error(l, e->span, "out of memory while storing string literal");
    }
    return emit(l, (AtriIRInst){.kind = ATRI_IR_STRING, .type = ATRI_IR_PTR_CHAR, .span = e->span, .immediate = index}, out);
}
/* 按名称保守预扫取地址；同名遮蔽可能多分配存储，但不能漏掉真正需要稳定地址的变量。 */
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
static AtriId resolve_declaration(Lower *l, const AtriExpr *e, bool want_global) {
    bool global = false;
    AtriId id = atri_find_declaration(l->source, l->ast, l->ast_function->name.start, e->text, e->qualifier, &global);
    return id != ATRI_NONE && global == want_global ? id : ATRI_NONE;
}
static bool global_location(Lower *l, AtriId global, AtriSpan span, AtriId *out) {
    return emit(l, (AtriIRInst){.kind = ATRI_IR_LOCAL, .type = l->ir->globals[global].type,
        .span = span, .immediate = global+1}, out);
}
static bool expression(Lower *l, AtriId id, AtriIRType expected, AtriId *out);
static bool store(Lower *l, AtriId pointer, AtriId value, AtriSpan span);
static bool nonnull(Lower *l, AtriId pointer, AtriSpan span);
static AtriIRType infer_type(Lower *l, AtriId id, AtriIRType hint);
static AtriId field_index(Lower *l, AtriIRType type, AtriSpan name) {
    if (!atri_ir_is_struct(type)) { return ATRI_NONE; }
    const AtriIRStruct *t = &l->ir->structs[type-ATRI_IR_STRUCT];
    for (size_t i = 0; i < t->field_count; ++i) {
        if (same(l, name, l->ir->fields[t->first_field+i].name)) { return t->first_field+i; }
    }
    return ATRI_NONE;
}
/* location 与值快照分开：字段赋值定位原对象，实参和返回的结构体值则复制内容。 */
static bool location(Lower *l, AtriId id, bool write, AtriIRType *type, AtriId *out) {
    const AtriExpr *e = &l->ast->expressions[id];
    if (e->kind == ATRI_EXPR_NAME) {
        size_t b = e->qualifier.end ? ATRI_NONE : lookup(l, e->text);
        if (b != ATRI_NONE) {
            Binding *binding = &l->bindings[b]; *type = binding->type;
            if (l->constant_context && !binding->constant) {
                return error(l, e->span, "compile-time expression cannot read a runtime variable or parameter");
            }
            if (write && !binding->mutable) { return error(l, e->span, "cannot modify a read-only parameter or compile-time constant"); }
            if (binding->address != ATRI_NONE) { *out = binding->address; return true; }
            if (!write && atri_ir_is_aggregate(*type)) { *out = binding->value; return true; }
            return error(l, e->span, "expression has no addressable storage");
        }
        AtriId global = resolve_declaration(l, e, true);
        if (global != ATRI_NONE && !l->ir->globals[global].constant) {
            *type = l->ir->globals[global].type; return global_location(l, global, e->span, out);
        }
        return error(l, e->span, "unknown or non-addressable object");
    }
    if (e->kind == ATRI_EXPR_INDEX) {
        AtriIRType base_type = infer_type(l, e->left, ATRI_IR_INVALID);
        if (!atri_ir_is_array(base_type)) { return error(l, e->span, "indexing requires a fixed array object"); }
        AtriId base, index;
        const AtriExpr *parent = &l->ast->expressions[e->left];
        if (parent->kind == ATRI_EXPR_NAME || parent->kind == ATRI_EXPR_FIELD ||
            parent->kind == ATRI_EXPR_INDEX || parent->kind == ATRI_EXPR_DEREFERENCE) {
            if (!location(l, e->left, write, &base_type, &base)) { return false; }
        } else {
            if (write) { return error(l, e->span, "cannot modify an element of a temporary array"); }
            if (!expression(l, e->left, base_type, &base)) { return false; }
        }
        AtriIRArray array = l->ir->arrays[base_type-ATRI_IR_ARRAY];
        AtriIRType index_type = infer_type(l, e->right, ATRI_IR_I64);
        if (index_type != ATRI_IR_I32 && index_type != ATRI_IR_I64) { return error(l, e->span, "array index requires i32 or i64"); }
        if (!expression(l, e->right, index_type, &index)) { return false; }
        *type = array.element;
        return emit(l, (AtriIRInst){.kind = ATRI_IR_INDEX, .type = *type, .left = base, .right = index, .span = e->span}, out);
    }
    if (e->kind == ATRI_EXPR_DEREFERENCE) {
        AtriIRType pointer = infer_type(l, e->left, ATRI_IR_INVALID);
        *type = atri_ir_pointee(pointer);
        if (*type == ATRI_IR_INVALID) { return error(l, e->span, "type mismatch: dereference requires an i32 or struct pointer"); }
        return expression(l, e->left, pointer, out) && nonnull(l, *out, e->span);
    }
    if (e->kind == ATRI_EXPR_FIELD) {
        if (!atri_ir_is_struct(infer_type(l, e->left, ATRI_IR_INVALID))) {
            return error(l, e->span, "field access requires a struct object; struct pointers require explicit dereference");
        }
        AtriIRType base_type; AtriId base;
        const AtriExpr *parent = &l->ast->expressions[e->left];
        if (parent->kind == ATRI_EXPR_NAME || parent->kind == ATRI_EXPR_FIELD || parent->kind == ATRI_EXPR_DEREFERENCE || parent->kind == ATRI_EXPR_INDEX) {
            if (!location(l, e->left, write, &base_type, &base)) { return false; }
        } else {
            if (write) { return error(l, e->span, "cannot assign a field of a temporary value"); }
            if (!expression(l, e->left, ATRI_IR_INVALID, &base)) { return false; }
            base_type = l->function->instructions[base].type;
        }
        AtriId field = field_index(l, base_type, e->text);
        if (field == ATRI_NONE) { return error(l, e->text, "unknown field or non-struct object; struct pointers require explicit dereference"); }
        *type = l->ir->fields[field].type;
        return emit(l, (AtriIRInst){.kind = ATRI_IR_FIELD, .type = *type, .left = base, .immediate = field, .span = e->span}, out);
    }
    return error(l, e->span, "expression is not an addressable object");
}
static bool initialize_aggregate(Lower *l, const AtriExpr *e, AtriIRType type, AtriId *out) {
    if (atri_ir_is_array(type)) {
        if (l->constant_context) { return error(l, e->span, "compile-time array values are not supported yet"); }
        AtriIRArray array = l->ir->arrays[type-ATRI_IR_ARRAY];
        if (e->argument_count > array.length) { return error(l, e->span, "too many array initializer elements"); }
        AtriId object, zero;
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_LOCAL, .type = type, .span = e->span}, &object) ||
            !emit(l, (AtriIRInst){.kind = ATRI_IR_ZERO, .type = type, .span = e->span}, &zero) || !store(l, object, zero, e->span)) { return false; }
        for (size_t i = 0; i < array.length; ++i) {
            AtriId value, destination, index;
            if (i < e->argument_count) {
                const AtriInitializer *item = &l->ast->initializers[e->first_argument+i];
                if (item->name.end) { return error(l, item->name, "array initializer requires positional elements"); }
                if (!expression(l, item->expression, array.element, &value)) { return false; }
            } else {
                if (!atri_ir_is_aggregate(array.element)) { continue; }
                AtriExpr empty = {.kind = ATRI_EXPR_INITIALIZER, .span = e->span};
                if (!initialize_aggregate(l, &empty, array.element, &value)) { return false; }
            }
            if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = ATRI_IR_I64, .immediate = i, .span = e->span}, &index) ||
                !emit(l, (AtriIRInst){.kind = ATRI_IR_INDEX, .type = array.element, .left = object, .right = index, .span = e->span}, &destination) ||
                !store(l, destination, value, e->span)) { return false; }
        }
        return emit(l, (AtriIRInst){.kind = ATRI_IR_LOAD, .type = type, .left = object, .span = e->span}, out);
    }
    if (!atri_ir_is_struct(type)) { return error(l, e->span, "aggregate initializer requires an expected struct or array type"); }
    if (l->constant_context) { return error(l, e->span, "compile-time struct values are not supported yet"); }
    const AtriIRStruct *t = &l->ir->structs[type-ATRI_IR_STRUCT];
    AtriId object, zero;
    if (!emit(l, (AtriIRInst){.kind = ATRI_IR_LOCAL, .type = type, .span = e->span}, &object) ||
        !emit(l, (AtriIRInst){.kind = ATRI_IR_ZERO, .type = type, .span = e->span}, &zero) || !store(l, object, zero, e->span)) { return false; }
    /* 先检查重复/未知字段；随后显式项按源码顺序求值，省略字段应用常量默认值或递归默认初始化。 */
    for (size_t i = 0; i < e->argument_count; ++i) {
        const AtriInitializer *item = &l->ast->initializers[e->first_argument+i];
        if (!item->name.end) {
            return error(l, l->ast->expressions[item->expression].span, "struct initializer requires named fields");
        }
        if (field_index(l, type, item->name) == ATRI_NONE) { return error(l, item->name, "unknown struct initializer field"); }
        for (size_t j = 0; j < i; ++j) {
            if (same(l, item->name, l->ast->initializers[e->first_argument+j].name)) { return error(l, item->name, "duplicate struct initializer field"); }
        }
    }
    for (size_t i = 0; i < t->field_count; ++i) {
        size_t field = t->first_field+i; bool supplied = false;
        for (size_t j = 0; j < e->argument_count; ++j) {
            if (same(l, l->ir->fields[field].name, l->ast->initializers[e->first_argument+j].name)) { supplied = true; break; }
        }
        if (supplied) { continue; }
        AtriIRType ft = l->ir->fields[field].type;
        AtriId value, destination;
        if (atri_ir_is_aggregate(ft)) {
            AtriExpr empty = {.kind = ATRI_EXPR_INITIALIZER, .span = e->span};
            if (!initialize_aggregate(l, &empty, ft, &value)) { return false; }
        } else if (l->ast->fields[field].expression != ATRI_NONE) {
            if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CALL, .type = ft, .callee = l->ast->function_count+l->ast->global_count+field, .span = e->span}, &value)) { return false; }
        } else { continue; }
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_FIELD, .type = ft, .left = object, .immediate = field, .span = e->span}, &destination) || !store(l, destination, value, e->span)) { return false; }
    }
    for (size_t i = 0; i < e->argument_count; ++i) {
        const AtriInitializer *item = &l->ast->initializers[e->first_argument+i];
        size_t field = field_index(l, type, item->name);
        AtriIRType ft = l->ir->fields[field].type;
        AtriId value, destination;
        if (!expression(l, item->expression, ft, &value) ||
            !emit(l, (AtriIRInst){.kind = ATRI_IR_FIELD, .type = ft, .left = object, .immediate = field, .span = item->name}, &destination) || !store(l, destination, value, item->name)) { return false; }
    }
    return emit(l, (AtriIRInst){.kind = ATRI_IR_LOAD, .type = type, .left = object, .span = e->span}, out);
}

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
        /* 常量初始化随后会整段删除，不能在其中创建运行期基本块；私有指令负责求值时短路。 */
        return expression(l, e->left, ATRI_IR_BOOL, &a) && expression(l, e->right, ATRI_IR_BOOL, &b) &&
            emit(l, (AtriIRInst){.kind = ATRI_IR_LAZY_LOGICAL, .type = ATRI_IR_BOOL, .span = e->span,
                .op = e->op, .left = a, .right = b}, out);
    }
    /* 尚无 phi：两条路径把选中的 bool 写入同一临时对象，汇合后再读取；右侧仅在选中 RHS 路径时求值。 */
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
static bool conditional(Lower *l, const AtriExpr *e, AtriIRType expected, AtriId *out) {
    AtriIRType type = expected != ATRI_IR_INVALID ? expected : infer_type(l, e->right, ATRI_IR_INVALID);
    if (type == ATRI_IR_INVALID) { type = infer_type(l, e->alternative, ATRI_IR_I32); }
    AtriId condition, yes, no;
    if (!expression(l, e->left, ATRI_IR_BOOL, &condition)) { return false; }
    if (l->constant_context) {
        return expression(l, e->right, type, &yes) && expression(l, e->alternative, type, &no) &&
            emit(l, (AtriIRInst){.kind = ATRI_IR_LAZY_SELECT, .type = type, .left = condition, .right = yes, .callee = no, .span = e->span}, out);
    }
    AtriId local, yes_block = new_block(l), no_block = new_block(l), end = new_block(l);
    return emit(l, (AtriIRInst){.kind = ATRI_IR_LOCAL, .type = type, .span = e->span}, &local) &&
        branch(l, condition, yes_block, no_block, e->span) && label(l, yes_block, e->span) &&
        expression(l, e->right, type, &yes) && store(l, local, yes, e->span) && jump(l, end, e->span) &&
        label(l, no_block, e->span) && expression(l, e->alternative, type, &no) && store(l, local, no, e->span) &&
        jump(l, end, e->span) && label(l, end, e->span) &&
        emit(l, (AtriIRInst){.kind = ATRI_IR_LOAD, .type = type, .left = local, .span = e->span}, out);
}
static bool run_expression(Lower *l, const AtriExpr *e, AtriIRType expected, AtriId *out) {
    if (l->constant_context) { return expression(l, e->left, expected, out); }
    size_t first = l->function->instruction_count;
    l->constant_context = true;
    AtriId value, result;
    bool ok = expression(l, e->left, expected, &value);
    l->constant_context = false;
    if (!ok) { return false; }
    AtriIRType type = l->function->instructions[value].type;
    if (!atri_ir_is_integer(type) && type != ATRI_IR_BOOL) { return error(l, e->span, "run currently requires an integer or bool result"); }
    if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = type, .span = e->span}, &result)) { return false; }
    AtriIRConstant obligation = {.span = e->span, .expression = value, .result = result, .first_instruction = first};
    if (!atri_append((void **)&l->function->constants, &l->function->constant_count,
        &l->function->constant_capacity, sizeof(obligation), &obligation)) { return error(l, e->span, "out of memory"); }
    *out = result; return true;
}
/* 仅预测类型，不生成指令或提前执行实参，避免改变副作用顺序；无上下文的字面量暂不定型。 */
static AtriIRType infer_type(Lower *l, AtriId id, AtriIRType hint) {
    const AtriExpr *e = &l->ast->expressions[id];
    switch (e->kind) {
        case ATRI_EXPR_RUN: return infer_type(l, e->left, hint);
        case ATRI_EXPR_CONDITIONAL: {
            AtriIRType yes = infer_type(l, e->right, hint);
            return yes != ATRI_IR_INVALID ? yes : infer_type(l, e->alternative, hint);
        }
        case ATRI_EXPR_INDEX: {
            AtriIRType array = infer_type(l, e->left, ATRI_IR_INVALID);
            return atri_ir_is_array(array) ? l->ir->arrays[array-ATRI_IR_ARRAY].element : ATRI_IR_INVALID;
        }
        case ATRI_EXPR_BOOL: return ATRI_IR_BOOL;
        case ATRI_EXPR_INTEGER: return atri_ir_is_integer(hint) ? hint : ATRI_IR_INVALID;
        case ATRI_EXPR_STRING: return ATRI_IR_PTR_CHAR;
        case ATRI_EXPR_NULL: return atri_ir_is_pointer(hint) ? hint : ATRI_IR_INVALID;
        case ATRI_EXPR_ADDRESS: return atri_ir_pointer_type(infer_type(l, e->left, ATRI_IR_INVALID));
        case ATRI_EXPR_DEREFERENCE: return atri_ir_pointee(infer_type(l, e->left, ATRI_IR_INVALID));
        case ATRI_EXPR_INITIALIZER: return atri_ir_is_aggregate(hint) ? hint : ATRI_IR_INVALID;
        case ATRI_EXPR_FIELD: {
            AtriId field = field_index(l, infer_type(l, e->left, ATRI_IR_INVALID), e->text);
            return field == ATRI_NONE ? ATRI_IR_INVALID : l->ir->fields[field].type;
        }
        case ATRI_EXPR_NAME: {
            size_t b = e->qualifier.end ? ATRI_NONE : lookup(l, e->text);
            AtriId global = resolve_declaration(l, e, true);
            return b != ATRI_NONE ? l->bindings[b].type : global != ATRI_NONE ? l->ir->globals[global].type : ATRI_IR_INVALID;
        }
        case ATRI_EXPR_CALL: {
            AtriId callee = resolve_declaration(l, e, false);
            return callee == ATRI_NONE ? ATRI_IR_INVALID : l->ir->functions[callee].return_type;
        }
        case ATRI_EXPR_UNARY: return e->op == ATRI_NOT ? ATRI_IR_BOOL : infer_type(l, e->left, hint);
        case ATRI_EXPR_BINARY: {
            if (e->op >= ATRI_EQ) return ATRI_IR_BOOL;
            AtriIRType a = infer_type(l, e->left, ATRI_IR_INVALID), b = infer_type(l, e->right, ATRI_IR_INVALID);
            if (atri_ir_is_pointer(a) || atri_ir_is_pointer(b)) {
                return e->op == ATRI_SUB && atri_ir_is_pointer(a) && atri_ir_is_pointer(b) ?
                    ATRI_IR_I64 : atri_ir_is_pointer(a) ? a : b;
            }
            if (a == ATRI_IR_I64 || b == ATRI_IR_I64) return ATRI_IR_I64;
            if (a == ATRI_IR_I32 || b == ATRI_IR_I32) return ATRI_IR_I32;
            if (a == ATRI_IR_U8 || b == ATRI_IR_U8) return ATRI_IR_U8;
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
    if (l->constant_context) { return error(l, e->span, "compile-time pointer operations are not supported yet"); }
    if (e->kind == ATRI_EXPR_DEREFERENCE) { return expression(l, e->left, ATRI_IR_INVALID, out); }
    if (e->kind == ATRI_EXPR_NAME && !e->qualifier.end) {
        size_t b = lookup(l, e->text);
        if (b != ATRI_NONE && l->bindings[b].type != ATRI_IR_I32 && l->bindings[b].type != ATRI_IR_U8 && !atri_ir_is_aggregate(l->bindings[b].type) && !atri_ir_is_pointer(l->bindings[b].type)) {
            return error(l, e->span, "address-of supports only i32 variables and struct variables");
        }
        if (b != ATRI_NONE && atri_ir_is_pointer(l->bindings[b].type)) { return error(l, e->span, "taking the address of a pointer requires unsupported multi-level pointers"); }
        if (b != ATRI_NONE && !l->bindings[b].mutable) {
            return error(l, e->span, "cannot take the address of a compile-time constant or read-only parameter");
        }
    }
    AtriIRType target; AtriId storage;
    if (!location(l, id, true, &target, &storage)) { return false; }
    AtriIRType pointer = atri_ir_pointer_type(target);
    if (pointer == ATRI_IR_INVALID) { return error(l, e->span, "taking the address requires i32 or a struct; multi-level pointers are unsupported"); }
    return emit(l, (AtriIRInst){.kind = ATRI_IR_ADDRESS, .type = pointer, .left = storage, .span = e->span}, out);
}
static bool expression(Lower *l, AtriId id, AtriIRType expected, AtriId *out) {
    const AtriExpr *e = &l->ast->expressions[id];
    if (++l->depth > 256) { --l->depth; return error(l, e->span, "expression nesting limit exceeded"); }
    AtriIRInst inst = {.span = e->span, .type = ATRI_IR_I32};
    bool ok = false;
    switch (e->kind) {
        case ATRI_EXPR_RUN: ok = run_expression(l, e, expected, out); break;
        case ATRI_EXPR_CONDITIONAL: ok = conditional(l, e, expected, out); break;
        case ATRI_EXPR_STRING: ok = string_literal(l, e, out); break;
        case ATRI_EXPR_BOOL:
            inst.kind = ATRI_IR_CONST; inst.type = ATRI_IR_BOOL; inst.immediate = e->boolean;
            ok = emit(l, inst, out); break;
        case ATRI_EXPR_INTEGER:
            inst.kind = ATRI_IR_CONST;
            inst.type = atri_ir_is_integer(expected) ? expected : ATRI_IR_I32;
            if (literal(l, e->text, inst.type == ATRI_IR_U8 ? UINT8_MAX : inst.type == ATRI_IR_I64 ? (uint64_t)INT64_MAX : INT32_MAX, &inst.immediate)) ok = emit(l, inst, out);
            break;
        case ATRI_EXPR_NULL:
            if (!atri_ir_is_pointer(expected)) {
                error(l, e->span, "null requires a pointer type context");
            } else {
                inst.kind = ATRI_IR_CONST; inst.type = expected; ok = emit(l, inst, out);
            }
            break;
        case ATRI_EXPR_NAME: {
            size_t b = e->qualifier.end ? ATRI_NONE : lookup(l, e->text);
            AtriId global = resolve_declaration(l, e, true);
            if (b == ATRI_NONE && global != ATRI_NONE) {
                inst.type = l->ir->globals[global].type;
                if (l->ast->globals[global].declaration.kind == ATRI_STMT_CONSTANT) {
                    inst.kind = ATRI_IR_CALL; inst.callee = l->ast->function_count + global;
                    ok = emit(l, inst, out);
                } else if (l->constant_context) {
                    error(l, e->span, "compile-time initializer cannot read a runtime global");
                } else {
                    inst.kind = ATRI_IR_LOAD;
                    ok = global_location(l, global, e->span, &inst.left) && emit(l, inst, out);
                }
                break;
            }
            if (b == ATRI_NONE) error(l, e->text, "unknown local name (locals must be declared before use)");
            else if (l->constant_context && !l->bindings[b].constant)
                error(l, e->text, "compile-time constant initializer cannot read a runtime variable or parameter");
            else if (l->bindings[b].address != ATRI_NONE) {
                inst.kind = ATRI_IR_LOAD; inst.type = l->bindings[b].type; inst.left = l->bindings[b].address; ok = emit(l, inst, out);
            } else { *out = l->bindings[b].value; ok = true; }
            break;
        }
        case ATRI_EXPR_INITIALIZER: ok = initialize_aggregate(l, e, expected, out); break;
        case ATRI_EXPR_INDEX: case ATRI_EXPR_FIELD: {
            AtriIRType ft;
            if (location(l, id, false, &ft, &inst.left)) {
                inst.kind = ATRI_IR_LOAD; inst.type = ft; ok = emit(l, inst, out);
            }
            break;
        }
        case ATRI_EXPR_ADDRESS: ok = address(l, e->left, out); break;
        case ATRI_EXPR_DEREFERENCE:
            inst.kind = ATRI_IR_LOAD;
            if (location(l, id, false, &inst.type, &inst.left)) { ok = emit(l, inst, out); }
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
                if (!literal(l, l->ast->expressions[e->left].text, inst.type == ATRI_IR_U8 ? UINT8_MAX : inst.type == ATRI_IR_I64 ? (UINT64_C(1) << 63) : UINT32_C(2147483648), &inst.immediate)) break;
                uint64_t negated;
                if (!atri_integer_compute(inst.type, ATRI_NEG, inst.immediate, 0, &negated)) break;
                inst.immediate = negated; ok = emit(l, inst, out);
            } else if (expression(l, e->left, inst.type, &inst.left)) ok = emit(l, inst, out);
            break;
        case ATRI_EXPR_BINARY: {
            if (e->op == ATRI_AND || e->op == ATRI_OR) { ok = logical(l, e, out); break; }
            AtriIRType a = infer_type(l, e->left, ATRI_IR_INVALID), b = infer_type(l, e->right, ATRI_IR_INVALID);
            if (l->ast->expressions[e->left].kind == ATRI_EXPR_NULL && a == ATRI_IR_INVALID) {
                a = atri_ir_is_pointer(b) ? b : ATRI_IR_PTR_I32;
            }
            if (l->ast->expressions[e->right].kind == ATRI_EXPR_NULL && b == ATRI_IR_INVALID) {
                b = atri_ir_is_pointer(a) ? a : ATRI_IR_PTR_I32;
            }
            if (e->op >= ATRI_EQ && e->op <= ATRI_GE) {
                AtriIRType operand_type = a != ATRI_IR_INVALID ? a : b != ATRI_IR_INVALID ? b : ATRI_IR_I32;
                if (atri_ir_is_aggregate(operand_type)) { error(l, e->span, "struct equality and ordering are not automatically defined"); break; }
                if (!atri_ir_is_integer(operand_type) && e->op != ATRI_EQ && e->op != ATRI_NE) {
                    error(l, e->span, "ordered comparisons require integer operands"); break;
                }
                inst.kind = ATRI_IR_COMPARE; inst.type = ATRI_IR_BOOL; inst.op = e->op;
                if (expression(l, e->left, operand_type, &inst.left) &&
                    expression(l, e->right, operand_type, &inst.right)) ok = emit(l, inst, out);
                break;
            }
            if (atri_ir_is_pointer(a) || atri_ir_is_pointer(b)) {
                if (e->op != ATRI_ADD && e->op != ATRI_SUB) {
                    error(l, e->span, "pointer arithmetic supports only pointer +/- integer and pointer - pointer"); break;
                }
                if (e->op == ATRI_SUB && !atri_ir_is_pointer(a)) {
                    error(l, e->span, "cannot subtract a pointer from an integer"); break;
                }
                if (atri_ir_is_pointer(a) && atri_ir_is_pointer(b)) {
                    if (e->op != ATRI_SUB) { error(l, e->span, "cannot add two pointers"); break; }
                    if (a >= ATRI_IR_PTR_STRUCT && !atri_ir_type_size(l->ir, atri_ir_pointee(a))) { error(l, e->span, "pointer arithmetic on zero-size structs is not defined"); break; }
                    inst.kind = ATRI_IR_PTR_DIFF; inst.type = ATRI_IR_I64;
                    if (expression(l, e->left, a, &inst.left) &&
                        expression(l, e->right, a, &inst.right) &&
                        nonnull(l, inst.left, e->span) && nonnull(l, inst.right, e->span)) {
                        ok = emit(l, inst, out);
                    }
                } else {
                    AtriIRType pointer_type = atri_ir_is_pointer(a) ? a : b;
                    AtriIRType offset_type = atri_ir_is_pointer(a) ? b : a;
                    if (!atri_ir_is_integer(offset_type)) offset_type = ATRI_IR_I64;
                    AtriId left, right;
                    /* n+p 的 IR 统一保存为“指针、偏移”，但求值顺序仍须先 n 后 p。 */
                    if (!expression(l, e->left, atri_ir_is_pointer(a) ? pointer_type : offset_type, &left) ||
                        !expression(l, e->right, atri_ir_is_pointer(b) ? pointer_type : offset_type, &right)) {
                        break;
                    }
                    inst.kind = ATRI_IR_PTR_OFFSET; inst.type = pointer_type; inst.op = e->op;
                    inst.left = atri_ir_is_pointer(a) ? left : right; inst.right = atri_ir_is_pointer(a) ? right : left;
                    if (!nonnull(l, inst.left, e->span)) break;
                    const AtriIRInst *offset = &l->function->instructions[inst.right];
                    int64_t amount = offset->type == ATRI_IR_I64 ? atri_i64_signed(offset->immediate) : atri_i32_signed((uint32_t)offset->immediate);
                    int64_t stride = pointer_type == ATRI_IR_PTR_CHAR ? 1 : (int64_t)atri_ir_type_size(l->ir, atri_ir_pointee(pointer_type));
                    if (!stride) { error(l, e->span, "pointer arithmetic on zero-size structs is not defined"); break; }
                    if (offset->kind == ATRI_IR_CONST && (amount < INT64_MIN / stride || amount > INT64_MAX / stride)) {
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
            if (!e->qualifier.end && lookup(l, e->text) != ATRI_NONE) { error(l, e->text, "local value is not callable"); break; }
            AtriId callee = resolve_declaration(l, e, false);
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
    if (ok && expected != ATRI_IR_INVALID && l->function->instructions[*out].type != expected) {
        ok = error(l, e->span, "type mismatch: scalar, pointer and nominal struct types do not implicitly convert");
    }
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
        /* return/break 后的源码仍要检查；另开无前驱块，避免把这些指令接到已终结的路径上。 */
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
        if (s->kind == ATRI_STMT_CONSTANT && (atri_ir_is_pointer(expected) || atri_ir_is_aggregate(expected))) {
            return error(l, s->span, "compile-time pointer constants, struct constants and array constants are not supported yet");
        }
    } else if (s->kind == ATRI_STMT_RETURN) expected = l->function->return_type;
    else {
        const AtriExpr *target = &l->ast->expressions[s->target];
        if (target->kind == ATRI_EXPR_NAME) {
            binding = target->qualifier.end ? ATRI_NONE : lookup(l, target->text);
            AtriId global = resolve_declaration(l, target, true);
            if (binding == ATRI_NONE && global != ATRI_NONE) {
                if (l->ast->globals[global].declaration.kind == ATRI_STMT_CONSTANT) {
                    return error(l, target->span, "cannot assign to a compile-time constant");
                }
                expected = l->ir->globals[global].type;
                if (!global_location(l, global, target->span, &destination)) { return false; }
            } else {
            if (binding == ATRI_NONE) return error(l, target->span, "assignment to unknown name");
            if (!l->bindings[binding].mutable) return error(l, target->span, "cannot assign to a compile-time constant or read-only parameter");
            expected = l->bindings[binding].type; destination = l->bindings[binding].address;
            }
        } else if (target->kind == ATRI_EXPR_DEREFERENCE || target->kind == ATRI_EXPR_FIELD || target->kind == ATRI_EXPR_INDEX) {
            if (!location(l, s->target, true, &expected, &destination)) { return false; }
        } else return error(l, target->span, "assignment requires a mutable local or a dereferenced pointer");
    }
    size_t first = l->function->instruction_count;
    AtriId value;
    l->constant_context = s->kind == ATRI_STMT_CONSTANT;
    if (s->expression == ATRI_NONE && atri_ir_is_aggregate(expected)) {
        AtriExpr empty = {.kind = ATRI_EXPR_INITIALIZER, .span = s->span};
        if (!initialize_aggregate(l, &empty, expected, &value)) { return false; }
    } else if (s->expression == ATRI_NONE) {
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .span = s->span, .type = expected}, &value)) return false;
    } else if (!expression(l, s->expression, expected, &value)) return false;
    l->constant_context = false;
    if (s->kind == ATRI_STMT_CONSTANT) {
        /* 后续引用先指向稳定的结果占位；全部函数降低后，再求值并清除前面的初始化指令范围。 */
        AtriId result;
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .span = s->span, .type = expected}, &result)) return false;
        AtriIRConstant c = {.span = s->span, .expression = value, .result = result, .first_instruction = first};
        if (!atri_append((void **)&l->function->constants, &l->function->constant_count,
                          &l->function->constant_capacity, sizeof(c), &c)) return error(l, s->span, "out of memory");
        value = result;
    }
    if (declaration) {
        AtriId local = ATRI_NONE;
        if (s->kind == ATRI_STMT_VARIABLE && (l->memory || atri_ir_is_aggregate(expected) || ((expected == ATRI_IR_I32 || expected == ATRI_IR_U8) && address_taken(l, s->name)))) {
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
    if (atri_ir_is_pointer(expected) && l->function->instructions[value].kind == ATRI_IR_ADDRESS) {
        AtriId origin = l->function->instructions[value].left;
        while (l->function->instructions[origin].kind == ATRI_IR_FIELD || l->function->instructions[origin].kind == ATRI_IR_INDEX) { origin = l->function->instructions[origin].left; }
        if (l->function->instructions[origin].kind == ATRI_IR_LOCAL && l->function->instructions[origin].immediate == 0) {
            return error(l, s->span, "cannot directly return the address of a local variable");
        }
    }
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
            /* for 头部的绑定覆盖条件、step 和体；体另开内层作用域，不让体内声明泄漏到 step。 */
            size_t saved = l->binding_count; ++l->scope;
            if (s->initializer != ATRI_NONE && !statement(l, s->initializer)) return false;
            AtriId cond_block = new_block(l), body = new_block(l), step_block = new_block(l), end = new_block(l);
            /* 指令在数组中的先后不代表执行顺序：do 首次跳到 body，其余循环先进入条件块。 */
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
    /* 单份“变量→值”映射无法表达分支汇合及循环回边；有控制流时暂以 local/load/store 衔接。 */
    for (size_t i = f->first_statement; i < f->first_statement+f->statement_count; ++i)
        if (ast->statements[i].kind >= ATRI_STMT_BLOCK) return true;
    for (size_t i = 0; i < ast->expression_count; ++i) {
        const AtriExpr *e = &ast->expressions[i];
        if (e->span.start >= f->body.start && e->span.end <= f->body.end &&
            (e->kind == ATRI_EXPR_CONDITIONAL || (e->kind == ATRI_EXPR_BINARY && (e->op == ATRI_AND || e->op == ATRI_OR)))) return true;
    }
    return false;
}
/* 所有源码和 := 义务已检查；此处只据常量 bool 分支删除不可达块，不证明一般条件命题。 */
static bool prune(AtriIRFunction *f, AtriDiagnostic *d) {
    size_t count = f->block_count;
    bool *reachable = calloc(count, sizeof(*reachable));
    AtriId *map = malloc(count * sizeof(*map));
    if (!reachable || !map) { free(reachable); free(map); *d = (AtriDiagnostic){f->name, "out of memory"}; return false; }
    reachable[0] = true;
    bool changed;
    /* 块的物理顺序不保证拓扑有序且允许回边，因此反复传播到可达集合不再增长。 */
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
    /* 只压紧块号；指令暂用 NOP 保留原下标，已有值引用不必跟着重编号。 */
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
static bool known_integer(const AtriIRFunction *f, AtriId id, size_t depth, size_t *steps, uint64_t *out) {
    if (depth > 256 || !*steps) { return false; }
    --*steps;
    const AtriIRInst *v = &f->instructions[id];
    if (v->kind == ATRI_IR_CONST) { *out = v->immediate; return true; }
    if (v->kind != ATRI_IR_UNARY && v->kind != ATRI_IR_BINARY) { return false; }
    uint64_t a, b = 0;
    return known_integer(f, v->left, depth+1, steps, &a) &&
        (v->kind != ATRI_IR_BINARY || known_integer(f, v->right, depth+1, steps, &b)) &&
        atri_integer_compute(v->type, v->op, a, b, out);
}
static bool check_array_indices(Lower *l) {
    for (size_t n = 0; n < l->ir->function_count; ++n) {
        const AtriIRFunction *f = &l->ir->functions[n];
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            if (v->kind != ATRI_IR_INDEX) { continue; }
            AtriIRType base = f->instructions[v->left].type;
            if (atri_ir_is_pointer(base)) { base = atri_ir_pointee(base); }
            AtriIRArray array = l->ir->arrays[base-ATRI_IR_ARRAY];
            uint64_t bits;
            if (!array.length) { return error(l, v->span, "array index is statically out of bounds"); }
            size_t steps = 100000;
            if (known_integer(f, v->right, 0, &steps, &bits)) {
                int64_t index = f->instructions[v->right].type == ATRI_IR_I32 ? atri_i32_signed((uint32_t)bits) : atri_i64_signed(bits);
                if (index < 0 || (uint64_t)index >= array.length) { return error(l, v->span, "array index is statically out of bounds"); }
            }
        }
    }
    return true;
}
static bool c_value_compatible(const AtriIRProgram *p, AtriIRType type) {
    if (atri_ir_is_array(type)) {
        AtriIRArray array = p->arrays[type-ATRI_IR_ARRAY];
        return array.length != 0 && c_value_compatible(p, array.element);
    }
    if (!atri_ir_is_struct(type)) { return true; }
    const AtriIRStruct *t = &p->structs[type-ATRI_IR_STRUCT];
    if (!t->size) { return false; }
    for (size_t i = 0; i < t->field_count; ++i) {
        if (!c_value_compatible(p, p->fields[t->first_field+i].type)) { return false; }
    }
    return true;
}
bool atri_lower(AtriSource source, const AtriProgram *ast, AtriIRProgram *out, AtriDiagnostic *d) {
    *out = (AtriIRProgram){0}; *d = (AtriDiagnostic){0};
    AtriIRProgram ir = {.function_count = ast->function_count + ast->global_count + ast->field_count, .global_count = ast->global_count};
    ir.functions = calloc(ir.function_count ? ir.function_count : 1, sizeof(*ir.functions));
    if (!ir.functions) { *d = (AtriDiagnostic){{0,0}, "out of memory"}; return false; }
    Lower l = {.source = source, .ast = ast, .ir = &ir, .diagnostic = d};
    if (!atri_validate_modules(source, ast, d)) { goto failure; }
    if (!atri_layout_structs(source, ast, &ir, d)) { goto failure; }
    ir.globals = calloc(ast->global_count ? ast->global_count : 1, sizeof(*ir.globals));
    if (!ir.globals) { error(&l, (AtriSpan){0}, "out of memory"); goto failure; }
    for (size_t i = 0; i < ast->global_count; ++i) {
        const AtriStatement *g = &ast->globals[i].declaration;
        ir.globals[i].constant = g->kind == ATRI_STMT_CONSTANT;
        ir.globals[i].name = g->name;
        if (!type(&l, g->type, &ir.globals[i].type)) { goto failure; }
        if (atri_ir_is_aggregate(ir.globals[i].type)) { error(&l, g->span, "global struct storage and array storage are not supported yet"); goto failure; }
        if (ir.globals[i].constant && atri_ir_is_pointer(ir.globals[i].type)) {
            error(&l, g->span, "compile-time pointer constants, struct constants and array constants are not supported yet"); goto failure;
        }
        ir.functions[ast->function_count+i] = (AtriIRFunction){.name = g->name, .return_type = ir.globals[i].type, .block_count = 1};
    }
    for (size_t i = 0; i < ast->field_count; ++i) {
        AtriIRType ft = ir.fields[i].type;
        if (ast->fields[i].expression != ATRI_NONE && atri_ir_is_aggregate(ft)) {
            error(&l, ast->fields[i].name, "aggregate field default expressions are not supported yet"); goto failure;
        }
        ir.functions[ast->function_count+ast->global_count+i] = (AtriIRFunction){.name = ast->fields[i].name,
            .return_type = ast->fields[i].expression == ATRI_NONE ? ATRI_IR_I32 : ft, .block_count = 1};
    }
    /* 先收齐所有签名再降低函数体，使后定义函数和相互递归在同一套调用检查下成立。 */
    for (size_t i = 0; i < ast->function_count; ++i) {
        const AtriFunction *f = &ast->functions[i];
        AtriIRFunction *signature = &ir.functions[i];
        signature->module = atri_module_at(ast, f->name.start); signature->name = f->name; signature->parameter_count = f->parameter_count;
        signature->external_c = f->external_c; signature->block_count = f->external_c ? 0 : 1;
        if (!type(&l, f->return_type, &signature->return_type)) goto failure;
        if (f->external_c && (atri_ir_is_array(signature->return_type) || !c_value_compatible(&ir, signature->return_type))) { error(&l, f->return_type, "arrays and zero-size structs have no C by-value ABI"); goto failure; }
        signature->parameter_types = calloc(f->parameter_count ? f->parameter_count : 1, sizeof(*signature->parameter_types));
        if (!signature->parameter_types) { error(&l, f->name, "out of memory"); goto failure; }
        for (size_t a = 0; a < f->parameter_count; ++a) {
            AtriParameter parameter = ast->parameters[f->first_parameter+a];
            for (size_t previous = 0; previous < a; ++previous) {
                if (same(&l, parameter.name, ast->parameters[f->first_parameter+previous].name)) {
                    error(&l, parameter.name, "duplicate local or parameter name"); goto failure;
                }
            }
            if (!type(&l, parameter.type, &signature->parameter_types[a])) { goto failure; }
            if (f->external_c && (atri_ir_is_array(signature->parameter_types[a]) || !c_value_compatible(&ir, signature->parameter_types[a]))) { error(&l, parameter.type, "arrays and zero-size structs have no C by-value ABI"); goto failure; }
        }
    }
    for (size_t i = 0; i < ast->function_count; ++i) {
        const AtriFunction *f = &ast->functions[i];
        if (f->external_c) {
            continue;
        }
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
    /* 全局静态初始化借用同一常量解释器；临时函数在求值完成后删除。 */
    for (size_t i = 0; i < ast->global_count+ast->field_count; ++i) {
        AtriStatement field = {0};
        const AtriStatement *g;
        if (i < ast->global_count) { g = &ast->globals[i].declaration; }
        else {
            const AtriField *f = &ast->fields[i-ast->global_count];
            field = (AtriStatement){.name = f->name, .type = f->type, .span = f->name, .expression = f->expression}; g = &field;
        }
        AtriFunction context = {.name = g->name};
        l.ast_function = &context; l.function = &ir.functions[ast->function_count+i];
        l.binding_count = 0; l.constant_context = true;
        AtriId value, result, ignored;
        if (i >= ast->global_count && g->expression == ATRI_NONE) {
            /* 没有声明默认值的字段不消耗常量解释预算；仅保留临时索引位置。 */
            if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = ATRI_IR_I32, .span = g->span}, &value) ||
                !emit(&l, (AtriIRInst){.kind = ATRI_IR_RETURN, .type = ATRI_IR_I32, .left = value, .span = g->span}, &ignored)) { goto failure; }
            continue;
        }
        if (g->expression == ATRI_NONE) {
            if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = l.function->return_type, .span = g->span}, &value)) { goto failure; }
        } else if (!expression(&l, g->expression, l.function->return_type, &value)) { goto failure; }
        if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = l.function->return_type, .span = g->span}, &result)) { goto failure; }
        if (atri_ir_is_pointer(l.function->return_type)) {
            if (l.function->instructions[value].kind != ATRI_IR_CONST || l.function->instructions[value].immediate != 0) {
                error(&l, g->span, "global pointer initializer currently supports null only"); goto failure;
            }
            if (!emit(&l, (AtriIRInst){.kind = ATRI_IR_RETURN, .type = l.function->return_type, .left = result, .span = g->span}, &ignored)) { goto failure; }
            continue;
        }
        AtriIRConstant obligation = {.span = g->span, .expression = value, .result = result};
        if (!atri_append((void **)&l.function->constants, &l.function->constant_count, &l.function->constant_capacity, sizeof(obligation), &obligation) ||
            !emit(&l, (AtriIRInst){.kind = ATRI_IR_RETURN, .type = l.function->return_type, .left = result, .span = g->span}, &ignored)) { goto failure; }
    }
    free(l.bindings);
    if (!atri_resolve_constants(&ir, d) || !atri_ir_verify(&ir, d)) { atri_ir_destroy(&ir); return false; }
    for (size_t i = 0; i < ast->global_count+ast->field_count; ++i) {
        AtriIRFunction *initializer = &ir.functions[ast->function_count+i];
        uint64_t bits = initializer->instructions[initializer->instruction_count-2].immediate;
        if (i < ast->global_count) { ir.globals[i].bits = bits; }
        else { ir.fields[i-ast->global_count].default_bits = bits; }
    }
    for (size_t i = 0; i < ast->function_count; ++i) {
        AtriIRFunction *f = &ir.functions[i];
        for (size_t j = 0; j < f->instruction_count; ++j) {
            AtriIRInst *inst = &f->instructions[j];
            if (inst->kind == ATRI_IR_CALL && inst->callee >= ast->function_count) {
                size_t index = inst->callee-ast->function_count;
                inst->kind = ATRI_IR_CONST; inst->immediate = index < ast->global_count ? ir.globals[index].bits : ir.fields[index-ast->global_count].default_bits;
            }
        }
    }
    for (size_t i = ast->function_count; i < ir.function_count; ++i) {
        free(ir.functions[i].instructions); free(ir.functions[i].arguments); free(ir.functions[i].parameter_types);
    }
    ir.function_count = ast->function_count;
    if (!check_array_indices(&l)) { atri_ir_destroy(&ir); return false; }
    for (size_t i = 0; i < ir.function_count; ++i) {
        if (!ir.functions[i].external_c && !prune(&ir.functions[i], d)) {
            atri_ir_destroy(&ir); return false;
        }
    }
    if (!atri_ir_verify(&ir, d)) { atri_ir_destroy(&ir); return false; }
    *out = ir; return true;
failure:
    free(l.bindings); atri_ir_destroy(&ir); return false;
}
