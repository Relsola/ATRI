#ifndef ATRI_VM_BYTECODE_H
#define ATRI_VM_BYTECODE_H

#include "ir.h"

/* AtriIRInst 即带类型的寄存器字节码。VM 只为解释时的跳转和未完成的求值义务建索引。 */
typedef struct {
    const AtriIRInst *code;
    AtriId *labels, *constants, *skip_initializer;
} AtriVmFunction;

typedef struct { uint64_t bits; const unsigned char *text; size_t length; } AtriVmArgument;
struct AtriRunCache *atri_run_cache_create(void);
bool atri_vm_prepare(const AtriIRFunction *function, AtriVmFunction *output, AtriDiagnostic *diagnostic);
void atri_vm_release(AtriVmFunction *functions, size_t count);
bool atri_vm_generate(AtriIRProgram *program, AtriVmFunction *functions, AtriId function,
                      const AtriVmArgument *arguments, char **text, size_t *length, AtriDiagnostic *diagnostic);

#endif
