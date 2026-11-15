/*
 * Symbols.h
 *
 * What an identifier refers to, where it is declared and where it is used.
 */

#ifndef CXXE_LS_SYMBOLS_H
#define CXXE_LS_SYMBOLS_H

#include "Server.h"

typedef enum {
    SYM_NONE = 0,
    SYM_NODE,              /* IR declaration node (local to `unit` or imported) */
    SYM_LOCAL,             /* local variable found in a function body */
    SYM_NATIVE_DECORATOR   /* @register / @exposed */
} SymKind;

typedef struct {
    SymKind kind;
    LsUnit *unit;          /* unit whose IR holds `node` / the local */
    int64_t node;          /* SYM_NODE */
    int64_t scope;         /* SYM_LOCAL: enclosing callable; SYM_NODE param: its function */
    size_t decl_tok;       /* declaration name token in `unit`, SIZE_MAX if unknown */
    char name[256];
    char type[512];        /* declared type when known */
} LsSymbol;

typedef struct {
    LsUnit *unit;
    size_t tok;
    int is_decl;
} LsOcc;

int is_declaration_kind(NodeKind k);
int is_callable_kind_ls(NodeKind k);
size_t decl_name_token(const IR *ir, int64_t node);
int64_t enclosing_callable(const IR *ir, size_t tok);
/* Class owning a callable (inline method or out-of-line Type::method). */
int64_t callable_owner_class(const IR *ir, int64_t fn);
int64_t find_class_named(const IR *ir, const char *name);

/* Symbol designated by identifier token `tok` of `u`. */
int sym_resolve(LsServer *s, LsUnit *u, size_t tok, LsSymbol *out);
/* Fills unit/decl_tok for imported declarations from the index. */
int sym_locate(LsServer *s, LsSymbol *sym);
size_t sym_occurrences(LsServer *s, const LsSymbol *sym, LsOcc **out);

/* Class of the receiver before `.`/`->` at sep_tok (in `u`). The class may
   live in another unit (out_unit) and be an imported node. */
int receiver_class(LsServer *s, LsUnit *u, size_t sep_tok, LsUnit **out_unit, int64_t *out_cls);
/* Calls cb for every member of cls (and of its bases). */
void class_members(LsServer *s, LsUnit *u, int64_t cls,
                   void (*cb)(void *ctx, LsUnit *u, int64_t member), void *ctx);
/* Local variables visible at `before` inside callable fn: cb(name_tok). */
void list_locals(const LsUnit *u, int64_t fn, size_t before, void (*cb)(void *ctx, size_t name_tok), void *ctx);
/* Declared type text of a local whose name token is name_tok. */
void local_type(const LsUnit *u, size_t name_tok, char *out, size_t cap);

/* Declaration text (signature without body), for hover/signature help. */
char *decl_text(const LsUnit *u, int64_t node);
/* Parameter display text ("int x = 0"). */
char *param_text(const LsUnit *u, int64_t param);

#endif
