#define ATRI_BUILD_HOST
#include "meta_syntax.h"
#include "meta_plugin.h"
#include "lexer.h"
#include "array.h"

typedef struct {
    char *name;
    AtriOperator *ops;
    size_t count, capacity;
} Annotation;
static thread_local struct {
    Annotation *entries;
    size_t count, capacity, first;
    bool registering, failed;
} registry;

static char *dup_string(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out) {
        memcpy(out, s, n + 1);
    }
    return out;
}
static int32_t reject(const char *message, const char *name) {
    fprintf(stderr, "error: %s: %s\n", message, name ? name : "");
    registry.failed = true;
    return 0;
}
void atri_syntax_begin(void) {
    registry.first = registry.count;
    registry.registering = true;
    registry.failed = false;
}
bool atri_syntax_end(void) {
    registry.registering = false;
    return !registry.failed;
}
void atri_syntax_destroy(void) {
    for (size_t i = 0; i < registry.count; ++i) {
        Annotation *a = &registry.entries[i];
        for (size_t j = 0; j < a->count; ++j) {
            free((char *)a->ops[j].text);
        }
        free(a->name);
        free(a->ops);
    }
    free(registry.entries);
    registry.entries = NULL;
    registry.capacity = registry.count = 0;
}
ATRI_HOST_EXPORT int32_t atri_register_annotation(const char *name) {
    if (!registry.registering || !name || !*name) {
        return reject("invalid annotation registration", name);
    }
    AtriLexer lexer = atri_lexer_create((AtriSource){"<annotation>", name, strlen(name)});
    AtriDiagnostic ignored;
    AtriToken token = atri_lexer_next(&lexer, &ignored);
    if (token.kind != ATRI_TOKEN_IDENTIFIER || token.span.start || token.span.end != lexer.source.length) {
        return reject("annotation name must be an identifier", name);
    }
    for (size_t i = 0; i < registry.count; ++i) {
        if (!strcmp(name, registry.entries[i].name)) {
            return reject("duplicate annotation registration", name);
        }
    }
    Annotation a = {.name = dup_string(name)};
    if (!a.name || !atri_append((void **)&registry.entries, &registry.count, &registry.capacity, sizeof(a), &a)) {
        free(a.name);
        return reject("out of memory registering annotation", name);
    }
    return 1;
}
ATRI_HOST_EXPORT int32_t atri_register_infix(const char *annotation, const char *symbol, uint32_t prec,
                                             uint32_t right) {
    if (!registry.registering || !annotation || !symbol || !*symbol || !prec || prec >= 30 || right > 1) {
        return reject("invalid infix registration", annotation);
    }
    for (const unsigned char *c = (const unsigned char *)symbol; *c; ++c) {
        if (*c < 128 && !strchr("!$%&*+-/<=>^|~", *c)) {
            return reject("invalid infix spelling", symbol);
        }
    }
    if (strstr(symbol, "//") || strstr(symbol, "/*")) {
        return reject("operator overlaps a comment delimiter", symbol);
    }
    AtriLexer lexer = atri_lexer_create((AtriSource){"<operator>", symbol, strlen(symbol)});
    AtriDiagnostic ignored;
    AtriToken token = atri_lexer_next(&lexer, &ignored);
    if (token.kind != ATRI_TOKEN_ERROR && token.span.end == lexer.source.length) {
        return reject("operator spelling already belongs to the language", symbol);
    }
    for (size_t i = registry.first; i < registry.count; ++i) {
        Annotation *a = &registry.entries[i];
        if (strcmp(annotation, a->name)) {
            continue;
        }
        for (size_t j = 0; j < a->count; ++j) {
            if (!strcmp(symbol, a->ops[j].text)) {
                return reject("duplicate operator registration", symbol);
            }
        }
        AtriOperator op = {dup_string(symbol), strlen(symbol), prec, right != 0};
        if (!op.text || !atri_append((void **)&a->ops, &a->count, &a->capacity, sizeof(op), &op)) {
            free((char *)op.text);
            return reject("out of memory registering operator", symbol);
        }
        return 1;
    }
    return reject("operator requires an annotation registered by this plugin", annotation);
}
const AtriOperator *atri_match_operator(const AtriSyntaxScope *scope, const char *text, size_t length) {
    const AtriOperator *best = NULL;
    for (size_t i = 0; i < scope->count; ++i) {
        const Annotation *a = &registry.entries[scope->ids[i]];
        for (size_t j = 0; j < a->count; ++j) {
            const AtriOperator *op = &a->ops[j];
            if (op->size <= length && (!best || best->size < op->size) && !memcmp(text, op->text, op->size)) {
                best = op;
            }
        }
    }
    return best;
}
bool atri_activate_annotation(AtriSyntaxScope *scope, AtriSource s, AtriSpan name, AtriDiagnostic *d) {
    for (size_t i = 0; i < registry.count; ++i) {
        const Annotation *a = &registry.entries[i];
        if (name.end - name.start != strlen(a->name) || memcmp(s.text + name.start, a->name, strlen(a->name))) {
            continue;
        }
        for (size_t k = 0; k < scope->count; ++k) {
            const Annotation *other = &registry.entries[scope->ids[k]];
            if (a == other) {
                return true;
            }
            for (size_t j = 0; j < a->count; ++j) {
                for (size_t n = 0; n < other->count; ++n) {
                    if (!strcmp(a->ops[j].text, other->ops[n].text)) {
                        *d = (AtriDiagnostic){.span = name,
                                              .message = "conflicting operator definitions in this declaration"};
                        snprintf(d->detail, sizeof(d->detail), "%s (@%s, @%s)", a->ops[j].text, other->name,
                                 a->name);
                        return false;
                    }
                }
            }
        }
        if (!atri_append((void **)&scope->ids, &scope->count, &scope->capacity, sizeof(i), &i)) {
            *d = (AtriDiagnostic){.span = name, .message = "out of memory enabling annotation"};
            return false;
        }
        break;
    }
    return true;
}
