#ifndef ATRI_META_SYNTAX_H
#define ATRI_META_SYNTAX_H
#include "frontend.h"

typedef struct { const char *text; size_t size; unsigned precedence; bool right; } AtriOperator;
typedef struct { size_t *ids, count, capacity; } AtriSyntaxScope;
bool atri_activate_annotation(AtriSyntaxScope *scope, AtriSource source, AtriSpan name, AtriDiagnostic *d);
const AtriOperator *atri_match_operator(const AtriSyntaxScope *scope, const char *text, size_t length);
void atri_syntax_begin(void);
bool atri_syntax_end(void);
void atri_syntax_destroy(void);
#endif
