#include "meta_plugin.h"
#include <stdio.h>
#include <string.h>

__declspec(dllexport) int32_t atri_register_v1(void) {
    return !(atri_register_annotation("left") && atri_register_infix("left", "<>", 19, 0) &&
             atri_register_annotation("right") && atri_register_infix("right", "<>", 19, 1));
}

__declspec(dllexport) int32_t atri_on_decl_v1(uint64_t decl, uint32_t kind, const char *module,
        const char *name, const char *source, const char **replacement, const char **injection) {
    (void)kind;
    (void)module;
    (void)source;
    (void)injection;
    if (!strcmp(name, "many")) {
        *replacement = "#insert #run emit()";
        return 0;
    }
    if (atri_ast_has_annotation(decl, "skip")) { return 0; }
    uint64_t i = 0;
    while (i < atri_ast_expression_count(decl)) {
        uint64_t node = atri_ast_expression(decl, i++);
        const char *op = atri_ast_operator(node);
        char code[256];
        int size;
        if (!strcmp(op, "&") && atri_ast_has_annotation(decl, "deref")) {
            size = snprintf(code, sizeof(code), "*(%s)", atri_ast_source(atri_ast_operand(node, 0)));
        } else if (!strcmp(op, "<>")) {
            const char *a = atri_ast_source(atri_ast_operand(node, 0));
            const char *b = atri_ast_source(atri_ast_operand(node, 1));
            size = snprintf(code, sizeof(code), "(%s) - (%s)", a, b);
        } else { continue; }
        if (size < 0 || (size_t)size >= sizeof(code) || !atri_ast_replace(node, code)) { return 1; }
        i = 0;
    }
    return 0;
}
