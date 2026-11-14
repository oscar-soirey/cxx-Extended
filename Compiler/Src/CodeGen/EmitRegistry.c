/*
 * CodeGen/EmitRegistry.c
 *
 * Runtime registration emission (@register / @exposed classes, enums, factories).
 */

#include "Core/Common.h"
#include "CodeGen/CodeGenInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Sema/Resolve.h"

static void emit_cxxe_string(Str *out, const char *s) {
    size_t i;
    str_ch(out, '"');
    if (s) {
        for (i = 0; s[i]; ++i) {
            unsigned char c = (unsigned char)s[i];
            if (c == '\\' || c == '"') { str_ch(out, '\\'); str_ch(out, (char)c); }
            else if (c == '\n') str_put(out, "\\n");
            else if (c == '\r') str_put(out, "\\r");
            else if (c == '\t') str_put(out, "\\t");
            else str_ch(out, (char)c);
        }
    }
    str_ch(out, '"');
}

int class_has_runtime_features(const IR *ir) {
    size_t i;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (n->flags & NF_EXTERNAL_DECL) continue;
        if ((n->kind == N_CLASS || n->kind == N_STRUCT || n->kind == N_UNION || n->kind == N_ENUM) && (n->flags & NF_REGISTERED)) return 1;
        if (n->kind == N_GET_MEMBER && (n->flags & NF_GET_MEMBER_RUNTIME)) return 1;
        if (n->kind == N_FUNC_CALL && n->qualified &&
            (strcmp(n->qualified, "stde::factory_new") == 0 || strcmp(n->qualified, "stde::factory_find") == 0)) return 1;
        if (n->kind == N_MATCH_STMT) return 1;
    }
    for (i = 0; i < ir->tokens.n; ++i) if (strcmp(ir->tokens.v[i].text, "dynamic") == 0) return 1;
    return 0;
}

static void emit_method_callable_name(const IR *ir, const IRNode *method, Str *out) {
    size_t a = (size_t)method->first_tok, b = (size_t)method->last_tok, i;
    if (!method->name || strcmp(method->name, "operator") != 0) {
        str_put(out, method->name ? method->name : "");
        return;
    }
    for (i = a; i <= b && i < ir->tokens.n; ++i) {
        if (strcmp(ir->tokens.v[i].text, "operator") == 0) {
            /* For operator(), the actual parameter list is the second pair of
               parentheses. Taking source tokens until the final function-open
               paren gives the exact callable spelling. */
            size_t j = i + 1, last_open = SIZE_MAX;
            int d = 0;
            for (; j <= b; ++j) {
                const char *t = ir->tokens.v[j].text;
                if (strcmp(t, "(") == 0) { d++; last_open = j; }
                else if (strcmp(t, ")") == 0 && d > 0) d--;
                if (strcmp(t, "{") == 0 || strcmp(t, ";") == 0) break;
            }
            if (last_open != SIZE_MAX) {
                size_t end = last_open;
                while (end > i && (strcmp(ir->tokens.v[end - 1].text, ")") == 0 ||
                       strcmp(ir->tokens.v[end - 1].text, "]") == 0)) end--;
                for (j = i; j < end; ++j) {
                    str_put(out, ir->tokens.v[j].text);
                }
                return;
            }
            str_put(out, "operator");
            return;
        }
    }
    str_put(out, "operator");
}

static int field_is_readonly(const IRNode *field) {
    if (!field->type) return 0;
    if (strstr(field->type, "const") != NULL) return 1;
    if (strchr(field->type, '[') != NULL) return 1;
    return 0;
}

static void emit_registered_member_field(const IRNode *cls, const IRNode *field, Str *out) {
    const char *ctype = cls->qualified && *cls->qualified ? cls->qualified : cls->name;
    str_put(out, ".expose_field(");
    emit_cxxe_string(out, field->name ? field->name : "");
    str_put(out, ",\n        [](void* self) -> stde::DynamicValue {\n            auto* obj = static_cast<");
    str_put(out, ctype ? ctype : "void");
    str_put(out, "*>(self);\n            return stde::DynamicValue::from(obj->");
    str_put(out, field->name ? field->name : "");
    str_put(out, ");\n        }");

    if (!field_is_readonly(field)) {
        str_put(out, ",\n        [](void* self, const std::any& value) {\n            auto* obj = static_cast<");
        str_put(out, ctype ? ctype : "void");
        str_put(out, "*>(self);\n            using MemberType = std::decay_t<decltype(obj->");
        str_put(out, field->name ? field->name : "");
        str_put(out, ")>;\n            obj->");
        str_put(out, field->name ? field->name : "");
        str_put(out, " = std::any_cast<MemberType>(value);\n        }");
    }
    str_put(out, ")");
}

static size_t function_param_ids(const IR *ir, const IRNode *fn, int64_t *ids, size_t cap) {
    size_t n = 0;
    int64_t c;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind != N_PARAM_DECL) continue;
        if (ids && n < cap) ids[n] = c;
        n++;
    }
    return n;
}

static int method_returns_void(const IR *ir, const IRNode *method) {
    size_t a = (size_t)method->first_tok, b = (size_t)method->last_tok, i;
    const char *rt = method->return_type ? method->return_type : "";
    while (*rt && isspace((unsigned char)*rt)) rt++;
    if (strncmp(rt, "void", 4) == 0 &&
        (rt[4] == 0 || isspace((unsigned char)rt[4]) || rt[4] == '*')) return 1;

    /* C++ trailing return type: auto f(...) -> void */
    for (i = a; i + 1 <= b && i < ir->tokens.n; ++i) {
        if (strcmp(ir->tokens.v[i].text, "->") == 0 && i + 1 <= b &&
            strcmp(ir->tokens.v[i + 1].text, "void") == 0) return 1;
    }
    return 0;
}

static void emit_registered_member_method(const IR *ir, const IRNode *cls, const IRNode *method, Str *out) {
    const char *ctype = cls->qualified && *cls->qualified ? cls->qualified : cls->name;
    int64_t params[64];
    size_t pn = function_param_ids(ir, method, params, 64), i;

    if (pn > 64) die("@exposed method '%s' has too many parameters", method->name ? method->name : "<unnamed>");

    str_put(out, ".expose_method(");
    Str method_name; str_init(&method_name);
    emit_method_callable_name(ir, method, &method_name);
    emit_cxxe_string(out, method_name.data ? method_name.data : (method->name ? method->name : ""));
    str_free(&method_name);
    str_put(out, ",\n        [](void* self, const std::vector<std::any>& args) -> stde::DynamicValue {\n            if (args.size() != ");
    {
        char num[32]; snprintf(num, sizeof(num), "%zu", pn); str_put(out, num);
    }
    str_put(out, ") throw std::runtime_error(\"CXXE: invalid argument count for exposed method\");\n            auto* obj = static_cast<");
    str_put(out, ctype ? ctype : "void");
    str_put(out, "*>(self);\n");

    for (i = 0; i < pn; ++i) {
        const IRNode *param = &ir->nodes[params[i]];
        if (!param->type || !*param->type) die("CXXE: cannot emit runtime wrapper for exposed method parameter '%s'", param->name ? param->name : "<unnamed>");
        str_put(out, "            using P");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, " = ");
        str_put(out, param->type);
        str_put(out, ";\n            P");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, " a");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, " = stde::detail::unpack_argument<P");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, ">(args[");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, "]);\n");
    }

    if (method_returns_void(ir, method)) {
        str_put(out, "            obj->");
        emit_method_callable_name(ir, method, out);
        str_put(out, "(");
        for (i = 0; i < pn; ++i) {
            if (i) str_put(out, ", ");
            str_put(out, "a");
            { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        }
        str_put(out, ");\n            return stde::DynamicValue();\n");
    } else {
        str_put(out, "            return stde::DynamicValue::from(obj->");
        emit_method_callable_name(ir, method, out);
        str_put(out, "(");
        for (i = 0; i < pn; ++i) {
            if (i) str_put(out, ", ");
            str_put(out, "a");
            { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        }
        str_put(out, "));\n");
    }

    str_put(out, "        }");
    str_put(out, ")");
}

static void emit_registered_enum(const IR *ir, const IRNode *en, size_t index, Str *out) {
    int64_t c;
    str_put(out, "        auto cxxe_enum_");
    { char num[32]; snprintf(num, sizeof(num), "%zu", index); str_put(out, num); }
    str_put(out, " = stde::register_enum<"); str_put(out, en->qualified);
    str_put(out, ">("); emit_cxxe_string(out, en->qualified); str_put(out, ");\n");
    for (c = en->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *ev=&ir->nodes[c];
        if (ev->kind != N_ENUMERATOR || !ev->name || !*ev->name) continue;
        str_put(out, "        cxxe_enum_"); { char num[32]; snprintf(num,sizeof(num),"%zu",index); str_put(out,num); }
        str_put(out, ".value("); emit_cxxe_string(out, ev->name); str_put(out, ", ");
        str_put(out, en->qualified); str_put(out, "::"); str_put(out, ev->name); str_put(out, ");\n");
    }
}

void emit_registered_classes(const IR *ir, Str *out) {
    size_t i;
    int any = 0;
    str_put(out, "\n\nnamespace cxxe_generated {\nstatic void register_all() {\n    static const bool initialized = []() {\n");
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *cls = &ir->nodes[i];
        int64_t c;
        if (cls->flags & NF_EXTERNAL_DECL) continue;
        if (!((cls->kind == N_CLASS || cls->kind == N_STRUCT || cls->kind == N_UNION || cls->kind == N_ENUM) && (cls->flags & NF_REGISTERED))) continue;
        if (!cls->name || !*cls->name || !cls->qualified || !*cls->qualified)
            die("@register requires a named type");
        any = 1;
        if (cls->kind == N_ENUM) { emit_registered_enum(ir, cls, i, out); continue; }
        str_put(out, "        auto cxxe_class_");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, " = stde::register_class<");
        str_put(out, cls->qualified);
        str_put(out, ">(");
        emit_cxxe_string(out, cls->qualified);
        str_put(out, ");\n");
        for (c = cls->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
            const IRNode *m = &ir->nodes[c];
            if (!(m->flags & NF_EXPOSED)) continue;
            str_put(out, "        cxxe_class_");
            { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
            if (m->kind == N_FIELD_DECL) emit_registered_member_field(cls, m, out);
            else if (m->kind == N_METHOD) emit_registered_member_method(ir, cls, m, out);
            else die("CXXE: unsupported @exposed member kind");
            str_put(out, ";\n");
        }
    }
    str_put(out, "        return true;\n    }();\n    (void)initialized;\n}\n}\n");
    str_put(out, "namespace {\nstruct CxxeAutoRegistration {\n    CxxeAutoRegistration() { cxxe_generated::register_all(); }\n};\nstatic CxxeAutoRegistration cxxe_auto_registration;\n}\n");
    (void)any;
}

int node_is_factory_runtime_call(const IRNode *n) {
    return n->kind == N_FUNC_CALL && n->qualified &&
        (strcmp(n->qualified, "stde::factory_new") == 0 || strcmp(n->qualified, "stde::factory_find") == 0);
}

int find_call_close(const IR *ir, const IRNode *call, size_t *open_out, size_t *close_out) {
    size_t i, open = SIZE_MAX, close = SIZE_MAX;
    if (call->first_tok >= ir->tokens.n || call->last_tok >= ir->tokens.n) return 0;
    for (i = (size_t)call->first_tok; i <= (size_t)call->last_tok; ++i) {
        if (strcmp(ir->tokens.v[i].text, "(") == 0) { open = i; break; }
    }
    if (open == SIZE_MAX) return 0;
    {
        int d = 0;
        for (i = open; i <= (size_t)call->last_tok; ++i) {
            if (strcmp(ir->tokens.v[i].text, "(") == 0) d++;
            else if (strcmp(ir->tokens.v[i].text, ")") == 0 && --d == 0) { close = i; break; }
        }
    }
    if (close == SIZE_MAX) return 0;
    if (open_out) *open_out = open;
    if (close_out) *close_out = close;
    return 1;
}

void emit_factory_runtime(const IR *ir, const IRNode *call, Str *out) {
    size_t open, close, i;
    if (!find_call_close(ir, call, &open, &close)) {
        append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok);
        return;
    }

    /* new stde::factory_find(name)() is CXXE syntax for a runtime factory. */
    if (strcmp(call->qualified, "stde::factory_find") == 0 && call->first_tok > 0 &&
        strcmp(ir->tokens.v[call->first_tok - 1].text, "new") == 0) {
        size_t next = close + 1;
        if (next + 1 < ir->tokens.n && strcmp(ir->tokens.v[next].text, "(") == 0 && strcmp(ir->tokens.v[next + 1].text, ")") == 0) {
            str_put(out, "(cxxe_generated::register_all(), stde::factory_new(");
            if (open + 1 <= close - 1) {
                for (i = open + 1; i <= close - 1; ++i) {
                    str_put(out, ir->tokens.v[i].trivia);
                    str_put(out, ir->tokens.v[i].text);
                }
            }
            str_put(out, "))");
            return;
        }
    }

    str_put(out, "(cxxe_generated::register_all(), ");
    append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok);
    str_ch(out, ')');
}

void emit_get_member_runtime(const IR *ir, const IRNode *node, Str *out) {
    size_t a = (size_t)node->first_tok, b = (size_t)node->last_tok, i;
    if (!get_member_pattern(ir, node, NULL, NULL, NULL)) {
        append_token_bytes(ir, out, a, b);
        return;
    }
    for (i = a + 1; i + 3 <= b; ++i) {
        if ((strcmp(ir->tokens.v[i].text, ".") == 0 || strcmp(ir->tokens.v[i].text, "->") == 0) &&
            strcmp(ir->tokens.v[i + 1].text, "get_member") == 0 && strcmp(ir->tokens.v[i + 2].text, "(") == 0) {
            size_t close = SIZE_MAX, j = i + 2;
            int d = 0;
            for (; j <= b; ++j) {
                if (strcmp(ir->tokens.v[j].text, "(") == 0) d++;
                else if (strcmp(ir->tokens.v[j].text, ")") == 0 && --d == 0) { close = j; break; }
            }
            if (close == SIZE_MAX) break;
            str_put(out, "(cxxe_generated::register_all(), stde::get_member(");
            append_token_bytes(ir, out, a, i - 1);
            str_put(out, ", ");
            append_token_bytes(ir, out, i + 3, close - 1);
            str_put(out, "))");
            if (close + 1 <= b) append_token_bytes(ir, out, close + 1, b);
            return;
        }
    }
    append_token_bytes(ir, out, a, b);
}
