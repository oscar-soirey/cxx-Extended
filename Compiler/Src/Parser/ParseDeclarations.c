/*
 * Parser/ParseDeclarations.c
 *
 * Declarations: functions, parameters, variables, attributes, simple statements.
 */

#include "Core/Common.h"
#include "Parser/ParserInternal.h"
#include "Core/Memory.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"

NodeKind decl_kind_for_context(int class_context) {
    return class_context ? N_FIELD_DECL : N_VAR_DECL;
}

void infer_decl_name_type(Parser *p, size_t a, size_t b, int64_t n) {
    size_t eq = SIZE_MAX, semi = b;
    size_t i;
    for (i = a; i <= b; ++i) {
        if (str_eq(pt(p, i), "=") || str_eq(pt(p, i), ":")) { if (eq == SIZE_MAX) eq = i; }
        if (str_eq(pt(p, i), ";")) { semi = i; break; }
    }
    (void)semi;
    for (i = (eq == SIZE_MAX ? b : eq - 1); i >= a; --i) {
        if (is_identifier(p, i)) {
            node_set_name(p->ir, n, pt(p, i));
            node_set_qualified_scope(p, n, pt(p, i));
            break;
        }
        if (i == a) break;
    }
    if (eq != SIZE_MAX && eq > a) node_set_value_range(p, n, eq + 1, b);
    else node_set_value_range(p, n, a, b);
    {
        size_t name_i = SIZE_MAX;
        for (i = a; i <= b; ++i) if (is_identifier(p, i) && strcmp(pt(p, i), p->ir->nodes[n].name ? p->ir->nodes[n].name : "") == 0) { name_i = i; break; }
        if (name_i != SIZE_MAX && name_i > a) node_set_type_range(p, n, a, name_i - 1);
    }
}

void parse_parameters(Parser *p, int64_t fn, size_t a, size_t b) {
    size_t i = a, start = a;
    int depth = 0;
    while (i <= b) {
        const char *t = pt(p, i);
        if (str_eq(t, "(") || str_eq(t, "[") || str_eq(t, "{")) depth++;
        else if (str_eq(t, ")") || str_eq(t, "]") || str_eq(t, "}")) { if (depth) depth--; }
        if ((str_eq(t, ",") && depth == 0) || i == b) {
            size_t e = (i == b ? i : i - 1);
            if (start <= e) {
                int64_t pn = ir_add_node(p->ir, N_PARAM_DECL, start, e, fn);
                infer_decl_name_type(p, start, e, pn);
                ir_add_child(p->ir, fn, pn);
            }
            start = i + 1;
        }
        i++;
    }
}

int parse_function_like(Parser *p, size_t a, size_t b, int64_t parent, const char *class_name, size_t open_paren, int64_t *out_fn) {
    size_t close_paren, k, name_i = SIZE_MAX;
    NodeKind kind = N_FUNCTION;
    char *name = NULL;
    char *qual = NULL;
    int64_t fn;
    close_paren = find_matching(p, open_paren, b + 1);
    if (close_paren > b) return 0;

    /* Find function name immediately before ( or operator spelling. */
    if (open_paren > a) {
        k = open_paren - 1;
        if (is_identifier(p, k) || str_eq(pt(p, k), "operator") || str_eq(pt(p, k), "~")) {
            name_i = k;
        } else if (str_eq(pt(p, k), ">")) {
            int depth = 0;
            while (k > a) {
                if (str_eq(pt(p, k), ">")) depth++;
                else if (str_eq(pt(p, k), "<") && depth) { if (--depth == 0) break; }
                if (depth == 0 && is_identifier(p, k - 1)) { name_i = k - 1; break; }
                if (!k) break;
                k--;
            }
        }
    }
    if (name_i == SIZE_MAX) return 0;
    /* A free-function declaration needs a declaration prefix before its name.
       This prevents ordinary calls such as add(x) from being misclassified. */
    if (!class_name && name_i == a && !str_eq(pt(p, name_i), "operator")) return 0;
    if (!class_name && name_i > a) {
        size_t q;
        for (q = a; q < name_i; ++q) {
            const char *qt = pt(p, q);
            if (str_eq(qt, "=") || str_eq(qt, "+") || str_eq(qt, "-") ||
                str_eq(qt, "/") || str_eq(qt, "%") || str_eq(qt, "!") ||
                str_eq(qt, "?") || str_eq(qt, ".") || str_eq(qt, "->") ||
                str_eq(qt, ",")) return 0;
        }
    }
    if (str_eq(pt(p, name_i), "operator")) kind = N_OPERATOR_FUNCTION;
    else if (class_name && str_eq(pt(p, name_i), class_name)) kind = N_CONSTRUCTOR;
    else if (class_name && str_eq(pt(p, name_i), "~")) kind = N_DESTRUCTOR;
    else if (name_i > a && str_eq(pt(p, name_i - 1), "~")) kind = N_DESTRUCTOR;
    else if (class_name) kind = N_METHOD;

    name = xstrdup0(pt(p, name_i));
    qual = scope_qualified(p, name);
    {
        size_t function_last = b;
        size_t body_open = SIZE_MAX;
        fn = ir_add_node(p->ir, kind, a, b, parent);
        node_set_name(p->ir, fn, name);
        node_set_qualified(p->ir, fn, qual);
        if (kind == N_OPERATOR_FUNCTION) node_set_resolved(p->ir, fn, qual);
        if (name_i > a) node_set_return_range(p, fn, a, name_i - 1);
        parse_parameters(p, fn, open_paren + 1, close_paren - 1);

        /* CXXE accepts both `f(...) {}` and C++ trailing-return syntax
           `auto f(...) -> T {}`.  Locate the body in the latter form too,
           otherwise CXXE-only constructs inside the function (e.g. match)
           would remain in the emitted C++ unchanged. */
        if (close_paren + 1 <= b) {
            if (str_eq(pt(p, close_paren + 1), "{")) {
                body_open = close_paren + 1;
            } else if (str_eq(pt(p, close_paren + 1), "->")) {
                size_t j = close_paren + 2;
                while (j <= b && !str_eq(pt(p, j), "{") && !str_eq(pt(p, j), ";")) j++;
                if (j <= b && str_eq(pt(p, j), "{")) body_open = j;
            }
        }
        if (body_open != SIZE_MAX) {
            size_t close = find_matching(p, body_open, b + 1);
            if (close <= b) {
                function_last = close;
                p->ir->nodes[fn].last_tok = close;
                if (body_open + 1 <= close - 1) {
                    /* Insert property assignment nodes before the generic expression
                       nodes so emission order follows source order when reads and
                       writes overlap the same statement range. */
                    add_property_assignment_nodes_in_range(p, fn, body_open + 1, close - 1);
                    add_expr_nodes(p, fn, body_open + 1, close - 1);
                }
            }
        }
        p->ir->nodes[fn].last_tok = function_last;
        ir_add_child(p->ir, parent, fn);
    }
    if (out_fn) *out_fn = fn;
    free(name); free(qual);
    return 1;
}

void parse_attributes(Parser *p, size_t a, size_t b, int64_t parent) {
    size_t i = a;
    while (i + 1 <= b) {
        if (str_eq(pt(p, i), "[") && str_eq(pt(p, i + 1), "[")) {
            size_t j = i + 2;
            while (j + 1 <= b && !(str_eq(pt(p, j), "]") && str_eq(pt(p, j + 1), "]"))) j++;
            if (j + 1 <= b) {
                int64_t n = ir_add_node(p->ir, N_ATTRIBUTE, i, j + 1, parent);
                node_set_value_range(p, n, i, j + 1);
                ir_add_child(p->ir, parent, n);
                i = j + 2;
                continue;
            }
        }
        i++;
    }
}

void parse_simple_statement(Parser *p, size_t a, size_t b, int64_t parent) {
    if (a > b) return;
    const char *t = pt(p, a);
    NodeKind k = N_EXPR;
    if (str_eq(t, "return")) k = N_RETURN;
    else if (str_eq(t, "if")) k = N_IF_STMT;
    else if (str_eq(t, "for")) k = N_FOR_STMT;
    else if (str_eq(t, "while")) k = N_WHILE_STMT;
    else if (str_eq(t, "do")) k = N_DO_STMT;
    else if (str_eq(t, "switch")) k = N_SWITCH_STMT;
    else if (str_eq(t, "break")) k = N_BREAK_STMT;
    else if (str_eq(t, "continue")) k = N_CONTINUE_STMT;
    else if (str_eq(t, "goto")) k = N_GOTO_STMT;
    else if (str_eq(t, "throw")) k = N_THROW;
    else if (str_eq(t, "co_return")) k = N_CO_RETURN;
    else if (str_eq(t, "co_await")) k = N_CO_AWAIT;
    else if (str_eq(t, "co_yield")) k = N_CO_YIELD;
    else if (str_eq(t, "try")) k = N_TRY_STMT;
    else if (str_eq(t, "catch")) k = N_CATCH;
    else if (str_eq(t, "new")) k = N_NEW_EXPR;
    else if (str_eq(t, "delete")) k = N_DELETE_EXPR;
    else if (str_eq(t, "static_cast") || str_eq(t, "dynamic_cast") || str_eq(t, "reinterpret_cast") || str_eq(t, "const_cast")) k = N_CAST_EXPR;
    else if (str_eq(t, "sizeof")) k = N_SIZEOF_EXPR;
    else if (str_eq(t, "alignof")) k = N_ALIGNOF_EXPR;
    else if (str_eq(t, "typeid")) k = N_TYPEID_EXPR;
    else if (str_eq(t, "noexcept")) k = N_NOEXCEPT_EXPR;

    {
        int64_t n = ir_add_node(p->ir, k, a, b, parent);
        node_set_value_range(p, n, a, b);
        ir_add_child(p->ir, parent, n);
        add_expr_nodes(p, n, a, b);
    }
}

int looks_like_var_decl(Parser *p, size_t a, size_t b) {
    size_t i, semi = b;
    if (a > b) return 0;
    if (str_eq(pt(p, a), "return") || str_eq(pt(p, a), "if") || str_eq(pt(p, a), "for") ||
        str_eq(pt(p, a), "while") || str_eq(pt(p, a), "switch") || str_eq(pt(p, a), "throw")) return 0;

    /* Existing declaration forms with an obvious type/name boundary. */
    for (i = a; i <= b; ++i) {
        if (str_eq(pt(p, i), ";")) { semi = i; break; }
        if (str_eq(pt(p, i), "=") || str_eq(pt(p, i), ":")) continue;
        if (pk(p, i) == TK_IDENTIFIER || pk(p, i) == TK_PUNCT) {
            if (i > a && is_identifier(p, i) &&
                (str_eq(pt(p, i - 1), "*") || str_eq(pt(p, i - 1), "&") || is_keyword(pt(p, i - 1)))) return 1;
        }
    }
    if (is_keyword(pt(p, a)) || str_eq(pt(p, a), "dynamic")) return 1;

    /* Qualified / user-defined local declarations such as
       `Outer::Player p;` and `Character c;`. At this stage semantic lookup is
       deliberately deferred; the parser only needs the unambiguous syntactic
       property that the final declarator name is an identifier not preceded
       by member/qualified-access punctuation. Expression statements such as
       `object.member;` and `object->member;` therefore stay expressions. */
    if (semi > a) {
        size_t last = semi;
        if (last > a && str_eq(pt(p, last), ";")) --last;
        if (last > a && is_identifier(p, last) &&
            !str_eq(pt(p, last - 1), ".") && !str_eq(pt(p, last - 1), "->") &&
            !str_eq(pt(p, last - 1), "::")) {
            int has_type_tokens = 0;
            for (i = a; i < last; ++i) {
                if (is_identifier(p, i) || str_eq(pt(p, i), "::") || str_eq(pt(p, i), "*") ||
                    str_eq(pt(p, i), "&") || str_eq(pt(p, i), "const") || str_eq(pt(p, i), "volatile")) {
                    has_type_tokens = 1;
                    break;
                }
            }
            if (has_type_tokens) return 1;
        }
    }
    return 0;
}

/* Custom decorator declarations                                             */

/*                                                                           */

/* Syntax:                                                                   */

/*   void @my_decorator(T x) -> func(int a) { before(x); func(a); }         */

/*                                                                           */

/* The declaration itself is a CXXE-only construct and disappears from the  */

/* generated C++. At use sites (@my_decorator(...)), the decorator body is  */

/* instantiated around the decorated function.                              */

int find_top_level_at(Parser *p, size_t a, size_t b, size_t *at_out) {
    int par = 0, br = 0, cur = 0, angle = 0;
    size_t i;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(")) par++;
        else if (str_eq(t, ")") && par) par--;
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]") && br) br--;
        else if (str_eq(t, "{") && par == 0 && br == 0 && cur == 0 && angle == 0) {
            /* An ordinary top-level function/class body proves this is not a
               custom decorator declaration. */
            return 0;
        }
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}") && cur) cur--;
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (str_eq(t, ";") && par == 0 && br == 0 && cur == 0 && angle == 0) {
            /* A completed top-level declaration before `@` is not a custom
               decorator declaration. */
            /* A custom decorator declaration must reach `@` before it reaches
               the first top-level terminator/body. Otherwise an ordinary
               function definition followed by `@decorator` would be consumed
               as one decorator declaration. */
            return 0;
        }
        else if (str_eq(t, "@") && par == 0 && br == 0 && cur == 0 && angle == 0) {
            if (at_out) *at_out = i;
            return 1;
        }
    }
    return 0;
}
