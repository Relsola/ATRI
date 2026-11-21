#include "meta.h"
#include "meta_ast.h"
#include "lexer.h"
#include "array.h"
#include "source_map.h"
#include "vm_bytecode.h"

typedef struct {
    AtriSource source;
    size_t complete;
    AtriSourceMap *map;
} Unit;
typedef struct {
    Unit *owner, *definition;
    size_t definition_offset;
    char *name, *qualifier;
    AtriSource source;
    AtriProgram locations;
    AtriIRProgram ir;
    AtriVmFunction *functions;
    AtriId id;
} Generator;
typedef struct {
    struct AtriRunCache *run_cache;
    Unit **units;
    size_t count, capacity;
    Generator *generators;
    size_t gen_count, gen_capacity;
    const AtriListener *listeners;
    size_t listener_count, events, edits, generated_bytes;
    Unit *failure;
    Unit *note_units[sizeof(((AtriDiagnostic *)0)->notes) / sizeof(AtriDiagnosticLabel)];
    AtriDiagnostic *d;
} Build;

static char *copy(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}
static bool fail(Build *b, Unit *unit, AtriSpan span, const char *message) {
    b->failure = unit;
    *b->d = (AtriDiagnostic){.span = span, .message = message};
    return false;
}
static Unit *unit_named(Build *b, const char *path) {
    for (size_t i = 0; i < b->count; ++i) {
        if (!strcmp(b->units[i]->source.name, path)) {
            return b->units[i];
        }
    }
    return NULL;
}
static Unit *add_unit(Build *b, char *path, char *text, size_t size) {
    Unit *u = calloc(1, sizeof(*u));
    if (!u || !path || !text || !atri_append((void **)&b->units, &b->count, &b->capacity, sizeof(u), &u)) {
        free(u);
        free(path);
        free(text);
        return NULL;
    }
    u->source = (AtriSource){path, text, size};
    return u;
}
static bool edit(Build *b, Unit *u, AtriSpan range, AtriSpan origin, const char *text, size_t size,
                 const char *name) {
    const size_t limit = 64u * 1024u * 1024u;
    size_t retained = u->source.length - (range.end - range.start);
    if (++b->edits > 4096 || retained > limit || size > limit - retained || size > limit - b->generated_bytes) {
        return fail(b, u, origin, "source expansion exceeded its build resource limit");
    }
    b->generated_bytes += size;
    return atri_source_edit(&u->source, &u->map, range, origin, text, size, name) ||
           fail(b, u, origin, "out of memory expanding generated source");
}

/* 展开时只把已经交给全部监听器的声明提供给生成器。最后一次快照使用全部模块。 */
static bool snapshot(Build *b, Unit *root, bool index, AtriSource *source, AtriProgram *ast) {
    AtriSource *inputs = calloc(b->count, sizeof(*inputs));
    if (!inputs) {
        return fail(b, root, (AtriSpan){0}, "out of memory collecting module sources");
    }
    for (size_t i = 0; i < b->count; ++i) {
        inputs[i] = b->units[i]->source;
        inputs[i].length = b->units[i]->complete;
    }
    AtriSource first = root->source;
    first.length = root->complete;
    bool ok = atri_load_sources(first, inputs, b->count, index, source, ast, b->d);
    free(inputs);
    return ok;
}
static void close_generator(Generator *g) {
    atri_vm_release(g->functions, g->ir.function_count);
    atri_ir_destroy(&g->ir);
    atri_program_destroy(&g->locations);
    free((char *)g->source.text);
    free(g->name);
    free(g->qualifier);
}
static void map_generator_failure(Build *b, Unit *u, const AtriProgram *ast, AtriSpan site) {
    if (!ast->module_count) {
        b->failure = u;
        return;
    }
    AtriModule primary = ast->modules[atri_module_at(ast, b->d->span.start)];
    b->failure = unit_named(b, primary.path);
    b->d->span.start -= primary.span.start;
    b->d->span.end -= primary.span.start;
    for (size_t i = 0; i < b->d->note_count; ++i) {
        AtriSpan *span = &b->d->notes[i].span;
        AtriModule m = ast->modules[atri_module_at(ast, span->start)];
        b->note_units[i] = unit_named(b, m.path);
        span->start -= m.span.start;
        span->end -= m.span.start;
    }
    size_t before = b->d->note_count;
    atri_diagnostic_note(b->d, site, "generator needed at this insertion");
    if (b->d->note_count > before) {
        b->note_units[before] = u;
    }
}
static Generator *generator(Build *b, Unit *u, const char *qualifier, const char *name, AtriSpan site) {
    for (size_t i = 0; i < b->gen_count; ++i) {
        Generator *g = &b->generators[i];
        if (g->owner == u && !strcmp(g->name, name) && !strcmp(g->qualifier, qualifier)) {
            return g;
        }
    }
    Generator g = {.owner = u};
    AtriProgram ast = {0};
    bool ok = snapshot(b, u, true, &g.source, &ast);
    AtriId owner = *qualifier ? ATRI_NONE : 0, function = ATRI_NONE;
    if (ok && *qualifier) {
        for (size_t i = 0; i < ast.symbol_count; ++i) {
            const AtriModuleSymbol *sym = &ast.symbols[i];
            if (!sym->owner && sym->kind == ATRI_SYMBOL_MODULE && sym->length == strlen(qualifier) &&
                !memcmp(sym->text, qualifier, sym->length)) {
                owner = sym->target;
                break;
            }
        }
    }
    for (size_t i = 0; ok && i < ast.symbol_count; ++i) {
        const AtriModuleSymbol *sym = &ast.symbols[i];
        if (sym->owner == owner && sym->kind == ATRI_SYMBOL_FUNCTION && (!*qualifier || sym->exported) &&
            sym->length == strlen(name) && !memcmp(sym->text, name, sym->length)) {
            function = sym->target;
            break;
        }
    }
    if (!ok) {
        map_generator_failure(b, u, &ast, site);
    }
    if (ok && (function == ATRI_NONE || ast.functions[function].external_c)) {
        ok = fail(b, u, site, "#insert needs an already defined ATRI generator");
    }
    if (ok && !b->run_cache) {
        b->run_cache = atri_run_cache_create();
        if (!b->run_cache) {
            ok = fail(b, u, site, "out of memory preparing compile-time cache");
        }
    }
    if (ok) {
        ast.run_cache = b->run_cache;
        AtriSpan fn = ast.functions[function].name;
        AtriModule m = ast.modules[atri_module_at(&ast, fn.start)];
        g.definition = unit_named(b, m.path);
        g.definition_offset = fn.start - m.span.start;
        for (size_t i = 0; i < b->gen_count; ++i) {
            Generator *existing = &b->generators[i];
            if (existing->definition == g.definition && existing->definition_offset == g.definition_offset) {
                atri_program_destroy(&ast);
                close_generator(&g);
                return existing;
            }
        }
        size_t decl = 0;
        while (decl < ast.declaration_count && ast.declarations[decl].name.start != fn.start) {
            ++decl;
        }
        ok = decl < ast.declaration_count && atri_lower_generator(g.source, &ast, decl, &g.ir, b->d);
        if (!ok) {
            map_generator_failure(b, u, &ast, site);
        } else {
            while (g.id < g.ir.function_count && g.ir.functions[g.id].name.start != fn.start) {
                ++g.id;
            }
            if (g.id == g.ir.function_count || (g.ir.functions[g.id].return_type != ATRI_IR_PTR_U8 &&
                g.ir.functions[g.id].return_type != g.ir.string_type)) {
                ok = fail(b, u, site, "#insert generator must return *u8 or string source text");
            }
        }
    }
    if (ok) {
        g.locations.modules = ast.modules;
        g.locations.module_count = ast.module_count;
        ast.modules = NULL;
        ast.module_count = 0;
    }
    atri_program_destroy(&ast);
    if (ok) {
        g.functions = calloc(g.ir.function_count, sizeof(*g.functions));
        g.name = copy(name, strlen(name));
        g.qualifier = copy(qualifier, strlen(qualifier));
        ok = g.name && g.qualifier && g.functions &&
             atri_append((void **)&b->generators, &b->gen_count, &b->gen_capacity, sizeof(g), &g);
        if (!ok) {
            fail(b, u, site, "out of memory caching compiled generator");
        }
    }
    if (!ok) {
        close_generator(&g);
        return NULL;
    }
    return &b->generators[b->gen_count - 1];
}

static bool literal_argument(AtriSource s, AtriToken tok, bool negative, AtriIRType type, AtriIRType string_type, AtriVmArgument *out) {
    size_t pos = tok.span.start + 1;
    if (tok.kind == ATRI_TOKEN_INTEGER && atri_ir_is_integer(type)) {
        uint64_t max = atri_integer_max(type) + (negative && atri_integer_is_signed(type));
        if (!atri_integer_literal(s, tok.span, max, &out->bits)) {
            return false;
        }
        if (negative) {
            out->bits = (0 - out->bits) & atri_integer_mask(type);
        }
        return true;
    }
    if (negative) {
        return false;
    }
    if (tok.kind == ATRI_TOKEN_STRING && (type == ATRI_IR_PTR_U8 || type == string_type)) {
        unsigned char *bytes = malloc(tok.span.end - tok.span.start);
        if (!bytes) {
            return false;
        }
        while (pos < tok.span.end - 1) {
            bytes[out->length++] = atri_literal_byte(s, &pos);
        }
        if (type == ATRI_IR_PTR_U8) { bytes[out->length] = 0; }
        out->text = bytes;
        return true;
    }
    if (tok.kind == ATRI_TOKEN_CHARACTER && type == ATRI_IR_U8) {
        out->bits = atri_literal_byte(s, &pos);
        return true;
    }
    if (type == ATRI_IR_BOOL && (tok.kind == ATRI_TOKEN_TRUE || tok.kind == ATRI_TOKEN_FALSE)) {
        out->bits = tok.kind == ATRI_TOKEN_TRUE;
        return true;
    }
    return tok.kind == ATRI_TOKEN_NULL && atri_ir_is_pointer(type);
}

static bool expand_call(Build *b, Unit *u, AtriSpan marker, size_t limit) {
    AtriLexer lexer = atri_lexer_create(u->source);
    lexer.offset = marker.end;
    lexer.source.length = limit;
    AtriToken tok = atri_lexer_next(&lexer, b->d);
    if (tok.kind != ATRI_TOKEN_RUN) {
        return fail(b, u, tok.span, "expected #run after #insert");
    }
    tok = atri_lexer_next(&lexer, b->d);
    AtriSpan fn = tok.span, qualifier = {0};
    if (tok.kind != ATRI_TOKEN_IDENTIFIER) {
        return fail(b, u, tok.span, "expected a generator name after #insert #run");
    }
    tok = atri_lexer_next(&lexer, b->d);
    if (tok.kind == ATRI_TOKEN_SCOPE) {
        qualifier = fn;
        tok = atri_lexer_next(&lexer, b->d);
        fn = tok.span;
        if (tok.kind != ATRI_TOKEN_IDENTIFIER) {
            return fail(b, u, tok.span, "expected a qualified generator name");
        }
        tok = atri_lexer_next(&lexer, b->d);
    }
    if (tok.kind != ATRI_TOKEN_LEFT_PAREN) {
        return fail(b, u, tok.span, "expected '(' after generator name");
    }
    char *name = copy(u->source.text + fn.start, fn.end - fn.start);
    char *module = copy(u->source.text + qualifier.start, qualifier.end - qualifier.start);
    if (!name || !module) {
        free(name);
        free(module);
        return fail(b, u, fn, "out of memory reading generator name");
    }
    Generator *g = generator(b, u, module, name, fn);
    free(name);
    free(module);
    if (!g) {
        return false;
    }
    AtriIRFunction *function = &g->ir.functions[g->id];
    size_t count = atri_explicit_parameters(function);
    AtriVmArgument *args = calloc(count + 1, sizeof(*args));
    if (!args) {
        return fail(b, u, fn, "out of memory reading generator arguments");
    }
    bool ok = true;
    for (size_t i = 0; ok && i < count; ++i) {
        tok = atri_lexer_next(&lexer, b->d);
        if (i && tok.kind != ATRI_TOKEN_COMMA) {
            ok = fail(b, u, tok.span, "expected ',' between generator arguments");
            break;
        }
        if (i) {
            tok = atri_lexer_next(&lexer, b->d);
        }
        bool negative = tok.kind == ATRI_TOKEN_MINUS;
        if (negative) {
            tok = atri_lexer_next(&lexer, b->d);
        }
        if (!literal_argument(u->source, tok, negative, function->parameter_types[i], g->ir.string_type, &args[i])) {
            ok = fail(b, u, tok.span, "generator argument must be a known literal of the parameter type");
        }
    }
    char *result = NULL;
    size_t size = 0;
    if (ok) {
        tok = atri_lexer_next(&lexer, b->d);
        if (tok.kind != ATRI_TOKEN_RIGHT_PAREN) {
            ok = fail(b, u, tok.span, "expected ')' after generator arguments");
        }
    }
    AtriSpan site = {marker.start, tok.span.end};
    if (ok) {
        ok = atri_vm_generate(&g->ir, g->functions, g->id, args, &result, &size, b->d);
        if (!ok) {
            /* 下钻到生成器而不是把 VM 的跨模块偏移误当成当前文件的列。 */
            map_generator_failure(b, u, &g->locations, site);
        }
    }
    if (ok) {
        ok = edit(b, u, site, site, result, size, g->name);
    }
    for (size_t i = 0; i < count; ++i) {
        free((void *)args[i].text);
    }
    free(args);
    free(result);
    return ok;
}

/* 只向前查找词法上下文中有效的指令。执行一次替换后从当前声明的稳定边界再扫描。 */
static AtriSpan next_insert(AtriSource s, size_t pos) {
    if (!memchr(s.text + pos, '#', s.length - pos)) {
        return (AtriSpan){s.length, s.length};
    }
    AtriLexer lexer = atri_lexer_create(s);
    if (pos > lexer.offset) {
        lexer.offset = pos;
    }
    AtriDiagnostic ignored;
    for (;;) {
        AtriToken t = atri_lexer_next(&lexer, &ignored);
        if (t.kind == ATRI_TOKEN_INSERT || t.kind == ATRI_TOKEN_EOF) {
            return t.span;
        }
    }
}

static bool process(Build *b, Unit *u, size_t depth);
static bool import_unit(Build *b, Unit *u, AtriSpan span, size_t depth) {
    bool standard = atri_standard_import(u->source.text, span);
    if (!strcmp(u->source.name, "<stdin>") && !standard) {
        return fail(b, u, span, "relative imports require a source file path");
    }
    char *relative = atri_import_path(u->source.name, u->source.text, span);
    char *path = relative ? atri_canonical_path(relative) : NULL;
    if (!path) {
        fail(b, u, span, standard ? "cannot resolve standard library module" : "cannot resolve relative .ar module path");
        if (relative) {
            snprintf(b->d->detail, sizeof(b->d->detail), "%s", relative);
        }
        free(relative);
        return false;
    }
    free(relative);
    if (unit_named(b, path)) {
        free(path);
        return true;
    }
    FILE *file = fopen(path, "rb");
    size_t size = 0;
    char *text = file ? atri_read_stream(file, &size) : NULL;
    if (file) {
        fclose(file);
    }
    Unit *child = add_unit(b, path, text, size);
    if (!child) {
        return fail(b, u, span, "cannot load imported source");
    }
    return process(b, child, depth + 1);
}

static bool process(Build *b, Unit *u, size_t depth) {
    if (depth > 256) {
        return fail(b, u, (AtriSpan){0}, "module graph exceeds resource limits");
    }
    size_t plugin = 0, replacement_end = ATRI_NONE;
    uint64_t event = 0;
    bool optional_terminator = false;
    AtriSpan marker = next_insert(u->source, 0);
    if (!b->listener_count && marker.start == u->source.length && !strstr(u->source.text, "import")) {
        u->complete = u->source.length;
        return true;
    }
    while (u->complete < u->source.length || replacement_end != ATRI_NONE) {
        if (replacement_end != ATRI_NONE && marker.start < replacement_end) {
            size_t old_size = u->source.length;
            if (!expand_call(b, u, marker, replacement_end)) {
                return false;
            }
            replacement_end = u->source.length - (old_size - replacement_end);
            marker = next_insert(u->source, u->complete);
            continue;
        }
        /* 插入可能为已经结束的 enum/struct/#run 块提供可选分号。保留事件身份，
         * 在展开后消费该分隔符，最终解析仍看到完整声明。 */
        if (optional_terminator) {
            AtriLexer look = atri_lexer_create(u->source);
            look.offset = u->complete; look.source.length = marker.start;
            AtriDiagnostic ignored;
            AtriToken t = atri_lexer_next(&look, &ignored);
            if (t.kind != ATRI_TOKEN_EOF) { optional_terminator = false; }
            if (t.kind == ATRI_TOKEN_SEMICOLON) { u->complete = t.span.end; continue; }
        }
        AtriProgram one = {0};
        size_t next = u->complete;
        size_t limit = replacement_end != ATRI_NONE ? replacement_end : marker.start;
        bool ok = atri_parse_next(u->source, (AtriSpan){u->complete, limit}, &next, &one, b->d);
        if (!ok || !one.declaration_count) {
            atri_program_destroy(&one);
            if (replacement_end == ATRI_NONE && marker.start < u->source.length) {
                if (!expand_call(b, u, marker, u->source.length)) {
                    return false;
                }
                marker = next_insert(u->source, u->complete);
                continue;
            }
            if (!ok) {
                b->failure = u;
                return false;
            }
            if (replacement_end != ATRI_NONE) {
                return fail(b, u, (AtriSpan){u->complete, replacement_end},
                            "replacement must contain exactly one declaration");
            }
            u->complete = u->source.length;
            break;
        }
        AtriDeclaration decl = one.declarations[0];
        if (replacement_end != ATRI_NONE && next != replacement_end) {
            atri_program_destroy(&one);
            return fail(b, u, (AtriSpan){next, replacement_end},
                        "replacement must contain exactly one declaration");
        }
        replacement_end = ATRI_NONE;
        if (!plugin) {
            event = ++b->events;
        }
        bool changed = false;
        while (plugin < b->listener_count && !changed) {
            const AtriListener *listener = &b->listeners[plugin++];
            char *name = copy(u->source.text + decl.name.start, decl.name.end - decl.name.start);
            char *src = copy(u->source.text + decl.span.start, decl.span.end - decl.span.start);
            const char *replacement = NULL, *injection = NULL;
            AtriAstView view;
            atri_ast_begin(&view, u->source, &one, decl.span, event);
            if (!name || !src) {
                ok = fail(b, u, decl.span, "out of memory dispatching declaration");
            } else if (listener->callback(event, decl.kind, u->source.name, name, src, &replacement, &injection)) {
                ok = fail(b, u, decl.span, "metaprogram callback reported failure");
                snprintf(b->d->detail, sizeof(b->d->detail), "%s", listener->name);
            }
            if (view.invalid) {
                if (edit(b, u, decl.span, decl.span, view.invalid, strlen(view.invalid), listener->name)) {
                    *b->d = view.invalid_diagnostic;
                    b->d->span.start += decl.span.start;
                    b->d->span.end += decl.span.start;
                    b->failure = u;
                }
                ok = false;
            } else if (view.failed) {
                ok = fail(b, u, decl.span, "plugin AST edit failed");
            }
            if (view.changed && !replacement) {
                replacement = view.changed;
            }
            if (ok && injection && *injection) {
                size_t n = strlen(injection);
                char *append = malloc(n + 3);
                if (!append) {
                    ok = fail(b, u, decl.span, "out of memory injecting declarations");
                } else {
                    append[0] = '\n';
                    memcpy(append + 1, injection, n);
                    append[n + 1] = '\n';
                    size_t end = u->source.length;
                    ok = edit(b, u, (AtriSpan){end, end}, decl.span, append, n + 2, listener->name);
                    free(append);
                }
                if (ok && marker.start == marker.end) {
                    marker = next_insert(u->source, decl.span.end);
                }
            }
            if (ok && replacement && strcmp(src, replacement)) {
                size_t length = strlen(replacement);
                ok = edit(b, u, decl.span, decl.span, replacement, length, listener->name);
                if (ok) {
                    replacement_end = decl.span.start + length;
                }
                changed = ok;
            }
            atri_ast_end(&view);
            free(name);
            free(src);
            if (!ok) {
                break;
            }
        }
        if (ok && !changed) {
            optional_terminator = next == marker.start && marker.start < u->source.length &&
                u->source.text[decl.span.end - 1] == '}' && (decl.kind == ATRI_META_ENUM ||
                decl.kind == ATRI_META_STRUCT || decl.kind == ATRI_META_UNION || decl.kind == ATRI_META_RUN);
            u->complete = decl.span.end;
            plugin = 0;
            if (one.import_count) {
                ok = import_unit(b, u, one.imports[0].path, depth);
            }
        }
        atri_program_destroy(&one);
        if (!ok) {
            return false;
        }
        if (changed) {
            marker = next_insert(u->source, u->complete);
        }
    }
    return true;
}

/* 生成器可能来自导入模块。保留每个标签自己的模块，再投影到返回的诊断快照。 */
static void diagnostic_sources(Build *b, AtriSource *source, AtriProgram *p) {
    size_t size = 0;
    for (size_t i = 0; i < b->count; ++i) {
        if (b->units[i]->source.length >= SIZE_MAX - size) {
            return;
        }
        size += b->units[i]->source.length + 1;
    }
    char *joined = calloc(size, 1);
    p->modules = calloc(b->count, sizeof(*p->modules));
    if (!joined || !p->modules) {
        free(joined);
        free(p->modules);
        p->modules = NULL;
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i < b->count; ++i) {
        Unit *u = b->units[i];
        size_t length = u->source.length;
        p->modules[i] =
            (AtriModule){.path = (char *)u->source.name, .span = {start, start + length}, .origins = u->map};
        u->source.name = NULL;
        u->map = NULL;
        memcpy(joined + start, u->source.text, length);
        if (u == b->failure) {
            b->d->span.start += start;
            b->d->span.end += start;
        }
        for (size_t j = 0; j < b->d->note_count; ++j) {
            if (u == (b->note_units[j] ? b->note_units[j] : b->failure)) {
                b->d->notes[j].span.start += start;
                b->d->notes[j].span.end += start;
            }
        }
        start += length + 1;
    }
    source->text = joined;
    source->length = size - 1;
    p->module_count = b->count;
}
bool atri_expand_program(AtriSource input, const AtriListener *listeners, size_t count, AtriSource *source,
                         AtriProgram *program, AtriDiagnostic *d) {
    Build b = {.listeners = listeners, .listener_count = count, .d = d};
    char *path = atri_canonical_path(input.name);
    if (!path) {
        path = copy(input.name, strlen(input.name));
    }
    Unit *root = add_unit(&b, path, copy(input.text, input.length), input.length);
    if (!root) {
        return fail(&b, NULL, (AtriSpan){0}, "out of memory preparing source");
    }
    bool ok = process(&b, root, 0);
    if (ok) {
        ok = snapshot(&b, root, true, source, program);
    }
    if (b.failure && !program->module_count) {
        diagnostic_sources(&b, source, program);
    } else {
        for (size_t i = 0; i < program->module_count; ++i) {
            Unit *u = unit_named(&b, program->modules[i].path);
            if (u) {
                program->modules[i].origins = u->map;
                u->map = NULL;
            }
        }
    }
    for (size_t i = 0; i < b.gen_count; ++i) {
        close_generator(&b.generators[i]);
    }
    free(b.generators);
    for (size_t i = 0; i < b.count; ++i) {
        Unit *u = b.units[i];
        free((char *)u->source.text);
        free((char *)u->source.name);
        atri_source_map_destroy(u->map);
        free(u);
    }
    /* source.name 在外部由调用者持有；模块数组拥有规范路径。 */
    source->name = input.name;
    free(b.units);
    program->run_cache = b.run_cache;
    program->owns_run_cache = true;
    return ok;
}
