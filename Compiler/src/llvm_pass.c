#include "clang_driver.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* 只在 O1 加载 LLVM C ABI。LLVM 的 Windows 二进制发行包提供 LLVM-C.dll，
 * 不要求 O0 链接 LLVM，也不要求安装 C++ SDK 或 opt。 */
typedef void *Ref;
#define LLVM_API(X)                                                                                               \
    X(Ref, ContextCreate, (void))                                                                                 \
    X(void, ContextDispose, (Ref))                                                                                \
    X(Ref, CreateMemoryBufferWithMemoryRangeCopy, (const char *, size_t, const char *))                           \
    X(int, ParseIRInContext, (Ref, Ref, Ref *, char **))                                                          \
    X(void, DisposeModule, (Ref))                                                                                 \
    X(char *, PrintModuleToString, (Ref))                                                                         \
    X(void, DisposeMessage, (char *))                                                                             \
    X(Ref, GetFirstFunction, (Ref))                                                                               \
    X(Ref, GetNextFunction, (Ref))                                                                                \
    X(Ref, GetFirstBasicBlock, (Ref))                                                                             \
    X(Ref, GetNextBasicBlock, (Ref))                                                                              \
    X(Ref, GetFirstInstruction, (Ref))                                                                            \
    X(Ref, GetNextInstruction, (Ref))                                                                             \
    X(unsigned, GetInstructionOpcode, (Ref))                                                                      \
    X(Ref, GetOperand, (Ref, unsigned))                                                                           \
    X(Ref, IsAConstantInt, (Ref))                                                                                 \
    X(unsigned long long, ConstIntGetZExtValue, (Ref))                                                            \
    X(Ref, TypeOf, (Ref))                                                                                         \
    X(Ref, ConstInt, (Ref, unsigned long long, int))                                                              \
    X(void, ReplaceAllUsesWith, (Ref, Ref))                                                                       \
    X(void, InstructionEraseFromParent, (Ref))                                                                    \
    X(int, VerifyModule, (Ref, int, char **))

typedef struct {
#define FIELD(result, name, args) result(*name) args;
    LLVM_API(FIELD)
#undef FIELD
} Api;

static Ref simplify(const Api *api, Ref inst) {
    /* LLVMOpcode 中这些编号是公开 C ABI 的一部分。输入来自 ATRI 的无溢出标志整数运算。 */
    enum { ADD = 8, SUB = 10, MUL = 12, AND = 23, OR = 24, XOR = 25 };
    unsigned op = api->GetInstructionOpcode(inst);
    if (op != ADD && op != SUB && op != MUL && op != AND && op != OR && op != XOR) {
        return NULL;
    }
    Ref a = api->GetOperand(inst, 0), b = api->GetOperand(inst, 1);
    bool ac = api->IsAConstantInt(a) != NULL, bc = api->IsAConstantInt(b) != NULL;
    uint64_t x = ac ? api->ConstIntGetZExtValue(a) : 0, y = bc ? api->ConstIntGetZExtValue(b) : 0;
    if (ac && bc) {
        uint64_t bits = 0;
        switch (op) {
        case ADD:
            bits = x + y;
            break;
        case SUB:
            bits = x - y;
            break;
        case MUL:
            bits = x * y;
            break;
        case AND:
            bits = x & y;
            break;
        case OR:
            bits = x | y;
            break;
        case XOR:
            bits = x ^ y;
            break;
        }
        return api->ConstInt(api->TypeOf(inst), bits, 0);
    }
    if (bc && ((!y && (op == ADD || op == SUB || op == OR || op == XOR)) || (y == 1 && op == MUL))) {
        return a;
    }
    if (ac && ((!x && (op == ADD || op == OR || op == XOR)) || (x == 1 && op == MUL))) {
        return b;
    }
    return NULL;
}

bool atri_llvm_o1(FILE *input, FILE *output, AtriDiagnostic *d) {
    Api api;
#ifdef _WIN32
    char path[MAX_PATH], *name = NULL;
    DWORD length_found = SearchPathA(NULL, "clang.exe", NULL, MAX_PATH, path, &name);
    HMODULE library = NULL;
    if (length_found && length_found < MAX_PATH && name &&
        (size_t)(name - path) + sizeof("LLVM-C.dll") <= sizeof(path)) {
        strcpy(name, "LLVM-C.dll");
        library = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
#define LOAD(result, name, args)                                                                                  \
    do {                                                                                                          \
        FARPROC sym = GetProcAddress(library, "LLVM" #name);                                                      \
        if (!sym) {                                                                                               \
            goto missing;                                                                                         \
        }                                                                                                         \
        memcpy(&api.name, &sym, sizeof(sym));                                                                     \
    } while (0);
#else
    void *library = dlopen("libLLVM.so", RTLD_LAZY | RTLD_LOCAL);
#define LOAD(result, name, args)                                                                                  \
    do {                                                                                                          \
        void *sym = dlsym(library, "LLVM" #name);                                                                 \
        if (!sym) {                                                                                               \
            goto missing;                                                                                         \
        }                                                                                                         \
        memcpy(&api.name, &sym, sizeof(sym));                                                                     \
    } while (0);
#endif
    bool ok = false;
    if (!library) {
        goto missing;
    }
    LLVM_API(LOAD)
#undef LOAD
    size_t length;
    char *ir = atri_read_stream(input, &length);
    if (!ir) {
        goto done;
    }
    Ref context = api.ContextCreate(), module = NULL;
    Ref buffer = api.CreateMemoryBufferWithMemoryRangeCopy(ir, length, "ATRI");
    free(ir);
    char *message = NULL;
    if (api.ParseIRInContext(context, buffer, &module, &message)) {
        fprintf(stderr, "%s", message);
        api.DisposeMessage(message);
    } else {
        for (Ref fn = api.GetFirstFunction(module); fn; fn = api.GetNextFunction(fn)) {
            for (Ref block = api.GetFirstBasicBlock(fn); block; block = api.GetNextBasicBlock(block)) {
                for (Ref inst = api.GetFirstInstruction(block); inst;) {
                    Ref next = api.GetNextInstruction(inst), replacement = simplify(&api, inst);
                    if (replacement) {
                        api.ReplaceAllUsesWith(inst, replacement);
                        api.InstructionEraseFromParent(inst);
                    }
                    inst = next;
                }
            }
        }
        if (!api.VerifyModule(module, 2, &message)) {
            char *text = api.PrintModuleToString(module);
            ok = text && fputs(text, output) >= 0;
            if (text) {
                api.DisposeMessage(text);
            }
        } else if (message) {
            fprintf(stderr, "%s", message);
        }
        if (message) {
            api.DisposeMessage(message);
        }
        api.DisposeModule(module);
    }
    api.ContextDispose(context);
    goto done;
missing:
    *d = (AtriDiagnostic){.message = "O1 needs the LLVM C shared library on the toolchain search path"};
done:
#ifdef _WIN32
    if (library) {
        FreeLibrary(library);
    }
#else
    if (library) {
        dlclose(library);
    }
#endif
    return ok;
}
