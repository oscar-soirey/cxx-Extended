/*
 * Parser/ParserInternal.h
 *
 * Parser state and helpers shared between parser translation units.
 */

#ifndef CXXE_PARSER_PARSER_INTERNAL_H
#define CXXE_PARSER_PARSER_INTERNAL_H

#include "Core/Common.h"
#include "Ir/Ir.h"

typedef struct {
    IR *ir;
    size_t i;
    size_t end;
    int64_t root;
    char **scope;
    size_t scope_n;
    size_t scope_cap;
    int64_t *pending_decorators;
    size_t pending_decorator_n;
    size_t pending_decorator_cap;
} Parser;

const char *pt(Parser *p, size_t i);
uint32_t pk(Parser *p, size_t i);
int is_word_tok(Parser *p, size_t i);
int is_keyword(const char *s);
int is_identifier(Parser *p, size_t i);
int is_decorator_identifier(Parser *p, size_t i);
size_t find_matching(Parser *p, size_t open_i, size_t end);
size_t find_stmt_end(Parser *p, size_t i, size_t end);
char *token_range_text(Parser *p, size_t a, size_t b);
char *join_qualified(Parser *p, size_t a, size_t b);
char *scope_qualified(Parser *p, const char *name);
void scope_push(Parser *p, const char *name);
void scope_pop(Parser *p);
void node_set_name(IR *ir, int64_t id, const char *s);
void node_set_qualified(IR *ir, int64_t id, const char *s);
void node_set_type(IR *ir, int64_t id, const char *s);
void node_set_return(IR *ir, int64_t id, const char *s);
void node_set_value(IR *ir, int64_t id, const char *s);
void node_set_resolved(IR *ir, int64_t id, const char *s);
void node_set_value_range(Parser *p, int64_t id, size_t a, size_t b);
void node_set_type_range(Parser *p, int64_t id, size_t a, size_t b);
void node_set_return_range(Parser *p, int64_t id, size_t a, size_t b);
void node_set_qualified_scope(Parser *p, int64_t id, const char *name);
void node_set_name_range_owned(IR *ir, int64_t id, char *s);
void node_set_resolved_owned_scope(Parser *p, int64_t id, const char *name);
void parser_apply_pending(Parser *p, int64_t target);
size_t find_template_close_cpp(Parser *p, size_t open_i, size_t end_exclusive);
void parse_custom_decorator_template_decl(Parser *p, int64_t parent, size_t template_i, size_t template_close);
void parse_decorator_sequence(Parser *p, size_t *io_i, size_t b, int64_t parent);
int looks_like_assignment(Parser *p, size_t a, size_t b, size_t *op_out);
size_t find_top_call_paren(Parser *p, size_t a, size_t b);
void add_expr_nodes(Parser *p, int64_t parent, size_t a, size_t b);
NodeKind decl_kind_for_context(int class_context);
void infer_decl_name_type(Parser *p, size_t a, size_t b, int64_t n);
void parse_parameters(Parser *p, int64_t fn, size_t a, size_t b);
int parse_function_like(Parser *p, size_t a, size_t b, int64_t parent, const char *class_name, size_t open_paren, int64_t *out_fn);
void parse_attributes(Parser *p, size_t a, size_t b, int64_t parent);
const char *pp_keyword(const char *s);
void parse_preprocessor(Parser *p, size_t i, int64_t parent);
void parse_simple_statement(Parser *p, size_t a, size_t b, int64_t parent);
int64_t property_child_kind(const IR *ir, const IRNode *prop, NodeKind kind);
int property_has_get(const IR *ir, const IRNode *prop);
int property_has_set(const IR *ir, const IRNode *prop);
void add_property_assignment_nodes_in_range(Parser *p, int64_t parent, size_t a, size_t b);
void parse_class(Parser *p, size_t a, size_t b, int64_t parent, NodeKind kind);
void parse_enum(Parser *p, size_t a, size_t b, int64_t parent);
int looks_like_var_decl(Parser *p, size_t a, size_t b);
int find_top_level_at(Parser *p, size_t a, size_t b, size_t *at_out);
int looks_like_custom_decorator_decl(Parser *p, size_t a, size_t b, int64_t parent, size_t *end_out);
int parse_custom_decorator_decl(Parser *p, size_t a, size_t b, int64_t parent);

#endif /* CXXE_PARSER_PARSER_INTERNAL_H */
