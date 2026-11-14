/*
 * Parser/ParsePreprocessor.c
 *
 * Preprocessor directive recognition.
 */

#include "Core/Common.h"
#include "Parser/ParserInternal.h"
#include "Core/Memory.h"
#include "Ir/Ir.h"

const char *pp_keyword(const char *s) {
    const char *p = s;
    static char kw[64];
    size_t n = 0;
    while (*p && *p != '#') p++;
    if (*p == '#') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && !isspace((unsigned char)*p) && n + 1 < sizeof(kw)) {
        kw[n++] = *p++;
    }
    kw[n] = 0;
    return kw;
}

static NodeKind pp_kind(const char *s) {
    const char *p = pp_keyword(s);
    if (strcmp(p, "include") == 0 || strcmp(p, "include_next") == 0) return N_INCLUDE;
    if (strcmp(p, "define") == 0) return N_DEFINE;
    if (strcmp(p, "undef") == 0) return N_UNDEF;
    if (strcmp(p, "ifdef") == 0) return N_IFDEF;
    if (strcmp(p, "ifndef") == 0) return N_IFNDEF;
    if (strcmp(p, "if") == 0) return N_IF;
    if (strcmp(p, "elif") == 0 || strcmp(p, "elifdef") == 0 || strcmp(p, "elifndef") == 0) return N_ELIF;
    if (strcmp(p, "else") == 0) return N_ELSE;
    if (strcmp(p, "endif") == 0) return N_ENDIF;
    if (strcmp(p, "pragma") == 0) return N_PRAGMA;
    if (strcmp(p, "error") == 0) return N_ERROR_DIRECTIVE;
    if (strcmp(p, "warning") == 0) return N_WARNING_DIRECTIVE;
    if (strcmp(p, "module") == 0) return N_MODULE;
    if (strcmp(p, "import") == 0) return N_IMPORT;
    if (strcmp(p, "export") == 0) return N_EXPORT;
    return N_PREPROCESSOR;
}

void parse_preprocessor(Parser *p, size_t i, int64_t parent) {
    int64_t n = ir_add_node(p->ir, pp_kind(pt(p, i)), i, i, parent);
    const char *raw = pt(p, i);
    const char *kw = pp_keyword(raw);
    node_set_value(p->ir, n, raw);
    if (strcmp(kw, "include") == 0 || strcmp(kw, "include_next") == 0 || strcmp(kw, "import") == 0) {
        const char *q = raw;
        while (*q && *q != '#') q++;
        if (*q == '#') q++;
        while (*q && isspace((unsigned char)*q)) q++;
        while (*q && !isspace((unsigned char)*q)) q++;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == '"') {
            const char *e = strchr(q + 1, '"');
            if (e) node_set_name_range_owned(p->ir, n, xstrndup0(q + 1, (size_t)(e - (q + 1))));
        } else if (*q == '<') {
            const char *e = strchr(q + 1, '>');
            if (e) node_set_name_range_owned(p->ir, n, xstrndup0(q + 1, (size_t)(e - (q + 1))));
        } else if (*q) {
            const char *e = q;
            while (*e && !isspace((unsigned char)*e)) e++;
            node_set_name_range_owned(p->ir, n, xstrndup0(q, (size_t)(e - q)));
        }
        if (strcmp(kw, "include_next") == 0) p->ir->nodes[n].flags |= NF_INCLUDE_NEXT;
    } else if (strcmp(kw, "define") == 0) {
        const char *q = raw;
        while (*q && *q != '#') q++;
        if (*q) q++;
        while (*q && isspace((unsigned char)*q)) q++;
        q += 6;
        while (*q && isspace((unsigned char)*q)) q++;
        {
            const char *name_start = q;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
            if (q > name_start) node_set_name_range_owned(p->ir, n, xstrndup0(name_start, (size_t)(q - name_start)));
        }
    }
    ir_add_child(p->ir, parent, n);
}
