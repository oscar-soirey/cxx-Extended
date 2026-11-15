/*
 * Server.h
 *
 * Language server state. A "unit" is one parsed source file: an open editor
 * document (analysed with the full compiler pipeline) or a workspace file
 * from the index (lexed and parsed only).
 */

#ifndef CXXE_LS_SERVER_H
#define CXXE_LS_SERVER_H

#include "Core/Common.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Json.h"
#include "Text.h"

typedef struct {
    char *path;
    char *uri;        /* exact client URI for open documents */
    IR ir;            /* ir.source holds the unit text */
    LineMap lines;
    int has_ir;
} LsUnit;

typedef struct {
    int severity;     /* 1 error, 2 warning, 3 information */
    size_t start, end;/* byte offsets */
    char *message;
    const char *source;
} LsDiag;

typedef struct {
    LsDiag *v;
    size_t n;
} LsDiagList;

typedef struct {
    LsUnit unit;
    char *text;            /* latest text from the client */
    size_t text_len;
    long version;
    LsDiagList diags;      /* CXXE diagnostics of the last analysis */
    LsDiagList cpp_diags;  /* C++ compiler diagnostics of the last check */
    char *generated;       /* emitted C++ (NULL when the analysis failed) */
    int analysis_failed;
} LsDocument;

typedef struct {
    LsUnit unit;
    long long mtime;
    int seen;              /* mark for directory rescans */
    int external;          /* outside the workspace (reached via includes) */
} LsIndexFile;

typedef struct {
    LsDocument **docs;
    size_t doc_n;
    LsIndexFile **files;
    size_t file_n;
    char *root;            /* workspace folder path (may be NULL) */
    char **include_dirs;
    size_t include_dir_n;
    char *cpp_compiler;    /* "" disables the C++ check */
    char *cpp_standard;
    int cpp_check_on_save;
    char *temp_dir;
    int shutdown_requested;
} LsServer;

/* Diagnostics */
void diag_list_clear(LsDiagList *l);
void diag_add(LsDiagList *l, int severity, size_t start, size_t end, const char *source, const char *fmt, ...);

/* Units */
void unit_free(LsUnit *u);
/* Lex + parse (no includes/semantics), errors ignored. Takes ownership. */
void unit_parse_text(LsUnit *u, const char *path, char *text, size_t len);
size_t unit_token_at(const LsUnit *u, size_t offset);  /* SIZE_MAX if none */
void unit_token_range(const LsUnit *u, size_t tok, size_t *start, size_t *end);
void unit_put_range(Str *out, const LsUnit *u, size_t start, size_t end);
void unit_put_location(Str *out, const LsUnit *u, size_t start, size_t end);
const char *unit_uri(LsUnit *u);

/* Server */
LsDocument *server_doc(LsServer *s, const char *uri);
LsDocument *server_doc_by_path(LsServer *s, const char *path);
void server_settings(LsServer *s, const Json *settings);
const char *const *server_include_dirs(LsServer *s, size_t *n);

/* Analysis.c */
void analyze_document(LsServer *s, LsDocument *d);
void cpp_check_document(LsServer *s, LsDocument *d);
void publish_diagnostics(LsServer *s, LsDocument *d);

/* Index.c */
void index_refresh(LsServer *s);
void index_add_external(LsServer *s, const char *path);
/* Every unit known to the server: open documents first, then indexed files
   that are not open. */
size_t server_units(LsServer *s, LsUnit ***out);

#endif
