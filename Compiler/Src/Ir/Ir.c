/*
 * Ir/Ir.c
 *
 * CPIR in-memory representation: node kinds, flags and tree building.
 */

#include "Core/Common.h"
#include "Ir/Ir.h"
#include "Core/Memory.h"
#include "Lexer/Lexer.h"

void ir_init(IR *ir) { memset(ir, 0, sizeof(*ir)); }

static void ir_report(const IR *ir, uint64_t tok, int severity, const char *fmt, va_list ap) {
    unsigned line = 0, col = 0;
    if (ir && tok < ir->tokens.n) { line = ir->tokens.v[tok].line; col = ir->tokens.v[tok].column; }
    cxxe_diagnostic(severity, ir ? ir->path : NULL, line, col, fmt, ap);
}

void ir_die_at(const IR *ir, uint64_t tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ir_report(ir, tok, CXXE_DIAG_ERROR, fmt, ap);
    va_end(ap);
    exit(1); /* not reached */
}

void ir_warn_at(const IR *ir, uint64_t tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ir_report(ir, tok, CXXE_DIAG_WARNING, fmt, ap);
    va_end(ap);
}

static void ir_free_nodes(IR *ir) {
    size_t i;
    for (i = 0; i < ir->node_count; ++i) {
        free(ir->nodes[i].name);
        free(ir->nodes[i].qualified);
        free(ir->nodes[i].type);
        free(ir->nodes[i].return_type);
        free(ir->nodes[i].operator_spelling);
        free(ir->nodes[i].value);
        free(ir->nodes[i].resolved_name);
    }
    free(ir->nodes);
    ir->nodes = NULL;
    ir->node_count = ir->node_cap = 0;
}

void ir_replace_nodes(IR *dst, IR *src) {
    ir_free_nodes(dst);
    dst->nodes = src->nodes;
    dst->node_count = src->node_count;
    dst->node_cap = src->node_cap;
    src->nodes = NULL;
    src->node_count = src->node_cap = 0;
}

void ir_free(IR *ir) {
    size_t i;
    free(ir->source);
    free(ir->path);
    tokens_free(&ir->tokens);
    for (i = 0; i < ir->node_count; ++i) {
        free(ir->nodes[i].name);
        free(ir->nodes[i].qualified);
        free(ir->nodes[i].type);
        free(ir->nodes[i].return_type);
        free(ir->nodes[i].operator_spelling);
        free(ir->nodes[i].value);
        free(ir->nodes[i].resolved_name);
    }
    free(ir->nodes);
    memset(ir, 0, sizeof(*ir));
}

int64_t ir_add_node(IR *ir, NodeKind kind, uint64_t first, uint64_t last, int64_t parent) {
    IRNode n;
    memset(&n, 0, sizeof(n));
    n.kind = kind;
    n.first_tok = first;
    n.last_tok = last;
    n.parent = parent;
    n.first_child = n.last_child = n.next_sibling = n.prev_sibling = -1;
    if (ir->node_count == ir->node_cap) {
        ir->node_cap = ir->node_cap ? ir->node_cap * 2 : 1024;
        ir->nodes = (IRNode *)xrealloc(ir->nodes, ir->node_cap * sizeof(*ir->nodes));
    }
    ir->nodes[ir->node_count] = n;
    return (int64_t)ir->node_count++;
}

void ir_add_child(IR *ir, int64_t parent, int64_t child) {
    IRNode *p = &ir->nodes[parent];
    IRNode *c = &ir->nodes[child];
    c->parent = parent;
    if (p->last_child >= 0) {
        IRNode *prev = &ir->nodes[p->last_child];
        prev->next_sibling = child;
        c->prev_sibling = p->last_child;
    } else {
        p->first_child = child;
    }
    p->last_child = child;
}

const char *node_kind_name(NodeKind k) {
#define K(x) case x: return #x
    switch (k) {
        K(N_TRANSLATION_UNIT); K(N_RAW); K(N_PREPROCESSOR); K(N_INCLUDE); K(N_DEFINE);
        K(N_UNDEF); K(N_IFDEF); K(N_IFNDEF); K(N_IF); K(N_ELIF); K(N_ELSE); K(N_ENDIF);
        K(N_PRAGMA); K(N_ERROR_DIRECTIVE); K(N_WARNING_DIRECTIVE); K(N_ATTRIBUTE); K(N_DECORATOR);
        K(N_DECORATOR_TEMPLATE); K(N_DECORATOR_TEMPLATE_ARG); K(N_CUSTOM_DECORATOR); K(N_DECORATOR_TARGET);
        K(N_NAMESPACE); K(N_NAMESPACE_ALIAS); K(N_USING_DIRECTIVE); K(N_USING_DECL);
        K(N_LINKAGE_SPEC); K(N_MODULE); K(N_IMPORT); K(N_EXPORT); K(N_CLASS); K(N_STRUCT);
        K(N_UNION); K(N_ENUM); K(N_ENUMERATOR); K(N_TEMPLATE); K(N_TEMPLATE_TYPE_PARAM);
        K(N_TEMPLATE_NON_TYPE_PARAM); K(N_TEMPLATE_TEMPLATE_PARAM); K(N_CONCEPT); K(N_REQUIRES);
        K(N_TYPEDEF); K(N_ALIAS); K(N_FRIEND); K(N_FIELD_DECL); K(N_PROPERTY); K(N_PROPERTY_GET); K(N_PROPERTY_SET); K(N_PROPERTY_ACCESS); K(N_VAR_DECL); K(N_VAR_ASSIGN);
        K(N_FUNCTION); K(N_METHOD); K(N_CONSTRUCTOR); K(N_DESTRUCTOR);
        K(N_OPERATOR_FUNCTION); K(N_PARAM_DECL); K(N_RETURN); K(N_IF_STMT); K(N_FOR_STMT);
        K(N_RANGE_FOR_STMT); K(N_WHILE_STMT); K(N_DO_STMT); K(N_SWITCH_STMT); K(N_MATCH_STMT); K(N_MATCH_CASE); K(N_CASE_STMT);
        K(N_DEFAULT_STMT); K(N_BREAK_STMT); K(N_CONTINUE_STMT); K(N_GOTO_STMT); K(N_LABEL_STMT);
        K(N_TRY_STMT); K(N_CATCH); K(N_THROW); K(N_CO_RETURN); K(N_CO_AWAIT); K(N_CO_YIELD);
        K(N_EXPR); K(N_FUNC_CALL); K(N_CALL_ARG); K(N_NAMED_ARG); K(N_MEMBER_CALL); K(N_GET_MEMBER); K(N_VAR_REF); K(N_MEMBER_ACCESS);
        K(N_INDEX_EXPR); K(N_BINARY_EXPR); K(N_UNARY_EXPR); K(N_TERNARY_EXPR); K(N_ASSIGN_EXPR);
        K(N_CAST_EXPR); K(N_NEW_EXPR); K(N_DELETE_EXPR); K(N_LAMBDA); K(N_THIS_EXPR);
        K(N_INIT_LIST); K(N_LITERAL); K(N_SIZEOF_EXPR); K(N_ALIGNOF_EXPR); K(N_TYPEID_EXPR);
        K(N_NOEXCEPT_EXPR); K(N_TYPE_TRAIT); K(N_UNKNOWN);
    }
#undef K
    return "N_UNKNOWN";
}
