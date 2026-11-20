#include "vm_bytecode.h"
#include <stdlib.h>

bool atri_vm_prepare(const AtriIRFunction *f, AtriVmFunction *out, AtriDiagnostic *d) {
    if (out->code) {
        return true;
    }
    out->labels = calloc(f->block_count ? f->block_count : 1, sizeof(*out->labels));
    if (f->constant_count) {
        out->constants = calloc(f->instruction_count, sizeof(*out->constants));
        out->skip_initializer = calloc(f->instruction_count, sizeof(*out->skip_initializer));
    }
    if (!out->labels || (f->constant_count && (!out->constants || !out->skip_initializer))) {
        free(out->labels);
        free(out->constants);
        free(out->skip_initializer);
        *out = (AtriVmFunction){0};
        *d = (AtriDiagnostic){.span = f->name, .message = "out of memory preparing VM function"};
        return false;
    }
    for (size_t i = 0; i < f->instruction_count; ++i) {
        if (f->instructions[i].kind == ATRI_IR_LABEL) {
            out->labels[f->instructions[i].immediate] = i;
        }
    }
    for (size_t i = 0; i < f->constant_count; ++i) {
        const AtriIRConstant *c = &f->constants[i];
        out->constants[c->result] = i + 1;
        if (c->first_instruction < c->result) {
            out->skip_initializer[c->first_instruction] = c->result;
        }
    }
    out->code = f->instructions;
    return true;
}

void atri_vm_release(AtriVmFunction *functions, size_t count) {
    if (!functions) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(functions[i].labels);
        free(functions[i].constants);
        free(functions[i].skip_initializer);
    }
    free(functions);
}
