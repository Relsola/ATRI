#ifndef ATRI_META_H
#define ATRI_META_H
#include "ir.h"
#include "meta_plugin.h"

typedef struct AtriMetaPlugin AtriMetaPlugin;
AtriMetaPlugin *atri_meta_open(const char *path);
void atri_meta_close(AtriMetaPlugin *plugin);
AtriMetaCallback *atri_meta_callback(AtriMetaPlugin *plugin);
char *atri_canonical_path(const char *path);
char *atri_import_path(const char *parent, const char *text, AtriSpan span);
bool atri_lower_generator(AtriSource source, const AtriProgram *ast, size_t declaration, AtriIRProgram *ir, AtriDiagnostic *d);
typedef struct { const char *name; AtriMetaCallback *callback; } AtriListener;
bool atri_expand_program(AtriSource input, const AtriListener *listeners, size_t count,
                         AtriSource *source, AtriProgram *program, AtriDiagnostic *d);

#endif
