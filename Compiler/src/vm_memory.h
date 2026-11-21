#ifndef ATRI_VM_MEMORY_H
#define ATRI_VM_MEMORY_H
#include "ir.h"

typedef enum { VM_LOCAL, VM_HEAP, VM_LITERAL, VM_FILE } AtriVmStorage;
typedef struct {
    unsigned char *bytes;
    size_t size;
    FILE *file;
    AtriVmStorage kind;
} AtriVmObject;
typedef struct {
    AtriVmObject *objects;
    size_t count, capacity, allocated;
    uint64_t *literals;
} AtriVmMemory;

uint64_t atri_vm_object(AtriVmMemory *m, size_t size, AtriVmStorage kind, const void *initial);
void atri_vm_drop(AtriVmMemory *m, uint64_t address);
void atri_vm_memory_destroy(AtriVmMemory *m);
unsigned char *atri_vm_bytes(AtriVmMemory *m, uint64_t ptr, size_t size, bool write);
const char *atri_vm_text(AtriVmMemory *m, uint64_t ptr, size_t *length);
bool atri_vm_view(AtriVmMemory *m, uint64_t descriptor, const unsigned char **bytes, size_t *length);
uint64_t atri_vm_string_literal(AtriVmMemory *m, const unsigned char *bytes, size_t length, bool view);
bool atri_vm_offset(AtriVmMemory *m, uint64_t ptr, bool negative, uint64_t index, size_t stride, uint64_t *result);
bool atri_vm_host(AtriVmMemory *m, const AtriIRProgram *p, const AtriIRFunction *f, const uint64_t *args,
                  size_t argc, uint64_t *result, AtriSpan site, AtriDiagnostic *d);

#endif
