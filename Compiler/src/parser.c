#include "frontend.h"
#include "lexer.h"
#include "source_map.h"
#include "array.h"
#include "meta_plugin.h"

typedef struct {
    AtriLexer lexer;
    AtriToken current;
    size_t previous_end;
    AtriProgram program;
    AtriDiagnostic *diagnostic;
    bool failed;
    size_t functions_cap, parameters_cap, statements_cap, expressions_cap, arguments_cap;
    size_t blocks_cap, block_statements_cap, statement_depth;
    size_t depth;
    size_t array_types_cap, imports_cap, globals_cap, structs_cap, fields_cap, initializers_cap;
    size_t enums_cap, variants_cap, cases_cap, runs_cap;
} Parser;

static bool fail(Parser *p, const char *message) {
    if (!p->failed && p->current.kind != ATRI_TOKEN_ERROR) {
        *p->diagnostic = (AtriDiagnostic){.span = p->current.span, .message = message};
    }
    p->failed = true;
    return false;
}
static void advance(Parser *p) {
    if (p->failed) return;
    p->previous_end = p->current.span.end;
    p->current = atri_lexer_next(&p->lexer, p->diagnostic);
    /* 前瞻可能属于下一条声明，等真正消费该位置时才报告词法错误。 */
}
static bool expect(Parser *p, AtriTokenKind kind, const char *message) {
    if (p->failed) return false;
    if (p->current.kind != kind) return fail(p, message);
    advance(p);
    return !p->failed;
}
static bool append_expr(Parser *p, AtriExpr e, AtriId *id) {
    /* 左结合长链不会显著增加解析递归深度，但仍会生成深树；单独限制树高以保护后续遍历。 */
    if (e.custom && !p->program.unresolved_operator.end) { p->program.unresolved_operator = e.text; }
    e.tree_depth = 1;
    if (e.kind == ATRI_EXPR_UNARY || e.kind == ATRI_EXPR_BINARY ||
        e.kind == ATRI_EXPR_ADDRESS || e.kind == ATRI_EXPR_DEREFERENCE || e.kind == ATRI_EXPR_FIELD ||
        e.kind == ATRI_EXPR_INDEX || e.kind == ATRI_EXPR_CONDITIONAL || (e.kind == ATRI_EXPR_RUN && !e.run_block) || e.kind == ATRI_EXPR_CAST)
        e.tree_depth = p->program.expressions[e.left].tree_depth + 1;
    if ((e.kind == ATRI_EXPR_BINARY || e.kind == ATRI_EXPR_INDEX || e.kind == ATRI_EXPR_CONDITIONAL) && p->program.expressions[e.right].tree_depth + 1 > e.tree_depth)
        e.tree_depth = p->program.expressions[e.right].tree_depth + 1;
    if (e.kind == ATRI_EXPR_CONDITIONAL && p->program.expressions[e.alternative].tree_depth + 1 > e.tree_depth)
        e.tree_depth = p->program.expressions[e.alternative].tree_depth + 1;
    if (e.kind == ATRI_EXPR_INITIALIZER) {
        for (size_t i = 0; i < e.argument_count; ++i) {
            size_t depth = p->program.expressions[p->program.initializers[e.first_argument+i].expression].tree_depth+1;
            if (depth > e.tree_depth) { e.tree_depth = depth; }
        }
    }
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
static bool parse_type(Parser *p, AtriSpan *span, const char *message);

static bool parse_block(Parser *p, AtriId *id);

static bool parse_primary(Parser *p, AtriId *result) {
    AtriToken token = p->current;
    if (token.kind == ATRI_TOKEN_CAST) {
        advance(p);
        AtriSpan type;
        AtriId operand;
        if (!expect(p, ATRI_TOKEN_LEFT_PAREN, "expected '(' after cast") ||
            !parse_type(p, &type, "expected a conversion target type") ||
            !expect(p, ATRI_TOKEN_COMMA, "expected ',' after conversion target type") ||
            !parse_expression(p, 0, &operand)) { return false; }
        AtriSpan span = {token.span.start, p->current.span.end};
        if (!expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after conversion operand")) { return false; }
        return append_expr(p, (AtriExpr){.kind = ATRI_EXPR_CAST, .span = span, .text = type, .left = operand}, result);
    }
    if (token.kind == ATRI_TOKEN_SIZEOF || token.kind == ATRI_TOKEN_ALIGNOF) {
        advance(p);
        AtriSpan type;
        if (!expect(p, ATRI_TOKEN_LEFT_PAREN, "expected '(' before layout query type") ||
            !parse_type(p, &type, "layout queries require a complete object type")) { return false; }
        AtriSpan span = {token.span.start, p->current.span.end};
        if (!expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after layout query type")) { return false; }
        return append_expr(p, (AtriExpr){.kind = token.kind == ATRI_TOKEN_SIZEOF ? ATRI_EXPR_SIZEOF : ATRI_EXPR_ALIGNOF,
            .span = span, .text = type}, result);
    }
    if (token.kind == ATRI_TOKEN_RUN) {
        advance(p);
        if (p->current.kind == ATRI_TOKEN_LEFT_BRACE || p->current.kind == ATRI_TOKEN_ARROW) {
            AtriExpr run = {.kind = ATRI_EXPR_RUN, .run_block = true, .span.start = token.span.start};
            if (p->current.kind == ATRI_TOKEN_ARROW) {
                advance(p);
                if (!parse_type(p, &run.text, "expected a result type after #run ->")) { return false; }
            }
            if (!parse_block(p, &run.left)) { return false; }
            run.span.end = p->program.blocks[run.left].span.end;
            run.alternative = p->program.run_block_count++;
            return append_expr(p, run, result);
        }
        if (p->current.kind != ATRI_TOKEN_IDENTIFIER) { return fail(p, "#run requires a direct function call"); }
        AtriId operand;
        if (!parse_primary(p, &operand)) { return false; }
        if (p->program.expressions[operand].kind != ATRI_EXPR_CALL) { return fail(p, "#run requires a direct function call"); }
        return append_expr(p, (AtriExpr){.kind = ATRI_EXPR_RUN, .left = operand,
            .span = {token.span.start, p->program.expressions[operand].span.end}}, result);
    }
    if (token.kind == ATRI_TOKEN_LEFT_BRACE) {
        AtriExpr e = {.kind = ATRI_EXPR_INITIALIZER, .span.start = token.span.start};
        AtriInitializer *items = NULL;
        size_t count = 0, capacity = 0;
        bool ok = false;
        advance(p);
        while (p->current.kind != ATRI_TOKEN_RIGHT_BRACE) {
            AtriInitializer item = {0};
            if (p->current.kind == ATRI_TOKEN_DOT) {
                advance(p); item.name = p->current.span;
                if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected field name") ||
                    !expect(p, ATRI_TOKEN_EQUAL, "expected '=' after field name")) { goto initializer_done; }
            }
            if (!parse_expression(p, 0, &item.expression)) { goto initializer_done; }
            if (!atri_append((void **)&items, &count, &capacity, sizeof(item), &item)) { fail(p, "out of memory"); goto initializer_done; }
            if (p->current.kind != ATRI_TOKEN_COMMA) { break; }
            advance(p);
        }
        e.span.end = p->current.span.end;
        if (!expect(p, ATRI_TOKEN_RIGHT_BRACE, "expected '}' after struct initializer")) { goto initializer_done; }
        e.first_argument = p->program.initializer_count; e.argument_count = count;
        for (size_t i = 0; i < count; ++i) {
            if (!atri_append((void **)&p->program.initializers, &p->program.initializer_count, &p->initializers_cap, sizeof(items[i]), &items[i])) {
                fail(p, "out of memory"); goto initializer_done;
            }
        }
        ok = append_expr(p, e, result);
initializer_done:
        free(items); return ok;
    }
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
    if (token.kind == ATRI_TOKEN_CHARACTER) {
        advance(p);
        return append_expr(p, (AtriExpr){.kind = ATRI_EXPR_CHARACTER, .span = token.span, .text = token.span}, result);
    }
    if (token.kind == ATRI_TOKEN_INTEGER) {
        advance(p);
        return !p->failed && append_expr(p, (AtriExpr){
            .kind = ATRI_EXPR_INTEGER, .span = token.span, .text = token.span}, result);
    }
    if (token.kind == ATRI_TOKEN_STRING) {
        advance(p);
        return !p->failed && append_expr(p, (AtriExpr){
            .kind = ATRI_EXPR_STRING, .span = token.span, .text = token.span}, result);
    }
    if (token.kind == ATRI_TOKEN_IDENTIFIER || token.kind == ATRI_TOKEN_KEYWORD) {
        advance(p);
        if (p->failed) return false;
        AtriExpr e = {.kind = ATRI_EXPR_NAME, .span = token.span, .text = token.span,
            .keyword = token.kind == ATRI_TOKEN_KEYWORD};
        if (e.keyword && p->current.kind != ATRI_TOKEN_LEFT_PAREN) { return fail(p, "#keyword requires a node handle in parentheses"); }
        if (p->current.kind == ATRI_TOKEN_SCOPE) {
            e.qualifier = token.span;
            advance(p);
            e.text = p->current.span;
            e.span.end = e.text.end;
            if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a module member after '::'")) return false;
            if (p->current.kind == ATRI_TOKEN_SCOPE) {
                e.module = e.qualifier; e.qualifier = e.text;
                advance(p); e.text = p->current.span; e.span.end = e.text.end;
                if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected an enum member after '::'")) { return false; }
            }
        }
        if (p->current.kind == ATRI_TOKEN_LEFT_PAREN) {
            advance(p);
            /* 嵌套调用会先写入自己的实参边；当前调用先暂存 ID，最后一次性追加以保持范围连续。 */
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
        AtriSpan grouped = {token.span.start, p->current.span.end};
        if (!expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after expression")) { return false; }
        p->program.expressions[*result].span = grouped;
        return true;
    }
    if (token.kind == ATRI_TOKEN_MINUS || token.kind == ATRI_TOKEN_PLUS ||
        token.kind == ATRI_TOKEN_STAR || token.kind == ATRI_TOKEN_AMPERSAND || token.kind == ATRI_TOKEN_NOT || token.kind == ATRI_TOKEN_TILDE) {
        advance(p);
        AtriId operand;
        if (!parse_expression(p, 30, &operand)) return false;
        AtriExprKind kind = token.kind == ATRI_TOKEN_STAR ? ATRI_EXPR_DEREFERENCE :
                            token.kind == ATRI_TOKEN_AMPERSAND ? ATRI_EXPR_ADDRESS : ATRI_EXPR_UNARY;
        return append_expr(p, (AtriExpr){.kind = kind, .text = token.span,
            .span = {token.span.start, p->program.expressions[operand].span.end},
            .op = token.kind == ATRI_TOKEN_NOT ? ATRI_NOT : token.kind == ATRI_TOKEN_TILDE ? ATRI_BIT_NOT : token.kind == ATRI_TOKEN_PLUS ? ATRI_ADD : ATRI_NEG,
            .left = operand}, result);
    }
    return fail(p, "expected an expression");
}
static int precedence(AtriTokenKind kind, AtriOp *op) {
    switch (kind) {
        case ATRI_TOKEN_OR: *op = ATRI_OR; return 1;
        case ATRI_TOKEN_AND: *op = ATRI_AND; return 2;
        case ATRI_TOKEN_PIPE: *op = ATRI_BIT_OR; return 3;
        case ATRI_TOKEN_CARET: *op = ATRI_BIT_XOR; return 4;
        case ATRI_TOKEN_AMPERSAND: *op = ATRI_BIT_AND; return 5;
        case ATRI_TOKEN_SHL: *op = ATRI_SHL; return 8;
        case ATRI_TOKEN_SHR: *op = ATRI_SHR; return 8;
        case ATRI_TOKEN_EQ: *op = ATRI_EQ; return 6;
        case ATRI_TOKEN_NE: *op = ATRI_NE; return 6;
        case ATRI_TOKEN_LT: *op = ATRI_LT; return 7;
        case ATRI_TOKEN_LE: *op = ATRI_LE; return 7;
        case ATRI_TOKEN_GT: *op = ATRI_GT; return 7;
        case ATRI_TOKEN_GE: *op = ATRI_GE; return 7;
        case ATRI_TOKEN_PLUS: *op = ATRI_ADD; return 10;
        case ATRI_TOKEN_MINUS: *op = ATRI_SUB; return 10;
        case ATRI_TOKEN_STAR: *op = ATRI_MUL; return 20;
        case ATRI_TOKEN_SLASH: *op = ATRI_DIV; return 20;
        case ATRI_TOKEN_PERCENT: *op = ATRI_REM; return 20;
        default: return -1;
    }
}
/* 优先级爬升：右侧只吸收更高优先级，当前层循环接续同级运算，因此二元运算左结合。 */
static bool parse_expression(Parser *p, int minimum, AtriId *result) {
    if (++p->depth > 256) {
        --p->depth;
        return fail(p, "expression nesting exceeds the prototype limit of 256");
    }
    bool success = false;
    AtriId left;
    if (!parse_primary(p, &left)) goto done;
    for (;;) {
        if (p->current.kind == ATRI_TOKEN_LEFT_BRACKET) {
            /* 逗号索引沿用嵌套索引 AST，每个维度分别定型和检查。 */
            do {
                advance(p);
                AtriId index;
                if (!parse_expression(p, 0, &index)) { goto done; }
                size_t end = p->current.kind == ATRI_TOKEN_RIGHT_BRACKET ? p->current.span.end :
                    p->program.expressions[index].span.end;
                AtriExpr e = {.kind = ATRI_EXPR_INDEX, .left = left, .right = index,
                    .span = {p->program.expressions[left].span.start, end}};
                if (!append_expr(p, e, &left)) { goto done; }
            } while (p->current.kind == ATRI_TOKEN_COMMA);
            if (!expect(p, ATRI_TOKEN_RIGHT_BRACKET, "expected ']' after index")) { goto done; }
            continue;
        }
        if (p->current.kind == ATRI_TOKEN_DOT) {
            advance(p);
            AtriExpr field = {.kind = ATRI_EXPR_FIELD, .left = left, .text = p->current.span,
                .span = {p->program.expressions[left].span.start, p->current.span.end}};
            if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a field name after '.'") || !append_expr(p, field, &left)) { goto done; }
            continue;
        }
        AtriOp op = ATRI_ADD;
        AtriToken symbol = p->current;
        int prec = symbol.custom ? (int)symbol.custom->precedence : precedence(symbol.kind, &op);
        if (prec < minimum) break;
        advance(p);
        AtriId right;
        if (!parse_expression(p, prec + !(symbol.custom && symbol.custom->right), &right)) goto done;
        AtriSpan span = {p->program.expressions[left].span.start,
                         p->program.expressions[right].span.end};
        if (!append_expr(p, (AtriExpr){.kind = ATRI_EXPR_BINARY,
                .span = span, .text = symbol.span, .custom = symbol.custom != NULL, .op = op, .left = left, .right = right}, &left)) goto done;
    }
    if (minimum == 0 && p->current.kind == ATRI_TOKEN_QUESTION) {
        advance(p);
        AtriId yes, no;
        if (!parse_expression(p, 0, &yes) || !expect(p, ATRI_TOKEN_COLON, "expected ':' in conditional expression") ||
            !parse_expression(p, 0, &no)) { goto done; }
        if (!append_expr(p, (AtriExpr){.kind = ATRI_EXPR_CONDITIONAL, .left = left, .right = yes, .alternative = no,
                .span = {p->program.expressions[left].span.start, p->program.expressions[no].span.end}}, &left)) { goto done; }
    }
    *result = left;
    success = !p->failed;
done:
    --p->depth;
    return success;
}
static bool parse_type(Parser *p, AtriSpan *span, const char *message) {
    span->start = p->current.span.start;
    if (++p->depth > 256) { --p->depth; return fail(p, "type nesting exceeds the prototype limit of 256"); }
    if (p->current.kind == ATRI_TOKEN_STAR || p->current.kind == ATRI_TOKEN_LEFT_BRACKET) {
        AtriTokenKind prefix = p->current.kind;
        advance(p);
        AtriId length;
        AtriSpan element;
        bool ok = (prefix != ATRI_TOKEN_LEFT_BRACKET ||
            (parse_expression(p, 0, &length) && expect(p, ATRI_TOKEN_RIGHT_BRACKET, "expected ']' after array length"))) &&
            parse_type(p, &element, message);
        if (ok) {
            span->end = element.end;
            if (prefix == ATRI_TOKEN_LEFT_BRACKET) {
                AtriArrayType array = {.span = *span, .element = element, .length = length};
                ok = atri_append((void **)&p->program.array_types, &p->program.array_type_count,
                    &p->array_types_cap, sizeof(array), &array) || fail(p, "out of memory while parsing array type");
            }
        }
        --p->depth; return ok;
    }
    if (p->current.kind == ATRI_TOKEN_IDENTIFIER && p->current.span.end - p->current.span.start == 4 &&
        !memcmp(p->lexer.source.text + p->current.span.start, "list", 4)) {
        AtriLexer look = p->lexer;
        AtriDiagnostic ignored;
        if (atri_lexer_next(&look, &ignored).kind == ATRI_TOKEN_LEFT_PAREN) {
            advance(p);
            advance(p);
            AtriArrayType list = {.length = ATRI_NONE};
            bool ok = parse_type(p, &list.element, "expected list element type");
            span->end = p->current.span.end;
            list.span = *span;
            --p->depth;
            return ok && expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after list element type") &&
                (atri_append((void **)&p->program.array_types, &p->program.array_type_count,
                    &p->array_types_cap, sizeof(list), &list) || fail(p, "out of memory parsing list type"));
        }
    }
    --p->depth;
    span->end = p->current.span.end;
    if (!expect(p, ATRI_TOKEN_IDENTIFIER, message)) { return false; }
    if (p->current.kind == ATRI_TOKEN_SCOPE) {
        advance(p); span->end = p->current.span.end;
        return expect(p, ATRI_TOKEN_IDENTIFIER, "expected a qualified type name");
    }
    return true;
}
static bool declaration_start(Parser *p) {
    if (p->current.kind != ATRI_TOKEN_IDENTIFIER) return false;
    AtriLexer lookahead = p->lexer;
    AtriDiagnostic ignored;
    return atri_lexer_next(&lookahead, &ignored).kind == ATRI_TOKEN_COLON;
}
static bool named_import_start(Parser *p) {
    if (p->current.kind != ATRI_TOKEN_IDENTIFIER) { return false; }
    AtriLexer lookahead = p->lexer;
    AtriDiagnostic ignored;
    return atri_lexer_next(&lookahead, &ignored).kind == ATRI_TOKEN_SCOPE;
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
    /* 子块在递归中先完成；暂存本块的直接语句边，避免把子块语句混入同一连续范围。 */
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
static bool compound_operator(AtriTokenKind token, AtriOp *op) {
    switch (token) {
        case ATRI_TOKEN_ADD_ASSIGN: *op = ATRI_ADD; return true;
        case ATRI_TOKEN_SUB_ASSIGN: *op = ATRI_SUB; return true;
        case ATRI_TOKEN_MUL_ASSIGN: *op = ATRI_MUL; return true;
        case ATRI_TOKEN_DIV_ASSIGN: *op = ATRI_DIV; return true;
        case ATRI_TOKEN_REM_ASSIGN: *op = ATRI_REM; return true;
        case ATRI_TOKEN_AND_ASSIGN: *op = ATRI_BIT_AND; return true;
        case ATRI_TOKEN_OR_ASSIGN: *op = ATRI_BIT_OR; return true;
        case ATRI_TOKEN_XOR_ASSIGN: *op = ATRI_BIT_XOR; return true;
        case ATRI_TOKEN_SHL_ASSIGN: *op = ATRI_SHL; return true;
        case ATRI_TOKEN_SHR_ASSIGN: *op = ATRI_SHR; return true;
        default: return false;
    }
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
        s.compound = compound_operator(p->current.kind, &s.op);
        if (p->current.kind == ATRI_TOKEN_EQUAL || s.compound) {
            s.kind = ATRI_STMT_ASSIGN; s.target = lhs; advance(p);
            if (!parse_expression(p, 0, &s.expression)) return false;
        } else {
            if (p->program.expressions[lhs].kind != ATRI_EXPR_CALL && p->program.expressions[lhs].kind != ATRI_EXPR_RUN)
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
static bool parse_switch(Parser *p, AtriStatement *s) {
    s->kind = ATRI_STMT_SWITCH;
    advance(p);
    if (!condition(p, &s->expression) || !expect(p, ATRI_TOKEN_LEFT_BRACE, "expected '{' after switch expression")) { return false; }
    AtriCase *cases = NULL;
    size_t count = 0, capacity = 0;
    bool ok = false, has_default = false;
    while (!p->failed && p->current.kind != ATRI_TOKEN_RIGHT_BRACE) {
        AtriCase item = {.span.start = p->current.span.start, .expression = ATRI_NONE};
        if (p->current.kind == ATRI_TOKEN_CASE) {
            advance(p);
            if (!parse_expression(p, 0, &item.expression)) { goto done; }
        } else if (p->current.kind == ATRI_TOKEN_DEFAULT) {
            if (has_default) { fail(p, "duplicate switch default"); goto done; }
            has_default = true; advance(p);
        } else { fail(p, "expected case, default or '}' in switch"); goto done; }
        if (!expect(p, ATRI_TOKEN_COLON, "expected ':' after case/default")) { goto done; }
        AtriBlock body = {.span.start = p->current.span.start};
        AtriId *statements = NULL;
        size_t statement_count = 0, statement_capacity = 0;
        bool body_ok = true;
        while (!p->failed && p->current.kind != ATRI_TOKEN_CASE && p->current.kind != ATRI_TOKEN_DEFAULT && p->current.kind != ATRI_TOKEN_RIGHT_BRACE) {
            if (p->current.kind == ATRI_TOKEN_THROUGH) {
                advance(p); item.through = true;
                if (!expect(p, ATRI_TOKEN_SEMICOLON, "expected ';' after #through") ||
                    (p->current.kind != ATRI_TOKEN_CASE && p->current.kind != ATRI_TOKEN_DEFAULT)) {
                    fail(p, "#through must end a non-final switch case"); body_ok = false;
                }
                break;
            }
            AtriId statement;
            if (!parse_statement(p, &statement) || !atri_append((void **)&statements, &statement_count,
                &statement_capacity, sizeof(statement), &statement)) { body_ok = false; break; }
        }
        body.span.end = p->current.span.start;
        body.first_statement = p->program.block_statement_count; body.statement_count = statement_count;
        for (size_t i = 0; body_ok && i < statement_count; ++i) {
            body_ok = atri_append((void **)&p->program.block_statements, &p->program.block_statement_count,
                &p->block_statements_cap, sizeof(statements[i]), &statements[i]);
        }
        free(statements);
        item.body = p->program.block_count; item.span.end = body.span.end;
        if (!body_ok || !atri_append((void **)&p->program.blocks, &p->program.block_count, &p->blocks_cap, sizeof(body), &body) ||
            !atri_append((void **)&cases, &count, &capacity, sizeof(item), &item)) { fail(p, "cannot build switch cases"); goto done; }
        if (count > 1024) { fail(p, "switch exceeds the prototype limit of 1024 cases"); goto done; }
    }
    if (!expect(p, ATRI_TOKEN_RIGHT_BRACE, "expected '}' after switch")) { goto done; }
    s->first_case = p->program.case_count; s->case_count = count;
    for (size_t i = 0; i < count; ++i) {
        if (!atri_append((void **)&p->program.cases, &p->program.case_count, &p->cases_cap, sizeof(cases[i]), &cases[i])) {
            fail(p, "out of memory"); goto done;
        }
    }
    ok = true;
done:
    free(cases); return ok;
}
static bool parse_statement(Parser *p, AtriId *id) {
    if (++p->statement_depth > 256) { --p->statement_depth; return fail(p, "statement nesting exceeds the prototype limit of 256"); }
    AtriStatement s = new_statement(p);
    AtriTokenKind token = p->current.kind;
    bool ok = false, semicolon = false;
    switch (token) {
        case ATRI_TOKEN_SWITCH:
            if (!parse_switch(p, &s)) { goto done; }
            break;
        case ATRI_TOKEN_THROUGH:
            fail(p, "#through is only allowed at the end of a non-final switch case"); goto done;
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
            if (p->current.kind != ATRI_TOKEN_SEMICOLON && !parse_expression(p, 0, &s.expression)) goto done;
            semicolon = true; break;
        case ATRI_TOKEN_DEFER:
            s.kind = ATRI_STMT_DEFER; advance(p);
            if (p->current.kind == ATRI_TOKEN_LEFT_PAREN && !condition(p, &s.expression)) { goto done; }
            if (p->current.kind == ATRI_TOKEN_LEFT_BRACE) {
                if (!parse_block(p, &s.body)) { goto done; }
            } else {
                if (!parse_expression(p, 0, &s.target)) { goto done; }
                if (p->program.expressions[s.target].kind != ATRI_EXPR_CALL) {
                    fail(p, "defer requires a block or a direct function call"); goto done;
                }
                semicolon = true;
            }
            break;
        case ATRI_TOKEN_BREAK: case ATRI_TOKEN_CONTINUE:
            s.kind = token == ATRI_TOKEN_BREAK ? ATRI_STMT_BREAK : ATRI_STMT_CONTINUE;
            advance(p); semicolon = true; break;
        default:
            if (!simple_statement(p, &s, true)) goto done;
            semicolon = s.kind != ATRI_STMT_EXPR || !p->program.expressions[s.expression].run_block ||
                p->current.kind == ATRI_TOKEN_SEMICOLON; break;
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
        .first_statement = p->program.statement_count, .block = ATRI_NONE};
    if (p->current.kind == ATRI_TOKEN_EXTERN) {
        advance(p);
        if (!expect(p, ATRI_TOKEN_LEFT_PAREN, "expected '(' after extern")) {
            return false;
        }
        AtriSpan convention = p->current.span;
        if (p->current.kind != ATRI_TOKEN_IDENTIFIER || convention.end-convention.start != 1 ||
            p->lexer.source.text[convention.start] != 'C') {
            return fail(p, "only the C calling convention is supported");
        }
        advance(p);
        if (!expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after #extern(C")) {
            return false;
        }
        f.external_c = true;
    }
    if (!expect(p, ATRI_TOKEN_FN, "expected 'fn' at the top level")) return false;
    f.name = p->current.span;
    if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a function name") ||
        !expect(p, ATRI_TOKEN_LEFT_PAREN, "expected '(' after the function name")) return false;
    if (p->current.kind != ATRI_TOKEN_RIGHT_PAREN) {
        for (;;) {
            if (p->current.kind == ATRI_TOKEN_ELLIPSIS) {
                if (!f.external_c) { return fail(p, "variadic parameters currently require an extern(C) declaration"); }
                f.variadic = true; advance(p);
                if (p->current.kind != ATRI_TOKEN_RIGHT_PAREN) { return fail(p, "ellipsis must be the last parameter"); }
                break;
            }
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
    if (f.external_c) {
        f.span.end = p->current.span.end;
        if (!expect(p, ATRI_TOKEN_SEMICOLON, "extern(C) declarations require ';' and cannot have a body yet")) {
            return false;
        }
    } else {
        if (!parse_block(p, &f.block)) {
            return false;
        }
        f.body = p->program.blocks[f.block].span; f.span.end = f.body.end;
    }
    f.statement_count = p->program.statement_count - f.first_statement;
    return atri_append((void **)&p->program.functions, &p->program.function_count,
                       &p->functions_cap, sizeof(f), &f) || fail(p, "out of memory");
}
static bool parse_struct(Parser *p, bool exported) {
    AtriStruct structure = {.span.start = p->current.span.start, .first_field = p->program.field_count,
        .exported = exported, .is_union = p->current.kind == ATRI_TOKEN_UNION};
    advance(p); structure.name = p->current.span;
    if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a struct or union name")) { return false; }
    if (p->current.kind == ATRI_TOKEN_SEMICOLON) {
        structure.opaque = true; structure.span.end = p->current.span.end; advance(p);
        return atri_append((void **)&p->program.structs, &p->program.struct_count, &p->structs_cap, sizeof(structure), &structure) || fail(p, "out of memory");
    }
    if (!expect(p, ATRI_TOKEN_LEFT_BRACE, "expected '{' or ';' after struct/union name")) { return false; }
    while (p->current.kind != ATRI_TOKEN_RIGHT_BRACE) {
        AtriField field = {.name = p->current.span, .expression = ATRI_NONE};
        if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected a named struct/union field") ||
            !expect(p, ATRI_TOKEN_COLON, "expected ':' after field name") ||
            !parse_type(p, &field.type, "expected field type")) { return false; }
        if (p->current.kind == ATRI_TOKEN_EQUAL) {
            if (structure.is_union) { return fail(p, "union members cannot have declaration default values"); }
            advance(p); if (!parse_expression(p, 0, &field.expression)) { return false; }
        }
        if (!expect(p, ATRI_TOKEN_SEMICOLON, "expected ';' after struct/union field") ||
            !atri_append((void **)&p->program.fields, &p->program.field_count, &p->fields_cap, sizeof(field), &field)) { return fail(p, "cannot append struct/union field"); }
    }
    structure.field_count = p->program.field_count-structure.first_field;
    if (structure.is_union && !structure.field_count) { return fail(p, "union requires at least one named member"); }
    structure.span.end = p->current.span.end; advance(p);
    if (p->current.kind == ATRI_TOKEN_SEMICOLON) { advance(p); }
    return atri_append((void **)&p->program.structs, &p->program.struct_count, &p->structs_cap, sizeof(structure), &structure) || fail(p, "out of memory");
}
static bool enum_start(Parser *p) {
    if (!named_import_start(p)) { return false; }
    AtriLexer look = p->lexer;
    AtriDiagnostic ignored;
    atri_lexer_next(&look, &ignored);
    return atri_lexer_next(&look, &ignored).kind == ATRI_TOKEN_ENUM;
}
static bool parse_enum(Parser *p, bool exported) {
    AtriEnum item = {.name = p->current.span, .span.start = p->current.span.start,
        .first_variant = p->program.variant_count, .exported = exported};
    advance(p);
    if (!expect(p, ATRI_TOKEN_SCOPE, "expected '::' before enum") || !expect(p, ATRI_TOKEN_ENUM, "expected enum")) { return false; }
    if (p->current.kind == ATRI_TOKEN_LEFT_PAREN) {
        advance(p); item.underlying = p->current.span;
        if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected enum integer type or bit8/16/32/64") ||
            !expect(p, ATRI_TOKEN_RIGHT_PAREN, "expected ')' after enum underlying type")) { return false; }
    }
    if (!expect(p, ATRI_TOKEN_LEFT_BRACE, "expected '{' after enum")) { return false; }
    while (!p->failed && p->current.kind != ATRI_TOKEN_RIGHT_BRACE) {
        AtriEnumVariant variant = {.name = p->current.span, .expression = ATRI_NONE};
        if (!expect(p, ATRI_TOKEN_IDENTIFIER, "expected enum member name")) { return false; }
        if (p->current.kind == ATRI_TOKEN_EQUAL) {
            advance(p);
            if (!parse_expression(p, 0, &variant.expression)) { return false; }
        }
        if (!atri_append((void **)&p->program.variants, &p->program.variant_count, &p->variants_cap, sizeof(variant), &variant)) { return fail(p, "out of memory"); }
        if (p->current.kind != ATRI_TOKEN_COMMA) { break; }
        advance(p);
    }
    item.variant_count = p->program.variant_count-item.first_variant;
    if (!item.variant_count) { return fail(p, "enum requires at least one member"); }
    item.span.end = p->current.span.end;
    if (!expect(p, ATRI_TOKEN_RIGHT_BRACE, "expected '}' after enum members")) { return false; }
    if (p->current.kind == ATRI_TOKEN_SEMICOLON) { advance(p); }
    return atri_append((void **)&p->program.enums, &p->program.enum_count, &p->enums_cap, sizeof(item), &item) || fail(p, "out of memory");
}
static bool parse_range(AtriSource source, const AtriModule *modules, size_t count, size_t start, size_t *next,
                        bool index_declarations, AtriProgram *output, AtriDiagnostic *diagnostic) {
    *diagnostic = (AtriDiagnostic){0};
    Parser p = {.lexer = atri_lexer_create(source), .diagnostic = diagnostic, .program = *output,
        .functions_cap = output->function_count, .parameters_cap = output->parameter_count,
        .statements_cap = output->statement_count, .expressions_cap = output->expression_count,
        .arguments_cap = output->argument_count, .blocks_cap = output->block_count,
        .block_statements_cap = output->block_statement_count, .array_types_cap = output->array_type_count,
        .imports_cap = output->import_count, .globals_cap = output->global_count, .structs_cap = output->struct_count,
        .fields_cap = output->field_count, .initializers_cap = output->initializer_count, .enums_cap = output->enum_count,
        .variants_cap = output->variant_count, .cases_cap = output->case_count, .runs_cap = output->run_count};
    size_t module = 0, declaration_capacity = output->declaration_count, annotation_capacity = output->annotation_count;
    if (start) { p.lexer.offset = start; }
    if (count) { p.lexer.source.length = modules[0].span.end; }
    AtriSyntaxScope scope = {0};
    p.lexer.scope = &scope;
    advance(&p);
    while (!p.failed) {
        if (p.current.kind == ATRI_TOKEN_EOF) {
            if (++module >= count) { break; }
            p.lexer.offset = modules[module].span.start;
            p.lexer.source.length = modules[module].span.end;
            if (p.lexer.source.length-p.lexer.offset >= 3 &&
                !memcmp(source.text+p.lexer.offset, "\xEF\xBB\xBF", 3)) { p.lexer.offset += 3; }
            advance(&p); continue;
        }
        scope.count = 0;
        AtriDeclaration declaration = {.span.start = p.current.span.start, .first_annotation = p.program.annotation_count};
        while (p.current.kind == ATRI_TOKEN_ANNOTATION) {
            advance(&p);
            if (p.current.kind == ATRI_TOKEN_IDENTIFIER &&
                !atri_activate_annotation(&scope, source, p.current.span, diagnostic)) { p.failed=true; break; }
            if (p.current.kind == ATRI_TOKEN_IDENTIFIER && index_declarations) {
                if (!atri_append((void **)&p.program.annotations, &p.program.annotation_count, &annotation_capacity,
                                 sizeof(p.current.span), &p.current.span)) { fail(&p, "out of memory recording annotation"); break; }
                ++declaration.annotation_count;
            }
            if (!expect(&p, ATRI_TOKEN_IDENTIFIER, "expected an annotation name after '@'")) { break; }
        }
        if (p.failed) { break; }
        bool exported = false;
        if (p.current.kind == ATRI_TOKEN_EXPORT) { exported = true; advance(&p); }
        if (enum_start(&p)) {
            if (!parse_enum(&p, exported)) { break; }
            declaration.kind = ATRI_META_ENUM;
            declaration.name = p.program.enums[p.program.enum_count-1].name;
        } else if (p.current.kind == ATRI_TOKEN_STRUCT || p.current.kind == ATRI_TOKEN_UNION) {
            declaration.kind = p.current.kind == ATRI_TOKEN_STRUCT ? ATRI_META_STRUCT : ATRI_META_UNION;
            if (!parse_struct(&p, exported)) { break; }
            declaration.name = p.program.structs[p.program.struct_count-1].name;
        } else if (p.current.kind == ATRI_TOKEN_IMPORT ||
            named_import_start(&p)) {
            if (exported) { fail(&p, "re-exporting imports is not supported"); break; }
            AtriImport item = {.target = ATRI_NONE};
            if (p.current.kind == ATRI_TOKEN_IDENTIFIER) {
                item.name = p.current.span; advance(&p);
                if (!expect(&p, ATRI_TOKEN_SCOPE, "expected '::' before import")) break;
            }
            if (!expect(&p, ATRI_TOKEN_IMPORT, "expected import")) break;
            item.path = p.current.span;
            if (!expect(&p, ATRI_TOKEN_STRING, "expected a module path") ||
                !expect(&p, ATRI_TOKEN_SEMICOLON, "expected ';' after import")) break;
            if (!atri_append((void **)&p.program.imports, &p.program.import_count, &p.imports_cap, sizeof(item), &item)) { fail(&p, "out of memory"); break; }
            declaration.kind = ATRI_META_IMPORT;
            declaration.name = item.name;
        } else if (p.current.kind == ATRI_TOKEN_RUN) {
            if (exported) { fail(&p, "#run execution statements cannot be exported"); break; }
            AtriStatement run = new_statement(&p);
            run.kind = ATRI_STMT_EXPR;
            if (!parse_expression(&p, 0, &run.expression)) { break; }
            if (p.program.expressions[run.expression].kind != ATRI_EXPR_RUN) {
                fail(&p, "expected a standalone #run expression"); break;
            }
            bool semi = !p.program.expressions[run.expression].run_block || p.current.kind == ATRI_TOKEN_SEMICOLON;
            run.span.end = semi ? p.current.span.end : p.previous_end;
            AtriId id;
            if ((semi && !expect(&p, ATRI_TOKEN_SEMICOLON, "expected ';' after #run expression")) ||
                !append_statement(&p, run, &id) || !atri_append((void **)&p.program.runs, &p.program.run_count,
                    &p.runs_cap, sizeof(id), &id)) { fail(&p, "cannot build top-level #run statement"); break; }
            declaration.kind = ATRI_META_RUN;
        } else if (declaration_start(&p)) {
            AtriGlobal item = {.declaration = new_statement(&p), .exported = exported};
            if (!simple_statement(&p, &item.declaration, true)) { break; }
            item.declaration.span.end = p.current.span.end;
            if (!expect(&p, ATRI_TOKEN_SEMICOLON, "expected ';' after global declaration")) { break; }
            if (item.declaration.kind == ATRI_STMT_CONSTANT && item.declaration.expression == ATRI_NONE) { fail(&p, "compile-time constants require an initializer"); break; }
            if (!atri_append((void **)&p.program.globals, &p.program.global_count, &p.globals_cap, sizeof(item), &item)) { fail(&p, "out of memory"); break; }
            declaration.kind = ATRI_META_GLOBAL;
            declaration.name = item.declaration.name;
        } else {
            if (!parse_function(&p)) break;
            p.program.functions[p.program.function_count-1].exported = exported;
            declaration.kind = ATRI_META_FUNCTION;
            declaration.name = p.program.functions[p.program.function_count-1].name;
        }
        declaration.span.end = p.previous_end;
        if (index_declarations && !atri_append((void **)&p.program.declarations, &p.program.declaration_count,
                                               &declaration_capacity, sizeof(declaration), &declaration)) {
            fail(&p, "out of memory indexing declarations");
            break;
        }
        if (next) { break; }
    }
    if (next) { *next = p.current.span.start; }
    free(scope.ids);
    *output = p.program;
    return !p.failed;
}
bool atri_parse_fragment(AtriSource source, AtriSpan span, AtriProgram *output, AtriDiagnostic *diagnostic) {
    source.length = span.end;
    return parse_range(source, NULL, 0, span.start, NULL, true, output, diagnostic);
}
bool atri_parse_next(AtriSource source, AtriSpan span, size_t *next, AtriProgram *output, AtriDiagnostic *diagnostic) {
    source.length = span.end;
    if (span.end-span.start >= 3 && !memcmp(source.text + span.start, "\xEF\xBB\xBF", 3)) { span.start += 3; }
    return parse_range(source, NULL, 0, span.start, next, true, output, diagnostic);
}
bool atri_parse_sources(AtriSource source, const AtriModule *modules, size_t count, bool index_declarations,
                        AtriProgram *output, AtriDiagnostic *diagnostic) {
    *output = (AtriProgram){0};
    if (parse_range(source, modules, count, 0, NULL, index_declarations, output, diagnostic)) { return true; }
    atri_program_destroy(output);
    return false;
}
bool atri_parse(AtriSource source, AtriProgram *output, AtriDiagnostic *diagnostic) {
    return atri_parse_sources(source, NULL, 0, false, output, diagnostic);
}
void atri_program_destroy(AtriProgram *p) {
    free(p->declarations); free(p->annotations);
    free(p->enums); free(p->variants); free(p->cases); free(p->runs);
    for (size_t i = 0; i < p->module_count; ++i) { free(p->modules[i].path); atri_source_map_destroy(p->modules[i].origins); }
    free(p->array_types); free(p->structs); free(p->fields); free(p->initializers);
    if (p->owns_run_cache) { atri_run_cache_destroy(p->run_cache); }
    free(p->symbols); free(p->modules); free(p->imports); free(p->globals);
    free(p->functions); free(p->parameters); free(p->statements);
    free(p->expressions); free(p->arguments); free(p->blocks); free(p->block_statements);
    *p = (AtriProgram){0};
}
static void write_span(FILE *s, AtriSource source, AtriSpan span) {
    fwrite(source.text + span.start, 1, span.end - span.start, s);
}
static void dump_expr(FILE *s, AtriSource source, const AtriProgram *p, AtriId id) {
    const AtriExpr *e = &p->expressions[id];
    if (e->kind == ATRI_EXPR_CAST) {
        fputs("cast(", s); write_span(s, source, e->text); fputs(", ", s);
        dump_expr(s, source, p, e->left); fputc(')', s); return;
    }
    if (e->kind == ATRI_EXPR_SIZEOF || e->kind == ATRI_EXPR_ALIGNOF) {
        fputs(e->kind == ATRI_EXPR_SIZEOF ? "sizeof(" : "alignof(", s);
        write_span(s, source, e->text); fputc(')', s); return;
    }
    if (e->kind == ATRI_EXPR_RUN) {
        if (e->run_block) { write_span(s, source, e->span); }
        else { fputs("#run ", s); dump_expr(s, source, p, e->left); }
        return;
    }
    if (e->kind == ATRI_EXPR_INDEX) {
        dump_expr(s, source, p, e->left); fputc('[', s); dump_expr(s, source, p, e->right); fputc(']', s); return;
    }
    if (e->kind == ATRI_EXPR_CONDITIONAL) {
        fputc('(', s); dump_expr(s, source, p, e->left); fputs(" ? ", s);
        dump_expr(s, source, p, e->right); fputs(" : ", s); dump_expr(s, source, p, e->alternative); fputc(')', s); return;
    }
    if (e->kind == ATRI_EXPR_FIELD) {
        dump_expr(s, source, p, e->left); fputc('.', s); write_span(s, source, e->text); return;
    }
    if (e->kind == ATRI_EXPR_INITIALIZER) {
        fputc('{', s);
        for (size_t i = 0; i < e->argument_count; ++i) {
            const AtriInitializer *item = &p->initializers[e->first_argument+i];
            if (i) { fputs(", ", s); }
            if (item->name.end) { fputc('.', s); write_span(s, source, item->name); fputs(" = ", s); }
            dump_expr(s, source, p, item->expression);
        }
        fputc('}', s); return;
    }
    if ((e->kind == ATRI_EXPR_NAME || e->kind == ATRI_EXPR_CALL) && e->qualifier.end) {
        if (e->module.end) { write_span(s, source, e->module); fputs("::", s); }
        write_span(s, source, e->qualifier); fputs("::", s);
    }
    if (e->kind == ATRI_EXPR_INTEGER || e->kind == ATRI_EXPR_CHARACTER || e->kind == ATRI_EXPR_NAME || e->kind == ATRI_EXPR_NULL || e->kind == ATRI_EXPR_BOOL || e->kind == ATRI_EXPR_STRING) {
        write_span(s, source, e->text);
    } else if (e->kind == ATRI_EXPR_CALL) {
        write_span(s, source, e->text); fputc('(', s);
        for (size_t i = 0; i < e->argument_count; ++i) {
            if (i != 0) fputs(", ", s);
            dump_expr(s, source, p, p->arguments[e->first_argument + i]);
        }
        fputc(')', s);
    } else if (e->kind == ATRI_EXPR_UNARY || e->kind == ATRI_EXPR_ADDRESS || e->kind == ATRI_EXPR_DEREFERENCE) {
        fputc('(', s);
        fputc(e->kind == ATRI_EXPR_ADDRESS ? '&' : e->kind == ATRI_EXPR_DEREFERENCE ? '*' : e->op == ATRI_NOT ? '!' : e->op == ATRI_BIT_NOT ? '~' : e->op == ATRI_ADD ? '+' : '-', s);
        dump_expr(s, source, p, e->left); fputc(')', s);
    } else {
        const char *ops[] = {"+", "-", "*", "/", "%", "-", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "!", "&", "|", "^", "~", "<<", ">>"};
        fputc('(', s); dump_expr(s, source, p, e->left);
        if (e->custom) { fputc(' ', s); write_span(s, source, e->text); fputc(' ', s); }
        else { fprintf(s, " %s ", ops[e->op]); }
        dump_expr(s, source, p, e->right); fputc(')', s);
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
        "For", "Loop", "Break", "Continue", "Expression", "Switch", "Defer"};
    fprintf(s, "%*s%s", indent, "", names[st->kind]);
    if (st->kind == ATRI_STMT_CONSTANT || st->kind == ATRI_STMT_VARIABLE) {
        fputc(' ', s); write_span(s, source, st->name); fputs(": ", s); write_span(s, source, st->type);
        fputs(st->kind == ATRI_STMT_CONSTANT ? " := " : " = ", s);
        if (st->expression == ATRI_NONE) fputs("0 (default)", s);
    } else if (st->kind == ATRI_STMT_ASSIGN) {
        fputc(' ', s); dump_expr(s, source, p, st->target);
        if (st->compound) {
            static const char *ops[] = {"+", "-", "*", "/", "%", "-", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "!", "&", "|", "^", "~", "<<", ">>"};
            fprintf(s, " %s= ", ops[st->op]);
        } else { fputs(" = ", s); }
    } else if (st->expression != ATRI_NONE) fputc(' ', s);
    if (st->expression != ATRI_NONE) dump_expr(s, source, p, st->expression);
    if (st->kind == ATRI_STMT_DEFER && st->target != ATRI_NONE) {
        fputc(' ', s); dump_expr(s, source, p, st->target);
    }
    fputc('\n', s);
    if (st->kind == ATRI_STMT_SWITCH) {
        for (size_t i = 0; i < st->case_count; ++i) {
            const AtriCase *item = &p->cases[st->first_case+i];
            fprintf(s, "%*s%s", indent+2, "", item->expression == ATRI_NONE ? "Default" : "Case ");
            if (item->expression != ATRI_NONE) { dump_expr(s, source, p, item->expression); }
            fputc('\n', s); dump_block(s, source, p, item->body, indent+4);
            if (item->through) { fprintf(s, "%*s#through\n", indent+4, ""); }
        }
    }
    if (st->initializer != ATRI_NONE) dump_statement(s, source, p, st->initializer, indent + 2);
    if (st->body != ATRI_NONE) dump_block(s, source, p, st->body, indent + 2);
    if (st->step != ATRI_NONE) dump_statement(s, source, p, st->step, indent + 2);
    if (st->otherwise != ATRI_NONE) { fprintf(s, "%*sElse\n", indent, ""); dump_statement(s, source, p, st->otherwise, indent + 2); }
}
void atri_dump_ast(FILE *s, AtriSource source, const AtriProgram *p) {
    fputs("Program\n", s);
    for (size_t i = 0; i < p->declaration_count; ++i) {
        const AtriDeclaration *decl = &p->declarations[i];
        if (!decl->annotation_count) { continue; }
        fputs("  Annotations", s);
        for (size_t a = 0; a < decl->annotation_count; ++a) {
            fputs(" @", s); write_span(s, source, p->annotations[decl->first_annotation + a]);
        }
        if (decl->name.end) { fputs(" on ", s); write_span(s, source, decl->name); }
        fputc('\n', s);
    }
    for (size_t i = 0; i < p->enum_count; ++i) {
        const AtriEnum *item = &p->enums[i];
        fprintf(s, "  %sEnum ", item->exported ? "Export " : ""); write_span(s, source, item->name);
        fputc('\n', s);
        for (size_t j = 0; j < item->variant_count; ++j) {
            const AtriEnumVariant *variant = &p->variants[item->first_variant+j];
            fputs("    Member ", s); write_span(s, source, variant->name);
            if (variant->expression != ATRI_NONE) { fputs(" = ", s); dump_expr(s, source, p, variant->expression); }
            fputc('\n', s);
        }
    }
    for (size_t i = 0; i < p->struct_count; ++i) {
        const AtriStruct *t = &p->structs[i];
        fputs(t->exported ? "  Export " : "  ", s);
        fputs(t->is_union ? "Union " : "Struct ", s); write_span(s, source, t->name); fputc('\n', s);
        for (size_t j = 0; j < t->field_count; ++j) {
            const AtriField *f = &p->fields[t->first_field+j];
            fputs("    Field ", s); write_span(s, source, f->name); fputs(": ", s); write_span(s, source, f->type);
            if (f->expression != ATRI_NONE) { fputs(" = ", s); dump_expr(s, source, p, f->expression); }
            fputc('\n', s);
        }
    }
    for (size_t i = 0; i < p->import_count; ++i) {
        fputs("  Import ", s);
        if (p->imports[i].name.end) { write_span(s, source, p->imports[i].name); fputs(" :: ", s); }
        write_span(s, source, p->imports[i].path); fputc('\n', s);
    }
    for (size_t i = 0; i < p->global_count; ++i) {
        const AtriGlobal *g = &p->globals[i];
        fputs(g->exported ? "  Export Global " : "  Global ", s);
        write_span(s, source, g->declaration.name); fputs(": ", s); write_span(s, source, g->declaration.type);
        if (g->declaration.expression != ATRI_NONE) {
            fputs(g->declaration.kind == ATRI_STMT_CONSTANT ? " := " : " = ", s);
            dump_expr(s, source, p, g->declaration.expression);
        }
        fputc('\n', s);
    }
    for (size_t i = 0; i < p->run_count; ++i) { dump_statement(s, source, p, p->runs[i], 2); }
    for (size_t i = 0; i < p->function_count; ++i) {
        const AtriFunction *f = &p->functions[i];
        if (f->external_c) {
            fputs("  [[extern(C)]]\n", s);
        }
        if (f->exported) { fputs("  Export\n", s); }
        fputs("  Function ", s); write_span(s, source, f->name);
        fputs(" -> ", s); write_span(s, source, f->return_type); fputc('\n', s);
        if (f->variadic) { fputs("    Variadic ...\n", s); }
        for (size_t j = 0; j < f->parameter_count; ++j) {
            const AtriParameter *a = &p->parameters[f->first_parameter + j];
            fputs("    Parameter ", s); write_span(s, source, a->name);
            fputs(": ", s); write_span(s, source, a->type); fputc('\n', s);
        }
        if (!f->external_c) {
            dump_block(s, source, p, f->block, 4);
        }
    }
}
