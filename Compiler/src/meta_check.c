#include "meta.h"
#include "meta_plugin.h"
#include "lexer.h"
#include <stdlib.h>
#include <string.h>

static size_t declaration_at(const AtriProgram *p, size_t offset) {
    size_t first = 0, end = p->declaration_count;
    while (first < end) {
        size_t middle = first + (end-first)/2;
        if (p->declarations[middle].span.end <= offset) { first = middle + 1; }
        else { end = middle; }
    }
    return first;
}

/* 先用模块可见名称保守地取依赖闭包，再交给原降低器精确判型。
 * 同名局部可能多纳入一个声明。此索引只控制检查范围，不决定名称绑定。 */
static void dependencies(AtriSource source, const AtriProgram *p, size_t root, bool *selected, AtriId *pending) {
    size_t count = 1;
    pending[0] = root;
    selected[root] = true;
    for (size_t head = 0; head < count; ++head) {
        AtriSpan span = p->declarations[pending[head]].span;
        AtriLexer lexer = atri_lexer_create(source);
        lexer.offset = span.start;
        lexer.source.length = span.end;
        AtriToken previous = {0}, before = {0};
        for (;;) {
            AtriDiagnostic ignored;
            AtriToken token = atri_lexer_next(&lexer, &ignored);
            if (token.kind == ATRI_TOKEN_EOF || token.kind == ATRI_TOKEN_ERROR) { break; }
            if (token.kind == ATRI_TOKEN_IDENTIFIER && previous.kind != ATRI_TOKEN_DOT) {
                AtriSpan qualifier = previous.kind == ATRI_TOKEN_SCOPE ? before.span : (AtriSpan){0};
                AtriSpan name = atri_declaration_name(source, p, token.span, qualifier);
                if (name.end) {
                    size_t id = declaration_at(p, name.start);
                    if (id < p->declaration_count && !selected[id]) {
                        unsigned kind = p->declarations[id].kind;
                        AtriLexer look = lexer;
                        bool call = kind == ATRI_META_FUNCTION &&
                            atri_lexer_next(&look, &ignored).kind == ATRI_TOKEN_LEFT_PAREN;
                        if (call || kind == ATRI_META_STRUCT || kind == ATRI_META_UNION || kind == ATRI_META_ENUM) {
                            selected[id] = true; pending[count++] = id;
                        }
                    }
                }
            }
            before = previous;
            previous = token;
        }
    }
}

bool atri_lower_generator(AtriSource source, const AtriProgram *p, size_t root, AtriIRProgram *ir, AtriDiagnostic *d) {
    AtriProgram view = *p;
    view.generator_scope = true;
    bool *needed = calloc(p->declaration_count, sizeof(*needed));
    AtriId *pending = calloc(p->declaration_count, sizeof(*pending));
    /* 语句、参数、表达式和源码由原 AST 借出。仅重建进入本次检查的顶层表。 */
    view.functions = calloc(p->function_count + 1, sizeof(*view.functions));
    view.globals = calloc(p->global_count + 1, sizeof(*view.globals));
    view.structs = calloc(p->struct_count + 1, sizeof(*view.structs));
    view.fields = calloc(p->field_count + 1, sizeof(*view.fields));
    view.enums = calloc(p->enum_count + 1, sizeof(*view.enums));
    view.variants = calloc(p->variant_count + 1, sizeof(*view.variants));
    view.runs = calloc(p->run_count + 1, sizeof(*view.runs));
    view.symbols = NULL;
    view.symbol_count = 0;
    view.symbols_ready = false;
    view.function_count = view.global_count = view.struct_count = view.field_count = 0;
    view.enum_count = view.variant_count = view.run_count = 0;
    bool ok = false;
    if (!needed || !pending || !view.functions || !view.globals || !view.structs ||
        !view.fields || !view.enums || !view.variants || !view.runs) {
        *d = (AtriDiagnostic){.span = p->declarations[root].span, .message = "out of memory checking declaration dependencies"};
        goto done;
    }
    dependencies(source, p, root, needed, pending);
    for (size_t i = 0; i < p->function_count; ++i) {
        if (needed[declaration_at(p, p->functions[i].name.start)]) { view.functions[view.function_count++] = p->functions[i]; }
    }
    for (size_t i = 0; i < p->global_count; ++i) {
        if (needed[declaration_at(p, p->globals[i].declaration.name.start)]) { view.globals[view.global_count++] = p->globals[i]; }
    }
    for (size_t i = 0; i < p->struct_count; ++i) {
        AtriStruct s = p->structs[i];
        if (!needed[declaration_at(p, s.name.start)]) { continue; }
        if (s.field_count) { memcpy(view.fields + view.field_count, p->fields + s.first_field, s.field_count*sizeof(*view.fields)); }
        s.first_field = view.field_count;
        view.field_count += s.field_count;
        view.structs[view.struct_count++] = s;
    }
    for (size_t i = 0; i < p->enum_count; ++i) {
        AtriEnum e = p->enums[i];
        if (!needed[declaration_at(p, e.name.start)]) { continue; }
        memcpy(view.variants + view.variant_count, p->variants + e.first_variant, e.variant_count*sizeof(*view.variants));
        e.first_variant = view.variant_count;
        view.variant_count += e.variant_count;
        view.enums[view.enum_count++] = e;
    }
    for (size_t i = 0; i < p->run_count; ++i) {
        AtriId id = p->runs[i];
        if (needed[declaration_at(p, p->statements[id].span.start)]) { view.runs[view.run_count++] = id; }
    }
    ok = atri_lower(source, &view, ir, d);
done:
    free(needed); free(pending);
    free(view.functions); free(view.globals); free(view.structs); free(view.fields);
    free(view.enums); free(view.variants); free(view.runs); free(view.symbols);
    return ok;
}
