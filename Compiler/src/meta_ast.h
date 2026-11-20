#ifndef ATRI_META_AST_H
#define ATRI_META_AST_H
#include "meta_plugin.h"
#include "frontend.h"

typedef struct {
    AtriSource source;
    const AtriProgram *ast;
    AtriProgram parsed;
    AtriSpan range;
    uint64_t event, generation;
    char *changed, *invalid;
    AtriDiagnostic invalid_diagnostic;
    char **strings;
    size_t count, capacity;
    bool failed;
} AtriAstView;

void atri_ast_begin(AtriAstView *view, AtriSource source, const AtriProgram *ast, AtriSpan range, uint64_t event);
void atri_ast_end(AtriAstView *view);
#endif
