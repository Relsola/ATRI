#include "vm_bytecode.h"
#include "array.h"
#include "vm_memory.h"

typedef struct {
    AtriId function;
    const AtriVmFunction *body;
    const uint64_t *parameters;
    uint64_t *values, *locals;
    unsigned char *states;
    AtriVmMemory *memory;
    uint64_t root_context;
    size_t slots;
} Frame;

/* 每个等待点保留帧和 PC。依赖返回后继续下一阶段，不重新执行先前的调用或实参。 */
typedef enum { EVALUATE_VALUE, EXECUTE_BODY, RESOLVE_CONSTANT } TaskKind;
typedef enum { START, LEFT_READY, RIGHT_READY, ARGUMENT_READY, RESULT_READY } Resume;
typedef struct {
    TaskKind kind;
    Resume resume;
    Frame *frame;
    AtriId id;
    size_t argument;
    uint64_t *arguments;
} Task;
typedef struct {
    AtriIRProgram *program;
    AtriDiagnostic *diagnostic;
    AtriVmFunction *functions;
    AtriVmMemory memory;
    Task *tasks;
    size_t task_count, capacity, value_depth, remaining_steps;
    uint64_t result;
} Evaluation;

static bool error(Evaluation *e, AtriSpan span, const char *message) {
    *e->diagnostic = (AtriDiagnostic){.span = span, .message = message};
    return false;
}
static bool step(Evaluation *e, AtriSpan span) {
    if (!e->remaining_steps) {
        return error(e, span, "compile-time evaluation exceeded its step limit");
    }
    --e->remaining_steps;
    return true;
}
static void destroy_frame(Frame *frame) {
    if (frame) {
        if (frame->root_context) { atri_vm_drop(frame->memory, frame->root_context); }
        for (size_t i = 0; frame->locals && i < frame->slots; ++i) {
            if (frame->locals[i]) { atri_vm_drop(frame->memory, frame->locals[i]); }
        }
        free(frame->values);
        free(frame->states);
        free(frame->locals);
        free(frame);
    }
}
static Frame *create_frame(Evaluation *e, AtriId function, const uint64_t *args) {
    AtriIRFunction *f = &e->program->functions[function];
    AtriVmFunction *body = &e->functions[function];
    if (!atri_vm_prepare(f, body, e->diagnostic)) {
        return NULL;
    }
    Frame *frame = calloc(1, sizeof(*frame));
    if (frame) {
        frame->function = function;
        frame->slots = f->instruction_count;
        frame->memory = &e->memory;
        frame->body = body;
        frame->parameters = args;
        frame->values = calloc(f->instruction_count, sizeof(*frame->values));
        frame->states = calloc(f->instruction_count, sizeof(*frame->states));
        frame->locals = calloc(f->instruction_count, sizeof(*frame->locals));
        if (frame->values && frame->states && frame->locals) {
            return frame;
        }
    }
    destroy_frame(frame);
    error(e, f->name, "out of memory during compile-time evaluation");
    return NULL;
}
static bool push(Evaluation *e, Task task, AtriSpan span) {
    if (!atri_append((void **)&e->tasks, &e->task_count, &e->capacity, sizeof(task), &task)) {
        if (task.kind != EVALUATE_VALUE) {
            destroy_frame(task.frame);
        }
        return error(e, span, "out of memory scheduling compile-time evaluation");
    }
    return true;
}
static bool demand(Evaluation *e, Frame *frame, AtriId id) {
    AtriSpan span = e->program->functions[frame->function].instructions[id].span;
    if (!step(e, span)) {
        return false;
    }
    if (++e->value_depth > 256) {
        return error(e, span, "compile-time evaluation exceeded its recursion/dependency depth limit");
    }
    return push(e, (Task){.kind = EVALUATE_VALUE, .frame = frame, .id = id}, span);
}
/* 前缀快照与最终程序的函数编号不同。用模块、声明位置和义务序号识别同一求值请求。 */
typedef struct {
    char *path;
    size_t owner, site, ordinal;
    AtriIRType type;
    bool view;
    uint64_t bits;
    unsigned char *bytes;
    size_t size;
} SavedRun;
struct AtriRunCache { SavedRun *items; size_t count, capacity; };
struct AtriRunCache *atri_run_cache_create(void) { return calloc(1, sizeof(struct AtriRunCache)); }
void atri_run_cache_destroy(struct AtriRunCache *cache) {
    if (!cache) { return; }
    for (size_t i = 0; i < cache->count; ++i) { free(cache->items[i].path); free(cache->items[i].bytes); }
    free(cache->items); free(cache);
}
static SavedRun run_key(const Evaluation *e, AtriId id, size_t ordinal) {
    const AtriProgram *ast = e->program->lowering_ast;
    const AtriIRFunction *f = &e->program->functions[id];
    const AtriIRConstant *c = &f->constants[ordinal];
    AtriModule m = ast->modules[atri_module_at(ast, c->span.start)];
    return (SavedRun){.path = m.path, .owner = f->name.start - m.span.start, .site = c->span.start - m.span.start,
        .ordinal = ordinal, .type = f->instructions[c->result].type,
        .view = f->instructions[c->result].type == e->program->string_type};
}
static const SavedRun *find_run(const struct AtriRunCache *cache, SavedRun key) {
    for (size_t i = 0; i < cache->count; ++i) {
        const SavedRun *s = &cache->items[i];
        if (key.site == s->site && key.owner == s->owner && key.ordinal == s->ordinal &&
            key.view == s->view && (key.view || key.type == s->type) && !strcmp(key.path, s->path)) { return s; }
    }
    return NULL;
}
static bool save_run(Evaluation *e, AtriId id, size_t ordinal) {
    const AtriProgram *ast = e->program->lowering_ast;
    AtriIRConstant *c = &e->program->functions[id].constants[ordinal];
    if (!ast || !ast->run_cache || ast->owns_run_cache || c->capture_owner) { return true; }
    SavedRun item = run_key(e, id, ordinal);
    if (find_run(ast->run_cache, item)) { return true; }
    char *path = malloc(strlen(item.path) + 1);
    if (path) { strcpy(path, item.path); }
    item.path = path; item.bits = c->bits;
    const unsigned char *data = NULL;
    if (item.view) {
        if (!atri_vm_view(&e->memory, c->bits, &data, &item.size)) { free(path); return error(e, c->span, "expired compile-time string"); }
    } else if (item.type == ATRI_IR_PTR_U8 && c->bits) {
        data = atri_vm_bytes(&e->memory, c->bits, 0, false);
        if (!data) { free(path); return error(e, c->span, "expired compile-time pointer"); }
        item.size = e->memory.objects[(c->bits >> 32) - 1].size - (uint32_t)c->bits;
    }
    if (data) {
        item.bytes = malloc(item.size ? item.size : 1);
        if (!item.bytes) { free(path); return error(e, c->span, "out of memory storing compile-time bytes"); }
        memcpy(item.bytes, data, item.size);
    }
    struct AtriRunCache *cache = ast->run_cache;
    if (!path || !atri_append((void **)&cache->items, &cache->count, &cache->capacity, sizeof(item), &item)) {
        free(path); free(item.bytes); return error(e, c->span, "out of memory caching compile-time result");
    }
    return true;
}
static bool constant(Evaluation *e, AtriId function, AtriId index) {
    AtriIRConstant *c = &e->program->functions[function].constants[index];
    if (c->state == 2) {
        e->result = c->bits;
        return true;
    }
    if (c->state == 1) {
        return error(e, c->span, "cyclic compile-time constant dependency");
    }
    const AtriProgram *ast = e->program->lowering_ast;
    if (ast && ast->run_cache && !c->capture_owner) {
        const SavedRun *s = find_run(ast->run_cache, run_key(e, function, index));
        if (s) {
            c->bits = s->view ? atri_vm_string_literal(&e->memory, s->bytes, s->size, true) :
                s->bytes ? atri_vm_object(&e->memory, s->size, VM_LITERAL, s->bytes) : s->bits;
            if ((s->view || s->bytes) && !c->bits) { return error(e, c->span, "out of memory restoring compile-time bytes"); }
            c->state = 2; e->result = c->bits; return true;
        }
    }
    Frame *frame = create_frame(e, function, NULL);
    if (!frame || !push(e, (Task){.kind = RESOLVE_CONSTANT, .frame = frame, .id = index}, c->span)) {
        return false;
    }
    c->state = 1;
    return c->capture_owner ? constant(e, c->capture_owner - 1, c->expression) : demand(e, frame, c->expression);
}
static void pop(Evaluation *e) {
    Task *task = &e->tasks[--e->task_count];
    free(task->arguments);
    if (task->kind == EVALUATE_VALUE) {
        --e->value_depth;
    } else {
        destroy_frame(task->frame);
    }
}
static bool finish_value(Evaluation *e, Task *t, uint64_t result) {
    t->frame->values[t->id] = result;
    t->frame->states[t->id] = 2;
    e->result = result;
    pop(e);
    return true;
}
static bool call(Evaluation *e, Task *t, const AtriIRInst *v, AtriSpan span) {
    const AtriIRFunction *caller = &e->program->functions[t->frame->function];
    if (t->resume == START) {
        if (v->argument_count) {
            t->arguments = calloc(v->argument_count, sizeof(*t->arguments));
            if (!t->arguments) { return error(e, span, "out of memory during compile-time call"); }
        }
        if (v->procedure) {
            t->resume = LEFT_READY;
            return demand(e, t->frame, v->procedure-1);
        }
    } else if (t->resume == ARGUMENT_READY) {
        t->arguments[t->argument++] = e->result;
    }
    if (t->argument < v->argument_count) {
        t->resume = ARGUMENT_READY;
        return demand(e, t->frame, caller->arguments[v->first_argument + t->argument]);
    }
    if (e->program->functions[v->callee].external_c) {
        uint64_t bits;
        if (!atri_vm_host(&e->memory, e->program, &e->program->functions[v->callee],
                          t->arguments, v->argument_count, &bits, span, e->diagnostic)) { return false; }
        return finish_value(e, t, bits);
    }
    t->resume = RESULT_READY;
    AtriId target = v->callee;
    if (v->procedure) {
        uint64_t proc = t->frame->values[v->procedure-1];
        if (!proc || proc > e->program->function_count) { return error(e, span, "invalid allocator callback"); }
        target = (AtriId)(proc-1);
        if (!atri_ir_allocator_callback(&e->program->functions[target])) {
            return error(e, span, "invalid allocator callback signature");
        }
    }
    Frame *callee = create_frame(e, target, t->arguments);
    return callee && push(e, (Task){.kind = EXECUTE_BODY, .frame = callee}, span);
}
static bool value(Evaluation *e, Task *t) {
    Frame *frame = t->frame;
    const AtriIRInst *v = &frame->body->code[t->id];
    AtriSpan span = e->program->functions[frame->function].instructions[t->id].span;
    if (t->resume == RESULT_READY) {
        return finish_value(e, t, e->result);
    }
    if (t->resume == START) {
        if (frame->states[t->id] == 2) {
            return finish_value(e, t, frame->values[t->id]);
        }
        if (frame->states[t->id] == 1) {
            return error(e, span, "cyclic compile-time value dependency");
        }
        frame->states[t->id] = 1;
    }
    if (frame->body->constants && frame->body->constants[t->id]) {
        t->resume = RESULT_READY;
        return constant(e, frame->function, frame->body->constants[t->id] - 1);
    }
    switch (v->kind) {
        case ATRI_IR_FUNCTION: return finish_value(e, t, v->callee+1);
        case ATRI_IR_CONTEXT: {
            const AtriIRFunction *f = &e->program->functions[frame->function];
            if (frame->parameters && f->has_context) { return finish_value(e, t, frame->parameters[atri_explicit_parameters(f)]); }
            if (!frame->root_context) { frame->root_context = atri_vm_object(&e->memory, 16, VM_LOCAL, NULL); }
            return frame->root_context ? finish_value(e, t, frame->root_context) : error(e, span, "out of memory creating VM context");
        }
        case ATRI_IR_CONST: return finish_value(e, t, v->immediate);
        case ATRI_IR_PARAM:
            if (!frame->parameters) { return error(e, span, "compile-time expression depends on a runtime parameter"); }
            return finish_value(e, t, frame->parameters[v->immediate]);
        case ATRI_IR_STRING: {
            if (!e->memory.literals) {
                e->memory.literals = calloc(e->program->string_count, sizeof(*e->memory.literals));
                if (!e->memory.literals) { return error(e, span, "out of memory loading VM literals"); }
            }
            uint64_t *address = &e->memory.literals[v->immediate];
            const AtriIRString *str = &e->program->strings[v->immediate];
            if (!*address) {
                *address = atri_vm_string_literal(&e->memory, str->bytes, str->length, str->view);
            }
            return *address ? finish_value(e, t, *address) : error(e, span, "out of memory loading VM literal");
        }
        case ATRI_IR_CALL: return call(e, t, v, span);
        case ATRI_IR_ZERO: {
            uint64_t addr = atri_vm_object(&e->memory, atri_ir_type_size(e->program, v->type), VM_LOCAL, NULL);
            return addr ? finish_value(e, t, addr) : error(e, span, "out of memory creating VM aggregate");
        }
        case ATRI_IR_LOAD: case ATRI_IR_ADDRESS: case ATRI_IR_FIELD: case ATRI_IR_INDEX:
        case ATRI_IR_PTR_OFFSET: case ATRI_IR_PTR_DIFF: case ATRI_IR_COMPARE: case ATRI_IR_UNARY:
        case ATRI_IR_BINARY: case ATRI_IR_CAST: case ATRI_IR_C_PROMOTE:
        case ATRI_IR_LAZY_SELECT: case ATRI_IR_LAZY_LOGICAL: break;
        default: return error(e, span, "invalid instruction in compile-time expression");
    }
    if (t->resume == START) {
        t->resume = LEFT_READY;
        return demand(e, frame, v->left);
    }
    uint64_t left = frame->values[v->left];
    if (v->kind == ATRI_IR_LOAD) {
        size_t size = atri_ir_type_size(e->program, v->type);
        const unsigned char *src = atri_vm_bytes(&e->memory, left, size, false);
        if (!src) { return error(e, span, "invalid or expired pointer in compile-time load"); }
        uint64_t bits = 0;
        if (atri_ir_is_aggregate(v->type)) {
            bits = atri_vm_object(&e->memory, size, VM_LOCAL, src);
            if (!bits) { return error(e, span, "out of memory copying VM aggregate"); }
        } else { memcpy(&bits, src, size); }
        return finish_value(e, t, bits);
    }
    if (v->kind == ATRI_IR_ADDRESS) { return finish_value(e, t, left); }
    if (v->kind == ATRI_IR_FIELD) {
        uint64_t addr;
        if (!atri_vm_offset(&e->memory, left, false, e->program->fields[v->immediate].offset, 1, &addr)) {
            return error(e, span, "invalid compile-time field address");
        }
        return finish_value(e, t, addr);
    }
    if (v->kind == ATRI_IR_CAST || v->kind == ATRI_IR_C_PROMOTE) {
        AtriIRType source = frame->body->code[v->left].type;
        return finish_value(e, t, source == ATRI_IR_BOOL || atri_ir_is_pointer(source) ? left :
                            atri_integer_convert(source, v->type, left));
    }
    if (v->kind == ATRI_IR_LAZY_SELECT || v->kind == ATRI_IR_LAZY_LOGICAL) {
        if (v->kind == ATRI_IR_LAZY_LOGICAL && (v->op == ATRI_AND ? !left : left)) {
            return finish_value(e, t, left);
        }
        t->resume = RESULT_READY;
        return demand(e, frame, v->kind == ATRI_IR_LAZY_SELECT && !left ? v->callee : v->right);
    }
    if (v->kind != ATRI_IR_UNARY && t->resume == LEFT_READY) {
        t->resume = RIGHT_READY;
        return demand(e, frame, v->right);
    }
    uint64_t right = v->kind == ATRI_IR_UNARY ? 0 : e->result;
    if (v->kind == ATRI_IR_COMPARE) {
        return finish_value(e, t, atri_compare(frame->body->code[v->left].type, v->op, left, right));
    }
    if (v->kind == ATRI_IR_PTR_DIFF) {
        size_t stride = atri_ir_type_size(e->program, atri_ir_pointee(frame->body->code[v->left].type));
        if (!stride || (left >> 32) != (right >> 32)) { return error(e, span, "unrelated pointers in compile-time subtraction"); }
        int64_t distance = (int64_t)(uint32_t)left - (int64_t)(uint32_t)right;
        return finish_value(e, t, (uint64_t)(distance / (int64_t)stride));
    }
    if (v->kind == ATRI_IR_PTR_OFFSET || v->kind == ATRI_IR_INDEX) {
        AtriIRType index = frame->body->code[v->right].type;
        bool negative = atri_integer_is_signed(index) && atri_integer_signed(index, right) < 0;
        uint64_t magnitude = negative ? 0 - (uint64_t)atri_integer_signed(index, right) : right;
        size_t stride = atri_ir_type_size(e->program, v->kind == ATRI_IR_INDEX ? v->type : atri_ir_pointee(v->type));
        uint64_t address;
        if (!atri_vm_offset(&e->memory, left, negative != (v->kind == ATRI_IR_PTR_OFFSET && v->op == ATRI_SUB),
                            magnitude, stride, &address)) { return error(e, span, "compile-time pointer offset is out of bounds"); }
        return finish_value(e, t, address);
    }
    uint64_t bits;
    if (v->op == ATRI_NOT) {
        bits = !left;
    } else if (!atri_integer_compute(v->type, v->op, left, right, &bits)) {
        if (v->op == ATRI_SHL || v->op == ATRI_SHR) {
            return error(e, span, "compile-time shift count is outside the operand width");
        }
        return error(e, span, right == 0 ? "compile-time division or remainder by zero" :
                     "invalid compile-time signed integer division or remainder (INT_MIN / -1)");
    }
    return finish_value(e, t, bits);
}
static bool execute(Evaluation *e, Task *t) {
    Frame *frame = t->frame;
    const AtriIRFunction *f = &e->program->functions[frame->function];
    if (t->id >= f->instruction_count) {
        return error(e, f->name, "compile-time function reached its end without returning");
    }
    if (t->resume == START && frame->body->skip_initializer && frame->body->skip_initializer[t->id]) {
        t->id = frame->body->skip_initializer[t->id];
    }
    const AtriIRInst *v = &frame->body->code[t->id];
    AtriSpan span = f->instructions[t->id].span;
    if (t->resume == RESULT_READY) {
        t->resume = START;
        switch (v->kind) {
            case ATRI_IR_RETURN: pop(e); return true;
            case ATRI_IR_BRANCH: t->id = frame->body->labels[e->result ? v->target : v->alternative]; return true;
            case ATRI_IR_STORE: {
                size_t size = atri_ir_type_size(e->program, v->type);
                unsigned char *dst = atri_vm_bytes(&e->memory, frame->values[v->left], size, true);
                const void *src = atri_ir_is_aggregate(v->type) ?
                    (const void *)atri_vm_bytes(&e->memory, e->result, size, false) : (const void *)&e->result;
                if (!dst || !src) { return error(e, span, "invalid or read-only pointer in compile-time store"); }
                memmove(dst, src, size);
                break;
            }
            default: break; /* 普通值已在 demand 返回时写入对应寄存器。 */
        }
        ++t->id;
        return true;
    }
    if (!step(e, span)) {
        return false;
    }
    AtriId operand = t->id;
    switch (v->kind) {
        case ATRI_IR_LOCAL: {
            if (v->immediate) { return error(e, span, "compile-time expression cannot access a runtime global"); }
            size_t slot = t->id++, size = atri_ir_type_size(e->program, v->type);
            if (!frame->locals[slot]) { frame->locals[slot] = atri_vm_object(&e->memory, size, VM_LOCAL, NULL); }
            if (!frame->locals[slot]) { return error(e, span, "out of memory allocating VM local"); }
            memset(atri_vm_bytes(&e->memory, frame->locals[slot], size, true), 0, size);
            frame->values[slot] = frame->locals[slot];
            frame->states[slot] = 2;
            return true;
        }
        case ATRI_IR_LABEL: case ATRI_IR_NOP: ++t->id; return true;
        case ATRI_IR_JUMP: t->id = frame->body->labels[v->target]; return true;
        case ATRI_IR_UNREACHABLE: return error(e, span, "compile-time function reached its end without returning");
        case ATRI_IR_RETURN:
            if (v->type == ATRI_IR_VOID) {
                e->result = 0;
                pop(e);
                return true;
            }
            operand = v->left;
            break;
        case ATRI_IR_BRANCH: operand = v->left; break;
        case ATRI_IR_STORE: operand = v->right; break;
        default:
            /* 循环回边可再次经过同一 PC。本次执行刷新该槽，其余已执行的操作数仍有效。 */
            frame->states[t->id] = 0;
            break;
    }
    t->resume = RESULT_READY;
    return demand(e, frame, operand);
}

static bool drive(Evaluation *e) {
    while (e->task_count) {
        Task *t = &e->tasks[e->task_count - 1];
        switch (t->kind) {
            case EVALUATE_VALUE: if (!value(e, t)) { return false; } break;
            case EXECUTE_BODY: if (!execute(e, t)) { return false; } break;
            case RESOLVE_CONSTANT: {
                AtriIRConstant *ready = &e->program->functions[t->frame->function].constants[t->id];
                ready->bits = ready->discard ? 0 : e->result;
                e->result = ready->bits;
                ready->state = 2;
                if (!save_run(e, t->frame->function, t->id)) { return false; }
                pop(e);
                break;
            }
        }
    }
    return true;
}

static bool resolve(Evaluation *e) {
    for (size_t n = 0; n < e->program->function_count; ++n) {
        AtriIRFunction *f = &e->program->functions[n];
        if (!f->constant_count) {
            continue;
        }
        if (!e->functions) {
            e->functions = calloc(e->program->function_count, sizeof(*e->functions));
            if (!e->functions) {
                return error(e, f->name, "out of memory preparing compile-time bytecode");
            }
        }
        for (size_t c = 0; c < f->constant_count; ++c) {
            if (!constant(e, n, c)) {
                return false;
            }
            if (!drive(e)) { return false; }
        }
    }
    return true;
}

static void trace_task(Evaluation *e, Task *t) {
    const AtriIRFunction *f = &e->program->functions[t->frame->function];
    if (t->kind == EVALUATE_VALUE && f->instructions[t->id].kind == ATRI_IR_CALL) {
        atri_diagnostic_note(e->diagnostic, f->instructions[t->id].span, "called during compile-time evaluation here");
    } else if (t->kind == RESOLVE_CONSTANT) {
        atri_diagnostic_note(e->diagnostic, f->constants[t->id].span, "required by compile-time evaluation here");
    }
}
static bool materialize(Evaluation *e, const AtriIRConstant *c, AtriIRInst *result) {
    if (!c->bits && c->nonnull_use.end) {
        return error(e, c->nonnull_use, "cannot dereference a statically known null pointer");
    }
    result->immediate = c->bits;
    AtriIRString data = {.view = result->type == e->program->string_type};
    const unsigned char *bytes;
    if (data.view) {
        if (!atri_vm_view(&e->memory, c->bits, &bytes, &data.length)) { return error(e, c->span, "#run string refers to expired or invalid bytes"); }
        if (!bytes) { result->kind = ATRI_IR_ZERO; result->immediate = 0; return true; }
    } else {
        if (result->type != ATRI_IR_PTR_U8 || !c->bits) { return true; }
        size_t length;
        bytes = (const unsigned char *)atri_vm_text(&e->memory, c->bits, &length);
        if (!bytes) { return error(e, c->span, "#run byte-string result must be live and zero terminated"); }
        const AtriVmObject *object = &e->memory.objects[(c->bits >> 32) - 1];
        size_t remaining = object->size - (uint32_t)c->bits;
        data.length = remaining - (bytes[remaining - 1] == 0);
    }
    size_t size = data.length + !data.view;
    data.bytes = malloc(size ? size : 1);
    if (data.bytes) {
        memcpy(data.bytes, bytes, data.length);
        if (!data.view) { data.bytes[data.length] = 0; }
    }
    AtriIRProgram *p = e->program;
    result->kind = ATRI_IR_STRING;
    result->immediate = p->string_count;
    if (!data.bytes || !atri_append((void **)&p->strings, &p->string_count, &p->string_capacity, sizeof(data), &data)) {
        free(data.bytes); return error(e, c->span, "out of memory materializing #run byte string");
    }
    return true;
}
bool atri_resolve_constants(AtriIRProgram *p, AtriDiagnostic *d) {
    Evaluation e = {.program = p, .diagnostic = d, .remaining_steps = 100000};
    bool ok = resolve(&e);
    while (e.task_count) {
        trace_task(&e, &e.tasks[e.task_count-1]);
        pop(&e);
    }
    free(e.tasks);
    atri_vm_release(e.functions, p->function_count);
    if (!ok) {
        atri_vm_memory_destroy(&e.memory);
        return false;
    }
    /* 所有义务都成功后统一回填，包括永远不会在目标程序中执行的声明。 */
    for (size_t n = 0; n < p->function_count; ++n) {
        AtriIRFunction *f = &p->functions[n];
        for (size_t c = 0; c < f->constant_count; ++c) {
            const AtriIRConstant *binding = &f->constants[c];
            for (size_t i = binding->first_instruction; i < binding->result; ++i) {
                f->instructions[i].kind = ATRI_IR_NOP;
            }
            f->instructions[binding->result] = (AtriIRInst){.kind = ATRI_IR_CONST,
                .span = binding->span, .type = f->instructions[binding->result].type};
            if (!materialize(&e, binding, &f->instructions[binding->result])) {
                atri_vm_memory_destroy(&e.memory); return false;
            }
        }
        free(f->constants);
        f->constants = NULL;
        f->constant_count = 0;
        f->constant_capacity = 0;
    }
    atri_vm_memory_destroy(&e.memory);
    return true;
}

bool atri_vm_generate(AtriIRProgram *p, AtriVmFunction *functions, AtriId id, const AtriVmArgument *args,
                      char **text, size_t *length, AtriDiagnostic *d) {
    AtriIRFunction *f = &p->functions[id];
    Evaluation e = {.program = p, .diagnostic = d, .functions = functions, .remaining_steps = 100000};
    uint64_t *values = calloc(f->parameter_count + 1, sizeof(*values));
    bool ok = values != NULL;
    if (ok && f->has_context) {
        values[atri_explicit_parameters(f)] = atri_vm_object(&e.memory, 16, VM_LOCAL, NULL);
        ok = values[atri_explicit_parameters(f)] != 0;
    }
    for (size_t i = 0; ok && i < atri_explicit_parameters(f); ++i) {
        if (args[i].text) {
            values[i] = atri_vm_string_literal(&e.memory, args[i].text, args[i].length, f->parameter_types[i] == p->string_type);
            ok = values[i] != 0;
        } else { values[i] = args[i].bits; }
    }
    if (!ok) { error(&e, f->name, "out of memory preparing generator arguments"); }
    Frame *frame = ok ? create_frame(&e, id, values) : NULL;
    ok = frame && push(&e, (Task){.kind = EXECUTE_BODY, .frame = frame}, f->name) && drive(&e);
    if (ok) {
        const unsigned char *src = NULL;
        if (f->return_type == p->string_type) {
            ok = atri_vm_view(&e.memory, e.result, &src, length);
            if (ok && *length && memchr(src, 0, *length)) { ok = error(&e, f->name, "generated source contains a zero byte"); }
        } else {
            src = (const unsigned char *)atri_vm_text(&e.memory, e.result, length);
            ok = src != NULL;
        }
        if (!ok && !d->message) { error(&e, f->name, "generator returned a null, expired or unterminated source string"); }
        if (ok) {
            *text = malloc(*length + 1);
            if (*text) { if (*length) { memcpy(*text, src, *length); } (*text)[*length] = 0; }
            else { ok = error(&e, f->name, "out of memory copying generated source"); }
        }
    }
    while (e.task_count) {
        Task *task = &e.tasks[e.task_count-1];
        trace_task(&e, task);
        pop(&e);
    }
    free(e.tasks); free(values);
    atri_vm_memory_destroy(&e.memory);
    return ok;
}
