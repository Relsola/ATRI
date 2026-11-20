#ifndef ATRI_META_PLUGIN_H
#define ATRI_META_PLUGIN_H
#include <stdint.h>

/* Windows x64 C ABI。插件导出 atri_on_decl_v1。指针指向零结尾 UTF-8，
 * 输入只读；插件自有的输出字符串至少保持到宿主在回调返回后复制完成。 */
enum {
    ATRI_META_FUNCTION = 1, ATRI_META_GLOBAL, ATRI_META_STRUCT, ATRI_META_UNION,
    ATRI_META_ENUM, ATRI_META_IMPORT, ATRI_META_RUN
};
typedef int32_t AtriMetaCallback(uint64_t id, uint32_t kind, const char *module,
    const char *name, const char *source, const char **replacement, const char **injection);

#if defined(_WIN32) && defined(ATRI_BUILD_HOST)
#define ATRI_HOST_EXPORT __declspec(dllexport)
#else
#define ATRI_HOST_EXPORT
#endif
/* 可选入口 atri_register_v1() 在本次编译开始时调用，返回 0 表示成功。
 * 注册 API 返回 1 表示成功，优先级 1..29，right 为 0（左结合）或 1（右结合）。 */
typedef int32_t AtriMetaRegister(void);
ATRI_HOST_EXPORT int32_t atri_register_annotation(const char *name);
ATRI_HOST_EXPORT int32_t atri_register_infix(const char *annotation, const char *symbol, uint32_t precedence, uint32_t right);
ATRI_HOST_EXPORT int32_t atri_ast_has_annotation(uint64_t decl, const char *name);
ATRI_HOST_EXPORT uint64_t atri_ast_expression_count(uint64_t decl);
ATRI_HOST_EXPORT uint64_t atri_ast_expression(uint64_t decl, uint64_t index);
ATRI_HOST_EXPORT const char *atri_ast_operator(uint64_t expression);
ATRI_HOST_EXPORT uint64_t atri_ast_operand(uint64_t expression, uint32_t side);
ATRI_HOST_EXPORT const char *atri_ast_source(uint64_t node);
ATRI_HOST_EXPORT int32_t atri_ast_replace(uint64_t node, const char *source);
#endif
