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

/* Half-open byte offsets into the original, caller-owned source. */
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
typedef enum { ATRI_ADD, ATRI_SUB, ATRI_MUL, ATRI_DIV, ATRI_REM, ATRI_NEG } AtriOp;
typedef enum {
    ATRI_EXPR_INTEGER, ATRI_EXPR_NAME, ATRI_EXPR_UNARY, ATRI_EXPR_BINARY, ATRI_EXPR_CALL
} AtriExprKind;
typedef struct {
    AtriExprKind kind;
    AtriSpan span, text;
    AtriOp op;
    AtriId left, right;
    size_t first_argument, argument_count;
    size_t tree_depth;
} AtriExpr;
typedef enum {
    ATRI_STMT_CONST, ATRI_STMT_LET, ATRI_STMT_ASSIGN, ATRI_STMT_RETURN
} AtriStmtKind;
typedef struct {
    AtriStmtKind kind;
    AtriSpan span, name, type;
    AtriId expression;
} AtriStatement;
typedef struct { AtriSpan name, type; } AtriParameter;

typedef struct {
    AtriSpan span;
    AtriSpan name;
    AtriSpan return_type;
    AtriSpan body;
    size_t first_parameter, parameter_count;
    size_t first_statement, statement_count;
} AtriFunction;

/* Flat arrays and stable indices; each function owns parameter/statement ranges. */
typedef struct {
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
} AtriProgram;

/*
 * Source must outlive the AST. Output must not already own allocations.
 * On failure, output is empty and diagnostic contains the first error.
 * Successful output must be released with atri_program_destroy.
 * This is syntax parsing, without symbol or type checking.
 */
bool atri_parse(AtriSource source, AtriProgram *output,
                AtriDiagnostic *diagnostic);
void atri_program_destroy(AtriProgram *program);
void atri_dump_ast(FILE *stream, AtriSource source,
                   const AtriProgram *program);

AtriLocation atri_source_location(AtriSource source, size_t offset);
void atri_print_diagnostic(FILE *stream, AtriSource source,
                           AtriDiagnostic diagnostic);

#endif
