/*
 * CodeGen/EmitMatch.c
 *
 * match statement lowering.
 */

#include "Core/Common.h"
#include "CodeGen/CodeGenInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"

static int match_pattern_is_type(const IR *ir, size_t a, size_t b) {
    size_t i;
    if (a > b || b >= ir->tokens.n) return 0;
    if (!strcmp(ir->tokens.v[a].text, "default") || !strcmp(ir->tokens.v[a].text, "_")) return 1;
    for (i = a; i <= b; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (isalpha((unsigned char)t[0]) || t[0] == '_' || !strcmp(t, "::") || !strcmp(t, "<") ||
            !strcmp(t, ">") || !strcmp(t, ",") || !strcmp(t, "*") || !strcmp(t, "&") || !strcmp(t, "&&")) continue;
        return 0;
    }
    return 1;
}

void emit_match_statement(const IR *ir, const IRNode *node, Str *out) {
    size_t a=(size_t)node->first_tok, b=(size_t)node->last_tok, brace=(node->aux<ir->tokens.n?(size_t)node->aux:SIZE_MAX), close=b;
    size_t subject_a=a+1, subject_b, i, labels=0, label_start[128], label_colon[128];
    size_t candidate;
    int par=0, br=0, cur=0, angle=0;
    const char *subject_name;
    if (brace==SIZE_MAX || brace<=a+1 || brace>=close) ir_die_at(ir, node->first_tok, "invalid match statement");
    subject_b=brace-1;
    if (subject_a!=subject_b || ir->tokens.v[subject_a].kind!=TK_IDENTIFIER) ir_die_at(ir, node->first_tok, "match currently requires a simple variable as its value");
    subject_name=ir->tokens.v[subject_a].text;
    candidate=brace+1;
    for (i=brace+1;i<close;++i) {
        const char *t=ir->tokens.v[i].text;
        if (!strcmp(t,"(")) par++;
        else if (!strcmp(t,")")&&par) par--;
        else if (!strcmp(t,"[")) br++;
        else if (!strcmp(t,"]")&&br) br--;
        else if (!strcmp(t,"{")) cur++;
        else if (!strcmp(t,"}")) { if (cur) { cur--; if (!cur) candidate=i+1; } }
        else if (!strcmp(t,"<")) angle++;
        else if (!strcmp(t,">")&&angle) angle--;
        else if (!strcmp(t,";")&&!par&&!br&&!cur&&!angle) candidate=i+1;
        else if (!strcmp(t,":")&&!par&&!br&&!cur&&!angle&&candidate<i&&match_pattern_is_type(ir,candidate,i-1)) {
            if (labels>=128) ir_die_at(ir, node->first_tok, "too many match cases");
            label_start[labels]=candidate; label_colon[labels]=i; labels++; candidate=i+1;
        }
    }
    if (!labels) ir_die_at(ir, node->first_tok, "match must contain at least one case");
    str_put(out,"{ auto&& __cxxe_match_value = ("); append_token_bytes(ir,out,subject_a,subject_b); str_put(out,");\n");
    for (i=0;i<labels;++i) {
        size_t body_a=label_colon[i]+1, body_b=(i+1<labels?label_start[i+1]-1:close-1);
        int is_default=!strcmp(ir->tokens.v[label_start[i]].text,"default")||!strcmp(ir->tokens.v[label_start[i]].text,"_");
        if (i==0) str_put(out,is_default?"    if (true) {\n":"    if (__cxxe_match_value.is<");
        else str_put(out,is_default?"    else {\n":"    else if (__cxxe_match_value.is<");
        if (!is_default) { append_token_bytes(ir,out,label_start[i],label_colon[i]-1); str_put(out,">()) {\n        auto&& "); str_put(out,subject_name); str_put(out," = __cxxe_match_value.as<"); append_token_bytes(ir,out,label_start[i],label_colon[i]-1); str_put(out,">();\n"); }
        if (body_a<=body_b) { uint64_t bs=ir->tokens.v[body_a].byte_start, be=ir->tokens.v[body_b].byte_end; emit_source_range_with_properties(ir,out,bs,be); str_ch(out,'\n'); }
        str_put(out,"    }\n");
    }
    str_put(out,"}\n");
}
