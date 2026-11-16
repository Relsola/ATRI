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
    size_t blocks_cap, block_statements_cap, statement_depth;
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
    if (e.kind == ATRI_EXPR_UNARY || e.kind == ATRI_EXPR_BINARY ||
        e.kind == ATRI_EXPR_ADDRESS || e.kind == ATRI_EXPR_DEREFERENCE)
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
    if (token.kind == ATRI_TOKEN_TRUE || token.kind == ATRI_TOKEN_FALSE) {
        advance(p);
        return !p->failed && append_expr(p, (AtriExpr){.kind = ATRI_EXPR_BOOL,
            .span = token.span, .text = token.span, .boolean = token.kind == ATRI_TOKEN_TRUE}, result);
    }
    if (token.kind == ATRI_TOKEN_NULL) {
        advance(p);
        return !p->failed && append_expr(p, (AtriExpr){.kind = ATRI_EXPR_NULL,
            .span = token.span, .text = token.span}, result);
    }
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
    if (token.kind == ATRI_TOKEN_MINUS || token.kind == ATRI_TOKEN_PLUS ||
        token.kind == ATRI_TOKEN_STAR || token.kind == ATRI_TOKEN_AMPERSAND || token.kind == ATRI_TOKEN_NOT) {
        advance(p);
        AtriId operand;
        if (!parse_expression(p, 30, &operand)) return false;
        AtriExprKind kind = token.kind == ATRI_TOKEN_STAR ? ATRI_EXPR_DEREFERENCE :
                            token.kind == ATRI_TOKEN_AMPERSAND ? ATRI_EXPR_ADDRESS : ATRI_EXPR_UNARY;
        return append_expr(p, (AtriExpr){.kind = kind,
            .span = {token.span.start, p->program.expressions[operand].span.end},
            .op = token.kind == ATRI_TOKEN_NOT ? ATRI_NOT : token.kind == ATRI_TOKEN_PLUS ? ATRI_ADD : ATRI_NEG,
            .left = operand}, result);
    }
    return fail(p, "expected an integer expression, name, call or '('");
}
static int precedence(AtriTokenKind kind, AtriOp *op) {
    switch (kind) {
        case ATRI_TOKEN_OR: *op = ATRI_OR; return 1;
        case ATRI_TOKEN_AND: *op = ATRI_AND; return 2;
        case ATRI_TOKEN_EQ: *op = ATRI_EQ; return 3;
        case ATRI_TOKEN_NE: *op = ATRI_NE; return 3;
        case ATRI_TOKEN_LT: *op = ATRI_LT; return 4;
        case ATRI_TOKEN_LE: *op = ATRI_LE; return 4;
        case ATRI_TOKEN_GT: *op = ATRI_GT; return 4;
        case ATRI_TOKEN_GE: *op = ATRI_GE; return 4;
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
static bool parse_type(Parser *p, AtriSpan *span, const char *message) {
    span->start = p->current.span.start;
    while (p->current.kind == ATRI_TOKEN_STAR) advance(p);
    span->end = p->current.span.end;
    return expect(p, ATRI_TOKEN_IDENTIFIER, message);
}
static bool declaration_start(Parser *p) {
    if (p->current.kind != ATRI_TOKEN_IDENTIFIER) return false;
    AtriLexer lookahead = p->lexer;
    AtriDiagnostic ignored;
    return atri_lexer_next(&lookahead, &ignored).kind == ATRI_TOKEN_COLON;
}
static bool parse_statement(Parser *p, AtriId *id);
static bool append_statement(Parser *p, AtriStatement s, AtriId *id) {
    *id = p->program.statement_count;
    return atri_append((void **)&p->program.statements, &p->program.statement_count,
                       &p->statements_cap, sizeof(s), &s) || fail(p, "out of memory while building statements");
}
static bool parse_block(Parser *p, AtriId *id) {
    AtriBlock b = {.span.start = p->current.span.start};
    if (!expect(p, ATRI_TOKEN_LEFT_BRACE, "expected '{'; all control-flow bodies require braces")) return false;
    AtriId *items = NULL;
    size_t count = 0, capacity = 0;
    bool ok = false;
    while (!p->failed && p->current.kind != ATRI_TOKEN_RIGHT_BRACE) {
        if (p->current.kind == ATRI_TOKEN_EOF) { fail(p, "expected '}' to close the block"); goto done; }
        AtriId statement;
        if (!parse_statement(p, &statement)) goto done;
        if (!atri_append((void **)&items, &count, &capacity, sizeof(statement), &statement)) {
            fail(p, "out of memory"); goto done;
        }
    }
    b.span.end = p->current.span.end;
    if (!expect(p, ATRI_TOKEN_RIGHT_BRACE, "expected '}' to close the block")) goto done;
    b.first_statement = p->program.block_statement_count; b.statement_count = count;
    for (size_t i = 0; i < count; ++i)
        if (!atri_append((void **)&p->program.block_statements, &p->program.block_statement_count,
                          &p->block_statements_cap, sizeof(items[i]), &items[i])) { fail(p, "out of memory"); goto done; }
    *id = p->program.block_count;
    ok = atri_append((void **)&p->program.blocks, &p->program.block_count,
                     &p->blocks_cap, sizeof(b), &b) || fail(p, "out of memory");
done:
    free(items); return ok;
}
static AtriStatement new_statement(Parser *p) {
    return (AtriStatement){.span.start = p->current.span.start, .expression = ATRI_NONE,
        .target = ATRI_NONE, .body = ATRI_NONE, .otherwise = ATRI_NONE, .initializer = ATRI_NONE, .step = ATRI_NONE};
}
static bool simple_statement(Parser *p, AtriStatement *st, bool declarations) {
    AtriStatement s = *st;
    if (declaration_start(p)) {
        if (!declarations) return fail(p, "a for step cannot declare a variable");
        s.name = p->current.span;
        advance(p);
        if (p->current.kind == ATRI_TOKEN_COLON) {
            advance(p);
            if (!parse_type(p, &s.type, "expected a type name after ':'; type inference is not supported yet")) return false;
            s.kind = ATRI_STMT_VARIABLE;
            if (p->current.kind == ATRI_TOKEN_CONSTANT_INIT || p->current.kind == ATRI_TOKEN_EQUAL) {
                if (p->current.kind == ATRI_TOKEN_CONSTANT_INIT) s.kind = ATRI_STMT_CONSTANT;
                advance(p);
                if (!parse_expression(p, 0, &s.expression)) return false;
            } else if (p->current.kind == ATRI_TOKEN_COLON)
                return fail(p, "expected ':=' for a compile-time constant initializer");
        }
    } else {
        AtriId lhs;
        if (!parse_expression(p, 0, &lhs)) return false;
        if (p->current.kind == ATRI_TOKEN_EQUAL) {
            s.kind = ATRI_STMT_ASSIGN; s.target = lhs; advance(p);
            if (!parse_expression(p, 0, &s.expression)) return false;
        } else {
            if (p->program.expressions[lhs].kind != ATRI_EXPR_CALL)
                return fail(p, "expected ':' for a declaration or '=' for an assignment");
            s.kind = ATRI_STMT_EXPR; s.expression = lhs;
        }
    }
    *st = s; return true;
}
static bool condition(Parser *p, AtriId *id) {
    return expect(p, ATRI_TOKEN_LEFT_PAREN, "expected '(' before condition") &&
        parse_expression(p, 0, id) && expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after condition");
}
static bool parse_statement(Parser *p, AtriId *id) {
    if (++p->statement_depth > 256) { --p->statement_depth; return fail(p, "statement nesting exceeds the prototype limit of 256"); }
    AtriStatement s = new_statement(p);
    AtriTokenKind token = p->current.kind;
    bool ok = false, semicolon = false;
    switch (token) {
        case ATRI_TOKEN_LEFT_BRACE:
            s.kind = ATRI_STMT_BLOCK;
            if (!parse_block(p, &s.body)) goto done;
            break;
        case ATRI_TOKEN_IF:
            s.kind = ATRI_STMT_IF; advance(p);
            if (!condition(p, &s.expression) || !parse_block(p, &s.body)) goto done;
            if (p->current.kind == ATRI_TOKEN_ELSE) {
                advance(p);
                if (p->current.kind != ATRI_TOKEN_IF && p->current.kind != ATRI_TOKEN_LEFT_BRACE) {
                    fail(p, "expected '{' or 'if' after else"); goto done;
                }
                if (!parse_statement(p, &s.otherwise)) goto done;
            }
            break;
        case ATRI_TOKEN_WHILE:
            s.kind = ATRI_STMT_WHILE; advance(p);
            if (!condition(p, &s.expression) || !parse_block(p, &s.body)) goto done;
            break;
        case ATRI_TOKEN_DO:
            s.kind = ATRI_STMT_DO; advance(p);
            if (!parse_block(p, &s.body) || !expect(p, ATRI_TOKEN_WHILE, "expected 'while' after do body") ||
                !condition(p, &s.expression)) goto done;
            semicolon = true; break;
        case ATRI_TOKEN_LOOP:
            s.kind = ATRI_STMT_LOOP; advance(p);
            if (!parse_block(p, &s.body)) goto done;
            break;
        case ATRI_TOKEN_FOR:
            s.kind = ATRI_STMT_FOR; advance(p);
            if (!expect(p, ATRI_TOKEN_LEFT_PAREN, "expected '(' after for")) goto done;
            if (p->current.kind != ATRI_TOKEN_SEMICOLON) {
                AtriStatement init = new_statement(p);
                if (!simple_statement(p, &init, true)) goto done;
                init.span.end = p->current.span.start;
                if (!append_statement(p, init, &s.initializer)) goto done;
            }
            if (!expect(p, ATRI_TOKEN_SEMICOLON, "expected ';' after for initializer")) goto done;
            if (p->current.kind != ATRI_TOKEN_SEMICOLON && !parse_expression(p, 0, &s.expression)) goto done;
            if (!expect(p, ATRI_TOKEN_SEMICOLON, "expected ';' after for condition")) goto done;
            if (p->current.kind != ATRI_TOKEN_RIGHT_PAREN) {
                AtriStatement step = new_statement(p);
                if (!simple_statement(p, &step, false)) goto done;
                step.span.end = p->current.span.start;
                if (!append_statement(p, step, &s.step)) goto done;
            }
            if (!expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after for step") || !parse_block(p, &s.body)) goto done;
            break;
        case ATRI_TOKEN_RETURN:
            s.kind = ATRI_STMT_RETURN; advance(p);
            if (!parse_expression(p, 0, &s.expression)) goto done;
            semicolon = true; break;
        case ATRI_TOKEN_BREAK: case ATRI_TOKEN_CONTINUE:
            s.kind = token == ATRI_TOKEN_BREAK ? ATRI_STMT_BREAK : ATRI_STMT_CONTINUE;
            advance(p); semicolon = true; break;
        default:
            if (!simple_statement(p, &s, true)) goto done;
            semicolon = true; break;
    }
    s.span.end = p->current.span.start;
    if (semicolon) {
        s.span.end = p->current.span.end;
        if (!expect(p, ATRI_TOKEN_SEMICOLON, "expected ';' after statement")) goto done;
    }
    ok = append_statement(p, s, id);
done:
    --p->statement_depth; return ok;
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
            if (!parse_type(p, &param.type, "expected a parameter type")) return false;
            if (!atri_append((void **)&p->program.parameters, &p->program.parameter_count,
                              &p->parameters_cap, sizeof(param), &param)) return fail(p, "out of memory");
            if (p->current.kind != ATRI_TOKEN_COMMA) break;
            advance(p);
        }
    }
    f.parameter_count = p->program.parameter_count - f.first_parameter;
    if (!expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after parameters") ||
        !expect(p, ATRI_TOKEN_ARROW, "expected '->' before the return type")) return false;
    if (!parse_type(p, &f.return_type, "expected a return type name")) return false;
    if (!parse_block(p, &f.block)) return false;
    f.body = p->program.blocks[f.block].span; f.span.end = f.body.end;
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
    free(p->expressions); free(p->arguments); free(p->blocks); free(p->block_statements);
    *p = (AtriProgram){0};
}
static void write_span(FILE *s, AtriSource source, AtriSpan span) {
    fwrite(source.text + span.start, 1, span.end - span.start, s);
}
static void dump_expr(FILE *s, AtriSource source, const AtriProgram *p, AtriId id) {
    const AtriExpr *e = &p->expressions[id];
    if (e->kind == ATRI_EXPR_INTEGER || e->kind == ATRI_EXPR_NAME || e->kind == ATRI_EXPR_NULL || e->kind == ATRI_EXPR_BOOL) write_span(s, source, e->text);
    else if (e->kind == ATRI_EXPR_CALL) {
        write_span(s, source, e->text); fputc('(', s);
        for (size_t i = 0; i < e->argument_count; ++i) {
            if (i != 0) fputs(", ", s);
            dump_expr(s, source, p, p->arguments[e->first_argument + i]);
        }
        fputc(')', s);
    } else if (e->kind == ATRI_EXPR_UNARY || e->kind == ATRI_EXPR_ADDRESS || e->kind == ATRI_EXPR_DEREFERENCE) {
        fputc('(', s);
        fputc(e->kind == ATRI_EXPR_ADDRESS ? '&' : e->kind == ATRI_EXPR_DEREFERENCE ? '*' : e->op == ATRI_NOT ? '!' : e->op == ATRI_ADD ? '+' : '-', s);
        dump_expr(s, source, p, e->left); fputc(')', s);
    } else {
        const char *ops[] = {"+", "-", "*", "/", "%", "-", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "!"};
        fputc('(', s); dump_expr(s, source, p, e->left);
        fprintf(s, " %s ", ops[e->op]); dump_expr(s, source, p, e->right); fputc(')', s);
    }
}
static void dump_statement(FILE *s, AtriSource source, const AtriProgram *p, AtriId id, int indent);
static void dump_block(FILE *s, AtriSource source, const AtriProgram *p, AtriId id, int indent) {
    fprintf(s, "%*sBlock\n", indent, "");
    const AtriBlock *b = &p->blocks[id];
    for (size_t j = 0; j < b->statement_count; ++j)
        dump_statement(s, source, p, p->block_statements[b->first_statement + j], indent + 2);
}
static void dump_statement(FILE *s, AtriSource source, const AtriProgram *p, AtriId id, int indent) {
    const AtriStatement *st = &p->statements[id];
    if (st->kind == ATRI_STMT_BLOCK) { dump_block(s, source, p, st->body, indent); return; }
    static const char *names[] = {"Constant", "Variable", "Assign", "Return", "Block", "If", "While", "Do",
        "For", "Loop", "Break", "Continue", "Expression"};
    fprintf(s, "%*s%s", indent, "", names[st->kind]);
    if (st->kind == ATRI_STMT_CONSTANT || st->kind == ATRI_STMT_VARIABLE) {
        fputc(' ', s); write_span(s, source, st->name); fputs(": ", s); write_span(s, source, st->type);
        fputs(st->kind == ATRI_STMT_CONSTANT ? " := " : " = ", s);
        if (st->expression == ATRI_NONE) fputs("0 (default)", s);
    } else if (st->kind == ATRI_STMT_ASSIGN) {
        fputc(' ', s); dump_expr(s, source, p, st->target); fputs(" = ", s);
    } else if (st->expression != ATRI_NONE) fputc(' ', s);
    if (st->expression != ATRI_NONE) dump_expr(s, source, p, st->expression);
    fputc('\n', s);
    if (st->initializer != ATRI_NONE) dump_statement(s, source, p, st->initializer, indent + 2);
    if (st->body != ATRI_NONE) dump_block(s, source, p, st->body, indent + 2);
    if (st->step != ATRI_NONE) dump_statement(s, source, p, st->step, indent + 2);
    if (st->otherwise != ATRI_NONE) { fprintf(s, "%*sElse\n", indent, ""); dump_statement(s, source, p, st->otherwise, indent + 2); }
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
        dump_block(s, source, p, f->block, 4);
    }
}
