/*
 * Ir/IrSerialize.c
 *
 * Binary CPIR (de)serialization.
 */

#include "Core/Common.h"
#include "Ir/IrSerialize.h"
#include "Core/Memory.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"

static void w_u32(FILE *f, uint32_t x) { if (fwrite(&x, sizeof(x), 1, f) != 1) die("write error"); }

static void w_u64(FILE *f, uint64_t x) { if (fwrite(&x, sizeof(x), 1, f) != 1) die("write error"); }

static void r_u32(FILE *f, uint32_t *x) { if (fread(x, sizeof(*x), 1, f) != 1) die("invalid CPIR"); }

static void r_u64(FILE *f, uint64_t *x) { if (fread(x, sizeof(*x), 1, f) != 1) die("invalid CPIR"); }

static void w_blob(FILE *f, const void *p, uint64_t n) {
    w_u64(f, n);
    if (n && fwrite(p, 1, (size_t)n, f) != (size_t)n) die("write error");
}

static uint8_t *r_blob(FILE *f, uint64_t *n_out) {
    uint64_t n;
    uint8_t *p;
    r_u64(f, &n);
    p = (uint8_t *)xmalloc((size_t)n + 1);
    if (n && fread(p, 1, (size_t)n, f) != (size_t)n) die("invalid CPIR");
    p[n] = 0;
    if (n_out) *n_out = n;
    return p;
}

static void w_str(FILE *f, const char *s) { w_blob(f, s ? s : "", s ? strlen(s) : 0); }

static char *r_str(FILE *f) {
    uint64_t n;
    uint8_t *p = r_blob(f, &n);
    return (char *)p;
}

void save_ir(const IR *ir, const char *path) {
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f) die("cannot create '%s': %s", path, strerror(errno));
    if (fwrite(CPIR_MAGIC, 1, 4, f) != 4) die("write error");
    w_u32(f, CPIR_VERSION);
    w_u32(f, 0x01020304u);
    w_blob(f, ir->source, ir->source_size);
    w_u64(f, (uint64_t)ir->tokens.n);
    for (i = 0; i < ir->tokens.n; ++i) {
        const Token *t = &ir->tokens.v[i];
        w_u32(f, t->kind);
        w_u32(f, t->line);
        w_u32(f, t->column);
        w_u64(f, t->byte_start);
        w_u64(f, t->byte_end);
        w_str(f, t->trivia);
        w_str(f, t->text);
    }
    w_u64(f, (uint64_t)ir->node_count);
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        w_u32(f, (uint32_t)n->kind);
        w_u32(f, n->flags);
        w_u64(f, n->first_tok);
        w_u64(f, n->last_tok);
        w_u64(f, (uint64_t)n->parent);
        w_u64(f, (uint64_t)n->first_child);
        w_u64(f, (uint64_t)n->last_child);
        w_u64(f, (uint64_t)n->next_sibling);
        w_u64(f, (uint64_t)n->prev_sibling);
        w_str(f, n->name);
        w_str(f, n->qualified);
        w_str(f, n->type);
        w_str(f, n->return_type);
        w_str(f, n->operator_spelling);
        w_str(f, n->value);
        w_str(f, n->resolved_name);
        w_u64(f, n->aux);
    }
    if (fclose(f) != 0) die("cannot close '%s'", path);
}

static int64_t rd_i64(uint64_t x) { return (int64_t)x; }

void load_ir(IR *ir, const char *path) {
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ver, endian;
    uint64_t n, i;
    if (!f) die("cannot open '%s': %s", path, strerror(errno));
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, CPIR_MAGIC, 4) != 0) die("'%s' is not CPIR", path);
    r_u32(f, &ver);
    r_u32(f, &endian);
    if (ver != CPIR_VERSION) die("unsupported CPIR version %u", ver);
    if (endian != 0x01020304u) die("unsupported CPIR byte order");
    ir->source = r_blob(f, &n);
    ir->source_size = (size_t)n;
    r_u64(f, &n);
    ir->tokens.v = (Token *)xcalloc((size_t)n, sizeof(*ir->tokens.v));
    ir->tokens.n = ir->tokens.cap = (size_t)n;
    for (i = 0; i < n; ++i) {
        Token *t = &ir->tokens.v[i];
        r_u32(f, &t->kind); r_u32(f, &t->line); r_u32(f, &t->column);
        r_u64(f, &t->byte_start); r_u64(f, &t->byte_end);
        t->trivia = r_str(f); t->text = r_str(f);
    }
    r_u64(f, &n);
    ir->nodes = (IRNode *)xcalloc((size_t)n, sizeof(*ir->nodes));
    ir->node_count = ir->node_cap = (size_t)n;
    for (i = 0; i < n; ++i) {
        IRNode *x = &ir->nodes[i];
        uint64_t q;
        uint32_t k;
        r_u32(f, &k); x->kind = (NodeKind)k;
        r_u32(f, &x->flags);
        r_u64(f, &x->first_tok); r_u64(f, &x->last_tok);
        r_u64(f, &q); x->parent = rd_i64(q);
        r_u64(f, &q); x->first_child = rd_i64(q);
        r_u64(f, &q); x->last_child = rd_i64(q);
        r_u64(f, &q); x->next_sibling = rd_i64(q);
        r_u64(f, &q); x->prev_sibling = rd_i64(q);
        x->name = r_str(f); x->qualified = r_str(f); x->type = r_str(f); x->return_type = r_str(f);
        x->operator_spelling = r_str(f); x->value = r_str(f); x->resolved_name = r_str(f);
        r_u64(f, &x->aux);
    }
    fclose(f);
}
