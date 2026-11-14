/*
 * Parser/ParseDecorators.c
 *
 * CXXE native decorators (@decorator<...>) parsing.
 */

#include "Core/Common.h"
#include "Parser/ParserInternal.h"
#include "Core/Memory.h"
#include "Ir/Ir.h"

static void parser_pending_push(Parser *p, int64_t id) {
    if (p->pending_decorator_n == p->pending_decorator_cap) {
        p->pending_decorator_cap = p->pending_decorator_cap ? p->pending_decorator_cap * 2 : 8;
        p->pending_decorators = (int64_t *)xrealloc(p->pending_decorators,
            p->pending_decorator_cap * sizeof(*p->pending_decorators));
    }
    p->pending_decorators[p->pending_decorator_n++] = id;
}

void parser_apply_pending(Parser *p, int64_t target) {
    size_t i;
    for (i = 0; i < p->pending_decorator_n; ++i) {
        int64_t d = p->pending_decorators[i];
        p->ir->nodes[d].aux = (uint64_t)target;
        ir_add_child(p->ir, target, d);
    }
    p->pending_decorator_n = 0;
}

size_t find_template_close_cpp(Parser *p, size_t open_i, size_t end_exclusive) {
    size_t i;
    int depth = 0;
    if (open_i >= p->ir->tokens.n || !str_eq(pt(p, open_i), "<")) return SIZE_MAX;
    for (i = open_i; i < end_exclusive && i < p->ir->tokens.n; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "<")) depth++;
        else if (str_eq(t, ">")) {
            if (depth > 0) depth--;
            if (depth == 0) return i;
        } else if (str_eq(t, ">>")) {
            if (depth <= 2) return i;
            depth -= 2;
        } else if (str_eq(t, ">>>")) {
            if (depth <= 3) return i;
            depth -= 3;
        }
    }
    return SIZE_MAX;
}

static size_t split_template_args_parser(Parser *p, size_t a, size_t b,
                                          size_t *starts, size_t *ends, size_t cap) {
    size_t count = 0, start = a, i;
    int par = 0, br = 0, cur = 0, angle = 0;
    if (a > b) return 0;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(")) par++;
        else if (str_eq(t, ")") && par) par--;
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]") && br) br--;
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}") && cur) cur--;
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (str_eq(t, ">>")) angle -= angle >= 2 ? 2 : angle;
        else if (str_eq(t, ">>>")) angle -= angle >= 3 ? 3 : angle;
        else if (str_eq(t, ",") && par == 0 && br == 0 && cur == 0 && angle == 0) {
            if (start <= i - 1 && count < cap) {
                starts[count] = start;
                ends[count] = i - 1;
                count++;
            }
            start = i + 1;
        }
    }
    if (start <= b && count < cap) {
        starts[count] = start;
        ends[count] = b;
        count++;
    }
    return count;
}

static void parse_decorator_template_args(Parser *p, int64_t parent, size_t open, size_t close) {
    int64_t tpl = ir_add_node(p->ir, N_DECORATOR_TEMPLATE, open, close, parent);
    size_t starts[128], ends[128], n, i;
    node_set_name(p->ir, tpl, "template_args");
    node_set_value_range(p, tpl, open, close);
    ir_add_child(p->ir, parent, tpl);
    if (close <= open + 1) return;
    n = split_template_args_parser(p, open + 1, close - 1, starts, ends, 128);
    for (i = 0; i < n; ++i) {
        int64_t arg = ir_add_node(p->ir, N_DECORATOR_TEMPLATE_ARG, starts[i], ends[i], tpl);
        node_set_value_range(p, arg, starts[i], ends[i]);
        ir_add_child(p->ir, tpl, arg);
    }
}

void parse_custom_decorator_template_decl(Parser *p, int64_t parent,
                                                  size_t template_i, size_t template_close) {
    int64_t tpl = ir_add_node(p->ir, N_TEMPLATE, template_i, template_close, parent);
    size_t open = template_i + 1, starts[128], ends[128], n = 0, i;
    node_set_value_range(p, tpl, template_i, template_close);
    ir_add_child(p->ir, parent, tpl);
    while (open < template_close && !str_eq(pt(p, open), "<")) open++;
    if (open >= template_close || template_close <= open + 1) return;
    n = split_template_args_parser(p, open + 1, template_close - 1, starts, ends, 128);
    for (i = 0; i < n; ++i) {
        size_t aa = starts[i], bb = ends[i], j;
        NodeKind k = N_TEMPLATE_NON_TYPE_PARAM;
        const char *name = NULL;
        if (str_eq(pt(p, aa), "typename") || str_eq(pt(p, aa), "class")) k = N_TEMPLATE_TYPE_PARAM;
        else if (str_eq(pt(p, aa), "template")) k = N_TEMPLATE_TEMPLATE_PARAM;
        for (j = bb + 1; j-- > aa;) {
            if (is_identifier(p, j)) { name = pt(p, j); break; }
            if (j == aa) break;
        }
        {
            int64_t pn = ir_add_node(p->ir, k, aa, bb, tpl);
            if (name) node_set_name(p->ir, pn, name);
            node_set_value_range(p, pn, aa, bb);
            if (k == N_TEMPLATE_TYPE_PARAM) node_set_type(p->ir, pn, pt(p, aa));
            ir_add_child(p->ir, tpl, pn);
        }
    }
}

static size_t parse_one_decorator(Parser *p, size_t a, size_t b) {
    size_t i = a + 1, end, name_end;
    char *spelling = NULL, *qualified = NULL;
    if (i > b || !is_decorator_identifier(p, i)) return a;
    end = i;
    while (end + 2 <= b && str_eq(pt(p, end + 1), "::") && is_decorator_identifier(p, end + 2)) end += 2;
    name_end = end;
    if (end + 1 <= b && str_eq(pt(p, end + 1), "<")) {
        size_t tc = find_template_close_cpp(p, end + 1, b + 1);
        if (tc == SIZE_MAX) return a;
        end = tc;
    }
    if (end + 1 <= b && str_eq(pt(p, end + 1), "(")) {
        size_t pc = find_matching(p, end + 1, b + 1);
        if (pc > b) return a;
        end = pc;
    }
    spelling = join_qualified(p, i, name_end);
    qualified = scope_qualified(p, spelling);
    {
        int64_t n = ir_add_node(p->ir, N_DECORATOR, a, end, -1);
        p->ir->nodes[n].aux = UINT64_MAX;
        node_set_name(p->ir, n, spelling);
        node_set_qualified(p->ir, n, qualified);
        node_set_value_range(p, n, a, end);
        if (name_end + 1 <= end && str_eq(pt(p, name_end + 1), "<")) {
            size_t tc = find_template_close_cpp(p, name_end + 1, end + 1);
            if (tc != SIZE_MAX) parse_decorator_template_args(p, n, name_end + 1, tc);
        }
        free(spelling); free(qualified);
        if (str_eq(p->ir->nodes[n].name, "register") || str_eq(p->ir->nodes[n].name, "exposed")) {
            p->ir->nodes[n].flags |= NF_DECORATOR_NATIVE;
            if (str_eq(p->ir->nodes[n].name, "register")) p->ir->nodes[n].flags |= NF_DECORATOR_REGISTER;
            else p->ir->nodes[n].flags |= NF_DECORATOR_EXPOSED;
        }
        p->ir->nodes[n].parent = -1;
        parser_pending_push(p, n);
    }
    return end;
}

void parse_decorator_sequence(Parser *p, size_t *io_i, size_t b, int64_t parent) {
    size_t i = *io_i;
    (void)parent;
    while (i <= b && str_eq(pt(p, i), "@")) {
        size_t end = parse_one_decorator(p, i, b);
        if (end < i) break;
        i = end + 1;
    }
    *io_i = i;
}
