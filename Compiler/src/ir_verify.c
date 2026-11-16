#include "ir.h"
#include <stdlib.h>
#include <string.h>

/* SSA values must dominate their uses. Local objects carry mutable state across edges. */
typedef struct {
    const AtriIRFunction *f;
    size_t count, words, use_block;
    size_t *owners;
    uint64_t *dominators;
    bool *reachable;
} Check;
static bool bad(AtriDiagnostic *d, AtriSpan span, const char *message) {
    *d = (AtriDiagnostic){span, message}; return false;
}
static bool valid_type(AtriIRType t) { return atri_ir_is_integer(t) || t == ATRI_IR_BOOL || t == ATRI_IR_PTR_I32; }
static bool available(const Check *c, size_t use, AtriId id) {
    if (id >= use) return false;
    size_t block = c->owners[id];
    return !c->reachable[c->use_block] ||
        (c->dominators[c->use_block * c->words + block / 64] & (UINT64_C(1) << (block % 64))) != 0;
}
static bool operand(const Check *c, size_t use, AtriId id, AtriIRType type) {
    return available(c, use, id) && atri_ir_has_value(c->f->instructions[id].kind) &&
        c->f->instructions[id].type == type;
}
static bool location(const Check *c, size_t use, AtriId id, AtriIRType type) {
    if (!available(c, use, id)) return false;
    const AtriIRInst *v = &c->f->instructions[id];
    return v->kind == ATRI_IR_LOCAL ? v->type == type :
        type == ATRI_IR_I32 && operand(c, use, id, ATRI_IR_PTR_I32);
}
static bool terminator(AtriIRKind k) {
    return k == ATRI_IR_RETURN || k == ATRI_IR_JUMP || k == ATRI_IR_BRANCH || k == ATRI_IR_UNREACHABLE;
}
static size_t successors(const AtriIRFunction *f, size_t end, AtriId *out) {
    const AtriIRInst *v = &f->instructions[end];
    if (v->kind == ATRI_IR_JUMP) { out[0] = v->target; return 1; }
    if (v->kind == ATRI_IR_BRANCH) {
        const AtriIRInst *cond = &f->instructions[v->left];
        if (cond->kind == ATRI_IR_CONST) { out[0] = cond->immediate ? v->target : v->alternative; return 1; }
        out[0] = v->target; out[1] = v->alternative; return 2;
    }
    return 0;
}
static bool verify_function(const AtriIRProgram *p, const AtriIRFunction *f, AtriDiagnostic *d) {
    if (!valid_type(f->return_type) || (f->parameter_count && !f->parameter_types))
        return bad(d, f->name, "IR verifier: invalid function signature");
    for (size_t a = 0; a < f->parameter_count; ++a)
        if (!valid_type(f->parameter_types[a])) return bad(d, f->name, "IR verifier: invalid parameter type");
    if (f->constant_count) return bad(d, f->name, "IR verifier: unresolved compile-time constants");
    size_t blocks = f->block_count ? f->block_count : 1;
    if (!f->instruction_count || blocks > 4096) return bad(d, f->name, "IR verifier: invalid block count");
    Check c = {.f = f, .count = blocks, .words = (blocks + 63) / 64};
    size_t *ends = malloc(blocks * sizeof(*ends));
    bool *seen = calloc(blocks, sizeof(*seen));
    c.owners = malloc(f->instruction_count * sizeof(*c.owners));
    c.reachable = calloc(blocks, sizeof(*c.reachable));
    c.dominators = calloc(blocks * c.words, sizeof(*c.dominators));
    uint64_t *next = malloc(c.words * sizeof(*next));
    bool ok = false;
    if (!ends || !seen || !c.owners || !c.reachable || !c.dominators || !next) {
        bad(d, f->name, "out of memory in IR verifier"); goto done;
    }
    for (size_t b = 0; b < blocks; ++b) ends[b] = ATRI_NONE;
    size_t current = 0; bool closed = false; seen[0] = true;
    for (size_t i = 0; i < f->instruction_count; ++i) {
        const AtriIRInst *v = &f->instructions[i];
        if (!valid_type(v->type)) { bad(d, v->span, "IR verifier: invalid type"); goto done; }
        if (v->kind == ATRI_IR_LABEL) {
            if (!closed || v->immediate >= blocks || seen[v->immediate]) {
                bad(d, v->span, "IR verifier: invalid block label or missing terminator"); goto done;
            }
            current = (size_t)v->immediate; seen[current] = true; closed = false;
        } else if (closed && v->kind != ATRI_IR_NOP) {
            bad(d, v->span, "IR verifier: instruction after block terminator"); goto done;
        }
        c.owners[i] = current;
        if (v->kind == ATRI_IR_BRANCH &&
            (v->left >= i || !atri_ir_has_value(f->instructions[v->left].kind) || f->instructions[v->left].type != ATRI_IR_BOOL)) {
            bad(d, v->span, "IR verifier: branch condition must be bool"); goto done;
        }
        if ((v->kind == ATRI_IR_JUMP || v->kind == ATRI_IR_BRANCH) &&
            (v->target >= blocks || (v->kind == ATRI_IR_BRANCH && v->alternative >= blocks))) {
            bad(d, v->span, "IR verifier: invalid jump target"); goto done;
        }
        if (terminator(v->kind)) { ends[current] = i; closed = true; }
    }
    for (size_t b = 0; b < blocks; ++b)
        if (!seen[b] || ends[b] == ATRI_NONE) { bad(d, f->name, "IR verifier: block must end with a terminator"); goto done; }
    c.reachable[0] = true;
    bool changed;
    do {
        changed = false;
        for (size_t b = 0; b < blocks; ++b) if (c.reachable[b]) {
            AtriId edges[2]; size_t count = successors(f, ends[b], edges);
            for (size_t j = 0; j < count; ++j) if (!c.reachable[edges[j]]) { c.reachable[edges[j]] = true; changed = true; }
        }
    } while (changed);
    for (size_t b = 0; b < blocks; ++b) {
        if (c.reachable[b] && f->instructions[ends[b]].kind == ATRI_IR_UNREACHABLE) {
            bad(d, f->name, "function must return a value on every reachable path"); goto done;
        }
        for (size_t w = 0; w < c.words; ++w) c.dominators[b*c.words+w] = b == 0 ? 0 : UINT64_MAX;
        if (b == 0) c.dominators[0] = 1;
    }
    do {
        changed = false;
        for (size_t b = 1; b < blocks; ++b) if (c.reachable[b]) {
            for (size_t w = 0; w < c.words; ++w) next[w] = UINT64_MAX;
            for (size_t pred = 0; pred < blocks; ++pred) if (c.reachable[pred]) {
                AtriId edges[2]; size_t count = successors(f, ends[pred], edges);
                for (size_t e = 0; e < count; ++e) if (edges[e] == b)
                    for (size_t w = 0; w < c.words; ++w) next[w] &= c.dominators[pred*c.words+w];
            }
            next[b/64] |= UINT64_C(1) << (b%64);
            for (size_t w = 0; w < c.words; ++w)
                if (next[w] != c.dominators[b*c.words+w]) { changed = true; c.dominators[b*c.words+w] = next[w]; }
        }
    } while (changed);
    for (size_t i = 0; i < f->instruction_count; ++i) {
        const AtriIRInst *v = &f->instructions[i]; c.use_block = c.owners[i];
        bool valid = true;
        switch (v->kind) {
            case ATRI_IR_NOP: case ATRI_IR_LOCAL: case ATRI_IR_LABEL: case ATRI_IR_JUMP: case ATRI_IR_UNREACHABLE: break;
            case ATRI_IR_CONST:
                valid = !(v->type == ATRI_IR_I32 && v->immediate > UINT32_MAX) &&
                    !(v->type == ATRI_IR_BOOL && v->immediate > 1) &&
                    !(v->type == ATRI_IR_PTR_I32 && v->immediate != 0); break;
            case ATRI_IR_PARAM:
                valid = c.use_block == 0 && v->immediate < f->parameter_count && v->type == f->parameter_types[v->immediate]; break;
            case ATRI_IR_UNARY:
                valid = operand(&c, i, v->left, v->type) &&
                    ((v->op == ATRI_NEG && atri_ir_is_integer(v->type)) || (v->op == ATRI_NOT && v->type == ATRI_IR_BOOL)); break;
            case ATRI_IR_RETURN: valid = v->type == f->return_type && operand(&c, i, v->left, v->type); break;
            case ATRI_IR_BRANCH: valid = operand(&c, i, v->left, ATRI_IR_BOOL); break;
            case ATRI_IR_BINARY:
                valid = atri_ir_is_integer(v->type) && operand(&c, i, v->left, v->type) &&
                    operand(&c, i, v->right, v->type) && v->op >= ATRI_ADD && v->op <= ATRI_REM; break;
            case ATRI_IR_COMPARE: {
                AtriIRType t = v->left < i ? f->instructions[v->left].type : ATRI_IR_INVALID;
                valid = v->type == ATRI_IR_BOOL && operand(&c, i, v->left, t) && operand(&c, i, v->right, t) &&
                    v->op >= ATRI_EQ && v->op <= ATRI_GE &&
                    (atri_ir_is_integer(t) || ((t == ATRI_IR_BOOL || t == ATRI_IR_PTR_I32) && (v->op == ATRI_EQ || v->op == ATRI_NE)));
                break;
            }
            case ATRI_IR_ADDRESS:
                valid = v->type == ATRI_IR_PTR_I32 && available(&c, i, v->left) &&
                    f->instructions[v->left].kind == ATRI_IR_LOCAL && f->instructions[v->left].type == ATRI_IR_I32; break;
            case ATRI_IR_LOAD: valid = location(&c, i, v->left, v->type); break;
            case ATRI_IR_STORE: valid = location(&c, i, v->left, v->type) && operand(&c, i, v->right, v->type); break;
            case ATRI_IR_PTR_OFFSET:
                valid = v->type == ATRI_IR_PTR_I32 && (v->op == ATRI_ADD || v->op == ATRI_SUB) &&
                    operand(&c, i, v->left, ATRI_IR_PTR_I32) &&
                    (operand(&c, i, v->right, ATRI_IR_I32) || operand(&c, i, v->right, ATRI_IR_I64)); break;
            case ATRI_IR_PTR_DIFF:
                valid = v->type == ATRI_IR_I64 && operand(&c, i, v->left, ATRI_IR_PTR_I32) && operand(&c, i, v->right, ATRI_IR_PTR_I32); break;
            case ATRI_IR_CALL:
                valid = v->callee < p->function_count && v->type == p->functions[v->callee].return_type &&
                    v->argument_count == p->functions[v->callee].parameter_count &&
                    (!v->argument_count || p->functions[v->callee].parameter_types != NULL) &&
                    v->first_argument <= f->argument_count && v->argument_count <= f->argument_count-v->first_argument;
                if (valid) for (size_t a = 0; a < v->argument_count; ++a)
                    if (!operand(&c, i, f->arguments[v->first_argument+a], p->functions[v->callee].parameter_types[a])) valid = false;
                break;
            default: valid = false; break;
        }
        if (!valid) { bad(d, v->span, "IR verifier: invalid instruction, operands or dominance"); goto done; }
    }
    ok = true;
done:
    free(ends); free(seen); free(c.owners); free(c.reachable); free(c.dominators); free(next); return ok;
}
bool atri_ir_verify(const AtriIRProgram *p, AtriDiagnostic *d) {
    *d = (AtriDiagnostic){0};
    for (size_t i = 0; i < p->function_count; ++i) if (!verify_function(p, &p->functions[i], d)) return false;
    return true;
}
