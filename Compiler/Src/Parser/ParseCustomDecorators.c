/*
 * Parser/ParseCustomDecorators.c
 *
 * User-defined decorator declarations.
 */

#include "Core/Common.h"
#include "Parser/ParserInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"

int looks_like_custom_decorator_decl(Parser *p, size_t a, size_t b, int64_t parent, size_t *end_out) {
    size_t at = SIZE_MAX, name_i, params_open, params_close, func_i, target_open, target_close, body_open, body_close;
    size_t i;
    int par = 0, br = 0, cur = 0, angle = 0;
    (void)parent;
    if (a > b) return 0;
    /* Locate a top-level @ before the statement ends. */
    if (!find_top_level_at(p, a, b, &at) || at <= a || at + 1 > b) return 0;
    name_i = at + 1;
    if (!is_decorator_identifier(p, name_i)) return 0;
    i = name_i + 1;
    params_open = SIZE_MAX;
    if (i <= b && str_eq(pt(p, i), "(")) {
        params_open = i;
        params_close = find_matching(p, params_open, b + 1);
        if (params_close > b) return 0;
        i = params_close + 1;
    } else {
        params_close = SIZE_MAX;
    }
    if (i > b || !str_eq(pt(p, i), "->")) return 0;
    i++;
    if (i > b || !str_eq(pt(p, i), "func")) return 0;
    func_i = i;
    i++;
    if (i > b || !str_eq(pt(p, i), "(")) return 0;
    target_open = i;
    target_close = find_matching(p, target_open, b + 1);
    if (target_close > b) return 0;
    i = target_close + 1;
    while (i <= b && (str_eq(pt(p, i), "const") || str_eq(pt(p, i), "noexcept") || str_eq(pt(p, i), "&") || str_eq(pt(p, i), "&&") || str_eq(pt(p, i), "requires"))) i++;
    if (i > b || !str_eq(pt(p, i), "{")) return 0;
    body_open = i;
    body_close = find_matching(p, body_open, b + 1);
    if (body_close > b) return 0;
    if (end_out) *end_out = body_close;
    (void)cur; (void)par; (void)br; (void)angle; (void)func_i;
    return 1;
}

int parse_custom_decorator_decl(Parser *p, size_t a, size_t b, int64_t parent) {
    size_t at = SIZE_MAX, name_i, name_end, params_open, params_close, target_open, target_close, body_open, body_close, i;
    size_t template_i = SIZE_MAX, template_close = SIZE_MAX, decl_start = a;
    if (!find_top_level_at(p, a, b, &at) || at <= a || at + 1 > b) return 0;
    if (str_eq(pt(p, a), "template")) {
        template_i = a;
        {
            size_t open = a + 1;
            while (open < at && !str_eq(pt(p, open), "<")) open++;
            if (open >= at) return 0;
            template_close = find_template_close_cpp(p, open, at);
            if (template_close == SIZE_MAX || template_close + 1 >= at) return 0;
            decl_start = template_close + 1;
        }
    }
    name_i = at + 1;
    if (!is_decorator_identifier(p, name_i)) return 0;
    name_end = name_i;
    while (name_end + 2 <= b && str_eq(pt(p, name_end + 1), "::") && is_decorator_identifier(p, name_end + 2)) name_end += 2;
    i = name_end + 1;
    params_open = params_close = SIZE_MAX;
    if (i <= b && str_eq(pt(p, i), "(")) {
        params_open = i;
        params_close = find_matching(p, i, b + 1);
        if (params_close > b) return 0;
        i = params_close + 1;
    }
    if (i > b || !str_eq(pt(p, i), "->")) return 0;
    if (i + 2 > b || !str_eq(pt(p, i + 1), "func") || !str_eq(pt(p, i + 2), "(")) return 0;
    target_open = i + 2;
    target_close = find_matching(p, target_open, b + 1);
    if (target_close > b) return 0;
    i = target_close + 1;
    while (i <= b && (str_eq(pt(p, i), "const") || str_eq(pt(p, i), "noexcept") || str_eq(pt(p, i), "&") || str_eq(pt(p, i), "&&") || str_eq(pt(p, i), "requires"))) i++;
    if (i > b || !str_eq(pt(p, i), "{")) return 0;
    body_open = i;
    body_close = find_matching(p, body_open, b + 1);
    if (body_close > b) return 0;

    {
        int64_t n = ir_add_node(p->ir, N_CUSTOM_DECORATOR, a, body_close, parent);
        Str rt; str_init(&rt);
        {
            char *decl_return = token_range_text(p, decl_start, at - 1);
            str_put(&rt, decl_return);
            free(decl_return);
        }
        str_trim_ascii(&rt);
        node_set_return(p->ir, n, rt.data ? rt.data : "");
        str_free(&rt);
        {
            char *decorator_name = join_qualified(p, name_i, name_end);
            char *decorator_qualified = scope_qualified(p, decorator_name);
            node_set_name(p->ir, n, decorator_name);
            node_set_qualified(p->ir, n, decorator_qualified);
            free(decorator_name); free(decorator_qualified);
        }
        node_set_value_range(p, n, a, body_close);
        ir_add_child(p->ir, parent, n);
        if (template_i != SIZE_MAX) parse_custom_decorator_template_decl(p, n, template_i, template_close);
        if (params_open != SIZE_MAX && params_close > params_open + 1) parse_parameters(p, n, params_open + 1, params_close - 1);
        {
            int64_t target = ir_add_node(p->ir, N_DECORATOR_TARGET, target_open - 1, target_close, n);
            node_set_name(p->ir, target, "func");
            node_set_value_range(p, target, target_open - 1, target_close);
            ir_add_child(p->ir, n, target);
            if (target_close == target_open + 2 && str_eq(pt(p, target_open + 1), "...")) p->ir->nodes[target].flags |= NF_DECORATOR_VARIADIC;
            else if (target_close > target_open + 1) parse_parameters(p, target, target_open + 1, target_close - 1);
        }
        return 1;
    }
}
