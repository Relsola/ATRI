#include "meta_plugin.h"
#include <string.h>

__declspec(dllexport) int32_t message(uint64_t id, uint32_t kind, const char *module,
        const char *name, const char *source, const char **replacement, const char **injection) {
    (void)id;
    (void)module;
    if (kind == ATRI_META_FUNCTION && !strcmp(name, "main") && !strstr(source, "#run answer()")) {
        return 1;
    }
    if (kind == ATRI_META_FUNCTION && !strcmp(name, "answer")) {
        *replacement = "fn answer() -> i32 { return 43; }";
    }
    if (kind == ATRI_META_FUNCTION && !strcmp(name, "generate")) {
        *injection = "fn supplied() -> i32 { return 17; }";
    }
    if (kind == ATRI_META_FUNCTION && !strcmp(name, "invalid_edit")) {
        *replacement = "fn invalid_edit() -> i32 { return true; }";
    }
    if (kind == ATRI_META_FUNCTION && !strcmp(name, "reject_here")) { return 1; }
    return 0;
}
