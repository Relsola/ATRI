#include "meta.h"
#include "meta_syntax.h"
#include "lexer.h"
#include "clang_driver.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

struct AtriMetaPlugin {
    HMODULE library;
    AtriMetaCallback *callback;
    char directory[MAX_PATH], assembly[MAX_PATH], dll[MAX_PATH];
};
static bool workspace(AtriMetaPlugin *p) {
    char temp[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(temp), temp);
    if (!length || length >= sizeof(temp)) { return false; }
    for (unsigned i = 0; i < 256; ++i) {
        char directory[MAX_PATH];
        int n = snprintf(directory, sizeof(directory), "%satri-%lu-%u", temp, GetCurrentProcessId(), i);
        if (n < 0 || (size_t)n + sizeof("\\listener.dll") > sizeof(directory)) { return false; }
        if (CreateDirectoryA(directory, NULL)) {
            strcpy(p->directory, directory);
            snprintf(p->assembly, sizeof(p->assembly), "%s\\listener.s", directory);
            snprintf(p->dll, sizeof(p->dll), "%s\\listener.dll", directory);
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) { return false; }
    }
    return false;
}
static bool callback_symbol(AtriSource source, const AtriIRProgram *ir, AtriId *function) {
    const char *name = "atri_on_decl_v1";
    AtriIRType output = atri_ir_pointer_type(ATRI_IR_PTR_U8);
    AtriIRType signature[] = {ATRI_IR_U64, ATRI_IR_U32, ATRI_IR_PTR_U8, ATRI_IR_PTR_U8, ATRI_IR_PTR_U8, output, output};
    *function = ATRI_NONE;
    for (size_t i = 0; i < ir->function_count; ++i) {
        const AtriIRFunction *f = &ir->functions[i];
        if (f->module || f->external_c) { continue; }
        size_t n = f->name.end - f->name.start;
        const char *text = source.text + f->name.start;
        if (n == 4 && !memcmp(text, "main", n)) { return false; }
        if (n == strlen(name) && !memcmp(text, name, n)) {
            if (f->return_type != ATRI_IR_I32 || f->parameter_count != sizeof(signature)/sizeof(*signature) ||
                memcmp(f->parameter_types, signature, sizeof(signature))) { return false; }
            *function = i;
        }
    }
    return *function != ATRI_NONE;
}
/* 追加独立的内建模块，为各插件模块中的 #keyword 提供同一份外部签名，同时保留原源码区间。 */
static bool bind_keyword(AtriSource *source, const char *input, AtriProgram *ast, AtriDiagnostic *d) {
    bool used = false;
    for (size_t i = 0; i < ast->expression_count; ++i) {
        if (ast->expressions[i].keyword) { used = true; break; }
    }
    if (!used) { return true; }
    static const char declaration[] = "#extern(C) fn atri_ast_source(node:u64)->*u8;";
    static const char module[] = "<compiler plugin API>";
    size_t start = source->length + 1;
    if (start > SIZE_MAX - sizeof(declaration)) { goto exhausted; }
    char *text = malloc(start + sizeof(declaration));
    char *name = malloc(sizeof(module));
    if (!text || !name) { free(text); free(name); goto exhausted; }
    AtriModule *modules = realloc(ast->modules, (ast->module_count + 1) * sizeof(*modules));
    if (!modules) { free(text); free(name); goto exhausted; }
    ast->modules = modules;
    strcpy(name, module);
    memcpy(text, source->text, source->length); text[source->length] = '\n';
    memcpy(text + start, declaration, sizeof(declaration));
    if (source->text != input) { free((char *)source->text); }
    source->text = text; source->length = start + sizeof(declaration) - 1;
    AtriSpan span = {start, source->length};
    ast->modules[ast->module_count++] = (AtriModule){.path = name, .span = span};
    free(ast->symbols); ast->symbols = NULL; ast->symbol_count = 0; ast->symbols_ready = false;
    ast->keyword_function = ast->function_count + 1;
    return atri_parse_fragment(*source, span, ast, d);
exhausted:
    *d = (AtriDiagnostic){.message = "out of memory binding #keyword"};
    return false;
}
static bool build_listener(AtriMetaPlugin *p, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) { return false; }
    size_t length;
    char *input = atri_read_stream(file, &length);
    fclose(file);
    if (!input) { return false; }
    AtriProgram ast = {0};
    AtriIRProgram ir = {0};
    AtriDiagnostic d = {0};
    AtriSource source = {path, input, length};
    bool ok = atri_load_program(source, &source, &ast, &d) && bind_keyword(&source, input, &ast, &d) &&
              atri_lower(source, &ast, &ir, &d);
    AtriId symbol;
    bool uses_host_api = false;
    if (ok && !callback_symbol(source, &ir, &symbol)) {
        fprintf(stderr, "%s: expected atri_on_decl_v1(u64,u32,*u8,*u8,*u8,**u8,**u8)->i32 in a module without main\n", path);
        ok = false;
    }
    if (ok && !workspace(p)) {
        fprintf(stderr, "%s: cannot create plugin workspace (Windows error %lu)\n", path, GetLastError());
        ok = false;
    }
    if (ok) {
        FILE *assembly = fopen(p->assembly, "wb");
        ok = assembly && atri_emit_x64(assembly, source, &ir, &d);
        if (ok) {
            ok = fprintf(assembly, "\n.section .drectve,\"yn\"\n.ascii \" /EXPORT:atri_on_decl_v1=atri_fn_%zu\"\n", symbol) >= 0;
        }
        for (size_t i=0; ok && i<ir.function_count; ++i) {
            const AtriIRFunction *fn=&ir.functions[i];
            size_t length = fn->name.end - fn->name.start;
            const char *name = source.text + fn->name.start;
            if (fn->external_c && ((length >= 9 && !memcmp(name, "atri_ast_", 9)) ||
                (length >= 14 && !memcmp(name, "atri_register_", 14)))) { uses_host_api = true; }
            const char reg[]="atri_register_v1";
            if (!fn->module && !fn->external_c && fn->name.end-fn->name.start==sizeof(reg)-1 &&
                !memcmp(source.text+fn->name.start,reg,sizeof(reg)-1)) {
                ok=fn->return_type==ATRI_IR_I32 && !fn->parameter_count;
                if (ok) { ok=fprintf(assembly,".ascii \" /EXPORT:atri_register_v1=atri_fn_%zu\"\n",i)>=0; }
                else { fprintf(stderr,"%s: expected atri_register_v1()->i32\n",path); }
            }
        }
        if (assembly && fclose(assembly)) { ok = false; }
    }
    if (!ok && d.message) { atri_program_diagnostic(stderr, source, &ast, d); }
    atri_program_destroy(&ast);
    atri_ir_destroy(&ir);
    if (source.text != input) { free((char *)source.text); }
    free(input);
    if (!ok) { return false; }
    char import_library[MAX_PATH+32];
    snprintf(import_library, sizeof(import_library), "-Wl,/implib:%s\\listener.lib", p->directory);
    char host_lib[MAX_PATH];
    const char *args[] = {"clang", "--target=x86_64-pc-windows-msvc", "-shared", p->assembly, import_library,
                          "-o", p->dll, NULL, NULL};
    if (uses_host_api) {
        DWORD size = GetModuleFileNameA(NULL, host_lib, MAX_PATH);
        if (size < 4 || size >= MAX_PATH) { return false; }
        strcpy(host_lib + size - 4, ".lib");
        args[7] = host_lib;
    }
    FILE *empty = tmpfile();
    ok = empty && atri_run_clang(args, empty, stderr);
    if (empty) { fclose(empty); }
    return ok;
}
void atri_meta_close(AtriMetaPlugin *p) {
    if (!p) { return; }
    if (p->library) { FreeLibrary(p->library); }
    if (*p->directory) {
        const char *names[] = {"listener.s", "listener.dll", "listener.lib", "listener.exp"};
        for (size_t i = 0; i < sizeof(names)/sizeof(*names); ++i) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s\\%s", p->directory, names[i]);
            DeleteFileA(path);
        }
        RemoveDirectoryA(p->directory);
    }
    free(p);
}
AtriMetaPlugin *atri_meta_open(const char *input) {
    AtriMetaPlugin *p = calloc(1, sizeof(*p));
    char *path = _fullpath(NULL, input, 0);
    bool ok = p && path;
    const char *ext = strrchr(input, '.');
    if (ok && ext && !_stricmp(ext, ".ar")) {
        ok = build_listener(p, path);
    } else if (!ext || _stricmp(ext, ".dll")) {
        ok = false;
    }
    if (ok) {
        p->library = LoadLibraryExA(*p->dll ? p->dll : path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        FARPROC entry = p->library ? GetProcAddress(p->library, "atri_on_decl_v1") : NULL;
        if (!entry) {
            fprintf(stderr, "%s: %s failed (Windows error %lu)\n", input,
                    p->library ? "GetProcAddress" : "LoadLibraryEx", GetLastError());
        }
        if (entry) { memcpy(&p->callback, &entry, sizeof(entry)); }
        ok = entry != NULL;
        FARPROC reg = p->library ? GetProcAddress(p->library,"atri_register_v1") : NULL;
        if (ok && reg) {
            AtriMetaRegister *register_syntax;
            memcpy(&register_syntax,&reg,sizeof(reg));
            atri_syntax_begin();
            int32_t status=register_syntax();
            ok = atri_syntax_end() && status==0;
        }
    }
    free(path);
    if (!ok) {
        fprintf(stderr, "%s: cannot prepare declaration listener (expected .ar or a DLL exporting atri_on_decl_v1)\n", input);
        atri_meta_close(p);
        return NULL;
    }
    return p;
}
AtriMetaCallback *atri_meta_callback(AtriMetaPlugin *p) { return p->callback; }
#else
AtriMetaPlugin *atri_meta_open(const char *path) {
    fprintf(stderr, "%s: native metaprograms currently require a Windows x64 host\n", path);
    return NULL;
}
void atri_meta_close(AtriMetaPlugin *p) { (void)p; }
AtriMetaCallback *atri_meta_callback(AtriMetaPlugin *p) { (void)p; return NULL; }
#endif
