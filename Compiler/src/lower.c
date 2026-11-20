#include "ir.h"
#include "lexer.h"
#include "array.h"

typedef struct {
    AtriSpan name;
    /* address != ATRI_NONE 时是 IR local 的 ID；名称读写也必须经由它，才能观察指针别名修改。 */
    AtriId value, address;
    AtriIRType type;
    bool mutable, constant, unavailable;
    size_t scope;
    AtriId previous;
} Binding;
typedef struct { AtriId stop, again; size_t defer_count; } Loop;
typedef struct { const AtriStatement *statement; AtriId binding; } Deferred;
typedef struct { AtriId global, result; AtriIRType type; size_t offset; } StaticValue;
typedef struct { AtriId function, result; size_t group; AtriSpan span; uint64_t bits; } CaseValue;
static int case_order(const void *left, const void *right) {
    const CaseValue *a = left, *b = right;
    if (a->group != b->group) { return (a->group > b->group)-(a->group < b->group); }
    return (a->bits > b->bits)-(a->bits < b->bits);
}
typedef struct {
    AtriSource source;
    const AtriProgram *ast;
    AtriIRProgram *ir;
    AtriIRFunction *function;
    const AtriFunction *ast_function;
    Binding *bindings;
    size_t binding_count, binding_capacity, depth;
    AtriId visible_binding;
    Deferred *defers;
    size_t defer_count, defer_capacity, defer_depth;
    StaticValue *static_values;
    size_t static_count, static_capacity, static_steps;
    CaseValue *case_values;
    size_t case_count, case_capacity;
    bool constant_context;
    bool memory, closed; /* memory 为可变局部分配存储；closed 表示当前块已有终结指令。 */
    size_t scope;
    Loop loops[256];
    size_t loop_count, loop_floor;
    AtriDiagnostic *diagnostic;
    AtriId *block_functions;
} Lower;
static bool error(Lower *l, AtriSpan span, const char *message) {
    *l->diagnostic = (AtriDiagnostic){.span = span, .message = message}; return false;
}
static bool same(Lower *l, AtriSpan a, AtriSpan b) {
    return a.end - a.start == b.end - b.start &&
           memcmp(l->source.text + a.start, l->source.text + b.start, a.end - a.start) == 0;
}
static bool type(Lower *l, AtriSpan span, AtriIRType *out) {
    return atri_type_from_source(l->source, l->ast, l->ir, span, out, l->diagnostic);
}
static size_t lookup(Lower *l, AtriSpan name) {
    for (AtriId i = l->visible_binding; i != ATRI_NONE; i = l->bindings[i].previous) {
        if (same(l, name, l->bindings[i].name)) { return i; }
    }
    return ATRI_NONE;
}
static bool bind(Lower *l, Binding b) {
    size_t previous = lookup(l, b.name);
    if (previous != ATRI_NONE && l->bindings[previous].scope == l->scope) return error(l, b.name, "duplicate local or parameter name");
    b.scope = l->scope;
    b.previous = l->visible_binding;
    if (!atri_append((void **)&l->bindings, &l->binding_count, &l->binding_capacity, sizeof(b), &b)) {
        return error(l, b.name, "out of memory");
    }
    l->visible_binding = l->binding_count-1;
    return true;
}
static bool emit(Lower *l, AtriIRInst inst, AtriId *id) {
    return atri_ir_append(l->function, inst, id) || error(l, inst.span, "out of memory while lowering IR");
}
static bool literal(Lower *l, AtriSpan text, AtriIRType type, uint64_t limit, uint64_t *out) {
    if (atri_integer_literal(l->source, text, limit, out)) { return true; }
    const char *message = type == ATRI_IR_I8 ? "integer literal is outside the i8 range" :
        type == ATRI_IR_I16 ? "integer literal is outside the i16 range" :
        type == ATRI_IR_U8 ? "integer literal is outside the u8 range" :
        type == ATRI_IR_U16 ? "integer literal is outside the u16 range" :
        type == ATRI_IR_U32 ? "integer literal is outside the u32 range" :
        type == ATRI_IR_U64 ? "integer literal is outside the u64 range" :
        type == ATRI_IR_I32 ? "integer literal is outside the i32 range" : "integer literal is outside the i64 range";
    return error(l, text, message);
}
static bool string_literal(Lower *l, const AtriExpr *e, AtriId *out) {
    AtriIRString string = {.bytes = malloc(e->text.end-e->text.start)};
    if (!string.bytes) { return error(l, e->span, "out of memory while decoding string literal"); }
    size_t cursor = e->text.start+1, end = e->text.end-1;
    while (cursor < end) { string.bytes[string.length++] = atri_literal_byte(l->source, &cursor); }
    string.bytes[string.length] = 0;
    size_t index = l->ir->string_count;
    if (!atri_append((void **)&l->ir->strings, &l->ir->string_count, &l->ir->string_capacity, sizeof(string), &string)) {
        free(string.bytes); return error(l, e->span, "out of memory while storing string literal");
    }
    return emit(l, (AtriIRInst){.kind = ATRI_IR_STRING, .type = ATRI_IR_PTR_U8, .span = e->span, .immediate = index}, out);
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
    if (e->keyword) { return l->ast->keyword_function && !want_global ? l->ast->keyword_function - 1 : ATRI_NONE; }
    if (e->module.end) { return ATRI_NONE; }
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
static bool c_value_compatible(const AtriIRProgram *p, AtriIRType type);
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
            if (binding->unavailable || (l->constant_context && !binding->constant)) {
                return error(l, e->span, "compile-time expression cannot read a runtime variable or parameter");
            }
            if (write && !binding->mutable) { return error(l, e->span, "cannot modify a read-only parameter or compile-time constant"); }
            if (binding->address != ATRI_NONE) { *out = binding->address; return true; }
            if (!write && atri_ir_is_aggregate(*type)) { *out = binding->value; return true; }
            return error(l, e->span, "expression has no addressable storage");
        }
        AtriId global = resolve_declaration(l, e, true);
        if (global != ATRI_NONE && !l->ir->globals[global].constant) {
            if (l->constant_context) { return error(l, e->span, "compile-time initializer cannot read a runtime global"); }
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
        if (!atri_ir_is_integer(index_type)) { return error(l, e->span, "array index requires an integer"); }
        if (!expression(l, e->right, index_type, &index)) { return false; }
        *type = array.element;
        return emit(l, (AtriIRInst){.kind = ATRI_IR_INDEX, .type = *type, .left = base, .right = index, .span = e->span}, out);
    }
    if (e->kind == ATRI_EXPR_DEREFERENCE) {
        AtriIRType pointer = infer_type(l, e->left, ATRI_IR_INVALID);
        *type = atri_ir_pointee(pointer);
        if (*type == ATRI_IR_INVALID || *type == ATRI_IR_VOID || !atri_ir_valid_type(l->ir, *type)) {
            return error(l, e->span, "type mismatch: dereference requires a pointer to a complete object; void and opaque targets cannot be dereferenced");
        }
        return expression(l, e->left, pointer, out) && nonnull(l, *out, e->span);
    }
    if (e->kind == ATRI_EXPR_FIELD) {
        if (!atri_ir_is_struct(infer_type(l, e->left, ATRI_IR_INVALID))) {
            return error(l, e->span, "field access requires a struct or union object; pointers require explicit dereference");
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
        if (field == ATRI_NONE) { return error(l, e->text, "unknown field or non-aggregate object; pointers require explicit dereference"); }
        *type = l->ir->fields[field].type;
        return emit(l, (AtriIRInst){.kind = ATRI_IR_FIELD, .type = *type, .left = base, .immediate = field, .span = e->span}, out);
    }
    return error(l, e->span, "expression is not an addressable object");
}
static bool check_struct_initializer(Lower *l, const AtriExpr *e, AtriIRType type) {
    bool is_union = l->ir->structs[type-ATRI_IR_STRUCT].is_union;
    if (is_union && e->argument_count > 1) {
        return error(l, e->span, "union initializer may select at most one member");
    }
    for (size_t i = 0; i < e->argument_count; ++i) {
        const AtriInitializer *item = &l->ast->initializers[e->first_argument+i];
        if (!item->name.end) {
            return error(l, l->ast->expressions[item->expression].span,
                is_union ? "union initializer requires a named member" : "struct initializer requires named fields");
        }
        if (field_index(l, type, item->name) == ATRI_NONE) {
            return error(l, item->name, is_union ? "unknown union initializer member" : "unknown struct initializer field");
        }
        for (size_t j = 0; j < i; ++j) {
            if (same(l, item->name, l->ast->initializers[e->first_argument+j].name)) { return error(l, item->name, "duplicate struct initializer field"); }
        }
    }
    return true;
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
    if (!atri_ir_is_struct(type)) { return error(l, e->span, "aggregate initializer requires an expected struct, union or array type"); }
    if (l->constant_context) { return error(l, e->span, "compile-time struct/union values are not supported yet"); }
    const AtriIRStruct *t = &l->ir->structs[type-ATRI_IR_STRUCT];
    AtriId object, zero;
    if (!emit(l, (AtriIRInst){.kind = ATRI_IR_LOCAL, .type = type, .span = e->span}, &object) ||
        !emit(l, (AtriIRInst){.kind = ATRI_IR_ZERO, .type = type, .span = e->span}, &zero) || !store(l, object, zero, e->span)) { return false; }
    /* union 只初始化选中的成员；空列表保持整块零存储，不应用重叠成员的默认初始化。 */
    if (!check_struct_initializer(l, e, type)) { return false; }
    for (size_t i = 0; !t->is_union && i < t->field_count; ++i) {
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

/* 只物化静态对象；标量叶子借现有常量义务求值，不执行聚合 load/store 或宿主指针。 */
static bool static_initializer(Lower *l, AtriId global, AtriIRType type, AtriId id,
                               AtriId default_field, size_t offset, size_t depth, AtriSpan span) {
    if (depth > 256 || !l->static_steps) { return error(l, span, "static initializer exceeded its step/depth limit"); }
    --l->static_steps;
    const AtriExpr *e = id == ATRI_NONE ? NULL : &l->ast->expressions[id];
    if (e) { span = e->span; }
    if (atri_ir_is_aggregate(type)) {
        if (e && e->kind != ATRI_EXPR_INITIALIZER) {
            return error(l, span, "static aggregate initialization requires a brace initializer; aggregate calls and copies are not supported yet");
        }
        if (!e && !atri_ir_type_size(l->ir, type)) { return true; }
        if (atri_ir_is_array(type)) {
            AtriIRArray array = l->ir->arrays[type-ATRI_IR_ARRAY];
            if (e && e->argument_count > array.length) { return error(l, span, "too many array initializer elements"); }
            size_t stride = atri_ir_type_size(l->ir, array.element);
            for (size_t i = 0; i < array.length; ++i) {
                AtriId element = ATRI_NONE;
                if (e && i < e->argument_count) {
                    const AtriInitializer *item = &l->ast->initializers[e->first_argument+i];
                    if (item->name.end) { return error(l, item->name, "array initializer requires positional elements"); }
                    element = item->expression;
                }
                if (!static_initializer(l, global, array.element, element, ATRI_NONE, offset+i*stride, depth+1, span)) { return false; }
            }
            return true;
        }
        if (e && !check_struct_initializer(l, e, type)) { return false; }
        const AtriIRStruct *structure = &l->ir->structs[type-ATRI_IR_STRUCT];
        /* 默认字段无表达式副作用；显式项单独按源码顺序登记求值义务。 */
        for (size_t i = 0; !structure->is_union && i < structure->field_count; ++i) {
            size_t field = structure->first_field+i;
            bool supplied = false;
            for (size_t j = 0; e && j < e->argument_count; ++j) {
                if (same(l, l->ir->fields[field].name, l->ast->initializers[e->first_argument+j].name)) { supplied = true; break; }
            }
            if (!supplied && !static_initializer(l, global, l->ir->fields[field].type, ATRI_NONE, field,
                offset+l->ir->fields[field].offset, depth+1, span)) { return false; }
        }
        for (size_t i = 0; e && i < e->argument_count; ++i) {
            const AtriInitializer *item = &l->ast->initializers[e->first_argument+i];
            AtriId field = field_index(l, type, item->name);
            if (!static_initializer(l, global, l->ir->fields[field].type, item->expression, ATRI_NONE,
                offset+l->ir->fields[field].offset, depth+1, item->name)) { return false; }
        }
        return true;
    }
    if (!e && (default_field == ATRI_NONE || l->ast->fields[default_field].expression == ATRI_NONE)) { return true; }
    /* 当前指针字段的默认值/静态初值只允许 null；字段声明自身仍由下方统一流程检查。 */
    if (!e && atri_ir_is_pointer(type)) { return true; }
    size_t first = l->function->instruction_count;
    AtriId value, result;
    if (e) {
        if (!expression(l, id, type, &value)) { return false; }
    } else {
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CALL, .type = type, .span = span,
            .callee = l->ast->function_count+l->ast->global_count+default_field}, &value)) { return false; }
    }
    if (atri_ir_is_pointer(type)) {
        const AtriIRInst *pointer = &l->function->instructions[value];
        if (pointer->kind != ATRI_IR_CONST || pointer->immediate) { return error(l, span, "static pointer initializer currently supports null only"); }
        return true;
    }
    if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = type, .span = span}, &result)) { return false; }
    AtriIRConstant obligation = {.span = span, .expression = value, .result = result, .first_instruction = first};
    StaticValue output = {.global = global, .result = result, .type = type, .offset = offset};
    return (atri_append((void **)&l->function->constants, &l->function->constant_count,
        &l->function->constant_capacity, sizeof(obligation), &obligation) &&
        atri_append((void **)&l->static_values, &l->static_count, &l->static_capacity, sizeof(output), &output)) ||
        error(l, span, "out of memory while collecting static initializer values");
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
static bool block_call(Lower *l, const AtriExpr *e, AtriId *out);
static bool add_lexical_constant(Lower *l, AtriIRConstant c) {
    AtriIRFunction *f = l->function;
    /* defer 可在多条退出边生成同一表达式。各块各有结果槽，同一源码位置的副作用只求值一次。 */
    for (size_t i = 0; l->defer_depth && i < f->constant_count; ++i) {
        const AtriIRConstant *earlier = &f->constants[i];
        if (earlier->span.start == c.span.start && earlier->span.end == c.span.end &&
            earlier->lexical && !earlier->capture_owner) {
            c.capture_owner = (size_t)(f - l->ir->functions) + 1;
            c.expression = i; break;
        }
    }
    c.lexical = true;
    return atri_append((void **)&f->constants, &f->constant_count, &f->constant_capacity, sizeof(c), &c) ||
        error(l, c.span, "out of memory recording compile-time obligation");
}
static bool require_value(Lower *l, AtriId value, size_t first, AtriSpan span, bool discard, AtriId *out) {
    AtriId result;
    AtriIRType type = l->function->instructions[value].type;
    /* 无结果调用仍需执行求值义务，内部占位只用于删除求值区间。 */
    if (discard) { type = ATRI_IR_I32; }
    if (!atri_ir_is_integer(type) && type != ATRI_IR_BOOL && type != ATRI_IR_PTR_U8) { return error(l, span, "compile-time evaluation requires a materializable integer, bool or byte string or a discarded void call"); }
    if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = type, .span = span}, &result)) { return false; }
    AtriIRConstant obligation = {.span = span, .expression = value, .result = result, .first_instruction = first, .discard = discard};
    if (!add_lexical_constant(l, obligation)) { return false; }
    *out = result; return true;
}
static bool compile_time_value(Lower *l, AtriId operand, AtriSpan span, AtriIRType expected, bool discard, AtriId *out) {
    if (l->constant_context) { return expression(l, operand, expected, out); }
    size_t first = l->function->instruction_count;
    l->constant_context = true;
    AtriId value;
    bool ok = expression(l, operand, expected, &value);
    l->constant_context = false;
    return ok && require_value(l, value, first, span, discard, out);
}
static bool run(Lower *l, const AtriExpr *e, AtriIRType expected, bool discard, AtriId *out) {
    if (!e->run_block) { return compile_time_value(l, e->left, e->span, expected, discard, out); }
    size_t first = l->function->instruction_count;
    if (!block_call(l, e, out)) { return false; }
    return l->constant_context || require_value(l, *out, first, e->span, discard, out);
}
static AtriIRType integer_type_name(Lower *l, AtriSpan span) {
    AtriIRType type = atri_ir_builtin_type(l->source.text+span.start, span.end-span.start);
    return atri_ir_is_integer(type) ? type : ATRI_IR_INVALID;
}
static AtriIRType conversion_type(Lower *l, AtriSpan span) {
    AtriIRType target = integer_type_name(l, span);
    if (target == ATRI_IR_INVALID) {
        if (!type(l, span, &target) || (!atri_ir_is_pointer(target) && !atri_ir_is_integer(target))) { return ATRI_IR_INVALID; }
    }
    return target;
}
/* 仅预测类型，不生成指令或提前执行实参，避免改变副作用顺序；无上下文的字面量暂不定型。 */
static AtriIRType infer_type(Lower *l, AtriId id, AtriIRType hint) {
    const AtriExpr *e = &l->ast->expressions[id];
    switch (e->kind) {
        case ATRI_EXPR_CAST: return conversion_type(l, e->text);
        case ATRI_EXPR_SIZEOF: case ATRI_EXPR_ALIGNOF: return ATRI_IR_U64;
        case ATRI_EXPR_RUN: {
            if (!e->run_block) { return infer_type(l, e->left, hint); }
            AtriIRType result = ATRI_IR_VOID;
            return !e->text.end || type(l, e->text, &result) ? result : ATRI_IR_INVALID;
        }
        case ATRI_EXPR_CONDITIONAL: {
            AtriIRType yes = infer_type(l, e->right, hint);
            return yes != ATRI_IR_INVALID ? yes : infer_type(l, e->alternative, hint);
        }
        case ATRI_EXPR_INDEX: {
            AtriIRType array = infer_type(l, e->left, ATRI_IR_INVALID);
            return atri_ir_is_array(array) ? l->ir->arrays[array-ATRI_IR_ARRAY].element : ATRI_IR_INVALID;
        }
        case ATRI_EXPR_BOOL: return ATRI_IR_BOOL;
        case ATRI_EXPR_CHARACTER: return ATRI_IR_U8;
        case ATRI_EXPR_INTEGER: return atri_ir_is_integer(hint) ? hint : ATRI_IR_INVALID;
        case ATRI_EXPR_STRING: return ATRI_IR_PTR_U8;
        case ATRI_EXPR_NULL: return atri_ir_is_pointer(hint) ? hint : ATRI_IR_INVALID;
        case ATRI_EXPR_ADDRESS: return atri_ir_pointer_type(infer_type(l, e->left, ATRI_IR_INVALID));
        case ATRI_EXPR_DEREFERENCE: return atri_ir_pointee(infer_type(l, e->left, ATRI_IR_INVALID));
        case ATRI_EXPR_INITIALIZER: return atri_ir_is_aggregate(hint) ? hint : ATRI_IR_INVALID;
        case ATRI_EXPR_FIELD: {
            AtriId field = field_index(l, infer_type(l, e->left, ATRI_IR_INVALID), e->text);
            return field == ATRI_NONE ? ATRI_IR_INVALID : l->ir->fields[field].type;
        }
        case ATRI_EXPR_NAME: {
            AtriIRType member_type; uint64_t bits;
            if (atri_enum_member(l->source, l->ast, l->ir, e, &member_type, &bits)) { return member_type; }
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
            if ((e->op >= ATRI_EQ && e->op <= ATRI_GE) || e->op == ATRI_AND || e->op == ATRI_OR) { return ATRI_IR_BOOL; }
            AtriIRType a = infer_type(l, e->left, ATRI_IR_INVALID), b = infer_type(l, e->right, ATRI_IR_INVALID);
            if (atri_ir_is_pointer(a) || atri_ir_is_pointer(b)) {
                return e->op == ATRI_SUB && atri_ir_is_pointer(a) && atri_ir_is_pointer(b) ?
                    ATRI_IR_I64 : atri_ir_is_pointer(a) ? a : b;
            }
            if (e->op == ATRI_SHL || e->op == ATRI_SHR) { return a != ATRI_IR_INVALID ? a : hint; }
            return a != ATRI_IR_INVALID ? a : b != ATRI_IR_INVALID ? b :
                atri_ir_is_integer(hint) ? hint : ATRI_IR_INVALID;
        }
    }
    return ATRI_IR_INVALID;
}
static bool nonnull(Lower *l, AtriId pointer, AtriSpan span) {
    const AtriIRInst *v = &l->function->instructions[pointer];
    while (v->kind == ATRI_IR_CAST && atri_ir_is_pointer(v->type)) {
        v = &l->function->instructions[v->left];
    }
    if (v->kind == ATRI_IR_CONST && v->immediate == 0) {
        size_t slot = (size_t)(v - l->function->instructions);
        for (size_t i = 0; i < l->function->constant_count; ++i) {
            AtriIRConstant *c = &l->function->constants[i];
            if (c->result == slot) {
                if (!c->nonnull_use.end) { c->nonnull_use = span; }
                return true;
            }
        }
        return error(l, span, "cannot dereference a statically known null pointer");
    }
    return true;
}
static bool address(Lower *l, AtriId id, AtriId *out) {
    const AtriExpr *e = &l->ast->expressions[id];
    if (l->constant_context) { return error(l, e->span, "compile-time pointer operations are not supported yet"); }
    if (e->kind == ATRI_EXPR_DEREFERENCE) { return expression(l, e->left, ATRI_IR_INVALID, out); }
    if (e->kind == ATRI_EXPR_NAME && !e->qualifier.end) {
        size_t b = lookup(l, e->text);
        if (b != ATRI_NONE && !atri_ir_is_integer(l->bindings[b].type) && l->bindings[b].type != ATRI_IR_BOOL && !atri_ir_is_aggregate(l->bindings[b].type) && !atri_ir_is_pointer(l->bindings[b].type)) {
            return error(l, e->span, "address-of requires a supported scalar or aggregate variable");
        }
        if (b != ATRI_NONE && !l->bindings[b].mutable) {
            return error(l, e->span, "cannot take the address of a compile-time constant or read-only parameter");
        }
    }
    AtriIRType target; AtriId storage;
    if (!location(l, id, true, &target, &storage)) { return false; }
    AtriIRType pointer = atri_ir_pointer_type(target);
    if (pointer == ATRI_IR_INVALID) { return error(l, e->span, "cannot form a pointer to this object type within the pointer nesting limit"); }
    return emit(l, (AtriIRInst){.kind = ATRI_IR_ADDRESS, .type = pointer, .left = storage, .span = e->span}, out);
}
static bool binary_values(Lower *l, AtriOp op, AtriId left, AtriId right, AtriSpan span, AtriId *out) {
    AtriIRType a = l->function->instructions[left].type, b = l->function->instructions[right].type;
    AtriIRInst inst = {.span = span, .op = op, .left = left, .right = right, .type = a};
    bool compare = op >= ATRI_EQ && op <= ATRI_GE;
    if (compare) {
        if (a != b) { return error(l, span, "type mismatch: comparison operands must have the same type"); }
        if (atri_ir_is_aggregate(a)) { return error(l, span, "struct/union/array equality and ordering are not automatically defined"); }
        if (!atri_ir_is_integer(a) && op != ATRI_EQ && op != ATRI_NE) { return error(l, span, "ordered comparisons require integer operands"); }
        if (!atri_ir_is_integer(a) && a != ATRI_IR_BOOL && !atri_ir_is_pointer(a)) { return error(l, span, "comparison requires scalar operands"); }
        inst.kind = ATRI_IR_COMPARE; inst.type = ATRI_IR_BOOL; return emit(l, inst, out);
    }
    if (atri_ir_is_pointer(a) || atri_ir_is_pointer(b)) {
        AtriIRType pointer = atri_ir_is_pointer(a) ? a : b;
        AtriIRType target = atri_ir_pointee(pointer);
        if (target == ATRI_IR_VOID || !atri_ir_valid_type(l->ir, target) || !atri_ir_type_size(l->ir, target)) {
            return error(l, span, "pointer arithmetic on zero-size or incomplete targets is not defined");
        }
        if (op != ATRI_ADD && op != ATRI_SUB) { return error(l, span, "pointer arithmetic supports only pointer +/- integer and pointer - pointer"); }
        if (atri_ir_is_pointer(a) && atri_ir_is_pointer(b)) {
            if (op != ATRI_SUB) { return error(l, span, "cannot add two pointers"); }
            if (a != b) { return error(l, span, "type mismatch: pointer difference requires the same pointer type"); }
            inst.kind = ATRI_IR_PTR_DIFF; inst.type = ATRI_IR_I64;
            return nonnull(l, left, span) && nonnull(l, right, span) && emit(l, inst, out);
        }
        if (op == ATRI_SUB && !atri_ir_is_pointer(a)) { return error(l, span, "cannot subtract a pointer from an integer"); }
        AtriId offset = atri_ir_is_pointer(a) ? right : left;
        AtriIRType offset_type = l->function->instructions[offset].type;
        if (offset_type != ATRI_IR_I32 && offset_type != ATRI_IR_I64) { return error(l, span, "pointer offset requires i32 or i64; use an explicit integer conversion"); }
        inst.kind = ATRI_IR_PTR_OFFSET; inst.type = pointer;
        inst.left = atri_ir_is_pointer(a) ? left : right; inst.right = offset;
        if (!nonnull(l, inst.left, span)) { return false; }
        const AtriIRInst *value = &l->function->instructions[offset];
        int64_t amount = atri_integer_signed(offset_type, value->immediate);
        int64_t stride = (int64_t)atri_ir_type_size(l->ir, target);
        if (value->kind == ATRI_IR_CONST && (amount < INT64_MIN/stride || amount > INT64_MAX/stride)) {
            return error(l, span, "pointer offset times element size exceeds the signed 64-bit range");
        }
        return emit(l, inst, out);
    }
    if (!atri_ir_is_integer(a) || !atri_ir_is_integer(b)) { return error(l, span, "type mismatch: integer operation requires integer operands"); }
    if (op != ATRI_SHL && op != ATRI_SHR && a != b) { return error(l, span, "type mismatch: integer operands must have the same type"); }
    inst.kind = ATRI_IR_BINARY; return emit(l, inst, out);
}
static bool expression(Lower *l, AtriId id, AtriIRType expected, AtriId *out) {
    const AtriExpr *e = &l->ast->expressions[id];
    if (++l->depth > 256) { --l->depth; return error(l, e->span, "expression nesting limit exceeded"); }
    AtriIRInst inst = {.span = e->span, .type = ATRI_IR_I32};
    bool ok = false;
    switch (e->kind) {
        case ATRI_EXPR_CAST: {
            AtriIRType target = conversion_type(l, e->text);
            if (!atri_ir_is_integer(target) && !atri_ir_is_pointer(target)) {
                error(l, e->text, "explicit conversions require an integer target or data pointer target type"); break;
            }
            if (atri_ir_is_pointer(target) && l->ast->expressions[e->left].kind == ATRI_EXPR_NULL) {
                inst.kind = ATRI_IR_CONST; inst.type = target; ok = emit(l, inst, out); break;
            }
            if (atri_ir_is_pointer(target) && l->constant_context) { error(l, e->span, "compile-time pointer conversions are not supported yet"); break; }
            if (!expression(l, e->left, ATRI_IR_INVALID, &inst.left)) { break; }
            AtriIRType source = l->function->instructions[inst.left].type;
            if (atri_ir_is_integer(target) ? !atri_ir_is_integer(source) : !atri_ir_is_pointer(source)) {
                error(l, e->span, "cast requires integer operands for integer conversions or pointer operands for data pointer conversions"); break;
            }
            inst.kind = ATRI_IR_CAST; inst.type = target; ok = emit(l, inst, out); break;
        }
        case ATRI_EXPR_SIZEOF: case ATRI_EXPR_ALIGNOF: {
            AtriIRType object;
            if (!type(l, e->text, &object)) { break; }
            inst.kind = ATRI_IR_CONST; inst.type = ATRI_IR_U64;
            inst.immediate = e->kind == ATRI_EXPR_SIZEOF ? atri_ir_type_size(l->ir, object) : atri_ir_type_align(l->ir, object);
            ok = emit(l, inst, out); break;
        }
        case ATRI_EXPR_RUN: ok = run(l, e, expected, false, out); break;
        case ATRI_EXPR_CONDITIONAL: ok = conditional(l, e, expected, out); break;
        case ATRI_EXPR_STRING: ok = string_literal(l, e, out); break;
        case ATRI_EXPR_BOOL:
            inst.kind = ATRI_IR_CONST; inst.type = ATRI_IR_BOOL; inst.immediate = e->boolean;
            ok = emit(l, inst, out); break;
        case ATRI_EXPR_CHARACTER: {
            size_t cursor = e->text.start+1;
            inst.kind = ATRI_IR_CONST; inst.type = ATRI_IR_U8;
            inst.immediate = atri_literal_byte(l->source, &cursor);
            ok = emit(l, inst, out); break;
        }
        case ATRI_EXPR_INTEGER:
            inst.kind = ATRI_IR_CONST;
            inst.type = atri_ir_is_integer(expected) ? expected : ATRI_IR_I32;
            if (literal(l, e->text, inst.type, atri_integer_max(inst.type), &inst.immediate)) ok = emit(l, inst, out);
            break;
        case ATRI_EXPR_NULL:
            if (!atri_ir_is_pointer(expected)) {
                error(l, e->span, "null requires a pointer type context");
            } else {
                inst.kind = ATRI_IR_CONST; inst.type = expected; ok = emit(l, inst, out);
            }
            break;
        case ATRI_EXPR_NAME: {
            if (atri_enum_member(l->source, l->ast, l->ir, e, &inst.type, &inst.immediate)) {
                inst.kind = ATRI_IR_CONST; ok = emit(l, inst, out); break;
            }
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
            if (b == ATRI_NONE) {
                error(l, e->text, l->ast->generator_scope ? "unknown generator input (program globals are unavailable)" :
                      "unknown local name (locals must be declared before use)");
            }
            else if (l->bindings[b].unavailable || (l->constant_context && !l->bindings[b].constant))
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
            inst.kind = ATRI_IR_UNARY; inst.op = e->op;
            inst.type = infer_type(l, e->left, expected);
            if (!atri_ir_is_integer(inst.type)) { inst.type = ATRI_IR_I32; }
            if (e->op == ATRI_ADD) { ok = expression(l, e->left, inst.type, out); break; }
            if (e->op == ATRI_NEG && l->ast->expressions[e->left].kind == ATRI_EXPR_INTEGER) {
                inst.kind = ATRI_IR_CONST;
                uint64_t limit = atri_integer_max(inst.type) + (uint64_t)atri_integer_is_signed(inst.type);
                if (!literal(l, l->ast->expressions[e->left].text, inst.type, limit, &inst.immediate)) { break; }
                uint64_t negated;
                if (!atri_integer_compute(inst.type, ATRI_NEG, inst.immediate, 0, &negated)) { break; }
                inst.immediate = negated; ok = emit(l, inst, out);
            } else if (expression(l, e->left, inst.type, &inst.left)) { ok = emit(l, inst, out); }
            break;
        case ATRI_EXPR_BINARY: {
            if (e->custom) { ok = error(l, e->text, "custom operator must be rewritten by a declaration listener"); break; }
            if (e->op == ATRI_AND || e->op == ATRI_OR) { ok = logical(l, e, out); break; }
            AtriIRType a = infer_type(l, e->left, ATRI_IR_INVALID), b = infer_type(l, e->right, ATRI_IR_INVALID);
            bool compare = e->op >= ATRI_EQ && e->op <= ATRI_GE;
            if (!compare && (l->ast->expressions[e->left].kind == ATRI_EXPR_NULL || l->ast->expressions[e->right].kind == ATRI_EXPR_NULL)) {
                error(l, e->span, "null pointer arithmetic is invalid"); break;
            }
            if (compare) {
                AtriIRType operand = a != ATRI_IR_INVALID ? a : b != ATRI_IR_INVALID ? b : ATRI_IR_I32;
                a = operand; b = operand;
            } else if (atri_ir_is_pointer(a) || atri_ir_is_pointer(b)) {
                if (a == ATRI_IR_INVALID) { a = ATRI_IR_I64; }
                if (b == ATRI_IR_INVALID) { b = ATRI_IR_I64; }
            } else if (e->op == ATRI_SHL || e->op == ATRI_SHR) {
                if (a == ATRI_IR_INVALID) { a = atri_ir_is_integer(expected) ? expected : ATRI_IR_I32; }
                if (b == ATRI_IR_INVALID) { b = ATRI_IR_I32; }
            } else {
                AtriIRType operand = a != ATRI_IR_INVALID ? a : b != ATRI_IR_INVALID ? b :
                    atri_ir_is_integer(expected) ? expected : ATRI_IR_I32;
                a = operand; b = operand;
            }
            if (expression(l, e->left, a, &inst.left) && expression(l, e->right, b, &inst.right)) {
                ok = binary_values(l, e->op, inst.left, inst.right, e->span, out);
            }
            break;
        }
        case ATRI_EXPR_CALL: {
            if (!e->qualifier.end && lookup(l, e->text) != ATRI_NONE) { error(l, e->text, "local value is not callable"); break; }
            AtriId callee = resolve_declaration(l, e, false);
            if (callee == ATRI_NONE) {
                error(l, e->text, e->keyword ? "#keyword is available inside a declaration listener plugin" : "unknown function name");
                break;
            }
            const AtriIRFunction *f = &l->ir->functions[callee];
            if (e->argument_count < f->parameter_count || (!f->variadic && e->argument_count != f->parameter_count)) { error(l, e->span, "function argument count does not match its signature"); break; }
            AtriId *args = e->argument_count == 0 ? NULL : malloc(e->argument_count * sizeof(*args));
            if (e->argument_count != 0 && args == NULL) { error(l, e->span, "out of memory"); break; }
            ok = true;
            for (size_t a = 0; a < e->argument_count; ++a) {
                AtriId operand = l->ast->arguments[e->first_argument+a];
                AtriIRType expected_argument = a < f->parameter_count ? f->parameter_types[a] : ATRI_IR_INVALID;
                if (!expression(l, operand, expected_argument, &args[a])) {
                    if (a < f->parameter_count && callee < l->ast->function_count) {
                        AtriId parameter = l->ast->functions[callee].first_parameter + a;
                        atri_diagnostic_note(l->diagnostic, l->ast->parameters[parameter].type, "parameter declared here");
                    }
                    ok = false;
                    break;
                }
                if (a < f->parameter_count) { continue; }
                AtriIRType argument = l->function->instructions[args[a]].type;
                AtriSpan span = l->ast->expressions[operand].span;
                if (argument == ATRI_IR_BOOL || (atri_ir_is_integer(argument) && atri_integer_width(argument) < 32)) {
                    if (!emit(l, (AtriIRInst){.kind = ATRI_IR_C_PROMOTE, .type = ATRI_IR_I32, .left = args[a], .span = span}, &args[a])) { ok = false; break; }
                } else if (argument == ATRI_IR_VOID || atri_ir_is_array(argument) || !c_value_compatible(l->ir, argument)) {
                    ok = error(l, span, "variadic argument requires a supported C ABI value; arrays require an explicit element pointer"); break;
                }
            }
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
        char want[ATRI_IR_TYPE_NAME_SIZE], got[ATRI_IR_TYPE_NAME_SIZE];
        ok = error(l, e->span, "type mismatch");
        snprintf(l->diagnostic->detail, sizeof(l->diagnostic->detail), "expected %s, got %s",
                 atri_ir_type_name(expected, want), atri_ir_type_name(l->function->instructions[*out].type, got));
    }
    --l->depth; return ok;
}
static bool store(Lower *l, AtriId pointer, AtriId value, AtriSpan span) {
    AtriId ignored;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_STORE, .span = span,
        .left = pointer, .right = value, .type = l->function->instructions[value].type}, &ignored);
}
static bool statement(Lower *l, AtriId id);
static bool block(Lower *l, AtriId id, bool nested);
/* 块捕获的常量直接引用外层求值请求；嵌套捕获也经过同一条依赖链。 */
static bool block_call(Lower *l, const AtriExpr *e, AtriId *out) {
    AtriId *slot = &l->block_functions[e->alternative];
    if (!*slot) {
        AtriId id = l->ir->function_count++;
        *slot = id + 1;
        AtriIRFunction *f = &l->ir->functions[id];
        *f = (AtriIRFunction){.name = e->span, .return_type = ATRI_IR_VOID, .block_count = 1};
        if (e->text.end && !type(l, e->text, &f->return_type)) { return false; }
        AtriFunction context = {.name = e->span, .body = l->ast->blocks[e->left].span};
        Lower *sub = calloc(1, sizeof(*sub));
        if (!sub) { return error(l, e->span, "out of memory compiling #run block"); }
        sub->source = l->source;
        sub->ast = l->ast;
        sub->ir = l->ir;
        sub->function = f;
        sub->ast_function = &context;
        sub->visible_binding = ATRI_NONE;
        sub->memory = true;
        sub->block_functions = l->block_functions;
        sub->diagnostic = l->diagnostic;
        sub->case_values = l->case_values;
        sub->case_count = l->case_count;
        sub->case_capacity = l->case_capacity;
        bool ok = true;
        for (AtriId b = l->visible_binding; ok && b != ATRI_NONE; b = l->bindings[b].previous) {
            Binding captured = l->bindings[b];
            if (lookup(sub, captured.name) != ATRI_NONE) { continue; }
            captured.address = ATRI_NONE;
            captured.mutable = false;
            captured.unavailable = !captured.constant;
            if (captured.constant) {
                size_t c = 0;
                while (c < l->function->constant_count && l->function->constants[c].result != captured.value) { ++c; }
                if (c == l->function->constant_count) { ok = error(l, captured.name, "invalid #run capture"); break; }
                if (!emit(sub, (AtriIRInst){.kind = ATRI_IR_CONST, .type = captured.type, .span = captured.name},
                          &captured.value)) { ok = false; break; }
                AtriIRConstant ref = {.capture_owner = (size_t)(l->function - l->ir->functions) + 1, .expression = c,
                    .span = captured.name, .result = captured.value, .first_instruction = captured.value};
                if (!atri_append((void **)&f->constants, &f->constant_count, &f->constant_capacity, sizeof(ref), &ref)) {
                    ok = error(l, captured.name, "out of memory capturing constant"); break;
                }
            }
            ok = bind(sub, captured);
        }
        sub->scope++;
        if (ok) { ok = block(sub, e->left, false); }
        if (ok && !sub->closed) {
            AtriId ignored;
            AtriIRInst end = {.kind = f->return_type == ATRI_IR_VOID ? ATRI_IR_RETURN : ATRI_IR_UNREACHABLE,
                .span = e->span, .type = ATRI_IR_VOID, .left = ATRI_NONE};
            ok = emit(sub, end, &ignored);
        }
        l->case_values = sub->case_values; l->case_count = sub->case_count; l->case_capacity = sub->case_capacity;
        free(sub->bindings); free(sub->defers); free(sub);
        if (!ok) { return false; }
    }
    AtriId id = *slot - 1;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_CALL, .callee = id, .span = e->span,
        .type = l->ir->functions[id].return_type}, out);
}
static bool defer_body(Lower *l, const AtriStatement *s) {
    AtriId ignored;
    if (s->body != ATRI_NONE) { return block(l, s->body, true); }
    return expression(l, s->target, ATRI_IR_INVALID, &ignored);
}
static bool run_defer(Lower *l, const AtriStatement *s) {
    if (s->expression == ATRI_NONE) { return defer_body(l, s); }
    AtriId condition, body = new_block(l), end = new_block(l);
    return expression(l, s->expression, ATRI_IR_BOOL, &condition) &&
        branch(l, condition, body, end, s->span) && label(l, body, s->span) &&
        defer_body(l, s) && jump(l, end, s->span) && label(l, end, s->span);
}
static bool clean_up(Lower *l, size_t first) {
    if (first == l->defer_count) { return true; }
    AtriId saved_binding = l->visible_binding;
    size_t saved_floor = l->loop_floor;
    l->loop_floor = l->loop_count;
    ++l->defer_depth;
    /* 绑定链从登记点开始查找，存储仍是原对象。延迟体的新绑定和 defer 追加在当前列表末尾。 */
    for (size_t i = l->defer_count; i-- > first;) {
        Deferred deferred = l->defers[i];
        l->visible_binding = deferred.binding;
        if (l->function->block_count > 4090) {
            return error(l, deferred.statement->span, "control flow exceeds the prototype limit of 4096 blocks");
        }
        if (!run_defer(l, deferred.statement)) { return false; }
    }
    --l->defer_depth;
    l->loop_floor = saved_floor;
    l->visible_binding = saved_binding;
    return true;
}
static bool switch_statement(Lower *l, const AtriStatement *s) {
    AtriId selected;
    if (!expression(l, s->expression, ATRI_IR_INVALID, &selected)) { return false; }
    AtriIRType type = l->function->instructions[selected].type;
    if (!atri_ir_is_integer(type)) { return error(l, s->span, "switch requires an integer value (including enum members)"); }
    if (l->loop_count == 256) { return error(l, s->span, "loop/switch nesting limit exceeded"); }
    AtriId *bodies = malloc((s->case_count ? s->case_count : 1)*sizeof(*bodies));
    if (!bodies) { return error(l, s->span, "out of memory"); }
    bool ok = false;
    AtriId end = new_block(l), fallback = end;
    for (size_t i = 0; i < s->case_count; ++i) {
        bodies[i] = new_block(l);
        if (l->ast->cases[s->first_case+i].expression == ATRI_NONE) { fallback = bodies[i]; }
    }
    /* 同一 defer 中的 switch 可被多个退出边各降低一次，每份单独检查 case 重值。 */
    size_t case_group = l->case_count;
    for (size_t i = 0; i < s->case_count; ++i) {
        const AtriCase *item = &l->ast->cases[s->first_case+i];
        if (item->expression == ATRI_NONE) { continue; }
        AtriId constant, comparison, next = new_block(l);
        if (!compile_time_value(l, item->expression, item->span, type, false, &constant)) { goto done; }
        CaseValue value = {.function = (size_t)(l->function-l->ir->functions), .result = constant,
            .group = case_group, .span = item->span};
        if (!atri_append((void **)&l->case_values, &l->case_count, &l->case_capacity, sizeof(value), &value)) { error(l, item->span, "out of memory"); goto done; }
        if (!binary_values(l, ATRI_EQ, selected, constant, item->span, &comparison) ||
            !branch(l, comparison, bodies[i], next, item->span) || !label(l, next, item->span)) { goto done; }
    }
    if (!jump(l, fallback, s->span)) { goto done; }
    for (size_t i = 0; i < s->case_count; ++i) {
        const AtriCase *item = &l->ast->cases[s->first_case+i];
        if (!label(l, bodies[i], item->span)) { goto done; }
        /* again == NONE 表示 switch：break 可使用，continue 必须继续寻找外围循环。 */
        l->loops[l->loop_count++] = (Loop){end, ATRI_NONE, l->defer_count};
        bool body_ok = block(l, item->body, true);
        --l->loop_count;
        if (!body_ok) { goto done; }
        bool shared = !l->ast->blocks[item->body].statement_count;
        AtriId target = (item->through || shared) && i+1 < s->case_count ? bodies[i+1] : end;
        if (!jump(l, target, item->span)) { goto done; }
    }
    ok = label(l, end, s->span);
done:
    free(bodies); return ok;
}
static bool block(Lower *l, AtriId id, bool nested) {
    size_t saved = l->binding_count, first_defer = l->defer_count;
    AtriId saved_binding = l->visible_binding;
    if (nested) ++l->scope;
    const AtriBlock *b = &l->ast->blocks[id];
    for (size_t i = 0; i < b->statement_count; ++i) {
        /* return/break 后的源码仍要检查；另开无前驱块，避免把这些指令接到已终结的路径上。 */
        if (l->closed && !label(l, new_block(l), b->span)) return false;
        if (!statement(l, l->ast->block_statements[b->first_statement+i])) return false;
    }
    if (!l->closed && !clean_up(l, first_defer)) { return false; }
    l->defer_count = first_defer;
    if (nested) { l->binding_count = saved; l->visible_binding = saved_binding; --l->scope; }
    return true;
}
static bool simple(Lower *l, const AtriStatement *s) {
    if (s->kind == ATRI_STMT_EXPR) {
        AtriId ignored;
        const AtriExpr *e = &l->ast->expressions[s->expression];
        return e->kind == ATRI_EXPR_RUN ? run(l, e, ATRI_IR_INVALID, true, &ignored) :
            expression(l, s->expression, ATRI_IR_INVALID, &ignored);
    }
    if (s->kind == ATRI_STMT_RETURN) {
        if (l->defer_depth) { return error(l, s->span, "cannot return from a defer body"); }
        if (l->function->return_type == ATRI_IR_VOID) {
            if (s->expression != ATRI_NONE) { return error(l, s->span, "void functions require 'return;' without a value"); }
            if (!clean_up(l, 0)) { return false; }
            AtriId ignored; l->closed = true;
            return emit(l, (AtriIRInst){.kind = ATRI_IR_RETURN, .type = ATRI_IR_VOID, .left = ATRI_NONE, .span = s->span}, &ignored);
        }
        if (s->expression == ATRI_NONE) { return error(l, s->span, "non-void functions must return a value"); }
    }
    AtriIRType expected;
    AtriId destination = ATRI_NONE, binding = ATRI_NONE;
    bool declaration = s->kind == ATRI_STMT_VARIABLE || s->kind == ATRI_STMT_CONSTANT;
    if (declaration) {
        if (!type(l, s->type, &expected)) return false;
        if (s->kind == ATRI_STMT_CONSTANT && (atri_ir_is_pointer(expected) || atri_ir_is_aggregate(expected))) {
            return error(l, s->span, "compile-time pointer constants, struct constants, union constants and array constants are not supported yet");
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
    AtriId previous = ATRI_NONE;
    if (s->compound) {
        if (destination != ATRI_NONE) {
            if (!emit(l, (AtriIRInst){.kind = ATRI_IR_LOAD, .type = expected, .left = destination, .span = s->span}, &previous)) { return false; }
        } else { previous = l->bindings[binding].value; }
    }
    size_t first = l->function->instruction_count;
    AtriId value;
    l->constant_context = s->kind == ATRI_STMT_CONSTANT;
    if (s->expression == ATRI_NONE && atri_ir_is_aggregate(expected)) {
        AtriExpr empty = {.kind = ATRI_EXPR_INITIALIZER, .span = s->span};
        if (!initialize_aggregate(l, &empty, expected, &value)) { return false; }
    } else if (s->expression == ATRI_NONE) {
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .span = s->span, .type = expected}, &value)) return false;
    } else {
        AtriIRType right_type = expected;
        if (s->compound && (atri_ir_is_pointer(expected) || s->op == ATRI_SHL || s->op == ATRI_SHR)) {
            right_type = infer_type(l, s->expression, ATRI_IR_INVALID);
            if (right_type == ATRI_IR_INVALID) { right_type = atri_ir_is_pointer(expected) ? ATRI_IR_I64 : ATRI_IR_I32; }
        }
        if (!expression(l, s->expression, right_type, &value)) { return false; }
        if (s->compound) {
            if (!binary_values(l, s->op, previous, value, s->span, &value)) { return false; }
            if (l->function->instructions[value].type != expected) { return error(l, s->span, "type mismatch: compound assignment must preserve the target type"); }
        }
    }
    l->constant_context = false;
    if (s->kind == ATRI_STMT_CONSTANT) {
        /* 后续引用先指向稳定的结果占位；全部函数降低后，再求值并清除前面的初始化指令范围。 */
        AtriId result;
        if (!emit(l, (AtriIRInst){.kind = ATRI_IR_CONST, .span = s->span, .type = expected}, &result)) return false;
        AtriIRConstant c = {.span = s->span, .expression = value, .result = result, .first_instruction = first};
        if (!add_lexical_constant(l, c)) { return false; }
        value = result;
    }
    if (declaration) {
        AtriId local = ATRI_NONE;
        if (s->kind == ATRI_STMT_VARIABLE && (l->memory || atri_ir_is_aggregate(expected) || address_taken(l, s->name))) {
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
    AtriId returned = value;
    while (l->function->instructions[returned].kind == ATRI_IR_CAST && atri_ir_is_pointer(l->function->instructions[returned].type)) {
        returned = l->function->instructions[returned].left;
    }
    if (atri_ir_is_pointer(expected) && l->function->instructions[returned].kind == ATRI_IR_ADDRESS) {
        AtriId origin = l->function->instructions[returned].left;
        while (l->function->instructions[origin].kind == ATRI_IR_FIELD || l->function->instructions[origin].kind == ATRI_IR_INDEX) { origin = l->function->instructions[origin].left; }
        if (l->function->instructions[origin].kind == ATRI_IR_LOCAL && l->function->instructions[origin].immediate == 0) {
            return error(l, s->span, "cannot directly return the address of a local variable");
        }
    }
    /* 先取得返回值（聚合为已有的值快照），延迟体可继续修改原绑定。 */
    if (!clean_up(l, 0)) { return false; }
    AtriId ignored; l->closed = true;
    return emit(l, (AtriIRInst){.kind = ATRI_IR_RETURN, .span = s->span, .type = expected, .left = value}, &ignored);
}
static bool statement(Lower *l, AtriId id) {
    const AtriStatement *s = &l->ast->statements[id];
    if (l->function->block_count > 4090) return error(l, s->span, "control flow exceeds the prototype limit of 4096 blocks");
    switch (s->kind) {
        case ATRI_STMT_SWITCH: return switch_statement(l, s);
        case ATRI_STMT_BLOCK: return block(l, s->body, true);
        case ATRI_STMT_DEFER: {
            Deferred deferred = {.statement = s, .binding = l->visible_binding};
            return atri_append((void **)&l->defers, &l->defer_count, &l->defer_capacity, sizeof(deferred), &deferred) ||
                error(l, s->span, "out of memory while registering defer");
        }
        case ATRI_STMT_BREAK: case ATRI_STMT_CONTINUE: {
            for (size_t i = l->loop_count; i-- > l->loop_floor;) {
                Loop target = l->loops[i];
                AtriId destination = s->kind == ATRI_STMT_BREAK ? target.stop : target.again;
                if (destination != ATRI_NONE) {
                    return clean_up(l, target.defer_count) && jump(l, destination, s->span);
                }
            }
            if (l->defer_depth) { return error(l, s->span, "break and continue in a defer body require a target inside that body"); }
            return error(l, s->span, s->kind == ATRI_STMT_BREAK ? "break requires an enclosing loop or switch" :
                "continue requires an enclosing loop, not only a switch");
        }
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
            AtriId saved_binding = l->visible_binding;
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
            l->loops[l->loop_count++] = (Loop){end, s->kind == ATRI_STMT_FOR ? step_block : cond_block, l->defer_count};
            if (!block(l, s->body, true)) return false;
            --l->loop_count;
            if (!jump(l, step_block, s->span) || !label(l, step_block, s->span)) return false;
            if (s->step != ATRI_NONE && !statement(l, s->step)) return false;
            if (!jump(l, cond_block, s->span) || !label(l, end, s->span)) return false;
            l->binding_count = saved; l->visible_binding = saved_binding; --l->scope; return true;
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
    if (!reachable || !map) { free(reachable); free(map); *d = (AtriDiagnostic){.span = f->name, .message = "out of memory"}; return false; }
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
    if (v->kind == ATRI_IR_CAST) {
        uint64_t source;
        if (!known_integer(f, v->left, depth+1, steps, &source)) { return false; }
        *out = atri_integer_convert(f->instructions[v->left].type, v->type, source); return true;
    }
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
                if (!atri_integer_is_signed(f->instructions[v->right].type)) {
                    if (bits >= array.length) { return error(l, v->span, "array index is statically out of bounds"); }
                    continue;
                }
                int64_t index = atri_integer_signed(f->instructions[v->right].type, bits);
                if (index < 0 || (uint64_t)index >= array.length) { return error(l, v->span, "array index is statically out of bounds"); }
            }
        }
    }
    return true;
}
static bool check_shifts(Lower *l) {
    for (size_t n = 0; n < l->ir->function_count; ++n) {
        const AtriIRFunction *f = &l->ir->functions[n];
        for (size_t i = 0; i < f->instruction_count; ++i) {
            const AtriIRInst *v = &f->instructions[i];
            if (v->kind != ATRI_IR_BINARY || (v->op != ATRI_SHL && v->op != ATRI_SHR)) { continue; }
            uint64_t count;
            size_t steps = 100000;
            if (known_integer(f, v->right, 0, &steps, &count) && count >= atri_integer_width(v->type)) {
                return error(l, v->span, "shift count is outside the operand width");
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
bool atri_lower(AtriSource source, AtriProgram *ast, AtriIRProgram *out, AtriDiagnostic *d) {
    *out = (AtriIRProgram){0}; *d = (AtriDiagnostic){0};
    AtriIRProgram ir = {.source = source, .lowering_ast = ast, .function_count = ast->function_count + ast->global_count + ast->field_count + ast->run_count, .global_count = ast->global_count};
    size_t capacity = ir.function_count + ast->run_block_count;
    ir.functions = calloc(capacity ? capacity : 1, sizeof(*ir.functions));
    if (!ir.functions) { *d = (AtriDiagnostic){.span = {0,0}, .message = "out of memory"}; return false; }
    Lower l = {.source = source, .ast = ast, .ir = &ir, .diagnostic = d, .static_steps = 100000};
    if (ast->unresolved_operator.end && !ast->generator_scope) {
        error(&l, ast->unresolved_operator, "custom operator must be rewritten by a declaration listener"); goto failure;
    }
    if (ast->run_block_count) {
        l.block_functions = calloc(ast->run_block_count, sizeof(*l.block_functions));
        if (!l.block_functions) { error(&l, (AtriSpan){0}, "out of memory preparing #run blocks"); goto failure; }
    }
    if (!atri_prepare_symbols(source, ast, d)) { goto failure; }
    if (!atri_layout_structs(source, ast, &ir, d)) { goto failure; }
    ir.globals = calloc(ast->global_count ? ast->global_count : 1, sizeof(*ir.globals));
    if (!ir.globals) { error(&l, (AtriSpan){0}, "out of memory"); goto failure; }
    for (size_t i = 0; i < ast->global_count; ++i) {
        const AtriStatement *g = &ast->globals[i].declaration;
        ir.globals[i].constant = g->kind == ATRI_STMT_CONSTANT;
        ir.globals[i].name = g->name;
        if (!type(&l, g->type, &ir.globals[i].type)) { goto failure; }
        bool aggregate = atri_ir_is_aggregate(ir.globals[i].type);
        if (ir.globals[i].constant && (atri_ir_is_pointer(ir.globals[i].type) || aggregate)) {
            error(&l, g->span, "compile-time pointer constants, struct constants, union constants and array constants are not supported yet"); goto failure;
        }
        if (aggregate) {
            ir.globals[i].data_size = atri_ir_type_size(&ir, ir.globals[i].type);
            ir.globals[i].data = calloc(ir.globals[i].data_size ? ir.globals[i].data_size : 1, 1);
            if (!ir.globals[i].data) { error(&l, g->span, "out of memory for static aggregate storage"); goto failure; }
        }
        ir.functions[ast->function_count+i] = (AtriIRFunction){.name = g->name,
            .return_type = aggregate ? ATRI_IR_I32 : ir.globals[i].type, .block_count = 1};
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
        signature->external_c = f->external_c; signature->variadic = f->variadic; signature->block_count = f->external_c ? 0 : 1;
        if (f->return_type.end-f->return_type.start == 4 && !memcmp(source.text+f->return_type.start, "void", 4)) {
            signature->return_type = ATRI_IR_VOID;
        } else if (!type(&l, f->return_type, &signature->return_type)) { goto failure; }
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
        l.function = &ir.functions[i]; l.ast_function = f; l.binding_count = 0; l.visible_binding = ATRI_NONE; l.scope = 0;
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
            AtriIRInst end = {.kind = ATRI_IR_UNREACHABLE, .span = f->body};
            if (l.function->return_type == ATRI_IR_VOID) { end.kind = ATRI_IR_RETURN; end.type = ATRI_IR_VOID; end.left = ATRI_NONE; }
            if (!emit(&l, end, &ignored)) { goto failure; }
        }
    }
    /* 顶层 #run 和全局/字段初值借用同一解释器，求值后删除临时函数。 */
    for (size_t i = 0; i < ast->run_count; ++i) {
        const AtriStatement *s = &ast->statements[ast->runs[i]];
        AtriFunction context = {.name = s->span};
        l.ast_function = &context;
        l.function = &ir.functions[ast->function_count+ast->global_count+ast->field_count+i];
        *l.function = (AtriIRFunction){.name = s->span, .return_type = ATRI_IR_I32, .block_count = 1};
        l.binding_count = 0; l.visible_binding = ATRI_NONE; l.constant_context = false;
        AtriId zero, ignored;
        if (!simple(&l, s) || !emit(&l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = ATRI_IR_I32, .span = s->span}, &zero) ||
            !emit(&l, (AtriIRInst){.kind = ATRI_IR_RETURN, .type = ATRI_IR_I32, .left = zero, .span = s->span}, &ignored)) { goto failure; }
    }
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
        l.binding_count = 0; l.visible_binding = ATRI_NONE; l.constant_context = true;
        AtriId value, result, ignored;
        if (i < ast->global_count && atri_ir_is_aggregate(ir.globals[i].type)) {
            if (!static_initializer(&l, i, ir.globals[i].type, g->expression, ATRI_NONE, 0, 0, g->span) ||
                !emit(&l, (AtriIRInst){.kind = ATRI_IR_CONST, .type = ATRI_IR_I32, .span = g->span}, &value) ||
                !emit(&l, (AtriIRInst){.kind = ATRI_IR_RETURN, .type = ATRI_IR_I32, .left = value, .span = g->span}, &ignored)) { goto failure; }
            continue;
        }
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
    free(l.block_functions); free(l.bindings); free(l.defers);
    if (!atri_resolve_constants(&ir, d) || !atri_ir_verify(&ir, d)) { free(l.case_values); free(l.static_values); atri_ir_destroy(&ir); return false; }
    for (size_t i = 0; i < l.case_count; ++i) {
        CaseValue *value = &l.case_values[i];
        value->bits = ir.functions[value->function].instructions[value->result].immediate;
    }
    if (l.case_count) { qsort(l.case_values, l.case_count, sizeof(*l.case_values), case_order); }
    for (size_t i = 1; i < l.case_count; ++i) {
        if (l.case_values[i-1].group == l.case_values[i].group && l.case_values[i-1].bits == l.case_values[i].bits) {
            error(&l, l.case_values[i].span, "duplicate switch case value after compile-time evaluation");
            free(l.case_values); free(l.static_values); atri_ir_destroy(&ir); return false;
        }
    }
    free(l.case_values);
    for (size_t i = 0; i < ast->global_count+ast->field_count; ++i) {
        AtriIRFunction *initializer = &ir.functions[ast->function_count+i];
        uint64_t bits = initializer->instructions[initializer->instruction_count-2].immediate;
        if (i < ast->global_count) {
            if (!atri_ir_is_aggregate(ir.globals[i].type)) { ir.globals[i].bits = bits; }
        }
        else { ir.fields[i-ast->global_count].default_bits = bits; }
    }
    for (size_t i = 0; i < l.static_count; ++i) {
        const StaticValue *value = &l.static_values[i];
        uint64_t bits = ir.functions[ast->function_count+value->global].instructions[value->result].immediate;
        /* 当前目标为小端 Windows x64；不能 memcpy 宿主整数来决定目标对象表示。 */
        size_t width = atri_ir_type_size(&ir, value->type);
        for (size_t b = 0; b < width; ++b) { ir.globals[value->global].data[value->offset+b] = (unsigned char)(bits >> (b*8)); }
    }
    free(l.static_values);
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
    if (!check_shifts(&l) || !atri_ir_verify(&ir, d)) { atri_ir_destroy(&ir); return false; }
    ir.lowering_ast = NULL;
    *out = ir; return true;
failure:
    free(l.block_functions); free(l.case_values); free(l.static_values); free(l.bindings); free(l.defers); atri_ir_destroy(&ir); return false;
}
