#include "vm_memory.h"
#include "array.h"
#include <limits.h>

static AtriVmObject *object(AtriVmMemory *m, uint64_t ptr) {
    uint64_t id = ptr >> 32;
    return id && id <= m->count ? &m->objects[id - 1] : NULL;
}

uint64_t atri_vm_object(AtriVmMemory *m, size_t size, AtriVmStorage kind, const void *initial) {
    if (m->count >= UINT32_MAX || size > 64u * 1024u * 1024u - m->allocated) {
        return 0;
    }
    AtriVmObject o = {.size = size, .kind = kind};
    if (kind != VM_FILE) {
        o.bytes = calloc(size ? size : 1, 1);
        if (!o.bytes) {
            return 0;
        }
        if (initial) {
            memcpy(o.bytes, initial, size);
        }
    }
    if (!atri_append((void **)&m->objects, &m->count, &m->capacity, sizeof(o), &o)) {
        free(o.bytes);
        return 0;
    }
    m->allocated += size;
    return (uint64_t)m->count << 32;
}

void atri_vm_drop(AtriVmMemory *m, uint64_t addr) {
    AtriVmObject *o = object(m, addr);
    if (o) {
        if (o->file) {
            fclose(o->file);
        }
        free(o->bytes);
        m->allocated -= o->size;
        *o = (AtriVmObject){0};
    }
}

void atri_vm_memory_destroy(AtriVmMemory *m) {
    for (size_t i = 0; i < m->count; ++i) {
        atri_vm_drop(m, (uint64_t)(i + 1) << 32);
    }
    free(m->objects);
    free(m->literals);
}

unsigned char *atri_vm_bytes(AtriVmMemory *m, uint64_t ptr, size_t size, bool write) {
    AtriVmObject *o = object(m, ptr);
    size_t offset = (uint32_t)ptr;
    if (!o || !o->bytes || offset > o->size || size > o->size - offset || (write && o->kind == VM_LITERAL)) {
        return NULL;
    }
    return o->bytes + offset;
}

const char *atri_vm_text(AtriVmMemory *m, uint64_t ptr, size_t *length) {
    unsigned char *p = atri_vm_bytes(m, ptr, 0, false);
    if (!p) {
        return NULL;
    }
    const unsigned char *end = memchr(p, 0, object(m, ptr)->size - (uint32_t)ptr);
    if (!end) {
        return NULL;
    }
    *length = (size_t)(end - p);
    return (const char *)p;
}

bool atri_vm_offset(AtriVmMemory *m, uint64_t ptr, bool negative, uint64_t index, size_t stride, uint64_t *out) {
    AtriVmObject *o = object(m, ptr);
    uint32_t offset = (uint32_t)ptr;
    if (!o || !o->bytes || offset > o->size || !stride) {
        return false;
    }
    size_t room = negative ? offset : o->size - offset;
    if (index > room / stride) {
        return false;
    }
    *out = negative ? ptr - index * stride : ptr + index * stride;
    return true;
}

static bool named(AtriSource source, AtriSpan name, const char *s) {
    return name.end - name.start == strlen(s) && !memcmp(source.text + name.start, s, strlen(s));
}

static bool pointer_kind(const AtriIRProgram *p, AtriIRType type, char expected) {
    if (expected == 's') {
        return type == ATRI_IR_PTR_U8;
    }
    if (expected == 'p') {
        return type == ATRI_IR_PTR_VOID;
    }
    if (expected == 'i') {
        return type == ATRI_IR_I32;
    }
    if (expected == 'l') {
        return type == ATRI_IR_U64;
    }
    if (expected == 'v') {
        return type == ATRI_IR_VOID;
    }
    AtriIRType to = atri_ir_pointee(type);
    return atri_ir_is_struct(to) && p->structs[to - ATRI_IR_STRUCT].opaque &&
           named(p->source, p->structs[to - ATRI_IR_STRUCT].name, "FILE");
}

typedef enum {
    OPEN,
    CLOSE,
    READ,
    WRITE,
    EOF_TEST,
    FILE_ERROR,
    FLUSH,
    GET_CHAR,
    PUT_CHAR,
    PUT_STRING,
    STR_LEN,
    COPY,
    ALLOC,
    DEALLOC
} HostOp;
typedef struct {
    const char *name, *signature;
    HostOp op;
} HostFunction;

static bool file_mode(const char *s) {
    if (!s || !*s || !strchr("rwa", *s)) {
        return false;
    }
    bool update = false, binary_or_text = false;
    for (size_t i = 1; s[i]; ++i) {
        if (s[i] == '+' && !update) {
            update = true;
        } else if ((s[i] == 'b' || s[i] == 't') && !binary_or_text) {
            binary_or_text = true;
        } else if (s[i] == 'x' && *s == 'w' && !s[i + 1]) {
            return true;
        } else {
            return false;
        }
    }
    return true;
}
bool atri_vm_host(AtriVmMemory *m, const AtriIRProgram *p, const AtriIRFunction *f, const uint64_t *a, size_t argc,
                  uint64_t *result, AtriSpan span, AtriDiagnostic *d) {
    /* 签名首字母为返回类型。宿主 FILE* 只存于对象表；向 libc 传缓冲区前检查实际字节范围。 */
    static const HostFunction functions[] = {
        {"fopen", "fss", OPEN},     {"fclose", "if", CLOSE},   {"fread", "lpllf", READ},
        {"fwrite", "lpllf", WRITE}, {"feof", "if", EOF_TEST},  {"ferror", "if", FILE_ERROR},
        {"fflush", "if", FLUSH},    {"fgetc", "if", GET_CHAR}, {"fputc", "iif", PUT_CHAR},
        {"puts", "is", PUT_STRING}, {"strlen", "ls", STR_LEN}, {"memcpy", "pppl", COPY},
        {"malloc", "pl", ALLOC},    {"free", "vp", DEALLOC}};
    const HostFunction *host = NULL;
    for (size_t i = 0; i < sizeof(functions) / sizeof(*functions); ++i) {
        if (named(p->source, f->name, functions[i].name)) {
            host = &functions[i];
            break;
        }
    }
    if (!host) {
        *d = (AtriDiagnostic){.span = span, .message = "unbound host C function cannot execute at compile time"};
        atri_diagnostic_note(d, f->name, "extern(C) declared here");
        return false;
    }
    bool matches = !f->variadic && strlen(host->signature) == argc + 1 && argc == f->parameter_count &&
                   pointer_kind(p, f->return_type, host->signature[0]);
    for (size_t i = 0; matches && i < argc; ++i) {
        matches = pointer_kind(p, f->parameter_types[i], host->signature[i + 1]);
    }
    if (!matches) {
        *d = (AtriDiagnostic){.span = span, .message = "host C function signature does not match its declaration"};
        atri_diagnostic_note(d, f->name, "extern(C) declared here");
        return false;
    }
    *result = 0;
    size_t length;
    if (host->op == OPEN || host->op == PUT_STRING || host->op == STR_LEN) {
        const char *text = atri_vm_text(m, a[0], &length);
        if (!text) {
            goto invalid;
        }
        if (host->op == STR_LEN) {
            *result = length;
            return true;
        }
        if (host->op == PUT_STRING) {
            *result = (uint32_t)puts(text);
            return true;
        }
        const char *mode = atri_vm_text(m, a[1], &length);
        if (!file_mode(mode)) {
            goto invalid;
        }
        FILE *stream = fopen(text, mode);
        if (!stream) {
            return true;
        }
        *result = atri_vm_object(m, 0, VM_FILE, NULL);
        if (!*result) {
            fclose(stream);
            goto exhausted;
        }
        object(m, *result)->file = stream;
        return true;
    }
    if (host->op == ALLOC) {
        *result = atri_vm_object(m, a[0], VM_HEAP, NULL);
        return true;
    }
    if (host->op == DEALLOC) {
        if (!a[0]) {
            return true;
        }
        AtriVmObject *o = object(m, a[0]);
        if (!o || !o->bytes || o->kind != VM_HEAP || (uint32_t)a[0]) {
            goto invalid;
        }
        atri_vm_drop(m, a[0]);
        return true;
    }
    if (host->op == COPY) {
        unsigned char *dst = atri_vm_bytes(m, a[0], a[2], true), *src = atri_vm_bytes(m, a[1], a[2], false);
        if (!dst || !src) {
            goto invalid;
        }
        memmove(dst, src, a[2]);
        *result = a[0];
        return true;
    }
    uint64_t handle = a[argc - 1];
    AtriVmObject *o = object(m, handle);
    if (!o || !o->file || (uint32_t)handle) {
        goto invalid;
    }
    switch (host->op) {
    case CLOSE:
        *result = (uint32_t)fclose(o->file);
        o->file = NULL;
        break;
    case EOF_TEST:
        *result = (uint32_t)feof(o->file);
        break;
    case FILE_ERROR:
        *result = (uint32_t)ferror(o->file);
        break;
    case FLUSH:
        *result = (uint32_t)fflush(o->file);
        break;
    case GET_CHAR:
        *result = (uint32_t)fgetc(o->file);
        break;
    case PUT_CHAR:
        *result = (uint32_t)fputc((unsigned char)a[0], o->file);
        break;
    case READ:
    case WRITE: {
        if (a[1] && a[2] > SIZE_MAX / a[1]) {
            goto invalid;
        }
        if (!a[1] || !a[2]) {
            return true;
        }
        unsigned char *buffer = atri_vm_bytes(m, a[0], a[1] * a[2], host->op == READ);
        if (!buffer) {
            goto invalid;
        }
        *result = host->op == READ ? fread(buffer, a[1], a[2], o->file) : fwrite(buffer, a[1], a[2], o->file);
        break;
    }
    default:
        break;
    }
    return true;
invalid:
    *d = (AtriDiagnostic){.span = span,
                          .message = "invalid buffer, string or file handle in compile-time host call"};
    return false;
exhausted:
    *d = (AtriDiagnostic){.span = span, .message = "compile-time VM exhausted memory"};
    return false;
}
