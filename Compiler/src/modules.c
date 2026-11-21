#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "frontend.h"
#include "meta.h"
#include "lexer.h"
#include "source_map.h"
#include "array.h"
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    AtriModule *modules;
    char **texts;
    size_t text_count;
    size_t count, capacity, text_capacity, length;
    AtriDiagnostic *diagnostic;
    const AtriSource *overlays;
    size_t overlay_count;
} Loader;

char *atri_read_stream(FILE *stream, size_t *length) {
    size_t capacity = 4096;
    char *text = malloc(capacity + 1);
    if (text == NULL) {
        return NULL;
    }
    *length = 0;
    for (;;) {
        if (*length == capacity) {
            if (capacity > (SIZE_MAX - 1) / 2) {
                free(text);
                return NULL;
            }
            capacity *= 2;
            char *grown = realloc(text, capacity + 1);
            if (grown == NULL) {
                free(text);
                return NULL;
            }
            text = grown;
        }
        size_t count = fread(text + *length, 1, capacity - *length, stream);
        *length += count;
        if (count == 0) {
            if (ferror(stream)) {
                free(text);
                return NULL;
            }
            break;
        }
    }
    text[*length] = '\0';
    return text;
}

char *atri_canonical_path(const char *path) {
#ifdef _WIN32
    char *normalized = _fullpath(NULL, path, 0);
    if (!normalized) { return NULL; }
    HANDLE file = CreateFileA(normalized, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(normalized);
    if (file == INVALID_HANDLE_VALUE) { return NULL; }
    DWORD size = GetFinalPathNameByHandleA(file, NULL, 0, FILE_NAME_NORMALIZED);
    char *result = size ? malloc((size_t)size + 1) : NULL;
    if (result && !GetFinalPathNameByHandleA(file, result, size + 1, FILE_NAME_NORMALIZED)) {
        free(result); result = NULL;
    }
    CloseHandle(file);
    if (result && !strncmp(result, "\\\\?\\UNC\\", 8)) {
        memmove(result+2, result+8, strlen(result+8)+1); result[0] = result[1] = '\\';
    } else if (result && !strncmp(result, "\\\\?\\", 4)) {
        memmove(result, result+4, strlen(result+4)+1);
    }
    return result;
#else
    return realpath(path, NULL);
#endif
}

bool atri_standard_import(const char *text, AtriSpan span) {
    return span.end-span.start >= 7 && !memcmp(text+span.start+1, "std::", 5);
}

char *atri_import_path(const char *parent, const char *text, AtriSpan span) {
    if (span.end <= span.start + 2 || text[span.start+1] == '/') { return NULL; }
    const char *name = text + span.start + 1;
    size_t name_size = span.end - span.start - 2;
    if (atri_standard_import(text, span)) {
        name += 5;
        name_size -= 5;
        if (!name_size) { return NULL; }
        for (size_t i = 0; i < name_size; ++i) {
            char c = name[i];
            if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
                !(c >= '0' && c <= '9') && c != '_') { return NULL; }
        }
#ifdef _WIN32
        char directory[32768];
        DWORD n = GetModuleFileNameA(NULL, directory, sizeof(directory));
        if (!n || n >= sizeof(directory)) { return NULL; }
        char *slash = strrchr(directory, '\\');
        if (!slash) { return NULL; }
        *slash = 0;
        const char *suffix = "/stdlib/";
#else
        const char *directory = ATRI_STDLIB_DIR, *suffix = "/";
#endif
        size_t prefix = strlen(directory) + strlen(suffix);
        char *path = malloc(prefix + name_size + 4);
        if (path) {
            memcpy(path, directory, strlen(directory));
            memcpy(path + strlen(directory), suffix, strlen(suffix));
            memcpy(path + prefix, name, name_size);
            memcpy(path + prefix + name_size, ".ar", 4);
        }
        return path;
    }
    if (!strcmp(parent, "<stdin>")) { return NULL; }
    for (size_t i = span.start + 1; i < span.end - 1; ++i) {
        if (text[i] == '\\' || text[i] == ':') { return NULL; }
    }
    size_t end = strlen(parent);
    while (end && parent[end-1] != '/' && parent[end-1] != '\\') { --end; }
    size_t length = span.end - span.start - 2;
    char *result = malloc(end + length + 4);
    if (!result) { return NULL; }
    memcpy(result, parent, end);
    memcpy(result + end, text + span.start + 1, length);
    memcpy(result + end + length, ".ar", 4);
    return result;
}

static bool load(Loader *loader, char *path, char *text, size_t length, size_t depth) {
    if (depth > 256 || length > SIZE_MAX-loader->length-1) {
        *loader->diagnostic = (AtriDiagnostic){.span = {0}, .message = "module graph exceeds resource limits"};
        free(path); free(text); return false;
    }
    size_t index = loader->count;
    AtriModule module = {.path = path, .span = {loader->length, loader->length + length}};
    if (!atri_append((void **)&loader->modules, &loader->count, &loader->capacity, sizeof(module), &module)) {
        free(path); free(text); return false;
    }
    size_t text_count = index;
    if (!atri_append((void **)&loader->texts, &text_count, &loader->text_capacity, sizeof(text), &text)) {
        free(text); free(loader->modules[--loader->count].path); return false;
    }
    loader->text_count = text_count;
    loader->length += length + 1;
    AtriLexer lexer = atri_lexer_create((AtriSource){path, text, length});
    for (;;) {
        AtriToken token = atri_lexer_next(&lexer, loader->diagnostic);
        /* 本次只发现 import。实际解析用声明自己的运算符表，并报告词法诊断。 */
        if (token.kind == ATRI_TOKEN_ERROR) { continue; }
        if (token.kind == ATRI_TOKEN_EOF) { return true; }
        if (token.kind != ATRI_TOKEN_IMPORT) { continue; }
        token = atri_lexer_next(&lexer, loader->diagnostic);
        if (token.kind != ATRI_TOKEN_STRING) {
            *loader->diagnostic = (AtriDiagnostic){.span = {module.span.start + token.span.start, module.span.start + token.span.end}, .message = "expected a module path"};
            return false;
        }
        char *candidate = atri_import_path(path, text, token.span);
        char *resolved = candidate ? atri_canonical_path(candidate) : NULL;
        free(candidate);
        if (!resolved) {
            const char *message = atri_standard_import(text, token.span) ? "cannot resolve standard library module" :
                !strcmp(path, "<stdin>") ? "relative imports require a source file path" : "cannot resolve relative .ar module path";
            *loader->diagnostic = (AtriDiagnostic){.span = {module.span.start + token.span.start, module.span.start + token.span.end}, .message = message};
            return false;
        }
        bool found = false;
        for (size_t i = 0; i < loader->count; ++i) {
            if (!strcmp(resolved, loader->modules[i].path)) { found = true; break; }
        }
        if (found) { free(resolved); continue; }
        for (size_t i = 0; i < loader->overlay_count; ++i) {
            const AtriSource *overlay = &loader->overlays[i];
            if (strcmp(resolved, overlay->name)) { continue; }
            char *copy = malloc(overlay->length+1);
            if (!copy) { free(resolved); return false; }
            memcpy(copy, overlay->text, overlay->length);
            copy[overlay->length] = 0;
            if (!load(loader, resolved, copy, overlay->length, depth+1)) { return false; }
            found = true;
            break;
        }
        if (found) { continue; }
        FILE *file = fopen(resolved, "rb");
        if (!file) { free(resolved); return false; }
        size_t count = 0, capacity = 0;
        char *bytes = NULL;
        int byte;
        bool ok = true;
        while ((byte = fgetc(file)) != EOF) {
            char value = (char)byte;
            if (!atri_append((void **)&bytes, &count, &capacity, 1, &value)) { ok = false; break; }
        }
        if (ferror(file)) { ok = false; }
        fclose(file);
        char zero = 0;
        if (!atri_append((void **)&bytes, &count, &capacity, 1, &zero)) { ok = false; }
        if (!ok) { free(bytes); free(resolved); return false; }
        if (!load(loader, resolved, bytes, count-1, depth+1)) { return false; }
    }
}

AtriId atri_standard_module(AtriSource source, const AtriProgram *program, const char *path) {
    size_t length = strlen(path);
    for (size_t i = 0; i < program->import_count; ++i) {
        AtriSpan name = program->imports[i].path;
        if (name.end - name.start == length + 2 && !memcmp(source.text + name.start + 1, path, length)) {
            return program->imports[i].target;
        }
    }
    return ATRI_NONE;
}

AtriId atri_module_at(const AtriProgram *program, size_t offset) {
    size_t lo = 0, hi = program->module_count;
    while (lo < hi) {
        size_t mid = lo + (hi-lo)/2;
        if (program->modules[mid].span.end < offset) { lo = mid+1; }
        else { hi = mid; }
    }
    return lo < program->module_count ? lo : 0;
}

static int symbol_order(const void *left, const void *right) {
    const AtriModuleSymbol *a = left, *b = right;
    if (a->owner != b->owner) { return a->owner < b->owner ? -1 : 1; }
    size_t length = a->length < b->length ? a->length : b->length;
    int order = memcmp(a->text, b->text, length);
    return order ? order : a->length < b->length ? -1 : a->length != b->length;
}
static const AtriModuleSymbol *find_symbol(AtriSource source, const AtriProgram *p, AtriId module, AtriSpan name) {
    if (p->symbol_count == 0) { return NULL; }
    AtriModuleSymbol key = {.owner = module, .text = source.text+name.start, .length = name.end-name.start};
    return bsearch(&key, p->symbols, p->symbol_count, sizeof(key), symbol_order);
}

/* 未限定名称查询当前模块的可见表；经模块别名查找时，只允许访问目标模块显式导出的声明。 */
static const AtriModuleSymbol *resolve_symbol(AtriSource source, const AtriProgram *p, size_t offset,
                                               AtriSpan name, AtriSpan qualifier) {
    AtriId owner = atri_module_at(p, offset);
    if (qualifier.start == qualifier.end) { return find_symbol(source, p, owner, name); }
    const AtriModuleSymbol *binding = find_symbol(source, p, owner, qualifier);
    if (!binding || binding->kind != ATRI_SYMBOL_MODULE) { return NULL; }
    const AtriModuleSymbol *symbol = find_symbol(source, p, binding->target, name);
    return symbol && symbol->exported ? symbol : NULL;
}

AtriId atri_find_declaration(AtriSource source, const AtriProgram *p, size_t offset,
                             AtriSpan name, AtriSpan qualifier, bool *global) {
    const AtriModuleSymbol *symbol = resolve_symbol(source, p, offset, name, qualifier);
    if (!symbol || (symbol->kind != ATRI_SYMBOL_FUNCTION && symbol->kind != ATRI_SYMBOL_GLOBAL)) { return ATRI_NONE; }
    *global = symbol->kind == ATRI_SYMBOL_GLOBAL;
    return symbol->target;
}
AtriId atri_find_struct(AtriSource source, const AtriProgram *p, size_t offset, AtriSpan name, AtriSpan qualifier) {
    const AtriModuleSymbol *symbol = resolve_symbol(source, p, offset, name, qualifier);
    return symbol && symbol->kind == ATRI_SYMBOL_STRUCT ? symbol->target : ATRI_NONE;
}
AtriId atri_find_enum(AtriSource source, const AtriProgram *p, size_t offset, AtriSpan name, AtriSpan qualifier) {
    const AtriModuleSymbol *symbol = resolve_symbol(source, p, offset, name, qualifier);
    return symbol && symbol->kind == ATRI_SYMBOL_ENUM ? symbol->target : ATRI_NONE;
}

AtriSpan atri_declaration_name(AtriSource source, const AtriProgram *p, AtriSpan name, AtriSpan qualifier) {
    const AtriModuleSymbol *symbol = resolve_symbol(source, p, name.start, name, qualifier);
    if (symbol) {
        switch (symbol->kind) {
            case ATRI_SYMBOL_FUNCTION: return p->functions[symbol->target].name;
            case ATRI_SYMBOL_GLOBAL: return p->globals[symbol->target].declaration.name;
            case ATRI_SYMBOL_STRUCT: return p->structs[symbol->target].name;
            case ATRI_SYMBOL_ENUM: return p->enums[symbol->target].name;
            case ATRI_SYMBOL_MODULE: break;
        }
    }
    return (AtriSpan){0};
}

static AtriModuleSymbol module_symbol(AtriSource source, const AtriProgram *p, AtriSpan name,
                                      AtriSymbolKind kind, AtriId target, bool exported) {
    return (AtriModuleSymbol){.text = source.text+name.start, .length = name.end-name.start, .span = name,
        .owner = atri_module_at(p, name.start), .kind = kind, .target = target, .exported = exported};
}
typedef struct {
    AtriModuleSymbol *items;
    size_t count, capacity;
} SymbolTable;

static bool append_symbol(SymbolTable *table, AtriModuleSymbol symbol, AtriDiagnostic *d) {
    if (!atri_append((void **)&table->items, &table->count, &table->capacity, sizeof(symbol), &symbol)) {
        *d = (AtriDiagnostic){.span = symbol.span, .message = "out of memory"};
        return false;
    }
    return true;
}

bool atri_prepare_symbols(AtriSource source, AtriProgram *p, AtriDiagnostic *d) {
    if (p->symbols_ready) { return true; }
    SymbolTable table = {0};
    for (size_t i = 0; i < p->function_count; ++i) {
        const AtriFunction *f = &p->functions[i];
        if (!append_symbol(&table, module_symbol(source, p, f->name, ATRI_SYMBOL_FUNCTION, i, f->exported), d)) { goto failure; }
    }
    for (size_t i = 0; i < p->global_count; ++i) {
        const AtriGlobal *g = &p->globals[i];
        if (!append_symbol(&table, module_symbol(source, p, g->declaration.name, ATRI_SYMBOL_GLOBAL, i, g->exported), d)) { goto failure; }
    }
    for (size_t i = 0; i < p->struct_count; ++i) {
        const AtriStruct *t = &p->structs[i];
        if (!append_symbol(&table, module_symbol(source, p, t->name, ATRI_SYMBOL_STRUCT, i, t->exported), d)) { goto failure; }
    }
    for (size_t i = 0; i < p->enum_count; ++i) {
        const AtriEnum *e = &p->enums[i];
        if (!append_symbol(&table, module_symbol(source, p, e->name, ATRI_SYMBOL_ENUM, i, e->exported), d)) { goto failure; }
    }
    /* 只遍历本轮导入前的原始声明。导入项保留声明 ID，仅在当前模块可见，不能被再次导出。 */
    size_t declaration_count = table.count;
    for (size_t i = 0; i < p->import_count; ++i) {
        const AtriImport *import = &p->imports[i];
        if (import->target == ATRI_NONE) {
            *d = (AtriDiagnostic){.span = import->path, .message = "imports require the module loader"};
            goto failure;
        }
        if (import->name.end) {
            AtriModuleSymbol binding = module_symbol(source, p, import->name, ATRI_SYMBOL_MODULE, import->target, false);
            binding.span = import->path;
            if (!append_symbol(&table, binding, d)) { goto failure; }
            continue;
        }
        AtriId owner = atri_module_at(p, import->path.start);
        for (size_t j = 0; j < declaration_count; ++j) {
            AtriModuleSymbol symbol = table.items[j];
            if (symbol.owner != import->target || !symbol.exported) { continue; }
            symbol.owner = owner;
            symbol.span = import->path;
            symbol.exported = false;
            if (!append_symbol(&table, symbol, d)) { goto failure; }
        }
    }
    if (table.count) { qsort(table.items, table.count, sizeof(*table.items), symbol_order); }
    for (size_t i = 1; i < table.count; ++i) {
        if (!symbol_order(&table.items[i-1], &table.items[i])) {
            AtriSpan first = table.items[i-1].span, second = table.items[i].span;
            if (first.start > second.start) { AtriSpan swap = first; first = second; second = swap; }
            *d = (AtriDiagnostic){.span = second, .message = "duplicate function, global or imported name"};
            atri_diagnostic_note(d, first, "previous declaration here");
            goto failure;
        }
    }
    p->symbols = table.items;
    p->symbol_count = table.count;
    p->symbols_ready = true;
    return true;
failure:
    free(table.items);
    return false;
}

static void program_label(FILE *stream, AtriSource source, const AtriProgram *program,
                          AtriSpan span, const char *level, const char *message, const char *detail) {
    const AtriSourceMap *map = NULL;
    if (program->module_count) {
        AtriModule module = program->modules[atri_module_at(program, span.start)];
        map = module.origins;
        source = (AtriSource){module.path, source.text + module.span.start, module.span.end-module.span.start};
        span.start -= module.span.start;
        span.end = span.end > module.span.end ? source.length : span.end-module.span.start;
    }
    atri_source_label(stream, map, source, span, level, message, detail);
}

void atri_program_diagnostic(FILE *stream, AtriSource source, const AtriProgram *program, AtriDiagnostic d) {
    program_label(stream, source, program, d.span, "error", d.message, d.detail);
    for (size_t i = 0; i < d.note_count; ++i) {
        program_label(stream, source, program, d.notes[i].span, "note", d.notes[i].message, "");
    }
    if (d.omitted) { fprintf(stream, "note: %zu further locations omitted\n", d.omitted); }
}

bool atri_load_program(AtriSource input, AtriSource *source, AtriProgram *program, AtriDiagnostic *diagnostic) {
    return atri_expand_program(input, NULL, 0, source, program, diagnostic);
}

bool atri_discover_sources(AtriSource input, const AtriSource *overlays, size_t overlay_count,
                       AtriSource *source, AtriProgram *program, AtriDiagnostic *diagnostic) {
    *diagnostic = (AtriDiagnostic){0};
    Loader loader = {.diagnostic = diagnostic, .overlays = overlays, .overlay_count = overlay_count};
    char *text = malloc(input.length+1);
    char *path = atri_canonical_path(input.name);
    if (!path) { path = malloc(strlen(input.name)+1); if (path) { strcpy(path, input.name); } }
    if (!text || !path) { free(text); free(path); *diagnostic = (AtriDiagnostic){.span = {0}, .message = "out of memory"}; return false; }
    memcpy(text, input.text, input.length); text[input.length] = 0;
    bool ok = load(&loader, path, text, input.length, 0);
    char *combined = calloc(loader.length+1, 1);
    if (!combined) { ok = false; }
    for (size_t i = 0; i < loader.count; ++i) {
        if (combined && loader.texts && i < loader.text_count && loader.texts[i]) {
            memcpy(combined+loader.modules[i].span.start, loader.texts[i], loader.modules[i].span.end-loader.modules[i].span.start);
            combined[loader.modules[i].span.end] = '\n';
        }
        if (loader.texts && i < loader.text_count) { free(loader.texts[i]); }
    }
    free(loader.texts);
    if (!combined) {
        for (size_t i = 0; i < loader.count; ++i) { free(loader.modules[i].path); }
        free(loader.modules); *diagnostic = (AtriDiagnostic){.span = {0}, .message = "out of memory"}; return false;
    }
    *source = (AtriSource){input.name, combined, loader.length};
    *program = (AtriProgram){0};
    program->modules = loader.modules; program->module_count = loader.count;
    if (!ok) { if (!diagnostic->message) { *diagnostic = (AtriDiagnostic){.span = {0}, .message = "cannot load module"}; } return false; }
    return true;
}
bool atri_bind_imports(AtriSource source, AtriProgram *program, AtriDiagnostic *diagnostic) {
    for (size_t i = 0; i < program->import_count; ++i) {
        AtriImport *item = &program->imports[i];
        if (item->target != ATRI_NONE) { continue; }
        AtriId owner = atri_module_at(program, item->path.start);
        char *candidate = atri_import_path(program->modules[owner].path, source.text, item->path);
        char *resolved = candidate ? atri_canonical_path(candidate) : NULL;
        free(candidate);
        for (size_t j = 0; resolved && j < program->module_count; ++j) {
            if (!strcmp(resolved, program->modules[j].path)) { item->target = j; break; }
        }
        free(resolved);
        if (item->target == ATRI_NONE) { *diagnostic = (AtriDiagnostic){.span = item->path, .message = "cannot resolve module"}; return false; }
    }
    return atri_prepare_symbols(source, program, diagnostic);
}
bool atri_load_sources(AtriSource input, const AtriSource *overlays, size_t overlay_count, bool index_declarations,
                       AtriSource *source, AtriProgram *program, AtriDiagnostic *diagnostic) {
    if (!atri_discover_sources(input, overlays, overlay_count, source, program, diagnostic)) { return false; }
    AtriModule *modules = program->modules;
    size_t count = program->module_count;
    bool ok = atri_parse_sources(*source, modules, count, index_declarations, program, diagnostic);
    program->modules = modules;
    program->module_count = count;
    return ok && atri_bind_imports(*source, program, diagnostic);
}
