#ifndef ATRI_FRONTEND_H
#define ATRI_FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *name;
    const char *text;
    size_t length;
} AtriSource;

/* 原始源码的半开字节区间 [start,end)，不复制名称或字面量；调用方必须保持源码存活。 */
typedef struct {
    size_t start;
    size_t end;
} AtriSpan;

typedef struct {
    AtriSpan span;
    const char *message;
} AtriDiagnostic;

typedef struct {
    size_t line;
    size_t column;
    size_t line_start;
    size_t line_end;
} AtriLocation;

typedef size_t AtriId;
#define ATRI_NONE ((AtriId)-1)
typedef enum {
    ATRI_ADD, ATRI_SUB, ATRI_MUL, ATRI_DIV, ATRI_REM, ATRI_NEG,
    ATRI_EQ, ATRI_NE, ATRI_LT, ATRI_LE, ATRI_GT, ATRI_GE, ATRI_AND, ATRI_OR, ATRI_NOT,
    ATRI_BIT_AND, ATRI_BIT_OR, ATRI_BIT_XOR, ATRI_BIT_NOT, ATRI_SHL, ATRI_SHR
} AtriOp;
typedef enum {
    ATRI_EXPR_INTEGER, ATRI_EXPR_CHARACTER, ATRI_EXPR_NAME, ATRI_EXPR_UNARY, ATRI_EXPR_BINARY, ATRI_EXPR_CALL,
    ATRI_EXPR_ADDRESS, ATRI_EXPR_DEREFERENCE, ATRI_EXPR_NULL, ATRI_EXPR_BOOL, ATRI_EXPR_STRING, ATRI_EXPR_FIELD, ATRI_EXPR_INITIALIZER,
    ATRI_EXPR_INDEX, ATRI_EXPR_CONDITIONAL, ATRI_EXPR_RUN, ATRI_EXPR_CAST, ATRI_EXPR_SIZEOF, ATRI_EXPR_ALIGNOF
} AtriExprKind;
typedef struct {
    AtriExprKind kind;
    AtriSpan span, text, qualifier, module;
    AtriOp op;
    AtriId left, right, alternative;
    size_t first_argument, argument_count;
    size_t tree_depth;
    bool boolean;
} AtriExpr;
typedef enum {
    ATRI_STMT_CONSTANT, ATRI_STMT_VARIABLE, ATRI_STMT_ASSIGN, ATRI_STMT_RETURN,
    ATRI_STMT_BLOCK, ATRI_STMT_IF, ATRI_STMT_WHILE, ATRI_STMT_DO,
    ATRI_STMT_FOR, ATRI_STMT_LOOP, ATRI_STMT_BREAK, ATRI_STMT_CONTINUE, ATRI_STMT_EXPR,
    ATRI_STMT_SWITCH
} AtriStmtKind;
typedef struct {
    AtriStmtKind kind;
    bool compound;
    AtriOp op;
    AtriSpan span, name, type;
    AtriId expression;
    AtriId target;
    AtriId body, otherwise, initializer, step;
    size_t first_case, case_count;
} AtriStatement;
typedef struct {
    AtriSpan span;
    size_t first_statement, statement_count;
} AtriBlock;
typedef struct { AtriSpan span, element; AtriId length; } AtriArrayType;
typedef struct { AtriSpan name, type; } AtriParameter;

typedef struct {
    AtriSpan span;
    AtriSpan name;
    AtriSpan return_type;
    AtriSpan body;
    size_t first_parameter, parameter_count;
    size_t first_statement, statement_count;
    AtriId block;
    bool external_c;
    bool variadic;
    bool exported;
} AtriFunction;

typedef struct { AtriSpan name, type; AtriId expression; } AtriField;
/* struct/union 共用名义类型表；union 不增加有效成员标签或对象状态。 */
typedef struct { AtriSpan name, span; size_t first_field, field_count; bool exported, opaque, is_union; } AtriStruct;
typedef struct { AtriSpan name; AtriId expression; } AtriInitializer;
typedef struct { AtriSpan name, span, underlying; size_t first_variant, variant_count; bool exported; } AtriEnum;
typedef struct { AtriSpan name; AtriId expression; } AtriEnumVariant;
typedef struct { AtriSpan span; AtriId expression, body; bool through; } AtriCase;
typedef struct { AtriSpan name, path; AtriId target; } AtriImport;
typedef struct { AtriStatement declaration; bool exported; } AtriGlobal;
typedef struct { char *path; AtriSpan span; } AtriModule;
typedef struct {
    const char *text;
    size_t length;
    AtriId owner, target;
    unsigned kind; /* 0 函数，1 全局，2 模块绑定，3 struct/union，4 enum。 */
    bool exported;
    AtriSpan span;
} AtriModuleSymbol;

/* 节点以稳定 ID 互相引用，扩容不使引用失效；块的语句范围指向 block_statements 边数组。 */
typedef struct {
    AtriEnum *enums;
    size_t enum_count;
    AtriEnumVariant *variants;
    size_t variant_count;
    AtriCase *cases;
    size_t case_count;
    AtriId *runs;
    size_t run_count;
    AtriArrayType *array_types;
    size_t array_type_count;
    AtriStruct *structs;
    size_t struct_count;
    AtriField *fields;
    size_t field_count;
    AtriInitializer *initializers;
    size_t initializer_count;
    AtriModuleSymbol *symbols;
    size_t symbol_count;
    AtriImport *imports;
    size_t import_count;
    AtriGlobal *globals;
    size_t global_count;
    AtriModule *modules;
    size_t module_count;
    AtriFunction *functions;
    size_t function_count;
    AtriParameter *parameters;
    size_t parameter_count;
    AtriStatement *statements;
    size_t statement_count;
    AtriExpr *expressions;
    size_t expression_count;
    AtriId *arguments;
    size_t argument_count;
    AtriBlock *blocks;
    size_t block_count;
    AtriId *block_statements;
    size_t block_statement_count;
} AtriProgram;

/*
 * source 的寿命必须覆盖 AST；output 传入时不能持有未释放的分配。
 * 失败时 output 清空，diagnostic 保留首个错误；成功后由 atri_program_destroy 释放。
 * 此接口仅解析语法，不检查名称与类型。
 */
bool atri_parse(AtriSource source, AtriProgram *output,
                AtriDiagnostic *diagnostic);
bool atri_parse_sources(AtriSource source, const AtriModule *modules, size_t count, AtriProgram *program, AtriDiagnostic *diagnostic);
bool atri_load_program(AtriSource input, AtriSource *source, AtriProgram *program, AtriDiagnostic *diagnostic);
bool atri_validate_modules(AtriSource source, const AtriProgram *program, AtriDiagnostic *diagnostic);
AtriId atri_module_at(const AtriProgram *program, size_t offset);
AtriId atri_find_declaration(AtriSource source, const AtriProgram *program, size_t offset,
                             AtriSpan name, AtriSpan qualifier, bool *global);
AtriId atri_find_struct(AtriSource source, const AtriProgram *program, size_t offset, AtriSpan name, AtriSpan qualifier);
AtriId atri_find_enum(AtriSource source, const AtriProgram *program, size_t offset, AtriSpan name, AtriSpan qualifier);
void atri_program_diagnostic(FILE *stream, AtriSource source, const AtriProgram *program, AtriDiagnostic diagnostic);
void atri_program_destroy(AtriProgram *program);
void atri_dump_ast(FILE *stream, AtriSource source,
                   const AtriProgram *program);

AtriLocation atri_source_location(AtriSource source, size_t offset);
void atri_print_diagnostic(FILE *stream, AtriSource source,
                           AtriDiagnostic diagnostic);

#endif
