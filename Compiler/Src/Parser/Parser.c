/*
 * Parser/Parser.c
 *
 * Region/statement dispatcher and parse_source entry point.
 */

#include "Core/Common.h"
#include "Parser/Parser.h"
#include "Core/Memory.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"
#include "Parser/ParserInternal.h"

/* End token (inclusive) of the single declaration starting at `a`:
   - a function definition ends at its body's closing brace;
   - a class/struct/union/enum, a variable with a braced initializer, an alias
     or a concept ends at the next top-level ';'. */
static size_t find_decl_end(Parser *p, size_t a, size_t b) {
    size_t i = a;
    int saw_eq = 0, is_type = 0;
    while (i <= b && (str_eq(pt(p, i), "[[") || str_eq(pt(p, i), "export"))) {
        if (str_eq(pt(p, i), "[[")) { while (i <= b && !str_eq(pt(p, i), "]]")) i++; }
        i++;
    }
    if (i <= b && (str_eq(pt(p, i), "class") || str_eq(pt(p, i), "struct") ||
                   str_eq(pt(p, i), "union") || str_eq(pt(p, i), "enum") ||
                   str_eq(pt(p, i), "using") || str_eq(pt(p, i), "concept"))) is_type = 1;
    for (; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(") || str_eq(t, "[")) { size_t c = find_matching(p, i, b + 1); if (c > b) return b; i = c; continue; }
        if (str_eq(t, "=")) saw_eq = 1;
        if (str_eq(t, ";")) return i;
        if (str_eq(t, "{")) {
            size_t c = find_matching(p, i, b + 1);
            if (c > b) return b;
            if (!saw_eq && !is_type) return c;
            i = c;
        }
    }
    return b;
}

static void parse_region_items(Parser *p, size_t a, size_t b, int64_t parent, int class_context, const char *class_name) {
    size_t i = a;
    while (i <= b) {
        if (pk(p, i) == TK_PP) {
            parse_preprocessor(p, i, parent);
            i++;
            continue;
        }

        if (str_eq(pt(p, i), "@")) {
            parse_decorator_sequence(p, &i, b, parent);
            continue;
        }

        /* CXXE custom decorator declaration: return_type @name(...) -> func(...) { ... } */
        {
            size_t custom_end = SIZE_MAX;
            if (looks_like_custom_decorator_decl(p, i, b, parent, &custom_end)) {
                if (parse_custom_decorator_decl(p, i, custom_end, parent)) {
                    i = custom_end + 1;
                    continue;
                }
            }
        }

        if (str_eq(pt(p, i), "[[")) {
            parse_attributes(p, i, b, parent);
            i++;
            continue;
        }

        if (str_eq(pt(p, i), "template")) {
            size_t j = (i + 1 <= b) ? find_template_close_cpp(p, i + 1, b + 1) : SIZE_MAX;
            if (j != SIZE_MAX && j < b) {
                /* The template header only prefixes one declaration: delimit
                   exactly that declaration and parse it like any other item,
                   so functions, classes and properties inside templates are
                   visible to the CXXE passes (named arguments, properties...). */
                size_t e = find_decl_end(p, j + 1, b);
                int64_t n = ir_add_node(p->ir, N_TEMPLATE, i, e, parent);
                ir_add_child(p->ir, parent, n);
                parse_region_items(p, j + 1, e, n, class_context, class_name);
                i = e + 1;
                continue;
            }
        }

        if (str_eq(pt(p, i), "namespace")) {
            size_t name_i = (i + 1 <= b && is_identifier(p, i + 1)) ? i + 1 : SIZE_MAX;
            size_t brace = i + 1;
            while (brace <= b && !str_eq(pt(p, brace), "{")) brace++;
            if (brace <= b) {
                size_t close = find_matching(p, brace, b + 1);
                int64_t n = ir_add_node(p->ir, N_NAMESPACE, i, close <= b ? close : b, parent);
                if (name_i != SIZE_MAX) {
                    node_set_name(p->ir, n, pt(p, name_i));
                    node_set_qualified_scope(p, n, pt(p, name_i));
                    scope_push(p, pt(p, name_i));
                }
                node_set_value_range(p, n, i, close <= b ? close : b);
                ir_add_child(p->ir, parent, n);
                if (close > brace) parse_region_items(p, brace + 1, close - 1, n, 0, NULL);
                if (name_i != SIZE_MAX) scope_pop(p);
                i = (close <= b ? close + 1 : b + 1);
                continue;
            }
        }

        if (str_eq(pt(p, i), "class") || str_eq(pt(p, i), "struct") || str_eq(pt(p, i), "union")) {
            NodeKind k = str_eq(pt(p, i), "class") ? N_CLASS : (str_eq(pt(p, i), "struct") ? N_STRUCT : N_UNION);
            size_t e = find_stmt_end(p, i, b + 1);
            size_t brace = i;
            while (brace <= b && !str_eq(pt(p, brace), "{")) brace++;
            if (brace <= b) {
                size_t close = find_matching(p, brace, b + 1);
                parse_class(p, i, close <= b ? close : e, parent, k);
                /* parse_class consumes and applies pending decorators itself. */
                i = (close <= b ? close + 1 : e + 1);
                continue;
            }
        }

        if (str_eq(pt(p, i), "enum")) {
            size_t e = find_stmt_end(p, i, b + 1);
            {
                size_t before = p->ir->node_count;
                parse_enum(p, i, e, parent);
                if (p->ir->node_count > before) parser_apply_pending(p, (int64_t)before);
            }
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "typedef")) {
            size_t e = find_stmt_end(p, i, b + 1);
            int64_t n = ir_add_node(p->ir, N_TYPEDEF, i, e, parent);
            infer_decl_name_type(p, i, e, n);
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "using")) {
            size_t e = find_stmt_end(p, i, b + 1);
            NodeKind k = N_USING_DECL;
            size_t j;
            for (j = i + 1; j <= e; ++j) if (str_eq(pt(p, j), "namespace")) k = N_USING_DIRECTIVE;
            int64_t n = ir_add_node(p->ir, k, i, e, parent);
            if (i + 1 <= e && is_identifier(p, i + 1)) node_set_name(p->ir, n, pt(p, i + 1));
            node_set_value_range(p, n, i, e);
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "concept")) {
            size_t e = find_stmt_end(p, i, b + 1);
            int64_t n = ir_add_node(p->ir, N_CONCEPT, i, e, parent);
            if (i + 1 <= e && is_identifier(p, i + 1)) node_set_name(p->ir, n, pt(p, i + 1));
            node_set_value_range(p, n, i, e);
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "friend")) {
            size_t e = find_stmt_end(p, i, b + 1);
            int64_t n = ir_add_node(p->ir, N_FRIEND, i, e, parent);
            node_set_value_range(p, n, i, e);
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        /* Find a top-level (' in this declaration/statement. */
        {
            size_t e = find_stmt_end(p, i, b + 1);
            size_t open = find_top_call_paren(p, i, e);
            if (open != SIZE_MAX) {
                /* A function definition/declaration is recognizable by { or ; after ). */
                size_t close = find_matching(p, open, e + 1);
                size_t after = close < e ? close + 1 : close;
                int functionish = 0;
                if (after <= e && (str_eq(pt(p, after), "{") || str_eq(pt(p, after), ";") || str_eq(pt(p, after), "const") || str_eq(pt(p, after), "noexcept") || str_eq(pt(p, after), "->") || str_eq(pt(p, after), "override") || str_eq(pt(p, after), "final"))) functionish = 1;
                if (functionish) {
                    int64_t fn_id = -1;
                    size_t function_end = e;
                    /* find_stmt_end() deliberately stops at the first top-level
                       semicolon, which is not enough for a function body.  Extend
                       the parser range to the matching closing brace so CXXE-only
                       statements (such as match) inside the function are seen. */
                    if (after <= e && str_eq(pt(p, after), "{")) {
                        size_t body_close = find_matching(p, after, b + 1);
                        if (body_close <= b) function_end = body_close;
                    }
                    if (parse_function_like(p, i, function_end, parent, class_name, open, &fn_id)) {
                        if (fn_id >= 0) parser_apply_pending(p, fn_id);
                        if (fn_id >= 0) i = (size_t)p->ir->nodes[fn_id].last_tok + 1;
                        else i = function_end + 1;
                        continue;
                    }
                }
            }

            /* Control statements / blocks. */
            if (str_eq(pt(p, i), "if") || str_eq(pt(p, i), "for") || str_eq(pt(p, i), "while") || str_eq(pt(p, i), "switch") || str_eq(pt(p, i), "catch")) {
                size_t open = i + 1;
                while (open <= e && !str_eq(pt(p, open), "(")) open++;
                if (open <= e) {
                    size_t c = find_matching(p, open, e + 1);
                    size_t body = c + 1;
                    if (body <= e && str_eq(pt(p, body), "{")) {
                        size_t bc = find_matching(p, body, e + 1);
                        NodeKind sk = str_eq(pt(p, i), "if") ? N_IF_STMT : str_eq(pt(p, i), "for") ? N_FOR_STMT : str_eq(pt(p, i), "while") ? N_WHILE_STMT : str_eq(pt(p, i), "switch") ? N_SWITCH_STMT : N_CATCH;
                        int64_t n = ir_add_node(p->ir, sk, i, bc <= e ? bc : e, parent);
                        node_set_value_range(p, n, i, bc <= e ? bc : e);
                        ir_add_child(p->ir, parent, n);
                        if (bc > body) parse_region_items(p, body + 1, bc - 1, n, 0, NULL);
                        i = (bc <= e ? bc + 1 : e + 1);
                        continue;
                    }
                }
            }

            /* Generic declaration / expression. */
            if (looks_like_var_decl(p, i, e)) {
                int64_t n = ir_add_node(p->ir, decl_kind_for_context(class_context), i, e, parent);
                infer_decl_name_type(p, i, e, n);
                ir_add_child(p->ir, parent, n);
                parser_apply_pending(p, n);
                if (looks_like_assignment(p, i, e, NULL)) {
                    int64_t a = ir_add_node(p->ir, N_VAR_ASSIGN, i, e, n);
                    node_set_value_range(p, a, i, e);
                    ir_add_child(p->ir, n, a);
                }
            } else {
                size_t before = p->ir->node_count;
                parse_simple_statement(p, i, e, parent);
                if (p->ir->node_count > before) parser_apply_pending(p, (int64_t)before);
            }
            i = e + 1;
        }
    }
}

void parse_source(IR *ir) {
    Parser p;
    int64_t root;
    memset(&p, 0, sizeof(p));
    p.ir = ir;
    p.end = ir->tokens.n ? ir->tokens.n - 1 : 0;
    /* The driver may pre-seed the tree with declarations imported from
       headers under an existing root; parse into it. */
    if (ir->node_count && ir->nodes[0].kind == N_TRANSLATION_UNIT) root = 0;
    else root = ir_add_node(ir, N_TRANSLATION_UNIT, 0, p.end, -1);
    p.root = root;
    parse_region_items(&p, 0, p.end ? p.end - 1 : 0, root, 0, NULL);
    while (p.scope_n) scope_pop(&p);
    free(p.pending_decorators);
    free(p.scope);
}
