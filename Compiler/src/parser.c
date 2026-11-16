#include "frontend.h"
#include "lexer.h"
#include "array.h"

typedef struct {
    AtriLexer lexer;
    AtriToken current;
    AtriProgram program;
    AtriDiagnostic *diagnostic;
    bool failed;
    size_t functions_cap, parameters_cap, statements_cap, expressions_cap, arguments_cap;
    size_t depth;
} Parser;

static bool fail(Parser *p, const char *message) {
    if (!p->failed) *p->diagnostic = (AtriDiagnostic){p->current.span, message};
    p->failed = true;
    return false;
}
static void advance(Parser *p) {
    if (p->failed) return;
    p->current = atri_lexer_next(&p->lexer, p->diagnostic);
    if (p->current.kind == ATRI_TOKEN_ERROR) p->failed = true;
}
static bool expect(Parser *p, AtriTokenKind kind, const char *message) {
    if (p->failed) return false;
    if (p->current.kind != kind) return fail(p, message);
    advance(p);
    return !p->failed;
}
static bool append_expr(Parser *p, AtriExpr e, AtriId *id) {
    e.tree_depth = 1;
    if (e.kind == ATRI_EXPR_UNARY || e.kind == ATRI_EXPR_BINARY)
        e.tree_depth = p->program.expressions[e.left].tree_depth + 1;
    if (e.kind == ATRI_EXPR_BINARY && p->program.expressions[e.right].tree_depth + 1 > e.tree_depth)
        e.tree_depth = p->program.expressions[e.right].tree_depth + 1;
    if (e.kind == ATRI_EXPR_CALL)
        for (size_t a = 0; a < e.argument_count; ++a) {
            size_t depth = p->program.expressions[p->program.arguments[e.first_argument + a]].tree_depth + 1;
            if (depth > e.tree_depth) e.tree_depth = depth;
        }
    if (e.tree_depth > 256) return fail(p, "expression tree exceeds the prototype limit of 256");
    *id = p->program.expression_count;
    return atri_append((void **)&p->program.expressions, &p->program.expression_count,
                       &p->expressions_cap, sizeof(e), &e) ||
           fail(p, "out of memory while building expressions");
}
static bool parse_expression(Parser *p, int minimum, AtriId *result);

static bool parse_primary(Parser *p, AtriId *result) {
    AtriToken token = p->current;
    if (token.kind == ATRI_TOKEN_INTEGER) {
        advance(p);
        return !p->failed && append_expr(p, (AtriExpr){
            .kind = ATRI_EXPR_INTEGER, .span = token.span, .text = token.span}, result);
    }
    if (token.kind == ATRI_TOKEN_IDENTIFIER) {
        advance(p);
        if (p->failed) return false;
        AtriExpr e = {.kind = ATRI_EXPR_NAME, .span = token.span, .text = token.span};
        if (p->current.kind == ATRI_TOKEN_LEFT_PAREN) {
            advance(p);
            AtriId *arguments = NULL;
            size_t count = 0, capacity = 0;
            bool success = false;
            if (p->current.kind != ATRI_TOKEN_RIGHT_PAREN) {
                for (;;) {
                    AtriId argument;
                    if (!parse_expression(p, 0, &argument)) goto cleanup;
                    if (!atri_append((void **)&arguments, &count, &capacity,
                                      sizeof(argument), &argument)) {
                        fail(p, "out of memory while building call arguments");
                        goto cleanup;
                    }
                    if (p->current.kind != ATRI_TOKEN_COMMA) break;
                    advance(p);
                }
            }
            e.kind = ATRI_EXPR_CALL;
            e.span.end = p->current.span.end;
            e.first_argument = p->program.argument_count;
            e.argument_count = count;
            if (!expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after arguments")) goto cleanup;
            for (size_t i = 0; i < count; ++i) {
                if (!atri_append((void **)&p->program.arguments, &p->program.argument_count,
                                  &p->arguments_cap, sizeof(arguments[i]), &arguments[i])) {
                    fail(p, "out of memory while building call arguments");
                    goto cleanup;
                }
            }
            success = append_expr(p, e, result);
cleanup:
            free(arguments);
            return success;
        }
        return append_expr(p, e, result);
    }
    if (token.kind == ATRI_TOKEN_LEFT_PAREN) {
        advance(p);
        if (!parse_expression(p, 0, result)) return false;
        return expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after expression");
    }
    if (token.kind == ATRI_TOKEN_MINUS || token.kind == ATRI_TOKEN_PLUS) {
        advance(p);
        AtriId operand;
        if (!parse_expression(p, 30, &operand)) return false;
        if (token.kind == ATRI_TOKEN_PLUS) { *result = operand; return true; }
        return append_expr(p, (AtriExpr){.kind = ATRI_EXPR_UNARY,
            .span = {token.span.start, p->program.expressions[operand].span.end},
            .op = ATRI_NEG, .left = operand}, result);
    }
    return fail(p, "expected an integer expression, name, call or '('");
}
static int precedence(AtriTokenKind kind, AtriOp *op) {
    switch (kind) {
        case ATRI_TOKEN_PLUS: *op = ATRI_ADD; return 10;
        case ATRI_TOKEN_MINUS: *op = ATRI_SUB; return 10;
        case ATRI_TOKEN_STAR: *op = ATRI_MUL; return 20;
        case ATRI_TOKEN_SLASH: *op = ATRI_DIV; return 20;
        case ATRI_TOKEN_PERCENT: *op = ATRI_REM; return 20;
        default: return -1;
    }
}
static bool parse_expression(Parser *p, int minimum, AtriId *result) {
    if (++p->depth > 256) {
        --p->depth;
        return fail(p, "expression nesting exceeds the prototype limit of 256");
    }
    bool success = false;
    AtriId left;
    if (!parse_primary(p, &left)) goto done;
    for (;;) {
        AtriOp op = ATRI_ADD;
        int prec = precedence(p->current.kind, &op);
        if (prec < minimum) break;
        advance(p);
        AtriId right;
        if (!parse_expression(p, prec + 1, &right)) goto done;
        AtriSpan span = {p->program.expressions[left].span.start,
                         p->program.expressions[right].span.end};
        if (!append_expr(p, (AtriExpr){.kind = ATRI_EXPR_BINARY,
                .span = span, .op = op, .left = left, .right = right}, &left)) goto done;
    }
    *result = left;
    success = !p->failed;
done:
    --p->depth;
    return success;
}
static bool parse_statement(Parser *p) {
    AtriStatement s = {.span.start = p->current.span.start, .expression = ATRI_NONE};
    if (p->current.kind == ATRI_TOKEN_RETURN) {
        s.kind = ATRI_STMT_RETURN;
        advance(p);
        if (!parse_expression(p, 0, &s.expression)) return false;
    } else if (p->current.kind == ATRI_TOKEN_CONST || p->current.kind == ATRI_TOKEN_LET) {
        s.kind = p->current.kind == ATRI_TOKEN_CONST ? ATRI_STMT_CONST : ATRI_STMT_LET;
        advance(p);
        s.name = p->current.span;
        if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a binding name") ||
            !expect(p, ATRI_TOKEN_COLON, "expected ':' after the binding name")) return false;
        s.type = p->current.span;
        if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a type name")) return false;
        if (p->current.kind == ATRI_TOKEN_EQUAL) {
            advance(p);
            if (!parse_expression(p, 0, &s.expression)) return false;
        } else if (s.kind == ATRI_STMT_CONST) return fail(p, "const requires an initializer");
    } else if (p->current.kind == ATRI_TOKEN_IDENTIFIER) {
        s.kind = ATRI_STMT_ASSIGN;
        s.name = p->current.span;
        advance(p);
        if (!expect(p, ATRI_TOKEN_EQUAL, "expected '=' for an assignment") ||
            !parse_expression(p, 0, &s.expression)) return false;
    } else return fail(p, "expected 'return', 'const', 'let' or an assignment");
    s.span.end = p->current.span.end;
    if (!expect(p, ATRI_TOKEN_SEMICOLON, "expected ';' after statement")) return false;
    return atri_append((void **)&p->program.statements, &p->program.statement_count,
                       &p->statements_cap, sizeof(s), &s) ||
           fail(p, "out of memory while building statements");
}
static bool parse_function(Parser *p) {
    AtriFunction f = {.span.start = p->current.span.start,
        .first_parameter = p->program.parameter_count,
        .first_statement = p->program.statement_count};
    if (!expect(p, ATRI_TOKEN_FN, "expected 'fn' at the top level")) return false;
    f.name = p->current.span;
    if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a function name") ||
        !expect(p, ATRI_TOKEN_LEFT_PAREN, "expected '(' after the function name")) return false;
    if (p->current.kind != ATRI_TOKEN_RIGHT_PAREN) {
        for (;;) {
            AtriParameter param = {.name = p->current.span};
            if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a parameter name") ||
                !expect(p, ATRI_TOKEN_COLON, "expected ':' after the parameter name")) return false;
            param.type = p->current.span;
            if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a parameter type")) return false;
            if (!atri_append((void **)&p->program.parameters, &p->program.parameter_count,
                              &p->parameters_cap, sizeof(param), &param)) return fail(p, "out of memory");
            if (p->current.kind != ATRI_TOKEN_COMMA) break;
            advance(p);
        }
    }
    f.parameter_count = p->program.parameter_count - f.first_parameter;
    if (!expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after parameters") ||
        !expect(p, ATRI_TOKEN_ARROW, "expected '->' before the return type")) return false;
    f.return_type = p->current.span;
    if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a return type name")) return false;
    f.body.start = p->current.span.start;
    if (!expect(p, ATRI_TOKEN_LEFT_BRACE, "expected '{' to start the function body")) return false;
    while (!p->failed && p->current.kind != ATRI_TOKEN_RIGHT_BRACE) {
        if (p->current.kind == ATRI_TOKEN_EOF) return fail(p, "expected '}' to close the function body");
        if (!parse_statement(p)) return false;
    }
    f.body.end = p->current.span.end;
    f.span.end = f.body.end;
    if (!expect(p, ATRI_TOKEN_RIGHT_BRACE, "expected '}' to close the function body")) return false;
    f.statement_count = p->program.statement_count - f.first_statement;
    return atri_append((void **)&p->program.functions, &p->program.function_count,
                       &p->functions_cap, sizeof(f), &f) || fail(p, "out of memory");
}
bool atri_parse(AtriSource source, AtriProgram *output, AtriDiagnostic *diagnostic) {
    *output = (AtriProgram){0};
    *diagnostic = (AtriDiagnostic){0};
    Parser p = {.lexer = atri_lexer_create(source), .diagnostic = diagnostic};
    advance(&p);
    while (!p.failed && p.current.kind != ATRI_TOKEN_EOF) {
        if (!parse_function(&p)) break;
    }
    if (p.failed) { atri_program_destroy(&p.program); return false; }
    *output = p.program;
    return true;
}
void atri_program_destroy(AtriProgram *p) {
    free(p->functions); free(p->parameters); free(p->statements);
    free(p->expressions); free(p->arguments);
    *p = (AtriProgram){0};
}
static void write_span(FILE *s, AtriSource source, AtriSpan span) {
    fwrite(source.text + span.start, 1, span.end - span.start, s);
}
static void dump_expr(FILE *s, AtriSource source, const AtriProgram *p, AtriId id) {
    const AtriExpr *e = &p->expressions[id];
    if (e->kind == ATRI_EXPR_INTEGER || e->kind == ATRI_EXPR_NAME) write_span(s, source, e->text);
    else if (e->kind == ATRI_EXPR_CALL) {
        write_span(s, source, e->text); fputc('(', s);
        for (size_t i = 0; i < e->argument_count; ++i) {
            if (i != 0) fputs(", ", s);
            dump_expr(s, source, p, p->arguments[e->first_argument + i]);
        }
        fputc(')', s);
    } else if (e->kind == ATRI_EXPR_UNARY) {
        fputs("(-", s); dump_expr(s, source, p, e->left); fputc(')', s);
    } else {
        const char ops[] = "+-*/%";
        fputc('(', s); dump_expr(s, source, p, e->left);
        fprintf(s, " %c ", ops[e->op]); dump_expr(s, source, p, e->right); fputc(')', s);
    }
}
void atri_dump_ast(FILE *s, AtriSource source, const AtriProgram *p) {
    fputs("Program\n", s);
    for (size_t i = 0; i < p->function_count; ++i) {
        const AtriFunction *f = &p->functions[i];
        fputs("  Function ", s); write_span(s, source, f->name);
        fputs(" -> ", s); write_span(s, source, f->return_type); fputc('\n', s);
        for (size_t j = 0; j < f->parameter_count; ++j) {
            const AtriParameter *a = &p->parameters[f->first_parameter + j];
            fputs("    Parameter ", s); write_span(s, source, a->name);
            fputs(": ", s); write_span(s, source, a->type); fputc('\n', s);
        }
        fputs("    Block\n", s);
        for (size_t j = 0; j < f->statement_count; ++j) {
            const AtriStatement *st = &p->statements[f->first_statement + j];
            fputs("      ", s);
            if (st->kind == ATRI_STMT_RETURN) fputs("Return ", s);
            else {
                fputs(st->kind == ATRI_STMT_CONST ? "Const " : st->kind == ATRI_STMT_LET ? "Let " : "Assign ", s);
                write_span(s, source, st->name);
                if (st->kind != ATRI_STMT_ASSIGN) {
                    fputs(": ", s); write_span(s, source, st->type);
                }
                fputs(" = ", s);
            }
            if (st->expression == ATRI_NONE) fputs("0 (default)", s);
            else dump_expr(s, source, p, st->expression);
            fputc('\n', s);
        }
    }
}
