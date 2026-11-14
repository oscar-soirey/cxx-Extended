/*
 * Parser/ParserCore.c
 *
 * Token access, scope tracking and node attribute helpers.
 */

#include "Core/Common.h"
#include "Parser/ParserInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"

const char *pt(Parser *p, size_t i) {
    if (i >= p->ir->tokens.n) return "";
    return p->ir->tokens.v[i].text;
}

uint32_t pk(Parser *p, size_t i) {
    if (i >= p->ir->tokens.n) return TK_EOF;
    return p->ir->tokens.v[i].kind;
}

static int peq(Parser *p, size_t i, const char *s) { return str_eq(pt(p, i), s); }

int is_word_tok(Parser *p, size_t i) {
    uint32_t k = pk(p, i);
    return k == TK_IDENTIFIER || k == TK_NUMBER || k == TK_STRING || k == TK_CHAR;
}

int is_keyword(const char *s) {
    static const char *const kws[] = {
        "alignas","alignof","and","and_eq","asm","atomic_cancel","atomic_commit",
        "atomic_noexcept","auto","bitand","bitor","bool","break","case","catch",
        "char","char8_t","char16_t","char32_t","class","compl","concept","const",
        "consteval","constexpr","constinit","const_cast","continue","co_await","co_return",
        "co_yield","decltype","default","delete","do","double","dynamic_cast","else",
        "enum","explicit","export","extern","false","float","for","friend","goto",
        "if","inline","int","long","mutable","namespace","new","noexcept","not","not_eq",
        "nullptr","operator","or","or_eq","private","protected","public","reflexpr","register",
        "reinterpret_cast","requires","return","short","signed","sizeof","static","static_assert",
        "static_cast","struct","switch","synchronized","template","this","thread_local","throw",
        "true","try","typedef","typeid","typename","union","unsigned","using","virtual","void",
        "volatile","wchar_t","while","dynamic","match","xor","xor_eq","module","import","override","final","transaction_safe",
        "transaction_safe_dynamic","__attribute__","__declspec","nullptr","constinit","requires","concept",
        NULL
    };
    size_t i;
    for (i = 0; kws[i]; ++i) if (strcmp(kws[i], s) == 0) return 1;
    return 0;
}

int is_identifier(Parser *p, size_t i) {
    return pk(p, i) == TK_IDENTIFIER && !is_keyword(pt(p, i));
}

int is_decorator_identifier(Parser *p, size_t i) {
    return pk(p, i) == TK_IDENTIFIER || pk(p, i) == TK_PUNCT;
}

static size_t skip_balanced(Parser *p, size_t i, size_t end, const char *open, const char *close) {
    int depth = 0;
    while (i < end) {
        if (peq(p, i, open)) depth++;
        else if (peq(p, i, close)) {
            depth--;
            if (depth == 0) return i;
        }
        i++;
    }
    return end ? end - 1 : 0;
}

size_t find_matching(Parser *p, size_t open_i, size_t end) {
    const char *open = pt(p, open_i);
    const char *close = NULL;
    if (str_eq(open, "(")) close = ")";
    else if (str_eq(open, "[")) close = "]";
    else if (str_eq(open, "{")) close = "}";
    else if (str_eq(open, "<")) close = ">";
    else return open_i;
    return skip_balanced(p, open_i, end, open, close);
}

size_t find_stmt_end(Parser *p, size_t i, size_t end) {
    int par = 0, br = 0, cur = 0;
    while (i < end) {
        if (peq(p, i, "(")) par++;
        else if (peq(p, i, ")")) { if (par) par--; }
        else if (peq(p, i, "[")) br++;
        else if (peq(p, i, "]")) { if (br) br--; }
        else if (peq(p, i, "{")) cur++;
        else if (peq(p, i, "}")) { if (cur) cur--; else return i; }
        else if (peq(p, i, ";") && par == 0 && br == 0 && cur == 0) return i;
        i++;
    }
    return end ? end - 1 : 0;
}

char *token_range_text(Parser *p, size_t a, size_t b) {
    Str s;
    size_t i;
    str_init(&s);
    if (b < a) return xstrdup0("");
    if (b >= p->ir->tokens.n) b = p->ir->tokens.n - 1;
    for (i = a; i <= b; ++i) {
        str_put(&s, p->ir->tokens.v[i].trivia);
        str_put(&s, p->ir->tokens.v[i].text);
    }
    return str_take(&s);
}

char *join_qualified(Parser *p, size_t a, size_t b) {
    Str s;
    size_t i;
    int need = 0;
    str_init(&s);
    for (i = a; i <= b && i < p->ir->tokens.n; ++i) {
        const char *t = pt(p, i);
        if (is_identifier(p, i)) {
            if (need) str_put(&s, "::");
            str_put(&s, t);
            need = 1;
        }
    }
    if (!s.len && a <= b) str_put(&s, pt(p, a));
    return str_take(&s);
}

char *scope_qualified(Parser *p, const char *name) {
    Str s;
    size_t i;
    str_init(&s);
    for (i = 0; i < p->scope_n; ++i) {
        if (s.len) str_put(&s, "::");
        str_put(&s, p->scope[i]);
    }
    if (name && *name) {
        if (s.len) str_put(&s, "::");
        str_put(&s, name);
    }
    return str_take(&s);
}

void scope_push(Parser *p, const char *name) {
    if (!name || !*name) return;
    if (p->scope_n == p->scope_cap) {
        p->scope_cap = p->scope_cap ? p->scope_cap * 2 : 16;
        p->scope = (char **)xrealloc(p->scope, p->scope_cap * sizeof(*p->scope));
    }
    p->scope[p->scope_n++] = xstrdup0(name);
}

void scope_pop(Parser *p) {
    if (p->scope_n) free(p->scope[--p->scope_n]);
}

void node_set_name(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].name);
    ir->nodes[id].name = xstrdup0(s ? s : "");
}

void node_set_qualified(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].qualified);
    ir->nodes[id].qualified = xstrdup0(s ? s : "");
}

void node_set_type(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].type);
    ir->nodes[id].type = xstrdup0(s ? s : "");
}

void node_set_return(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].return_type);
    ir->nodes[id].return_type = xstrdup0(s ? s : "");
}

void node_set_value(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].value);
    ir->nodes[id].value = xstrdup0(s ? s : "");
}

void node_set_resolved(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].resolved_name);
    ir->nodes[id].resolved_name = xstrdup0(s ? s : "");
}

void node_set_value_range(Parser *p, int64_t id, size_t a, size_t b) {
    char *s = token_range_text(p, a, b);
    node_set_value(p->ir, id, s);
    free(s);
}

void node_set_type_range(Parser *p, int64_t id, size_t a, size_t b) {
    char *s = token_range_text(p, a, b);
    node_set_type(p->ir, id, s);
    free(s);
}

void node_set_return_range(Parser *p, int64_t id, size_t a, size_t b) {
    char *s = token_range_text(p, a, b);
    node_set_return(p->ir, id, s);
    free(s);
}

void node_set_qualified_scope(Parser *p, int64_t id, const char *name) {
    char *s = scope_qualified(p, name);
    node_set_qualified(p->ir, id, s);
    free(s);
}

void node_set_name_range_owned(IR *ir, int64_t id, char *s) {
    node_set_name(ir, id, s);
    free(s);
}

void node_set_resolved_owned_scope(Parser *p, int64_t id, const char *name) {
    char *s = scope_qualified(p, name);
    node_set_resolved(p->ir, id, s);
    free(s);
}
