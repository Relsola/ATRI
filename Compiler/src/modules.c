#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "frontend.h"
#include "lexer.h"
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
} Loader;

static char *canonical(const char *path) {
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

static char *import_path(const char *parent, const char *text, AtriSpan span) {
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
        *loader->diagnostic = (AtriDiagnostic){{0}, "module graph exceeds resource limits"};
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
        if (token.kind == ATRI_TOKEN_ERROR) {
            loader->diagnostic->span.start += module.span.start;
            loader->diagnostic->span.end += module.span.start;
            return false;
        }
        if (token.kind == ATRI_TOKEN_EOF) { return true; }
        if (token.kind != ATRI_TOKEN_IMPORT) { continue; }
        if (!strcmp(path, "<stdin>")) {
            *loader->diagnostic = (AtriDiagnostic){token.span, "relative imports require a source file path"}; return false;
        }
        token = atri_lexer_next(&lexer, loader->diagnostic);
        if (token.kind != ATRI_TOKEN_STRING) {
            *loader->diagnostic = (AtriDiagnostic){{module.span.start + token.span.start, module.span.start + token.span.end}, "expected a module path"};
            return false;
        }
        bool valid = token.span.end > token.span.start + 2 && text[token.span.start+1] != '/';
        for (size_t i = token.span.start + 1; i < token.span.end - 1; ++i) {
            if (text[i] == '\\' || text[i] == ':') { valid = false; }
        }
        char *candidate = valid ? import_path(path, text, token.span) : NULL;
        char *resolved = candidate ? canonical(candidate) : NULL;
        free(candidate);
        if (!resolved) {
            *loader->diagnostic = (AtriDiagnostic){{module.span.start + token.span.start, module.span.start + token.span.end}, "cannot resolve relative .ar module path"};
            return false;
        }
        bool found = false;
        for (size_t i = 0; i < loader->count; ++i) {
            if (!strcmp(resolved, loader->modules[i].path)) { found = true; break; }
        }
        if (found) { free(resolved); continue; }
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
    AtriModuleSymbol key = {.owner = module, .text = source.text+name.start, .length = name.end-name.start};
    return bsearch(&key, p->symbols, p->symbol_count, sizeof(key), symbol_order);
}

static bool same(AtriSource source, AtriSpan a, AtriSpan b) {
    return a.end-a.start == b.end-b.start && !memcmp(source.text+a.start, source.text+b.start, a.end-a.start);
}

/* 定义与导入保留独立身份；返回的是原声明 ID，不复制节点或函数体。 */
AtriId atri_find_declaration(AtriSource source, const AtriProgram *p, size_t offset,
                             AtriSpan name, AtriSpan qualifier, bool *global) {
    AtriId owner = atri_module_at(p, offset), target = owner;
    bool qualified = qualifier.end != qualifier.start;
    if (p->symbols) {
        if (qualified) {
            const AtriModuleSymbol *binding = find_symbol(source, p, owner, qualifier);
            if (!binding || binding->kind != 2) { return ATRI_NONE; }
            target = binding->target;
        }
        const AtriModuleSymbol *symbol = find_symbol(source, p, target, name);
        if (!symbol || symbol->kind >= 2 || (qualified && !symbol->exported)) { return ATRI_NONE; }
        *global = symbol->kind == 1; return symbol->target;
    }
    if (qualified) {
        target = ATRI_NONE;
        for (size_t i = 0; i < p->import_count; ++i) {
            const AtriImport *item = &p->imports[i];
            if (atri_module_at(p, item->path.start) == owner && same(source, qualifier, item->name)) { target = item->target; break; }
        }
        if (target == ATRI_NONE) { return ATRI_NONE; }
    }
    for (size_t kind = 0; kind < 2; ++kind) {
        size_t count = kind ? p->global_count : p->function_count;
        for (size_t i = 0; i < count; ++i) {
            AtriSpan candidate = kind ? p->globals[i].declaration.name : p->functions[i].name;
            bool exported = kind ? p->globals[i].exported : p->functions[i].exported;
            AtriId module = atri_module_at(p, candidate.start);
            bool visible = qualified ? module == target && exported : module == owner;
            if (!qualified && exported && module != owner) {
                for (size_t j = 0; j < p->import_count; ++j) {
                    const AtriImport *item = &p->imports[j];
                    if (atri_module_at(p, item->path.start) == owner && item->name.start == item->name.end && item->target == module) { visible = true; }
                }
            }
            if (visible && same(source, name, candidate)) { *global = kind != 0; return i; }
        }
    }
    return ATRI_NONE;
}

static AtriId find_type(AtriSource source, const AtriProgram *p, size_t offset, AtriSpan name, AtriSpan qualifier, unsigned kind) {
    AtriId owner = atri_module_at(p, offset), target = owner;
    if (p->symbols) {
        if (qualifier.end) {
            const AtriModuleSymbol *binding = find_symbol(source, p, owner, qualifier);
            if (!binding || binding->kind != 2) { return ATRI_NONE; }
            target = binding->target;
        }
        const AtriModuleSymbol *symbol = find_symbol(source, p, target, name);
        return symbol && symbol->kind == kind && (!qualifier.end || symbol->exported) ? symbol->target : ATRI_NONE;
    }
    size_t count = kind == 3 ? p->struct_count : p->enum_count;
    for (size_t i = 0; i < count; ++i) {
        AtriSpan candidate = kind == 3 ? p->structs[i].name : p->enums[i].name;
        if (atri_module_at(p, candidate.start) == owner && same(source, name, candidate) && !qualifier.end) { return i; }
    }
    return ATRI_NONE;
}
AtriId atri_find_struct(AtriSource source, const AtriProgram *p, size_t offset, AtriSpan name, AtriSpan qualifier) {
    return find_type(source, p, offset, name, qualifier, 3);
}
AtriId atri_find_enum(AtriSource source, const AtriProgram *p, size_t offset, AtriSpan name, AtriSpan qualifier) {
    return find_type(source, p, offset, name, qualifier, 4);
}

static bool build_symbols(AtriSource source, AtriProgram *p, AtriDiagnostic *diagnostic) {
    size_t capacity = 0;
    for (size_t phase = 0; phase < 2; ++phase) {
        size_t limit = phase ? p->import_count : p->function_count + p->global_count + p->struct_count + p->enum_count;
        for (size_t i = 0; i < limit; ++i) {
            size_t declarations = phase && !p->imports[i].name.end ? p->function_count + p->global_count + p->struct_count + p->enum_count : 1;
            for (size_t j = 0; j < declarations; ++j) {
                AtriModuleSymbol item = {0};
                if (phase && p->imports[i].target == ATRI_NONE) {
                    *diagnostic = (AtriDiagnostic){p->imports[i].path, "imports require the module loader"}; return false;
                }
                if (phase && p->imports[i].name.end) {
                    item.kind = 2; item.span = p->imports[i].name; item.target = p->imports[i].target;
                } else {
                    size_t index = phase ? j : i;
                    size_t types = p->function_count+p->global_count;
                    item.kind = index < p->function_count ? 0 : index < types ? 1 : index < types+p->struct_count ? 3 : 4;
                    item.target = item.kind == 0 ? index : item.kind == 1 ? index-p->function_count :
                        item.kind == 3 ? index-types : index-types-p->struct_count;
                    item.span = item.kind == 0 ? p->functions[item.target].name : item.kind == 1 ? p->globals[item.target].declaration.name :
                        item.kind == 3 ? p->structs[item.target].name : p->enums[item.target].name;
                    item.exported = item.kind == 0 ? p->functions[item.target].exported : item.kind == 1 ? p->globals[item.target].exported :
                        item.kind == 3 ? p->structs[item.target].exported : p->enums[item.target].exported;
                    if (phase && (!item.exported || atri_module_at(p, item.span.start) != p->imports[i].target)) { continue; }
                }
                item.owner = atri_module_at(p, phase ? p->imports[i].path.start : item.span.start);
                item.text = source.text+item.span.start; item.length = item.span.end-item.span.start;
                if (phase) { item.exported = false; item.span = p->imports[i].path; }
                if (!atri_append((void **)&p->symbols, &p->symbol_count, &capacity, sizeof(item), &item)) {
                    *diagnostic = (AtriDiagnostic){item.span, "out of memory"}; return false;
                }
            }
        }
    }
    if (p->symbol_count) { qsort(p->symbols, p->symbol_count, sizeof(*p->symbols), symbol_order); }
    for (size_t i = 1; i < p->symbol_count; ++i) {
        if (!symbol_order(&p->symbols[i-1], &p->symbols[i])) {
            *diagnostic = (AtriDiagnostic){p->symbols[i].span, "duplicate function, global or imported name"}; return false;
        }
    }
    return true;
}
bool atri_validate_modules(AtriSource source, const AtriProgram *p, AtriDiagnostic *diagnostic) {
    if (p->symbols) { return true; }
    AtriProgram temporary = *p;
    temporary.symbols = NULL; temporary.symbol_count = 0;
    bool ok = build_symbols(source, &temporary, diagnostic);
    free(temporary.symbols); return ok;
}

void atri_program_diagnostic(FILE *stream, AtriSource source, const AtriProgram *program, AtriDiagnostic diagnostic) {
    if (program->module_count) {
        AtriModule module = program->modules[atri_module_at(program, diagnostic.span.start)];
        source = (AtriSource){module.path, source.text + module.span.start, module.span.end-module.span.start};
        diagnostic.span.start -= module.span.start;
        diagnostic.span.end = diagnostic.span.end > module.span.end ? source.length : diagnostic.span.end-module.span.start;
    }
    atri_print_diagnostic(stream, source, diagnostic);
}

bool atri_load_program(AtriSource input, AtriSource *source, AtriProgram *program, AtriDiagnostic *diagnostic) {
    Loader loader = {.diagnostic = diagnostic};
    char *text = malloc(input.length+1);
    char *path = canonical(input.name);
    if (!path) { path = malloc(strlen(input.name)+1); if (path) { strcpy(path, input.name); } }
    if (!text || !path) { free(text); free(path); *diagnostic = (AtriDiagnostic){{0}, "out of memory"}; return false; }
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
        free(loader.modules); *diagnostic = (AtriDiagnostic){{0}, "out of memory"}; return false;
    }
    *source = (AtriSource){input.name, combined, loader.length};
    *program = (AtriProgram){0};
    if (ok) { ok = atri_parse_sources(*source, loader.modules, loader.count, program, diagnostic); }
    program->modules = loader.modules; program->module_count = loader.count;
    if (!ok) { if (!diagnostic->message) { *diagnostic = (AtriDiagnostic){{0}, "cannot load module"}; } return false; }
    for (size_t i = 0; i < program->import_count; ++i) {
        AtriImport *item = &program->imports[i];
        AtriId owner = atri_module_at(program, item->path.start);
        char *candidate = import_path(program->modules[owner].path, combined, item->path);
        char *resolved = candidate ? canonical(candidate) : NULL;
        free(candidate);
        for (size_t j = 0; resolved && j < program->module_count; ++j) {
            if (!strcmp(resolved, program->modules[j].path)) { item->target = j; break; }
        }
        free(resolved);
        if (item->target == ATRI_NONE) { *diagnostic = (AtriDiagnostic){item->path, "cannot resolve module"}; return false; }
    }
    return build_symbols(*source, program, diagnostic);
}
