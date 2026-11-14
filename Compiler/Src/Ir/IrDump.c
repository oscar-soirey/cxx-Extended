/*
 * Ir/IrDump.c
 *
 * Human-readable CPIR inspection (dump command).
 */

#include "Core/Common.h"
#include "Ir/IrDump.h"
#include "Ir/Ir.h"
#include "Ir/IrSerialize.h"

static void dump_tree(const IR *ir, int64_t id, int depth) {
    size_t i;
    const IRNode *n = &ir->nodes[id];
    for (i = 0; i < (size_t)depth; ++i) fputs("  ", stdout);
    printf("%s [%" PRIu64 "..%" PRIu64 "]", node_kind_name(n->kind), n->first_tok, n->last_tok);
    if (n->name && *n->name) printf(" name=%s", n->name);
    if (n->qualified && *n->qualified) printf(" qualified=%s", n->qualified);
    if (n->type && *n->type) printf(" type=%s", n->type);
    if (n->return_type && *n->return_type) printf(" return=%s", n->return_type);
    if (n->resolved_name && *n->resolved_name) printf(" resolved=%s", n->resolved_name);
    if (n->kind == N_DECORATOR && n->aux != UINT64_MAX) printf(" target=%" PRIu64, n->aux);
    if (n->flags & NF_DECORATOR_CUSTOM) printf(" custom");
    if (n->flags & NF_REGISTERED) printf(" registered");
    if (n->flags & NF_EXPOSED) printf(" exposed");
    if ((n->flags & NF_ACCESS_MASK) == NF_ACCESS_PUBLIC && (n->kind == N_FIELD_DECL || n->kind == N_METHOD || n->kind == N_FUNCTION)) printf(" access=public");
    else if ((n->flags & NF_ACCESS_MASK) == NF_ACCESS_PROTECTED && (n->kind == N_FIELD_DECL || n->kind == N_METHOD || n->kind == N_FUNCTION)) printf(" access=protected");
    if (n->value && *n->value) {
        size_t len = strlen(n->value);
        printf(" value=");
        if (len > 120) fwrite(n->value, 1, 120, stdout), fputs("...", stdout);
        else fputs(n->value, stdout);
    }
    putchar('\n');
    for (id = n->first_child; id >= 0; id = ir->nodes[id].next_sibling) dump_tree(ir, id, depth + 1);
}

void dump_ir(const IR *ir) {
    printf("CPIR version %u\n", CPIR_VERSION);
    printf("source bytes: %zu\n", ir->source_size);
    printf("tokens: %zu\n", ir->tokens.n);
    printf("nodes: %zu\n", ir->node_count);
    if (ir->node_count) dump_tree(ir, 0, 0);
}
