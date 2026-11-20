#ifndef ATRI_CLANG_DRIVER_H
#define ATRI_CLANG_DRIVER_H
#include "ir.h"

bool atri_llvm_optimize(FILE *out, AtriSource source, const AtriIRProgram *ir, unsigned level, bool llvm_text, AtriDiagnostic *d);
bool atri_llvm_o1(FILE *input, FILE *output, AtriDiagnostic *d);
bool atri_run_clang(const char *const *args, FILE *input, FILE *output);
bool atri_native_file(const char *path, AtriSource source, const AtriIRProgram *ir, unsigned level, bool link, AtriDiagnostic *d);
#endif
