/*
 * Parser/ParseClasses.c
 *
 * Classes, structs, unions, enums and CXXE properties.
 */

#include "Core/Common.h"
#include "Parser/ParserInternal.h"
#include "Core/Memory.h"
#include "Ir/Ir.h"
#include "Sema/TypeLookup.h"

static void mark_access(IR *ir, int64_t node, int access) {
    ir->nodes[node].flags = (ir->nodes[node].flags & ~NF_ACCESS_MASK) | (uint32_t)access;
}

int64_t property_child_kind(const IR *ir, const IRNode *prop, NodeKind kind) {
    int64_t c;
    for (c = prop->first_child; c >= 0; c = ir->nodes[c].next_sibling)
        if (ir->nodes[c].kind == kind) return c;
    return -1;
}

int property_has_get(const IR *ir, const IRNode *prop) { return property_child_kind(ir, prop, N_PROPERTY_GET) >= 0; }

int property_has_set(const IR *ir, const IRNode *prop) { return property_child_kind(ir, prop, N_PROPERTY_SET) >= 0; }

static int parse_property(Parser *p, size_t a, size_t b, int64_t cls, int access, size_t *end_out) {
    size_t name_i, brace, close, i;
    if (a > b || !str_eq(pt(p, a), "property") || a + 1 > b || !is_identifier(p, a + 1)) return 0;
    name_i = a + 1;
    brace = name_i + 1;
    if (brace > b || !str_eq(pt(p, brace), "{")) return 0;
    close = find_matching(p, brace, b + 1);
    if (close > b) return 0;
    {
        int64_t prop = ir_add_node(p->ir, N_PROPERTY, a, close, cls);
        node_set_name(p->ir, prop, pt(p, name_i));
        node_set_qualified_scope(p, prop, pt(p, name_i));
        node_set_value_range(p, prop, a, close);
        mark_access(p->ir, prop, access);
        ir_add_child(p->ir, cls, prop);
        for (i = brace + 1; i < close; ) {
            if ((str_eq(pt(p, i), "get") || str_eq(pt(p, i), "set")) && i + 1 < close && str_eq(pt(p, i + 1), "{")) {
                size_t oc = find_matching(p, i + 1, close + 1);
                if (oc <= close) {
                    NodeKind nk = str_eq(pt(p, i), "get") ? N_PROPERTY_GET : N_PROPERTY_SET;
                    int64_t child = ir_add_node(p->ir, nk, i, oc, prop);
                    node_set_name(p->ir, child, pt(p, i));
                    node_set_value_range(p, child, i, oc);
                    ir_add_child(p->ir, prop, child);
                    i = oc + 1;
                    continue;
                }
            }
            i++;
        }
        if (!property_has_get(p->ir, &p->ir->nodes[prop]) && !property_has_set(p->ir, &p->ir->nodes[prop]))
            die("property '%s' must contain at least a get or set block", pt(p, name_i));
        parser_apply_pending(p, prop);
        if (end_out) *end_out = close;
        return 1;
    }
}

static int find_field_in_class_by_name(const IR *ir, int64_t cls, const char *name, int64_t *out_id) {
    int64_t c;
    if (cls < 0 || (size_t)cls >= ir->node_count || !name || !*name) return 0;
    for (c = ir->nodes[cls].first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *n = &ir->nodes[c];
        if (n->kind == N_FIELD_DECL && n->name && strcmp(n->name, name) == 0) {
            if (out_id) *out_id = c;
            return 1;
        }
    }
    return 0;
}

static void validate_class_properties(const IR *ir, int64_t cls) {
    int64_t c;
    if (cls < 0 || (size_t)cls >= ir->node_count) return;
    for (c = ir->nodes[cls].first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *prop = &ir->nodes[c];
        int64_t field = -1;
        if (prop->kind != N_PROPERTY || !prop->name || !*prop->name) continue;
        if (!find_field_in_class_by_name(ir, cls, prop->name, &field)) {
            die("CXXE: property '%s' must have a variable with the exact same name in the same type", prop->name);
        }
    }
}

void add_property_assignment_nodes_in_range(Parser *p, int64_t parent, size_t a, size_t b) {
    size_t start=a,i;
    int par=0,br=0,cur=0;
    for(i=a;i<=b;++i){
        const char*t=pt(p,i);
        if(!strcmp(t,"("))par++; else if(!strcmp(t,")")&&par)par--; else if(!strcmp(t,"["))br++; else if(!strcmp(t,"]")&&br)br--; else if(!strcmp(t,"{"))cur++; else if(!strcmp(t,"}")&&cur)cur--;
        if((!par&&!br&&!cur&&strcmp(t,";")==0) || (i==b)){
            size_t end = (i==b && strcmp(t,";")!=0) ? i : i;
            if(start<=end){
                size_t op=SIZE_MAX;
                if(looks_like_assignment(p,start,end,&op) && op>start && is_identifier(p,op-1)){
                    size_t pn=op-1;
                    int64_t pid=-1;
                    if(pn>start && (str_eq(pt(p,pn-1),".")||str_eq(pt(p,pn-1),"->")) &&
                       resolve_property_for_member_ir(p->ir, pn - 1, pt(p, pn - 2), pt(p, pn), &pid)){
                        int64_t n=ir_add_node(p->ir,N_ASSIGN_EXPR,start,end,parent);
                        node_set_name(p->ir,n,pt(p,op));
                        node_set_value_range(p, n, start, end);
                        ir_add_child(p->ir,parent,n);
                    }
                }
            }
            start=i+1;
        }
    }
}

static void parse_class_body(Parser *p, size_t a, size_t b, int64_t cls, const char *class_name) {
    size_t i = a, start = a;
    int access = (p->ir->nodes[cls].kind == N_CLASS) ? NF_ACCESS_PRIVATE : NF_ACCESS_PUBLIC;

    while (i <= b) {
        if (str_eq(pt(p, i), "public") || str_eq(pt(p, i), "protected") || str_eq(pt(p, i), "private")) {
            if (i + 1 <= b && str_eq(pt(p, i + 1), ":") && start == i) {
                if (str_eq(pt(p, i), "public")) access = NF_ACCESS_PUBLIC;
                else if (str_eq(pt(p, i), "protected")) access = NF_ACCESS_PROTECTED;
                else access = NF_ACCESS_PRIVATE;
                i += 2;
                start = i;
                continue;
            }
        }

        if (str_eq(pt(p, i), "@")) {
            if (start < i) {
                /* An incomplete previous declaration: keep it losslessly. */
                int64_t raw = ir_add_node(p->ir, N_RAW, start, i - 1, cls);
                node_set_value_range(p, raw, start, i - 1);
                mark_access(p->ir, raw, access);
                ir_add_child(p->ir, cls, raw);
                parser_apply_pending(p, raw);
            }
            parse_decorator_sequence(p, &i, b, cls);
            start = i;
            continue;
        }

        if (str_eq(pt(p, i), "property") && start == i) {
            size_t property_end = SIZE_MAX;
            if (parse_property(p, i, b, cls, access, &property_end)) {
                i = property_end + 1;
                start = i;
                continue;
            }
        }

        if (str_eq(pt(p, i), ";")) {
            if (start <= i) {
                size_t open = find_top_call_paren(p, start, i);
                int64_t n;
                n = -1;
                if (open != SIZE_MAX && parse_function_like(p, start, i, cls, class_name, open, &n)) {
                    if (n >= 0) mark_access(p->ir, n, access);
                    if (n >= 0) parser_apply_pending(p, n);
                } else {
                    n = ir_add_node(p->ir, N_FIELD_DECL, start, i, cls);
                    infer_decl_name_type(p, start, i, n);
                    mark_access(p->ir, n, access);
                    ir_add_child(p->ir, cls, n);
                    parser_apply_pending(p, n);
                }
            }
            start = i + 1;
        } else if (str_eq(pt(p, i), "{") && start < i) {
            /* Function definition: parse_function_like knows how to consume its body. */
            size_t open_paren = find_top_call_paren(p, start, i);
            if (open_paren != SIZE_MAX) {
                size_t e = find_stmt_end(p, start, b + 1);
                /* Consume exactly a member-function body here. The general
                   find_stmt_end() helper intentionally continues through
                   nested braces until a top-level semicolon, but a class
                   body has subsequent members that must remain visible. */
                if (str_eq(pt(p, i), "{")) {
                    size_t body_close = find_matching(p, i, b + 1);
                    if (body_close <= b) e = body_close;
                }
                int64_t n = -1;
                if (parse_function_like(p, start, e, cls, class_name, open_paren, &n) && n >= 0) {
                    mark_access(p->ir, n, access);
                    parser_apply_pending(p, n);
                    i = e;
                    start = e + 1;
                }
            }
        }
        i++;
    }

    if (start <= b) {
        int64_t raw = ir_add_node(p->ir, N_RAW, start, b, cls);
        node_set_value_range(p, raw, start, b);
        mark_access(p->ir, raw, access);
        ir_add_child(p->ir, cls, raw);
        parser_apply_pending(p, raw);
    }
}

void parse_class(Parser *p, size_t a, size_t b, int64_t parent, NodeKind kind) {
    size_t i = a + 1, name_i = SIZE_MAX, brace = SIZE_MAX, close = SIZE_MAX;
    char *name = NULL, *qname = NULL;
    while (i <= b && i < p->end) {
        if (is_identifier(p, i) && name_i == SIZE_MAX) name_i = i;
        if (str_eq(pt(p, i), "{")) { brace = i; break; }
        i++;
    }
    if (brace == SIZE_MAX) {
        int64_t n = ir_add_node(p->ir, kind, a, b, parent);
        mark_access(p->ir, n, kind == N_CLASS ? NF_ACCESS_PRIVATE : NF_ACCESS_PUBLIC);
        node_set_value_range(p, n, a, b);
        if (name_i != SIZE_MAX) {
            name = xstrdup0(pt(p, name_i));
            qname = scope_qualified(p, name);
            node_set_name(p->ir, n, name);
            node_set_qualified(p->ir, n, qname);
        }
        ir_add_child(p->ir, parent, n);
        parser_apply_pending(p, n);
        free(name); free(qname);
        return;
    }
    close = find_matching(p, brace, b + 1);
    if (close > b) close = b;
    {
        int64_t n = ir_add_node(p->ir, kind, a, close, parent);
        mark_access(p->ir, n, kind == N_CLASS ? NF_ACCESS_PRIVATE : NF_ACCESS_PUBLIC);
        if (name_i != SIZE_MAX) {
            name = xstrdup0(pt(p, name_i));
            qname = scope_qualified(p, name);
            node_set_name(p->ir, n, name);
            node_set_qualified(p->ir, n, qname);
            scope_push(p, name);
        }
        node_set_value_range(p, n, a, close);
        ir_add_child(p->ir, parent, n);
        parser_apply_pending(p, n);
        if (name_i != SIZE_MAX) {
            parse_class_body(p, brace + 1, close ? close - 1 : close, n, name);
            validate_class_properties(p->ir, n);
        }
        if (name_i != SIZE_MAX) scope_pop(p);
    }
    free(name); free(qname);
}

void parse_enum(Parser *p, size_t a, size_t b, int64_t parent) {
    size_t i = a + 1, brace = SIZE_MAX, name_i = SIZE_MAX;
    while (i <= b) {
        if (is_identifier(p, i) && name_i == SIZE_MAX) name_i = i;
        if (str_eq(pt(p, i), "{")) { brace = i; break; }
        i++;
    }
    if (brace == SIZE_MAX) return;
    size_t close = find_matching(p, brace, b + 1);
    if (close > b) close = b;
    int64_t en = ir_add_node(p->ir, N_ENUM, a, close, parent);
    if (name_i != SIZE_MAX) {
        node_set_name(p->ir, en, pt(p, name_i));
        node_set_qualified_scope(p, en, pt(p, name_i));
    }
    ir_add_child(p->ir, parent, en);
    size_t s = brace + 1;
    for (i = brace + 1; i <= close; ++i) {
        if (str_eq(pt(p, i), ",") || i == close) {
            if (s < i) {
                int64_t e = ir_add_node(p->ir, N_ENUMERATOR, s, i == close ? i - 1 : i - 1, en);
                if (is_identifier(p, s)) node_set_name(p->ir, e, pt(p, s));
                node_set_value_range(p, e, s, i - 1);
                ir_add_child(p->ir, en, e);
            }
            s = i + 1;
        }
    }
}
