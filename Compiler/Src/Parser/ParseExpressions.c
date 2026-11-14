/*
 * Parser/ParseExpressions.c
 *
 * Expression recognition: calls, named arguments, member access, properties.
 */

#include "Core/Common.h"
#include "Parser/ParserInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"
#include "Sema/TypeLookup.h"

int looks_like_assignment(Parser *p, size_t a, size_t b, size_t *op_out) {
    int par = 0, br = 0, cur = 0;
    size_t i;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(")) par++;
        else if (str_eq(t, ")")) { if (par) par--; }
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]")) { if (br) br--; }
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}")) { if (cur) cur--; }
        else if ((par | br | cur) == 0 &&
                 (str_eq(t, "=") || str_eq(t, "+=") || str_eq(t, "-=") || str_eq(t, "*=") ||
                  str_eq(t, "/=") || str_eq(t, "%=") || str_eq(t, "&=") || str_eq(t, "|=") ||
                  str_eq(t, "^=") || str_eq(t, "<<=") || str_eq(t, ">>="))) {
            if (op_out) *op_out = i;
            return 1;
        }
    }
    return 0;
}

size_t find_top_call_paren(Parser *p, size_t a, size_t b) {
    int angle = 0, bracket = 0;
    size_t i;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (str_eq(t, "[")) bracket++;
        else if (str_eq(t, "]") && bracket) bracket--;
        else if (str_eq(t, "(") && angle == 0 && bracket == 0) return i;
    }
    return SIZE_MAX;
}

static int find_top_level_token(Parser *p, size_t a, size_t b, const char *want, size_t *out) {
    int par = 0, br = 0, cur = 0, angle = 0;
    size_t i;
    if (a > b) return 0;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(") ) par++;
        else if (str_eq(t, ")")) { if (par) par--; }
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]")) { if (br) br--; }
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}")) { if (cur) cur--; }
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (par == 0 && br == 0 && cur == 0 && angle == 0 && str_eq(t, want)) {
            if (out) *out = i;
            return 1;
        }
    }
    return 0;
}

static int call_arg_is_named(Parser *p, size_t a, size_t b, size_t *eq_out) {
    size_t eq;
    if (a > b || !is_identifier(p, a)) return 0;
    if (find_top_level_token(p, a, b, "=", &eq)) {
        if (eq == a || eq != a + 1) return 0;
        if (eq_out) *eq_out = eq;
        return 1;
    }
    return 0;
}

static void parse_call_arguments(Parser *p, int64_t call, size_t a, size_t b) {
    size_t i = a, start = a;
    int par = 0, br = 0, cur = 0, angle = 0;
    while (i <= b) {
        const char *t = pt(p, i);
        if (str_eq(t, "(")) par++;
        else if (str_eq(t, ")")) { if (par) par--; }
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]")) { if (br) br--; }
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}")) { if (cur) cur--; }
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;

        if ((str_eq(t, ",") && par == 0 && br == 0 && cur == 0 && angle == 0) || i == b) {
            size_t e = (i == b ? i : i - 1);
            if (start <= e) {
                size_t eq = SIZE_MAX;
                int named = call_arg_is_named(p, start, e, &eq);
                int64_t an = ir_add_node(p->ir, named ? N_NAMED_ARG : N_CALL_ARG, start, e, call);
                p->ir->nodes[an].aux = UINT64_MAX;
                if (named) {
                    node_set_name(p->ir, an, pt(p, start));
                    node_set_qualified(p->ir, an, pt(p, start));
                    {
                        char *av = token_range_text(p, eq + 1, e);
                        Str clean;
                        str_init(&clean);
                        str_put(&clean, av);
                        str_trim_ascii(&clean);
                        node_set_value(p->ir, an, clean.data ? clean.data : "");
                        str_free(&clean);
                        free(av);
                    }
                } else {
                    node_set_value_range(p, an, start, e);
                }
                ir_add_child(p->ir, call, an);
                /* Arguments are full expressions: nested calls (named or not),
                   property accesses, factories... must be visible too. */
                if (named) { if (eq + 1 <= e) add_expr_nodes(p, an, eq + 1, e); }
                else add_expr_nodes(p, an, start, e);
            }
            start = i + 1;
        }
        i++;
    }
}

static int find_property_for_parser_member(Parser *p, size_t sep_tok, const char *member, int64_t *out_id) {
    const char *receiver;
    if (!p || sep_tok == 0 || sep_tok >= p->ir->tokens.n || !member) return 0;
    receiver = p->ir->tokens.v[sep_tok - 1].text;
    return resolve_property_for_member_ir(p->ir, sep_tok, receiver, member, out_id);
}

void add_expr_nodes(Parser *p, int64_t parent, size_t a, size_t b) {
    IR *ir = p->ir;
    size_t i;
    if (a > b || b >= ir->tokens.n) return;

    /* Assignment nodes describe one expression/statement range. This helper
       is also called for an entire compound function body, so creating one
       node merely because an `=` exists somewhere in that body produces a
       span over all following statements. The emitter then sees that broad
       native node and skips unrelated constructs. Only create the node when
       the range contains at most one top-level statement terminator and no
       top-level block boundary. */
    {
        size_t op, j;
        int top_semis = 0, top_blocks = 0;
        int par = 0, br = 0, cur = 0;
        for (j = a; j <= b; ++j) {
            const char *jt = pt(p, j);
            if (str_eq(jt, "(")) par++;
            else if (str_eq(jt, ")") && par) par--;
            else if (str_eq(jt, "[")) br++;
            else if (str_eq(jt, "]") && br) br--;
            else if (str_eq(jt, "{")) { if (!par && !br) top_blocks++; cur++; }
            else if (str_eq(jt, "}") && cur) { cur--; }
            else if (str_eq(jt, ";") && !par && !br && !cur) top_semis++;
        }
        if (!top_blocks && top_semis <= 1 && looks_like_assignment(p, a, b, &op)) {
            int64_t n = ir_add_node(ir, N_ASSIGN_EXPR, a, b, parent);
            ir->nodes[n].flags = 1;
            node_set_name(ir, n, pt(p, op));
            node_set_value_range(p, n, a, b);
            ir_add_child(ir, parent, n);
        }
    }

    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "match")) {
            size_t brace = SIZE_MAX, close = SIZE_MAX, j;
            int mpar = 0, mbr = 0, mangle = 0;
            for (j = i + 1; j <= b; ++j) {
                const char *mt = pt(p, j);
                if (str_eq(mt, "(")) mpar++;
                else if (str_eq(mt, ")") && mpar) mpar--;
                else if (str_eq(mt, "[")) mbr++;
                else if (str_eq(mt, "]") && mbr) mbr--;
                else if (str_eq(mt, "<")) mangle++;
                else if (str_eq(mt, ">") && mangle) mangle--;
                else if (str_eq(mt, "{") && !mpar && !mbr && !mangle) { brace = j; break; }
            }
            if (brace != SIZE_MAX) {
                close = find_matching(p, brace, b + 1);
                if (close <= b) {
                    int64_t mn = ir_add_node(ir, N_MATCH_STMT, i, close, parent);
                    node_set_value_range(p, mn, i, close);
                    ir->nodes[mn].aux = brace;
                    ir_add_child(ir, parent, mn);
                    i = close;
                    continue;
                }
            }
            ir_die_at(ir, i, "invalid match statement");
        } else if (str_eq(t, "this")) {
            int64_t n = ir_add_node(ir, N_THIS_EXPR, i, i, parent);
            node_set_name(ir, n, "this");
            ir_add_child(ir, parent, n);
        } else if (pk(p, i) == TK_NUMBER || pk(p, i) == TK_STRING || pk(p, i) == TK_CHAR || str_eq(t, "true") || str_eq(t, "false") || str_eq(t, "nullptr")) {
            int64_t n = ir_add_node(ir, N_LITERAL, i, i, parent);
            node_set_value(ir, n, t);
            ir_add_child(ir, parent, n);
        } else if ((str_eq(t, ".") || str_eq(t, "->")) && i + 1 <= b && pk(p, i + 1) == TK_IDENTIFIER) {
            int64_t prop = -1;
            if (i > a && find_property_for_parser_member(p, i, pt(p, i + 1), &prop)) {
                int64_t n = ir_add_node(ir, N_PROPERTY_ACCESS, i - 1, i + 1, parent);
                node_set_name(ir, n, pt(p, i + 1));
                node_set_qualified(ir, n, ir->nodes[prop].qualified);
                ir->nodes[n].aux = (uint64_t)prop;
                node_set_value_range(p, n, i - 1, i + 1);
                ir_add_child(ir, parent, n);
                /* Postfix property mutation is a distinct expression. Keep the
                   full `receiver.member++/--` span so the backend can lower it
                   without ever producing `get_member()++`. */
                if (i + 2 <= b && (str_eq(pt(p, i + 2), "++") || str_eq(pt(p, i + 2), "--"))) {
                    int64_t un = ir_add_node(ir, N_UNARY_EXPR, i - 1, i + 2, parent);
                    node_set_name(ir, un, pt(p, i + 2));
                    node_set_value_range(p, un, i - 1, i + 2);
                    ir->nodes[un].aux = (uint64_t)prop;
                    ir_add_child(ir, parent, un);
                }
            }
        } else if ((str_eq(t, "++") || str_eq(t, "--")) && i + 3 <= b &&
                   is_identifier(p, i + 1) && (str_eq(pt(p, i + 2), ".") || str_eq(pt(p, i + 2), "->")) &&
                   pk(p, i + 3) == TK_IDENTIFIER) {
            int64_t prop = -1;
            if (find_property_for_parser_member(p, i + 2, pt(p, i + 3), &prop)) {
                int64_t un = ir_add_node(ir, N_UNARY_EXPR, i, i + 3, parent);
                node_set_name(ir, un, t);
                node_set_value_range(p, un, i, i + 3);
                ir->nodes[un].aux = (uint64_t)prop;
                ir_add_child(ir, parent, un);
                i += 3;
                continue;
            }
        } else if (is_identifier(p, i)) {
            /* CXXE native member intrinsic: object.get_member("name").
               Represent the whole intrinsic call so the backend can replace it
               with object->name while preserving an optional outer invocation. */
            if (i + 3 <= b && (str_eq(pt(p, i + 1), ".") || str_eq(pt(p, i + 1), "->")) &&
                str_eq(pt(p, i + 2), "get_member") && str_eq(pt(p, i + 3), "(")) {
                size_t close = find_matching(p, i + 3, b + 1);
                if (close <= b) {
                    int64_t n = ir_add_node(ir, N_FUNC_CALL, i, close, parent);
                    node_set_name(ir, n, "get_member");
                    node_set_qualified(ir, n, "get_member");
                    node_set_value_range(p, n, i, close);
                    if (i + 4 <= close - 1) parse_call_arguments(p, n, i + 4, close - 1);
                    ir_add_child(ir, parent, n);
                    i = close;
                    continue;
                }
            }
            size_t j = i;
            /* qualified identifier a :: b :: c */
            while (j + 2 <= b && str_eq(pt(p, j + 1), "::") && is_word_tok(p, j + 2)) j += 2;
            if (j + 1 <= b && (str_eq(pt(p, j + 1), "(") || str_eq(pt(p, j + 1), "<"))) {
                size_t open = SIZE_MAX;
                if (str_eq(pt(p, j + 1), "(")) open = j + 1;
                else if (str_eq(pt(p, j + 1), "<")) {
                    size_t q = find_matching(p, j + 1, b + 1);
                    if (q + 1 <= b && str_eq(pt(p, q + 1), "(")) open = q + 1;
                }
                if (open != SIZE_MAX) {
                    size_t close = find_matching(p, open, b + 1);
                    if (close <= b) {
                        int64_t n = ir_add_node(ir, N_FUNC_CALL, i, close, parent);
                        char *qname = join_qualified(p, i, j);
                        char *resolved = scope_qualified(p, qname);
                        node_set_name(ir, n, pt(p, j));
                        node_set_qualified(ir, n, qname);
                        node_set_resolved(ir, n, resolved);
                        node_set_value_range(p, n, i, close);
                        if (open + 1 <= close - 1) parse_call_arguments(p, n, open + 1, close - 1);
                        ir_add_child(ir, parent, n);
                        free(qname);
                        free(resolved);
                        i = close;
                        continue;
                    }
                }
            }
            {
                int64_t n = ir_add_node(ir, N_VAR_REF, i, j, parent);
                char *qname = join_qualified(p, i, j);
                node_set_name(ir, n, pt(p, j));
                node_set_qualified(ir, n, qname);
                node_set_resolved_owned_scope(p, n, qname);
                ir_add_child(ir, parent, n);
                free(qname);
            }
        }

        if (str_eq(t, ".") || str_eq(t, "->") || str_eq(t, "::")) {
            if (i + 1 <= b && (is_word_tok(p, i + 1) || str_eq(pt(p, i + 1), "operator"))) {
                int64_t n = ir_add_node(ir, N_MEMBER_ACCESS, i, i + 1, parent);
                node_set_name(ir, n, pt(p, i + 1));
                node_set_value(ir, n, t);
                ir_add_child(ir, parent, n);
            }
        }

        if (str_eq(t, "[") && i > a) {
            size_t close = find_matching(p, i, b + 1);
            if (close <= b) {
                int64_t n = ir_add_node(ir, N_INDEX_EXPR, i, close, parent);
                node_set_value_range(p, n, i, close);
                ir_add_child(ir, parent, n);
                if (i + 1 <= close - 1) add_expr_nodes(p, n, i + 1, close - 1);
                i = close;
            }
        }

        if (str_eq(t, "new")) {
            int64_t n = ir_add_node(ir, N_NEW_EXPR, i, b, parent);
            node_set_value_range(p, n, i, b);
            ir_add_child(ir, parent, n);
        } else if (str_eq(t, "delete")) {
            int64_t n = ir_add_node(ir, N_DELETE_EXPR, i, b, parent);
            node_set_value_range(p, n, i, b);
            ir_add_child(ir, parent, n);
        }
    }
}
